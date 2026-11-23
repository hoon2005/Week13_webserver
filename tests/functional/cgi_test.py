"""CGI GET(M3)을 확인하는 기능 테스트입니다.

사용법: python3 cgi_test.py <build 폴더>

임시 폴더에 tests/fixtures/www/ 내용, build/adder, 테스트용 셸 스크립트 CGI를
넣고 서버를 빈 포트로 띄웁니다. 셸 스크립트는 /bin/sh로 실행되므로 Linux와
macOS에서 같게 동작합니다. 서버 실행과 대기에는 baseline_test.py와
restart_test.py의 도우미를 씁니다.
"""

import os
import shutil
import socket
import sys
import tempfile
import time
import unittest

import baseline_test as base
import restart_test
import static_test

# CGI 실행 중에 서버를 다시 켜는 테스트에서 CGI가 살아 있는 시간입니다.
SLOW_CGI_SEC = 2

ECHO_SCRIPT = """#!/bin/sh
printf 'Content-Type: text/plain\\r\\n\\r\\n'
printf '%s' "$QUERY_STRING"
"""

CRASH_SCRIPT = """#!/bin/sh
printf 'Content-Type: text/plain\\r\\n\\r\\n'
kill -SEGV $$
"""

SLOW_SCRIPT = """#!/bin/sh
printf 'Content-Type: text/plain\\r\\n\\r\\nslow\\n'
exec sleep %d
""" % SLOW_CGI_SEC


class CgiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        adder = os.path.join(base.BUILD_DIR, "adder")
        for path in (base.SERVER_PATH, adder):
            if not os.access(path, os.X_OK):
                raise RuntimeError("실행 파일이 없습니다: %s (먼저 make)" % path)

        cls.work_dir = tempfile.mkdtemp(prefix="server-cgi-")
        cls.www = os.path.join(cls.work_dir, "www")
        shutil.copytree(base.FIXTURE_DIR, cls.www)
        cgi_dir = os.path.join(cls.www, "cgi-bin")
        os.makedirs(os.path.join(cgi_dir, "sub"))
        shutil.copy2(adder, os.path.join(cgi_dir, "adder"))
        cls.write_script("echo", ECHO_SCRIPT, 0o755)
        cls.write_script("crash", CRASH_SCRIPT, 0o755)
        cls.write_script("slow", SLOW_SCRIPT, 0o755)
        cls.write_script("noexec", ECHO_SCRIPT, 0o644)

        cls.port = base.find_free_port()
        cls.proc, cls.log = cls.start(cls.port, "server.log")

    @classmethod
    def tearDownClass(cls):
        restart_test.stop_server(cls.proc, cls.log)
        shutil.rmtree(cls.work_dir, ignore_errors=True)

    @classmethod
    def write_script(cls, name, text, mode):
        path = os.path.join(cls.www, "cgi-bin", name)
        with open(path, "w") as f:
            f.write(text)
        os.chmod(path, mode)

    @classmethod
    def start(cls, port, log_name):
        log_path = os.path.join(cls.work_dir, log_name)
        proc, log = restart_test.start_server(port, cls.www, log_path)
        try:
            base.wait_until_listening(proc, port, log_path)
        except Exception:
            restart_test.stop_server(proc, log)
            raise
        return proc, log

    def get(self, path, port=None):
        """GET을 보내고 (상태 코드, 헤더 dict, 본문)을 돌려줍니다."""
        raw = restart_test.get_until_server_closes(port or self.port, path)
        return static_test.parse_response(raw)

    def assert_server_alive(self):
        status, _, _ = self.get("/home.html")
        self.assertEqual(status, 200, "서버가 이후 요청에 200을 주지 않습니다")

    # adder

    def test_adder는_두_정수의_합을_돌려준다(self):
        for query, expected in (
            ("1&2", b"1 + 2 = 3"),
            ("-5&3", b"-5 + 3 = -2"),
            ("2147483646&1", b"2147483646 + 1 = 2147483647"),
        ):
            with self.subTest(query=query):
                status, headers, body = self.get("/cgi-bin/adder?" + query)
                self.assertEqual(status, 200)
                self.assertEqual(headers.get("content-type"), "text/html")
                self.assertIn(expected, body)

    def test_adder에_잘못된_입력을_주면_200과_안내_본문을_주고_서버는_살아_있다(self):
        for query in (
            "",
            "?1",
            "?&2",
            "?1&",
            "?abc&2",
            "?1x&2",
            "?1&2&3",
            "?2147483647&1",
            "?-2147483648&-1",
            "?99999999999&1",
        ):
            with self.subTest(query=query):
                status, _, body = self.get("/cgi-bin/adder" + query)
                self.assertEqual(status, 200)
                self.assertIn(b"Invalid input", body)
        self.assert_server_alive()

    # CGI 실행

    def test_쿼리는_QUERY_STRING으로_그대로_넘어간다(self):
        status, _, body = self.get("/cgi-bin/echo?a=1&b=%20")
        self.assertEqual(status, 200)
        self.assertEqual(body, b"a=1&b=%20")

    def test_없는_CGI_프로그램은_404다(self):
        status, _, _ = self.get("/cgi-bin/nope")
        self.assertEqual(status, 404)

    def test_실행_권한이_없거나_폴더면_403이다(self):
        for path in ("/cgi-bin/noexec", "/cgi-bin/sub", "/cgi-bin/"):
            with self.subTest(path=path):
                status, _, _ = self.get(path)
                self.assertEqual(status, 403)

    def test_CGI_프로그램이_죽어도_서버는_살아_있다(self):
        raw = restart_test.get_until_server_closes(self.port, "/cgi-bin/crash")
        self.assertTrue(raw.startswith(b"HTTP/1.0 200"), raw[:40])
        self.assert_server_alive()

    def test_CGI가_실행_중일_때_서버를_다시_켜도_bind된다(self):
        # CGI가 listen 소켓을 물려받으면(FD_CLOEXEC가 없으면) CGI가 끝날 때까지
        # 그 포트에 다른 소켓을 bind할 수 없습니다.
        port = base.find_free_port()
        proc, log = self.start(port, "slow-first.log")
        try:
            with socket.create_connection(
                (base.HOST, port), timeout=base.REQUEST_TIMEOUT_SEC
            ) as sock:
                sock.sendall(b"GET /cgi-bin/slow HTTP/1.0\r\n\r\n")
                # 상태 줄(서버)과 스크립트 출력(CGI)은 따로 도착할 수 있습니다.
                received = b""
                while b"slow" not in received:
                    chunk = sock.recv(4096)
                    self.assertTrue(chunk, "CGI 출력 전에 연결이 닫혔습니다")
                    received += chunk
                restart_test.stop_server(proc, log)

                started = time.monotonic()
                proc, log = self.start(port, "slow-second.log")
                self.assertLess(time.monotonic() - started, SLOW_CGI_SEC)
                status, _, _ = self.get("/home.html", port=port)
                self.assertEqual(status, 200)
        finally:
            restart_test.stop_server(proc, log)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
