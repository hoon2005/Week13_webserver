# 검증 실행 방법

M8 검증을 다시 실행하는 방법입니다. 무엇을 확인하는지는 [테스트 시나리오](scenarios.csv)에, 실행 결과는 [M8 검증 기록](../measurements/001-m8-validation.md)에 있습니다.

## 명령어 요약

Linux 또는 WSL2 Ubuntu의 Bash에서 저장소 루트로 이동한 뒤, 위에서부터 한 단계씩 실행합니다. 실행 전에 [준비](#준비)를 확인하고, 로그를 파일에 남길 때는 [로그 남기기](#로그-남기기)를 먼저 실행합니다.

```bash
# 1~3. 개발 빌드: 단위·기능·포맷, 부하 중 메모리 오류·누수
make verify
python3 -B tests/stress/soak.py asan-load --conns 32 --seconds 60
python3 -B tests/stress/soak.py asan-load --conns 32 --seconds 600

# 4. TSan 빌드: 단위 검사, 실제 서버의 동시 요청·종료
make tsan

# 5~7. Release 빌드: 기능과 스트레스
make release
python3 -B tests/test_output.py check functional build/release \
  --functional tests/functional/baseline_test.py tests/functional/scenario_test.py
python3 -B tests/stress/stress.py

# 8. Release 장시간 자원 검사 (10분)
python3 -B tests/stress/soak.py long-run --conns 32 --minutes 10 --interval 60

# 9. Release 전체 성능 측정 (성능을 다시 잴 때만 실행, 약 30분)
make load
```

- 각 번호의 판정은 [순서와 합격 기준](#순서와-합격-기준)에서 확인합니다. 7번 스트레스 검사는 화면에 나온 `기대`와 `결과`를 직접 대조합니다.
- 성능 예비 실행과 스레드 비교는 [성능 측정](#성능-측정), 시연 페이지의 기대 결과는 [브라우저 수동 확인](#브라우저-수동-확인).

## 준비

```bash
git rev-parse --show-toplevel
git status --short --branch
```

- 첫 명령으로 저장소 루트 경로를 확인합니다. 루트에서 검증 명령을 실행합니다.
- 두 번째 명령으로 검증할 브랜치와 수정 파일을 확인합니다. 수정한 파일이 있으면 그 내용도 테스트에 포함됩니다.
- 도구 설치는 [개발 환경](../../CONVENTION.md#개발-환경)을 참고합니다. 장시간 자원 검사는 `/proc`이 있는 Linux 환경에서 실행합니다.
- 필요한 기록이 있으면 `make clean` 전에 보관합니다. `make clean`은 로그와 성능 결과를 포함한 `build/`를 지웁니다.

## 순서와 합격 기준

같은 코드를 세 가지로 빌드합니다. 검사 도구를 켜면 서버가 느려지고, 함께 켤 수 없는 도구도 있기 때문입니다.

| 순서 | 빌드 | 검사 | 시간 | 합격 기준 |
|---|---|---|---|---|
| 1 | 개발 | 단위·기능·포맷 | 약 1분 | `완료`, 실패·건너뜀 없음 |
| 2·3 | 개발 | 부하 중 메모리 오류·누수 | 1분·10분 | 요청 오류 0건, 서버 정상 종료, 메모리 오류·누수 보고 없음 |
| 4 | TSan | 단위 검사, 서버 동시 요청·종료 | | `완료`, 데이터 경합 보고 없음 |
| 5 | Release | 빌드 | | 오류 없이 완료 |
| 6 | Release | 핵심 기능 | 약 10초 | `완료`, 실패·건너뜀 없음 |
| 7 | Release | 스트레스 5개 | 약 2분 30초 | 아래 스트레스 표와 대조 |
| 8 | Release | 장시간 자원 | 10분 | 다섯 줄 모두 `[통과]`. 요청 오류 0건, 부하 종료 후 파일·소켓(fd), 스레드, 좀비 수가 시작값과 같고 메모리 사용량이 매번 연속 증가하지 않음 |
| 9 | Release | 성능 | 약 30분 | 측정마다 `[통과]`. 요청 오류 0건, 서버 정상 종료 |

- 2·3·7번은 1번에서 만든 개발 빌드의 실행 파일도 씁니다. 1번을 먼저 실행합니다.
- 9번은 성능을 다시 잴 때만 실행합니다. 다른 측정이나 프로그램과 동시에 돌리지 않습니다.

### 스트레스 결과 대조

스트레스 검사는 통과를 자동으로 판정하지 않습니다. 줄마다 `[수동 확인]`이 나오고, 그 아래 `기대`와 `결과`를 직접 비교합니다.

| 시나리오 | 기대 결과 |
|---|---|
| `conn-idle-flood` | `200`, 대기 시간이 출력된 상한 이하 |
| `keep-alive-hog` | 점유하던 연결이 끊긴 뒤 `200` |
| `cgi-slow-saturate` | CGI 16건 모두 `504`, 정적 요청 `200` |
| `proxy-loop` | 순환 요청 `504`, 정적 요청 `200` |
| `parse-header-size` | 큰 헤더 요청 `400`, 이후 정상 요청 `200` |

## 브라우저 수동 확인

| 확인 동작 | 기대 결과 |
|---|---|
| 정적 파일 화면에서 ‘없는 파일 요청’ 선택 | `404 Not Found` 표시 |
| CGI 계산기에서 `12 + 30`을 GET과 POST로 각각 실행 | 두 방식 모두 `42` 표시 |
| Round-Robin에서 요청 10개 전송 | 백엔드 A·B가 5개씩 응답 |

[브라우저 시연 실행 방법](../../demo/README.md#1-브라우저로-동작-확인)

## 로그 남기기

- **기능·TSan·자원 검사 등의 실행 로그**를 저장하려면 같은 터미널에서 아래 준비 명령을 한 번 실행합니다. 이후 `run_check`가 출력하는 `원본 로그:` 경로에서 이번 실행의 로그를 엽니다.
- **성능 측정 수치**는 자동으로 저장됩니다. [성능 결과 읽기](#성능-결과-읽기)에서 열어 볼 파일을 확인합니다.

```bash
LOGDIR="build/validation/$(date -u +%Y%m%d-%H%M%SZ)"
mkdir -p "$LOGDIR"
printf '이번 실행의 저장 폴더: %s/%s\n' "$PWD" "$LOGDIR"
set -o pipefail

run_check() {
  python3 -B tests/test_output.py run "$LOGDIR" "$@"
}
```

같은 터미널에서 로그를 남길 명령 앞에 `run_check <로그 이름>`을 붙입니다. 같은 이름으로 다시 실행해도 매번 별도 파일이 만들어집니다.

```bash
run_check dev-verify make verify
run_check release-stress python3 -B tests/stress/stress.py
```

### 실행한 코드와 환경 기록

어떤 커밋과 환경에서 나온 결과인지 보관할 때 실행합니다.

```bash
{
  date -u
  git rev-parse HEAD
  git status --short --branch
  uname -a
  nproc
  cc --version
  python3 --version
  clang-format --version
} 2>&1 | tee "$LOGDIR/environment.log"
```

`environment.log`에는 커밋, 운영체제, 코어 수, 도구 버전이 남습니다. 컴퓨터 이름도 들어가므로 공유하기 전에 확인합니다.

## 성능 측정

성능은 다른 검사가 끝난 뒤 따로 잽니다. `wrk`와 `ab`가 필요합니다. 전체 측정은 요약의 9번 `make load`를 실행합니다.

측정 도구와 결과 저장이 제대로 동작하는지 먼저 확인하려면 작은 정적 파일로 짧게 실행합니다.

```bash
python3 -B tests/stress/load.py static-small --quick
```

예비 실행 폴더는 측정 흐름을 확인하는 데 사용합니다. 최종 수치는 `make load`나 측정 조건을 정해 실행한 본 측정 폴더에서 가져옵니다.

### 보고서의 작업 스레드 비교 다시 실행하기

[측정 기록의 실험 B](../measurements/001-m8-validation.md#b-작업-스레드-수)는 동시 접속 **32개**에서 작업 스레드 수를 비교한 결과입니다. 기본 작업 스레드 16개보다 연결이 많은 상태에서 스레드를 늘리면 대기가 얼마나 줄어드는지 확인한 실험입니다.

`make load`는 실험 A에서 최고 처리량의 95%에 처음 도달한 접속 수를 B에 사용합니다. 보관된 전체 측정에서는 이 접속 수가 4개였습니다. 보고서와 같은 32개 조건을 재현하려면 아래 명령을 실행합니다.

```bash
python3 -B tests/stress/load.py threads --sat-conns 32
```

## 성능 결과 읽기

`make load`와 `tests/stress/load.py`는 실행할 때마다 `build/load/<시각>/`에 결과를 남깁니다. 최종 수치를 볼 때는 본 측정의 마지막 출력에 나온 `결과:` 경로에서 `report.md`를 엽니다.

| 파일 | 내용 |
|---|---|
| `report.md` | 측정 조건과 숫자 표 |
| `report.html` | 같은 결과의 그래프 |
| `results.json` | 다시 분석할 때 쓰는 원본 데이터 |

- 측정마다 워밍업 3초 뒤 10초씩 3회 재고, 처리량이 중간인 회차의 값을 씁니다.
- `도구 한계`가 표시된 구간은 부하 도구가 CPU를 다 써서, 그 처리량만으로 서버의 한계를 판단하지 않습니다.
- 한 측정만 다시 하거나 조건을 바꾸려면 `python3 -B tests/stress/load.py --help`를 봅니다.
