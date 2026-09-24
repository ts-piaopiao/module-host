# 输出管理器（OutputManager）

## 一、背景与目标

### 1.1 问题

当前架构：所有按键（脚本 + 远程）都走 `plugin_execute → real_input → 串口`，
每次调用发生在帧循环内（33ms 粒度）。

两个后果：
1. 远程操作的按键时刻被量化到帧网格，手感不同于本机键盘
2. 脚本输出时序落在 33ms 网格，产生梳状特征（长序列可识别）

### 1.2 目标

1. **远程操作事件驱动**：按键按下/抬起立即发出，不经过帧循环
2. **控制模式独占**：进入控制模式时脚本输出被截断，退出后恢复
3. **时序精度可达 ms 级**：发送线程可 sleep 到毫秒，不受 33ms 限制
4. **串口独占**：唯一持有者，避免并发写入

### 1.3 不在本设计范围

- 0-33ms 随机阻塞（模糊化脚本梳状，S12 候选）
- 脚本输出"时长意图"（方案 D，需要重构脚本接口）

## 二、架构

```
core.exe 内核
  │
  ├─ OutputManager（新，内核模块，非插件）
  │    ├─ 唯一持有串口句柄
  │    ├─ 内部发送线程
  │    ├─ SendScript(action)      ← 脚本入口（帧同步，可暂停）
  │    ├─ SendAsync(action)       ← 远程入口（事件驱动，永不暂停）
  │    └─ SetScriptPaused(bool)   ← 控制模式切换
  │
  ├─ ScriptHost → CppScript → OutputManager::SendScript
  │
  ├─ remote_server → OutputManager::SendAsync（绕过帧循环）
  │
  └─ real_input（插件）—— 删除，逻辑合并进 OutputManager
```

## 三、接口定义

```cpp
// src/core/output_manager.h
#pragma once
#include "core_contract.h"
#include <string>

class OutputManager {
public:
    OutputManager();
    ~OutputManager();

    OutputManager(const OutputManager&) = delete;
    OutputManager& operator=(const OutputManager&) = delete;

    // 打开串口。config 为原始配置（读 input_port / input_baud）。
    // 返回 false 表示串口打开失败。
    bool Start(const std::string& config);

    // 停止并关闭串口。幂等。
    void Stop();

    // 脚本入口：帧同步调用。内部检查 script_paused：
    //   - 若暂停：直接丢弃所有 action
    //   - 未暂停：入队，由发送线程发出
    void SendScript(const core_decision* decision);

    // 远程入口：事件驱动调用。永不暂停，立即入队。
    // 供 remote_server 直接从网络线程调用。
    void SendAsync(const core_action* action);

    // 控制模式：true = 脚本输出被丢弃。
    void SetScriptPaused(bool paused);

    // 查询当前是否暂停。
    bool IsScriptPaused() const;

private:
    struct Impl;
    Impl* impl_;
};
```

## 四、线程模型

### 4.1 队列

```
std::mutex mutex;
std::deque<core_action> queue;
std::condition_variable cv;
bool stop_flag = false;
```

- 生产：SendScript / SendAsync（多线程调用）
- 消费：发送线程

### 4.2 发送线程

```
while (!stop_flag) {
    // 阻塞等队列非空
    cv.wait(...);
    // 取一个 action
    // 翻译为 mk.* 命令
    // 写串口
    // 可选：sleep 到下一帧（S12 加随机延迟）
}
```

### 4.3 为什么用 cv 不用 Sleep 轮询

- SendAsync 要求"立即发出"，用轮询会有 33ms 延迟
- cv 让线程零延迟响应
- 无事件时线程睡眠，不占 CPU

## 五、控制模式语义

### 5.1 状态

- `script_paused = false`：默认，脚本输出正常发出
- `script_paused = true`：远程独占，脚本输出被丢弃

### 5.2 切换时机

- remote_client 按 F12 进入控制模式 → remote_server 通知 → SetScriptPaused(true)
- 按 F12 退出 → SetScriptPaused(false)

### 5.7 F12 协议扩展

明确 F12 触发控制模式的网络层协议：

- TCP 上行新增 `type = 4` = ControlMode
- 载荷：`int32 on`（0 = 退出，1 = 进入）
- remote_client：按 F12 时发送
- remote_server：解析后调 `output->SetScriptPaused(on != 0)`

### 5.3 状态同步（关键）

进入控制模式时：
1. SetScriptPaused(true)
2. **释放脚本当前按下的所有键**（否则脚本按着 W，人接管后 W 一直按着）
3. 脚本状态不重置（CppScript 内部 facing / state 继续更新，只是输出被丢弃）

退出控制模式时：
1. SetScriptPaused(false)
2. **不补发脚本按键**（脚本下一帧自然输出）
3. 脚本状态不清空

### 5.4 同步实现

OutputManager 需要跟踪"脚本当前按下的键"：

```cpp
// Impl 加字段
std::set<int> script_pressed_keys;   // 脚本按下的 VK 码
```

SendScript 时更新这个集合；SetScriptPaused(true) 时遍历集合发 release。

### 5.5 差分输出约定

明确脚本输出的格式与责任：

- 脚本通过 SendScript 输出的是**差分的 core_decision**
- 每帧的 decision 只包含"从上一帧到本帧的变化"（press 或 release 的 action）
- OutputManager 维护 script_pressed_keys，收到 press 时 insert，收到 release 时 erase
- 脚本有责任保证"press 过的键，最终会 release"（在 Shutdown 或状态切换时）

### 5.6 竞态处理

明确多线程下的键集合一致性保证：

- script_pressed_keys 由 OutputManager 内部 mutex 保护
- SendScript 和 SetScriptPaused 通过同一个 mutex 串行化
- SetScriptPaused(true) 实现流程：
    lock
    script_paused = true
    for vk in script_pressed_keys: enqueue release
    script_pressed_keys.clear()
    unlock
- SendScript 实现流程：
    lock
    if script_paused: unlock; return
    for action in decision: enqueue; update script_pressed_keys
    unlock
- 这样无论 SendScript 与 SetScriptPaused 如何交替，都不会漏 release

## 六、串口协议

沿用 real_input 的实现：
- 打开 COM 口（input_port）
- 波特率 115200
- 8N1
- 命令格式 mk.*（见 real_input.cpp 现有实现）

### 6.1 串口写失败的语义

明确发送线程对写失败的处理策略：

- 写失败：打印错误日志，丢弃该 action，不阻塞线程
- 连续失败 10 次：尝试重新打开串口
- 重新打开成功：清空队列，继续
- 重新打开失败：日志报错，继续运行（后续 action 都会失败）

## 七、迁移步骤

### 7.1 新增

- src/core/output_manager.h / .cpp

### 7.2 修改

- main.cpp：创建 OutputManager，传给 remote_server 和 script_host
- remote_server：加 SetOutputSink(OutputManager*) 接口，收到 human 事件直接调 SendAsync
- script_host：CppScript 内部调 OutputManager::SendScript
- CMakeLists：加 output_manager.cpp，删 real_input target

### 7.3 删除

- src/real/real_input.cpp / .h
- plugins_real/input_plugin.dll（不再生成）

### 7.4 契约影响

**无 ABI 变更**。input 插件从"插件"变成"内核模块"，但：
- core_contract.h 里的 plugin_* 6 个符号不变
- policy / capture 插件继续存在
- 输入插件不再有 plugin_meta，因为它不是插件

### 7.5 关于状态跟踪的说明

明确与 real_input 旧实现的区别：

- real_input 的"自动释放"是错误推断（根据本帧 action 猜上一帧状态）
- OutputManager 的 script_pressed_keys 是记录（脚本明确 press 过的键）
- 两者不矛盾：前者被动猜，后者主动记

## 八、验收

### 8.1 远程操作手感

- 本机键盘按 W：从按下到串口发出 < 5ms
- 远程按 W：从按下到串口发出 < 10ms（含网络延迟）
- 按住 W 2 秒：游戏里角色持续移动 2 秒 ± 10ms
- 按键间隔：不再量化到 33ms 网格

### 8.2 控制模式

- 进入控制模式：脚本输出暂停，脚本按的键立即释放
- 控制模式内：只有远程输入生效
- 退出控制模式：脚本输出恢复，从下一帧开始

### 8.3 回归

- 假插件验收 13/13 PASS
- runtime_errors 6/6 PASS
- 真实硬件端到端仍正常

## 九、一句话原则

脚本走队列，远程直通；串口独占一个模块，时序不再受帧率约束。