# techContext — 技术栈、环境与约束

> 环境/工具链变化、新增依赖或踩到新坑时更新本文件，并同步 `docs/env-traps.md`。

## 1. 技术栈

| 层 | 选型 |
|---|---|
| 语言 | C++17（宿主与插件）、C11（契约交叉验证） |
| 构建 | CMake + MSVC（Visual Studio 18 Community，x64） |
| 平台 | Windows，PowerShell **5.1** 执行脚本 |
| 推理 | ONNX Runtime + DirectML（`yolo_policy`，阶段5） |
| 采集 | Media Foundation（`real_capture`，1920x1080@30 BGRA8） |
| 输出 | 串口 **115200 8N1**，命令格式 `mk.*` |
| 远程 | TCP：JPEG 推流下行 + 键鼠事件上行（`remote_server` / `remote_client`） |
| 遥测 | 自研 HTTP + WebSocket（`telemetry_server`），端口 **6601** |
| 前端 | 无框架，原生 JS + Canvas，**内嵌**在 `telemetry_server.cpp` 的字符串里 |
| 版本控制 | git（`core.autocrlf=true`） |

## 2. 构建与验收命令

### 2.1 构建（必须先加载 VsDevCmd）

```powershell
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul 2>&1 && cmake --build build --config Release'
```

配置：

```powershell
cmake -S . -B build -DBUILD_STAGE2_PLUGINS=OFF     # OFF：无硬件 / 阶段0-2
cmake -S . -B build -DBUILD_STAGE2_PLUGINS=ON      # ON ：带真实插件
```

### 2.2 验收

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run_all.ps1     # 主门禁，11 步
powershell -ExecutionPolicy Bypass -File scripts\run_script_acceptance.ps1
powershell -ExecutionPolicy Bypass -File scripts\run_output_probe.ps1
```

### 2.3 手动跑 trace（易错）

```powershell
$env:MH_SCRIPT_SEED = '42'    # ⚠ 必须手动设置！run_script_acceptance.ps1 是在脚本内部设置的
experiments\script_replay\build\Release\script_replay.exe `
    --input experiments\script_replay\fixtures\<name>.jsonl `
    --output <path>\<name>.jsonl
```

### 2.4 起遥测界面

```powershell
build\Release\core.exe --replay <trace.jsonl> --replay-speed 1.0
# 浏览器打开 http://localhost:6601
```

`--replay` 模式**不加载插件、不开串口**，读 trace 重新发布到遥测后退出。
注意：`RunReplayMode` 结束即 `return`，回放跑完进程会退出。

### 2.5 git 路径

git 不在默认 PATH，需追加：`$env:Path += ";C:\Program Files\Git\cmd"`

## 3. 编码约束（**高危，务必遵守**）

1. **含中文的 `.ps1` 必须存为 UTF-8 with BOM**（E1，见 `docs/env-traps.md`）。
2. **MSVC 编译含中文的 C/C++ 源文件必须加 `/utf-8`**，否则字符串常量乱码。
3. `.ps1` 行尾统一 CRLF，改完不要「顺手统一换行」（E2）。
4. 注释末尾不要出现 `\`（E3 会触发续行，可能拼错符号）。
5. PowerShell 控制台默认 GBK：`git diff` 里的中文在 PowerShell 里会乱码。
   要拿**原文**用 cmd 原生重定向保留字节：
   `cmd /c "git diff -- <file> > out.diff"` 再用编辑器读。
6. `Get-Content` 读文件需显式 `-Encoding UTF8` 才能正确读 UTF-8 中文。

## 4. 环境陷阱索引（`docs/env-traps.md`，目前 E1–E9）

| # | 陷阱 | 要点 |
|---|---|---|
| E1 | PS 5.1 对无 BOM `.ps1` 按 GBK 解码 | 中文注释可能吞掉下一行（反引号续行），赋值静默失效；已修 `515674d` |
| E2 | `.ps1` 行尾不统一 | 保留原文件行尾 |
| E3 | 注释末尾 `\` | 可能触发续行拼错 token |
| E4 | `GetTickCount64` / `Sleep` 分辨率 **15.6ms** | 时序断言不能按 1ms 精度写 |
| E5 | `Set-Content -Encoding UTF8` 写出 BOM | 想要无 BOM 时会踩 |
| E6 | `findstr` 正则匹配失败 | 复杂模式改用 PowerShell `-match` / `-like` |
| E7 | PowerShell 双引号内 `@{u}` 被解析为哈希表 | 用单引号 `'@{u}'` 或 `origin/master`；含 `@`/`$`/`{}` 的字面量一律单引号 |
| E8 | 用陈旧 `origin/master` 引用估算领先数 | 算 ahead/behind 前先 `git fetch origin`；push 影响面以 push 输出 `old..new` 为准 |
| E9 | `localhost:6601` 返回 502（须用 `127.0.0.1`） | 遥测服务器只绑 IPv4；浏览器可能解析 localhost 为 IPv6 |

> **维护规则**：每发现一处追加一条；已修复的标注修复 commit。

## 5. 关键常量与枚举

### 5.1 脚本状态

```text
StateId: IDLE=0, CHASE=1, ATTACK=2, ATTACK_TURN=3, RECOVERY=4
```

### 5.2 攻击带几何（`script_config.h` ↔ 前端 `BAND_*` 必须同值）

```text
kBandXApex      = 0.005     尖端 x（贴角色正前方）
kBandApexSlope  = 1.0       顶点半角正切（1.0 → 顶点总角 90°）
kBandXMaxSame   = 0.1458    远端竖直边 x（= 280px @1920）
kBandYMin       = -0.074    上边界
kBandYMax       = +0.019    下边界
```

前端同名常量：`BAND_X_APEX` / `BAND_X_FAR` / `BAND_APEX_SLOPE` / `BAND_Y_TOP` / `BAND_Y_BOT`。
`kBandXMin`、`kBandXBulge`、`BAND_X_MIN`、`BAND_X_BULGE`、`BAND_Y_HALF_*` **已于 v3 删除**。

### 5.3 目标筛选

```text
cls == 1
|d_fy| <= 0.028      同平台
```

### 5.4 遥测端口

`6601`（`main.cpp` 中 `telemetry->Start(6601)`）

## 6. 依赖与约束

- **无硬件也能全绿**：阶段 0/1/2 + script + output probe 全部不依赖采集卡/串口。
- 真实插件输出在 `plugins_real/`，与假插件 `plugins/` 分离，避免污染 CI。
- 插件配置由宿主 `--config` **原文透传**，前缀 `capture_` / `policy_` 由宿主放行、
  插件自行解析。
- `script_replay.exe` 的 `--output` **缺省不落盘**，不给参数就没有 trace 产出。
- 前端脚本是**闭包作用域**，页面里的 `BAND_X_APEX` / `drawBand` 等**在 evaluate 里不可达**
  （`window.X` 不存在）；要验证前端几何只能读 canvas 像素或看源码。

## 7. 常见误判（别踩）

| 现象 | 真实原因 |
|---|---|
| 改了几何但某 fixture SHA 不变 | 该 fixture 内 in-band 判定**翻转帧 = 0**（**不是没编进去**）；v2 曾归因「窄框恒真」，v3 实测判据是逐帧比对 flip count |
| `git diff --stat` 报 LF→CRLF warning | `core.autocrlf=true` 的正常提示，非错误 |
| 前端页面 `frame=--` | 回放 `buffering` 态本来就不渲染，等 `replay_end` |
| 前端永远 `me unlocked` | 用了 fixture 而非 trace 喂 `core --replay` |
| trace 与预期不符 | 先确认 `MH_SCRIPT_SEED` 是否真的设上了（E1 吞行） |
