/*
 * route - 요청 경로를 보고 어느 백엔드들로 보낼지 고릅니다 (Reverse Proxy).
 *   규칙 하나: "/api/=127.0.0.1:8001,127.0.0.1:8002"
 *              접두사    백엔드 목록 (차례로 돌아가며 사용)
 *   시작할 때 main이 규칙을 채우고, 이후에는 순번(next)만 바뀝니다.
 */
#ifndef ROUTE_H
#define ROUTE_H

#include "proxy.h"

#include <pthread.h>
#include <stddef.h>

#define ROUTE_MAX 16        /* 규칙 수 상한 */
#define ROUTE_BACKEND_MAX 8 /* 규칙 하나의 백엔드 수 상한 */
#define ROUTE_PREFIX_MAX 256

struct route {
    char prefix[ROUTE_PREFIX_MAX]; /* '/'로 시작하고 '/'로 끝남 */
    struct backend backends[ROUTE_BACKEND_MAX];
    int backend_count;
    int next;             /* 다음 요청이 먼저 시도할 백엔드 번호 */
    pthread_mutex_t lock; /* next를 여러 작업자가 같이 바꿈 */
};

struct route_table {
    struct route routes[ROUTE_MAX];
    int count;
};

/*
 * 규칙 문자열 하나를 해석해 표에 더합니다. 백엔드 이름은 이때 한 번만 주소로
 * 바꿉니다. 성공하면 NULL, 실패하면 이유를 적은 문자열.
 */
const char *route_table_add(struct route_table *table, const char *rule);

/* 순번 락을 정리합니다 */
void route_table_destroy(struct route_table *table);

/*
 * 경로(쿼리 제외, len바이트)와 맞는 규칙 중 접두사가 가장 긴 것을 찾습니다.
 * 없으면 NULL (서버가 정적 파일이나 CGI로 직접 처리).
 */
struct route *route_table_find(struct route_table *table, const char *path,
                               size_t len);

/*
 * port에서 모든 주소로 기다리는 이 서버를 가리키는 백엔드를 찾습니다. 없으면 NULL.
 *   자기 자신에게 보내면 요청 하나가 끝없이 돌며 작업자를 모두 차지함
 *   (RFC 9110 7.6). 다른 프록시를 거쳐 돌아오는 경우는 알 수 없음.
 */
const struct backend *route_table_find_self(const struct route_table *table,
                                            int port);

/* 이번 요청이 먼저 시도할 백엔드 번호 (Round-Robin) */
int route_next_backend(struct route *route);

#endif
