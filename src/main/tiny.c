/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include "csapp.h"
#include "io.h"
#include "net.h"
#include "request.h"
#include "response.h"
#include <strings.h>

static void connection_handle(int fd);
static void cgi_target_serve(int fd, const struct request_target *target);
void serve_dynamic(int fd, char *filename, char *cgiargs);

int main(int argc, char **argv)
{
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

    /* Check command line args */
    if (argc != 2) {
	fprintf(stderr, "usage: %s <port>\n", argv[0]);
	exit(1);
    }

    /*
     * 끊긴 연결에 쓰면 SIGPIPE의 기본 동작으로 서버가 죽습니다. 무시하면 write가
     * EPIPE로 실패를 돌려주고, 그 연결만 정리합니다. 무시 설정은 execve 뒤에도
     * 남으므로 CGI 자식은 실행 전에 기본 동작으로 되돌립니다.
     */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        fprintf(stderr, "%s: cannot ignore SIGPIPE\n", argv[0]);
        exit(1);
    }

    listenfd = net_open_listen(argv[1]);
    if (listenfd < 0) {
        fprintf(stderr, "%s: cannot listen on port %s\n", argv[0], argv[1]);
        exit(1);
    }
    while (1) {
        connfd = net_accept(listenfd, &clientaddr, &clientlen);
        if (connfd < 0) {
            fprintf(stderr, "%s: cannot accept connections, exiting\n",
                    argv[0]);
            close(listenfd);
            exit(1);
        }
        /* 숫자로만 바꿔 DNS 조회를 하지 않습니다. 못 바꿔도 요청은 처리합니다. */
        if (getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE,
                        port, MAXLINE, NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
            printf("Accepted connection from (%s, %s)\n", hostname, port);
        }
        connection_handle(connfd);
        if (close(connfd) < 0) {
            fprintf(stderr, "close(connfd): %s\n", strerror(errno));
        }
    }
}
/* $end tinymain */

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
        cgi_target_serve(fd, &target);
        break;
    case REQUEST_TARGET_FORBIDDEN:
        response_send_error(fd, 403, "Forbidden",
                            "The path is not allowed.");
        break;
    case REQUEST_TARGET_TOO_LONG:
        break;
    }
}

/*
 * CGI 프로그램이 있고 실행할 수 있는지 Tiny와 같은 기준으로 본 뒤 Tiny의
 * serve_dynamic으로 실행합니다. M3에서 교체합니다.
 */
static void cgi_target_serve(int fd, const struct request_target *target)
{
    struct stat sbuf;
    char filename[REQUEST_PATH_SIZE], cgiargs[REQUEST_LINE_SIZE];

    if (stat(target->path, &sbuf) < 0) {
        response_send_error(fd, 404, "Not Found", "The file was not found.");
        return;
    }
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) {
        response_send_error(fd, 403, "Forbidden",
                            "The CGI program cannot be run.");
        return;
    }
    strcpy(filename, target->path);
    strcpy(cgiargs, target->args);
    serve_dynamic(fd, filename, cgiargs);
}

/*
 * serve_dynamic - run a CGI program on behalf of the client
 */
/* $begin serve_dynamic */
void serve_dynamic(int fd, char *filename, char *cgiargs)
{
    char buf[MAXLINE], *emptylist[] = { NULL };

    /* Return first part of HTTP response */
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));

    if (Fork() == 0) { /* Child */ //line:netp:servedynamic:fork
        /* 서버가 무시하는 SIGPIPE를 CGI 프로그램에서는 기본 동작으로 되돌립니다. */
        if (signal(SIGPIPE, SIG_DFL) == SIG_ERR) {
            _exit(1);
        }
	/* Real server would set all CGI vars here */
	setenv("QUERY_STRING", cgiargs, 1); //line:netp:servedynamic:setenv
	Dup2(fd, STDOUT_FILENO);         /* Redirect stdout to client */ //line:netp:servedynamic:dup2
	Execve(filename, emptylist, environ); /* Run CGI program */ //line:netp:servedynamic:execve
    }
    Wait(NULL); /* Parent waits for and reaps child */ //line:netp:servedynamic:wait
}
/* $end serve_dynamic */
