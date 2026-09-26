#include "output_manager.h"

#include "recorder.h"

#include <windows.h>

#include <string>
#include <deque>
#include <set>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <random>

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

void ParseConfig(const std::string& config, std::string* port, DWORD* baud,
                 int* min_hold_min_ms, int* min_hold_max_ms, int* min_gap_ms) {
    port->assign("COM6");
    *baud = 115200;
    *min_hold_min_ms = 70;
    *min_hold_max_ms = 90;
    *min_gap_ms = 30;
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
        } else if (key == "output_min_hold_min_ms") {
            if (ParseInt(value, &num) && num >= 0) {
                *min_hold_min_ms = num;
            }
        } else if (key == "output_min_hold_max_ms") {
            if (ParseInt(value, &num) && num >= 0) {
                *min_hold_max_ms = num;
            }
        } else if (key == "output_min_gap_ms") {
            if (ParseInt(value, &num) && num >= 0) {
                *min_gap_ms = num;
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

// 用 CreateWaitableTimerEx 实现亚毫秒精度等待。
// 相比 Sleep(1)（实际精度 ~15ms），此方案不受系统时钟分辨率限制。
// 无全局副作用（不使用 timeBeginPeriod）。
bool WaitPreciseUs(HANDLE h_timer, uint64_t us) {
    if (h_timer == nullptr) {
        // 降级：无高精度 timer 时用 Sleep
        if (us >= 1000) {
            Sleep(static_cast<DWORD>(us / 1000));
        } else if (us > 0) {
            Sleep(0);  // 让出 CPU
        }
        return true;
    }
    LARGE_INTEGER due;
    // 负值 = 相对时间；单位 100ns；1us = 10 * 100ns
    due.QuadPart = -static_cast<LONGLONG>(us) * 10LL;
    if (!SetWaitableTimer(h_timer, &due, 0, nullptr, nullptr, FALSE)) {
        return false;
    }
    WaitForSingleObject(h_timer, INFINITE);
    return true;
}

// 微秒级挂钟（QPC），与 Recorder 的时间戳同源，无 15.6ms 量化。
uint64_t NowUs() {
    static LARGE_INTEGER freq = []() {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(
        (static_cast<double>(now.QuadPart) * 1e6) / static_cast<double>(freq.QuadPart));
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
    std::set<int> script_intent_keys;

    std::thread worker;

    int consecutive_failures = 0;
    HANDLE h_timer = nullptr;
    std::mt19937 rng;
    int min_hold_min_ms = 70;
    int min_hold_max_ms = 90;
    int min_gap_ms = 30;                        // 同键两次 press 至少
    std::map<int32_t, uint64_t> last_press_us;  // 键 -> 上次实际发送 press 的微秒时刻
    std::map<int32_t, uint64_t> last_release_us;
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

    ParseConfig(config, &impl_->port_name, &impl_->baud_rate,
                &impl_->min_hold_min_ms, &impl_->min_hold_max_ms, &impl_->min_gap_ms);

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

    // 尝试创建高精度定时器。失败不阻塞（降级为 Sleep）。
    impl_->h_timer = CreateWaitableTimerExW(
        nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (impl_->h_timer == nullptr) {
        std::fprintf(stderr, "[output] CreateWaitableTimerEx 失败，降级为 Sleep 精度\n");
    }

    // 初始化 mt19937 随机源
    impl_->rng.seed(static_cast<uint32_t>(GetTickCount64()));

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

            // 1. 抖动（脚本新批时）
            if (item.batch_id != 0 && item.batch_id != impl_->last_sent_script_batch_id) {
                std::uniform_int_distribution<uint32_t> dist(0, 33000);
                const uint32_t delay_us = dist(impl_->rng);
                if (delay_us > 0) {
                    WaitPreciseUs(impl_->h_timer, delay_us);
                }
                impl_->last_sent_script_batch_id = item.batch_id;
            }

            // 2. 最小间隔约束（仅脚本来源，基于 QPC）
            //    目标：确保本次发送的实际时刻 ≥ 上次同键相关动作 + 最小间隔。
            if (item.source == 0 && item.act.kind == CORE_ACTION_KEY) {
                const int32_t vk = item.act.a;
                const uint64_t now_us = NowUs();
                uint64_t earliest_us = 0;
                int hold_sample_ms = impl_->min_hold_min_ms;
                if (impl_->min_hold_max_ms > impl_->min_hold_min_ms) {
                    std::uniform_int_distribution<int> hd(
                        impl_->min_hold_min_ms, impl_->min_hold_max_ms);
                    hold_sample_ms = hd(impl_->rng);
                }
                if (item.act.b == 1) {  // press
                    auto it_r = impl_->last_release_us.find(vk);
                    if (it_r != impl_->last_release_us.end()) {
                        uint64_t cand = it_r->second + static_cast<uint64_t>(hold_sample_ms) * 1000ULL;
                        if (cand > earliest_us) earliest_us = cand;
                    }
                    auto it_p = impl_->last_press_us.find(vk);
                    if (it_p != impl_->last_press_us.end()) {
                        uint64_t cand = it_p->second + static_cast<uint64_t>(impl_->min_gap_ms) * 1000ULL;
                        if (cand > earliest_us) earliest_us = cand;
                    }
                } else if (item.act.b == 0) {  // release
                    auto it_p = impl_->last_press_us.find(vk);
                    if (it_p != impl_->last_press_us.end()) {
                        uint64_t cand = it_p->second + static_cast<uint64_t>(hold_sample_ms) * 1000ULL;
                        if (cand > earliest_us) earliest_us = cand;
                    }
                }
                if (earliest_us > now_us) {
                    WaitPreciseUs(impl_->h_timer, earliest_us - now_us);
                }
            }

            // 3. 记录约束状态（紧接在发送前，记录"实际发送时刻"）
            if (item.source == 0 && item.act.kind == CORE_ACTION_KEY) {
                const uint64_t t_us = NowUs();
                if (item.act.b == 1) {
                    impl_->last_press_us[item.act.a] = t_us;
                } else if (item.act.b == 0) {
                    impl_->last_release_us[item.act.a] = t_us;
                }
            }

            // 4. 发送记录
            if (sink != nullptr) {
                sink->RecordSend(item.source, item.act);
            }

            // 5. 实际发送
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
    if (impl_->h_timer != nullptr) {
        CloseHandle(impl_->h_timer);
        impl_->h_timer = nullptr;
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

    // 从脚本输出提取"当前意图按住的键"（只接受 press 动作）
    std::set<int> intent_now;
    for (uint32_t i = 0; i < decision->out_count; ++i) {
        const core_action& act = decision->actions[i];
        if (act.kind == CORE_ACTION_KEY && act.b == 1) {
            intent_now.insert(static_cast<int>(act.a));
        }
        // 脚本按新约定不输出 release，但容错：忽略所有 b == 0 的 KEY 动作
    }

    const uint64_t batch = ++impl_->script_batch_counter;

    // 差分之一：上一帧意图中消失的键 → release
    for (int vk : impl_->script_intent_keys) {
        if (intent_now.find(vk) == intent_now.end()) {
            QueuedAction qa;
            qa.act.kind = CORE_ACTION_KEY;
            qa.act.a = vk;
            qa.act.b = 0;
            qa.act.c = 0;
            qa.source = 0;
            qa.batch_id = batch;
            impl_->queue.push_back(qa);
            impl_->script_pressed_keys.erase(vk);
        }
    }

    // 差分二：本帧意图中新增的键 → press
    for (int vk : intent_now) {
        if (impl_->script_intent_keys.find(vk) == impl_->script_intent_keys.end()) {
            QueuedAction qa;
            qa.act.kind = CORE_ACTION_KEY;
            qa.act.a = vk;
            qa.act.b = 1;
            qa.act.c = 0;
            qa.source = 0;
            qa.batch_id = batch;
            impl_->queue.push_back(qa);
            impl_->script_pressed_keys.insert(vk);
        }
    }

    impl_->script_intent_keys = std::move(intent_now);
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
        impl_->script_intent_keys.clear();
        impl_->last_press_us.clear();
        impl_->last_release_us.clear();
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