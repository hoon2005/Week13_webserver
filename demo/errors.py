#!/usr/bin/env python3
"""데모: 일부러 이상한 요청과 실패 상황을 만들어, 서버가 맞는 상태 코드로 답하고
그 뒤에도 살아 있는지 표로 보여줍니다.

    python3 demo/errors.py          모든 상황 (CGI 시간 초과 때문에 약 15초)
    python3 demo/errors.py --fast   10초 걸리는 504(CGI 시간 초과)는 건너뜀

임시 폴더에 tests/fixtures/www와 build/adder를 복사하고 build/tiny를 띄웁니다.
/api는 아무도 듣지 않는 포트로 보내도록 걸어서 502를 만듭니다. 끝나면 서버를
끄고 임시 폴더를 지웁니다. 먼저 make로 빌드되어 있어야 합니다.
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(REPO, "build")
FIXTURE = os.path.join(REPO, "tests", "fixtures", "www")
HOST = "127.0.0.1"

GREEN, RED, BOLD, DIM, OFF = "\033[32m", "\033[31m", "\033[1m", "\033[2m", "\033[0m"


def free_port():
    with socket.socket() as s:
        s.bind((HOST, 0))
        return s.getsockname()[1]


def send_raw(port, data, timeout=15):
    """data를 보내고 서버가 닫을 때까지 읽어 상태 코드(int)를 돌려줍니다."""
    with socket.create_connection((HOST, port), timeout=timeout) as s:
        s.sendall(data)
        buf = b""
        while b"\r\n" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
    line = buf.split(b"\r\n", 1)[0].decode(errors="replace")
    parts = line.split()
    return int(parts[1]) if len(parts) >= 2 and parts[1].isdigit() else None


def req(method, path, extra=b""):
    return (
        f"{method} {path} HTTP/1.1\r\nHost: demo\r\nConnection: close\r\n".encode()
        + extra
        + b"\r\n"
    )


def drop_mid_request(port):
    """요청을 반쯤 보내고 끊은 뒤, 서버가 다음 요청을 정상 처리하는지."""
    s = socket.create_connection((HOST, port))
    s.sendall(b"POST /cgi-bin/adder HTTP/1.1\r\nHost: demo\r\nContent-Length: 100\r\n\r\nabc")
    s.close()
    time.sleep(0.2)
    return send_raw(port, req("GET", "/home.html"))


def main():
    fast = "--fast" in sys.argv
    tiny = os.path.join(BUILD, "tiny")
    adder = os.path.join(BUILD, "adder")
    for p in (tiny, adder):
        if not os.path.exists(p):
            sys.exit(f"{p}가 없습니다. 먼저 make를 실행하세요.")

    work = tempfile.mkdtemp()
    www = os.path.join(work, "www")
    shutil.copytree(FIXTURE, www)
    shutil.copy2(adder, os.path.join(www, "cgi-bin", "adder"))

    port = free_port()
    dead = free_port()  # 아무도 듣지 않는 포트: 백엔드가 죽은 상황
    log = open(os.path.join(work, "tiny.log"), "w")
    proc = subprocess.Popen(
        [tiny, str(port), "--root", www, "--route", f"/api={HOST}:{dead}"],
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    try:
        for _ in range(50):
            try:
                socket.create_connection((HOST, port), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.1)

        big = 2 * 1024 * 1024
        cases = [
            ("정상 요청", "GET /home.html", 200,
             lambda: send_raw(port, req("GET", "/home.html"))),
            ("CGI 덧셈", "GET /cgi-bin/adder?1&2", 200,
             lambda: send_raw(port, req("GET", "/cgi-bin/adder?1&2"))),
            ("없는 파일", "GET /nope.html", 404,
             lambda: send_raw(port, req("GET", "/nope.html"))),
            ("정적 파일에 POST", "POST /home.html", 405,
             lambda: send_raw(port, req("POST", "/home.html", b"Content-Length: 0\r\n"))),
            ("요청 줄이 깨짐", "HELLO", 400,
             lambda: send_raw(port, b"HELLO\r\n\r\n")),
            ("상위 폴더로 탈출 시도", "GET /../etc/passwd", 403,
             lambda: send_raw(port, req("GET", "/../etc/passwd"))),
            ("본문이 너무 큼 (2 MiB)", "POST, Content-Length: 2097152", 413,
             lambda: send_raw(port, req("POST", "/cgi-bin/adder",
                                        f"Content-Length: {big}\r\n".encode()))),
            ("지원하지 않는 메서드", "DELETE /home.html", 501,
             lambda: send_raw(port, req("DELETE", "/home.html"))),
            ("CGI가 실패하고 종료", "GET /cgi-bin/fail", 500,
             lambda: send_raw(port, req("GET", "/cgi-bin/fail"))),
            ("CGI 출력이 1 MiB 초과", "GET /cgi-bin/big", 500,
             lambda: send_raw(port, req("GET", "/cgi-bin/big"))),
            ("백엔드가 모두 꺼짐", "GET /api/users", 502,
             lambda: send_raw(port, req("GET", "/api/users"))),
        ]
        if not fast:
            cases.append(("CGI가 끝나지 않음 (10초 대기)", "GET /cgi-bin/slow", 504,
                          lambda: send_raw(port, req("GET", "/cgi-bin/slow"))))
        cases.append(("요청 도중 연결 끊김 → 다음 요청", "POST 반쯤 → 끊기 → GET", 200,
                      lambda: drop_mid_request(port)))

        print(f"{BOLD}서버: 127.0.0.1:{port}   (/api → 127.0.0.1:{dead}, 듣는 서버 없음){OFF}\n")
        print(f"{BOLD}  {pad('상황', 34)}{pad('보낸 요청', 32)}기대  실제{OFF}")
        passed = 0
        for name, sent, want, run in cases:
            if want == 504:
                print(f"{DIM}  ... CGI 시간 초과를 기다리는 중 (10초){OFF}", end="\r", flush=True)
            try:
                got = run()
            except OSError as e:
                got = None
                sent += f" ({e.__class__.__name__})"
            ok = got == want
            passed += ok
            mark = f"{GREEN}✅{OFF}" if ok else f"{RED}❌{OFF}"
            print(f"\r\033[K  {pad(name, 34)}{pad(sent, 32)}{want:>4}  {str(got):>4}  {mark}")

        alive = proc.poll() is None
        print()
        print(f"  서버 프로세스: {GREEN + '살아 있음' + OFF if alive else RED + '죽음' + OFF}")
        color = GREEN if passed == len(cases) and alive else RED
        print(f"{color}{BOLD}  {passed}/{len(cases)} 통과{OFF}")
        return 0 if passed == len(cases) and alive else 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        shutil.rmtree(work, ignore_errors=True)


def pad(text, width):
    """한글은 화면에서 두 칸을 차지하므로 그만큼 빼고 공백을 채웁니다."""
    w = sum(2 if ord(c) > 0x1100 else 1 for c in text)
    return text + " " * max(1, width - w)


if __name__ == "__main__":
    sys.exit(main())
