#include "telemetry_server.h"

#include "httplib/httplib.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

// 状态面板页（UI-1c-2，占位页为 UI-1c-1）。内嵌在源文件里，避免运行目录依赖。
const char* const kIndexHtml = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>module-host monitor</title>
<link rel="icon" href="data:,">
<style>
  :root {
    --bg: #1e1e1e;
    --fg: #d4d4d4;
    --muted: #808080;
    --ok: #4ec9b0;
    --bad: #f48771;
    --label: #9cdcfe;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    padding: 24px;
    background: var(--bg);
    color: var(--fg);
    font: 14px/1.5 "Consolas", "Menlo", monospace;
  }
  header {
    display: flex;
    align-items: baseline;
    gap: 16px;
    margin-bottom: 20px;
    padding-bottom: 12px;
    border-bottom: 1px solid #333;
  }
  header h1 {
    margin: 0;
    font-size: 16px;
    font-weight: 600;
  }
  #conn {
    font-size: 12px;
    padding: 2px 8px;
    border-radius: 3px;
  }
  .conn-ok  { color: var(--bg); background: var(--ok); }
  .conn-bad { color: var(--bg); background: var(--bad); }
  main {
    display: grid;
    grid-template-columns: 160px 1fr;
    gap: 6px 16px;
    max-width: 640px;
  }
  .label {
    color: var(--label);
    text-align: right;
  }
  .value { color: var(--fg); }
  .badge {
    display: inline-block;
    padding: 1px 8px;
    border-radius: 3px;
    font-weight: 600;
  }
  .state-IDLE         { background: #3c3c3c; color: #d4d4d4; }
  .state-CHASE        { background: #0e639c; color: #ffffff; }
  .state-ATTACK       { background: #d16969; color: #ffffff; }
  .state-ATTACK_TURN  { background: #c586c0; color: #ffffff; }
  .state-RECOVERY     { background: #4ec9b0; color: #1e1e1e; }
  .state-unknown      { background: #3c3c3c; color: #d4d4d4; }
  #reconnect-hint {
    margin-top: 24px;
    color: var(--muted);
    font-size: 12px;
  }
  #view {
    display: block;
    margin: 24px auto 0;
    background: #181818;
    border: 1px solid #333;
    width: 800px;
    height: 450px;
  }
  #controls {
    display: flex;
    align-items: center;
    gap: 8px;
    margin: 12px auto 0;
    max-width: 800px;
    padding: 8px 12px;
    background: #252525;
    border: 1px solid #333;
    border-radius: 3px;
  }
  #controls.hidden { display: none; }
  #controls button {
    background: #3c3c3c;
    color: var(--fg);
    border: 1px solid #555;
    border-radius: 3px;
    padding: 4px 10px;
    font: inherit;
    cursor: pointer;
  }
  #controls button:hover { background: #4a4a4a; }
  #controls button:disabled { opacity: 0.4; cursor: default; }
  #seek {
    flex: 1;
    min-width: 120px;
  }
  #time {
    font-size: 12px;
    color: var(--muted);
    white-space: nowrap;
  }
  #speed {
    background: #3c3c3c;
    color: var(--fg);
    border: 1px solid #555;
    border-radius: 3px;
    padding: 3px 6px;
    font: inherit;
  }
</style>
</head>
<body>
<header>
  <h1>module-host monitor</h1>
  <span id="conn" class="conn-bad">connecting…</span>
</header>
<main>
  <span class="label">state</span>       <span class="value"><span id="state" class="badge state-unknown">--</span></span>
  <span class="label">frame</span>       <span class="value" id="frame">--</span>
  <span class="label">facing</span>      <span class="value" id="facing">--</span>
  <span class="label">me</span>          <span class="value" id="me">--</span>
  <span class="label">target</span>      <span class="value" id="target">--</span>
  <span class="label">active_key</span>  <span class="value" id="key">--</span>
  <span class="label">desired_e</span>   <span class="value" id="e">--</span>
</main>
<canvas id="view"></canvas>
<div id="controls" class="hidden">
  <button id="btn-play" title="播放/暂停">▶</button>
  <button id="btn-prev" title="上一帧">⏮</button>
  <button id="btn-next" title="下一帧">⏭</button>
  <input id="seek" type="range" min="0" max="0" value="0">
  <span id="time">0 / 0</span>
  <select id="speed">
    <option value="0.5">0.5x</option>
    <option value="1" selected>1x</option>
    <option value="2">2x</option>
    <option value="4">4x</option>
  </select>
</div>
<p id="reconnect-hint">断开后 1 秒自动重连。</p>
<script>
(function () {
  "use strict";

  // ===== 状态面板 =====
  var STATE_NAMES = ["IDLE", "CHASE", "ATTACK", "ATTACK_TURN", "RECOVERY"];
  var ws = null;
  var reconnectTimer = null;

  function el(id) { return document.getElementById(id); }

  function setConn(text, ok) {
    var e = el("conn");
    e.textContent = text;
    e.className = ok ? "conn-ok" : "conn-bad";
  }

  function fmt(n, digits) {
    if (typeof n !== "number" || !isFinite(n)) return "--";
    return n.toFixed(digits);
  }

  function hex2(n) {
    if (typeof n !== "number") return "--";
    var s = (n >>> 0).toString(16).toUpperCase();
    if (s.length < 2) s = "0" + s;
    return "0x" + s;
  }

  // ===== 画布 =====
  var CANVAS_W = 800;
  var CANVAS_H = 450;

  // 攻击区常量（与 ScriptConfig 同步，见 docs/ui-design.md §13.5）
  var BAND_X_MIN     = 0.010;
  var BAND_X_MAX     = 0.1458;
  var BAND_Y_TOP     = -0.074;
  var BAND_Y_BOT     = 0.019;
  var BAND_Y_HALF_UP = 0.074;
  var BAND_Y_HALF_DN = 0.019;

  var canvas = null;
  var ctx = null;

  function initCanvas() {
    canvas = el("view");
    if (!canvas) return;
    var dpr = window.devicePixelRatio || 1;
    canvas.width  = Math.round(CANVAS_W * dpr);
    canvas.height = Math.round(CANVAS_H * dpr);
    ctx = canvas.getContext("2d");
    if (ctx) {
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    }
  }

  function clearCanvas() {
    if (!ctx) return;
    ctx.fillStyle = "#181818";
    ctx.fillRect(0, 0, CANVAS_W, CANVAS_H);
  }

  function drawGrid() {
    if (!ctx) return;
    ctx.strokeStyle = "#2a2a2a";
    ctx.lineWidth = 1;
    var i, x, y;
    for (i = 1; i < 10; i++) {
      x = (CANVAS_W * i) / 10;
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, CANVAS_H);
      ctx.stroke();
    }
    for (i = 1; i < 10; i++) {
      y = (CANVAS_H * i) / 10;
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(CANVAS_W, y);
      ctx.stroke();
    }
  }

  function drawBand(me_fx, me_fy, facing) {
    if (!ctx) return;
    var N = 32;
    var i, t, y, yHalf, s, xOuter, cx, cy;

    ctx.beginPath();
    cx = me_fx * CANVAS_W + facing * BAND_X_MIN * CANVAS_W;
    cy = me_fy * CANVAS_H + BAND_Y_TOP * CANVAS_H;
    ctx.moveTo(cx, cy);

    for (i = 0; i <= N; i++) {
      t = i / N;
      y = BAND_Y_TOP + (BAND_Y_BOT - BAND_Y_TOP) * t;
      yHalf = (y < 0) ? BAND_Y_HALF_UP : BAND_Y_HALF_DN;
      s = 1 - (y / yHalf) * (y / yHalf);
      if (s < 0) s = 0;
      xOuter = BAND_X_MAX * Math.sqrt(s);
      if (xOuter < BAND_X_MIN) xOuter = BAND_X_MIN;
      cx = me_fx * CANVAS_W + facing * xOuter * CANVAS_W;
      cy = me_fy * CANVAS_H + y * CANVAS_H;
      ctx.lineTo(cx, cy);
    }

    cx = me_fx * CANVAS_W + facing * BAND_X_MIN * CANVAS_W;
    cy = me_fy * CANVAS_H + BAND_Y_BOT * CANVAS_H;
    ctx.lineTo(cx, cy);

    ctx.closePath();
    ctx.fillStyle = "rgba(14, 99, 156, 0.18)";
    ctx.fill();
    ctx.strokeStyle = "#4fc3f7";
    ctx.lineWidth = 1;
    ctx.stroke();
  }

  function drawDets(dets) {
    if (!ctx || !dets) return;
    for (var i = 0; i < dets.length; i++) {
      var d = dets[i];
      var left   = (d.cx - d.w * 0.5) * CANVAS_W;
      var top    = (d.cy - d.h * 0.5) * CANVAS_H;
      var width  = d.w * CANVAS_W;
      var height = d.h * CANVAS_H;
      ctx.strokeStyle = (d.cls === 0) ? "#4ec9b0" : "#e0e0e0";
      ctx.lineWidth = 1;
      ctx.strokeRect(left, top, width, height);
    }
  }

  function drawMe(me_fx, me_fy) {
    if (!ctx) return;
    var x = me_fx * CANVAS_W;
    var y = me_fy * CANVAS_H;
    ctx.beginPath();
    ctx.arc(x, y, 5, 0, Math.PI * 2);
    ctx.fillStyle = "#4ec9b0";
    ctx.fill();
    ctx.strokeStyle = "#4ec9b0";
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(x - 10, y); ctx.lineTo(x + 10, y);
    ctx.moveTo(x, y - 10); ctx.lineTo(x, y + 10);
    ctx.stroke();
  }

  function drawTarget(target_cx, me_fy) {
    if (!ctx) return;
    var x = target_cx * CANVAS_W;
    var y = me_fy * CANVAS_H;
    ctx.strokeStyle = "rgba(244, 135, 113, 0.5)";
    ctx.lineWidth = 1;
    ctx.setLineDash([4, 4]);
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, CANVAS_H);
    ctx.stroke();
    ctx.strokeStyle = "#f48771";
    ctx.beginPath();
    ctx.arc(x, y, 6, 0, Math.PI * 2);
    ctx.stroke();
    ctx.setLineDash([]);
  }

  function drawScene(d) {
    if (!ctx) return;
    var s = d.script || {};

    clearCanvas();
    drawGrid();

    if (s.me_locked === true) {
      drawBand(s.me_fx, s.me_fy, s.facing === -1 ? -1 : 1);
    }

    if (d.dets) {
      drawDets(d.dets);
    }

    if (s.me_locked === true) {
      drawMe(s.me_fx, s.me_fy);
    }

    if (s.target_locked === true) {
      drawTarget(s.target_cx, s.me_fy);
    }
  }

  // ===== 单帧渲染 =====
  function renderFrame(d) {
    var s = d.script || {};

    el("frame").textContent = (typeof d.frame === "number") ? d.frame : "--";

    var name = (typeof s.state === "number" && STATE_NAMES[s.state]) || "unknown";
    var stateEl = el("state");
    stateEl.textContent = name;
    stateEl.className = "badge state-" + name;

    if (s.facing === 1)       el("facing").textContent = "1 (→)";
    else if (s.facing === -1) el("facing").textContent = "-1 (←)";
    else                      el("facing").textContent = "--";

    el("me").textContent = s.me_locked
      ? "locked (" + fmt(s.me_fx, 3) + ", " + fmt(s.me_fy, 3) + ")"
      : "unlocked";

    el("target").textContent = s.target_locked
      ? "locked cx=" + fmt(s.target_cx, 3)
      : "unlocked";

    el("key").textContent = hex2(s.active_key);
    el("e").textContent   = s.desired_e ? "true" : "false";

    drawScene(d);
  }

  // ===== 回放控制（UI-3c-2） =====
  // 三态机：live（实时，直接渲染）/ buffering（缓存中）/ playing（本地播放）
  var FRAME_MS = 33;          // trace 帧间隔 ~33ms（30fps）
  var mode = "live";
  var buffer = [];
  var playIdx = 0;            // 下一帧要渲染的索引
  var playSpeed = 1.0;
  var playTimer = null;
  var playing = false;

  function setMode(m) {
    mode = m;
    var ctl = el("controls");
    if (!ctl) return;
    if (m === "playing") ctl.classList.remove("hidden");
    else                 ctl.classList.add("hidden");
  }

  function updateControls() {
    var total = buffer.length;
    var cur = playIdx > 0 ? playIdx - 1 : 0;
    if (total > 0 && cur >= total) cur = total - 1;

    el("time").textContent = cur + " / " + total;

    var seek = el("seek");
    seek.max = total > 0 ? (total - 1) : 0;
    seek.value = cur;

    el("btn-play").textContent = playing ? "⏸" : "▶";
    el("btn-prev").disabled = (cur <= 0);
    el("btn-next").disabled = (total === 0) || (cur >= total - 1);
  }

  function playStep() {
    if (!playing) return;
    if (playIdx >= buffer.length) {
      playing = false;
      updateControls();
      return;
    }
    renderFrame(buffer[playIdx]);
    playIdx++;
    updateControls();
    playTimer = setTimeout(playStep, FRAME_MS / playSpeed);
  }

  function playStart() {
    if (playing) return;
    if (buffer.length === 0) return;
    if (playIdx >= buffer.length) playIdx = 0;
    playing = true;
    updateControls();
    playStep();
  }

  function playPause() {
    if (playing) {
      playing = false;
      if (playTimer) { clearTimeout(playTimer); playTimer = null; }
      updateControls();
    } else {
      playStart();
    }
  }

  function stepPrev() {
    playing = false;
    if (playTimer) { clearTimeout(playTimer); playTimer = null; }
    var cur = playIdx > 0 ? playIdx - 1 : 0;
    if (cur > 0) cur--;
    if (buffer.length > 0) {
      renderFrame(buffer[cur]);
      playIdx = cur + 1;
      updateControls();
    }
  }

  function stepNext() {
    playing = false;
    if (playTimer) { clearTimeout(playTimer); playTimer = null; }
    if (playIdx < buffer.length) {
      renderFrame(buffer[playIdx]);
      playIdx++;
      updateControls();
    }
  }

  function seekTo(idx) {
    playing = false;
    if (playTimer) { clearTimeout(playTimer); playTimer = null; }
    if (buffer.length === 0) return;
    if (idx < 0) idx = 0;
    if (idx >= buffer.length) idx = buffer.length - 1;
    renderFrame(buffer[idx]);
    playIdx = idx + 1;
    updateControls();
  }

  function bindControls() {
    var btnPlay = el("btn-play");
    var btnPrev = el("btn-prev");
    var btnNext = el("btn-next");
    var seek = el("seek");
    var speed = el("speed");

    if (btnPlay) btnPlay.onclick = playPause;
    if (btnPrev) btnPrev.onclick = stepPrev;
    if (btnNext) btnNext.onclick = stepNext;
    if (seek) seek.oninput = function () {
      seekTo(parseInt(seek.value, 10));
    };
    if (speed) speed.onchange = function () {
      playSpeed = parseFloat(speed.value) || 1.0;
    };
  }

  // ===== 消息分流 =====
  function handleMessage(raw) {
    var d;
    try { d = JSON.parse(raw); } catch (err) { return; }

    if (d.v === 0) {
      if (d.meta === "replay_begin") {
        playing = false;
        if (playTimer) { clearTimeout(playTimer); playTimer = null; }
        buffer = [];
        playIdx = 0;
        setMode("buffering");
        updateControls();
      } else if (d.meta === "replay_end") {
        setMode("playing");
        updateControls();
        playStart();
      }
      return;
    }

    if (d.v === 1) {
      if (mode === "buffering") {
        buffer.push(d);
        updateControls();
      } else if (mode === "live") {
        renderFrame(d);
      }
      // playing 中收到新帧：忽略（core 一次推完才发 replay_end）
      return;
    }
  }

  // ===== 连接 =====
  function scheduleReconnect() {
    if (reconnectTimer !== null) return;
    reconnectTimer = setTimeout(function () {
      reconnectTimer = null;
      connect();
    }, 1000);
  }

  function connect() {
    setConn("connecting…", false);
    var url = (location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws";
    ws = new WebSocket(url);

    ws.onopen = function () {
      setConn("connected", true);
      if (mode === "playing" && buffer.length > 0) {
        // 离线保留的回放状态：一旦连上新的 core（多半是实时模式）就清空，
        // 避免旧回放帧污染实时流（5.6 要求）。回放 core 重连会随后收到
        // replay_begin，重新进入 buffering。
        playing = false;
        if (playTimer) { clearTimeout(playTimer); playTimer = null; }
        buffer = [];
        playIdx = 0;
        setMode("live");
      }
    };

    ws.onmessage = function (ev) {
      handleMessage(ev.data);
    };

    ws.onerror = function () {
      // onclose 会随后触发
    };

    ws.onclose = function () {
      setConn("disconnected — reconnecting…", false);
      ws = null;
      if (mode === "playing") {
        // 回放已完整缓存（replay_end 已到）：core 随即退出属正常，
        // 不清理回放状态，控制条与本地 buffer 保留，供离线核验/继续播放。
        updateControls();
      } else {
        // live / buffering 中断线：清理回放状态，回到 live
        playing = false;
        if (playTimer) { clearTimeout(playTimer); playTimer = null; }
        buffer = [];
        playIdx = 0;
        setMode("live");
      }
      scheduleReconnect();
    };
  }

  initCanvas();
  bindControls();
  connect();
})();
</script>
</body>
</html>
)HTML";

// FrameBundle -> 单行 JSON。字段名严格按 §五：
// v/frame/t/script/dets；script 9 字段；dets 每项 cls/conf/cx/cy/w/h/id。
nlohmann::json BundleToJson(const telemetry::FrameBundle& bundle) {
    nlohmann::json j;
    j["v"] = bundle.v;
    j["frame"] = bundle.frame;
    j["t"] = bundle.t;

    nlohmann::json script;
    script["state"] = bundle.script.state;
    script["facing"] = bundle.script.facing;
    script["me_locked"] = bundle.script.me_locked;
    script["me_fx"] = bundle.script.me_fx;
    script["me_fy"] = bundle.script.me_fy;
    script["target_locked"] = bundle.script.target_locked;
    script["target_cx"] = bundle.script.target_cx;
    script["active_key"] = bundle.script.active_key;
    script["desired_e"] = bundle.script.desired_e;
    j["script"] = script;

    nlohmann::json dets = nlohmann::json::array();
    for (const auto& d : bundle.dets) {
        nlohmann::json item;
        item["cls"] = d.cls;
        item["conf"] = d.conf;
        item["cx"] = d.cx;
        item["cy"] = d.cy;
        item["w"] = d.w;
        item["h"] = d.h;
        item["id"] = d.id;
        dets.push_back(item);
    }
    j["dets"] = dets;
    return j;
}

// 控制消息（docs/ui-design.md §十四）。调用者保证是单行合法 JSON。
constexpr char kReplayBegin[] = R"({"v":0,"meta":"replay_begin"})";

}  // namespace

struct TelemetryServer::Impl {
    httplib::Server svr;
    std::thread listener_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    std::mutex clients_mutex;
    std::set<httplib::ws::WebSocket*> clients;
    // 是否处于回放模式：为 true 时新连接补发一次 replay_begin。
    // 只在持有 clients_mutex 时读写（与发送同临界区，保证顺序）。
    bool replay_mode = false;
    bool routes_registered = false;
};

TelemetryServer::TelemetryServer() : impl_(new Impl()) {}

TelemetryServer::~TelemetryServer() {
    Stop();
    delete impl_;
    impl_ = nullptr;
}

bool TelemetryServer::Start(int port) {
    if (impl_ == nullptr) return false;
    if (impl_->running.load()) return true;

    Impl* im = impl_;
    if (!im->routes_registered) {
        im->svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(kIndexHtml, "text/html; charset=utf-8");
        });

        im->svr.Get("/favicon.ico", [](const httplib::Request&, httplib::Response& res) {
            res.status = 204;
        });

        // cpp-httplib 0.58 只有这一个 WebSocket 入口：连接建立后 handler
        // 在本线程内以 read() 阻塞直到连接结束。open/close 语义由
        // handler 进入/退出时的加锁插入/删除等价实现。
        im->svr.WebSocket("/ws", [im](const httplib::Request&,
                                      httplib::ws::WebSocket& ws) {
            {
                std::lock_guard<std::mutex> lock(im->clients_mutex);
                im->clients.insert(&ws);
                // 回放模式下给新连接补发 replay_begin（UI-3c-1a）。
                // 必须与 insert 同一临界区：PublishRaw 同样持 clients_mutex，
                // 因此这条控制消息一定先于任何帧写出，是该连接的首条消息。
                if (im->replay_mode) {
                    ws.send(kReplayBegin);
                }
            }
            // 自设读超时后 read() 在帧边界返回 Timeout 且连接保持可用。
            // 否则 read() 会一直等到对端回 Close 或 300s 兜底超时，
            // Stop()（尤其是对端不回 Close 时）就退不出去。
            ws.set_read_timeout(std::chrono::milliseconds(500));
            std::string msg;
            while (!im->stopping.load()) {
                const auto r = ws.read(msg);
                if (r == httplib::ws::Fail) break;
                if (r == httplib::ws::Timeout) continue;  // 回到循环头重查 stopping
                // Text/Binary：UI-1c-1 不处理上行消息。
            }
            std::lock_guard<std::mutex> lock(im->clients_mutex);
            im->clients.erase(&ws);
        });
        im->routes_registered = true;
    }

    im->stopping.store(false);
    im->listener_thread = std::thread([im, port] {
        im->svr.listen("127.0.0.1", port);
    });
    // 阻塞直到绑定成功（is_running_）或绑定失败（is_decommissioned）。
    im->svr.wait_until_ready();
    if (!im->svr.is_running()) {
        if (im->listener_thread.joinable()) im->listener_thread.join();
        return false;
    }
    im->running.store(true);
    return true;
}

void TelemetryServer::Stop() {
    if (impl_ == nullptr) return;
    Impl* im = impl_;
    if (!im->running.exchange(false)) return;

    // 先置 stopping：handler 的 read() 自设了 500ms 超时，最多 500ms 后
    // 回到循环头退出，这样即使对端不回 Close，Stop() 也不会卡到 300s
    // 兜底超时。再对每个连接发 Close（对端回包时 handler 会立刻退出）。
    im->stopping.store(true);
    {
        std::lock_guard<std::mutex> lock(im->clients_mutex);
        for (auto* ws : im->clients) {
            ws->close();
        }
    }

    im->svr.stop();
    if (im->listener_thread.joinable()) im->listener_thread.join();

    std::lock_guard<std::mutex> lock(im->clients_mutex);
    im->clients.clear();
}

void TelemetryServer::Publish(const telemetry::FrameBundle& bundle) {
    if (impl_ == nullptr) return;
    if (!impl_->running.load()) return;

    std::string payload;
    try {
        payload = BundleToJson(bundle).dump();
    } catch (...) {
        return;
    }
    PublishRaw(payload);
}

void TelemetryServer::PublishRaw(const std::string& json_line) {
    if (impl_ == nullptr) return;
    Impl* im = impl_;
    if (!im->running.load()) return;

    std::lock_guard<std::mutex> lock(im->clients_mutex);
    for (auto it = im->clients.begin(); it != im->clients.end();) {
        auto* ws = *it;
        if (ws == nullptr || !ws->is_open() || !ws->send(json_line)) {
            it = im->clients.erase(it);
        } else {
            ++it;
        }
    }
}

void TelemetryServer::SetReplayMode(bool on) {
    if (impl_ == nullptr) return;
    Impl* im = impl_;

    // 置位与广播在同一临界区内完成，避免两种竞态：
    //   - 先广播后置位：间隙里连上的客户端会漏掉 replay_begin；
    //   - 先置位后广播：间隙里连上的客户端会收到两次（自己补发一次 +
    //     这里的广播一次）。置位前已连接的客户端由本次广播覆盖，
    //     置位后新连接由 handler 补发，两者互不重叠。
    std::lock_guard<std::mutex> lock(im->clients_mutex);
    const bool was = im->replay_mode;
    im->replay_mode = on;
    if (!on || was) return;

    for (auto it = im->clients.begin(); it != im->clients.end();) {
        auto* ws = *it;
        if (ws == nullptr || !ws->is_open() || !ws->send(kReplayBegin)) {
            it = im->clients.erase(it);
        } else {
            ++it;
        }
    }
}
