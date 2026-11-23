/* CS:APP open_listenfd()를 대신하는 우리 소켓 코드입니다.
 * [개선 1] setsockopt 반환값 확인 [개선 2] 에러 경로에서도 목록 해제
 * [개선 3] 실패 원인(errno)을 보존해 출력하고 호출자에게 돌려줌
 * [개선 4] accept가 실패해도 서버를 종료하지 않음 (net_accept) */
#include "net.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* listen()의 backlog: handshake가 끝난 연결을 쌓아 두는 큐의 길이 */
#define LISTEN_BACKLOG 1024

int net_listen(const char *port)
{
    struct addrinfo hints, *listp, *p;
    int fd = -1;
    int optval = 1;
    int rc;
    int saved_errno = 0;

    /* 모든 IP에서 받을 TCP 주소 후보 목록을 받습니다 */
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG | AI_NUMERICSERV;
    rc = getaddrinfo(NULL, port, &hints, &listp);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo 실패 (port %s): %s\n", port,
                gai_strerror(rc));
        return -1;
    }

    /* 후보를 하나씩 socket → setsockopt → bind 해 보고, 성공하면 멈춥니다 */
    for (p = listp; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            saved_errno = errno;
            continue;
        }

        /* [개선 1] 원본은 반환값을 안 봤습니다. SO_REUSEADDR 없이 진행하면
         * 재시작 때 bind 실패 원인을 찾기 어려우므로 이 후보는 버립니다 */
        rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
        if (rc < 0) {
            saved_errno = errno;
            fprintf(stderr, "setsockopt(SO_REUSEADDR) 실패: %s\n",
                    strerror(saved_errno));
            close(fd);
            continue;
        }

        if (bind(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        /* [개선 3] 뒤의 close/freeaddrinfo가 errno를 덮어쓸 수 있어
         * bind 실패 원인(예: EADDRINUSE)을 바로 저장합니다 */
        saved_errno = errno;
        if (close(fd) < 0) {
            fprintf(stderr, "close 실패: %s\n", strerror(errno));
            /* [개선 2] 원본은 여기서 해제 없이 return해 목록이 누수됐습니다 */
            freeaddrinfo(listp);
            errno = saved_errno;
            return -1;
        }
    }

    freeaddrinfo(listp);
    if (!p) {
        fprintf(stderr, "port %s에 bind하지 못했습니다: %s\n", port,
                strerror(saved_errno));
        errno = saved_errno;
        return -1;
    }

    if (listen(fd, LISTEN_BACKLOG) < 0) {
        saved_errno = errno;
        fprintf(stderr, "listen 실패: %s\n", strerror(saved_errno));
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

int net_accept(int listen_fd, struct sockaddr *addr, socklen_t *addrlen)
{
    /* addrlen은 입력(버퍼 크기)이자 출력(실제 주소 길이)이라 재시도마다
     * 처음 값으로 되돌립니다 */
    socklen_t addrlen_in = addrlen ? *addrlen : 0;
    int fd;

    for (;;) {
        fd = accept(listen_fd, addr, addrlen);
        if (fd >= 0) {
            return fd;
        }
        /* [개선 4] CS:APP Accept는 실패하면 exit해 서버 전체가 죽습니다.
         * 시그널로 끊긴 경우는 다시 기다리고, 그 밖의 실패(예: fd 부족
         * EMFILE, 연결 중단 ECONNABORTED)는 알리고 이 연결만 포기합니다 */
        if (errno == EINTR) {
            if (addrlen) {
                *addrlen = addrlen_in;
            }
            continue;
        }
        int saved_errno = errno;
        fprintf(stderr, "accept 실패: %s\n", strerror(saved_errno));
        errno = saved_errno;
        return -1;
    }
}
