"""M4·M5 중간 점검에서 찾은 문제의 회귀 테스트입니다.

사용법: python3 issues_test.py <build 폴더>
서버를 띄우는 도우미는 phase2_test.py의 것을 씁니다.
"""

import os
import statistics
import sys
import time
import unittest

from phase2_test import Server, exchange, get, read_response


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


class PathTest(unittest.TestCase):
    """C2·C3: 쿼리를 떼고 경로 조각으로 판정한다"""

    @classmethod
    def setUpClass(cls):
        cls.server = Server("--timeout", "1")
        os.makedirs(os.path.join(cls.server.www, "sub", "cgi-bin"))
        with open(os.path.join(cls.server.www, "sub", "cgi-bin", "x"), "w") as f:
            f.write("not executed")

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_정적_파일에_쿼리가_붙어도_같은_파일을_준다(self):
        for path in ("/style.css?v=1", "/home.html?x=cgi-bin", "/?a=b/c"):
            with self.subTest(path=path):
                self.assertEqual(get(self.server, path)[0], 200)

    def test_쿼리_속의_점_두_개는_경로로_보지_않는다(self):
        self.assertEqual(get(self.server, "/home.html?p=/../x")[0], 200)

    def test_CGI_쿼리는_프로그램에_그대로_전달된다(self):
        status, _, body, _ = get(self.server, "/cgi-bin/adder?3&4")
        self.assertEqual(status, 200)
        self.assertIn(b"3 + 4 = 7", body)

    def test_점_조각과_빈_조각으로_CGI_파일을_내려받을_수_없다(self):
        for path in (
            "//cgi-bin/adder", "/./cgi-bin/adder", "/a/../cgi-bin/adder",
            "/cgi-bin/./adder", "/cgi-bin//adder", "/CGI-BIN/adder",
            "//style.css", "/./style.css", "/..", "/a/.",
        ):
            with self.subTest(path=path):
                self.assertEqual(get(self.server, path)[0], 403)

    def test_점으로_시작하는_파일_이름은_점_조각이_아니다(self):
        # 파일이 없어서 404. 403이면 이름을 조각 전체로 잘못 본 것
        for path in ("/.hidden", "/..a", "/a."):
            with self.subTest(path=path):
                self.assertEqual(get(self.server, path)[0], 404)

    def test_첫_조각이_아닌_cgi_bin은_정적_폴더다(self):
        status, _, body, _ = get(self.server, "/sub/cgi-bin/x")
        self.assertEqual(status, 200)
        self.assertEqual(body, b"not executed")

    def test_cgi_bin_폴더_자체는_403이다(self):
        for path in ("/cgi-bin", "/cgi-bin/"):
            with self.subTest(path=path):
                self.assertEqual(get(self.server, path)[0], 403)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
