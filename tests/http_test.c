/*
 * http 파서 단위 테스트 (final.md 8.5)
 *   소켓 없이 문자열만 넣어 요청 구조체가 맞게 채워지는지 확인합니다.
 */
#include "acutest.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 요청 줄 하나를 해석 (줄바꿈 포함 문자열) */
static int parse_line(struct http_request *req, const char *line)
{
    return http_parse_request_line(req, line, strlen(line), HTTP_LINE_MAX);
}

static int parse_header(struct http_request *req, const char *line)
{
    return http_parse_header_line(req, line, strlen(line), HTTP_LINE_MAX);
}

/*
 * 요청 전체(요청 줄 + 헤더 + 빈 줄)를 해석합니다. 반환: 0 또는 상태 코드.
 * headers는 "이름: 값\r\n"을 이어 붙인 문자열.
 */
static int parse_request(struct http_request *req, const char *request_line,
                         const char *headers)
{
    const char *p = headers;
    int rc;

    http_request_init(req);
    rc = parse_line(req, request_line);
    if (rc != 0) {
        return rc;
    }
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p + 1) : strlen(p);
        rc = http_parse_header_line(req, p, len, HTTP_LINE_MAX);
        if (rc != 0) {
            return rc;
        }
        p += len;
    }
    rc = parse_header(req, "\r\n");
    if (rc != 1) {
        return -1;
    }
    return http_finish_headers(req);
}

static struct http_request *new_request(void)
{
    struct http_request *req = malloc(sizeof(*req));
    http_request_init(req);
    return req;
}

static void test_request_line_ok(void)
{
    struct http_request *req = new_request();

    TEST_CHECK(parse_line(req, "GET /home.html HTTP/1.1\r\n") == 0);
    TEST_CHECK(strcmp(req->method, "GET") == 0);
    TEST_CHECK(strcmp(req->uri, "/home.html") == 0);
    TEST_CHECK(strcmp(req->version, "HTTP/1.1") == 0);
    TEST_CHECK(req->minor == 1);

    http_request_init(req);
    TEST_CHECK(parse_line(req, "head / HTTP/1.0\n") == 0);
    TEST_CHECK(strcmp(req->method, "HEAD") == 0);
    TEST_CHECK(req->minor == 0);
    free(req);
}

static void test_request_line_errors(void)
{
    struct http_request *req = new_request();
    char *longline = malloc(HTTP_LINE_MAX);

    /* 줄바꿈 없이 버퍼가 다 참 → 414 */
    memset(longline, 'a', HTTP_LINE_MAX - 1);
    TEST_CHECK(http_parse_request_line(req, longline, HTTP_LINE_MAX - 1,
                                       HTTP_LINE_MAX) == 414);
    /* 짧은데 줄바꿈 없음 (중간에 끊김) → 400 */
    TEST_CHECK(http_parse_request_line(req, "GET / HTTP/1.1", 14,
                                       HTTP_LINE_MAX) == 400);
    TEST_CHECK(parse_line(req, "GET /\r\n") == 400);
    TEST_CHECK(parse_line(req, "GET / HTTP/1.1 extra\r\n") == 400);
    TEST_CHECK(parse_line(req, "GET / HTTP1.1\r\n") == 400);
    TEST_CHECK(parse_line(req, "GET / HTTP/2.0\r\n") == 505);
    TEST_CHECK(parse_line(req, "GET / HTTP/1.2\r\n") == 505);
    TEST_CHECK(parse_line(req, "FOO / HTTP/1.1\r\n") == 501);
    TEST_CHECK(parse_line(req, "PUT / HTTP/1.1\r\n") == 501);
    free(longline);
    free(req);
}

static void test_header_case_and_spaces(void)
{
    struct http_request *req = new_request();

    TEST_CHECK(parse_request(req, "POST /x HTTP/1.1\r\n",
                             "host: a\r\n"
                             "content-length:   7  \r\n") == 0);
    TEST_CHECK(req->has_content_length);
    TEST_CHECK(req->content_length == 7);
    TEST_CHECK(strcmp(req->host, "a") == 0);

    TEST_CHECK(parse_request(req, "POST /x HTTP/1.1\r\n",
                             "HOST: a\r\n"
                             "CONTENT-LENGTH: 3\r\n") == 0);
    TEST_CHECK(req->content_length == 3);
    TEST_CHECK(http_body_length(req) == 3);
    free(req);
}

static void test_connection_defaults(void)
{
    struct http_request *req = new_request();

    TEST_CHECK(parse_request(req, "GET / HTTP/1.1\r\n", "Host: a\r\n") == 0);
    TEST_CHECK(req->keep_alive == 1);
    TEST_CHECK(parse_request(req, "GET / HTTP/1.0\r\n", "") == 0);
    TEST_CHECK(req->keep_alive == 0);
    TEST_CHECK(parse_request(req, "GET / HTTP/1.1\r\n",
                             "Host: a\r\nConnection: close\r\n") == 0);
    TEST_CHECK(req->keep_alive == 0);
    TEST_CHECK(parse_request(req, "GET / HTTP/1.0\r\n",
                             "Connection: Keep-Alive\r\n") == 0);
    TEST_CHECK(req->keep_alive == 1);
    free(req);
}

static void test_content_length_errors(void)
{
    struct http_request *req = new_request();
    long n;

    TEST_CHECK(parse_request(req, "POST / HTTP/1.1\r\n",
                             "Host: a\r\nContent-Length: abc\r\n") == 400);
    TEST_CHECK(parse_request(req, "POST / HTTP/1.1\r\n",
                             "Host: a\r\nContent-Length: -1\r\n") == 400);
    TEST_CHECK(parse_request(req, "POST / HTTP/1.1\r\n",
                             "Host: a\r\nContent-Length: 7\r\n"
                             "Content-Length: 8\r\n") == 400);
    /* 같은 값이 두 번이면 허용 */
    TEST_CHECK(parse_request(req, "POST / HTTP/1.1\r\n",
                             "Host: a\r\nContent-Length: 7\r\n"
                             "Content-Length: 7\r\n") == 0);
    /* 상한 초과 → 413 (본문을 받기 전) */
    TEST_CHECK(parse_request(req, "POST / HTTP/1.1\r\n",
                             "Host: a\r\nContent-Length: 2000000\r\n") == 413);
    TEST_CHECK(http_parse_content_length("0", &n) == 0 && n == 0);
    TEST_CHECK(http_parse_content_length("", &n) == -1);
    free(req);
}

static void test_transfer_encoding_and_host(void)
{
    struct http_request *req = new_request();

    TEST_CHECK(parse_request(req, "POST / HTTP/1.1\r\n",
                             "Host: a\r\nContent-Length: 5\r\n"
                             "Transfer-Encoding: chunked\r\n") == 400);
    TEST_CHECK(parse_request(req, "POST / HTTP/1.1\r\n",
                             "Host: a\r\nTransfer-Encoding: chunked\r\n") ==
               501);
    TEST_CHECK(parse_request(req, "GET / HTTP/1.1\r\n", "") == 400);
    TEST_CHECK(parse_request(req, "GET / HTTP/1.0\r\n", "") == 0);
    TEST_CHECK(parse_request(req, "GET / HTTP/1.1\r\n",
                             "Host: a\r\nHost: b\r\n") == 400);
    /* CL과 TE가 모두 없으면 본문 0바이트 */
    TEST_CHECK(parse_request(req, "POST / HTTP/1.1\r\n", "Host: a\r\n") == 0);
    TEST_CHECK(http_body_length(req) == 0);
    free(req);
}

static void test_expect(void)
{
    struct http_request *req = new_request();

    TEST_CHECK(parse_request(req, "POST / HTTP/1.1\r\n",
                             "Host: a\r\nExpect: 100-continue\r\n") == 0);
    TEST_CHECK(req->expect == HTTP_EXPECT_CONTINUE);
    TEST_CHECK(parse_request(req, "POST / HTTP/1.1\r\n",
                             "Host: a\r\nExpect: foo\r\n") == 417);
    /* HTTP/1.0은 Expect 무시 */
    TEST_CHECK(parse_request(req, "POST / HTTP/1.0\r\n", "Expect: foo\r\n") ==
               0);
    TEST_CHECK(req->expect == HTTP_EXPECT_NONE);
    free(req);
}

static void test_unknown_header_kept_in_raw_block(void)
{
    struct http_request *req = new_request();

    TEST_CHECK(parse_request(req, "GET / HTTP/1.1\r\n",
                             "Host: a\r\nX-Foo: bar\r\n") == 0);
    TEST_CHECK(req->raw_len == strlen("Host: a\r\nX-Foo: bar\r\n"));
    TEST_CHECK(memcmp(req->raw_headers, "Host: a\r\nX-Foo: bar\r\n",
                      req->raw_len) == 0);
    free(req);
}

static void test_bad_header_lines(void)
{
    struct http_request *req = new_request();
    char line[200];

    TEST_CHECK(parse_request(req, "GET / HTTP/1.1\r\n",
                             "Host: a\r\nno colon here\r\n") == 400);
    TEST_CHECK(parse_request(req, "GET / HTTP/1.1\r\n",
                             "Host: a\r\nBad Name: x\r\n") == 400);
    /* 줄바꿈 없는 헤더 줄 (MAXLINE 초과로 잘림) */
    http_request_init(req);
    TEST_CHECK(http_parse_header_line(req, "X: y", 4, HTTP_LINE_MAX) == 400);

    /* 헤더 전체가 8 KiB를 넘으면 400 */
    http_request_init(req);
    TEST_CHECK(parse_line(req, "GET / HTTP/1.1\r\n") == 0);
    memset(line, 'a', sizeof(line));
    memcpy(line, "X-Pad: ", 7);
    line[sizeof(line) - 2] = '\r';
    line[sizeof(line) - 1] = '\n';
    int rc = 0;
    for (int i = 0; i < 100 && rc == 0; i++) {
        rc = http_parse_header_line(req, line, sizeof(line), HTTP_LINE_MAX);
    }
    TEST_CHECK(rc == 400);
    free(req);
}

static void test_mime(void)
{
    TEST_CHECK(strcmp(http_mime_type("./home.html"), "text/html") == 0);
    TEST_CHECK(strcmp(http_mime_type("./style.css"), "text/css") == 0);
    TEST_CHECK(strcmp(http_mime_type("./app.js"), "text/javascript") == 0);
    TEST_CHECK(strcmp(http_mime_type("./data.json"), "text/plain") == 0);
    TEST_CHECK(strcmp(http_mime_type("./a.css.txt"), "text/plain") == 0);
    TEST_CHECK(strcmp(http_mime_type("./photo.jpeg"), "image/jpeg") == 0);
    TEST_CHECK(strcmp(http_mime_type("./dir.v2/README"), "text/plain") == 0);
}

/* NUL이 든 줄은 strlen으로 길이를 셀 수 없어 크기를 직접 넘김 */
#define PARSE_HEADER_BYTES(req, s)                                             \
    http_parse_header_line(req, s, sizeof(s) - 1, HTTP_LINE_MAX)

static void test_nul_byte_is_rejected(void)
{
    struct http_request *req = new_request();
    static const char line[] = "GET /home.html\0zzz HTTP/1.1\r\n";

    TEST_CHECK(http_parse_request_line(req, line, sizeof(line) - 1,
                                       HTTP_LINE_MAX) == 400);
    http_request_init(req);
    TEST_CHECK(PARSE_HEADER_BYTES(req, "Content-Length: 7\0abc\r\n") == 400);
    TEST_CHECK(!req->has_content_length);
    TEST_CHECK(req->raw_len == 0); /* 프록시가 넘길 원본에도 남지 않음 */
    free(req);
}

static void test_cr_inside_line_is_rejected(void)
{
    struct http_request *req = new_request();

    TEST_CHECK(parse_line(req, "GET /a\rb HTTP/1.1\r\n") == 400);
    http_request_init(req);
    TEST_CHECK(parse_header(req, "X-A: 1\rContent-Length: 5\r\n") == 400);
    TEST_CHECK(req->raw_len == 0);
    /* 줄바꿈이 \n 하나인 줄은 지금처럼 받음 */
    TEST_CHECK(parse_header(req, "X-A: 1\n") == 0);
    free(req);
}

static void test_uri_must_start_with_slash(void)
{
    struct http_request *req = new_request();

    TEST_CHECK(parse_line(req, "GET ./secret.txt HTTP/1.1\r\n") == 400);
    TEST_CHECK(parse_line(req, "GET home.html HTTP/1.1\r\n") == 400);
    TEST_CHECK(parse_line(req, "GET http://a/ HTTP/1.1\r\n") == 400);
    /* 모르는 메서드는 URI 형식과 상관없이 501 */
    TEST_CHECK(parse_line(req, "OPTIONS * HTTP/1.1\r\n") == 501);
    free(req);
}

static void test_host_format(void)
{
    static const char *const good[] = {"a",
                                       "a:8000",
                                       "my_host.example.com",
                                       "127.0.0.1:80",
                                       "[::1]",
                                       "[::1]:80",
                                       "a:",
                                       ""};
    static const char *const bad[] = {"a b",  "a/b", "a@b",    "a:80x", "a:b:c",
                                      "[::1", "[]",  "[::1]x", "<x>",   "a\"b"};
    struct http_request *req = new_request();
    char line[64];

    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        http_request_init(req);
        snprintf(line, sizeof(line), "Host: %s\r\n", good[i]);
        TEST_CHECK_(parse_header(req, line) == 0, "Host '%s'", good[i]);
    }
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        http_request_init(req);
        snprintf(line, sizeof(line), "Host: %s\r\n", bad[i]);
        TEST_CHECK_(parse_header(req, line) == 400, "Host '%s'", bad[i]);
    }
    free(req);
}

/* 블록을 걸러 낸 결과가 expected와 같은지 */
static int filter_gives(const char *block, const char *const *skip,
                        const char *expected)
{
    char out[256];
    long n = http_headers_filter(block, strlen(block), skip, out, sizeof(out));

    return n == (long)strlen(expected) && memcmp(out, expected, (size_t)n) == 0;
}

static void test_header_next_splits_lines(void)
{
    const char *block = "Content-Type:  text/html \r\nX: a:b\nEmpty:\r\n";
    struct http_header h;
    size_t pos = 0;

    TEST_CHECK(http_header_next(block, strlen(block), &pos, &h) == 1);
    TEST_CHECK(http_header_is(&h, "content-type"));
    TEST_CHECK(h.value_len == 9 && memcmp(h.value, "text/html", 9) == 0);
    TEST_CHECK(http_header_next(block, strlen(block), &pos, &h) == 1);
    TEST_CHECK(h.value_len == 3 && memcmp(h.value, "a:b", 3) == 0);
    TEST_CHECK(http_header_next(block, strlen(block), &pos, &h) == 1);
    TEST_CHECK(http_header_is(&h, "Empty") && h.value_len == 0);
    TEST_CHECK(http_header_next(block, strlen(block), &pos, &h) == 0);
}

static void test_header_next_rejects_bad_lines(void)
{
    static const char *const bad[] = {"no colon\r\n", ": value\r\n",
                                      "Bad Name: x\r\n", " folded: x\r\n",
                                      "X: a\rb\r\n"};

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        struct http_header h;
        size_t pos = 0;
        TEST_CHECK_(http_header_next(bad[i], strlen(bad[i]), &pos, &h) == -1,
                    "%s", bad[i]);
    }
}

static void test_filter_drops_hop_by_hop(void)
{
    static const char *const none[] = {NULL};
    static const char *const skip[] = {"Content-Length", NULL};

    TEST_CHECK(filter_gives("A: 1\r\nConnection: close\r\nKeep-Alive: x\r\n"
                            "TE: y\r\nUpgrade: z\r\nB: 2\r\n",
                            none, "A: 1\r\nB: 2\r\n"));
    /* Connection에 적힌 이름. 대소문자를 구분하지 않고, 앞부분만 같으면 아님 */
    TEST_CHECK(filter_gives("X-Hop: 1\r\nConnection: close, x-hop\r\n"
                            "X-Hop2: 2\r\n",
                            none, "X-Hop2: 2\r\n"));
    /* 호출한 쪽이 직접 쓸 헤더 */
    TEST_CHECK(filter_gives("Content-Length: 3\r\nA: 1\r\n", skip, "A: 1\r\n"));
    TEST_CHECK(filter_gives("", none, ""));
    TEST_CHECK(!filter_gives("broken\r\n", none, ""));
}

static void test_filter_reports_small_buffer(void)
{
    static const char *const none[] = {NULL};
    char out[8];

    TEST_CHECK(http_headers_filter("Abc: 1\r\n", 8, none, out, 8) == 8);
    TEST_CHECK(http_headers_filter("Abcd: 1\r\n", 9, none, out, 8) == -1);
}

TEST_LIST = {
    {"request_line_ok", test_request_line_ok},
    {"request_line_errors", test_request_line_errors},
    {"header_case_and_spaces", test_header_case_and_spaces},
    {"connection_defaults", test_connection_defaults},
    {"content_length_errors", test_content_length_errors},
    {"transfer_encoding_and_host", test_transfer_encoding_and_host},
    {"expect", test_expect},
    {"unknown_header_kept_in_raw_block", test_unknown_header_kept_in_raw_block},
    {"bad_header_lines", test_bad_header_lines},
    {"mime", test_mime},
    {"NUL이 든 요청 줄과 헤더 줄은 400이다", test_nul_byte_is_rejected},
    {"줄 중간에 CR이 있으면 400이다", test_cr_inside_line_is_rejected},
    {"/로 시작하지 않는 URI는 400이다", test_uri_must_start_with_slash},
    {"Host는 이름[:포트] 형식만 받고 나머지는 400이다", test_host_format},
    {"헤더 블록을 한 줄씩 이름과 값으로 나눈다", test_header_next_splits_lines},
    {"형식이 틀린 헤더 줄은 오류다", test_header_next_rejects_bad_lines},
    {"한 구간 전용 헤더와 Connection에 적힌 헤더를 뺀다",
     test_filter_drops_hop_by_hop},
    {"걸러 낸 헤더가 버퍼보다 길면 오류다", test_filter_reports_small_buffer},
    {NULL, NULL}};
