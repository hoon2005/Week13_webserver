#!/bin/sh
# 데모: 브라우저로 볼 서버를 띄웁니다. 웹 페이지는 demo/www/에 있습니다.
#
#   ./demo/web.sh            http://localhost:8000/
#   PORT=9000 ./demo/web.sh  포트 바꾸기
#
# 빌드하고, CGI 프로그램(build/adder)을 demo/www/cgi-bin/에 복사한 뒤 서버를 띄웁니다.
# Ctrl+C로 끕니다.
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=${PORT:-8000}

make -s all >/dev/null || { echo "빌드 실패"; exit 1; }
cp build/adder demo/www/cgi-bin/adder

BOLD=$(printf '\033[1m'); OFF=$(printf '\033[0m')
echo "${BOLD}브라우저에서 열기: http://localhost:$PORT/${OFF}"
echo "  웹 페이지 폴더: demo/www/"
echo "  끄기: Ctrl+C"
echo
exec ./build/tiny "$PORT" --root demo/www
