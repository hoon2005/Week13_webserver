"""웹서버 괴롭히기 시나리오 5개를 재현합니다 (M8 측정용).

make verify에는 들어가지 않습니다(오래 걸림). 직접 실행합니다.

사용법:
    make release
    python3 tests/stress/stress.py            # 시나리오 1~5 전부 (약 2분 30초)
    python3 tests/stress/stress.py 2 4        # 고른 시나리오만
    python3 tests/stress/stress.py 1 --conns 100   # 시나리오 1의 연결 수 조절

서버는 build/release/tiny(sanitizer 없음)를 임시 폴더에서 빈 포트로 띄우고,
시나리오가 끝나면 끕니다. 작업자 16명, 읽기 타임아웃 5초(기본값)입니다.
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FIXTURE_DIR = os.path.join(REPO_ROOT, "tests", "fixtures", "www")
TINY = os.path.join(REPO_ROOT, "build", "release", "tiny")
ADDER = os.path.join(REPO_ROOT, "build", "adder")
HOST = "127.0.0.1"


def free_port():
    with socket.socket() as s:
        s.bind((HOST, 0))
        return s.getsockname()[1]


class Tiny:
    """임시 www 폴더에서 tiny 하나를 띄움"""

    def __init__(self, www, *args):
        self.port = free_port()
        self.proc = subprocess.Popen(
            [TINY, str(self.port), "--root", www] + list(args),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                socket.create_connection((HOST, self.port), timeout=0.3).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("tiny가 뜨지 않았습니다")

    def stop(self):
        self.proc.kill()
        self.proc.wait()


def timed_get(port, path="/home.html", timeout=300):
    """정상 손님 한 명: (상태 코드, 걸린 초)"""
    start = time.monotonic()
    try:
        with socket.create_connection((HOST, port), timeout=timeout) as s:
            s.sendall(("GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
                       % path).encode())
            data = b""
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                data += chunk
        status = data.split(b" ", 2)[1].decode() if data else "응답 없음"
    except OSError as e:
        status = type(e).__name__
    return status, time.monotonic() - start


def report(title, how, result):
    print("\n[%s]" % title)
    print("  방법: %s" % how)
    print("  결과: %s" % result)


# ============================== 시나리오 ==============================


def scenario1(www, conns):
    server = Tiny(www)
    try:
        idle = [socket.create_connection((HOST, server.port)) for _ in range(conns)]
        time.sleep(0.3)
        status, sec = timed_get(server.port)
        for s in idle:
            s.close()
    finally:
        server.stop()
    report("시나리오 1. 아무것도 안 보내는 연결 %d개" % conns,
           "연결만 %d개 열어 두고 정상 손님이 home.html 요청" % conns,
           "%s, %.1f초 대기 (예상: %d개 ÷ 16명 × 5초 ≈ %.0f초)"
           % (status, sec, conns, conns / 16 * 5))


def scenario2(www):
    server = Tiny(www)
    stop = threading.Event()

    def hog():
        s = socket.create_connection((HOST, server.port))
        f = s.makefile("rb")
        for _ in range(5):  # 4초마다 요청 5번 (타임아웃 5초 직전)
            if stop.is_set():
                break
            s.sendall(b"GET /home.html HTTP/1.1\r\nHost: x\r\n\r\n")
            length = 0
            while True:
                line = f.readline()
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":")[1])
                if line in (b"\r\n", b""):
                    break
            f.read(length)
            time.sleep(4)
        s.close()

    try:
        threads = [threading.Thread(target=hog) for _ in range(16)]
        for t in threads:
            t.start()
        time.sleep(1)
        status, sec = timed_get(server.port)
        stop.set()
        for t in threads:
            t.join()
    finally:
        server.stop()
    report("시나리오 2. keep-alive 연결 16개가 4초마다 요청",
           "작업자 16명을 붙잡은 상태에서 정상 손님이 요청",
           "%s, %.1f초 대기 (공격 연결이 요청 5번을 마치고 끊길 때까지)" % (status, sec))


def scenario3(www):
    server = Tiny(www)
    try:
        threads = [threading.Thread(target=timed_get,
                                    args=(server.port, "/cgi-bin/slow"))
                   for _ in range(16)]
        for t in threads:
            t.start()
        time.sleep(0.5)
        status, sec = timed_get(server.port)
        for t in threads:
            t.join()
    finally:
        server.stop()
    report("시나리오 3. 느린 CGI 16개로 작업자 모두 점유",
           "sleep 30 하는 CGI 16개를 보낸 뒤 정상 손님이 정적 파일 요청",
           "%s, %.1f초 대기 (CGI 제한 10초에 풀림)" % (status, sec))


def scenario4(www):
    pa, pb = free_port(), free_port()
    procs = []
    try:
        for me, other in ((pa, pb), (pb, pa)):
            procs.append(subprocess.Popen(
                [TINY, str(me), "--root", www, "--route",
                 "/api=127.0.0.1:%d" % other, "--proxy-timeout", "3"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(0.5)
        result = {}

        def bystander():
            time.sleep(0.2)
            result["static"] = timed_get(pa)

        t = threading.Thread(target=bystander)
        t.start()
        status, sec = timed_get(pa, "/api/x")
        t.join()
    finally:
        for p in procs:
            p.kill()
            p.wait()
    report("시나리오 4. 프록시 두 대가 서로를 가리킴 (A→B→A)",
           "A의 /api → B, B의 /api → A로 설정하고 A에 /api/x 요청",
           "순환 요청 %s, %.1f초 / 그동안 다른 손님의 정적 요청 %s, %.1f초 대기"
           % (status, sec, result["static"][0], result["static"][1]))


def scenario5(www):
    server = Tiny(www)
    try:
        headers = b"".join(b"X-H%d: %s\r\n" % (i, b"a" * 90) for i in range(200))
        start = time.monotonic()
        with socket.create_connection((HOST, server.port), timeout=5) as s:
            s.sendall(b"GET /home.html HTTP/1.1\r\nHost: x\r\n" + headers + b"\r\n")
            first = s.recv(100).split(b"\r\n")[0].decode()
        sec = time.monotonic() - start
        after, _ = timed_get(server.port)
    finally:
        server.stop()
    report("시나리오 5. 헤더 폭탄 (100바이트 헤더 200줄, 약 20 KiB)",
           "큰 헤더를 붙인 요청을 보내고 이어서 정상 요청",
           "%s (%.3f초) / 그 뒤 정상 요청 %s" % (first, sec, after))


def main():
    parser = argparse.ArgumentParser(description="웹서버 괴롭히기 시나리오")
    parser.add_argument("scenarios", nargs="*", type=int, default=[1, 2, 3, 4, 5])
    parser.add_argument("--conns", type=int, default=300,
                        help="시나리오 1의 연결 수 (기본 300)")
    args = parser.parse_args()

    for path in (TINY, ADDER):
        if not os.access(path, os.X_OK):
            sys.exit("%s가 없습니다. 먼저 make && make release" % path)

    work = tempfile.mkdtemp(prefix="tiny-stress-")
    www = os.path.join(work, "www")
    shutil.copytree(FIXTURE_DIR, www)
    shutil.copy2(ADDER, os.path.join(www, "cgi-bin", "adder"))
    for name in os.listdir(os.path.join(www, "cgi-bin")):
        os.chmod(os.path.join(www, "cgi-bin", name), 0o755)

    runners = {1: lambda: scenario1(www, args.conns), 2: lambda: scenario2(www),
               3: lambda: scenario3(www), 4: lambda: scenario4(www),
               5: lambda: scenario5(www)}
    print("release 빌드, 작업자 16명, 읽기 타임아웃 5초")
    try:
        for n in args.scenarios:
            runners[n]()
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
