/*
 * conn_queue.h - main 스레드가 accept한 연결(connfd)을 작업자 스레드에게
 *   넘기는 고정 크기 작업 큐입니다 (final.md 2.2).
 *
 *   main:   accept → conn_queue_push (가득 차면 기다림)
 *   작업자: conn_queue_pop (비면 기다림) → 요청 처리 → close
 *
 *   원형 배열 + mutex 1개 + 조건 변수 2개(not_empty, not_full)로 만듭니다.
 *   넣는 값이 int 하나라 배열로 충분하고, 넣고 뺄 때 malloc이 없습니다.
 */
#ifndef CONN_QUEUE_H
#define CONN_QUEUE_H

#include <pthread.h>

struct conn_queue {
    int *buf;           /* 연결 fd를 담는 칸 cap개 (init에서 한 번 malloc) */
    int cap;            /* 칸 수 */
    int head;           /* 다음에 꺼낼 칸 */
    int tail;           /* 다음에 넣을 칸 */
    int count;          /* 지금 들어 있는 개수 */
    int shutdown;       /* 1이면 더 넣지 않음. 남은 것은 다 꺼낸 뒤 pop이 -1 */
    pthread_mutex_t mu; /* 위 필드는 모두 이 락을 잡고 읽고 씀 */
    pthread_cond_t not_empty; /* pop이 기다리는 곳: 하나 들어오면 깨움 */
    pthread_cond_t not_full;  /* push가 기다리는 곳: 하나 빠지면 깨움 */
};

/* 칸 cap개짜리 큐를 만듭니다. 성공 0, 실패 -1 (cap < 1 또는 메모리·락 생성 실패) */
int conn_queue_init(struct conn_queue *q, int cap);

/* fd를 넣습니다. 가득 차 있으면 자리가 날 때까지 기다립니다.
 * 성공 0. shutdown 뒤에는 넣지 않고 -1 → fd는 호출한 쪽이 닫아야 합니다. */
int conn_queue_push(struct conn_queue *q, int fd);

/* fd를 하나 꺼냅니다. 비어 있으면 들어올 때까지 기다립니다.
 * shutdown 뒤에도 남은 fd는 먼저 다 돌려주고, 비면 -1 (작업자가 끝날 신호). */
int conn_queue_pop(struct conn_queue *q);

/* 종료 표시를 하고, 기다리는 push·pop을 모두 깨웁니다.
 * 서버에서 부르는 것은 M7(안전한 종료)이고, 지금은 단위 테스트에서만 씁니다. */
void conn_queue_shutdown(struct conn_queue *q);

/* 큐 자원을 해제합니다. 큐를 쓰는 스레드가 모두 끝난 뒤에만 부릅니다. */
void conn_queue_destroy(struct conn_queue *q);

#endif
