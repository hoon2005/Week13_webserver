# 검증 실행 방법

M8 검증을 다시 실행하는 방법입니다. 무엇을 확인하는지는 [테스트 시나리오](scenarios.csv)에, 실행 결과는 [M8 검증 기록](../measurements/001-m8-validation.md)에 있습니다.

## 준비

- Linux 또는 WSL2 Ubuntu의 Bash에서 저장소 루트로 이동합니다. 장시간 자원 검사는 `/proc`이 있는 Linux에서만 실행됩니다.
- 도구는 [CONVENTION.md](../../CONVENTION.md#개발-환경)를 따릅니다. 성능 측정(`make load`)에는 `wrk`와 `ab`가 모두 필요합니다.
- 검증할 커밋에서 수정된 파일이 없는지 `git status --short`로 확인합니다. 수정된 파일이 있으면 그 수정까지 결과에 섞입니다.
- 결과를 다시 볼 일이 있으면 `make clean`을 실행하지 않습니다. 로그와 측정 결과가 `build/` 아래에 있습니다.

## 순서와 합격 기준

같은 코드를 세 가지로 빌드합니다. 검사 도구를 켜면 서버가 느려지고, 함께 켤 수 없는 도구도 있기 때문입니다.

| 순서 | 빌드 | 명령 | 시간 | 합격 기준 |
|---|---|---|---|---|
| 1 | 개발 | `make verify` | 약 1분 | 마지막 줄에 `완료`, `[실패]` 없음 |
| 2 | 개발 | `python3 -B tests/stress/soak.py asan-load --conns 32 --seconds 60` | 1분 | 세 줄 모두 `[통과]` |
| 3 | 개발 | 2번 명령에서 `--seconds 600` | 10분 | 세 줄 모두 `[통과]` |
| 4 | TSan | `make tsan` | | `완료`, 데이터 경합 보고 없음 |
| 5 | Release | `make release` | | 오류 없이 끝남 |
| 6 | Release | `python3 -B tests/test_output.py check functional build/release --functional tests/functional/baseline_test.py tests/functional/scenario_test.py` | 약 10초 | `완료`, `[실패]` 없음 |
| 7 | Release | `python3 -B tests/stress/stress.py` | 약 2분 30초 | 아래 표와 대조 |
| 8 | Release | `python3 -B tests/stress/soak.py long-run --conns 32 --minutes 10 --interval 60` | 10분 | 다섯 줄 모두 `[통과]` |
| 9 | Release | `make load` | 약 30분 | 측정마다 `[통과]` (요청 오류 0, 서버 정상 종료) |

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

## 로그 남기기

위 명령은 결과를 화면에만 보여 줍니다. 원본 로그를 파일로 남기려면 같은 터미널에서 아래를 한 번 실행합니다.

```bash
LOGDIR="build/validation/$(date -u +%Y%m%d-%H%M%SZ)"
mkdir -p "$LOGDIR"
set -o pipefail
run_check() { python3 -B tests/test_output.py run "$LOGDIR" "$@"; }
{ date -u; git rev-parse HEAD; git status --short --branch; uname -a; nproc; cc --version; python3 --version; clang-format --version; } 2>&1 | tee "$LOGDIR/environment.log"
```

그다음 각 명령 앞에 `run_check <로그 이름>`을 붙입니다. 명령이 끝나면 `원본 로그: ...`로 파일 위치가 나옵니다.

```bash
run_check dev-verify make verify
run_check release-stress python3 -B tests/stress/stress.py
```

`environment.log`에는 커밋, 운영체제, 코어 수, 도구 버전이 남습니다. 컴퓨터 이름도 들어가므로 공유하기 전에 확인합니다.

## 성능 결과 읽기

`make load`와 `tests/stress/load.py`는 실행할 때마다 `build/load/<시각>/`에 결과를 남깁니다.

| 파일 | 내용 |
|---|---|
| `report.md` | 측정 조건과 숫자 표 |
| `report.html` | 같은 결과의 그래프 |
| `results.json` | 다시 분석할 때 쓰는 원본 데이터 |

- 측정마다 워밍업 3초 뒤 10초씩 3회 재고, 처리량이 중간인 회차의 값을 씁니다.
- `도구 한계`가 표시된 구간은 부하 도구가 CPU를 다 써서, 그 처리량만으로 서버의 한계를 판단하지 않습니다.
- 한 측정만 다시 하거나 조건을 바꾸려면 `python3 -B tests/stress/load.py --help`를 봅니다.
