#pragma once

#include "iscript.h"
#include "script_types.h"

// 自机锁定：从 cls==0 的检测框中按先验位置/上一帧锁定位置选一个，
// 维护跨帧时间状态（locked / lost_since）。
void SelectMe(const ScriptWorld& world, MeLockState* m);

// 目标锁定：从 cls==1 且与自机同层的检测框中按层级/距离选一个，
// 维护跨帧时间状态（locked / lost_since / last_target_switch_ms）。
void SelectTarget(const ScriptWorld& world, const MeLockState& me,
                  int facing, TargetLockState* t);
