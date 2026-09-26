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

// 方向键最短按住的分位表（真人方向键 hold 的低分位，截断到 1000ms 上限）。
// 用途：约束脚本方向键"最短按住"，消除 <250ms 的碎步；
// 追击到达攻击带时若已过此下限则正常松手——不影响长按追击。
constexpr int64_t kDirHoldProfile[][2] = {
    {  0, 300000},   // 300ms
    { 50, 400000},   // 400ms
    {100, 500000},   // 500ms
};
constexpr int kDirHoldProfileSize = 3;

#endif  // MODULE_HOST_HUMAN_PROFILE_H
