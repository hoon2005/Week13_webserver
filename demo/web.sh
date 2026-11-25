#!/bin/sh
# 데모: 브라우저로 볼 서버를 띄웁니다. 웹 페이지는 demo/www/에 있습니다.
#
#   ./demo/web.sh            http://localhost:8000/
#   BASE=9000 ./demo/web.sh  포트 바꾸기 (프록시 9000, A 9001, B 9002)
#
# 서버 3개를 띄웁니다.
#   백엔드 A  127.0.0.1:BASE+1  (demo/lb/www_a)
#   백엔드 B  127.0.0.1:BASE+2  (demo/lb/www_b)
#   메인      127.0.0.1:BASE    정적 파일, CGI는 직접 처리하고
#                               /api로 시작하는 요청은 A, B로 번갈아 넘김
# 메인 서버의 로그가 이 터미널에 그대로 찍힙니다. Ctrl+C로 모두 끕니다.
set -u
cd "$(dirname "$0")/.." || exit 1
BASE=${BASE:-${PORT:-8000}}
P=$BASE; PA=$((BASE + 1)); PB=$((BASE + 2))

make -s all >/dev/null || { echo "빌드 실패"; exit 1; }
cp build/adder demo/www/cgi-bin/adder

WORK=$(mktemp -d)
PID_A=; PID_B=
cleanup() {
    for pid in $PID_A $PID_B; do kill "$pid" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# 포트가 열릴 때까지 최대 5초 기다림
wait_port() {
    i=0
    while ! curl -s -o /dev/null "http://127.0.0.1:$1/" 2>/dev/null; do
        i=$((i + 1))
        [ $i -gt 50 ] && { echo "포트 $1이 열리지 않습니다 (이미 쓰는 중인지 확인)"; exit 1; }
        sleep 0.1
    done
}

./build/tiny $PA --root demo/lb/www_a >"$WORK/a.log" 2>&1 &
PID_A=$!
./build/tiny $PB --root demo/lb/www_b >"$WORK/b.log" 2>&1 &
PID_B=$!
wait_port $PA
wait_port $PB

BOLD=$(printf '\033[1m'); OFF=$(printf '\033[0m')
echo "${BOLD}브라우저에서 열기: http://localhost:$P/${OFF}"
echo "  백엔드 A  127.0.0.1:$PA  (demo/lb/www_a)"
echo "  백엔드 B  127.0.0.1:$PB  (demo/lb/www_b)"
echo "  메인      127.0.0.1:$P   --root demo/www --route /api=127.0.0.1:$PA,127.0.0.1:$PB"
echo "  끄기: Ctrl+C"
echo

./build/tiny $P --root demo/www --route /api=127.0.0.1:$PA,127.0.0.1:$PB
