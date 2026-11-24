/*
 * cgi.c - CGI 실행과 출력 해석 (설명은 cgi.h, 순서는 final.md 4.3)
 *
 *   부모                                   자식
 *   pipe(출력용)
 *   fork ───────────────────────────────▶  dup2(infd → 0), dup2(파이프 쓰기 → 1)
 *   파이프 쓰기 쪽 close                   fd 3 이상 모두 close → execve
 *   poll로 남은 시간만큼 기다리며 read     (CGI가 출력하고 끝남)
 *     - 출력 상한 초과 → kill → 500
 *     - 시간 초과     → kill → 504
 *   EOF 뒤 waitpid(WNOHANG)를 마감까지 반복
 *   종료 상태 확인 → 0 또는 500
 */
#include "cgi.h"
#include "http.h" /* http_status_text */

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* 단조 시계(ms). 시스템 시간을 바꿔도 거꾸로 가지 않아 마감 계산에 씀 */
static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};

    nanosleep(&ts, NULL);
}

/* waitpid를 EINTR이면 다시 부름 */
static pid_t waitpid_retry(pid_t pid, int *wstatus, int options)
{
    pid_t r;

    do {
        r = waitpid(pid, wstatus, options);
    } while (r < 0 && errno == EINTR);
    return r;
}

/* 끝나지 않는 자식을 멈추고 회수. 회수하지 않으면 좀비로 남음 */
static void child_kill(pid_t pid)
{
    kill(pid,
         SIGKILL); /* SIGTERM은 CGI가 무시할 수 있으므로 무시할 수 없는 신호 */
    waitpid_retry(pid, NULL, 0);
}

/*
 * 자식: 표준 입출력을 연결하고 CGI 프로그램으로 바뀝니다.
 * 멀티스레드 서버에서 fork한 자식이므로 malloc을 쓰는 함수는 부르지 않습니다
 * (async-signal-safe: dup2, close, execve, _exit만. final.md 2.5).
 */
static void child_exec(const char *filename, char *const envp[], int infd,
                       int outfd, int fd_close_max)
{
    char *argv[] = {NULL}; /* CGI 프로그램에 넘길 인자 없음 */

    if (dup2(infd, STDIN_FILENO) < 0 || dup2(outfd, STDOUT_FILENO) < 0) {
        _exit(127);
    }
    /* 0, 1, 2만 남김: 다른 손님 소켓, 리스닝 소켓, 파이프 원본이 넘어가지 않게 */
    for (int i = 3; i < fd_close_max; i++) {
        close(i);
    }
    execve(filename, argv, envp);
    _exit(
        127); /* execve 실패. exit 대신 _exit: 부모의 stdio 버퍼를 비우지 않음 */
}

int cgi_run(const char *filename, char *const envp[], int infd, int timeout_sec,
            size_t out_max, int fd_close_max, struct cgi_output *out)
{
    int pipefd[2] = {-1, -1};
    pid_t pid = -1;
    size_t cap = 0;
    long long deadline;
    int wstatus;
    int result = 500;

    out->data = NULL;
    out->len = 0;

    if (pipe(pipefd) < 0) {
        return 500;
    }
    pid = fork();
    if (pid < 0) {
        goto cleanup; /* fork 실패 (프로세스 수 한도 등) → 500 */
    }
    if (pid == 0) {
        close(pipefd[0]); /* 자식은 읽기 쪽이 필요 없음 */
        child_exec(filename, envp, infd, pipefd[1], fd_close_max);
    }
    /* 부모: 쓰기 쪽을 닫아야, 자식이 끝났을 때 read가 EOF(0)를 받음 */
    close(pipefd[1]);
    pipefd[1] = -1;

    deadline = now_ms() + (long long)timeout_sec * 1000;

    /* 1. 출력을 EOF까지 모음. poll로 남은 시간만큼만 기다림 */
    while (1) {
        long long remain =
            deadline -
            now_ms(); /* 매번 다시 계산: 여러 번 깨어나도 총 시간은 그대로 */
        struct pollfd pfd = {pipefd[0], POLLIN, 0};
        ssize_t n;
        int r;

        if (remain <= 0) {
            child_kill(pid);
            pid = -1;
            result = 504;
            goto cleanup;
        }
        r = poll(&pfd, 1, (int)remain);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            child_kill(pid);
            pid = -1;
            goto cleanup;
        }
        if (r == 0) { /* 시간 끝: 위에서 504 처리 */
            continue;
        }

        if (out->len ==
            cap) { /* 버퍼를 두 배로. out_max + 1까지만 키워 초과를 알아냄 */
            size_t ncap = cap == 0 ? 4096 : cap * 2;
            char *p;

            if (ncap > out_max + 1) {
                ncap = out_max + 1;
            }
            p = realloc(out->data, ncap);
            if (p == NULL) {
                child_kill(pid);
                pid = -1;
                goto cleanup;
            }
            out->data = p;
            cap = ncap;
        }
        n = read(pipefd[0], out->data + out->len, cap - out->len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            child_kill(pid);
            pid = -1;
            goto cleanup;
        }
        if (n == 0) { /* EOF: 자식이 stdout을 닫음 (보통 끝났다는 뜻) */
            break;
        }
        out->len += (size_t)n;
        if (out->len > out_max) { /* 출력 상한 초과 */
            child_kill(pid);
            pid = -1;
            goto cleanup;
        }
    }

    /* 2. EOF 뒤에도 마감을 지킴: stdout만 닫고 계속 도는 CGI가 있으면
     * 막히는 waitpid(0)에서 작업자가 묶이므로, WNOHANG으로 짧게 확인을 반복 */
    while (1) {
        pid_t r = waitpid_retry(pid, &wstatus, WNOHANG);

        if (r == pid) {
            pid = -1; /* 회수함 */
            break;
        }
        if (r < 0) { /* 회수할 수 없음 (일어나지 않아야 함) */
            pid = -1;
            goto cleanup;
        }
        if (now_ms() >= deadline) {
            child_kill(pid);
            pid = -1;
            result = 504;
            goto cleanup;
        }
        sleep_ms(10);
    }

    /* 3. 종료 상태: 0으로 정상 종료했을 때만 성공. exec 실패(127), 1, 시그널 → 500 */
    if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0) {
        result = 0;
    }

cleanup: /* 모든 경로가 여기서 fd와 버퍼를 정리 */
    if (pipefd[0] >= 0) {
        close(pipefd[0]);
    }
    if (pipefd[1] >= 0) {
        close(pipefd[1]);
    }
    if (pid > 0) { /* fork는 됐는데 아직 회수 안 함 */
        child_kill(pid);
    }
    if (result != 0) { /* 실패하면 모은 출력은 쓰지 않음 */
        cgi_output_free(out);
    }
    return result;
}

void cgi_output_free(struct cgi_output *out)
{
    free(out->data);
    out->data = NULL;
    out->len = 0;
}

/* 헤더 이름(길이 nlen)이 name과 같은지 (대소문자 무시) */
static int name_is(const char *s, size_t nlen, const char *name)
{
    return strlen(name) == nlen && strncasecmp(s, name, nlen) == 0;
}

/* header 버퍼 끝에 이어 쓰기. 넘치면 -1 */
__attribute__((format(printf, 4, 5))) static int
append(char *header, size_t cap, size_t *pos, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(header + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *pos) {
        return -1;
    }
    *pos += (size_t)n;
    return 0;
}

int cgi_response_build(const char *data, size_t len, int keep_alive,
                       char *header, size_t header_cap, int *status,
                       const char **body, size_t *body_len)
{
    const char *crlf2 = NULL, *lf2 = NULL, *hend, *bstart;
    const char *p;
    const char *reason = NULL;
    size_t reason_len = 0;
    size_t pos = 0;
    char lines[8192]; /* CGI 헤더 줄을 모아 둠 (상태 줄을 먼저 써야 하므로) */
    size_t lpos = 0;

    *status = 200;
    if (len == 0) { /* 아무것도 출력하지 않음 */
        return 500;
    }

    /* 1. 헤더와 본문을 나누는 첫 빈 줄: "\r\n\r\n" 또는 "\n\n" 중 먼저 나오는 것 */
    for (size_t i = 0; i + 1 < len; i++) {
        if (!lf2 && data[i] == '\n' && data[i + 1] == '\n') {
            lf2 = data + i;
        }
        if (!crlf2 && i + 3 < len && memcmp(data + i, "\r\n\r\n", 4) == 0) {
            crlf2 = data + i;
        }
        if (lf2 || crlf2) {
            break;
        }
    }
    if (crlf2 && (!lf2 || crlf2 < lf2)) {
        hend = crlf2;
        bstart = crlf2 + 4;
    } else if (lf2) {
        hend = lf2;
        bstart = lf2 + 2;
    } else {
        return 500; /* 빈 줄 없음: 헤더 없이 본문만 출력한 CGI */
    }

    /* 2. 헤더 줄 하나씩: "이름: 값" */
    p = data;
    while (p < hend) {
        const char *nl = memchr(p, '\n', (size_t)(hend - p));
        const char *eol = nl ? nl : hend;
        const char *colon, *v, *vend;
        size_t nlen;

        vend = eol;
        if (vend > p && vend[-1] == '\r') {
            vend--;
        }
        colon = memchr(p, ':', (size_t)(vend - p));
        if (colon == NULL || colon == p) { /* "이름: 값" 모양이 아님 */
            return 500;
        }
        nlen = (size_t)(colon - p);
        v = colon + 1;
        while (v < vend && (*v == ' ' || *v == '\t')) {
            v++;
        }

        if (name_is(p, nlen, "Status")) {
            /* "404 Not Found": 세 자리 숫자 + (선택) 이유 문구 */
            if (vend - v < 3 || !isdigit((unsigned char)v[0]) ||
                !isdigit((unsigned char)v[1]) ||
                !isdigit((unsigned char)v[2]) ||
                (vend - v > 3 && v[3] != ' ')) {
                return 500;
            }
            *status = (v[0] - '0') * 100 + (v[1] - '0') * 10 + (v[2] - '0');
            if (*status < 200 || *status > 599) {
                return 500;
            }
            if (vend - v > 4) {
                reason = v + 4;
                reason_len = (size_t)(vend - reason);
            }
        } else if (!name_is(p, nlen, "Content-Length") &&
                   !name_is(p, nlen, "Connection") &&
                   !name_is(p, nlen, "Transfer-Encoding")) {
            /* 서버가 정하는 세 헤더 말고는 그대로 전달 */
            size_t l = (size_t)(vend - p);
            if (lpos + l + 2 > sizeof(lines)) {
                return 500;
            }
            memcpy(lines + lpos, p, l);
            memcpy(lines + lpos + l, "\r\n", 2);
            lpos += l + 2;
        }
        p = nl ? nl + 1 : hend;
    }

    *body = bstart;
    *body_len = (size_t)(data + len - bstart);

    /* 3. 응답 헤더: 상태 줄 + Server + CGI 헤더 + 서버가 센 길이 + Connection */
    if (reason == NULL) {
        reason = http_status_text(*status);
        reason_len = strlen(reason);
    }
    if (append(header, header_cap, &pos, "HTTP/1.1 %d %.*s\r\n", *status,
               (int)reason_len, reason) < 0 ||
        append(header, header_cap, &pos, "Server: Tiny Web Server\r\n") < 0 ||
        append(header, header_cap, &pos, "%.*s", (int)lpos, lines) < 0 ||
        append(header, header_cap, &pos,
               "Content-Length: %zu\r\nConnection: %s\r\n\r\n", *body_len,
               keep_alive ? "keep-alive" : "close") < 0) {
        return 500;
    }
    return 0;
}
