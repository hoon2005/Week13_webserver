"""Phase 2(M4 동시성, M5 HTTP 완성) 기능 테스트입니다. docs/prd/final.md 8장.

사용법: python3 phase2_test.py <build 폴더>

임시 폴더에 tests/fixtures/www/ 내용과 build/adder를 복사하고 build/tiny를 빈
포트로 띄웁니다. 타임아웃 테스트가 빨리 끝나도록 --timeout 1로 띄웁니다.
CGI 시간 제한(10초) 테스트가 있어 전체가 15초쯤 걸립니다.
"""

import concurrent.futures
import os
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FIXTURE_DIR = os.path.join(REPO_ROOT, "tests", "fixtures", "www")
BUILD_DIR = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build")

HOST = "127.0.0.1"
START_TIMEOUT_SEC = 10


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((HOST, 0))
        return sock.getsockname()[1]


class Server:
    """tiny 하나를 임시 폴더에서 띄우고 끄는 도우미"""

    def __init__(self, *args):
        tiny = os.path.join(BUILD_DIR, "tiny")
        adder = os.path.join(BUILD_DIR, "adder")
        for path in (tiny, adder):
            if not os.access(path, os.X_OK):
                raise RuntimeError("실행 파일이 없습니다: %s (먼저 make)" % path)
        self.work_dir = tempfile.mkdtemp(prefix="tiny-phase2-")
        self.www = os.path.join(self.work_dir, "www")
        shutil.copytree(FIXTURE_DIR, self.www)
        cgi_dir = os.path.join(self.www, "cgi-bin")
        os.makedirs(cgi_dir, exist_ok=True)
        shutil.copy2(adder, os.path.join(cgi_dir, "adder"))
        # git에서 실행 권한이 빠져도 테스트가 돌도록 다시 켬
        for name in os.listdir(cgi_dir):
            path = os.path.join(cgi_dir, name)
            os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)
        self.port = find_free_port()
        self.log_path = os.path.join(self.work_dir, "tiny.log")
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen(
            [tiny, str(self.port), "--root", self.www] + list(args),
            stdin=subprocess.DEVNULL,
            stdout=self.log,
            stderr=subprocess.STDOUT,
        )
        self._wait_until_listening()

    def _wait_until_listening(self):
        deadline = time.monotonic() + START_TIMEOUT_SEC
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                self.stop()
                raise RuntimeError("tiny가 시작하자마자 종료했습니다")
            try:
                with socket.create_connection((HOST, self.port), timeout=0.5):
                    return
            except OSError:
                time.sleep(0.05)
        self.stop()
        raise RuntimeError("tiny가 포트를 열지 않았습니다")

    def alive(self):
        return self.proc.poll() is None

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.log.close()
        shutil.rmtree(self.work_dir, ignore_errors=True)

    def connect(self, timeout=5):
        return socket.create_connection((HOST, self.port), timeout=timeout)


def read_response(f):
    """소켓 파일에서 응답 하나를 읽습니다. 연결이 닫혔으면 None"""
    status_line = f.readline()
    if not status_line:
        return None
    parts = status_line.decode().split(" ", 2)
    status = int(parts[1])
    headers = {}
    header_list = []
    while True:
        line = f.readline()
        if line in (b"\r\n", b"\n", b""):
            break
        name, _, value = line.decode().partition(":")
        headers[name.strip().lower()] = value.strip()
        header_list.append(name.strip().lower())
    body = b""
    if status != 100:
        length = int(headers.get("content-length", "0"))
        body = f.read(length)
    return status, headers, body, header_list


def exchange(server, data, timeout=5):
    """요청을 보내고 응답 하나를 받습니다"""
    with server.connect(timeout) as sock:
        sock.sendall(data)
        return read_response(sock.makefile("rb"))


def get(server, path, extra=b""):
    return exchange(
        server, b"GET " + path.encode() + b" HTTP/1.1\r\nHost: x\r\n" + extra + b"\r\n"
    )


def closed_by_server(sock, wait):
    """서버가 연결을 닫으면 True (recv가 b''를 돌려줌)"""
    sock.settimeout(wait)
    try:
        while True:
            data = sock.recv(4096)
            if not data:
                return True
    except (socket.timeout, TimeoutError):
        return False
    except ConnectionResetError:
        return True


class Phase2Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Server("--timeout", "1")

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def assert_alive(self):
        status, _, _, _ = get(self.server, "/home.html")
        self.assertEqual(status, 200)

    # ---------- 8.1 M4 동시성 ----------

    def test_idle_connection_does_not_block_others(self):
        with self.server.connect() as idle:
            start = time.monotonic()
            status, _, _, _ = get(self.server, "/home.html")
            self.assertEqual(status, 200)
            self.assertLess(time.monotonic() - start, 0.9)
            idle.close()

    def test_read_timeout_closes_idle_connection(self):
        with self.server.connect() as idle:
            start = time.monotonic()
            self.assertTrue(closed_by_server(idle, 4))
            self.assertLess(time.monotonic() - start, 3)

    def test_concurrent_static_and_cgi(self):
        def one(i):
            if i % 2:
                return get(self.server, "/home.html")[0], b""
            status, _, body, _ = get(self.server, "/cgi-bin/adder?1&2")
            return status, body

        with concurrent.futures.ThreadPoolExecutor(max_workers=50) as pool:
            results = list(pool.map(one, range(100)))
        self.assertTrue(all(status == 200 for status, _ in results))
        self.assertTrue(all(b"1 + 2 = 3" in body for status, body in results[::2]))
        time.sleep(0.2)
        out = subprocess.run(
            ["ps", "-A", "-o", "ppid=,stat="], capture_output=True, text=True
        ).stdout
        zombies = [
            line
            for line in out.splitlines()
            if line.split()[0] == str(self.server.proc.pid) and "Z" in line.split()[1]
        ]
        self.assertEqual(zombies, [])

    def test_cgi_does_not_inherit_other_client_socket(self):
        # 1) 다른 손님 연결을 먼저 열어 둠 (keep-alive로 작업자가 붙잡고 있음)
        # 2) slow CGI를 fork → 이때 열려 있는 fd를 CGI가 물려받을 수 있음
        # 3) 처음 연결은 아무것도 보내지 않아 읽기 타임아웃(1초)으로 서버가
        #    조용히 close함. 클라이언트가 EOF를 받아야 함
        #    (CGI가 소켓을 물고 있으면 서버가 close해도 FIN이 나가지 않음)
        with self.server.connect() as sock:
            f = sock.makefile("rb")
            sock.sendall(b"GET /home.html HTTP/1.1\r\nHost: x\r\n\r\n")
            self.assertEqual(read_response(f)[0], 200)
            slow = self.server.connect(timeout=15)
            try:
                slow.sendall(b"GET /cgi-bin/slow HTTP/1.1\r\nHost: x\r\n\r\n")
                time.sleep(0.3)
                self.assertTrue(closed_by_server(sock, 4))
            finally:
                slow.close()

    # ---------- 8.2 M5 헤더·본문 ----------

    def test_post_adder(self):
        body = b"a=1&b=2"
        status, headers, resp, names = exchange(
            self.server,
            b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n"
            b"Content-Type: application/x-www-form-urlencoded\r\n\r\n" + body,
        )
        self.assertEqual(status, 200)
        self.assertIn(b"1 + 2 = 3", resp)
        self.assertEqual(names.count("content-length"), 1)

    def test_get_adder(self):
        status, _, body, _ = get(self.server, "/cgi-bin/adder?1&2")
        self.assertEqual(status, 200)
        self.assertIn(b"1 + 2 = 3", body)

    def test_expect_100_continue(self):
        with self.server.connect() as sock:
            f = sock.makefile("rb")
            sock.sendall(
                b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n"
                b"Expect: 100-continue\r\n\r\n"
            )
            self.assertEqual(read_response(f)[0], 100)
            sock.sendall(b"a=4&b=5")
            status, _, body, _ = read_response(f)
            self.assertEqual(status, 200)
            self.assertIn(b"4 + 5 = 9", body)

    def test_too_large_body_is_413_before_body(self):
        with self.server.connect() as sock:
            sock.sendall(
                b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\n"
                b"Content-Length: 2000000\r\nExpect: 100-continue\r\n\r\n"
            )
            status, headers, _, _ = read_response(sock.makefile("rb"))
            self.assertEqual(status, 413)
            self.assertEqual(headers["connection"], "close")

    def test_request_errors(self):
        cases = [
            (b"GET / HTTP/1.1\r\n\r\n", 400),  # Host 없음
            (
                b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
                b"Transfer-Encoding: chunked\r\n\r\n",
                400,
            ),
            (b"POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", 501),
            (b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n", 400),
            (b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\nExpect: foo\r\n"
             b"Content-Length: 1\r\n\r\nx", 417),
            (b"GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505),
            (b"FOO / HTTP/1.1\r\nHost: x\r\n\r\n", 501),
            (b"GET /" + b"a" * 10000 + b" HTTP/1.1\r\nHost: x\r\n\r\n", 414),
            (b"GET / HTTP/1.1\r\nHost: x\r\nno colon\r\n\r\n", 400),
        ]
        for data, expected in cases:
            with self.subTest(expected=expected, data=data[:40]):
                status, headers, _, _ = exchange(self.server, data)
                self.assertEqual(status, expected)
                self.assertIn("content-length", headers)
                self.assertEqual(headers["connection"], "close")

    def test_error_response_survives_unread_request_data(self):
        # 서버가 다 읽지 않은 데이터(긴 요청 줄)를 남기고 오류 응답 뒤 닫아도
        # 클라이언트가 응답을 끝까지 받아야 함 (RST로 응답이 사라지면 안 됨)
        with self.server.connect() as sock:
            sock.sendall(b"GET /" + b"a" * 20000 + b" HTTP/1.1\r\nHost: x\r\n\r\n")
            time.sleep(0.3)
            status, headers, body, _ = read_response(sock.makefile("rb"))
            self.assertEqual(status, 414)
            self.assertEqual(len(body), int(headers["content-length"]))

    def test_static_post_is_405_and_keeps_connection(self):
        # M7-3에서 501 → 405 + Allow로 바뀜 (docs/prd/m6m7.md)
        status, headers, _, _ = exchange(
            self.server, b"POST /home.html HTTP/1.1\r\nHost: x\r\n\r\n"
        )
        self.assertEqual(status, 405)
        self.assertEqual(headers["allow"], "GET, HEAD")
        self.assertEqual(headers["connection"], "keep-alive")

    def test_body_cut_off_does_not_hurt_server(self):
        with self.server.connect() as sock:
            sock.sendall(
                b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\nab"
            )
        time.sleep(0.1)
        self.assert_alive()

    def test_head(self):
        status, headers, body, _ = exchange(
            self.server, b"HEAD /home.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
        )
        with open(os.path.join(FIXTURE_DIR, "home.html"), "rb") as f:
            size = len(f.read())
        self.assertEqual(status, 200)
        self.assertEqual(int(headers["content-length"]), size)
        self.assertEqual(body, b"")

    def test_head_on_cgi_is_405(self):
        # M7-3에서 501 → 405 + Allow로 바뀜 (docs/prd/m6m7.md)
        status, headers, body, _ = exchange(
            self.server, b"HEAD /cgi-bin/adder HTTP/1.1\r\nHost: x\r\n\r\n"
        )
        self.assertEqual(status, 405)
        self.assertEqual(headers["allow"], "GET, POST")
        self.assertEqual(body, b"")

    def test_mime(self):
        for path, expected in [
            ("/style.css", "text/css"),
            ("/app.js", "text/javascript"),
            ("/a.css.txt", "text/plain"),
            ("/home.html", "text/html"),
        ]:
            with self.subTest(path=path):
                status, headers, _, _ = get(self.server, path)
                self.assertEqual(status, 200)
                self.assertEqual(headers["content-type"], expected)

    # ---------- 8.3 M5 CGI ----------

    def test_cgi_failures_are_500(self):
        for name in ("fail", "nohdr", "big"):
            with self.subTest(cgi=name):
                status, _, _, _ = get(self.server, "/cgi-bin/" + name)
                self.assertEqual(status, 500)
                self.assert_alive()

    def test_cgi_timeout_is_504(self):
        start = time.monotonic()
        with self.server.connect(timeout=20) as sock:
            sock.sendall(b"GET /cgi-bin/slow HTTP/1.1\r\nHost: x\r\n\r\n")
            status, _, _, _ = read_response(sock.makefile("rb"))
        elapsed = time.monotonic() - start
        self.assertEqual(status, 504)
        self.assertGreater(elapsed, 9)
        self.assertLess(elapsed, 13)
        out = subprocess.run(
            ["ps", "-A", "-o", "ppid=,comm="], capture_output=True, text=True
        ).stdout
        children = [
            line for line in out.splitlines()
            if line.split()[0] == str(self.server.proc.pid)
        ]
        self.assertEqual(children, [])

    # ---------- 8.4 keep-alive ----------

    def test_two_requests_on_one_connection(self):
        with self.server.connect() as sock:
            f = sock.makefile("rb")
            for _ in range(2):
                sock.sendall(b"GET /home.html HTTP/1.1\r\nHost: x\r\n\r\n")
                status, headers, _, _ = read_response(f)
                self.assertEqual(status, 200)
                self.assertEqual(headers["connection"], "keep-alive")

    def test_connection_close_closes(self):
        with self.server.connect() as sock:
            sock.sendall(b"GET /home.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            status, headers, _, _ = read_response(sock.makefile("rb"))
            self.assertEqual(headers["connection"], "close")
            self.assertTrue(closed_by_server(sock, 2))

    def test_http10_closes(self):
        with self.server.connect() as sock:
            sock.sendall(b"GET /home.html HTTP/1.0\r\n\r\n")
            status, headers, _, _ = read_response(sock.makefile("rb"))
            self.assertEqual(status, 200)
            self.assertEqual(headers["connection"], "close")
            self.assertTrue(closed_by_server(sock, 2))

    def test_pipelining(self):
        with self.server.connect() as sock:
            sock.sendall(
                b"GET /home.html HTTP/1.1\r\nHost: x\r\n\r\n"
                b"GET /style.css HTTP/1.1\r\nHost: x\r\n\r\n"
            )
            f = sock.makefile("rb")
            first = read_response(f)
            second = read_response(f)
            self.assertEqual(first[1]["content-type"], "text/html")
            self.assertEqual(second[1]["content-type"], "text/css")

    def test_idle_after_response_is_closed(self):
        with self.server.connect() as sock:
            sock.sendall(b"GET /home.html HTTP/1.1\r\nHost: x\r\n\r\n")
            read_response(sock.makefile("rb"))
            self.assertTrue(closed_by_server(sock, 4))

    def test_bad_request_stops_pipeline(self):
        with self.server.connect() as sock:
            sock.sendall(
                b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n"
                b"GET /home.html HTTP/1.1\r\nHost: x\r\n\r\n"
            )
            f = sock.makefile("rb")
            self.assertEqual(read_response(f)[0], 400)
            self.assertIsNone(read_response(f))

    def test_cgi_keep_alive(self):
        with self.server.connect() as sock:
            f = sock.makefile("rb")
            for a, b in ((1, 2), (3, 4)):
                sock.sendall(
                    b"GET /cgi-bin/adder?%d&%d HTTP/1.1\r\nHost: x\r\n\r\n" % (a, b)
                )
                status, headers, body, _ = read_response(f)
                self.assertEqual(status, 200)
                self.assertEqual(headers["connection"], "keep-alive")
                self.assertIn(b"= %d" % (a + b), body)

    def test_head_errors_have_no_body(self):
        status, headers, body, _ = exchange(
            self.server, b"HEAD /nope.html HTTP/1.1\r\nHost: x\r\n\r\n"
        )
        self.assertEqual(status, 404)
        self.assertGreater(int(headers["content-length"]), 0)
        self.assertEqual(body, b"")


class SmallPoolTest(unittest.TestCase):
    """작업자와 큐가 작을 때 (8.1 작업자 포화, 큐 가득 참, 8.4 요청 수 상한)"""

    def test_workers_saturated_then_recover(self):
        server = Server("--threads", "2", "--timeout", "1")
        try:
            idle = [server.connect() for _ in range(2)]
            time.sleep(0.2)
            start = time.monotonic()
            status, _, _, _ = get(server, "/home.html")
            self.assertEqual(status, 200)
            self.assertLess(time.monotonic() - start, 4)
            for s in idle:
                s.close()
        finally:
            server.stop()

    def test_queue_full_waits_in_backlog(self):
        server = Server("--threads", "1", "--queue", "1", "--timeout", "1")
        try:
            idle = [server.connect() for _ in range(3)]
            time.sleep(0.2)
            status, _, _, _ = exchange(
                server, b"GET /home.html HTTP/1.1\r\nHost: x\r\n\r\n", timeout=10
            )
            self.assertEqual(status, 200)
            for s in idle:
                s.close()
        finally:
            server.stop()

    def test_keepalive_max(self):
        server = Server("--keepalive-max", "3")
        try:
            with server.connect() as sock:
                f = sock.makefile("rb")
                values = []
                for _ in range(3):
                    sock.sendall(b"GET /home.html HTTP/1.1\r\nHost: x\r\n\r\n")
                    values.append(read_response(f)[1]["connection"])
                self.assertEqual(values, ["keep-alive", "keep-alive", "close"])
                self.assertTrue(closed_by_server(sock, 2))
        finally:
            server.stop()


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
