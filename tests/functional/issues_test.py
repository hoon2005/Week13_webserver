"""M4·M5 중간 점검에서 찾은 문제의 회귀 테스트입니다.

사용법: python3 issues_test.py <build 폴더>
서버를 띄우는 도우미는 phase2_test.py의 것을 씁니다.
"""

import os
import statistics
import sys
import time
import unittest

from phase2_test import Server, exchange, read_response


class RequestCheckTest(unittest.TestCase):
    """C1 폴더 밖 접근, A1 NUL 글자, T3 Host 형식"""

    @classmethod
    def setUpClass(cls):
        cls.server = Server("--timeout", "1")
        # 루트(www) 바로 위 폴더의 파일. 응답에 나오면 안 됨
        with open(os.path.join(cls.server.work_dir, "secret.txt"), "w") as f:
            f.write("SECRET")

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_slash_로_시작하지_않는_URI로_루트_밖_파일을_읽을_수_없다(self):
        status, headers, body, _ = exchange(
            self.server, b"GET ./secret.txt HTTP/1.0\r\n\r\n"
        )
        self.assertEqual(status, 400)
        self.assertNotIn(b"SECRET", body)
        self.assertEqual(headers["connection"], "close")

    def test_Content_Length_값에_NUL이_있으면_400이다(self):
        status, _, _, _ = exchange(
            self.server,
            b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\n"
            b"Content-Length: 7\x00abc\r\n\r\na=1&b=2",
        )
        self.assertEqual(status, 400)

    def test_URI에_NUL이_있으면_400이다(self):
        status, _, _, _ = exchange(
            self.server, b"GET /home.html\x00zzz HTTP/1.1\r\nHost: x\r\n\r\n"
        )
        self.assertEqual(status, 400)

    def test_헤더_값_중간에_CR이_있으면_400이다(self):
        status, _, _, _ = exchange(
            self.server,
            b"GET /home.html HTTP/1.1\r\nHost: x\r\nX-A: 1\rX-B: 2\r\n\r\n",
        )
        self.assertEqual(status, 400)

    def test_형식이_틀린_Host는_400이다(self):
        status, _, _, _ = exchange(
            self.server, b"GET /home.html HTTP/1.1\r\nHost: a/b\r\n\r\n"
        )
        self.assertEqual(status, 400)


class KeepAliveDelayTest(unittest.TestCase):
    """B1: 헤더와 본문을 따로 써도 keep-alive 요청이 늦어지지 않는다"""

    ROUNDS = 15

    def request_ms(self, server, path, keep):
        """요청 하나가 끝날 때까지 걸린 시간(ms)의 중간값"""
        extra = b"" if keep else b"Connection: close\r\n"
        data = b"GET " + path + b" HTTP/1.1\r\nHost: x\r\n" + extra + b"\r\n"
        times = []
        sock = None
        for _ in range(self.ROUNDS):
            if sock is None:
                sock = server.connect()
                f = sock.makefile("rb")
            start = time.monotonic()
            sock.sendall(data)
            read_response(f)
            times.append((time.monotonic() - start) * 1000)
            if not keep:
                f.close()
                sock.close()
                sock = None
        if sock is not None:
            f.close()
            sock.close()
        return statistics.median(times)

    def test_정적_파일과_오류와_CGI_모두_매번_새로_연결할_때보다_늦지_않다(self):
        # 기계마다 속도가 달라 절대 시간 대신 "매번 새 연결"과 비교함.
        # 지연이 있으면 keep-alive 쪽만 요청마다 약 40ms 늦어짐
        server = Server()
        try:
            for path in (b"/home.html", b"/nope", b"/cgi-bin/adder?1&2"):
                with self.subTest(path=path):
                    close_ms = self.request_ms(server, path, keep=False)
                    keep_ms = self.request_ms(server, path, keep=True)
                    self.assertLess(keep_ms, close_ms + 25)
        finally:
            server.stop()


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
