# 팀 규칙

> 상태: 초안. 첫 팀 회의에서 확정합니다. 확정 뒤에는 아래 8번 규칙으로만 바꿉니다.

1. **브랜치:** `main`은 PR로만 변경합니다. 작업 브랜치는 `<이름>/<주제>`로 만듭니다(예: `dabin/http-parser`). 실험 브랜치도 같은 형식으로 만들 수 있지만 `main`으로 PR을 올리지 않습니다. 실험에서 쓸모 있다고 판명된 부분만 작업 브랜치로 옮겨 PR을 올립니다.
2. **PR 전:** `main`의 최신 내용을 작업 브랜치에 반영합니다.

   ```bash
   git fetch origin
   git merge origin/main
   ```

3. **병합:** 승인 수와 병합 방식은 팀 회의에서 정합니다(후보: 승인 1건 또는 2건 / merge commit 또는 squash). 승인 후에 diff가 바뀌면 다시 승인을 받습니다.
4. **`main` 실패:** `main`의 CI가 실패하면 본 사람이 되돌리거나 고치는 일을 먼저 합니다.
5. **커밋:** 첫 줄에 변경 목적과 결과를 한국어 문장으로 적습니다.

   ```text
   Content-Length만큼 본문을 다 읽은 뒤에 요청을 처리하도록 고칩니다
   ```

6. **설명:** 작성자는 PR을 올리기 전에 본문과 diff를 읽고, 생성 파일과 비밀 정보가 없는지 확인합니다. 병합된 코드는 누가 작성했든 원리를 말로 설명할 수 있어야 합니다.
7. **PR 본문:** `.github/pull_request_template.md`의 칸을 채웁니다. 특히 요청이 어디까지 처리되는지와 curl, 로그, 테스트로 확인하는 방법을 적습니다.
8. **규칙 변경:** 이 파일도 PR과 3번에서 정한 승인으로 변경합니다. GitHub 설정(브랜치 보호 등)은 PR로 바꿀 수 없으므로, PR에 적어 승인받은 뒤에 레포 오너가 반영합니다.
9. **판정:** 빌드와 테스트 결과는 PR 하단의 CI에서 확인합니다. 검증 명령은 `make verify`입니다. 팀원이 WSL(Ubuntu)과 macOS를 함께 쓰므로, CI도 같은 명령을 Ubuntu와 macOS에서 실행합니다. 두 OS에서 같은 검증을 돌려 OS 차이를 일찍 찾는 것이 팀의 이식성 검증 방식입니다. PR 전에 로컬에서 먼저 실행합니다.

## 개발 환경

팀은 macOS와 WSL2+Ubuntu에서 개발합니다. 아래 설치 안내는 두 환경을 함께 적었습니다. WSL을 쓰면 Windows 폴더(`/mnt/c/...`)가 아닌 WSL 홈 폴더(`~/...`)에 레포를 받아야 빌드가 빠르고 파일 권한 문제가 없습니다.

| 도구 | 쓰는 곳 | 설치 |
| --- | --- | --- |
| C 컴파일러, make | 빌드 | Ubuntu: `sudo apt install build-essential` / macOS: `xcode-select --install` |
| python3 | 기능 테스트 | Ubuntu: 기본으로 있습니다 / macOS: 위의 `xcode-select --install`로 함께 설치됩니다 |
| clang-format 21.1.8 | `make lint`, `make format` | `pipx install clang-format==21.1.8` |
| curl | 기능 확인 | Ubuntu: `sudo apt install curl` / macOS: 기본으로 있습니다 |
| wrk 또는 ab | 부하 테스트 | Ubuntu: `sudo apt install wrk apache2-utils` / macOS: `brew install wrk` |

- macOS의 `brew` 명령은 Homebrew가 있어야 동작합니다. Homebrew는 macOS에 기본으로 들어 있지 않으므로, 없으면 https://brew.sh 안내대로 먼저 설치합니다.
- pipx가 없으면 Ubuntu는 `sudo apt install pipx`, macOS는 `brew install pipx`로 설치하고 `pipx ensurepath`를 한 번 실행합니다. 그다음 터미널을 새로 엽니다.
- clang-format은 CI와 같은 버전을 씁니다. 버전이 다르면 서로의 줄을 계속 다시 포맷하게 됩니다.
- 설치가 끝나면 아래 명령으로 도구를 확인합니다. C 파일과 테스트가 없을 때는 `make verify`가 컴파일러, python3, clang-format을 실행하지 않습니다. 그래서 `make verify`가 통과해도 도구가 설치됐는지는 알 수 없습니다.

  ```bash
  cc --version
  python3 --version
  clang-format --version   # 21.1.8이어야 합니다
  ```

## 빌드 명령

```bash
make            # 개발 빌드 (ASan + UBSan)
make test       # 단위 테스트
make functional # 기능 테스트
make lint       # 코드 모양 검사. 위반이나 설정 오류가 있으면 실패로 끝납니다
make format     # 코드 모양 자동 수정
make verify     # CI와 같은 전체 검증
make release    # 부하 테스트용 -O2 빌드 (sanitizer 없음)
make tsan       # 단위 테스트를 ThreadSanitizer로 실행
make help       # 명령 목록
```

평소 작업 흐름은 아래와 같습니다. `make verify`는 위반을 알려 주기만 하고 고치지 않으므로, 그 전에 `make format`을 실행합니다. `make lint`와 `make format`은 git이 아는 파일만 보므로 새 파일은 먼저 `git add`합니다.

```text
코드 수정 → make → ./build/<이름> 실행 후 curl로 확인 → git add → make format → make verify → 커밋, 푸시, PR
```

### 소스 배치

| 위치 | 내용 | 빌드 결과 |
| --- | --- | --- |
| `src/*.c`, `src/*.h` | 여러 실행 파일과 테스트가 함께 쓰는 모듈. `main()`이 없습니다 | `build/obj/*.o` |
| `src/main/<이름>.c` | `main()`이 있는 파일 하나가 실행 파일 하나입니다 | `build/<이름>` |
| `tests/<모듈>_test.c` | 모듈 하나의 단위 테스트 | `build/test/<모듈>_test` |
| `tests/functional/<이름>_test.py` | 실행 파일을 띄워 바깥에서 요청을 보내는 기능 테스트. 첫 인자로 `build` 폴더 경로를 받습니다 | 없음 |

파일을 추가하면 Makefile을 고치지 않아도 빌드 대상에 들어갑니다. 단, `src/main/`의 파일 이름은 `build/` 아래 폴더 이름(`obj`, `test`, `release`, `tsan`)과 겹치면 안 됩니다.

## 코드

빌드와 코드 검사의 언어 기준은 C11과 POSIX(`_POSIX_C_SOURCE=200809L`)입니다. LMS는 한 소스를 여러 환경(macOS, WSL, Linux)에서 빌드하려면 이식성을 고려하라고 합니다. 팀원이 macOS와 WSL을 함께 쓰므로, POSIX 안에 머물고 POSIX 밖의 함수(`epoll`, `kqueue` 등)를 쓰면 `#ifdef`로 플랫폼을 나눕니다.

모양은 `.clang-format`이 정하고, `make lint`(`clang-format --dry-run --Werror`)가 검사합니다. Linux 스타일에 스페이스 4칸 들여쓰기, 항상 쓰는 중괄호, 80열을 더했습니다. 아래 `이름`과 `함수` 규칙은 도구가 검사하지 않으므로 리뷰에서 봅니다.

- **에디터:** clangd를 쓰면 `compile_flags.txt`를 읽고 빌드와 같은 옵션으로 경고를 보여 줍니다. `.editorconfig`는 줄바꿈, 들여쓰기, 파일 끝 줄바꿈을 맞춥니다.
- **검사 대상:** `make lint`와 `make format`은 git이 아는 파일만 봅니다. 새 파일은 `git add`한 뒤에 검사됩니다.
- **가져온 코드:** `.clang-format-ignore`에 적은 파일(`tests/acutest.h`)은 clang-format이 건너뜁니다. 원본과 대조할 수 있게 원래 모양을 지킵니다.
- **외부 라이브러리:** LMS가 완성된 HTTP 또는 이벤트 루프 라이브러리(libevent, libuv, mongoose 등)를 금지합니다. 표준 C 라이브러리와 POSIX API만 씁니다. 테스트 도구(`acutest.h`)는 제품 코드에 링크하지 않습니다.
- **줄바꿈:** `.gitattributes`가 모든 텍스트 파일을 LF로 고정합니다. Windows에서 받은 파일이 CRLF이면 WSL의 `make`와 Python 스크립트가 깨지기 때문입니다.

### 이름

1. **이름은 `snake_case`로 짓고, 상수와 매크로는 대문자로 짓습니다**(`request_parse`, `HEADER_SIZE_MAX`).

2. **다른 파일에서 부르는 함수는 `모듈_동사`로 짓습니다.** 이름만 보고 어느 파일에 있는지 알 수 있습니다. 파일 안의 `static` 함수는 다루는 대상을 앞에 둡니다.
3. **동사의 뜻은 하나로 고정합니다.**

   | 동사 | 뜻 |
   |---|---|
   | `create` / `destroy` | 할당해서 만듭니다 / 해제합니다. 짝으로 씁니다 |
   | `init` | 이미 있는 메모리를 초기화합니다. 할당하지 않습니다 |
   | `set` / `get` | 값을 씁니다 / 읽습니다. `get`은 바꾸지 않습니다 |
   | `find` | 찾습니다. 없으면 `NULL`이나 `false`를 돌려줍니다 |
   | `is_` / `has_` | 참과 거짓을 묻고 `bool`을 돌려줍니다 |

4. **같은 개념은 모든 파일에서 같은 이름을 씁니다.** 새 개념이 생기면 이 표에 한 줄 더합니다.

   | 개념 | 이름 | 쓰지 않는 이름 |
   |---|---|---|
   | 파일 디스크립터 | `fd` | `file_descriptor` |

5. **구조체에는 `typedef`를 붙이지 않고, 숫자 상수에는 이름을 붙입니다**(`HEADER_SIZE_MAX`). `struct`가 보여야 값인지 구조체인지 바로 알 수 있기 때문입니다.

### 함수

1. **함수는 단계의 나열로 읽히게 씁니다.** 이름을 붙일 수 있는 독립된 작업은 분리했을 때 호출부가 더 읽기 쉬우면 동사 이름의 `static` 함수로 뺍니다. 자원 해제처럼 순서가 중요한 단계는 호출부에서 흐름이 보이게 씁니다.
2. **파일은 위에서 아래로 읽히게 배치합니다.** 파일 위쪽에 `static` 함수 선언을 모으고, 공개 함수(실행 파일은 `main`)를 먼저 둡니다. helper는 공개 함수가 처음 부르는 순서대로 그 뒤에 둡니다.

## 테스트

- 테스트 이름은 동작을 말하는 한국어 문장입니다. 검증하는 엣지 케이스는 테스트 이름에 드러나게 씁니다.
- 단위 테스트 도구는 `tests/acutest.h`입니다. `TEST_LIST`에 `{"한국어 문장", 함수}`로 적으면 그 문장이 테스트 이름으로 출력됩니다.

   ```c
   #include "acutest.h"

   static void test_example(void)
   {
       TEST_CHECK(1 + 1 == 2);
   }

   TEST_LIST = {
       {"1 더하기 1은 2다", test_example},
       {NULL, NULL},
   };
   ```

- 테스트는 공개 함수를 호출해 동작을 확인합니다. 기대값은 검사 대상 함수로 계산하지 않고 직접 적습니다.
- 기능 테스트는 서버를 빈 포트에 띄우고, 끝나면 반드시 종료합니다. 테스트가 쓰는 고정 입력(정적 파일 등)은 `tests/` 아래에 두고 커밋합니다.
- 메모리 검사는 `make test`의 ASan이 맡습니다. Linux에서는 프로그램이 끝날 때 누수도 함께 보고합니다. macOS의 기본 clang은 누수 검사를 지원하지 않으므로, 누수 확인은 Linux(WSL 또는 CI)의 결과로 판정합니다.

## 문서 위치

| 내용 | 남는 곳 |
| ---- | ------- |
| 개인 PRD 초안과 최종 PRD, 결정과 그 이유 | `docs/prd/` (작성 방법은 `docs/prd/README.md`) |
| 부하 테스트 등 측정 결과 | `docs/measurements/<세 자리 번호>-<이름>-<주제>.md` |
| 작은 이유 | 그 코드 줄의 주석 |
| 엣지 케이스 | 테스트 이름 |
| 확정된 팀 규칙 | 이 파일 |
