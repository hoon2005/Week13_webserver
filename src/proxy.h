/*
 * proxy - 요청 하나를 백엔드에 보내고 응답을 손님에게 전달합니다.
 *   어떤 백엔드로 보낼지(라우팅)와 오류 페이지는 모릅니다. 부르는 쪽이 정합니다.
 *
 *   쓰는 순서
 *     proxy_open   연결 → 요청 전송 → 응답 헤더 받기. 아직 손님에게 쓴 것이 없으므로
 *                  실패하면 부르는 쪽이 502, 504를 보낼 수 있음
 *     proxy_relay  응답을 손님에게 전달. 쓰기 시작한 뒤라 실패하면 닫는 수밖에 없음
 *     proxy_close  백엔드 연결 닫기
 */
#ifndef PROXY_H
#define PROXY_H

#include "http.h"

#include <netinet/in.h>
#include <stddef.h>

#define BACKEND_NAME_MAX 128
#define PROXY_HEAD_MAX 8192 /* 백엔드 응답 헤더 전체 상한 */

struct backend {
    char name[BACKEND_NAME_MAX]; /* "127.0.0.1:8001" (로그용) */
    struct sockaddr_in addr;
};

struct proxy_response {
    int status;
    char reason[64];
    long content_length; /* 없으면 -1 */
};

/* 백엔드 하나와 주고받는 동안의 상태 */
struct proxy {
    int fd;
    int timeout_ms;
    const struct backend *backend; /* 연결한 백엔드 */
    struct proxy_response resp;
    int has_body; /* HEAD, 204, 304면 0 */
    char
        buf[PROXY_HEAD_MAX]; /* 응답 앞부분. 헤더 뒤에 본문이 딸려 올 수 있음 */
    size_t len;              /* buf에 받은 바이트 수 */
    size_t head_len;         /* 상태 줄 + 헤더 줄 길이 (빈 줄 제외) */
    size_t body_start;       /* buf에서 본문이 시작하는 자리 */
};

enum proxy_result {
    PROXY_DONE,    /* 끝까지 전달 */
    PROXY_ABORTED, /* 전달하다 실패. 손님은 Content-Length로 잘린 것을 앎 */
    PROXY_ABORTED_UNKNOWN_LENGTH /* 길이 없는 응답이 백엔드 쪽에서 끊김.
                                    그냥 닫으면 손님이 완성된 응답으로 봄 */
};

/*
 * backends 중 first번부터 차례로 연결해 요청을 보내고 최종 응답 헤더까지 받습니다.
 *   이 백엔드에 연결하지 못하면(거부, 도달 불가) 다음 백엔드를 시도함. 아직 요청을
 *   보내지 않아 두 번 실행될 위험이 없음. 시간 초과나 우리 쪽 소켓 부족은 다음
 *   백엔드로 풀리지 않으므로 바로 끝냄. 요청을 보낸 뒤의 실패도 다시 시도하지 않음.
 *   연결, 요청 전송, 헤더 받기를 모두 합쳐 timeout_sec초 안에 끝나야 함.
 * 반환: 0, 502 (연결 실패, 소켓 부족, 잘못된 응답), 504 (시간 초과)
 *   0이 아니면 연결은 이미 닫혀 있음 (p->backend는 마지막에 시도한 백엔드)
 */
int proxy_open(struct proxy *p, const struct http_request *req,
               const struct backend *backends, int count, int first,
               int timeout_sec);

/*
 * 응답을 손님에게 전달합니다. 본문은 모으지 않고 받은 만큼 바로 씁니다.
 *   *keep이 1이면 Connection: keep-alive. 길이를 모르는 본문이면 연결을 닫아야
 *   끝을 알릴 수 있으므로 *keep을 0으로 바꿈.
 *   본문은 timeout_sec초 동안 한 바이트도 오지 않으면 실패로 봄.
 */
enum proxy_result proxy_relay(struct proxy *p, int clientfd, int *keep);

void proxy_close(struct proxy *p);

/* ---- 아래는 단위 테스트에서 따로 부르는 조각 ---- */

/* 백엔드로 보낼 요청 줄과 헤더를 만듭니다. 반환: 길이, 넘치면 -1 */
long proxy_request_build(const struct http_request *req, char *dst, size_t cap);

/*
 * 응답 상태 줄과 헤더 줄(빈 줄 제외)을 해석합니다. 성공 0.
 * 형식 오류, 서로 다른 Content-Length, Transfer-Encoding이면 -1.
 */
int proxy_response_parse(const char *head, size_t len,
                         struct proxy_response *resp);

#endif
