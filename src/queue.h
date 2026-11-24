/*
 * queue - 메인 스레드가 받은 연결(fd)을 작업자 스레드에게 넘기는 작업 큐입니다.
 */
#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>

#define QUEUE_SIZE 64

struct queue {
    int fds[QUEUE_SIZE];  /* 기다리는 연결들. 끝에 닿으면 0번 칸으로 돌아감 */
    int front;            /* 다음에 꺼낼 칸 */
    int count;            /* 지금 들어 있는 개수 */
    pthread_mutex_t lock; /* 한 번에 한 스레드만 큐를 만지게 하는 자물쇠 */
    pthread_cond_t not_empty; /* "연결이 들어왔다"고 작업자를 깨우는 벨 */
    pthread_cond_t not_full;  /* "자리가 났다"고 메인을 깨우는 벨 */
};

/* 빈 큐로 초기화합니다. 성공하면 0, 실패하면 -1 */
int queue_init(struct queue *q);

/* 연결 하나를 넣습니다. 꽉 차 있으면 자리가 날 때까지 기다립니다 */
void queue_push(struct queue *q, int fd);

/* 먼저 들어온 연결을 꺼냅니다. 비어 있으면 들어올 때까지 기다립니다 */
int queue_pop(struct queue *q);

#endif
