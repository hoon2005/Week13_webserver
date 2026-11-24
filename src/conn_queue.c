/*
 * conn_queue.c - 연결 fd 작업 큐 (설명은 conn_queue.h)
 *
 * 규칙 세 가지
 *   1. 큐 필드는 mu를 잡은 동안에만 읽고 씁니다. 락이 없으면 두 작업자가
 *      같은 칸을 꺼내거나(같은 연결을 둘이 처리) 연결 하나가 사라집니다.
 *   2. 조건 변수에서 깨어나면 while로 조건을 다시 확인합니다. 깨어났는데
 *      다른 작업자가 먼저 꺼내 갔거나, 이유 없이 깨는 경우(spurious wakeup)가
 *      있기 때문입니다.
 *   3. 락은 큐를 만지는 순간에만 잡습니다. 요청 처리는 락 밖에서 합니다.
 */
#include "conn_queue.h"

#include <stdlib.h>

int conn_queue_init(struct conn_queue *q, int cap)
{
    if (cap < 1) {
        return -1;
    }
    q->buf = malloc(sizeof(*q->buf) * (size_t)cap);
    if (q->buf == NULL) {
        return -1;
    }
    q->cap = cap;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->shutdown = 0;

    /* pthread 함수는 실패하면 0이 아닌 오류 번호를 돌려줌 */
    if (pthread_mutex_init(&q->mu, NULL) != 0) {
        goto fail_buf;
    }
    if (pthread_cond_init(&q->not_empty, NULL) != 0) {
        goto fail_mu;
    }
    if (pthread_cond_init(&q->not_full, NULL) != 0) {
        goto fail_not_empty;
    }
    return 0;

    /* 앞에서 만든 것만 거꾸로 해제 */
fail_not_empty:
    pthread_cond_destroy(&q->not_empty);
fail_mu:
    pthread_mutex_destroy(&q->mu);
fail_buf:
    free(q->buf);
    q->buf = NULL;
    return -1;
}

int conn_queue_push(struct conn_queue *q, int fd)
{
    pthread_mutex_lock(&q->mu);
    /* 가득 찼으면 기다림. wait는 기다리는 동안 락을 풀고, 깨어나면 다시 잡음 */
    while (q->count == q->cap && !q->shutdown) {
        pthread_cond_wait(&q->not_full, &q->mu);
    }
    if (q->shutdown) { /* 종료 중이면 넣지 않음 (fd는 호출한 쪽이 닫음) */
        pthread_mutex_unlock(&q->mu);
        return -1;
    }

    q->buf[q->tail] = fd;
    q->tail = (q->tail + 1) % q->cap; /* 배열 끝에 닿으면 0번 칸으로 돌아감 */
    q->count++;

    /* 비어서 기다리던 작업자 하나를 깨움 (넣은 것이 하나라 하나만) */
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

int conn_queue_pop(struct conn_queue *q)
{
    int fd;

    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !q->shutdown) {
        pthread_cond_wait(&q->not_empty, &q->mu);
    }
    if (q->count == 0) { /* 여기 왔다면 shutdown이고 남은 것도 없음 */
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    /* shutdown이어도 남은 연결은 먼저 꺼내 줌 (받아 놓은 손님은 처리) */

    fd = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;

    /* 가득 차서 기다리던 main을 깨움 */
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return fd;
}

void conn_queue_shutdown(struct conn_queue *q)
{
    pthread_mutex_lock(&q->mu);
    q->shutdown = 1;
    /* signal은 하나만 깨우므로, 기다리는 스레드가 모두 종료를 알도록 broadcast */
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mu);
}

void conn_queue_destroy(struct conn_queue *q)
{
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->mu);
    free(q->buf);
    q->buf = NULL;
}
