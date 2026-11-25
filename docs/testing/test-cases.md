# 테스트 케이스: 브라우저 검증과 로드밸런싱 검증

> 기준 코드: `demo/bloro` 브랜치 (75d1a3d). 확인일 2026-11-25, macOS.
> 데모 스크립트 사용법은 [`demo/README.md`](../../demo/README.md), 엣지케이스는 [`edge-cases.md`](edge-cases.md)에 있습니다.

## 준비

```sh
make
./demo/web.sh      # 브라우저 검증 (포트 8000)
./demo/lb.sh       # 로드밸런싱 검증 (포트 8000~8002)
```

두 스크립트는 같은 포트 8000을 쓰므로 하나를 끄고 다른 하나를 켭니다.
브라우저는 개발자 도구 → Network 탭 → "Disable cache"를 켠 상태에서 확인합니다.

---

## 1. 브라우저 검증 (`demo/web.sh`, `http://localhost:8000/`)

### 1-1. 정적 파일과 MIME 타입

| ID | 확인 내용 | 방법 | 기대 결과 | 결과 |
|---|---|---|---|---|
| B-01 | 루트 경로가 `home.html`로 연결됨 | `/` 열기 | 200, `text/html`, 데모 페이지가 보임 | ✅ |
| B-02 | HTML | Network 탭에서 `home.html` | `Content-Type: text/html` | ✅ |
| B-03 | CSS가 적용됨 | Network 탭에서 `style.css`, 화면의 글꼴·테두리·색 | `Content-Type: text/css`, 스타일 적용 | ✅ |
| B-04 | JS가 실행됨 | 표의 `app.js` 칸 | `Content-Type: text/javascript`, 칸이 "✅ JS가 실행됨"으로 바뀜 | ✅ |
| B-05 | 이미지 | Network 탭에서 `crying_cat.jpg` | `Content-Type: image/jpeg`, `Content-Length: 66381`, 고양이 사진 보임 | ✅ |

> B-03, B-04는 MIME 타입이 틀리면 브라우저가 파일을 무시하므로 화면만 봐도 판정됩니다.
> `demo/bug.sh css_mime_plain`으로 `.css`를 `text/plain`으로 바꾸면 테스트가 실패하는 것도 확인할 수 있습니다.

### 1-2. CGI (`/cgi-bin/adder`)

| ID | 확인 내용 | 방법 | 기대 결과 | 결과 |
|---|---|---|---|---|
| B-06 | GET 폼 | GET 칸에 `1`, `2` 입력 후 더하기 | 200, "The answer is: 1 + 2 = 3", 주소창에 `?a=1&b=2` | ✅ |
| B-07 | POST 폼 | POST 칸에 `1`, `2` 입력 후 더하기 | 200, 같은 답, 주소창에 값 없음, Network 탭 Payload에 `a=1&b=2` | ✅ |
| B-08 | 답이 서버에서 만들어짐 | 다른 숫자(예: `40`, `2`)로 다시 제출 | 매번 새 답(`42`). 페이지에 미리 적힌 답 없음 | ✅ |
| B-09 | 음수 | `-3`, `5` | `-3 + 5 = 2` | ✅ |

### 1-3. 오류

| ID | 확인 내용 | 방법 | 기대 결과 | 결과 |
|---|---|---|---|---|
| B-10 | 없는 파일 | `/nope.html` 링크 클릭 | Network 탭 404, 화면에 "Tiny couldn't find this file" | ✅ |
| B-11 | 서버가 계속 살아 있음 | 404 뒤 뒤로 가기 → 새로고침 | 200으로 다시 보임 | ✅ |

---

## 2. 로드밸런싱 검증 (`demo/lb.sh`)

구성:

| 역할 | 주소 | 루트 |
|---|---|---|
| 프록시 | `127.0.0.1:8000` | `demo/www` (`/api`가 아닌 요청은 직접 처리) |
| 백엔드 A | `127.0.0.1:8001` | `demo/lb/www_a` |
| 백엔드 B | `127.0.0.1:8002` | `demo/lb/www_b` |

프록시 실행 옵션: `--route /api=127.0.0.1:8001,127.0.0.1:8002`

### 2-1. Round-Robin

| ID | 확인 내용 | 방법 | 기대 결과 | 결과 |
|---|---|---|---|---|
| L-01 | 번갈아 분배 | `/api/whoami.txt` 6번 (각각 새 연결) | A, B, A, B, A, B | ✅ |
| L-02 | 한 연결 안에서도 번갈아 분배 | `curl`에 URL 4개를 주어 keep-alive 연결 하나로 요청 | A, B, A, B | ✅ |
| L-03 | 동시 요청 | 10개를 동시에 요청 | A 5개, B 5개 (순서는 섞일 수 있음) | ✅ |
| L-04 | 프록시 로그 | `p.log`에서 `->` 줄 | `GET /api/whoami.txt HTTP/1.1 200 -> 127.0.0.1:8001` 처럼 보낸 백엔드가 찍힘 | ✅ |
| L-05 | POST 본문 중계 | 프록시 → tiny 백엔드의 `/cgi-bin/adder`에 POST `a=1&b=2` | 200, 답 3 (기능 테스트 `test_tiny_as_backend_post_adder`) | ✅ |
| L-06 | HEAD 중계 | `curl -I /api/whoami.txt` | 200, 헤더만 오고 본문 없음 | ✅ |

### 2-2. 장애 대응

| ID | 확인 내용 | 방법 | 기대 결과 | 결과 |
|---|---|---|---|---|
| L-07 | 백엔드 하나 꺼짐 | A를 끄고 4번 요청 | 4번 모두 B가 200으로 응답 (A 차례였던 요청도 B로 넘어감) | ✅ |
| L-08 | 꺼진 백엔드 복구 | A를 다시 켜고 4번 요청 | 별도 설정 없이 A, B 번갈아 응답으로 돌아옴 | ✅ |
| L-09 | 백엔드 모두 꺼짐 | B까지 끄고 2번 요청 | 502 Bad Gateway | ✅ |
| L-10 | 모두 꺼져도 프록시는 살아 있음 | 그 상태에서 `/home.html` | 200 (프록시가 직접 처리) | ✅ |

### 2-3. 라우팅 경계

| ID | 확인 내용 | 방법 | 기대 결과 | 결과 |
|---|---|---|---|---|
| L-11 | 접두사 경계 | `/apiary` | 백엔드로 보내지 않고 프록시가 직접 처리 → 404 | ✅ |
| L-12 | 대소문자 구분 | `/API/whoami.txt` | 규칙과 맞지 않음 → 프록시가 직접 처리 → 404 | ✅ |
| L-13 | 백엔드에 없는 파일 | `/api/nope.txt` | 백엔드의 404를 그대로 전달 | ✅ |
| L-14 | 경로 탈출 | `curl --path-as-is /api/../home.html` | 백엔드로 보내기 전에 403 | ✅ |

### 2-4. 잘못된 `--route`는 시작을 거부

| ID | 입력 | 기대 메시지 | 결과 |
|---|---|---|---|
| L-15 | `--route /x=127.0.0.1:<자기 포트>` | `백엔드 '...'가 이 프록시 자신입니다` | ✅ |
| L-16 | `--route api=127.0.0.1:1` | `접두사는 '/'로 시작해야 합니다` | ✅ |
| L-17 | `--route /a=127.0.0.1:99999` | `포트는 1~65535입니다` | ✅ |
| L-18 | `--route /a=... --route /a=...` | `같은 접두사가 두 번 나옵니다` | ✅ |

---

## 3. 자동화된 테스트와의 대응

위 수동 케이스 대부분은 `make functional`에서도 자동으로 확인합니다.

| 수동 케이스 | 자동 테스트 (`tests/functional/`) |
|---|---|
| B-02~B-05 | `phase2_test.py` (MIME 타입 표) |
| L-01 | `m6m7_test.py::test_round_robin` |
| L-05 | `test_tiny_as_backend_post_adder`, `test_post_body_and_type` |
| L-06 | `test_head` |
| L-07 | `test_one_backend_down` |
| L-09 | `test_all_backends_down_is_502` |
| L-11 | `test_prefix_boundary` |
| L-10 | `test_unmatched_served_locally` |
| L-14 | `test_unclean_paths_are_403` |
| L-15~L-18 | `test_bad_routes_refuse_to_start` |

자동 테스트가 없는 것: B-01, B-06~B-11의 브라우저 화면 확인, L-02(keep-alive 안의 분배), L-03(동시 요청 분배 비율), L-08(복구), L-12(대소문자).
