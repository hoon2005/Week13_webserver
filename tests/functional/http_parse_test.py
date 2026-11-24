"""M5-1 요청 파서·응답 형식, M5-2 요청 본문, M5-3 CGI 응답 기능 테스트입니다.

사용법: python3 http_parse_test.py <build 폴더>

baseline_test.py처럼 임시 폴더에 tests/fixtures/www/와 build/adder를 복사합니다.
다른 점은 서버를 임시 폴더(www의 부모)에서 띄우고 --root www로 정적 루트를
넘긴다는 것입니다. 그래서 --root가 동작해야 모든 테스트가 통과합니다.

http.client는 잘못된 요청을 보내지 못하므로 소켓에 요청 바이트를 그대로 씁니다.
닫힐 때까지 읽어 응답 하나를 받으므로, M5-4a부터 연결을 유지할 수 있는 요청
(HTTP/1.1 정적 GET)에는 "Connection: close"를 붙여 서버가 응답 뒤 닫게 합니다.
keep-alive 자체는 keepalive_test.py에서 확인합니다. 엣지 케이스 ID는 docs/plan/phase2-edge-cases-http.md 기준입니다.
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


class Response:
    def __init__(self, raw):
        self.raw = raw
        head, sep, self.body = raw.partition(b"\r\n\r\n")
        lines = head.split(b"\r\n")
        self.status_line = lines[0].decode("latin-1") if lines[0] else ""
        parts = self.status_line.split(" ", 2)
        self.version = parts[0] if parts else ""
        self.status = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
        self.headers = {}
        for line in lines[1:]:
            name, _, value = line.decode("latin-1").partition(":")
            self.headers[name.strip().lower()] = value.strip()


class HttpParseTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tiny = os.path.join(BUILD_DIR, "tiny")
        adder = os.path.join(BUILD_DIR, "adder")
        for path in (tiny, adder):
            if not os.access(path, os.X_OK):
                raise RuntimeError("실행 파일이 없습니다: %s (먼저 make)" % path)

        cls.work_dir = tempfile.mkdtemp(prefix="tiny-http-parse-")
        www = os.path.join(cls.work_dir, "www")
        shutil.copytree(FIXTURE_DIR, www)
        os.makedirs(os.path.join(www, "cgi-bin"), exist_ok=True)
        shutil.copy2(adder, os.path.join(www, "cgi-bin", "adder"))

        cls.port = find_free_port()
        cls.log_path = os.path.join(cls.work_dir, "tiny.log")
        cls.log = open(cls.log_path, "w")
        cls.proc = subprocess.Popen(
            [tiny, str(cls.port), "--root", "www"],
            cwd=cls.work_dir,  # www 밖에서 띄움: --root가 없으면 파일을 못 찾음
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

    def send_raw(self, data, close_write=False, timeout=REQUEST_TIMEOUT_SEC):
        """data를 보내고 서버가 닫을 때까지 읽은 바이트를 Response로 돌려줌."""
        with socket.create_connection((HOST, self.port), timeout=timeout) as s:
            s.sendall(data)
            if close_write:
                s.shutdown(socket.SHUT_WR)
            chunks = []
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
        return Response(b"".join(chunks))

    def get(self, path, method="GET", extra=b""):
        return self.send_raw(
            method.encode() + b" " + path.encode()
            + b" HTTP/1.1\r\nHost: x\r\nConnection: close\r\n" + extra + b"\r\n"
        )

    def assert_framed(self, resp):
        """모든 응답: HTTP/1.1 상태 줄, Content-Length가 본문 길이와 같음, Connection 있음."""
        self.assertEqual(resp.version, "HTTP/1.1", resp.status_line)
        self.assertIn("content-length", resp.headers)
        self.assertEqual(int(resp.headers["content-length"]), len(resp.body))
        self.assertEqual(resp.headers.get("connection"), "close")

    # ---------- 정상 응답과 --root ----------

    def test_static_200_framed(self):
        resp = self.get("/home.html")
        self.assertEqual(resp.status, 200)
        self.assert_framed(resp)
        with open(os.path.join(FIXTURE_DIR, "home.html"), "rb") as f:
            self.assertEqual(resp.body, f.read())

    def test_http10_without_host(self):  # E-H9
        resp = self.send_raw(b"GET /home.html HTTP/1.0\r\n\r\n")
        self.assertEqual(resp.status, 200)

    def test_leading_blank_line_skipped(self):  # E-R9
        resp = self.send_raw(b"\r\nGET /home.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        self.assertEqual(resp.status, 200)

    def test_lf_only_line_endings(self):  # E-R10
        resp = self.send_raw(b"GET /home.html HTTP/1.1\nHost: x\nConnection: close\n\n")
        self.assertEqual(resp.status, 200)

    def test_cgi_get_still_works(self):
        resp = self.get("/cgi-bin/adder?1&2")
        self.assertEqual(resp.status, 200)
        self.assertEqual(resp.version, "HTTP/1.1")
        self.assertIn(b"3", resp.body)

    # ---------- 요청 줄 오류 ----------

    def test_long_uri_414(self):  # E-R1
        resp = self.send_raw(b"GET /" + b"a" * 10000 + b" HTTP/1.1\r\nHost: x\r\n\r\n")
        self.assertEqual(resp.status, 414)
        self.assert_framed(resp)

    def test_bad_request_line_400(self):  # E-R2, E-R3, E-R11, E-R12
        for line in (
            b"GET /\r\n",
            b"GET / HTTP/1.1 extra\r\n",
            b"GET  /  HTTP/1.1\r\n",
            b"GET http://x/home.html HTTP/1.1\r\n",
        ):
            with self.subTest(line=line):
                resp = self.send_raw(line + b"Host: x\r\n\r\n")
                self.assertEqual(resp.status, 400)
                self.assert_framed(resp)

    def test_bad_version_format_400(self):  # E-R4
        resp = self.send_raw(b"GET / HTTP/1\r\nHost: x\r\n\r\n")
        self.assertEqual(resp.status, 400)

    def test_unsupported_version_505(self):  # E-R5, E-R6
        for ver in (b"HTTP/2.0", b"HTTP/0.9"):
            with self.subTest(ver=ver):
                resp = self.send_raw(b"GET / " + ver + b"\r\nHost: x\r\n\r\n")
                self.assertEqual(resp.status, 505)
                self.assert_framed(resp)

    def test_unknown_method_501(self):  # E-R7, E-R8
        for method in ("FOO", "get", "DELETE"):
            with self.subTest(method=method):
                resp = self.get("/", method=method)
                self.assertEqual(resp.status, 501)
                self.assert_framed(resp)

    def test_incomplete_request_no_response(self):  # E-R13, E-H13
        for data in (b"GET / HT", b"GET / HTTP/1.1\r\nHost: x\r\n"):
            with self.subTest(data=data):
                resp = self.send_raw(data, close_write=True)
                self.assertEqual(resp.raw, b"")
        self.assertEqual(self.get("/home.html").status, 200)  # 서버는 살아 있음

    # ---------- 헤더 오류 ----------

    def test_missing_host_400(self):  # E-H7
        resp = self.send_raw(b"GET /home.html HTTP/1.1\r\n\r\n")
        self.assertEqual(resp.status, 400)

    def test_duplicate_host_400(self):  # E-H8
        resp = self.send_raw(b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n")
        self.assertEqual(resp.status, 400)

    def test_bad_header_line_400(self):  # E-H4, E-H10, E-H11
        for line in (b"BadHeader\r\n", b"Host : x\r\n", b"X-A: 1\r\n  continued\r\n"):
            with self.subTest(line=line):
                resp = self.send_raw(b"GET / HTTP/1.1\r\nHost: x\r\n" + line + b"\r\n")
                self.assertEqual(resp.status, 400)

    def test_header_line_too_long_400(self):  # E-H5
        resp = self.get("/", extra=b"X-Long: " + b"a" * 10000 + b"\r\n")
        self.assertEqual(resp.status, 400)

    def test_headers_total_too_big_400(self):  # E-H6
        extra = b"".join(b"X-Pad-%03d: %s\r\n" % (i, b"a" * 86) for i in range(100))
        resp = self.get("/", extra=extra)
        self.assertEqual(resp.status, 400)

    def test_content_length_errors_400(self):  # E-B3, E-B5, E-B8
        for extra in (
            b"Content-Length: abc\r\n",
            b"Content-Length: 7\r\nContent-Length: 8\r\n",
            b"Content-Length: 5\r\nTransfer-Encoding: chunked\r\n",
        ):
            with self.subTest(extra=extra):
                resp = self.get("/", method="POST", extra=extra)
                self.assertEqual(resp.status, 400)

    def test_transfer_encoding_only_501(self):  # E-B9
        resp = self.get("/", method="POST", extra=b"Transfer-Encoding: chunked\r\n")
        self.assertEqual(resp.status, 501)

    # ---------- 메서드와 HEAD ----------

    def test_head_static_same_headers_no_body(self):  # E-M7
        get = self.get("/home.html")
        head = self.get("/home.html", method="HEAD")
        self.assertEqual(head.status, 200)
        self.assertEqual(head.body, b"")
        self.assertEqual(head.headers["content-length"], get.headers["content-length"])
        self.assertEqual(head.headers["content-type"], get.headers["content-type"])

    def test_head_missing_404_no_body(self):  # E-M9
        resp = self.get("/nope.html", method="HEAD")
        self.assertEqual(resp.status, 404)
        self.assertEqual(resp.body, b"")
        self.assertGreater(int(resp.headers["content-length"]), 0)

    def test_head_cgi_501(self):  # E-M8
        resp = self.get("/cgi-bin/adder", method="HEAD")
        self.assertEqual(resp.status, 501)
        self.assertEqual(resp.body, b"")

    def test_post_static_501(self):  # E-U7
        resp = self.get("/home.html", method="POST", extra=b"Content-Length: 0\r\n")
        self.assertEqual(resp.status, 501)
        self.assert_framed(resp)

    def test_error_responses_framed(self):
        for path, status in (("/nope.html", 404), ("/../x", 403), ("/cgi-bin/", 403)):
            with self.subTest(path=path):
                resp = self.get(path)
                self.assertEqual(resp.status, status)
                self.assert_framed(resp)

    # ---------- 본문 (M5-2) ----------

    def post(self, path, body, extra=b"", version=b"HTTP/1.1"):
        return self.send_raw(
            b"POST " + path.encode() + b" " + version + b"\r\nHost: x\r\n"
            + b"Content-Length: %d\r\n" % len(body) + extra + b"\r\n" + body
        )

    def test_post_adder_form(self):  # E-A1
        resp = self.post("/cgi-bin/adder", b"a=1&b=2",
                         extra=b"Content-Type: application/x-www-form-urlencoded\r\n")
        self.assertEqual(resp.status, 200)
        self.assertIn(b"1 + 2 = 3", resp.body)

    def test_post_adder_plain(self):
        resp = self.post("/cgi-bin/adder", b"10&-4")
        self.assertIn(b"10 + -4 = 6", resp.body)

    def test_post_without_content_length(self):  # E-B1
        resp = self.send_raw(b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\n\r\n")
        self.assertEqual(resp.status, 200)
        self.assertIn(b"Give two numbers", resp.body)

    def test_adder_messages(self):  # E-A3 ~ E-A6
        for path, text in (
            ("/cgi-bin/adder", b"Give two numbers"),
            ("/cgi-bin/adder?a&b", b"whole numbers"),
            ("/cgi-bin/adder?99999999999999999999&1", b"between"),
            ("/cgi-bin/adder?1", b"exactly two"),
            ("/cgi-bin/adder?1&2&3", b"exactly two"),
        ):
            with self.subTest(path=path):
                resp = self.get(path)
                self.assertEqual(resp.status, 200)
                self.assertIn(text, resp.body)

    # ---------- CGI 응답 (M5-3) ----------

    def test_cgi_response_framed_by_server(self):  # E-G7, E-G8
        resp = self.get("/cgi-bin/adder?1&2")
        self.assertEqual(resp.status, 200)
        self.assert_framed(resp)  # 서버가 센 Content-Length가 본문 길이와 같음
        head = resp.raw.partition(b"\r\n\r\n")[0].lower()
        self.assertEqual(head.count(b"content-length:"), 1)
        self.assertEqual(resp.headers.get("content-type"), "text/html")

    def test_cgi_failures_500(self):  # E-G2, E-G4, E-G9
        for name in ("fail", "nohdr", "big"):
            with self.subTest(cgi=name):
                start = time.monotonic()
                resp = self.get("/cgi-bin/" + name)
                self.assertEqual(resp.status, 500)
                self.assert_framed(resp)
                self.assertLess(time.monotonic() - start, 5)

    def child_commands(self):
        out = subprocess.run(["ps", "-axo", "ppid=,command="], capture_output=True,
                             text=True, check=True).stdout
        return [line.split(None, 1)[1] for line in out.splitlines()
                if line.split() and line.split()[0] == str(self.proc.pid)]

    def test_cgi_timeout_504(self):  # E-G11, E-P3 (약 10초 걸림)
        start = time.monotonic()
        # 서버의 CGI 상한(10초)보다 길게 기다려야 504를 받음
        resp = self.send_raw(b"GET /cgi-bin/slow HTTP/1.1\r\nHost: x\r\n\r\n", timeout=20)
        elapsed = time.monotonic() - start
        self.assertEqual(resp.status, 504)
        self.assert_framed(resp)
        self.assertGreater(elapsed, 9)
        self.assertLess(elapsed, 13)
        self.assertEqual(self.child_commands(), [])  # slow(sleep)가 남지 않음

    def test_expect_100_continue(self):  # E-X1
        with socket.create_connection((HOST, self.port), timeout=REQUEST_TIMEOUT_SEC) as s:
            s.sendall(b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\n"
                      b"Content-Length: 7\r\nExpect: 100-continue\r\n\r\n")
            s.settimeout(0.5)  # 100이 오지 않으면 curl처럼 기다리게 됨 → 바로 와야 함
            first = s.recv(1024)
            self.assertEqual(first, b"HTTP/1.1 100 Continue\r\n\r\n")
            s.settimeout(REQUEST_TIMEOUT_SEC)
            s.sendall(b"a=1&b=2")
            rest = b""
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                rest += chunk
        resp = Response(rest)
        self.assertEqual(resp.status, 200)
        self.assertIn(b"1 + 2 = 3", resp.body)

    def test_expect_with_too_big_body_413(self):  # E-X2
        resp = self.send_raw(b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\n"
                             b"Content-Length: 2000000\r\nExpect: 100-continue\r\n\r\n")
        self.assertEqual(resp.status, 413)  # 100 없이 바로 413
        self.assertNotIn(b"100 Continue", resp.raw)
        self.assert_framed(resp)

    def test_too_big_body_413_before_body(self):  # E-B10
        start = time.monotonic()
        resp = self.send_raw(b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\n"
                             b"Content-Length: 2000000\r\n\r\n")
        self.assertEqual(resp.status, 413)
        self.assertLess(time.monotonic() - start, 3)  # 본문을 기다리지 않음

    def test_exactly_limit_is_accepted(self):  # E-B14
        body = b"a" * (1 << 20)
        with socket.create_connection((HOST, self.port), timeout=REQUEST_TIMEOUT_SEC) as s:
            s.sendall(b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\n"
                      b"Content-Length: %d\r\n\r\n" % len(body))
            s.sendall(body)
            raw = b""
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                raw += chunk
        resp = Response(raw)
        self.assertEqual(resp.status, 200)
        self.assertIn(b"too long", resp.body)  # adder가 읽고 길다고 안내

    def test_other_expect_417(self):  # E-X3
        resp = self.post("/cgi-bin/adder", b"x", extra=b"Expect: foo\r\n")
        self.assertEqual(resp.status, 417)

    def test_http10_ignores_expect(self):  # E-X4
        resp = self.post("/cgi-bin/adder", b"a=1&b=2", extra=b"Expect: foo\r\n",
                         version=b"HTTP/1.0")
        self.assertEqual(resp.status, 200)
        self.assertIn(b"1 + 2 = 3", resp.body)

    def test_short_body_no_response(self):  # E-B12
        resp = self.send_raw(b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\n"
                             b"Content-Length: 100\r\n\r\nab", close_write=True)
        self.assertEqual(resp.raw, b"")
        self.assertEqual(self.get("/home.html").status, 200)

    def test_get_with_body_is_read_and_ignored(self):  # E-B11
        resp = self.send_raw(b"GET /home.html HTTP/1.1\r\nHost: x\r\n"
                             b"Content-Length: 5\r\n\r\nhello")
        self.assertEqual(resp.status, 200)

    # ---------- MIME ----------

    def test_mime_types(self):  # E-M1, E-M2
        for path, mime in (
            ("/home.html", "text/html"),
            ("/style.css", "text/css"),
            ("/app.js", "text/javascript"),
            ("/a.css.txt", "text/plain"),
            ("/data.json", "text/plain"),
            ("/godzilla.gif", "image/gif"),
            ("/godzilla.jpg", "image/jpeg"),
        ):
            with self.subTest(path=path):
                resp = self.get(path)
                self.assertEqual(resp.status, 200)
                self.assertEqual(resp.headers.get("content-type"), mime)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
