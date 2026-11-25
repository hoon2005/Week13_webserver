# 발표 데모

노트북 한 대에서 모두 실행합니다. 따로 사이트를 배포할 필요는 없습니다.
모든 스크립트는 저장소 루트에서 실행하고, 먼저 `make`로 빌드되어 있어야 합니다.

| 파일 | 보여주는 것 | 걸리는 시간 |
|---|---|---|
| `demo/errors.py` | 이상한 요청과 실패 상황에서 맞는 상태 코드로 답하고, 그 뒤에도 서버가 살아 있음 | 약 12초 (`--fast`면 2초) |
| `demo/lb.sh` | 서버 3개(프록시 + 백엔드 A, B)로 Round-Robin, 백엔드 장애 대응, 502 | Enter로 진행 |
| `demo/bug.sh <이름>` | 코드에 버그를 넣으면 테스트가 잡고, 되돌리면 다시 통과 | 버그당 10~20초 |
| `make verify` | 전체 검증: 빌드(ASan/UBSan), 단위 테스트, 기능 테스트 67개, lint | 약 50초 |

## 발표 순서 예시 (5~7분)

### 1. 브라우저로 동작 확인

```sh
./build/tiny 8000 --root tests/fixtures/www
```

- `http://localhost:8000/home.html`: 정적 페이지와 이미지
- `http://localhost:8000/nope.html`: 404 페이지

브라우저는 캐시와 추가 요청이 있어서, 응답 하나하나를 보여줄 때는 아래처럼 터미널을 씁니다.

### 2. 실패 상황 표

```sh
python3 demo/errors.py
```

404, 405, 400, 403, 413, 501, 500, 502, 504와 요청 도중 연결이 끊긴 상황까지
표로 보여주고, 마지막에 서버 프로세스가 살아 있는지 확인합니다.

### 3. 로드밸런싱

```sh
./demo/lb.sh
```

1. 6번 요청 → 서버 A, B가 번갈아 응답
2. 백엔드 A를 끔 → B가 모두 받음
3. B도 끔 → 502, 프록시가 직접 처리하는 `/home.html`은 그대로 200
4. 프록시 로그에서 어느 백엔드로 보냈는지 확인

포트 8000~8002를 이미 쓰고 있으면 `BASE=9000 ./demo/lb.sh`로 바꿉니다.

### 4. "테스트가 진짜 버그를 잡는가"

```sh
./demo/bug.sh                         # 목록
./demo/bug.sh round_robin_first_only  # 로드밸런서가 첫 번째 백엔드만 고름
./demo/bug.sh cgi_timeout_500         # CGI 시간 초과 때 504 대신 500
./demo/bug.sh css_mime_plain          # .css를 text/plain으로 보냄
```

각 실행은 버그 넣기(diff 출력) → 관련 테스트 FAIL → 되돌리기 → 같은 테스트 OK 순서로
진행됩니다. 끝나면(중간에 Ctrl+C를 눌러도) 패치를 되돌리므로 원본 코드는 바뀌지 않습니다.
버그는 `demo/bugs/*.patch`에 있습니다.

### 5. 전체 검증

```sh
make verify
```

macOS에서는 Linux 전용 검사(`/proc`) 2개와 연결 시간 초과 테스트 1개가 skip됩니다.
CI의 Ubuntu 쪽에서는 모두 실행되며, CI는 추가로 `make tsan`(데이터 경합 검사)을 돌립니다.
시간이 걸리므로 발표 전에 미리 실행한 화면이나 GitHub Actions 결과를 캡처해 두면 좋습니다.

## 리허설

```sh
./demo/lb.sh --auto            # 멈추지 않고 끝까지
python3 demo/errors.py --fast
```
