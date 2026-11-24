/*
 * cgi.h - CGI 프로그램 실행과 출력 해석 (final.md 4장)
 *
 *   M5-2까지: 서버가 200을 먼저 보내고, CGI가 소켓에 직접 썼습니다.
 *     → CGI가 실패해도 200, 끝나지 않으면 작업자가 무한정 기다림, 응답 길이를 모름
 *   M5-3부터: CGI 출력을 파이프로 끝까지 모은 뒤 서버가 응답을 만듭니다.
 *     cgi_run           : fork → 자식 exec → 부모가 출력을 모음 (시간·크기 상한)
 *     cgi_response_build: 모은 출력을 헤더와 본문으로 나눠 응답 헤더를 만듦
 *   실패하면 500, 시간을 넘기면 504를 보낼 수 있고, 서버가 길이를 세므로
 *   CGI 응답에도 keep-alive를 쓸 수 있습니다 (M5-4b).
 *
 *   소켓을 모르므로 tests/cgi_test.c가 픽스처 CGI로 바로 검사합니다.
 */
#ifndef CGI_H
#define CGI_H

#include <stddef.h>

#define CGI_TIMEOUT_SEC 10    /* CGI 실행 시간 상한 (final.md 2.4) */
#define CGI_OUT_MAX (1 << 20) /* CGI 출력 상한 1 MiB (헤더 포함 전체) */

/* CGI가 출력한 바이트 전체. cgi_run이 malloc하고 cgi_output_free가 해제 */
struct cgi_output {
    char *data;
    size_t len;
};

/*
 * filename을 실행하고 표준 출력을 모두 모읍니다.
 *   envp:         자식에게 넘길 환경 변수 (fork 전에 호출한 쪽이 만들어 둠)
 *   infd:         자식의 표준 입력 (본문 임시 파일 또는 /dev/null). 닫지 않음
 *   timeout_sec:  이 시간 안에 출력을 다 내고 끝나야 함
 *   out_max:      이보다 많이 출력하면 실패
 *   fd_close_max: 자식이 execve 전에 닫을 fd 번호의 끝 (3번부터)
 * 반환: 0 (정상 종료, 종료 코드 0), 500 (실행 실패, 비정상 종료, 출력 초과),
 *       504 (시간 초과). 0이 아닐 때도 out은 비어 있거나 해제할 수 있는 상태
 */
int cgi_run(const char *filename, char *const envp[], int infd, int timeout_sec,
            size_t out_max, int fd_close_max, struct cgi_output *out);

void cgi_output_free(struct cgi_output *out);

/*
 * CGI 출력 "헤더 줄들 + 빈 줄 + 본문"을 나눠 응답 헤더를 header에 씁니다.
 *   상태 줄: CGI가 "Status: 404 Not Found"를 주면 그 값, 없으면 200 OK
 *   CGI의 Content-Length, Connection, Transfer-Encoding은 버리고 서버가 씀
 *   (Content-Length는 실제 본문 길이, Connection은 keep_alive에 따라)
 * 성공하면 0, *status·*body·*body_len을 채움.
 * 출력이 비었거나, 빈 줄이 없거나, 헤더 모양이 틀리거나, header에 다 안 들어가면 500
 */
int cgi_response_build(const char *data, size_t len, int keep_alive,
                       char *header, size_t header_cap, int *status,
                       const char **body, size_t *body_len);

#endif
