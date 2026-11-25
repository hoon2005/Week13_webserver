/*
 * proxy - Reverse Proxy (docs/prd/m6m7.md 2장)
 *
 * 요청 하나의 중계 순서 (proxy_forward)
 *   1. 백엔드로 보낼 요청 헤더 만들기 (HTTP/1.0, 홉 단위 헤더 제거)
 *   2. Round-Robin 순번부터 백엔드에 연결. 연결 실패는 다음 백엔드로
 *   3. 요청 헤더 + 본문 보내기
 *   4. 응답 헤더를 8 KiB까지 모아 확인 (중간 응답 100, 103은 버림)
 *   5. 클라이언트에 상태 줄과 헤더를 보내고, 본문은 받는 대로 전달
 * 연결부터 4까지 합계 timeout초, 본문은 새 데이터 간격 timeout초입니다.
 * 백엔드 소켓은 논블로킹으로 두고 기다림은 모두 poll로 합니다.
 */
#include "proxy.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ============================== 규칙 해석과 라우팅 ============================== */

static int backend_parse(struct proxy_backend *b, const char *s, size_t len,
                         int listen_port, char *err, size_t errlen);
static int addr_is_local(const struct sockaddr_in *addr);

void proxy_table_init(struct proxy_table *t)
{
    memset(t, 0, sizeof(*t));
}

int proxy_route_add(struct proxy_table *t, const char *spec, int listen_port,
                    char *err, size_t errlen)
{
    const char *eq = strchr(spec, '=');
    struct proxy_route *r;
    const char *p;
    size_t plen;

    if (eq == NULL || eq == spec || eq[1] == '\0') {
        snprintf(err, errlen, "형식은 접두사=이름:포트[,이름:포트...]입니다");
        return -1;
    }
    if (spec[0] != '/') { /* 끝의 '/'를 떼기 전에 검사 */
        snprintf(err, errlen, "접두사는 '/'로 시작해야 합니다");
        return -1;
    }
    plen = (size_t)(eq - spec);
    while (plen > 0 && spec[plen - 1] == '/') {
        plen--; /* "/api/" → "/api", "/" → "" */
    }
    if (plen >= PROXY_PREFIX_MAX) {
        snprintf(err, errlen, "접두사가 너무 깁니다");
        return -1;
    }
    for (int i = 0; i < t->count; i++) {
        if (t->routes[i].prefix_len == plen &&
            memcmp(t->routes[i].prefix, spec, plen) == 0) {
            snprintf(err, errlen, "같은 접두사가 두 번 나옵니다");
            return -1;
        }
    }
    if (t->count == PROXY_ROUTES_MAX) {
        snprintf(err, errlen, "규칙은 최대 %d개입니다", PROXY_ROUTES_MAX);
        return -1;
    }

    r = &t->routes[t->count];
    memset(r, 0, sizeof(*r));
    memcpy(r->prefix, spec, plen);
    r->prefix[plen] = '\0';
    r->prefix_len = plen;

    p = eq + 1;
    while (1) { /* 쉼표로 나눈 백엔드 */
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);

        if (r->backend_count == PROXY_BACKENDS_MAX) {
            snprintf(err, errlen, "규칙당 백엔드는 최대 %d개입니다",
                     PROXY_BACKENDS_MAX);
            return -1;
        }
        if (backend_parse(&r->backends[r->backend_count], p, len, listen_port,
                          err, errlen) < 0) {
            return -1;
        }
        r->backend_count++;
        if (comma == NULL) {
            break;
        }
        p = comma + 1;
    }
    atomic_init(&r->next, 0);
    t->count++; /* 모두 통과한 뒤에 표에 넣음 */
    return 0;
}

/* "이름:포트" 하나를 해석하고 주소를 찾습니다 */
static int backend_parse(struct proxy_backend *b, const char *s, size_t len,
                         int listen_port, char *err, size_t errlen)
{
    char host[PROXY_NAME_MAX];
    const char *colon = NULL;
    struct addrinfo hints, *res = NULL;
    long port = 0;
    int rc;

    for (size_t i = 0; i < len; i++) {
        if (s[i] == ':') {
            colon = s + i; /* 마지막 ':' */
        }
    }
    if (len == 0 || colon == NULL || colon == s || colon == s + len - 1) {
        snprintf(err, errlen, "백엔드 '%.*s'는 이름:포트 형식이어야 합니다",
                 (int)len, s);
        return -1;
    }
    for (const char *c = colon + 1; c < s + len; c++) {
        if (*c < '0' || *c > '9' || port > 65535) {
            port = -1;
            break;
        }
        port = port * 10 + (*c - '0');
    }
    if (port < 1 || port > 65535) {
        snprintf(err, errlen, "포트는 1~65535입니다: '%.*s'", (int)len, s);
        return -1;
    }
    if ((size_t)(colon - s) >= sizeof(host) || len >= sizeof(b->name)) {
        snprintf(err, errlen, "백엔드 이름이 너무 깁니다");
        return -1;
    }
    memcpy(host, s, (size_t)(colon - s));
    host[colon - s] = '\0';

    /* 이름은 시작할 때 한 번만 찾음. 요청마다 DNS를 기다리지 않음 (M6-5) */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || res == NULL) {
        snprintf(err, errlen, "백엔드 '%s'의 주소를 찾지 못했습니다", host);
        if (res != NULL) {
            freeaddrinfo(res);
        }
        return -1;
    }
    memcpy(&b->addr, res->ai_addr, sizeof(b->addr));
    freeaddrinfo(res);
    b->addr.sin_port = htons((unsigned short)port);
    memcpy(b->name, s, len);
    b->name[len] = '\0';

    /* 요청이 자기 자신에게 되돌아오면 작업자를 모두 차지함 (RFC 9110 7.6) */
    if (port == listen_port && addr_is_local(&b->addr)) {
        snprintf(err, errlen, "백엔드 '%s'가 이 프록시 자신입니다", b->name);
        return -1;
    }
    return 0;
}

/* 이 컴퓨터의 주소인지: 그 주소에 bind할 수 있으면 이 컴퓨터의 주소 */
static int addr_is_local(const struct sockaddr_in *addr)
{
    struct sockaddr_in a = *addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int local;

    if (fd < 0) {
        return 0;
    }
    a.sin_port = 0;
    local = bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0;
    close(fd);
    return local;
}

struct proxy_route *proxy_route_find(struct proxy_table *t, const char *uri)
{
    const char *q = strchr(uri, '?');
    size_t path_len = q ? (size_t)(q - uri) : strlen(uri);
    struct proxy_route *best = NULL;

    for (int i = 0; i < t->count; i++) {
        struct proxy_route *r = &t->routes[i];
        size_t n = r->prefix_len;

        if (n > path_len || memcmp(uri, r->prefix, n) != 0) {
            continue;
        }
        if (n < path_len && uri[n] != '/') {
            continue; /* "/api" 규칙에 "/apiary"는 걸리지 않음 */
        }
        if (best == NULL || n > best->prefix_len) {
            best = r; /* 가장 긴 접두사 */
        }
    }
    return best;
}

int proxy_backend_start(struct proxy_route *r)
{
    unsigned n = atomic_fetch_add_explicit(&r->next, 1, memory_order_relaxed);
    return (int)(n % (unsigned)r->backend_count);
}

/* ============================== 헤더 만들기 ============================== */

/* 헤더 블록을 한 줄씩: *pos부터 한 줄의 [start, end)(줄바꿈 제외). 없으면 0 */
static int line_next(const char *buf, size_t len, size_t *pos, size_t *start,
                     size_t *end)
{
    size_t i = *pos;

    if (i >= len) {
        return 0;
    }
    *start = i;
    while (i < len && buf[i] != '\n') {
        i++;
    }
    *end = i;
    if (*end > *start && buf[*end - 1] == '\r') {
        (*end)--;
    }
    *pos = i < len ? i + 1 : len;
    return 1;
}

static int name_is(const char *name, size_t len, const char *want)
{
    return strlen(want) == len && strncasecmp(name, want, len) == 0;
}

static int name_in_list(const char *name, size_t len, const char *const *list)
{
    for (int i = 0; list[i] != NULL; i++) {
        if (name_is(name, len, list[i])) {
            return 1;
        }
    }
    return 0;
}

/*
 * 블록의 Connection 헤더 값에 name이 적혀 있으면 1.
 * "Connection: keep-alive, X-Hop"이면 X-Hop도 홉 단위 헤더 (RFC 9110 7.6.1)
 */
static int name_listed_in_connection(const char *block, size_t blen,
                                     const char *name, size_t nlen)
{
    size_t pos = 0, s, e;

    while (line_next(block, blen, &pos, &s, &e)) {
        const char *line = block + s;
        const char *colon = memchr(line, ':', e - s);
        const char *v, *end = block + e;

        if (colon == NULL ||
            !name_is(line, (size_t)(colon - line), "Connection")) {
            continue;
        }
        v = colon + 1;
        while (v < end) {
            const char *t, *te;
            while (v < end && (*v == ' ' || *v == '\t' || *v == ',')) {
                v++;
            }
            t = v;
            while (v < end && *v != ',') {
                v++;
            }
            te = v;
            while (te > t && (te[-1] == ' ' || te[-1] == '\t')) {
                te--;
            }
            if (te > t && (size_t)(te - t) == nlen &&
                strncasecmp(t, name, nlen) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

/* out에 printf 형식으로 덧붙임. 넘치면 -1 */
static int append(char *out, size_t cap, size_t *len, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int append(char *out, size_t cap, size_t *len, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*len >= cap) {
        return -1;
    }
    va_start(ap, fmt);
    n = vsnprintf(out + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *len) {
        return -1;
    }
    *len += (size_t)n;
    return 0;
}

/* 프록시와 클라이언트 사이에서만 쓰는 헤더 (RFC 9110 7.6.1, 11.7.2) */
static const char *const REQUEST_DROP[] = {
    "Connection", "Keep-Alive", "Proxy-Connection", "TE", "Transfer-Encoding",
    "Upgrade", "Proxy-Authorization", "Trailer",
    /* 서버가 처리하는 헤더 */
    "Expect", "X-Forwarded-For",
    /* 검사한 값으로 다시 쓰는 헤더 */
    "Host", "Content-Length", NULL};

int proxy_request_build(const struct http_request *req, const char *client_ip,
                        char *out, size_t cap)
{
    size_t len = 0, pos = 0, s, e;

    if (append(out, cap, &len, "%s %s HTTP/1.0\r\n", req->method, req->uri) <
        0) {
        return -1;
    }
    while (line_next(req->raw_headers, req->raw_len, &pos, &s, &e)) {
        const char *line = req->raw_headers + s;
        const char *colon = memchr(line, ':', e - s);
        size_t nlen;

        if (colon == NULL) {
            continue; /* 파서가 이미 거절하므로 오지 않음 */
        }
        nlen = (size_t)(colon - line);
        if (name_in_list(line, nlen, REQUEST_DROP) ||
            name_listed_in_connection(req->raw_headers, req->raw_len, line,
                                      nlen)) {
            continue;
        }
        if (append(out, cap, &len, "%.*s\r\n", (int)(e - s), line) < 0) {
            return -1;
        }
    }
    /* Host는 클라이언트 값 그대로. HTTP/1.0에서 없으면 만들지 않음 */
    if (req->host_count == 1 &&
        append(out, cap, &len, "Host: %s\r\n", req->host) < 0) {
        return -1;
    }
    if ((req->has_content_length || req->body_len > 0) &&
        append(out, cap, &len, "Content-Length: %zu\r\n", req->body_len) < 0) {
        return -1;
    }
    if (append(out, cap, &len,
               "X-Forwarded-For: %s\r\n"
               "Via: 1.%d tiny\r\n"
               "Connection: close\r\n\r\n",
               client_ip, req->minor) < 0) {
        return -1;
    }
    return (int)len;
}

size_t proxy_head_end(const char *buf, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] != '\n') {
            continue;
        }
        if (buf[i + 1] == '\n') {
            return i + 2;
        }
        if (i + 2 < len && buf[i + 1] == '\r' && buf[i + 2] == '\n') {
            return i + 3;
        }
    }
    return 0;
}

/* 응답에서 빼는 홉 단위 헤더. Transfer-Encoding은 502라 여기 없음 */
static const char *const RESPONSE_DROP[] = {
    "Connection", "Keep-Alive", "Proxy-Connection",   "TE",
    "Trailer",    "Upgrade",    "Proxy-Authenticate", NULL};

/* "HTTP/1.x SSS 문구" 상태 줄. 맞으면 0 */
static int status_line_parse(const char *line, size_t len, int *status,
                             const char **reason, size_t *reason_len)
{
    if (len < 12 || memcmp(line, "HTTP/1.", 7) != 0 || line[7] < '0' ||
        line[7] > '9' || line[8] != ' ') {
        return -1;
    }
    for (int i = 9; i < 12; i++) {
        if (line[i] < '0' || line[i] > '9') {
            return -1;
        }
    }
    if (len > 12 && line[12] != ' ') {
        return -1;
    }
    *status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
    if (*status < 100) {
        return -1;
    }
    *reason = len > 13 ? line + 13 : "";
    *reason_len = len > 13 ? len - 13 : 0;
    return 0;
}

int proxy_response_parse(const char *head, size_t len, int head_request,
                         struct proxy_response *res, char *out, size_t cap,
                         size_t *out_len)
{
    size_t pos = 0, s, e, olen = 0;
    const char *reason;
    size_t reason_len;

    memset(res, 0, sizeof(*res));
    if (!line_next(head, len, &pos, &s, &e) || http_has_ctl(head + s, e - s) ||
        status_line_parse(head + s, e - s, &res->status, &reason, &reason_len) <
            0) {
        return -1;
    }
    if (res->status == 101) {
        return -1; /* 프로토콜 전환은 중계하지 않음 */
    }
    if (res->status < 200) {
        return 1; /* 100, 103 등 중간 응답: 다음 응답을 읽음 */
    }
    /* 중계자는 자기 버전(HTTP/1.1)으로 보냄 (RFC 9112 2.3) */
    if (reason_len == 0) {
        reason = http_status_text(res->status);
        reason_len = strlen(reason);
    }
    if (append(out, cap, &olen, "HTTP/1.1 %d %.*s\r\n", res->status,
               (int)reason_len, reason) < 0) {
        return -1;
    }

    while (line_next(head, len, &pos, &s, &e)) {
        const char *line = head + s;
        const char *colon;
        size_t nlen;

        if (e == s) {
            break; /* 빈 줄: 헤더 끝 */
        }
        colon = memchr(line, ':', e - s);
        if (colon == NULL || http_has_ctl(line, e - s)) {
            return -1;
        }
        nlen = (size_t)(colon - line);
        if (!http_is_token(line, nlen)) {
            return -1;
        }
        if (name_is(line, nlen, "Transfer-Encoding")) {
            return -1; /* HTTP/1.0 요청에는 올 수 없음 (RFC 9112 6.1) */
        }
        if (name_is(line, nlen, "Content-Length")) {
            char value[32];
            const char *v = colon + 1, *ve = line + (e - s);
            long n;
            while (v < ve && (*v == ' ' || *v == '\t')) {
                v++;
            }
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) {
                ve--;
            }
            if ((size_t)(ve - v) >= sizeof(value)) {
                return -1;
            }
            memcpy(value, v, (size_t)(ve - v));
            value[ve - v] = '\0';
            if (http_parse_content_length(value, &n) < 0 ||
                (res->has_length && res->length != n)) {
                return -1; /* 숫자가 아님, 서로 다른 길이 */
            }
            res->has_length = 1;
            res->length = n;
            continue; /* 호출한 쪽이 하나만 다시 씀 */
        }
        if (name_in_list(line, nlen, RESPONSE_DROP) ||
            name_listed_in_connection(head, len, line, nlen)) {
            continue;
        }
        if (append(out, cap, &olen, "%.*s\r\n", (int)(e - s), line) < 0) {
            return -1;
        }
    }

    /* 204를 HEAD보다 먼저 봄: 두 규칙이 겹치면 204가 우선 (F4, RFC 9110 8.6) */
    if (res->status == 204) {
        res->body = PROXY_BODY_NONE;
        res->has_length = 0; /* 204에는 Content-Length를 보내지 않음 */
    } else if (head_request || res->status == 304) {
        res->body = PROXY_BODY_NONE; /* 길이는 있으면 그대로 알려 줌 */
    } else if (res->has_length) {
        res->body = PROXY_BODY_LENGTH;
    } else {
        res->body = PROXY_BODY_UNTIL_CLOSE;
    }
    *out_len = olen;
    return 0;
}

/* ============================== 중계 ============================== */

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* fd가 events 상태가 될 때까지 최대 wait_ms 기다림. 1 준비, 0 시간 초과, -1 오류 */
static int fd_wait(int fd, short events, long wait_ms)
{
    while (1) {
        struct pollfd p = {fd, events, 0};
        int r;

        if (wait_ms < 0) {
            wait_ms = 0;
        }
        r = poll(&p, 1, (int)wait_ms);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        return r < 0 ? -1 : (r > 0);
    }
}

/* 블로킹 클라이언트 소켓에 모두 씀 (쓰기 타임아웃은 SO_SNDTIMEO) */
static int client_write(int fd, const char *buf, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            return -1;
        }
        buf += w;
        n -= (size_t)w;
    }
    return 0;
}

/* 결과 코드: 연결과 전송 단계의 실패 */
enum { STEP_OK = 0, STEP_FAIL = -1, STEP_TIMEOUT = -2 };

/* 논블로킹 백엔드 소켓에 deadline까지 모두 씀 */
static int backend_write(int fd, const char *buf, size_t n, long deadline)
{
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int r = fd_wait(fd, POLLOUT, deadline - now_ms());
            if (r == 0 || deadline - now_ms() <= 0) {
                return STEP_TIMEOUT;
            }
            if (r < 0) {
                return STEP_FAIL;
            }
            continue;
        }
        if (w <= 0) {
            return STEP_FAIL; /* 백엔드가 먼저 끊음 (EPIPE, ECONNRESET) */
        }
        buf += w;
        n -= (size_t)w;
    }
    return STEP_OK;
}

/*
 * 백엔드 하나에 논블로킹 connect.
 * 반환: 연결된 fd, STEP_FAIL(거부, 도달 불가), STEP_TIMEOUT,
 *       -3 (소켓을 열지 못함: fd 부족)
 */
static int backend_connect(const struct proxy_backend *b, long wait_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int err = 0;
    socklen_t elen = sizeof(err);
    int one = 1;
    int r;

    if (fd < 0) {
        return -3;
    }
    /* 요청 헤더와 POST 본문을 나눠 쓰므로 백엔드 쪽에도 Nagle 지연을 끔 (F1) */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    /* CGI 자식이 물려받지 않게 함. fork와의 틈은 CGI 자식이 fd를 모두 닫아서 막음 */
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0) {
        close(fd);
        return STEP_FAIL;
    }
    if (connect(fd, (const struct sockaddr *)&b->addr, sizeof(b->addr)) == 0) {
        return fd;
    }
    if (errno != EINPROGRESS && errno != EINTR) {
        close(fd);
        return STEP_FAIL;
    }
    r = fd_wait(fd, POLLOUT, wait_ms);
    if (r <= 0) {
        close(fd);
        return r == 0 ? STEP_TIMEOUT : STEP_FAIL;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
        close(fd);
        return STEP_FAIL;
    }
    return fd;
}

/*
 * 연결 단계 재시도 (M6-15): 순번부터 백엔드마다 한 번씩.
 * 성공하면 fd와 *chosen, 실패하면 -1과 *status(502, 504)
 */
static int backends_try(struct proxy_route *route, long deadline, int *chosen,
                        int *status)
{
    int start = proxy_backend_start(route);
    int timed_out = 0;

    for (int i = 0; i < route->backend_count; i++) {
        int idx = (start + i) % route->backend_count;
        long left = deadline - now_ms();
        int fd;

        if (left <= 0) {
            *status = 504; /* 합계 시간이 지남: 남은 백엔드를 시도하지 않음 */
            return -1;
        }
        fd = backend_connect(&route->backends[idx],
                             left < PROXY_CONNECT_MS ? left : PROXY_CONNECT_MS);
        if (fd >= 0) {
            *chosen = idx;
            return fd;
        }
        if (fd == -3) {
            *status = 502; /* fd 부족: 다음 백엔드도 열 수 없음 */
            return -1;
        }
        if (fd == STEP_TIMEOUT) {
            timed_out = 1;
        }
    }
    /* 시도 순서와 상관없이 정함: 시간 초과가 한 번이라도 있으면 504 */
    *status = timed_out ? 504 : 502;
    return -1;
}

/*
 * 최종 응답 헤더를 buf에 모읍니다. 성공하면 헤더 길이(*hlen)와 모은 길이(*blen).
 * 반환: STEP_OK, STEP_FAIL(끊김, 8 KiB 초과, 형식 오류 → 502), STEP_TIMEOUT(504)
 */
static int response_head_read(int bfd, char *buf, size_t cap, size_t *blen,
                              long deadline, int head_request,
                              struct proxy_response *res, char *out,
                              size_t out_cap, size_t *out_len)
{
    size_t len = 0;

    while (1) {
        size_t end = proxy_head_end(buf, len);

        if (end > PROXY_HEAD_MAX || (end == 0 && len >= PROXY_HEAD_MAX)) {
            return STEP_FAIL; /* 헤더가 8 KiB를 넘음 */
        }
        if (end > 0) {
            int rc = proxy_response_parse(buf, end, head_request, res, out,
                                          out_cap, out_len);
            if (rc < 0) {
                return STEP_FAIL;
            }
            memmove(buf, buf + end, len - end); /* 헤더를 떼고 남은 것은 본문 */
            len -= end;
            if (rc == 1) {
                continue; /* 중간 응답은 버리고 같은 마감으로 다음 응답 */
            }
            *blen = len;
            return STEP_OK;
        }

        long left = deadline - now_ms();
        if (left <= 0) {
            return STEP_TIMEOUT;
        }
        int r = fd_wait(bfd, POLLIN, left);
        if (r == 0) {
            return STEP_TIMEOUT;
        }
        if (r < 0) {
            return STEP_FAIL;
        }
        ssize_t n = read(bfd, buf + len, cap - len);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
        if (n <= 0) {
            return STEP_FAIL; /* 응답 전에 끊김 */
        }
        len += (size_t)n;
    }
}

/*
 * 본문 전달 (M6-11, M6-13). buf의 앞 have바이트는 이미 받은 본문.
 * 반환: PROXY_REPLIED, PROXY_CLOSE, PROXY_RESET
 */
static int body_relay(int cfd, int bfd, const struct proxy_response *res,
                      char *buf, size_t cap, size_t have, long idle_ms)
{
    long left = res->body == PROXY_BODY_LENGTH ? res->length : -1;

    while (1) {
        if (have > 0) {
            size_t n = have;
            if (left >= 0 && (long)n > left) {
                n = (size_t)left; /* 길이보다 더 온 바이트는 버림 */
            }
            if (n > 0 && client_write(cfd, buf, n) < 0) {
                return PROXY_CLOSE; /* 클라이언트가 끊음: 둘 다 바로 닫음 */
            }
            if (left >= 0) {
                left -= (long)n;
            }
        }
        if (left == 0) {
            return PROXY_REPLIED;
        }

        int r = fd_wait(bfd, POLLIN, idle_ms);
        ssize_t n = 0;
        if (r > 0) {
            n = read(bfd, buf, cap);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
                have = 0;
                continue;
            }
        }
        if (r > 0 && n == 0) { /* 백엔드가 닫음 */
            if (left < 0) {
                return PROXY_REPLIED; /* 길이 없는 응답의 정상 끝 */
            }
            return PROXY_CLOSE; /* 길이보다 덜 옴: 클라이언트는 길이로 앎 */
        }
        if (r <= 0 || n < 0) { /* 시간 초과, 오류 */
            /* 길이를 모르면 FIN이 성공으로 보이므로 RST로 알림 */
            return left < 0 ? PROXY_RESET : PROXY_CLOSE;
        }
        have = (size_t)n;
    }
}

int proxy_forward(int client_fd, const struct http_request *req,
                  const char *client_ip, struct proxy_route *route,
                  int timeout_sec, atomic_int *stopping, int *keep, int *status,
                  const char **backend)
{
    /* 요청 줄(8 KiB) + 헤더(8 KiB) + 추가 헤더 */
    char reqbuf[HTTP_LINE_MAX + HDR_RAW_MAX + 1024];
    char buf[PROXY_HEAD_MAX * 2];
    char head[PROXY_HEAD_MAX + 256];
    struct proxy_response res;
    long deadline = now_ms() + (long)timeout_sec * 1000;
    size_t head_len = 0, have = 0;
    int head_request = strcmp(req->method, "HEAD") == 0;
    int reqlen, bfd, chosen = 0, rc, client_keep;

    *backend = NULL;
    reqlen = proxy_request_build(req, client_ip, reqbuf, sizeof(reqbuf));
    if (reqlen < 0) {
        *status = 502;
        return PROXY_FAILED;
    }

    bfd = backends_try(route, deadline, &chosen, status);
    if (bfd < 0) {
        return PROXY_FAILED;
    }
    *backend = route->backends[chosen].name;

    /* 요청 헤더 + 본문 보내기 */
    rc = backend_write(bfd, reqbuf, (size_t)reqlen, deadline);
    if (rc == STEP_OK && req->body_len > 0) {
        rc = backend_write(bfd, req->body, req->body_len, deadline);
    }
    if (rc == STEP_OK) {
        rc = response_head_read(bfd, buf, sizeof(buf), &have, deadline,
                                head_request, &res, head, sizeof(head),
                                &head_len);
    }
    if (rc != STEP_OK) {
        close(bfd);
        *status = rc == STEP_TIMEOUT ? 504 : 502;
        return PROXY_FAILED;
    }

    /* 클라이언트 헤더: 길이를 모르면 닫음, 종료 중이면 닫음 (M7-7) */
    client_keep =
        *keep && res.body != PROXY_BODY_UNTIL_CLOSE && !atomic_load(stopping);
    if ((res.has_length && append(head, sizeof(head), &head_len,
                                  "Content-Length: %ld\r\n", res.length) < 0) ||
        append(head, sizeof(head), &head_len, "Connection: %s\r\n\r\n",
               client_keep ? "keep-alive" : "close") < 0) {
        close(bfd);
        *status = 502;
        return PROXY_FAILED;
    }
    *status = res.status;
    *keep = client_keep;
    if (client_write(client_fd, head, head_len) < 0) {
        close(bfd);
        return PROXY_CLOSE;
    }
    if (res.body == PROXY_BODY_NONE) {
        close(bfd);
        return PROXY_REPLIED;
    }
    rc = body_relay(client_fd, bfd, &res, buf, sizeof(buf), have,
                    (long)timeout_sec * 1000);
    close(bfd);
    return rc;
}
