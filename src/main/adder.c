/*
 * adder - 두 수를 더하는 CGI 프로그램입니다. Tiny의 adder를 대신합니다.
 *   서버가 환경 변수 QUERY_STRING에 "1&2"처럼 넘겨주면 1 + 2를 계산합니다.
 *   표준 출력이 클라이언트 소켓으로 연결되어 있으므로(서버의 dup2),
 *   printf로 쓴 헤더와 본문이 그대로 응답이 됩니다.
 *   상태 줄과 Server 헤더는 서버가 먼저 보내고, 나머지 헤더는 여기서 씁니다.
 */
#include "csapp.h"

int main(void)
{
    char content[MAXLINE];
    char *query = getenv("QUERY_STRING");
    char *amp;
    int n1 = 0;
    int n2 = 0;

    /* "1&2"를 '&'에서 나눠 두 수로 바꾼다 */
    if (query != NULL) {
        amp = strchr(query, '&');
        *amp = '\0';
        n1 = atoi(query);
        n2 = atoi(amp + 1);
    }

    /* 응답 본문 */
    snprintf(content, sizeof(content),
             "Welcome to add.com: THE Internet addition portal.\r\n<p>"
             "The answer is: %d + %d = %d\r\n<p>"
             "Thanks for visiting!\r\n",
             n1, n2, n1 + n2);

    /* 나머지 헤더와 빈 줄, 본문 */
    printf("Connection: close\r\n");
    printf("Content-length: %d\r\n", (int)strlen(content));
    printf("Content-type: text/html\r\n\r\n");
    printf("%s", content);
    fflush(stdout);
    exit(0);
}