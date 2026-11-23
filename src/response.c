#include "response.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "io.h"

/* 상태 줄과 헤더를 담는 버퍼 크기입니다. */
#define RESPONSE_HEADER_SIZE 512

/* 오류 응답 본문을 담는 버퍼 크기입니다. 본문에는 고정 문구만 들어갑니다. */
#define RESPONSE_ERROR_BODY_SIZE 1024

/* 파일 본문을 한 번에 읽고 쓰는 크기입니다. */
#define FILE_CHUNK_SIZE 8192

static int file_open_error_send(int fd, int err, const char *path);
static int file_send(int fd, int file_fd, off_t size, const char *path);
static const char *mime_type_get(const char *path);
static int header_send(int fd, int status, const char *reason, long long length,
                       const char *type);

/*
 * 오류 응답을 보냅니다. 본문에는 요청 내용을 넣지 않습니다.
 * 다 보내면 0, 쓰기가 실패하면 -1입니다.
 */
int response_send_error(int fd, int status, const char *reason,
                        const char *message)
{
    char body[RESPONSE_ERROR_BODY_SIZE];
    int len = snprintf(body, sizeof(body),
                       "<html><head><title>%d %s</title></head>\r\n"
                       "<body>\r\n<h1>%d %s</h1>\r\n<p>%s</p>\r\n"
                       "</body></html>\r\n",
                       status, reason, status, reason, message);

    if (len < 0 || (size_t)len >= sizeof(body)) {
        return -1;
    }
    if (header_send(fd, status, reason, len, "text/html") < 0 ||
        io_write_all(fd, body, (size_t)len) < 0) {
        return -1;
    }
    return 0;
}

/*
 * path의 파일을 200으로 보냅니다. 파일이 없으면 404, 읽을 수 없거나 일반 파일이
 * 아니면 403을 보냅니다. 응답을 끝까지 보냈으면 0, 쓰기가 실패했거나 전송 중
 * 파일이 줄었거나 열기가 다른 이유로 실패해서 연결을 닫아야 하면 -1입니다.
 * Content-Length는 연 파일의 fstat 크기이고, 그만큼만 보냅니다.
 */
int response_send_static(int fd, const char *path)
{
    struct stat st;
    int file_fd = open(path, O_RDONLY);
    int rc;

    if (file_fd < 0) {
        return file_open_error_send(fd, errno, path);
    }
    if (fstat(file_fd, &st) < 0) {
        fprintf(stderr, "response: fstat(%s): %s\n", path, strerror(errno));
        close(file_fd);
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        close(file_fd);
        return response_send_error(fd, 403, "Forbidden",
                                   "The file is not a regular file.");
    }

    rc = file_send(fd, file_fd, st.st_size, path);
    close(file_fd);
    return rc;
}

/* open 실패 이유에 맞는 오류 응답을 보냅니다. 500은 M5에서 추가합니다. */
static int file_open_error_send(int fd, int err, const char *path)
{
    switch (err) {
    case ENOENT:
    case ENOTDIR:
    case ENAMETOOLONG:
        return response_send_error(fd, 404, "Not Found",
                                   "The file was not found.");
    case EACCES:
        return response_send_error(fd, 403, "Forbidden",
                                   "The file cannot be read.");
    default:
        fprintf(stderr, "response: open(%s): %s\n", path, strerror(err));
        return -1;
    }
}

/* 헤더를 보낸 뒤 파일 본문을 size바이트만큼 보냅니다. */
static int file_send(int fd, int file_fd, off_t size, const char *path)
{
    char chunk[FILE_CHUNK_SIZE];
    off_t remaining = size;

    if (header_send(fd, 200, "OK", (long long)size, mime_type_get(path)) < 0) {
        return -1;
    }
    while (remaining > 0) {
        size_t want = remaining < (off_t)sizeof(chunk) ? (size_t)remaining
                                                       : sizeof(chunk);
        ssize_t n = read(file_fd, chunk, want);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            fprintf(stderr, "response: read(%s): %s\n", path, strerror(errno));
            return -1;
        }
        if (n == 0) {
            /* 이미 보낸 Content-Length를 채울 수 없으니 연결을 닫습니다. */
            fprintf(stderr, "response: %s shrank while sending\n", path);
            return -1;
        }
        if (io_write_all(fd, chunk, (size_t)n) < 0) {
            return -1;
        }
        remaining -= n;
    }
    return 0;
}

/* 마지막 구간의 확장자로 MIME 타입을 정합니다. 모르면 text/plain입니다. */
static const char *mime_type_get(const char *path)
{
    const char *name = strrchr(path, '/');
    const char *dot = strrchr(name != NULL ? name : path, '.');

    if (dot == NULL) {
        return "text/plain";
    }
    if (strcmp(dot, ".html") == 0) {
        return "text/html";
    }
    if (strcmp(dot, ".gif") == 0) {
        return "image/gif";
    }
    if (strcmp(dot, ".png") == 0) {
        return "image/png";
    }
    if (strcmp(dot, ".jpg") == 0) {
        return "image/jpeg";
    }
    return "text/plain";
}

static int header_send(int fd, int status, const char *reason, long long length,
                       const char *type)
{
    char header[RESPONSE_HEADER_SIZE];
    int len = snprintf(header, sizeof(header),
                       "HTTP/1.0 %d %s\r\n"
                       "Content-Length: %lld\r\n"
                       "Content-Type: %s\r\n"
                       "\r\n",
                       status, reason, length, type);

    if (len < 0 || (size_t)len >= sizeof(header)) {
        return -1;
    }
    return io_write_all(fd, header, (size_t)len);
}
