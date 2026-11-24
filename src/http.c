/*
 * http.c - HTTP 요청 파서 (설명은 http.h)
 *
 * 규칙의 근거
 *   요청 줄 (final.md 3.3): 길이 414 → 세 부분 400 → 버전 형식 400 → 버전 505
 *                           → 메서드 501 순서로 확인
 *   헤더 (3.2): 이름은 대소문자 무시, 값은 앞뒤 공백 제거, 모르는 헤더는 무시
 *   본문의 끝 (3.4): Content-Length만 지원. TE만 오면 501, CL+TE면 400
 *   그 밖에 RFC 9112를 따른 것은 엣지 케이스 문서(phase2-edge-cases-http.md)의
 *   "추가" 항목입니다 (공백 여러 개, 이름 뒤 공백, 줄 접기, NUL 등은 400).
 */
#include "http.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>
#include <strings.h>

void http_request_init(struct http_request *req)
{
    memset(req, 0, sizeof(*req)); /* 문자열 필드는 모두 "", 숫자·플래그는 0 */
    req->expect = HTTP_EXPECT_NONE;
    req->body = NULL;
}

enum http_line_state http_line_check(const char *buf, ssize_t n, size_t bufsize)
{
    if (n <= 0) { /* 0: 아무것도 못 읽고 끊김, -1: 읽기 오류 또는 타임아웃 */
        return HTTP_LINE_INCOMPLETE;
    }
    if (buf[n - 1] == '\n') {
        return HTTP_LINE_OK;
    }
    /* rio_readlineb는 최대 bufsize - 1 글자를 읽음. 그만큼 찼는데 줄바꿈이 없으면
     * 줄이 버퍼보다 긴 것이고, 덜 찼는데 없으면 중간에 끊긴 것 */
    if ((size_t)n >= bufsize - 1) {
        return HTTP_LINE_TOO_LONG;
    }
    return HTTP_LINE_INCOMPLETE;
}

/* 줄 끝의 "\n" 또는 "\r\n"을 뺀 길이 */
static size_t line_content_len(const char *line, size_t len)
{
    if (len > 0 && line[len - 1] == '\n') {
        len--;
        if (len > 0 && line[len - 1] == '\r') {
            len--;
        }
    }
    return len;
}

int http_is_blank_line(const char *line, size_t len)
{
    return len > 0 && line_content_len(line, len) == 0;
}

/* 내용 안에 NUL이나 줄 끝이 아닌 CR이 있으면 1. 문자열 함수가 거기서 끊기거나
 * 헤더를 다르게 나누는 일을 막으려고 거절함 */
static int has_bad_bytes(const char *s, size_t n)
{
    return memchr(s, '\0', n) != NULL || memchr(s, '\r', n) != NULL;
}

int http_request_line_parse(struct http_request *req, const char *line,
                            size_t len)
{
    size_t n = line_content_len(line, len);
    const char *end = line + n;
    const char *sp1, *sp2;
    const char *uri, *ver;
    size_t mlen, ulen, vlen;

    if (has_bad_bytes(line, n)) {
        return 400;
    }

    /* 정확히 공백 두 개로 세 부분: "GET /x HTTP/1.1".
     * 공백이 하나뿐이거나(두 부분), 세 개 이상이거나(네 부분 또는 공백 여러 개) → 400 */
    sp1 = memchr(line, ' ', n);
    if (sp1 == NULL) {
        return 400;
    }
    sp2 = memchr(sp1 + 1, ' ', (size_t)(end - (sp1 + 1)));
    if (sp2 == NULL || memchr(sp2 + 1, ' ', (size_t)(end - (sp2 + 1)))) {
        return 400;
    }
    mlen = (size_t)(sp1 - line);
    uri = sp1 + 1;
    ulen = (size_t)(sp2 - uri);
    ver = sp2 + 1;
    vlen = (size_t)(end - ver);
    if (mlen == 0 || ulen == 0 ||
        vlen == 0) { /* 공백이 붙어 있어 빈 부분이 생김 */
        return 400;
    }

    /* 버전 형식: "HTTP/" + 숫자 + "." + 숫자. "HTTP/1", "http/1.1"은 형식 오류 400 */
    if (vlen != 8 || memcmp(ver, "HTTP/", 5) != 0 ||
        !isdigit((unsigned char)ver[5]) || ver[6] != '.' ||
        !isdigit((unsigned char)ver[7])) {
        return 400;
    }
    memcpy(req->version, ver, vlen);
    req->version[vlen] = '\0';
    /* 형식은 맞지만 1.0, 1.1이 아님 (2.0, 0.9 등) → 505 */
    if (ver[5] != '1' || (ver[7] != '0' && ver[7] != '1')) {
        return 505;
    }
    req->minor = ver[7] - '0';

    /* 메서드: 대소문자를 구분 (RFC 9110). "get"도 501.
     * 배열보다 길면 GET, POST, HEAD일 수 없으므로 501 */
    if (mlen >= sizeof(req->method)) {
        return 501;
    }
    memcpy(req->method, line, mlen);
    req->method[mlen] = '\0';
    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "POST") != 0 &&
        strcmp(req->method, "HEAD") != 0) {
        return 501;
    }

    /* URI는 '/'로 시작하는 경로만 (http://... 같은 전체 주소 형식은 받지 않음) */
    if (uri[0] != '/') {
        return 400;
    }
    /* 줄 전체가 MAXLINE보다 짧으므로 uri도 항상 들어감 */
    memcpy(req->uri, uri, ulen);
    req->uri[ulen] = '\0';
    return 0;
}

/* 헤더 이름(길이 nlen)이 name과 같은지 (대소문자 무시) */
static int name_is(const char *s, size_t nlen, const char *name)
{
    return strlen(name) == nlen && strncasecmp(s, name, nlen) == 0;
}

/* 값을 dst(크기 cap)에 복사. 들어가지 않으면 자르지 않고 -1 */
static int value_copy(char *dst, size_t cap, const char *v, size_t vlen)
{
    if (vlen >= cap) {
        return -1;
    }
    memcpy(dst, v, vlen);
    dst[vlen] = '\0';
    return 0;
}

/* Content-Length 값: 숫자만, 비어 있지 않음, long 범위 안. "+7", "7abc", 음수 → -1 */
static int content_length_parse(const char *v, size_t vlen, long *out)
{
    long x = 0;

    if (vlen == 0) {
        return -1;
    }
    for (size_t i = 0; i < vlen; i++) {
        if (!isdigit((unsigned char)v[i])) {
            return -1;
        }
        if (x > (LONG_MAX - (v[i] - '0')) / 10) { /* 다음 자리에서 넘침 */
            return -1;
        }
        x = x * 10 + (v[i] - '0');
    }
    *out = x;
    return 0;
}

/* Connection 값은 쉼표로 나뉜 토큰 목록: "keep-alive, Upgrade" */
static void connection_tokens_parse(struct http_request *req, const char *v,
                                    size_t vlen)
{
    size_t i = 0;

    while (i < vlen) {
        size_t start, stop;

        while (i < vlen && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) {
            i++;
        }
        start = i;
        while (i < vlen && v[i] != ',') {
            i++;
        }
        stop = i;
        while (stop > start && (v[stop - 1] == ' ' || v[stop - 1] == '\t')) {
            stop--;
        }
        if (name_is(v + start, stop - start, "close")) {
            req->conn_close = 1;
        } else if (name_is(v + start, stop - start, "keep-alive")) {
            req->conn_keep_alive = 1;
        }
    }
}

int http_header_line_parse(struct http_request *req, const char *line,
                           size_t len)
{
    size_t n;
    const char *colon, *v, *vend;
    size_t nlen, vlen;
    long cl;

    /* 원본 블록에 받은 그대로(줄바꿈 포함) 이어 붙임. 넘치면 헤더 전체가 8 KiB 초과 */
    if (req->raw_len + len > sizeof(req->raw_headers)) {
        return 400;
    }
    memcpy(req->raw_headers + req->raw_len, line, len);
    req->raw_len += len;

    n = line_content_len(line, len);
    if (n == 0 || has_bad_bytes(line, n)) {
        return 400;
    }
    /* 공백으로 시작하는 줄 = 앞 헤더를 잇는 옛 문법(줄 접기). RFC 9112 §5.2: 거절 */
    if (line[0] == ' ' || line[0] == '\t') {
        return 400;
    }
    colon = memchr(line, ':', n);
    if (colon == NULL || colon == line) { /* ':'가 없거나 이름이 비어 있음 */
        return 400;
    }
    nlen = (size_t)(colon - line);
    /* "Host : x"처럼 이름 뒤에 공백 → RFC 9112 §5.1: 거절 */
    if (memchr(line, ' ', nlen) || memchr(line, '\t', nlen)) {
        return 400;
    }

    /* 값 앞뒤 공백·탭 제거 */
    v = colon + 1;
    vend = line + n;
    while (v < vend && (*v == ' ' || *v == '\t')) {
        v++;
    }
    while (vend > v && (vend[-1] == ' ' || vend[-1] == '\t')) {
        vend--;
    }
    vlen = (size_t)(vend - v);

    if (name_is(line, nlen, "Host")) {
        /* 중복이면 어느 쪽을 믿을지 모름, 비어 있으면 쓸 수 없음 */
        if (req->has_host || vlen == 0 ||
            value_copy(req->host, sizeof(req->host), v, vlen) < 0) {
            return 400;
        }
        req->has_host = 1;
    } else if (name_is(line, nlen, "Content-Length")) {
        if (content_length_parse(v, vlen, &cl) < 0) {
            return 400;
        }
        /* 같은 값이 두 번이면 받아들이고, 다른 값이면 본문 끝을 믿을 수 없음 */
        if (req->has_content_length && req->content_length != cl) {
            return 400;
        }
        req->content_length = cl;
        req->has_content_length = 1;
    } else if (name_is(line, nlen, "Transfer-Encoding")) {
        req->has_transfer_encoding = 1; /* 값과 상관없이 지원하지 않음 */
    } else if (name_is(line, nlen, "Content-Type")) {
        if (value_copy(req->content_type, sizeof(req->content_type), v, vlen) <
            0) {
            return 400;
        }
    } else if (name_is(line, nlen, "Connection")) {
        connection_tokens_parse(req, v, vlen);
    } else if (name_is(line, nlen, "Expect")) {
        req->expect = name_is(v, vlen, "100-continue")
                          ? HTTP_EXPECT_100_CONTINUE
                          : HTTP_EXPECT_OTHER;
    }
    /* 모르는 헤더는 원본 블록에만 남기고 무시 */
    return 0;
}

int http_headers_validate(struct http_request *req)
{
    /* HTTP/1.1은 Host가 필수 (RFC 9112 §3.2). 1.0은 없어도 됨 */
    if (req->minor == 1 && !req->has_host) {
        return 400;
    }
    /* 둘 다 오면 서버마다 다른 쪽을 믿어 본문 끝이 달라짐 (request smuggling) */
    if (req->has_content_length && req->has_transfer_encoding) {
        return 400;
    }
    if (req->has_transfer_encoding) { /* chunked 등은 이번 범위 밖 */
        return 501;
    }
    /* 1.1은 기본이 유지, 1.0은 기본이 닫기. close가 있으면 항상 닫기 */
    if (req->minor == 1) {
        req->keep_alive = !req->conn_close;
    } else {
        req->keep_alive = req->conn_keep_alive && !req->conn_close;
    }
    return 0;
}

/* 확장자(점 뒤)와 MIME 타입. 대소문자를 구분 (지금 동작 유지) */
static const struct {
    const char *ext;
    const char *mime;
} MIME_TYPES[] = {
    {"html", "text/html"}, {"css", "text/css"},   {"js", "text/javascript"},
    {"png", "image/png"},  {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
};

const char *http_mime_type(const char *path)
{
    const char *base = strrchr(path, '/');
    const char *dot;

    /* 폴더 이름의 점(/v1.2/readme)은 보지 않도록 마지막 '/' 뒤만 봄 */
    base = base ? base + 1 : path;
    /* 마지막 점 뒤가 확장자. a.css.txt → txt. ".hidden"처럼 점으로 시작하면 확장자 없음 */
    dot = strrchr(base, '.');
    if (dot == NULL || dot == base) {
        return "text/plain";
    }
    for (size_t i = 0; i < sizeof(MIME_TYPES) / sizeof(MIME_TYPES[0]); i++) {
        if (strcmp(dot + 1, MIME_TYPES[i].ext) == 0) {
            return MIME_TYPES[i].mime;
        }
    }
    return "text/plain"; /* 표에 없으면 text/plain (Tiny와 같음) */
}

const char *http_status_text(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 413:
        return "Content Too Large";
    case 414:
        return "URI Too Long";
    case 417:
        return "Expectation Failed";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 504:
        return "Gateway Timeout";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "Error";
    }
}
