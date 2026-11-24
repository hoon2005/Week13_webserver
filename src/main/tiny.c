/*
 * tiny.c - 정적 파일과 CGI(GET)를 처리하는 작은 웹 서버입니다.
 *   CS:APP 11.6절 Tiny와 같은 일을 합니다. Phase 1 계획(M1~M3)에 따라
 *   Tiny의 각 부분을 같은 동작의 우리 코드로 바꿔 끼웠습니다.
 *     M1 소켓 연결: listen_socket_open, main의 accept 반복문
 *     M2 정적 GET:  doit과 단계 함수들, static_file_serve, http_error_send
 *     M3 CGI GET:   cgi_serve ([M5-3] 실행과 출력 해석은 cgi.c로 옮김)
 *   리뷰에서 나온 안정성 수정 5가지도 반영했습니다. 클라이언트 한 명의 문제
 *   (연결 중단, 깨진 요청, 빈 파일, fork 실패)로 서버 전체가 꺼지지 않고
 *   그 요청만 끝냅니다.
 *   Phase 2 M4-2: 스레드 풀로 동시에 처리합니다. main은 accept한 연결을 작업
 *   큐(conn_queue.c)에 넣기만 하고, 미리 만든 작업자 N개가 꺼내 처리합니다.
 *   (M4-1의 연결당 스레드 버전은 부하 측정 비교 기준으로 그 커밋에 남아 있습니다)
 *   Phase 2 M4-3: 접속 소켓에 읽기·쓰기 타임아웃을 걸고, CGI 자식이 다른 손님
 *   소켓과 리스닝 소켓을 물려받지 않게 막고(FD_CLOEXEC + 자식 fd 정리),
 *   자식은 fork 뒤에 malloc을 쓰는 함수(setenv 등)를 부르지 않습니다.
 *   Phase 2 M5-1: 요청 줄과 헤더를 http.c의 파서로 읽어 검사하고(400/414/501/505),
 *   모든 응답을 HTTP/1.1 + Content-Length + Connection으로 보냅니다.
 *   HEAD(정적 파일만), CSS·JS MIME, --root를 지원합니다.
 *   Phase 2 M5-2: Content-Length만큼 요청 본문을 읽습니다(1 MiB 넘으면 413,
 *   Expect: 100-continue에 답함). CGI POST는 본문을 임시 파일에 담아 stdin으로 넘깁니다.
 *   Phase 2 M5-3: CGI 출력을 파이프로 모은 뒤 서버가 응답을 만듭니다(cgi.c).
 *   CGI가 실패하면 500, 10초 안에 안 끝나면 504, Content-Length는 서버가 셉니다.
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
#include "http.h" /* [M5-1] 요청 파서, MIME, 상태 문구 */
#include "cgi.h"  /* [M5-3] CGI 실행과 출력 해석 */

/* M1 */
static int listen_socket_open(const char *port); /* 새로 만듦: csapp.c의 open_listenfd 대신 */

/* M4-2: 스레드 풀과 실행 인자 */
static void args_parse(int argc, char **argv);  /* 실행 인자 → cfg */
static void *worker_main(void *arg);            /* 작업자: 큐에서 꺼내 doit → close 반복 */

/* M4-3: 소켓 설정 */
static int conn_accept(int listenfd);           /* accept + FD_CLOEXEC (OS별 분기) */
static int fd_cloexec_set(int fd);              /* fd에 FD_CLOEXEC 켜기 */
static int conn_timeouts_set(int connfd);       /* SO_RCVTIMEO, SO_SNDTIMEO 걸기 */

/* M2: 요청 처리 ([M5-1] 단계 함수는 응답을 직접 보내지 않고 상태 코드를 돌려줌) */
void doit(int fd);
static int request_read(rio_t *rp, struct http_request *req); /* [M5-1] 요청 줄 + 헤더 */
static void conn_linger(int fd); /* [M5-1] 읽지 않은 요청 바이트를 비우고 닫을 준비 */
static int uri_check(const char *uri);                    /* 새로 만듦: ../ 차단 */
static int uri_has_dot_dot(const char *path, size_t len); /* uri_check가 쓰는 도우미 */
static int uri_parse(char *uri, char *filename, char *cgiargs); /* parse_uri 대신 */
static int file_find(const char *filename, struct stat *sbuf); /* doit에서 분리 */
static int file_check_readable(const struct stat *sbuf);  /* doit에서 분리 */
static int file_check_executable(const struct stat *sbuf); /* doit에서 분리 */

/* M2: 정적 응답, 오류 응답 */
static int static_file_serve(int fd, const char *filename, int filesize,
                             int head); /* serve_static 대신 */
static void http_error_send(int fd, int head, int status, const char *cause,
                            const char *longmsg); /* clienterror 대신 */

/* M3: CGI */
static int cgi_serve(int fd, const char *filename, const char *cgiargs,
                     const struct http_request *req);  /* serve_dynamic 대신 (부모 쪽) */
static int cgi_input_open(const struct http_request *req); /* [M5-2] CGI stdin 준비 */


/* ============================== M1 소켓 연결 ============================== */

/*
 * [M4-2] 실행 인자로 받는 설정값 (final.md 0장 "실행 방법")
 * timeout은 M4-3에서 소켓 타임아웃에, root는 M5-1에서 시작 폴더에 씁니다.
 * keepalive_max는 받아 두기만 하고 M5-4a에서 사용합니다.
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

    /* [M5-1] --root: 정적 파일과 CGI를 찾는 폴더. 파일 경로는 "." + URI로 만드므로
     * (uri_parse) 시작할 때 그 폴더로 이동하면 경로 해석을 따로 바꿀 필요가 없음.
     * 스레드를 만들기 전이라 다른 스레드가 현재 폴더를 쓰는 중일 일도 없음 */
    if (cfg.root != NULL && chdir(cfg.root) < 0) {
        perror(cfg.root);
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
 *        이 소켓이 자식에게 넘어갈 수 있음 → CGI 자식(cgi.c)이 fd 3 이상을 모두
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


/* [M5-1] 요청을 읽다가 실패했을 때 오류 본문에 넣을 설명 */
static const char *request_error_msg(int status)
{
    switch (status) {
    case 413:
        return "The request body is larger than 1 MiB";
    case 414:
        return "The request line is too long";
    case 417:
        return "Tiny only understands Expect: 100-continue";
    case 501:
        return "Tiny does not implement this method or transfer encoding";
    case 505:
        return "Tiny supports only HTTP/1.0 and HTTP/1.1";
    default:
        return "Tiny couldn't understand this request";
    }
}

/*
 * Tiny의 doit은 모든 일을 직접 했음. 여기서는 역할별 함수를 순서대로 부르고,
 * 함수가 오류 상태 코드를 돌려주면 http_error_send로 응답하고 끝냅니다.
 * [M5-1] 오류 응답을 보내는 곳을 doit 한 군데로 모았습니다. 단계 함수는 상태
 * 코드만 돌려주므로, HEAD 요청이면 본문을 빼는 일을 여기서 한 번에 처리합니다.
 * 흐름: 요청 읽기(요청 줄 + 헤더, request_read) → URI 확인 → URI 해석
 *       → 메서드 확인 → 파일 찾기 → (정적) 읽기 권한 확인 → static_file_serve
 *                                   (동적) 실행 권한 확인 → cgi_serve
 * 이 단계에서는 모든 응답 뒤에 연결을 닫습니다 (keep-alive는 M5-4a).
 * [M5-2] 헤더 다음에 본문을 읽습니다: 크기 검사(413) → Expect(417, 100 Continue)
 *   → Content-Length만큼 읽기. 본문을 받지 않고 거절했으면 남은 바이트를 비우고 닫습니다.
 */
void doit(int fd)
{
    struct http_request req; /* [M5-1] 요청 줄, 헤더 필드, 원본 헤더 블록 */
    char filename[MAXLINE], cgiargs[MAXLINE];
    struct stat sbuf;
    rio_t rio;
    int is_static;
    int status;
    int head; /* HEAD 요청이면 1: 헤더는 GET과 같고 본문만 뺌 */
    int unread; /* 읽지 않은 요청 바이트가 남았을 수 있으면 1 → 닫기 전에 비움 */

    /* 1. 요청 읽기: 연결에 읽기 버퍼를 붙이고 요청 줄과 헤더를 파싱한다 */
    http_request_init(&req);
    rio_readinitb(&rio, fd);
    status = request_read(&rio, &req);
    if (status < 0) { /* 끊김·타임아웃: 응답할 상대가 없으므로 그냥 끝냄 */
        return;
    }
    head = strcmp(req.method, "HEAD") == 0;
    /* 요청 읽기가 중간에 멈췄거나(오류), 본문이 있는데 아직 읽지 않았으면 1 */
    unread = status > 0 || (req.has_content_length && req.content_length > 0);
    if (status > 0) { /* 형식 오류 400, 긴 요청 줄 414, 501, 505 */
        http_error_send(fd, head, status, "request", request_error_msg(status));
        goto done;
    }

    /* [M5-2] 1-1. 본문을 받기 전 검사: 1 MiB 초과 413, 이상한 Expect 417.
     * 크기를 먼저 보므로, 100-continue를 보낸 큰 본문은 100 없이 413을 받음 */
    status = http_body_check(&req);
    if (status == 0) {
        status = http_expect_check(&req);
    }
    if (status == HTTP_EXPECT_SEND_100) {
        /* "보내도 된다"고 답함. 답하지 않으면 curl은 약 1초 기다린 뒤 본문을 보냄 */
        const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (rio_writen(fd, (void *)cont, sizeof(cont) - 1) !=
            (ssize_t)(sizeof(cont) - 1)) {
            goto done; /* 끊김 */
        }
        status = 0;
    }
    if (status != 0) { /* 413, 417: 본문은 받지 않음 → done에서 남은 바이트를 비움 */
        http_error_send(fd, head, status, "request body",
                        request_error_msg(status));
        goto done;
    }

    /* [M5-2] 1-2. 본문 읽기: 헤더를 읽은 같은 rio로 Content-Length만큼.
     * GET에 붙은 본문도 읽어서 버림 (읽지 않으면 keep-alive에서 다음 요청으로 읽힘) */
    status = http_body_read(&req, &rio);
    if (status < 0) { /* 본문 도중 끊김·타임아웃: 응답할 상대가 없음 */
        unread = 0;
        goto done;
    }
    if (status > 0) { /* 메모리 부족 500 */
        http_error_send(fd, head, status, "request body",
                        "Tiny couldn't allocate the request body");
        goto done;
    }
    unread = 0; /* 요청을 끝까지 읽었음 */

    /* 2. URI 확인: 폴더 밖으로 나가는 ..이 있으면 403 */
    if (uri_check(req.uri) != 0) {
        http_error_send(fd, head, 403, req.uri,
                        "Tiny doesn't serve files outside its folder");
        goto done;
    }

    /* 3. URI 해석: 정적/동적 판단, 파일 경로와 CGI 인자 만들기 */
    is_static = uri_parse(req.uri, filename, cgiargs); /* parse_uri 대신 */
    /* [안정성 수정] 경로가 버퍼보다 길어 잘렸으면, 잘린 경로로 엉뚱한 파일을
     * 찾지 않도록 파일 조회 전에 414를 보내고 끝냄 */
    if (is_static < 0) {
        http_error_send(fd, head, 414, req.uri,
                        "Tiny couldn't handle such a long path");
        goto done;
    }

    /* 4. 메서드 확인: 정적/동적에 따라 지원하는 메서드가 다름 (Phase 1처럼 파일 찾기 전) */
    if (is_static && strcmp(req.method, "POST") == 0) {
        /* 정적 파일 POST는 Phase 1처럼 501 유지 (final.md 6장) */
        http_error_send(fd, head, 501, req.method,
                        "Tiny does not implement POST for static files");
        goto done;
    }
    if (!is_static && head) { /* CGI HEAD는 범위 밖 (final.md 3.8) */
        http_error_send(fd, head, 501, req.method,
                        "Tiny does not implement HEAD for CGI programs");
        goto done;
    }

    /* 5. 파일 찾기: 없으면 404 */
    if (file_find(filename, &sbuf) != 0) {
        http_error_send(fd, head, 404, filename, "Tiny couldn't find this file");
        goto done;
    }

    /* 6. 응답 분기: 권한을 확인하고 정적 / 동적 처리로 보낸다 */
    if (is_static) {
        if (file_check_readable(&sbuf) != 0) {
            http_error_send(fd, head, 403, filename, "Tiny couldn't read the file");
            goto done;
        }
        status = static_file_serve(fd, filename, sbuf.st_size, head);
        if (status != 0) { /* 파일 준비 실패: 아직 아무것도 안 보냈으므로 500 */
            http_error_send(fd, head, status, filename,
                            "Tiny couldn't open the file");
        }
    } else {
        if (file_check_executable(&sbuf) != 0) {
            http_error_send(fd, head, 403, filename,
                            "Tiny couldn't run the CGI program");
            goto done;
        }
        status = cgi_serve(fd, filename, cgiargs, &req); /* serve_dynamic 대신 */
        /* [M5-3] 출력을 모은 뒤 응답하므로, 실패해도 아직 아무것도 안 보냈음 */
        if (status == 504) {
            http_error_send(fd, head, 504, filename,
                            "The CGI program didn't finish in time");
        } else if (status != 0) {
            http_error_send(fd, head, status, filename,
                            "The CGI program failed");
        }
    }

done:
    if (unread) {
        conn_linger(fd);
    }
    http_request_free(&req); /* [M5-2] 본문 버퍼는 모든 경로에서 해제 */
}
/*
 * [M5-1] 응답을 보낸 뒤, 아직 읽지 않은 요청 바이트를 잠깐 읽어 버립니다.
 * 받은 바이트가 남은 소켓을 close하면 커널은 FIN 대신 RST를 보냅니다. 그러면
 * 클라이언트는 이미 도착한 응답까지 버리고 "Connection reset"을 받습니다
 * (긴 URI에 414를 보냈는데 클라이언트가 응답을 못 받는 문제로 발견).
 *   1. shutdown(SHUT_WR): 보내기 쪽만 먼저 닫아 FIN을 보냄 → 클라이언트는 응답 끝을 앎
 *   2. 남은 바이트를 읽어 버림. 1초 동안 안 오거나, 끝(0)이 오거나, LINGER_MAX를
 *      넘으면 멈춤 (작업자를 오래 붙잡지 않게)
 * 실제 close는 worker_main이 합니다.
 */
#define LINGER_MAX BODY_MAX /* 비울 최대 바이트: 본문 상한과 같은 1 MiB */
static void conn_linger(int fd)
{
    char buf[4096];
    struct timeval tv = {1, 0};
    size_t total = 0;
    ssize_t n;

    shutdown(fd, SHUT_WR);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (total < LINGER_MAX && (n = read(fd, buf, sizeof(buf))) > 0) {
        total += (size_t)n;
    }
}

/*
 * [M5-1] 요청 줄과 헤더를 빈 줄까지 읽어 req에 채웁니다.
 * Phase 1의 request_line_read + request_headers_skip을 대신합니다. 줄을 읽는
 * 일만 여기서 하고, 줄을 나누고 검사하는 일은 http.c의 파서가 합니다.
 *   0:  정상 (req가 채워짐)
 *   -1: 줄바꿈 전에 끊김, 읽기 오류, 타임아웃 → 응답할 상대가 없으므로 그냥 닫음
 *   그 밖: 보낼 오류 상태 코드 (400, 414, 501, 505). 응답 뒤 닫음
 * 읽은 줄은 확인용으로 터미널에 출력합니다 (Phase 1과 같음).
 */

static int request_read(rio_t *rp, struct http_request *req)
{
    char buf[MAXLINE];
    ssize_t n;
    int status;

    /* Rio_readlineb(대문자) 대신 rio_readlineb: 대문자는 읽기 오류 시 exit함 */
    n = rio_readlineb(rp, buf, MAXLINE);
    /* RFC 9112 §2.2: 요청 줄 앞의 빈 줄 하나는 건너뜀 (앞 요청 본문 뒤 남은 CRLF 등) */
    if (n > 0 && http_is_blank_line(buf, (size_t)n)) {
        n = rio_readlineb(rp, buf, MAXLINE);
    }
    switch (http_line_check(buf, n, sizeof(buf))) {
    case HTTP_LINE_INCOMPLETE:
        return -1;
    case HTTP_LINE_TOO_LONG: /* 요청 줄이 MAXLINE을 넘음 = 매우 긴 URI */
        return 414;
    case HTTP_LINE_OK:
        break;
    }
    fwrite(buf, 1, (size_t)n, stdout); /* 확인용 출력. NUL이 있어도 끝까지 */
    status = http_request_line_parse(req, buf, (size_t)n);
    if (status != 0) { /* 형식이 틀리면 헤더는 읽지 않고 바로 오류 응답 */
        return status;
    }

    /* 헤더: 빈 줄이 나올 때까지 한 줄씩 */
    while (1) {
        n = rio_readlineb(rp, buf, MAXLINE);
        switch (http_line_check(buf, n, sizeof(buf))) {
        case HTTP_LINE_INCOMPLETE: /* 빈 줄 전에 끊김 */
            return -1;
        case HTTP_LINE_TOO_LONG: /* 헤더 한 줄이 MAXLINE을 넘음 */
            return 400;
        case HTTP_LINE_OK:
            break;
        }
        fwrite(buf, 1, (size_t)n, stdout);
        if (http_is_blank_line(buf, (size_t)n)) { /* 빈 줄 = 헤더 끝 */
            break;
        }
        status = http_header_line_parse(req, buf, (size_t)n);
        if (status != 0) {
            return status;
        }
    }
    /* 헤더끼리 얽힌 규칙: Host 필수, CL+TE, TE만 */
    return http_headers_validate(req);
}

/*
 * (새로 만든 함수)
 * Tiny는 URI 앞에 "."만 붙여 파일을 찾아서, /../etc/passwd로 웹 폴더 밖의
 * 파일을 읽거나 /cgi-bin/../../bin/ls로 다른 프로그램을 실행할 수 있었습니다.
 * URI 해석보다 먼저 확인해서 정적, 동적 요청 모두 막습니다.
 * 막아야 하면 403, 괜찮으면 0.
 */
static int uri_check(const char *uri)
{
    const char *question = strchr(uri, '?'); /* 쿼리 문자열 시작 위치 */
    /* '?'가 있으면 그 앞까지만, 없으면 URI 전체가 경로 */
    size_t path_len = question ? (size_t)(question - uri) : strlen(uri);

    return uri_has_dot_dot(uri, path_len) ? 403 : 0; /* 경로 조각 중 ".."이 있으면 403 */
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
 * 채웁니다. 없으면 404, 있으면 0.
 */
static int file_find(const char *filename, struct stat *sbuf)
{
    return stat(filename, sbuf) < 0 ? 404 : 0;
}

/*
 * 정적 파일을 보내도 되는지 확인합니다.
 * 일반 파일이 아니거나(폴더, 특수 파일) 주인 읽기 권한이 없으면 403.
 */
static int file_check_readable(const struct stat *sbuf)
{
    return (!S_ISREG(sbuf->st_mode) || !(S_IRUSR & sbuf->st_mode)) ? 403 : 0;
}

/*
 * CGI 프로그램을 실행해도 되는지 확인합니다.
 * 일반 파일이 아니거나 주인 실행 권한이 없으면 403.
 */
static int file_check_executable(const struct stat *sbuf)
{
    return (!S_ISREG(sbuf->st_mode) || !(S_IXUSR & sbuf->st_mode)) ? 403 : 0;
}

/*
 * 정적 파일을 200 응답으로 보냅니다. Tiny의 serve_static을 대신합니다.
 *
 * [안정성 수정] 순서를 바꿨습니다.
 *   Tiny: 200 헤더 전송 → 파일 열기 → mmap → 본문 전송
 *   지금: 파일 열기 → mmap → (준비가 끝나면) 200 헤더 전송 → 본문 전송
 *   - 파일 준비가 실패하면 아직 아무것도 안 보냈으므로 500을 돌려줘 doit이 보냄
 *   - 크기 0인 파일은 mmap이 실패(EINVAL)해서 Tiny에서는 Mmap이 서버를
 *     종료시켰음. 크기 0이면 mmap 없이 Content-Length: 0으로 헤더만 보냄
 *   - Open/Mmap/Close/Rio_writen(대문자)은 실패하면 exit하므로 소문자로 바꾸고
 *     실패하면 이 요청만 끝냄
 * [M5-1] HEAD면 GET과 같은 헤더(Content-Length 포함)만 보내고 본문은 뺍니다.
 *   파일 내용이 필요 없으므로 열지도 않습니다 (읽기 권한은 doit이 이미 확인).
 * 반환: 0(보냈거나 보내다 끊김), 500(파일 준비 실패, 아직 아무것도 안 보냄)
 */
static int static_file_serve(int fd, const char *filename, int filesize,
                             int head)
{
    char header[MAXBUF];
    int header_len; /* snprintf가 만든 헤더 길이 (strlen 대신 사용) */
    int srcfd;
    char *srcp = NULL; /* 크기 0이거나 HEAD면 mmap하지 않으므로 NULL로 둠 */

    /* 1~2. 파일 준비: 열기 (Open 대신 open) → 메모리에 올리기 (Mmap 대신 mmap).
     * 크기 0이면 올릴 내용이 없고, HEAD면 본문을 보내지 않으므로 건너뜀 */
    if (!head && filesize > 0) {
        srcfd = open(filename, O_RDONLY, 0);
        if (srcfd < 0) { /* 열기 실패: 아직 아무것도 안 보냈으므로 500 */
            return 500;
        }
        srcp = mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
        close(srcfd); /* mmap 뒤에는 fd가 필요 없음 (Close 대신 close) */
        if (srcp == MAP_FAILED) {
            return 500;
        }
    }

    /* 3. 준비가 끝났으므로 200 헤더 전송.
     * Tiny는 sprintf로 한 줄씩 이어 붙였음 → snprintf 한 번으로 헤더 전체를 만듦.
     * [M5-1] HTTP/1.1 상태 줄, Connection 추가 (이 단계는 항상 close) */
    header_len = snprintf(header, sizeof(header),
                          "HTTP/1.1 200 OK\r\n"
                          "Server: Tiny Web Server\r\n"
                          "Connection: close\r\n"
                          "Content-Length: %d\r\n"
                          "Content-Type: %s\r\n\r\n",
                          filesize, http_mime_type(filename)); /* get_filetype 대신 */

    /* 4. 헤더를 다 보냈을 때만 본문 전송 (Rio_writen 대신 rio_writen).
     * 쓰기가 실패하면(클라이언트가 끊음) 더 보내지 않고 정리만 함 */
    if (rio_writen(fd, header, header_len) == header_len && srcp != NULL) {
        rio_writen(fd, srcp, filesize); /* 실패해도 이 요청만 끝남 */
    }

    /* 5. 정리: mmap했던 경우에만 해제 (Munmap 대신 munmap) */
    if (srcp != NULL) {
        munmap(srcp, filesize);
    }
    return 0;
}

/*
 * 오류 응답(상태 줄, 헤더, 짧은 HTML 본문)을 보냅니다.
 * Tiny의 clienterror를 대신합니다.
 * [M5-1] 상태 줄은 HTTP/1.1, 이유 문구는 http_status_text가 정합니다.
 *   클라이언트가 응답 끝을 알 수 있게 Content-Length와 Connection을 넣습니다.
 *   HEAD 요청이면 헤더(Content-Length 포함)만 보내고 본문은 뺍니다.
 */
static void http_error_send(int fd, int head, int status, const char *cause,
                            const char *longmsg)
{
    char header[MAXLINE]; /* Tiny는 buf 하나를 헤더 줄마다 다시 썼음 → 헤더용 버퍼 따로 */
    char body[MAXBUF];
    const char *shortmsg = http_status_text(status);
    int header_len;
    int body_len;

    /* Tiny는 sprintf(body, "%s...", body)로 자기 자신에 이어 붙였음 (정의되지 않은 동작)
     * → snprintf 한 번으로 본문 전체를 만듦 */
    body_len = snprintf(body, sizeof(body),
                        "<html><title>Tiny Error</title>"
                        "<body bgcolor=ffffff>\r\n"
                        "%d: %s\r\n"
                        "<p>%s: %s\r\n"
                        "<hr><em>The Tiny Web server</em>\r\n",
                        status, shortmsg, longmsg, cause);
    /* cause(URI 등)가 아주 길어 잘렸으면, 실제로 버퍼에 들어간 길이만큼만 보냄 */
    if (body_len >= (int)sizeof(body)) {
        body_len = sizeof(body) - 1;
    }

    /* 본문 길이를 알아야 Content-Length를 쓰므로 본문을 먼저 만듦 */
    header_len = snprintf(header, sizeof(header),
                          "HTTP/1.1 %d %s\r\n"
                          "Server: Tiny Web Server\r\n"
                          "Connection: close\r\n"
                          "Content-Length: %d\r\n"
                          "Content-Type: text/html\r\n\r\n",
                          status, shortmsg, body_len);

    /* [안정성 수정] Rio_writen(대문자)은 쓰기 실패 시 exit함 → rio_writen.
     * 헤더 쓰기가 실패하면(클라이언트가 끊음) 본문은 보내지 않음 */
    if (rio_writen(fd, header, header_len) != header_len || head) {
        return;
    }
    rio_writen(fd, body, body_len); /* 실패해도 이 요청만 끝남 */
}

/* ============================== M3 CGI GET ============================== */


/*
 * CGI 요청을 처리합니다. Tiny의 serve_dynamic 중 부모가 하던 일입니다.
 * [M5-3] 순서를 바꿨습니다 (final.md 4.1~4.4).
 *   M5-2까지: 200 상태 줄을 먼저 보냄 → fork → CGI가 소켓에 직접 씀
 *     - CGI가 실패해도 클라이언트는 200 + 빈 본문을 받음
 *     - 끝나지 않는 CGI를 기다리는 동안 작업자가 묶임
 *     - 응답이 어디서 끝나는지 서버가 몰라 keep-alive를 할 수 없음
 *   지금: 입력 준비 → cgi_run(출력을 파이프로 끝까지 모음, 1 MiB·10초 상한)
 *         → cgi_response_build(헤더/본문 나누기, 서버가 길이를 셈) → 한 번에 보냄
 * 반환: 0(보냈거나 보내다 끊김), 500(입력 준비·실행 실패, 출력 이상), 504(시간 초과).
 * 0이 아니면 아직 아무것도 보내지 않았으므로 doit이 오류 응답을 보냅니다.
 */
static int cgi_serve(int fd, const char *filename, const char *cgiargs,
                     const struct http_request *req)
{
    char header[MAXBUF];
    /* [M4-3] CGI에 넘길 환경 변수를 fork 전에 부모가 만들어 둠 (cgi.c의 child_exec 설명).
     * 스택 배열이라 malloc이 없고, 자식은 fork로 복사된 이 배열을 그대로 execve에 넘김.
     * [M5-2] REQUEST_METHOD는 실제 메서드, POST면 CONTENT_LENGTH, 있으면 CONTENT_TYPE 추가 */
    char query_env[MAXLINE + sizeof("QUERY_STRING=")];
    char method_env[sizeof("REQUEST_METHOD=") + sizeof(req->method)];
    char length_env[sizeof("CONTENT_LENGTH=") + 24]; /* 24: 64비트 정수 자릿수보다 넉넉히 */
    char type_env[sizeof("CONTENT_TYPE=") + sizeof(req->content_type)];
    char *envp[5];
    int envc = 0;
    struct cgi_output out = {NULL, 0};
    const char *body;
    size_t body_len;
    int code;
    int infd;
    int status;
    int n;

    n = snprintf(query_env, sizeof(query_env), "QUERY_STRING=%s", cgiargs);
    if (n < 0 || n >= (int)sizeof(query_env)) { /* 잘렸으면 잘린 쿼리로 실행하지 않음 */
        return 500;
    }
    envp[envc++] = query_env;
    snprintf(method_env, sizeof(method_env), "REQUEST_METHOD=%s", req->method);
    envp[envc++] = method_env;
    if (strcmp(req->method, "POST") == 0) { /* 본문 길이: CGI가 stdin에서 읽을 바이트 수 */
        snprintf(length_env, sizeof(length_env), "CONTENT_LENGTH=%zu",
                 req->body_len);
        envp[envc++] = length_env;
    }
    if (req->content_type[0] != '\0') { /* 해석하지 않고 그대로 넘김 */
        snprintf(type_env, sizeof(type_env), "CONTENT_TYPE=%s",
                 req->content_type);
        envp[envc++] = type_env;
    }
    envp[envc] = NULL;

    /* [M5-2] CGI의 표준 입력 준비 (본문 임시 파일 또는 /dev/null) */
    infd = cgi_input_open(req);
    if (infd < 0) {
        perror("cgi input");
        return 500;
    }

    /* [M5-3] 실행하고 출력을 모음. fork·exec·kill·waitpid는 모두 cgi_run 안에서 */
    status = cgi_run(filename, envp, infd, CGI_TIMEOUT_SEC, CGI_OUT_MAX,
                     fd_close_max, &out);
    close(infd); /* 임시 파일은 이름이 없으므로 여기서 닫으면 사라짐 */
    if (status != 0) { /* 500 또는 504. out은 이미 해제됨 */
        return status;
    }

    /* [M5-3] 출력 해석: Status 반영, CGI의 Content-Length·Connection은 버림.
     * keep-alive는 M5-4b에서 켬 (지금은 항상 close) */
    if (cgi_response_build(out.data, out.len, 0, header, sizeof(header), &code,
                           &body, &body_len) != 0) {
        cgi_output_free(&out);
        return 500;
    }
    /* [안정성 수정] Rio_writen 대신 rio_writen. 헤더를 다 보냈을 때만 본문 */
    if (rio_writen(fd, header, strlen(header)) == (ssize_t)strlen(header) &&
        body_len > 0) {
        rio_writen(fd, (void *)body, body_len); /* 실패해도 이 요청만 끝남 */
    }
    cgi_output_free(&out);
    return 0;
}

/*
 * [M5-2] CGI의 표준 입력으로 쓸 fd를 엽니다 (final.md 4.3).
 *   POST: 본문을 임시 파일에 써 두고 처음 위치로 되감은 fd
 *   그 밖: /dev/null (읽으면 바로 끝)
 * 파이프 대신 임시 파일을 쓰는 이유: 파이프는 버퍼(보통 64 KiB)가 차면 쓰기가 멈춤.
 * CGI가 stdin을 다 읽기 전에 출력부터 하면, 부모는 쓰기에서·CGI는 출력에서 서로를
 * 기다리는 교착이 생길 수 있음. 파일에 미리 다 써 두면 부모는 기다릴 일이 없음.
 * mkstemp 직후 unlink: 이름은 지우고 fd만 남김 → 서버가 중간에 죽어도 디스크에 안 남음.
 * 실패하면 -1 (열어 둔 fd는 닫음)
 */
static int cgi_input_open(const struct http_request *req)
{
    char path[1024];
    const char *dir;
    size_t off = 0;
    int fd;
    int n;

    if (strcmp(req->method, "POST") != 0) {
        fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) {
            fd_cloexec_set(fd); /* 다른 작업자의 CGI에게 새지 않게 (자식 fd 정리가 한 번 더 막음) */
        }
        return fd;
    }

    dir = getenv("TMPDIR"); /* 서버 어디서도 setenv를 안 하므로 스레드에서 읽어도 안전 */
    if (dir == NULL || dir[0] == '\0') {
        dir = "/tmp";
    }
    n = snprintf(path, sizeof(path), "%s/tiny-cgi-XXXXXX", dir);
    if (n < 0 || n >= (int)sizeof(path)) {
        return -1;
    }
    fd = mkstemp(path); /* 겹치지 않는 이름으로 만들고 읽기·쓰기로 염 */
    if (fd < 0) {
        return -1;
    }
    unlink(path);
    fd_cloexec_set(fd);

    while (off < req->body_len) { /* write는 일부만 쓸 수 있으므로 다 쓸 때까지 */
        ssize_t w = write(fd, req->body + off, req->body_len - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        off += (size_t)w;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) { /* CGI가 처음부터 읽도록 되감기 */
        close(fd);
        return -1;
    }
    return fd;
}
