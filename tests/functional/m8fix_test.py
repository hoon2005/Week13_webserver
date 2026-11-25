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


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
