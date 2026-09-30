#include "script_geometry.h"

#include "script_config.h"

#include <cmath>

bool IsInBand(const TargetLockState& t, const MeLockState& me, int facing) {
    if (!t.has || !me.valid) return false;
    // 攻击带 v3：五边形（尖点 + 45° 斜边 + 远端竖直边；X 前方 9.6px~280px；Y 上方 80px / 下方 20px）
    const float t_left = t.cx - t.w * 0.5f;
    const float t_right = t.cx + t.w * 0.5f;
    float x_near, x_far;
    if (facing > 0) {
        x_near = t_left - me.fx;
        x_far  = t_right - me.fx;
    } else {
        x_near = me.fx - t_right;
        x_far  = me.fx - t_left;
    }
    if (x_far < ScriptConfig::kBandXApex) return false;
    if (x_near > ScriptConfig::kBandXMaxSame) return false;

    // Y 轴：怪物 bbox 与 [me.fy + kBandYMin, me.fy + kBandYMax] 相交
    const float t_top = t.cy - t.h * 0.5f;
    const float t_bot = t.cy + t.h * 0.5f;
    const float band_top = me.fy + ScriptConfig::kBandYMin;
    const float band_bot = me.fy + ScriptConfig::kBandYMax;
    if (t_bot < band_top || t_top > band_bot) return false;

    // 近端斜边约束：x_inner 随 |y| 线性增大
    // 用 bbox 与 band 相交区间的中点作为"代表高度"
    const float y_clip_top = (t_top > band_top) ? t_top : band_top;
    const float y_clip_bot = (t_bot < band_bot) ? t_bot : band_bot;
    const float y_rep = (y_clip_top + y_clip_bot) * 0.5f - me.fy;

    // v3：近端尖点 + 45° 斜边；远端竖直常数
    const float x_inner_at_y = ScriptConfig::kBandXApex
                             + std::fabs(y_rep) / ScriptConfig::kBandApexSlope;
    if (x_far < x_inner_at_y) return false;
    if (x_near > ScriptConfig::kBandXMaxSame) return false;
    return true;
}

bool IsInBand(float me_fx, float me_fy, float t_cx, float t_cy,
              float t_h, float t_w, int facing) {
    const float t_left = t_cx - t_w * 0.5f;
    const float t_right = t_cx + t_w * 0.5f;
    float x_near, x_far;
    if (facing > 0) {
        x_near = t_left - me_fx;
        x_far  = t_right - me_fx;
    } else {
        x_near = me_fx - t_right;
        x_far  = me_fx - t_left;
    }
    if (x_far < ScriptConfig::kBandXApex) return false;
    if (x_near > ScriptConfig::kBandXMaxSame) return false;

    const float t_top = t_cy - t_h * 0.5f;
    const float t_bot = t_cy + t_h * 0.5f;
    const float band_top = me_fy + ScriptConfig::kBandYMin;
    const float band_bot = me_fy + ScriptConfig::kBandYMax;
    if (t_bot < band_top || t_top > band_bot) return false;

    const float y_clip_top = (t_top > band_top) ? t_top : band_top;
    const float y_clip_bot = (t_bot < band_bot) ? t_bot : band_bot;
    const float y_rep = (y_clip_top + y_clip_bot) * 0.5f - me_fy;

    // v3：近端尖点 + 45° 斜边；远端竖直常数
    const float x_inner_at_y = ScriptConfig::kBandXApex
                             + std::fabs(y_rep) / ScriptConfig::kBandApexSlope;
    if (x_far < x_inner_at_y) return false;
    if (x_near > ScriptConfig::kBandXMaxSame) return false;
    return true;
}
