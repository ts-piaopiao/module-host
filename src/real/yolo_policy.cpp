#include "core_contract.h"
#include <windows.h>
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <cstdio>

static Ort::Env* g_env = nullptr;
static Ort::Session* g_session = nullptr;
static std::string g_model_path;
static std::string g_input_name;
static std::string g_output_name;
static uint64_t g_last_input_hash = 0;

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

static void PreprocessFrame(const core_frame* frame,
                            uint32_t input_size,
                            std::vector<float>& out_tensor,
                            float* out_scale,
                            int* out_dw,
                            int* out_dh)
{
    const uint32_t W = frame->width;
    const uint32_t H = frame->height;
    const uint32_t S = input_size;

    const float r = (std::min)(static_cast<float>(S) / W, static_cast<float>(S) / H);
    const int new_w = static_cast<int>(W * r + 0.5f);
    const int new_h = static_cast<int>(H * r + 0.5f);
    const int dw = (static_cast<int>(S) - new_w) / 2;
    const int dh = (static_cast<int>(S) - new_h) / 2;

    *out_scale = r;
    *out_dw = dw;
    *out_dh = dh;

    const float pad = 114.0f / 255.0f;
    out_tensor.assign(3 * S * S, pad);

    const uint8_t* base = frame->data;
    const uint32_t stride = frame->stride;

    for (int y = 0; y < new_h; ++y) {
        const int sy = (std::min)(static_cast<int>(y / r), static_cast<int>(H) - 1);
        const uint8_t* src_row = base + sy * stride;
        const int oy = y + dh;
        for (int x = 0; x < new_w; ++x) {
            const int sx = (std::min)(static_cast<int>(x / r), static_cast<int>(W) - 1);
            const uint8_t* p = src_row + sx * 4;
            const float b = p[0] / 255.0f;
            const float g = p[1] / 255.0f;
            const float rch = p[2] / 255.0f;
            const int ox = x + dw;
            out_tensor[0 * S * S + oy * S + ox] = b;
            out_tensor[1 * S * S + oy * S + ox] = g;
            out_tensor[2 * S * S + oy * S + ox] = rch;
        }
    }

    std::fprintf(stderr, "[yolo] sample: B[0]=%.4f G[0]=%.4f R[0]=%.4f B[mid]=%.4f G[mid]=%.4f R[mid]=%.4f\n",
                 out_tensor[0], out_tensor[S * S],
                 out_tensor[2 * S * S],
                 out_tensor[S * S / 2],
                 out_tensor[S * S + S * S / 2],
                 out_tensor[2 * S * S + S * S / 2]);
}

static void PrintInputStats(const std::vector<float>& tensor)
{
    float mn = tensor[0], mx = tensor[0];
    double sum = 0.0;
    for (float v : tensor) {
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum += v;
    }
    const double mean = sum / tensor.size();
    std::fprintf(stderr, "[yolo] 输入张量: size=%zu min=%.4f max=%.4f mean=%.4f\n",
                 tensor.size(), mn, mx, mean);
}

static void PrintOutputStats(const float* data, size_t count)
{
    float mn = data[0], mx = data[0];
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        if (data[i] < mn) mn = data[i];
        if (data[i] > mx) mx = data[i];
        sum += data[i];
    }
    const double mean = sum / count;
    std::fprintf(stderr, "[yolo] 输出张量: count=%zu min=%.4f max=%.4f mean=%.4f\n",
                 count, mn, mx, mean);
    std::fprintf(stderr, "[yolo] 输出前 12 个值:");
    for (size_t i = 0; i < 12 && i < count; ++i) {
        std::fprintf(stderr, " %.4f", data[i]);
    }
    std::fprintf(stderr, "\n");
}

static uint64_t ComputeTensorHash(const std::vector<float>& tensor) {
    uint64_t hash = 1469598103934665603ULL;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(tensor.data());
    const size_t n = tensor.size() * sizeof(float);
    for (size_t i = 0; i < n; i += 64) {
        hash ^= p[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct Detection {
    int cls;
    float conf;
    float cx, cy;
    float w, h;
    int track_id = 0;
};

static float IoU(const Detection& a, const Detection& b) {
    const float ax0 = a.cx - a.w * 0.5f;
    const float ay0 = a.cy - a.h * 0.5f;
    const float ax1 = a.cx + a.w * 0.5f;
    const float ay1 = a.cy + a.h * 0.5f;
    const float bx0 = b.cx - b.w * 0.5f;
    const float by0 = b.cy - b.h * 0.5f;
    const float bx1 = b.cx + b.w * 0.5f;
    const float by1 = b.cy + b.h * 0.5f;
    const float ix0 = (std::max)(ax0, bx0);
    const float iy0 = (std::max)(ay0, by0);
    const float ix1 = (std::min)(ax1, bx1);
    const float iy1 = (std::min)(ay1, by1);
    const float iw = (ix1 > ix0) ? (ix1 - ix0) : 0.0f;
    const float ih = (iy1 > iy0) ? (iy1 - iy0) : 0.0f;
    const float inter = iw * ih;
    const float area_a = a.w * a.h;
    const float area_b = b.w * b.h;
    const float uni = area_a + area_b - inter;
    if (uni <= 0.0f) return 0.0f;
    return inter / uni;
}

struct Track {
    int id = 0;
    int cls = 0;
    float cx = 0, cy = 0, w = 0, h = 0;
    int lost_frames = 0;
    int age = 0;
};

static float IoU(const Track& t, const Detection& d) {
    const float t_x1 = t.cx - t.w / 2, t_y1 = t.cy - t.h / 2;
    const float t_x2 = t.cx + t.w / 2, t_y2 = t.cy + t.h / 2;
    const float d_x1 = d.cx - d.w / 2, d_y1 = d.cy - d.h / 2;
    const float d_x2 = d.cx + d.w / 2, d_y2 = d.cy + d.h / 2;
    const float ix1 = (std::max)(t_x1, d_x1);
    const float iy1 = (std::max)(t_y1, d_y1);
    const float ix2 = (std::min)(t_x2, d_x2);
    const float iy2 = (std::min)(t_y2, d_y2);
    const float iw = ix2 - ix1;
    const float ih = iy2 - iy1;
    if (iw <= 0 || ih <= 0) return 0.0f;
    const float inter = iw * ih;
    const float area_t = t.w * t.h;
    const float area_d = d.w * d.h;
    return inter / (area_t + area_d - inter + 1e-6f);
}

class Tracker {
public:
    void Update(std::vector<Detection>& dets) {
        for (auto& t : tracks_) t.lost_frames += 1;

        for (auto& d : dets) {
            int best_idx = -1;
            float best_iou = 0.3f;
            for (size_t i = 0; i < tracks_.size(); ++i) {
                if (tracks_[i].cls != d.cls) continue;
                const float iou = IoU(tracks_[i], d);
                if (iou > best_iou) {
                    best_iou = iou;
                    best_idx = (int)i;
                }
            }
            if (best_idx >= 0) {
                auto& t = tracks_[best_idx];
                t.cx = d.cx; t.cy = d.cy; t.w = d.w; t.h = d.h;
                t.lost_frames = 0;
                t.age += 1;
                d.track_id = t.id;
            } else {
                Track t;
                t.id = next_id_++;
                t.cls = d.cls;
                t.cx = d.cx; t.cy = d.cy; t.w = d.w; t.h = d.h;
                t.lost_frames = 0;
                t.age = 1;
                tracks_.push_back(t);
                d.track_id = t.id;
            }
        }

        tracks_.erase(
            std::remove_if(tracks_.begin(), tracks_.end(),
                [](const Track& t) { return t.lost_frames > 30; }),
            tracks_.end());
    }

private:
    std::vector<Track> tracks_;
    int next_id_ = 1;
};

static Tracker g_tracker;

static void PostprocessDetections(
    const float* out_data,
    size_t num_classes,
    size_t num_anchors,
    float conf_thr,
    float iou_thr,
    float scale,
    int dw, int dh,
    uint32_t orig_w, uint32_t orig_h,
    std::vector<Detection>& out)
{
    out.clear();
    std::vector<Detection> candidates;
    for (size_t i = 0; i < num_anchors; ++i) {
        float cx = out_data[0 * num_anchors + i];
        float cy = out_data[1 * num_anchors + i];
        float w  = out_data[2 * num_anchors + i];
        float h  = out_data[3 * num_anchors + i];

        int best_cls = -1;
        float best_conf = 0.0f;
        for (size_t c = 0; c < num_classes; ++c) {
            float s = out_data[(4 + c) * num_anchors + i];
            if (s > best_conf) {
                best_conf = s;
                best_cls = (int)c;
            }
        }
        if (best_conf < conf_thr) continue;

        float cx_orig = (cx - dw) / scale;
        float cy_orig = (cy - dh) / scale;
        float w_orig  = w / scale;
        float h_orig  = h / scale;

        Detection d;
        d.cls = best_cls;
        d.conf = best_conf;
        d.cx = cx_orig / orig_w;
        d.cy = cy_orig / orig_h;
        d.w  = w_orig  / orig_w;
        d.h  = h_orig  / orig_h;

        if (d.cx < 0 || d.cx > 1 || d.cy < 0 || d.cy > 1) continue;

        const float min_px = 30.0f / 640.0f;
        if (d.w < min_px && d.h < min_px) continue;

        candidates.push_back(d);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Detection& a, const Detection& b) { return a.conf > b.conf; });

    std::vector<bool> suppressed(candidates.size(), false);
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (suppressed[i]) continue;
        out.push_back(candidates[i]);
        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (suppressed[j]) continue;
            if (candidates[j].cls != candidates[i].cls) continue;
            if (IoU(candidates[i], candidates[j]) > iou_thr) {
                suppressed[j] = true;
            }
        }
    }
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
    const size_t num_outputs = g_session->GetOutputCount();
    if (num_inputs > 0) {
        g_input_name = g_session->GetInputNameAllocated(0, allocator).get();
    }
    if (num_outputs > 0) {
        g_output_name = g_session->GetOutputNameAllocated(0, allocator).get();
    }
    fprintf(stderr, "[yolo] 输入名: %s\n", g_input_name.c_str());
    fprintf(stderr, "[yolo] 输出名: %s\n", g_output_name.c_str());

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
    if (out == nullptr) {
        return CORE_ERR_DECIDE;
    }
    out->out_count = 0;

    if (g_session == nullptr || g_env == nullptr) {
        return CORE_ERR_DECIDE;
    }
    if (intent == nullptr || intent->frame == nullptr) {
        std::fprintf(stderr, "[yolo] decide: intent 或 frame 为空，跳过推理\n");
        return CORE_OK;
    }
    const core_frame* f = intent->frame;
    if (f->data == nullptr || f->width == 0 || f->height == 0 ||
        f->format != CORE_PIXEL_FORMAT_BGRA8) {
        std::fprintf(stderr, "[yolo] decide: frame 无效\n");
        return CORE_OK;
    }

    static constexpr uint32_t kInputSize = 640;
    std::vector<float> input_tensor;
    float scale = 0.0f;
    int dw = 0, dh = 0;
    PreprocessFrame(f, kInputSize, input_tensor, &scale, &dw, &dh);
    std::fprintf(stderr, "[yolo] frame: %ux%u stride=%u pts_ms=%lld\n",
                 f->width, f->height, f->stride, (long long)f->pts_ms);

    const uint64_t h = ComputeTensorHash(input_tensor);
    if (h == g_last_input_hash) {
        std::fprintf(stderr, "[yolo] 输入帧重复，跳过推理\n");
        return CORE_OK;
    }
    g_last_input_hash = h;

    PrintInputStats(input_tensor);

    const std::array<int64_t, 4> input_shape = { 1, 3, kInputSize, kInputSize };
    Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input_tensor_ort = Ort::Value::CreateTensor<float>(
        mem_info,
        input_tensor.data(),
        input_tensor.size(),
        input_shape.data(),
        input_shape.size());

    const char* input_names[] = { g_input_name.c_str() };
    const char* output_names[] = { g_output_name.c_str() };

    std::vector<Ort::Value> outputs;
    try {
        outputs = g_session->Run(
            Ort::RunOptions{ nullptr },
            input_names, &input_tensor_ort, 1,
            output_names, 1);
    } catch (const Ort::Exception& e) {
        std::fprintf(stderr, "[yolo] 推理异常: %s\n", e.what());
        return CORE_OK;
    }

    if (!outputs.empty() && outputs[0].IsTensor()) {
        const float* out_data = outputs[0].GetTensorData<float>();
        auto out_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        size_t count = 1;
        std::fprintf(stderr, "[yolo] 输出 shape: [");
        for (size_t i = 0; i < out_shape.size(); ++i) {
            std::fprintf(stderr, "%lld%s", (long long)out_shape[i],
                         i + 1 < out_shape.size() ? ", " : "");
            count *= static_cast<size_t>(out_shape[i]);
        }
        std::fprintf(stderr, "]\n");
        PrintOutputStats(out_data, count);

        if (out_shape.size() == 3 && out_shape[2] > 0) {
            std::vector<Detection> detections;
            PostprocessDetections(
                out_data,
                2,
                static_cast<size_t>(out_shape[2]),
                0.25f,
                0.45f,
                scale, dw, dh,
                f->width, f->height,
                detections);

            g_tracker.Update(detections);

            std::fprintf(stderr, "[yolo] 检测到 %zu 个目标\n", detections.size());
            for (const auto& d : detections) {
                const char* name = (d.cls == 0) ? "me" : (d.cls == 1) ? "monster" : "?";
                std::fprintf(stderr, "[yolo]   %s id=%d conf=%.3f cx=%.3f cy=%.3f w=%.3f h=%.3f\n",
                             name, d.track_id, d.conf, d.cx, d.cy, d.w, d.h);
            }
        }
    }

    return CORE_OK;
}

core_error plugin_execute(const core_decision* decision, core_execute_result* out) {
    (void)decision;
    (void)out;
    return CORE_OK;
}

}  // extern "C"
