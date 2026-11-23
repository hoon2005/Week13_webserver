"""정적 GET과 요청 검사(M2)를 확인하는 기능 테스트입니다.

사용법: python3 static_test.py <build 폴더>

임시 폴더에 tests/fixtures/www/ 내용과 테스트용 파일(빈 파일, 읽기 권한 없는
파일, 하위 폴더 등)을 만들고 서버(build/<SERVER_NAME>)를 빈 포트로 띄웁니다. 응답 없이 연결을
닫아야 하는 요청을 보낸 뒤에는 같은 서버에 정상 GET을 보내 서버가 살아 있는지
확인합니다. 서버 실행과 대기에는 baseline_test.py의 도우미를 씁니다.
"""

import os
import shutil
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import unittest

import baseline_test as base

# 서버가 받는 줄 하나의 최대 길이('\n' 포함)입니다. src/request.h의 REQUEST_LINE_SIZE - 1.
LINE_MAX = 8191

# 응답 도중 끊기 테스트용 파일 크기입니다. 소켓 버퍼(보통 수 MB)보다 커야 서버가
# 아직 쓰고 있는 동안 연결이 끊깁니다.
BIG_FILE_SIZE = 32 * 1024 * 1024


class StaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        server = base.SERVER_PATH
        adder = os.path.join(base.BUILD_DIR, "adder")
        for path in (server, adder):
            if not os.access(path, os.X_OK):
                raise RuntimeError("실행 파일이 없습니다: %s (먼저 make)" % path)

        cls.work_dir = tempfile.mkdtemp(prefix="server-static-")
        cls.www = os.path.join(cls.work_dir, "www")
        shutil.copytree(base.FIXTURE_DIR, cls.www)
        os.makedirs(os.path.join(cls.www, "cgi-bin"))
        shutil.copy2(adder, os.path.join(cls.www, "cgi-bin", "adder"))
        os.makedirs(os.path.join(cls.www, "sub"))
        cls.write_file("sub/page.html", b"<p>sub</p>\n")
        cls.write_file("empty.txt", b"")
        cls.write_file("a..b.html", b"<p>dots</p>\n")
        cls.write_file("big.txt", b"x" * BIG_FILE_SIZE)
        cls.secret = cls.write_file("secret.html", b"<p>secret</p>\n")
        os.chmod(cls.secret, 0)

        cls.port = base.find_free_port()
        cls.log_path = os.path.join(cls.work_dir, "server.log")
        cls.log = open(cls.log_path, "w")
        cls.proc = subprocess.Popen(
            [server, str(cls.port)],
            cwd=cls.www,
            stdin=subprocess.DEVNULL,
            stdout=cls.log,
            stderr=subprocess.STDOUT,
        )
        try:
            base.wait_until_listening(cls.proc, cls.port, cls.log_path)
        except Exception:
            cls.stop_server()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.stop_server()

    @classmethod
    def stop_server(cls):
        if cls.proc.poll() is None:
            cls.proc.terminate()
            try:
                cls.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                cls.proc.kill()
                cls.proc.wait()
        cls.log.close()
        os.chmod(cls.secret, stat.S_IRUSR | stat.S_IWUSR)
        shutil.rmtree(cls.work_dir, ignore_errors=True)

    @classmethod
    def write_file(cls, name, data):
        path = os.path.join(cls.www, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def send_raw(self, data, half_close=False):
        """바이트를 그대로 보내고 서버가 닫을 때까지 읽은 응답 전체를 돌려줍니다.

        서버가 읽지 않은 데이터를 남기고 닫으면 연결이 리셋될 수 있습니다.
        그 경우도 "서버가 닫았다"로 보고 그때까지 받은 내용을 돌려줍니다.
        """
        chunks = []
        with socket.create_connection(
            (base.HOST, self.port), timeout=base.REQUEST_TIMEOUT_SEC
        ) as sock:
            try:
                sock.sendall(data)
                if half_close:
                    sock.shutdown(socket.SHUT_WR)
                while True:
                    chunk = sock.recv(65536)
                    if not chunk:
                        break
                    chunks.append(chunk)
            except (ConnectionResetError, BrokenPipeError):
                pass
        return b"".join(chunks)

    def get(self, path):
        """GET을 보내고 (상태 코드, 헤더 dict, 본문)을 돌려줍니다."""
        raw = self.send_raw(("GET %s HTTP/1.0\r\n\r\n" % path).encode())
        return parse_response(raw)

    def assert_server_alive(self):
        status, _, _ = self.get("/home.html")
        self.assertEqual(status, 200, "서버가 이후 요청에 200을 주지 않습니다")

    def fixture_bytes(self, name):
        with open(os.path.join(base.FIXTURE_DIR, name), "rb") as f:
            return f.read()

    # 정적 파일 응답

    def test_html_파일은_text_html과_실제_크기의_Content_Length로_온다(self):
        status, headers, body = self.get("/home.html")
        expected = self.fixture_bytes("home.html")
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("content-type"), "text/html")
        self.assertEqual(headers.get("content-length"), str(len(expected)))
        self.assertEqual(body, expected)

    def test_gif_이미지는_image_gif로_원본과_같은_바이트가_온다(self):
        status, headers, body = self.get("/godzilla.gif")
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("content-type"), "image/gif")
        self.assertEqual(body, self.fixture_bytes("godzilla.gif"))

    def test_jpg_이미지는_image_jpeg로_원본과_같은_바이트가_온다(self):
        status, headers, body = self.get("/godzilla.jpg")
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("content-type"), "image/jpeg")
        self.assertEqual(body, self.fixture_bytes("godzilla.jpg"))

    def test_빈_파일은_200과_Content_Length_0으로_온다(self):
        status, headers, body = self.get("/empty.txt")
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("content-length"), "0")
        self.assertEqual(body, b"")
        self.assert_server_alive()

    def test_정적_파일_요청의_쿼리는_무시된다(self):
        status, _, body = self.get("/home.html?x=1")
        self.assertEqual(status, 200)
        self.assertEqual(body, self.fixture_bytes("home.html"))

    def test_이름에_점_두_개가_들어간_파일은_200이다(self):
        status, _, _ = self.get("/a..b.html")
        self.assertEqual(status, 200)

    def test_없는_파일은_404다(self):
        status, _, _ = self.get("/sub/nope.html")
        self.assertEqual(status, 404)

    def test_폴더는_403이다(self):
        status, _, _ = self.get("/sub")
        self.assertEqual(status, 403)

    @unittest.skipIf(
        hasattr(os, "geteuid") and os.geteuid() == 0,
        "root는 권한 없는 파일도 읽을 수 있습니다",
    )
    def test_읽기_권한_없는_파일은_403이다(self):
        status, _, _ = self.get("/secret.html")
        self.assertEqual(status, 403)

    def test_큰_파일을_받다가_손님이_끊어도_서버는_살아_있다(self):
        with socket.create_connection(
            (base.HOST, self.port), timeout=base.REQUEST_TIMEOUT_SEC
        ) as sock:
            sock.sendall(b"GET /big.txt HTTP/1.0\r\n\r\n")
            self.assertTrue(sock.recv(4096).startswith(b"HTTP/1.0 200"))
            # 닫을 때 FIN 대신 RST를 보내게 해서, 서버의 다음 write가 바로 실패하게 합니다.
            sock.setsockopt(
                socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0)
            )
        self.assert_server_alive()

    def test_요청만_보내고_바로_닫은_손님에게_쓰다가_SIGPIPE가_나도_서버는_살아_있다(self):
        # 손님이 먼저 FIN을 보내면 서버 소켓은 CLOSE_WAIT이 됩니다. 서버의 첫 write에
        # 손님 커널이 RST로 답하고, 그 뒤의 write는 EPIPE와 함께 SIGPIPE를 받습니다.
        # SIGPIPE를 무시하지 않으면 여기서 서버가 죽습니다.
        with socket.create_connection(
            (base.HOST, self.port), timeout=base.REQUEST_TIMEOUT_SEC
        ) as sock:
            sock.sendall(b"GET /big.txt HTTP/1.0\r\n\r\n")
        self.assert_server_alive()

    # 경로 규칙 (Q17, Q18, Q27)

    def test_구간이_점이거나_비었거나_상위_이동이면_403이다(self):
        for path in (
            "/../home.html",
            "/sub/../home.html",
            "/./home.html",
            "//home.html",
            "/sub//page.html",
            "/sub/.",
            "home.html",
        ):
            with self.subTest(path=path):
                status, _, _ = self.get(path)
                self.assertEqual(status, 403)
        self.assert_server_alive()

    def test_다른_표기의_cgi_bin은_정적_파일로_내려가지_않고_403이다(self):
        for path in (
            "//cgi-bin/adder",
            "/./cgi-bin/adder",
            "/CGI-BIN/adder",
            "/Cgi-Bin/adder?1&2",
        ):
            with self.subTest(path=path):
                status, _, _ = self.get(path)
                self.assertEqual(status, 403)

    # 해석할 수 없거나 너무 긴 요청 (Q2, Q19)

    def test_요청_라인이_정확히_최대_길이면_처리한다(self):
        line = request_line_of_length(LINE_MAX)
        raw = self.send_raw(line + b"\r\n\r\n")
        self.assertTrue(raw.startswith(b"HTTP/1.0 404"), raw[:40])

    def test_요청_라인이_최대_길이를_넘으면_응답_없이_닫고_서버는_살아_있다(self):
        line = request_line_of_length(LINE_MAX + 1)
        self.assertEqual(self.send_raw(line + b"\r\n"), b"")
        self.assert_server_alive()

    def test_헤더_줄이_최대_길이를_넘으면_응답_없이_닫고_서버는_살아_있다(self):
        header = b"X-Long: " + b"a" * LINE_MAX + b"\r\n"
        raw = self.send_raw(b"GET /home.html HTTP/1.0\r\n" + header + b"\r\n")
        self.assertEqual(raw, b"")
        self.assert_server_alive()

    def test_토큰이_셋이_아닌_요청_라인은_응답_없이_닫고_서버는_살아_있다(self):
        for line in (
            b"GARBAGE\r\n",
            b"GET /home.html\r\n",
            b"GET /home.html HTTP/1.0 extra\r\n",
            b"\r\n",
        ):
            with self.subTest(line=line):
                self.assertEqual(self.send_raw(line + b"\r\n"), b"")
        self.assert_server_alive()

    def test_줄바꿈_없이_끊긴_요청_라인은_응답_없이_닫는다(self):
        raw = self.send_raw(b"GET /home.html HTTP/1.0", half_close=True)
        self.assertEqual(raw, b"")
        self.assert_server_alive()

    def test_빈_줄_전에_끊긴_헤더는_응답_없이_닫는다(self):
        raw = self.send_raw(
            b"GET /home.html HTTP/1.0\r\nHost: x\r\n", half_close=True
        )
        self.assertEqual(raw, b"")
        self.assert_server_alive()

    def test_LF만으로_끝나는_요청도_처리한다(self):
        raw = self.send_raw(b"GET /home.html HTTP/1.0\nHost: x\n\n")
        self.assertTrue(raw.startswith(b"HTTP/1.0 200"), raw[:40])


def request_line_of_length(total):
    """'\\r\\n'까지 포함해 total바이트가 되는 GET 요청 라인을 만듭니다('\\r\\n' 제외)."""
    fixed = len(b"GET / HTTP/1.0\r\n")
    return b"GET /" + b"a" * (total - fixed) + b" HTTP/1.0"


def parse_response(raw):
    head, sep, body = raw.partition(b"\r\n\r\n")
    if not sep:
        raise AssertionError("응답에 헤더 끝이 없습니다: %r" % raw[:80])
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split(" ")[1])
    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(":")
        headers[name.strip().lower()] = value.strip()
    return status, headers, body


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
