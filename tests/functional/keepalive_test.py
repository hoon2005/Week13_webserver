"""M5-4a keep-alive 기능 테스트입니다.

사용법: python3 keepalive_test.py <build 폴더>

서버를 --keepalive-max 3 --timeout 2로 띄웁니다. 요청 수 상한과 idle 타임아웃을
짧은 시간 안에 확인하기 위해서입니다 (기본값은 100건, 5초).
한 연결에서 응답을 여러 개 받아야 하므로, 닫힐 때까지 읽지 않고 Content-Length만큼만
읽습니다. 1단계(M5-4a)는 본문 없는 정적 GET의 200만 연결을 유지합니다.
엣지 케이스 ID는 docs/plan/phase2-edge-cases-http.md의 K 구분입니다.
"""

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

from baseline_test import (
    BUILD_DIR,
    FIXTURE_DIR,
    HOST,
    REQUEST_TIMEOUT_SEC,
    find_free_port,
    wait_until_listening,
)
from http_parse_test import Response

KEEPALIVE_MAX = 3
IDLE_TIMEOUT = 2

GET_HOME = b"GET /home.html HTTP/1.1\r\nHost: x\r\n\r\n"


class Conn:
    """한 연결에서 응답을 하나씩 읽는 도우미. 다음 응답의 앞부분은 buf에 남겨 둠."""

    def __init__(self, port):
        self.sock = socket.create_connection((HOST, port), timeout=REQUEST_TIMEOUT_SEC)
        self.buf = b""

    def send(self, data):
        self.sock.sendall(data)

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise EOFError("서버가 연결을 닫음")
        self.buf += chunk

    def read_response(self, head_request=False):
        while b"\r\n\r\n" not in self.buf:
            self._fill()
        head, _, rest = self.buf.partition(b"\r\n\r\n")
        resp = Response(head + b"\r\n\r\n")
        # HEAD 응답은 Content-Length가 있어도 본문이 없음
        length = 0 if head_request else int(resp.headers.get("content-length", "0"))
        while len(rest) < length:
            self._fill()
            head, _, rest = self.buf.partition(b"\r\n\r\n")
        resp.body = rest[:length]
        self.buf = rest[length:]
        return resp

    def closed_by_server(self):
        """서버가 닫았으면 True (남은 바이트 없이 EOF)."""
        try:
            return self.buf == b"" and self.sock.recv(1) == b""
        except ConnectionResetError:
            return True

    def close(self):
        self.sock.close()


class KeepAliveTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tiny = os.path.join(BUILD_DIR, "tiny")
        adder = os.path.join(BUILD_DIR, "adder")
        cls.work_dir = tempfile.mkdtemp(prefix="tiny-keepalive-")
        www = os.path.join(cls.work_dir, "www")
        shutil.copytree(FIXTURE_DIR, www)
        os.makedirs(os.path.join(www, "cgi-bin"), exist_ok=True)
        shutil.copy2(adder, os.path.join(www, "cgi-bin", "adder"))

        cls.port = find_free_port()
        cls.log_path = os.path.join(cls.work_dir, "tiny.log")
        cls.log = open(cls.log_path, "w")
        cls.proc = subprocess.Popen(
            [tiny, str(cls.port), "--root", "www",
             "--keepalive-max", str(KEEPALIVE_MAX), "--timeout", str(IDLE_TIMEOUT)],
            cwd=cls.work_dir,
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

    def setUp(self):
        self.conn = Conn(self.port)

    def tearDown(self):
        self.conn.close()

    def test_two_requests_one_connection(self):  # E-K1
        for _ in range(2):
            self.conn.send(GET_HOME)
            resp = self.conn.read_response()
            self.assertEqual(resp.status, 200)
            self.assertEqual(resp.headers.get("connection"), "keep-alive")
            with open(os.path.join(FIXTURE_DIR, "home.html"), "rb") as f:
                self.assertEqual(resp.body, f.read())

    def test_pipelining(self):  # E-K5
        self.conn.send(GET_HOME + b"GET /style.css HTTP/1.1\r\nHost: x\r\n\r\n")
        first = self.conn.read_response()
        second = self.conn.read_response()
        self.assertEqual(first.headers.get("content-type"), "text/html")
        self.assertEqual(second.headers.get("content-type"), "text/css")

    def test_connection_close(self):  # E-K2
        self.conn.send(b"GET /home.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        resp = self.conn.read_response()
        self.assertEqual(resp.headers.get("connection"), "close")
        self.assertTrue(self.conn.closed_by_server())

    def test_http10_closes(self):  # E-K3
        self.conn.send(b"GET /home.html HTTP/1.0\r\n\r\n")
        resp = self.conn.read_response()
        self.assertEqual(resp.headers.get("connection"), "close")
        self.assertTrue(self.conn.closed_by_server())

    def test_keepalive_max(self):  # E-K6
        for i in range(1, KEEPALIVE_MAX + 1):
            self.conn.send(GET_HOME)
            resp = self.conn.read_response()
            self.assertEqual(resp.status, 200)
            expected = "close" if i == KEEPALIVE_MAX else "keep-alive"
            self.assertEqual(resp.headers.get("connection"), expected, "요청 %d" % i)
        self.assertTrue(self.conn.closed_by_server())

    def test_pipelined_past_max_are_dropped(self):  # E-K12
        self.conn.send(GET_HOME * (KEEPALIVE_MAX + 1))
        for i in range(KEEPALIVE_MAX):
            self.assertEqual(self.conn.read_response().status, 200)
        self.assertTrue(self.conn.closed_by_server())  # 네 번째는 처리하지 않고 닫음

    def test_idle_timeout(self):  # E-K7
        self.conn.send(GET_HOME)
        self.conn.read_response()
        start = time.monotonic()
        self.assertTrue(self.conn.closed_by_server())
        elapsed = time.monotonic() - start
        self.assertGreater(elapsed, IDLE_TIMEOUT - 0.5)
        self.assertLess(elapsed, IDLE_TIMEOUT + 2)

    def test_other_responses_close_in_stage1(self):  # M5-4a 범위: 정적 GET 200만 유지
        for req in (
            b"GET /nope.html HTTP/1.1\r\nHost: x\r\n\r\n",          # 404
            b"HEAD /home.html HTTP/1.1\r\nHost: x\r\n\r\n",          # HEAD
            b"GET /cgi-bin/adder?1&2 HTTP/1.1\r\nHost: x\r\n\r\n",   # CGI
            b"GET /home.html HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nab",  # 본문 있음
        ):
            with self.subTest(req=req.split(b"\r\n")[0]):
                conn = Conn(self.port)
                try:
                    conn.send(req)
                    resp = conn.read_response(head_request=req.startswith(b"HEAD"))
                    self.assertEqual(resp.headers.get("connection"), "close")
                    self.assertTrue(conn.closed_by_server())
                finally:
                    conn.close()

    def test_client_closes_quietly(self):  # E-K8
        self.conn.send(GET_HOME)
        self.conn.read_response()
        self.conn.close()
        time.sleep(0.2)
        other = Conn(self.port)  # 서버는 계속 동작
        try:
            other.send(GET_HOME)
            self.assertEqual(other.read_response().status, 200)
        finally:
            other.close()


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
