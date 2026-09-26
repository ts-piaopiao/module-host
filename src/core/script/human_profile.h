#ifndef MODULE_HOST_HUMAN_PROFILE_H
#define MODULE_HOST_HUMAN_PROFILE_H

#include <cstdint>

// 真人按键分布（从真实采集数据分析得出，2026-09-26）
// 数据来源：30 分钟真人远程操作，715 次 E 键、502 次方向键
// 存储格式：{(累计百分位 0-100), 时长微秒}
//
// 采样方式：均匀取 u ∈ [0,100]，在相邻分位点间线性插值。
// 这保留了真人分布的钟形/长尾形状，而非均匀分布。

// E 键按住时长（真人）
// 直方图峰值在 100-150ms，钟形，右尾长
constexpr int64_t kEHoldProfile[][2] = {
    {  0,  70000},   // 70ms  — 真人 p5 水平，避免极限短按
    { 10,  90000},   // 90ms
    { 25, 124000},   // 124ms
    { 50, 141000},   // 141ms — 中位数
    { 75, 169000},   // 169ms
    { 90, 194000},   // 194ms
    { 99, 250000},   // 250ms
    {100, 350000},   // 350ms — 长尾收紧（原 834ms 是单次极端值）
};
constexpr int kEHoldProfileSize = 8;

#endif  // MODULE_HOST_HUMAN_PROFILE_H
