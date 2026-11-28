"""Phase 2 M6 Reverse Proxy, M7 안정성 기능 테스트입니다. docs/prd/m6m7.md 7장.

사용법: python3 m6m7_test.py <build 폴더>

백엔드는 이 파일의 Backend(파이썬 소켓 서버)로 띄웁니다. 백엔드는 받은 메서드,
경로, 헤더, 본문 길이와 자기 포트를 돌려주고, 경로에 따라 늦은 응답, 길이 없는
응답, 중간에 끊긴 응답을 만듭니다. tiny는 임시 폴더에서 빈 포트로 띄웁니다.
"""

import os
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FIXTURE_DIR = os.path.join(REPO_ROOT, "tests", "fixtures", "www")
BUILD_DIR = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build")

HOST = "127.0.0.1"
START_TIMEOUT_SEC = 10
IS_LINUX = sys.platform.startswith("linux")
BIG_FILE_SIZE = 32 * 1024 * 1024


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((HOST, 0))
        return sock.getsockname()[1]


def read_head(f):
    """상태 줄과 헤더를 읽음. 연결이 닫혔으면 None"""
    status_line = f.readline()
    if not status_line:
        return None
    status = int(status_line.decode().split(" ", 2)[1])
    headers = {}
    while True:
        line = f.readline()
        if line in (b"\r\n", b"\n", b""):
            break
        name, _, value = line.decode().partition(":")
        headers.setdefault(name.strip().lower(), value.strip())
    return status, headers


def read_response(f, head_request=False):
    """응답 하나: (상태, 헤더, 본문). 길이가 없으면 닫힐 때까지 읽음"""
    got = read_head(f)
    if got is None:
        return None
    status, headers = got
    if head_request or status in (100, 204, 304):
        return status, headers, b""
    if "content-length" in headers:
        return status, headers, f.read(int(headers["content-length"]))
    return status, headers, f.read()


class Server:
    """tiny 하나를 임시 폴더에서 띄우고 끄는 도우미"""

    def __init__(self, *args, new_session=False, port=None, wait=True, env=None):
        tiny = os.path.join(BUILD_DIR, "tiny")
        adder = os.path.join(BUILD_DIR, "adder")
        for path in (tiny, adder):
            if not os.access(path, os.X_OK):
                raise RuntimeError("실행 파일이 없습니다: %s (먼저 make)" % path)
        self.work_dir = tempfile.mkdtemp(prefix="tiny-m6m7-")
        self.www = os.path.join(self.work_dir, "www")
        shutil.copytree(FIXTURE_DIR, self.www)
        cgi_dir = os.path.join(self.www, "cgi-bin")
        shutil.copy2(adder, os.path.join(cgi_dir, "adder"))
        for name in os.listdir(cgi_dir):  # git에서 실행 권한이 빠져도 돌도록
            path = os.path.join(cgi_dir, name)
            os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)
        self.port = port or find_free_port()
        self.log_path = os.path.join(self.work_dir, "tiny.log")
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen(
            [tiny, str(self.port), "--root", self.www] + list(args),
            stdin=subprocess.DEVNULL,
            stdout=self.log,
            stderr=subprocess.STDOUT,
            start_new_session=new_session,
            env=env,
        )
        self.stopped = False
        if wait:
            self._wait_until_listening()

    def _wait_until_listening(self):
        deadline = time.monotonic() + START_TIMEOUT_SEC
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                log = self.read_log()
                self.cleanup()
                raise RuntimeError("tiny가 시작하자마자 종료했습니다\n" + log)
            try:
                with socket.create_connection((HOST, self.port), timeout=0.5):
                    return
            except OSError:
                time.sleep(0.05)
        self.cleanup()
        raise RuntimeError("tiny가 포트를 열지 않았습니다")

    def read_log(self):
        self.log.flush()
        with open(self.log_path, "r", errors="replace") as f:
            return f.read()

    def connect(self, timeout=5):
        return socket.create_connection((HOST, self.port), timeout=timeout)

    def exchange(self, data, timeout=5, head_request=False):
        with self.connect(timeout) as sock:
            sock.sendall(data)
            return read_response(sock.makefile("rb"), head_request)

    def get(self, path, extra=b""):
        return self.exchange(
            b"GET " + path.encode() + b" HTTP/1.1\r\nHost: x\r\n" + extra + b"\r\n"
        )

    def wait_exit(self, timeout):
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def cleanup(self):
        if self.stopped:
            return
        self.stopped = True
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        self.log.close()
        shutil.rmtree(self.work_dir, ignore_errors=True)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            self.wait_exit(8)
        self.cleanup()


class Backend:
    """테스트용 백엔드. 경로로 동작을 고릅니다.

    /slow?초     그만큼 기다린 뒤 응답        /silent   응답하지 않음
    /nolen       Content-Length 없이 응답      /cutlen   길이보다 덜 보내고 닫음
    /cutnolen    길이 없이 일부만 보내고 RST    /204      204 No Content
    그 외        200, 본문에 받은 요청 정보
    """

    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((HOST, 0))
        self.sock.listen(64)
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.requests = []
        self.lock = threading.Lock()
        self.running = True
        self.thread = threading.Thread(target=self._accept_loop, daemon=True)
        self.thread.start()

    def _accept_loop(self):
        while self.running:
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def _serve(self, conn):
        try:
            conn.settimeout(30)
            f = conn.makefile("rb")
            request_line = f.readline().decode()
            headers = []
            while True:
                line = f.readline()
                if line in (b"\r\n", b"\n", b""):
                    break
                name, _, value = line.decode().partition(":")
                headers.append((name.strip(), value.strip()))
            lower = {n.lower(): v for n, v in headers}
            body = f.read(int(lower.get("content-length", "0")))
            method, path, version = request_line.split()
            with self.lock:
                self.requests.append(
                    {"method": method, "path": path, "version": version,
                     "headers": headers, "body": body}
                )
            self._respond(conn, method, path, len(body), lower)
        except OSError:
            pass
        finally:
            conn.close()

    def _respond(self, conn, method, path, body_len, headers):
        if path.startswith("/api/silent"):
            time.sleep(20)
            return
        if path.startswith("/api/slow"):
            time.sleep(float(path.split("?")[1]))
        if path.startswith("/api/nolen"):
            conn.sendall(b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n")
            conn.sendall(b"no length body " * 100)
            return
        if path.startswith("/api/cutlen"):
            conn.sendall(b"HTTP/1.0 200 OK\r\nContent-Length: 100\r\n\r\n0123456789")
            return
        if path.startswith("/api/cutnolen"):
            conn.sendall(b"HTTP/1.0 200 OK\r\n\r\npartial")
            time.sleep(0.2)
            # SO_LINGER 0으로 닫아 RST: 백엔드 오류로 중간에 끊긴 상황
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                            struct.pack("ii", 1, 0))
            return
        if path.startswith("/api/204"):
            conn.sendall(b"HTTP/1.0 204 No Content\r\n\r\n")
            return
        body = ("port=%d method=%s path=%s len=%d type=%s\n" % (
            self.port, method, path, body_len, headers.get("content-type", "-")
        )).encode()
        head = ("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                "Content-Length: %d\r\nConnection: close\r\nX-Backend: %d\r\n\r\n"
                % (len(body), self.port)).encode()
        conn.sendall(head if method == "HEAD" else head + body)

    def last(self):
        with self.lock:
            return self.requests[-1]

    def count(self):
        with self.lock:
            return len(self.requests)

    def close(self):
        self.running = False
        self.thread.join()
        self.sock.close()


def closed_by_server(sock, wait):
    sock.settimeout(wait)
    try:
        while True:
            if not sock.recv(65536):
                return True
    except (socket.timeout, TimeoutError):
        return False
    except ConnectionResetError:
        return True


def unused_port():
    """아무도 듣지 않는 포트 (연결 거부)"""
    return find_free_port()


# ============================== M6 ==============================


class ProxyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.b1 = Backend()
        cls.b2 = Backend()
        cls.server = Server(
            "--route", "/api=127.0.0.1:%d,127.0.0.1:%d" % (cls.b1.port, cls.b2.port),
            "--proxy-timeout", "2",
        )

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()
        cls.b1.close()
        cls.b2.close()

    def test_round_robin(self):
        ports = []
        for _ in range(4):
            status, headers, _ = self.server.get("/api/users")
            self.assertEqual(status, 200)
            ports.append(int(headers["x-backend"]))
        self.assertEqual(sorted(set(ports)), sorted([self.b1.port, self.b2.port]))
        self.assertNotEqual(ports[0], ports[1])
        self.assertEqual(ports[0], ports[2])
        self.assertEqual(ports[1], ports[3])
        log = self.server.read_log()
        self.assertIn("-> 127.0.0.1:%d" % self.b1.port, log)
        self.assertIn("-> 127.0.0.1:%d" % self.b2.port, log)

    def test_prefix_boundary(self):
        status, headers, body = self.server.get("/api")
        self.assertEqual(status, 200)
        self.assertIn(b"path=/api ", body)
        status, headers, _ = self.server.get("/apiary")
        self.assertEqual(status, 404)  # 서버가 직접 처리
        self.assertNotIn("x-backend", headers)

    def test_unmatched_served_locally(self):
        status, headers, _ = self.server.get("/home.html")
        self.assertEqual(status, 200)
        self.assertEqual(headers["content-type"], "text/html")
        status, _, body = self.server.get("/cgi-bin/adder?1&2")
        self.assertEqual(status, 200)
        self.assertIn(b"1 + 2 = 3", body)

    def test_post_body_and_type(self):
        status, _, body = self.server.exchange(
            b"POST /api/users HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n"
            b"Content-Type: application/json\r\n\r\n{\"a\":1}"
        )
        self.assertEqual(status, 200)
        self.assertIn(b"method=POST", body)
        self.assertIn(b"len=7", body)
        self.assertIn(b"type=application/json", body)

    def test_head(self):
        start = time.monotonic()
        status, headers, body = self.server.exchange(
            b"HEAD /api/users HTTP/1.1\r\nHost: x\r\n\r\n", head_request=True
        )
        self.assertEqual(status, 200)
        self.assertGreater(int(headers["content-length"]), 0)
        self.assertLess(time.monotonic() - start, 1.5)  # 본문을 기다리지 않음

    def test_expect_not_forwarded(self):
        with self.server.connect() as sock:
            f = sock.makefile("rb")
            sock.sendall(
                b"POST /api/up HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n"
                b"Expect: 100-continue\r\n\r\n"
            )
            self.assertEqual(read_head(f)[0], 100)
            sock.sendall(b"abc")
            status, _, body = read_response(f)
            self.assertEqual(status, 200)
            self.assertIn(b"len=3", body)
        names = [n.lower() for n, _ in self.last_request("/api/up")["headers"]]
        self.assertNotIn("expect", names)

    def last_request(self, path):
        for b in (self.b1, self.b2):
            with b.lock:
                for r in reversed(b.requests):
                    if r["path"] == path:
                        return r
        self.fail("백엔드가 %s를 받지 않았습니다" % path)

    def test_forwarded_headers(self):
        status, _, _ = self.server.exchange(
            b"GET /api/hdr?id=1 HTTP/1.1\r\nHost: example.com\r\n"
            b"Connection: keep-alive, X-Hop\r\nX-Hop: 1\r\nKeep-Alive: 5\r\n"
            b"X-End: 1\r\nX-Forwarded-For: 1.2.3.4\r\n\r\n"
        )
        self.assertEqual(status, 200)
        req = self.last_request("/api/hdr?id=1")
        headers = {n.lower(): v for n, v in req["headers"]}
        self.assertEqual(req["version"], "HTTP/1.0")
        self.assertEqual(headers["host"], "example.com")
        self.assertEqual(headers["x-forwarded-for"], "127.0.0.1")
        self.assertEqual(headers["via"], "1.1 tiny")
        self.assertEqual(headers["connection"], "close")
        self.assertEqual(headers["x-end"], "1")
        self.assertNotIn("x-hop", headers)
        self.assertNotIn("keep-alive", headers)

    def test_unclean_paths_are_403(self):
        for path in ("/api/../x", "/api//x", "/api/./x", "//home.html"):
            with self.subTest(path=path):
                before = self.b1.count() + self.b2.count()
                status, _, _ = self.server.get(path)
                self.assertEqual(status, 403)
                self.assertEqual(self.b1.count() + self.b2.count(), before)

    def test_other_methods_are_501(self):
        status, headers, _ = self.server.exchange(
            b"DELETE /api/users/3 HTTP/1.1\r\nHost: x\r\n\r\n"
        )
        self.assertEqual(status, 501)
        self.assertEqual(headers["connection"], "close")

    def test_keep_alive_with_length(self):
        with self.server.connect() as sock:
            f = sock.makefile("rb")
            for _ in range(2):
                sock.sendall(b"GET /api/users HTTP/1.1\r\nHost: x\r\n\r\n")
                status, headers, _ = read_response(f)
                self.assertEqual(status, 200)
                self.assertEqual(headers["connection"], "keep-alive")

    def test_no_length_response_closes(self):
        with self.server.connect() as sock:
            sock.sendall(b"GET /api/nolen HTTP/1.1\r\nHost: x\r\n\r\n")
            status, headers, body = read_response(sock.makefile("rb"))
            self.assertEqual(status, 200)
            self.assertNotIn("content-length", headers)
            self.assertEqual(headers["connection"], "close")
            self.assertEqual(body, b"no length body " * 100)

    def test_204(self):
        with self.server.connect() as sock:
            f = sock.makefile("rb")
            sock.sendall(b"GET /api/204 HTTP/1.1\r\nHost: x\r\n\r\n")
            status, headers, _ = read_response(f)
            self.assertEqual(status, 204)
            self.assertNotIn("content-length", headers)
            self.assertEqual(headers["connection"], "keep-alive")

    def test_cut_with_length_closes(self):
        with self.server.connect() as sock:
            sock.sendall(b"GET /api/cutlen HTTP/1.1\r\nHost: x\r\n\r\n")
            f = sock.makefile("rb")
            status, headers = read_head(f)
            self.assertEqual(int(headers["content-length"]), 100)
            self.assertEqual(f.read(), b"0123456789")  # 덜 오고 닫힘

    def test_cut_without_length_is_reset(self):
        with self.server.connect() as sock:
            sock.sendall(b"GET /api/cutnolen HTTP/1.1\r\nHost: x\r\n\r\n")
            f = sock.makefile("rb")
            self.assertEqual(read_head(f)[0], 200)
            with self.assertRaises(ConnectionResetError):
                while f.read(4096):
                    pass

    def test_silent_backend_is_504(self):
        start = time.monotonic()
        status, headers, _ = self.server.get("/api/silent")
        elapsed = time.monotonic() - start
        self.assertEqual(status, 504)
        self.assertEqual(headers["connection"], "keep-alive")
        self.assertGreater(elapsed, 1.5)
        self.assertLess(elapsed, 4)

    def test_slow_but_in_time(self):
        status, _, _ = self.server.get("/api/slow?1")
        self.assertEqual(status, 200)


class ProxyFailureTest(unittest.TestCase):
    def test_one_backend_down(self):
        b = Backend()
        server = Server("--route", "/api=127.0.0.1:%d,127.0.0.1:%d" % (
            unused_port(), b.port))
        try:
            for _ in range(4):
                status, headers, _ = server.get("/api/x")
                self.assertEqual(status, 200)
                self.assertEqual(int(headers["x-backend"]), b.port)
        finally:
            server.stop()
            b.close()

    def test_all_backends_down_is_502(self):
        server = Server("--route", "/api=127.0.0.1:%d,127.0.0.1:%d" % (
            unused_port(), unused_port()))
        try:
            status, headers, _ = server.get("/api/x")
            self.assertEqual(status, 502)
            self.assertEqual(headers["connection"], "keep-alive")
            self.assertEqual(server.get("/home.html")[0], 200)
        finally:
            server.stop()

    def test_connect_timeout_mixed_is_504(self):
        # listen(0) 소켓의 대기열을 채우면 새 연결이 응답을 받지 못함 (연결 시간 초과)
        full = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        full.bind((HOST, 0))
        full.listen(0)
        port = full.getsockname()[1]
        fillers = []
        try:
            for _ in range(8):
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.setblocking(False)
                s.connect_ex((HOST, port))
                fillers.append(s)
            time.sleep(0.2)
            probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            probe.settimeout(1)
            try:
                probe.connect((HOST, port))
                self.skipTest("이 환경에서는 연결 시간 초과를 만들 수 없음")
            except socket.timeout:
                pass
            except OSError:
                self.skipTest("이 환경에서는 연결 시간 초과를 만들 수 없음")
            finally:
                probe.close()
            for routes in (
                "/api=127.0.0.1:%d,127.0.0.1:%d" % (port, unused_port()),
                "/api=127.0.0.1:%d,127.0.0.1:%d" % (unused_port(), port),
            ):
                with self.subTest(routes=routes):
                    server = Server("--route", routes)
                    try:
                        start = time.monotonic()
                        status, _, _ = server.exchange(
                            b"GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n", timeout=15)
                        self.assertEqual(status, 504)
                        self.assertLess(time.monotonic() - start, 5)
                    finally:
                        server.stop()
        finally:
            for s in fillers:
                s.close()
            full.close()

    def test_tiny_as_backend_post_adder(self):
        backend = Server()
        proxy = Server("--route", "/cgi-bin=127.0.0.1:%d" % backend.port)
        try:
            status, _, body = proxy.exchange(
                b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n"
                b"Content-Type: application/x-www-form-urlencoded\r\n\r\na=1&b=2"
            )
            self.assertEqual(status, 200)
            self.assertIn(b"1 + 2 = 3", body)
            self.assertIn("/cgi-bin/adder", backend.read_log())
        finally:
            proxy.stop()
            backend.stop()

    def test_bad_routes_refuse_to_start(self):
        port = find_free_port()
        for spec in ("/x=127.0.0.1:%d" % port, "api=127.0.0.1:80", "/a=127.0.0.1:0",
                     "/a=127.0.0.1:80,"):
            with self.subTest(spec=spec):
                server = Server("--route", spec, port=port, wait=False)
                try:
                    self.assertEqual(server.wait_exit(10), 1)
                    self.assertIn("--route", server.read_log())
                finally:
                    server.cleanup()


# ============================== M7 ==============================


class RequestCheckTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Server("--timeout", "1")

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_bad_requests(self):
        cases = [
            (b"G@T / HTTP/1.1\r\nHost: x\r\n\r\n", 400),
            (b"PUT / HTTP/1.1\r\nHost: x\r\n\r\n", 501),
            (b"GET ./secret HTTP/1.1\r\nHost: x\r\n\r\n", 400),
            (b"GET / HTTP/1.1\r\nHost: x\r\nX-A: a\x00b\r\n\r\n", 400),
            (b"GET / HTTP/1.1\r\nHost: x\r\nConnection : close\r\n\r\n", 400),
        ]
        for data, expected in cases:
            with self.subTest(data=data[:30]):
                status, headers, _ = self.server.exchange(data)
                self.assertEqual(status, expected)
                self.assertEqual(headers["connection"], "close")

    def test_405_and_404_order(self):
        status, headers, _ = self.server.exchange(
            b"POST /home.html HTTP/1.1\r\nHost: x\r\n\r\n")
        self.assertEqual((status, headers["allow"]), (405, "GET, HEAD"))
        status, headers, body = self.server.exchange(
            b"HEAD /cgi-bin/adder?1&2 HTTP/1.1\r\nHost: x\r\n\r\n", head_request=True)
        self.assertEqual((status, headers["allow"]), (405, "GET, POST"))
        status, _, _ = self.server.exchange(
            b"POST /nofile.html HTTP/1.1\r\nHost: x\r\n\r\n")
        self.assertEqual(status, 404)
        status, _, _ = self.server.exchange(
            b"HEAD /cgi-bin/none HTTP/1.1\r\nHost: x\r\n\r\n", head_request=True)
        self.assertEqual(status, 404)

    def test_slow_header_is_cut_at_deadline(self):
        # --timeout 1: 첫 바이트부터 헤더 끝까지 2초
        with self.server.connect(timeout=10) as sock:
            data = b"GET /home.html HTTP/1.1\r\nHost: x\r\nX-Slow: " + b"a" * 100
            start = time.monotonic()
            closed_at = None
            for byte in data:
                try:
                    sock.sendall(bytes([byte]))
                except OSError:
                    closed_at = time.monotonic()
                    break
                sock.settimeout(0.3)
                try:
                    if sock.recv(1) == b"":
                        closed_at = time.monotonic()
                        break
                except (socket.timeout, TimeoutError):
                    pass
                except ConnectionResetError:
                    closed_at = time.monotonic()
                    break
            self.assertIsNotNone(closed_at)
            self.assertGreater(closed_at - start, 1.5)
            self.assertLess(closed_at - start, 3.0)

    def test_slow_body_is_cut_at_deadline(self):
        # --timeout 1: 본문은 읽기 시작 뒤 6초
        with self.server.connect(timeout=15) as sock:
            sock.sendall(b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\n"
                         b"Content-Length: 100\r\n\r\n")
            start = time.monotonic()
            closed_at = None
            for _ in range(100):
                try:
                    sock.sendall(b"a")
                except OSError:
                    closed_at = time.monotonic()
                    break
                sock.settimeout(0.3)
                try:
                    if sock.recv(1) == b"":
                        closed_at = time.monotonic()
                        break
                except (socket.timeout, TimeoutError):
                    pass
                except ConnectionResetError:
                    closed_at = time.monotonic()
                    break
            self.assertIsNotNone(closed_at)
            self.assertGreater(closed_at - start, 5.0)
            self.assertLess(closed_at - start, 7.5)

    def test_mixed_concurrent_requests(self):
        b = Backend()
        server = Server("--route", "/api=127.0.0.1:%d" % b.port)
        results = []
        lock = threading.Lock()

        def one(i):
            path = ("/home.html", "/cgi-bin/adder?1&2", "/api/x")[i % 3]
            try:
                status = server.get(path)[0]
            except OSError as e:
                status = repr(e)
            with lock:
                results.append(status)

        try:
            threads = [threading.Thread(target=one, args=(i,)) for i in range(50)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            self.assertEqual(results, [200] * 50)
        finally:
            server.stop()
            b.close()

    def test_client_drop_during_big_file(self):
        with open(os.path.join(self.server.www, "big.bin"), "wb") as f:
            f.write(b"x" * BIG_FILE_SIZE)
        with self.server.connect() as sock:
            sock.sendall(b"GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n")
            sock.recv(1000)
        status, _, _ = self.server.get("/home.html")
        self.assertEqual(status, 200)


class ShutdownTest(unittest.TestCase):
    def test_sigterm_finishes_big_download(self):
        server = Server()
        try:
            with open(os.path.join(server.www, "big.bin"), "wb") as f:
                f.write(b"x" * BIG_FILE_SIZE)
            with server.connect(timeout=10) as sock:
                sock.sendall(b"GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n")
                f = sock.makefile("rb")
                status, headers = read_head(f)
                first = f.read(1024)
                server.proc.send_signal(signal.SIGTERM)
                time.sleep(0.3)
                # 종료 중: 새 연결은 받지 않음
                with self.assertRaises(OSError):
                    socket.create_connection((HOST, server.port), timeout=1).close()
                rest = f.read(int(headers["content-length"]) - len(first))
                self.assertEqual(len(first) + len(rest), BIG_FILE_SIZE)
            self.assertEqual(server.wait_exit(10), 0)
            log = server.read_log()
            self.assertIn("종료 시작", log)
            self.assertIn("종료 완료", log)
        finally:
            server.cleanup()

    def test_idle_keepalive_does_not_delay_shutdown(self):
        server = Server()  # 읽기 타임아웃 기본 5초
        try:
            with server.connect() as sock:
                sock.sendall(b"GET /home.html HTTP/1.1\r\nHost: x\r\n\r\n")
                read_response(sock.makefile("rb"))
                start = time.monotonic()
                server.proc.send_signal(signal.SIGTERM)
                self.assertEqual(server.wait_exit(10), 0)
                self.assertLess(time.monotonic() - start, 2)
        finally:
            server.cleanup()

    def test_ctrl_c_to_group_keeps_cgi_request(self):
        # 서버를 자기 프로세스 그룹에서 띄우고 그룹 전체에 SIGINT (터미널의 Ctrl+C)
        server = Server(new_session=True)
        try:
            with server.connect(timeout=10) as sock:
                sock.sendall(b"GET /cgi-bin/delay HTTP/1.1\r\nHost: x\r\n\r\n")
                time.sleep(0.3)
                os.killpg(server.proc.pid, signal.SIGINT)
                status, headers, body = read_response(sock.makefile("rb"))
                self.assertEqual(status, 200)
                self.assertEqual(body, b"delayed\n")
                self.assertEqual(headers["connection"], "close")  # 종료 중의 응답
            self.assertEqual(server.wait_exit(10), 0)
        finally:
            server.cleanup()

    def test_second_signal_exits_now(self):
        server = Server()
        cgi_pids = []
        try:
            with server.connect(timeout=20) as sock:
                sock.sendall(b"GET /cgi-bin/slow HTTP/1.1\r\nHost: x\r\n\r\n")
                time.sleep(0.3)
                out = subprocess.run(["ps", "-A", "-o", "pid=,ppid="],
                                     capture_output=True, text=True).stdout
                cgi_pids = [int(line.split()[0]) for line in out.splitlines()
                            if line.split()[1] == str(server.proc.pid)]
                server.proc.send_signal(signal.SIGTERM)
                time.sleep(0.3)
                self.assertIsNone(server.proc.poll())  # CGI를 기다리는 중
                start = time.monotonic()
                server.proc.send_signal(signal.SIGINT)
                self.assertEqual(server.wait_exit(5), 1)
                self.assertLess(time.monotonic() - start, 2)
        finally:
            server.cleanup()
            for pid in cgi_pids:  # 강제 종료로 남은 CGI(sleep)를 정리 (M7-9 한계)
                try:
                    os.kill(pid, signal.SIGKILL)
                except OSError:
                    pass

    def test_clean_exit_without_leaks(self):
        b = Backend()
        server = Server("--route", "/api=127.0.0.1:%d" % b.port)
        try:
            server.get("/home.html")
            server.get("/cgi-bin/adder?1&2")
            server.get("/api/x")
            server.get("/nope")
            server.exchange(b"G@T / HTTP/1.1\r\n\r\n")
            server.exchange(b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\n"
                            b"Content-Length: 3\r\n\r\n1&2")
            server.proc.send_signal(signal.SIGTERM)
            code = server.wait_exit(10)
            log = server.read_log()
            self.assertEqual(code, 0, log)
            self.assertNotIn("LeakSanitizer", log)
        finally:
            server.cleanup()
            b.close()


@unittest.skipUnless(IS_LINUX, "/proc가 있는 Linux에서만 확인")
class ResourceTest(unittest.TestCase):
    def fd_count(self, server):
        return len(os.listdir("/proc/%d/fd" % server.proc.pid))

    def test_fds_and_zombies_after_drops(self):
        b = Backend()
        server = Server("--route", "/api=127.0.0.1:%d" % b.port, "--timeout", "1")
        try:
            with open(os.path.join(server.www, "big.bin"), "wb") as f:
                f.write(b"x" * BIG_FILE_SIZE)
            server.get("/home.html")
            time.sleep(0.5)
            before = self.fd_count(server)
            for _ in range(3):
                for data in (
                    b"GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n",
                    b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\n"
                    b"Content-Length: 100\r\n\r\nab",
                    b"GET /cgi-bin/delay HTTP/1.1\r\nHost: x\r\n\r\n",
                    b"GET /api/slow?0.5 HTTP/1.1\r\nHost: x\r\n\r\n",
                    b"GET /api/nolen HTTP/1.1\r\nHost: x\r\n\r\n",
                ):
                    sock = server.connect()
                    sock.sendall(data)
                    time.sleep(0.05)
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                    struct.pack("ii", 1, 0))
                    sock.close()
            time.sleep(3)  # CGI(1초), 백엔드(0.5초), 본문 마감(1초)이 끝나길 기다림
            self.assertEqual(self.fd_count(server), before)
            out = subprocess.run(["ps", "-A", "-o", "ppid=,stat="],
                                 capture_output=True, text=True).stdout
            zombies = [line for line in out.splitlines()
                       if line.split()[0] == str(server.proc.pid)
                       and "Z" in line.split()[1]]
            self.assertEqual(zombies, [])
        finally:
            server.stop()
            b.close()

    def test_cgi_signal_settings_restored(self):
        server = Server()
        try:
            status, _, body = server.get("/cgi-bin/sigs")
            self.assertEqual(status, 200)
            values = dict(line.split(":\t") for line in body.decode().splitlines())
            blocked = int(values["SigBlk"], 16)
            ignored = int(values["SigIgn"], 16)
            bit = lambda sig: 1 << (sig - 1)
            self.assertEqual(blocked & (bit(signal.SIGINT) | bit(signal.SIGTERM)), 0)
            self.assertEqual(ignored & bit(signal.SIGPIPE), 0)
        finally:
            server.stop()


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
