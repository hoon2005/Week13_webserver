#!/bin/sh
# 데모: 코드에 일부러 버그를 넣으면 테스트가 잡는지 보여줍니다.
#
#   ./demo/bug.sh                 버그 목록
#   ./demo/bug.sh <버그 이름>     버그 넣기 → 빌드 → 관련 테스트(실패해야 함)
#                                 → 원래대로 되돌리기 → 같은 테스트(통과해야 함)
#
# 버그는 demo/bugs/<이름>.patch에 있습니다. 끝나면(중간에 Ctrl+C를 눌러도)
# 항상 git apply -R로 되돌리므로 원본 코드는 바뀌지 않습니다.
set -u
cd "$(dirname "$0")/.." || exit 1

# 버그 이름 → "설명|단위 테스트|기능 테스트"
info() {
    case "$1" in
    cgi_timeout_500)
        echo "CGI 시간 초과 때 504 대신 500을 보냄|-|phase2_test Phase2Test.test_cgi_timeout_is_504" ;;
    round_robin_first_only)
        echo "로드밸런서가 항상 첫 번째 백엔드만 고름|proxy_test round_robin_order|m6m7_test ProxyTest.test_round_robin" ;;
    css_mime_plain)
        echo ".css 파일을 text/css 대신 text/plain으로 보냄|http_test mime|phase2_test Phase2Test.test_mime" ;;
    *) return 1 ;;
    esac
}

if [ $# -ne 1 ] || ! info "$1" >/dev/null; then
    echo "사용법: $0 <버그 이름>"
    echo
    for f in demo/bugs/*.patch; do
        name=$(basename "$f" .patch)
        printf "  %-24s %s\n" "$name" "$(info "$name" | cut -d'|' -f1)"
    done
    exit 2
fi

BUG=$1
PATCH=demo/bugs/$BUG.patch
DESC=$(info "$BUG" | cut -d'|' -f1)
UNIT=$(info "$BUG" | cut -d'|' -f2)
FUNC=$(info "$BUG" | cut -d'|' -f3)

RED=$(printf '\033[31m'); GREEN=$(printf '\033[32m'); BOLD=$(printf '\033[1m'); OFF=$(printf '\033[0m')

if ! git apply --check "$PATCH" 2>/dev/null; then
    echo "패치를 적용할 수 없습니다. 코드가 이미 바뀌었는지 git status로 확인하세요."
    exit 1
fi

applied=0
revert() {
    if [ $applied -eq 1 ]; then
        git apply -R "$PATCH" && applied=0
    fi
}
trap 'revert; exit 130' INT TERM
trap 'revert' EXIT

# 관련 테스트만 실행. 성공하면 0
run_tests() {
    rc=0
    if [ "$UNIT" != "-" ]; then
        set -- $UNIT
        echo "${BOLD}\$ ./build/test/$1 $2${OFF}"
        "./build/test/$1" "$2" || rc=1
    fi
    set -- $FUNC
    echo "${BOLD}\$ 기능 테스트 $1 $2${OFF}"
    python3 -B -c '
import sys, unittest
mod, name = sys.argv[1], sys.argv[2]
sys.argv = [sys.argv[0], "build"]
sys.path.insert(0, "tests/functional")
m = __import__(mod)
suite = unittest.defaultTestLoader.loadTestsFromName(name, m)
ok = unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful()
sys.exit(0 if ok else 1)
' "$1" "$2" || rc=1
    return $rc
}

build() {
    make -s all >/dev/null || { echo "빌드 실패"; exit 1; }
}

echo "${BOLD}== 1. 버그 넣기: $DESC${OFF}"
git apply "$PATCH" && applied=1
git --no-pager diff --color -- src
build

echo
echo "${BOLD}== 2. 버그가 있는 코드로 테스트 (실패해야 정상)${OFF}"
if run_tests; then
    echo "${RED}테스트가 버그를 잡지 못했습니다${OFF}"
    caught=0
else
    echo "${RED}${BOLD}>> 테스트가 버그를 잡았습니다 (FAIL)${OFF}"
    caught=1
fi

echo
echo "${BOLD}== 3. 원래 코드로 되돌리고 다시 테스트 (통과해야 정상)${OFF}"
revert
build
if run_tests; then
    echo "${GREEN}${BOLD}>> 원래 코드는 통과합니다 (OK)${OFF}"
    [ $caught -eq 1 ] && exit 0
fi
exit 1
