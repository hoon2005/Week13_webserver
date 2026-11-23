# M1 소켓 연결 교체 설계 (bloro)

> 개인 PRD 초안입니다. M1(소켓 연결)에서 Tiny의 "포트를 열고 접속을 받고 닫는" 부분을 우리 코드로 바꾸며 정한 것과 그 이유를 적습니다. 코드는 PR #5(`phase1/bloro`)에 있습니다.

## 설계 선택

### 원본 `open_listenfd`에서 고친 네 가지

Tiny의 `main()`이 쓰던 CS:APP 래퍼(`Open_listenfd`, `Accept`, `Getnameinfo`, `Close`)를 우리 코드(`src/net.c`)와 표준 함수로 바꿨습니다. 바깥에서 보는 동작은 같고, 기준선 테스트 5개가 그대로 통과합니다.

| # | 원본 | 바꾼 것 | 이유 |
| --- | --- | --- | --- |
| 1 | `setsockopt` 반환값을 보지 않음 | 실패하면 출력하고 그 주소 후보를 버림 | SO_REUSEADDR 없이 뜨면 재시작 때 TIME_WAIT로 bind가 실패하는데, 그때는 원인을 찾기 어렵습니다 |
| 2 | bind 실패 뒤 close도 실패하면 `freeaddrinfo` 없이 반환 | 그 경로에서도 목록 해제 | getaddrinfo가 할당한 목록이 누수됩니다. 에러 경로에서도 자원을 회수합니다 |
| 3 | 실패 원인을 출력하지 않음 | 실패 직후 errno를 저장해 출력하고 errno에 되돌려 둠 | 뒤의 close, freeaddrinfo가 errno를 덮어쓸 수 있습니다 |
| 4 | `Accept` 실패 시 `exit` | `EINTR`은 재시도, 그 밖에는 로그 후 이 연결만 포기 | ECONNABORTED처럼 손님 쪽 사정으로도 실패할 수 있습니다 |

`getnameinfo`와 `close`도 실패하면 로그만 남기고 계속합니다. 주소 변환은 로그용이고, close는 실패해도 다시 부르면 안 되기 때문입니다. 서버 시작(listen 소켓 열기)에 실패할 때만 종료합니다.

`getaddrinfo`는 그대로 씁니다. `sockaddr_in`을 손으로 채우는 것보다 IPv4와 IPv6를 같은 코드로 다룰 수 있고, 주소 후보를 차례로 시도하는 구조도 원본과 같아 비교하기 쉽습니다.

### accept 실패 처리의 한계

지금은 `EINTR`만 재시도하고 나머지는 모두 "로그 후 다음 연결"입니다. `EMFILE`(fd 부족)이 계속되면 accept가 즉시 실패를 반복해 로그가 빠르게 쌓일 수 있습니다. errno별로 나눠 fd 부족 계열은 잠깐 쉬었다가 재시도하고, `EBADF`처럼 listen 소켓 자체가 망가진 경우는 종료하는 방식이 더 낫다고 봅니다.

## 동시성 제안 (Phase 2)

Tiny는 반복 서버라 한 손님의 `read`에서 블로킹되면 다음 손님을 accept하지 못합니다. 세 후보를 비교하면 다음과 같습니다.

| 후보 | 소켓 모드 | Tiny에서 바꿀 양 | 장단점 |
| --- | --- | --- | --- |
| `fork` | 블로킹 | 적음 | 쉽지만 연결당 비용이 크고, 프록시 캐시를 프로세스끼리 공유하기 어렵습니다 |
| 스레드 풀 | 블로킹 | 적음 (`doit`을 작업 스레드에서 호출) | 연결 큐에 mutex/condvar가 필요합니다. CS:APP 12장 `sbuf` 예제가 있습니다 |
| `poll`/`epoll`/`kqueue` | 논블로킹 | 많음 (`doit`을 상태 머신으로) | C10K에 강하지만 부분 읽기 상태를 연결마다 관리해야 하고, epoll/kqueue는 OS별로 갈립니다 |

**추천은 스레드 풀입니다.** 지금 블로킹 구조를 그대로 쓸 수 있고 과제 문서도 권장합니다. 시간이 남으면 이식성이 좋은 `poll()`로 이벤트 루프를 따로 실험합니다. 연결 수를 늘렸을 때 스레드 풀의 처리량이 한계에 걸리면 이벤트 루프를 다시 검토합니다.

## 요청 흐름

```text
./build/tiny 8000
  → net_listen("8000"): getaddrinfo → socket → setsockopt(SO_REUSEADDR) → bind → listen
    (이후 3-way handshake는 커널이 처리하고 끝난 연결을 Accept 큐에 쌓음)

curl에서 GET /home.html 요청
  → net_accept: Accept 큐에서 연결 하나를 꺼내 connfd 반환 (큐가 비면 블로킹)
  → getnameinfo: 클라이언트 주소를 로그용 문자열로 (실패해도 계속)
  → doit(connfd): 요청 읽기 → 정적 파일 응답 (Tiny 원본 그대로)
  → close(connfd): 이 손님 연결만 닫음. listenfd는 계속 열어 둠
  → 다시 net_accept
```

`listenfd`는 서버에 하나, `connfd`는 연결마다 하나 생깁니다. 반복 서버라 동시에 열린 connfd는 항상 하나입니다.
