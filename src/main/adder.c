/*
 * QUERY_STRING "<정수>&<정수>"의 두 수를 더해 HTML로 돌려주는 CGI 프로그램입니다.
 * 입력이 잘못됐거나 합이 int 범위를 넘으면 안내 본문을 돌려줍니다.
 * 상태 줄은 서버가 먼저 보내므로 여기서는 헤더와 본문만 씁니다.
 */
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONTENT_SIZE 512

static bool args_parse(const char *query, int *a, int *b);
static bool number_parse(const char *start, const char *end, int *out);
static bool sum_fits(int a, int b);
static void page_print(const char *content);

int main(void)
{
    const char *query = getenv("QUERY_STRING");
    char content[CONTENT_SIZE];
    int a;
    int b;

    if (query != NULL && args_parse(query, &a, &b)) {
        snprintf(content, sizeof(content),
                 "<p>The answer is: %d + %d = %d</p>\r\n", a, b, a + b);
    } else {
        snprintf(content, sizeof(content),
                 "<p>Invalid input. Send two integers like "
                 "/cgi-bin/adder?1&amp;2 whose sum fits in an int.</p>\r\n");
    }
    page_print(content);
    return 0;
}

/* "<정수>&<정수>"를 a와 b로 읽습니다. 형식이 틀리거나 합이 넘치면 false입니다. */
static bool args_parse(const char *query, int *a, int *b)
{
    const char *amp = strchr(query, '&');

    if (amp == NULL) {
        return false;
    }
    return number_parse(query, amp, a) &&
           number_parse(amp + 1, amp + 1 + strlen(amp + 1), b) &&
           sum_fits(*a, *b);
}

/*
 * [start, end) 구간 전체가 10진 정수 하나이고 int 범위 안이면 out에 쓰고 true를
 * 돌려줍니다. 숫자를 하나도 못 읽었거나, end 전에 멈췄거나, 범위를 넘으면
 * false입니다.
 */
static bool number_parse(const char *start, const char *end, int *out)
{
    char *stop;
    long value;

    errno = 0;
    value = strtol(start, &stop, 10);
    if (stop == start || stop != end || errno == ERANGE) {
        return false;
    }
    if (value < INT_MIN || value > INT_MAX) {
        return false;
    }
    *out = (int)value;
    return true;
}

/* a + b를 계산하기 전에 int 범위 안인지 봅니다. 넘친 뒤에는 확인할 수 없습니다. */
static bool sum_fits(int a, int b)
{
    if (b > 0 && a > INT_MAX - b) {
        return false;
    }
    if (b < 0 && a < INT_MIN - b) {
        return false;
    }
    return true;
}

static void page_print(const char *content)
{
    printf("Connection: close\r\n");
    printf("Content-Length: %zu\r\n", strlen(content));
    printf("Content-Type: text/html\r\n\r\n");
    printf("%s", content);
    fflush(stdout);
}
