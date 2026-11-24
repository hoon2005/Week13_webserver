/*
 * conn_queue_test.c - 작업 큐 단위 테스트
 *
 *   make test  : ASan + UBSan으로 실행 (Linux에서는 종료 시 누수 검사도)
 *   make tsan  : ThreadSanitizer로 실행 (락을 빠뜨린 데이터 경합을 찾음)
 *
 * 엣지 케이스 문서(docs/plan/phase2-edge-cases-concurrency.md)의
 * E-C9 ~ E-C12를 여기서 확인합니다.
 */
#include "acutest.h"
#include "conn_queue.h"

#include <time.h>

/* 다른 스레드가 wait에 들어갈 시간을 줌. 테스트 결과는 이 시간에 기대지 않고,
 * "기다리고 있어야 할 스레드가 아직 안 끝났는지"를 확인하는 데만 씀 */
static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* ---------- 한 스레드에서 순서 확인 ---------- */

/* 먼저 넣은 것이 먼저 나오고, 배열 끝에서 0번 칸으로 돌아가도 순서가 유지됨 */
static void test_fifo_and_wraparound(void)
{
    struct conn_queue q;

    TEST_ASSERT(conn_queue_init(&q, 3) == 0);
    TEST_CHECK(conn_queue_push(&q, 10) == 0);
    TEST_CHECK(conn_queue_push(&q, 11) == 0);
    TEST_CHECK(conn_queue_push(&q, 12) == 0);
    TEST_CHECK(conn_queue_pop(&q) == 10);
    TEST_CHECK(conn_queue_push(&q, 13) == 0); /* tail이 0번 칸으로 돌아감 */
    TEST_CHECK(conn_queue_pop(&q) == 11);
    TEST_CHECK(conn_queue_pop(&q) == 12);
    TEST_CHECK(conn_queue_pop(&q) == 13); /* head도 0번 칸으로 돌아감 */
    conn_queue_destroy(&q);
}

static void test_init_rejects_bad_cap(void)
{
    struct conn_queue q;

    TEST_CHECK(conn_queue_init(&q, 0) == -1);
    TEST_CHECK(conn_queue_init(&q, -5) == -1);
}

/* E-C11: shutdown 뒤에도 남은 것은 다 꺼내 주고, 그다음 -1 */
static void test_shutdown_drains_remaining(void)
{
    struct conn_queue q;

    TEST_ASSERT(conn_queue_init(&q, 4) == 0);
    conn_queue_push(&q, 1);
    conn_queue_push(&q, 2);
    conn_queue_push(&q, 3);
    conn_queue_shutdown(&q);
    TEST_CHECK(conn_queue_pop(&q) == 1);
    TEST_CHECK(conn_queue_pop(&q) == 2);
    TEST_CHECK(conn_queue_pop(&q) == 3);
    TEST_CHECK(conn_queue_pop(&q) == -1);
    conn_queue_destroy(&q);
}

/* E-C12: shutdown 뒤 push는 -1 (넣지 않음) */
static void test_push_after_shutdown_fails(void)
{
    struct conn_queue q;

    TEST_ASSERT(conn_queue_init(&q, 4) == 0);
    conn_queue_shutdown(&q);
    TEST_CHECK(conn_queue_push(&q, 7) == -1);
    TEST_CHECK(conn_queue_pop(&q) == -1);
    conn_queue_destroy(&q);
}

/* ---------- 기다림 확인 ---------- */

struct pop_arg {
    struct conn_queue *q;
    int result;
};

static void *pop_thread(void *p)
{
    struct pop_arg *a = p;
    a->result = conn_queue_pop(a->q);
    return NULL;
}

/* 빈 큐에서 pop은 기다렸다가, push가 오면 그 값을 받음 */
static void test_pop_waits_for_push(void)
{
    struct conn_queue q;
    struct pop_arg a = {&q, 0};
    pthread_t t;

    TEST_ASSERT(conn_queue_init(&q, 2) == 0);
    TEST_ASSERT(pthread_create(&t, NULL, pop_thread, &a) == 0);
    sleep_ms(50); /* pop이 not_empty에서 기다리게 둠 */
    TEST_CHECK(conn_queue_push(&q, 42) == 0);
    pthread_join(t, NULL); /* join 뒤에는 a.result를 안전하게 읽음 */
    TEST_CHECK(a.result == 42);
    conn_queue_destroy(&q);
}

/* 빈 큐에서 기다리던 pop을 shutdown이 깨우고, -1을 받음 */
static void test_shutdown_wakes_waiting_pop(void)
{
    struct conn_queue q;
    struct pop_arg a = {&q, 0};
    pthread_t t;

    TEST_ASSERT(conn_queue_init(&q, 2) == 0);
    TEST_ASSERT(pthread_create(&t, NULL, pop_thread, &a) == 0);
    sleep_ms(50);
    conn_queue_shutdown(&q);
    pthread_join(t, NULL);
    TEST_CHECK(a.result == -1);
    conn_queue_destroy(&q);
}

struct push_arg {
    struct conn_queue *q;
    int fd;
    int result;
    int done; /* push가 끝났는지. done_mu를 잡고 읽고 씀 */
    pthread_mutex_t done_mu;
};

static void *push_thread(void *p)
{
    struct push_arg *a = p;
    int r = conn_queue_push(a->q, a->fd);

    pthread_mutex_lock(&a->done_mu);
    a->result = r;
    a->done = 1;
    pthread_mutex_unlock(&a->done_mu);
    return NULL;
}

static int push_done(struct push_arg *a)
{
    int d;

    pthread_mutex_lock(&a->done_mu);
    d = a->done;
    pthread_mutex_unlock(&a->done_mu);
    return d;
}

/* E-C3: 가득 찬 큐에 push하면 503 없이 기다리고, pop으로 자리가 나면 들어감 */
static void test_push_waits_when_full(void)
{
    struct conn_queue q;
    struct push_arg a = {&q, 2, 0, 0, PTHREAD_MUTEX_INITIALIZER};
    pthread_t t;

    TEST_ASSERT(conn_queue_init(&q, 1) == 0);
    TEST_CHECK(conn_queue_push(&q, 1) == 0); /* 이제 가득 참 */
    TEST_ASSERT(pthread_create(&t, NULL, push_thread, &a) == 0);
    sleep_ms(50);
    TEST_CHECK(!push_done(&a)); /* 자리가 없으니 아직 기다리는 중이어야 함 */
    TEST_CHECK(conn_queue_pop(&q) == 1); /* 자리를 만들면 */
    pthread_join(t, NULL);               /* 기다리던 push가 끝나고 */
    TEST_CHECK(a.result == 0);
    TEST_CHECK(conn_queue_pop(&q) == 2); /* 그 값이 들어가 있음 */
    conn_queue_destroy(&q);
    pthread_mutex_destroy(&a.done_mu);
}

/* 가득 차서 기다리던 push를 shutdown이 깨우고, -1을 받음 */
static void test_shutdown_wakes_waiting_push(void)
{
    struct conn_queue q;
    struct push_arg a = {&q, 2, 0, 0, PTHREAD_MUTEX_INITIALIZER};
    pthread_t t;

    TEST_ASSERT(conn_queue_init(&q, 1) == 0);
    conn_queue_push(&q, 1);
    TEST_ASSERT(pthread_create(&t, NULL, push_thread, &a) == 0);
    sleep_ms(50);
    conn_queue_shutdown(&q);
    pthread_join(t, NULL);
    TEST_CHECK(a.result == -1);
    conn_queue_destroy(&q);
    pthread_mutex_destroy(&a.done_mu);
}

/* ---------- 여러 스레드 동시 사용 (E-C9, E-C10) ---------- */

#define PRODUCERS 4
#define CONSUMERS                                                              \
    6 /* 큐 칸 수보다 많은 소비자: 깨어났는데 빈 큐인 경우를 만듦 */
#define PER_PRODUCER 2000
#define TOTAL (PRODUCERS * PER_PRODUCER)

struct producer_arg {
    struct conn_queue *q;
    int first; /* first, first+1, ... 를 PER_PRODUCER개 넣음 (값이 겹치지 않음) */
};

static void *producer(void *p)
{
    struct producer_arg *a = p;

    for (int i = 0; i < PER_PRODUCER; i++) {
        conn_queue_push(a->q, a->first + i);
    }
    return NULL;
}

struct consumer_arg {
    struct conn_queue *q;
    int *seen; /* 이 소비자만 쓰는 배열: seen[v] = v를 꺼낸 횟수 */
};

static void *consumer(void *p)
{
    struct consumer_arg *a = p;
    int v;

    while ((v = conn_queue_pop(a->q)) != -1) {
        if (v >= 0 && v < TOTAL) {
            a->seen[v]++;
        }
    }
    return NULL;
}

/* 넣은 값 TOTAL개가 빠짐없이, 정확히 한 번씩 꺼내짐 */
static void test_many_producers_consumers(void)
{
    static int seen[CONSUMERS][TOTAL]; /* 스택에 두기엔 커서 static */
    struct conn_queue q;
    pthread_t pt[PRODUCERS], ct[CONSUMERS];
    struct producer_arg pa[PRODUCERS];
    struct consumer_arg ca[CONSUMERS];
    int bad = 0;

    TEST_ASSERT(conn_queue_init(&q, 4) == 0);
    for (int c = 0; c < CONSUMERS; c++) {
        ca[c] = (struct consumer_arg){&q, seen[c]};
        TEST_ASSERT(pthread_create(&ct[c], NULL, consumer, &ca[c]) == 0);
    }
    for (int p = 0; p < PRODUCERS; p++) {
        pa[p] = (struct producer_arg){&q, p * PER_PRODUCER};
        TEST_ASSERT(pthread_create(&pt[p], NULL, producer, &pa[p]) == 0);
    }

    for (int p = 0; p < PRODUCERS; p++) {
        pthread_join(pt[p], NULL);
    }
    conn_queue_shutdown(
        &q); /* 다 넣었으니 종료 → 소비자는 남은 것까지 꺼내고 끝남 */
    for (int c = 0; c < CONSUMERS; c++) {
        pthread_join(ct[c], NULL);
    }

    for (int v = 0; v < TOTAL; v++) {
        int total = 0;
        for (int c = 0; c < CONSUMERS; c++) {
            total += seen[c][v];
        }
        if (total != 1) {
            bad++;
        }
    }
    TEST_CHECK(bad == 0);
    TEST_MSG("한 번이 아닌 횟수로 꺼내진 값: %d개", bad);
    conn_queue_destroy(&q);
}

TEST_LIST = {
    {"fifo_and_wraparound", test_fifo_and_wraparound},
    {"init_rejects_bad_cap", test_init_rejects_bad_cap},
    {"shutdown_drains_remaining", test_shutdown_drains_remaining},
    {"push_after_shutdown_fails", test_push_after_shutdown_fails},
    {"pop_waits_for_push", test_pop_waits_for_push},
    {"shutdown_wakes_waiting_pop", test_shutdown_wakes_waiting_pop},
    {"push_waits_when_full", test_push_waits_when_full},
    {"shutdown_wakes_waiting_push", test_shutdown_wakes_waiting_push},
    {"many_producers_consumers", test_many_producers_consumers},
    {NULL, NULL},
};
