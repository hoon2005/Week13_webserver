/*
 * proxy - 백엔드 연결, 요청 다시 쓰기, 응답 전달
 */
#include "proxy.h"
#include "csapp.h"

#include <ctype.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <time.h>

/* 백엔드 읽기와 쓰기의 결과 (0 이상은 바이트 수) */
#define IO_ERROR (-1)
#define IO_TIMEOUT (-2)

static int backend_connect(const struct backend *b, long deadline);
static int backend_write(int fd, const char *buf, size_t n, long deadline);
static ssize_t backend_read(int fd, char *buf, size_t n, long deadline);
static int response_head_read(struct proxy *p, long deadline);
static int head_end_find(const char *buf, size_t len, size_t *head_len,
                         size_t *body_start);
static enum proxy_result body_relay(struct proxy *p, int clientfd);

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int proxy_open(struct proxy *p, const struct http_request *req,
               const struct backend *backends, int count, int first,
               int timeout_sec)
{
    /* 헤더는 2배로 잡음: 줄 끝이 \n뿐이면 \r\n으로 쓰면서 길어짐 */
    char head[HTTP_LINE_MAX + HDR_RAW_MAX * 2 + 512];
    long deadline = now_ms() + timeout_sec * 1000L;
    long head_len;
    int rc = IO_ERROR;

    p->fd = -1;
    p->len = 0;
    p->timeout_ms = timeout_sec * 1000;

    /* 1. 연결. 다음 백엔드로 넘어가도 마감 시각은 처음 것을 그대로 씀 */
    for (int i = 0; i < count && p->fd < 0; i++) {
        p->backend = &backends[(first + i) % count];
        rc = backend_connect(p->backend, deadline);
        if (rc == IO_TIMEOUT) {
            return 504;
        }
        p->fd = rc;
    }
    if (p->fd < 0) {
        return 502;
    }

    /* 2. 요청 보내기 */
    head_len = proxy_request_build(req, head, sizeof(head));
    rc = IO_ERROR;
    if (head_len >= 0) {
        rc = backend_write(p->fd, head, (size_t)head_len, deadline);
    }
    if (rc == 0 && req->body_len > 0) {
        rc = backend_write(p->fd, req->body, req->body_len, deadline);
    }

    /* 3. 최종 응답 헤더 받기. 100, 103은 최종 응답이 아니므로 버리고 다시 읽음 */
    while (rc == 0) {
        rc = response_head_read(p, deadline);
        if (rc == 0 && (p->resp.status >= 200 || p->resp.status == 101)) {
            break;
        }
        if (rc == 0) {
            p->len -= p->body_start;
            memmove(p->buf, p->buf + p->body_start, p->len);
        }
    }
    if (rc == 0 && p->resp.status == 101) {
        rc = IO_ERROR; /* 프로토콜 전환을 요청한 적이 없음 */
    }
    if (rc != 0) {
        proxy_close(p);
        return rc == IO_TIMEOUT ? 504 : 502;
    }
    p->has_body = strcmp(req->method, "HEAD") != 0 && p->resp.status != 204 &&
                  p->resp.status != 304;
    return 0;
}

/*
 * deadline(ms)까지 fd가 준비되기를 기다립니다. 준비되면 0.
 * 끊기거나 오류가 나도 0이고, 이어지는 read나 write가 그 오류를 알려 줌
 */
static int fd_wait(int fd, short events, long deadline)
{
    while (1) {
        long left = deadline - now_ms();
        struct pollfd pfd = {fd, events, 0};
        int r;

        if (left <= 0) {
            return IO_TIMEOUT;
        }
        r = poll(&pfd, 1, (int)left);
        if (r > 0) {
            return 0;
        }
        if (r < 0 && errno != EINTR) {
            return IO_ERROR;
        }
    }
}

/*
 * 백엔드에 연결해 non-blocking 소켓을 돌려줍니다. 실패하면 IO_ERROR, IO_TIMEOUT.
 * blocking connect는 꺼진 컴퓨터를 만나면 1분 넘게 멈출 수 있어, non-blocking으로
 * 시작하고 poll로 마감까지만 기다립니다.
 */
static int backend_connect(const struct backend *b, long deadline)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1, err = 0, rc = 0;
    socklen_t len = sizeof(err);

    if (fd < 0) {
        return IO_ERROR;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC); /* CGI가 물려받지 않게 */
    fcntl(fd, F_SETFL, O_NONBLOCK);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    if (connect(fd, (const SA *)&b->addr, sizeof(b->addr)) < 0) {
        if (errno != EINPROGRESS) {
            rc = IO_ERROR;
        } else {
            rc = fd_wait(fd, POLLOUT, deadline);
            /* 쓸 수 있게 됐어도 연결 실패일 수 있어 결과를 따로 확인 */
            if (rc == 0 &&
                (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 ||
                 err != 0)) {
                rc = IO_ERROR;
            }
        }
    }
    if (rc != 0) {
        close(fd);
        return rc;
    }
    return fd;
}

static int backend_write(int fd, const char *buf, size_t n, long deadline)
{
    while (n > 0) {
        int rc = fd_wait(fd, POLLOUT, deadline);
        ssize_t w;

        if (rc != 0) {
            return rc;
        }
        w = write(fd, buf, n);
        if (w < 0 && errno != EINTR && errno != EAGAIN) {
            return IO_ERROR;
        }
        if (w > 0) {
            buf += w;
            n -= (size_t)w;
        }
    }
    return 0;
}

/* 한 번 읽습니다. 0이면 백엔드가 닫음 */
static ssize_t backend_read(int fd, char *buf, size_t n, long deadline)
{
    while (1) {
        int rc = fd_wait(fd, POLLIN, deadline);
        ssize_t r;

        if (rc != 0) {
            return rc;
        }
        r = read(fd, buf, n);
        if (r >= 0) {
            return r;
        }
        if (errno != EINTR && errno != EAGAIN) {
            return IO_ERROR;
        }
    }
}

/*
 * 요청 줄은 HTTP/1.0으로 씁니다. 서버는 1.0 요청에 chunked로 답할 수 없으므로
 * (RFC 9112 6.1) 응답의 끝을 Content-Length나 연결 종료로 알 수 있습니다.
 * Host와 Content-Length는 원본에서 빼고 직접 씁니다. 손님이 "Connection: Host"를
 * 보내 이 헤더들을 지우게 하는 것도 막습니다.
 */
long proxy_request_build(const struct http_request *req, char *dst, size_t cap)
{
    static const char *const skip[] = {"Host", "Content-Length", "Expect",
                                       NULL};
    size_t used;
    long n;

    n = snprintf(dst, cap, "%s %s HTTP/1.0\r\n", req->method, req->uri);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    used = (size_t)n;
    n = http_headers_filter(req->raw_headers, req->raw_len, skip, dst + used,
                            cap - used);
    if (n < 0) {
        return -1;
    }
    used += (size_t)n;

    if (req->host_count > 0) {
        n = snprintf(dst + used, cap - used, "Host: %s\r\n", req->host);
        if (n < 0 || (size_t)n >= cap - used) {
            return -1;
        }
        used += (size_t)n;
    }
    if (req->has_content_length || strcmp(req->method, "POST") == 0) {
        n = snprintf(dst + used, cap - used, "Content-Length: %zu\r\n",
                     req->body_len);
        if (n < 0 || (size_t)n >= cap - used) {
            return -1;
        }
        used += (size_t)n;
    }
    n = snprintf(dst + used, cap - used,
                 "Via: 1.%d tiny\r\n"
                 "Connection: close\r\n\r\n",
                 req->minor);
    if (n < 0 || (size_t)n >= cap - used) {
        return -1;
    }
    return (long)used + n;
}

/* buf에 헤더 끝(빈 줄)이 들어올 때까지 읽고 해석합니다 */
static int response_head_read(struct proxy *p, long deadline)
{
    while (!head_end_find(p->buf, p->len, &p->head_len, &p->body_start)) {
        ssize_t n;

        if (p->len == sizeof(p->buf)) {
            return IO_ERROR; /* 헤더가 8 KiB를 넘음 */
        }
        n = backend_read(p->fd, p->buf + p->len, sizeof(p->buf) - p->len,
                         deadline);
        if (n < 0) {
            return (int)n;
        }
        if (n == 0) {
            return IO_ERROR; /* 헤더가 끝나기 전에 닫음 */
        }
        p->len += (size_t)n;
    }
    return proxy_response_parse(p->buf, p->head_len, &p->resp) == 0 ? 0
                                                                    : IO_ERROR;
}

/* 빈 줄을 찾으면 1. head_len은 마지막 헤더 줄의 줄바꿈까지 */
static int head_end_find(const char *buf, size_t len, size_t *head_len,
                         size_t *body_start)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] != '\n') {
            continue;
        }
        if (buf[i + 1] == '\n') {
            *body_start = i + 2;
        } else if (i + 2 < len && buf[i + 1] == '\r' && buf[i + 2] == '\n') {
            *body_start = i + 3;
        } else {
            continue;
        }
        *head_len = i + 1;
        return 1;
    }
    return 0;
}

int proxy_response_parse(const char *head, size_t len,
                         struct proxy_response *resp)
{
    const char *nl = memchr(head, '\n', len);
    const char *status = head + 9; /* "HTTP/1.x " 다음 */
    struct http_header h;
    size_t line_len, pos;
    int rc;

    /* 상태 줄: "HTTP/1.x 200 OK" (문구는 없어도 됨) */
    if (nl == NULL) {
        return -1;
    }
    line_len = (size_t)(nl - head);
    if (line_len > 0 && head[line_len - 1] == '\r') {
        line_len--;
    }
    if (line_len < 12 || memcmp(head, "HTTP/1.", 7) != 0 ||
        !isdigit((unsigned char)head[7]) || head[8] != ' ' ||
        !isdigit((unsigned char)status[0]) ||
        !isdigit((unsigned char)status[1]) ||
        !isdigit((unsigned char)status[2]) ||
        (line_len > 12 && head[12] != ' ') ||
        memchr(head, '\0', line_len) != NULL ||
        memchr(head, '\r', line_len) != NULL) {
        return -1;
    }
    resp->status =
        (status[0] - '0') * 100 + (status[1] - '0') * 10 + (status[2] - '0');
    if (resp->status < 100 || resp->status > 599) {
        return -1;
    }
    resp->reason[0] = '\0';
    if (line_len > 13) {
        snprintf(resp->reason, sizeof(resp->reason), "%.*s",
                 (int)(line_len - 13), head + 13);
    }

    resp->content_length = -1;
    pos = (size_t)(nl - head) + 1;
    while ((rc = http_header_next(head, len, &pos, &h)) == 1) {
        char value[24];
        long n;

        if (http_header_is(&h, "Transfer-Encoding")) {
            return -1; /* 1.0 요청에는 올 수 없음. 본문 끝을 믿을 수 없음 */
        }
        if (!http_header_is(&h, "Content-Length")) {
            continue;
        }
        if (h.value_len >= sizeof(value)) {
            return -1;
        }
        memcpy(value, h.value, h.value_len);
        value[h.value_len] = '\0';
        if (http_parse_content_length(value, &n) < 0 ||
            (resp->content_length >= 0 && resp->content_length != n)) {
            return -1;
        }
        resp->content_length = n;
    }
    return rc;
}

enum proxy_result proxy_relay(struct proxy *p, int clientfd, int *keep)
{
    static const char *const skip[] = {"Content-Length", NULL};
    const struct proxy_response *resp = &p->resp;
    const char *headers = memchr(p->buf, '\n', p->head_len) + 1;
    size_t headers_len = p->head_len - (size_t)(headers - p->buf);
    char head[PROXY_HEAD_MAX * 2]; /* 줄 끝을 \r\n으로 쓰면서 길어질 수 있음 */
    size_t used;
    long n;

    if (p->has_body && resp->content_length < 0) {
        *keep = 0; /* 닫아야 본문의 끝을 알릴 수 있음 */
    }

    /* 상태 줄 + 넘길 헤더 + 길이 + Connection */
    used = (size_t)snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\n",
                            resp->status, resp->reason);
    n = http_headers_filter(headers, headers_len, skip, head + used,
                            sizeof(head) - used);
    if (n < 0) {
        return PROXY_ABORTED;
    }
    used += (size_t)n;
    /* 204에는 길이를 쓰지 않음. 본문 없는 HEAD, 304는 백엔드 값이 있을 때만 */
    if (resp->content_length >= 0 && resp->status != 204) {
        used +=
            (size_t)snprintf(head + used, sizeof(head) - used,
                             "Content-Length: %ld\r\n", resp->content_length);
    }
    used += (size_t)snprintf(head + used, sizeof(head) - used,
                             "Connection: %s\r\n\r\n",
                             *keep ? "keep-alive" : "close");
    if (rio_writen(clientfd, head, used) != (ssize_t)used) {
        return PROXY_ABORTED;
    }
    return p->has_body ? body_relay(p, clientfd) : PROXY_DONE;
}

/* 받은 만큼 바로 쓰며 본문을 옮깁니다. 길이가 있으면 그만큼, 없으면 EOF까지 */
static enum proxy_result body_relay(struct proxy *p, int clientfd)
{
    int has_length = p->resp.content_length >= 0;
    long left = p->resp.content_length; /* 길이가 있을 때만 씀 */
    size_t n = p->len - p->body_start;  /* 헤더와 함께 온 본문 앞부분 */
    char *chunk = p->buf + p->body_start;

    while (1) {
        ssize_t r;

        if (has_length && (long)n > left) {
            n = (size_t)left; /* 알린 길이보다 더 온 바이트는 버림 */
        }
        if (n > 0 && rio_writen(clientfd, chunk, n) != (ssize_t)n) {
            return PROXY_ABORTED; /* 손님이 끊었거나 받지 않음 */
        }
        left -= (long)n;
        if (has_length && left == 0) {
            return PROXY_DONE;
        }

        /* 무수신 시간은 읽을 때마다 다시 셈 (손님에게 쓰는 시간은 빠짐) */
        r = backend_read(p->fd, p->buf, sizeof(p->buf),
                         now_ms() + p->timeout_ms);
        if (r == 0 && !has_length) {
            return PROXY_DONE;
        }
        if (r <= 0) {
            return has_length ? PROXY_ABORTED : PROXY_ABORTED_UNKNOWN_LENGTH;
        }
        chunk = p->buf;
        n = (size_t)r;
    }
}

void proxy_close(struct proxy *p)
{
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
}
