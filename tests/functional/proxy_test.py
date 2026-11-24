"""M6 Reverse Proxy 기능 테스트입니다.

사용법: python3 proxy_test.py <build 폴더>

백엔드는 이 파일 안의 작은 소켓 서버입니다. 응답을 바이트 그대로 정할 수 있어서
늦은 응답, 잘린 응답, 잘못된 응답도 만들 수 있습니다. 서버를 띄우는 도우미는
phase2_test.py의 것을 씁니다.
"""

import socket
import struct
import sys
import threading
import time
import unittest

from phase2_test import HOST, Server, exchange, find_free_port, get
from phase2_test import read_response

TIMEOUT_SEC = 1  # 시험용 --proxy-timeout


def ok(body, extra=b""):
    return b"HTTP/1.0 200 OK\r\nContent-Length: %d\r\n%s\r\n%s" % (
        len(body), extra, body)


class Backend:
    """연결마다 요청을 끝까지 읽고 answer(conn, request)를 부르는 백엔드"""

    def __init__(self, answer):
        self.answer = answer
        self.requests = []
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((HOST, 0))
        self.sock.listen(64)
        self.name = "%s:%d" % self.sock.getsockname()
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self.serve, args=(conn,), daemon=True).start()

    def serve(self, conn):
        with conn:
            data = b""
            while b"\r\n\r\n" not in data:
                part = conn.recv(65536)
                if not part:
                    return
                data += part
            head, _, body = data.partition(b"\r\n\r\n")
            length = 0
            for line in head.split(b"\r\n")[1:]:
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":")[1])
            while len(body) < length:
                body += conn.recv(65536)
            self.requests.append((head, body))
            try:
                self.answer(conn, head)
            except OSError:
                pass  # 프록시가 먼저 끊음

    def close(self):
        # accept에서 기다리는 스레드까지 깨워 포트를 확실히 닫음
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()


def reply(data):
    """항상 같은 바이트를 보내는 answer"""
    return lambda conn, head: conn.sendall(data)


class ProxyCase(unittest.TestCase):
    """테스트마다 백엔드와 프록시를 띄우고 정리"""

    def backend(self, answer):
        b = Backend(answer)
        self.addCleanup(b.close)
        return b

    def proxy(self, *rules):
        args = ["--timeout", "1", "--proxy-timeout", str(TIMEOUT_SEC)]
        for rule in rules:
            args += ["--proxy", rule]
        server = Server(*args)
        self.addCleanup(server.stop)
        return server

    def one(self, answer):
        """/api/ 규칙 하나와 백엔드 하나"""
        b = self.backend(answer)
        return self.proxy("/api/=" + b.name), b

    def log(self, server):
        time.sleep(0.1)
        with open(server.log_path) as f:
            return f.read()


class RoutingTest(ProxyCase):
    def test_경로에_따라_백엔드와_직접_처리를_나눈다(self):
        api = self.backend(reply(ok(b"api")))
        static = self.backend(reply(ok(b"static")))
        server = self.proxy("/api/=" + api.name, "/static/=" + static.name)
        self.assertEqual(get(server, "/api/health")[2], b"api")
        self.assertEqual(get(server, "/static/site.css")[2], b"static")
        self.assertEqual(get(server, "/apix/health")[0], 404)
        self.assertEqual(get(server, "/home.html")[0], 200)
        self.assertIn(b"1 + 2 = 3", get(server, "/cgi-bin/adder?1&2")[2])
        self.assertIn("GET /api/health HTTP/1.1 200 -> " + api.name, self.log(server))

    def test_백엔드_두_개를_번갈아_고른다(self):
        a = self.backend(reply(ok(b"a")))
        b = self.backend(reply(ok(b"b")))
        server = self.proxy("/api/=%s,%s" % (a.name, b.name))
        bodies = [get(server, "/api/x")[2] for _ in range(6)]
        self.assertEqual(bodies, [b"a", b"b"] * 3)

    def test_쿼리에_있는_접두사는_규칙에_걸리지_않는다(self):
        server, b = self.one(reply(ok(b"api")))
        self.assertEqual(get(server, "/home.html?/api/x")[0], 200)
        self.assertEqual(b.requests, [])

    def test_점_두_개로_접두사를_벗어나는_경로는_403이고_백엔드로_가지_않는다(self):
        server, b = self.one(reply(ok(b"api")))
        self.assertEqual(get(server, "/api/../secret")[0], 403)
        self.assertEqual(b.requests, [])

    def test_잘못된_규칙이면_서버가_시작하지_않는다(self):
        for rules in (
            ["/api/"], ["/api=127.0.0.1:1"], ["/api/=127.0.0.1"],
            ["/api/=127.0.0.1:1", "/api/=127.0.0.1:2"],
        ):
            with self.subTest(rules=rules), self.assertRaises(RuntimeError):
                self.proxy(*rules)


class RequestTest(ProxyCase):
    def test_요청_줄과_헤더를_다시_써서_보낸다(self):
        server, b = self.one(reply(ok(b"")))
        get(server, "/api/users?id=1", b"X-End: 1\r\nConnection: X-Hop\r\nX-Hop: 1\r\n")
        lines = b.requests[0][0].split(b"\r\n")
        self.assertEqual(lines[0], b"GET /api/users?id=1 HTTP/1.0")
        self.assertIn(b"Host: x", lines)
        self.assertIn(b"X-End: 1", lines)
        self.assertIn(b"Via: 1.1 tiny", lines)
        self.assertIn(b"Connection: close", lines)
        self.assertNotIn(b"X-Hop: 1", lines)

    def test_POST_본문을_그대로_보낸다(self):
        server, b = self.one(reply(ok(b"done")))
        body = bytes(range(256)) * 2000  # 512000바이트, NUL 포함
        status, _, resp, _ = exchange(
            server,
            b"POST /api/up HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n"
            % len(body) + body,
        )
        self.assertEqual((status, resp), (200, b"done"))
        self.assertEqual(b.requests[0][1], body)

    def test_Expect는_프록시가_답하고_백엔드로_보내지_않는다(self):
        server, b = self.one(reply(ok(b"done")))
        with server.connect() as sock:
            f = sock.makefile("rb")
            sock.sendall(
                b"POST /api/ HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n"
                b"Expect: 100-continue\r\n\r\n"
            )
            self.assertEqual(read_response(f)[0], 100)
            sock.sendall(b"abc")
            self.assertEqual(read_response(f)[0], 200)
        self.assertNotIn(b"expect", b.requests[0][0].lower())

    def test_1MiB를_넘는_본문은_백엔드에_연결하지_않고_413이다(self):
        server, b = self.one(reply(ok(b"")))
        status, _, _, _ = exchange(
            server,
            b"POST /api/ HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n",
        )
        self.assertEqual(status, 413)
        self.assertEqual(b.requests, [])


class ResponseTest(ProxyCase):
    def test_상태와_헤더와_본문을_전달하고_구간_헤더는_바꾼다(self):
        server, _ = self.one(reply(
            b"HTTP/1.1 404 Nope\r\nContent-Type: text/x\r\nContent-Length: 2\r\n"
            b"Connection: close\r\nKeep-Alive: 9\r\nX-End: 1\r\n\r\nno"))
        status, headers, body, names = get(server, "/api/x")
        self.assertEqual((status, body), (404, b"no"))
        self.assertEqual(headers["content-type"], "text/x")
        self.assertEqual(headers["x-end"], "1")
        self.assertEqual(headers["connection"], "keep-alive")
        self.assertNotIn("keep-alive", names)
        self.assertEqual(names.count("connection"), 1)
        self.assertEqual(names.count("content-length"), 1)

    def test_1MiB보다_큰_응답도_전달한다(self):
        big = bytes(range(256)) * 20000  # 5120000바이트
        server, _ = self.one(reply(ok(big)))
        status, _, body, _ = get(server, "/api/big")
        self.assertEqual(status, 200)
        self.assertTrue(body == big)

    def test_한_연결에서_프록시와_정적_요청을_이어서_처리한다(self):
        server, _ = self.one(reply(ok(b"api")))
        with server.connect() as sock:
            f = sock.makefile("rb")
            for path, want in ((b"/api/1", b"api"), (b"/style.css", None),
                               (b"/api/2", b"api")):
                sock.sendall(b"GET %s HTTP/1.1\r\nHost: x\r\n\r\n" % path)
                status, headers, body, _ = read_response(f)
                self.assertEqual(status, 200)
                self.assertEqual(headers["connection"], "keep-alive")
                if want is not None:
                    self.assertEqual(body, want)

    def test_파이프라이닝한_프록시_요청의_응답_순서가_지켜진다(self):
        def echo(conn, head):
            conn.sendall(ok(head.split(b" ")[1]))
        server, _ = self.one(echo)
        with server.connect() as sock:
            sock.sendall(b"".join(
                b"GET /api/%d HTTP/1.1\r\nHost: x\r\n\r\n" % i for i in range(3)))
            f = sock.makefile("rb")
            for i in range(3):
                self.assertEqual(read_response(f)[2], b"/api/%d" % i)

    def test_길이가_없으면_끝까지_전달하고_연결을_닫는다(self):
        server, _ = self.one(reply(b"HTTP/1.0 200 OK\r\n\r\nno length"))
        with server.connect() as sock:
            sock.sendall(b"GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n")
            data = b""
            while True:
                part = sock.recv(4096)  # 정상 종료라 ConnectionReset이 나면 안 됨
                if not part:
                    break
                data += part
        head, _, body = data.partition(b"\r\n\r\n")
        self.assertEqual(body, b"no length")
        self.assertIn(b"Connection: close", head)
        self.assertNotIn(b"Content-Length", head)

    def test_HEAD와_204와_304는_헤더에서_끝나고_연결을_유지한다(self):
        cases = (
            (b"HEAD", b"HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\n", "10"),
            (b"HEAD", b"HTTP/1.0 200 OK\r\n\r\n", None),
            (b"GET", b"HTTP/1.0 204 No Content\r\nContent-Length: 3\r\n\r\nabc", None),
            (b"GET", b"HTTP/1.0 304 Not Modified\r\nContent-Length: 10\r\n\r\n", "10"),
            (b"GET", b"HTTP/1.0 304 Not Modified\r\n\r\n", None),
        )
        for method, data, length in cases:
            with self.subTest(method=method, data=data):
                server, _ = self.one(reply(data))
                with server.connect() as sock:
                    sock.sendall(method + b" /api/x HTTP/1.1\r\nHost: x\r\n\r\n")
                    f = sock.makefile("rb")
                    head = b""
                    while not head.endswith(b"\r\n\r\n"):
                        line = f.readline()
                        self.assertNotEqual(line, b"")
                        head += line
                    text = head.decode().lower()
                    if length is None:
                        self.assertNotIn("content-length", text)
                    else:
                        self.assertIn("content-length: " + length, text)
                    self.assertIn("connection: keep-alive", text)
                    # 본문이 새어 나오면 다음 응답의 상태 줄이 깨짐
                    sock.sendall(b"GET /home.html HTTP/1.1\r\nHost: x\r\n\r\n")
                    self.assertEqual(read_response(f)[0], 200)

    def test_100과_103은_버리고_최종_응답을_전달한다(self):
        server, _ = self.one(reply(
            b"HTTP/1.1 100 Continue\r\n\r\n"
            b"HTTP/1.1 103 Early Hints\r\nLink: x\r\n\r\n" + ok(b"final")))
        status, headers, body, _ = get(server, "/api/x")
        self.assertEqual((status, body), (200, b"final"))
        self.assertNotIn("link", headers)

    def test_알린_길이보다_더_온_바이트는_다음_응답에_섞이지_않는다(self):
        server, _ = self.one(reply(
            b"HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nokHTTP/1.1 500 X\r\n\r\n"))
        with server.connect() as sock:
            f = sock.makefile("rb")
            for _ in range(2):
                sock.sendall(b"GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n")
                self.assertEqual(read_response(f)[2], b"ok")


class FailureTest(ProxyCase):
    def test_백엔드가_모두_꺼져_있으면_502이고_연결을_유지한다(self):
        server = self.proxy("/api/=%s:%d" % (HOST, find_free_port()))
        with server.connect() as sock:
            f = sock.makefile("rb")
            sock.sendall(b"GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n")
            status, headers, _, _ = read_response(f)
            self.assertEqual(status, 502)
            self.assertEqual(headers["connection"], "keep-alive")
            sock.sendall(b"GET /home.html HTTP/1.1\r\nHost: x\r\n\r\n")
            self.assertEqual(read_response(f)[0], 200)

    def test_하나가_꺼져_있으면_다음_백엔드가_답한다(self):
        alive = self.backend(reply(ok(b"alive")))
        dead = "%s:%d" % (HOST, find_free_port())
        server = self.proxy("/api/=%s,%s" % (dead, alive.name))
        for _ in range(4):
            self.assertEqual(get(server, "/api/x")[2], b"alive")

    def test_요청을_보낸_뒤의_실패는_다른_백엔드로_다시_보내지_않는다(self):
        def hang_up(conn, head):
            pass  # 요청을 읽고 답 없이 닫음
        bad = self.backend(hang_up)
        good = self.backend(reply(ok(b"good")))
        server = self.proxy("/api/=%s,%s" % (bad.name, good.name))
        status, _, _, _ = exchange(
            server, b"POST /api/x HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx")
        self.assertEqual(status, 502)
        self.assertEqual(len(bad.requests), 1)
        self.assertEqual(good.requests, [])

    def test_답하지_않는_백엔드는_504다(self):
        server, _ = self.one(lambda conn, head: time.sleep(5))
        start = time.monotonic()
        status, headers, _, _ = get(server, "/api/x")
        elapsed = time.monotonic() - start
        self.assertEqual(status, 504)
        self.assertEqual(headers["connection"], "keep-alive")
        self.assertGreater(elapsed, TIMEOUT_SEC * 0.9)
        self.assertLess(elapsed, TIMEOUT_SEC + 1)

    def test_헤더를_한_글자씩_보내는_백엔드도_전체_시간으로_504다(self):
        def drip(conn, head):
            for c in b"HTTP/1.0 200 OK\r\nX: " + b"a" * 100:
                conn.sendall(bytes([c]))
                time.sleep(0.2)
        server, _ = self.one(drip)
        start = time.monotonic()
        self.assertEqual(get(server, "/api/x")[0], 504)
        self.assertLess(time.monotonic() - start, TIMEOUT_SEC + 1)

    def test_계속_보내는_긴_본문은_시간_제한보다_오래_걸려도_전달한다(self):
        def slow_body(conn, head):
            conn.sendall(b"HTTP/1.0 200 OK\r\nContent-Length: 8\r\n\r\n")
            for _ in range(8):
                time.sleep(TIMEOUT_SEC * 0.4)
                conn.sendall(b"x")
        server, _ = self.one(slow_body)
        status, _, body, _ = exchange(
            server, b"GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n", timeout=10)
        self.assertEqual((status, body), (200, b"x" * 8))

    def test_믿을_수_없는_응답은_502다(self):
        for data in (
            b"", b"hello\r\n\r\n", b"HTTP/1.0 200 OK\r\nX: 1\r\n",
            b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
            b"HTTP/1.0 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab",
            b"HTTP/1.1 101 Switching Protocols\r\n\r\n",
            b"HTTP/1.0 200 OK\r\nX: " + b"a" * 9000 + b"\r\n\r\n",
        ):
            with self.subTest(data=data[:50]):
                server, _ = self.one(reply(data))
                self.assertEqual(get(server, "/api/x")[0], 502)

    def test_본문_도중_끊긴_응답은_잘린_채_닫히고_서버는_살아_있다(self):
        server, _ = self.one(reply(b"HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\nabc"))
        with server.connect() as sock:
            sock.sendall(b"GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n")
            f = sock.makefile("rb")
            status, _, body, _ = read_response(f)
            self.assertEqual((status, body), (200, b"abc"))
            self.assertEqual(f.read(), b"")
        self.assertEqual(get(server, "/home.html")[0], 200)
        self.assertIn("(중단)", self.log(server))

    def test_길이_없는_응답이_백엔드_오류로_끊기면_RST로_알린다(self):
        def reset(conn, head):
            conn.sendall(b"HTTP/1.0 200 OK\r\n\r\nabc")
            time.sleep(0.3)
            conn.setsockopt(
                socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        for answer in (reset, self.stall):
            with self.subTest(answer=answer.__name__):
                server, _ = self.one(answer)
                with server.connect() as sock:
                    sock.sendall(b"GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n")
                    with self.assertRaises(ConnectionResetError):
                        while sock.recv(4096):
                            pass

    @staticmethod
    def stall(conn, head):
        conn.sendall(b"HTTP/1.0 200 OK\r\n\r\nabc")
        time.sleep(5)

    def test_손님이_받다가_떠나도_서버는_살아_있다(self):
        def endless(conn, head):
            conn.sendall(b"HTTP/1.0 200 OK\r\n\r\n")
            while True:
                conn.sendall(b"x" * 65536)
        server, _ = self.one(endless)
        with server.connect() as sock:
            sock.sendall(b"GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n")
            sock.recv(1000)
        time.sleep(0.5)
        self.assertEqual(get(server, "/home.html")[0], 200)
        self.assertIn("(중단)", self.log(server))

    def test_백엔드를_기다리는_동안_다른_손님은_바로_응답을_받는다(self):
        server, _ = self.one(lambda conn, head: time.sleep(5))
        with server.connect() as sock:
            sock.sendall(b"GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n")
            time.sleep(0.1)
            start = time.monotonic()
            self.assertEqual(get(server, "/home.html")[0], 200)
            self.assertLess(time.monotonic() - start, 0.5)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
