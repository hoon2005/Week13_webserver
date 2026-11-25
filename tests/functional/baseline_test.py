"""Tiny 원본의 기준선 기능 테스트입니다.

사용법: python3 baseline_test.py <build 폴더>

임시 폴더에 tests/fixtures/www/ 내용과 build/adder(cgi-bin/adder)를 복사하고,
그 폴더를 현재 폴더로 해서 build/tiny를 빈 포트로 띄웁니다. 레포 폴더에는
아무것도 쓰지 않습니다. 테스트가 끝나면 서버를 끄고 임시 폴더를 지웁니다.
"""

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
START_TIMEOUT_SEC = 10
REQUEST_TIMEOUT_SEC = 10


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((HOST, 0))
        return sock.getsockname()[1]


def wait_until_listening(proc, port, log_path):
    deadline = time.monotonic() + START_TIMEOUT_SEC
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(
                "tiny가 시작하자마자 종료했습니다 (종료 코드 %d)\n%s"
                % (proc.returncode, read_log(log_path))
            )
        try:
            with socket.create_connection((HOST, port), timeout=0.5):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(
        "tiny가 %d초 안에 포트 %d를 열지 않았습니다\n%s"
        % (START_TIMEOUT_SEC, port, read_log(log_path))
    )


def read_log(log_path):
    with open(log_path, "r", errors="replace") as log:
        return log.read()


class TinyBaselineTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tiny = os.path.join(BUILD_DIR, "tiny")
        adder = os.path.join(BUILD_DIR, "adder")
        for path in (tiny, adder):
            if not os.access(path, os.X_OK):
                raise RuntimeError("실행 파일이 없습니다: %s (먼저 make)" % path)

        cls.work_dir = tempfile.mkdtemp(prefix="tiny-baseline-")
        www = os.path.join(cls.work_dir, "www")
        shutil.copytree(FIXTURE_DIR, www)
        os.makedirs(os.path.join(www, "cgi-bin"), exist_ok=True)
        shutil.copy2(adder, os.path.join(www, "cgi-bin", "adder"))

        cls.port = find_free_port()
        cls.log_path = os.path.join(cls.work_dir, "tiny.log")
        cls.log = open(cls.log_path, "w")
        cls.proc = subprocess.Popen(
            [tiny, str(cls.port)],
            cwd=www,
            stdin=subprocess.DEVNULL,
            stdout=cls.log,
            stderr=subprocess.STDOUT,
        )
        try:
            wait_until_listening(cls.proc, cls.port, cls.log_path)
        except Exception:
            cls.stop_server()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.stop_server()

    @classmethod
    def stop_server(cls):
        if cls.proc.poll() is None:
            cls.proc.terminate()
            try:
                cls.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                cls.proc.kill()
                cls.proc.wait()
        cls.log.close()
        shutil.rmtree(cls.work_dir, ignore_errors=True)

    def request(self, method, path):
        conn = http.client.HTTPConnection(
            HOST, self.port, timeout=REQUEST_TIMEOUT_SEC
        )
        try:
            conn.request(method, path)
            resp = conn.getresponse()
            return resp.status, resp.read()
        finally:
            conn.close()

    def test_get_home_html(self):
        status, body = self.request("GET", "/home.html")
        self.assertEqual(status, 200)
        with open(os.path.join(FIXTURE_DIR, "home.html"), "rb") as f:
            self.assertEqual(body, f.read())

    def test_get_root_serves_home_html(self):
        status, body = self.request("GET", "/")
        self.assertEqual(status, 200)
        with open(os.path.join(FIXTURE_DIR, "home.html"), "rb") as f:
            self.assertEqual(body, f.read())

    def test_get_adder_cgi(self):
        status, body = self.request("GET", "/cgi-bin/adder?1&2")
        self.assertEqual(status, 200)
        self.assertIn(b"3", body)

    def test_get_missing_file_is_404(self):
        status, _ = self.request("GET", "/nope.html")
        self.assertEqual(status, 404)

    def test_static_post_is_405(self):
        # Phase 2 M7-3: 정적 파일에 POST는 501이 아니라 405 (docs/prd/m6m7.md)
        status, _ = self.request("POST", "/home.html")
        self.assertEqual(status, 405)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
