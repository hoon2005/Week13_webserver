/*
 * proxy 단위 테스트: 요청 다시 쓰기, 응답 헤더 해석, 응답 전달
 *   전달은 socketpair 두 쌍(백엔드 쪽, 손님 쪽)으로 확인합니다.
 */
#include "acutest.h"
#include "proxy.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---------- 요청 다시 쓰기 ---------- */

/* 요청 줄과 헤더로 구조체를 채움 */
static struct http_request *new_request(const char *line, const char *headers)
{
    struct http_request *req = malloc(sizeof(*req));
    const char *p = headers;

    http_request_init(req);
    TEST_ASSERT(
        http_parse_request_line(req, line, strlen(line), HTTP_LINE_MAX) == 0);
    while (*p != '\0') {
        size_t len = strcspn(p, "\n") + 1;
        TEST_ASSERT(http_parse_header_line(req, p, len, HTTP_LINE_MAX) == 0);
        p += len;
    }
    return req;
}

static int build_gives(const struct http_request *req, const char *expected)
{
    char out[1024];
    long n = proxy_request_build(req, out, sizeof(out));
    int same =
        n == (long)strlen(expected) && memcmp(out, expected, (size_t)n) == 0;

    TEST_CHECK(same);
    TEST_MSG("만든 요청:\n%.*s", (int)(n < 0 ? 0 : n), out);
    return same;
}

static void test_request_is_rewritten(void)
{
    struct http_request *req =
        new_request("GET /api/users?id=1 HTTP/1.1\r\n",
                    "Host: example.com:8080\r\nAccept: */*\r\n");

    build_gives(req, "GET /api/users?id=1 HTTP/1.0\r\n"
                     "Accept: */*\r\n"
                     "Host: example.com:8080\r\n"
                     "Via: 1.1 tiny\r\n"
                     "Connection: close\r\n\r\n");
    free(req);
}

static void test_request_drops_hop_by_hop_and_expect(void)
{
    struct http_request *req = new_request(
        "POST /api/ HTTP/1.1\r\n",
        "Host: a\r\nContent-Length: 3\r\nExpect: 100-continue\r\n"
        "Connection: keep-alive, X-Hop\r\nKeep-Alive: 5\r\nX-Hop: 1\r\n"
        "X-End: 2\r\n");

    req->body = strdup("abc");
    req->body_len = 3;
    build_gives(req, "POST /api/ HTTP/1.0\r\n"
                     "X-End: 2\r\n"
                     "Host: a\r\n"
                     "Content-Length: 3\r\n"
                     "Via: 1.1 tiny\r\n"
                     "Connection: close\r\n\r\n");
    http_request_free(req);
    free(req);
}

static void test_connection_cannot_remove_host_and_length(void)
{
    struct http_request *req = new_request(
        "POST /api/ HTTP/1.1\r\n", "Host: a\r\nContent-Length: 0\r\n"
                                   "Connection: Host, Content-Length\r\n");

    build_gives(req, "POST /api/ HTTP/1.0\r\n"
                     "Host: a\r\n"
                     "Content-Length: 0\r\n"
                     "Via: 1.1 tiny\r\n"
                     "Connection: close\r\n\r\n");
    free(req);
}

static void test_request_without_host_and_existing_via(void)
{
    struct http_request *req =
        new_request("HEAD /api/ HTTP/1.0\r\n", "Via: 1.1 first\r\n");

    build_gives(req, "HEAD /api/ HTTP/1.0\r\n"
                     "Via: 1.1 first\r\n"
                     "Via: 1.0 tiny\r\n"
                     "Connection: close\r\n\r\n");
    free(req);
}

static void test_request_too_long_for_buffer(void)
{
    struct http_request *req =
        new_request("GET /api/ HTTP/1.1\r\n", "Host: a\r\n");
    char out[40];

    TEST_CHECK(proxy_request_build(req, out, sizeof(out)) == -1);
    free(req);
}

/* ---------- 응답 헤더 해석 ---------- */

static int parse(const char *head, struct proxy_response *resp)
{
    return proxy_response_parse(head, strlen(head), resp);
}

static void test_response_head(void)
{
    struct proxy_response r;

    TEST_CHECK(parse("HTTP/1.1 404 Not Found\r\nContent-Length: 12\r\n", &r) ==
               0);
    TEST_CHECK(r.status == 404);
    TEST_CHECK(strcmp(r.reason, "Not Found") == 0);
    TEST_CHECK(r.content_length == 12);

    TEST_CHECK(parse("HTTP/1.0 200 \nX: y\n", &r) == 0);
    TEST_CHECK(r.reason[0] == '\0');
    TEST_CHECK(r.content_length == -1);
    TEST_CHECK(parse("HTTP/1.0 200\r\n", &r) == 0);
    /* 같은 길이가 두 번이면 허용 */
    TEST_CHECK(parse("HTTP/1.0 200 OK\r\nContent-Length: 1\r\n"
                     "content-length: 1\r\n",
                     &r) == 0);
}

static void test_bad_response_heads(void)
{
    static const char *const bad[] = {
        "",
        "hello\r\n",
        "HTTP/2.0 200 OK\r\n",
        "HTTP/1.1 20 OK\r\n",
        "HTTP/1.1 2000 OK\r\n",
        "HTTP/1.1 600 X\r\n",
        "HTTP/1.1 099 X\r\n",
        "HTTP/1.1  200 OK\r\n",
        "HTTP/1.1 200 OK\r\nno colon\r\n",
        "HTTP/1.1 200 OK\r\nA: 1\r\n folded\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: abc\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n",
        "HTTP/1.1 204 No Content\r\nTransfer-Encoding: chunked\r\n",
    };
    struct proxy_response r;

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_CHECK_(parse(bad[i], &r) == -1, "%s", bad[i]);
    }
}

/* ---------- 응답 전달 ---------- */

struct relay_case {
    struct proxy p;
    int backend; /* 시험이 백엔드 노릇을 하는 쪽 */
    int client;  /* 시험이 손님 노릇을 하는 쪽 */
    int serverfd;
    char got[4096];
};

/* 헤더까지 받은 상태를 만듦. head 뒤에 early(함께 온 본문)를 붙임 */
static struct relay_case *relay_start(const char *head, const char *early,
                                      int has_body)
{
    struct relay_case *c = calloc(1, sizeof(*c));
    int b[2], k[2];

    signal(SIGPIPE, SIG_IGN);
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, b) == 0);
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, k) == 0);
    c->backend = b[0];
    c->p.fd = b[1];
    fcntl(b[1], F_SETFL, O_NONBLOCK);
    c->client = k[0];
    c->serverfd = k[1];
    c->p.timeout_ms = 200;
    c->p.has_body = has_body;
    c->p.len =
        (size_t)snprintf(c->p.buf, sizeof(c->p.buf), "%s\r\n%s", head, early);
    c->p.head_len = strlen(head);
    c->p.body_start = strlen(head) + 2;
    TEST_ASSERT(proxy_response_parse(head, strlen(head), &c->p.resp) == 0);
    return c;
}

/* 손님이 받은 것을 모두 읽고 정리 */
static void relay_finish(struct relay_case *c)
{
    size_t len = 0;
    ssize_t n;

    close(c->serverfd);
    while ((n = read(c->client, c->got + len, sizeof(c->got) - 1 - len)) > 0) {
        len += (size_t)n;
    }
    c->got[len] = '\0';
    close(c->client);
    proxy_close(&c->p);
    if (c->backend >= 0) {
        close(c->backend);
    }
}

static void backend_say(struct relay_case *c, const char *s)
{
    TEST_ASSERT(write(c->backend, s, strlen(s)) == (ssize_t)strlen(s));
}

static void test_relay_stops_at_content_length(void)
{
    struct relay_case *c = relay_start(
        "HTTP/1.0 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n", "ab",
        1);
    int keep = 1;

    backend_say(c, "cdeEXTRA"); /* 알린 길이보다 더 보냄 */
    TEST_CHECK(proxy_relay(&c->p, c->serverfd, &keep) == PROXY_DONE);
    TEST_CHECK(keep == 1);
    relay_finish(c);
    TEST_CHECK(strcmp(c->got, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                              "Connection: keep-alive\r\n\r\nabcde") == 0);
    free(c);
}

static void test_relay_without_length_closes(void)
{
    struct relay_case *c = relay_start("HTTP/1.0 200 OK\r\n", "ab", 1);
    int keep = 1;

    backend_say(c, "cd");
    close(c->backend);
    c->backend = -1;
    TEST_CHECK(proxy_relay(&c->p, c->serverfd, &keep) == PROXY_DONE);
    TEST_CHECK(keep == 0);
    relay_finish(c);
    TEST_CHECK(strcmp(c->got,
                      "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nabcd") == 0);
    free(c);
}

static void test_relay_bodyless_responses(void)
{
    struct relay_case *c;
    int keep = 1;

    /* 204는 백엔드가 길이를 줘도 쓰지 않음 */
    c = relay_start("HTTP/1.0 204 No Content\r\nContent-Length: 3\r\n", "abc",
                    0);
    TEST_CHECK(proxy_relay(&c->p, c->serverfd, &keep) == PROXY_DONE);
    relay_finish(c);
    TEST_CHECK(strcmp(c->got, "HTTP/1.1 204 No Content\r\n"
                              "Connection: keep-alive\r\n\r\n") == 0);
    free(c);

    /* 304, HEAD는 백엔드 값만. 없으면 만들지 않음 */
    c = relay_start("HTTP/1.0 304 Not Modified\r\nContent-Length: 9\r\n", "",
                    0);
    TEST_CHECK(proxy_relay(&c->p, c->serverfd, &keep) == PROXY_DONE);
    relay_finish(c);
    TEST_CHECK(strstr(c->got, "Content-Length: 9\r\n") != NULL);
    free(c);

    c = relay_start("HTTP/1.0 304 Not Modified\r\n", "", 0);
    TEST_CHECK(proxy_relay(&c->p, c->serverfd, &keep) == PROXY_DONE);
    TEST_CHECK(keep == 1);
    relay_finish(c);
    TEST_CHECK(strstr(c->got, "Content-Length") == NULL);
    free(c);
}

static void test_relay_backend_failures(void)
{
    struct relay_case *c;
    int keep = 1;

    /* 길이보다 적게 보내고 닫음 */
    c = relay_start("HTTP/1.0 200 OK\r\nContent-Length: 5\r\n", "ab", 1);
    close(c->backend);
    c->backend = -1;
    TEST_CHECK(proxy_relay(&c->p, c->serverfd, &keep) == PROXY_ABORTED);
    relay_finish(c);
    free(c);

    /* 길이가 있는데 더 오지 않음 */
    c = relay_start("HTTP/1.0 200 OK\r\nContent-Length: 5\r\n", "ab", 1);
    TEST_CHECK(proxy_relay(&c->p, c->serverfd, &keep) == PROXY_ABORTED);
    relay_finish(c);
    free(c);

    /* 길이가 없는데 더 오지 않음 */
    c = relay_start("HTTP/1.0 200 OK\r\n", "ab", 1);
    TEST_CHECK(proxy_relay(&c->p, c->serverfd, &keep) ==
               PROXY_ABORTED_UNKNOWN_LENGTH);
    relay_finish(c);
    free(c);
}

static void test_relay_client_gone(void)
{
    struct relay_case *c = relay_start("HTTP/1.0 200 OK\r\n", "ab", 1);
    int keep = 1;

    close(c->client);
    TEST_CHECK(proxy_relay(&c->p, c->serverfd, &keep) == PROXY_ABORTED);
    c->client = open("/dev/null", O_RDONLY);
    relay_finish(c);
    free(c);
}

TEST_LIST = {
    {"요청 줄을 HTTP/1.0으로 쓰고 Via를 붙인다", test_request_is_rewritten},
    {"한 구간 전용 헤더와 Expect를 백엔드로 보내지 않는다",
     test_request_drops_hop_by_hop_and_expect},
    {"Connection에 적어도 Host와 Content-Length는 지워지지 않는다",
     test_connection_cannot_remove_host_and_length},
    {"Host가 없는 1.0 요청과 이미 있는 Via를 처리한다",
     test_request_without_host_and_existing_via},
    {"요청이 버퍼보다 길면 오류다", test_request_too_long_for_buffer},
    {"응답 상태 줄과 Content-Length를 읽는다", test_response_head},
    {"형식이 틀리거나 끝을 알 수 없는 응답은 오류다", test_bad_response_heads},
    {"Content-Length만큼만 전달하고 더 온 것은 버린다",
     test_relay_stops_at_content_length},
    {"길이가 없으면 백엔드가 닫을 때까지 전달하고 닫는다",
     test_relay_without_length_closes},
    {"204와 304는 본문 없이 끝나고 길이 헤더 규칙이 다르다",
     test_relay_bodyless_responses},
    {"본문 도중 백엔드가 끊기거나 멈추면 중단이다",
     test_relay_backend_failures},
    {"손님이 떠났으면 중단이다", test_relay_client_gone},
    {NULL, NULL}};
