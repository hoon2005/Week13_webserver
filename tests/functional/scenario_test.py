"""테스트 시나리오 표에서 테스트 위치가 비어 있던 행의 기능 테스트입니다.

사용법: python3 scenario_test.py <build 폴더>

테스트 이름 앞의 ID는 시나리오 표의 ID입니다. 서버와 테스트용 백엔드 도우미는
m6m7_test.py의 것을 씁니다.
"""

import os
import resource
import signal
import socket
import subprocess
import sys
import threading
import time
import unittest
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m6m7_test import (  # noqa: E402
    FIXTURE_DIR,
    HOST,
    IS_LINUX,
    Backend,
    Server,
    read_head,
    read_response,
)


def get_on(f, sock, path):
    """열린 연결로 GET 하나를 보내고 응답을 읽음"""
    sock.sendall(("GET %s HTTP/1.1\r\nHost: x\r\n\r\n" % path).encode())
    return read_response(f)


def children_of(pid):
    out = subprocess.run(["ps", "-A", "-o", "pid=,ppid="],
                         capture_output=True, text=True).stdout
    return [int(line.split()[0]) for line in out.splitlines()
            if line.split()[1] == str(pid)]


# ============================== 정적 파일, 연결 ==============================


class StaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Server()
        www = cls.server.www
        os.makedirs(os.path.join(www, "docs"))
        with open(os.path.join(www, "docs", "index.html"), "w") as f:
            f.write("<p>docs</p>")
        noread = os.path.join(www, "noread.html")
        with open(noread, "w") as f:
            f.write("secret")
        os.chmod(noread, 0o000)
        noexec = os.path.join(www, "cgi-bin", "noexec")
        with open(noexec, "w") as f:
            f.write("#!/bin/sh\nprintf 'Content-type: text/plain\\r\\n\\r\\nran'\n")
        os.chmod(noexec, 0o644)
        with open(os.path.join(www, "crying_cat.jpg"), "wb") as f:
            f.write(b"\xff\xd8\xff\xe0cat")

    @classmethod
    def tearDownClass(cls):
        os.chmod(os.path.join(cls.server.www, "noread.html"), 0o644)
        cls.server.stop()

    def test_static_image(self):
        """static-image: 이미지 파일을 올바른 타입으로 돌려주는지"""
        for name, ctype in (("godzilla.gif", "image/gif"),
                            ("godzilla.jpg", "image/jpeg")):
            with self.subTest(name=name):
                status, headers, body = self.server.get("/" + name)
                self.assertEqual(status, 200)
                self.assertEqual(headers["content-type"], ctype)
                with open(os.path.join(FIXTURE_DIR, name), "rb") as f:
                    self.assertEqual(body, f.read())

    def test_static_dir_perm_403(self):
        """static-dir-perm-403: 폴더와 권한 없는 파일을 403으로 거절하고 연결을 유지하는지"""
        with self.server.connect() as sock:
            f = sock.makefile("rb")
            for path in ("/docs", "/noread.html", "/cgi-bin/noexec"):
                with self.subTest(path=path):
                    status, headers, _ = get_on(f, sock, path)
                    self.assertEqual(status, 403)
                    self.assertEqual(headers["connection"], "keep-alive")
            self.assertEqual(get_on(f, sock, "/home.html")[0], 200)

    def test_cgi_dir_missing(self):
        """cgi-dir-missing: CGI 폴더 자체와 없는 CGI를 GET하면 403, 404이고 서버가 유지되는지"""
        self.assertEqual(self.server.get("/cgi-bin/")[0], 403)
        self.assertEqual(self.server.get("/cgi-bin/nope")[0], 404)
        self.assertIsNone(self.server.proc.poll())
        self.assertEqual(self.server.get("/home.html")[0], 200)


class PercentPathTest(unittest.TestCase):
    """static-percent-path: 경로의 퍼센트 인코딩을 한 번만 풀고, 푼 뒤에 다시 검사하는지

    규칙 (tiny.c uri_path_decode)
      %XX는 한 번만 풂. 잘못된 표기와 제어 문자는 400, 인코딩된 '/'(%2F)는 404
      푼 경로에 "..", "." 조각이 있으면 403
      쿼리는 풀지 않음. 중계 요청은 원래 URI를 그대로 넘김
    """

    KOREAN = "내 사진.jpg"

    @classmethod
    def setUpClass(cls):
        cls.server = Server()
        www = cls.server.www
        cls.files = {"crying_cat.jpg": b"\xff\xd8cat", cls.KOREAN: b"\xff\xd8korean",
                     "a?b.html": b"<p>question</p>", "%2e%2e": b"literal"}
        for name, data in cls.files.items():
            with open(os.path.join(www, name), "wb") as f:
                f.write(data)
        os.makedirs(os.path.join(www, "docs"))
        with open(os.path.join(www, "docs", "index.html"), "w") as f:
            f.write("<p>docs</p>")

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_encoded_names_are_found(self):
        """인코딩된 이름이 실제 파일로 풀림 (한글, 공백, '?', '_', '.')"""
        cases = (
            ("/crying%5Fcat.jpg", "crying_cat.jpg"),
            ("/" + urllib.parse.quote(self.KOREAN), self.KOREAN),
            ("/a%3Fb.html", "a?b.html"),          # %3F는 쿼리 시작이 아니라 이름의 일부
            ("/%252e%252e", "%2e%2e"),            # 한 번만 풀어서 "%2e%2e"라는 이름
        )
        for path, name in cases:
            with self.subTest(path=path):
                status, _, body = self.server.get(path)
                self.assertEqual(status, 200)
                self.assertEqual(body, self.files[name])
        status, headers, _ = self.server.get("/home%2Ehtml?v=1")
        self.assertEqual(status, 200)
        self.assertEqual(headers["content-type"], "text/html")

    def test_decoded_dot_segments_are_403(self):
        """풀고 나서 ".."이나 "."이 되는 조각은 403 (풀기 전 검사를 피하지 못함)"""
        for path in ("/%2e%2e/etc/passwd", "/docs/%2E%2E/home.html",
                     "/docs/.%2e/home.html", "/%2e/home.html"):
            with self.subTest(path=path):
                self.assertEqual(self.server.get(path)[0], 403)

    def test_bad_encodings_are_400_and_connection_survives(self):
        """잘못된 %와 제어 문자는 400, 인코딩된 '/'는 404. 서버와 연결은 유지"""
        cases = (("/home%G1.html", 400), ("/home.html%", 400), ("/home.html%2", 400),
                 ("/home.html%00.jpg", 400), ("/a%0Ab", 400), ("/a%7Fb", 400),
                 ("/docs%2Findex.html", 404), ("/docs%2findex.html", 404))
        with self.server.connect() as sock:
            f = sock.makefile("rb")
            for path, expected in cases:
                with self.subTest(path=path):
                    self.assertEqual(get_on(f, sock, path)[0], expected)
            self.assertEqual(get_on(f, sock, "/home.html")[0], 200)
        self.assertIsNone(self.server.proc.poll())

    def test_cgi_path_decoded_but_query_kept(self):
        """CGI 경로도 풀어서 찾고, QUERY_STRING은 인코딩된 채로 넘김"""
        status, _, body = self.server.get("/cgi%2Dbin/adder?1&2")
        self.assertEqual(status, 200)
        self.assertIn(b"1 + 2 = 3", body)
        status, _, body = self.server.get("/cgi-bin/query?a%20b%2Fc")
        self.assertEqual(status, 200)
        self.assertEqual(body, b"a%20b%2Fc")


class PercentProxyTest(unittest.TestCase):
    def test_proxy_forwards_encoded_uri_as_is(self):
        """static-percent-path: 중계 요청은 풀지 않고 받은 URI 그대로 백엔드에 넘김"""
        backend = Backend()
        server = Server("--route", "/api=%s:%d" % (HOST, backend.port))
        try:
            for path in ("/api/%EB%82%B4%20a.html?q=%2F", "/api/%2e%2e/x"):
                with self.subTest(path=path):
                    self.assertEqual(server.get(path)[0], 200)
                    self.assertEqual(backend.requests[-1]["path"], path)
        finally:
            server.stop()
            backend.close()


class RestartTest(unittest.TestCase):
    def test_conn_restart(self):
        """conn-restart: CGI 실행 중 kill -9 해도 같은 포트로 바로 다시 켜지는지

        CGI가 리스닝 소켓을 물려받으면 서버가 죽어도 포트가 열린 채로 남아
        bind가 실패함 (FD_CLOEXEC, 자식의 fd 정리로 막음)
        """
        first = Server()
        port = first.port
        cgi = []
        second = None
        try:
            sock = first.connect(timeout=20)
            sock.sendall(b"GET /cgi-bin/slow HTTP/1.1\r\nHost: x\r\n\r\n")
            time.sleep(0.5)
            cgi = children_of(first.proc.pid)
            self.assertTrue(cgi, "slow CGI가 실행되지 않음")
            first.proc.send_signal(signal.SIGKILL)
            first.proc.wait()
            sock.close()
            second = Server(port=port)  # bind에 실패하면 여기서 예외
            self.assertEqual(second.get("/home.html")[0], 200)
        finally:
            first.cleanup()
            if second is not None:
                second.stop()
            for pid in cgi:  # 고아가 된 slow CGI(sleep 30) 정리
                try:
                    os.kill(pid, signal.SIGKILL)
                except OSError:
                    pass


# ============================== fd 고갈 ==============================


def open_fds(pid):
    return sorted(int(n) for n in os.listdir("/proc/%d/fd" % pid))


def limit_with_one_free_slot(fds):
    """[0, L) 안에 비어 있는 fd 번호가 정확히 하나인 가장 작은 L"""
    used = set(fds)
    free = 0
    limit = 0
    while True:
        if limit not in used:
            free += 1
            if free == 1:
                return limit + 1
        limit += 1


@unittest.skipUnless(IS_LINUX and hasattr(resource, "prlimit"),
                     "실행 중인 프로세스의 fd 한도를 바꾸는 prlimit은 Linux에서만")
class FdExhaustTest(unittest.TestCase):
    def test_res_fd_exhaust_accept(self):
        """res-fd-exhaust-accept: fd가 모자라도 서버가 죽지 않고, 연결을 닫으면 회복하는지"""
        server = Server("--timeout", "1")
        clients = []
        try:
            pid = server.proc.pid
            soft, hard = resource.prlimit(pid, resource.RLIMIT_NOFILE)
            resource.prlimit(pid, resource.RLIMIT_NOFILE,
                             (len(open_fds(pid)) + 8, hard))
            for _ in range(40):  # 한도보다 많은 연결 (나머지는 backlog에서 대기)
                clients.append(socket.create_connection((HOST, server.port)))
            time.sleep(1)
            self.assertIsNone(server.proc.poll(), "서버가 죽음")
            self.assertIn("Too many open files", server.read_log())
            for c in clients:
                c.close()
            clients = []
            time.sleep(1.5)
            status, _, _ = server.exchange(
                b"GET /home.html HTTP/1.1\r\nHost: x\r\n\r\n", timeout=10)
            self.assertEqual(status, 200)
        finally:
            for c in clients:
                c.close()
            server.stop()

    def test_res_fd_exhaust_backend(self):
        """res-fd-exhaust-backend: 백엔드 소켓을 못 열면 다음 백엔드를 시도하지 않고 바로 502인지"""
        b1, b2 = Backend(), Backend()
        server = Server("--route", "/api=127.0.0.1:%d,127.0.0.1:%d" % (b1.port, b2.port))
        try:
            pid = server.proc.pid
            self.assertEqual(server.get("/home.html")[0], 200)
            time.sleep(0.3)
            soft, hard = resource.prlimit(pid, resource.RLIMIT_NOFILE)
            # 빈 자리 하나는 이 요청의 연결(accept)이 쓰고, 백엔드 소켓은 열 수 없음
            resource.prlimit(pid, resource.RLIMIT_NOFILE,
                             (limit_with_one_free_slot(open_fds(pid)), hard))
            before = b1.count() + b2.count()
            start = time.monotonic()
            status, _, _ = server.get("/api/x")
            self.assertEqual(status, 502)
            self.assertLess(time.monotonic() - start, 1)
            self.assertEqual(b1.count() + b2.count(), before)
            resource.prlimit(pid, resource.RLIMIT_NOFILE, (soft, hard))
            self.assertEqual(server.get("/api/x")[0], 200)
        finally:
            server.stop()
            b1.close()
            b2.close()


# ============================== 프록시 + tiny 백엔드 ==============================


def tiny_backend(name, port=None):
    """api/whoami.txt에 자기 이름을 담은 tiny 백엔드"""
    server = Server(port=port)
    os.makedirs(os.path.join(server.www, "api"), exist_ok=True)
    with open(os.path.join(server.www, "api", "whoami.txt"), "w") as f:
        f.write(name)
    return server


class ProxyTinyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.a = tiny_backend("A")
        cls.b = tiny_backend("B")
        cls.proxy = Server("--route", "/api=127.0.0.1:%d,127.0.0.1:%d"
                           % (cls.a.port, cls.b.port))

    @classmethod
    def tearDownClass(cls):
        cls.proxy.stop()
        cls.a.stop()
        cls.b.stop()

    def test_proxy_query_to_tiny(self):
        """proxy-query-to-tiny: 쿼리가 붙은 요청이 백엔드 tiny까지 가서 성공하는지"""
        status, _, body = self.proxy.get("/api/whoami.txt?x=1")
        self.assertEqual(status, 200)
        self.assertIn(body, (b"A", b"B"))

    def test_proxy_prefix_kept(self):
        """proxy-prefix-kept: 경로 접두사를 떼지 않고 그대로 넘기는지"""
        self.assertEqual(self.proxy.get("/api/whoami.txt")[0], 200)
        time.sleep(0.2)
        logs = self.a.read_log() + self.b.read_log()
        self.assertIn("GET /api/whoami.txt HTTP/1.0 200", logs)

    def test_proxy_rr_same_conn(self):
        """proxy-rr-same-conn: 한 keep-alive 연결 안에서도 요청마다 분배하는지"""
        with self.proxy.connect() as sock:
            f = sock.makefile("rb")
            bodies = []
            for _ in range(4):
                status, headers, body = get_on(f, sock, "/api/whoami.txt")
                self.assertEqual(status, 200)
                self.assertEqual(headers["connection"], "keep-alive")
                bodies.append(body)
        self.assertNotEqual(bodies[0], bodies[1])
        self.assertEqual(bodies, [bodies[0], bodies[1]] * 2)


class ProxyRecoverTest(unittest.TestCase):
    def test_proxy_backend_recover(self):
        """proxy-backend-recover: 꺼졌던 백엔드가 다시 켜지면 분배 대상으로 돌아오는지"""
        a, b = tiny_backend("A"), tiny_backend("B")
        a_port = a.port
        proxy = Server("--route", "/api=127.0.0.1:%d,127.0.0.1:%d" % (a_port, b.port))
        try:
            a.stop()
            seen = [proxy.get("/api/whoami.txt")[2] for _ in range(2)]
            self.assertEqual(seen, [b"B", b"B"])
            a = tiny_backend("A", port=a_port)  # 프록시는 다시 켜지 않음
            seen = [proxy.get("/api/whoami.txt")[2] for _ in range(2)]
            self.assertEqual(seen, [b"A", b"B"])
        finally:
            proxy.stop()
            a.stop()
            b.stop()


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
