/*
 * cgi_test.c - CGI 실행(cgi_run)과 출력 해석(cgi_response_build) 단위 테스트
 *
 * make test는 레포 맨 위 폴더에서 실행하므로 픽스처 CGI를 상대 경로로 부릅니다.
 * 픽스처에 없는 동작은 임시 폴더에 작은 셸 스크립트를 만들어 씁니다.
 * 실패 경로마다 fd가 새지 않았는지(다음에 열리는 fd 번호가 그대로인지) 확인하고,
 * 출력 버퍼 누수는 Linux CI의 LeakSanitizer가 확인합니다 (final.md 8.6).
 * 엣지 케이스 ID: phase2-edge-cases-http.md의 G, 동시성 문서의 P
 */
#include "acutest.h"
#include "cgi.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FIXTURE "tests/fixtures/www/cgi-bin/"
#define CLOSE_MAX 256

static char *empty_env[] = {NULL};

/* 다음에 열릴 fd 번호. 테스트 전후로 같으면 fd가 새지 않은 것 */
static int next_fd(void)
{
    int fd = open("/dev/null", O_RDONLY);

    close(fd);
    return fd;
}

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* body를 담은 셸 스크립트를 임시 폴더에 만들고 경로를 path에 씀 */
static int script_make(char *path, size_t cap, const char *body)
{
    int fd;
    size_t len = strlen(body);

    snprintf(path, cap, "/tmp/tiny-cgi-test-XXXXXX");
    fd = mkstemp(path);
    if (fd < 0) {
        return -1;
    }
    if (write(fd, "#!/bin/sh\n", 10) != 10 ||
        write(fd, body, len) != (ssize_t)len || fchmod(fd, 0700) < 0) {
        close(fd);
        unlink(path);
        return -1;
    }
    close(fd); /* 쓰기용으로 열린 채 실행하면 Linux는 ETXTBSY로 거절 */
    return 0;
}

/* 픽스처나 스크립트를 /dev/null 입력으로 실행 */
static int run(const char *path, int timeout, size_t out_max,
               struct cgi_output *out)
{
    int infd = open("/dev/null", O_RDONLY);
    int r = cgi_run(path, empty_env, infd, timeout, out_max, CLOSE_MAX, out);

    close(infd);
    return r;
}

/* ---------- cgi_run ---------- */

/* E-G2: 종료 코드 1 → 500, 출력 버퍼 없음 */
static void test_run_fail(void)
{
    struct cgi_output out;
    int before = next_fd();

    TEST_CHECK(run(FIXTURE "fail", 5, CGI_OUT_MAX, &out) == 500);
    TEST_CHECK(out.data == NULL && out.len == 0);
    TEST_CHECK(next_fd() == before);
}

/* E-G1: 실행할 수 없음 → 자식 _exit(127) → 500 */
static void test_run_exec_fails(void)
{
    struct cgi_output out;

    TEST_CHECK(run(FIXTURE "no-such-cgi", 5, CGI_OUT_MAX, &out) == 500);
    TEST_CHECK(out.data == NULL);
}

/* E-G4: 실행은 성공하지만 빈 줄이 없어 해석 단계에서 500 */
static void test_run_nohdr(void)
{
    struct cgi_output out;
    char header[1024];
    const char *body;
    size_t body_len;
    int status;
    int before = next_fd();

    TEST_ASSERT(run(FIXTURE "nohdr", 5, CGI_OUT_MAX, &out) == 0);
    TEST_CHECK(out.len > 0);
    TEST_CHECK(cgi_response_build(out.data, out.len, 0, header, sizeof(header),
                                  &status, &body, &body_len) == 500);
    cgi_output_free(&out);
    TEST_CHECK(out.data == NULL);
    TEST_CHECK(next_fd() == before);
}

/* E-G9: 출력 1 MiB 초과 → CGI를 멈추고 500 (2 MiB를 다 기다리지 않음) */
static void test_run_big(void)
{
    struct cgi_output out;
    int before = next_fd();

    TEST_CHECK(run(FIXTURE "big", 5, CGI_OUT_MAX, &out) == 500);
    TEST_CHECK(out.data == NULL);
    TEST_CHECK(next_fd() == before);
}

/* E-G10: 상한과 정확히 같은 크기는 받고, 1바이트라도 넘으면 500 */
static void test_run_output_limit_edge(void)
{
    struct cgi_output out;
    char path[64];

    TEST_ASSERT(script_make(path, sizeof(path), "printf '0123456789'\n") == 0);
    TEST_CHECK(run(path, 5, 10, &out) == 0);
    TEST_CHECK(out.len == 10);
    cgi_output_free(&out);
    TEST_CHECK(run(path, 5, 9, &out) == 500);
    unlink(path);
}

/* E-G11, E-P3: 끝나지 않는 CGI → 시간 상한에서 504 (테스트는 1초로 줄임) */
static void test_run_slow_timeout(void)
{
    struct cgi_output out;
    double start = now_sec();
    int before = next_fd();

    TEST_CHECK(run(FIXTURE "slow", 1, CGI_OUT_MAX, &out) == 504);
    TEST_CHECK(now_sec() - start < 3);
    TEST_CHECK(out.data == NULL);
    TEST_CHECK(next_fd() == before);
}

/* E-P4: stdout을 닫고 계속 도는 CGI → EOF 뒤에도 마감을 지켜 504 */
static void test_run_closes_stdout_then_hangs(void)
{
    struct cgi_output out;
    char path[64];
    double start = now_sec();

    TEST_ASSERT(script_make(path, sizeof(path),
                            "printf 'Content-type: text/plain\\r\\n\\r\\nhi'\n"
                            "exec 1>&-\n"
                            "exec /bin/sleep 30\n") == 0);
    TEST_CHECK(run(path, 1, CGI_OUT_MAX, &out) == 504);
    TEST_CHECK(now_sec() - start < 3);
    unlink(path);
}

/* E-G12: 출력이 천천히 와도 전체 시간 안이면 정상 */
static void test_run_slow_output_within_deadline(void)
{
    struct cgi_output out;
    char path[64];

    TEST_ASSERT(script_make(path, sizeof(path),
                            "printf 'Content-type: text/plain\\r\\n\\r\\n'\n"
                            "for i in 1 2 3; do echo line; /bin/sleep 0.2; "
                            "done\n") == 0);
    TEST_CHECK(run(path, 3, CGI_OUT_MAX, &out) == 0);
    /* 헤더 28바이트 + "line\n" 3번 = 43바이트 (out.data는 NUL로 끝나지 않음) */
    TEST_CHECK(out.len == 28 + 15);
    TEST_CHECK(out.len >= 5 &&
               memcmp(out.data + out.len - 5, "line\n", 5) == 0);
    cgi_output_free(&out);
    unlink(path);
}

/* stdin으로 넘긴 fd를 CGI가 읽음 (M5-2의 본문 임시 파일 자리) */
static void test_run_reads_stdin(void)
{
    const char text[] = "Content-type: text/plain\r\n\r\nhello";
    struct cgi_output out;
    char path[64], inpath[64];
    int infd;

    TEST_ASSERT(script_make(path, sizeof(path), "exec /bin/cat\n") == 0);
    snprintf(inpath, sizeof(inpath), "/tmp/tiny-cgi-in-XXXXXX");
    infd = mkstemp(inpath);
    TEST_ASSERT(infd >= 0);
    unlink(inpath);
    TEST_ASSERT(write(infd, text, sizeof(text) - 1) ==
                (ssize_t)(sizeof(text) - 1));
    lseek(infd, 0, SEEK_SET);

    TEST_CHECK(
        cgi_run(path, empty_env, infd, 5, CGI_OUT_MAX, CLOSE_MAX, &out) == 0);
    TEST_CHECK(out.len == sizeof(text) - 1);
    TEST_CHECK(out.data && memcmp(out.data, text, out.len) == 0);
    cgi_output_free(&out);
    close(infd);
    unlink(path);
}

/* ---------- cgi_response_build ---------- */

static int build(const char *data, int keep_alive, char *header, size_t cap,
                 int *status, const char **body, size_t *body_len)
{
    return cgi_response_build(data, strlen(data), keep_alive, header, cap,
                              status, body, body_len);
}

static void test_build_basic(void)
{
    char header[1024];
    const char *body;
    size_t body_len;
    int status;

    TEST_ASSERT(build("Content-type: text/html\r\n\r\n<p>hi</p>", 0, header,
                      sizeof(header), &status, &body, &body_len) == 0);
    TEST_CHECK(status == 200);
    TEST_CHECK(body_len == 9 && memcmp(body, "<p>hi</p>", 9) == 0);
    TEST_CHECK(strncmp(header, "HTTP/1.1 200 OK\r\n", 17) == 0);
    TEST_CHECK(strstr(header, "Content-type: text/html\r\n") != NULL);
    TEST_CHECK(strstr(header, "Content-Length: 9\r\n") != NULL);
    TEST_CHECK(strstr(header, "Connection: close\r\n\r\n") != NULL);
}

/* E-G6: Status 헤더가 상태 줄이 되고, 헤더로는 전달하지 않음 */
static void test_build_status_header(void)
{
    char header[1024];
    const char *body;
    size_t body_len;
    int status;

    TEST_ASSERT(build("Status: 404 Not Found\r\nContent-type: text/plain\r\n"
                      "\r\nnope",
                      0, header, sizeof(header), &status, &body,
                      &body_len) == 0);
    TEST_CHECK(status == 404);
    TEST_CHECK(strncmp(header, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    TEST_CHECK(strstr(header, "Status:") == NULL);

    TEST_ASSERT(build("Status: 503\r\n\r\n", 0, header, sizeof(header), &status,
                      &body, &body_len) == 0);
    TEST_CHECK(status ==
               503); /* 이유 문구가 없으면 서버 표 (없는 코드는 "Error") */

    TEST_CHECK(build("Status: abc\r\n\r\n", 0, header, sizeof(header), &status,
                     &body, &body_len) == 500);
    TEST_CHECK(build("Status: 99\r\n\r\n", 0, header, sizeof(header), &status,
                     &body, &body_len) == 500);
}

/* E-G7, E-G8: CGI의 Content-Length·Connection은 버리고 서버 값 하나만 */
static void test_build_drops_server_headers(void)
{
    char header[1024];
    const char *body;
    size_t body_len;
    int status;

    TEST_ASSERT(build("Content-Length: 999\r\nConnection: keep-alive\r\n"
                      "Content-type: text/plain\r\n\r\nabc",
                      0, header, sizeof(header), &status, &body,
                      &body_len) == 0);
    TEST_CHECK(strstr(header, "999") == NULL);
    TEST_CHECK(strstr(header, "Content-Length: 3\r\n") != NULL);
    TEST_CHECK(strstr(header, "keep-alive") == NULL);
    TEST_CHECK(strstr(header, "Connection: close") != NULL);

    TEST_ASSERT(build("Content-type: text/plain\r\n\r\nabc", 1, header,
                      sizeof(header), &status, &body, &body_len) == 0);
    TEST_CHECK(strstr(header, "Connection: keep-alive\r\n") != NULL);
}

/* E-G4, E-G5: \n\n만 있으면 정상, 빈 줄이 없거나 출력이 없으면 500 */
static void test_build_blank_line_rules(void)
{
    char header[1024];
    const char *body;
    size_t body_len;
    int status;

    TEST_CHECK(build("Content-type: text/plain\n\nbody", 0, header,
                     sizeof(header), &status, &body, &body_len) == 0);
    TEST_CHECK(body_len == 4 && memcmp(body, "body", 4) == 0);
    TEST_CHECK(build("just a body", 0, header, sizeof(header), &status, &body,
                     &body_len) == 500);
    TEST_CHECK(
        build("", 0, header, sizeof(header), &status, &body, &body_len) == 500);
    TEST_CHECK(build("no colon here\r\n\r\nbody", 0, header, sizeof(header),
                     &status, &body, &body_len) == 500);
    /* 본문 없이 헤더만 */
    TEST_CHECK(build("Content-type: text/plain\r\n\r\n", 0, header,
                     sizeof(header), &status, &body, &body_len) == 0);
    TEST_CHECK(body_len == 0);
}

/* 응답 헤더가 버퍼에 다 안 들어가면 자르지 않고 500 */
static void test_build_header_overflow(void)
{
    char header[32];
    const char *body;
    size_t body_len;
    int status;

    TEST_CHECK(build("Content-type: text/plain\r\n\r\nx", 0, header,
                     sizeof(header), &status, &body, &body_len) == 500);
}

TEST_LIST = {
    {"run_fail", test_run_fail},
    {"run_exec_fails", test_run_exec_fails},
    {"run_nohdr", test_run_nohdr},
    {"run_big", test_run_big},
    {"run_output_limit_edge", test_run_output_limit_edge},
    {"run_slow_timeout", test_run_slow_timeout},
    {"run_closes_stdout_then_hangs", test_run_closes_stdout_then_hangs},
    {"run_slow_output_within_deadline", test_run_slow_output_within_deadline},
    {"run_reads_stdin", test_run_reads_stdin},
    {"build_basic", test_build_basic},
    {"build_status_header", test_build_status_header},
    {"build_drops_server_headers", test_build_drops_server_headers},
    {"build_blank_line_rules", test_build_blank_line_rules},
    {"build_header_overflow", test_build_header_overflow},
    {NULL, NULL},
};
