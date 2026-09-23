# module-host

模块宿主系统框架。

| 项 | 名称 |
|---|---|
| 项目名 | `module-host` |
| CMake 工程名 | `ModuleHost` |
| 核心 target | `module_host_core` |
| 产物 | `core.exe` |
| 唯一契约 | `core_contract.h` |

一句话原则：先锁契约，再锁加载类错误，最后锁正常闭环和运行期错误。

---

## 阶段 0：契约定义

### 目标

验证 `core_contract.h` 在 C11 与 C++17 下均可编译，编译期断言全部生效。

### 产出

- `core_contract.h`
- `tests/test_contract_c.c`
- `tests/test_contract_cpp.cpp`

### 验收命令

```powershell
cl /nologo /std:c11 /I. /c tests\test_contract_c.c
cl /nologo /std:c++17 /I. /c tests\test_contract_cpp.cpp
```

两个编译都必须通过，断言全部生效。

---

## 阶段 1：加载器与加载类错误路径

### 目标

验证 C++ 内核加载器与加载类错误路径：缺 DLL、缺符号、ABI 不匹配、元数据无效、kind 不匹配、`plugin_init` 失败；三个最小桩齐全时加载成功。

### 构建命令

```powershell
cmake -S . -B build -DBUILD_STAGE2_PLUGINS=OFF
cmake --build build --config Release
```

### 验收命令

```powershell
build\Release\core.exe --plugins-dir build\Release\stubs
scripts\prepare_bad_plugin_dirs.ps1
scripts\run_acceptance.ps1 -Stage stage1
```

### 门禁说明

阶段 1 的 `bad_plugins` / `stubs` 门禁只在 `BUILD_STAGE2_PLUGINS=OFF` 下生效。阶段 1 的 `core.exe` 只做加载校验，不跑 5 帧循环；验收成功打印 `[内核] 加载成功`，退出码 0。

`-Stage stage1` 会强制校验 `core.exe` 为 OFF 构建；`-Stage stage2` 会强制校验 `core.exe` 为 ON 构建；兼容旧用法 `-Suite bad_plugins|runtime_errors|all`。

预期验收结果：`SUMMARY` 中 `failed=0`（`-Stage stage1` 含 `stage1_build_gate` 与 `bad_plugins` 全场景）。

---

## 阶段 2：正常闭环与运行期错误

### 目标

验证三个假插件的 5 帧正常闭环，以及运行期错误路径：`capture_fail`、`decide_fail`、`decide_over_count`、`decide_empty`、`execute_fail`。

### 构建命令

```powershell
cmake -S . -B build -DBUILD_STAGE2_PLUGINS=ON
cmake --build build --config Release
```

### 验收命令

```powershell
build\Release\core.exe --plugins-dir build\Release\plugins
scripts\prepare_runtime_error_dirs.ps1
scripts\run_acceptance.ps1 -Stage stage2
```

### 门禁说明

阶段 2 只跑正常 `plugins` 和 `runtime_errors`，不再验收 `bad_plugins` / `stubs`。

`-Stage stage1` 会强制校验 `core.exe` 为 OFF 构建；`-Stage stage2` 会强制校验 `core.exe` 为 ON 构建；兼容旧用法 `-Suite bad_plugins|runtime_errors|all`。

- 正常闭环：退出码 0，输出 `[帧 1]` 至 `[帧 5]`，末尾 `[内核] 5 帧完成`。
- 运行期错误：`-Stage stage2` 含 `stage2_build_gate` 与 `runtime_errors` 全场景，`failed=0` 为通过。

---

## 阶段 3：真实硬件集成

### 目标

把假插件替换成真实硬件插件：采集卡拉帧、串口发键鼠。
契约从 ABI=1 升到 ABI=2，新增 core_action、core_execute_result 等。

### 真实插件目录

真实插件输出到 build\Release\plugins_real\：
- capture_plugin.dll（从采集卡拉帧，1920x1080@30 BGRA8）
- input_plugin.dll（走串口 115200 8N1，发 mk.* 文本命令）
- policy_plugin.dll（当前为测试桩，产出"按 I"两个动作）

### 运行命令

    core.exe --config build\Release\real_test.ini

real_test.ini 示例：

    plugins_dir = D:\dev\module-host\build\Release\plugins_real
    frames = 5
    capture_device = 0
    capture_width = 1920
    capture_height = 1080
    capture_fps = 30
    capture_format = auto
    input_port = COM6
    input_baud = 115200

### 前置条件

- 采集卡插在 USB 口，被系统识别为视频采集设备
- 串口设备插在 COM6（或配置里指定的端口），未被子程序占用
- 真实插件只在 BUILD_STAGE2_PLUGINS=ON 时构建
- 假插件（plugins/）仍然存在，用于无硬件环境下的回归

### 不在本阶段范围

- 真实 policy（YOLO 推理，独立任务）
- 远程画面推流
- 人工操作与自动决策的仲裁

---

## core.exe 命令行

### 用法

    core.exe --plugins-dir <目录> [--config <路径>]
    core.exe --help
    core.exe --version

### 选项

| 选项 | 说明 |
|---|---|
| `--plugins-dir <目录>` | 指定插件目录，优先级高于配置文件 |
| `--config <路径>` | 指定配置文件 |
| `--help, -h` | 显示帮助并退出，优先级最高 |
| `--version, -v` | 显示版本并退出，优先级最高 |

### 优先级

1. `--help` / `--version` 出现即早退，不读配置文件，不解析插件目录。
2. `--plugins-dir` 命令行覆盖配置文件中的 `plugins_dir`。
3. 都没有时打印 `[错误] 缺失参数: --plugins-dir` 并退出非 0。

### 配置文件格式

极简 key = value 行格式，UTF-8：

    # 以 # 开头为整行注释
    plugins_dir = D:\path\to\plugins
    frames = 5
    log_path = D:\path\to\core.log

支持的键：

| 键 | 类型 | 说明 |
|---|---|---|
| `plugins_dir` | 字符串 | 插件目录，命令行 `--plugins-dir` 优先 |
| `frames` | 整数 1..100 | 阶段 2 的帧数，默认 5 |
| `log_path` | 字符串 | 日志文件路径，同时写到 stdout 和该文件 |
| `capture_device` | 整数 | 采集卡设备索引，默认 0 |
| `capture_width` | 整数 | 期望宽度，默认 1920 |
| `capture_height` | 整数 | 期望高度，默认 1080 |
| `capture_fps` | 整数 | 期望帧率，默认 30 |
| `capture_format` | 字符串 | yuy2 / mjpg / nv12 / auto，默认 auto |
| `input_port` | 字符串 | 串口号，如 COM6，默认 COM6 |
| `input_baud` | 整数 | 波特率，默认 115200 |

规则：
- 以 `capture_` / `policy_` / `input_` 开头的键由对应插件解释，宿主只透传不校验。
- 其他未知键报 `[错误] 配置无效: 未知配置项: <key>` 并退出 1。
- 无 `=` 的行报 `[错误] 配置无效: 配置行格式错误` 并退出 1。
- `frames` 非数字报 `[错误] 配置无效: frames 不是数字` 并退出 1。
- `frames` 越界报 `[错误] 配置无效: frames 超出范围: <值>` 并退出 1。
- `log_path` 打不开时报 `[错误] 日志打开失败: <path>` 并退出 1。
- 未提供 `--config` 时不读任何配置文件，行为与之前完全一致。

### 日志

- `log_path` 生效时，所有 `[内核]` / `[帧 N]` / `[错误]` 输出同时写到 stdout 和日志文件。
- `--help` / `--version` 的输出不写日志。
- 未提供 `log_path` 时不写日志。

---

## 错误消息模板清单

与项目文档一致，错误消息模板固定如下：

```text
[错误] 缺失 DLL: capture_plugin.dll
[错误] 缺失符号: plugin_capture
[错误] 元数据无效: 段数不是 4
[错误] ABI 不匹配: 期望 1 实际 2
[错误] 初始化失败: fake_capture
[错误] 捕获失败
[错误] 决策失败
[错误] out_count 违约: 9 > 8
[错误] 执行失败
```

验收依赖的关键字与退出码以 `docs/module-host-framework.md`（权威设计文档）为准。
