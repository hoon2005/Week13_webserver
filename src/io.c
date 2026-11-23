#include "io.h"

void io_init(struct io_reader *reader, int fd)
{
    rio_readinitb(&reader->rio, fd);
}

/*
 * 줄 하나를 '\n'까지 읽어 buf에 넣고 끝에 '\0'을 붙입니다. 돌려주는 값은
 * '\n'을 포함한 길이입니다. 그래서 한 줄은 최대 size - 1바이트입니다.
 * 상대가 아무것도 보내지 않고 닫았으면 0을 돌려줍니다.
 * 읽기가 실패했거나, 줄이 size - 1바이트 안에서 '\n'으로 끝나지 않았으면
 * (너무 길거나 중간에 끊김) -1을 돌려줍니다. 이때 buf 내용은 쓰지 않습니다.
 */
ssize_t io_read_line(struct io_reader *reader, char *buf, size_t size)
{
    ssize_t n = rio_readlineb(&reader->rio, buf, size);

    if (n <= 0) {
        return n;
    }
    if (buf[n - 1] != '\n') {
        return -1;
    }
    return n;
}

/* len바이트를 모두 씁니다. 다 쓰면 0, 쓰기가 실패하면 -1입니다. */
int io_write_all(int fd, const void *buf, size_t len)
{
    if (rio_writen(fd, (void *)buf, len) != (ssize_t)len) {
        return -1;
    }
    return 0;
}
