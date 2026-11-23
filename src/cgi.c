#include "cgi.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "io.h"
#include "response.h"

extern char **environ;

enum program_status {
    PROGRAM_OK,
    PROGRAM_NOT_FOUND,
    PROGRAM_FORBIDDEN,
    PROGRAM_ERROR,
};

static enum program_status program_check(const char *path);
static void child_exec(int fd, const char *path, const char *args);
static int child_wait(pid_t pid, const char *path);

/*
 * path의 CGI 프로그램을 실행하고 그 출력을 손님에게 그대로 보냅니다.
 * 프로그램이 없으면 404, 일반 파일이 아니거나 실행 권한이 없으면 403입니다.
 *
 * 상태 줄은 fork 전에 서버가 보냅니다. fork 뒤에 보내면 부모와 자식이 같은
 * 소켓에 동시에 써서 출력 순서가 섞일 수 있습니다. 그래서 프로그램이 실패해도
 * 손님은 이미 200을 받은 상태로 응답이 끊길 수 있습니다(500은 M5에서 다룹니다).
 *
 * 프로그램을 실행해서 끝날 때까지 기다렸거나 오류 응답을 보냈으면 0,
 * 쓰기·fork·wait가 실패해서 연결을 닫아야 하면 -1입니다.
 */
int cgi_run(int fd, const char *path, const char *args)
{
    static const char status_line[] = "HTTP/1.0 200 OK\r\n";
    pid_t pid;

    switch (program_check(path)) {
    case PROGRAM_OK:
        break;
    case PROGRAM_NOT_FOUND:
        return response_send_error(fd, 404, "Not Found",
                                   "The CGI program was not found.");
    case PROGRAM_FORBIDDEN:
        return response_send_error(fd, 403, "Forbidden",
                                   "The CGI program cannot be run.");
    case PROGRAM_ERROR:
        return -1;
    }

    if (io_write_all(fd, status_line, sizeof(status_line) - 1) < 0) {
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "cgi: fork: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        child_exec(fd, path, args);
    }
    return child_wait(pid, path);
}

/* 실행할 수 있는 일반 파일인지 Tiny와 같은 기준(소유자 실행 권한)으로 봅니다. */
static enum program_status program_check(const char *path)
{
    struct stat st;

    if (stat(path, &st) < 0) {
        if (errno == ENOENT || errno == ENOTDIR || errno == ENAMETOOLONG) {
            return PROGRAM_NOT_FOUND;
        }
        if (errno == EACCES) {
            return PROGRAM_FORBIDDEN;
        }
        fprintf(stderr, "cgi: stat(%s): %s\n", path, strerror(errno));
        return PROGRAM_ERROR;
    }
    if (!S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) {
        return PROGRAM_FORBIDDEN;
    }
    return PROGRAM_OK;
}

/*
 * 자식 프로세스에서 실행합니다. 돌아오지 않습니다.
 * 준비 단계나 execve가 실패하면 exit 대신 _exit로 끝냅니다. exit는 부모에게서
 * 물려받은 stdio 버퍼를 비우면서 같은 출력을 두 번 쓸 수 있습니다.
 * listen 소켓은 FD_CLOEXEC라서 execve가 성공하는 순간 닫힙니다.
 */
static void child_exec(int fd, const char *path, const char *args)
{
    char *argv[] = {(char *)path, NULL};
    const char *step = NULL;

    /* 서버가 무시하는 SIGPIPE를 CGI 프로그램에서는 기본 동작으로 되돌립니다. */
    if (signal(SIGPIPE, SIG_DFL) == SIG_ERR) {
        step = "signal(SIGPIPE, SIG_DFL)";
    } else if (setenv("QUERY_STRING", args, 1) < 0) {
        step = "setenv(QUERY_STRING)";
    } else if (dup2(fd, STDOUT_FILENO) < 0) {
        step = "dup2";
    }
    if (step != NULL) {
        fprintf(stderr, "cgi: %s: %s\n", step, strerror(errno));
        _exit(1);
    }
    /* 출력은 stdout으로 가니, 같은 연결을 가리키는 원래 fd는 닫습니다. */
    if (fd != STDOUT_FILENO) {
        close(fd);
    }

    execve(path, argv, environ);
    fprintf(stderr, "cgi: execve(%s): %s\n", path, strerror(errno));
    _exit(1);
}

/* 그 자식만 기다립니다. 시그널로 중단되면 다시 기다립니다. */
static int child_wait(pid_t pid, const char *path)
{
    int status;

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "cgi: waitpid(%s): %s\n", path, strerror(errno));
            return -1;
        }
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "cgi: %s killed by signal %d\n", path,
                WTERMSIG(status));
    } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        fprintf(stderr, "cgi: %s exited with %d\n", path, WEXITSTATUS(status));
    }
    return 0;
}
