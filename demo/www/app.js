// 이 파일이 text/javascript로 와서 실행되면 JS 칩이 초록색으로 바뀝니다.
document.getElementById("js").classList.add("on");

// ---- 01 정적 파일: 이미지가 image/jpeg로 와서 그려졌는지 ----------------------

var cat = document.getElementById("cat");
function catLoaded() {
    if (cat.complete && cat.naturalWidth > 0) document.getElementById("chip-img").classList.add("on");
}
cat.addEventListener("load", catLoaded);
catLoaded();

// ---- 공통 ----------------------------------------------------------------

var HOST = location.host || "localhost:8000";
var PORT = Number(location.port || 80);
var PORT_A = PORT + 1, PORT_B = PORT + 2;
function $(id) { return document.getElementById(id); }
function setText(id, t) { $(id).textContent = t; }
setText("ports", ":" + PORT + " :" + PORT_A + " :" + PORT_B);
setText("v-port-p", ":" + PORT + " · 프록시"); setText("pane-p", ":" + PORT);
setText("v-port-a", ":" + PORT_A); setText("pane-a", ":" + PORT_A);
setText("v-port-b", ":" + PORT_B); setText("pane-b", ":" + PORT_B);

// 이 페이지를 우리 서버가 아닌 곳(예: VS Code Live Server :5500)에서 열면
// /cgi-bin, /api 요청이 그 서버로 가서 404가 납니다. 응답의 Server 헤더로 확인해 알려 줍니다.
fetch(location.pathname, { method: "GET", cache: "no-store" }).then(function (res) {
    var server = res.headers.get("server") || "";
    if (server.indexOf("Tiny") < 0) {
        var w = $("warn");
        w.innerHTML = "⚠ 이 페이지는 우리 서버가 아니라 <code>" + esc(location.host || "file://") + "</code>" +
            (server ? " (" + esc(server) + ")" : "") + "에서 열렸습니다. CGI와 프록시 요청이 그쪽으로 가서 404가 납니다. " +
            "터미널에서 <code>./demo/web.sh</code>를 실행하고 <code>http://localhost:8000/</code>으로 여세요.";
        w.hidden = false;
    }
}).catch(function () {});

function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }
function esc(s) {
    return String(s).replace(/[&<>"]/g, function (c) {
        return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c];
    });
}
function pad(n, w) { return String(n).padStart(w || 2, "0"); }
function clock() {
    var d = new Date();
    return pad(d.getHours()) + ":" + pad(d.getMinutes()) + ":" + pad(d.getSeconds()) + "." + pad(d.getMilliseconds(), 3);
}
var REASON = { 200: "OK", 400: "Bad Request", 403: "Forbidden", 404: "Not Found", 405: "Method Not Allowed",
               500: "Internal Server Error", 502: "Bad Gateway", 504: "Gateway Timeout" };

// 캐시 없이 요청 하나. 서버에 닿지 못하면 status 0
function send(method, path, body) {
    var opts = { method: method, cache: "no-store" };
    if (body !== undefined) {
        opts.body = body;
        opts.headers = { "Content-Type": "application/x-www-form-urlencoded" };
    }
    var t0 = performance.now();
    return fetch(path, opts).then(function (res) {
        return res.text().then(function (text) {
            return { status: res.status, reason: res.statusText || REASON[res.status] || "",
                     body: text, ms: performance.now() - t0 };
        });
    }).catch(function () {
        return { status: 0, reason: "연결 실패", body: "", ms: performance.now() - t0 };
    });
}

// ---- 왼쪽: 요청 이력 -------------------------------------------------------

var hist = $("history");
var hs = { total: 0, ok: 0, err: 0 };
function addHistory(method, path, r, where, whoClass) {
    var empty = hist.querySelector(".empty");
    if (empty) empty.remove();
    hs.total++;
    if (r.status >= 200 && r.status < 300) hs.ok++; else hs.err++;
    setText("h-total", hs.total); setText("h-ok", hs.ok); setText("h-err", hs.err);

    var li = document.createElement("li");
    li.className = "new";
    li.innerHTML =
        '<span class="meth' + (method === "POST" ? " post" : "") + '">' + method + "</span>" +
        '<span class="path" title="' + esc(path) + '">' + esc(path) + "</span>" +
        '<span class="st s' + String(r.status)[0] + '">' + (r.status || "실패") + "</span>" +
        '<span class="meta"><span>#' + hs.total + " · " + clock() + '</span><span class="' + (whoClass || "") + '">' +
        esc(where) + " · " + r.ms.toFixed(1) + " ms</span></span>";
    hist.insertBefore(li, hist.firstChild);
    while (hist.children.length > 200) hist.removeChild(hist.lastChild);
}
$("h-clear").addEventListener("click", function () {
    hs = { total: 0, ok: 0, err: 0 };
    setText("h-total", 0); setText("h-ok", 0); setText("h-err", 0);
    hist.innerHTML = '<li class="empty">아직 요청이 없습니다</li>';
});

// ---- 01 없는 파일: 404를 페이지 안에서 바로 보여 줌 ----------------------------

$("nf-btn").addEventListener("click", function () {
    send("GET", "/nope.html").then(function (r) {
        addHistory("GET", "/nope.html", r, "메인 서버");
        var text = new DOMParser().parseFromString(r.body, "text/html").body.textContent
            .replace(/\s+/g, " ").trim();
        var badge = $("nf-status");
        badge.textContent = (r.status || "실패") + " " + r.reason;
        badge.className = "status-badge" + (r.status === 200 ? " ok" : "");
        setText("nf-body", text.slice(0, 60) || "(본문 없음)");
        var box = $("nf-result");
        box.hidden = true;
        void box.offsetWidth; // 다시 누를 때도 애니메이션
        box.hidden = false;
    });
});

// ---- 02 CGI 계산기: GET은 값이 주소에, POST는 본문에 ---------------------------

var calc = $("calc");
var answer = $("answer");
var method = "POST";
calc.querySelectorAll("button").forEach(function (b) {
    b.addEventListener("click", function () {
        method = b.dataset.method;
        calc.querySelectorAll("button").forEach(function (o) { o.classList.toggle("on", o === b); });
    });
});

calc.addEventListener("submit", function (e) {
    e.preventDefault();
    var m0 = method;
    var a = $("a").value.trim(), b = $("b").value.trim();
    var params = "a=" + encodeURIComponent(a) + "&b=" + encodeURIComponent(b);
    var path = "/cgi-bin/adder";
    var lines;

    if (m0 === "GET") {
        $("addr").innerHTML = "http://" + esc(HOST) + path + '<span class="q">?' + esc(params) + "</span>";
        lines = ['<span class="m">GET</span> ' + path + '<span class="q">?' + esc(params) + "</span> HTTP/1.1",
                 '<span class="h">Host: ' + esc(HOST) + "</span>",
                 "",
                 '<span class="cm">// 본문 없음 — 값은 주소에 있음</span>'];
        $("calc-note").innerHTML = "<b>GET</b>: 값 <code>" + esc(params) + "</code>이 <b>주소창</b>에 그대로 보입니다. 서버는 이것을 <code>QUERY_STRING</code>으로 CGI에 넘깁니다.";
    } else {
        $("addr").innerHTML = "http://" + esc(HOST) + path;
        lines = ['<span class="m">POST</span> ' + path + " HTTP/1.1",
                 '<span class="h">Host: ' + esc(HOST) + "</span>",
                 '<span class="h">Content-Type: application/x-www-form-urlencoded</span>',
                 '<span class="h">Content-Length: ' + params.length + "</span>",
                 "",
                 '<span class="body">' + esc(params) + "</span>"];
        $("calc-note").innerHTML = "<b>POST</b>: 주소창에는 <code>" + path + "</code>만 있고, 값 <code>" + esc(params) +
            "</code>은 <b>요청 본문</b>에 담깁니다. 서버는 이것을 CGI의 표준 입력으로 넘깁니다.";
    }
    $("req-raw").innerHTML = lines.join("\n");
    answer.className = "answer";
    answer.textContent = "…";

    var req = m0 === "GET" ? send("GET", path + "?" + params) : send("POST", path, params);
    req.then(function (r) {
        addHistory(m0, m0 === "GET" ? path + "?" + params : path, r, "메인 서버 (CGI)");
        var m = r.body.match(/The answer is:\s*(-?\d+)\s*\+\s*(-?\d+)\s*=\s*(-?\d+)/);
        if (r.status === 200 && m) {
            answer.textContent = m[3];
        } else {
            answer.className = "answer err";
            answer.textContent = r.status === 200 ? "정수 두 개를 넣어 주세요" : "요청 실패 (" + (r.status || "연결 안 됨") + ")";
        }
    });
});

// ---- 03 Round-Robin: 그림 애니메이션 ----------------------------------------

var dots = $("dots");
var SVGNS = "http://www.w3.org/2000/svg";

// path를 따라 점 하나를 움직입니다.
function travel(pathId, cls, ms) {
    var path = $(pathId);
    var len = path.getTotalLength();
    var c = document.createElementNS(SVGNS, "circle");
    c.setAttribute("r", 7);
    c.setAttribute("class", cls);
    dots.appendChild(c);
    path.classList.add(cls === "dot-in" ? "hot-in" : cls === "dot-a" ? "hot-a" : "hot-b");
    return new Promise(function (resolve) {
        var t0 = performance.now();
        function step(now) {
            var t = Math.min(1, (now - t0) / ms);
            var e = t < 0.5 ? 2 * t * t : 1 - Math.pow(-2 * t + 2, 2) / 2;
            var p = path.getPointAtLength(e * len);
            c.setAttribute("cx", p.x);
            c.setAttribute("cy", p.y);
            if (t < 1) requestAnimationFrame(step);
            else {
                c.remove();
                setTimeout(function () { path.classList.remove("hot-in", "hot-a", "hot-b"); }, 120);
                resolve();
            }
        }
        requestAnimationFrame(step);
    });
}
function pulse(id) {
    var g = $(id);
    g.classList.remove("pulse");
    void g.getBoundingClientRect();
    g.classList.add("pulse");
}

// ---- 03 Round-Robin: 로그 창과 숫자 -----------------------------------------

var logP = $("log-p"), logA = $("log-a"), logB = $("log-b");
var trail = $("trail");
var rrButtons = document.querySelectorAll("#rr-seg button, #rr-reset");
var cnt, rrNo;

function println(el, html, cls) {
    el.insertAdjacentHTML("beforeend", '<span class="line ' + (cls || "") + '">' + html + "</span>");
    el.scrollTop = el.scrollHeight;
}
function updateKpi() {
    var done = cnt.a + cnt.b;
    var pa = done ? Math.round(cnt.a * 100 / done) : 0;
    var pb = done ? 100 - pa : 0;
    setText("cnt-all", cnt.all); setText("cnt-all2", cnt.all);
    setText("cnt-a", cnt.a); setText("cnt-a2", cnt.a);
    setText("cnt-b", cnt.b); setText("cnt-b2", cnt.b);
    setText("pct-a", pa + "%"); setText("pct-b", pb + "%");
    $("bar-a").style.width = (done ? cnt.a * 100 / done : 0) + "%";
    $("bar-b").style.width = (done ? cnt.b * 100 / done : 0) + "%";
}

function resetRR() {
    cnt = { a: 0, b: 0, all: 0 };
    rrNo = 0;
    updateKpi();
    trail.innerHTML = "";
    setText("v-next", "다음 → A");
    logP.innerHTML = ""; logA.innerHTML = ""; logB.innerHTML = "";
    println(logP, '<span class="cmd">$ ./build/tiny ' + PORT + " --root demo/www --route /api=127.0.0.1:" + PORT_A + ",127.0.0.1:" + PORT_B + "</span>");
    println(logA, '<span class="cmd">$ ./build/tiny ' + PORT_A + " --root demo/lb/www_a</span>");
    println(logB, '<span class="cmd">$ ./build/tiny ' + PORT_B + " --root demo/lb/www_b</span>");
}
resetRR();

function rrOnce(fast) {
    var path = "/api/whoami.txt";
    rrNo++;
    var no = "#" + pad(rrNo);
    var tIn = fast ? 45 : 260, tOut = fast ? 55 : 340;

    var req = send("GET", path);
    return Promise.all([req, travel("path-in", "dot-in", tIn)]).then(function (res) {
        var r = res[0];
        pulse("v-proxy");
        // 백엔드는 "나는 서버 A (포트 8001)"처럼 자기 이름을 답합니다.
        var m = r.status === 200 && r.body.match(/서버\s*([AB])/);
        cnt.all++;
        if (!m) {
            println(logP, '<span class="ts">[' + clock() + "]</span> " + no + " GET " + path +
                ' <span class="bad">' + (r.status || "연결 실패") + "</span>");
            updateKpi();
            addHistory("GET", path, r, "프록시 (응답 없음)");
            var x = document.createElement("span");
            x.className = "x"; x.textContent = "✕";
            trail.appendChild(x);
            return;
        }
        var k = m[1].toLowerCase();
        var port = k === "a" ? PORT_A : PORT_B;
        println(logP, '<span class="ts">[' + clock() + "]</span> " + no + " GET " + path + ' HTTP/1.1 <span class="ok">200</span> ' +
            '<span class="to-' + k + '">-&gt; 127.0.0.1:' + port + "</span>", "mark-" + k);
        return travel("path-" + k, "dot-" + k, tOut).then(function () {
            pulse("v-" + k);
            cnt[k]++;
            updateKpi();
            setText("v-next", "다음 → " + (k === "a" ? "B" : "A"));
            println(k === "a" ? logA : logB, '<span class="ts">[' + clock() + "]</span> " + no + " GET " + path +
                ' HTTP/1.1 <span class="ok">200</span>', "mark-" + k);
            var chip = document.createElement("span");
            chip.className = k; chip.textContent = m[1];
            trail.appendChild(chip);
            while (trail.children.length > 30) trail.removeChild(trail.firstChild);
            addHistory("GET", path, r, "백엔드 " + m[1], "who-" + k);
        });
    });
}

function run(n, btn) {
    var fast = n >= 100;
    rrButtons.forEach(function (b) { b.disabled = true; });
    btn.classList.add("on");
    $("state-p").textContent = "처리 중";
    $("state-p").className = "state busy";
    var p = Promise.resolve();
    for (var i = 0; i < n; i++) {
        p = p.then(function () { return rrOnce(fast); }).then(function () { return sleep(fast ? 0 : 120); });
    }
    return p.then(function () {
        rrButtons.forEach(function (b) { b.disabled = false; });
        btn.classList.remove("on");
        $("state-p").textContent = "대기 중";
        $("state-p").className = "state";
    });
}

document.querySelectorAll("#rr-seg button").forEach(function (b) {
    b.addEventListener("click", function () { run(Number(b.dataset.n), b); });
});
$("rr-reset").addEventListener("click", resetRR);
