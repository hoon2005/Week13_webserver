#ifndef NET_H
#define NET_H

#include <sys/socket.h>

/* listen()에 요청하는 대기열 크기입니다. 커널이 SOMAXCONN 이하로 줄일 수 있습니다. */
#define NET_LISTEN_BACKLOG 1024

int net_open_listen(const char *port);
int net_accept(int listen_fd, struct sockaddr_storage *addr,
               socklen_t *addr_len);

#endif
