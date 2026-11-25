/*
 * proxy 단위 테스트 (docs/prd/m6m7.md 7장)
 *   규칙 해석, 라우팅, Round-Robin, 요청 헤더 만들기, 응답 헤더 해석을
 *   소켓 없이 확인합니다. 백엔드 이름은 숫자 주소만 써서 DNS를 기다리지 않습니다.
 */
#include "acutest.h"
#include "proxy.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define LISTEN_PORT 8000

static char err[512];

static struct proxy_table *new_table(void)
{
    struct proxy_table *t = malloc(sizeof(*t));
    proxy_table_init(t);
    return t;
}

static int add(struct proxy_table *t, const char *spec)
{
    return proxy_route_add(t, spec, LISTEN_PORT, err, sizeof(err));
}

/* 요청 전체(요청 줄 + 헤더)를 해석 */
static int parse_request(struct http_request *req, const char *text)
{
    const char *p = text;
    int rc;

    http_request_init(req);
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p + 1) : strlen(p);
        rc = (p == text) ? http_parse_request_line(req, p, len, HTTP_LINE_MAX)
                         : http_parse_header_line(req, p, len, HTTP_LINE_MAX);
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            return rc;
        }
        p += len;
    }
    return http_finish_headers(req);
}

static void test_route_parse_ok(void)
{
    struct proxy_table *t = new_table();

    TEST_CHECK(add(t, "/api=127.0.0.1:8001,127.0.0.1:8002") == 0);
    TEST_CHECK(t->count == 1);
    TEST_CHECK(strcmp(t->routes[0].prefix, "/api") == 0);
    TEST_CHECK(t->routes[0].backend_count == 2);
    TEST_CHECK(strcmp(t->routes[0].backends[1].name, "127.0.0.1:8002") == 0);
    TEST_CHECK(ntohs(t->routes[0].backends[1].addr.sin_port) == 8002);

    /* 끝의 '/'는 떼고, "/"는 빈 접두사 */
    TEST_CHECK(add(t, "/static/=127.0.0.1:9001") == 0);
    TEST_CHECK(strcmp(t->routes[1].prefix, "/static") == 0);
    TEST_CHECK(add(t, "/=127.0.0.1:9002") == 0);
    TEST_CHECK(t->routes[2].prefix_len == 0);
    /* 다른 포트면 이 컴퓨터의 주소여도 자기 자신이 아님 */
    TEST_CHECK(add(t, "/self=127.0.0.1:8080") == 0);
    free(t);
}

static void test_route_parse_errors(void)
{
    struct proxy_table *t = new_table();
    char spec[64];

    TEST_CHECK(add(t, "api=127.0.0.1:8001") < 0);   /* '/'로 시작하지 않음 */
    TEST_CHECK(add(t, "/api") < 0);                 /* '=' 없음 */
    TEST_CHECK(add(t, "/api=") < 0);                /* 백엔드 없음 */
    TEST_CHECK(add(t, "/api=127.0.0.1") < 0);       /* 포트 없음 */
    TEST_CHECK(add(t, "/api=127.0.0.1:0") < 0);     /* 포트 범위 */
    TEST_CHECK(add(t, "/api=127.0.0.1:65536") < 0); /* 포트 범위 */
    TEST_CHECK(add(t, "/api=127.0.0.1:80x") < 0);   /* 숫자가 아님 */
    TEST_CHECK(add(t, "/api=127.0.0.1:8001,") < 0); /* 빈 백엔드 */
    TEST_CHECK(add(t, "/api=:8001") < 0);           /* 이름 없음 */
    TEST_CHECK(add(t, "=127.0.0.1:8001") < 0);      /* 접두사 없음 */
    TEST_CHECK(add(t, "/api=999.1.1.1:8001") < 0);  /* 주소를 찾지 못함 */
    TEST_CHECK(t->count == 0); /* 실패한 규칙은 표에 들어가지 않음 */

    /* 자기 자신: 이 컴퓨터의 주소이고 수신 포트가 같음 */
    TEST_CHECK(add(t, "/loop=127.0.0.1:8000") < 0);
    TEST_CHECK(add(t, "/loop=0.0.0.0:8000") < 0);
    TEST_CHECK(add(t, "/loop=127.0.0.1:8001,127.0.0.1:8000") < 0);

    /* 같은 접두사 ("/api"와 "/api/"도 같음) */
    TEST_CHECK(add(t, "/api=127.0.0.1:8001") == 0);
    TEST_CHECK(add(t, "/api=127.0.0.1:8002") < 0);
    TEST_CHECK(add(t, "/api/=127.0.0.1:8002") < 0);

    /* 규칙당 백엔드 8개 */
    TEST_CHECK(add(t, "/b=127.0.0.1:1,127.0.0.1:2,127.0.0.1:3,127.0.0.1:4,"
                      "127.0.0.1:5,127.0.0.1:6,127.0.0.1:7,127.0.0.1:8") == 0);
    TEST_CHECK(add(t, "/c=127.0.0.1:1,127.0.0.1:2,127.0.0.1:3,127.0.0.1:4,"
                      "127.0.0.1:5,127.0.0.1:6,127.0.0.1:7,127.0.0.1:8,"
                      "127.0.0.1:9") < 0);

    /* 규칙 16개 */
    for (int i = t->count; i < PROXY_ROUTES_MAX; i++) {
        snprintf(spec, sizeof(spec), "/r%d=127.0.0.1:9000", i);
        TEST_CHECK(add(t, spec) == 0);
    }
    TEST_CHECK(add(t, "/one-more=127.0.0.1:9000") < 0);
    free(t);
}

static const char *find(struct proxy_table *t, const char *uri)
{
    struct proxy_route *r = proxy_route_find(t, uri);
    return r ? r->prefix : NULL;
}

static void test_route_find(void)
{
    struct proxy_table *t = new_table();

    TEST_CHECK(add(t, "/api=127.0.0.1:8001") == 0);
    TEST_CHECK(add(t, "/api/admin=127.0.0.1:8002") == 0);

    /* '/' 경계 */
    TEST_CHECK(strcmp(find(t, "/api"), "/api") == 0);
    TEST_CHECK(strcmp(find(t, "/api/"), "/api") == 0);
    TEST_CHECK(strcmp(find(t, "/api/users/3"), "/api") == 0);
    TEST_CHECK(find(t, "/apiary") == NULL);
    TEST_CHECK(find(t, "/home.html") == NULL);
    TEST_CHECK(find(t, "/") == NULL);
    /* 쿼리는 보지 않음 */
    TEST_CHECK(strcmp(find(t, "/api?x=1"), "/api") == 0);
    TEST_CHECK(find(t, "/a?/api") == NULL);
    /* 가장 긴 접두사 */
    TEST_CHECK(strcmp(find(t, "/api/admin/x"), "/api/admin") == 0);
    TEST_CHECK(strcmp(find(t, "/api/admin"), "/api/admin") == 0);
    TEST_CHECK(strcmp(find(t, "/api/administrator"), "/api") == 0);

    /* "/" 규칙: 모든 경로에 걸리고, 더 긴 규칙이 우선 */
    TEST_CHECK(add(t, "/=127.0.0.1:8003") == 0);
    TEST_CHECK(strcmp(find(t, "/home.html"), "") == 0);
    TEST_CHECK(strcmp(find(t, "/"), "") == 0);
    TEST_CHECK(strcmp(find(t, "/apiary"), "") == 0);
    TEST_CHECK(strcmp(find(t, "/api/x"), "/api") == 0);
    free(t);
}

static void test_round_robin_order(void)
{
    struct proxy_table *t = new_table();
    struct proxy_route *r;

    TEST_CHECK(add(t, "/api=127.0.0.1:8001,127.0.0.1:8002,127.0.0.1:8003") ==
               0);
    r = &t->routes[0];
    for (int i = 0; i < 7; i++) {
        TEST_CHECK(proxy_backend_start(r) == i % 3);
    }
    free(t);
}

#define RR_THREADS 8
#define RR_PER_THREAD 3000

struct rr_arg {
    struct proxy_route *route;
    int counts[2];
};

static void *rr_worker(void *p)
{
    struct rr_arg *a = p;
    for (int i = 0; i < RR_PER_THREAD; i++) {
        a->counts[proxy_backend_start(a->route)]++;
    }
    return NULL;
}

/* 여러 스레드가 순번을 올려도 빠지거나 겹치지 않음 (make tsan으로 경합 확인) */
static void test_round_robin_threads(void)
{
    struct proxy_table *t = new_table();
    pthread_t th[RR_THREADS];
    struct rr_arg args[RR_THREADS];
    int total[2] = {0, 0};

    TEST_CHECK(add(t, "/api=127.0.0.1:8001,127.0.0.1:8002") == 0);
    for (int i = 0; i < RR_THREADS; i++) {
        args[i].route = &t->routes[0];
        args[i].counts[0] = args[i].counts[1] = 0;
        pthread_create(&th[i], NULL, rr_worker, &args[i]);
    }
    for (int i = 0; i < RR_THREADS; i++) {
        pthread_join(th[i], NULL);
        total[0] += args[i].counts[0];
        total[1] += args[i].counts[1];
    }
    TEST_CHECK(total[0] == RR_THREADS * RR_PER_THREAD / 2);
    TEST_CHECK(total[1] == RR_THREADS * RR_PER_THREAD / 2);
    free(t);
}

/* out(요청 블록)에 정확히 이 줄이 있으면 1 */
static int has_line(const char *out, const char *line)
{
    char want[512];
    snprintf(want, sizeof(want), "\r\n%s\r\n", line);
    return strstr(out, want) != NULL;
}

static void test_request_build(void)
{
    struct http_request *req = malloc(sizeof(*req));
    char out[20000];
    int n;

    TEST_CHECK(parse_request(req, "GET /api/users?id=1 HTTP/1.1\r\n"
                                  "Host: example.com\r\n"
                                  "Connection: keep-alive, X-Hop\r\n"
                                  "X-Hop: 1\r\n"
                                  "Keep-Alive: timeout=5\r\n"
                                  "Upgrade: websocket\r\n"
                                  "Proxy-Authorization: secret\r\n"
                                  "TE: trailers\r\n"
                                  "X-End: 1\r\n"
                                  "Cookie: a=b\r\n"
                                  "X-Forwarded-For: 1.2.3.4\r\n"
                                  "\r\n") == 0);
    n = proxy_request_build(req, "203.0.113.7", out, sizeof(out));
    TEST_ASSERT(n > 0);
    out[n] = '\0';
    TEST_MSG("%s", out);

    TEST_CHECK(strncmp(out, "GET /api/users?id=1 HTTP/1.0\r\n", 30) == 0);
    TEST_CHECK(has_line(out, "X-End: 1"));
    TEST_CHECK(has_line(out, "Cookie: a=b"));
    TEST_CHECK(has_line(out, "Host: example.com"));
    TEST_CHECK(has_line(out, "X-Forwarded-For: 203.0.113.7"));
    TEST_CHECK(has_line(out, "Via: 1.1 tiny"));
    TEST_CHECK(has_line(out, "Connection: close"));
    TEST_CHECK(strstr(out, "X-Hop") == NULL);
    TEST_CHECK(strstr(out, "Keep-Alive") == NULL);
    TEST_CHECK(strstr(out, "Upgrade") == NULL);
    TEST_CHECK(strstr(out, "Proxy-Authorization") == NULL);
    TEST_CHECK(strstr(out, "TE:") == NULL);
    TEST_CHECK(strstr(out, "1.2.3.4") == NULL);
    TEST_CHECK(strstr(out, "Content-Length") == NULL);
    /* 블록은 빈 줄로 끝남 */
    TEST_CHECK(n >= 4 && memcmp(out + n - 4, "\r\n\r\n", 4) == 0);
    free(req);
}

static void test_request_build_body_and_http10(void)
{
    struct http_request *req = malloc(sizeof(*req));
    char out[20000];
    int n;

    /* POST: Content-Length는 검사한 본문 길이로 하나만, Expect는 뺌 */
    TEST_CHECK(parse_request(req, "POST /api HTTP/1.1\r\n"
                                  "Host: a\r\n"
                                  "Content-Length: 7\r\n"
                                  "Content-Length: 7\r\n"
                                  "Content-Type: text/plain\r\n"
                                  "Expect: 100-continue\r\n"
                                  "\r\n") == 0);
    req->body_len = 7;
    n = proxy_request_build(req, "127.0.0.1", out, sizeof(out));
    TEST_ASSERT(n > 0);
    out[n] = '\0';
    TEST_CHECK(has_line(out, "Content-Length: 7"));
    TEST_CHECK(strstr(strstr(out, "Content-Length") + 1, "Content-Length") ==
               NULL);
    TEST_CHECK(has_line(out, "Content-Type: text/plain"));
    TEST_CHECK(strstr(out, "Expect") == NULL);

    /* HTTP/1.0에 Host가 없으면 만들지 않음, Via는 클라이언트 버전 */
    TEST_CHECK(parse_request(req, "HEAD /api HTTP/1.0\r\n\r\n") == 0);
    n = proxy_request_build(req, "127.0.0.1", out, sizeof(out));
    TEST_ASSERT(n > 0);
    out[n] = '\0';
    TEST_CHECK(strncmp(out, "HEAD /api HTTP/1.0\r\n", 20) == 0);
    TEST_CHECK(strstr(out, "Host:") == NULL);
    TEST_CHECK(has_line(out, "Via: 1.0 tiny"));

    /* 버퍼가 모자라면 -1 */
    TEST_CHECK(proxy_request_build(req, "127.0.0.1", out, 16) == -1);
    free(req);
}

/* 응답 헤더 해석: 반환값과 out, res */
static int parse_response(const char *text, int head_request,
                          struct proxy_response *res, char *out, size_t cap)
{
    size_t out_len = 0;
    int rc = proxy_response_parse(text, strlen(text), head_request, res, out,
                                  cap, &out_len);
    if (rc == 0) {
        out[out_len] = '\0';
    }
    return rc;
}

static void test_response_parse(void)
{
    struct proxy_response res;
    char out[9000];

    /* 길이 있음: 상태 줄 버전을 1.1로, 홉 단위 헤더와 Content-Length는 뺌 */
    TEST_CHECK(parse_response("HTTP/1.0 200 OK\r\n"
                              "Content-Type: text/html\r\n"
                              "Content-Length: 12\r\n"
                              "Connection: close, X-Hop\r\n"
                              "X-Hop: 1\r\n"
                              "Keep-Alive: timeout=5\r\n"
                              "Server: backend\r\n\r\n",
                              0, &res, out, sizeof(out)) == 0);
    TEST_CHECK(res.status == 200);
    TEST_CHECK(res.has_length && res.length == 12);
    TEST_CHECK(res.body == PROXY_BODY_LENGTH);
    TEST_CHECK(strncmp(out, "HTTP/1.1 200 OK\r\n", 17) == 0);
    TEST_CHECK(strstr(out, "Content-Type: text/html\r\n") != NULL);
    TEST_CHECK(strstr(out, "Server: backend\r\n") != NULL);
    TEST_CHECK(strstr(out, "Content-Length") == NULL);
    TEST_CHECK(strstr(out, "Connection") == NULL);
    TEST_CHECK(strstr(out, "X-Hop") == NULL);
    TEST_CHECK(strstr(out, "Keep-Alive") == NULL);

    /* 길이 없음 → 닫을 때까지 */
    TEST_CHECK(
        parse_response("HTTP/1.0 200 OK\n\n", 0, &res, out, sizeof(out)) == 0);
    TEST_CHECK(res.body == PROXY_BODY_UNTIL_CLOSE && !res.has_length);

    /* HEAD 응답과 304: 본문 없음, 길이는 있으면 그대로 */
    TEST_CHECK(parse_response("HTTP/1.0 200 OK\r\nContent-Length: 50\r\n\r\n",
                              1, &res, out, sizeof(out)) == 0);
    TEST_CHECK(res.body == PROXY_BODY_NONE && res.length == 50);
    TEST_CHECK(parse_response("HTTP/1.0 304 Not Modified\r\n\r\n", 0, &res, out,
                              sizeof(out)) == 0);
    TEST_CHECK(res.body == PROXY_BODY_NONE && !res.has_length);

    /* 204: 본문 없음, Content-Length를 보내지 않음 */
    TEST_CHECK(parse_response("HTTP/1.0 204 No Content\r\n"
                              "Content-Length: 0\r\n\r\n",
                              0, &res, out, sizeof(out)) == 0);
    TEST_CHECK(res.body == PROXY_BODY_NONE && !res.has_length);

    /* 문구가 없으면 기본 문구 */
    TEST_CHECK(parse_response("HTTP/1.0 404\r\nContent-Length: 0\r\n\r\n", 0,
                              &res, out, sizeof(out)) == 0);
    TEST_CHECK(strncmp(out, "HTTP/1.1 404 Not Found\r\n", 24) == 0);

    /* 중간 응답 */
    TEST_CHECK(parse_response("HTTP/1.1 100 Continue\r\n\r\n", 0, &res, out,
                              sizeof(out)) == 1);
    TEST_CHECK(parse_response("HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n",
                              0, &res, out, sizeof(out)) == 1);
}

static void test_response_parse_errors(void)
{
    struct proxy_response res;
    char out[9000];
    const char *bad[] = {
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: x\r\n\r\n",
        "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.0 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n",
        "HTTP/1.0 200 OK\r\nContent-Length: abc\r\n\r\n",
        "HTTP/1.0 200 OK\r\nno colon\r\n\r\n",
        "HTTP/1.0 200 OK\r\nBad Name: x\r\n\r\n",
        "HTTP/2 200 OK\r\n\r\n",
        "HTTP/1.0 20 OK\r\n\r\n",
        "HTTP/1.0 2000 OK\r\n\r\n",
        "garbage\r\n\r\n",
        "HTTP/1.0 099 Low\r\n\r\n",
        NULL};

    for (int i = 0; bad[i] != NULL; i++) {
        TEST_CHECK_(parse_response(bad[i], 0, &res, out, sizeof(out)) == -1,
                    "%s", bad[i]);
    }
    /* 같은 길이가 두 번이면 허용 */
    TEST_CHECK(parse_response("HTTP/1.0 200 OK\r\nContent-Length: 5\r\n"
                              "Content-Length: 5\r\n\r\n",
                              0, &res, out, sizeof(out)) == 0);
}

static void test_head_end(void)
{
    TEST_CHECK(proxy_head_end("HTTP/1.0 200 OK\r\n\r\nbody", 23) == 19);
    TEST_CHECK(proxy_head_end("HTTP/1.0 200 OK\n\nbody", 21) == 17);
    TEST_CHECK(proxy_head_end("HTTP/1.0 200 OK\r\nA: b\r\n", 24) == 0);
}

TEST_LIST = {
    {"route_parse_ok", test_route_parse_ok},
    {"route_parse_errors", test_route_parse_errors},
    {"route_find", test_route_find},
    {"round_robin_order", test_round_robin_order},
    {"round_robin_threads", test_round_robin_threads},
    {"request_build", test_request_build},
    {"request_build_body_and_http10", test_request_build_body_and_http10},
    {"response_parse", test_response_parse},
    {"response_parse_errors", test_response_parse_errors},
    {"head_end", test_head_end},
    {NULL, NULL}};
