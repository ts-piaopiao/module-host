#pragma once

#include "script_types.h"

// 攻击带五边形判定（散参数版本）。
bool IsInBand(float me_fx, float me_fy, float t_cx, float t_cy,
              float t_h, float t_w, int facing);

// 攻击带五边形判定（结构体版本）。
bool IsInBand(const TargetLockState& t, const MeLockState& me, int facing);
