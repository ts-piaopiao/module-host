#include "core_contract.h"
#include <windows.h>
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>
#include <string>
#include <vector>
#include <cstdio>

static Ort::Env* g_env = nullptr;
static Ort::Session* g_session = nullptr;
static std::string g_model_path;

static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

static std::string GetConfigValue(const char* config, const char* key) {
    if (config == nullptr || key == nullptr) {
        return {};
    }
    const std::string cfg(config);
    const std::string k(key);
    size_t pos = cfg.find(k);
    if (pos == std::string::npos) {
        return {};
    }
    pos += k.size();
    if (pos < cfg.size() && cfg[pos] == '=') {
        ++pos;
    } else if (pos + 1 < cfg.size() && cfg[pos] == ' ' && cfg[pos + 1] == '=') {
        pos += 2;
    } else if (pos < cfg.size() && cfg[pos] == ' ') {
        ++pos;
    }
    while (pos < cfg.size() && (cfg[pos] == ' ' || cfg[pos] == '\t')) {
        ++pos;
    }
    size_t end = pos;
    while (end < cfg.size() && cfg[end] != '\n' && cfg[end] != '\r') {
        ++end;
    }
    std::string val = cfg.substr(pos, end - pos);
    while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) {
        val.pop_back();
    }
    return val;
}

extern "C" {

const char* plugin_meta(void) {
    return "yolo_policy|0.1.0|3|policy";
}

core_error plugin_init(uint32_t host_abi, const char* config) {
    if (host_abi != 3) {
        return CORE_ERR_ABI_MISMATCH;
    }

    g_model_path = GetConfigValue(config, "policy_model_path");
    if (g_model_path.empty()) {
        g_model_path = "D:\\dev\\module-host\\models\\yolo11s.onnx";
    }

    if (g_env == nullptr) {
        g_env = new Ort::Env(nullptr, ORT_LOGGING_LEVEL_WARNING, "yolo_policy");
    }

    Ort::SessionOptions options;
    OrtStatus* dml_status = OrtSessionOptionsAppendExecutionProvider_DML(options, 0);
    if (dml_status != nullptr) {
        Ort::Status st(dml_status);
        fprintf(stderr, "[yolo] DirectML 不可用，回退 CPU: %s\n", st.GetErrorMessage().c_str());
    }

    try {
        const std::wstring wpath = ToWide(g_model_path);
        g_session = new Ort::Session(*g_env, wpath.c_str(), options);
    } catch (const std::exception& e) {
        fprintf(stderr, "[yolo] 模型加载失败: %s (%s)\n", g_model_path.c_str(), e.what());
        return CORE_ERR_INIT;
    }

    Ort::AllocatorWithDefaultOptions allocator;
    fprintf(stderr, "[yolo] 模型加载成功: %s\n", g_model_path.c_str());

    const size_t num_inputs = g_session->GetInputCount();
    fprintf(stderr, "[yolo] 输入数量: %zu\n", num_inputs);
    for (size_t i = 0; i < num_inputs; ++i) {
        auto info = g_session->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo();
        auto shape = info.GetShape();
        std::string shape_str = "[";
        for (size_t j = 0; j < shape.size(); ++j) {
            if (j > 0) shape_str += ", ";
            shape_str += std::to_string(shape[j]);
        }
        shape_str += "]";
        fprintf(stderr, "[yolo] 输入 %zu shape: %s\n", i, shape_str.c_str());
        char* name = g_session->GetInputNameAllocated(i, allocator).release();
        if (name != nullptr) {
            allocator.Free(name);
        }
    }

    const size_t num_outputs = g_session->GetOutputCount();
    fprintf(stderr, "[yolo] 输出数量: %zu\n", num_outputs);
    for (size_t i = 0; i < num_outputs; ++i) {
        auto info = g_session->GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo();
        auto shape = info.GetShape();
        std::string shape_str = "[";
        for (size_t j = 0; j < shape.size(); ++j) {
            if (j > 0) shape_str += ", ";
            shape_str += std::to_string(shape[j]);
        }
        shape_str += "]";
        fprintf(stderr, "[yolo] 输出 %zu shape: %s\n", i, shape_str.c_str());
        char* name = g_session->GetOutputNameAllocated(i, allocator).release();
        if (name != nullptr) {
            allocator.Free(name);
        }
    }

    return CORE_OK;
}

core_error plugin_release(void) {
    delete g_session;
    g_session = nullptr;
    delete g_env;
    g_env = nullptr;
    return CORE_OK;
}

core_error plugin_capture(core_frame* out) {
    (void)out;
    return CORE_OK;
}

core_error plugin_decide(const core_intent* intent, core_decision* out) {
    (void)intent;
    (void)out;
    return CORE_OK;
}

core_error plugin_execute(const core_decision* decision, core_execute_result* out) {
    (void)decision;
    (void)out;
    return CORE_OK;
}

}  // extern "C"
