/*
 * HTTP/1.0 웹 서버입니다. 접속을 하나씩 순서대로 받아 GET 요청에 정적 파일이나
 * CGI 프로그램의 출력으로 응답합니다. 현재 폴더가 문서 루트입니다.
 *
 * 사용법: server <port>
 */
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cgi.h"
#include "io.h"
#include "net.h"
#include "request.h"
#include "response.h"

/* 접속 로그에 찍는 손님 주소와 포트의 버퍼 크기입니다. */
#define CLIENT_HOST_SIZE 64
#define CLIENT_PORT_SIZE 16

static void connection_log(const struct sockaddr_storage *addr,
                           socklen_t addr_len);
static void connection_handle(int fd);

int main(int argc, char **argv)
{
    struct sockaddr_storage client_addr;
    socklen_t client_len;
    int listen_fd;
    int conn_fd;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    /*
     * 끊긴 연결에 쓰면 SIGPIPE의 기본 동작으로 서버가 죽습니다. 무시하면 write가
     * EPIPE로 실패를 돌려주고, 그 연결만 정리합니다. 무시 설정은 execve 뒤에도
     * 남으므로 CGI 자식은 실행 전에 기본 동작으로 되돌립니다(cgi.c).
     */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        fprintf(stderr, "%s: cannot ignore SIGPIPE\n", argv[0]);
        exit(1);
    }

    listen_fd = net_open_listen(argv[1]);
    if (listen_fd < 0) {
        fprintf(stderr, "%s: cannot listen on port %s\n", argv[0], argv[1]);
        exit(1);
    }

    for (;;) {
        conn_fd = net_accept(listen_fd, &client_addr, &client_len);
        if (conn_fd < 0) {
            fprintf(stderr, "%s: cannot accept connections, exiting\n",
                    argv[0]);
            close(listen_fd);
            exit(1);
        }
        connection_log(&client_addr, client_len);
        connection_handle(conn_fd);
        if (close(conn_fd) < 0) {
            fprintf(stderr, "close(conn_fd): %s\n", strerror(errno));
        }
    }
}

/* 숫자로만 바꿔 DNS 조회를 하지 않습니다. 못 바꾸면 로그만 생략합니다. */
static void connection_log(const struct sockaddr_storage *addr,
                           socklen_t addr_len)
{
    char host[CLIENT_HOST_SIZE];
    char port[CLIENT_PORT_SIZE];

    if (getnameinfo((const struct sockaddr *)addr, addr_len, host, sizeof(host),
                    port, sizeof(port), NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        printf("Accepted connection from (%s, %s)\n", host, port);
    }
}

/*
 * 요청 하나를 읽고 응답합니다. 요청 라인이나 헤더를 해석할 수 없으면 응답 없이
 * 돌아가고, main이 연결을 닫습니다.
 * 요청 라인 → 헤더 → 메서드 확인 → 경로 규칙 → 정적 파일 또는 CGI 순서입니다.
 * 헤더를 먼저 다 읽는 이유: 읽지 않은 데이터가 남은 채로 닫으면 상대가 응답을
 * 받기 전에 연결이 리셋될 수 있습니다.
 */
static void connection_handle(int fd)
{
    struct io_reader reader;
    struct request req;
    struct request_target target;

    io_init(&reader, fd);
    if (request_read_line(&reader, &req) < 0) {
        return;
    }
    printf("%s %s %s\n", req.method, req.uri, req.version);
    if (request_skip_headers(&reader) < 0) {
        return;
    }

    if (strcasecmp(req.method, "GET") != 0) {
        response_send_error(fd, 501, "Not Implemented",
                            "This method is not implemented.");
        return;
    }

    switch (request_map_uri(req.uri, &target)) {
    case REQUEST_TARGET_STATIC:
        response_send_static(fd, target.path);
        break;
    case REQUEST_TARGET_CGI:
        cgi_run(fd, target.path, target.args);
        break;
    case REQUEST_TARGET_FORBIDDEN:
        response_send_error(fd, 403, "Forbidden", "The path is not allowed.");
        break;
    case REQUEST_TARGET_TOO_LONG:
        break;
    }
}
