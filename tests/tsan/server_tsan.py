"""conn-tsan-server: ThreadSanitizer로 빌드한 서버 전체에 데이터 경합이 없는지 봅니다.

사용법: python3 server_tsan.py <tsan build 폴더>   (make tsan이 실행)

TSan으로 빌드한 tiny를 프록시와 백엔드로 띄우고, 정적·CGI·중계 요청을 동시에 보낸 뒤
SIGTERM으로 끕니다. 경합이 하나라도 보고되면 종료 코드가 0이 아니고 로그에 남습니다.
"""

import os
import signal
import sys
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                                "functional"))
from m6m7_test import Server  # noqa: E402  (BUILD_DIR은 sys.argv[1]에서 읽음)

# 경고가 나도 끝까지 돌고, 끝날 때 종료 코드로 알림
ENV = dict(os.environ, TSAN_OPTIONS="halt_on_error=0 exitcode=66")


class TsanServerTest(unittest.TestCase):
    def test_conn_tsan_server(self):
        backend = Server(env=ENV)
        os.makedirs(os.path.join(backend.www, "api"))
        with open(os.path.join(backend.www, "api", "home.html"), "w") as f:
            f.write("<p>backend</p>")
        proxy = Server("--route", "/api=127.0.0.1:%d" % backend.port, env=ENV)
        paths = ("/home.html", "/cgi-bin/adder?1&2", "/api/home.html")
        results = []
        lock = threading.Lock()

        def worker(i):
            for j in range(6):
                try:
                    status = proxy.get(paths[(i + j) % 3])[0]
                except OSError as e:
                    status = repr(e)
                with lock:
                    results.append(status)

        try:
            threads = [threading.Thread(target=worker, args=(i,)) for i in range(16)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            self.assertEqual(results, [200] * len(results))
            for server in (proxy, backend):
                server.proc.send_signal(signal.SIGTERM)
                code = server.wait_exit(30)
                log = server.read_log()
                self.assertNotIn("ThreadSanitizer", log, log[-3000:])
                self.assertEqual(code, 0, log[-3000:])
        finally:
            proxy.cleanup()
            backend.cleanup()


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
