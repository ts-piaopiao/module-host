#include "cpp_script.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using nlohmann::json;

namespace {

constexpr int kInvariantCount = 7;
constexpr long long kFrameMs = 33;
constexpr long long kMinEHoldMs = 100;
constexpr long long kMaxEHoldMs = 500;
constexpr int64_t kMinEGapMs = 800;

struct DetRaw {
    int cls = 0;
    int id = 0;
    float conf = 0.0f;
    float cx = 0.0f;
    float cy = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

struct FrameData {
    uint64_t t = 0;
    std::vector<DetRaw> dets;
};

struct Args {
    std::string input;
    std::string output;
    std::string config_path;
};

const char* InvariantName(int idx) {
    switch (idx) {
        case 0: return "I1 me未锁定 -> 不动 active_key / 不发E";
        case 1: return "I2 active_key 只能是 0x00/0x25/0x27/0x45";
        case 2: return "I3 desired_e=true -> state 只能是 2/3";
        case 3: return "I4 desired_e 连续帧时长 [100,500]ms";
        case 4: return "I5 两次E release 间隔 >= 800ms";
        case 5: return "I6 进入 state=3 的前一帧 facing 与 target 反号";
        case 6: return "I7 state 只能是 0/1/2/3/4";
        default: return "?";
    }
}

bool ParseArgs(int argc, char** argv, Args* out) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--input" && i + 1 < argc) {
            out->input = argv[++i];
        } else if (a == "--output" && i + 1 < argc) {
            out->output = argv[++i];
        } else if (a == "--config" && i + 1 < argc) {
            out->config_path = argv[++i];
        } else {
            std::fprintf(stderr, "未知参数: %s\n", a.c_str());
            return false;
        }
    }
    if (out->input.empty()) {
        std::fprintf(stderr, "用法: script_replay --input <events.jsonl> [--output <trace.jsonl>] [--config <file>]\n");
        return false;
    }
    return true;
}

bool ReadTextFile(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!ParseArgs(argc, argv, &args)) return 1;

    std::string config_text;
    if (!args.config_path.empty()) {
        if (!ReadTextFile(args.config_path, &config_text)) {
            std::fprintf(stderr, "无法读取配置: %s\n", args.config_path.c_str());
            return 1;
        }
    }

    std::ifstream in(args.input, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        std::fprintf(stderr, "无法打开输入: %s\n", args.input.c_str());
        return 1;
    }

    std::map<uint64_t, FrameData> frames;
    long long lines_total = 0;
    long long det_lines = 0;
    long long bad_lines = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        ++lines_total;
        json j;
        try {
            j = json::parse(line);
        } catch (...) {
            ++bad_lines;
            continue;
        }
        if (!j.is_object()) {
            ++bad_lines;
            continue;
        }
        if (j.value("type", "") != "det") continue;

        DetRaw d;
        d.cls = j.value("cls", 0);
        d.id = j.value("id", 0);
        d.conf = j.value("conf", 0.0f);
        d.cx = j.value("cx", 0.0f);
        d.cy = j.value("cy", 0.0f);
        d.w = j.value("w", 0.0f);
        d.h = j.value("h", 0.0f);

        const uint64_t frame = j.value("frame", 0ULL);
        const uint64_t t = j.value("t", 0ULL);
        FrameData& fd = frames[frame];
        if (fd.dets.empty()) fd.t = t;
        fd.dets.push_back(d);
        ++det_lines;
    }
    in.close();

    std::printf("输入: %s\n", args.input.c_str());
    std::printf("行数: %lld (det %lld, 解析失败 %lld), 帧数: %zu\n",
                lines_total, det_lines, bad_lines, frames.size());

    CppScript script;
    if (!script.Init(config_text)) {
        std::fprintf(stderr, "脚本 Init 失败\n");
        return 1;
    }

    std::ofstream trace;
    if (!args.output.empty()) {
        trace.open(args.output, std::ios::out | std::ios::trunc);
        if (!trace.is_open()) {
            std::fprintf(stderr, "无法打开输出: %s\n", args.output.c_str());
            script.Shutdown();
            return 1;
        }
    }

    long long viol[kInvariantCount] = {0};

    CppScriptDebugInfo prev{};
    bool has_prev = false;
    bool prev_e = false;
    long long e_run_frames = 0;
    uint64_t e_run_start_frame = 0;
    bool has_e_release = false;
    int64_t last_e_release_t = 0;

    long long replayed = 0;
    for (const auto& kv : frames) {
        const uint64_t frame_idx = kv.first;
        const FrameData& fd = kv.second;
        const uint64_t now_ms = fd.t;

        core_detections dets{};
        dets.count = 0;
        for (const DetRaw& d : fd.dets) {
            if (dets.count >= CORE_MAX_DETECTIONS) break;
            core_detection& o = dets.items[dets.count];
            o.cls = d.cls;
            o.track_id = d.id;
            o.conf = d.conf;
            o.cx = d.cx;
            o.cy = d.cy;
            o.w = d.w;
            o.h = d.h;
            ++dets.count;
        }

        ScriptWorld w{};
        w.frame_index = frame_idx;
        w.now_ms = now_ms;
        w.me_valid = false;
        w.me_fx = 0.0f;
        w.me_fy = 0.0f;
        w.dets = &dets;

        script.OnFrame(w);

        core_decision dec{};
        dec.out_count = 0;
        script.GetDecision(&dec);

        CppScriptDebugInfo dbg{};
        script.GetDebugInfo(&dbg);

        // I1: me未锁定 -> 不动键 / 不发E
        if (!dbg.me_locked && (dbg.active_key != 0 || dbg.desired_e)) ++viol[0];
        // I2: active_key 白名单
        if (!(dbg.active_key == 0x00 || dbg.active_key == 0x25 ||
              dbg.active_key == 0x27 || dbg.active_key == 0x45)) {
            ++viol[1];
        }
        // I3: desired_e -> ATTACK/ATTACK_TURN
        if (dbg.desired_e && !(dbg.state == 2 || dbg.state == 3)) ++viol[2];
        // I7: state 枚举范围
        if (dbg.state < 0 || dbg.state > 4) ++viol[6];
        // I6: 进入 ATTACK_TURN 的前一帧 facing（本帧 facing 已被 phase0 翻转）
        //     与本帧 target 位置反号（target 在身后）
        if (has_prev && prev.state != 3 && dbg.state == 3 && dbg.target_locked) {
            bool ok = false;
            if (dbg.target_cx > dbg.me_fx && prev.facing == -1) ok = true;
            if (dbg.target_cx < dbg.me_fx && prev.facing == 1) ok = true;
            if (!ok) ++viol[5];
        }

        // I4/I5: desired_e 连续段跟踪
        if (dbg.desired_e) {
            if (!prev_e) e_run_start_frame = frame_idx;
            ++e_run_frames;
            if (!prev_e && has_e_release) {
                const int64_t gap = static_cast<int64_t>(now_ms) - last_e_release_t;
                if (gap < kMinEGapMs) {
                    ++viol[4];
                    std::printf("[I5] 违例 frame=%llu: 距上次E释放 %lldms < 800ms\n",
                                (unsigned long long)frame_idx, (long long)gap);
                }
            }
        } else if (prev_e) {
            const long long ms = e_run_frames * kFrameMs;
            if (ms < kMinEHoldMs || ms > kMaxEHoldMs) {
                ++viol[3];
                std::printf("[I4] 违例 frame=%llu..%llu: E连续 %lld帧 = %lldms 超出[100,500]\n",
                            (unsigned long long)e_run_start_frame,
                            (unsigned long long)frame_idx,
                            (long long)e_run_frames, (long long)ms);
            }
            has_e_release = true;
            last_e_release_t = static_cast<int64_t>(now_ms);
            e_run_frames = 0;
        }

        if (trace.is_open()) {
            json o;
            o["frame"] = frame_idx;
            o["t"] = now_ms;
            o["state"] = dbg.state;
            o["facing"] = dbg.facing;
            o["me_locked"] = dbg.me_locked ? 1 : 0;
            o["me_fx"] = dbg.me_fx;
            o["me_fy"] = dbg.me_fy;
            o["target_locked"] = dbg.target_locked ? 1 : 0;
            o["target_cx"] = dbg.target_cx;
            o["active_key"] = dbg.active_key;
            o["desired_e"] = dbg.desired_e ? 1 : 0;
            o["e_long_hold"] = dbg.e_long_hold ? 1 : 0;
            o["dets"] = dets.count;
            json arr = json::array();
            for (uint32_t i = 0; i < dec.out_count; ++i) {
                json a;
                a["kind"] = dec.actions[i].kind;
                a["a"] = dec.actions[i].a;
                a["b"] = dec.actions[i].b;
                a["c"] = dec.actions[i].c;
                arr.push_back(a);
            }
            o["out"] = arr;
            trace << o.dump() << '\n';
        }

        prev = dbg;
        has_prev = true;
        prev_e = dbg.desired_e;
        ++replayed;
    }

    if (e_run_frames > 0) {
        // 数据在 E 按住中结束：只可能确定"过长"，"过短"是截断导致的假象
        const long long ms = e_run_frames * kFrameMs;
        if (ms > kMaxEHoldMs) {
            ++viol[3];
            std::printf("[I4] 违例 frame=%llu..(数据结束): E连续 %lld帧 = %lldms 超出[100,500]\n",
                        (unsigned long long)e_run_start_frame,
                        (long long)e_run_frames, (long long)ms);
        }
    }

    if (trace.is_open()) trace.flush();

    script.Shutdown();

    bool all_pass = true;
    for (int i = 0; i < kInvariantCount; ++i) {
        if (viol[i] != 0) all_pass = false;
    }

    std::printf("\n回放帧数: %lld\n", replayed);
    std::printf("不变式检查:\n");
    for (int i = 0; i < kInvariantCount; ++i) {
        if (viol[i] == 0) {
            std::printf("  I%d: PASS\n", i + 1);
        } else {
            std::printf("  I%d: FAIL (%lld 次违例) %s\n", i + 1, viol[i], InvariantName(i));
        }
    }
    std::printf("总体: %s\n", all_pass ? "PASS" : "FAIL");
    std::fflush(stdout);

    return all_pass ? 0 : 1;
}
