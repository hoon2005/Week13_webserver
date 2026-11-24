"""M7 안정성 기능 테스트입니다.

사용법: python3 stability_test.py <build 폴더>
서버를 띄우고 끄는 도우미는 checked_server.py입니다 (종료 코드와 로그도 확인).
"""

import sys
import threading
import time
import unittest

from checked_server import CheckedServer as Server
from phase2_test import get, read_response

GET_HOME = b"GET /home.html HTTP/1.1\r\nHost: x\r\n\r\n"


class DeadlineTest(unittest.TestCase):
    """R3: 조금씩 계속 보내는 손님도 정해진 시간 안에 끝낸다
    (--timeout 1이면 무수신 1초, 헤더 전체 2초, 본문 전체 6초)"""

    @classmethod
    def setUpClass(cls):
        cls.server = Server("--timeout", "1")

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def seconds_until_closed(self, first, drip, gap):
        """first를 보낸 뒤 gap초마다 drip을 보낼 때, 서버가 닫을 때까지의 시간"""
        with self.server.connect() as sock:
            sock.sendall(first)
            start = time.monotonic()
            sock.settimeout(gap)
            while time.monotonic() - start < 12:
                try:
                    if sock.recv(4096) == b"":
                        break
                except (TimeoutError, OSError) as e:
                    if isinstance(e, ConnectionError):
                        break
                    if drip:
                        try:
                            sock.sendall(drip)
                        except ConnectionError:
                            break
            return time.monotonic() - start

    def test_한_줄을_한_글자씩_보내도_헤더_시간_안에_닫는다(self):
        self.assertTrue(1.5 < self.seconds_until_closed(b"G", b"E", 0.3) < 3.5)

    def test_헤더_줄을_계속_보내도_헤더_시간_안에_닫는다(self):
        seconds = self.seconds_until_closed(
            b"GET / HTTP/1.1\r\nHost: x\r\n", b"X-A: 1\r\n", 0.3)
        self.assertTrue(1.5 < seconds < 3.5, seconds)

    def test_본문을_한_글자씩_보내도_본문_시간_안에_닫는다(self):
        seconds = self.seconds_until_closed(
            b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\nContent-Length: 999\r\n\r\n",
            b"a", 0.4)
        self.assertTrue(5 < seconds < 8, seconds)

    def test_쓰다가_멈추면_전체_시간을_기다리지_않고_무수신_시간에_닫는다(self):
        for first in (
            b"GET / HT",
            b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\nContent-Length: 9\r\n\r\na",
        ):
            with self.subTest(first=first):
                seconds = self.seconds_until_closed(first, b"", 0.1)
                self.assertTrue(0.8 < seconds < 1.7, seconds)

    def test_요청마다_시간을_새로_잰다(self):
        # 한 연결에서 헤더 시간(2초)보다 오래 걸쳐 요청해도 닫히지 않음
        with self.server.connect() as sock:
            f = sock.makefile("rb")
            for _ in range(4):
                sock.sendall(GET_HOME)
                self.assertEqual(read_response(f)[0], 200)
                time.sleep(0.8)

    def test_마감_안에_나눠_보낸_요청은_정상으로_처리한다(self):
        with self.server.connect() as sock:
            for part in (b"GET /home", b".html HTTP/1.1\r\nHo", b"st: x\r\n\r\n"):
                sock.sendall(part)
                time.sleep(0.3)
            self.assertEqual(read_response(sock.makefile("rb"))[0], 200)


class KeepAliveYieldTest(unittest.TestCase):
    """D8: 기다리는 손님이 있으면 keep-alive 연결이 자리를 내준다"""

    def test_쉬지_않고_요청하는_연결이_작업자를_모두_차지해도_새_손님이_응답을_받는다(self):
        server = Server("--threads", "2", "--keepalive-max", "100000")
        stop = threading.Event()

        def busy():
            while not stop.is_set():
                try:
                    with server.connect() as sock:
                        f = sock.makefile("rb")
                        while not stop.is_set():
                            sock.sendall(GET_HOME)
                            headers = read_response(f)[1]
                            if headers["connection"] == "close":
                                break
                            time.sleep(0.1)
                except OSError:
                    pass

        threads = [threading.Thread(target=busy) for _ in range(2)]
        for t in threads:
            t.start()
        try:
            time.sleep(0.5)
            for _ in range(5):
                start = time.monotonic()
                self.assertEqual(get(server, "/home.html")[0], 200)
                self.assertLess(time.monotonic() - start, 2)
        finally:
            stop.set()
            for t in threads:
                t.join()
            server.stop()

    def test_작업자가_남아_있으면_keep_alive를_유지한다(self):
        server = Server("--threads", "4")
        try:
            with server.connect() as sock:
                f = sock.makefile("rb")
                for _ in range(20):
                    get(server, "/home.html", b"Connection: close\r\n")
                    sock.sendall(GET_HOME)
                    self.assertEqual(read_response(f)[1]["connection"], "keep-alive")
        finally:
            server.stop()


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
