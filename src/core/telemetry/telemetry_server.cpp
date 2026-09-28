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
<p id="reconnect-hint">断开后 1 秒自动重连。</p>
<script>
(function () {
  "use strict";

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

  function render(msg) {
    var d;
    try { d = JSON.parse(msg); } catch (err) { return; }
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
  }

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
    };

    ws.onmessage = function (ev) {
      render(ev.data);
    };

    ws.onerror = function () {
      // onclose 会随后触发
    };

    ws.onclose = function () {
      setConn("disconnected — reconnecting…", false);
      ws = null;
      scheduleReconnect();
    };
  }

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

}  // namespace

struct TelemetryServer::Impl {
    httplib::Server svr;
    std::thread listener_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    std::mutex clients_mutex;
    std::set<httplib::ws::WebSocket*> clients;
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

        // cpp-httplib 0.58 只有这一个 WebSocket 入口：连接建立后 handler
        // 在本线程内以 read() 阻塞直到连接结束。open/close 语义由
        // handler 进入/退出时的加锁插入/删除等价实现。
        im->svr.WebSocket("/ws", [im](const httplib::Request&,
                                      httplib::ws::WebSocket& ws) {
            {
                std::lock_guard<std::mutex> lock(im->clients_mutex);
                im->clients.insert(&ws);
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
    Impl* im = impl_;
    if (!im->running.load()) return;

    std::string payload;
    try {
        payload = BundleToJson(bundle).dump();
    } catch (...) {
        return;
    }

    std::lock_guard<std::mutex> lock(im->clients_mutex);
    for (auto it = im->clients.begin(); it != im->clients.end();) {
        auto* ws = *it;
        if (ws == nullptr || !ws->is_open() || !ws->send(payload)) {
            it = im->clients.erase(it);
        } else {
            ++it;
        }
    }
}
