/*
 * adder - 두 수를 더하는 CGI 프로그램입니다. Tiny의 adder를 대신합니다.
 *   GET:  서버가 환경 변수 QUERY_STRING에 "1&2"처럼 넘겨주면 1 + 2를 계산합니다.
 *   POST: [M5-2] 표준 입력에서 CONTENT_LENGTH바이트를 읽어 같은 방식으로 계산합니다.
 *   입력 형식은 "1&2"와 "a=1&b=2"(폼 형식, '=' 앞 이름은 무엇이든) 둘 다 받습니다.
 *   표준 출력이 클라이언트 소켓으로 연결되어 있으므로(서버의 dup2),
 *   printf로 쓴 헤더와 본문이 그대로 응답이 됩니다.
 *   상태 줄과 Server 헤더는 서버가 먼저 보냅니다.
 *   [M5-2] Connection, Content-length는 서버가 정하므로 출력하지 않습니다
 *   (M5-3에서 서버가 출력을 모아 길이를 셉니다). 지금은 서버가 응답 뒤 연결을
 *   닫으므로, 클라이언트는 연결이 닫힐 때 본문이 끝난 것을 압니다.
 *   [M5-2] 입력이 없거나, 숫자가 아니거나, 범위를 넘으면 안내 메시지를 출력합니다.
 */
#include "csapp.h"

#include <limits.h>

#define INPUT_MAX 1024 /* 두 수를 담기에 충분한 입력 길이. 넘으면 안내 메시지 */

/* "1" 또는 "a=1" 한 조각을 정수로. '=' 뒤가 숫자. 성공 0, 형식 오류 -1, 범위 초과 -2 */
static int number_parse(const char *s, long *out)
{
    const char *eq = strchr(s, '=');
    char *end;
    long v;

    if (eq != NULL) { /* 폼 형식: 이름은 보지 않고 값만 */
        s = eq + 1;
    }
    if (*s == '\0') {
        return -1;
    }
    errno = 0;
    v = strtol(s, &end, 10);
    if (*end != '\0') { /* "12x", "abc" */
        return -1;
    }
    if (errno == ERANGE || v < INT_MIN || v > INT_MAX) {
        return -2;
    }
    *out = v;
    return 0;
}

/* POST 본문을 stdin에서 buf로. 성공하면 읽은 길이, 너무 길면 -2, 읽기 실패 -1 */
static long body_read(char *buf, size_t cap)
{
    const char *len_str = getenv("CONTENT_LENGTH");
    long len = len_str ? atol(len_str) : 0;
    long got = 0;

    if (len < 0 || (size_t)len >= cap) {
        return -2;
    }
    while (got < len) { /* read는 일부만 돌려줄 수 있으므로 다 읽을 때까지 */
        ssize_t n = read(STDIN_FILENO, buf + got, (size_t)(len - got));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        got += n;
    }
    buf[got] = '\0';
    return got;
}

int main(void)
{
    char content[MAXLINE];
    char input[INPUT_MAX];
    const char *method = getenv("REQUEST_METHOD");
    const char *msg = NULL; /* 안내 메시지. NULL이면 계산 결과를 출력 */
    char *amp;
    long n1 = 0;
    long n2 = 0;
    int r1, r2;

    /* 1. 입력 가져오기: POST는 본문, 그 밖은 쿼리 문자열 */
    if (method != NULL && strcmp(method, "POST") == 0) {
        long got = body_read(input, sizeof(input));
        if (got == -2) {
            msg = "The input is too long.";
        } else if (got < 0) {
            msg = "Couldn't read the request body.";
        }
    } else {
        const char *query = getenv("QUERY_STRING");
        if (query == NULL || strlen(query) >= sizeof(input)) {
            msg = "The input is too long.";
        } else {
            strcpy(input, query); /* 길이를 위에서 확인함 */
        }
    }

    /* 2. "1&2"를 '&'에서 정확히 두 조각으로 나눠 숫자로 바꾼다.
     * Tiny는 '&'가 없으면 NULL을 역참조해 죽었음 */
    if (msg == NULL) {
        amp = strchr(input, '&');
        if (input[0] == '\0') {
            msg = "Give two numbers, e.g. /cgi-bin/adder?1&2 or POST a=1&b=2";
        } else if (amp == NULL || strchr(amp + 1, '&') != NULL) {
            msg = "Give exactly two numbers separated by '&'.";
        } else {
            *amp = '\0';
            r1 = number_parse(input, &n1);
            r2 = number_parse(amp + 1, &n2);
            if (r1 == -2 || r2 == -2) {
                msg = "Each number must be between -2147483648 and 2147483647.";
            } else if (r1 < 0 || r2 < 0) {
                msg = "Both values must be whole numbers.";
            }
        }
    }

    /* 3. 응답 본문. 두 수가 int 범위라 합은 long에 들어감 */
    if (msg != NULL) {
        snprintf(content, sizeof(content),
                 "Welcome to add.com: THE Internet addition portal.\r\n<p>"
                 "%s\r\n<p>"
                 "Thanks for visiting!\r\n",
                 msg);
    } else {
        snprintf(content, sizeof(content),
                 "Welcome to add.com: THE Internet addition portal.\r\n<p>"
                 "The answer is: %ld + %ld = %ld\r\n<p>"
                 "Thanks for visiting!\r\n",
                 n1, n2, n1 + n2);
    }

    /* 4. 헤더(Content-type만)와 빈 줄, 본문 */
    printf("Content-type: text/html\r\n\r\n");
    printf("%s", content);
    fflush(stdout);
    exit(0);
}
