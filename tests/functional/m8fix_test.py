"""M8 우선 수정 목록(F1~F11)의 회귀 테스트입니다.

사용법: python3 m8fix_test.py <build 폴더>

각 테스트는 수정 전 코드에서는 실패하고, 고친 뒤에는 통과합니다.
서버와 테스트용 백엔드 도우미는 m6m7_test.py의 것을 씁니다.
"""

import os
import socket
import statistics
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m6m7_test import HOST, Server, read_head, read_response  # noqa: E402


def keepalive_times(server, path, count):
    """한 연결로 path를 count번 요청해 응답마다 걸린 초를 돌려줌"""
    times = []
    with server.connect() as sock:
        f = sock.makefile("rb")
        for _ in range(count):
            start = time.monotonic()
            sock.sendall(("GET %s HTTP/1.1\r\nHost: x\r\n\r\n" % path).encode())
            status, _, _ = read_response(f)
            times.append(time.monotonic() - start)
            assert status == 200
    return times


class F1NagleTest(unittest.TestCase):
    """F1: 같은 연결의 두 번째 요청부터 작은 응답이 약 44ms 늦어짐"""

    @classmethod
    def setUpClass(cls):
        cls.server = Server()

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_keepalive_small_response_is_not_slower_than_new_connection(self):
        # 절대 시간 대신, 같은 연결의 두 번째 요청부터를 새 연결의 첫 요청과 비교함
        fresh = [keepalive_times(self.server, "/home.html", 1)[0] for _ in range(5)]
        reused = keepalive_times(self.server, "/home.html", 6)[1:]
        self.assertLess(statistics.median(reused),
                        statistics.median(fresh) + 0.02,
                        "새 연결 %s, 같은 연결 %s" % (fresh, reused))


class F2SendFailureTest(unittest.TestCase):
    """F2: 응답 전송에 실패해도 닫기 전에 최대 1초를 더 기다림"""

    def test_send_failure_closes_without_lingering(self):
        # 작업자 1명: 멈춘 연결을 닫아야 다음 손님을 받음. 쓰기 타임아웃 2초
        server = Server("--threads", "1", "--timeout", "1")
        try:
            with open(os.path.join(server.www, "big.bin"), "wb") as f:
                f.write(b"x" * (50 * 1024 * 1024))
            stalled = server.connect(timeout=30)
            stalled.sendall(b"GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n")
            result = {}

            def next_customer():
                result["reply"] = server.exchange(
                    b"GET /home.html HTTP/1.1\r\nHost: x\r\n\r\n", timeout=30)
                result["done"] = time.monotonic()

            import threading
            t = threading.Thread(target=next_customer)
            t.start()
            # 전송 실패 뒤에 남는 요청 로그를 기다림 (stalled는 읽지 않음)
            logged = None
            deadline = time.monotonic() + 25
            while time.monotonic() < deadline:
                if "/big.bin" in server.read_log():
                    logged = time.monotonic()
                    break
                time.sleep(0.01)
            t.join()
            stalled.close()
            self.assertIsNotNone(logged, "전송 실패 로그가 없음")
            self.assertEqual(result["reply"][0], 200)
            # 로그가 찍힌 뒤 연결을 바로 닫으면 다음 손님은 곧 응답을 받음
            self.assertLess(result["done"] - logged, 0.5)
        finally:
            server.stop()


class F3ErrorPageEscapeTest(unittest.TestCase):
    """F3: 요청에서 온 HTML 태그가 404·403 오류 페이지에 그대로 들어감"""

    @classmethod
    def setUpClass(cls):
        cls.server = Server()

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def check(self, path, status):
        got, _, body = self.server.exchange(
            b"GET " + path + b" HTTP/1.1\r\nHost: x\r\n\r\n")
        self.assertEqual(got, status)
        return body

    def test_404_escapes_html_special_characters(self):
        body = self.check(b'/<script>alert(1)</script>&"q.html', 404)
        self.assertNotIn(b"<script>", body)
        self.assertIn(b"&lt;script&gt;alert(1)&lt;/script&gt;&amp;&quot;q.html",
                      body)

    def test_403_escapes_html_special_characters(self):
        body = self.check(b"/a/../<b>x</b>", 403)
        self.assertNotIn(b"<b>", body)
        self.assertIn(b"&lt;b&gt;x&lt;/b&gt;", body)


def one_shot_backend(response):
    """연결 하나를 받아 요청 헤더를 읽고 response를 그대로 보낸 뒤 닫는 백엔드"""
    import threading
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind((HOST, 0))
    listener.listen(1)

    def serve():
        conn, _ = listener.accept()
        with conn:
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                data += chunk
            conn.sendall(response)
        listener.close()

    threading.Thread(target=serve, daemon=True).start()
    return listener.getsockname()[1]


class F4Head204Test(unittest.TestCase):
    """F4: 중계한 HEAD + 204 응답에 Content-Length가 붙음"""

    def test_head_204_relay_has_no_content_length(self):
        port = one_shot_backend(b"HTTP/1.0 204 No Content\r\nContent-Length: 5\r\n\r\n")
        server = Server("--route", "/api=127.0.0.1:%d" % port)
        try:
            status, headers, body = server.exchange(
                b"HEAD /api/x HTTP/1.1\r\nHost: x\r\n\r\n", head_request=True)
            self.assertEqual(status, 204)
            self.assertNotIn("content-length", headers)
            self.assertEqual(body, b"")
        finally:
            server.stop()


class F5HeadErrorBodyTest(unittest.TestCase):
    """F5: HEAD 요청의 버전·URI 검사에서 오류가 나면 HTML 본문이 붙음"""

    @classmethod
    def setUpClass(cls):
        cls.server = Server()

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def raw(self, data):
        with self.server.connect() as sock:
            sock.sendall(data)
            out = b""
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    return out
                out += chunk

    def test_head_request_line_errors_have_no_body(self):
        for line, status in ((b"HEAD / HTTP/2.0", b"505"),
                             (b"HEAD ./x HTTP/1.1", b"400"),
                             (b"HEAD / HTTP/1.1 extra", b"400")):
            with self.subTest(line=line):
                out = self.raw(line + b"\r\nHost: x\r\n\r\n")
                head, _, body = out.partition(b"\r\n\r\n")
                self.assertTrue(head.startswith(b"HTTP/1.1 " + status), head)
                self.assertEqual(body, b"")


class F6LongPathTest(unittest.TestCase):
    """F6: CGI가 받는 PATH가 1,018바이트에서 잘림"""

    def test_cgi_receives_full_path(self):
        long_path = "/usr/bin:/bin:/" + "a" * 2000
        env = dict(os.environ, PATH=long_path)
        server = Server(env=env)
        try:
            status, _, body = server.get("/cgi-bin/pathlen")
            self.assertEqual(status, 200)
            self.assertEqual(body.strip(), str(len(long_path)).encode())
        finally:
            server.stop()


class F7AdderRangeTest(unittest.TestCase):
    """F7: adder가 입력 하나라도 1,073,741,823을 넘으면 거절"""

    @classmethod
    def setUpClass(cls):
        cls.server = Server()

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_sum_within_int_is_computed(self):
        for query, answer in (("2000000000&1", b"2000000000 + 1 = 2000000001"),
                              ("-2000000000&-1", b"-2000000000 + -1 = -2000000001"),
                              ("2147483647&-2147483648", b"= -1")):
            with self.subTest(query=query):
                status, _, body = self.server.get("/cgi-bin/adder?" + query)
                self.assertEqual(status, 200)
                self.assertIn(answer, body)

    def test_sum_beyond_int_is_explained(self):
        for query in ("2147483647&1", "-2147483648&-1"):
            with self.subTest(query=query):
                status, _, body = self.server.get("/cgi-bin/adder?" + query)
                self.assertEqual(status, 200)
                self.assertIn(b"out of range", body)
                self.assertNotIn(b"answer", body)

    def test_input_beyond_int_is_invalid(self):
        status, _, body = self.server.get("/cgi-bin/adder?2147483648&0")
        self.assertIn(b"Invalid number", body)


class F8CgiOutputRulesTest(unittest.TestCase):
    """F8: CGI 출력이 HTTP 응답 규칙을 어겨도 그대로 나감 (권장안 A)"""

    @classmethod
    def setUpClass(cls):
        cls.server = Server()

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def two_on_one_connection(self, query):
        """한 연결로 CGI 요청과 home.html을 이어 보냄. 두 응답의 원문을 돌려줌"""
        with self.server.connect() as sock:
            sock.sendall(b"GET /cgi-bin/emit?" + query + b" HTTP/1.1\r\nHost: x\r\n\r\n"
                         b"GET /home.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            out = b""
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    return out
                out += chunk

    def test_204_and_304_end_at_headers(self):
        # 304는 CGI가 Content-Length: 4를 줘도 뺌. 서버가 세는 길이(0)는
        # 200일 때의 길이가 아니라 틀린 값이기 때문 (RFC 9110 8.6)
        for query, status in ((b"204", b"204"), (b"304", b"304")):
            with self.subTest(status=status):
                out = self.two_on_one_connection(query)
                head, _, rest = out.partition(b"\r\n\r\n")
                self.assertTrue(head.startswith(b"HTTP/1.1 " + status), head)
                self.assertNotIn(b"content-length", head.lower())
                # 남은 본문 없이 바로 다음 응답이 시작해야 함
                self.assertTrue(rest.startswith(b"HTTP/1.1 200"), rest[:40])

    def test_invalid_cgi_output_is_500(self):
        for query in (b"100", b"101", b"te", b"badname", b"emptyname", b"ctl"):
            with self.subTest(query=query):
                status, _, body = self.server.get("/cgi-bin/emit?" + query.decode())
                self.assertEqual(status, 500)
                self.assertNotIn(b"BODY", body)

    def test_hop_by_hop_headers_are_dropped(self):
        status, headers, body = self.server.get("/cgi-bin/emit?hop")
        self.assertEqual(status, 200)
        self.assertNotIn("keep-alive", headers)
        self.assertNotIn("upgrade", headers)
        self.assertEqual(headers["x-ok"], "1")
        self.assertEqual(body, b"BODY")


class F9QueryAndCgiPathTest(unittest.TestCase):
    """F9: 쿼리까지 파일 이름에 넣거나 CGI로 잘못 분류해 404·403으로 실패 (권장안 A)"""

    @classmethod
    def setUpClass(cls):
        cls.server = Server()
        www = cls.server.www
        os.makedirs(os.path.join(www, "docs"))
        with open(os.path.join(www, "docs", "cgi-bin-guide.html"), "w") as f:
            f.write("<p>notes</p>")
        os.makedirs(os.path.join(www, "a", "cgi-bin"))
        script = os.path.join(www, "a", "cgi-bin", "hello")
        with open(script, "w") as f:
            f.write("#!/bin/sh\nprintf 'Content-type: text/plain\\r\\n\\r\\nfrom cgi'\n")
        os.chmod(script, 0o755)

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_static_file_with_query_is_found(self):
        status, headers, _ = self.server.get("/style.css?v=1")
        self.assertEqual(status, 200)
        self.assertEqual(headers["content-type"], "text/css")
        status, _, _ = self.server.get("/?v=2")
        self.assertEqual(status, 200)  # 폴더 → home.html

    def test_query_text_does_not_make_cgi(self):
        status, headers, _ = self.server.get("/home.html?from=cgi-bin")
        self.assertEqual(status, 200)
        self.assertEqual(headers["content-type"], "text/html")

    def test_cgi_only_when_first_segment_is_cgi_bin(self):
        status, _, body = self.server.get("/docs/cgi-bin-guide.html")
        self.assertEqual(status, 200)
        self.assertEqual(body, b"<p>notes</p>")
        # 첫 조각이 아니면 정적 파일 (동작 변경: 전에는 CGI로 실행됨)
        status, _, body = self.server.get("/a/cgi-bin/hello")
        self.assertEqual(status, 200)
        self.assertTrue(body.startswith(b"#!/bin/sh"), body)
        # 첫 조각이 cgi-bin이면 지금처럼 CGI
        status, _, body = self.server.get("/cgi-bin/adder?1&2")
        self.assertIn(b"1 + 2 = 3", body)


class F10HostFormatTest(unittest.TestCase):
    """F10: 잘못된 Host 값도 통과하며 정적 파일 요청에서는 200이 나옴"""

    @classmethod
    def setUpClass(cls):
        cls.server = Server()

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_malformed_host_is_400(self):
        for host in (b"a/b", b"a b", b"[::1", b"a:80x"):
            with self.subTest(host=host):
                status, _, _ = self.server.exchange(
                    b"GET /home.html HTTP/1.1\r\nHost: " + host + b"\r\n\r\n")
                self.assertEqual(status, 400)

    def test_valid_hosts_still_work(self):
        for host in (b"localhost:8000", b"127.0.0.1", b"[::1]:80", b"example.com"):
            with self.subTest(host=host):
                status, _, _ = self.server.exchange(
                    b"GET /home.html HTTP/1.1\r\nHost: " + host + b"\r\n\r\n")
                self.assertEqual(status, 200)


class F11LongConnectionValueTest(unittest.TestCase):
    """F11: Connection 값의 앞 255바이트만 봐서 뒤쪽의 close를 놓침"""

    @classmethod
    def setUpClass(cls):
        cls.server = Server()

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_close_after_long_value_closes_connection(self):
        for n in (248, 249, 1000):
            with self.subTest(n=n):
                value = b"a" * n + b", close"
                status, headers, _ = self.server.exchange(
                    b"GET /home.html HTTP/1.1\r\nHost: x\r\nConnection: " + value
                    + b"\r\n\r\n")
                self.assertEqual(status, 200)
                self.assertEqual(headers["connection"], "close")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
