"""M4·M5 중간 점검에서 찾은 문제의 회귀 테스트입니다.

사용법: python3 issues_test.py <build 폴더>
서버를 띄우는 도우미는 phase2_test.py의 것을 씁니다.
"""

import os
import sys
import unittest

from phase2_test import Server, exchange


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


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
