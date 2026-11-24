"""서버를 끌 때 제대로 끝났는지도 확인하는 도우미입니다. 테스트 파일이 아닙니다.

phase2_test.py의 Server는 끄기만 하고 결과를 보지 않습니다. 그래서 종료하다가 생긴
누수나 메모리 오류가 조용히 지나갑니다. CheckedServer는 정리할 때 SIGTERM을 보내고
종료 코드와 로그를 확인합니다. 로그는 도우미가 지우기 전에 읽습니다.

누수는 누수 검사가 켜진 빌드(Linux의 ASan 개발 빌드)에서만 드러납니다.
"""

import subprocess

from phase2_test import Server

EXIT_SEC = 8  # 이보다 오래 걸리면 실패


class CheckedServer(Server):
    started = False

    def __init__(self, *args):
        self.expected_code = 0  # 강제로 끝내는 테스트는 기대하는 코드로 바꿈
        super().__init__(*args)
        self.started = True

    def read_log(self):
        with open(self.log_path, errors="replace") as f:
            return f.read()

    def stop(self):
        if not self.started:  # 시작에 실패한 서버는 종료를 확인하지 않음
            super().stop()
            return
        if self.alive():
            self.proc.terminate()
        try:
            code = self.proc.wait(timeout=EXIT_SEC)
        except subprocess.TimeoutExpired:
            code = None
        log = self.read_log()
        super().stop()
        assert code == self.expected_code, "종료 코드 %s (기대 %s)\n%s" % (
            "없음: %d초 안에 끝나지 않음" % EXIT_SEC if code is None else code,
            self.expected_code, log)
        for word in ("Sanitizer", "runtime error"):
            assert word not in log, log
