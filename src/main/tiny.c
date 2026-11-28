/*
 * tiny.c - 정적 파일과 CGI를 처리하고, 규칙에 걸린 요청은 백엔드로 중계하는
 *          웹 서버입니다.
 *   Phase 1: CS:APP Tiny의 각 부분을 같은 동작의 우리 코드로 교체 (M1~M3)
 *   Phase 2: docs/prd/m4m5.md의 M4 동시성, M5 HTTP 완성
 *            docs/prd/m6m7.md의 M6 Reverse Proxy(src/proxy.c), M7 안정성
 *
 * 사용법:
 *   ./build/tiny <port> [--root DIR] [--threads N] [--queue N]
 *                       [--timeout 초] [--keepalive-max N]
 *                       [--route 접두사=이름:포트,이름:포트]... [--proxy-timeout 초]
 *
 * 요청 하나의 흐름 (m6m7.md 4장)
 *   [main]   poll(리스닝 소켓, 종료 파이프) → accept → 작업 큐에 넣기
 *   [작업자] 큐에서 꺼내기 → 요청 반복 처리 → close
 *   요청 하나: 요청 줄·헤더(마감 10초) → 요청 검사 → (Expect) → 본문(마감 30초)
 *             → 경로 검사(403) → 라우팅 → 백엔드 중계 / 정적 파일 / CGI
 *             → keep-alive 판단
 *   [시그널 스레드] SIGINT·SIGTERM → 종료 파이프 → 큐 종료. 두 번째 신호는 즉시 종료
 */
#if defined(__linux__)
/* syscall(SYS_close_range)용. _GNU_SOURCE는 csapp.h의 gai_error와 충돌해서 쓰지 않음 */
#define _DEFAULT_SOURCE
#endif

#include "csapp.h"
#include "conn_queue.h"
#include "http.h"
#include "proxy.h"

#include <netinet/tcp.h>
#include <poll.h>
#include <stdatomic.h>
#include <strings.h>
#include <time.h>
#if defined(__linux__)
#include <sys/syscall.h>
/* glibc는 accept4를 _GNU_SOURCE에서만 선언하므로 직접 선언 (함수는 glibc에 있음) */
int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags);
#endif

#define CGI_TIMEOUT_SEC 10     /* CGI 실행 시간 제한 (m4m5.md 2.4) */
#define CGI_OUT_MAX (1L << 20) /* CGI 출력 상한 1 MiB (m4m5.md 4.4) */
#define HEADER_DEADLINE_MULT 2 /* 요청 줄·헤더 마감 = --timeout의 2배 (M7-4) */
#define BODY_DEADLINE_MULT 6   /* 본문 마감 = --timeout의 6배 (M7-4) */

/* ============================== 설정과 공유 상태 ============================== */

/* 실행 인자로 정하는 값. main이 작업자를 만들기 전에 채우고 이후에는 읽기만 함 */
struct server_config {
    const char *port;
    int port_num;
    const char *root;  /* NULL이면 현재 폴더 (Phase 1 동작 유지) */
    int threads;       /* 작업자 수 */
    int queue;         /* 작업 큐 크기 */
    int timeout;       /* 읽기 타임아웃(초). 쓰기는 2배 */
    int keepalive_max; /* 연결당 요청 수 상한 */
    int proxy_timeout; /* 백엔드 응답 헤더까지 합계, 본문 데이터 간격(초) */
    long open_max;     /* CGI 자식에서 fd를 닫을 범위 (fork 전에 계산) */
    char *path_env;    /* CGI에 넘길 PATH=... (시작할 때 길이만큼 할당, F6) */
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

/* 라우팅 규칙. 시작할 때 채우고 이후에는 Round-Robin 순번(atomic)만 바뀜 */
static struct proxy_table routes;

/*
 * 종료 처리 (M7-5, M7-6)
 *   stopping: 첫 종료 신호를 받으면 1. 응답 헤더를 쓸 때 보고 Connection: close
 *   cleanup_done: main이 작업자를 모두 join하면 1. 시그널 스레드를 깨울 때 씀
 *   wake_pipe: 종료 알림. 1바이트를 쓰고 읽지 않아서 기다리는 모두가 같은 알림을 봄
 */
static atomic_int stopping;
static atomic_int cleanup_done;
static int wake_pipe[2] = {-1, -1};
static sigset_t stop_signals; /* SIGINT, SIGTERM */

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s <port> [--root DIR] [--threads N] [--queue N] "
            "[--timeout 초] [--keepalive-max N]\n"
            "       [--route 접두사=이름:포트[,이름:포트...]]... "
            "[--proxy-timeout 초]\n",
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
    char err[512];

    if (argc < 2 || strncmp(argv[1], "--", 2) == 0) {
        usage(argv[0]);
    }
    cfg.port =
        argv[1]; /* 첫 번째 인자는 지금처럼 포트 (기존 기능 테스트 유지) */
    cfg.port_num = positive_int_arg(argv[0], argv[1]);
    if (cfg.port_num > 65535) {
        usage(argv[0]);
    }
    proxy_table_init(&routes);
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
        } else if (strcmp(argv[i], "--route") == 0) {
            /* 잘못된 규칙은 실행 중이 아니라 시작할 때 거절 (M6-4) */
            if (proxy_route_add(&routes, argv[i + 1], cfg.port_num, err,
                                sizeof(err)) < 0) {
                fprintf(stderr, "--route %s: %s\n", argv[i + 1], err);
                exit(1);
            }
        } else {
            usage(argv[0]);
        }
    }
}

/* ============================== M1 + M4 + M7: 소켓, 스레드, 종료 ============================== */

static int listen_socket_open(const char *port);
static void accept_loop(int listenfd, struct conn_queue *queue);
static int accept_cloexec(int listenfd);
static void conn_socket_setup(int connfd);
static void *worker_main(void *arg);
static void *signal_main(void *arg);
static int serve_connection(int connfd);
static void connection_close(int connfd, int next);
static long now_ms(void);

int main(int argc, char **argv)
{
    struct conn_queue queue;
    pthread_t *workers;
    pthread_t signal_thread;
    const char *path;
    int listenfd, rc;

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

    /* CGI 자식은 fork 뒤 async-signal-safe 함수만 써야 하므로 미리 계산 (m4m5.md 2.6) */
    cfg.open_max = sysconf(_SC_OPEN_MAX);
    if (cfg.open_max <= 0 || cfg.open_max > 65536) {
        cfg.open_max = 65536;
    }
    /* 서버의 PATH를 자르지 않고 넘김. fork 전에 한 번만 할당해서 자식은 할당하지 않음 */
    path = getenv("PATH");
    if (path == NULL) {
        path = "/usr/bin:/bin";
    }
    cfg.path_env = malloc(strlen("PATH=") + strlen(path) + 1);
    if (cfg.path_env == NULL) {
        fprintf(stderr, "PATH를 준비하지 못했습니다\n");
        exit(1);
    }
    strcpy(cfg.path_env, "PATH=");
    strcat(cfg.path_env, path);

    /* 다른 스레드를 만들기 전에 막음. 새 스레드는 이 설정을 물려받고,
     * 시그널은 시그널 스레드의 sigwait로만 받음 (M7-5) */
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    rc = pthread_sigmask(SIG_BLOCK, &stop_signals, NULL);
    if (rc != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(rc));
        exit(1);
    }

    if (pipe(wake_pipe) < 0) {
        perror("pipe");
        exit(1);
    }
    fcntl(wake_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(wake_pipe[1], F_SETFD, FD_CLOEXEC);

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
        rc = pthread_create(&workers[i], NULL, worker_main, &queue);
        if (rc != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            exit(1);
        }
    }
    rc = pthread_create(&signal_thread, NULL, signal_main, &queue);
    if (rc != 0) {
        fprintf(stderr, "pthread_create: %s\n", strerror(rc));
        exit(1);
    }

    /* 종료 알림이 올 때까지 accept (M7-6) */
    accept_loop(listenfd, &queue);

    /* 정리: 새 연결을 받지 않음 → 작업자가 큐에 남은 연결까지 처리하고 끝남 */
    close(listenfd);
    for (int i = 0; i < cfg.threads; i++) {
        pthread_join(workers[i], NULL);
    }
    /* 시그널 스레드는 두 번째 신호를 기다리는 중. 정리 완료를 알리고 깨워 join */
    atomic_store(&cleanup_done, 1);
    pthread_kill(signal_thread, SIGTERM);
    pthread_join(signal_thread, NULL);

    conn_queue_destroy(&queue);
    free(workers);
    free(cfg.path_env);
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    printf("종료 완료\n");
    return 0;
}

/*
 * main: 리스닝 소켓과 종료 파이프를 함께 poll로 기다림 (M7-6).
 *   둘이 함께 준비되면 알림을 먼저 보고 accept하지 않음 (큐에 넣을 수 없는 연결)
 *   큐가 가득 차면 conn_queue_push 안에서 기다림 (503 없음). 종료가 시작되면
 *   conn_queue_shutdown이 깨우고, 넣지 못한 연결은 닫음.
 */
static void accept_loop(int listenfd, struct conn_queue *queue)
{
    while (1) {
        struct pollfd p[2] = {{wake_pipe[0], POLLIN, 0}, {listenfd, POLLIN, 0}};
        int connfd;

        if (poll(p, 2, -1) < 0) {
            if (errno != EINTR) {
                perror("poll");
            }
            continue;
        }
        if (p[0].revents != 0) {
            return; /* 종료 알림 */
        }
        if (p[1].revents == 0) {
            continue;
        }
        connfd = accept_cloexec(listenfd);
        if (connfd < 0) {
            /* 논블로킹이라 poll과 accept 사이에 클라이언트가 끊어도 멈추지 않음 */
            if (errno == EMFILE || errno == ENFILE) {
                perror("accept");
                struct timespec ts = {0, 10 * 1000 * 1000};
                nanosleep(&ts,
                          NULL); /* fd가 모자라면 잠깐 쉬어 바쁜 반복을 막음 */
            } else if (errno != EAGAIN && errno != EWOULDBLOCK &&
                       errno != EINTR && errno != ECONNABORTED) {
                perror("accept");
            }
            continue;
        }
        conn_socket_setup(connfd);
        if (conn_queue_push(queue, connfd) < 0) {
            close(connfd); /* 종료가 시작됨: fd를 남기지 않음 */
        }
    }
}

/* 작업자: 큐에서 연결을 꺼내 끝날 때까지 처리 → 닫기 → 반복.
 * 락은 큐를 만질 때만 잡고, 요청 처리는 락 밖에서 함.
 * 종료 뒤에도 큐에 남은 연결은 다 꺼내서 처리함 (M7-7) */
static void *worker_main(void *arg)
{
    struct conn_queue *queue = arg;
    int connfd;

    while ((connfd = conn_queue_pop(queue)) >= 0) {
        connection_close(connfd, serve_connection(connfd));
    }
    return NULL;
}

/*
 * 시그널 스레드 (M7-5, M7-8)
 *   첫 SIGINT·SIGTERM: 종료 플래그 → 종료 파이프에 1바이트 → 큐 종료. 알리기만 함
 *   두 번째: 종류와 상관없이 즉시 종료 (_exit)
 *   main이 정리를 마치고 깨운 것은 두 번째 신호로 세지 않음
 */
static void *signal_main(void *arg)
{
    struct conn_queue *queue = arg;
    int sig;

    while (sigwait(&stop_signals, &sig) != 0) {
    }
    atomic_store(&stopping, 1);
    while (write(wake_pipe[1], "x", 1) < 0 && errno == EINTR) {
    }
    conn_queue_shutdown(queue);
    printf("종료 시작 (%s): 새 연결을 받지 않고 처리 중인 요청을 마칩니다\n",
           sig == SIGINT ? "SIGINT" : "SIGTERM");

    while (1) {
        if (sigwait(&stop_signals, &sig) != 0) {
            continue;
        }
        if (atomic_load(&cleanup_done)) {
            return NULL;
        }
        static const char msg[] = "두 번째 종료 신호: 즉시 종료합니다\n";
        if (write(STDOUT_FILENO, msg, sizeof(msg) - 1) < 0) {
            /* 로그를 못 써도 종료함 */
        }
        _exit(1);
    }
}

/*
 * socket → setsockopt(SO_REUSEADDR) → bind → listen.
 * 리스닝 소켓에 FD_CLOEXEC를 걸어 CGI가 물려받지 않게 함 (m4m5.md 2.6).
 * 논블로킹: poll이 깨운 뒤 클라이언트가 끊어도 accept에서 멈추지 않음 (M7-6).
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
    if (fcntl(listenfd, F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(listenfd, F_SETFL, fcntl(listenfd, F_GETFL) | O_NONBLOCK) < 0) {
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
 * accept하면서 접속 소켓에 FD_CLOEXEC를 겁니다 (m4m5.md 2.6 첫 겹).
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
 * 접속 소켓 준비
 *   블로킹으로 되돌림: macOS는 리스닝 소켓의 논블로킹 설정을 물려줌 (M7-6)
 *   쓰기 타임아웃(--timeout의 2배): 응답을 받아 가지 않는 손님 (m4m5.md 2.4)
 *   읽기는 poll로 마감을 지키므로 SO_RCVTIMEO를 쓰지 않음 (M7-4)
 *   TCP_NODELAY: 헤더와 본문을 write 두 번에 나눠 보내므로, Nagle이 작은 본문을
 *   헤더의 ACK까지 붙잡고 클라이언트는 ACK를 늦게 보내(Delayed ACK) keep-alive의
 *   두 번째 요청부터 약 40ms씩 늦어짐 (F1, RFC 1122 4.2.3.2, 4.2.3.4)
 */
static void conn_socket_setup(int connfd)
{
    struct timeval snd = {cfg.timeout * 2, 0};
    int flags = fcntl(connfd, F_GETFL);
    int one = 1;

    if (flags >= 0 && (flags & O_NONBLOCK)) {
        fcntl(connfd, F_SETFL, flags & ~O_NONBLOCK);
    }
    setsockopt(connfd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    setsockopt(connfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

/* ============================== M7-4: 요청 수신 마감 ============================== */

/*
 * 연결당 하나의 읽기 버퍼 (rio를 대신함).
 *   rio는 read 앞에서 기다리는 방법을 바꿀 수 없어서, 읽기마다 poll로
 *   "마감까지 남은 시간"과 "읽기 타임아웃" 중 짧은 쪽만큼만 기다리게 직접 만듦.
 *   파이프라이닝으로 다음 요청이 이미 버퍼에 있으면 소켓을 기다리지 않음.
 */
struct conn_reader {
    int fd;
    size_t cnt; /* 버퍼에 남은 바이트 */
    char *ptr;  /* 다음에 읽을 위치 */
    char buf[8192];
};

enum read_result { READ_EOF = 0, READ_OK = 1, READ_FAIL = -1, READ_STOP = -2 };

static void reader_init(struct conn_reader *r, int fd)
{
    r->fd = fd;
    r->cnt = 0;
    r->ptr = r->buf;
}

/*
 * 버퍼가 비었을 때 소켓에서 채움.
 *   기다리는 시간: min(마감까지 남은 시간, 읽기 타임아웃). 지나면 READ_FAIL
 *   watch_wake: 종료 파이프도 함께 봄. 소켓과 알림이 함께 준비되면 소켓 먼저
 */
static int reader_fill(struct conn_reader *r, long deadline, int watch_wake)
{
    while (1) {
        struct pollfd p[2] = {{r->fd, POLLIN, 0}, {wake_pipe[0], POLLIN, 0}};
        long left = deadline - now_ms();
        long wait = (long)cfg.timeout * 1000;
        ssize_t n;
        int rc;

        if (left <= 0) {
            return READ_FAIL; /* 전체 마감: 바이트가 와도 연장하지 않음 */
        }
        rc = poll(p, watch_wake ? 2 : 1, (int)(left < wait ? left : wait));
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc <= 0) {
            return READ_FAIL; /* 오류, 새 바이트가 오지 않음 */
        }
        if (p[0].revents == 0) {
            return READ_STOP; /* 종료 알림만 옴 */
        }
        n = read(r->fd, r->buf, sizeof(r->buf));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            return READ_FAIL;
        }
        if (n == 0) {
            return READ_EOF;
        }
        r->cnt = (size_t)n;
        r->ptr = r->buf;
        return READ_OK;
    }
}

/* 요청의 첫 바이트를 기다림 (keep-alive의 다음 요청 포함): --timeout초 */
static int reader_wait_first(struct conn_reader *r, int watch_wake)
{
    if (r->cnt > 0) {
        return READ_OK;
    }
    return reader_fill(r, now_ms() + (long)cfg.timeout * 1000, watch_wake);
}

/*
 * 한 줄 읽기 (rio_readlineb와 같은 규칙): '\n'까지 최대 maxlen - 1바이트.
 * 반환: 읽은 바이트 수, 0(바로 EOF), -1(마감, 오류)
 */
static ssize_t reader_readline(struct conn_reader *r, char *usrbuf,
                               size_t maxlen, long deadline)
{
    size_t n = 0;

    while (n + 1 < maxlen) {
        if (r->cnt == 0) {
            int rc = reader_fill(r, deadline, 0);
            if (rc == READ_EOF) {
                break;
            }
            if (rc != READ_OK) {
                return -1;
            }
        }
        char c = *r->ptr++;
        r->cnt--;
        usrbuf[n++] = c;
        if (c == '\n') {
            break;
        }
    }
    usrbuf[n] = '\0';
    return (ssize_t)n;
}

/* 정확히 len바이트 읽기. 반환: len, 그보다 적으면 끊김이나 마감 */
static ssize_t reader_readn(struct conn_reader *r, char *usrbuf, size_t len,
                            long deadline)
{
    size_t n = 0;

    while (n < len) {
        size_t chunk;
        if (r->cnt == 0 && reader_fill(r, deadline, 0) != READ_OK) {
            break;
        }
        chunk = r->cnt < len - n ? r->cnt : len - n;
        memcpy(usrbuf + n, r->ptr, chunk);
        r->ptr += chunk;
        r->cnt -= chunk;
        n += chunk;
    }
    return (ssize_t)n;
}

/* ============================== 응답 쓰기 ============================== */

/*
 * keep 값: 1 연결 유지, 0 응답 뒤 닫음, KEEP_SEND_FAILED 응답을 보내다 실패.
 * 보내다 실패하면 지킬 응답이 없으므로 남은 데이터를 비우지 않고 바로 닫음
 * (F2, m4m5.md "응답을 보내다 실패 → 바로 닫음")
 */
#define KEEP_SEND_FAILED (-1)

/* response_head_send의 length로 주면 Content-Length 줄을 쓰지 않음 (204, 304) */
#define NO_CONTENT_LENGTH ((size_t)-1)

static int write_all(int fd, const void *buf, size_t n)
{
    return rio_writen(fd, (void *)buf, n) == (ssize_t)n ? 0 : -1;
}

/*
 * 응답 헤더를 씁니다 (m4m5.md 0장 "응답 형식").
 *   상태 줄 HTTP/1.1, 모든 응답에 Content-Length와 Connection.
 *   content_type이 NULL이면 Content-Type 줄을 쓰지 않음 (CGI는 extra에 들어 있음).
 *   extra: "이름: 값\r\n" 형식의 추가 헤더 (없으면 NULL)
 *   종료가 시작됐으면 Connection: close로 보내고 *keep을 0으로 (M7-7)
 *   length가 NO_CONTENT_LENGTH면 Content-Length 줄을 쓰지 않음 (204, 304)
 */
static int response_head_send(int fd, int status, const char *reason,
                              const char *content_type, const char *extra,
                              size_t length, int *keep)
{
    char header[MAXBUF * 2];
    char type_line[HTTP_FIELD_MAX + 32] = "";
    char length_line[64] = "";
    int n;

    if (atomic_load(&stopping)) {
        *keep = 0;
    }
    if (content_type != NULL) {
        snprintf(type_line, sizeof(type_line), "Content-Type: %s\r\n",
                 content_type);
    }
    if (length != NO_CONTENT_LENGTH) {
        snprintf(length_line, sizeof(length_line), "Content-Length: %zu\r\n",
                 length);
    }
    n = snprintf(header, sizeof(header),
                 "HTTP/1.1 %d %s\r\n"
                 "Server: Tiny Web Server\r\n"
                 "%s%s%s"
                 "Connection: %s\r\n\r\n",
                 status, reason ? reason : http_status_text(status), type_line,
                 extra ? extra : "", length_line,
                 *keep > 0 ? "keep-alive" : "close");
    if (n < 0 || n >= (int)sizeof(header) ||
        write_all(fd, header, (size_t)n) < 0) {
        *keep = KEEP_SEND_FAILED;
        return -1;
    }
    return 0;
}

/*
 * 요청에서 온 글자를 HTML에 넣을 수 있게 바꿈: < > & " (F3, 반사형 XSS 방지).
 * 결과가 out에 다 들어가지 않으면 들어가는 데까지만 씀 (항상 NUL로 끝남).
 */
static void html_escape(const char *in, char *out, size_t cap)
{
    size_t n = 0;

    for (; *in != '\0'; in++) {
        const char *rep;
        char one[2] = {*in, '\0'};
        size_t len;

        switch (*in) {
        case '<':
            rep = "&lt;";
            break;
        case '>':
            rep = "&gt;";
            break;
        case '&':
            rep = "&amp;";
            break;
        case '"':
            rep = "&quot;";
            break;
        default:
            rep = one;
            break;
        }
        len = strlen(rep);
        if (n + len >= cap) {
            break;
        }
        memcpy(out + n, rep, len);
        n += len;
    }
    out[n] = '\0';
}

/*
 * 오류 응답 (Phase 1의 http_error_send에 Content-Length, Connection 추가).
 * HEAD 요청이면 본문을 빼고 보냄. 쓰기가 실패하면 *keep을 KEEP_SEND_FAILED로.
 * extra: 추가 헤더 (405의 Allow 등, 없으면 NULL). 보낸 상태 코드를 돌려줌.
 * cause는 요청에서 온 글자라 HTML 특수문자를 바꿔서 넣음 (F3).
 */
static int error_reply_extra(int fd, const struct http_request *req, int status,
                             const char *extra, const char *cause,
                             const char *longmsg, int *keep)
{
    char body[MAXBUF];
    char safe_cause[MAXBUF];
    int body_len;
    int send_body = !(req != NULL && strcmp(req->method, "HEAD") == 0);

    html_escape(cause, safe_cause, sizeof(safe_cause));

    body_len = snprintf(body, sizeof(body),
                        "<html><title>Tiny Error</title>"
                        "<body bgcolor=ffffff>\r\n"
                        "%d: %s\r\n"
                        "<p>%s: %s\r\n"
                        "<hr><em>The Tiny Web server</em>\r\n",
                        status, http_status_text(status), longmsg, safe_cause);
    if (body_len >= (int)sizeof(body)) {
        body_len = sizeof(body) - 1;
    }
    if (response_head_send(fd, status, NULL, "text/html", extra,
                           (size_t)body_len, keep) < 0 ||
        (send_body && write_all(fd, body, (size_t)body_len) < 0)) {
        *keep = KEEP_SEND_FAILED;
    }
    return status;
}

static int error_reply(int fd, const struct http_request *req, int status,
                       const char *cause, const char *longmsg, int *keep)
{
    return error_reply_extra(fd, req, status, NULL, cause, longmsg, keep);
}

/* 요청마다 한 줄 로그. printf 한 번이라 스레드끼리 한 줄 안에서는 섞이지 않음.
 * 프록시 요청이면 보낸 백엔드를 붙임 (Round-Robin 확인용) */
static void request_log(const struct http_request *req, int status,
                        const char *backend)
{
    printf("%s %s %s %d%s%s\n", req->method[0] ? req->method : "-",
           req->uri[0] ? req->uri : "-", req->version[0] ? req->version : "-",
           status, backend ? " -> " : "", backend ? backend : "");
}

/* ============================== M5: 연결과 요청 처리 ============================== */

/* request_handle의 결과: 연결을 어떻게 할지 */
enum conn_next {
    CONN_CLOSE_SILENT =
        0,         /* 바로 닫음 (끊김, 마감, 본문 도중 실패, 응답 전송 실패) */
    CONN_KEEP = 1, /* 다음 요청을 기다림 */
    CONN_CLOSE_REPLIED =
        2,               /* 응답을 보낸 뒤 닫음 → 남은 데이터를 비우고 닫음 */
    CONN_CLOSE_RESET = 3 /* RST로 닫음: 길이 없는 프록시 응답이 잘림 (M6-13) */
};

static int request_handle(int fd, struct conn_reader *r, int can_keep,
                          int watch_wake, const char *client_ip);

/* 응답을 보낸 뒤의 keep 값을 연결 처리로 바꿈 (F2) */
static int conn_after(int keep)
{
    if (keep > 0) {
        return CONN_KEEP;
    }
    return keep < 0 ? CONN_CLOSE_SILENT : CONN_CLOSE_REPLIED;
}
static int request_route(int fd, struct http_request *req, int *keep);
static int proxy_handle(int fd, struct http_request *req,
                        struct proxy_route *route, const char *client_ip,
                        int *keep);
static int uri_path_is_clean(const char *uri);
static int uri_segments_are_clean(const char *path, size_t len);

/*
 * 연결 하나를 처리합니다 (m4m5.md 5.1).
 *   읽기 버퍼는 연결당 한 번만 준비함. 파이프라이닝으로 다음 요청 앞부분이
 *   이미 버퍼에 들어와 있을 수 있어서, 요청마다 새로 만들면 그 데이터를 잃음.
 *   이 연결의 요청 수가 --keepalive-max에 닿으면 그 응답에 Connection: close.
 *   종료 알림은 두 번째 요청을 기다릴 때부터 봄: 큐에 있던 연결의 첫 요청은
 *   처리함 (M7-6, M7-7)
 */
static int serve_connection(int connfd)
{
    struct conn_reader r;
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    char client_ip[INET_ADDRSTRLEN] = "unknown";
    int next;

    /* 백엔드에 X-Forwarded-For로 알려 줄 클라이언트 IP (M6-10) */
    if (getpeername(connfd, (SA *)&peer, &peer_len) == 0) {
        inet_ntop(AF_INET, &peer.sin_addr, client_ip, sizeof(client_ip));
    }
    reader_init(&r, connfd);
    for (int n = 1;; n++) {
        next =
            request_handle(connfd, &r, n < cfg.keepalive_max, n > 1, client_ip);
        if (next != CONN_KEEP) {
            break;
        }
    }
    return next;
}

static void connection_close_lingering(int connfd);

/* serve_connection의 결과에 맞게 연결을 닫음 */
static void connection_close(int connfd, int next)
{
    if (next == CONN_CLOSE_REPLIED) {
        connection_close_lingering(connfd);
    } else if (next == CONN_CLOSE_RESET) {
        /* SO_LINGER 0초로 닫으면 FIN 대신 RST: 클라이언트가 잘린 응답을 알 수 있음 */
        struct linger lg = {1, 0};
        setsockopt(connfd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    }
    close(connfd);
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
 * 요청 하나를 읽고 응답합니다. 반환은 enum conn_next.
 * 한 줄 규칙: 요청을 끝까지 읽었고 응답 길이를 알면 유지, 아니면 닫음 (5.2)
 *
 * 수신 마감 (M7-4, --timeout이 5초일 때)
 *   첫 바이트 5초, 새 바이트 간격 5초, 첫 바이트부터 헤더 끝까지 10초,
 *   본문 전체 30초. 마감이 지나면 응답 없이 닫음.
 */
static int request_handle(int fd, struct conn_reader *r, int can_keep,
                          int watch_wake, const char *client_ip)
{
    struct http_request req;
    char line[MAXLINE];
    struct proxy_route *route;
    ssize_t n;
    long body_len, deadline;
    int status;
    int keep = 0;

    http_request_init(&req);

    /* 0. 첫 바이트: 끊김, 타임아웃, 쉬는 중 종료 알림이면 조용히 닫음 (M7-6) */
    if (reader_wait_first(r, watch_wake) != READ_OK) {
        return CONN_CLOSE_SILENT;
    }
    deadline = now_ms() + (long)cfg.timeout * HEADER_DEADLINE_MULT * 1000;

    /* 1. 요청 줄. 0(클라이언트가 끊음)이나 -1(마감)이면 조용히 닫음 */
    n = reader_readline(r, line, MAXLINE, deadline);
    if (n <= 0) {
        return CONN_CLOSE_SILENT;
    }
    status = http_parse_request_line(&req, line, (size_t)n, MAXLINE);
    if (status != 0) { /* 414, 400, 505, 501: 본문 경계를 모르므로 닫음 */
        error_reply(fd, &req, status, "request line", "Bad request line",
                    &keep);
        request_log(&req, status, NULL);
        return conn_after(keep);
    }

    /* 2. 헤더: 빈 줄까지 "이름: 값" → 구조체 + 원본 블록 */
    while (1) {
        n = reader_readline(r, line, MAXLINE, deadline);
        if (n <= 0) {
            return CONN_CLOSE_SILENT; /* 헤더 도중 끊김, 마감 */
        }
        status = http_parse_header_line(&req, line, (size_t)n, MAXLINE);
        if (status == 1) {
            break;
        }
        if (status != 0) {
            error_reply(fd, &req, status, "header", "Bad header", &keep);
            request_log(&req, status, NULL);
            return conn_after(keep);
        }
    }

    /* 3. 헤더 검사: Host, CL+TE, TE, 413, 417 */
    status = http_finish_headers(&req);
    if (status != 0) {
        error_reply(fd, &req, status, req.uri, "Request rejected", &keep);
        request_log(&req, status, NULL);
        return conn_after(keep);
    }
    keep = req.keep_alive && can_keep;
    body_len = http_body_length(&req);

    /* 4. Expect: 100-continue면 본문을 받기 전에 계속 보내라고 알림 (3.6) */
    if (req.expect == HTTP_EXPECT_CONTINUE && body_len > 0) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (write_all(fd, cont, sizeof(cont) - 1) < 0) {
            return CONN_CLOSE_SILENT;
        }
    }

    /* 5. 본문: 정확히 CL바이트. 마감은 헤더 검사와 100 Continue 뒤부터 셈 */
    if (body_len > 0) {
        deadline = now_ms() + (long)cfg.timeout * BODY_DEADLINE_MULT * 1000;
        req.body = malloc((size_t)body_len);
        if (req.body == NULL) {
            keep = 0;
            error_reply(fd, &req, 500, req.uri, "Out of memory", &keep);
            request_log(&req, 500, NULL);
            return conn_after(keep);
        }
        n = reader_readn(r, req.body, (size_t)body_len, deadline);
        if (n != (ssize_t)body_len) { /* 도중에 끊김, 마감: 응답 없이 닫음 */
            http_request_free(&req);
            return CONN_CLOSE_SILENT;
        }
        req.body_len = (size_t)body_len;
    }

    /* 6. 경로 검사: 라우팅 전에 모든 요청에 같은 규칙 (M6-7) */
    if (!uri_path_is_clean(req.uri)) {
        status = error_reply(fd, &req, 403, req.uri,
                             "Tiny doesn't serve this path", &keep);
        request_log(&req, status, NULL);
        http_request_free(&req);
        return conn_after(keep);
    }

    /* 7. 라우팅: 규칙에 걸리면 백엔드로, 아니면 직접 처리 (M6-2, M6-6) */
    route = routes.count > 0 ? proxy_route_find(&routes, req.uri) : NULL;
    if (route != NULL) {
        int next = proxy_handle(fd, &req, route, client_ip, &keep);
        http_request_free(&req);
        return next;
    }
    status = request_route(fd, &req, &keep);
    request_log(&req, status, NULL);
    http_request_free(&req);
    return conn_after(keep);
}

/*
 * 백엔드로 중계 (src/proxy.c). 결과를 연결 처리로 바꿈.
 *   백엔드 실패(502, 504)는 요청을 끝까지 읽은 뒤의 오류라 연결 유지 (M6-16)
 *   본문 도중 실패는 바로 닫거나 RST (M6-13)
 */
static int proxy_handle(int fd, struct http_request *req,
                        struct proxy_route *route, const char *client_ip,
                        int *keep)
{
    const char *backend = NULL;
    int status = 0;
    int rc = proxy_forward(fd, req, client_ip, route, cfg.proxy_timeout,
                           &stopping, keep, &status, &backend);
    int next;

    switch (rc) {
    case PROXY_FAILED:
        error_reply(fd, req, status, req->uri,
                    status == 504 ? "Backend did not respond in time"
                                  : "Backend failed",
                    keep);
        next = conn_after(*keep);
        break;
    case PROXY_REPLIED:
        next = conn_after(*keep);
        break;
    case PROXY_RESET:
        next = CONN_CLOSE_RESET;
        break;
    default:
        next = CONN_CLOSE_SILENT;
        break;
    }
    request_log(req, status, backend);
    return next;
}

/*
 * '?' 앞 경로에 "..", "." 조각이나 중간의 빈 조각("//")이 있으면 0 (M6-7).
 * 경로 끝의 '/'는 허용. URI는 '/'로 시작함 (파서가 검사, M7-1).
 */
static int uri_path_is_clean(const char *uri)
{
    const char *q = strchr(uri, '?');

    return uri_segments_are_clean(uri, q ? (size_t)(q - uri) : strlen(uri));
}

/*
 * 경로 len바이트를 '/'로 나눠 검사. '?'를 경로 끝으로 보지 않음:
 * 디코딩한 경로에서는 '?'가 %3F에서 나온 파일 이름의 일부이기 때문 (static-percent-path)
 */
static int uri_segments_are_clean(const char *path, size_t len)
{
    size_t start = 1;

    for (size_t i = 1; i <= len; i++) {
        if (i < len && path[i] != '/') {
            continue;
        }
        size_t seg = i - start;
        if (seg == 0 && i < len) {
            return 0; /* "//" */
        }
        if (seg == 1 && path[start] == '.') {
            return 0; /* "/./" */
        }
        if (seg == 2 && path[start] == '.' && path[start + 1] == '.') {
            return 0; /* "/../" */
        }
        start = i + 1;
    }
    return 1;
}

/* ============================== M2: 경로 확인 (Phase 1에서 이어짐) ============================== */

enum uri_decode { DECODE_OK, DECODE_BAD, DECODE_SLASH, DECODE_LONG };

static enum uri_decode uri_path_decode(const char *uri, char *path, size_t cap);
static int uri_parse(const char *path, const char *query, char *filename,
                     char *cgiargs);
static int static_serve(int fd, const struct http_request *req,
                        const char *filename, off_t filesize, int *keep);
static int cgi_serve(int fd, const struct http_request *req,
                     const char *filename, const char *cgiargs, int *keep);

/*
 * 요청을 정적 파일 / CGI로 보냄. 보낸 상태 코드를 돌려줌.
 * 검사 순서: 파일 없음 404 → 메서드 405 → 파일 종류와 권한 403 (M7-3)
 */
static int request_route(int fd, struct http_request *req, int *keep)
{
    char path[MAXLINE], filename[MAXLINE], cgiargs[MAXLINE];
    const char *query = strchr(req->uri, '?');
    struct stat sbuf;
    int is_static;

    /* 퍼센트 디코딩은 직접 처리하는 요청에만. 중계는 원래 URI를 그대로 넘김 */
    switch (uri_path_decode(req->uri, path, sizeof(path))) {
    case DECODE_BAD:
        return error_reply(fd, req, 400, req->uri,
                           "Bad percent-encoding in the path", keep);
    case DECODE_SLASH:
        return error_reply(fd, req, 404, req->uri,
                           "Tiny doesn't accept an encoded slash", keep);
    case DECODE_LONG:
        *keep = 0;
        return error_reply(fd, req, 414, req->uri, "Path too long", keep);
    default:
        break;
    }
    /* 6단계 검사는 디코딩 전 URI였으므로 %2e%2e 같은 것을 여기서 다시 막음 */
    if (!uri_segments_are_clean(path, strlen(path))) {
        return error_reply(fd, req, 403, req->uri,
                           "Tiny doesn't serve this path", keep);
    }

    is_static = uri_parse(path, query ? query + 1 : NULL, filename, cgiargs);
    if (is_static < 0) {
        *keep = 0;
        return error_reply(fd, req, 414, req->uri, "Path too long", keep);
    }
    if (stat(filename, &sbuf) < 0) {
        return error_reply(fd, req, 404, filename,
                           "Tiny couldn't find this file", keep);
    }

    if (is_static) {
        if (strcmp(req->method, "POST") ==
            0) { /* 서버는 알지만 이 대상에는 안 됨 */
            return error_reply_extra(fd, req, 405, "Allow: GET, HEAD\r\n",
                                     req->method,
                                     "Static files allow GET and HEAD", keep);
        }
        if (!S_ISREG(sbuf.st_mode) || !(S_IRUSR & sbuf.st_mode)) {
            return error_reply(fd, req, 403, filename,
                               "Tiny couldn't read the file", keep);
        }
        return static_serve(fd, req, filename, sbuf.st_size, keep);
    }

    if (strcmp(req->method, "HEAD") == 0) { /* CGI에 HEAD: 본문 없이 405 */
        return error_reply_extra(fd, req, 405, "Allow: GET, POST\r\n",
                                 req->method, "CGI programs allow GET and POST",
                                 keep);
    }
    if (!S_ISREG(sbuf.st_mode) || !(S_IXUSR & sbuf.st_mode)) {
        return error_reply(fd, req, 403, filename,
                           "Tiny couldn't run the CGI program", keep);
    }
    return cgi_serve(fd, req, filename, cgiargs, keep);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/*
 * URI의 '?' 앞 경로에서 %XX를 풀어 path에 씀 (static-percent-path).
 * 브라우저는 공백, 한글 파일 이름을 인코딩해서 보내므로 풀어야 파일을 찾음.
 *   딱 한 번만 풂: "%252e"는 "%2e"라는 글자가 되고 다시 풀리지 않음.
 *     두 번 풀면 "%252e%252e"가 ".."이 되어 경로 검사를 피함
 *   잘못된 표기("%G1", 끝에 남은 "%")와 제어 문자(%00~%1F, %7F)는 DECODE_BAD (400).
 *     %00은 C 문자열을 거기서 끊어 다른 파일을 열게 만듦
 *   %2F는 DECODE_SLASH (404): 인코딩된 '/'로 경로 조각 경계를 숨기지 못하게
 *     (Apache 기본값 AllowEncodedSlashes Off와 같음)
 *   '+'는 공백으로 바꾸지 않음 (폼 인코딩 규칙이라 경로에는 해당 없음)
 *   쿼리는 풀지 않음: CGI의 QUERY_STRING은 인코딩된 채로 넘기는 것이 규약
 */
static enum uri_decode uri_path_decode(const char *uri, char *path, size_t cap)
{
    const char *q = strchr(uri, '?');
    size_t len = q ? (size_t)(q - uri) : strlen(uri);
    size_t j = 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)uri[i];

        if (c == '%') {
            int hi, lo;

            if (len - i < 3) {
                return DECODE_BAD;
            }
            hi = hex_value(uri[i + 1]);
            lo = hex_value(uri[i + 2]);
            if (hi < 0 || lo < 0) {
                return DECODE_BAD;
            }
            c = (unsigned char)(hi * 16 + lo);
            i += 2;
            if (c == '/') {
                return DECODE_SLASH;
            }
            if (c < 0x20 || c == 0x7f) {
                return DECODE_BAD;
            }
        }
        if (j + 1 >= cap) {
            return DECODE_LONG;
        }
        path[j++] = (char)c;
    }
    path[j] = '\0';
    return DECODE_OK;
}

/*
 * 디코딩한 경로와 쿼리로 정적/동적을 나누고 파일 경로와 CGI 인자를 만듭니다.
 * 정적 1, 동적 0, 경로가 버퍼보다 길면 -1. query는 '?' 뒤 (없으면 NULL).
 *   경로는 이미 '?'에서 잘린 것이라 다시 '?'로 자르지 않음 (%3F는 파일 이름의 일부)
 *   경로의 첫 조각이 cgi-bin일 때만 CGI. "/docs/cgi-bin-notes.html"이나
 *   "/home.html?x=cgi-bin"은 정적 파일 (F9)
 *   정적 파일은 쿼리를 무시: "/style.css?v=2" → ./style.css
 */
static int uri_parse(const char *path, const char *query, char *filename,
                     char *cgiargs)
{
    size_t path_len = strlen(path);
    int is_cgi = path_len >= 8 && strncmp(path, "/cgi-bin", 8) == 0 &&
                 (path_len == 8 || path[8] == '/');
    int n;

    if (!is_cgi) {
        cgiargs[0] = '\0';
        n = snprintf(filename, MAXLINE, ".%s%s", path,
                     path[path_len - 1] == '/' ? "home.html" : "");
        return (n < 0 || n >= MAXLINE) ? -1 : 1;
    }
    n = snprintf(cgiargs, MAXLINE, "%s", query ? query : "");
    if (n < 0 || n >= MAXLINE) {
        return -1;
    }
    n = snprintf(filename, MAXLINE, ".%s", path);
    return (n < 0 || n >= MAXLINE) ? -1 : 0;
}

/*
 * 정적 파일 응답. 파일을 준비(open, mmap)한 뒤 200을 보냄.
 * HEAD면 같은 헤더(Content-Length 포함)만 보내고 본문은 뺌 (3.8).
 * MIME은 마지막 확장자로 판단 (3.7).
 */
static int static_serve(int fd, const struct http_request *req,
                        const char *filename, off_t filesize, int *keep)
{
    int srcfd;
    char *srcp = NULL;
    int send_body = strcmp(req->method, "HEAD") != 0;

    srcfd = open(filename, O_RDONLY, 0);
    if (srcfd < 0) {
        return error_reply(fd, req, 500, filename,
                           "Tiny couldn't open the file", keep);
    }
    if (filesize > 0 && send_body) {
        srcp = mmap(0, (size_t)filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
        if (srcp == MAP_FAILED) {
            close(srcfd);
            return error_reply(fd, req, 500, filename,
                               "Tiny couldn't map the file", keep);
        }
    }
    close(srcfd);

    if (response_head_send(fd, 200, NULL, http_mime_type(filename), NULL,
                           (size_t)filesize, keep) < 0 ||
        (srcp != NULL && write_all(fd, srcp, (size_t)filesize) < 0)) {
        *keep = KEEP_SEND_FAILED; /* 응답을 보내다 실패하면 바로 닫음 */
    }
    if (srcp != NULL) {
        munmap(srcp, (size_t)filesize);
    }
    return 200;
}

/* ============================== M5: CGI (m4m5.md 4장) ============================== */

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
 * 자식에서 fd 3 이상을 모두 닫습니다 (m4m5.md 2.6 두 번째 겹).
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
static int cgi_output_collect(pid_t pid, int outfd, char **out, size_t *out_len)
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
    /* CGI는 자기 프로세스 그룹에 있음. 그룹과 CGI 자신에게 모두 보냄:
     * 아직 setpgid 전이면 그룹이 없고, 그룹을 옮겼으면 그룹 kill이 안 닿음 (M7-9) */
    kill(-pid, SIGKILL);
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
 * CGI 출력("헤더 몇 줄 + 빈 줄 + 본문")을 나눠 응답을 만듭니다 (4.4).
 *   Status: 404 Not Found가 있으면 그 상태, 없으면 200
 *   Content-Length, Connection은 CGI 값을 무시하고 서버가 직접 씀
 *   빈 줄이 없거나, 출력이 비었거나, ':' 없는 헤더 줄이면 500
 * 중계 응답(proxy_response_parse)과 같은 HTTP 규칙으로 검사 (F8)
 *   500: 1xx 상태(101 포함), Transfer-Encoding, 토큰이 아닌 헤더 이름, 제어 문자
 *   204, 304: 본문과 Content-Length 없이 헤더에서 끝냄 (RFC 9110 8.6, 9112 6.3)
 *     204: RFC가 Content-Length를 금지함
 *     304: 선택이 아니라 필수. 서버가 쓰는 Content-Length는 실제 본문 길이(0)인데,
 *          304의 Content-Length는 같은 요청에 200을 보냈을 때의 길이여야 해서
 *          0은 틀린 값임 (RFC 9110 8.6). 중계는 백엔드가 준 값을 그대로 전달할
 *          수 있어 304의 길이를 남김. 그래서 304에서만 CGI와 중계 규칙이 다름
 *   홉 단위 헤더(Keep-Alive, Upgrade 등)는 뺌
 */

/* CGI가 써도 클라이언트에 보내지 않는 헤더: 서버가 쓰거나 한 구간 전용 */
static const char *const CGI_DROP[] = {
    "Content-Length", "Connection", "Keep-Alive", "Proxy-Connection", "TE",
    "Trailer",        "Upgrade",    NULL};

static int cgi_header_dropped(const char *name, size_t len)
{
    for (int i = 0; CGI_DROP[i] != NULL; i++) {
        if (strlen(CGI_DROP[i]) == len &&
            strncasecmp(name, CGI_DROP[i], len) == 0) {
            return 1;
        }
    }
    return 0;
}
static int cgi_reply(int fd, const struct http_request *req, const char *out,
                     size_t len, int *keep)
{
    char extra[MAXBUF];
    char reason[64] = "OK";
    size_t extra_len = 0, hdr_end = 0, body_start = 0, pos = 0;
    int status = 200;
    int found = 0;

    for (size_t i = 0; i < len; i++) { /* 헤더와 본문을 나누는 빈 줄 찾기 */
        if (i + 1 < len && out[i] == '\n' && out[i + 1] == '\n') {
            hdr_end = i + 1;
            body_start = i + 2;
            found = 1;
            break;
        }
        if (i + 3 < len && memcmp(out + i, "\r\n\r\n", 4) == 0) {
            hdr_end = i + 2;
            body_start = i + 4;
            found = 1;
            break;
        }
    }
    if (len == 0 || !found) {
        return error_reply(fd, req, 500, req->uri, "CGI output has no header",
                           keep);
    }

    extra[0] = '\0';
    while (pos < hdr_end) { /* 헤더 줄 하나씩 */
        size_t eol = pos, end;
        const char *colon;
        while (eol < hdr_end && out[eol] != '\n') {
            eol++;
        }
        end = eol;
        if (end > pos && out[end - 1] == '\r') {
            end--;
        }
        if (end > pos) {
            size_t line_len = end - pos;
            colon = memchr(out + pos, ':', line_len);
            if (colon == NULL || http_has_ctl(out + pos, line_len)) {
                return error_reply(fd, req, 500, req->uri,
                                   "Malformed CGI header", keep);
            }
            size_t name_len = (size_t)(colon - (out + pos));
            if (!http_is_token(out + pos, name_len)) {
                return error_reply(fd, req, 500, req->uri,
                                   "Malformed CGI header name", keep);
            }
            if (name_len == 17 &&
                strncasecmp(out + pos, "Transfer-Encoding", 17) == 0) {
                return error_reply(fd, req, 500, req->uri,
                                   "CGI must not set Transfer-Encoding", keep);
            }
            if (name_len == 6 && strncasecmp(out + pos, "Status", 6) == 0) {
                const char *v = colon + 1;
                const char *vend = out + end;
                char num[4] = "";
                while (v < vend && (*v == ' ' || *v == '\t')) {
                    v++;
                }
                if (vend - v < 3) {
                    return error_reply(fd, req, 500, req->uri,
                                       "Malformed CGI status", keep);
                }
                memcpy(num, v, 3);
                status = atoi(num);
                if (status < 200 || status > 599) { /* 1xx는 최종 응답이 아님 */
                    return error_reply(fd, req, 500, req->uri,
                                       "Malformed CGI status", keep);
                }
                v += 3;
                while (v < vend && *v == ' ') {
                    v++;
                }
                snprintf(reason, sizeof(reason), "%.*s", (int)(vend - v), v);
                if (reason[0] == '\0') {
                    snprintf(reason, sizeof(reason), "%s",
                             http_status_text(status));
                }
            } else if (cgi_header_dropped(out + pos, name_len)) {
                /* 서버가 직접 쓰거나 한 구간 전용 */
            } else {
                if (extra_len + line_len + 3 > sizeof(extra)) {
                    return error_reply(fd, req, 500, req->uri,
                                       "CGI header too large", keep);
                }
                memcpy(extra + extra_len, out + pos, line_len);
                extra_len += line_len;
                memcpy(extra + extra_len, "\r\n", 3);
                extra_len += 2;
            }
        }
        pos = eol + 1;
    }

    /* 본문 없이 헤더에서 끝남. 304에 길이를 쓰면 틀린 값(0)이라 반드시 뺌 */
    if (status == 204 || status == 304) {
        if (response_head_send(fd, status, reason, NULL, extra,
                               NO_CONTENT_LENGTH, keep) < 0) {
            *keep = KEEP_SEND_FAILED;
        }
        return status;
    }
    if (response_head_send(fd, status, reason, NULL, extra, len - body_start,
                           keep) < 0 ||
        (len > body_start &&
         write_all(fd, out + body_start, len - body_start) < 0)) {
        *keep = KEEP_SEND_FAILED;
    }
    return status;
}

/*
 * CGI 실행 (m4m5.md 4.3, M7-9)
 *   fork 전(부모): envp, 입력 fd, 출력 파이프 준비
 *   자식: dup2(입력 → stdin, 파이프 → stdout) → fd 3 이상 close → execve
 *         (멀티스레드 fork 뒤라 setenv, malloc을 쓰지 않음)
 *   부모: 출력을 모아(1 MiB, 10초) 응답 생성. 실패 500, 시간 초과 504
 */
static int cgi_serve(int fd, const struct http_request *req,
                     const char *filename, const char *cgiargs, int *keep)
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
    sigset_t no_signals;
    struct sigaction default_action;

    /* 1. envp: 부모가 fork 전에 만들어 둠 (2.5) */
    snprintf(env_method, sizeof(env_method), "REQUEST_METHOD=%s", req->method);
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
        return error_reply(fd, req, 500, filename, "CGI input failed", keep);
    }
    if (pipe(pipefd) < 0) {
        close(infd);
        return error_reply(fd, req, 500, filename, "CGI pipe failed", keep);
    }
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    /* 3. fork. 자식에서 쓸 시그널 설정은 fork 전에 만들어 둠 */
    sigemptyset(&no_signals);
    memset(&default_action, 0, sizeof(default_action));
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    pid = fork();
    if (pid < 0) {
        close(infd);
        close(pipefd[0]);
        close(pipefd[1]);
        return error_reply(fd, req, 500, filename, "CGI fork failed", keep);
    }
    if (pid == 0) { /* 자식: async-signal-safe 함수만 */
        /* 자기 프로세스 그룹: 터미널의 Ctrl+C는 서버 그룹 전체에 가므로,
         * 나누지 않으면 정상 종료 중인 CGI 요청이 함께 죽음 (M7-9) */
        setpgid(0, 0);
        /* 막은 시그널과 무시한 SIGPIPE는 execve 뒤에도 남음 → 되돌림 */
        sigaction(SIGPIPE, &default_action, NULL);
        sigprocmask(SIG_SETMASK, &no_signals, NULL);
        if (dup2(infd, STDIN_FILENO) < 0 ||
            dup2(pipefd[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        child_close_fds();
        execve(filename, argv, envp);
        _exit(127); /* execve 실패 → 부모가 500 */
    }

    setpgid(pid, pid); /* 부모도 설정: kill(-pid)가 자식 실행 전에도 닿게 함 */

    /* 4. 부모: 자식이 쓰는 쪽을 닫아야 자식이 끝날 때 EOF를 받음 */
    close(infd);
    close(pipefd[1]);
    result = cgi_output_collect(pid, pipefd[0], &out, &out_len);
    close(pipefd[0]);

    if (result == CGI_TIMEOUT) {
        return error_reply(fd, req, 504, filename,
                           "CGI program did not finish in time", keep);
    }
    if (result == CGI_FAILED) {
        return error_reply(fd, req, 500, filename, "CGI program failed", keep);
    }
    status = cgi_reply(fd, req, out, out_len, keep);
    free(out);
    return status;
}
