# Phase 1 교체 설계 (다빈)

> 개인 PRD 초안입니다. Phase 1(M1~M3)에서 Tiny를 우리 모듈로 바꾸며 정한 것과 그 이유를 적습니다. 회의에서 아래 안건을 정한 뒤 `final.md`로 옮길 후보입니다. 코드는 PR #3(`phase1/dabin`)에 있습니다.

## 회의 안건

| # | 정할 것 | 제안 | 이유 |
| --- | --- | --- | --- |
| 1 | M3 이후 `csapp.c/h` (구현 계획 6장) | 빼기 | 의존성이 `io.c`와 `io.h`에 모여 있습니다. 요청·응답·CGI 코드는 `io_` 함수만 호출합니다 |
| 2 | M7 항목인 SIGPIPE 무시를 M2로 앞당긴 것 | 유지 | 응답 중 연결이 끊겨 쓰기에 실패해도 그 연결만 정리하고 다음 요청을 받을 수 있습니다 |
| 3 | 잘못된 요청에 새 상태 코드(400, 413, 414)를 쓰지 않는 것 | 유지 | 새 상태 코드는 M7 범위입니다. Phase 1은 Tiny가 쓰던 403, 404, 501 안에서 고르고, 해석할 수 없는 요청은 응답 없이 닫습니다 |
| 4 | 개념 이름 표에 추가할 이름 (CONVENTION "이름" 4번) | 아래 [개념 이름 후보](#개념-이름-후보) | 모듈 사이에서 같은 개념을 같은 이름으로 부르기 위해서입니다 |

## 목표와 범위

Tiny가 하던 일을 그대로 하면서 안쪽 코드를 우리 모듈로 바꿉니다. 기준선 테스트(`baseline_test.py`)가 그대로 통과하고, 레포에 Tiny 코드가 남지 않으면 목표를 달성한 것으로 봅니다.

동작은 그대로 두되, 교체하는 코드 안에서 다음 두 가지를 같이 막습니다.

- **시스템 호출 실패:** 쓰기나 CGI 시작에 실패하면 그 연결만 정리합니다. 접속을 기다리는 소켓을 계속 쓸 수 없으면 서버를 종료합니다.
- **잘못된 요청:** 너무 길거나 깨진 요청은 연결을 닫고, `..` 경로 구간은 `403`으로 거부합니다. `adder`에는 잘못된 값이 오면 안내 본문을 보냅니다. 문서 루트에 둔 심볼릭 링크는 운영 조건으로 관리합니다.

동시성, 헤더 해석, POST, keep-alive, 400/413/414/500 응답은 Phase 2로 미룹니다.

| 마일스톤 | 바꾼 Tiny 부분 | 우리 모듈 |
| --- | --- | --- |
| M1 | `Open_listenfd`, main의 `Accept` 반복문 | `net.c` |
| M2 | `doit`, `read_requesthdrs`, `parse_uri`, `serve_static`, `get_filetype`, `clienterror` | `io.c`, `request.c`, `response.c` |
| M3 | `serve_dynamic`, `main`, `adder` | `cgi.c`, `src/main/server.c`, `src/main/adder.c` |

실행 파일은 `build/tiny` 대신 `build/server`입니다.

## 요청 흐름

```text
curl에서 GET /cgi-bin/adder?1&2 요청
  → net_accept가 연결 fd를 돌려줌
  → request_read_line: "GET", "/cgi-bin/adder?1&2", "HTTP/1.1"
  → request_skip_headers: 빈 줄까지 읽고 버림
  → 메서드가 GET인지 확인 (아니면 501)
  → request_map_uri: 경로 "/cgi-bin/adder"는 규칙을 통과, 첫 구간이 cgi-bin → CGI
                     path "./cgi-bin/adder", args "1&2"
  → cgi_run: 실행 가능 확인 → "HTTP/1.0 200 OK" → fork → 자식이 adder 실행
  → adder가 헤더와 "The answer is: 1 + 2 = 3"을 연결에 씀
  → 부모가 자식을 회수하고, main이 연결을 닫음
```

정적 파일은 `request_map_uri`가 STATIC으로 분류하고 `response_send_static`이 open → fstat → 헤더 → 본문 순서로 보냅니다. 경로 규칙에 걸리면 403, 요청 라인이나 헤더를 해석할 수 없으면 응답 없이 닫습니다.

## 설계 선택

### 실패 처리: 그 연결만 정리합니다

csapp의 대문자 래퍼(`Accept`, `Rio_writen` 등)는 실패하면 `exit`합니다. 그러면 손님 하나가 연결을 끊는 것만으로 서버 전체가 죽을 수 있습니다. 우리 함수는 반환값으로 실패를 알립니다. 요청 처리가 끝나면 `main`이 그 연결을 닫습니다. 서버 시작에 실패하거나, 이후 listen 소켓을 계속 사용할 수 없으면 서버를 종료합니다.

`accept`의 실패는 errno별로 나눕니다. 한 가지로 묶으면 일시적인 오류에도 서버가 죽습니다.

| errno | 처리 |
| --- | --- |
| `EINTR` | 바로 다시 시도 |
| `ECONNABORTED`, Linux의 연결별 네트워크 오류(`EPROTO`, `ENETDOWN` 등, accept(2) 기준) | 기록하고 바로 다시 시도 |
| `EMFILE`, `ENFILE`, `ENOBUFS`, `ENOMEM`, 그 밖의 알 수 없는 오류 | 기록하고 1초 뒤 다시 시도 |
| `EBADF`, `EINVAL`, `ENOTSOCK` | 기록하고 서버 종료 |

`ENONET`은 Linux에만 있어서 `#ifdef`로 감쌌습니다.

### 모듈: io / request / response / cgi / net

| 모듈 | 하는 일 | 공개 함수 |
| --- | --- | --- |
| `net` | 포트를 열고 접속을 받습니다 | `net_open_listen`, `net_accept` |
| `io` | 줄 단위로 읽고, 끝까지 씁니다 | `io_init`, `io_read_line`, `io_write_all` |
| `request` | 읽은 요청을 해석하고 URI를 실제 경로로 바꿉니다 | `request_read_line`, `request_skip_headers`, `request_map_uri` |
| `response` | 정적 파일과 오류 응답을 보냅니다 | `response_send_static`, `response_send_error` |
| `cgi` | CGI 프로그램을 실행합니다 | `cgi_run` |

요청 라인과 헤더는 같은 읽기 버퍼(`struct io_reader`)로 읽습니다. 요청 라인을 읽을 때 헤더 일부가 이미 버퍼에 들어와 있기 때문입니다. 지금은 `io.c`에서 csapp의 rio를 호출하고, `io.h`의 `struct io_reader`가 rio 버퍼를 담습니다. 안건 1에서 csapp를 빼기로 하면 `io` 모듈에서 읽기·쓰기와 버퍼를 교체합니다. M5의 헤더 파싱은 `request`에, 500 응답은 `response`에 붙일 수 있습니다.

### 경로 규칙: 정규화하지 않고, 애매하면 403

쿼리(`?` 뒤)를 떼고 경로만 봅니다.

- `/`로 시작해야 합니다. 중간에 빈 구간(`//`)이 있거나 구간 전체가 `.` 또는 `..`이면 403입니다. 끝의 `/`는 허용하고 `home.html`을 붙입니다.
- 파일을 찾기 **전에** 거부하므로, 파일이 있든 없든 같은 403이 나가서 존재 여부가 드러나지 않습니다.
- `a..b.html`처럼 이름에 점이 들어간 파일은 통과합니다.

`..`가 루트를 넘을 때만 거부하는 정규화 방식도 검토했습니다. Phase 1에서는 서버가 받은 경로 표기 자체에 위 규칙을 적용합니다. 루트 안에서 해석될 수 있는 `.`이나 `..` 구간도 일관되게 403으로 거부합니다.

### CGI 판별: 첫 구간이 정확히 `cgi-bin`일 때만

Tiny는 `strstr`로 URI 전체를 검사해서 쿼리 문자열에 있는 `cgi-bin`도 CGI 분기에 영향을 줬습니다. 지금은 쿼리를 분리한 뒤 첫 경로 구간이 정확히 `cgi-bin`일 때만 CGI로 처리합니다.

ASCII 대소문자만 다른 표기는 403입니다. 대소문자를 무시하는 볼륨에서는 두 표기가 같은 파일을 가리킬 수 있습니다. 파일을 열기 전에 거부해서 CGI 파일이 정적 응답으로 나가는 일을 막고, 두 OS에서 같은 결과를 확인합니다.

### 길이 한도: 줄과 경로 두 곳에서

- 요청 라인과 헤더 줄은 `'\n'` 포함 최대 8191바이트입니다(8192 버퍼에 끝의 `'\0'`까지). 이 안에서 `'\n'`으로 끝나지 않으면(너무 길거나 중간에 끊김) 파싱하기 전에 응답 없이 닫습니다.
- 경로를 만들 때 `snprintf`의 잘림도 따로 확인합니다.
- M7에서는 URI가 너무 긴 경우 414를 보냅니다.

Tiny는 긴 URI에서 `parse_uri`의 버퍼가 넘쳤고, 빈 줄 없이 끊긴 헤더에서는 무한 루프에 빠졌습니다(`rio_readlineb`가 EOF에서 버퍼를 바꾸지 않음).

### 파일 전송: open → fstat → 8 KiB씩

- 먼저 `O_NONBLOCK`으로 열고 `fstat`으로 일반 파일인지 확인합니다. FIFO처럼 열 때 대기할 수 있는 특수 파일도 확인 후 403을 보낼 수 있습니다. `Content-Length`는 **실제로 연 파일의 `fstat` 크기**입니다.
- 8 KiB 버퍼로 read와 write를 반복합니다. Tiny의 mmap은 크기가 0이면 실패해서 빈 파일 요청 하나로 서버가 죽었고, 전송 중 파일이 줄면 SIGBUS가 납니다.
- 빈 파일은 200과 `Content-Length: 0`입니다. 전송 중 파일이 줄거나 write가 실패하면 그 연결만 닫습니다.
- open 실패: `ENOENT`, `ENOTDIR`, `ENAMETOOLONG` → 404, `EACCES` → 403, 그 밖 → 닫기(500은 M5). 일반 파일이 아니면 403입니다.

### CGI 실행 순서

```text
실행 가능한지 검사 (없음 404, 일반 파일 아님·실행 권한 없음 403)
→ "HTTP/1.0 200 OK" 쓰기 (실패하면 fork하지 않고 닫기)
→ fork (실패하면 닫기)
→ 자식: SIGPIPE 기본 동작 복구 → QUERY_STRING → dup2(stdout) → 원래 fd 닫기 → execve
        하나라도 실패하면 _exit(1)
→ 부모: waitpid(그 자식), EINTR이면 다시
```

- 상태 줄을 fork 전에 보내는 이유: fork 뒤에 보내면 부모와 자식이 같은 소켓에 동시에 써서 순서가 섞일 수 있습니다.
- `exit` 대신 `_exit`: `exit`는 부모에게서 물려받은 stdio 버퍼를 비우면서 같은 출력을 두 번 쓸 수 있습니다.
- `wait(NULL)` 대신 `waitpid(pid)`: M4에서 자식이 여럿이 되면 `wait`는 다른 자식을 회수할 수 있습니다.
- listen 소켓은 `FD_CLOEXEC`입니다. 이것이 없으면 CGI가 listen 소켓을 물려받아서, CGI가 실행되는 동안 서버를 다시 켜면 bind가 실패합니다.

### SIGPIPE 무시 (안건 2)

끊긴 연결에 쓰면 SIGPIPE의 기본 동작으로 프로세스가 끝납니다. 그러면 write가 실패를 돌려주기 전에 서버가 죽어서, 위의 "그 연결만 정리"가 동작하지 않습니다. 서버는 시작할 때 SIGPIPE를 무시하고, CGI 자식은 execve 전에 기본 동작으로 되돌립니다. 무시 설정은 execve 뒤에도 남기 때문입니다.

연결 끊김 테스트는 다운로드 도중 끊는 경우와 요청을 보낸 직후 응답을 읽지 않고 닫는 경우를 확인합니다. 두 경우 모두 같은 서버에 다음 GET을 보내 200을 받는지 봅니다.

### adder 입력 검사

두 값을 각각 `strtol`로 읽습니다. 숫자를 하나도 못 읽었거나, 끝까지 못 읽었거나, `ERANGE`이거나, int 범위 밖이면 잘못된 입력입니다. 합이 int 범위인지는 **더하기 전에** 봅니다. 잘못된 입력에는 200과 안내 본문을 보냅니다. 서버가 상태 줄을 먼저 보내므로 adder는 상태 코드를 바꿀 수 없기 때문입니다.

Tiny의 adder는 `&`가 없으면 NULL에 써서 죽었고, `2147483647&1`에서는 덧셈이 넘쳤습니다(개발 빌드에서는 UBSan이 프로세스를 멈춥니다).

### 알려진 한계

| 한계 | 다룰 곳 |
| --- | --- |
| CGI 실행이 실패하면 손님은 이미 200을 받은 상태로 끊길 수 있습니다 | M5 (500, 응답 순서) |
| 서버가 요청과 헤더를 끝없이 기다립니다. 한 손님이 멈추면 다른 손님도 기다립니다 | M4 (동시성), M7 (시간 제한) |
| 심볼릭 링크는 막지 않습니다. 운영 조건으로 관리합니다: **문서 루트 밖을 가리키는 링크와, 정적 파일 영역에서 `cgi-bin`으로 이어지는 링크를 두지 않습니다** | M7 (루트 설정, C4) |
| `FD_CLOEXEC`는 listen 소켓에만 있습니다. 스레드를 쓰면 다른 연결의 fd도 CGI에 물려갑니다 | M4 |
| stdout이 파일로 가면 접속 로그가 버퍼에 남았다가 SIGTERM으로 종료될 때 사라집니다 | M7 (종료 처리) |

## 검증 계획

`make verify`가 기능 테스트 34개를 Ubuntu와 macOS CI에서 돌립니다.

| 파일 | 확인하는 것 |
| --- | --- |
| `baseline_test.py` (5) | Tiny 기준선: `/home.html`, `/` → 200, `adder?1&2` → `1 + 2 = 3`, 없는 파일 404, POST 501 |
| `restart_test.py` (1) | 요청을 처리한 서버를 끄고 바로 같은 포트로 켜도 200 |
| `static_test.py` (21) | Content-Type, 이미지 바이트 일치, 빈 파일, 403/404(FIFO 포함), 경로 규칙, `cgi-bin` 변형 표기, 길이 한도 직전·직후, 깨진 요청, 응답 도중 끊김(SIGPIPE 포함) |
| `cgi_test.py` (7) | adder 정상·잘못된 입력·경계값, QUERY_STRING 전달, 404/403, CGI가 죽어도 서버 생존, CGI 실행 중 서버 재시작 |

잘못된 요청을 보낸 테스트는 끝에 같은 서버로 정상 GET을 보내 200이 오는지 봅니다. 서버가 살아 있는지가 핵심이기 때문입니다.

기능을 빼면 테스트가 실패하는지도 확인했습니다. 테스트가 실제로 그 기능을 검증한다는 뜻입니다.

| 뺀 것 | 실패한 테스트 |
| --- | --- |
| `SO_REUSEADDR` | 재시작 테스트 (`bind: Address already in use`) |
| SIGPIPE 무시 | SIGPIPE 생존 테스트 (서버가 죽어서 뒤의 테스트도 오류) |
| `FD_CLOEXEC` | CGI 실행 중 재시작 테스트 |

브라우저 확인은 레포 루트에서 `make`를 실행한 뒤 아래 명령으로 서버를 띄웁니다. `http://localhost:8000`을 열면 HTML 안의 이미지가 보여야 합니다.

```sh
(cd tests/fixtures/www && ../../../build/server 8000)
```

## 개념 이름 후보

CONVENTION "이름" 4번 표에 추가를 제안합니다.

| 개념 | 이름 | 쓰지 않는 이름 |
| --- | --- | --- |
| 접속을 기다리는 소켓 | `listen_fd` | `listenfd`, `sockfd` |
| 손님 하나와 연결된 소켓 | `conn_fd` (모듈 함수 인자에서는 `fd`) | `connfd`, `clientfd` |
| 요청을 줄 단위로 읽는 버퍼 상태 | `reader` (`struct io_reader`) | `rio`, `rp` |
| URI를 바꾼 실제 경로와 CGI 인자 | `target` (`struct request_target`의 `path`, `args`) | `filename`, `cgiargs` |
