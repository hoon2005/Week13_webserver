"""서버를 끈 직후 같은 포트로 다시 켤 수 있는지 확인하는 기능 테스트입니다.

사용법: python3 restart_test.py <build 폴더>

서버는 응답을 보낸 뒤 연결을 먼저 닫습니다. 그래서 서버 쪽 포트에 TIME_WAIT
연결이 남고, SO_REUSEADDR가 없으면 바로 다시 켤 때 bind가 실패합니다.
서버 실행과 대기에는 baseline_test.py의 도우미를 씁니다.
"""

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import unittest

import baseline_test as base


def start_server(port, www, log_path):
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [base.SERVER_PATH, str(port)],
        cwd=www,
        stdin=subprocess.DEVNULL,
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    return proc, log


def stop_server(proc, log):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    log.close()


def get_until_server_closes(port, path):
    """요청을 보내고 서버가 연결을 닫을 때까지 읽어서 응답 전체를 돌려줍니다.

    서버가 먼저 닫은 것을 확인한 뒤에 이쪽을 닫아야 TIME_WAIT이 서버 쪽에 남습니다.
    """
    with socket.create_connection(
        (base.HOST, port), timeout=base.REQUEST_TIMEOUT_SEC
    ) as sock:
        sock.sendall(("GET %s HTTP/1.0\r\n\r\n" % path).encode())
        chunks = []
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                return b"".join(chunks)
            chunks.append(chunk)


class RestartTest(unittest.TestCase):
    def setUp(self):
        server = base.SERVER_PATH
        if not os.access(server, os.X_OK):
            raise RuntimeError("실행 파일이 없습니다: %s (먼저 make)" % server)
        self.work_dir = tempfile.mkdtemp(prefix="server-restart-")
        self.www = os.path.join(self.work_dir, "www")
        shutil.copytree(base.FIXTURE_DIR, self.www)

    def tearDown(self):
        shutil.rmtree(self.work_dir, ignore_errors=True)

    def run_once(self, port, log_name):
        log_path = os.path.join(self.work_dir, log_name)
        proc, log = start_server(port, self.www, log_path)
        try:
            base.wait_until_listening(proc, port, log_path)
            return get_until_server_closes(port, "/home.html")
        finally:
            stop_server(proc, log)

    def test_요청을_처리한_서버를_끄고_바로_같은_포트로_다시_켜면_200을_준다(self):
        port = base.find_free_port()

        first = self.run_once(port, "first.log")
        self.assertTrue(first.startswith(b"HTTP/1.0 200"), first[:80])

        second = self.run_once(port, "second.log")
        self.assertTrue(second.startswith(b"HTTP/1.0 200"), second[:80])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
