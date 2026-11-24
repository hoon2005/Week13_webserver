/*
 * tiny.c - 정적 파일과 CGI(GET)를 처리하는 작은 웹 서버입니다.
 *   CS:APP 11.6절 Tiny와 같은 일을 합니다. Phase 1 계획(M1~M3)에 따라
 *   Tiny의 각 부분을 같은 동작의 우리 코드로 바꿔 끼웠습니다.
 *     M1 소켓 연결: listen_socket_open, main의 accept 반복문
 *     M2 정적 GET:  doit과 단계 함수들, static_file_serve, http_error_send
 *     M3 CGI GET:   cgi_serve, cgi_child_run
 *   리뷰에서 나온 안정성 수정 5가지도 반영했습니다. 클라이언트 한 명의 문제
 *   (연결 중단, 깨진 요청, 빈 파일, fork 실패)로 서버 전체가 꺼지지 않고
 *   그 요청만 끝냅니다.
 *   Phase 2 M4-2: 스레드 풀로 동시에 처리합니다. main은 accept한 연결을 작업
 *   큐(conn_queue.c)에 넣기만 하고, 미리 만든 작업자 N개가 꺼내 처리합니다.
 *   (M4-1의 연결당 스레드 버전은 부하 측정 비교 기준으로 그 커밋에 남아 있습니다)
 *   Phase 2 M4-3: 접속 소켓에 읽기·쓰기 타임아웃을 걸고, CGI 자식이 다른 손님
 *   소켓과 리스닝 소켓을 물려받지 않게 막고(FD_CLOEXEC + 자식 fd 정리),
 *   자식은 fork 뒤에 malloc을 쓰는 함수(setenv 등)를 부르지 않습니다.
 *   사용법: ./build/tiny <port> [--root DIR] [--threads N] [--queue N]
 *                       [--timeout 초] [--keepalive-max N]
 */
/* [M4-3] Linux의 accept4(SOCK_CLOEXEC)는 GNU 확장이라 _GNU_SOURCE가 있어야 선언됨.
 * 헤더를 include하기 전에 정의해야 하므로 맨 위에 둠. macOS에는 accept4가 없음 */
#ifdef __linux__
#define _GNU_SOURCE
#endif
#include "csapp.h"
#include "conn_queue.h"
#include <strings.h> /* strcasecmp (메서드 대소문자 무시 비교) */

/* M1 */
static int listen_socket_open(const char *port); /* 새로 만듦: csapp.c의 open_listenfd 대신 */

/* M4-2: 스레드 풀과 실행 인자 */
static void args_parse(int argc, char **argv);  /* 실행 인자 → cfg */
static void *worker_main(void *arg);            /* 작업자: 큐에서 꺼내 doit → close 반복 */

/* M4-3: 소켓 설정 */
static int conn_accept(int listenfd);           /* accept + FD_CLOEXEC (OS별 분기) */
static int fd_cloexec_set(int fd);              /* fd에 FD_CLOEXEC 켜기 */
static int conn_timeouts_set(int connfd);       /* SO_RCVTIMEO, SO_SNDTIMEO 걸기 */

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
static void cgi_child_run(int fd, const char *filename, char *const envp[]);  /* serve_dynamic의 자식 부분 분리 */


/* ============================== M1 소켓 연결 ============================== */

/*
 * [M4-2] 실행 인자로 받는 설정값 (final.md 0장 "실행 방법")
 * timeout은 M4-3에서 소켓 타임아웃에 씁니다. root와 keepalive_max는
 * 받아 두기만 하고 M5-1(root), M5-4a(keepalive_max)에서 사용합니다.
 */
struct server_config {
    const char *port;
    const char *root;  /* NULL이면 지금처럼 현재 폴더 기준 */
    int threads;       /* 작업자 수 */
    int queue;         /* 작업 큐 칸 수 */
    int timeout;       /* 읽기 타임아웃(초). 쓰기는 2배 */
    int keepalive_max; /* 연결 하나에서 처리할 최대 요청 수 */
};

/* 기본값: final.md 2.3절 (작업자 16, 큐 64), 2.4절 (5초), 5장 (100) */
static struct server_config cfg = {NULL, NULL, 16, 64, 5, 100};

/* main과 작업자가 함께 쓰는 작업 큐. 큐 안의 값은 conn_queue.c가 락으로 지킴 */
static struct conn_queue conn_q;

/* 작업자 스레드 id. M7(안전한 종료)에서 pthread_join할 때 씀 */
static pthread_t *workers;

/* [M4-3] CGI 자식이 execve 전에 닫을 fd 번호의 끝 (3번부터 이 값 - 1까지 닫음).
 * 자식에서 sysconf를 부르지 않도록 main이 시작할 때 한 번 계산해 둠 */
static int fd_close_max;
#define FD_CLOSE_LIMIT 4096 /* 한도가 아주 큰 시스템에서 close 루프가 길어지지 않게 */

/*
 * 1) 실행 인자를 읽는다 (args_parse)
 * 2) SIGPIPE를 무시하도록 설정한다
 * 3) 실행 인자로 받은 포트에 리스닝 소켓을 연다 (listen_socket_open)
 * 4) [M4-2] 작업 큐를 만들고 작업자 스레드 N개를 미리 띄운다
 * 5) 무한 반복: accept로 연결 하나를 꺼내 connfd를 받고 → 큐에 넣고 → 다음 accept
 *    (큐가 가득 차면 503을 보내지 않고 자리가 날 때까지 기다림. final.md 2.2)
 */
int main(int argc, char **argv)
{
    int listenfd;
    int connfd;
    int err;

    args_parse(argc, argv); /* 틀린 인자면 사용법을 출력하고 여기서 exit(1) */

    /* [안정성 수정] 이미 끊긴 클라이언트에게 쓰면 커널이 SIGPIPE를 보내고,
     * 기본 동작은 서버 종료임. 무시하도록 바꾸면 쓰기가 -1(EPIPE)로 실패할 뿐이라
     * 그 연결만 정리하고 서버는 계속 동작함 */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        exit(1);
    }

    /* Open_listenfd(csapp.c) 대신 우리가 만든 listen_socket_open 사용 */
    listenfd = listen_socket_open(cfg.port);
    if (listenfd < 0) { /* 시작부터 소켓을 못 열면 서버를 띄울 수 없으므로 종료 */
        exit(1);
    }

    /* [M4-3] CGI 자식이 닫을 fd 범위. 한도를 못 읽으면(-1) 상한값을 씀 */
    {
        long open_max = sysconf(_SC_OPEN_MAX);
        fd_close_max = (open_max < 0 || open_max > FD_CLOSE_LIMIT)
                           ? FD_CLOSE_LIMIT
                           : (int)open_max;
    }

    /* [M4-2] 큐와 작업자는 시작할 때 한 번만 만듦. 여기서 실패하면 서버를
     * 띄울 수 없으므로 종료 (연결마다 만들던 M4-1과 달리 실행 중에는 만들지 않음) */
    if (conn_queue_init(&conn_q, cfg.queue) < 0) {
        fprintf(stderr, "작업 큐를 만들지 못했습니다\n");
        exit(1);
    }
    workers = malloc(sizeof(*workers) * (size_t)cfg.threads);
    if (workers == NULL) {
        perror("malloc");
        exit(1);
    }
    for (int i = 0; i < cfg.threads; i++) {
        /* pthread_create는 errno가 아니라 오류 번호를 반환값으로 돌려줌 → strerror(err) */
        err = pthread_create(&workers[i], NULL, worker_main, NULL);
        if (err != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
            exit(1);
        }
    }

    while (1) {
        /* Accept(대문자) 대신 accept(소문자): 대문자는 실패하면 exit해서 서버 전체가 꺼짐.
         * [M4-3] conn_accept가 accept와 FD_CLOEXEC 설정을 함께 함 */
        connfd = conn_accept(listenfd);
        /* 연결 하나가 accept 직전에 끊기는 등(ECONNABORTED) 실패해도 */
        if (connfd < 0) {
            perror("accept"); /* 원인만 출력하고 */
            continue;         /* 서버는 계속 살아서 다음 연결을 기다림 */
        }
        /* Tiny의 Getnameinfo + "Accepted connection from ..." 출력은 뺌 (응답 동작과 무관) */

        /* [M4-3] 타임아웃을 못 걸면, 그 연결이 작업자를 무한정 붙잡을 수 있으므로 받지 않음 */
        if (conn_timeouts_set(connfd) < 0) {
            perror("setsockopt");
            close(connfd);
            continue;
        }

        /* [M4-2] 큐에 넣기만 하고 바로 다음 accept로. 처리는 작업자가 함.
         * M4-1처럼 malloc한 칸이 필요 없음: 큐가 값(int)을 복사해 담기 때문 */
        if (conn_queue_push(&conn_q, connfd) < 0) {
            close(connfd); /* 큐가 종료 중이면(M7) 받지 않고 닫음 */
        }
    }
}

/*
 * [M4-2] 작업자 스레드입니다. 시작할 때 cfg.threads개를 만들어 두고,
 * 각자 큐에서 연결을 하나씩 꺼내 처음부터 끝까지 처리합니다.
 * 큐가 비면 conn_queue_pop 안에서 기다립니다 (CPU를 쓰지 않음).
 * 큐가 종료되면(M7) pop이 -1을 돌려주고 작업자가 끝납니다.
 */
static void *worker_main(void *arg)
{
    int connfd;

    (void)arg; /* 넘겨받는 값 없음 */
    while ((connfd = conn_queue_pop(&conn_q)) != -1) {
        /* 락은 pop 안에서만 잡고 풀림: 요청 처리는 락 밖이라 작업자끼리 동시에 진행 */
        doit(connfd);  /* 요청 하나 처리 (Phase 1 코드 그대로) */
        close(connfd); /* Close(대문자) 대신 close(소문자): 닫기 실패로 서버가 꺼지지 않게 함 */
    }
    return NULL;
}

/* ============================== M4-2 실행 인자 ============================== */

static void usage_exit(const char *prog)
{
    fprintf(stderr,
            "usage: %s <port> [--root DIR] [--threads N] [--queue N]\n"
            "              [--timeout 초] [--keepalive-max N]\n"
            "  기본값: --threads %d --queue %d --timeout %d --keepalive-max %d\n",
            prog, cfg.threads, cfg.queue, cfg.timeout, cfg.keepalive_max);
    exit(1);
}

/*
 * 문자열 s를 정수로 읽어 [min, max] 안이면 *out에 넣고 0, 아니면 -1.
 * atoi는 "abc"를 0으로, "12x"를 12로 읽고 오류를 알려 주지 않아서 strtol을 씀.
 */
static int int_arg_parse(const char *s, int min, int max, int *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') { /* 범위 초과, 숫자 없음, 뒤에 글자 */
        return -1;
    }
    if (v < min || v > max) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

/*
 * <port>는 지금처럼 첫 번째 인자 (기존 기능 테스트가 ./tiny <port>로 띄움).
 * 나머지는 "--이름 값" 쌍이고 순서는 상관없음. 틀리면 사용법 출력 후 exit(1).
 */
static void args_parse(int argc, char **argv)
{
    /* 옵션 이름 → 값을 넣을 곳, 허용 범위 */
    struct {
        const char *name;
        int *dst;
        int min, max;
    } int_opts[] = {
        {"--threads", &cfg.threads, 1, 1024},
        {"--queue", &cfg.queue, 1, 65536},
        {"--timeout", &cfg.timeout, 1, 3600},
        {"--keepalive-max", &cfg.keepalive_max, 1, 1000000},
    };
    size_t n_int_opts = sizeof(int_opts) / sizeof(int_opts[0]);

    if (argc < 2 || argv[1][0] == '-') { /* 포트가 없거나 옵션이 먼저 오면 */
        usage_exit(argv[0]);
    }
    cfg.port = argv[1];

    for (int i = 2; i < argc; i += 2) {
        const char *name = argv[i];
        const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;
        size_t k;

        if (value == NULL) { /* "--threads" 뒤에 값이 없음 */
            fprintf(stderr, "%s: 값이 없습니다\n", name);
            usage_exit(argv[0]);
        }
        if (strcmp(name, "--root") == 0) {
            cfg.root = value;
            continue;
        }
        for (k = 0; k < n_int_opts; k++) {
            if (strcmp(name, int_opts[k].name) == 0) {
                break;
            }
        }
        if (k == n_int_opts) { /* 모르는 옵션 */
            fprintf(stderr, "%s: 모르는 옵션입니다\n", name);
            usage_exit(argv[0]);
        }
        if (int_arg_parse(value, int_opts[k].min, int_opts[k].max,
                          int_opts[k].dst) < 0) {
            fprintf(stderr, "%s %s: %d 이상 %d 이하의 정수여야 합니다\n", name,
                    value, int_opts[k].min, int_opts[k].max);
            usage_exit(argv[0]);
        }
    }
}

/* ============================== M4-3 소켓 설정 ============================== */

/*
 * [M4-3] accept로 연결을 받고, CGI 자식에게 새지 않도록 FD_CLOEXEC를 켭니다.
 * FD_CLOEXEC가 켜진 fd는 execve할 때 커널이 자동으로 닫습니다.
 *
 * Linux: accept4(SOCK_CLOEXEC)가 받는 순간 함께 켜므로 빈틈이 없음.
 * macOS: accept4가 없어 accept 뒤에 fcntl로 켬. 그 사이에 다른 작업자가 fork하면
 *        이 소켓이 자식에게 넘어갈 수 있음 → cgi_child_run이 fd 3 이상을 모두
 *        닫아서 한 번 더 막음 (final.md 2.6 "두 겹")
 * 성공하면 connfd, 실패하면 -1 (errno 유지)
 */
static int conn_accept(int listenfd)
{
    struct sockaddr_in clientaddr; /* 주소는 쓰지 않지만 accept가 채울 곳이 필요 */
    socklen_t clientlen = sizeof(clientaddr);
    int connfd;

#ifdef __linux__
    connfd = accept4(listenfd, (SA *)&clientaddr, &clientlen, SOCK_CLOEXEC);
#else
    connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
    if (connfd >= 0 && fd_cloexec_set(connfd) < 0) {
        int saved = errno; /* close가 errno를 바꾸지 않게 보관 */
        close(connfd);
        errno = saved;
        return -1;
    }
#endif
    return connfd;
}

/* fd에 FD_CLOEXEC를 켭니다. 기존 플래그는 유지. 성공 0, 실패 -1 */
static int fd_cloexec_set(int fd)
{
    int flags = fcntl(fd, F_GETFD);

    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

/*
 * [M4-3] 접속 소켓에 타임아웃을 겁니다 (final.md 2.4).
 *   읽기 SO_RCVTIMEO = cfg.timeout초: 접속만 하고 요청을 안 보내는 손님,
 *     아주 느린 요청을 정리. 시간이 지나면 read가 -1(EAGAIN)을 돌려주고
 *     rio_readlineb도 -1 → doit이 응답 없이 끝나고 작업자가 다음 연결로 감
 *   쓰기 SO_SNDTIMEO = 2배: 응답을 아주 느리게 받아 가는 손님을 정리
 * 한계: read 한 번 기준이라 몇 초마다 한 글자씩 보내면 계속 붙잡힘 (Slowloris, 범위 밖)
 * 성공 0, 실패 -1
 */
static int conn_timeouts_set(int connfd)
{
    struct timeval rcv = {cfg.timeout, 0};
    struct timeval snd = {cfg.timeout * 2, 0};

    if (setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv)) < 0) {
        return -1;
    }
    return setsockopt(connfd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
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

    /* [M4-3] CGI 자식이 리스닝 소켓을 물려받지 않게 함. 물려받으면 서버를 껐다 켤 때
     * 아직 도는 CGI가 포트를 잡고 있어 bind가 실패함. 다른 스레드가 아직 없으므로
     * socket과 fcntl 사이에 fork가 끼어들 틈은 없음 */
    if (fd_cloexec_set(listenfd) < 0) {
        perror("fcntl");
        close(listenfd);
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
    int header_len;
    pid_t pid;
    /* [M4-3] CGI에 넘길 환경 변수를 fork 전에 부모가 만들어 둠 (아래 cgi_child_run 설명).
     * 스택 배열이라 malloc이 없고, 자식은 fork로 복사된 이 배열을 그대로 execve에 넘김 */
    char query_env[MAXLINE + sizeof("QUERY_STRING=")];
    char method_env[] = "REQUEST_METHOD=GET";
    char *envp[] = {query_env, method_env, NULL};
    int n;

    n = snprintf(query_env, sizeof(query_env), "QUERY_STRING=%s", cgiargs);
    if (n < 0 || n >= (int)sizeof(query_env)) { /* 잘렸으면 잘린 쿼리로 실행하지 않음 */
        return;
    }

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
    if (pid == 0) {                           /* 자식 프로세스이면 */
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
 *
 * [M4-3] 멀티스레드 서버에서 fork하면 자식에는 fork를 부른 스레드 하나만 복사됩니다.
 * 그 순간 다른 작업자가 malloc 내부 락을 잡고 있었다면, 자식에서 그 락은 풀어 줄
 * 스레드가 없어 영원히 잠깁니다. 그래서 자식은 malloc을 쓰지 않는 함수
 * (async-signal-safe: dup2, close, execve, _exit)만 씁니다.
 *   - setenv(내부에서 malloc) → 부모가 fork 전에 만든 envp를 execve에 넘김
 *   - Dup2, Execve(대문자 래퍼) → 실패하면 fprintf + exit를 부름 → 소문자 + _exit
 * 또 execve 전에 fd 3번 이상을 모두 닫아, 다른 손님 소켓과 리스닝 소켓이
 * CGI 프로그램에 넘어가지 않게 합니다 (FD_CLOEXEC 설정 전 틈까지 막음).
 */
static void cgi_child_run(int fd, const char *filename, char *const envp[])
{
    char *argv[] = {NULL}; /* Tiny의 emptylist: CGI 프로그램에 넘길 인자 없음 */

    /* 표준 출력(1번)을 클라이언트 소켓으로 돌림 → printf가 곧바로 응답이 됨.
     * dup2로 만든 1번에는 FD_CLOEXEC가 없으므로 execve 뒤에도 남음 */
    if (dup2(fd, STDOUT_FILENO) < 0) {
        _exit(127);
    }
    /* 0, 1, 2(표준 입출력)만 남기고 모두 닫음. 원래 소켓 fd도 1번에 복사했으니 닫아도 됨 */
    for (int i = 3; i < fd_close_max; i++) {
        close(i);
    }
    execve(filename, argv, envp); /* 이 프로세스를 CGI 프로그램으로 바꿈. 성공하면 돌아오지 않음 */
    _exit(127); /* 여기 왔다면 execve 실패. exit 대신 _exit: 부모에게서 복사된 stdio 버퍼를 비우지 않음 */
}