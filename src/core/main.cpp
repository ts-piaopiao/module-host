#include <windows.h>

#include "core_contract.h"
#include "config.h"
#include "remote_server.h"
#include "script/script_host.h"
#include "recorder.h"
#include "output_manager.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

std::ofstream g_log_file;

const char* const kDllNames[] = {
    "capture_plugin.dll",
    "policy_plugin.dll",
};

const char* const kExpectedKinds[] = {
    "capture",
    "policy",
};

constexpr int kDllCount = 2;

const char* const kSymbolNames[] = {
    "plugin_meta",
    "plugin_init",
    "plugin_release",
    "plugin_capture",
    "plugin_decide",
    "plugin_execute",
};

constexpr int kSymbolCount = 6;

using PluginMetaFn = const char* (*)(void);
using PluginInitFn = core_error (*)(uint32_t, const char*);
using PluginReleaseFn = core_error (*)(void);
using PluginCaptureFn = core_error (*)(core_frame*);
using PluginDecideFn = core_error (*)(const core_intent*, core_decision*);
using PluginExecuteFn = core_error (*)(const core_decision*, core_execute_result*);

struct PluginState {
    HMODULE module = nullptr;
    PluginReleaseFn release = nullptr;
    PluginCaptureFn capture = nullptr;
    PluginDecideFn decide = nullptr;
    PluginExecuteFn execute = nullptr;
    bool inited = false;
};

void LogPrintf(const char* fmt, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (written <= 0) {
        return;
    }
    const size_t len = (static_cast<size_t>(written) < sizeof(buffer))
                           ? static_cast<size_t>(written)
                           : sizeof(buffer) - 1;
    std::fwrite(buffer, 1, len, stdout);
    if (g_log_file.is_open()) {
        g_log_file.write(buffer, static_cast<std::streamsize>(len));
    }
    std::fflush(stdout);
    if (g_log_file.is_open()) {
        g_log_file.flush();
    }
}

std::string JoinPath(const std::string& dir, const char* name) {
    std::string path = dir;
    if (!path.empty() && path.back() != '\\' && path.back() != '/') {
        path += '\\';
    }
    path += name;
    return path;
}

std::vector<std::string> SplitByPipe(const std::string& input) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : input) {
        if (ch == CORE_PLUGIN_META_SEP) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(current);
    return parts;
}

bool AllDigits(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return true;
}

unsigned long long ParseDecimal(const std::string& text) {
    unsigned long long value = 0;
    for (char ch : text) {
        value = value * 10ULL + static_cast<unsigned long long>(ch - '0');
    }
    return value;
}

void Cleanup(PluginState* states, int count) {
    for (int i = count - 1; i >= 0; --i) {
        if (states[i].inited && states[i].release != nullptr) {
            states[i].release();
            states[i].inited = false;
        }
    }
    for (int i = count - 1; i >= 0; --i) {
        if (states[i].module != nullptr) {
            FreeLibrary(states[i].module);
            states[i].module = nullptr;
        }
    }
}

bool LoadSymbols(HMODULE module, FARPROC* symbols) {
    for (int i = 0; i < kSymbolCount; ++i) {
        symbols[i] = GetProcAddress(module, kSymbolNames[i]);
        if (symbols[i] == nullptr) {
            LogPrintf("[错误] 缺失符号: %s\n", kSymbolNames[i]);
            return false;
        }
    }
    return true;
}

bool ParseAndCheckMeta(const char* meta, const char* expected_kind, std::string* out_name) {
    if (meta == nullptr) {
        LogPrintf("[错误] 元数据无效: NULL\n");
        return false;
    }

    const std::vector<std::string> parts = SplitByPipe(std::string(meta));
    if (parts.size() != 4) {
        LogPrintf("[错误] 元数据无效: 段数不是 4\n");
        return false;
    }

    const std::string& name = parts[0];
    const std::string& version = parts[1];
    const std::string& abi = parts[2];
    const std::string& kind = parts[3];

    if (name.size() > CORE_PLUGIN_META_NAME_MAX || name.find(CORE_PLUGIN_META_SEP) != std::string::npos) {
        LogPrintf("[错误] 元数据无效: name 非法\n");
        return false;
    }

    if (version.size() > CORE_PLUGIN_META_VERSION_MAX || version.find(CORE_PLUGIN_META_SEP) != std::string::npos) {
        LogPrintf("[错误] 元数据无效: version 非法\n");
        return false;
    }

    if (!AllDigits(abi)) {
        LogPrintf("[错误] 元数据无效: ABI 非数字\n");
        return false;
    }

    const unsigned long long abi_value = ParseDecimal(abi);
    if (abi_value != static_cast<unsigned long long>(CORE_ABI_VERSION)) {
        LogPrintf("[错误] ABI 不匹配: 期望 %d 实际 %llu\n", CORE_ABI_VERSION, abi_value);
        return false;
    }

    if (kind != expected_kind) {
        LogPrintf("[错误] 元数据无效: kind 不匹配\n");
        return false;
    }

    *out_name = name;
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::unique_ptr<RemoteServer> remote;
    struct RecorderDeleter {
        void operator()(Recorder* r) const {
            if (r) { r->Stop(); delete r; }
        }
    };
    std::unique_ptr<Recorder, RecorderDeleter> recorder;
    bool show_help = false;
    bool show_version = false;
    std::string plugins_dir;
    std::string config_path;
    std::string record_path;
    std::string input_port_override;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            show_help = true;
        } else if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            show_version = true;
        } else if (std::strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                LogPrintf("[错误] 缺失参数: --config\n");
                return 1;
            }
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--plugins-dir") == 0) {
            if (i + 1 >= argc) {
                LogPrintf("[错误] 缺失参数: --plugins-dir\n");
                return 1;
            }
            plugins_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--record") == 0) {
            if (i + 1 >= argc) {
                LogPrintf("[错误] 缺失参数: --record\n");
                return 1;
            }
            record_path = argv[++i];
        } else if (std::strcmp(argv[i], "--input-port") == 0) {
            if (i + 1 >= argc) {
                LogPrintf("[错误] 缺失参数: --input-port\n");
                return 1;
            }
            input_port_override = argv[++i];
        }
    }

    if (show_help) {
        std::printf("module-host core\n");
        std::printf("用法: core.exe --plugins-dir <目录>\n");
        std::printf("选项:\n");
        std::printf("  --plugins-dir <目录>   指定插件目录\n");
        std::printf("  --config <路径>       指定配置文件\n");
        std::printf("  --record <路径>       记录每帧检测与决策到 JSONL\n");
        std::printf("  --input-port <端口>   指定串口，none 表示不打开串口（mock 模式）\n");
        std::printf("  --help, -h            显示本帮助\n");
        std::printf("  --version, -v         显示版本\n");
        return 0;
    }

    if (show_version) {
        std::printf("module-host core\n");
        std::printf("ABI 版本: %d\n", CORE_ABI_VERSION);
#if defined(BUILD_STAGE2_PLUGINS) && BUILD_STAGE2_PLUGINS
        std::printf("当前构建: STAGE2=ON\n");
#else
        std::printf("当前构建: STAGE2=OFF\n");
#endif
        std::printf("构建配置: --config 可用\n");
        return 0;
    }

    CoreConfig config;
    if (!config_path.empty()) {
        std::string config_err;
        if (!LoadConfigFile(config_path, &config, &config_err)) {
            LogPrintf("[错误] 配置无效: %s\n", config_err.c_str());
            return 1;
        }
    }

    std::string raw_config;
    if (!config_path.empty()) {
        std::ifstream raw_in(config_path, std::ios::in | std::ios::binary);
        if (!raw_in.is_open()) {
            LogPrintf("[错误] 读取配置失败: %s\n", config_path.c_str());
            return 1;
        }
        raw_config.assign(std::istreambuf_iterator<char>(raw_in),
                          std::istreambuf_iterator<char>());
    }

    if (!input_port_override.empty()) {
        if (!raw_config.empty() && raw_config.back() != '\n') {
            raw_config += '\n';
        }
        raw_config += "input_port = " + input_port_override + "\n";
    }

    if (config.has_log_path && !config.log_path.empty()) {
        g_log_file.open(config.log_path, std::ios::out | std::ios::trunc);
        if (!g_log_file.is_open()) {
            std::printf("[错误] 日志打开失败: %s\n", config.log_path.c_str());
            return 1;
        }
    }

    if (!record_path.empty()) {
        recorder.reset(new Recorder());
        if (!recorder->Start(record_path)) {
            LogPrintf("[错误] 记录启动失败: %s\n", record_path.c_str());
            recorder.reset();
            // 不退出，只是不记录
        } else {
            LogPrintf("[内核] 记录已启动: %s\n", record_path.c_str());
        }
    }

    std::string effective_plugins_dir = plugins_dir;
    if (effective_plugins_dir.empty() && config.has_plugins_dir) {
        effective_plugins_dir = config.plugins_dir;
    }

    if (effective_plugins_dir.empty()) {
        LogPrintf("[错误] 缺失参数: --plugins-dir\n");
        return 1;
    }

    const int frame_count = config.has_frames ? config.frames : 5;

    if (config.remote_port != 0) {
        remote = std::make_unique<RemoteServer>();
        if (!remote->Start(config.remote_port, config.remote_jpeg_quality)) {
            LogPrintf("[错误] 远程端口启动失败: %d\n", config.remote_port);
            return 1;
        }
        LogPrintf("[内核] 远程监听: 端口 %d\n", config.remote_port);
    }

    OutputManager output_manager;
    if (recorder) {
        output_manager.SetSendSink(recorder.get());
    }
    if (!output_manager.Start(raw_config)) {
        LogPrintf("[错误] 串口打开失败\n");
        return 1;
    }
    LogPrintf("[内核] 输出已启动\n");

    PluginState states[kDllCount] = {};

    for (int i = 0; i < kDllCount; ++i) {
        const std::string path = JoinPath(effective_plugins_dir, kDllNames[i]);
        states[i].module = LoadLibraryA(path.c_str());
        if (states[i].module == nullptr) {
            LogPrintf("[错误] 缺失 DLL: %s\n", kDllNames[i]);
            Cleanup(states, kDllCount);
            return 1;
        }

        FARPROC symbols[kSymbolCount] = {};
        if (!LoadSymbols(states[i].module, symbols)) {
            Cleanup(states, kDllCount);
            return 1;
        }

        const PluginMetaFn meta_fn = reinterpret_cast<PluginMetaFn>(symbols[0]);
        const PluginInitFn init_fn = reinterpret_cast<PluginInitFn>(symbols[1]);
        const PluginReleaseFn release_fn = reinterpret_cast<PluginReleaseFn>(symbols[2]);
        const PluginCaptureFn capture_fn = reinterpret_cast<PluginCaptureFn>(symbols[3]);
        const PluginDecideFn decide_fn = reinterpret_cast<PluginDecideFn>(symbols[4]);
        const PluginExecuteFn execute_fn = reinterpret_cast<PluginExecuteFn>(symbols[5]);

        std::string name;
        if (!ParseAndCheckMeta(meta_fn(), kExpectedKinds[i], &name)) {
            Cleanup(states, kDllCount);
            return 1;
        }

        if (init_fn(CORE_ABI_VERSION, raw_config.c_str()) != CORE_OK) {
            LogPrintf("[错误] 初始化失败: %s\n", name.c_str());
            Cleanup(states, kDllCount);
            return 1;
        }

        states[i].release = release_fn;
        states[i].capture = capture_fn;
        states[i].decide = decide_fn;
        states[i].execute = execute_fn;
        states[i].inited = true;
    }

#if defined(BUILD_STAGE2_PLUGINS) && BUILD_STAGE2_PLUGINS
    core_frame frame = {};
    core_intent intent = {};
    core_decision decision_policy = {};
    core_detections detections = {};
    ScriptHost script_host;
    if (!script_host.Start(raw_config)) {
        LogPrintf("[错误] 脚本启动失败\n");
        Cleanup(states, kDllCount);
        return 1;
    }

    // 远程输出直通 OutputManager
    if (remote) {
        remote->SetOutputSink(&output_manager);
    }

    int frame_index = 0;
    for (;;) {
        ++frame_index;
        if (frame_count > 0 && frame_index > frame_count) break;
        LogPrintf("[帧 %d] 起始\n", frame_index);
        detections.count = 0;

        if (states[0].capture(&frame) != CORE_OK) {
            LogPrintf("[错误] 捕获失败\n");
            Cleanup(states, kDllCount);
            return 1;
        }

        if (remote && frame.data != nullptr && frame.width > 0 && frame.height > 0 &&
            frame.format == CORE_PIXEL_FORMAT_BGRA8) {
            remote->PushFrame(frame.data, frame.width, frame.height);
        }

        intent.param1 = 0;
        intent.frame = &frame;
        intent.detections_out = &detections;   // 新增

        if (states[1].decide(&intent, &decision_policy) != CORE_OK) {
            LogPrintf("[错误] 决策失败\n");
            Cleanup(states, kDllCount);
            return 1;
        }
        if (decision_policy.out_count > CORE_DECISION_CAPACITY) {
            LogPrintf("[错误] out_count 违约: %u > %d\n",
                      decision_policy.out_count, CORE_DECISION_CAPACITY);
            Cleanup(states, kDllCount);
            return 1;
        }

        if (recorder) {
            for (uint32_t di = 0; di < detections.count; ++di) {
                recorder->RecordDetection(static_cast<uint64_t>(frame_index), detections.items[di]);
            }
        }

        float me_fx = 0.0f, me_fy = 0.0f;
        const bool me_valid = script_host.GetMeLock(&me_fx, &me_fy);

        core_decision decision_decided = {};
        script_host.OnFrame(static_cast<uint64_t>(frame_index), GetTickCount64(),
                            me_valid, me_fx, me_fy, &detections);
        script_host.GetDecision(&decision_decided);

        if (recorder) {
            recorder->RecordDecision(static_cast<uint64_t>(frame_index),
                                     me_valid, me_fx, me_fy,
                                     &decision_decided);
        }

        // 人类事件只用于 recorder 记录（不回灌到 decision）
        std::vector<core_action> events;
        if (remote) {
            events = remote->PopHumanEvents();
        }

        if (recorder) {
            for (const auto& ev : events) {
                recorder->RecordHuman(static_cast<uint64_t>(frame_index), ev);
            }
        }

        // 脚本决策直接走 SendScript（human 走 SendAsync，两条独立路径）
        output_manager.SendScript(&decision_decided);

        if (decision_decided.out_count == 0) {
            LogPrintf("[内核] 无意图\n");
        }
    }

    if (frame_count > 0) {
        LogPrintf("[内核] %d 帧完成\n", frame_count);
    } else {
        LogPrintf("[内核] 无限模式结束\n");
    }
    script_host.Stop();
    output_manager.Stop();
    Cleanup(states, kDllCount);
    return 0;
#else
    LogPrintf("[内核] 加载成功\n");
    if (config.remote_port != 0) {
        LogPrintf("[内核] 远程模式驻留中，按 Ctrl+C 退出\n");
        for (;;) {
            Sleep(1000);
            const std::vector<core_action> events = remote->PopHumanEvents();
            for (const core_action& act : events) {
                const char* kind_name = "unknown";
                switch (act.kind) {
                case CORE_ACTION_KEY: kind_name = "KEY"; break;
                case CORE_ACTION_POINTER_MOVE: kind_name = "MOVE"; break;
                case CORE_ACTION_POINTER_BUTTON: kind_name = "BUTTON"; break;
                default: break;
                }
                LogPrintf("[远程] kind=%s a=%d b=%d c=%d\n",
                          kind_name, act.a, act.b, act.c);
            }
        }
    }
    Cleanup(states, kDllCount);
    return 0;
#endif
}
