#include <windows.h>

#include "core_contract.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char* const kDllNames[] = {
    "capture_plugin.dll",
    "policy_plugin.dll",
    "input_plugin.dll",
};

const char* const kExpectedKinds[] = {
    "capture",
    "policy",
    "input",
};

constexpr int kDllCount = 3;

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
using PluginInitFn = core_error (*)(uint32_t);
using PluginReleaseFn = core_error (*)(void);
using PluginCaptureFn = core_error (*)(core_frame*);
using PluginDecideFn = core_error (*)(const core_intent*, core_decision*);
using PluginExecuteFn = core_error (*)(const core_decision*);

struct PluginState {
    HMODULE module = nullptr;
    PluginReleaseFn release = nullptr;
    PluginCaptureFn capture = nullptr;
    PluginDecideFn decide = nullptr;
    PluginExecuteFn execute = nullptr;
    bool inited = false;
};

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
            std::printf("[错误] 缺失符号: %s\n", kSymbolNames[i]);
            return false;
        }
    }
    return true;
}

bool ParseAndCheckMeta(const char* meta, const char* expected_kind, std::string* out_name) {
    if (meta == nullptr) {
        std::printf("[错误] 元数据无效: NULL\n");
        return false;
    }

    const std::vector<std::string> parts = SplitByPipe(std::string(meta));
    if (parts.size() != 4) {
        std::printf("[错误] 元数据无效: 段数不是 4\n");
        return false;
    }

    const std::string& name = parts[0];
    const std::string& version = parts[1];
    const std::string& abi = parts[2];
    const std::string& kind = parts[3];

    if (name.size() > CORE_PLUGIN_META_NAME_MAX || name.find(CORE_PLUGIN_META_SEP) != std::string::npos) {
        std::printf("[错误] 元数据无效: name 非法\n");
        return false;
    }

    if (version.size() > CORE_PLUGIN_META_VERSION_MAX || version.find(CORE_PLUGIN_META_SEP) != std::string::npos) {
        std::printf("[错误] 元数据无效: version 非法\n");
        return false;
    }

    if (!AllDigits(abi)) {
        std::printf("[错误] 元数据无效: ABI 非数字\n");
        return false;
    }

    const unsigned long long abi_value = ParseDecimal(abi);
    if (abi_value != static_cast<unsigned long long>(CORE_ABI_VERSION)) {
        std::printf("[错误] ABI 不匹配: 期望 %d 实际 %llu\n", CORE_ABI_VERSION, abi_value);
        return false;
    }

    if (kind != expected_kind) {
        std::printf("[错误] 元数据无效: kind 不匹配\n");
        return false;
    }

    *out_name = name;
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    bool show_help = false;
    bool show_version = false;
    std::string plugins_dir;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            show_help = true;
        } else if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            show_version = true;
        } else if (std::strcmp(argv[i], "--plugins-dir") == 0) {
            if (i + 1 >= argc) {
                std::printf("[错误] 缺失参数: --plugins-dir\n");
                return 1;
            }
            plugins_dir = argv[++i];
        }
    }

    if (show_help) {
        std::printf("module-host core\n");
        std::printf("用法: core.exe --plugins-dir <目录>\n");
        std::printf("选项:\n");
        std::printf("  --plugins-dir <目录>   指定插件目录\n");
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
        return 0;
    }

    if (plugins_dir.empty()) {
        std::printf("[错误] 缺失参数: --plugins-dir\n");
        return 1;
    }

    PluginState states[kDllCount] = {};

    for (int i = 0; i < kDllCount; ++i) {
        const std::string path = JoinPath(plugins_dir, kDllNames[i]);
        states[i].module = LoadLibraryA(path.c_str());
        if (states[i].module == nullptr) {
            std::printf("[错误] 缺失 DLL: %s\n", kDllNames[i]);
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

        if (init_fn(CORE_ABI_VERSION) != CORE_OK) {
            std::printf("[错误] 初始化失败: %s\n", name.c_str());
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
    core_decision decision = {};

    for (int i = 1; i <= 5; ++i) {
        std::printf("[帧 %d] 起始\n", i);

        if (states[0].capture(&frame) != CORE_OK) {
            std::printf("[错误] 捕获失败\n");
            Cleanup(states, kDllCount);
            return 1;
        }

        if (states[1].decide(&intent, &decision) != CORE_OK) {
            std::printf("[错误] 决策失败\n");
            Cleanup(states, kDllCount);
            return 1;
        }

        if (decision.out_count > CORE_DECISION_CAPACITY) {
            std::printf("[错误] out_count 违约: %u > %d\n",
                        decision.out_count, CORE_DECISION_CAPACITY);
            Cleanup(states, kDllCount);
            return 1;
        }

        if (decision.out_count == 0) {
            std::printf("[内核] 无意图\n");
        }

        for (uint32_t item = 0; item < decision.out_count; ++item) {
            if (states[2].execute(&decision) != CORE_OK) {
                std::printf("[错误] 执行失败\n");
                Cleanup(states, kDllCount);
                return 1;
            }
        }
    }

    std::printf("[内核] 5 帧完成\n");
    Cleanup(states, kDllCount);
    return 0;
#else
    std::printf("[内核] 加载成功\n");
    Cleanup(states, kDllCount);
    return 0;
#endif
}
