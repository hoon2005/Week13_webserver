/*
 * proxy - Reverse Proxy (docs/prd/m6m7.md 2장)
 *   라우팅 규칙(--route) 해석, 경로 접두사 라우팅, Round-Robin,
 *   백엔드로 보낼 요청 헤더와 클라이언트로 보낼 응답 헤더 만들기,
 *   백엔드 연결과 중계를 맡습니다.
 *
 *   헤더를 만드는 함수와 규칙을 해석하는 함수는 소켓을 쓰지 않아서
 *   tests/proxy_test.c에서 소켓 없이 확인합니다.
 */
#ifndef PROXY_H
#define PROXY_H

#include "http.h"

#include <netinet/in.h>
#include <stdatomic.h>
#include <stddef.h>

#define PROXY_ROUTES_MAX 16   /* 규칙 수 상한 (M6-4) */
#define PROXY_BACKENDS_MAX 8  /* 규칙당 백엔드 수 상한 (M6-4) */
#define PROXY_PREFIX_MAX 256  /* 접두사 길이 상한 */
#define PROXY_NAME_MAX 300    /* 로그에 쓰는 "이름:포트" */
#define PROXY_CONNECT_MS 3000 /* 백엔드 하나에 연결하는 시간 상한 (M6-14) */
#define PROXY_HEAD_MAX 8192   /* 백엔드 응답 헤더 상한 (M6-11) */

struct proxy_backend {
    struct sockaddr_in addr; /* 시작할 때 한 번 찾은 IPv4 주소 (M6-5) */
    char name[PROXY_NAME_MAX];
};

struct proxy_route {
    char prefix[PROXY_PREFIX_MAX]; /* 끝의 '/'를 뗀 접두사. "/" 규칙은 "" */
    size_t prefix_len;
    struct proxy_backend backends[PROXY_BACKENDS_MAX];
    int backend_count;
    atomic_uint next; /* Round-Robin 순번. 여러 작업자가 올림 (M6-17) */
};

/* 고정 크기 표라 규칙을 위한 동적 할당과 해제가 없음 (M6-4) */
struct proxy_table {
    struct proxy_route routes[PROXY_ROUTES_MAX];
    int count;
};

void proxy_table_init(struct proxy_table *t);

/*
 * "--route" 값 하나("/api=127.0.0.1:8001,127.0.0.1:8002")를 표에 더합니다.
 *   형식 오류, '/'로 시작하지 않는 접두사, 잘못된 포트, 같은 접두사, 상한 초과,
 *   주소를 찾지 못한 이름, 프록시 자신(이 컴퓨터의 주소 + listen_port)은 거절.
 * 반환: 0, 실패하면 -1 (err에 이유)
 */
int proxy_route_add(struct proxy_table *t, const char *spec, int listen_port,
                    char *err, size_t errlen);

/*
 * URI의 '?' 앞 경로와 맞는 규칙 중 가장 긴 접두사를 찾습니다 (M6-4).
 * 접두사 다음 글자가 '/'이거나 경로 끝일 때만 맞음. 없으면 NULL.
 */
struct proxy_route *proxy_route_find(struct proxy_table *t, const char *uri);

/* Round-Robin: 이번 요청이 처음 시도할 백엔드 번호 (M6-17) */
int proxy_backend_start(struct proxy_route *r);

/*
 * 백엔드로 보낼 요청 줄과 헤더(빈 줄까지)를 만듭니다 (M6-9, M6-10).
 *   요청 줄은 HTTP/1.0, 홉 단위 헤더와 Connection에 적힌 헤더, Expect,
 *   클라이언트가 보낸 X-Forwarded-For는 빼고, Host와 Content-Length는 다시 쓰고,
 *   X-Forwarded-For, Via, Connection: close를 더합니다. 본문은 넣지 않음.
 * 반환: 쓴 길이, 버퍼가 모자라면 -1
 */
int proxy_request_build(const struct http_request *req, const char *client_ip,
                        char *out, size_t cap);

/* 응답 본문을 어떻게 읽을지 (M6-12) */
enum proxy_body {
    PROXY_BODY_NONE,       /* 본문 없음: HEAD, 204, 304 */
    PROXY_BODY_LENGTH,     /* Content-Length만큼 */
    PROXY_BODY_UNTIL_CLOSE /* 백엔드가 닫을 때까지. 클라이언트 연결도 닫음 */
};

struct proxy_response {
    int status;
    int has_length;
    long length;
    int body; /* enum proxy_body */
};

/*
 * buf에서 헤더 끝(빈 줄)을 찾아 본문이 시작하는 위치를 돌려줍니다.
 * 아직 빈 줄이 없으면 0.
 */
size_t proxy_head_end(const char *buf, size_t len);

/*
 * 백엔드 응답 헤더(상태 줄부터 빈 줄까지 len바이트)를 확인하고, 클라이언트에
 * 보낼 상태 줄(HTTP/1.1)과 헤더를 out에 씁니다 (M6-11, M6-12).
 *   out에는 홉 단위 헤더, Connection에 적힌 헤더, Content-Length를 뺀 줄만
 *   들어갑니다. Content-Length와 Connection은 호출한 쪽이 붙입니다.
 * 반환: 0 정상, 1 중간 응답(100, 103 등: 전달하지 않고 다음 응답을 읽음),
 *       -1 잘못된 응답(502: 형식 오류, Transfer-Encoding, 101, 서로 다른 길이)
 */
int proxy_response_parse(const char *head, size_t len, int head_request,
                         struct proxy_response *res, char *out, size_t cap,
                         size_t *out_len);

/* proxy_forward의 결과: 호출한 쪽이 연결을 어떻게 할지 */
enum proxy_result {
    PROXY_REPLIED, /* 응답을 보냄. *status, *keep 확인 */
    PROXY_FAILED,  /* 아무것도 보내지 않음. *status(502, 504)로 오류 응답 */
    PROXY_CLOSE,   /* 본문 도중 실패: 응답 없이 바로 닫음 (M6-13) */
    PROXY_RESET    /* 길이를 모르는 본문 도중 실패: RST로 닫음 (M6-13) */
};

/*
 * 요청 하나를 route의 백엔드로 중계합니다.
 *   연결 실패는 다음 백엔드로 넘어가고(M6-15), 연결부터 최종 응답 헤더까지
 *   합계 timeout_sec초, 본문은 데이터 간격 timeout_sec초(M6-14).
 *   *keep은 클라이언트 연결을 유지할지. 응답 헤더를 쓰기 직전에 *stopping이
 *   서 있으면 Connection: close로 보냄 (M7-7).
 *   *backend에는 요청을 보낸 백엔드 이름 (로그용, 없으면 NULL).
 */
int proxy_forward(int client_fd, const struct http_request *req,
                  const char *client_ip, struct proxy_route *route,
                  int timeout_sec, atomic_int *stopping, int *keep, int *status,
                  const char **backend);

#endif
