# M5 헤더 파싱과 요청 본문 처리

> 개인 PRD 초안입니다. M5 중 **헤더 파싱**과 **요청 본문(POST) 읽기**를 다룹니다. 본문의 끝은 `Content-Length`로 판단하고, 본문은 상한을 둔 메모리 버퍼에 모두 읽습니다. `Expect: 100-continue`를 처리해서 큰 본문은 받기 전에 거절합니다.

회의에서 정할 질문

1. 어떤 헤더를 꺼내 저장하고, 어떤 헤더는 읽고 버릴까?
2. 본문의 끝은 무엇으로 판단할까? (`Content-Length` / `Transfer-Encoding: chunked`)
3. 본문은 다 모은 뒤 처리할까(버퍼링), 읽으면서 바로 넘길까(스트리밍)?
4. `Expect: 100-continue`에 답할까, 무시할까?

## 목표와 범위

지금 서버는 헤더를 빈 줄까지 읽고 버리기만 합니다(`request_headers_skip`). 그래서 본문이 있는 요청(POST)을 처리할 수 없고, keep-alive에서 다음 요청이 어디서 시작하는지도 알 수 없습니다.

이번 범위에서 할 일은 다음과 같습니다.

- 헤더 줄을 `이름: 값`으로 나눠 필요한 값을 요청 구조체에 저장합니다.
- `Content-Length`만큼 본문을 정확히 읽습니다. 상한을 넘는 본문은 읽지 않고 거절합니다.
- `Expect: 100-continue`가 오면 본문을 받기 전에 `100 Continue` 또는 오류로 답합니다.
- 잘못된 헤더와 본문에 맞는 오류 응답을 보내고, 서버는 계속 동작합니다.

목표 달성 기준은 다음과 같습니다.

- `curl -d "a=1&b=2"` 요청의 본문 7바이트가 CGI 프로그램에 그대로 전달됩니다.
- 상한을 넘는 본문은 본문 전송 전에 `413`으로 거절됩니다.
- 아래 [오류 응답](#오류-응답) 표의 요청마다 기대한 상태 코드가 오고, 다음 요청에도 정상 응답합니다.
- 파서 단위 테스트 `tests/http_test.c`가 통과합니다.

이번 범위에 넣지 않는 것입니다.

| 항목 | 처리 | 이유 |
| --- | --- | --- |
| `Transfer-Encoding: chunked` 해석 | `501` | 과제 문구가 "`Content-Length` 기반 본문 길이 처리". `curl -d`와 HTML form은 Content-Length로 보냄. 여유가 있으면 확장 (아래 [다시 검토할 조건](#다시-검토할-조건)) |
| 본문 스트리밍 | 버퍼링만 | 이번 POST는 form 데이터 수준. 아래 설계 선택 3 |
| keep-alive 반복 처리 | 이번엔 `Connection` 값 저장까지만 | 같은 연결에서 `doit`을 반복하는 일은 M5의 keep-alive 항목에서 함. 다만 이 문서의 규칙(본문을 정확히 다 읽기, 경계가 불확실하면 연결 닫기)이 그 전제 조건임 |
| `Host` 기반 라우팅 | 저장과 검사만 | Proxy 단계에서 씀 |

## 설계 선택

### 1. 어떤 헤더를 꺼낼까?

HTTP 헤더는 종류가 정해져 있지 않고 클라이언트가 상황에 따라 골라 보냅니다. 서버는 **필요한 헤더만 꺼내고 나머지는 읽고 버립니다.** 모르는 헤더는 무시하는 것이 HTTP 규칙입니다. 헤더 이름은 대소문자를 구분하지 않으므로 `strcasecmp`로 비교하고, 값 앞뒤의 공백은 잘라냅니다.

| 헤더 | 저장할 값 | 쓰는 곳 |
| --- | --- | --- |
| `Host` | 문자열 | HTTP/1.1인데 없으면 `400`. 지금은 검사만 하고 Proxy에서 백엔드를 고를 때 씀 |
| `Content-Length` | 0 이상의 정수, 있었는지 여부 | 본문을 몇 바이트 읽을지 |
| `Connection` | `close` / `keep-alive` / 없음 | 응답 후 연결을 닫을지. 없으면 HTTP/1.1은 유지, HTTP/1.0은 닫음 |
| `Content-Type` | 문자열 | 서버는 해석하지 않고 CGI에 `CONTENT_TYPE`으로 넘김 |
| `Transfer-Encoding` | 있었는지 여부 | 있으면 거절 |
| `Expect` | `100-continue` / 그 외 / 없음 | 본문을 받기 전에 답할지 |

요청 구조체 예시는 다음과 같습니다. 파서는 소켓을 모르고 문자열만 받아 이 구조체를 채우게 해서, 단위 테스트에서 소켓 없이 검증할 수 있게 합니다.

```c
struct http_request {
    char method[16], uri[MAXLINE], version[16];
    char host[256];
    char content_type[256];
    long content_length;     /* has_content_length가 0이면 의미 없음 */
    int  has_content_length;
    int  has_transfer_encoding;
    int  expect_continue;    /* Expect: 100-continue */
    int  keep_alive;         /* 버전과 Connection 헤더로 결정 */
    char *body;              /* malloc. 요청이 끝나면 free */
};
```

### 2. 본문의 끝은 무엇으로 판단할까?

TCP는 바이트가 끊김 없이 이어지는 흐름이라, 본문이 어디서 끝나는지를 HTTP가 따로 알려 줘야 합니다.

| 방식 | 설명 | 결정 |
| --- | --- | --- |
| `Content-Length: n` | 정확히 n바이트가 본문 | **지원** |
| `Transfer-Encoding: chunked` | `크기\r\n데이터\r\n` 반복, 크기 0이면 끝 | 이번엔 `501` |
| 연결 끊김까지 | 상대가 닫으면 끝 | 요청에는 쓸 수 없음 (끊으면 응답을 못 보냄) |

**`Content-Length`와 `Transfer-Encoding`이 같이 오면 `400`을 보내고 연결을 닫습니다.** 서버마다 다른 쪽을 믿으면 본문이 끝나는 위치를 서로 다르게 판단하게 됩니다. 그러면 남은 바이트가 같은 연결의 "다음 요청"으로 처리되는 request smuggling이 생길 수 있습니다. 지금은 서버 한 대라 다음 요청이 꼬이는 버그 수준이지만, Reverse Proxy와 keep-alive를 같이 만들면 보안 문제가 됩니다.

`Content-Length`가 여러 번 오는데 값이 서로 다르거나, 숫자가 아니거나, 음수이면 `400`을 보냅니다.

### 3. 본문은 버퍼링할까, 스트리밍할까?

둘 다 TCP 위에서 받은 같은 바이트를 다루며 정확도는 같습니다. 차이는 받은 바이트를 **언제, 얼마나 모아서 넘기는지**입니다.

| | 버퍼링 | 스트리밍 |
| --- | --- | --- |
| 방식 | Content-Length만큼 `malloc`해서 다 읽은 뒤 CGI에 넘김 | 조금 읽을 때마다 바로 CGI 파이프에 씀 |
| 메모리 | 본문 크기만큼 | 작은 고정 버퍼 |
| 구현 | 단순. 파서와 처리부가 분리되어 단위 테스트가 쉬움 | 읽기와 쓰기가 섞이고, 중간 실패 처리가 복잡 |
| 큰 본문 | 상한으로 거절 | 받을 수는 있지만 작업자 스레드가 그동안 묶임. 결국 상한이 필요 |

**추천은 버퍼링 + 상한입니다.** "매우 큰 요청 본문" 엣지 케이스를 막는 것은 스트리밍이 아니라 **상한**입니다. 헤더에서 `Content-Length`를 먼저 확인하고, 상한을 넘으면 `malloc`하기 전에 `413`을 보냅니다. 상한보다 큰 본문에는 메모리를 쓰지 않습니다.

- **상한 값:** `BODY_MAX = 1 MiB` (1,048,576바이트)를 제안합니다. form 데이터에는 충분히 큽니다.
- **최대 메모리:** M4에서 스레드 풀을 쓰면 본문 버퍼 최대 메모리는 `BODY_MAX × 작업자 수`로 계산됩니다 (예: 1 MiB × 8 = 8 MiB).
- **해제:** 본문 버퍼는 응답 후, 그리고 중간에 실패해서 요청을 끝낼 때 모두 `free`합니다. ASan으로 누수를 확인합니다.

### 4. `Expect: 100-continue`에 답할까?

curl은 POST 본문이 일정 크기(보통 1KB)를 넘으면 `Expect: 100-continue`를 붙이고, 서버가 "보내도 된다"고 답할 때까지 본문을 미룹니다.

- **무시하면:** 서버는 본문을, curl은 서버 답을 서로 기다립니다. curl이 약 1초 뒤 포기하고 본문을 보내므로 결과는 같지만 그런 요청마다 1초씩 늦어집니다. 큰 본문 테스트와 부하 테스트 숫자가 왜곡됩니다.
- **답하면:** 본문 검사(411, 413 등)를 본문을 읽기 전에 하고, 통과하면 `HTTP/1.1 100 Continue\r\n\r\n`을 보낸 뒤 본문을 읽습니다. 통과하지 못하면 `100` 없이 바로 오류를 보냅니다.

**추천은 답하는 쪽입니다.** 어차피 하려던 검사를 본문 읽기 앞으로 옮기고 분기 하나를 더하는 정도입니다. 그리고 상한을 넘는 본문을 **보내기도 전에** 거절할 수 있어서, 설계 선택 3의 상한과 잘 맞습니다.

| 조건 | 처리 |
| --- | --- |
| HTTP/1.1, `Expect: 100-continue`, 본문 검사 통과, `Content-Length > 0` | `100 Continue` 전송 후 본문 읽기 |
| `Expect: 100-continue`, 본문 검사 실패 | `100` 없이 오류 응답 후 **연결 닫기**. 클라이언트가 기다리다 본문을 보낼 수 있는데, 연결을 유지하면 그 본문이 다음 요청으로 읽힘 |
| `100-continue`가 아닌 Expect 값 | `417 Expectation Failed` |
| HTTP/1.0 요청 | Expect 무시 (100 Continue는 HTTP/1.1에만 있음) |

### 다시 검토할 조건

- 파일 업로드처럼 1 MiB를 넘는 본문을 **정상으로** 받아야 하면 스트리밍을 검토합니다.
- chunked로 보내는 클라이언트를 지원해야 하면 chunked 디코딩을 추가합니다. RFC 7230은 HTTP/1.1 수신자가 chunked를 해석할 수 있어야 한다(MUST)고 정하므로, 이번 `501`은 표준을 다 지킨 것이 아니라 **범위 제한**입니다. 조각을 모아 버퍼에 붙이고 모은 길이를 `CONTENT_LENGTH`로 넘기면 되므로 버퍼링 구조에 그대로 들어갑니다.

## 요청 흐름

`curl -d "a=1&b=2" http://127.0.0.1:8000/cgi-bin/adder` 요청을 예로 듭니다.

```text
요청 라인 읽기        "POST /cgi-bin/adder HTTP/1.1"
  → 메서드 확인        GET, POST 허용 (그 외 501)
  → 헤더 파싱          빈 줄까지 한 줄씩 "이름: 값"으로 나눠 구조체에 저장
                       Host=127.0.0.1:8000, Content-Length=7,
                       Content-Type=application/x-www-form-urlencoded
  → 헤더 검사          Host 없음(1.1) 400 / TE 있음 501 / CL+TE 400
                       POST인데 CL 없음 411 / CL > BODY_MAX 413 / 이상한 Expect 417
  → Expect 처리        100-continue였으면 "HTTP/1.1 100 Continue" 전송
  → 본문 읽기          같은 rio로 rio_readnb(&rio, body, 7)
                       7바이트보다 적게 오면(연결 끊김) free 후 종료
  → URI 확인, 해석, 파일 찾기, 권한 확인 (지금과 같음)
  → CGI 실행           pipe 생성 → fork
                       자식: 파이프 읽는 쪽을 STDIN으로, 소켓을 STDOUT으로 dup2
                             REQUEST_METHOD=POST, CONTENT_LENGTH=7,
                             CONTENT_TYPE 설정 → execve
                       부모: 파이프 쓰는 쪽에 본문 7바이트 쓰고 닫음 (CGI가 EOF를 알게)
                             → waitpid → body free
```

구현할 때 주의할 점입니다.

- **본문은 반드시 헤더를 읽은 같은 `rio`로 읽습니다.** `rio_readlineb`는 소켓에서 넉넉히 읽어 두기 때문에 본문 앞부분이 이미 rio 버퍼에 있을 수 있습니다. 소켓에서 `read`를 직접 하면 그 앞부분을 잃습니다.
- **GET에 본문이 붙어 와도 읽어서 버립니다** (상한 적용). 읽지 않으면 keep-alive에서 그 본문이 다음 요청으로 읽힙니다.
- **CGI가 stdin을 읽지 않고 끝나도 서버는 멈추지 않습니다.** 부모의 파이프 쓰기가 `EPIPE`로 실패하는데, `SIGPIPE`는 이미 무시하므로 그 요청만 끝냅니다. 본문이 파이프 버퍼(보통 64KB)보다 크면 부모가 쓰기에서 잠시 기다릴 수 있지만, 자식은 출력을 파이프가 아니라 소켓에 쓰므로 서로 기다리는 교착은 생기지 않습니다.
- **본문 경계를 확신할 수 없는 오류**(CL+TE, 잘못된 CL, 본문 도중 끊김, Expect 거절)는 응답 후 **연결을 닫습니다.**
- `adder`도 POST를 받도록 고칩니다. `REQUEST_METHOD`가 POST이면 stdin에서 `CONTENT_LENGTH`만큼 읽어 쿼리 문자열 대신 씁니다.

### 오류 응답

| 상황 | 응답 | 연결 |
| --- | --- | --- |
| 헤더 줄에 `:`가 없음, 헤더 줄이 `MAXLINE`보다 김 | `400` | 닫음 |
| HTTP/1.1인데 `Host` 없음 | `400` | 닫음 |
| `Content-Length`가 숫자가 아님, 음수, 서로 다른 값으로 여러 번 | `400` | 닫음 |
| `Content-Length`와 `Transfer-Encoding`이 같이 옴 | `400` | 닫음 |
| `Transfer-Encoding`만 옴 | `501` | 닫음 |
| POST인데 `Content-Length` 없음 | `411 Length Required` | 닫음 |
| `Content-Length > BODY_MAX` | `413 Payload Too Large` | 닫음 |
| `Expect` 값이 `100-continue`가 아님 | `417 Expectation Failed` | 닫음 |
| 본문 도중 연결 끊김 | 없음 (상대가 없음) | 닫음 |

## 검증 계획

**준비:** `make`로 빌드하고, `tests/fixtures/www`를 루트로 서버를 `8000`번에 띄웁니다. `cgi-bin/adder`는 POST 본문을 읽도록 고친 버전입니다.

| 요청 | 성공 조건 |
| --- | --- |
| `curl -i -d "a=1&b=2" localhost:8000/cgi-bin/adder` | `200`, 본문에 `1 + 2 = 3` |
| `curl -i 'localhost:8000/cgi-bin/adder?1&2'` | 기존 GET 결과 그대로 (회귀 없음) |
| `head -c 2000 /dev/zero \| curl -v --data-binary @- localhost:8000/cgi-bin/adder` | 로그에 `< HTTP/1.1 100 Continue`가 보이고 1초 지연 없이 응답 |
| `head -c 2000000 /dev/zero \| curl -v --data-binary @- localhost:8000/cgi-bin/adder` | 본문을 보내지 않고 바로 `413` |
| `printf 'POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\n\r\n' \| nc localhost 8000` | `411` |
| `printf 'GET / HTTP/1.1\r\n\r\n' \| nc localhost 8000` | `400` (Host 없음) |
| `printf 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n' \| nc localhost 8000` | `400` 후 연결 닫힘 |
| `printf 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n' \| nc localhost 8000` | `400` |
| `curl -i -H "Expect: foo" -d x localhost:8000/cgi-bin/adder` | `417` |
| `printf 'POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\nab' \| nc -w1 localhost 8000` (본문 도중 끊김) | 서버가 죽지 않고, 이어서 보낸 `curl localhost:8000/` 이 `200` |

위 요청들 뒤에 `curl localhost:8000/home.html`이 매번 `200`이면 서버가 살아 있는 것입니다. 기능 테스트는 `tests/functional/`에 같은 요청으로 추가합니다.

**단위 테스트 `tests/http_test.c`:** 파서에 문자열을 넣고 구조체 값을 확인합니다.

- 헤더 이름 대소문자 무시 (`content-length`, `CONTENT-LENGTH`)
- 값 앞뒤 공백 제거 (`Content-Length:   7  `)
- `Connection` 기본값: HTTP/1.1 → keep-alive, HTTP/1.0 → close, 명시하면 그 값
- 잘못된 `Content-Length` (`abc`, `-1`, `7` 다음 `8`) → 오류
- CL+TE, TE만, Host 없음(1.1), Host 없음(1.0은 허용)
- 모르는 헤더(`X-Foo: bar`)는 무시

**누수:** 위 요청을 모두 보낸 뒤 서버를 종료했을 때 ASan(Linux에서는 LeakSanitizer 포함)이 누수를 보고하지 않는지 확인합니다. 특히 413, 본문 도중 끊김, CGI 실패 경로에서 `body`가 해제되는지 봅니다.
