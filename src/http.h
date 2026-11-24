/*
 * http.h - HTTP 요청 파서와 응답에 쓰는 표 (final.md 3장)
 *
 *   파서는 소켓을 모릅니다. tiny.c가 소켓에서 한 줄씩 읽어 넘기면, 줄 문자열만
 *   보고 struct http_request를 채웁니다. 그래서 tests/http_test.c가 소켓 없이
 *   문자열만으로 파서를 검사할 수 있습니다.
 *
 *   한 요청을 읽는 순서 (tiny.c의 request_read)
 *     1. http_request_init
 *     2. 요청 줄: http_line_check → http_request_line_parse
 *     3. 헤더 줄: 빈 줄이 올 때까지 http_line_check → http_header_line_parse
 *     4. 빈 줄 뒤: http_headers_validate (Host, Content-Length, Transfer-Encoding)
 *
 *   parse/validate 함수는 성공하면 0, 실패하면 보낼 HTTP 상태 코드(400, 501,
 *   505)를 돌려줍니다. 이 오류들은 모두 요청이 어디서 끝나는지 믿을 수 없는
 *   경우라, 응답 뒤에 연결을 닫습니다 (final.md 5.2).
 */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <sys/types.h>

#include "csapp.h" /* MAXLINE: 요청 줄과 헤더 줄 한 줄의 최대 길이 */

#define HDR_RAW_MAX 8192   /* 헤더 전체(원본 블록) 상한. 넘으면 400 */
#define BODY_MAX (1 << 20) /* 요청 본문 상한 1 MiB. M5-2에서 씀 */

/* Expect 헤더 값 (M5-2에서 100 Continue / 417 판단에 씀) */
enum http_expect {
    HTTP_EXPECT_NONE,         /* 헤더 없음 */
    HTTP_EXPECT_100_CONTINUE, /* "100-continue" (대소문자 무시) */
    HTTP_EXPECT_OTHER,        /* 그 밖의 값 */
};

/* 소켓에서 읽은 한 줄의 상태 (http_line_check) */
enum http_line_state {
    HTTP_LINE_OK,         /* 줄바꿈까지 다 읽음 */
    HTTP_LINE_INCOMPLETE, /* 줄바꿈 전에 끊김·읽기 오류·타임아웃 → 응답 없이 닫음 */
    HTTP_LINE_TOO_LONG, /* 버퍼가 찰 때까지 줄바꿈이 없음 → 414(요청 줄) / 400(헤더) */
};

struct http_request {
    /* 요청 줄 */
    char method[16]; /* "GET", "POST", "HEAD" 중 하나 (그 외는 501) */
    char uri[MAXLINE];
    char version[16]; /* "HTTP/1.0" 또는 "HTTP/1.1" */
    int minor;        /* 버전의 뒷자리: 0 또는 1 */

    /* 서버가 쓰는 헤더 필드 (final.md 3.2) */
    char host[256];
    int has_host;
    char content_type
        [256];           /* 해석하지 않고 CGI에 CONTENT_TYPE으로 넘김 (M5-2) */
    long content_length; /* has_content_length가 0이면 의미 없음 */
    int has_content_length;
    int has_transfer_encoding;
    enum http_expect expect;
    int conn_close;      /* Connection 헤더에 close 토큰이 있음 */
    int conn_keep_alive; /* Connection 헤더에 keep-alive 토큰이 있음 */
    int keep_alive; /* 버전 + Connection으로 정한 값 (http_headers_validate) */

    /* 원본 헤더 블록: 헤더 줄을 받은 그대로 이어 붙임. M6 프록시가 그대로 전달할
     * 때 씀. 이 버퍼가 차는 것이 곧 "헤더 전체 8 KiB 상한"이다 */
    char raw_headers[HDR_RAW_MAX];
    size_t raw_len;

    char *body; /* M5-2에서 malloc. 요청이 끝나면 free */
};

/* 구조체를 비웁니다. 요청마다 한 번 (keep-alive에서는 요청마다 다시) */
void http_request_init(struct http_request *req);

/* rio_readlineb(buf, bufsize)가 돌려준 n을 보고 줄 상태를 판단합니다 */
enum http_line_state http_line_check(const char *buf, ssize_t n,
                                     size_t bufsize);

/* "\r\n" 또는 "\n"만 있는 줄(헤더 끝)이면 1 */
int http_is_blank_line(const char *line, size_t len);

/* 요청 줄 "메서드 URI 버전\r\n"을 나눠 req에 넣습니다. 성공 0, 실패 400/501/505 */
int http_request_line_parse(struct http_request *req, const char *line,
                            size_t len);

/* 헤더 줄 하나를 원본 블록에 붙이고, 아는 헤더면 필드에 저장합니다.
 * 빈 줄은 넘기지 않습니다 (http_is_blank_line으로 먼저 확인). 성공 0, 실패 400 */
int http_header_line_parse(struct http_request *req, const char *line,
                           size_t len);

/* 헤더를 다 읽은 뒤 서로 얽힌 규칙을 확인하고 keep_alive를 정합니다.
 * 성공 0, 실패 400(Host, CL+TE) / 501(TE만) */
int http_headers_validate(struct http_request *req);

/* 파일 경로의 마지막 확장자로 Content-Type 값을 정합니다 (final.md 3.7) */
const char *http_mime_type(const char *path);

/* 상태 코드의 이유 문구. 예: 404 → "Not Found" */
const char *http_status_text(int status);

#endif
