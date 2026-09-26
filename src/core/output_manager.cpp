#include "output_manager.h"

#include "recorder.h"

#include <windows.h>

#include <string>
#include <deque>
#include <set>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>

namespace {

std::string Trim(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r' || s[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return s.substr(begin, end - begin);
}

bool ParseInt(const std::string& text, int* out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }
    *out = static_cast<int>(value);
    return true;
}

void ParseConfig(const std::string& config, std::string* port, DWORD* baud) {
    port->assign("COM6");
    *baud = 115200;
    if (config.empty()) {
        return;
    }
    size_t pos = 0;
    const std::string content(config);
    while (pos <= content.size()) {
        size_t nl = content.find('\n', pos);
        if (nl == std::string::npos) {
            nl = content.size();
        }
        std::string line = content.substr(pos, nl - pos);
        pos = nl + 1;
        line = Trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = Trim(line.substr(0, eq));
        const std::string value = Trim(line.substr(eq + 1));
        int num = 0;
        if (key == "input_port") {
            if (!value.empty()) {
                port->assign(value);
            }
        } else if (key == "input_baud") {
            if (ParseInt(value, &num) && num > 0) {
                *baud = static_cast<DWORD>(num);
            }
        }
    }
}

bool SendCommand(HANDLE h, const std::string& cmd) {
    std::string payload = cmd;
    payload.push_back('\n');
    DWORD written = 0;
    if (!WriteFile(h, payload.data(), static_cast<DWORD>(payload.size()),
                   &written, nullptr)) {
        return false;
    }
    return true;
}

const char* KeyName(int32_t vk) {
    if (vk >= 0x41 && vk <= 0x5A) {
        static thread_local char buf[2];
        buf[0] = static_cast<char>(vk - 0x41 + 'a');
        buf[1] = '\0';
        return buf;
    }
    if (vk >= 0x30 && vk <= 0x39) {
        static thread_local char buf[2];
        buf[0] = static_cast<char>(vk - 0x30 + '0');
        buf[1] = '\0';
        return buf;
    }
    switch (vk) {
    case 0x0D: return "enter";
    case 0x1B: return "esc";
    case 0x20: return "space";
    case 0x09: return "tab";
    case 0x08: return "backspace";
    case 0x10: return "shift";
    case 0x11: return "ctrl";
    case 0x12: return "alt";
    case 0x26: return "up";
    case 0x28: return "down";
    case 0x25: return "left";
    case 0x27: return "right";
    default: return nullptr;
    }
}

const char* ButtonName(int32_t id) {
    switch (id) {
    case 1: return "LEFT";
    case 2: return "RIGHT";
    case 3: return "MIDDLE";
    default: return nullptr;
    }
}

bool OpenPort(const std::string& port, DWORD baud, HANDLE* out_handle) {
    std::wstring path = L"\\\\.\\";
    for (size_t i = 0; i < port.size(); ++i) {
        path.push_back(static_cast<wchar_t>(static_cast<unsigned char>(port[i])));
    }

    HANDLE h = CreateFileW(path.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           0,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        CloseHandle(h);
        return false;
    }
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(h, &dcb)) {
        CloseHandle(h);
        return false;
    }

    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 100;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 500;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    SetCommTimeouts(h, &timeouts);

    SendCommand(h, "mk.release");

    *out_handle = h;
    return true;
}

bool TranslateAndSend(HANDLE h, const core_action& a, bool mock) {
    std::string cmd;
    switch (a.kind) {
    case CORE_ACTION_NONE:
        return true;
    case CORE_ACTION_POINTER_MOVE:
        cmd = "mk.move " + std::to_string(a.a) + " " + std::to_string(a.b);
        break;
    case CORE_ACTION_POINTER_BUTTON: {
        const char* btn = ButtonName(a.a);
        if (btn == nullptr) return true;
        if (a.b == 1) cmd = std::string("mk.mdown ") + btn;
        else if (a.b == 0) cmd = std::string("mk.mup ") + btn;
        else return true;
        break;
    }
    case CORE_ACTION_KEY: {
        const char* name = KeyName(a.a);
        if (name == nullptr) return true;
        if (a.b == 1) cmd = std::string("mk.press ") + name;
        else if (a.b == 0) cmd = std::string("mk.release ") + name;
        else return true;
        break;
    }
    case CORE_ACTION_WAIT:
        if (a.a > 0) Sleep(static_cast<DWORD>(a.a));
        return true;
    case CORE_ACTION_CUSTOM:
        return true;
    default:
        return true;
    }
    if (cmd.empty()) return true;
    if (mock) {
        std::fprintf(stderr, "[output/mock] %s\n", cmd.c_str());
        return true;
    }
    return SendCommand(h, cmd);
}

}  // namespace

struct QueuedAction {
    core_action act;
    uint64_t source;      // 0=script 1=remote 2=pause
    uint64_t batch_id;   // 0 = 立即发送（SendAsync / SetScriptPaused）；>=1 = 脚本批号
};

struct OutputManager::Impl {
    HANDLE port = INVALID_HANDLE_VALUE;
    std::string port_name;
    bool mock_mode = false;
    DWORD baud_rate = 115200;

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<QueuedAction> queue;
    bool stop_flag = false;
    bool started = false;
    bool script_paused = false;

    std::set<int> script_pressed_keys;

    std::thread worker;

    int consecutive_failures = 0;
    Recorder* send_sink = nullptr;
    uint64_t script_batch_counter = 0;
    uint64_t last_sent_script_batch_id = 0;
};

OutputManager::OutputManager() : impl_(new Impl()) {}

OutputManager::~OutputManager() {
    Stop();
    delete impl_;
}

bool OutputManager::Start(const std::string& config) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    if (impl_->started) return true;

    ParseConfig(config, &impl_->port_name, &impl_->baud_rate);

    if (impl_->port_name == "none") {
        impl_->mock_mode = true;
        impl_->port = INVALID_HANDLE_VALUE;
        std::fprintf(stderr, "[output] mock 模式：不打开真实串口\n");
        std::fprintf(stderr, "[output/mock] mk.release\n");
    } else {
        impl_->mock_mode = false;
        if (!OpenPort(impl_->port_name, impl_->baud_rate, &impl_->port)) {
            return false;
        }
    }

    impl_->started = true;
    impl_->stop_flag = false;
    impl_->worker = std::thread([this]() {
        while (true) {
            QueuedAction item;
            Recorder* sink = nullptr;
            {
                std::unique_lock<std::mutex> lk(impl_->mutex);
                impl_->cv.wait(lk, [this]() {
                    return impl_->stop_flag || !impl_->queue.empty();
                });
                if (impl_->stop_flag && impl_->queue.empty()) break;
                if (impl_->queue.empty()) continue;
                item = impl_->queue.front();
                impl_->queue.pop_front();
                sink = impl_->send_sink;
            }

            // 跨批抖动：脚本新批的首个动作前随机延迟 0-33ms。
            // 同批动作保持零延迟，保护游戏方向切换（release + press 不可被拉开）。
            // batch_id == 0 的动作（SendAsync / SetScriptPaused 的即时释放）不延迟。
            if (item.batch_id != 0 && item.batch_id != impl_->last_sent_script_batch_id) {
                const int delay = std::rand() % 34;
                if (delay > 0) Sleep(static_cast<DWORD>(delay));
                impl_->last_sent_script_batch_id = item.batch_id;
            }

            if (sink != nullptr) {
                sink->RecordSend(item.source, item.act);
            }

            bool ok = TranslateAndSend(impl_->port, item.act, impl_->mock_mode);
            if (!ok) {
                impl_->consecutive_failures++;
                std::fprintf(stderr, "[output] 写串口失败, 连续 %d 次\n", impl_->consecutive_failures);
                if (impl_->consecutive_failures >= 10) {
                    std::fprintf(stderr, "[output] 尝试重开串口\n");
                    if (impl_->port != INVALID_HANDLE_VALUE) {
                        CloseHandle(impl_->port);
                        impl_->port = INVALID_HANDLE_VALUE;
                    }
                    if (OpenPort(impl_->port_name, impl_->baud_rate, &impl_->port)) {
                        {
                            std::lock_guard<std::mutex> lk2(impl_->mutex);
                            impl_->queue.clear();
                            impl_->consecutive_failures = 0;
                            impl_->last_sent_script_batch_id = 0;
                        }
                    }
                }
            } else {
                impl_->consecutive_failures = 0;
            }
        }
    });

    return true;
}

void OutputManager::Stop() {
    {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        if (!impl_->started) return;
        impl_->stop_flag = true;
        impl_->cv.notify_all();
    }
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    if (impl_->port != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->port);
        impl_->port = INVALID_HANDLE_VALUE;
    }
    impl_->started = false;
}

void OutputManager::SendScript(const core_decision* decision) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    if (!impl_->started || impl_->script_paused) {
        return;
    }
    if (decision == nullptr || decision->out_count > CORE_DECISION_CAPACITY) {
        return;
    }
    const uint64_t batch = ++impl_->script_batch_counter;
    for (uint32_t i = 0; i < decision->out_count; ++i) {
        const core_action& act = decision->actions[i];
        QueuedAction qa;
        qa.act = act;
        qa.source = 0;
        qa.batch_id = batch;
        impl_->queue.push_back(qa);
        if (act.kind == CORE_ACTION_KEY) {
            if (act.b == 1) {
                impl_->script_pressed_keys.insert(act.a);
            } else if (act.b == 0) {
                impl_->script_pressed_keys.erase(act.a);
            }
        }
    }
    impl_->cv.notify_one();
}

void OutputManager::SendAsync(const core_action* action) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    if (!impl_->started || action == nullptr) {
        return;
    }
    QueuedAction qa;
    qa.act = *action;
    qa.source = 1;
    qa.batch_id = 0;
    impl_->queue.push_back(qa);
    impl_->cv.notify_one();
}

void OutputManager::SetScriptPaused(bool paused) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    if (!impl_->started) return;
    if (paused == impl_->script_paused) return;
    impl_->script_paused = paused;
    if (paused) {
        for (int vk : impl_->script_pressed_keys) {
            QueuedAction qa;
            qa.act.kind = CORE_ACTION_KEY;
            qa.act.a = vk;
            qa.act.b = 0;
            qa.act.c = 0;
            qa.source = 2;
            qa.batch_id = 0;
            impl_->queue.push_back(qa);
        }
        impl_->script_pressed_keys.clear();
        impl_->cv.notify_one();
    }
}

void OutputManager::SetSendSink(Recorder* recorder) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->send_sink = recorder;
}

bool OutputManager::IsScriptPaused() const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    return impl_->script_paused;
}