"""테스트 출력을 정리하고 필요할 때 원본 로그를 저장합니다."""

import argparse
from collections import Counter
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
from shutil import get_terminal_size
import subprocess
import sys
from unicodedata import combining, east_asian_width


RAW_OUTPUT_ENV = "W13_RAW_TEST_OUTPUT"
SCENARIO_FILE = Path(__file__).with_name("test_scenarios.json")
SUITE_TITLES = {
    "conn_queue_test": "[단위 테스트] 작업 큐",
    "http_test": "[단위 테스트] HTTP 파서",
    "proxy_test": "[단위 테스트] 프록시",
    "baseline_test.py": "[기능 테스트] 기본 요청·응답",
    "m6m7_test.py": "[기능 테스트] 프록시와 서버 종료",
    "m8fix_test.py": "[기능 테스트] M8 수정 항목",
    "phase2_test.py": "[기능 테스트] HTTP·CGI·동시 연결",
    "scenario_test.py": "[기능 테스트] 서버 시나리오",
    "server_tsan.py": "[TSan] 동시 요청과 서버 종료",
}
CHECK_TITLES = {
    "verify": "개발 검증", "tsan": "TSan 검증",
    "unit": "단위 테스트", "functional": "기능 테스트",
}
PY_TEST = re.compile(
    r"^(?P<name>test_\w+) \([^\n]+?\)(?:\n[^\n]+)? \.\.\. "
    r"(?P<result>ok|FAIL|ERROR|skipped(?: .*)?|expected failure|unexpected success)$"
)
PY_TEST_HEADER = re.compile(r"^test_\w+ \([^\n]+\)$")
C_TEST = re.compile(r"^Test (.*?)\s+\[\s*(OK|FAILED|SKIPPED)\s*\]")
CASE_LINE = re.compile(r"^\S.* : .*\[(?:통과|실패|건너뜀|수동 확인)\]$")
REPEATED_SUCCESS = re.compile(
    r"^(?:SUCCESS: No unit tests have failed\.|Ran \d+ tests? in [\d.]+s|OK(?: \([^)]+\))?|[-]{70})$"
)


def line_text(line):
    return line.decode("utf-8", errors="replace").replace("\r\n", "\n").rstrip("\r\n")


def group_test_lines(lines):
    pending = None
    for line in lines:
        if pending is not None:
            pair = pending + line
            if PY_TEST.match(line_text(pair)):
                yield pair
                pending = None
                continue
            yield pending
            pending = None
        if PY_TEST_HEADER.match(line_text(line)):
            pending = line
        else:
            yield line
    if pending is not None:
        yield pending


def test_result(line):
    match = PY_TEST.match(line) or C_TEST.match(line)
    if not match:
        return None
    name, result = match.groups()
    if result.startswith("skipped") or result == "SKIPPED":
        status = "건너뜀" + result[len("skipped"):]
    elif result in ("ok", "OK"):
        status = "통과"
    elif result == "expected failure":
        status = "예상 실패"
    else:
        status = "실패"
    return name.rstrip("."), status


def screen_width(text):
    return sum(
        2 if east_asian_width(char) in ("W", "F") else 1
        for char in text
        if not combining(char)
    )


def shorten(text, limit):
    if screen_width(text) <= limit:
        return text
    used = 0
    for index, char in enumerate(text):
        used += screen_width(char)
        if used >= limit:
            return text[:index] + "…"
    return text


def use_color():
    return (
        sys.stdout.isatty() and "NO_COLOR" not in os.environ
        and os.environ.get("TERM") != "dumb"
    )


def print_suite(title):
    """make verify와 같은 묶음 제목. stress, soak, load가 씁니다."""
    print(f"\n{title}", flush=True)


def print_case(name, text, status):
    """make verify와 같은 결과 한 줄. 측정값이 들어가므로 길어도 자르지 않습니다."""
    column = max(16, get_terminal_size((120, 24)).columns - 13)
    padding = " " * max(0, 24 - screen_width(name))
    gap = " " * max(2, column - screen_width(name + padding + " : " + text))
    shown = f"\x1b[96m{name}\x1b[0m" if use_color() else name
    print(f"{shown}{padding} : {text}{gap}[{status}]", flush=True)


class TerminalTests:
    def __init__(self, suite=""):
        self.suite = Path(suite).name
        self.scenarios = json.loads(SCENARIO_FILE.read_text(encoding="utf-8"))
        self.status_column = max(16, get_terminal_size((120, 24)).columns - 13)
        self.color = use_color()
        self.raw = os.environ.get(RAW_OUTPUT_ENV) == "1"
        self.counts = Counter()

    def write(self, line):
        text = line_text(line)
        result = test_result(text)
        if text.startswith("== ") and len(text.split()) > 1:
            self.suite = Path(text.split()[1]).name
            if not self.raw and self.suite in SUITE_TITLES:
                line = f"\n{SUITE_TITLES[self.suite]}\n".encode("utf-8")
        elif result:
            name, status = result
            if status.startswith("건너뜀"):
                reason = status[len("건너뜀"):]
                status = "건너뜀"
            else:
                reason = ""
            self.counts[status] += 1
            if not self.raw:
                line = self.format_case(name, status, reason)
        elif self.color and CASE_LINE.match(text):
            # run_check로 돌린 stress, soak, load의 줄. 로그에는 색 없이 남습니다.
            name = text.split(" : ", 1)[0].rstrip()
            line = f"\x1b[96m{name}\x1b[0m{text[len(name):]}\n".encode("utf-8")
        elif not self.raw and self.suite and REPEATED_SUCCESS.match(text):
            return
        sys.stdout.buffer.write(line)
        sys.stdout.buffer.flush()

    def format_case(self, name, status, reason):
        name, scenario = self.scenarios.get(self.suite, {}).get(
            name, (name, "시나리오 설명 없음")
        )
        text_width = self.status_column - 2
        scenario_room = min(18, text_width // 3)
        shown_name = shorten(name, text_width - scenario_room - 3)
        name_padding = " " * max(0, min(24, text_width // 3) - screen_width(shown_name))
        prefix = shown_name + name_padding + " : "
        shown_scenario = shorten(scenario + reason, text_width - screen_width(prefix))
        gap = self.status_column - screen_width(prefix + shown_scenario)
        if self.color:
            shown_name = "\x1b[96m" + shown_name + "\x1b[0m"
        return f"{shown_name}{name_padding} : {shown_scenario}{' ' * gap}[{status}]\n".encode(
            "utf-8"
        )


def stream_command(command, terminal, env=None, log=None):
    with subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env
    ) as process:
        for line in group_test_lines(process.stdout):
            if log is not None:
                log.write(line)
            terminal.write(line)
        code = process.wait()
    return 128 - code if code < 0 else code


def run_logged(log_dir, check, command):
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", check):
        raise ValueError("로그 이름에는 영문 소문자·숫자·하이픈을 사용하세요.")
    log_dir.mkdir(parents=True, exist_ok=True)
    started = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%fZ")
    path = log_dir / f"{check}-{started}.log"
    suite = next((Path(arg).name for arg in command if arg.endswith("_test.py")), "")
    terminal = TerminalTests(suite)
    with path.open("xb") as log:
        try:
            return stream_command(
                command, terminal, log=log,
                env=dict(os.environ, **{
                    RAW_OUTPUT_ENV: "1",
                    "COLUMNS": str(get_terminal_size((120, 24)).columns),
                }),
            )
        except OSError as error:
            message = f"명령 실행 실패: {error}\n"
            log.write(message.encode("utf-8"))
            print(message, end="", file=sys.stderr)
            return 127
        except KeyboardInterrupt:
            return 130
        finally:
            print(f"원본 로그: {path.resolve()}")


def count_result(label, counts):
    if not counts:
        return f"{label} 건너뜀 (대상 없음)"
    result = f"{label} {counts['통과']}개 통과"
    for name in ("건너뜀", "예상 실패"):
        if counts[name]:
            result += f", {counts[name]}개 {name}"
    return result


def run_checks(args):
    terminal = TerminalTests()
    summary = []
    for label, paths in (("단위", args.unit), ("기능", args.functional)):
        if paths is None:
            continue
        terminal.counts.clear()
        for path in paths:
            prefix = f"== {path}\n".encode("utf-8")
            terminal.write(prefix)
            if label == "단위":
                command = [os.path.join(".", path)]
                env = dict(os.environ, TSAN_OPTIONS="exitcode=1") if args.kind == "tsan" else None
            else:
                command = [sys.executable, "-B", path, args.build]
                env = None
            code = stream_command(command, terminal, env=env)
            if code:
                print(f"[실패] {path}", file=sys.stderr)
                return code
        summary.append(count_result("서버" if args.kind == "tsan" and label == "기능" else label,
                                    terminal.counts))
    if args.format is not None:
        if args.format:
            if subprocess.call(["clang-format", "--dry-run", "--Werror", *args.format]):
                print("[실패] 코드 포맷", file=sys.stderr)
                return 1
            summary.append("코드 포맷 통과")
        else:
            summary.append("코드 포맷 건너뜀 (대상 없음)")
    print(f"\n[{CHECK_TITLES[args.kind]} 완료] " + " · ".join(summary))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    execute = actions.add_parser("run")
    execute.add_argument("log_dir", type=Path)
    execute.add_argument("check")
    execute.add_argument("command", nargs=argparse.REMAINDER)
    check = actions.add_parser("check")
    check.add_argument("kind", choices=CHECK_TITLES)
    check.add_argument("build")
    check.add_argument("--unit", nargs="*")
    check.add_argument("--functional", nargs="*")
    check.add_argument("--format", nargs="*")
    args = parser.parse_args()
    try:
        if args.action == "run":
            if not args.command:
                execute.error("실행할 테스트 명령을 입력하세요.")
            return run_logged(args.log_dir, args.check, args.command)
        return run_checks(args)
    except (OSError, ValueError) as error:
        print(f"[실패] {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
