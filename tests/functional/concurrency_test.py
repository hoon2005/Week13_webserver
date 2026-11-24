"""여러 손님이 동시에 접속했을 때의 기능 테스트입니다.

사용법: python3 concurrency_test.py <build 폴더>
"""

import concurrent.futures
import http.client
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FIXTURE_DIR = os.path.join(REPO_ROOT, "tests", "fixtures", "www")
BUILD_DIR = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build")

HOST = "127.0.0.1"
WORKER_COUNT = 16


class ConcurrencyTest(unittest.TestCase):
    def setUp(self):
        self.work_dir = tempfile.mkdtemp(prefix="tiny-concurrency-")
        self.addCleanup(shutil.rmtree, self.work_dir, True)
        www = os.path.join(self.work_dir, "www")
        shutil.copytree(FIXTURE_DIR, www)
        os.makedirs(os.path.join(www, "cgi-bin"))
        shutil.copy2(os.path.join(BUILD_DIR, "adder"), os.path.join(www, "cgi-bin"))

        with socket.socket() as sock:
            sock.bind((HOST, 0))
            self.port = sock.getsockname()[1]
        self.proc = subprocess.Popen(
            [os.path.join(BUILD_DIR, "tiny"), str(self.port)],
            cwd=www,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self.addCleanup(self.stop_server)
        self.wait_until_listening()

    def stop_server(self):
        self.proc.kill()
        self.proc.wait()

    def wait_until_listening(self):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                socket.create_connection((HOST, self.port), timeout=0.5).close()
                return
            except OSError:
                time.sleep(0.05)
        self.fail("서버가 포트를 열지 않았습니다")

    def get(self, path, timeout=10):
        conn = http.client.HTTPConnection(HOST, self.port, timeout=timeout)
        try:
            conn.request("GET", path)
            resp = conn.getresponse()
            return resp.status, resp.read()
        finally:
            conn.close()

    def open_idle(self, count):
        """연결만 걸고 아무것도 보내지 않는 손님을 만듭니다."""
        for _ in range(count):
            sock = socket.create_connection((HOST, self.port), timeout=5)
            self.addCleanup(sock.close)

    def test_조용한_손님이_하나_있어도_다른_손님은_바로_응답을_받는다(self):
        self.open_idle(1)
        self.assertEqual(self.get("/home.html", timeout=2)[0], 200)

    def test_작업자가_한_명만_남아도_응답을_받는다(self):
        self.open_idle(WORKER_COUNT - 1)
        self.assertEqual(self.get("/home.html", timeout=2)[0], 200)

    def test_파일_요청_100개를_동시에_보내도_모두_같은_내용을_받는다(self):
        with open(os.path.join(FIXTURE_DIR, "home.html"), "rb") as f:
            expected = f.read()
        with concurrent.futures.ThreadPoolExecutor(50) as pool:
            for result in pool.map(self.get, ["/home.html"] * 100):
                self.assertEqual(result, (200, expected))

    def test_CGI를_동시에_실행해도_각자_자기_계산_결과를_받는다(self):
        paths = ["/cgi-bin/adder?%d&1000" % i for i in range(40)]
        with concurrent.futures.ThreadPoolExecutor(20) as pool:
            for i, (status, body) in enumerate(pool.map(self.get, paths)):
                self.assertEqual(status, 200)
                self.assertIn(b"%d + 1000 = %d" % (i, i + 1000), body)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
