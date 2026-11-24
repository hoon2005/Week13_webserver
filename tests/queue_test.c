#include "acutest.h"
#include "queue.h"

#include <time.h>

#define THREADS 4
#define ITEMS 1000

static struct queue q;

/* 다른 스레드가 먼저 대기 상태에 들어갈 시간을 줍니다(0.05초) */
static void sleep_briefly(void)
{
    struct timespec ts = {0, 50000000};

    nanosleep(&ts, NULL);
}

static void test_order(void)
{
    TEST_CHECK(queue_init(&q) == 0);
    queue_push(&q, 10);
    queue_push(&q, 20);
    queue_push(&q, 30);
    TEST_CHECK(queue_pop(&q) == 10);
    TEST_CHECK(queue_pop(&q) == 20);
    TEST_CHECK(queue_pop(&q) == 30);
}

static void test_wrap_around(void)
{
    TEST_CHECK(queue_init(&q) == 0);
    for (int i = 0; i < QUEUE_SIZE * 3; i++) {
        queue_push(&q, i);
        queue_push(&q, i + 1000);
        TEST_CHECK(queue_pop(&q) == i);
        TEST_CHECK(queue_pop(&q) == i + 1000);
    }
}

static int popped;

static void *pop_one(void *arg)
{
    (void)arg;
    popped = queue_pop(&q);
    return NULL;
}

static void test_pop_waits(void)
{
    pthread_t tid;

    TEST_CHECK(queue_init(&q) == 0);
    TEST_ASSERT(pthread_create(&tid, NULL, pop_one, NULL) == 0);
    sleep_briefly(); /* 꺼내는 스레드가 먼저 기다리게 함 */
    queue_push(&q, 7);
    pthread_join(tid, NULL);
    TEST_CHECK(popped == 7);
}

static void *push_one(void *arg)
{
    (void)arg;
    queue_push(&q, 999);
    return NULL;
}

static void test_push_waits(void)
{
    pthread_t tid;

    TEST_CHECK(queue_init(&q) == 0);
    for (int i = 0; i < QUEUE_SIZE; i++) {
        queue_push(&q, i);
    }
    TEST_ASSERT(pthread_create(&tid, NULL, push_one, NULL) == 0);
    sleep_briefly(); /* 넣는 스레드가 먼저 기다리게 함 */

    pthread_mutex_lock(&q.lock);
    TEST_CHECK(q.count == QUEUE_SIZE);
    pthread_mutex_unlock(&q.lock);

    TEST_CHECK(queue_pop(&q) == 0);
    pthread_join(tid, NULL);
    for (int i = 1; i < QUEUE_SIZE; i++) {
        TEST_CHECK(queue_pop(&q) == i);
    }
    TEST_CHECK(queue_pop(&q) == 999);
}

/* 각 값이 몇 번 나왔는지 셉니다. 여러 스레드가 함께 쓰므로 락으로 지킵니다 */
static int seen[THREADS * ITEMS];
static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;

static void *push_many(void *arg)
{
    int start = *(int *)arg;

    for (int i = 0; i < ITEMS; i++) {
        queue_push(&q, start + i);
    }
    return NULL;
}

static void *pop_many(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITEMS; i++) {
        int value = queue_pop(&q);

        pthread_mutex_lock(&seen_lock);
        seen[value]++;
        pthread_mutex_unlock(&seen_lock);
    }
    return NULL;
}

static void test_many_threads(void)
{
    pthread_t push_tids[THREADS];
    pthread_t pop_tids[THREADS];
    int starts[THREADS];

    TEST_CHECK(queue_init(&q) == 0);
    for (int i = 0; i < THREADS; i++) {
        starts[i] = i * ITEMS;
        TEST_ASSERT(
            pthread_create(&push_tids[i], NULL, push_many, &starts[i]) == 0);
        TEST_ASSERT(pthread_create(&pop_tids[i], NULL, pop_many, NULL) == 0);
    }
    for (int i = 0; i < THREADS; i++) {
        pthread_join(push_tids[i], NULL);
        pthread_join(pop_tids[i], NULL);
    }
    for (int i = 0; i < THREADS * ITEMS; i++) {
        TEST_CHECK(seen[i] == 1);
    }
}

TEST_LIST = {
    {"먼저 넣은 연결이 먼저 나온다", test_order},
    {"배열 끝에 닿아도 순서가 유지된다", test_wrap_around},
    {"큐가 비어 있으면 연결이 들어올 때까지 기다린다", test_pop_waits},
    {"큐가 꽉 차 있으면 자리가 날 때까지 기다린다", test_push_waits},
    {"여러 스레드가 함께 써도 연결이 빠지거나 겹치지 않는다",
     test_many_threads},
    {NULL, NULL},
};
