#ifndef IO_H
#define IO_H

#include <stddef.h>
#include <sys/types.h>

#include "csapp.h"

/*
 * 연결 하나에서 줄 단위로 읽기 위한 버퍼 상태입니다. 요청 라인과 헤더를 읽는
 * 동안 같은 reader를 써야 합니다. 요청 라인을 읽을 때 헤더 일부가 이미 버퍼에
 * 들어와 있기 때문입니다. 지금은 안쪽을 csapp의 rio로 채웁니다. 호출부는
 * io_ 함수만 쓰므로 rio를 바꿔도 이 구조체 안쪽만 바뀝니다.
 */
struct io_reader {
    rio_t rio;
};

void io_init(struct io_reader *reader, int fd);
ssize_t io_read_line(struct io_reader *reader, char *buf, size_t size);
int io_write_all(int fd, const void *buf, size_t len);

#endif
