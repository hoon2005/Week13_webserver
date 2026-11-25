/*
 * http - 요청 줄, 헤더 해석과 MIME 판단 (m4m5.md 3장)
 */
#include "http.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void http_request_init(struct http_request *req)
{
    memset(req, 0, sizeof(*req));
}

void http_request_free(struct http_request *req)
{
    free(req->body);
    req->body = NULL;
    req->body_len = 0;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t';
}

/* RFC 9110 5.6.2 토큰 글자: 영문자, 숫자, ! # $ % & ' * + - . ^ _ ` | ~ */
static int is_tchar(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
        return 1;
    }
    return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

int http_is_token(const char *s, size_t len)
{
    if (len == 0) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        if (!is_tchar((unsigned char)s[i])) {
            return 0;
        }
    }
    return 1;
}

int http_has_ctl(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f) {
            return 1; /* NUL, 줄 중간의 CR 등 */
        }
    }
    return 0;
}

/*
 * Host 값 검사 (F10, RFC 9112 3.2): 이름[:포트] 또는 [IPv6][:포트].
 *   이름은 RFC 3986 reg-name 글자만 받고, %는 뒤에 16진수 두 자리가 있어야 함.
 *   이름이 비면 거절 (http URI의 호스트는 빌 수 없음, RFC 9110 4.2.1).
 *   [v1.x] 같은 IPvFuture와 [::1%lo] 같은 영역 표시는 받지 않음.
 *   규칙과 테스트 목록은 참고 커밋 7383e40 (phase2/dabin-m6m7)
 */
static int host_is_valid(const char *v)
{
    if (*v == '[') {
        char text[INET6_ADDRSTRLEN];
        struct in6_addr addr;
        size_t n = strspn(++v, "0123456789abcdefABCDEF:.");

        if (v[n] != ']' || n >= sizeof(text)) {
            return 0;
        }
        memcpy(text, v, n);
        text[n] = '\0';
        if (inet_pton(AF_INET6, text, &addr) != 1) {
            return 0;
        }
        v += n + 1;
    } else {
        const char *start = v;

        while (*v != '\0') {
            if (*v == '%') {
                if (!isxdigit((unsigned char)v[1]) ||
                    !isxdigit((unsigned char)v[2])) {
                    return 0;
                }
                v += 3;
            } else if (isalnum((unsigned char)*v) ||
                       strchr("-._~!$&'()*+,;=", *v) != NULL) {
                v++;
            } else {
                break;
            }
        }
        if (v == start) {
            return 0;
        }
    }
    if (*v == ':') { /* 포트는 숫자만. 비어 있어도 됨 (RFC 3986 3.2.3) */
        v++;
        v += strspn(v, "0123456789");
    }
    return *v == '\0';
}

/* 줄 끝의 \r\n 또는 \n을 뺀 길이 */
static size_t line_content_len(const char *line, size_t len)
{
    if (len > 0 && line[len - 1] == '\n') {
        len--;
    }
    if (len > 0 && line[len - 1] == '\r') {
        len--;
    }
    return len;
}

/* "HTTP/x.y" 형식이면 0, 아니면 -1. 맞으면 major, minor를 채움 */
static int version_parse(const char *v, int *major, int *minor)
{
    if (strlen(v) != 8 || strncmp(v, "HTTP/", 5) != 0) {
        return -1;
    }
    if (v[5] < '0' || v[5] > '9' || v[6] != '.' || v[7] < '0' || v[7] > '9') {
        return -1;
    }
    *major = v[5] - '0';
    *minor = v[7] - '0';
    return 0;
}

/*
 * 줄 앞의 메서드가 GET, POST, HEAD 중 하나면 먼저 저장 (F5).
 * 버전이나 URI 검사에서 실패해도 오류 응답이 HEAD인지 알아서 본문을 뺄 수 있음.
 * 모르는 메서드는 저장하지 않음 (아래 검사가 400, 501을 정함).
 */
static void method_store_early(struct http_request *req, const char *line,
                               size_t len)
{
    static const char *const known[] = {"GET", "POST", "HEAD"};
    size_t n = 0;

    while (n < len && http_is_token(line + n, 1)) {
        n++;
    }
    if (n == len || !is_space(line[n])) {
        return; /* 메서드 뒤에 공백이 없으면 요청 줄 형식이 아님 */
    }
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strlen(known[i]) == n && strncasecmp(line, known[i], n) == 0) {
            strcpy(req->method, known[i]);
            return;
        }
    }
}

int http_parse_request_line(struct http_request *req, const char *line,
                            size_t len, size_t maxlen)
{
    const char *tok[3];
    size_t tok_len[3];
    size_t end, i = 0;
    int count = 0;
    int major, minor;

    if (len == 0) {
        return 400;
    }
    method_store_early(req, line, len);
    /* 줄바꿈이 없음: 버퍼가 다 찼으면 너무 긴 줄, 아니면 중간에 끊긴 줄 */
    if (line[len - 1] != '\n') {
        return (len >= maxlen - 1) ? 414 : 400;
    }
    end = line_content_len(line, len);
    if (http_has_ctl(line, end)) {
        return 400; /* 탭이 아닌 제어 문자 (M7-1) */
    }

    /* 공백으로 나눈 조각이 정확히 세 개(메서드, URI, 버전)여야 함 */
    while (i < end) {
        while (i < end && is_space(line[i])) {
            i++;
        }
        if (i == end) {
            break;
        }
        if (count == 3) {
            return 400;
        }
        tok[count] = line + i;
        while (i < end && !is_space(line[i])) {
            i++;
        }
        tok_len[count] = (size_t)(line + i - tok[count]);
        count++;
    }
    if (count != 3) {
        return 400;
    }

    /* 버전: 형식이 틀리면 400, 지원하지 않는 버전이면 505 */
    if (tok_len[2] >= HTTP_VERSION_MAX) {
        return 400;
    }
    memcpy(req->version, tok[2], tok_len[2]);
    req->version[tok_len[2]] = '\0';
    if (version_parse(req->version, &major, &minor) < 0) {
        return 400;
    }
    if (major != 1 || (minor != 0 && minor != 1)) {
        return 505;
    }
    req->minor = minor;

    /* URI: '/'로 시작해야 함 (M7-1). 정적 경로는 "." + URI라 "./x"는 "../x"가 됨 */
    if (tok[1][0] != '/') {
        return 400;
    }
    /* 요청 줄 전체가 HTTP_LINE_MAX 안이므로 넘치지 않음 */
    if (tok_len[1] >= sizeof(req->uri)) {
        return 414;
    }
    memcpy(req->uri, tok[1], tok_len[1]);
    req->uri[tok_len[1]] = '\0';

    /* 메서드: 토큰이 아니면 400, 형식은 맞지만 GET, POST, HEAD가 아니면 501 (M7-2).
     * 대소문자는 Phase 1처럼 구분하지 않음 */
    if (!http_is_token(tok[0], tok_len[0])) {
        return 400;
    }
    if (tok_len[0] >= HTTP_METHOD_MAX) {
        return 501;
    }
    memcpy(req->method, tok[0], tok_len[0]);
    req->method[tok_len[0]] = '\0';
    if (strcasecmp(req->method, "GET") == 0) {
        strcpy(req->method, "GET");
    } else if (strcasecmp(req->method, "POST") == 0) {
        strcpy(req->method, "POST");
    } else if (strcasecmp(req->method, "HEAD") == 0) {
        strcpy(req->method, "HEAD");
    } else {
        return 501;
    }
    return 0;
}

int http_parse_content_length(const char *value, long *out)
{
    long n = 0;

    if (*value == '\0') {
        return -1;
    }
    for (const char *p = value; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return -1; /* 숫자가 아님, 음수(-) 포함 */
        }
        if (n > (LONG_MAX - (*p - '0')) / 10) {
            n = LONG_MAX; /* 너무 큰 값: 그대로 두면 상한 검사에서 413 */
        } else {
            n = n * 10 + (*p - '0');
        }
    }
    *out = n;
    return 0;
}

/*
 * Connection 값("keep-alive", "close", "Upgrade, close" 등)을 해석.
 * 복사하지 않고 값 전체를 쉼표로 나눠 훑음: 받아들인 값(헤더 한 줄 8 KiB까지)은
 * 길이와 상관없이 끝까지 봐서 뒤쪽의 close도 놓치지 않음 (F11, RFC 9112 9.6)
 */
static int connection_parse(const char *value)
{
    int result = HTTP_CONN_NONE;
    const char *p = value;

    while (*p != '\0') {
        const char *t, *e;

        while (*p == ',' || is_space(*p)) {
            p++;
        }
        t = p;
        while (*p != '\0' && *p != ',') {
            p++;
        }
        e = p;
        while (e > t && is_space(e[-1])) {
            e--;
        }
        if ((size_t)(e - t) == 5 && strncasecmp(t, "close", 5) == 0) {
            return HTTP_CONN_CLOSE; /* close가 있으면 무조건 닫음 */
        }
        if ((size_t)(e - t) == 10 && strncasecmp(t, "keep-alive", 10) == 0) {
            result = HTTP_CONN_KEEP_ALIVE;
        }
    }
    return result;
}

int http_parse_header_line(struct http_request *req, const char *line,
                           size_t len, size_t maxlen)
{
    char name[HTTP_LINE_MAX];
    char value[HTTP_LINE_MAX];
    const char *colon;
    size_t end, name_len, vstart, vend;

    (void)maxlen;
    if (len == 0 || line[len - 1] != '\n') {
        return 400; /* 줄이 MAXLINE보다 길거나 중간에 끊김 */
    }
    end = line_content_len(line, len);
    if (end == 0) {
        return 1; /* 빈 줄: 헤더 끝 */
    }
    if (http_has_ctl(line, end)) {
        return 400; /* 한 줄로 본 헤더를 백엔드가 두 줄로 나누지 않게 (M7-1) */
    }

    /* 원본 헤더 블록에 이어 붙임. 차면 그 자체가 헤더 전체 8 KiB 상한 */
    if (req->raw_len + len > sizeof(req->raw_headers)) {
        return 400;
    }
    memcpy(req->raw_headers + req->raw_len, line, len);
    req->raw_len += len;

    colon = memchr(line, ':', end);
    if (colon == NULL || colon == line) {
        return 400; /* ':'가 없거나 이름이 비었음 */
    }
    name_len = (size_t)(colon - line);
    if (!http_is_token(line, name_len)) {
        return 400; /* 이름은 토큰만. ':' 앞의 공백도 여기서 걸림 (M7-1) */
    }
    memcpy(name, line, name_len);
    name[name_len] = '\0';

    /* 값 앞뒤 공백 제거 */
    vstart = name_len + 1;
    vend = end;
    while (vstart < vend && is_space(line[vstart])) {
        vstart++;
    }
    while (vend > vstart && is_space(line[vend - 1])) {
        vend--;
    }
    memcpy(value, line + vstart, vend - vstart);
    value[vend - vstart] = '\0';

    if (strcasecmp(name, "Host") == 0) {
        req->host_count++;
        if (strlen(value) >= sizeof(req->host) || !host_is_valid(value)) {
            return 400;
        }
        strcpy(req->host, value);
    } else if (strcasecmp(name, "Content-Length") == 0) {
        long n;
        if (http_parse_content_length(value, &n) < 0) {
            return 400;
        }
        if (req->has_content_length && req->content_length != n) {
            return 400; /* 서로 다른 값이 여러 번 */
        }
        req->content_length = n;
        req->has_content_length = 1;
    } else if (strcasecmp(name, "Connection") == 0) {
        int c = connection_parse(value);
        /* Connection 헤더가 여러 번 오면 close가 하나라도 있으면 close */
        if (c == HTTP_CONN_CLOSE) {
            req->connection = HTTP_CONN_CLOSE;
        } else if (c == HTTP_CONN_KEEP_ALIVE &&
                   req->connection != HTTP_CONN_CLOSE) {
            req->connection = HTTP_CONN_KEEP_ALIVE;
        }
    } else if (strcasecmp(name, "Content-Type") == 0) {
        if (strlen(value) >= sizeof(req->content_type)) {
            return 400;
        }
        strcpy(req->content_type, value);
    } else if (strcasecmp(name, "Transfer-Encoding") == 0) {
        req->has_transfer_encoding = 1;
    } else if (strcasecmp(name, "Expect") == 0) {
        req->expect = (strcasecmp(value, "100-continue") == 0)
                          ? HTTP_EXPECT_CONTINUE
                          : HTTP_EXPECT_OTHER;
    }
    /* 모르는 헤더는 무시 (원본 블록에는 남음) */
    return 0;
}

int http_finish_headers(struct http_request *req)
{
    /* HTTP/1.1은 Host가 정확히 하나. 1.0은 없어도 되지만 중복은 거절 */
    if (req->host_count > 1 || (req->minor == 1 && req->host_count == 0)) {
        return 400;
    }
    /* CL과 TE가 같이 오면 본문 끝을 믿을 수 없음 (request smuggling) */
    if (req->has_content_length && req->has_transfer_encoding) {
        return 400;
    }
    if (req->has_transfer_encoding) {
        return 501; /* chunked 미지원 */
    }
    /* 본문을 받기 전에(malloc 전에) 상한 검사 */
    if (req->has_content_length && req->content_length > BODY_MAX) {
        return 413;
    }
    /* HTTP/1.0의 Expect는 무시 */
    if (req->minor == 0) {
        req->expect = HTTP_EXPECT_NONE;
    } else if (req->expect == HTTP_EXPECT_OTHER) {
        return 417;
    }

    if (req->minor == 1) {
        req->keep_alive = (req->connection != HTTP_CONN_CLOSE);
    } else {
        req->keep_alive = (req->connection == HTTP_CONN_KEEP_ALIVE);
    }
    return 0;
}

long http_body_length(const struct http_request *req)
{
    /* CL과 TE가 모두 없으면 본문 0바이트 (RFC 9112 6.3) */
    return req->has_content_length ? req->content_length : 0;
}

struct mime_entry {
    const char *ext;
    const char *type;
};

static const struct mime_entry MIME_TYPES[] = {
    {"html", "text/html"}, {"css", "text/css"},   {"js", "text/javascript"},
    {"png", "image/png"},  {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
};

const char *http_mime_type(const char *filename)
{
    const char *slash = strrchr(filename, '/');
    const char *base = slash ? slash + 1 : filename;
    const char *dot = strrchr(base, '.'); /* 마지막 확장자만 봄 */

    if (dot != NULL) {
        for (size_t i = 0; i < sizeof(MIME_TYPES) / sizeof(MIME_TYPES[0]);
             i++) {
            if (strcasecmp(dot + 1, MIME_TYPES[i].ext) == 0) {
                return MIME_TYPES[i].type;
            }
        }
    }
    return "text/plain";
}

const char *http_status_text(int status)
{
    switch (status) {
    case 100:
        return "Continue";
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 411:
        return "Length Required";
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
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "Unknown";
    }
}
