/*
 * tiny.c - 정적 파일과 CGI를 처리하는 웹 서버입니다.
 *   Phase 1: CS:APP Tiny의 각 부분을 같은 동작의 우리 코드로 교체 (M1~M3)
 *   Phase 2: docs/prd/final.md의 M4 동시성, M5 HTTP 완성
 *     M4 스레드 풀(작업 큐는 src/conn_queue.c), 소켓 타임아웃, fd 상속 차단
 *     M5 요청 파서(src/http.c), 본문, Expect, CGI 구조 변경, keep-alive
 *     M6 Reverse Proxy (규칙은 src/route.c, 중계는 src/proxy.c)
 *
 * 사용법:
 *   ./build/tiny <port> [--root DIR] [--threads N] [--queue N]
 *                       [--timeout 초] [--keepalive-max N]
 *                       [--proxy /접두사/=백엔드,백엔드]... [--proxy-timeout 초]
 *
 * 요청 하나의 흐름 (final.md 7장)
 *   [main]   accept → FD_CLOEXEC, 타임아웃 설정 → 작업 큐에 넣기
 *   [작업자] 큐에서 꺼내기 → rio 준비(연결당 한 번) → 요청 반복 처리 → close
 *   요청 하나: 요청 줄 → 헤더 → 헤더 검사 → (Expect) → 본문 → 경로 확인
 *             → 백엔드 / 정적 파일 / CGI → 응답 → keep-alive 판단
 */
#if defined(__linux__)
/* syscall(SYS_close_range)용. _GNU_SOURCE는 csapp.h의 gai_error와 충돌해서 쓰지 않음 */
#define _DEFAULT_SOURCE
#endif

#include "csapp.h"
#include "conn_queue.h"
#include "http.h"
#include "proxy.h"
#include "route.h"

#include <ctype.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <strings.h>
#include <time.h>
#if defined(__linux__)
#include <sys/syscall.h>
/* glibc는 accept4를 _GNU_SOURCE에서만 선언하므로 직접 선언 (함수는 glibc에 있음) */
int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags);
#endif

#define CGI_TIMEOUT_SEC 10      /* CGI 실행 시간 제한 (final.md 2.4) */
#define CGI_OUT_MAX (1L << 20)  /* CGI 출력 상한 1 MiB (final.md 4.4) */

/* ============================== 설정 ============================== */

/* 실행 인자로 정하는 값. main이 작업자를 만들기 전에 채우고 이후에는 읽기만 함 */
struct server_config {
    const char *port;
    const char *root;  /* NULL이면 현재 폴더 (Phase 1 동작 유지) */
    int threads;       /* 작업자 수 */
    int queue;         /* 작업 큐 크기 */
    int timeout;       /* 읽기 타임아웃(초). 쓰기는 2배 */
    int keepalive_max; /* 연결당 요청 수 상한 */
    int proxy_timeout; /* 백엔드 시간 제한(초) */
    long open_max;     /* CGI 자식에서 fd를 닫을 범위 (fork 전에 계산) */
    char path_env[1024]; /* CGI에 넘길 PATH=... */
};

static struct server_config cfg = {
    .port = NULL,
    .root = NULL,
    .threads = 16,
    .queue = 64,
    .timeout = 5,
    .keepalive_max = 100,
    .proxy_timeout = 10,
};

/* --proxy로 받은 라우팅 규칙. 작업자를 만든 뒤에는 순번만 바뀜 */
static struct route_table routes;

/* main이 넣고 작업자가 꺼내는 작업 큐. 응답할 때도 기다리는 손님이 있는지 봄 */
static struct conn_queue queue;

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s <port> [--root DIR] [--threads N] [--queue N] "
            "[--timeout 초] [--keepalive-max N] "
            "[--proxy /접두사/=이름:포트,이름:포트]... [--proxy-timeout 초]\n",
            prog);
    exit(1);
}

/* 양의 정수 인자 해석. 잘못되면 usage */
static int positive_int_arg(const char *prog, const char *s)
{
    char *end;
    long v = strtol(s, &end, 10);

    if (*s == '\0' || *end != '\0' || v <= 0 || v > 100000) {
        usage(prog);
    }
    return (int)v;
}

static void config_parse(int argc, char **argv)
{
    if (argc < 2 || strncmp(argv[1], "--", 2) == 0) {
        usage(argv[0]);
    }
    cfg.port = argv[1]; /* 첫 번째 인자는 지금처럼 포트 (기존 기능 테스트 유지) */
    for (int i = 2; i < argc; i += 2) {
        if (i + 1 >= argc) {
            usage(argv[0]);
        }
        if (strcmp(argv[i], "--root") == 0) {
            cfg.root = argv[i + 1];
        } else if (strcmp(argv[i], "--threads") == 0) {
            cfg.threads = positive_int_arg(argv[0], argv[i + 1]);
        } else if (strcmp(argv[i], "--queue") == 0) {
            cfg.queue = positive_int_arg(argv[0], argv[i + 1]);
        } else if (strcmp(argv[i], "--timeout") == 0) {
            cfg.timeout = positive_int_arg(argv[0], argv[i + 1]);
        } else if (strcmp(argv[i], "--keepalive-max") == 0) {
            cfg.keepalive_max = positive_int_arg(argv[0], argv[i + 1]);
        } else if (strcmp(argv[i], "--proxy-timeout") == 0) {
            cfg.proxy_timeout = positive_int_arg(argv[0], argv[i + 1]);
        } else if (strcmp(argv[i], "--proxy") == 0) {
            const char *error = route_table_add(&routes, argv[i + 1]);
            if (error != NULL) {
                fprintf(stderr, "--proxy %s: %s\n", argv[i + 1], error);
                exit(1);
            }
        } else {
            usage(argv[0]);
        }
    }
}

/* ============================== M1 + M4: 소켓과 스레드 풀 ============================== */

/* 요청 하나를 마친 뒤 연결을 어떻게 할지 */
enum conn_next {
    CONN_KEEP,          /* 다음 요청을 기다림 */
    CONN_CLOSE_REPLIED, /* 응답을 다 보냄 → 남은 데이터를 비우고 닫음 */
    CONN_CLOSE_NOW,     /* 응답 없이, 또는 보내다 실패해서 바로 닫음 */
    CONN_RESET          /* 길이 없는 응답이 잘림 → RST로 끊어 알림 */
};

static int listen_socket_open(const char *port);
static int accept_cloexec(int listenfd);
static void socket_options_set(int connfd);
static void *worker_main(void *arg);
static enum conn_next serve_connection(int connfd);
static void connection_close_lingering(int connfd);
static void connection_reset_set(int connfd);

int main(int argc, char **argv)
{
    pthread_t *workers;
    const char *path;
    int listenfd;

    config_parse(argc, argv);

    /* --root: 정적 루트로 이동. 경로 해석은 Phase 1처럼 "." + URI */
    if (cfg.root != NULL && chdir(cfg.root) < 0) {
        perror("chdir");
        exit(1);
    }

    /* 이미 끊긴 클라이언트에게 쓰면 SIGPIPE로 서버가 죽음 → 무시하고 쓰기 실패로 처리 */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        exit(1);
    }

    /* 로그가 파일로 갈 때도 한 줄씩 바로 나가게 함 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* CGI 자식은 fork 뒤 async-signal-safe 함수만 써야 하므로 미리 계산 (final.md 2.6) */
    cfg.open_max = sysconf(_SC_OPEN_MAX);
    if (cfg.open_max <= 0 || cfg.open_max > 65536) {
        cfg.open_max = 65536;
    }
    path = getenv("PATH");
    snprintf(cfg.path_env, sizeof(cfg.path_env), "PATH=%s",
             path ? path : "/usr/bin:/bin");

    listenfd = listen_socket_open(cfg.port);
    if (listenfd < 0) {
        exit(1);
    }

    if (conn_queue_init(&queue, cfg.queue) < 0) {
        fprintf(stderr, "작업 큐를 만들지 못했습니다\n");
        exit(1);
    }
    workers = malloc(sizeof(pthread_t) * (size_t)cfg.threads);
    if (workers == NULL) {
        fprintf(stderr, "작업자 목록을 만들지 못했습니다\n");
        exit(1);
    }
    for (int i = 0; i < cfg.threads; i++) {
        int rc = pthread_create(&workers[i], NULL, worker_main, NULL);
        if (rc != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            exit(1);
        }
    }

    /* main은 accept만 함. 큐가 가득 차면 conn_queue_push 안에서 기다림 (503 없음) */
    while (1) {
        int connfd = accept_cloexec(listenfd);
        if (connfd < 0) {
            if (errno != EINTR && errno != ECONNABORTED) {
                perror("accept");
            }
            if (errno == EMFILE || errno == ENFILE) {
                /* fd가 모자라면 잠깐 쉬어 바쁜 반복을 막음 */
                struct timespec ts = {0, 10 * 1000 * 1000};
                nanosleep(&ts, NULL);
            }
            continue;
        }
        socket_options_set(connfd);
        if (conn_queue_push(&queue, connfd) < 0) {
            close(connfd); /* 종료 중 (M7) */
        }
    }
    /* 종료 처리(sigwait 스레드, pthread_join)는 M7에서 추가 */
}

/* 작업자: 큐에서 연결을 꺼내 끝날 때까지 처리 → 닫기 → 반복.
 * 락은 큐를 만질 때만 잡고, 요청 처리는 락 밖에서 함 */
static void *worker_main(void *arg)
{
    int connfd;

    (void)arg;
    while ((connfd = conn_queue_pop(&queue)) >= 0) {
        enum conn_next next = serve_connection(connfd);
        if (next == CONN_CLOSE_REPLIED) {
            connection_close_lingering(connfd);
        } else if (next == CONN_RESET) {
            connection_reset_set(connfd);
        }
        close(connfd);
    }
    return NULL;
}

/*
 * socket → setsockopt(SO_REUSEADDR) → bind → listen.
 * 리스닝 소켓에 FD_CLOEXEC를 걸어 CGI가 물려받지 않게 함 (final.md 2.6).
 * 작업자를 만들기 전에 부르므로 socket과 fcntl 사이에 fork하는 스레드가 없음.
 */
static int listen_socket_open(const char *port)
{
    struct sockaddr_in addr;
    int listenfd;
    int optval = 1;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("socket");
        return -1;
    }
    if (fcntl(listenfd, F_SETFD, FD_CLOEXEC) < 0) {
        perror("fcntl");
        close(listenfd);
        return -1;
    }
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval,
                   sizeof(optval)) < 0) {
        perror("setsockopt");
        close(listenfd);
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)atoi(port));
    if (bind(listenfd, (SA *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listenfd);
        return -1;
    }
    if (listen(listenfd, LISTENQ) < 0) {
        perror("listen");
        close(listenfd);
        return -1;
    }
    return listenfd;
}

/*
 * accept하면서 접속 소켓에 FD_CLOEXEC를 겁니다 (final.md 2.6 첫 겹).
 *   Linux: accept4(SOCK_CLOEXEC)로 원자적으로 설정
 *   macOS: accept 후 fcntl. 그 사이에 다른 작업자가 fork하는 틈은
 *          CGI 자식이 fd 3 이상을 모두 닫아서 막음 (두 번째 겹)
 */
static int accept_cloexec(int listenfd)
{
#if defined(__linux__)
    return accept4(listenfd, NULL, NULL, SOCK_CLOEXEC);
#else
    int connfd = accept(listenfd, NULL, NULL);
    if (connfd >= 0) {
        fcntl(connfd, F_SETFD, FD_CLOEXEC);
    }
    return connfd;
#endif
}

/*
 * 읽기 타임아웃(--timeout초)과 쓰기 타임아웃(2배)을 겁니다 (final.md 2.4).
 *   접속만 하고 보내지 않는 손님, keep-alive로 쉬는 연결, 응답을 받아 가지 않는
 *   손님이 작업자를 붙잡지 못하게 합니다. 시간이 지나면 read/write가 -1을
 *   돌려주고 rio 함수도 -1을 돌려주므로 요청이 응답 없이 끝납니다.
 *
 * TCP_NODELAY: 작은 데이터도 바로 보냅니다. 응답은 헤더와 본문을 따로 쓰는데,
 *   이 옵션이 없으면 커널이 헤더의 ACK가 올 때까지 본문을 붙잡고(Nagle) 손님은
 *   ACK를 약 40ms 늦게 보내서, keep-alive 요청마다 그만큼 느려집니다.
 */
static void socket_options_set(int connfd)
{
    struct timeval rcv = {cfg.timeout, 0};
    struct timeval snd = {cfg.timeout * 2, 0};
    int on = 1;

    setsockopt(connfd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
    setsockopt(connfd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
}

/* ============================== 응답 쓰기 ============================== */

/*
 * 응답 헤더를 쓰기 직전에 부릅니다. 큐에서 기다리는 손님이 있으면 keep-alive를
 * 포기합니다. 작업자보다 많은 손님이 왔을 때, 먼저 온 연결들이 쉬지 않고 요청하면
 * 나중에 온 손님이 요청 수 상한(100건)까지 기다리게 되기 때문입니다.
 */
static void conn_next_yield(enum conn_next *next)
{
    if (*next == CONN_KEEP && conn_queue_has_waiting(&queue)) {
        *next = CONN_CLOSE_REPLIED;
    }
}

static int write_all(int fd, const void *buf, size_t n)
{
    return rio_writen(fd, (void *)buf, n) == (ssize_t)n ? 0 : -1;
}

/*
 * 응답 헤더를 씁니다 (final.md 0장 "응답 형식").
 *   상태 줄 HTTP/1.1, 모든 응답에 Content-Length와 Connection.
 *   content_type이 NULL이면 Content-Type 줄을 쓰지 않음 (CGI는 extra에 들어 있음).
 *   extra: "이름: 값\r\n" 형식의 추가 헤더 (없으면 NULL)
 *   length가 -1이면 Content-Length 줄을 쓰지 않음 (본문이 없는 204, 304)
 */
static int response_head_send(int fd, int status, const char *reason,
                              const char *content_type, const char *extra,
                              long length, enum conn_next *next)
{
    char header[MAXBUF * 2];
    char type_line[HTTP_FIELD_MAX + 32] = "";
    char length_line[48] = "";
    int n;

    conn_next_yield(next);
    if (length >= 0) {
        snprintf(length_line, sizeof(length_line), "Content-Length: %ld\r\n",
                 length);
    }

    if (content_type != NULL) {
        snprintf(type_line, sizeof(type_line), "Content-Type: %s\r\n",
                 content_type);
    }
    n = snprintf(header, sizeof(header),
                 "HTTP/1.1 %d %s\r\n"
                 "Server: Tiny Web Server\r\n"
                 "%s%s%s"
                 "Connection: %s\r\n\r\n",
                 status, reason ? reason : http_status_text(status), type_line,
                 extra ? extra : "", length_line,
                 *next == CONN_KEEP ? "keep-alive" : "close");
    if (n < 0 || n >= (int)sizeof(header)) {
        return -1;
    }
    return write_all(fd, header, (size_t)n);
}

/*
 * 오류 응답 (Phase 1의 http_error_send에 Content-Length, Connection 추가).
 * HEAD 요청이면 본문을 빼고 보냄. 쓰기가 실패하면 *next를 바로 닫기로.
 * 보낸 상태 코드를 돌려줌 (로그용).
 */
static int error_reply(int fd, const struct http_request *req, int status,
                       const char *cause, const char *longmsg, enum conn_next *next)
{
    char body[MAXBUF];
    int body_len;
    int send_body = !(req != NULL && strcmp(req->method, "HEAD") == 0);

    body_len = snprintf(body, sizeof(body),
                        "<html><title>Tiny Error</title>"
                        "<body bgcolor=ffffff>\r\n"
                        "%d: %s\r\n"
                        "<p>%s: %s\r\n"
                        "<hr><em>The Tiny Web server</em>\r\n",
                        status, http_status_text(status), longmsg, cause);
    if (body_len >= (int)sizeof(body)) {
        body_len = sizeof(body) - 1;
    }
    if (response_head_send(fd, status, NULL, "text/html", NULL, body_len,
                           next) < 0 ||
        (send_body && write_all(fd, body, (size_t)body_len) < 0)) {
        *next = CONN_CLOSE_NOW;
    }
    return status;
}

/* 요청마다 한 줄 로그. printf 한 번이라 스레드끼리 한 줄 안에서는 섞이지 않음.
 * note: 뒤에 붙일 글 (백엔드 이름 등, 없으면 "") */
static void request_log(const struct http_request *req, int status,
                        const char *note)
{
    printf("%s %s %s %d%s\n", req->method[0] ? req->method : "-",
           req->uri[0] ? req->uri : "-", req->version[0] ? req->version : "-",
           status, note);
}

/* ============================== M5: 연결과 요청 처리 ============================== */

static long now_ms(void);
static int request_wait(int fd, rio_t *rio);
static enum conn_next request_handle(int fd, rio_t *rio, int can_keep);
static int request_route(int fd, struct http_request *req, enum conn_next *next,
                         char *note, size_t note_size);

/*
 * 연결 하나를 처리합니다 (final.md 5.1).
 *   rio는 연결당 한 번만 준비함. 파이프라이닝으로 다음 요청 앞부분이 이미
 *   rio 버퍼에 들어와 있을 수 있어서, 요청마다 새로 만들면 그 데이터를 잃음.
 *   이 연결의 요청 수가 --keepalive-max에 닿으면 그 응답에 Connection: close.
 */
static enum conn_next serve_connection(int connfd)
{
    rio_t rio;
    enum conn_next next = CONN_KEEP;

    rio_readinitb(&rio, connfd);
    rio.rio_idle_ms = cfg.timeout * 1000;
    for (int n = 1; next == CONN_KEEP; n++) {
        if (!request_wait(connfd, &rio)) {
            return CONN_CLOSE_NOW;
        }
        next = request_handle(connfd, &rio, n < cfg.keepalive_max);
    }
    return next;
}

/*
 * 다음 요청의 첫 바이트를 기다립니다. 왔으면 1, --timeout 안에 안 오면 0.
 * 파이프라이닝한 다음 요청은 이미 rio 버퍼에 있고 소켓은 비어 있을 수 있으므로
 * 버퍼를 먼저 봅니다. 끊긴 연결도 1이고, 이어지는 읽기가 끊긴 것을 알아냅니다.
 */
static int request_wait(int fd, rio_t *rio)
{
    long deadline = now_ms() + cfg.timeout * 1000L;

    if (rio->rio_cnt > 0) {
        return 1;
    }
    while (1) {
        struct pollfd p = {fd, POLLIN, 0};
        long left = deadline - now_ms();
        int r = left > 0 ? poll(&p, 1, (int)left) : 0;

        if (r < 0 && errno == EINTR) {
            continue;
        }
        return r > 0;
    }
}

/*
 * 응답을 보낸 뒤 연결을 닫을 때, 클라이언트가 보낸 데이터 중 서버가 읽지 않은
 * 부분이 소켓에 남아 있으면(아주 긴 요청 줄, 오류 뒤에 이어 보낸 요청 등)
 * close가 FIN 대신 RST를 보냅니다. 그러면 클라이언트가 방금 보낸 오류 응답을
 * 읽기 전에 연결이 끊겨 응답을 잃을 수 있습니다 (macOS CI에서 414가 이렇게 실패).
 *
 * 그래서 먼저 shutdown(SHUT_WR)으로 "더 보낼 것 없음(FIN)"을 알리고,
 * 클라이언트가 닫을 때까지(EOF) 남은 데이터를 읽어 버린 뒤 닫습니다.
 * 상대가 계속 보내거나 닫지 않아도 최대 1초, 1 MiB까지만 기다립니다.
 */
static void connection_close_lingering(int connfd)
{
    char buf[4096];
    long deadline = now_ms() + 1000;
    size_t drained = 0;

    if (shutdown(connfd, SHUT_WR) < 0) {
        return;
    }
    while (drained < (size_t)BODY_MAX) {
        long left = deadline - now_ms();
        struct pollfd p = {connfd, POLLIN, 0};
        ssize_t n;
        int r;

        if (left <= 0) {
            break;
        }
        r = poll(&p, 1, (int)left);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            break;
        }
        n = read(connfd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break; /* 클라이언트가 닫음(EOF) 또는 오류 */
        }
        drained += (size_t)n;
    }
}

/*
 * 다음 close가 FIN 대신 RST를 보내게 합니다. Content-Length가 없는 응답은 FIN이
 * 곧 "끝"이라, 잘린 응답을 그냥 닫으면 손님이 완성된 응답으로 받습니다.
 * shutdown(SHUT_WR)을 먼저 부르면 FIN이 나가 버리므로 비우기와 함께 쓰지 않음.
 */
static void connection_reset_set(int connfd)
{
    struct linger linger = {1, 0};

    setsockopt(connfd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
}

/*
 * 요청 하나를 읽고 응답합니다. 반환은 enum conn_next.
 * 한 줄 규칙: 요청을 끝까지 읽었고 응답 길이를 알면 유지, 아니면 닫음 (5.2)
 *
 * 첫 바이트가 온 뒤 헤더 전체는 --timeout의 2배, 본문 전체는 6배 안에 받아야
 * 합니다(기본 10초, 30초). 그 안에서도 --timeout 동안 아무것도 안 오면 끝냅니다.
 * 시간이 지나면 rio 함수가 -1을 돌려주므로 응답 없이 닫습니다.
 */
static enum conn_next request_handle(int fd, rio_t *rio, int can_keep)
{
    struct http_request req;
    char line[MAXLINE];
    char note[BACKEND_NAME_MAX + 32] = "";
    ssize_t n;
    long body_len;
    int status;
    enum conn_next next = CONN_CLOSE_REPLIED;

    http_request_init(&req);
    rio->rio_deadline = now_ms() + cfg.timeout * 2000L;

    /* 1. 요청 줄. 0(클라이언트가 끊음)이나 -1(읽기 타임아웃)이면 조용히 닫음 */
    n = rio_readlineb(rio, line, MAXLINE);
    if (n <= 0) {
        return CONN_CLOSE_NOW;
    }
    status = http_parse_request_line(&req, line, (size_t)n, MAXLINE);
    if (status != 0) { /* 414, 400, 505, 501: 본문 경계를 모르므로 닫음 */
        error_reply(fd, &req, status, "request line", "Bad request line",
                    &next);
        request_log(&req, status, "");
        return next;
    }

    /* 2. 헤더: 빈 줄까지 "이름: 값" → 구조체 + 원본 블록 */
    while (1) {
        n = rio_readlineb(rio, line, MAXLINE);
        if (n <= 0) {
            return CONN_CLOSE_NOW; /* 헤더 도중 끊김, 타임아웃 */
        }
        status = http_parse_header_line(&req, line, (size_t)n, MAXLINE);
        if (status == 1) {
            break;
        }
        if (status != 0) {
            error_reply(fd, &req, status, "header", "Bad header", &next);
            request_log(&req, status, "");
            return next;
        }
    }

    /* 3. 헤더 검사: Host, CL+TE, TE, 413, 417 */
    status = http_finish_headers(&req);
    if (status != 0) {
        error_reply(fd, &req, status, req.uri, "Request rejected", &next);
        request_log(&req, status, "");
        return next;
    }
    body_len = http_body_length(&req);

    /* 4. Expect: 100-continue면 본문을 받기 전에 계속 보내라고 알림 (3.6) */
    if (req.expect == HTTP_EXPECT_CONTINUE && body_len > 0) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (write_all(fd, cont, sizeof(cont) - 1) < 0) {
            return CONN_CLOSE_NOW;
        }
    }

    /* 5. 본문: 헤더를 읽은 같은 rio로 정확히 CL바이트 (GET에 붙어 와도 읽고 버림) */
    if (body_len > 0) {
        req.body = malloc((size_t)body_len);
        if (req.body == NULL) {
            error_reply(fd, &req, 500, req.uri, "Out of memory", &next);
            request_log(&req, 500, "");
            return next;
        }
        rio->rio_deadline = now_ms() + cfg.timeout * 6000L;
        n = rio_readnb(rio, req.body, (size_t)body_len);
        if (n != (ssize_t)body_len) { /* 도중에 끊김, 타임아웃: 응답 없이 닫음 */
            http_request_free(&req);
            return CONN_CLOSE_NOW;
        }
        req.body_len = (size_t)body_len;
    }

    /* 6. 경로 확인 → 정적 파일 / CGI → 응답 */
    if (req.keep_alive && can_keep) {
        next = CONN_KEEP;
    }
    status = request_route(fd, &req, &next, note, sizeof(note));
    request_log(&req, status, note);
    http_request_free(&req);
    return next;
}

/* ============================== M2: 경로 확인 (Phase 1에서 이어짐) ============================== */

/* URI가 가리키는 것 (uri_parse의 결과) */
enum uri_target {
    URI_STATIC,    /* 정적 파일 */
    URI_CGI,       /* CGI 프로그램 */
    URI_FORBIDDEN, /* 허용하지 않는 경로 → 403 */
    URI_TOO_LONG   /* 파일 경로가 버퍼보다 김 → 414 */
};

static int path_is_allowed(const char *path, size_t len, int is_local);
static enum uri_target uri_parse(const char *uri, char *filename,
                                 char *cgiargs);
static int static_serve(int fd, const struct http_request *req,
                        const char *filename, off_t filesize, enum conn_next *next);
static int cgi_serve(int fd, const struct http_request *req,
                     const char *filename, const char *cgiargs, enum conn_next *next);
static int proxy_serve(int fd, const struct http_request *req,
                       struct route *route, enum conn_next *next, char *note,
                       size_t note_size);

/* 요청을 백엔드 / 정적 파일 / CGI로 보냄. 보낸 상태 코드를 돌려줌 */
static int request_route(int fd, struct http_request *req, enum conn_next *next,
                         char *note, size_t note_size)
{
    char filename[MAXLINE], cgiargs[MAXLINE];
    size_t path_len = strcspn(req->uri, "?");
    struct stat sbuf;
    struct route *route;
    enum uri_target target;

    /* ".."은 어디로 가는 요청이든 403. 규칙의 접두사를 벗어나는 것을 막음 */
    if (!path_is_allowed(req->uri, path_len, 0)) {
        return error_reply(fd, req, 403, req->uri,
                           "Tiny doesn't serve this path", next);
    }
    /* 라우팅 규칙에 걸리면 백엔드로, 아니면 아래에서 직접 처리 */
    route = route_table_find(&routes, req->uri, path_len);
    if (route != NULL) {
        return proxy_serve(fd, req, route, next, note, note_size);
    }

    target = uri_parse(req->uri, filename, cgiargs);
    if (target == URI_FORBIDDEN) {
        return error_reply(fd, req, 403, req->uri,
                           "Tiny doesn't serve this path", next);
    }
    if (target == URI_TOO_LONG) {
        *next = CONN_CLOSE_REPLIED;
        return error_reply(fd, req, 414, req->uri, "Path too long", next);
    }
    if (stat(filename, &sbuf) < 0) {
        return error_reply(fd, req, 404, filename,
                           "Tiny couldn't find this file", next);
    }

    if (target == URI_STATIC) {
        if (strcmp(req->method, "POST") == 0) { /* 정적 파일 POST: 501 유지 */
            return error_reply(fd, req, 501, req->method,
                               "Tiny does not implement this method", next);
        }
        if (!S_ISREG(sbuf.st_mode) || !(S_IRUSR & sbuf.st_mode)) {
            return error_reply(fd, req, 403, filename,
                               "Tiny couldn't read the file", next);
        }
        return static_serve(fd, req, filename, sbuf.st_size, next);
    }

    if (strcmp(req->method, "HEAD") == 0) { /* CGI에 HEAD: 501 */
        return error_reply(fd, req, 501, req->method,
                           "Tiny does not implement HEAD for CGI", next);
    }
    if (!S_ISREG(sbuf.st_mode) || !(S_IXUSR & sbuf.st_mode)) {
        return error_reply(fd, req, 403, filename,
                           "Tiny couldn't run the CGI program", next);
    }
    return cgi_serve(fd, req, filename, cgiargs, next);
}

/*
 * 경로(쿼리 제외)를 '/'로 나눈 조각을 검사합니다.
 *   "..": 루트 밖으로 나가거나 라우팅 접두사를 벗어남
 *   is_local이면 "."과 중간의 빈 조각("//")도 거절: "/./cgi-bin/x"처럼 첫 조각이
 *     cgi-bin이 아닌 척하면 CGI 프로그램 파일이 정적 파일로 내려감
 *   끝의 '/'는 허용 (기본 파일 home.html)
 * URL 표기만 봅니다. 루트 안의 심볼릭 링크가 어디를 가리키는지는 보지 않습니다.
 */
static int path_is_allowed(const char *path, size_t len, int is_local)
{
    size_t start = 1; /* path[0]은 '/' */

    for (size_t i = 1; i <= len; i++) {
        if (i < len && path[i] != '/') {
            continue;
        }
        size_t seg = i - start;
        if (seg == 2 && path[start] == '.' && path[start + 1] == '.') {
            return 0;
        }
        if (is_local &&
            ((seg == 0 && i < len) || (seg == 1 && path[start] == '.'))) {
            return 0;
        }
        start = i + 1;
    }
    return 1;
}

/*
 * URI에서 쿼리를 떼고 정적/CGI로 나눈 뒤 파일 경로와 CGI 인자를 만듭니다.
 *   첫 조각이 정확히 "cgi-bin"이면 CGI. 대소문자만 다르면 거절 (대소문자를
 *   구분하지 않는 macOS 파일 시스템에서 프로그램 파일이 내려가는 것을 막음).
 *   URI는 바꾸지 않음 (로그에 원본 유지).
 */
static enum uri_target uri_parse(const char *uri, char *filename,
                                 char *cgiargs)
{
    size_t path_len = strcspn(uri, "?");
    size_t first_len = strcspn(uri + 1, "/?");
    const char *suffix = "";
    enum uri_target target = URI_STATIC;
    int n;

    cgiargs[0] = '\0';
    if (!path_is_allowed(uri, path_len, 1)) {
        return URI_FORBIDDEN;
    }
    if (first_len == 7 && strncasecmp(uri + 1, "cgi-bin", 7) == 0) {
        if (strncmp(uri + 1, "cgi-bin", 7) != 0) {
            return URI_FORBIDDEN;
        }
        target = URI_CGI;
        if (uri[path_len] == '?') {
            snprintf(cgiargs, MAXLINE, "%s", uri + path_len + 1);
        }
    } else if (uri[path_len - 1] == '/') {
        suffix = "home.html";
    }
    n = snprintf(filename, MAXLINE, ".%.*s%s", (int)path_len, uri, suffix);
    return (n < 0 || n >= MAXLINE) ? URI_TOO_LONG : target;
}

/*
 * 정적 파일 응답. 파일을 준비(open, mmap)한 뒤 200을 보냄.
 * HEAD면 같은 헤더(Content-Length 포함)만 보내고 본문은 뺌 (3.8).
 * MIME은 마지막 확장자로 판단 (3.7).
 */
static int static_serve(int fd, const struct http_request *req,
                        const char *filename, off_t filesize, enum conn_next *next)
{
    int srcfd;
    char *srcp = NULL;
    int send_body = strcmp(req->method, "HEAD") != 0;

    srcfd = open(filename, O_RDONLY, 0);
    if (srcfd < 0) {
        return error_reply(fd, req, 500, filename,
                           "Tiny couldn't open the file", next);
    }
    if (filesize > 0 && send_body) {
        srcp = mmap(0, (size_t)filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
        if (srcp == MAP_FAILED) {
            close(srcfd);
            return error_reply(fd, req, 500, filename,
                               "Tiny couldn't map the file", next);
        }
    }
    close(srcfd);

    if (response_head_send(fd, 200, NULL, http_mime_type(filename), NULL,
                           (long)filesize, next) < 0 ||
        (srcp != NULL && write_all(fd, srcp, (size_t)filesize) < 0)) {
        *next = CONN_CLOSE_NOW; /* 응답을 보내다 실패하면 바로 닫음 */
    }
    if (srcp != NULL) {
        munmap(srcp, (size_t)filesize);
    }
    return 200;
}

/* ============================== M5: CGI (final.md 4장) ============================== */

enum cgi_result { CGI_OK, CGI_FAILED, CGI_TIMEOUT };

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * CGI 입력: POST면 본문을 담은 임시 파일, 그 외에는 /dev/null.
 *   mkstemp 직후 unlink해서 이름은 사라지고 fd만 남음.
 *   서버가 중간에 죽어도 임시 파일이 디스크에 남지 않음.
 *   부모는 파이프에 쓰지 않으므로 "부모는 쓰기에서, CGI는 출력에서" 서로
 *   기다리는 교착이 생기지 않음 (4.2).
 */
static int cgi_input_open(const struct http_request *req)
{
    char tmpl[] = "/tmp/tiny-cgi-XXXXXX";
    int fd;

    if (strcmp(req->method, "POST") != 0) {
        return open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
    fd = mkstemp(tmpl);
    if (fd < 0) {
        return -1;
    }
    unlink(tmpl);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if ((req->body_len > 0 && write_all(fd, req->body, req->body_len) < 0) ||
        lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * 자식에서 fd 3 이상을 모두 닫습니다 (final.md 2.6 두 번째 겹).
 *   다른 작업자가 맡은 손님 소켓, 리스닝 소켓을 CGI가 물려받지 않게 함.
 *   fork 뒤라 async-signal-safe 함수(close)만 씀. 범위는 부모가 미리 계산.
 */
static void child_close_fds(void)
{
#if defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, 3U, ~0U, 0U) == 0) {
        return; /* 한 번에 닫기 (Linux 5.9 이상) */
    }
#endif
    for (long i = 3; i < cfg.open_max; i++) {
        close((int)i);
    }
}

/*
 * CGI 출력을 파이프에서 끝까지 모읍니다.
 *   마감 시간(시작 + 10초)까지 poll로 기다리며 읽음.
 *   1 MiB 초과 → kill → FAILED(500), 시간 초과 → kill → TIMEOUT(504).
 *   EOF 뒤에도 CGI가 계속 돌 수 있어 waitpid(WNOHANG)를 마감까지 반복 (4.3).
 *   성공하면 *out에 malloc한 출력 (호출한 쪽이 free).
 */
static int cgi_output_collect(pid_t pid, int outfd, char **out,
                              size_t *out_len)
{
    long deadline = now_ms() + CGI_TIMEOUT_SEC * 1000L;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    int result = CGI_OK;
    int wstatus = 0;

    if (buf == NULL) {
        result = CGI_FAILED;
        goto kill_child;
    }

    while (1) { /* 출력 모으기 */
        long left = deadline - now_ms();
        struct pollfd p = {outfd, POLLIN, 0};
        ssize_t n;
        int r;

        if (left <= 0) {
            result = CGI_TIMEOUT;
            goto kill_child;
        }
        r = poll(&p, 1, (int)left);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r < 0) {
            result = CGI_FAILED;
            goto kill_child;
        }
        if (r == 0) {
            result = CGI_TIMEOUT;
            goto kill_child;
        }
        if (len == cap) { /* 상한 + 1바이트까지 늘려서 초과를 알아냄 */
            size_t ncap = cap * 2;
            char *nbuf;
            if (ncap > (size_t)CGI_OUT_MAX + 1) {
                ncap = (size_t)CGI_OUT_MAX + 1;
            }
            nbuf = realloc(buf, ncap);
            if (nbuf == NULL) {
                result = CGI_FAILED;
                goto kill_child;
            }
            buf = nbuf;
            cap = ncap;
        }
        n = read(outfd, buf + len, cap - len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            result = CGI_FAILED;
            goto kill_child;
        }
        if (n == 0) {
            break; /* EOF */
        }
        len += (size_t)n;
        if (len > (size_t)CGI_OUT_MAX) {
            result = CGI_FAILED;
            goto kill_child;
        }
    }

    while (1) { /* EOF 뒤: 끝날 때까지 마감 안에서 기다림 */
        pid_t w = waitpid(pid, &wstatus, WNOHANG);
        if (w == pid) {
            break;
        }
        if (w < 0 && errno != EINTR) {
            result = CGI_FAILED;
            goto done;
        }
        if (now_ms() >= deadline) {
            result = CGI_TIMEOUT;
            goto kill_child;
        }
        struct timespec ts = {0, 5 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        result = CGI_FAILED; /* exec 실패(127), 0이 아닌 종료, 시그널로 죽음 */
    }
    goto done;

kill_child:
    kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }

done:
    if (result == CGI_OK) {
        *out = buf;
        *out_len = len;
    } else {
        free(buf);
    }
    return result;
}

/*
 * CGI의 "Status: 404 Not Found" 값을 해석합니다. 성공 0, 실패 -1.
 * 1xx는 중간 응답이라 최종 응답이 될 수 없으므로 실패로 봅니다.
 */
static int cgi_status_parse(const struct http_header *h, int *status,
                            char *reason, size_t cap)
{
    const char *v = h->value;
    const char *end = h->value + h->value_len;

    if (h->value_len < 3 || !isdigit((unsigned char)v[0]) ||
        !isdigit((unsigned char)v[1]) || !isdigit((unsigned char)v[2])) {
        return -1;
    }
    *status = (v[0] - '0') * 100 + (v[1] - '0') * 10 + (v[2] - '0');
    if (*status < 200 || *status > 599) {
        return -1;
    }
    v += 3;
    while (v < end && *v == ' ') {
        v++;
    }
    snprintf(reason, cap, "%.*s", (int)(end - v), v);
    if (reason[0] == '\0') {
        snprintf(reason, cap, "%s", http_status_text(*status));
    }
    return 0;
}

/*
 * CGI 출력("헤더 몇 줄 + 빈 줄 + 본문")을 나눠 응답을 만듭니다 (4.4).
 *   Status: 404 Not Found가 있으면 그 상태, 없으면 200
 *   Content-Length, Connection은 CGI 값을 무시하고 서버가 직접 씀
 *   한 구간 전용 헤더(Keep-Alive 등)는 빼고 나머지는 그대로 전달
 *   204, 304는 본문이 없는 상태라 본문과 Content-Length를 보내지 않음
 *   빈 줄이 없거나, 출력이 비었거나, 헤더 형식이 틀리거나, 1xx 상태거나,
 *   Transfer-Encoding이 있으면 500. (이 헤더만 떼고 보내면 CGI가 이미 chunked로
 *   나눠 쓴 본문을 일반 본문인 것처럼 내보낼 수 있음)
 */
static int cgi_reply(int fd, const struct http_request *req, const char *out,
                     size_t len, enum conn_next *next)
{
    static const char *const skip[] = {"Status", "Content-Length", NULL};
    char extra[MAXBUF];
    char reason[64] = "OK";
    struct http_header h;
    size_t hdr_end = 0, body_start = 0, pos = 0;
    long extra_len, body_len;
    int status = 200;
    int found = 0, rc;

    for (size_t i = 0; i < len; i++) { /* 헤더와 본문을 나누는 빈 줄 찾기 */
        if (i + 1 < len && out[i] == '\n' && out[i + 1] == '\n') {
            hdr_end = i > 0 ? i + 1 : 0; /* 빈 줄로 시작하면 헤더가 없음 */
            body_start = i + 2;
            found = 1;
            break;
        }
        if (i + 3 < len && memcmp(out + i, "\r\n\r\n", 4) == 0) {
            hdr_end = i > 0 ? i + 2 : 0;
            body_start = i + 4;
            found = 1;
            break;
        }
    }
    if (len == 0 || !found) {
        return error_reply(fd, req, 500, req->uri,
                           "CGI output has no header", next);
    }

    while ((rc = http_header_next(out, hdr_end, &pos, &h)) == 1) {
        if (http_header_is(&h, "Transfer-Encoding")) {
            return error_reply(fd, req, 500, req->uri,
                               "CGI output has Transfer-Encoding", next);
        }
        if (http_header_is(&h, "Status") &&
            cgi_status_parse(&h, &status, reason, sizeof(reason)) < 0) {
            return error_reply(fd, req, 500, req->uri,
                               "Malformed CGI status", next);
        }
    }
    extra_len = http_headers_filter(out, hdr_end, skip, extra,
                                    sizeof(extra) - 1);
    if (rc < 0 || extra_len < 0) {
        return error_reply(fd, req, 500, req->uri, "Malformed CGI header",
                           next);
    }
    extra[extra_len] = '\0';

    body_len = (long)(len - body_start);
    if (status == 204 || status == 304) {
        body_len = 0;
    }
    if (response_head_send(fd, status, reason, NULL, extra,
                           (status == 204 || status == 304) ? -1 : body_len,
                           next) < 0 ||
        (body_len > 0 &&
         write_all(fd, out + body_start, (size_t)body_len) < 0)) {
        *next = CONN_CLOSE_NOW;
    }
    return status;
}

/*
 * CGI 실행 (final.md 4.3)
 *   fork 전(부모): envp, 입력 fd, 출력 파이프 준비
 *   자식: dup2(입력 → stdin, 파이프 → stdout) → fd 3 이상 close → execve
 *         (멀티스레드 fork 뒤라 setenv, malloc을 쓰지 않음)
 *   부모: 출력을 모아(1 MiB, 10초) 응답 생성. 실패 500, 시간 초과 504
 */
static int cgi_serve(int fd, const struct http_request *req,
                     const char *filename, const char *cgiargs, enum conn_next *next)
{
    char env_method[32], env_query[MAXLINE + 16], env_length[48];
    char env_type[HTTP_FIELD_MAX + 16], env_protocol[48];
    char *envp[10];
    char *argv[2];
    int k = 0;
    int infd, pipefd[2];
    pid_t pid;
    char *out = NULL;
    size_t out_len = 0;
    int result, status;

    /* 1. envp: 부모가 fork 전에 만들어 둠 (2.5) */
    snprintf(env_method, sizeof(env_method), "REQUEST_METHOD=%s",
             req->method);
    snprintf(env_query, sizeof(env_query), "QUERY_STRING=%s", cgiargs);
    snprintf(env_protocol, sizeof(env_protocol), "SERVER_PROTOCOL=%s",
             req->version);
    envp[k++] = env_method;
    envp[k++] = env_query;
    envp[k++] = env_protocol;
    envp[k++] = "GATEWAY_INTERFACE=CGI/1.1";
    envp[k++] = cfg.path_env;
    if (strcmp(req->method, "POST") == 0) {
        snprintf(env_length, sizeof(env_length), "CONTENT_LENGTH=%zu",
                 req->body_len);
        envp[k++] = env_length;
        if (req->content_type[0] != '\0') {
            snprintf(env_type, sizeof(env_type), "CONTENT_TYPE=%s",
                     req->content_type);
            envp[k++] = env_type;
        }
    }
    envp[k] = NULL;
    argv[0] = (char *)filename;
    argv[1] = NULL;

    /* 2. 입력 (임시 파일 또는 /dev/null), 출력 파이프 */
    infd = cgi_input_open(req);
    if (infd < 0) {
        return error_reply(fd, req, 500, filename, "CGI input failed", next);
    }
    if (pipe(pipefd) < 0) {
        close(infd);
        return error_reply(fd, req, 500, filename, "CGI pipe failed", next);
    }
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    /* 3. fork */
    pid = fork();
    if (pid < 0) {
        close(infd);
        close(pipefd[0]);
        close(pipefd[1]);
        return error_reply(fd, req, 500, filename, "CGI fork failed", next);
    }
    if (pid == 0) { /* 자식: async-signal-safe 함수만 */
        if (dup2(infd, STDIN_FILENO) < 0 ||
            dup2(pipefd[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        child_close_fds();
        execve(filename, argv, envp);
        _exit(127); /* execve 실패 → 부모가 500 */
    }

    /* 4. 부모: 자식이 쓰는 쪽을 닫아야 자식이 끝날 때 EOF를 받음 */
    close(infd);
    close(pipefd[1]);
    result = cgi_output_collect(pid, pipefd[0], &out, &out_len);
    close(pipefd[0]);

    if (result == CGI_TIMEOUT) {
        return error_reply(fd, req, 504, filename,
                           "CGI program did not finish in time", next);
    }
    if (result == CGI_FAILED) {
        return error_reply(fd, req, 500, filename, "CGI program failed",
                           next);
    }
    status = cgi_reply(fd, req, out, out_len, next);
    free(out);
    return status;
}

/* ============================== M6: Reverse Proxy ============================== */

/*
 * 규칙에 걸린 요청을 백엔드로 보내고 응답을 전달합니다.
 *   응답을 쓰기 전의 실패(proxy_open)는 502, 504 오류 페이지로 답할 수 있고,
 *   쓰기 시작한 뒤의 실패(proxy_relay)는 연결을 닫는 수밖에 없음.
 */
static int proxy_serve(int fd, const struct http_request *req,
                       struct route *route, enum conn_next *next, char *note,
                       size_t note_size)
{
    struct proxy p;
    const char *cut = "";
    int status, keep;

    status = proxy_open(&p, req, route->backends, route->backend_count,
                        route_next_backend(route), cfg.proxy_timeout);
    if (status != 0) {
        snprintf(note, note_size, " -> %s", p.backend->name);
        return error_reply(fd, req, status, req->uri,
                           "Tiny couldn't get a response from the backend",
                           next);
    }
    status = p.resp.status;
    conn_next_yield(next);
    keep = *next == CONN_KEEP;

    switch (proxy_relay(&p, fd, &keep)) {
    case PROXY_DONE:
        *next = keep ? CONN_KEEP : CONN_CLOSE_REPLIED;
        break;
    case PROXY_ABORTED:
        *next = CONN_CLOSE_NOW;
        cut = " (중단)";
        break;
    case PROXY_ABORTED_UNKNOWN_LENGTH:
        *next = CONN_RESET;
        cut = " (중단)";
        break;
    }
    snprintf(note, note_size, " -> %s%s", p.backend->name, cut);
    proxy_close(&p);
    return status;
}
