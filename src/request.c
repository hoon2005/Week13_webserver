#include "request.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define CGI_DIR "cgi-bin"

/* 요청 라인의 토큰 사이에 올 수 있는 글자입니다. */
#define REQUEST_LINE_SPACES " \t"

static void line_end_strip(char *line);
static int line_token_copy(char **cursor, char *dst, size_t dst_size);
static bool header_is_end(const char *line);
static bool path_is_allowed(const char *path, size_t path_len);
static bool segment_is_dot(const char *segment, size_t len);
static enum request_target_kind cgi_dir_check(const char *path,
                                              size_t path_len);
static int path_build(char *dst, size_t dst_size, const char *path,
                      size_t path_len, const char *suffix);
static int text_copy(char *dst, size_t dst_size, const char *src);

/*
 * 요청 라인 한 줄을 읽어 method, uri, version으로 나눕니다.
 * 성공하면 0, 상대가 닫았거나 줄이 너무 길거나 토큰이 정확히 셋이 아니면
 * -1입니다. -1이면 응답 없이 연결을 닫습니다.
 */
int request_read_line(struct io_reader *reader, struct request *req)
{
    char line[REQUEST_LINE_SIZE];
    char *cursor = line;

    if (io_read_line(reader, line, sizeof(line)) <= 0) {
        return -1;
    }
    line_end_strip(line);

    if (line_token_copy(&cursor, req->method, sizeof(req->method)) < 0 ||
        line_token_copy(&cursor, req->uri, sizeof(req->uri)) < 0 ||
        line_token_copy(&cursor, req->version, sizeof(req->version)) < 0) {
        return -1;
    }
    /* 네 번째 토큰이 있으면 해석할 수 없는 요청입니다. */
    if (cursor[strspn(cursor, REQUEST_LINE_SPACES)] != '\0') {
        return -1;
    }
    return 0;
}

/*
 * 빈 줄이 나올 때까지 헤더 줄을 읽고 버립니다. 빈 줄은 "\r\n"과 "\n"을 모두
 * 받습니다. 성공하면 0, 빈 줄 전에 상대가 닫았거나 헤더 줄 하나가 너무 길면
 * -1입니다.
 */
int request_skip_headers(struct io_reader *reader)
{
    char line[REQUEST_LINE_SIZE];

    for (;;) {
        if (io_read_line(reader, line, sizeof(line)) <= 0) {
            return -1;
        }
        if (header_is_end(line)) {
            return 0;
        }
    }
}

/*
 * URI를 실제로 열 경로로 바꿉니다. 쿼리('?' 뒤)는 떼고 경로만 봅니다.
 * 경로 규칙: '/'로 시작해야 합니다. 중간에 빈 구간("//")이 있거나 구간 전체가
 * "." 또는 ".."이면 거부합니다. 끝의 '/'는 허용하고 기본 파일 이름을 붙입니다.
 * 정규화하지 않으므로 여기서 본 경로와 실제로 여는 경로가 같습니다.
 * 첫 구간이 정확히 "cgi-bin"이면 CGI이고, 대소문자만 다르면 거부합니다.
 */
enum request_target_kind request_map_uri(const char *uri,
                                         struct request_target *target)
{
    size_t path_len = strcspn(uri, "?");
    const char *query = uri[path_len] == '?' ? uri + path_len + 1 : "";
    const char *suffix = "";
    enum request_target_kind kind;
    int rc;

    if (!path_is_allowed(uri, path_len)) {
        return REQUEST_TARGET_FORBIDDEN;
    }
    kind = cgi_dir_check(uri, path_len);
    if (kind == REQUEST_TARGET_FORBIDDEN) {
        return kind;
    }

    if (kind == REQUEST_TARGET_STATIC) {
        query = ""; /* 정적 파일은 쿼리를 쓰지 않습니다. */
        if (uri[path_len - 1] == '/') {
            suffix = REQUEST_DEFAULT_FILE;
        }
    }
    rc = path_build(target->path, sizeof(target->path), uri, path_len, suffix);
    if (rc < 0) {
        return REQUEST_TARGET_TOO_LONG;
    }
    if (text_copy(target->args, sizeof(target->args), query) < 0) {
        return REQUEST_TARGET_TOO_LONG;
    }
    return kind;
}

/* 줄 끝의 "\r\n" 또는 "\n"을 지웁니다. */
static void line_end_strip(char *line)
{
    size_t len = strlen(line);

    if (len > 0 && line[len - 1] == '\n') {
        line[--len] = '\0';
    }
    if (len > 0 && line[len - 1] == '\r') {
        line[--len] = '\0';
    }
}

/*
 * cursor에서 공백을 건너뛰고 다음 토큰을 dst에 복사한 뒤 cursor를 토큰 뒤로
 * 옮깁니다. 토큰이 없거나 dst에 안 들어가면 -1입니다.
 */
static int line_token_copy(char **cursor, char *dst, size_t dst_size)
{
    char *start = *cursor + strspn(*cursor, REQUEST_LINE_SPACES);
    size_t len = strcspn(start, REQUEST_LINE_SPACES);

    if (len == 0 || len >= dst_size) {
        *cursor = start;
        return -1;
    }
    memcpy(dst, start, len);
    dst[len] = '\0';
    *cursor = start + len;
    return 0;
}

static bool header_is_end(const char *line)
{
    return strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0;
}

/* 경로 규칙을 지키는지 봅니다. 규칙은 request_map_uri 주석에 있습니다. */
static bool path_is_allowed(const char *path, size_t path_len)
{
    size_t start;
    size_t end;

    if (path_len == 0 || path[0] != '/') {
        return false;
    }
    for (start = 1; start <= path_len; start = end + 1) {
        end = start;
        while (end < path_len && path[end] != '/') {
            end++;
        }
        if (end == start && end < path_len) {
            return false; /* 중간의 빈 구간 "//" */
        }
        if (segment_is_dot(path + start, end - start)) {
            return false;
        }
    }
    return true;
}

static bool segment_is_dot(const char *segment, size_t len)
{
    return (len == 1 && segment[0] == '.') ||
           (len == 2 && segment[0] == '.' && segment[1] == '.');
}

/*
 * 첫 구간이 정확히 "cgi-bin"이면 CGI, ASCII 대소문자만 다르면 거부, 아니면
 * 정적 파일입니다. 대소문자를 무시하는 파일 시스템(macOS 기본)에서 변형된
 * 표기로 CGI 프로그램 파일이 정적 파일로 내려가는 것을 막습니다.
 */
static enum request_target_kind cgi_dir_check(const char *path, size_t path_len)
{
    const size_t dir_len = strlen(CGI_DIR);
    const char *first = path + 1;
    size_t first_len = strcspn(first, "/");

    if (first_len > path_len - 1) {
        first_len = path_len - 1;
    }
    if (first_len != dir_len) {
        return REQUEST_TARGET_STATIC;
    }
    if (strncmp(first, CGI_DIR, dir_len) == 0) {
        return REQUEST_TARGET_CGI;
    }
    if (strncasecmp(first, CGI_DIR, dir_len) == 0) {
        return REQUEST_TARGET_FORBIDDEN;
    }
    return REQUEST_TARGET_STATIC;
}

/* dst에 "." + path(path_len바이트) + suffix를 씁니다. 안 들어가면 -1입니다. */
static int path_build(char *dst, size_t dst_size, const char *path,
                      size_t path_len, const char *suffix)
{
    int n = snprintf(dst, dst_size, ".%.*s%s", (int)path_len, path, suffix);

    if (n < 0 || (size_t)n >= dst_size) {
        return -1;
    }
    return 0;
}

/* src를 dst에 복사합니다. 안 들어가면 -1입니다. */
static int text_copy(char *dst, size_t dst_size, const char *src)
{
    int n = snprintf(dst, dst_size, "%s", src);

    if (n < 0 || (size_t)n >= dst_size) {
        return -1;
    }
    return 0;
}
