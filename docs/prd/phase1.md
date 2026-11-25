# Phase 1 PRD: 교체 구현과 안정성 수정

> Phase 1을 설계하던 시점의 기록입니다. 최종 코드 기준의 설계는 [final.md](final.md)에 있습니다. Phase 1(M1~M3)에서 Tiny의 각 부분을 같은 동작의 우리 코드로 바꾼 내용과, 리뷰에서 나온 안정성 문제 5가지를 어떻게 고쳤는지 적습니다.

## 목표와 범위

Tiny의 소켓 연결(M1), 정적 GET(M2), CGI GET(M3)을 우리 코드로 바꾸면서 정상 요청에는 원래 Tiny와 같은 응답을 돌려줍니다. 여기에 더해, **클라이언트 한 명의 문제가 서버 전체 종료로 이어지지 않게** 합니다. 문제가 생긴 요청만 끝내고 서버는 다음 연결을 계속 받습니다.

목표 달성 기준은 다음과 같습니다.

- 기준선 테스트 5개(`make verify`)가 그대로 통과합니다.
- 부하 테스트용 빌드(`make release`)가 성공합니다.
- 아래 다섯 상황 뒤에도 서버가 살아 있고, 다음 요청에 정상 응답합니다.

| 문제 | 지금 고쳐야 하는 이유 |
| --- | --- |
| 부하 테스트용 빌드 실패 | 부하 테스트에 쓸 실행 파일을 만들 수 없음 |
| 클라이언트 연결 중단 시 서버 종료 | 다운로드를 취소한 클라이언트 한 명 때문에 서버 전체가 종료될 수 있음 |
| 불완전한 요청에 서버가 멈추거나 종료 | 헤더 도중 연결이 닫히면 무한 반복할 수 있고, 요청 줄이 불완전하면 읽지 못한 값을 사용할 수 있음 |
| 빈 파일 요청으로 서버 종료 | 정상적인 빈 파일 요청 하나로 서버가 종료될 수 있음 |
| CGI 자식 프로세스 생성 실패로 서버 종료 | 계산기 요청 하나를 실행하지 못한 일이 웹 서버 전체 종료로 이어질 수 있음 |

파일을 모듈로 나누는 일과 `tiny.c` 삭제는 이번 범위에 넣지 않습니다. 코드 리뷰로 채택할 코드를 정한 뒤 팀에서 결정합니다. `adder`에 `&`가 없을 때의 오류, 헤더 해석, keep-alive는 이후 마일스톤(M5, M7)으로 미룹니다.

## Phase 1 교체 내용

Tiny의 각 부분을 같은 동작의 우리 코드로 바꿨습니다. 모두 `src/main/tiny.c` 안에서 바꿨고, 정상 요청에 대한 응답 바이트는 원래 Tiny와 같습니다.

### M1 소켓 연결

| Tiny | 우리 코드 | 바꾼 내용 |
| --- | --- | --- |
| `Open_listenfd` (csapp.c) | `listen_socket_open` | getaddrinfo 후보 목록을 도는 대신 IPv4 주소(`sockaddr_in`)를 직접 채워 socket → SO_REUSEADDR → bind → listen이 그대로 보이게 함. setsockopt 실패도 확인. 실패하면 소켓을 닫고 -1 |
| main의 `Accept`, `Close` | `accept`, `close` | 대문자 래퍼는 실패 시 exit. accept가 실패하면(예: ECONNABORTED) 원인만 출력하고 다음 연결을 기다림 |
| `Getnameinfo` 접속 기록 | 뺌 | 응답 동작과 무관. 그래서 클라이언트 주소도 `sockaddr_storage` 대신 `sockaddr_in` 사용 |

### M2 정적 GET

| Tiny | 우리 코드 | 바꾼 내용 |
| --- | --- | --- |
| `doit` (모든 일을 직접 함) | `doit` + 단계 함수 | 요청 라인 읽기, 메서드 확인, 파일 찾기, 권한 확인을 `request_line_read`, `method_check`, `file_find`, `file_check_readable`, `file_check_executable`로 분리. doit은 순서대로 부르고 분기만 함 |
| `read_requesthdrs` | `request_headers_skip` | 헤더를 빈 줄까지 읽고 버림 |
| 없음 | `uri_check` (+ `uri_has_dot_dot`) | 경로 조각이 정확히 `..`이면 403. 웹 폴더 밖 파일 읽기, 다른 프로그램 실행을 막음 (M2 질문). `a..b.html`은 허용 |
| `parse_uri` | `uri_parse` | 같은 규칙. strcpy, strcat 대신 snprintf로 버퍼 크기를 지킴 |
| `serve_static` | `static_file_serve` | 헤더를 snprintf 한 번으로 만들어 한 번에 보냄 |
| `get_filetype` | `file_type_get` + `FILE_TYPES` 표 | if 나열 대신 확장자·타입 표를 앞에서부터 확인. 순서와 규칙은 같음. M5에서 CSS, JS를 한 줄씩 추가하면 됨 |
| `clienterror` | `http_error_send` | 헤더와 본문을 각각 snprintf 한 번으로 만듦. Tiny의 `sprintf(body, "%s...", body)`(자기 자신에 이어 붙이기) 제거, 긴 cause에도 버퍼를 넘지 않음 |

### M3 CGI GET

| Tiny | 우리 코드 | 바꾼 내용 |
| --- | --- | --- |
| `serve_dynamic`의 부모 쪽 | `cgi_serve` | 200과 Server 헤더를 한 번에 보내고, 자식을 만들고, 끝나면 회수 |
| `serve_dynamic`의 자식 쪽 | `cgi_child_run` | QUERY_STRING 설정 → dup2로 표준 출력을 소켓으로 → execve로 adder 실행 |
| `adder.c` | `adder.c` | strcpy 복사 없이 `&` 앞뒤를 바로 숫자로 바꾸고, 본문을 snprintf 한 번으로 만듦. 출력은 같음 |

### 교체 확인

- 원본 Tiny와 같은 요청 14개(home.html, /, gif, jpg, adder 두 개, 404, POST 501, 소문자 get, 폴더 403, 읽기 권한 없는 파일 403, 실행 권한 없는 CGI 403, 없는 CGI 404, ?v=2)를 보냈을 때 응답이 바이트 단위로 모두 같음
- 의도한 차이는 `../` 요청 하나(원본 404 → 403)
- 아래 안정성 수정 5가지는 이 교체 뒤에 추가로 반영한 것이며, 오류 경로의 동작만 바뀜

## 설계 선택

### 1. 실패를 어디서 멈출까?

Tiny는 csapp.c의 대문자 래퍼(`Rio_writen`, `Mmap`, `Fork` 등)를 써서 시스템 함수가 실패하면 **서버 프로세스 전체가 exit**했습니다. 실패의 원인은 클라이언트 한 명의 요청인데, 결과는 모든 사용자의 접속 불가였습니다.

- **대문자 래퍼 유지**는 코드가 짧지만, 실패 하나가 곧 서버 종료입니다.
- **소문자 함수와 반환값 확인**은 확인 코드가 늘지만, 실패한 요청만 끝낼 수 있습니다.

**요청을 처리하는 경로에서는 소문자 함수로 바꾸고 반환값을 확인합니다.** 서버 시작 단계(소켓 열기)에서의 실패는 서버를 띄울 수 없으므로 그대로 종료합니다. CGI 자식 프로세스 안의 `Dup2`, `Execve`는 실패해도 자식만 끝나므로 대문자 래퍼를 그대로 둡니다.

### 2. "요청 종료"할 때 클라이언트에게 무엇을 보낼까?

응답을 보낼 수 있는 상황이면 원인에 맞는 오류 응답을 보내고, 보낼 수 없는 상황이면 연결만 닫습니다.

| 상황 | 응답 | 이유 |
| --- | --- | --- |
| 요청 줄에 줄바꿈이 없거나 세 항목이 아님 | `400 Bad Request` | 클라이언트 요청 형식의 문제 |
| 만든 파일 경로가 버퍼보다 김 | `414 URI Too Long` | 잘린 경로로 다른 파일을 찾지 않도록 파일 조회 전에 중단 |
| 파일 열기, mmap 실패 | `500 Internal Server Error` | 서버 쪽 문제. 200을 보내기 전이라 오류 응답 가능 |
| 헤더 도중 끊김, 읽기·쓰기 실패 | 없음 (연결만 닫음) | 응답을 받을 상대가 없음 |
| CGI `fork` 실패 | 없음 (연결만 닫음) | `200` 헤더가 이미 나가서 상태를 바꿀 수 없음 |

CGI는 `fork` 전에 `200` 헤더를 보내는 Tiny의 순서를 유지합니다. 헤더를 `fork` 뒤로 옮기면 부모가 쓰는 헤더와 자식(adder)이 쓰는 출력의 순서가 섞일 수 있기 때문입니다. `fork` 실패 시 `500`을 보내야 한다는 요구가 생기면, 자식이 헤더까지 출력하는 방식을 다시 검토합니다.

### 3. 문제별 수정

| 문제 | 원인 | 수정 |
| --- | --- | --- |
| 부하 테스트용 빌드 실패 | `uri_parse`의 `snprintf` 반환값을 확인하지 않아 `-O2`에서 "잘릴 수 있음" 경고가 나고 `-Werror`로 빌드 실패 | 반환값(만들려던 전체 길이)이 `MAXLINE` 이상이면 `-1`. `doit`이 파일 조회 전에 `414`를 보내고 끝냄 |
| 연결 중단 시 서버 종료 | 끊긴 소켓에 쓰면 `SIGPIPE`(기본 동작: 종료). 무시해도 `Rio_writen`이 쓰기 실패 시 exit | `main` 시작 시 `signal(SIGPIPE, SIG_IGN)`. `Rio_readlineb`, `Rio_writen`을 `rio_readlineb`, `rio_writen`으로 바꾸고 실패하면 그 연결만 정리 |
| 불완전한 요청 | 헤더 읽기 결과를 확인하지 않아 빈 줄 전에 끊기면 같은 줄을 끝없이 비교. `sscanf` 결과를 확인하지 않아 `"GET"`만 오면 초기화되지 않은 uri 사용 | 요청 줄은 줄바꿈과 세 항목을 확인하고 아니면 `400`. 헤더는 빈 줄을 만났을 때만 다음 단계로, 끊기거나 읽기 실패하면 요청 종료 |
| 빈 파일 요청 | 크기 0으로 `mmap`하면 실패(EINVAL)하고 `Mmap`이 서버를 종료 | 파일 열기와 `mmap`을 먼저 하고 준비가 끝난 뒤 `200` 전송. 크기 0이면 `mmap` 없이 `Content-length: 0`으로 응답. 열기나 `mmap` 실패 시 `500` |
| CGI `fork` 실패 | `Fork`가 실패하면(프로세스 수 한도 등) 서버를 종료 | `fork` 반환값을 확인해 실패하면 원인만 출력하고 요청 종료. 같은 이유로 `Wait`도 `waitpid`로 바꿈 |

`SIGPIPE` 무시 설정은 `execve` 뒤에도 유지되므로 adder도 끊긴 소켓에 쓸 때 종료되지 않고 쓰기 실패만 받습니다. `send`의 `MSG_NOSIGNAL`로도 막을 수 있지만 리눅스 전용이라 macOS CI까지 고려해 `signal`을 씁니다.

## 요청 흐름

```text
main
  signal(SIGPIPE, SIG_IGN)                      ← 끊긴 소켓에 써도 서버가 꺼지지 않음
  listen_socket_open(port)                       socket → SO_REUSEADDR → bind → listen
  반복: accept → doit(connfd) → close

doit(connfd)
  request_line_read     줄바꿈 없음 / 세 항목 아님 → 400        끊김 → 종료
  method_check          GET 아님 → 501
  request_headers_skip  빈 줄 전에 끊김 / 읽기 실패 → 종료 (무한 반복 없음)
  uri_check             경로 조각에 ".." → 403
  uri_parse             경로가 버퍼보다 김 → 414
  file_find             없음 → 404
  정적: file_check_readable → static_file_serve
          open / mmap 실패 → 500
          크기 0 → Content-length: 0, 본문 없음
          준비 완료 → 200 헤더 → 본문 (쓰기 실패 → 정리만)
  동적: file_check_executable → cgi_serve
          200 헤더 (쓰기 실패 → 종료)
          fork 실패 → 종료 (연결만 닫음)
          자식: QUERY_STRING 설정 → dup2 → execve(adder)
          부모: waitpid로 회수
```

정상 요청의 응답 바이트는 원래 Tiny와 같습니다. 달라지는 것은 오류 경로뿐입니다.

## 검증 계획

준비: `make verify`, `make release`로 빌드합니다. 서버를 `./build/tiny 8080`으로 띄우고, 실행 폴더에 `home.html`, 빈 파일 `empty.html`(`: > empty.html`), 큰 파일 `big.jpg`(예: 20MB)를 둡니다.

| 확인할 것 | 요청 / 방법 | 성공 기준 |
| --- | --- | --- |
| 기준선 동작 유지 | `make verify` | 기준선 테스트 5개 통과 |
| 부하 테스트용 빌드 | `make release` | `build/release/tiny`가 만들어짐 |
| 요청 줄에 메서드만 옴 | `printf 'GET\r\n\r\n' \| nc 127.0.0.1 8080` | `400 Bad Request` |
| 요청 줄 중간에 끊김 | `printf 'GET /home.html HTTP/1.0' \| nc -N 127.0.0.1 8080` | `400 Bad Request`, 서버 유지 |
| 헤더 도중 끊김 | `printf 'GET /home.html HTTP/1.0\r\nHost: x\r\n' \| nc -N 127.0.0.1 8080` | 응답 없이 바로 닫힘. 서버가 멈추지 않고 다음 요청에 응답 |
| 빈 파일 | `curl -i http://127.0.0.1:8080/empty.html` | `200`, `Content-length: 0`, 서버 유지 |
| 다운로드 중 취소 | `curl http://127.0.0.1:8080/big.jpg \| head -c 100 > /dev/null` | 서버 유지, 이어서 `curl -i http://127.0.0.1:8080/` 이 `200` |
| CGI 정상 동작 | `curl -i "http://127.0.0.1:8080/cgi-bin/adder?1&2"` | `1 + 2 = 3` |

각 상황 뒤에는 서버가 살아 있는지 `curl -i http://127.0.0.1:8080/`로 확인합니다.

### 실험 결과

- `make verify`: 기준선 테스트 5개 통과. `make release`: gcc, clang 모두 `-O2 -Werror`로 빌드 성공
- 위 표의 요청을 소켓으로 직접 보냄
  - `GET`만 보냄, 줄바꿈 없이 끊음 → 둘 다 `400`
  - 헤더 도중 끊음 → 응답 없이 바로 닫힘 (Tiny는 무한 반복)
  - 빈 파일 → `200`, `Content-length: 0`
  - 20MB 파일 다운로드 중 강제로 끊음 → 서버 유지, 다음 요청 `200`
  - adder → `1 + 2 = 3`
  - 모든 상황 뒤에 서버 프로세스가 살아 있음
- `414`는 직접 만들기 어렵습니다. 요청 줄 자체가 `MAXLINE`(8192)을 넘으면 그 전에 `400`이 나가기 때문입니다. 컴파일러가 이 관계를 알 수 없어 경고가 났으므로, 반환값 확인은 경고를 없애고 앞으로 버퍼 크기가 바뀌어도 잘린 경로를 쓰지 않게 하는 방어 코드입니다.
- `fork` 실패는 프로세스 수 한도를 낮춰야 재현할 수 있어 직접 실험하지 않았고, 코드로 확인했습니다.