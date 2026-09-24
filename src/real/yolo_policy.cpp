#include "core_contract.h"
#include <windows.h>
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>

static Ort::Env* g_env = nullptr;
static Ort::Session* g_session = nullptr;
static std::string g_model_path;
static std::string g_input_name;
static std::string g_output_name;
static uint64_t g_last_input_hash = 0;
static int g_verbose = 0;

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

    if (g_verbose) {
        std::fprintf(stderr, "[yolo] sample: B[0]=%.4f G[0]=%.4f R[0]=%.4f B[mid]=%.4f G[mid]=%.4f R[mid]=%.4f\n",
                     out_tensor[0], out_tensor[S * S],
                     out_tensor[2 * S * S],
                     out_tensor[S * S / 2],
                     out_tensor[S * S + S * S / 2],
                     out_tensor[2 * S * S + S * S / 2]);
    }
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
    if (g_verbose) {
        std::fprintf(stderr, "[yolo] 输入张量: size=%zu min=%.4f max=%.4f mean=%.4f\n",
                     tensor.size(), mn, mx, mean);
    }
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
    if (g_verbose) {
        std::fprintf(stderr, "[yolo] 输出张量: count=%zu min=%.4f max=%.4f mean=%.4f\n",
                     count, mn, mx, mean);
        std::fprintf(stderr, "[yolo] 输出前 12 个值:");
        for (size_t i = 0; i < 12 && i < count; ++i) {
            std::fprintf(stderr, " %.4f", data[i]);
        }
        std::fprintf(stderr, "\n");
    }
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

static float IoU_xywh(const Detection& a, const Detection& b) {
    const float ax1 = a.cx - a.w * 0.5f;
    const float ay1 = a.cy - a.h * 0.5f;
    const float ax2 = a.cx + a.w * 0.5f;
    const float ay2 = a.cy + a.h * 0.5f;
    const float bx1 = b.cx - b.w * 0.5f;
    const float by1 = b.cy - b.h * 0.5f;
    const float bx2 = b.cx + b.w * 0.5f;
    const float by2 = b.cy + b.h * 0.5f;
    const float ix1 = (std::max)(ax1, bx1);
    const float iy1 = (std::max)(ay1, by1);
    const float ix2 = (std::min)(ax2, bx2);
    const float iy2 = (std::min)(ay2, by2);
    const float iw = ix2 - ix1;
    const float ih = iy2 - iy1;
    if (iw <= 0 || ih <= 0) return 0.0f;
    const float inter = iw * ih;
    const float area_a = a.w * a.h;
    const float area_b = b.w * b.h;
    return inter / (area_a + area_b - inter + 1e-6f);
}

static void FilterDetections(std::vector<Detection>& dets) {
    // 规则 1+2：边缘 + 低 conf
    std::vector<Detection> stage1;
    stage1.reserve(dets.size());
    for (const auto& d : dets) {
        if (d.cls == 1) {
            if (d.cx < 0.05f || d.cx > 0.95f) continue;
            if (d.conf < 0.25f) continue;
        }
        stage1.push_back(d);
    }

    // 规则 3：同类 IoU > 0.8 合并
    // 按 conf 从高到低排序
    std::sort(stage1.begin(), stage1.end(),
              [](const Detection& a, const Detection& b) {
                  return a.conf > b.conf;
              });

    std::vector<Detection> stage2;
    std::vector<bool> suppressed(stage1.size(), false);
    for (size_t i = 0; i < stage1.size(); ++i) {
        if (suppressed[i]) continue;
        stage2.push_back(stage1[i]);
        // 抑制后面同类且 IoU > 0.8 的
        for (size_t j = i + 1; j < stage1.size(); ++j) {
            if (suppressed[j]) continue;
            if (stage1[i].cls != stage1[j].cls) continue;
            const float iou = IoU_xywh(stage1[i], stage1[j]);
            const float dx = stage1[i].cx - stage1[j].cx;
            const float dy = stage1[i].cy - stage1[j].cy;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (iou > 0.8f || dist < 0.03f) {
                suppressed[j] = true;
            }
        }
    }

    dets = std::move(stage2);
}

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

        constexpr float kMatchDx = 0.06f;
        constexpr float kMatchDy = 0.06f;

        struct Match { int det_idx; int track_idx; float dist2; };
        std::vector<Match> candidates;
        for (size_t di = 0; di < dets.size(); ++di) {
            for (size_t ti = 0; ti < tracks_.size(); ++ti) {
                if (tracks_[ti].cls != dets[di].cls) continue;
                const float dx = tracks_[ti].cx - dets[di].cx;
                const float dy = tracks_[ti].cy - dets[di].cy;
                if (std::fabs(dx) > kMatchDx || std::fabs(dy) > kMatchDy) continue;
                candidates.push_back({ (int)di, (int)ti, dx * dx + dy * dy });
            }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Match& a, const Match& b) { return a.dist2 < b.dist2; });

        std::vector<bool> det_used(dets.size(), false);
        std::vector<bool> track_used(tracks_.size(), false);
        for (const auto& m : candidates) {
            if (det_used[m.det_idx] || track_used[m.track_idx]) continue;
            det_used[m.det_idx] = true;
            track_used[m.track_idx] = true;
            auto& t = tracks_[m.track_idx];
            auto& d = dets[m.det_idx];
            t.cx = d.cx; t.cy = d.cy; t.w = d.w; t.h = d.h;
            t.lost_frames = 0;
            t.age += 1;
            d.track_id = t.id;
        }

        for (size_t di = 0; di < dets.size(); ++di) {
            if (det_used[di]) continue;
            Track t;
            t.id = next_id_++;
            t.cls = dets[di].cls;
            t.cx = dets[di].cx; t.cy = dets[di].cy;
            t.w = dets[di].w; t.h = dets[di].h;
            t.lost_frames = 0;
            t.age = 1;
            tracks_.push_back(t);
            dets[di].track_id = t.id;
        }

        if (g_verbose) {
            for (const auto& d : dets) {
                std::fprintf(stderr, "[tracker] d_cls=%d d_pos=(%.3f,%.3f) matched_id=%d\n",
                             d.cls, d.cx, d.cy, d.track_id);
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

namespace {

// 像素快照（由主线程写，推理线程读）
std::mutex g_frame_mutex;
std::vector<uint8_t> g_frame_bgra;
uint32_t g_frame_w = 0;
uint32_t g_frame_h = 0;
uint32_t g_frame_stride = 0;
uint64_t g_frame_seq = 0;

// 检测结果（由推理线程写，主线程读）
std::mutex g_result_mutex;
std::vector<Detection> g_result_detections;
uint64_t g_result_seq = 0;

// 推理线程控制
std::thread g_infer_thread;
std::atomic<bool> g_infer_stop{false};
int g_infer_fps = 10;

}  // namespace

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

        // w 和 h 都 < 20/640 才丢弃（原 30/640，放宽 1.5 倍）
        const float min_px = 20.0f / 640.0f;
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

static void InferenceLoop() {
    static constexpr uint32_t kInputSize = 640;
    uint64_t last_seq = 0;
    std::vector<uint8_t> local_bgra;

    while (!g_infer_stop) {
        uint32_t w = 0, h = 0, stride = 0;
        uint64_t seq = 0;
        {
            std::lock_guard<std::mutex> lk(g_frame_mutex);
            if (g_frame_bgra.empty() || g_frame_seq == last_seq) {
                w = 0;
            } else {
                w = g_frame_w;
                h = g_frame_h;
                stride = g_frame_stride;
                seq = g_frame_seq;
                local_bgra = g_frame_bgra;
            }
        }
        if (w == 0 || h == 0 || seq == last_seq || local_bgra.empty()) {
            Sleep(5);
            continue;
        }
        last_seq = seq;

        core_frame f = {};
        f.width = w;
        f.height = h;
        f.stride = stride;
        f.format = CORE_PIXEL_FORMAT_BGRA8;
        f.data = local_bgra.data();
        f.size = local_bgra.size();
        f.pts_ms = 0;

        std::vector<float> input_tensor;
        float scale = 0.0f;
        int dw = 0, dh = 0;
        PreprocessFrame(&f, kInputSize, input_tensor, &scale, &dw, &dh);
        if (g_verbose) {
            std::fprintf(stderr, "[yolo] frame: %ux%u stride=%u seq=%llu\n",
                         w, h, stride, (unsigned long long)seq);
        }

        const uint64_t hsh = ComputeTensorHash(input_tensor);
        if (hsh == g_last_input_hash) {
            std::fprintf(stderr, "[yolo] 输入帧重复，跳过推理\n");
            Sleep(1000 / (g_infer_fps > 0 ? g_infer_fps : 10));
            continue;
        }
        g_last_input_hash = hsh;

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
            Sleep(1000 / (g_infer_fps > 0 ? g_infer_fps : 10));
            continue;
        }

        std::vector<Detection> detections;
        if (!outputs.empty() && outputs[0].IsTensor()) {
            const float* out_data = outputs[0].GetTensorData<float>();
            auto out_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
            size_t count = 1;
            for (size_t i = 0; i < out_shape.size(); ++i) {
                count *= static_cast<size_t>(out_shape[i]);
            }
            if (g_verbose) {
                std::fprintf(stderr, "[yolo] 输出 shape: [");
                for (size_t i = 0; i < out_shape.size(); ++i) {
                    std::fprintf(stderr, "%lld%s", (long long)out_shape[i],
                                 i + 1 < out_shape.size() ? ", " : "");
                }
                std::fprintf(stderr, "]\n");
            }
            PrintOutputStats(out_data, count);

            if (out_shape.size() == 3 && out_shape[2] > 0) {
                PostprocessDetections(
                    out_data,
                    2,
                    static_cast<size_t>(out_shape[2]),
                    0.25f,
                    0.45f,
                    scale, dw, dh,
                    f.width, f.height,
                    detections);

                FilterDetections(detections);

                g_tracker.Update(detections);
            }
        }

        {
            std::lock_guard<std::mutex> lk(g_result_mutex);
            g_result_detections = detections;
            g_result_seq = seq;
        }

        Sleep(1000 / (g_infer_fps > 0 ? g_infer_fps : 10));
    }
}

extern "C" {

const char* plugin_meta(void) {
    return "yolo_policy|0.1.0|4|policy";
}

core_error plugin_init(uint32_t host_abi, const char* config) {
    if (host_abi != 4) {
        return CORE_ERR_ABI_MISMATCH;
    }

    g_model_path = GetConfigValue(config, "policy_model_path");
    if (g_model_path.empty()) {
        g_model_path = "D:\\dev\\module-host\\models\\yolo11s.onnx";
    }

    const std::string verbose_str = GetConfigValue(config, "policy_verbose");
    g_verbose = (verbose_str == "1") ? 1 : 0;

    const std::string fps_str = GetConfigValue(config, "policy_fps");
    g_infer_fps = 10;
    if (!fps_str.empty()) {
        const int v = std::atoi(fps_str.c_str());
        if (v > 0) g_infer_fps = v;
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

    g_infer_stop = false;
    if (g_infer_thread.joinable()) {
        g_infer_thread.join();
    }
    g_infer_thread = std::thread(InferenceLoop);

    return CORE_OK;
}

core_error plugin_release(void) {
    g_infer_stop = true;
    if (g_infer_thread.joinable()) {
        g_infer_thread.join();
    }
    {
        std::lock_guard<std::mutex> lk(g_frame_mutex);
        g_frame_bgra.clear();
        g_frame_bgra.shrink_to_fit();
        g_frame_w = 0;
        g_frame_h = 0;
        g_frame_stride = 0;
        g_frame_seq = 0;
    }
    {
        std::lock_guard<std::mutex> lk(g_result_mutex);
        g_result_detections.clear();
        g_result_detections.shrink_to_fit();
        g_result_seq = 0;
    }
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
    if (intent == nullptr) {
        return CORE_OK;
    }

    if (intent->frame != nullptr && intent->frame->data != nullptr &&
        intent->frame->width > 0 && intent->frame->height > 0 &&
        intent->frame->format == CORE_PIXEL_FORMAT_BGRA8) {
        const core_frame* f = intent->frame;
        const size_t need = static_cast<size_t>(f->width) * f->height * 4u;
        std::lock_guard<std::mutex> lk(g_frame_mutex);
        g_frame_bgra.assign(f->data, f->data + need);
        g_frame_w = f->width;
        g_frame_h = f->height;
        g_frame_stride = f->stride;
        g_frame_seq += 1;
    }

    if (intent->detections_out != nullptr) {
        std::lock_guard<std::mutex> lk(g_result_mutex);
        uint32_t n = 0;
        for (const auto& d : g_result_detections) {
            if (n >= CORE_MAX_DETECTIONS) break;
            auto& o = intent->detections_out->items[n];
            o.cls = d.cls;
            o.track_id = d.track_id;
            o.conf = d.conf;
            o.cx = d.cx;
            o.cy = d.cy;
            o.w = d.w;
            o.h = d.h;
            n++;
        }
        intent->detections_out->count = n;
    }

    return CORE_OK;
}

core_error plugin_execute(const core_decision* decision, core_execute_result* out) {
    (void)decision;
    (void)out;
    return CORE_OK;
}

}  // extern "C"
