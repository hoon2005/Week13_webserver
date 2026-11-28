/*
 * adder - 두 수를 더하는 CGI 프로그램입니다 (m4m5.md 4.5).
 *   GET:  QUERY_STRING "1&2" 또는 "a=1&b=2"
 *   POST: 표준 입력에서 CONTENT_LENGTH바이트 ("a=1&b=2" 또는 "1&2")
 *   출력은 CGI 형식: Content-type 헤더 + 빈 줄 + 본문.
 *   상태 줄, Content-Length, Connection은 서버가 씁니다.
 */
#include "csapp.h"

#include <limits.h>

#define INPUT_MAX 1024

/* "a=1" 또는 "1"에서 숫자를 꺼냅니다. 성공 0, 형식 오류나 범위 초과 -1 */
static int number_parse(const char *part, int *out)
{
    const char *eq = strchr(part, '=');
    const char *s = eq ? eq + 1 : part;
    char *end;
    long v;

    if (*s == '\0') {
        return -1;
    }
    errno = 0;
    v = strtol(s, &end, 10);
    if (*end != '\0' || errno == ERANGE || v < INT_MIN || v > INT_MAX) {
        return -1; /* 숫자가 아니거나 int를 넘는 값. 합의 범위는 main이 검사 (F7) */
    }
    *out = (int)v;
    return 0;
}

/* 입력 문자열을 얻습니다. POST면 표준 입력, 그 외에는 QUERY_STRING */
static const char *input_read(char *buf, size_t size)
{
    const char *method = getenv("REQUEST_METHOD");
    const char *len_str = getenv("CONTENT_LENGTH");
    const char *query = getenv("QUERY_STRING");
    size_t want, got = 0;

    if (method == NULL || strcmp(method, "POST") != 0) {
        return query ? query : "";
    }
    want = len_str ? (size_t)strtoul(len_str, NULL, 10) : 0;
    if (want > size - 1) {
        want = size - 1;
    }
    while (got < want) {
        ssize_t n = read(STDIN_FILENO, buf + got, want - got);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    buf[got] = '\0';
    /* 폼 전송 끝의 줄바꿈 제거 */
    while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r')) {
        buf[--got] = '\0';
    }
    return buf;
}

int main(void)
{
    char input[INPUT_MAX];
    char work[INPUT_MAX];
    char content[MAXLINE];
    const char *query = input_read(input, sizeof(input));
    char *amp;
    int n1, n2;
    long long sum;

    snprintf(work, sizeof(work), "%s", query);
    amp = strchr(work, '&');
    if (work[0] == '\0' || amp == NULL) {
        snprintf(content, sizeof(content),
                 "Welcome to add.com: THE Internet addition portal.\r\n<p>"
                 "Usage: /cgi-bin/adder?1&2 or POST a=1&b=2\r\n");
    } else {
        *amp = '\0';
        if (number_parse(work, &n1) < 0 || number_parse(amp + 1, &n2) < 0) {
            snprintf(content, sizeof(content),
                     "Welcome to add.com: THE Internet addition portal.\r\n<p>"
                     "Invalid number. Use integers like 1&2 or a=1&b=2\r\n");
        } else if ((sum = (long long)n1 + n2) < INT_MIN || sum > INT_MAX) {
            /* 넓은 타입으로 더해 넘침 없이 합을 검사함 (F7) */
            snprintf(content, sizeof(content),
                     "Welcome to add.com: THE Internet addition portal.\r\n<p>"
                     "The sum is out of range. Use a pair whose sum is "
                     "between %d and %d\r\n",
                     INT_MIN, INT_MAX);
        } else {
            snprintf(content, sizeof(content),
                     "Welcome to add.com: THE Internet addition portal.\r\n<p>"
                     "The answer is: %d + %d = %d\r\n<p>"
                     "Thanks for visiting!\r\n",
                     n1, n2, (int)sum);
        }
    }

    printf("Content-type: text/html\r\n\r\n");
    printf("%s", content);
    fflush(stdout);
    return 0;
}
