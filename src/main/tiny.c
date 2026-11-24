/*
 * tiny.c - 정적 파일과 CGI(GET)를 처리하는 작은 웹 서버입니다.
 *   CS:APP 11.6절 Tiny와 같은 일을 합니다. Phase 1 계획(M1~M3)에 따라
 *   Tiny의 각 부분을 같은 동작의 우리 코드로 바꿔 끼웠습니다.
 *     M1 소켓 연결: listen_socket_open, main의 accept 반복문
 *     M2 정적 GET:  doit과 단계 함수들, static_file_serve, http_error_send
 *     M3 CGI GET:   cgi_serve, cgi_child_run
 *     M4 동시성:    main이 연결을 큐에 넣고, 작업자 스레드들이 꺼내 처리
 *   리뷰에서 나온 안정성 수정 5가지도 반영했습니다. 클라이언트 한 명의 문제
 *   (연결 중단, 깨진 요청, 빈 파일, fork 실패)로 서버 전체가 꺼지지 않고
 *   그 요청만 끝냅니다.
 *   사용법: ./build/tiny <port>
 */
#include "csapp.h"
#include "queue.h"
#include <strings.h> /* strcasecmp (메서드 대소문자 무시 비교) */

#define WORKER_COUNT 16 /* 미리 만들어 둘 작업자 스레드 수 */

static struct queue conn_queue; /* 메인과 작업자들이 함께 쓰는 작업 큐 */

/* M4 */
static void workers_start(void);
static void *worker_run(void *arg);

/* M1 */
static int listen_socket_open(const char *port); /* 새로 만듦: csapp.c의 open_listenfd 대신 */

/* M2: 요청 처리 */
void doit(int fd);
static int request_line_read(int fd, rio_t *rp, char *method, char *uri,
                             char *version);              /* doit에서 분리 */
static int method_check(int fd, const char *method);      /* doit에서 분리 */
static int request_headers_skip(rio_t *rp);               /* read_requesthdrs 대신 */
static int uri_check(int fd, const char *uri);            /* 새로 만듦: ../ 차단 */
static int uri_has_dot_dot(const char *path, size_t len); /* uri_check가 쓰는 도우미 */
static int uri_parse(char *uri, char *filename, char *cgiargs); /* parse_uri 대신 */
static int file_find(int fd, const char *filename, struct stat *sbuf); /* doit에서 분리 */
static int file_check_readable(int fd, const char *filename,
                               const struct stat *sbuf);  /* doit에서 분리 */
static int file_check_executable(int fd, const char *filename,
                                 const struct stat *sbuf); /* doit에서 분리 */

/* M2: 정적 응답, 오류 응답 */
static void static_file_serve(int fd, const char *filename, int filesize); /* serve_static 대신 */
static const char *file_type_get(const char *filename); /* get_filetype 대신 */
static void http_error_send(int fd, const char *cause, const char *errnum,
                            const char *shortmsg, const char *longmsg); /* clienterror 대신 */

/* M3: CGI */
static void cgi_serve(int fd, const char *filename, const char *cgiargs);     /* serve_dynamic 대신 (부모 쪽) */
static void cgi_child_run(int fd, const char *filename, char **envp); /* serve_dynamic의 자식 부분 분리 */


/* ============================== M1 소켓 연결 ============================== */

/*
 * 1) SIGPIPE를 무시하도록 설정한다
 * 2) 실행 인자로 받은 포트에 리스닝 소켓을 연다 (listen_socket_open)
 * 3) 작업자 스레드들을 만든다 (workers_start)
 * 4) 무한 반복: accept로 연결 하나를 받아 큐에 넣는다
 *    요청 처리(doit)와 close는 큐에서 꺼낸 작업자가 한다
 */
int main(int argc, char **argv)
{
    int listenfd;
    int connfd;
    socklen_t clientlen;
    /* Tiny는 sockaddr_storage(IPv4/IPv6 모두 담는 큰 구조체)를 썼음.
     * 우리는 IPv4만 쓰고 Getnameinfo 접속 기록도 뺐으므로 sockaddr_in으로 충분 */
    struct sockaddr_in clientaddr;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    /* [안정성 수정] 이미 끊긴 클라이언트에게 쓰면 커널이 SIGPIPE를 보내고,
     * 기본 동작은 서버 종료임. 무시하도록 바꾸면 쓰기가 -1(EPIPE)로 실패할 뿐이라
     * 그 연결만 정리하고 서버는 계속 동작함 */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        exit(1);
    }

    /* Open_listenfd(csapp.c) 대신 우리가 만든 listen_socket_open 사용 */
    listenfd = listen_socket_open(argv[1]);
    if (listenfd < 0) { /* 시작부터 소켓을 못 열면 서버를 띄울 수 없으므로 종료 */
        exit(1);
    }

    workers_start();

    while (1) {
        clientlen = sizeof(clientaddr);
        /* Accept(대문자) 대신 accept(소문자): 대문자는 실패하면 exit해서 서버 전체가 꺼짐 */
        connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
        /* 연결 하나가 accept 직전에 끊기는 등(ECONNABORTED) 실패해도 */
        if (connfd < 0) {
            perror("accept"); /* 원인만 출력하고 */
            continue;         /* 서버는 계속 살아서 다음 연결을 기다림 */
        }
        /* Tiny의 Getnameinfo + "Accepted connection from ..." 출력은 뺌 (응답 동작과 무관) */
        /* CGI를 execve할 때 자동으로 닫히게 함. 이 표시가 없으면 동시에 실행된
         * 다른 손님의 CGI가 이 연결을 물려받아, 우리가 닫아도 연결이 안 끝남 */
        if (fcntl(connfd, F_SETFD, FD_CLOEXEC) < 0) {
            perror("fcntl");
        }
        queue_push(&conn_queue, connfd);
    }
}

/* ============================== M4 동시성 ============================== */

/*
 * 작업 큐를 준비하고 작업자 스레드들을 만듭니다.
 * 서버 시작 단계이므로 실패하면 서버를 종료합니다.
 */
static void workers_start(void)
{
    pthread_t tid;

    if (queue_init(&conn_queue) < 0) {
        fprintf(stderr, "queue_init failed\n");
        exit(1);
    }
    for (int i = 0; i < WORKER_COUNT; i++) {
        if (pthread_create(&tid, NULL, worker_run, NULL) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            exit(1);
        }
    }
}

/*
 * 작업자 스레드 하나가 하는 일입니다. 서버가 꺼질 때까지 반복합니다.
 * 큐에서 연결 꺼내기(없으면 대기) → 요청 처리 → 연결 닫기
 */
static void *worker_run(void *arg)
{
    int connfd;

    (void)arg; /* pthread_create의 함수 모양을 맞추기 위한 인자. 쓰지 않음 */
    while (1) {
        connfd = queue_pop(&conn_queue);
        doit(connfd);
        /* Close(대문자) 대신 close(소문자): 닫기 실패로 서버가 꺼지지 않게 함 */
        close(connfd);
    }
    return NULL;
}

/*
 * socket → setsockopt(SO_REUSEADDR) → bind → listen 순서로 엽니다.
 * 성공하면 리스닝 소켓 fd, 실패하면 -1.
 *
 * open_listenfd는 getaddrinfo로 주소 후보 목록을 받아 하나씩 시도했습니다
 * (IPv4, IPv6 모두 가능). 여기서는 IPv4 주소 구조체(sockaddr_in)에 직접
 * 값을 채웁니다. 네 단계가 코드에 그대로 보이고, 우리 서버는 IPv4
 * (127.0.0.1, localhost)로만 접속하므로 충분합니다.
 */
static int listen_socket_open(const char *port)
{
    struct sockaddr_in addr; /* getaddrinfo 대신 IPv4 주소 구조체를 직접 채움 */
    int listenfd;
    int optval = 1;          /* SO_REUSEADDR를 켜기(1) 위한 값 */

    /* 1. socket: TCP(SOCK_STREAM) 소켓을 만든다. 아직 주소가 없다 */
    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("socket"); /* 실패 원인 출력 */
        return -1;
    }

    /* 2. SO_REUSEADDR: 서버를 껐다 바로 켜면, 앞 연결이 TIME_WAIT 상태로
     * 포트를 잡고 있어 bind가 "Address already in use"로 실패한다.
     * 이 옵션을 bind 전에 켜면 그 상태의 포트도 다시 쓸 수 있다.
     * (open_listenfd도 켰지만 실패 확인은 안 했음 → 여기서는 실패를 확인) */
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval,
                   sizeof(optval)) < 0) {
        perror("setsockopt");
        close(listenfd); /* 이미 만든 소켓을 닫고 */
        return -1;       /* 실패를 알림 */
    }

    /* 3. bind: 이 컴퓨터의 모든 IP(INADDR_ANY)의 port번 포트를 소켓에 붙인다 */
    memset(&addr, 0, sizeof(addr));                   /* 구조체를 0으로 비움 */
    addr.sin_family = AF_INET;                        /* IPv4 */
    addr.sin_addr.s_addr = htonl(INADDR_ANY);         /* 모든 IP, 네트워크 바이트 순서로 */
    addr.sin_port = htons((unsigned short)atoi(port)); /* "8080" → 숫자 → 네트워크 바이트 순서 */
    if (bind(listenfd, (SA *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listenfd);
        return -1;
    }

    /* 4. listen: 연결을 받는 소켓으로 바꾼다. 이제부터 커널이 3-way handshake를
     * 처리하고, 끝난 연결을 최대 LISTENQ(1024)개까지 대기 큐에 쌓는다 */
    if (listen(listenfd, LISTENQ) < 0) {
        perror("listen");
        close(listenfd);
        return -1;
    }
    return listenfd;
}

/* ============================== M2 정적 GET: 요청 처리 ============================== */


/*
 * Tiny의 doit은 모든 일을 직접 했음. 여기서는 역할별 함수를 순서대로 부르고,
 * 함수가 -1을 돌려주면(오류 응답을 이미 보냈거나 응답할 상대가 없으면)
 * 거기서 끝냅니다.
 * 흐름: 요청 라인 읽기 → 메서드 확인 → 헤더 건너뛰기 → URI 확인
 *       → URI 해석 → 파일 찾기 → (정적) 읽기 권한 확인 → static_file_serve
 *                               (동적) 실행 권한 확인 → cgi_serve
 */
void doit(int fd)
{
    char method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    struct stat sbuf;
    rio_t rio;
    int is_static;

    /* 1. 요청 읽기: 연결에 읽기 버퍼를 붙이고 첫 줄을 나눈다 */
    rio_readinitb(&rio, fd);
    /* [안정성 수정] 첫 줄이 불완전하거나 세 칸이 아니면 400을 보내고 끝냄 */
    if (request_line_read(fd, &rio, method, uri, version) < 0) {
        return;
    }

    /* 2. 메서드 확인: GET이 아니면 501 */
    if (method_check(fd, method) < 0) { /* doit 안의 strcasecmp 검사 → 함수로 분리 */
        return;
    }

    /* 3. 헤더 건너뛰기: 빈 줄까지 읽고 버린다 */
    /* [안정성 수정] 빈 줄 전에 연결이 끊기거나 읽기가 실패하면 여기서 끝냄
     * (Tiny는 이때 무한 반복했음) */
    if (request_headers_skip(&rio) < 0) {
        return;
    }

    /* 4. URI 확인: 폴더 밖으로 나가는 ..이 있으면 403 (새로 추가된 단계) */
    if (uri_check(fd, uri) < 0) {
        return;
    }

    /* 5. URI 해석: 정적/동적 판단, 파일 경로와 CGI 인자 만들기 */
    is_static = uri_parse(uri, filename, cgiargs); /* parse_uri 대신 */
    /* [안정성 수정] 경로가 버퍼보다 길어 잘렸으면, 잘린 경로로 엉뚱한 파일을
     * 찾지 않도록 파일 조회 전에 414를 보내고 끝냄 */
    if (is_static < 0) {
        http_error_send(fd, uri, "414", "URI Too Long",
                        "Tiny couldn't handle such a long path");
        return;
    }

    /* 6. 파일 찾기: 없으면 404 */
    if (file_find(fd, filename, &sbuf) < 0) { /* doit 안의 stat 검사 → 함수로 분리 */
        return;
    }

    /* 7. 응답 분기: 권한을 확인하고 정적 / 동적 처리로 보낸다 */
    if (is_static) {
        if (file_check_readable(fd, filename, &sbuf) < 0) { /* 읽기 권한 검사 → 함수로 분리 */
            return;
        }
        static_file_serve(fd, filename, sbuf.st_size); /* serve_static 대신 */
    } else {
        if (file_check_executable(fd, filename, &sbuf) < 0) { /* 실행 권한 검사 → 함수로 분리 */
            return;
        }
        cgi_serve(fd, filename, cgiargs); /* serve_dynamic 대신 */
    }
}

/*
 * 요청 라인(첫 줄)을 읽어 메서드, URI, 버전으로 나눕니다.
 *   예: "GET /home.html HTTP/1.1" → "GET", "/home.html", "HTTP/1.1"
 * 읽은 줄은 확인용으로 터미널에 출력합니다.
 *
 * [안정성 수정] Tiny는 줄이 끝까지 왔는지, 세 칸이 다 채워졌는지 확인하지 않아
 * "GET"만 오면 비어 있는(쓰레기) uri, version 값으로 다음 단계를 진행했습니다.
 *   - 읽기 실패나 바로 끊김: 응답할 상대가 없으므로 -1
 *   - 줄바꿈이 없음(중간에 끊김, 또는 MAXLINE보다 김): 400 후 -1
 *   - 세 칸이 아님: 400 후 -1
 */
static int request_line_read(int fd, rio_t *rp, char *method, char *uri,
                             char *version)
{
    char buf[MAXLINE];
    ssize_t n;

    /* Rio_readlineb(대문자) 대신 rio_readlineb: 대문자는 읽기 오류 시 exit함 */
    n = rio_readlineb(rp, buf, MAXLINE);
    if (n <= 0) { /* 0: 아무것도 못 읽고 끊김, -1: 읽기 오류 */
        return -1;
    }
    printf("%s", buf); /* 확인용으로 요청 라인 출력 */

    if (buf[n - 1] != '\n') { /* 줄바꿈까지 오지 않은 불완전한 줄 */
        http_error_send(fd, "request line", "400", "Bad Request",
                        "Tiny couldn't read a complete request line");
        return -1;
    }
    /* 공백 기준으로 세 칸으로 나누고, 세 칸이 다 채워졌는지 확인 */
    if (sscanf(buf, "%s %s %s", method, uri, version) != 3) {
        http_error_send(fd, "request line", "400", "Bad Request",
                        "Tiny needs a method, a URI and a version");
        return -1;
    }
    return 0;
}

/*
 * Tiny가 처리할 수 있는 메서드(GET)인지 확인합니다.
 * doit 안에 있던 코드를 그대로 뺐습니다. 동작 변화 없음.
 */
static int method_check(int fd, const char *method)
{
    if (strcasecmp(method, "GET")) { /* 대소문자 구분 없이 GET이 아니면 */
        http_error_send(fd, method, "501", "Not Implemented",
                        "Tiny does not implement this method"); /* 501 응답 */
        return -1;
    }
    return 0;
}

/*
 * 헤더를 빈 줄(\r\n)까지 한 줄씩 읽어 출력하고 버립니다.
 * 내용을 쓰지는 않지만, 읽어서 비워야 요청이 어디서 끝나는지 맞춰집니다.
 * Tiny의 read_requesthdrs를 대신합니다.
 *
 * [안정성 수정] Tiny는 읽기 결과를 확인하지 않아, 빈 줄 전에 연결이 끊기면
 * buf가 그대로 남아 같은 줄을 끝없이 비교했습니다(무한 반복).
 * 빈 줄을 만났을 때만 0, 끊김이나 읽기 오류면 -1을 돌려줍니다.
 */
static int request_headers_skip(rio_t *rp)
{
    char buf[MAXLINE];
    ssize_t n;

    while (1) {
        n = rio_readlineb(rp, buf, MAXLINE); /* 한 줄 읽기 (Rio_readlineb 대신) */
        if (n <= 0) {                        /* 빈 줄 전에 끊김(0) 또는 오류(-1) */
            return -1;                       /* 응답할 상대가 없으므로 그냥 끝냄 */
        }
        printf("%s", buf);                   /* 출력만 하고 버림 (Tiny와 같음) */
        if (strcmp(buf, "\r\n") == 0) {      /* 빈 줄 = 헤더 끝 */
            return 0;                        /* 이때만 다음 단계로 */
        }
    }
}

/*
 * (새로 만든 함수)
 * Tiny는 URI 앞에 "."만 붙여 파일을 찾아서, /../etc/passwd로 웹 폴더 밖의
 * 파일을 읽거나 /cgi-bin/../../bin/ls로 다른 프로그램을 실행할 수 있었습니다.
 * URI 해석보다 먼저 확인해서 정적, 동적 요청 모두 막습니다.
 */
static int uri_check(int fd, const char *uri)
{
    const char *question = strchr(uri, '?'); /* 쿼리 문자열 시작 위치 */
    /* '?'가 있으면 그 앞까지만, 없으면 URI 전체가 경로 */
    size_t path_len = question ? (size_t)(question - uri) : strlen(uri);

    if (uri_has_dot_dot(uri, path_len)) { /* 경로 조각 중 ".."이 있으면 */
        http_error_send(fd, uri, "403", "Forbidden",
                        "Tiny doesn't serve files outside its folder"); /* 403 응답 */
        return -1;
    }
    return 0;
}

/*
 * (새로 만든 함수, uri_check 전용)
 * path 앞 len 글자를 '/'로 나눈 조각 중 정확히 ".."이 있으면 1.
 * a..b.html처럼 이름 안에 점이 붙은 것은 막지 않습니다.
 */
static int uri_has_dot_dot(const char *path, size_t len)
{
    size_t start = 0; /* 현재 조각의 시작 위치 */

    for (size_t i = 0; i <= len; i++) {
        if (i == len || path[i] == '/') {   /* 조각이 끝나는 곳('/' 또는 경로 끝) */
            if (i - start == 2 && path[start] == '.' &&
                path[start + 1] == '.') {   /* 조각이 딱 두 글자 ".."이면 */
                return 1;
            }
            start = i + 1; /* 다음 조각은 '/' 바로 뒤부터 */
        }
    }
    return 0;
}

/*
 * URI를 정적/동적으로 나누고 파일 경로와 CGI 인자를 만듭니다.
 * 정적이면 1, 동적이면 0. Tiny의 parse_uri와 같은 규칙입니다.
 * strcpy, strcat 대신 snprintf로 버퍼 크기(MAXLINE)를 지켜 복사합니다.
 *
 * [안정성 수정] snprintf는 버퍼를 넘지 않는 대신 뒤를 잘라 버립니다.
 * 반환값(만들려던 전체 길이)이 MAXLINE 이상이면 잘린 것이므로 -1을 돌려줍니다.
 * 반환값을 확인하지 않으면 -O2 빌드(make release)에서 "잘릴 수 있음" 경고가
 * -Werror 때문에 오류가 되어 빌드가 실패했습니다.
 */
static int uri_parse(char *uri, char *filename, char *cgiargs)
{
    char *ptr;
    int n; /* snprintf가 만들려던 전체 길이 */

    if (!strstr(uri, "cgi-bin")) { /* cgi-bin이 없으면 정적 */
        cgiargs[0] = '\0';         /* strcpy(cgiargs, "") 대신: CGI 인자 없음 */
        /* strcpy + strcat 세 번 대신 snprintf 한 번:
         * "." + URI, '/'로 끝나면 home.html을 붙임 */
        n = snprintf(filename, MAXLINE, ".%s%s", uri,
                     uri[strlen(uri) - 1] == '/' ? "home.html" : "");
        if (n < 0 || n >= MAXLINE) { /* [안정성 수정] 경로가 잘렸으면 */
            return -1;
        }
        return 1;
    }

    ptr = strchr(uri, '?'); /* 동적: 쿼리 문자열 시작 위치 */
    if (ptr) {
        n = snprintf(cgiargs, MAXLINE, "%s", ptr + 1); /* strcpy 대신: '?' 뒤를 CGI 인자로 */
        if (n < 0 || n >= MAXLINE) {                   /* [안정성 수정] 잘렸으면 */
            return -1;
        }
        *ptr = '\0'; /* URI를 '?' 앞에서 자름 */
    } else {
        cgiargs[0] = '\0'; /* strcpy(cgiargs, "") 대신 */
    }
    n = snprintf(filename, MAXLINE, ".%s", uri); /* strcpy + strcat 대신: "." + '?' 앞부분 */
    if (n < 0 || n >= MAXLINE) {                 /* [안정성 수정] 잘렸으면 */
        return -1;
    }
    return 0;
}

/*
 * stat으로 파일이 있는지 확인하고 파일 정보(종류, 권한, 크기)를 sbuf에
 * 채웁니다. doit 안에 있던 코드를 그대로 뺐습니다. 동작 변화 없음.
 */
static int file_find(int fd, const char *filename, struct stat *sbuf)
{
    if (stat(filename, sbuf) < 0) { /* 파일이 없으면 */
        http_error_send(fd, filename, "404", "Not found",
                        "Tiny couldn't find this file"); /* 404 응답 */
        return -1;
    }
    return 0;
}

/*
 * 정적 파일을 보내도 되는지 확인합니다.
 * doit 안에 있던 코드를 그대로 뺐습니다. 동작 변화 없음.
 */
static int file_check_readable(int fd, const char *filename,
                               const struct stat *sbuf)
{
    /* 일반 파일이 아니거나(폴더, 특수 파일) 주인 읽기 권한이 없으면 */
    if (!(S_ISREG(sbuf->st_mode)) || !(S_IRUSR & sbuf->st_mode)) {
        http_error_send(fd, filename, "403", "Forbidden",
                        "Tiny couldn't read the file"); /* 403 응답 */
        return -1;
    }
    return 0;
}

/*
 * CGI 프로그램을 실행해도 되는지 확인합니다.
 * doit 안에 있던 코드를 그대로 뺐습니다. 동작 변화 없음.
 */
static int file_check_executable(int fd, const char *filename,
                                 const struct stat *sbuf)
{
    /* 일반 파일이 아니거나 주인 실행 권한이 없으면 */
    if (!(S_ISREG(sbuf->st_mode)) || !(S_IXUSR & sbuf->st_mode)) {
        http_error_send(fd, filename, "403", "Forbidden",
                        "Tiny couldn't run the CGI program"); /* 403 응답 */
        return -1;
    }
    return 0;
}

/* 확장자와 MIME 타입 한 쌍 (get_filetype의 if 나열을 표로 바꾸기 위해 새로 만듦) */
struct file_type {
    const char *extension; /* 예: ".html" */
    const char *mime;      /* 예: "text/html" */
};

/* Tiny의 get_filetype과 같은 순서, 같은 규칙.
 * M5에서 CSS, JS는 여기에 한 줄씩 추가하면 됨 */
static const struct file_type FILE_TYPES[] = {
    {".html", "text/html"},
    {".gif", "image/gif"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
};


/*
 * 정적 파일을 200 응답으로 보냅니다. Tiny의 serve_static을 대신합니다.
 *
 * [안정성 수정] 순서를 바꿨습니다.
 *   Tiny: 200 헤더 전송 → 파일 열기 → mmap → 본문 전송
 *   지금: 파일 열기 → mmap → (준비가 끝나면) 200 헤더 전송 → 본문 전송
 *   - 파일 준비가 실패하면 아직 아무것도 안 보냈으므로 500을 보낼 수 있음
 *   - 크기 0인 파일은 mmap이 실패(EINVAL)해서 Tiny에서는 Mmap이 서버를
 *     종료시켰음. 크기 0이면 mmap 없이 Content-length: 0으로 헤더만 보냄
 *   - Open/Mmap/Close/Rio_writen(대문자)은 실패하면 exit하므로 소문자로 바꾸고
 *     실패하면 이 요청만 끝냄
 */
static void static_file_serve(int fd, const char *filename, int filesize)
{
    char header[MAXBUF];
    int header_len; /* snprintf가 만든 헤더 길이 (strlen 대신 사용) */
    int srcfd;
    char *srcp = NULL; /* 크기 0인 파일이면 mmap하지 않으므로 NULL로 둠 */

    /* 1. 파일 준비: 열기 (Open 대신 open) */
    srcfd = open(filename, O_RDONLY, 0);
    if (srcfd < 0) { /* 열기 실패: 아직 아무것도 안 보냈으므로 500 */
        http_error_send(fd, filename, "500", "Internal Server Error",
                        "Tiny couldn't open the file");
        return;
    }
    /* 2. 파일 준비: 메모리에 올리기. 크기 0이면 올릴 내용이 없으므로 건너뜀 */
    if (filesize > 0) {
        srcp = mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0); /* Mmap 대신 mmap */
        if (srcp == MAP_FAILED) {                                    /* 실패하면 */
            close(srcfd);                                            /* 열어 둔 파일을 닫고 */
            http_error_send(fd, filename, "500", "Internal Server Error",
                            "Tiny couldn't map the file");           /* 500 */
            return;
        }
    }
    close(srcfd); /* mmap 뒤에는 fd가 필요 없음 (Close 대신 close) */

    /* 3. 준비가 끝났으므로 200 헤더 전송.
     * Tiny는 sprintf로 한 줄씩 이어 붙였음 → snprintf 한 번으로 헤더 전체를 만듦.
     * 크기 0이면 Content-length: 0 */
    header_len = snprintf(header, sizeof(header),
                          "HTTP/1.0 200 OK\r\n"
                          "Server: Tiny Web Server\r\n"
                          "Content-length: %d\r\n"
                          "Content-type: %s\r\n\r\n",
                          filesize, file_type_get(filename)); /* get_filetype 대신 */

    /* 4. 헤더를 다 보냈을 때만 본문 전송 (Rio_writen 대신 rio_writen).
     * 쓰기가 실패하면(클라이언트가 끊음) 더 보내지 않고 정리만 함 */
    if (rio_writen(fd, header, header_len) == header_len && filesize > 0) {
        rio_writen(fd, srcp, filesize); /* 실패해도 이 요청만 끝남 */
    }

    /* 5. 정리: mmap했던 경우에만 해제 (Munmap 대신 munmap) */
    if (srcp != NULL) {
        munmap(srcp, filesize);
    }
}

/*
 * 파일 이름으로 Content-type 값을 정합니다. Tiny의 get_filetype을 대신합니다.
 * Tiny는 결과를 filetype 버퍼에 복사했지만, 여기서는 문자열을 바로 돌려줍니다.
 */
static const char *file_type_get(const char *filename)
{
    size_t count = sizeof(FILE_TYPES) / sizeof(FILE_TYPES[0]); /* 표의 칸 수 */

    /* if-else 나열 대신 표를 앞에서부터 확인 */
    for (size_t i = 0; i < count; i++) {
        if (strstr(filename, FILE_TYPES[i].extension)) { /* 이름에 확장자가 들어 있으면 */
            return FILE_TYPES[i].mime;                   /* 그 타입 */
        }
    }
    return "text/plain"; /* 표에 없으면 text/plain (Tiny와 같음) */
}

/*
 * 오류 응답(상태 줄, Content-type, 짧은 HTML 본문)을 보냅니다.
 * Tiny의 clienterror를 대신합니다.
 */
static void http_error_send(int fd, const char *cause, const char *errnum,
                     const char *shortmsg, const char *longmsg)
{
    char header[MAXLINE]; /* Tiny는 buf 하나를 헤더 줄마다 다시 썼음 → 헤더용 버퍼 따로 */
    char body[MAXBUF];
    int header_len;
    int body_len;

    /* Tiny는 sprintf로 한 줄씩 만들어 그때마다 보냄 → snprintf 한 번으로 헤더 전체 */
    header_len = snprintf(header, sizeof(header),
                          "HTTP/1.0 %s %s\r\n"
                          "Content-type: text/html\r\n\r\n",
                          errnum, shortmsg);
    /* Tiny는 sprintf(body, "%s...", body)로 자기 자신에 이어 붙였음 (정의되지 않은 동작)
     * → snprintf 한 번으로 본문 전체를 만듦 */
    body_len = snprintf(body, sizeof(body),
                        "<html><title>Tiny Error</title>"
                        "<body bgcolor=ffffff>\r\n"
                        "%s: %s\r\n"
                        "<p>%s: %s\r\n"
                        "<hr><em>The Tiny Web server</em>\r\n",
                        errnum, shortmsg, longmsg, cause);
    /* cause(URI 등)가 아주 길어 잘렸으면, 실제로 버퍼에 들어간 길이만큼만 보냄 */
    if (body_len >= (int)sizeof(body)) {
        body_len = sizeof(body) - 1;
    }

    /* [안정성 수정] Rio_writen(대문자)은 쓰기 실패 시 exit함 → rio_writen.
     * 헤더 쓰기가 실패하면(클라이언트가 끊음) 본문은 보내지 않음 */
    if (rio_writen(fd, header, header_len) != header_len) {
        return;
    }
    rio_writen(fd, body, body_len); /* 실패해도 이 요청만 끝남 */
}

/* ============================== M3 CGI GET ============================== */


/*
 * CGI 요청을 처리합니다. Tiny의 serve_dynamic 중 부모가 하던 일입니다.
 * 1) 상태 줄과 Server 헤더를 먼저 보낸다. 나머지 헤더와 본문은 CGI
 *    프로그램이 직접 출력한다
 * 2) fork로 자식을 만들어 자식이 프로그램을 실행한다
 * 3) 부모는 자식이 끝날 때까지 기다렸다가 회수한다. 회수하지 않으면
 *    끝난 자식이 좀비 프로세스로 남는다
 *
 * [안정성 수정] Fork(대문자)는 실패하면(프로세스 수 한도 등) 서버를 exit시켰음.
 * fork의 반환값을 확인해서 실패하면 이 요청만 끝냄.
 * 200 헤더는 이미 나갔으므로 500으로 바꿀 수 없어 연결만 닫음(main이 close).
 * 헤더를 fork 뒤로 옮기면 부모의 헤더와 자식의 출력 순서가 섞일 수 있어
 * 순서는 Tiny와 같게 둠.
 */
static void cgi_serve(int fd, const char *filename, const char *cgiargs)
{
    char header[MAXLINE];
    char query_env[MAXLINE + 16]; /* "QUERY_STRING=" + 쿼리 문자열 */
    char *envp[] = {query_env, NULL};
    int header_len;
    pid_t pid;

    /* CGI에 넘길 환경 변수를 fork 전에 만든다. 스레드가 여럿일 때 자식에서
     * setenv를 부르면, 다른 스레드가 잡고 있던 락을 기다리다 멈출 수 있음 */
    snprintf(query_env, sizeof(query_env), "QUERY_STRING=%s", cgiargs);

    /* Tiny는 sprintf + Rio_writen을 두 번 → snprintf 한 번으로 만들어 한 번에 보냄 */
    header_len = snprintf(header, sizeof(header),
                          "HTTP/1.0 200 OK\r\n"
                          "Server: Tiny Web Server\r\n");
    /* [안정성 수정] Rio_writen 대신 rio_writen. 이미 끊겼으면 자식을 만들지 않음 */
    if (rio_writen(fd, header, header_len) != header_len) {
        return;
    }

    pid = fork(); /* [안정성 수정] Fork 대신 fork */
    if (pid < 0) {         /* 자식 생성 실패 */
        perror("fork");    /* 원인만 출력하고 */
        return;            /* 이 요청만 끝냄 (서버는 계속 동작) */
    }
    if (pid == 0) {                        /* 자식 프로세스이면 */
        cgi_child_run(fd, filename, envp); /* 자식 일은 함수로 분리 (돌아오지 않음) */
    }
    /* 부모: 방금 만든 자식이 끝날 때까지 기다렸다가 회수.
     * Wait(대문자)도 실패하면 exit하므로 waitpid로 바꾸고 실패는 출력만 함 */
    if (waitpid(pid, NULL, 0) < 0) {
        perror("waitpid");
    }
}

/*
 * 자식 프로세스에서 CGI 프로그램을 실행합니다.
 * Tiny의 serve_dynamic에서 if (Fork() == 0) { ... } 안에 있던 코드를 뺐습니다.
 * fork 뒤의 자식은 dup2와 execve만 합니다. 실패하면 _exit로 자식만 끝냅니다.
 * (exit는 부모에게서 복사된 출력 버퍼까지 내보내므로 쓰지 않음)
 */
static void cgi_child_run(int fd, const char *filename, char **envp)
{
    char *argv[] = {NULL}; /* Tiny의 emptylist: CGI 프로그램에 넘길 인자 없음 */

    if (dup2(fd, STDOUT_FILENO) < 0) { /* 표준 출력(1번)을 클라이언트 소켓으로 돌림 → printf가 곧바로 응답이 됨 */
        _exit(1);
    }
    execve(filename, argv, envp); /* 이 프로세스를 CGI 프로그램으로 바꿈. 성공하면 돌아오지 않음 */
    _exit(1);                     /* 여기 왔다면 execve 실패 */
}