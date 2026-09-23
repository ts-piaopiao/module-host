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
scripts\run_acceptance.ps1 -Suite bad_plugins
```

### 门禁说明

阶段 1 的 `bad_plugins` / `stubs` 门禁只在 `BUILD_STAGE2_PLUGINS=OFF` 下生效。阶段 1 的 `core.exe` 只做加载校验，不跑 5 帧循环；验收成功打印 `[内核] 加载成功`，退出码 0。

预期验收结果：`SUMMARY total=13 passed=13 failed=0`。

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
scripts\run_acceptance.ps1 -Suite runtime_errors
```

### 门禁说明

阶段 2 只跑正常 `plugins` 和 `runtime_errors`，不再验收 `bad_plugins` / `stubs`。

- 正常闭环：退出码 0，输出 `[帧 1]` 至 `[帧 5]`，末尾 `[内核] 5 帧完成`。
- 运行期错误：预期验收结果 `SUMMARY total=5 passed=5 failed=0`。

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

验收依赖的关键字与退出码以 `docs/module-host-framework.md`（根目录 `项目文档.md` 的权威副本）为准。
