/*
 * http - 요청 줄과 헤더를 해석해 요청 구조체를 채웁니다 (final.md 3장).
 *   파서는 소켓을 모르고 문자열만 받습니다. 그래서 단위 테스트
 *   (tests/http_test.c)에서 소켓 없이 검증할 수 있습니다.
 *
 *   파서 함수는 성공하면 0, 실패하면 보낼 HTTP 상태 코드(400, 414, ...)를
 *   돌려줍니다.
 */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define HTTP_LINE_MAX 8192  /* 요청 줄, 헤더 한 줄 상한 (csapp의 MAXLINE) */
#define HDR_RAW_MAX 8192    /* 헤더 전체 상한 */
#define BODY_MAX (1L << 20) /* 본문 상한 1 MiB */
#define HTTP_METHOD_MAX 16
#define HTTP_VERSION_MAX 16
#define HTTP_FIELD_MAX 256

enum http_expect {
    HTTP_EXPECT_NONE = 0,
    HTTP_EXPECT_CONTINUE, /* 100-continue */
    HTTP_EXPECT_OTHER     /* 그 외 값 → 417 */
};

enum http_connection {
    HTTP_CONN_NONE = 0, /* 헤더 없음 → 버전 기본값 */
    HTTP_CONN_CLOSE,
    HTTP_CONN_KEEP_ALIVE
};

struct http_request {
    char method[HTTP_METHOD_MAX]; /* GET, POST, HEAD (대문자로 저장) */
    char uri[HTTP_LINE_MAX];
    char version[HTTP_VERSION_MAX];
    int minor; /* HTTP/1.0이면 0, HTTP/1.1이면 1 */

    /* 서버가 쓰는 필드 */
    char host[HTTP_FIELD_MAX];
    int host_count; /* 1.1에서 0개나 2개 이상이면 400 */
    char content_type[HTTP_FIELD_MAX];
    long content_length; /* has_content_length가 0이면 의미 없음 */
    int has_content_length;
    int has_transfer_encoding;
    int expect;     /* enum http_expect */
    int connection; /* enum http_connection */
    int keep_alive; /* 버전과 Connection 헤더로 결정 (http_finish_headers) */

    /* 원본 헤더 블록: M6 프록시가 백엔드로 그대로 전달 */
    char raw_headers[HDR_RAW_MAX];
    size_t raw_len;

    char *body; /* malloc. http_request_free로 해제 */
    size_t body_len;
};

/* 구조체를 비웁니다. keep-alive에서 요청마다 부릅니다 */
void http_request_init(struct http_request *req);

/* 본문 메모리를 해제합니다 */
void http_request_free(struct http_request *req);

/*
 * 요청 줄 하나(줄바꿈 포함, len바이트)를 해석합니다.
 *   maxlen은 읽을 때 쓴 버퍼 크기입니다. 줄바꿈 없이 maxlen - 1바이트가
 *   찼으면 너무 긴 요청 줄(414), 그보다 짧은데 줄바꿈이 없으면 400입니다.
 * 반환: 0, 400, 414, 501, 505
 */
int http_parse_request_line(struct http_request *req, const char *line,
                            size_t len, size_t maxlen);

/*
 * 헤더 한 줄(줄바꿈 포함)을 해석합니다.
 * 반환: 0 (헤더 하나 저장), 1 (빈 줄 = 헤더 끝), 400
 */
int http_parse_header_line(struct http_request *req, const char *line,
                           size_t len, size_t maxlen);

/*
 * 빈 줄까지 읽은 뒤 헤더 전체를 검사하고 keep_alive를 정합니다.
 * 반환: 0, 400 (Host, CL+TE), 413 (본문 상한), 417 (Expect), 501 (TE만)
 */
int http_finish_headers(struct http_request *req);

/* 읽어야 할 본문 바이트 수 (Content-Length가 없으면 0) */
long http_body_length(const struct http_request *req);

/* Content-Length 값 해석: 0 이상 정수만. 성공 0, 실패 -1 */
int http_parse_content_length(const char *value, long *out);

/*
 * 탭이 아닌 ASCII 제어 문자가 있으면 1. 요청 줄, 헤더 줄, 백엔드 상태 줄에 씁니다.
 *   NUL: C 문자열이 거기서 끝나 뒤를 못 봄 ("7\0abc"를 7로 읽음)
 *   CR:  백엔드가 줄을 우리와 다르게 나누면 요청 경계가 어긋남
 *   그 밖: 헤더 값과 URI에 쓸 수 없는 글자 (RFC 9110 5.5). 백엔드의 파서가
 *          우리와 다르게 읽을 수 있음
 */
int http_has_control(const char *s, size_t n);

/*
 * 헤더 블록("이름: 값" 줄이 이어진 것, 빈 줄 제외)에서 pos 위치의 헤더 하나를
 * 읽고 pos를 다음 줄로 옮깁니다. 이름과 값은 블록 안을 가리키며 NUL로 끝나지
 * 않습니다. CGI 출력, 백엔드 응답, 손님의 원본 헤더에 같이 씁니다.
 * 반환: 1 (하나 읽음), 0 (끝), -1 (형식 오류)
 */
struct http_header {
    const char *line; /* 이름부터 값 끝까지 (줄바꿈 제외) */
    size_t line_len;
    const char *name;
    size_t name_len;
    const char *value; /* 앞뒤 공백을 뺀 값 */
    size_t value_len;
};
int http_header_next(const char *block, size_t len, size_t *pos,
                     struct http_header *out);

/* 헤더 이름이 name과 같으면 1 (대소문자 무시) */
int http_header_is(const struct http_header *h, const char *name);

/*
 * 다음 구간으로 넘길 헤더만 "이름: 값\r\n"으로 dst에 복사합니다 (RFC 9110 7.6.1).
 *   빼는 것: 한 구간 전용 헤더(Connection, Keep-Alive, Transfer-Encoding 등),
 *           Connection 값에 적힌 헤더, skip(NULL로 끝나는 배열)에 적은 헤더
 * 반환: 복사한 길이, 형식 오류이거나 dst가 모자라면 -1
 */
long http_headers_filter(const char *block, size_t len, const char *const *skip,
                         char *dst, size_t cap);

/* 파일 이름의 마지막 확장자로 Content-Type을 정합니다 */
const char *http_mime_type(const char *filename);

/* 상태 코드의 기본 문구 (예: 404 → "Not Found") */
const char *http_status_text(int status);

#endif
