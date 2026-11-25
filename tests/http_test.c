/*
 * http 파서 단위 테스트 (m4m5.md 8.5)
 *   소켓 없이 문자열만 넣어 요청 구조체가 맞게 채워지는지 확인합니다.
 */
#include "acutest.h"
#include "http.h"

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

/* M7-1, M7-2: 프록시와 백엔드가 다르게 해석할 수 있는 요청 (m6m7.md 3장) */
static void test_request_validation(void)
{
    struct http_request *req = new_request();

    /* 메서드: 토큰이 아니면 400, 형식은 맞지만 모르면 501 */
    TEST_CHECK(parse_line(req, "G@T / HTTP/1.1\r\n") == 400);
    TEST_CHECK(parse_line(req, "GE(T / HTTP/1.1\r\n") == 400);
    TEST_CHECK(parse_line(req, "M-SEARCH_X / HTTP/1.1\r\n") == 501);
    TEST_CHECK(parse_line(req, "VERYLONGMETHODNAME / HTTP/1.1\r\n") == 501);
    TEST_CHECK(parse_line(req, "DELETE /api/users/3 HTTP/1.1\r\n") == 501);

    /* URI는 '/'로 시작해야 함 */
    TEST_CHECK(parse_line(req, "GET ./secret HTTP/1.1\r\n") == 400);
    TEST_CHECK(parse_line(req, "GET http://example.com/ HTTP/1.1\r\n") == 400);
    TEST_CHECK(parse_line(req, "GET * HTTP/1.1\r\n") == 400);

    /* 요청 줄의 제어 문자 (탭은 허용: 구분자) */
    TEST_CHECK(http_parse_request_line(req, "GET /a\0b HTTP/1.1\r\n", 19,
                                       HTTP_LINE_MAX) == 400);
    TEST_CHECK(parse_line(req, "GET /a\rb HTTP/1.1\r\n") == 400);
    TEST_CHECK(parse_line(req, "GET\t/\tHTTP/1.1\r\n") == 0);

    /* 헤더 이름은 토큰. ':' 앞 공백, 줄 앞 공백(obs-fold) 거절 */
    TEST_CHECK(parse_request(req, "GET / HTTP/1.1\r\n",
                             "Host: a\r\nConnection : close\r\n") == 400);
    TEST_CHECK(parse_request(req, "GET / HTTP/1.1\r\n",
                             "Host: a\r\n folded: x\r\n") == 400);
    TEST_CHECK(parse_request(req, "GET / HTTP/1.1\r\n",
                             "Host: a\r\nX(y): 1\r\n") == 400);
    /* 헤더 줄의 제어 문자 */
    TEST_CHECK(parse_request(req, "GET / HTTP/1.1\r\n",
                             "Host: a\r\nX-A: 1\rX-B: 2\r\n") == 400);
    http_request_init(req);
    TEST_CHECK(parse_line(req, "GET / HTTP/1.1\r\n") == 0);
    TEST_CHECK(
        http_parse_header_line(req, "X-A: a\0b\r\n", 10, HTTP_LINE_MAX) == 400);
    /* 값 안의 탭은 허용 */
    TEST_CHECK(parse_request(req, "GET / HTTP/1.1\r\n",
                             "Host: a\r\nX-A: a\tb\r\n") == 0);

    TEST_CHECK(http_is_token("Content-Type", 12));
    TEST_CHECK(!http_is_token("", 0));
    TEST_CHECK(!http_is_token("a b", 3));
    free(req);
}

/* F5: 요청 줄의 다른 부분에서 실패해도 HEAD인지는 알아 둠 (오류 응답의 본문을 빼려고) */
static void test_method_saved_before_other_checks(void)
{
    struct http_request *req = new_request();
    char longline[HTTP_LINE_MAX];

    TEST_CHECK(parse_line(req, "HEAD / HTTP/2.0\r\n") == 505);
    TEST_CHECK(strcmp(req->method, "HEAD") == 0);
    http_request_init(req);
    TEST_CHECK(parse_line(req, "head ./x HTTP/1.1\r\n") == 400);
    TEST_CHECK(strcmp(req->method, "HEAD") == 0);
    http_request_init(req);
    TEST_CHECK(parse_line(req, "HEAD / HTTP/1.1 extra\r\n") == 400);
    TEST_CHECK(strcmp(req->method, "HEAD") == 0);

    /* 줄바꿈 없이 버퍼가 다 찬 414에서도 첫 조각이 HEAD면 알아 둠 */
    http_request_init(req);
    memset(longline, 'a', sizeof(longline));
    memcpy(longline, "HEAD /", 6);
    TEST_CHECK(http_parse_request_line(req, longline, sizeof(longline) - 1,
                                       sizeof(longline)) == 414);
    TEST_CHECK(strcmp(req->method, "HEAD") == 0);

    /* 모르는 메서드나 토큰이 아닌 메서드는 저장하지 않음 */
    http_request_init(req);
    TEST_CHECK(parse_line(req, "PUT / HTTP/2.0\r\n") == 505);
    TEST_CHECK(req->method[0] == '\0');
    free(req);
}

/* F10: Host 값의 형식 검사 (RFC 9112 3.2, RFC 3986 3.2.2). 목록은 참고 커밋 7383e40 */
static void test_host_format(void)
{
    static const char *const good[] = {"a",
                                       "a:8000",
                                       "my_host.example.com",
                                       "127.0.0.1:80",
                                       "[::1]",
                                       "[::1]:80",
                                       "a:",
                                       "a%41b",
                                       "[::ffff:1.2.3.4]",
                                       "[2001:db8::1]:8080"};
    static const char *const bad[] = {
        "a b",     "a/b",       "a@b",       "a:80x",  "a:b:c",
        "[::1",    "[]",        "[::1]x",    "<x>",    "a\"b",
        "",        ":80",       "a%zz",      "a%4",    "a%",
        "[:::::]", "[12345::]", "[1.2.3.4]", "[v1.x]", "[::1%lo]"};
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

/* F11: 긴 Connection 값의 뒤쪽에 있는 close도 읽음 (RFC 9112 9.6) */
static void test_connection_close_at_end_of_long_value(void)
{
    struct http_request *req = new_request();
    char headers[1200];
    char filler[1000];

    for (size_t n = 250; n <= 260; n++) {
        memset(filler, 'a', n);
        filler[n] = '\0';
        snprintf(headers, sizeof(headers), "Host: x\r\nConnection: %s, close\r\n",
                 filler);
        TEST_CHECK(parse_request(req, "GET / HTTP/1.1\r\n", headers) == 0);
        TEST_CHECK_(req->keep_alive == 0, "앞 조각 %zu글자", n);
    }
    memset(filler, 'a', 900);
    filler[900] = '\0';
    snprintf(headers, sizeof(headers), "Host: x\r\nConnection: %s, keep-alive\r\n",
             filler);
    TEST_CHECK(parse_request(req, "GET / HTTP/1.0\r\n", headers) == 0);
    TEST_CHECK(req->keep_alive == 1);
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
    {"request_validation", test_request_validation},
    {"요청 줄 검사에 실패해도 HEAD 메서드는 먼저 저장한다",
     test_method_saved_before_other_checks},
    {"Host는 이름[:포트] 또는 [IPv6][:포트] 형식만 받고 나머지는 400이다",
     test_host_format},
    {"긴 Connection 값의 맨 뒤에 있는 close도 연결을 닫게 한다",
     test_connection_close_at_end_of_long_value},
    {"mime", test_mime},
    {NULL, NULL}};
