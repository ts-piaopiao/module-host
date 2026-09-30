# projectbrief — 项目目标与范围

> 基础文档。定义「做什么、不做什么」。范围变化时必须同步本文件。

## 1. 项目标识

| 项 | 名称 |
|---|---|
| 仓库名 | `module-host` |
| 中文名 | 模块宿主系统框架 |
| CMake 工程名 | `ModuleHost` |
| 核心 target | `module_host_core` |
| 核心产物 | `core.exe`（**必须保持不变**） |
| 唯一契约头 | `core_contract.h`（**必须保持不变**） |
| C++ 命名空间 | `module_host` 或 `mh` |

**命名禁用词**（项目名与命名不得出现，易被误判）：
`game / auto / bot / script / macro / cheat / assist / input / vision / AI`

> 注：`script` 作为**代码内目录与类名**（`src/core/script/`、`CppScript`）是允许的，
> 禁用词约束的是**项目名 / 产物名**层面的对外命名。

## 2. 一句话原则

> **先锁契约，再锁加载类错误，最后锁正常闭环和运行期错误。**

## 3. 项目目标（framework §二）

1. 以 `core_contract.h` 作为**唯一契约**（宿主与插件之间只此一个头）。
2. 阶段 0：验证契约在 **C11 与 C++17** 下均可编译，编译期断言全部生效。
3. 阶段 1：验证 C++ 内核**加载器与加载类错误路径**。
4. 阶段 2：验证两个假插件（capture / policy）、正常 5 帧闭环与运行期错误路径。
5. 所有验收由 **PowerShell 脚本自动化**完成，不依赖硬件。

## 4. 阶段划分与门禁

| 阶段 | 主题 | 验收 |
|---|---|---|
| 0 | 契约定义 | C11 + C++17 双编译，断言全通过 |
| 1 | 加载器与加载类错误 | **11/11** 场景（`stage1_build_gate` + 10 个 bad_plugins） |
| 2 | 正常闭环与运行期错误 | **5/5** 场景（`stage2_build_gate` + 4 个 runtime_errors） |
| 3 | 契约扩展与真实插件集成 | 采集卡（Media Foundation）+ 串口输出 + 远程操作，真实插件与假插件**分目录隔离**，不破坏无硬件 CI |
| 4 | 远程操作与仲裁 | 结论：**仲裁架构演进后天然不需要**（两条输出路径从源头独立） |
| 5 | YOLO 推理集成 | ONNX Runtime + DirectML，policy 内产检测框 |
| 6 | 宿主内决策层与 me_lock | `IScript` / `ScriptHost` / `CppScript` 三层，替代早期 `Decider` 设计 |
| 7 | 战斗状态机 | 5 状态 `IDLE / CHASE / ATTACK / ATTACK_TURN / RECOVERY` |

**契约变更门禁**：契约一旦要改，必须**单独作为「契约变更阶段」重新验收所有插件**
（见 `docs/contract-change-procedure.md`）。

## 5. 范围边界（非目标）

### 5.1 框架层（framework §十 风险与禁止事项）

- 不提前引入真实截屏、真实输入、配置系统、Python、大模型、仲裁模块。
- 阶段 0/1/2 期间**不依赖硬件**（无采集卡、无串口也能全绿）。

### 5.2 UI 层（`docs/ui-design.md` §二，v1 明确不做）

| # | 不做 | 理由 |
|---|---|---|
| N1 | 画面叠加（检测框 / me / target / 攻击区） | 需要视频流传输，放二期 |
| N2 | 反向控制（启停 / 调参 / 注入按键） | 需要权限与并发同步，放三期 |
| N3 | 远程鉴权 | 同机 localhost，二期再说 |
| N4 | 帧级录制回放（场景 D） | 已有 `script_replay`，二期接同一份前端 |
| N5 | 引入 Qt / ImGui / Electron | 违背「UI 实现可替换」 |

**UI 可替换判据**：换 UI 实现时，`core.exe` 是否需要重编译？答案必须是「否」。

## 6. 验收入口

一键全量验收（**主门禁**）：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run_all.ps1
```

11 步，**不依赖硬件**，当前状态 **11/11 全绿**：

| STEP | 内容 |
|---|---|
| 1 | 契约编译（C11 + C++17） |
| 2 | OFF configure + build |
| 3 | OFF stubs 加载 |
| 4 | prepare_bad_plugin_dirs |
| 5 | OFF 阶段 1 验收（11 场景） |
| 6 | ON configure + build |
| 7 | script replay 构建 + 6 fixture 回放（`run_script_acceptance.ps1`） |
| 8 | output probe 构建 + 回放 |
| 9 | ON 正常 plugins 5 帧闭环 |
| 10 | prepare_runtime_error_dirs |
| 11 | ON 阶段 2 验收（5 场景） |

## 7. 关键文档索引

| 文档 | 内容 |
|---|---|
| `docs/module-host-framework.md` | 总体实施与分阶段落地（含实现状态 §十三） |
| `docs/contract-change-procedure.md` | 契约变更流程 |
| `docs/stage3.md` ~ `stage7.md` | 各阶段背景与实现状态 |
| `docs/output-manager.md` | 串口输出（115200 8N1，`mk.*` 命令格式） |
| `docs/recorder.md` | JSONL 录制 |
| `docs/ui-design.md` | 可视化界面设计（含 §13.5 攻击区几何规范） |
| `docs/env-traps.md` | 环境陷阱 E1~E6（**每发现一处追加一条**） |
| `README.md` | 各阶段验收命令速查 |
