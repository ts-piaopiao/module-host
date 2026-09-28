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

// UI-1c-1 占位页。内嵌在源文件里，避免运行目录依赖。
const char* const kIndexHtml = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head><meta charset="utf-8"><title>module-host monitor</title></head>
<body>
<h1>module-host monitor</h1>
<p>UI-1c-1 placeholder — WebSocket /ws</p>
<pre id="out">(waiting for data...)</pre>
<script>
const ws = new WebSocket(`ws://${location.host}/ws`);
ws.onmessage = (e) => { document.getElementById('out').textContent = e.data; };
ws.onclose = () => { document.getElementById('out').textContent = '(disconnected)'; };
ws.onerror = () => { document.getElementById('out').textContent = '(connection error)'; };
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
