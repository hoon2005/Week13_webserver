"""부하 측정 (시나리오 표 load-a-*, load-b-threads, load-keepalive-off, load-tool-cpu).

make verify에는 들어가지 않습니다. wrk, ab(apache2-utils)가 있어야 하고, 전부 돌리면
기본 설정으로 30분 정도 걸립니다.

사용법:
    python3 tests/stress/load.py                 # 전부 (A 4종 -> B -> keepalive-off). release 빌드는 스크립트가 직접 최신화
    python3 tests/stress/load.py --quick         # 흐름만 확인 (3초, 1회, 워밍업 1초)
    python3 tests/stress/load.py static-small    # 하나만
    python3 tests/stress/load.py threads --sat-conns 32

측정 이름 (시나리오 표의 행)
    static-small   load-a-static-small  wrk, home.html, -c 1 2 4 ... 256
    static-large   load-a-static-large  wrk, 1 MiB 파일, 같은 조건
    cgi            load-a-cgi           wrk, /cgi-bin/adder?1&2, 같은 조건
    proxy          load-a-proxy         wrk, /api/home.html -> 백엔드(우리 서버) 2개
    threads        load-b-threads       wrk, -c는 static-small의 포화 지점, --threads 1 4 8 16 32
    keepalive-off  load-keepalive-off   ab(-k 없음), home.html, A와 같은 -c
    load-tool-cpu는 따로 돌지 않고, 위 측정마다 서버와 부하 도구의 CPU 사용률을 같이 기록합니다.

조건: release 빌드, 서버 --threads 16(B 제외), 측정마다 워밍업 1번 + 3회 중간값.

판정과 기록
    오류 0         wrk의 소켓 오류(connect, read, write, timeout)와 2xx/3xx가 아닌 응답,
                   ab의 Failed requests와 Non-2xx responses를 모두 더한 값이 0
    중간값         3회 중 초당 처리 건수가 가운데인 회차의 처리량, p50, p99를 기록
    포화 지점      초당 처리 건수가 최댓값의 95%에 처음 닿는 -c
    도구 한계      부하 도구가 자기 스레드 수만큼의 코어를 90% 이상 쓰면, 그 구간의 숫자는
                   서버 한계가 아니라 도구 한계로 봄 (load-tool-cpu)
    CPU 공유       서버와 도구를 합친 CPU 사용률이 전체 코어의 90% 이상이면 표시.
                   같은 컴퓨터에서 재므로 서로 CPU를 뺏는 구간이라는 뜻

결과는 build/load/<시각>/ 아래 results.json, report.md(시트에 옮길 표), report.html(그래프)
로 남습니다. 오류가 하나라도 있으면 종료 코드 1입니다.
"""

import argparse
import datetime
import glob
import html
import json
import os
import re
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from test_output import print_case, print_suite  # noqa: E402

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FIXTURE_DIR = os.path.join(REPO_ROOT, "tests", "fixtures", "www")
RELEASE_DIR = os.path.join(REPO_ROOT, "build", "release")
OUT_ROOT = os.path.join(REPO_ROOT, "build", "load")
HOST = "127.0.0.1"
CLK_TCK = os.sysconf("SC_CLK_TCK")
NCPU = os.cpu_count() or 1

DEFAULT_CONNS = [1, 2, 4, 8, 16, 32, 64, 128, 256]
DEFAULT_THREADS = [1, 4, 8, 16, 32]
SERVER_THREADS = 16

# 측정 이름 -> (시나리오 표 ID, 설명, 요청 경로)
A_TARGETS = {
    "static-small": ("load-a-static-small", "작은 정적 파일 (home.html)", "/home.html"),
    "static-large": ("load-a-static-large", "큰 정적 파일 (1 MiB)", "/big.bin"),
    "cgi": ("load-a-cgi", "CGI (/cgi-bin/adder?1&2)", "/cgi-bin/adder?1&2"),
    "proxy": ("load-a-proxy", "중계 (/api -> 백엔드 2개)", "/api/home.html"),
}
ALL_ORDER = ["static-small", "static-large", "cgi", "proxy", "threads", "keepalive-off"]


# ============================== 서버 ==============================

def free_port():
    with socket.socket() as s:
        s.bind((HOST, 0))
        return s.getsockname()[1]


def make_www(work):
    www = tempfile.mkdtemp(prefix="www-", dir=work)
    shutil.rmtree(www)
    shutil.copytree(FIXTURE_DIR, www)
    shutil.copy2(os.path.join(RELEASE_DIR, "adder"), os.path.join(www, "cgi-bin", "adder"))
    for name in os.listdir(os.path.join(www, "cgi-bin")):
        os.chmod(os.path.join(www, "cgi-bin", name), 0o755)
    with open(os.path.join(www, "big.bin"), "wb") as f:
        f.write(os.urandom(1024 * 1024))
    os.makedirs(os.path.join(www, "api"))
    shutil.copy2(os.path.join(www, "home.html"), os.path.join(www, "api", "home.html"))
    return www


class Tiny:
    def __init__(self, work, *args):
        self.port = free_port()
        self.log_path = os.path.join(work, "tiny-%d.log" % self.port)
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen(
            [os.path.join(RELEASE_DIR, "tiny"), str(self.port), "--root", make_www(work)]
            + [str(a) for a in args],
            stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                break
            try:
                socket.create_connection((HOST, self.port), timeout=0.3).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("tiny가 뜨지 않았습니다 (로그: %s)" % self.log_path)

    @property
    def pid(self):
        return self.proc.pid

    def alive(self):
        return self.proc.poll() is None

    def stop(self):
        if self.alive():
            self.proc.send_signal(signal.SIGTERM)
        try:
            code = self.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            code = self.proc.wait()
        self.log.close()
        return code


class Setup:
    """측정 하나에 필요한 서버들. front가 부하를 받는 서버"""

    def __init__(self, work, threads=SERVER_THREADS, proxy=False):
        self.servers = []
        if proxy:
            backends = [Tiny(work, "--threads", threads) for _ in range(2)]
            self.servers += backends
            route = "/api=" + ",".join("%s:%d" % (HOST, b.port) for b in backends)
            self.front = Tiny(work, "--threads", threads, "--route", route)
        else:
            self.front = Tiny(work, "--threads", threads)
        self.servers.insert(0, self.front)

    def url(self, path):
        return "http://%s:%d%s" % (HOST, self.front.port, path)

    def pids(self):
        return [s.pid for s in self.servers]

    def alive(self):
        return all(s.alive() for s in self.servers)

    def stop(self):
        return [s.stop() for s in self.servers]


# ============================== CPU (load-tool-cpu) ==============================

def cpu_ticks(pid, children=False):
    """/proc/<pid>/stat의 utime+stime (children이면 기다려 준 자식 것까지)"""
    try:
        with open("/proc/%d/stat" % pid) as f:
            fields = f.read().rsplit(")", 1)[1].split()
    except OSError:
        return None
    # ")" 뒤 필드는 3번(state)부터. utime=14, stime=15, cutime=16, cstime=17
    total = int(fields[11]) + int(fields[12])
    if children:
        total += int(fields[13]) + int(fields[14])
    return total


def run_tool(cmd, server_pids):
    """부하 도구를 돌리고 (출력, 서버 CPU%, 도구 CPU%)를 돌려줌. 100% = 코어 하나"""
    before = {p: cpu_ticks(p, children=True) for p in server_pids}
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    tool_ticks = 0
    # 도구가 끝나면 /proc 항목이 사라지므로, 도는 동안 마지막 값을 계속 읽어 둠
    while proc.poll() is None:
        t = cpu_ticks(proc.pid)
        if t is not None:
            tool_ticks = t
        time.sleep(0.2)
    out = proc.stdout.read()
    elapsed = time.monotonic() - t0
    server = 0
    for p in server_pids:
        after = cpu_ticks(p, children=True)
        if after is not None and before[p] is not None:
            server += after - before[p]
    to_pct = lambda ticks: round(100.0 * ticks / CLK_TCK / elapsed, 1)
    return out, proc.returncode, to_pct(server), to_pct(tool_ticks)


# ============================== wrk, ab 출력 해석 ==============================

TIME_UNITS = {"us": 0.001, "ms": 1.0, "s": 1000.0, "m": 60000.0}
SIZE_UNITS = {"B": 1, "KB": 1024, "MB": 1024 ** 2, "GB": 1024 ** 3, "TB": 1024 ** 4}


def ms(text):
    m = re.fullmatch(r"([\d.]+)(us|ms|s|m)", text)
    return float(m.group(1)) * TIME_UNITS[m.group(2)]


def parse_wrk(out):
    r = {}
    m = re.search(r"Requests/sec:\s+([\d.]+)", out)
    if not m:
        raise ValueError("wrk 출력을 읽지 못했습니다:\n" + out)
    r["rps"] = float(m.group(1))
    m = re.search(r"Transfer/sec:\s+([\d.]+)(\w+)", out)
    r["mib_per_sec"] = round(float(m.group(1)) * SIZE_UNITS[m.group(2)] / 1024 ** 2, 2) if m else None
    for pct in ("50", "99"):
        m = re.search(r"^\s+%s(?:\.0+)?%%\s+([\d.]+\w+)" % pct, out, re.M)
        r["p" + pct] = round(ms(m.group(1)), 3) if m else None
    errors = 0
    m = re.search(r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)", out)
    if m:
        errors += sum(int(x) for x in m.groups())
    m = re.search(r"Non-2xx or 3xx responses: (\d+)", out)
    if m:
        errors += int(m.group(1))
    r["errors"] = errors
    return r


def parse_ab(out):
    r = {}
    m = re.search(r"Requests per second:\s+([\d.]+)", out)
    if not m:
        raise ValueError("ab 출력을 읽지 못했습니다:\n" + out)
    r["rps"] = float(m.group(1))
    m = re.search(r"Transfer rate:\s+([\d.]+) \[Kbytes/sec\]", out)
    r["mib_per_sec"] = round(float(m.group(1)) / 1024, 2) if m else None
    for pct in ("50", "99"):
        m = re.search(r"^\s+%s%%\s+(\d+)" % pct, out, re.M)
        r["p" + pct] = float(m.group(1)) if m else None
    errors = 0
    for pat in (r"Failed requests:\s+(\d+)", r"Non-2xx responses:\s+(\d+)"):
        m = re.search(pat, out)
        if m:
            errors += int(m.group(1))
    r["errors"] = errors
    return r


# ============================== 측정 ==============================

class Runner:
    def __init__(self, args, work):
        self.args = args
        self.work = work

    def wrk_cmd(self, url, conns, duration):
        threads = max(1, min(conns, self.args.wrk_threads))
        return threads, ["wrk", "-t", str(threads), "-c", str(conns), "-d", "%ds" % duration,
                         "--timeout", "%ds" % self.args.tool_timeout, "--latency", url]

    def ab_cmd(self, url, conns, duration):
        # -t만 주면 요청 수가 5만으로 묶이므로 -n을 뒤에 넉넉히 줌 (ab는 -n 크기만큼 메모리를 잡음)
        n = max(50000, duration * 100000, conns)
        return 1, ["ab", "-q", "-r", "-c", str(conns), "-t", str(duration), "-n", str(n),
                   "-s", str(self.args.tool_timeout), url]

    def point(self, setup, url, conns, tool, label=None):
        """한 -c 값: 워밍업 1번 + runs번, 초당 처리 건수 중간값 회차를 대표로"""
        make = self.wrk_cmd if tool == "wrk" else self.ab_cmd
        parse = parse_wrk if tool == "wrk" else parse_ab
        if self.args.warmup > 0:
            _, cmd = make(url, conns, self.args.warmup)
            run_tool(cmd, setup.pids())
        runs = []
        for _ in range(self.args.runs):
            tool_threads, cmd = make(url, conns, self.args.duration)
            out, code, server_cpu, tool_cpu = run_tool(cmd, setup.pids())
            r = parse(out)
            r["server_cpu"] = server_cpu
            r["tool_cpu"] = tool_cpu
            r["tool_threads"] = tool_threads
            runs.append(r)
            if not setup.alive():
                raise RuntimeError("측정 도중 서버가 죽었습니다")
        mid = sorted(runs, key=lambda r: r["rps"])[len(runs) // 2]
        result = dict(mid)
        result["conns"] = conns
        result["errors"] = sum(r["errors"] for r in runs)
        result["rps_runs"] = [round(r["rps"], 1) for r in runs]
        result["tool_limited"] = mid["tool_cpu"] >= 90 * mid["tool_threads"]
        result["cpu_shared"] = mid["server_cpu"] + mid["tool_cpu"] >= 90 * NCPU
        text = "%10.1f req/s  p50 %8.3fms  p99 %8.3fms  오류 %d  CPU 서버 %5.1f%% 도구 %5.1f%%%s" % (
            result["rps"], result["p50"] or 0, result["p99"] or 0, result["errors"],
            result["server_cpu"], result["tool_cpu"],
            "  도구 한계" if result["tool_limited"] else "")
        print_case(label or "-c %d" % conns, text, "실패" if result["errors"] else "통과")
        return result

    def series_a(self, name, tool="wrk"):
        sid, desc, path = A_TARGETS[name]
        setup = Setup(self.work, proxy=(name == "proxy"))
        try:
            points = [self.point(setup, setup.url(path), c, tool) for c in self.args.conns]
        finally:
            codes = setup.stop()
        return {"points": points, "stop_codes": codes}

    def series_b(self, sat):
        points = []
        codes = []
        for t in self.args.threads_list:
            setup = Setup(self.work, threads=t)
            try:
                p = self.point(setup, setup.url("/home.html"), sat, "wrk", label="-t %d" % t)
            finally:
                codes += setup.stop()
            p["threads"] = t
            points.append(p)
        return {"points": points, "stop_codes": codes, "conns": sat}


def saturation(points):
    best = max(p["rps"] for p in points)
    for p in points:
        if p["rps"] >= 0.95 * best:
            return p["conns"]
    return points[-1]["conns"]


def previous_saturation():
    for path in sorted(glob.glob(os.path.join(OUT_ROOT, "*", "results.json")), reverse=True):
        with open(path) as f:
            data = json.load(f)
        s = data.get("measurements", {}).get("static-small")
        if s and "saturation" in s:
            return s["saturation"], path
    return None, None


# ============================== 보고서 ==============================

def fmt(v, digits=1):
    return "-" if v is None else ("%.*f" % (digits, v))


def notes(p):
    out = []
    if p["tool_limited"]:
        out.append("도구 한계")
    if p["cpu_shared"]:
        out.append("CPU 공유")
    return ", ".join(out)


def write_md(path, data):
    lines = ["# 부하 측정 결과", "",
             "- 시각: %s" % data["started"],
             "- 커밋: %s" % data["commit"],
             "- 코어 수: %d, 측정 %d초 x %d회 (워밍업 %d초), wrk 스레드 %d" % (
                 NCPU, data["args"]["duration"], data["args"]["runs"], data["args"]["warmup"],
                 data["args"]["wrk_threads"]),
             "- 지연 시간 단위는 ms. ab의 p50, p99는 ms 정수로만 나옴", ""]
    for name, m in data["measurements"].items():
        lines += ["## %s: %s" % (m["id"], m["desc"]), "",
                  "판정: **%s** (오류 합계 %d, 서버 종료 코드 %s)" % (
                      "통과" if m["passed"] else "실패", m["errors"], m["stop_codes"]), ""]
        if "saturation" in m:
            lines += ["포화 지점: -c %d" % m["saturation"], ""]
        key = "threads" if name == "threads" else "conns"
        head = "--threads" if name == "threads" else "-c"
        lines += ["| %s | 초당 처리 건수 | 3회 | MiB/s | p50 | p99 | 오류 | 서버 CPU%% | 도구 CPU%% | 비고 |" % head,
                  "|---|---|---|---|---|---|---|---|---|---|"]
        for p in m["points"]:
            lines.append("| %d | %s | %s | %s | %s | %s | %d | %s | %s | %s |" % (
                p[key], fmt(p["rps"]), " / ".join(fmt(x) for x in p["rps_runs"]),
                fmt(p["mib_per_sec"], 2), fmt(p["p50"], 3), fmt(p["p99"], 3), p["errors"],
                fmt(p["server_cpu"]), fmt(p["tool_cpu"]), notes(p)))
        lines.append("")
    lines += ["## 시나리오 표에 옮길 실제 결과", ""]
    for name, m in data["measurements"].items():
        lines.append("- %s: %s" % (m["id"], summary_line(name, m)))
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def summary_line(name, m):
    pts = m["points"]
    best = max(pts, key=lambda p: p["rps"])
    failed_exits = [code for code in m["stop_codes"] if code != 0]
    verdict = "통과" if m["passed"] else "실패 (요청 오류 %d건)" % m["errors"]
    if failed_exits:
        verdict += ", 서버 종료 코드 %s" % failed_exits
    if name == "threads":
        return "%s. -c %d에서 --threads %d일 때 최고 %.0f req/s (p99 %sms)" % (
            verdict, m["conns"], best["threads"], best["rps"], fmt(best["p99"], 2))
    text = "%s. 최고 %.0f req/s (-c %d, p50 %sms, p99 %sms)" % (
        verdict, best["rps"], best["conns"], fmt(best["p50"], 2), fmt(best["p99"], 2))
    if "saturation" in m:
        text += ", 포화 지점 -c %d" % m["saturation"]
    if best["mib_per_sec"] and name == "static-large":
        text += ", 전송량 %.0f MiB/s" % best["mib_per_sec"]
    limited = [str(p.get("conns")) for p in pts if p["tool_limited"]]
    if limited:
        text += ", 도구 한계 구간 -c " + " ".join(limited)
    return text


COLORS = ["#2a6fdb", "#d9822b", "#1f9d55", "#c0392b", "#8e44ad"]


def svg_chart(title, xs, series, xlabel, ylabel, log_x=True):
    """간단한 꺾은선 그래프. series = [(이름, [y...])]"""
    w, h, ml, mr, mt, mb = 640, 300, 70, 140, 30, 45
    pw, ph = w - ml - mr, h - mt - mb
    import math
    fx = (lambda x: math.log2(x)) if log_x else (lambda x: x)
    x0, x1 = fx(xs[0]), fx(xs[-1])
    ymax = max(max(v for v in ys if v is not None) for _, ys in series) or 1
    sx = lambda x: ml + (pw * (fx(x) - x0) / (x1 - x0) if x1 > x0 else pw / 2)
    sy = lambda y: mt + ph - ph * y / (ymax * 1.1)
    parts = ['<svg viewBox="0 0 %d %d" width="%d" height="%d" font-family="sans-serif" font-size="12">' % (w, h, w, h),
             '<text x="%d" y="18" font-weight="bold">%s</text>' % (ml, html.escape(title))]
    for i in range(5):
        y = ymax * 1.1 * i / 4
        parts.append('<line x1="%d" x2="%d" y1="%.1f" y2="%.1f" stroke="#ddd"/>' % (ml, ml + pw, sy(y), sy(y)))
        parts.append('<text x="%d" y="%.1f" text-anchor="end">%.0f</text>' % (ml - 6, sy(y) + 4, y))
    for x in xs:
        parts.append('<text x="%.1f" y="%d" text-anchor="middle">%d</text>' % (sx(x), mt + ph + 16, x))
    parts.append('<text x="%d" y="%d" text-anchor="middle">%s</text>' % (ml + pw / 2, h - 6, html.escape(xlabel)))
    parts.append('<text x="14" y="%d" transform="rotate(-90 14 %d)" text-anchor="middle">%s</text>' % (
        mt + ph / 2, mt + ph / 2, html.escape(ylabel)))
    for i, (name, ys) in enumerate(series):
        c = COLORS[i % len(COLORS)]
        pts = " ".join("%.1f,%.1f" % (sx(x), sy(y)) for x, y in zip(xs, ys) if y is not None)
        parts.append('<polyline fill="none" stroke="%s" stroke-width="2" points="%s"/>' % (c, pts))
        for x, y in zip(xs, ys):
            if y is not None:
                parts.append('<circle cx="%.1f" cy="%.1f" r="3" fill="%s"/>' % (sx(x), sy(y), c))
        parts.append('<rect x="%d" y="%d" width="12" height="3" fill="%s"/>' % (ml + pw + 14, mt + 10 + 18 * i, c))
        parts.append('<text x="%d" y="%d">%s</text>' % (ml + pw + 30, mt + 15 + 18 * i, html.escape(name)))
    parts.append("</svg>")
    return "\n".join(parts)


def write_html(path, md_path, data):
    ms_ = data["measurements"]
    charts = []
    a_names = [n for n in ("static-small", "static-large", "cgi", "proxy", "keepalive-off") if n in ms_]
    if a_names:
        xs = data["args"]["conns"]
        label = lambda n: ms_[n]["id"].replace("load-", "")
        charts.append(svg_chart("동시 접속 수에 따른 초당 처리 건수", xs,
                                [(label(n), [p["rps"] for p in ms_[n]["points"]]) for n in a_names],
                                "-c (동시 접속 수)", "req/s"))
        charts.append(svg_chart("동시 접속 수에 따른 p99 지연 (ms)", xs,
                                [(label(n), [p["p99"] for p in ms_[n]["points"]]) for n in a_names],
                                "-c (동시 접속 수)", "ms"))
    if "threads" in ms_:
        b = ms_["threads"]
        xs = [p["threads"] for p in b["points"]]
        charts.append(svg_chart("작업 스레드 수에 따른 초당 처리 건수 (-c %d)" % b["conns"], xs,
                                [("req/s", [p["rps"] for p in b["points"]])], "--threads", "req/s"))
    with open(md_path) as f:
        md = f.read()
    body = ["<!DOCTYPE html><html lang='ko'><head><meta charset='utf-8'><title>부하 측정 결과</title>",
            "<style>body{font-family:sans-serif;max-width:1000px;margin:2em auto;padding:0 1em}"
            "table{border-collapse:collapse;font-size:13px}td,th{border:1px solid #ccc;padding:3px 8px;text-align:right}"
            "pre{background:#f6f6f6;padding:1em;overflow:auto}.pass{color:#1f9d55}.fail{color:#c0392b}</style></head><body>",
            "<h1>부하 측정 결과</h1><p>%s, 커밋 %s, 코어 %d개</p>" % (data["started"], data["commit"], NCPU)]
    body += ["<h2>판정</h2><ul>"]
    for n, m in ms_.items():
        body.append("<li class='%s'>%s: %s</li>" % ("pass" if m["passed"] else "fail", m["id"],
                                                    html.escape(summary_line(n, m))))
    body.append("</ul>")
    body += charts
    body += ["<h2>표 전체 (report.md)</h2><pre>%s</pre></body></html>" % html.escape(md)]
    with open(path, "w") as f:
        f.write("\n".join(body))


# ============================== main ==============================

def git_commit():
    try:
        return subprocess.run(["git", "-C", REPO_ROOT, "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True).stdout.strip() or "?"
    except OSError:
        return "?"


def int_list(text):
    return [int(x) for x in text.replace(",", " ").split()]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", choices=ALL_ORDER + ["all"], default="all",
                    metavar="측정", help="all(기본) 또는 " + " ".join(ALL_ORDER))
    ap.add_argument("--duration", type=int, default=10, help="한 번 측정하는 초 (기본 10)")
    ap.add_argument("--warmup", type=int, default=3, help="워밍업 초, 0이면 생략 (기본 3)")
    ap.add_argument("--runs", type=int, default=3, help="반복 횟수, 중간값을 씀 (기본 3)")
    ap.add_argument("--conns", type=int_list, default=DEFAULT_CONNS, help="-c 목록 (기본 1,2,4,...,256)")
    ap.add_argument("--threads-list", type=int_list, default=DEFAULT_THREADS, help="B의 --threads 목록")
    ap.add_argument("--sat-conns", type=int, help="B의 -c. 없으면 static-small 결과의 포화 지점")
    ap.add_argument("--wrk-threads", type=int, default=2, help="wrk -t (기본 2, -c보다 크면 -c로 줄임)")
    ap.add_argument("--tool-timeout", type=int, default=10, help="wrk/ab가 응답을 기다리는 초 (기본 10)")
    ap.add_argument("--quick", action="store_true", help="흐름 확인용: 3초, 1회, 워밍업 1초")
    args = ap.parse_args()
    if args.quick:
        args.duration, args.runs, args.warmup = 3, 1, 1

    if isinstance(args.names, str):
        args.names = [args.names]
    names = ALL_ORDER if "all" in args.names else [n for n in ALL_ORDER if n in args.names]
    need = {"wrk"} if any(n != "keepalive-off" for n in names) else set()
    if "keepalive-off" in names:
        need.add("ab")
    missing = [t for t in sorted(need) if shutil.which(t) is None]
    if missing:
        sys.exit("%s가 없습니다: sudo apt install wrk apache2-utils" % ", ".join(missing))
    # 소스가 바뀌었는데 예전 release 빌드로 재면 숫자가 틀림 (TCP_NODELAY가 없던 빌드는
    # keep-alive 요청마다 44ms를 기다려 연결당 초당 23건에 묶였음). 매번 make로 최신화
    built = subprocess.run(["make", "-C", REPO_ROOT, "release"], capture_output=True, text=True)
    if built.returncode != 0:
        sys.exit("make release 실패:\n" + built.stdout + built.stderr)

    started = datetime.datetime.now()
    out_dir = os.path.join(OUT_ROOT, started.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out_dir)
    data = {"started": started.isoformat(timespec="seconds"), "commit": git_commit(), "ncpu": NCPU,
            "args": {k: v for k, v in vars(args).items() if k != "names"}, "measurements": {}}
    work = tempfile.mkdtemp(prefix="load-")
    runner = Runner(args, work)

    def save():
        with open(os.path.join(out_dir, "results.json"), "w") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
        md = os.path.join(out_dir, "report.md")
        write_md(md, data)
        write_html(os.path.join(out_dir, "report.html"), md, data)

    try:
        for name in names:
            if name in A_TARGETS:
                sid, desc, _ = A_TARGETS[name]
                print_suite("[성능 측정] %s · %s" % (sid, desc))
                m = runner.series_a(name)
                if name == "static-small":
                    m["saturation"] = saturation(m["points"])
                    print("  포화 지점: -c %d" % m["saturation"])
            elif name == "keepalive-off":
                sid, desc = "load-keepalive-off", "연결 유지 끔 (ab, -k 없음, home.html)"
                print_suite("[성능 측정] %s · %s" % (sid, desc))
                m = runner.series_a("static-small", tool="ab")
                m["saturation"] = saturation(m["points"])
                print("  포화 지점: -c %d" % m["saturation"])
            else:
                sid = "load-b-threads"
                sat = args.sat_conns
                if sat is None and "static-small" in data["measurements"]:
                    sat = data["measurements"]["static-small"]["saturation"]
                if sat is None:
                    sat, src = previous_saturation()
                    if sat is None:
                        sys.exit("B의 -c를 정할 수 없습니다. static-small을 먼저 돌리거나 --sat-conns를 주세요")
                    print("  (이전 결과 %s의 포화 지점 사용)" % src)
                desc = "작업 스레드 수 효과 (home.html, -c %d)" % sat
                print_suite("[성능 측정] %s · %s" % (sid, desc))
                m = runner.series_b(sat)
            m["id"], m["desc"] = sid, desc
            m["errors"] = sum(p["errors"] for p in m["points"])
            m["passed"] = m["errors"] == 0 and all(c == 0 for c in m["stop_codes"])
            data["measurements"][name] = m
            save()
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print_suite("[성능 측정] 요약")
    for name, measurement in data["measurements"].items():
        text = summary_line(name, measurement)
        if measurement["passed"]:
            text = text.split(". ", 1)[1]
        print_case(measurement["id"], text, "통과" if measurement["passed"] else "실패")

    print("\n결과: %s" % out_dir)
    print("  report.md   시트에 옮길 표")
    print("  report.html 그래프 (WSL에서 열기: cd %s && explorer.exe report.html)" % out_dir)
    failed = [m["id"] for m in data["measurements"].values() if not m["passed"]]
    if failed:
        sys.stdout.flush()
        print("[실패] " + ", ".join(failed), file=sys.stderr)
        return 1
    print("\n[성능 측정 완료] %d개 통과 (오류 0, 서버 정상 종료)" % len(data["measurements"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
