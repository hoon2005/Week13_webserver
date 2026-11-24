"""M7 종료와 CGI 프로세스 정리 기능 테스트입니다.

사용법: python3 shutdown_test.py <build 폴더>

서버를 띄우는 도우미는 phase2_test.py의 것을 씁니다. 그 도우미의 stop()은 5초가
지나면 강제로 끄고 로그를 지우므로, 종료를 확인하는 테스트는 wait_exit()로 직접
기다리고 시간 초과도 실패로 봅니다.
"""

import functools
import os
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest
from unittest import mock

from phase2_test import HOST, Server, closed_by_server, exchange, find_free_port, get
from phase2_test import read_response
from proxy_test import Backend, reply

GET_HOME = b"GET /home.html HTTP/1.1\r\nHost: x\r\n\r\n"


def cgi_request(name):
    return b"GET /cgi-bin/%s HTTP/1.1\r\nHost: x\r\n\r\n" % name.encode()


def alive(pid):
    """프로세스가 있고 좀비가 아니면 True"""
    out = subprocess.run(
        ["ps", "-o", "stat=", "-p", str(pid)], capture_output=True, text=True
    ).stdout.strip()
    return out != "" and not out.startswith("Z")


def wait_until(check, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if check():
            return True
        time.sleep(0.02)
    return False


class ShutdownCase(unittest.TestCase):
    def server(self, *args):
        # 새 세션으로 띄워, 터미널의 Ctrl+C처럼 프로세스 그룹에 시그널을 보낼 수
        # 있게 함 (테스트 자신은 다른 그룹이라 안전)
        popen = functools.partial(subprocess.Popen, start_new_session=True)
        with mock.patch("phase2_test.subprocess.Popen", popen):
            server = Server(*args)
        self.addCleanup(server.stop)
        return server

    def cgi(self, server, name, script):
        """셸 CGI를 만듦. CGI의 현재 폴더는 서버의 루트(www)"""
        path = os.path.join(server.www, "cgi-bin", name)
        with open(path, "w") as f:
            f.write("#!/bin/sh\n" + script)
        os.chmod(path, 0o755)

    def started_pid(self, server, name):
        """CGI가 자기 pid를 파일에 적을 때까지 기다려 그 pid를 돌려줌"""
        path = os.path.join(server.www, name)

        def ready():
            if not os.path.exists(path):
                return False
            with open(path) as f:
                return f.read().endswith("\n")

        self.assertTrue(wait_until(ready, 5), "CGI가 시작되지 않음")
        with open(path) as f:
            pid = int(f.read())
        self.addCleanup(self.kill_quietly, pid)
        return pid

    @staticmethod
    def kill_quietly(pid):
        for target in (pid, -pid):
            try:
                os.kill(target, signal.SIGKILL)
            except OSError:
                pass

    def log(self, server):
        server.log.flush()
        with open(server.log_path, errors="replace") as f:
            return f.read()

    def wait_exit(self, server, seconds, code):
        """seconds 안에 끝나고 종료 코드가 code인지. 시간 초과도 실패"""
        try:
            got = server.proc.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            self.fail("%g초 안에 끝나지 않음\n%s" % (seconds, self.log(server)))
        self.assertEqual(got, code, self.log(server))


class GracefulTest(ShutdownCase):
    def test_여러_실패를_거친_뒤_SIGTERM으로_끝내면_종료_코드_0이고_누수가_없다(self):
        # ASan 빌드는 프로그램이 정상 종료할 때 누수를 검사함 (Linux).
        # 실패 경로의 해제를 보려고 먼저 실패하는 요청들을 보냄
        def reset(conn, head):
            conn.sendall(b"HTTP/1.0 200 OK\r\n\r\nabc")
            time.sleep(0.2)
            conn.setsockopt(
                socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))

        backends = [
            Backend(reply(b"HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok")),
            Backend(reply(b"HTTP/1.0 200 OK\r\nContent-Length: 9\r\n\r\ncut")),
            Backend(reply(b"broken\r\n\r\n")),
            Backend(reset),
            Backend(lambda conn, head: time.sleep(3)),
        ]
        args = ["--timeout", "1", "--proxy-timeout", "1"]
        for i, b in enumerate(backends):
            self.addCleanup(b.close)
            args += ["--proxy", "/p%d/=%s" % (i, b.name)]
        args += ["--proxy", "/dead/=%s:%d" % (HOST, find_free_port())]
        server = self.server(*args)

        for path in ["/home.html", "/nope", "/cgi-bin/adder?1&2", "/cgi-bin/fail",
                     "/cgi-bin/nohdr", "/cgi-bin/big", "/dead/x"]:
            get(server, path)
        for i in range(len(backends)):
            try:
                get(server, "/p%d/x" % i)
            except OSError:
                pass  # RST로 끊긴 응답
        for data in (
            b"POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n",
            b"POST /p0/ HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc",
            b"GET / HTTP/1.1\r\n\r\n",
        ):
            exchange(server, data)
        with server.connect() as sock:  # 본문 도중 끊김
            sock.sendall(b"POST /p0/ HTTP/1.1\r\nHost: x\r\nContent-Length: 9\r\n\r\nab")

        server.proc.send_signal(signal.SIGTERM)
        self.wait_exit(server, 5, 0)
        log = self.log(server)
        self.assertIn("종료 완료", log)
        self.assertNotIn("Sanitizer", log)

    def test_쉬고_있는_keep_alive_연결은_종료를_늦추지_않는다(self):
        server = self.server("--timeout", "10")
        with server.connect() as sock:
            sock.sendall(GET_HOME)
            read_response(sock.makefile("rb"))
            time.sleep(0.2)
            server.proc.send_signal(signal.SIGTERM)
            self.wait_exit(server, 2, 0)

    def test_아무_연결이_없어도_SIGINT로_바로_끝난다(self):
        server = self.server()
        server.proc.send_signal(signal.SIGINT)
        self.wait_exit(server, 2, 0)

    def test_터미널의_CtrlC에도_실행_중인_CGI_요청을_마치고_끝난다(self):
        server = self.server()
        self.cgi(server, "work", "echo $$ > work.pid\nsleep 1\n"
                                 "printf 'Content-Type: text/plain\\n\\ndone'\n")
        with server.connect() as sock:
            sock.sendall(cgi_request("work"))
            self.started_pid(server, "work.pid")
            os.killpg(server.proc.pid, signal.SIGINT)
            status, headers, body, _ = read_response(sock.makefile("rb"))
        self.assertEqual((status, body), (200, b"done"))
        self.assertEqual(headers["connection"], "close")
        self.wait_exit(server, 3, 0)

    def test_받는_중인_큰_파일도_끝까지_보내고_끝난다(self):
        server = self.server()
        size = 20 << 20
        with open(os.path.join(server.www, "big.bin"), "wb") as f:
            f.write(b"x" * size)
        with server.connect() as sock:
            sock.sendall(b"GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n")
            f = sock.makefile("rb")
            f.read(1000)
            server.proc.send_signal(signal.SIGTERM)
            time.sleep(0.5)
            rest = f.read()
        self.assertGreater(1000 + len(rest), size)
        self.wait_exit(server, 3, 0)

    def test_프록시로_기다리던_요청도_마치고_끝난다(self):
        def slow(conn, head):
            time.sleep(1)
            conn.sendall(b"HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok")
        b = Backend(slow)
        self.addCleanup(b.close)
        server = self.server("--proxy", "/api/=" + b.name)
        with server.connect() as sock:
            sock.sendall(b"GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n")
            time.sleep(0.3)
            server.proc.send_signal(signal.SIGTERM)
            status, headers, body, _ = read_response(sock.makefile("rb"))
        self.assertEqual((status, body), (200, b"ok"))
        self.assertEqual(headers["connection"], "close")
        self.wait_exit(server, 3, 0)

    def test_큐에서_기다리던_연결도_요청_하나씩_응답을_받는다(self):
        server = self.server("--threads", "1", "--queue", "8")
        self.cgi(server, "work", "echo $$ > work.pid\nsleep 1\nprintf 'Content-Type: text/plain\\n\\n'\n")
        with server.connect() as a, server.connect() as b, server.connect() as c:
            a.sendall(cgi_request("work"))
            self.started_pid(server, "work.pid")
            for sock in (b, c):
                sock.sendall(GET_HOME + GET_HOME)  # 둘째 요청은 처리하지 않음
            server.proc.send_signal(signal.SIGTERM)
            for sock in (b, c):
                f = sock.makefile("rb")
                status, headers, _, _ = read_response(f)
                self.assertEqual((status, headers["connection"]), (200, "close"))
                self.assertIsNone(read_response(f))
        self.wait_exit(server, 3, 0)

    def test_큐가_꽉_차_main이_잠들어_있어도_종료가_시작된다(self):
        server = self.server("--threads", "1", "--queue", "1")
        self.cgi(server, "work", "echo $$ > work.pid\nsleep 1\nprintf 'Content-Type: text/plain\\n\\n'\n")
        a = server.connect()
        a.sendall(cgi_request("work"))
        self.started_pid(server, "work.pid")
        queued = server.connect()
        queued.sendall(GET_HOME)
        time.sleep(0.2)
        blocked = server.connect()  # accept됐지만 큐에 들어가지 못함
        time.sleep(0.2)
        server.proc.send_signal(signal.SIGTERM)
        self.assertTrue(closed_by_server(blocked, 0.6))  # 작업자가 바쁠 때 바로 닫음
        self.assertEqual(read_response(queued.makefile("rb"))[0], 200)
        self.wait_exit(server, 3, 0)
        for sock in (a, queued, blocked):
            sock.close()

    def test_종료가_시작되면_새_연결을_받지_않는다(self):
        server = self.server()
        self.cgi(server, "work", "echo $$ > work.pid\nsleep 1\nprintf 'Content-Type: text/plain\\n\\n'\n")
        with server.connect() as sock:
            sock.sendall(cgi_request("work"))
            self.started_pid(server, "work.pid")
            server.proc.send_signal(signal.SIGTERM)
            time.sleep(0.2)
            with self.assertRaises(ConnectionRefusedError):
                server.connect()
        self.wait_exit(server, 3, 0)

    def test_SIGTERM이_또_와도_강제로_끝내지_않는다(self):
        server = self.server()
        self.cgi(server, "work", "echo $$ > work.pid\nsleep 1\nprintf 'Content-Type: text/plain\\n\\ndone'\n")
        with server.connect() as sock:
            sock.sendall(cgi_request("work"))
            self.started_pid(server, "work.pid")
            for _ in range(2):
                server.proc.send_signal(signal.SIGTERM)
                time.sleep(0.2)
            self.assertEqual(read_response(sock.makefile("rb"))[2], b"done")
        self.wait_exit(server, 3, 0)


class ForcedTest(ShutdownCase):
    def test_정리_중에_SIGINT가_또_오면_실행_중인_CGI와_그_자식까지_끝내고_종료한다(self):
        server = self.server()
        self.cgi(server, "forever",
                 "sleep 300 &\necho $! > child.pid\necho $$ > forever.pid\nwait\n")
        with server.connect() as sock:
            sock.sendall(cgi_request("forever"))
            cgi = self.started_pid(server, "forever.pid")
            child = self.started_pid(server, "child.pid")
            os.killpg(server.proc.pid, signal.SIGINT)
            time.sleep(0.5)
            self.assertIsNone(server.proc.poll())  # 첫 번째는 마무리를 기다림
            self.assertTrue(alive(cgi))
            os.killpg(server.proc.pid, signal.SIGINT)
            self.wait_exit(server, 2, 1)
        self.assertTrue(wait_until(lambda: not alive(cgi), 2))
        self.assertTrue(wait_until(lambda: not alive(child), 2))


class CgiProcessTest(ShutdownCase):
    def test_CGI_실행_중에_서버가_죽어도_새_서버가_같은_포트에서_응답한다(self):
        # CGI가 리스닝 소켓을 물려받으면 포트가 잡혀 있어 bind가 실패함
        server = self.server()
        self.cgi(server, "forever", "echo $$ > forever.pid\nexec sleep 300\n")
        sock = server.connect()
        sock.sendall(cgi_request("forever"))
        cgi = self.started_pid(server, "forever.pid")
        os.kill(server.proc.pid, signal.SIGKILL)  # 서버 프로세스에만
        server.proc.wait(timeout=5)
        self.assertTrue(alive(cgi))

        with mock.patch("phase2_test.find_free_port", lambda: server.port):
            again = self.server()
        self.assertEqual(get(again, "/home.html")[0], 200)
        self.assertTrue(alive(cgi))
        sock.close()

    def test_시간_초과한_CGI는_자식까지_끝낸다(self):
        server = self.server()
        self.cgi(server, "forever", "sleep 300 &\necho $! > child.pid\nwait\n")
        with server.connect(timeout=20) as sock:
            sock.sendall(cgi_request("forever"))
            child = self.started_pid(server, "child.pid")
            self.assertEqual(read_response(sock.makefile("rb"))[0], 504)
        self.assertTrue(wait_until(lambda: not alive(child), 2))

    def test_CGI는_시그널을_기본_동작으로_시작한다(self):
        # 서버의 차단이나 무시를 물려받으면 자신에게 보낸 시그널에 죽지 않고 200이 됨
        server = self.server()
        for name in ("INT", "TERM", "PIPE"):
            with self.subTest(signal=name):
                self.cgi(server, "self" + name,
                         "kill -%s $$\nprintf 'Content-Type: text/plain\\n\\nstill alive'\n" % name)
                self.assertEqual(get(server, "/cgi-bin/self" + name)[0], 500)

    def test_CGI가_많이_겹쳐_실행돼도_좀비가_남지_않고_정상_종료한다(self):
        server = self.server()
        errors = []

        def work():
            for _ in range(10):
                if get(server, "/cgi-bin/adder?1&2")[0] != 200:
                    errors.append(1)

        threads = [threading.Thread(target=work) for _ in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(errors, [])
        out = subprocess.run(
            ["ps", "-A", "-o", "ppid="], capture_output=True, text=True).stdout
        self.assertNotIn(str(server.proc.pid), out.split())
        server.proc.send_signal(signal.SIGTERM)
        self.wait_exit(server, 3, 0)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
