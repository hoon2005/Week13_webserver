#ifndef REQUEST_H
#define REQUEST_H

#include "io.h"

/* 요청 라인과 헤더 줄 하나를 담는 버퍼 크기입니다. 줄은 '\n' 포함 최대 8191바이트입니다. */
#define REQUEST_LINE_SIZE 8192

/* "." + 경로 + 기본 파일 이름이 들어가는 크기입니다. */
#define REQUEST_PATH_SIZE (REQUEST_LINE_SIZE + 16)

/* 경로가 '/'로 끝날 때 붙이는 파일 이름입니다. */
#define REQUEST_DEFAULT_FILE "home.html"

struct request {
    char method[REQUEST_LINE_SIZE];
    char uri[REQUEST_LINE_SIZE];
    char version[REQUEST_LINE_SIZE];
};

enum request_target_kind {
    REQUEST_TARGET_STATIC,    /* path의 파일을 그대로 보냄 */
    REQUEST_TARGET_CGI,       /* path의 프로그램을 args로 실행 */
    REQUEST_TARGET_FORBIDDEN, /* 경로 규칙 위반: 403 */
    REQUEST_TARGET_TOO_LONG,  /* 경로가 버퍼에 안 들어감: 연결 닫기 */
};

struct request_target {
    char path[REQUEST_PATH_SIZE];
    char args[REQUEST_LINE_SIZE];
};

int request_read_line(struct io_reader *reader, struct request *req);
int request_skip_headers(struct io_reader *reader);
enum request_target_kind request_map_uri(const char *uri,
                                         struct request_target *target);

#endif
