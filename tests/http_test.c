/*
 * http_test.c - 요청 파서 단위 테스트 (소켓 없이 문자열만)
 *
 * final.md 8.5절 항목과 엣지 케이스 문서(docs/plan/phase2-edge-cases-http.md)의
 * R, H, B(M5-1 부분), M 구분을 확인합니다. 각 테스트 이름 옆에 ID를 적었습니다.
 */
#include "acutest.h"
#include "http.h"

#include <string.h>
#include <unistd.h>

/* ---------- 도우미 ---------- */

static int req_line(struct http_request *req, const char *line)
{
    return http_request_line_parse(req, line, strlen(line));
}

static int hdr(struct http_request *req, const char *line)
{
    return http_header_line_parse(req, line, strlen(line));
}

/*
 * 요청 전체(요청 줄 + 헤더 + 빈 줄)를 tiny.c의 request_read와 같은 순서로 파싱.
 * 각 줄은 '\n'으로 끝나야 함. 성공 0, 실패하면 상태 코드
 */
static int parse_all(struct http_request *req, const char *text)
{
    const char *p = text;
    int first = 1;

    http_request_init(req);
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p + 1) : strlen(p);
        int status;

        if (first) {
            status = http_request_line_parse(req, p, len);
            first = 0;
        } else if (http_is_blank_line(p, len)) {
            return http_headers_validate(req);
        } else {
            status = http_header_line_parse(req, p, len);
        }
        if (status != 0) {
            return status;
        }
        p += len;
    }
    return -1; /* 빈 줄이 없음 (테스트 문자열 실수) */
}

/* ---------- 줄 상태 ---------- */

static void test_line_check(void)
{
    char buf[16];

    TEST_CHECK(http_line_check("GET\n", 4, sizeof(buf)) == HTTP_LINE_OK);
    TEST_CHECK(http_line_check("", 0, sizeof(buf)) == HTTP_LINE_INCOMPLETE);
    TEST_CHECK(http_line_check("", -1, sizeof(buf)) == HTTP_LINE_INCOMPLETE);
    /* 줄바꿈 전에 끊김 (E-R13) */
    TEST_CHECK(http_line_check("GET / HT", 8, sizeof(buf)) ==
               HTTP_LINE_INCOMPLETE);
    /* 버퍼(16)가 찰 때까지 줄바꿈 없음 (rio_readlineb는 최대 15글자) */
    memset(buf, 'a', sizeof(buf));
    TEST_CHECK(http_line_check(buf, 15, sizeof(buf)) == HTTP_LINE_TOO_LONG);
}

static void test_blank_line(void)
{
    TEST_CHECK(http_is_blank_line("\r\n", 2));
    TEST_CHECK(http_is_blank_line("\n", 1)); /* E-R10: \n만 써도 됨 */
    TEST_CHECK(!http_is_blank_line("X: y\r\n", 6));
    TEST_CHECK(!http_is_blank_line("", 0));
}

/* ---------- 요청 줄 (R) ---------- */

static void test_request_line_ok(void)
{
    struct http_request req;

    http_request_init(&req);
    TEST_CHECK(req_line(&req, "GET /home.html HTTP/1.1\r\n") == 0);
    TEST_CHECK(strcmp(req.method, "GET") == 0);
    TEST_CHECK(strcmp(req.uri, "/home.html") == 0);
    TEST_CHECK(strcmp(req.version, "HTTP/1.1") == 0);
    TEST_CHECK(req.minor == 1);

    http_request_init(&req);
    TEST_CHECK(req_line(&req, "HEAD /cgi-bin/adder?1&2 HTTP/1.0\n") == 0);
    TEST_CHECK(strcmp(req.uri, "/cgi-bin/adder?1&2") == 0);
    TEST_CHECK(req.minor == 0);
}

/* E-R2, E-R3, E-R11, E-R12: 세 부분이 아니거나 URI가 '/'로 시작하지 않음 → 400 */
static void test_request_line_bad_shape(void)
{
    struct http_request req;
    const char *bad[] = {
        "GET /\r\n",                  /* 두 부분 */
        "GET / HTTP/1.1 extra\r\n",   /* 네 부분 */
        "GET  /  HTTP/1.1\r\n",       /* 공백 여러 개 */
        " GET / HTTP/1.1\r\n",        /* 앞에 공백 */
        "GET http://x/ HTTP/1.1\r\n", /* 전체 주소 형식 */
        "\r\n",                       /* 빈 줄 */
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        http_request_init(&req);
        TEST_CHECK_(req_line(&req, bad[i]) == 400, "%s", bad[i]);
    }
}

/* E-R4: 버전 형식 오류 400, E-R5·E-R6: 지원하지 않는 버전 505 */
static void test_request_line_version(void)
{
    struct http_request req;

    http_request_init(&req);
    TEST_CHECK(req_line(&req, "GET / HTTP/1\r\n") == 400);
    http_request_init(&req);
    TEST_CHECK(req_line(&req, "GET / http/1.1\r\n") == 400);
    http_request_init(&req);
    TEST_CHECK(req_line(&req, "GET / HTTP/1.10\r\n") == 400);
    http_request_init(&req);
    TEST_CHECK(req_line(&req, "GET / HTTP/2.0\r\n") == 505);
    http_request_init(&req);
    TEST_CHECK(req_line(&req, "GET / HTTP/0.9\r\n") == 505);
    http_request_init(&req);
    TEST_CHECK(req_line(&req, "GET / HTTP/1.2\r\n") == 505);
    /* 버전을 메서드보다 먼저 봄 (final.md 3.3 순서) */
    http_request_init(&req);
    TEST_CHECK(req_line(&req, "FOO / HTTP/2.0\r\n") == 505);
}

/* E-R7, E-R8: GET, POST, HEAD가 아니면 501. 대소문자 구분 */
static void test_request_line_method(void)
{
    struct http_request req;

    http_request_init(&req);
    TEST_CHECK(req_line(&req, "FOO / HTTP/1.1\r\n") == 501);
    http_request_init(&req);
    TEST_CHECK(req_line(&req, "get / HTTP/1.1\r\n") == 501);
    http_request_init(&req);
    TEST_CHECK(req_line(&req, "AVERYLONGMETHODNAME / HTTP/1.1\r\n") == 501);
    http_request_init(&req);
    TEST_CHECK(req_line(&req, "POST / HTTP/1.1\r\n") == 0);
}

/* E-H16과 같은 규칙: 요청 줄 안의 NUL, 줄 끝이 아닌 CR → 400 */
static void test_request_line_bad_bytes(void)
{
    struct http_request req;
    const char nul[] = "GET /a\0b HTTP/1.1\r\n";

    http_request_init(&req);
    TEST_CHECK(http_request_line_parse(&req, nul, sizeof(nul) - 1) == 400);
    http_request_init(&req);
    TEST_CHECK(req_line(&req, "GET /a\rb HTTP/1.1\r\n") == 400);
}

/* ---------- 헤더 (H) ---------- */

/* E-H1, E-H2: 이름 대소문자 무시, 값 앞뒤 공백 제거 */
static void test_header_case_and_trim(void)
{
    struct http_request req;

    TEST_ASSERT(parse_all(&req, "POST / HTTP/1.1\r\n"
                                "host:   example.com  \r\n"
                                "CONTENT-LENGTH:   7  \r\n"
                                "content-type: text/plain\r\n"
                                "\r\n") == 0);
    TEST_CHECK(strcmp(req.host, "example.com") == 0);
    TEST_CHECK(req.has_content_length && req.content_length == 7);
    TEST_CHECK(strcmp(req.content_type, "text/plain") == 0);
}

/* E-H3: 모르는 헤더는 무시하지만 원본 블록에는 남음 */
static void test_unknown_header_kept_in_raw(void)
{
    struct http_request req;
    const char *raw = "Host: x\r\nX-Foo: bar\r\n";

    TEST_ASSERT(parse_all(&req, "GET / HTTP/1.1\r\n"
                                "Host: x\r\n"
                                "X-Foo: bar\r\n"
                                "\r\n") == 0);
    TEST_CHECK(req.raw_len == strlen(raw));
    TEST_CHECK(memcmp(req.raw_headers, raw, strlen(raw)) == 0);
}

/* E-H4, E-H10, E-H11, E-H16: 모양이 틀린 헤더 줄 → 400 */
static void test_header_bad_shape(void)
{
    struct http_request req;
    const char *bad[] = {
        "BadHeader\r\n",   /* ':' 없음 */
        ": value\r\n",     /* 이름 없음 */
        "Host : x\r\n",    /* 이름 뒤 공백 */
        "  continued\r\n", /* 줄 접기 */
        "\tcontinued\r\n", /* 줄 접기 (탭) */
        "X-A: a\rb\r\n",   /* 중간에 CR */
    };
    const char nul[] = "X-A: a\0b\r\n";

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        http_request_init(&req);
        TEST_CHECK_(hdr(&req, bad[i]) == 400, "%s", bad[i]);
    }
    http_request_init(&req);
    TEST_CHECK(http_header_line_parse(&req, nul, sizeof(nul) - 1) == 400);
}

/* E-H6: 헤더 전체 8 KiB 초과 → 400 */
static void test_headers_total_limit(void)
{
    struct http_request req;
    char line[101];
    int status = 0;

    /* "X-Pad: aaaa...\r\n" 100바이트짜리 줄 */
    memset(line, 'a', sizeof(line));
    memcpy(line, "X-Pad: ", 7);
    line[98] = '\r';
    line[99] = '\n';
    line[100] = '\0';

    http_request_init(&req);
    for (int i = 0; i < 100 && status == 0; i++) { /* 100줄 = 10000바이트 */
        status = hdr(&req, line);
    }
    TEST_CHECK(status == 400);
    TEST_CHECK(req.raw_len <= HDR_RAW_MAX);

    /* 상한 안쪽(81줄 = 8100바이트)은 통과 */
    http_request_init(&req);
    status = 0;
    for (int i = 0; i < 81 && status == 0; i++) {
        status = hdr(&req, line);
    }
    TEST_CHECK(status == 0);
}

/* E-H7, E-H8, E-H9, E-H14: Host 규칙 */
static void test_host_rules(void)
{
    struct http_request req;

    TEST_CHECK(parse_all(&req, "GET / HTTP/1.1\r\n\r\n") == 400); /* 1.1 없음 */
    TEST_CHECK(parse_all(&req, "GET / HTTP/1.0\r\n\r\n") == 0);   /* 1.0 허용 */
    TEST_CHECK(parse_all(&req, "GET / HTTP/1.1\r\n"
                               "Host: a\r\nHost: b\r\n\r\n") == 400); /* 중복 */
    TEST_CHECK(parse_all(&req, "GET / HTTP/1.1\r\nHost:\r\n\r\n") == 400);
}

/* E-H15: 저장 버퍼(256)보다 긴 값은 자르지 않고 400 */
static void test_value_too_long(void)
{
    struct http_request req;
    char line[400];

    memset(line, 'a', sizeof(line));
    memcpy(line, "Content-Type: ", 14);
    memcpy(line + 300, "\r\n", 3);

    http_request_init(&req);
    TEST_CHECK(hdr(&req, line) == 400);
}

/* E-H12: Connection 토큰 목록, 대소문자 무시 / 기본값: 1.1 유지, 1.0 닫기 */
static void test_connection_keep_alive(void)
{
    struct http_request req;

    TEST_ASSERT(parse_all(&req, "GET / HTTP/1.1\r\nHost: x\r\n\r\n") == 0);
    TEST_CHECK(req.keep_alive == 1);
    TEST_ASSERT(parse_all(&req, "GET / HTTP/1.0\r\n\r\n") == 0);
    TEST_CHECK(req.keep_alive == 0);
    TEST_ASSERT(parse_all(&req, "GET / HTTP/1.1\r\nHost: x\r\n"
                                "Connection: Close\r\n\r\n") == 0);
    TEST_CHECK(req.keep_alive == 0);
    TEST_ASSERT(parse_all(&req, "GET / HTTP/1.0\r\n"
                                "Connection: keep-alive\r\n\r\n") == 0);
    TEST_CHECK(req.keep_alive == 1);
    TEST_ASSERT(parse_all(&req,
                          "GET / HTTP/1.1\r\nHost: x\r\n"
                          "Connection: keep-alive, Upgrade\r\n\r\n") == 0);
    TEST_CHECK(req.keep_alive == 1);
    TEST_ASSERT(parse_all(&req, "GET / HTTP/1.1\r\nHost: x\r\n"
                                "Connection: Upgrade, close\r\n\r\n") == 0);
    TEST_CHECK(req.keep_alive == 0);
}

/* E-X6: Expect 값 저장 (판단은 M5-2) */
static void test_expect_stored(void)
{
    struct http_request req;

    TEST_ASSERT(parse_all(&req, "GET / HTTP/1.1\r\nHost: x\r\n\r\n") == 0);
    TEST_CHECK(req.expect == HTTP_EXPECT_NONE);
    TEST_ASSERT(parse_all(&req, "POST / HTTP/1.1\r\nHost: x\r\n"
                                "Expect: 100-Continue\r\n\r\n") == 0);
    TEST_CHECK(req.expect == HTTP_EXPECT_100_CONTINUE);
    TEST_ASSERT(parse_all(&req, "POST / HTTP/1.1\r\nHost: x\r\n"
                                "Expect: foo\r\n\r\n") == 0);
    TEST_CHECK(req.expect == HTTP_EXPECT_OTHER);
}

/* ---------- 본문 길이 (B, M5-1 부분) ---------- */

/* E-B3 ~ E-B7: Content-Length 형식 */
static void test_content_length_format(void)
{
    struct http_request req;
    const char *bad[] = {
        "Content-Length: abc\r\n",
        "Content-Length: -1\r\n",
        "Content-Length: +7\r\n",
        "Content-Length: 7abc\r\n",
        "Content-Length: 7, 7\r\n",
        "Content-Length:\r\n",
        "Content-Length: 99999999999999999999\r\n", /* long 범위 초과 */
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        http_request_init(&req);
        TEST_CHECK_(hdr(&req, bad[i]) == 400, "%s", bad[i]);
    }

    http_request_init(&req);
    TEST_CHECK(hdr(&req, "Content-Length: 007\r\n") == 0);
    TEST_CHECK(req.content_length == 7);
    TEST_CHECK(hdr(&req, "Content-Length: 7\r\n") == 0);   /* 같은 값 반복 */
    TEST_CHECK(hdr(&req, "Content-Length: 8\r\n") == 400); /* 다른 값 */
}

/* E-B8, E-B9: CL+TE → 400, TE만 → 501 */
static void test_transfer_encoding(void)
{
    struct http_request req;

    TEST_CHECK(parse_all(&req, "POST / HTTP/1.1\r\nHost: x\r\n"
                               "Content-Length: 5\r\n"
                               "Transfer-Encoding: chunked\r\n\r\n") == 400);
    TEST_CHECK(parse_all(&req, "POST / HTTP/1.1\r\nHost: x\r\n"
                               "Transfer-Encoding: chunked\r\n\r\n") == 501);
    TEST_CHECK(parse_all(&req, "POST / HTTP/1.1\r\nHost: x\r\n"
                               "Transfer-Encoding: identity\r\n\r\n") == 501);
}

/* E-B1: CL도 TE도 없으면 본문 0바이트 (파서는 has_content_length = 0) */
static void test_no_body_headers(void)
{
    struct http_request req;

    TEST_ASSERT(parse_all(&req, "POST / HTTP/1.1\r\nHost: x\r\n\r\n") == 0);
    TEST_CHECK(!req.has_content_length && !req.has_transfer_encoding);
}

/* ---------- MIME (M) ---------- */

/* E-M1 ~ E-M6 */
static void test_mime_type(void)
{
    TEST_CHECK(strcmp(http_mime_type("./home.html"), "text/html") == 0);
    TEST_CHECK(strcmp(http_mime_type("./style.css"), "text/css") == 0);
    TEST_CHECK(strcmp(http_mime_type("./app.js"), "text/javascript") == 0);
    TEST_CHECK(strcmp(http_mime_type("./data.json"), "text/plain") == 0);
    TEST_CHECK(strcmp(http_mime_type("./a.css.txt"), "text/plain") == 0);
    TEST_CHECK(strcmp(http_mime_type("./godzilla.gif"), "image/gif") == 0);
    TEST_CHECK(strcmp(http_mime_type("./godzilla.jpg"), "image/jpeg") == 0);
    TEST_CHECK(strcmp(http_mime_type("./photo.jpeg"), "image/jpeg") == 0);
    TEST_CHECK(strcmp(http_mime_type("./a.png"), "image/png") == 0);
    TEST_CHECK(strcmp(http_mime_type("./README"), "text/plain") == 0);
    TEST_CHECK(strcmp(http_mime_type("./INDEX.HTML"), "text/plain") == 0);
    TEST_CHECK(strcmp(http_mime_type("./v1.2/readme"), "text/plain") == 0);
    TEST_CHECK(strcmp(http_mime_type("./.hidden"), "text/plain") == 0);
}

static void test_status_text(void)
{
    TEST_CHECK(strcmp(http_status_text(404), "Not Found") == 0);
    TEST_CHECK(strcmp(http_status_text(414), "URI Too Long") == 0);
    TEST_CHECK(strcmp(http_status_text(505), "HTTP Version Not Supported") ==
               0);
}

/* ---------- 본문 (B, X: M5-2) ---------- */

/* 문자열 하나로 Content-Length만 정한 요청 */
static void req_with_cl(struct http_request *req, long cl)
{
    http_request_init(req);
    req->minor = 1;
    req->has_content_length = 1;
    req->content_length = cl;
}

/* E-B10, E-B14: 1 MiB까지는 받고, 넘으면 413 (malloc 전에) */
static void test_body_check_limit(void)
{
    struct http_request req;

    req_with_cl(&req, BODY_MAX);
    TEST_CHECK(http_body_check(&req) == 0);
    req_with_cl(&req, BODY_MAX + 1L);
    TEST_CHECK(http_body_check(&req) == 413);
    TEST_CHECK(req.body == NULL); /* 검사만 하고 메모리는 잡지 않음 */
    http_request_init(&req);      /* CL 없음 */
    TEST_CHECK(http_body_check(&req) == 0);
}

/* E-X1 ~ E-X4, E-B2: Expect 판단 */
static void test_expect_check(void)
{
    struct http_request req;

    req_with_cl(&req, 7);
    TEST_CHECK(http_expect_check(&req) == 0); /* Expect 없음 */
    req.expect = HTTP_EXPECT_100_CONTINUE;
    TEST_CHECK(http_expect_check(&req) == HTTP_EXPECT_SEND_100);
    req.expect = HTTP_EXPECT_OTHER;
    TEST_CHECK(http_expect_check(&req) == 417);
    req.minor = 0; /* HTTP/1.0은 Expect를 무시 */
    TEST_CHECK(http_expect_check(&req) == 0);

    req_with_cl(&req, 0); /* 본문 0바이트면 100을 보낼 이유가 없음 */
    req.expect = HTTP_EXPECT_100_CONTINUE;
    TEST_CHECK(http_expect_check(&req) == 0);
}

/* pipe의 쓰기 쪽에 data를 쓰고, close_write면 닫음. 읽기 쪽 fd를 돌려줌.
 * pipe 버퍼(보통 64 KiB)보다 작은 데이터만 씀 */
static int pipe_with(const char *data, size_t len, int close_write, int *wfd)
{
    int fds[2];

    if (pipe(fds) < 0) {
        return -1;
    }
    if (write(fds[1], data, len) != (ssize_t)len) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (close_write) {
        close(fds[1]);
        *wfd = -1;
    } else {
        *wfd = fds[1];
    }
    return fds[0];
}

static void test_body_read_ok(void)
{
    struct http_request req;
    rio_t rio;
    int wfd;
    int rfd = pipe_with("hello", 5, 1, &wfd);

    TEST_ASSERT(rfd >= 0);
    rio_readinitb(&rio, rfd);
    req_with_cl(&req, 5);
    TEST_CHECK(http_body_read(&req, &rio) == 0);
    TEST_CHECK(req.body_len == 5);
    TEST_CHECK(req.body != NULL && memcmp(req.body, "hello", 5) == 0);
    http_request_free(&req);
    TEST_CHECK(req.body == NULL);
    close(rfd);
}

/* E-B12: 본문이 CL보다 짧은 채 끊김 → -1, 버퍼는 해제됨 (LeakSanitizer가 확인) */
static void test_body_read_short(void)
{
    struct http_request req;
    rio_t rio;
    int wfd;
    int rfd = pipe_with("ab", 2, 1, &wfd);

    TEST_ASSERT(rfd >= 0);
    rio_readinitb(&rio, rfd);
    req_with_cl(&req, 7);
    TEST_CHECK(http_body_read(&req, &rio) == -1);
    TEST_CHECK(req.body == NULL);
    TEST_CHECK(req.body_len == 0);
    close(rfd);
}

/* E-B1: CL이 없으면 읽지 않고 body = NULL */
static void test_body_read_none(void)
{
    struct http_request req;
    rio_t rio;
    int wfd;
    int rfd = pipe_with("x", 1, 1, &wfd);

    TEST_ASSERT(rfd >= 0);
    rio_readinitb(&rio, rfd);
    http_request_init(&req);
    TEST_CHECK(http_body_read(&req, &rio) == 0);
    TEST_CHECK(req.body == NULL && req.body_len == 0);
    close(rfd);
}

/* E-B16: 헤더를 읽을 때 rio가 본문 앞부분까지 미리 읽어 둠 → 같은 rio로 읽어야 함 */
static void test_body_after_headers_same_rio(void)
{
    const char text[] = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
                        "\r\nhello";
    struct http_request req;
    char line[MAXLINE];
    rio_t rio;
    ssize_t n;
    int wfd;
    int rfd = pipe_with(text, sizeof(text) - 1, 1, &wfd);
    int status;

    TEST_ASSERT(rfd >= 0);
    rio_readinitb(&rio, rfd);
    http_request_init(&req);
    n = rio_readlineb(&rio, line, sizeof(line));
    TEST_ASSERT(http_request_line_parse(&req, line, (size_t)n) == 0);
    while ((n = rio_readlineb(&rio, line, sizeof(line))) > 0 &&
           !http_is_blank_line(line, (size_t)n)) {
        TEST_ASSERT(http_header_line_parse(&req, line, (size_t)n) == 0);
    }
    TEST_ASSERT(http_headers_validate(&req) == 0);
    status = http_body_read(&req, &rio);
    TEST_CHECK(status == 0);
    TEST_CHECK(req.body_len == 5 && memcmp(req.body, "hello", 5) == 0);
    http_request_free(&req);
    close(rfd);
}

TEST_LIST = {
    {"line_check", test_line_check},
    {"blank_line", test_blank_line},
    {"request_line_ok", test_request_line_ok},
    {"request_line_bad_shape", test_request_line_bad_shape},
    {"request_line_version", test_request_line_version},
    {"request_line_method", test_request_line_method},
    {"request_line_bad_bytes", test_request_line_bad_bytes},
    {"header_case_and_trim", test_header_case_and_trim},
    {"unknown_header_kept_in_raw", test_unknown_header_kept_in_raw},
    {"header_bad_shape", test_header_bad_shape},
    {"headers_total_limit", test_headers_total_limit},
    {"host_rules", test_host_rules},
    {"value_too_long", test_value_too_long},
    {"connection_keep_alive", test_connection_keep_alive},
    {"expect_stored", test_expect_stored},
    {"content_length_format", test_content_length_format},
    {"transfer_encoding", test_transfer_encoding},
    {"no_body_headers", test_no_body_headers},
    {"mime_type", test_mime_type},
    {"status_text", test_status_text},
    {"body_check_limit", test_body_check_limit},
    {"expect_check", test_expect_check},
    {"body_read_ok", test_body_read_ok},
    {"body_read_short", test_body_read_short},
    {"body_read_none", test_body_read_none},
    {"body_after_headers_same_rio", test_body_after_headers_same_rio},
    {NULL, NULL},
};
