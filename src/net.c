#include "net.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* 자원 부족이나 알 수 없는 오류로 accept가 실패했을 때 다시 시도하기 전에 쉬는 시간 */
#define ACCEPT_RETRY_DELAY_SEC 1

/*
 * 숫자 주소를 글자로 바꿀 때 쓰는 버퍼 크기입니다. IPv6 숫자 주소는 46바이트
 * (INET6_ADDRSTRLEN)이고 scope id가 붙을 수 있어 여유를 둡니다. 더 길면
 * getnameinfo가 실패하고 "?"로 찍힙니다. 최종 글자는 "[호스트]:포트"입니다.
 */
#define ADDRESS_HOST_SIZE 64
#define ADDRESS_PORT_SIZE 16
#define ADDRESS_TEXT_SIZE (ADDRESS_HOST_SIZE + ADDRESS_PORT_SIZE + 4)

enum accept_action {
    ACCEPT_RETRY,           /* 기록 없이 바로 다시 시도 */
    ACCEPT_LOG_RETRY,       /* 기록하고 바로 다시 시도 */
    ACCEPT_LOG_DELAY_RETRY, /* 기록하고 잠깐 쉰 뒤 다시 시도 */
    ACCEPT_GIVE_UP,         /* listen 소켓 자체가 잘못됨 */
};

static int listen_socket_open(const struct addrinfo *candidate);
static void address_format(const struct sockaddr *addr, socklen_t addr_len,
                           char *text, size_t text_size);
static const char *family_name(int family);
static enum accept_action accept_error_action(int err);

/*
 * port에서 접속을 기다리는 소켓을 열어 fd를 돌려줍니다. 실패하면 -1입니다.
 * getaddrinfo가 준 후보 주소를 순서대로 시도하고, socket부터 listen까지
 * 모두 성공한 첫 주소를 씁니다. 어느 주소에 열었는지 stderr에 한 줄 남깁니다.
 */
int net_open_listen(const char *port)
{
    struct addrinfo hints;
    struct addrinfo *candidates;
    struct addrinfo *candidate;
    char address[ADDRESS_TEXT_SIZE];
    int listen_fd = -1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG | AI_NUMERICSERV;

    rc = getaddrinfo(NULL, port, &hints, &candidates);
    if (rc != 0) {
        fprintf(stderr, "net: getaddrinfo(port %s): %s\n", port,
                gai_strerror(rc));
        return -1;
    }

    for (candidate = candidates; candidate != NULL;
         candidate = candidate->ai_next) {
        listen_fd = listen_socket_open(candidate);
        if (listen_fd >= 0) {
            break;
        }
    }

    if (listen_fd >= 0) {
        address_format(candidate->ai_addr, candidate->ai_addrlen, address,
                       sizeof(address));
        fprintf(stderr, "net: listening on %s (%s)\n", address,
                family_name(candidate->ai_family));
    } else {
        fprintf(stderr, "net: no usable address for port %s\n", port);
    }

    freeaddrinfo(candidates);
    return listen_fd;
}

/*
 * 접속 하나를 받아 연결 fd를 돌려주고, 손님 주소를 addr와 addr_len에 씁니다.
 * accept가 손님 쪽 문제나 일시적인 자원 부족으로 실패하면 기록하고 다시
 * 시도합니다. 자원 부족과 알 수 없는 오류는 ACCEPT_RETRY_DELAY_SEC초 쉬고
 * 다시 시도하므로, 이 함수는 그만큼 늦게 돌아올 수 있습니다.
 * listen 소켓 자체가 잘못된 경우에만 -1을 돌려줍니다.
 */
int net_accept(int listen_fd, struct sockaddr_storage *addr,
               socklen_t *addr_len)
{
    for (;;) {
        socklen_t len = sizeof(*addr);
        int conn_fd = accept(listen_fd, (struct sockaddr *)addr, &len);
        int err;

        if (conn_fd >= 0) {
            *addr_len = len;
            return conn_fd;
        }

        err = errno;
        switch (accept_error_action(err)) {
        case ACCEPT_RETRY:
            break;
        case ACCEPT_LOG_RETRY:
            fprintf(stderr, "net: accept: %s (retrying)\n", strerror(err));
            break;
        case ACCEPT_LOG_DELAY_RETRY:
            fprintf(stderr, "net: accept: %s (retrying in %d s)\n",
                    strerror(err), ACCEPT_RETRY_DELAY_SEC);
            sleep(ACCEPT_RETRY_DELAY_SEC);
            break;
        case ACCEPT_GIVE_UP:
            fprintf(stderr, "net: accept: %s\n", strerror(err));
            return -1;
        }
    }
}

/*
 * 후보 주소 하나로 socket, setsockopt, bind, listen을 차례로 합니다.
 * 모두 성공하면 fd를, 하나라도 실패하면 연 소켓을 닫고 -1을 돌려줍니다.
 */
static int listen_socket_open(const struct addrinfo *candidate)
{
    char address[ADDRESS_TEXT_SIZE];
    const char *step;
    int optval = 1;
    int fd;

    address_format(candidate->ai_addr, candidate->ai_addrlen, address,
                   sizeof(address));

    fd = socket(candidate->ai_family, candidate->ai_socktype,
                candidate->ai_protocol);
    if (fd < 0) {
        fprintf(stderr, "net: socket(%s): %s\n", address, strerror(errno));
        return -1;
    }

    /* 서버를 끈 직후 같은 포트에 TIME_WAIT 연결이 남아 있어도 bind되게 합니다. */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        step = "setsockopt(SO_REUSEADDR)";
    } else if (bind(fd, candidate->ai_addr, candidate->ai_addrlen) < 0) {
        step = "bind";
    } else if (listen(fd, NET_LISTEN_BACKLOG) < 0) {
        step = "listen";
    } else {
        return fd;
    }

    fprintf(stderr, "net: %s(%s): %s\n", step, address, strerror(errno));
    close(fd);
    return -1;
}

/* 주소를 숫자로만 바꿉니다. DNS 조회를 하지 않습니다. 실패하면 "?"입니다. */
static void address_format(const struct sockaddr *addr, socklen_t addr_len,
                           char *text, size_t text_size)
{
    char host[ADDRESS_HOST_SIZE];
    char port[ADDRESS_PORT_SIZE];

    if (getnameinfo(addr, addr_len, host, sizeof(host), port, sizeof(port),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        snprintf(text, text_size, "?");
        return;
    }
    if (addr->sa_family == AF_INET6) {
        snprintf(text, text_size, "[%s]:%s", host, port);
    } else {
        snprintf(text, text_size, "%s:%s", host, port);
    }
}

static const char *family_name(int family)
{
    if (family == AF_INET) {
        return "IPv4";
    }
    if (family == AF_INET6) {
        return "IPv6";
    }
    return "other";
}

/*
 * accept가 실패한 errno를 보고 다음 행동을 정합니다.
 * 연결별 네트워크 오류 목록은 Linux accept(2)의 RETURN VALUE 절을 따릅니다.
 * 이 오류들은 새 연결에 이미 생긴 오류를 accept가 대신 알려 준 것이라서
 * listen 소켓은 멀쩡합니다.
 */
static enum accept_action accept_error_action(int err)
{
    switch (err) {
    case EINTR:
        return ACCEPT_RETRY;
    case ECONNABORTED:
    case ENETDOWN:
    case EPROTO:
    case ENOPROTOOPT:
    case EHOSTUNREACH:
    case EOPNOTSUPP:
    case ENETUNREACH:
#ifdef EHOSTDOWN
    case EHOSTDOWN:
#endif
#ifdef ENONET
    case ENONET: /* Linux에만 있습니다 */
#endif
        return ACCEPT_LOG_RETRY;
    case EBADF:
    case EINVAL:
    case ENOTSOCK:
        return ACCEPT_GIVE_UP;
    default:
        /* EMFILE, ENFILE, ENOBUFS, ENOMEM과 알 수 없는 오류 */
        return ACCEPT_LOG_DELAY_RETRY;
    }
}
