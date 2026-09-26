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

constexpr int kModBin = 33;          // 帧间隔 ms
constexpr int kIntervalHistMax = 100; // 间隔直方图上限（ms），超出的归最后一 bin

void PrintHistogram(const char* title, const std::vector<uint64_t>& bins,
                    uint64_t total, int width) {
    std::printf("\n%s (n=%llu):\n", title, (unsigned long long)total);
    const uint64_t maxv = bins.empty() ? 0 : *std::max_element(bins.begin(), bins.end());
    for (size_t i = 0; i < bins.size(); ++i) {
        const int bar = (maxv == 0) ? 0 : static_cast<int>(bins[i] * width / maxv);
        std::printf("  [%3zu] %8llu  ", i, (unsigned long long)bins[i]);
        for (int b = 0; b < bar; ++b) std::putchar('#');
        std::putchar('\n');
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string input;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--input" && i + 1 < argc) {
            input = argv[++i];
        }
    }
    if (input.empty()) {
        std::fprintf(stderr, "usage: send_log_analyzer --input <events.jsonl>\n");
        return 1;
    }

    std::ifstream in(input, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        std::fprintf(stderr, "cannot open: %s\n", input.c_str());
        return 1;
    }

    // 按 src 分组收集 snd 时间戳
    std::map<int, std::vector<uint64_t>> by_src;
    long long total_lines = 0;
    long long snd_count = 0;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        ++total_lines;
        json j;
        try { j = json::parse(line); } catch (...) { continue; }
        if (!j.is_object()) continue;
        if (j.value("type", "") != "snd") continue;
        ++snd_count;
        const int src = j.value("src", -1);
        const uint64_t t = j.value("t", 0ULL);
        by_src[src].push_back(t);
    }
    in.close();

    std::printf("input: %s\n", input.c_str());
    std::printf("total lines: %lld\n", total_lines);
    std::printf("snd events: %lld\n", snd_count);

    if (snd_count == 0) {
        std::printf("no snd events found.\n");
        return 0;
    }

    for (auto& kv : by_src) {
        const int src = kv.first;
        auto& ts = kv.second;
        std::sort(ts.begin(), ts.end());

        std::printf("\n=== src=%d (n=%zu) ===\n", src, ts.size());

        if (ts.size() < 2) {
            std::printf("(need >=2 events for interval analysis)\n");
            continue;
        }

        // 相邻间隔
        std::vector<uint64_t> intervals;
        intervals.reserve(ts.size() - 1);
        for (size_t i = 1; i < ts.size(); ++i) {
            intervals.push_back(ts[i] - ts[i - 1]);
        }
        std::sort(intervals.begin(), intervals.end());
        const uint64_t imin = intervals.front();
        const uint64_t imax = intervals.back();
        const uint64_t imed = intervals[intervals.size() / 2];
        uint64_t isum = 0;
        for (auto v : intervals) isum += v;
        const double imean = static_cast<double>(isum) / intervals.size();
        std::printf("interval min/median/max/mean: %llu / %llu / %llu / %.2f ms\n",
                    (unsigned long long)imin, (unsigned long long)imed,
                    (unsigned long long)imax, imean);

        // 间隔直方图（bin = 1ms，到 kIntervalHistMax）
        std::vector<uint64_t> ih(kIntervalHistMax + 1, 0);
        for (auto v : intervals) {
            if (v >= static_cast<uint64_t>(kIntervalHistMax)) ih[kIntervalHistMax]++;
            else ih[v]++;
        }
        PrintHistogram("interval histogram (ms)", ih, intervals.size(), 60);

        // mod 33 直方图
        std::vector<uint64_t> mh(kModBin, 0);
        for (auto v : ts) {
            mh[v % kModBin]++;
        }
        PrintHistogram("mod 33 histogram (uniform => comb solved)",
                       mh, ts.size(), 60);

        // mod 33 均匀性统计
        const double expected = static_cast<double>(ts.size()) / kModBin;
        double chi2 = 0.0;
        uint64_t mx = 0, mn = ts.size();
        for (auto v : mh) {
            const double d = static_cast<double>(v) - expected;
            chi2 += d * d / expected;
            if (v > mx) mx = v;
            if (v < mn) mn = v;
        }
        std::printf("\nmod-33 uniformity: chi2=%.1f (df=32, uniform≈32, "
                    "concentrated>>32), max/min=%llu/%llu (ideal≈%.0f)\n",
                    chi2, (unsigned long long)mx, (unsigned long long)mn,
                    expected);
    }

    return 0;
}
