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

    // 直方图：E 键 hold，方向键 hold，所有事件间隔
    for (auto& kv : hold_us) {
        char title[128];
        std::snprintf(title, sizeof(title), "hold histogram: %s (0x%02X)",
                      KeyName(kv.first), kv.first);
        PrintHistogramUs(title, kv.second, 50000, 2000000, 60);
    }
    PrintHistogramUs("inter-event interval histogram", inter_event_us, 50000, 2000000, 60);

    return 0;
}
