#pragma once

#include <cstdint>
#include <random>

// 从分位表分段线性采样。u ∈ [0,100] 均匀取，在相邻分位点插值。
int64_t SampleFromProfile(const int64_t profile[][2], int n,
                          std::mt19937& rng);

// E 键按住时长：从真人分位表采样（微秒转毫秒）。
int SampleEHoldMs(std::mt19937& rng);
