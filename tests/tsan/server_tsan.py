"""conn-tsan-server: ThreadSanitizer로 빌드한 서버 전체에 데이터 경합이 없는지 봅니다.

사용법: python3 server_tsan.py <tsan build 폴더>   (make tsan이 실행)

TSan으로 빌드한 tiny를 프록시와 백엔드로 띄우고, 정적·CGI·중계 요청을 동시에 보낸 뒤
SIGTERM으로 끕니다. 경합이 하나라도 보고되면 종료 코드가 0이 아니고 로그에 남습니다.
"""

import os
import signal
import subprocess
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                                "functional"))
from m6m7_test import Server  # noqa: E402  (BUILD_DIR은 sys.argv[1]에서 읽음)

# 경고가 나도 끝까지 돌고, 끝날 때 종료 코드로 알림
ENV = dict(os.environ, TSAN_OPTIONS="halt_on_error=0 exitcode=66")
# TSan 빌드는 느리므로 요청 하나를 넉넉히 기다림 (기능 테스트의 기본값은 5초)
REQUEST_TIMEOUT = 30


def get(server, path, timeout=REQUEST_TIMEOUT):
    return server.exchange(("GET %s HTTP/1.1\r\nHost: x\r\n\r\n" % path).encode(),
                           timeout=timeout)[0]


def diagnose(servers):
    """실패했을 때 원인을 볼 수 있게 서버 상태, 로그, 스레드 스택을 모음.
    macOS CI에서 요청이 전부 시간 초과로 끝난 적이 있어, 다음 실행에서 원인을 보려고 넣음"""
    out = []
    for name, server in servers:
        out.append("== %s (pid %d, 종료 코드 %s)" % (name, server.proc.pid, server.proc.poll()))
        out.append(server.read_log()[-3000:])
        if server.proc.poll() is not None:
            continue
        if sys.platform == "darwin":
            cmd = ["sample", str(server.proc.pid), "1"]
        else:
            cmd = ["gdb", "-p", str(server.proc.pid), "-batch", "-ex", "thread apply all bt"]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
            # sample 출력의 끝은 라이브러리 목록이라, 그 앞의 스레드 스택만 앞에서부터 남김
            stacks = (r.stdout + r.stderr).split("Binary Images:")[0]
            out.append("-- 스레드 스택 (%s)\n%s" % (cmd[0], stacks[:20000]))
        except (OSError, subprocess.TimeoutExpired) as e:
            out.append("-- 스레드 스택을 못 얻음: %r" % e)
    return "\n".join(out)


class TsanServerTest(unittest.TestCase):
    def test_conn_tsan_server(self):
        backend = Server(env=ENV)
        os.makedirs(os.path.join(backend.www, "api"))
        with open(os.path.join(backend.www, "api", "home.html"), "w") as f:
            f.write("<p>backend</p>")
        proxy = Server("--route", "/api=127.0.0.1:%d" % backend.port, env=ENV)
        servers = (("proxy", proxy), ("backend", backend))
        paths = ("/home.html", "/cgi-bin/adder?1&2", "/api/home.html")
        # macOS의 TSan은 다른 스레드가 도는 중에 CGI용 fork를 하면 서버 전체가 멈출 수
        # 있어(macOS CI에서 재현), 동시 단계에서는 CGI를 뺌. CGI는 아래 순차 단계에서
        # 검사하고, fork와 동시 요청이 겹치는 경우는 Linux CI가 검사함
        if sys.platform == "darwin":
            concurrent_paths = ("/home.html", "/api/home.html")
        else:
            concurrent_paths = paths
        results = []
        lock = threading.Lock()

        def worker(i):
            for j in range(6):
                try:
                    status = get(proxy, concurrent_paths[(i + j) % len(concurrent_paths)])
                except OSError as e:
                    status = repr(e)
                with lock:
                    results.append(status)

        try:
            # 먼저 하나씩 보내 봄: 느린 것인지, 아예 응답하지 않는 것인지 구분
            for path in paths:
                started = time.monotonic()
                try:
                    status = get(proxy, path, timeout=60)
                except OSError as e:
                    self.fail("첫 요청 %s: %r (%.1f초)\n%s" % (
                        path, e, time.monotonic() - started, diagnose(servers)))
                self.assertEqual(status, 200, path)
            threads = [threading.Thread(target=worker, args=(i,)) for i in range(16)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            if results != [200] * len(results):
                self.fail("동시 요청 결과 %r\n%s" % (results, diagnose(servers)))
            for server in (proxy, backend):
                server.proc.send_signal(signal.SIGTERM)
                code = server.wait_exit(30)
                log = server.read_log()
                self.assertNotIn("ThreadSanitizer", log, log[-3000:])
                self.assertEqual(code, 0, log[-3000:])
        finally:
            proxy.cleanup()
            backend.cleanup()


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
