/*
 * conn_queue - 스레드 풀의 작업 큐 (final.md 2.2)
 *
 * 규칙
 *   - 큐를 만지는 순간에만 락을 잡습니다. 요청 처리는 락 밖에서 합니다.
 *   - 조건 변수에서 깨어나면 while로 조건을 다시 확인합니다.
 *     (다른 작업자가 먼저 꺼내 갔거나, 이유 없이 깨어나는 spurious wakeup)
 */
#include "conn_queue.h"

#include <stdlib.h>

int conn_queue_init(struct conn_queue *q, int capacity)
{
    if (capacity <= 0) {
        return -1;
    }
    q->items = malloc(sizeof(int) * (size_t)capacity);
    if (q->items == NULL) {
        return -1;
    }
    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->idle = 0;
    q->shutdown = 0;
    if (pthread_mutex_init(&q->lock, NULL) != 0) {
        free(q->items);
        return -1;
    }
    if (pthread_cond_init(&q->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&q->lock);
        free(q->items);
        return -1;
    }
    if (pthread_cond_init(&q->not_full, NULL) != 0) {
        pthread_cond_destroy(&q->not_empty);
        pthread_mutex_destroy(&q->lock);
        free(q->items);
        return -1;
    }
    return 0;
}

void conn_queue_destroy(struct conn_queue *q)
{
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
    free(q->items);
    q->items = NULL;
}

int conn_queue_push(struct conn_queue *q, int connfd)
{
    pthread_mutex_lock(&q->lock);
    /* 가득 차 있으면 작업자가 하나 꺼내 갈 때까지 기다림 (503 없음) */
    while (q->count == q->capacity && !q->shutdown) {
        pthread_cond_wait(&q->not_full, &q->lock);
    }
    if (q->shutdown) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    q->items[q->tail] = connfd;
    q->tail = (q->tail + 1) % q->capacity; /* 끝에 닿으면 처음 칸으로 */
    q->count++;
    pthread_cond_signal(&q->not_empty); /* 잠든 작업자 하나를 깨움 */
    pthread_mutex_unlock(&q->lock);
    return 0;
}

int conn_queue_pop(struct conn_queue *q)
{
    int connfd;

    pthread_mutex_lock(&q->lock);
    q->idle++;
    while (q->count == 0 && !q->shutdown) {
        pthread_cond_wait(&q->not_empty, &q->lock);
    }
    q->idle--;
    if (q->count == 0) { /* shutdown이고 남은 것도 없음 */
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    connfd = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_cond_signal(&q->not_full); /* 기다리던 main을 깨움 */
    pthread_mutex_unlock(&q->lock);
    return connfd;
}

int conn_queue_has_waiting(struct conn_queue *q)
{
    int waiting;

    pthread_mutex_lock(&q->lock);
    waiting = q->count > q->idle;
    pthread_mutex_unlock(&q->lock);
    return waiting;
}

void conn_queue_shutdown(struct conn_queue *q)
{
    pthread_mutex_lock(&q->lock);
    q->shutdown = 1;
    pthread_cond_broadcast(&q->not_empty); /* 기다리는 작업자 모두 */
    pthread_cond_broadcast(&q->not_full);  /* 기다리는 main */
    pthread_mutex_unlock(&q->lock);
}
