#include "queue.h"

int queue_init(struct queue *q)
{
    q->front = 0;
    q->count = 0;
    if (pthread_mutex_init(&q->lock, NULL) != 0 ||
        pthread_cond_init(&q->not_empty, NULL) != 0 ||
        pthread_cond_init(&q->not_full, NULL) != 0) {
        return -1;
    }
    return 0;
}

void queue_push(struct queue *q, int fd)
{
    pthread_mutex_lock(&q->lock);

    /* 기다리는 동안에는 락이 풀리고, 깨어나면 락을 다시 잡은 상태로 돌아온다.
     * 깨어난 사이에 상황이 바뀌었을 수 있어 if가 아니라 while로 다시 확인한다 */
    while (q->count == QUEUE_SIZE) {
        pthread_cond_wait(&q->not_full, &q->lock);
    }

    q->fds[(q->front + q->count) % QUEUE_SIZE] = fd;
    q->count++;

    pthread_cond_signal(&q->not_empty); /* 자고 있는 작업자 한 명을 깨움 */
    pthread_mutex_unlock(&q->lock);
}

int queue_pop(struct queue *q)
{
    int fd;

    pthread_mutex_lock(&q->lock);

    while (q->count == 0) {
        pthread_cond_wait(&q->not_empty, &q->lock);
    }

    fd = q->fds[q->front];
    q->front = (q->front + 1) % QUEUE_SIZE;
    q->count--;

    pthread_cond_signal(&q->not_full); /* 자리를 기다리는 메인을 깨움 */
    pthread_mutex_unlock(&q->lock);
    return fd;
}
