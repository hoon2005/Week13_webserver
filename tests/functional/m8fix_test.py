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


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
