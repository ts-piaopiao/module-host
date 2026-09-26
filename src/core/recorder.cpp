#include "recorder.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

using nlohmann::json;

namespace {

std::string NowWallClock() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string NowDirSuffix() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
    return buf;
}

bool CreateDirRecursive(const std::string& path) {
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return true;
    }
    std::string partial;
    partial.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        char c = path[i];
        partial.push_back(c);
        if ((c == '\\' || c == '/') && i > 0) {
            std::string segment = partial;
            while (!segment.empty() && (segment.back() == '\\' || segment.back() == '/')) {
                segment.pop_back();
            }
            if (segment.empty() || (segment.size() == 2 && segment[1] == ':')) continue;
            CreateDirectoryA(segment.c_str(), nullptr);
        }
    }
    CreateDirectoryA(path.c_str(), nullptr);
    attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// 高精度时间戳：微秒级，基于硬件计数器。
// 用于替代 GetTickCount64()——后者在未调 timeBeginPeriod 时分辨率仅 15.6ms。
uint64_t NowMicros() {
    static LARGE_INTEGER freq = []() {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // 转微秒，避免溢出：now * 1e6 / freq
    return static_cast<uint64_t>(
        (static_cast<double>(now.QuadPart) * 1e6) / static_cast<double>(freq.QuadPart));
}

std::string ParentDir(const std::string& path) {
    size_t pos = path.find_last_of("\\/");
    if (pos == std::string::npos) return {};
    return path.substr(0, pos);
}

void CleanupOldSessions(const std::string& record_dir) {
    WIN32_FIND_DATAA fd{};
    std::string pattern = record_dir;
    if (!pattern.empty() && pattern.back() != '\\' && pattern.back() != '/') {
        pattern += "\\";
    }
    pattern += "session_*";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    const ULONGLONG k7Days = 7ULL * 24 * 60 * 60 * 1000;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (std::string(fd.cFileName) == "." || std::string(fd.cFileName) == "..") continue;
        // FILETIME is 100ns since 1601; convert delta approx via system time comparison
        // Use directory path mtime via GetFileAttributesEx for simplicity
        std::string full = record_dir;
        if (!full.empty() && full.back() != '\\' && full.back() != '/') full += "\\";
        full += fd.cFileName;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExA(full.c_str(), GetFileExInfoStandard, &fad)) {
            ULARGE_INTEGER mt{};
            mt.LowPart = fad.ftLastWriteTime.dwLowDateTime;
            mt.HighPart = fad.ftLastWriteTime.dwHighDateTime;
            // Convert FILETIME to approximate ms since epoch for comparison
            // FILETIME epoch is 1601-01-01; Unix epoch offset in 100ns:
            const ULONGLONG kEpochDiff = 116444736000000000ULL;
            if (mt.QuadPart > kEpochDiff) {
                ULONGLONG ms = (mt.QuadPart - kEpochDiff) / 10000ULL;
                ULONGLONG nowMs = []() {
                    FILETIME ftNow{};
                    GetSystemTimeAsFileTime(&ftNow);
                    ULARGE_INTEGER u{};
                    u.LowPart = ftNow.dwLowDateTime;
                    u.HighPart = ftNow.dwHighDateTime;
                    const ULONGLONG kEpochDiff2 = 116444736000000000ULL;
                    if (u.QuadPart > kEpochDiff2) return (u.QuadPart - kEpochDiff2) / 10000ULL;
                    return 0ULL;
                }();
                if (nowMs > ms && (nowMs - ms) > k7Days) {
                    // recursive delete
                    std::string cmd = "cmd /c rmdir /s /q \"" + full + "\"";
                    system(cmd.c_str());
                }
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

}  // namespace

struct Recorder::Impl {
    std::mutex mutex;
    std::deque<std::string> queue;
    std::thread worker;
    std::atomic<bool> stop_flag{false};
    std::ofstream file;
    std::atomic<bool> started{false};

    static constexpr size_t kQueueMax = 10000;

    void Enqueue(json j) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!started.load(std::memory_order_relaxed)) return;
        queue.push_back(j.dump());
        if (queue.size() > kQueueMax) {
            queue.pop_front();
        }
    }

    void DrainOnce() {
        std::deque<std::string> batch;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!started.load(std::memory_order_relaxed) || !file.is_open()) return;
            size_t n = queue.size();
            if (n > 1000) n = 1000;
            for (size_t i = 0; i < n; ++i) {
                batch.push_back(std::move(queue.front()));
                queue.pop_front();
            }
        }
        for (auto& line : batch) {
            file << line << '\n';
        }
        if (!batch.empty()) {
            file.flush();
        }
    }

    void WorkerLoop() {
        while (!stop_flag.load(std::memory_order_relaxed)) {
            Sleep(100);
            DrainOnce();
        }
        // drain remaining
        for (;;) {
            std::string line;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (queue.empty()) break;
                line = std::move(queue.front());
                queue.pop_front();
            }
            if (file.is_open()) {
                file << line << '\n';
            }
        }
        if (file.is_open()) {
            file.flush();
        }
    }
};

Recorder::Recorder() : impl_(new Impl()) {}

Recorder::~Recorder() {
    Stop();
    delete impl_;
    impl_ = nullptr;
}

bool Recorder::Start(const std::string& record_dir) {
    if (impl_->started) return false;
    if (record_dir.empty()) return false;

    if (!CreateDirRecursive(record_dir)) return false;

    std::string session_dir = record_dir;
    if (!session_dir.empty() && session_dir.back() != '\\' && session_dir.back() != '/') {
        session_dir += "\\";
    }
    session_dir += "session_" + NowDirSuffix();
    if (!CreateDirRecursive(session_dir)) return false;

    std::string events_path = session_dir + "\\events.jsonl";
    impl_->file.open(events_path, std::ios::out | std::ios::trunc);
    if (!impl_->file.is_open()) return false;

    // meta.json
    try {
        json meta;
        meta["abi_version"] = CORE_ABI_VERSION;
        meta["start_time"] = GetTickCount64();
        meta["start_wall"] = NowWallClock();
        std::string meta_path = session_dir + "\\meta.json";
        std::ofstream mf(meta_path, std::ios::out | std::ios::trunc);
        if (mf.is_open()) {
            mf << meta.dump(2) << '\n';
            mf.close();
        }
    } catch (...) {
        // meta failure is non-fatal
    }

    impl_->stop_flag.store(false, std::memory_order_relaxed);
    impl_->started.store(true, std::memory_order_relaxed);
    impl_->worker = std::thread(&Recorder::Impl::WorkerLoop, impl_);

    CleanupOldSessions(record_dir);

    return true;
}

void Recorder::Stop() {
    if (!impl_->started) return;
    impl_->stop_flag.store(true, std::memory_order_relaxed);
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->file.is_open()) {
            impl_->file.flush();
            impl_->file.close();
        }
        impl_->started.store(false, std::memory_order_relaxed);
    }
}

void Recorder::RecordDetection(uint64_t frame, const core_detection& d) {
    json j;
    j["t"] = NowMicros();
    j["type"] = "det";
    j["frame"] = frame;
    j["cls"] = d.cls;
    j["id"] = d.track_id;
    j["conf"] = d.conf;
    j["cx"] = d.cx;
    j["cy"] = d.cy;
    j["w"] = d.w;
    j["h"] = d.h;
    impl_->Enqueue(std::move(j));
}

void Recorder::RecordDecision(uint64_t frame, bool me_valid,
                              float me_fx, float me_fy,
                              const core_decision* dec) {
    json j;
    j["t"] = NowMicros();
    j["type"] = "dec";
    j["frame"] = frame;
    j["me_valid"] = me_valid ? 1 : 0;
    j["me_fx"] = me_fx;
    j["me_fy"] = me_fy;
    json arr = json::array();
    if (dec != nullptr) {
        for (uint32_t i = 0; i < dec->out_count; ++i) {
            json a;
            a["kind"] = dec->actions[i].kind;
            a["a"] = dec->actions[i].a;
            a["b"] = dec->actions[i].b;
            a["c"] = dec->actions[i].c;
            arr.push_back(a);
        }
    }
    j["out"] = arr;
    impl_->Enqueue(std::move(j));
}

void Recorder::RecordHuman(uint64_t frame, const core_action& a) {
    json j;
    j["t"] = NowMicros();
    j["type"] = "hum";
    j["frame"] = frame;
    j["kind"] = a.kind;
    j["a"] = a.a;
    j["b"] = a.b;
    j["c"] = a.c;
    impl_->Enqueue(std::move(j));
}

void Recorder::RecordSend(uint64_t src, const core_action& a) {
    json j;
    j["t"] = NowMicros();
    j["type"] = "snd";
    j["src"] = src;
    j["kind"] = a.kind;
    j["a"] = a.a;
    j["b"] = a.b;
    j["c"] = a.c;
    impl_->Enqueue(std::move(j));
}

void Recorder::RecordError(const char* msg) {
    json j;
    j["t"] = NowMicros();
    j["type"] = "err";
    j["level"] = "ERR";
    j["msg"] = msg != nullptr ? msg : "";
    impl_->Enqueue(std::move(j));
}
