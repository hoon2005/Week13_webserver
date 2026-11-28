/*
 * conn_queue - 스레드 풀의 작업 큐입니다 (m4m5.md 2.2).
 *   main 스레드가 accept한 connfd를 넣고, 작업자 스레드가 꺼냅니다.
 *   고정 크기 원형 배열 + mutex 1개 + 조건 변수 2개(not_empty, not_full).
 */
#ifndef CONN_QUEUE_H
#define CONN_QUEUE_H

#include <pthread.h>

struct conn_queue {
    int *items;   /* connfd를 담는 배열 (capacity칸) */
    int capacity; /* 배열 칸 수 */
    int head;     /* 다음에 꺼낼 칸 */
    int tail;     /* 다음에 넣을 칸 */
    int count;    /* 지금 들어 있는 개수 */
    int shutdown; /* 1이면 더 기다리지 않음 (M7 종료 처리용) */
    pthread_mutex_t lock;
    pthread_cond_t not_empty; /* 꺼낼 것이 생겼다는 신호 (작업자가 기다림) */
    pthread_cond_t not_full;  /* 빈자리가 생겼다는 신호 (main이 기다림) */
};

/* 큐를 만듭니다. 성공하면 0, 실패하면 -1 */
int conn_queue_init(struct conn_queue *q, int capacity);

/* 큐가 쓰던 메모리와 동기화 객체를 정리합니다. 기다리는 스레드가 없을 때 부릅니다 */
void conn_queue_destroy(struct conn_queue *q);

/* connfd를 넣습니다. 가득 차 있으면 빈자리가 날 때까지 기다립니다.
 * 넣었으면 0, shutdown 상태라 넣지 못했으면 -1 */
int conn_queue_push(struct conn_queue *q, int connfd);

/* connfd를 꺼냅니다. 비어 있으면 들어올 때까지 기다립니다.
 * shutdown 상태이고 큐가 비었으면 -1 (남은 것은 다 꺼낼 수 있음) */
int conn_queue_pop(struct conn_queue *q);

/* 종료 플래그를 세우고 기다리는 스레드를 모두 깨웁니다 */
void conn_queue_shutdown(struct conn_queue *q);

#endif
