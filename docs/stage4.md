# 阶段 4：远程操作与仲裁

## 一、背景

阶段 3 打通了真实硬件：采集卡拉帧、串口发键鼠。
阶段 4 引入第二个决策来源：远程人工操作。
目标机（接采集卡和串口盒）通过网络把画面推给客户端，
客户端把键鼠事件发回目标机。
目标机的宿主仲裁"人工决策"和"policy 决策"，人工优先。

本阶段不修改 core_contract.h，ABI 保持 2。

## 二、系统架构

两个节点：

- 目标机（跑 core.exe）
  ├─ capture 插件 → 采集卡 → core_frame
  ├─ policy 插件（当前是 test_policy，未来接 YOLO）→ core_decision
  ├─ TCP server（新）：收上行键鼠事件、发下行画面
  ├─ 宿主仲裁层（新）：合并 policy 决策与人工决策
  └─ input 插件 → 串口 → 目标设备

- 客户端（独立小程序）
  ├─ 连接目标机 TCP 端口
  ├─ 接收并显示画面（JPEG 解码后窗口显示）
  └─ 捕获本机键鼠事件，发送给目标机

## 三、线程模型

core.exe 内部两个线程：

- 主线程：帧循环（capture → policy → 仲裁 → input）
- 网络线程：TCP 接受连接、收上行、发下行

两个线程之间用两个队列通信：

- 帧队列（主线程 push，网络线程 pop）
  - 元素：core_frame 的像素快照（拷贝后），因为 core_frame.data 在下一帧前失效
  - 队列长度上限 1：新帧到达时丢弃尚未发送的旧帧，只保留最新一帧，最小化端到端延迟
- 人工事件队列（网络线程 push，主线程 pop）
  - 元素：core_action
  - 无长度限制（人工事件频率低）

两队列均用 std::mutex + std::condition_variable 保护。

日志：网络线程不直接写日志。需要日志时通过主线程打印。

## 四、网络协议（裸 TCP + 定长头）

所有消息格式：
  4 字节 大端长度 N（不含长度头本身）
  4 字节 大端类型 T
  N-4 字节 载荷

### 下行（目标机 → 客户端）

类型 1 = JPEG 帧
  载荷：JPEG 字节流
  说明：每帧一发，不做分片

类型 2 = 心跳
  载荷：空
  说明：每 2 秒一次，用于检测连接

### 上行（客户端 → 目标机）

类型 1 = 键盘事件
  载荷：int32 key_code（Windows Virtual-Key），int32 down（1 按下 / 0 抬起）

类型 2 = 鼠标移动
  载荷：int32 dx，int32 dy（相对位移）

类型 3 = 鼠标按键
  载荷：int32 button_id（1 左 / 2 右 / 3 中），int32 down

### 连接与重连

- 目标机监听，只接受一个客户端连接。
- 第二个客户端连入时，目标机拒绝并关闭。
- 客户端断线后，目标机回到等待状态。
- 客户端可以重连。

## 五、仲裁规则

### 人工窗口

- 每次收到人工事件，记录其时刻 t_human。
- 若 (now - t_human) < 500ms，视为"人工操作中"。

### 长按语义

人工窗口不仅要检测事件时刻，还要跟踪按键状态。

宿主维护一个 pressed_keys 集合：
- 收到键盘 down 事件（down==1）：把 key_code 加入 pressed_keys。
- 收到键盘 up 事件（down==0）：从 pressed_keys 移除。
- 鼠标按键同理，维护 pressed_buttons 集合。

人工操作中的判定改为：

  in_human_window = (now - t_last_event < 500ms) || (!pressed_keys.empty()) || (!pressed_buttons.empty())

即：人在按住任何键或鼠标按钮期间，无论是否产生新事件，都视为"人工操作中"。

这样可以正确处理"按住 W 两秒"的情况：虽然中间没有新事件，但 pressed_keys 里有 W，
窗口持续有效，YOLO 决策被抑制。

### 仲裁逻辑

每帧：

1. 主线程执行 capture。
2. 主线程执行 policy.decide()，得到 decision_policy。
3. 主线程检查人工事件队列：
   - 若非空，组装成 decision_human（把队列里所有 core_action 打包）。
   - 同时更新 t_human。
4. 若在人工窗口内：
   - 用 decision_human（若为空则用 NONE，不执行 policy）
   - decision_policy 丢弃
5. 若不在人工窗口：
   - 用 decision_policy
6. 把最终 decision 送给 input.execute()。

### 重要约束

- 每帧只调用一次 plugin_execute。
- 人工和 policy 的决策不合并，选其一。
- 人工事件不缓存跨帧（用过就丢）。
- 人工窗口内即使人工事件队列为空，也调用 plugin_execute 一次，
  传入 out_count == 0 的 decision。不允许跳过 plugin_execute 调用。

## 六、配置新增

在宿主配置文件里新增：

- remote_port         整数，默认 0（0 表示不启用远程功能）
- remote_jpeg_quality 整数，默认 80（1..100）

remote_port == 0 时，不启动 TCP server，行为与阶段 3 完全一致。

## 七、JPEG 编码

- 使用 Windows Imaging Component（WIC）进行 RGB32 → JPEG 编码。
- 编码在哪个线程：
  - 方案 A：主线程编码后把 JPEG 字节推给网络线程（主线程压力大）
  - 方案 B：主线程把像素快照推给网络线程，网络线程编码（本阶段采用）
- 编码质量用 remote_jpeg_quality。
- 若编码失败，跳过该帧，不中断。

## 八、阶段划分

- 4a：TCP server 骨架 + 心跳，客户端能连上并收到心跳
- 4b：下行 JPEG 推流，客户端能显示画面
  实现修正：4b 端到端验证发现 500ms select 超时导致推流只有 ~2fps，
  改为 10ms 轮询（有积压帧时 0ms）后恢复 30fps。
  帧队列长度从设计时的 2 改为 1。
- 4c：上行键鼠事件，能远程操作记事本
- 4d：仲裁层，人工优先规则生效
- 4e：与 policy 共存端到端验证（用 test_policy，人工可打断）

## 九、客户端（experiments/remote_client）

独立小工具，不接主框架。

- CMake 工程，独立构建
- 命令行参数：
    --host <ip>    默认 127.0.0.1
    --port <port>  必填
- 行为：
  - 连接目标机
  - Win32 窗口显示解码后的画面，窗口大小自适应
  - 捕获本机键盘（WM_KEYDOWN / WM_KEYUP）→ 发上行键盘事件
  - 捕获本机鼠标（相对位移 + 按键）→ 发上行鼠标事件
  - ESC 键不发送给远端（本地退出用）
- 鼠标采集模式（绝对 / 捕获切换）在 4c 阶段确定，本阶段不预设。
- 不做：
  - 不做加密
  - 不做断线自动重连
  - 不做多显示器支持

## 十、不在本阶段范围

- 真实 YOLO policy（阶段 5）
- 公网穿透 / NAT 打洞
- 加密与鉴权
- 多客户端
- 手机 / 浏览器客户端
- 画面编解码用 H264 / H265

## 十一、验收流程

阶段 4 完成后：

1. core.exe 加 --config <stage4_test.ini>（含 remote_port=9000）启动
2. 客户端连接
3. 客户端看到画面
4. 客户端在记事本里输入，目标机通过串口发出对应键
5. 同时 test_policy 每帧发一个 I，人工按键 500ms 内 YOLO 决策被丢弃
6. 人工停手后，YOLO 的 I 恢复

## 十二、一句话原则

先扩宿主，不改契约；人工优先，统一出口。

## 十三、实现状态（截至当前）

### 架构演进：为什么不再需要仲裁

原设计（第五节）的前提是：**远程操作和脚本输出都汇入同一个 input 插件，再由 input 插件发串口**。同源，才需要仲裁决定每帧听谁的。

实际演进去掉了这个前提：

1. **动机**：远程操作的按键时刻被量化到 33ms 帧网格，手感不同于本机键盘，长序列有梳状特征。
2. **动作**：远程操作改为"拿到就发"——`OutputManager::SendAsync` 从网络线程直达串口，不经过帧循环。
3. **职责迁移**：input 插件的串口翻译职责内化进 `OutputManager`（`src/core/output_manager.cpp`）。
4. **结果**：两条路径从源头就独立，不再汇入同一点。**仲裁不再是需求**——不是"未实现"，是"架构演进后天然不需要"。

### 实际输出路径

| 路径 | 调用者 | 线程 | 语义 |
|---|---|---|---|
| `SendScript` | `main.cpp` 帧循环 | 主线程 | 帧同步 |
| `SendAsync` | `remote_server` 收到 human 事件 | 网络线程 | 事件驱动，直达串口 |

两者在 `OutputManager` 内部用同一个 mutex 串行化写入队列，由 `OutputManager` 的发送线程统一发出。不存在"谁覆盖谁"的合并。

### 保留的功能：F12 控制模式

"人工接管"的需求由独立功能承担（**不是仲裁的替代品**，是另一件事）：

- `remote_client` 按 F12 → 发 TCP 上行 `type = 4`（ControlMode，载荷 `int32 on`）
- `remote_server` 收到后调 `output_manager.SetScriptPaused(on != 0)`
- `output_manager` 暂停脚本输出，并遍历 `script_pressed_keys` 发 release（避免脚本按着的键一直按住）
- 退出控制模式后脚本输出从下一帧恢复

详见 `docs/output-manager.md`。

### 已完成

- TCP server（裸 TCP + 定长头），只接受一个客户端，断线后回到等待状态
- 下行 JPEG 推流（WIC 编码，质量 `remote_jpeg_quality`）
- 上行键鼠事件（键盘 / 鼠标移动 / 鼠标按键）
- 独立客户端 `experiments/remote_client`（Win32 窗口 + JPEG 解码 + 键鼠采集）
- 配置项 `remote_port` / `remote_jpeg_quality`
- F12 控制模式全链路贯通（client → server → output_manager）

### 与设计文档的偏差

1. **架构图 `input 插件 → 串口`**：input 插件已移除，串口由 `OutputManager` 独占。
2. **架构图 `宿主仲裁层`**：未创建——架构演进后不再需要。
3. **第五节（仲裁规则）全部**：`t_human` / `pressed_keys` / `pressed_buttons` / `in_human_window` / 每帧调 `plugin_execute` 等机制未实现，也不再需要。
4. **主线程帧循环**：实际是 `capture → policy.decide → script_host.OnFrame → script_host.GetDecision → SendScript`，没有"仲裁"步骤。
5. **4d / 4e 阶段**：未按原计划执行。"人工接管"通过 F12 控制模式实现，不是逐帧窗口抑制。
6. **验收流程第 5、6 步**（"人工按键 500ms 内 YOLO 决策被丢弃"、"人工停手后 YOLO 恢复"）未执行——没有共享时间窗口，不适用。
