#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using nlohmann::json;

namespace {

struct KeyEvent {
    uint64_t t_us;
    int32_t vk;
    int32_t down;  // 1=press, 0=release
    int32_t src;
};

const char* KeyName(int32_t vk) {
    switch (vk) {
    case 0x25: return "LEFT";
    case 0x27: return "RIGHT";
    case 0x26: return "UP";
    case 0x28: return "DOWN";
    case 0x45: return "E";
    case 0x20: return "SPACE";
    default: return "?";
    }
}

bool IsDirectionKey(int32_t vk) {
    return vk == 0x25 || vk == 0x26 || vk == 0x27 || vk == 0x28;
}

void PrintPercentiles(const char* label, std::vector<uint64_t> samples) {
    if (samples.empty()) {
        std::printf("  %s: (none)\n", label);
        return;
    }
    std::sort(samples.begin(), samples.end());
    const size_t n = samples.size();
    auto pct = [&](double p) {
        return samples[static_cast<size_t>(p * (n - 1) + 0.5)];
    };
    uint64_t sum = 0;
    for (auto v : samples) sum += v;
    std::printf("  %s: n=%zu min=%llu p25=%llu p50=%llu p75=%llu p90=%llu max=%llu mean=%.0f (us)\n",
                label, n,
                (unsigned long long)samples.front(),
                (unsigned long long)pct(0.25),
                (unsigned long long)pct(0.50),
                (unsigned long long)pct(0.75),
                (unsigned long long)pct(0.90),
                (unsigned long long)samples.back(),
                static_cast<double>(sum) / n);
}

void PrintHistogramUs(const char* title, const std::vector<uint64_t>& samples,
                      uint64_t bin_us, uint64_t max_us, int width) {
    if (samples.empty()) {
        std::printf("\n%s: (empty)\n", title);
        return;
    }
    const size_t nbin = static_cast<size_t>(max_us / bin_us) + 1;
    std::vector<uint64_t> bins(nbin, 0);
    for (auto v : samples) {
        size_t b = static_cast<size_t>(v / bin_us);
        if (b >= nbin) b = nbin - 1;
        bins[b]++;
    }
    const uint64_t maxv = *std::max_element(bins.begin(), bins.end());
    std::printf("\n%s (n=%zu, bin=%llu us):\n", title, samples.size(),
                (unsigned long long)bin_us);
    for (size_t i = 0; i < nbin; ++i) {
        if (bins[i] == 0) continue;
        const int bar = (maxv == 0) ? 0 : static_cast<int>(bins[i] * width / maxv);
        std::printf("  [%5zu - %5zu ms] %6llu  ",
                    i * bin_us / 1000, (i + 1) * bin_us / 1000,
                    (unsigned long long)bins[i]);
        for (int b = 0; b < bar; ++b) std::putchar('#');
        std::putchar('\n');
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string input;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--input" && i + 1 < argc) input = argv[++i];
    }
    if (input.empty()) {
        std::fprintf(stderr, "usage: human_log_analyzer --input <events.jsonl>\n");
        return 1;
    }

    std::ifstream in(input, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        std::fprintf(stderr, "cannot open: %s\n", input.c_str());
        return 1;
    }

    std::vector<KeyEvent> events;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        json j;
        try { j = json::parse(line); } catch (...) { continue; }
        if (!j.is_object()) continue;
        if (j.value("type", "") != "snd") continue;
        if (j.value("kind", 0) != 3) continue;
        KeyEvent e;
        e.t_us = j.value("t", 0ULL);
        e.vk = j.value("a", 0);
        e.down = j.value("b", 0);
        e.src = j.value("src", 0);
        events.push_back(e);
    }
    in.close();

    std::printf("input: %s\n", input.c_str());
    std::printf("key events: %zu\n", events.size());
    if (events.empty()) return 0;

    // 按 VK 分组
    std::map<int32_t, std::vector<uint64_t>> hold_us;   // press->release 时长
    std::map<int32_t, std::vector<uint64_t>> gap_us;    // 同键 press->press 间隔
    std::map<int32_t, uint64_t> last_press_us;
    std::map<int32_t, uint64_t> last_release_us;

    // 相邻事件间隔（所有键）
    std::vector<uint64_t> inter_event_us;

    for (size_t i = 0; i < events.size(); ++i) {
        const auto& e = events[i];
        if (i > 0) {
            inter_event_us.push_back(e.t_us - events[i - 1].t_us);
        }
        if (e.down == 1) {
            auto it = last_press_us.find(e.vk);
            if (it != last_press_us.end()) {
                gap_us[e.vk].push_back(e.t_us - it->second);
            }
            last_press_us[e.vk] = e.t_us;
        } else if (e.down == 0) {
            auto it = last_press_us.find(e.vk);
            if (it != last_press_us.end()) {
                hold_us[e.vk].push_back(e.t_us - it->second);
                last_press_us.erase(it);
            }
            last_release_us[e.vk] = e.t_us;
        }
    }

    std::printf("\n=== per-key hold (press->release) ===\n");
    for (auto& kv : hold_us) {
        char label[64];
        std::snprintf(label, sizeof(label), "%s (0x%02X)", KeyName(kv.first), kv.first);
        PrintPercentiles(label, kv.second);
    }

    std::printf("\n=== per-key gap (press->next press, same key) ===\n");
    for (auto& kv : gap_us) {
        char label[64];
        std::snprintf(label, sizeof(label), "%s (0x%02X)", KeyName(kv.first), kv.second.empty() ? 0 : kv.first);
        PrintPercentiles(label, kv.second);
    }

    PrintPercentiles("inter-event", inter_event_us);

    // === 分析 1：方向键 release → 下一个 E press ===
    // 对应脚本：CHASE → ATTACK 的攻击反应延迟
    std::vector<uint64_t> dir_release_to_e_press_us;
    {
        uint64_t last_dir_release_us = 0;
        for (const auto& e : events) {
            if (e.down == 0 && IsDirectionKey(e.vk)) {
                last_dir_release_us = e.t_us;
            } else if (e.down == 1 && e.vk == 0x45) {
                if (last_dir_release_us != 0 && e.t_us > last_dir_release_us) {
                    const uint64_t dt = e.t_us - last_dir_release_us;
                    if (dt <= 5000000ULL) {  // 上限 5 秒，超过视为"不相关"
                        dir_release_to_e_press_us.push_back(dt);
                    }
                }
                last_dir_release_us = 0;  // 用完清零，避免一次 release 匹配多个 E
            }
        }
    }
    PrintPercentiles("dir_release -> E_press", dir_release_to_e_press_us);
    PrintHistogramUs("dir_release -> E_press histogram",
                     dir_release_to_e_press_us, 10000, 1000000, 60);

    // === 分析 2：E release → 下一个方向键 press ===
    // 对应脚本：RECOVERY → CHASE 的恢复延迟
    std::vector<uint64_t> e_release_to_dir_press_us;
    {
        uint64_t last_e_release_us = 0;
        for (const auto& e : events) {
            if (e.down == 0 && e.vk == 0x45) {
                last_e_release_us = e.t_us;
            } else if (e.down == 1 && IsDirectionKey(e.vk)) {
                if (last_e_release_us != 0 && e.t_us > last_e_release_us) {
                    const uint64_t dt = e.t_us - last_e_release_us;
                    if (dt <= 5000000ULL) {
                        e_release_to_dir_press_us.push_back(dt);
                    }
                }
                last_e_release_us = 0;
            }
        }
    }
    PrintPercentiles("E_release -> dir_press", e_release_to_dir_press_us);
    PrintHistogramUs("E_release -> dir_press histogram",
                     e_release_to_dir_press_us, 10000, 1000000, 60);

    // 直方图：E 键 hold，方向键 hold，所有事件间隔
    for (auto& kv : hold_us) {
        char title[128];
        std::snprintf(title, sizeof(title), "hold histogram: %s (0x%02X)",
                      KeyName(kv.first), kv.first);
        PrintHistogramUs(title, kv.second, 50000, 2000000, 60);
    }
    PrintHistogramUs("inter-event interval histogram", inter_event_us, 50000, 2000000, 60);

    // ============================================================
    // === 深度时序分析（新增）===
    // ============================================================

    // 按 key 分组的 press 时刻（用于间隔分析）
    std::map<int32_t, std::vector<uint64_t>> press_times;
    for (const auto& e : events) {
        if (e.down == 1) press_times[e.vk].push_back(e.t_us);
    }

    // --- 分析 1：E→E 间隔 ---
    {
        auto& ts = press_times[0x45];
        if (ts.size() >= 2) {
            std::vector<uint64_t> gaps;
            for (size_t i = 1; i < ts.size(); ++i) gaps.push_back(ts[i] - ts[i - 1]);
            PrintPercentiles("E -> E interval", gaps);
            // mod N 均匀性
            const int mods[] = {50, 100, 200, 500};
            for (int m : mods) {
                std::vector<uint64_t> bins(m, 0);
                for (auto v : gaps) bins[(v / 1000) % m]++;
                const double exp = static_cast<double>(gaps.size()) / m;
                double c2 = 0.0;
                for (auto v : bins) { double d = v - exp; c2 += d * d / exp; }
                std::printf("    E->E mod %d: chi2=%.1f (df=%d, uniform≈%d)\n",
                            m, c2, m - 1, m - 1);
            }
        }
    }

    // --- 分析 2：方向键切换频率（左↔右）---
    {
        std::vector<int32_t> dir_seq;
        for (const auto& e : events) {
            if (e.down == 1 && (e.vk == 0x25 || e.vk == 0x27)) {
                if (dir_seq.empty() || dir_seq.back() != e.vk) dir_seq.push_back(e.vk);
            }
        }
        int switches = 0;
        std::vector<uint64_t> switch_intervals;
        uint64_t last_switch_t = 0;
        for (size_t i = 1; i < dir_seq.size(); ++i) {
            if (dir_seq[i] != dir_seq[i - 1]) {
                switches++;
                // 用该次切换的 press 时刻（近似）
                // 简化：从 press_times 里找
            }
        }
        std::printf("\n方向键切换（左<->右）总次数: %d / 总方向按下次数: %zu\n",
                    switches, dir_seq.size());

        // 更准确：直接扫 snd 序列，找出方向键反向切换的时刻
        int32_t last_dir = 0;
        uint64_t last_dir_t = 0;
        int reverse_count = 0;
        std::vector<uint64_t> reverse_intervals;
        for (const auto& e : events) {
            if (e.down == 1 && (e.vk == 0x25 || e.vk == 0x27)) {
                if (last_dir != 0 && last_dir != e.vk) {
                    reverse_count++;
                    if (last_dir_t > 0) reverse_intervals.push_back(e.t_us - last_dir_t);
                }
                last_dir = e.vk;
                last_dir_t = e.t_us;
            }
        }
        std::printf("方向键反向切换次数: %d\n", reverse_count);
        if (!reverse_intervals.empty()) {
            PrintPercentiles("反向切换间隔", reverse_intervals);
        }
    }

    // --- 分析 3：方向键持续按住时长（连续同向）---
    {
        std::vector<uint64_t> continuous_holds;
        uint64_t run_start = 0;
        int32_t run_vk = 0;
        for (const auto& e : events) {
            if (e.down == 1 && (e.vk == 0x25 || e.vk == 0x27)) {
                if (run_vk != e.vk) {
                    if (run_vk != 0 && run_start > 0) {
                        continuous_holds.push_back(e.t_us - run_start);
                    }
                    run_vk = e.vk;
                    run_start = e.t_us;
                }
            } else if (e.down == 0 && e.vk == run_vk) {
                if (run_start > 0) {
                    continuous_holds.push_back(e.t_us - run_start);
                    run_start = 0;
                    run_vk = 0;
                }
            }
        }
        if (!continuous_holds.empty()) {
            PrintPercentiles("方向键连续按住（同向累计）", continuous_holds);
        }
    }

    // --- 分析 4：E 时是否同时按方向键 ---
    {
        // 简化：把所有 KEY 的按下/释放事件按时间排序，看 E 按下时刻是否有方向键正处于按下状态
        std::map<int32_t, uint64_t> press_start;
        int e_alone = 0;
        int e_with_dir = 0;
        for (const auto& e : events) {
            if (e.down == 1) {
                press_start[e.vk] = e.t_us;
                if (e.vk == 0x45) {
                    // 检查当前是否有方向键按下
                    bool dir_down = false;
                    for (int32_t vk : {0x25, 0x26, 0x27, 0x28}) {
                        auto it = press_start.find(vk);
                        if (it != press_start.end() && it->second > 0) {
                            dir_down = true; break;
                        }
                    }
                    if (dir_down) e_with_dir++; else e_alone++;
                }
            } else if (e.down == 0) {
                press_start.erase(e.vk);
            }
        }
        std::printf("\nE 按下时同时有方向键: %d 次\n", e_with_dir);
        std::printf("E 按下时无方向键（纯攻击）: %d 次\n", e_alone);
    }

    // --- 分析 5：相邻 snd 事件的最小间隔（人类物理极限）---
    {
        uint64_t min_gap = UINT64_MAX;
        int min_idx = -1;
        for (size_t i = 1; i < events.size(); ++i) {
            uint64_t g = events[i].t_us - events[i - 1].t_us;
            if (g < min_gap) { min_gap = g; min_idx = (int)i; }
        }
        std::printf("\n相邻事件最小间隔: %llu us (%.2f ms) @ i=%d\n",
                    (unsigned long long)min_gap, min_gap / 1000.0, min_idx);
    }

    // --- 分析 6：最长无操作期 ---
    {
        uint64_t max_gap = 0;
        int max_idx = -1;
        for (size_t i = 1; i < events.size(); ++i) {
            uint64_t g = events[i].t_us - events[i - 1].t_us;
            if (g > max_gap) { max_gap = g; max_idx = (int)i; }
        }
        std::printf("最长无操作间隔: %llu us (%.2f s) @ i=%d\n",
                    (unsigned long long)max_gap, max_gap / 1e6, max_idx);
    }

    // --- 分析 7：hold 的 mod N 均匀性 ---
    {
        std::vector<uint64_t> all_holds;
        for (auto& kv : hold_us) for (auto v : kv.second) all_holds.push_back(v);
        if (!all_holds.empty()) {
            const int mods[] = {50, 100, 200};
            std::printf("\n所有 hold 的 mod N 均匀性:\n");
            for (int m : mods) {
                std::vector<uint64_t> bins(m, 0);
                for (auto v : all_holds) bins[(v / 1000) % m]++;
                const double exp = static_cast<double>(all_holds.size()) / m;
                double c2 = 0.0;
                for (auto v : bins) { double d = v - exp; c2 += d * d / exp; }
                std::printf("  hold mod %d: chi2=%.1f (df=%d, uniform≈%d)\n",
                            m, c2, m - 1, m - 1);
            }
        }
    }

    return 0;
}
