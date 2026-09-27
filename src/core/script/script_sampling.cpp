#include "script_sampling.h"

#include "human_profile.h"

// 从分位表分段线性采样。u ∈ [0,100] 均匀取，在相邻分位点插值。
int64_t SampleFromProfile(const int64_t profile[][2], int n,
                          std::mt19937& rng) {
    std::uniform_real_distribution<double> dist(0.0, 100.0);
    const double u = dist(rng);
    for (int i = 0; i < n - 1; ++i) {
        if (u <= static_cast<double>(profile[i + 1][0])) {
            const double p0 = static_cast<double>(profile[i][0]);
            const double p1 = static_cast<double>(profile[i + 1][0]);
            const double v0 = static_cast<double>(profile[i][1]);
            const double v1 = static_cast<double>(profile[i + 1][1]);
            if (p1 <= p0) return static_cast<int64_t>(v0);
            const double t = (u - p0) / (p1 - p0);
            return static_cast<int64_t>(v0 + t * (v1 - v0));
        }
    }
    return profile[n - 1][1];
}

// E 键按住时长：从真人分位表采样（微秒转毫秒）
int SampleEHoldMs(std::mt19937& rng) {
    const int64_t us = SampleFromProfile(kEHoldProfile, kEHoldProfileSize, rng);
    return static_cast<int>(us / 1000);
}
