/*
 * conn_queue 단위 테스트 (m4m5.md 8.1 경쟁 상태)
 *   make test: ASan으로 실행, make tsan: ThreadSanitizer로 실행
 */
#include "acutest.h"
#include "conn_queue.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* 넣은 순서대로 꺼내지고, 배열 끝에서 처음 칸으로 돌아가도 순서가 유지됨 */
static void test_fifo_and_wraparound(void)
{
    struct conn_queue q;

    TEST_ASSERT(conn_queue_init(&q, 3) == 0);
    for (int round = 0; round < 4; round++) {
        TEST_CHECK(conn_queue_push(&q, round * 10 + 1) == 0);
        TEST_CHECK(conn_queue_push(&q, round * 10 + 2) == 0);
        TEST_CHECK(conn_queue_pop(&q) == round * 10 + 1);
        TEST_CHECK(conn_queue_pop(&q) == round * 10 + 2);
    }
    TEST_CHECK(q.count == 0);
    conn_queue_destroy(&q);
}

static void test_init_rejects_zero_capacity(void)
{
    struct conn_queue q;

    TEST_CHECK(conn_queue_init(&q, 0) == -1);
}

/* shutdown 뒤에는 남은 것은 꺼낼 수 있고, 다 꺼내면 -1. 넣기는 -1 */
static void test_shutdown_drains_then_stops(void)
{
    struct conn_queue q;

    TEST_ASSERT(conn_queue_init(&q, 4) == 0);
    TEST_CHECK(conn_queue_push(&q, 7) == 0);
    conn_queue_shutdown(&q);
    TEST_CHECK(conn_queue_push(&q, 8) == -1);
    TEST_CHECK(conn_queue_pop(&q) == 7);
    TEST_CHECK(conn_queue_pop(&q) == -1);
    conn_queue_destroy(&q);
}

/* 여러 스레드가 동시에 넣고 꺼내도 넣은 값이 정확히 한 번씩 꺼내짐 */
#define PRODUCERS 4
#define CONSUMERS 4
#define PER_PRODUCER 2000
#define TOTAL (PRODUCERS * PER_PRODUCER)

struct shared {
    struct conn_queue q;
    pthread_mutex_t seen_lock;
    int seen[TOTAL];
};

struct producer_arg {
    struct shared *s;
    int base;
};

static void *producer(void *arg)
{
    struct producer_arg *p = arg;

    for (int i = 0; i < PER_PRODUCER; i++) {
        conn_queue_push(&p->s->q, p->base + i);
    }
    return NULL;
}

static void *consumer(void *arg)
{
    struct shared *s = arg;
    int value;

    while ((value = conn_queue_pop(&s->q)) >= 0) {
        pthread_mutex_lock(&s->seen_lock);
        s->seen[value]++;
        pthread_mutex_unlock(&s->seen_lock);
    }
    return NULL;
}

static void test_concurrent_each_value_once(void)
{
    struct shared *s = calloc(1, sizeof(*s));
    pthread_t prod[PRODUCERS], cons[CONSUMERS];
    struct producer_arg args[PRODUCERS];
    int wrong = 0;

    TEST_ASSERT(s != NULL);
    /* 작은 큐라 main(생산자)이 가득 참에서 기다리는 경로도 지나감 */
    TEST_ASSERT(conn_queue_init(&s->q, 8) == 0);
    pthread_mutex_init(&s->seen_lock, NULL);

    for (int i = 0; i < CONSUMERS; i++) {
        pthread_create(&cons[i], NULL, consumer, s);
    }
    for (int i = 0; i < PRODUCERS; i++) {
        args[i].s = s;
        args[i].base = i * PER_PRODUCER;
        pthread_create(&prod[i], NULL, producer, &args[i]);
    }
    for (int i = 0; i < PRODUCERS; i++) {
        pthread_join(prod[i], NULL);
    }
    conn_queue_shutdown(&s->q); /* 남은 것을 다 꺼내면 소비자가 끝남 */
    for (int i = 0; i < CONSUMERS; i++) {
        pthread_join(cons[i], NULL);
    }

    for (int i = 0; i < TOTAL; i++) {
        if (s->seen[i] != 1) {
            wrong++;
        }
    }
    TEST_CHECK(wrong == 0);
    TEST_MSG("한 번씩 꺼내지지 않은 값: %d개", wrong);

    pthread_mutex_destroy(&s->seen_lock);
    conn_queue_destroy(&s->q);
    free(s);
}

TEST_LIST = {{"fifo_and_wraparound", test_fifo_and_wraparound},
             {"init_rejects_zero_capacity", test_init_rejects_zero_capacity},
             {"shutdown_drains_then_stops", test_shutdown_drains_then_stops},
             {"concurrent_each_value_once", test_concurrent_each_value_once},
             {NULL, NULL}};
