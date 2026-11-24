/*
 * http - 요청 줄, 헤더 해석과 MIME 판단 (final.md 3장)
 */
#include "http.h"

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
    /* 줄바꿈이 없음: 버퍼가 다 찼으면 너무 긴 줄, 아니면 중간에 끊긴 줄 */
    if (line[len - 1] != '\n') {
        return (len >= maxlen - 1) ? 414 : 400;
    }
    end = line_content_len(line, len);

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

    /* URI (요청 줄 전체가 HTTP_LINE_MAX 안이므로 넘치지 않음) */
    if (tok_len[1] >= sizeof(req->uri)) {
        return 414;
    }
    memcpy(req->uri, tok[1], tok_len[1]);
    req->uri[tok_len[1]] = '\0';

    /* 메서드: GET, POST, HEAD만. 대소문자는 Phase 1처럼 구분하지 않음 */
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

/* Connection 값("keep-alive", "close", "Upgrade, close" 등)을 해석 */
static int connection_parse(const char *value)
{
    char buf[HTTP_FIELD_MAX];
    char *save = NULL;
    int result = HTTP_CONN_NONE;

    size_t n = strlen(value);

    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1; /* 아주 긴 값은 앞부분만 봄 */
    }
    memcpy(buf, value, n);
    buf[n] = '\0';
    for (char *t = strtok_r(buf, ",", &save); t != NULL;
         t = strtok_r(NULL, ",", &save)) {
        char *e;
        while (is_space(*t)) {
            t++;
        }
        e = t + strlen(t);
        while (e > t && is_space(e[-1])) {
            *--e = '\0';
        }
        if (strcasecmp(t, "close") == 0) {
            return HTTP_CONN_CLOSE; /* close가 있으면 무조건 닫음 */
        }
        if (strcasecmp(t, "keep-alive") == 0) {
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
    for (size_t i = 0; i < name_len; i++) {
        if (is_space(line[i])) {
            return 400; /* 이름 안이나 ':' 앞의 공백은 허용하지 않음 */
        }
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
        if (strlen(value) >= sizeof(req->host)) {
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
