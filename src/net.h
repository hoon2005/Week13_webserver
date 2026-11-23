#ifndef NET_H
#define NET_H

#include <sys/socket.h>

/*
 * port(예: "8000")에서 연결을 받는 listen 소켓을 열고 그 fd를 돌려줍니다.
 * socket → setsockopt(SO_REUSEADDR) → bind → listen까지 마친 상태입니다.
 * 실패하면 원인을 stderr에 출력하고 -1을 돌려줍니다. socket, bind, listen
 * 단계의 실패면 errno에 마지막 실패 원인이 남아 있습니다.
 */
int net_listen(const char *port);

/*
 * listen_fd의 Accept 큐에서 연결 하나를 꺼내 connfd를 돌려줍니다.
 * 큐가 비어 있으면 기다립니다. 시그널로 끊기면(EINTR) 다시 기다리고,
 * 그 밖의 실패는 원인을 stderr에 출력하고 -1을 돌려줍니다.
 * addr와 addrlen은 accept()와 같이 쓰며, 필요 없으면 NULL을 넘깁니다.
 */
int net_accept(int listen_fd, struct sockaddr *addr, socklen_t *addrlen);

#endif
