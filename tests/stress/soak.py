"""오래 부하를 주는 자원 검사 두 가지입니다 (시나리오 표 res-asan-load, res-long-run).

make verify에는 들어가지 않습니다(1분, 10분 걸림). 직접 실행합니다.

사용법:
    make && make release
    python3 tests/stress/soak.py asan-load              # 개발 빌드(ASan), 32개 동시 접속 1분
    python3 tests/stress/soak.py long-run               # release 빌드, 10분, 1분마다 기록
    python3 tests/stress/soak.py long-run --minutes 2   # 짧게 확인

판정
  asan-load  SIGTERM 뒤 종료 코드 0, 로그에 Sanitizer 보고 없음, 요청 오류 0
  long-run   fd 수, 스레드 수, 좀비 수는 처음과 같고 RSS는 계속 올라가지 않음 (Linux)
통과하면 종료 코드 0, 아니면 1입니다.
"""

import argparse
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FIXTURE_DIR = os.path.join(REPO_ROOT, "tests", "fixtures", "www")
HOST = "127.0.0.1"
PATHS = ("/home.html", "/style.css", "/cgi-bin/adder?1&2", "/api/home.html")


def free_port():
    with socket.socket() as s:
        s.bind((HOST, 0))
        return s.getsockname()[1]


def make_www(work, build):
    www = os.path.join(work, "www-%d" % len(os.listdir(work)))
    shutil.copytree(FIXTURE_DIR, www)
    shutil.copy2(os.path.join(build, "adder"), os.path.join(www, "cgi-bin", "adder"))
    for name in os.listdir(os.path.join(www, "cgi-bin")):
        os.chmod(os.path.join(www, "cgi-bin", name), 0o755)
    os.makedirs(os.path.join(www, "api"))
    shutil.copy2(os.path.join(www, "home.html"), os.path.join(www, "api", "home.html"))
    return www


class Tiny:
    def __init__(self, work, build, *args):
        self.port = free_port()
        self.log_path = os.path.join(work, "tiny-%d.log" % self.port)
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen(
            [os.path.join(build, "tiny"), str(self.port), "--root",
             make_www(work, build)] + list(args),
            stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                socket.create_connection((HOST, self.port), timeout=0.3).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("tiny가 뜨지 않았습니다")

    def stop(self, timeout=30):
        """SIGTERM으로 끄고 종료 코드를 돌려줌"""
        self.proc.send_signal(signal.SIGTERM)
        try:
            code = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            code = self.proc.wait()
        self.log.close()
        return code

    def read_log(self):
        with open(self.log_path, errors="replace") as f:
            return f.read()


class Load:
    """keep-alive 연결 conns개가 PATHS를 돌아가며 쉬지 않고 요청"""

    def __init__(self, port, conns):
        self.port = port
        self.stop_flag = threading.Event()
        self.ok = 0
        self.errors = []
        self.lock = threading.Lock()
        self.threads = [threading.Thread(target=self._run, args=(i,), daemon=True)
                        for i in range(conns)]

    def start(self):
        for t in self.threads:
            t.start()

    def stop(self):
        self.stop_flag.set()
        for t in self.threads:
            t.join()

    def _run(self, i):
        n = i
        while not self.stop_flag.is_set():
            try:
                with socket.create_connection((HOST, self.port), timeout=10) as s:
                    f = s.makefile("rb")
                    for _ in range(50):  # 연결 하나로 50번, 그다음 새 연결
                        if self.stop_flag.is_set():
                            break
                        path = PATHS[n % len(PATHS)]
                        n += 1
                        s.sendall(("GET %s HTTP/1.1\r\nHost: x\r\n\r\n" % path).encode())
                        status = int(f.readline().split()[1])
                        length = 0
                        while True:
                            line = f.readline()
                            if line.lower().startswith(b"content-length:"):
                                length = int(line.split(b":")[1])
                            if line in (b"\r\n", b""):
                                break
                        f.read(length)
                        with self.lock:
                            if status == 200:
                                self.ok += 1
                            else:
                                self.errors.append("%s → %d" % (path, status))
            except (OSError, ValueError, IndexError) as e:
                if not self.stop_flag.is_set():
                    with self.lock:
                        self.errors.append(repr(e))


def start_servers(work, build):
    backend = Tiny(work, build)
    proxy = Tiny(work, build, "--route", "/api=127.0.0.1:%d" % backend.port)
    return proxy, backend


def asan_load(args, work):
    build = os.path.join(REPO_ROOT, "build")
    proxy, backend = start_servers(work, build)
    load = Load(proxy.port, args.conns)
    print("asan-load: 개발 빌드(ASan), 동시 접속 %d개로 %d초 부하" % (args.conns, args.seconds))
    load.start()
    time.sleep(args.seconds)
    load.stop()
    codes = {"프록시": proxy.stop(), "백엔드": backend.stop()}
    logs = proxy.read_log() + backend.read_log()
    reports = [line for line in logs.splitlines()
               if "Sanitizer" in line or "runtime error" in line]
    print("  요청 %d건 성공, 오류 %d건" % (load.ok, len(load.errors)))
    print("  종료 코드: %s" % ", ".join("%s %d" % kv for kv in codes.items()))
    print("  Sanitizer 보고: %s" % ("없음" if not reports else "\n    ".join(reports[:10])))
    for e in load.errors[:5]:
        print("  오류 예: %s" % e)
    return all(c == 0 for c in codes.values()) and not reports and not load.errors


def snapshot(pid):
    fds = len(os.listdir("/proc/%d/fd" % pid))
    rss = threads = 0
    with open("/proc/%d/status" % pid) as f:
        for line in f:
            if line.startswith("VmRSS:"):
                rss = int(line.split()[1])
            elif line.startswith("Threads:"):
                threads = int(line.split()[1])
    out = subprocess.run(["ps", "-A", "-o", "ppid=,stat="], capture_output=True,
                         text=True).stdout
    zombies = sum(1 for line in out.splitlines()
                  if line.split()[0] == str(pid) and "Z" in line.split()[1])
    return {"fd": fds, "rss_kb": rss, "threads": threads, "zombies": zombies}


def long_run(args, work):
    if not sys.platform.startswith("linux"):
        print("long-run: /proc가 있는 Linux에서만 실행합니다")
        return True
    build = os.path.join(REPO_ROOT, "build", "release")
    proxy, backend = start_servers(work, build)
    pid = proxy.proc.pid
    load = Load(proxy.port, args.conns)
    print("long-run: release 빌드, 동시 접속 %d개로 %d분, %d초마다 기록 (프록시 프로세스)"
          % (args.conns, args.minutes, args.interval))
    samples = [("시작", snapshot(pid))]
    load.start()
    end = time.monotonic() + args.minutes * 60
    while time.monotonic() < end:
        time.sleep(min(args.interval, max(0, end - time.monotonic())))
        samples.append(("%d초" % (len(samples) * args.interval), snapshot(pid)))
    load.stop()
    time.sleep(2)  # 남은 연결이 정리되길 기다림
    samples.append(("부하 뒤", snapshot(pid)))
    proxy.stop()
    backend.stop()

    print("  %-8s %6s %10s %8s %6s" % ("시점", "fd", "RSS(KB)", "스레드", "좀비"))
    for label, s in samples:
        print("  %-8s %6d %10d %8d %6d" % (label, s["fd"], s["rss_kb"], s["threads"],
                                          s["zombies"]))
    print("  요청 %d건 성공, 오류 %d건" % (load.ok, len(load.errors)))

    first, last = samples[0][1], samples[-1][1]
    during = [s for _, s in samples[1:-1]]
    ok = True
    for key in ("fd", "threads", "zombies"):
        if last[key] != first[key]:
            print("  실패: %s가 처음 %d에서 부하 뒤 %d" % (key, first[key], last[key]))
            ok = False
    # RSS는 처음 몇 분 동안 늘 수 있음. 첫 기록 뒤로 계속 오르기만 하면 실패
    rss = [s["rss_kb"] for s in during]
    if len(rss) >= 3 and all(b > a for a, b in zip(rss, rss[1:])):
        print("  실패: RSS가 기록할 때마다 계속 올라감 %s" % rss)
        ok = False
    if load.errors:
        print("  실패: 요청 오류 %s" % load.errors[:5])
        ok = False
    return ok


def main():
    parser = argparse.ArgumentParser(description="오래 부하를 주는 자원 검사")
    parser.add_argument("check", choices=["asan-load", "long-run"])
    parser.add_argument("--conns", type=int, default=32)
    parser.add_argument("--seconds", type=int, default=60, help="asan-load 시간")
    parser.add_argument("--minutes", type=int, default=10, help="long-run 시간")
    parser.add_argument("--interval", type=int, default=60, help="long-run 기록 간격(초)")
    args = parser.parse_args()

    work = tempfile.mkdtemp(prefix="tiny-soak-")
    try:
        ok = asan_load(args, work) if args.check == "asan-load" else long_run(args, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("결과: %s" % ("통과" if ok else "실패"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
