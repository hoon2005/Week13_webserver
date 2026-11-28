#!/bin/sh
# 데모: 노트북 한 대에서 서버 3개를 띄워 로드밸런싱(Round-Robin)과 장애 대응을 보여줍니다.
#
#   백엔드 A  127.0.0.1:8001  (demo/lb/www_a)
#   백엔드 B  127.0.0.1:8002  (demo/lb/www_b)
#   프록시    127.0.0.1:8000  /api로 시작하는 요청은 A, B로 번갈아 넘기고
#                             나머지는 직접 처리 (demo/www)
#
#   ./demo/lb.sh          단계마다 Enter를 누르면 다음으로 (발표용)
#   ./demo/lb.sh --auto   멈추지 않고 끝까지 (리허설용)
#
# 포트를 바꾸려면 BASE=9000 ./demo/lb.sh (프록시 9000, A 9001, B 9002)
set -u
cd "$(dirname "$0")/.." || exit 1

AUTO=0
[ "${1:-}" = "--auto" ] && AUTO=1
BASE=${BASE:-8000}
P=$BASE; PA=$((BASE + 1)); PB=$((BASE + 2))

BOLD=$(printf '\033[1m'); RED=$(printf '\033[31m'); GREEN=$(printf '\033[32m'); OFF=$(printf '\033[0m')

make -s all >/dev/null || { echo "빌드 실패"; exit 1; }

# 프록시가 직접 서비스할 폴더는 demo/www (web.sh와 같은 페이지). WORK에는 로그만
WORK=$(mktemp -d)
cp build/adder demo/www/cgi-bin/adder

PID_A=; PID_B=; PID_P=
cleanup() {
    for pid in $PID_P $PID_A $PID_B; do kill "$pid" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

pause() {
    echo
    if [ $AUTO -eq 0 ]; then
        printf "${BOLD}[Enter] %s${OFF}" "$1"
        read -r _
    else
        echo "${BOLD}== $1${OFF}"
    fi
}

# 포트가 열릴 때까지 최대 5초 기다림
wait_port() {
    i=0
    while ! curl -s -o /dev/null "http://127.0.0.1:$1/" 2>/dev/null; do
        i=$((i + 1))
        [ $i -gt 50 ] && { echo "포트 $1이 열리지 않습니다 (이미 쓰는 중인지 확인)"; exit 1; }
        sleep 0.1
    done
}

# 요청 n번: 몇 번째, 상태 코드, 본문
hit() {
    n=$1
    i=1
    while [ $i -le "$n" ]; do
        out=$(curl -s -w '\n%{http_code}' "http://127.0.0.1:$P/api/whoami.txt")
        code=$(printf '%s' "$out" | tail -n 1)
        body=$(printf '%s' "$out" | sed '$d' | head -n 1)
        if [ "$code" = 200 ]; then
            echo "  ${i}번째 요청 → ${GREEN}$code${OFF}  $body"
        else
            case "$code" in
            502) name="Bad Gateway (살아 있는 백엔드 없음)" ;;
            504) name="Gateway Timeout" ;;
            000) name="연결 실패" ;;
            *) name="" ;;
            esac
            echo "  ${i}번째 요청 → ${RED}$code${OFF}  $name"
        fi
        i=$((i + 1))
    done
}

echo "${BOLD}서버 3개를 띄웁니다${OFF}"
./build/tiny $PA --root demo/lb/www_a >"$WORK/a.log" 2>&1 &
PID_A=$!
./build/tiny $PB --root demo/lb/www_b >"$WORK/b.log" 2>&1 &
PID_B=$!
wait_port $PA
wait_port $PB
./build/tiny $P --root demo/www --route /api=127.0.0.1:$PA,127.0.0.1:$PB >"$WORK/p.log" 2>&1 &
PID_P=$!
wait_port $P
echo "  백엔드 A  127.0.0.1:$PA"
echo "  백엔드 B  127.0.0.1:$PB"
echo "  프록시    127.0.0.1:$P   (--route /api=127.0.0.1:$PA,127.0.0.1:$PB)"
echo "  브라우저: http://127.0.0.1:$P/ (프록시가 직접 처리)"

pause "1. 프록시에 /api/whoami.txt를 6번 요청 → A, B가 번갈아 응답"
hit 6

pause "2. 백엔드 A를 끕니다 → B가 모두 받음"
kill $PID_A; wait $PID_A 2>/dev/null; PID_A=
echo "  ${RED}백엔드 A 종료${OFF}"
hit 4

pause "3. 백엔드 B도 끕니다 → 502 Bad Gateway, 프록시는 계속 살아 있음"
kill $PID_B; wait $PID_B 2>/dev/null; PID_B=
echo "  ${RED}백엔드 B 종료${OFF}"
hit 2
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$P/home.html")
echo "  /home.html (프록시가 직접 처리) → ${GREEN}$code${OFF}"

pause "4. 프록시 로그: 어느 백엔드로 보냈는지"
grep -- "->" "$WORK/p.log" | tail -n 12 | sed 's/^/  /'

pause "끝. 서버를 모두 종료합니다"
