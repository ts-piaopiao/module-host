# 环境陷阱记录

**用途**：记录 module-host 项目开发过程中踩过的环境/工具链陷阱，供新对话与新人参考。
**维护规则**：每发现一处，追加一条。已修复的标注修复 commit。

---

## E1：PowerShell 5.1 对 UTF-8 无 BOM 的 `.ps1` 按 GBK 解码

**现象**：脚本运行行为与源码不符。某行赋值、调用或控制语句看似存在，却从未生效，
且 `$ErrorActionPreference = 'Stop'` 不报错，`PSParser::Tokenize` 返回 `errs=0`。

**根因**：Windows PowerShell 5.1 加载 `.ps1` 文件时，若文件无 BOM，
按系统 ANSI 代码页（简体中文 Windows 为 GBK）解码。UTF-8 中文注释的字节序列
在 GBK 视角下可能被解成**以续行符（反引号 `` ` ``）结尾**，
导致 PowerShell 把下一行**续进注释**。赋值被静默吞掉。

**首次踩坑**：`scripts/run_script_acceptance.ps1` 加入
`$env:MH_SCRIPT_SEED = '42'`（前一行是 4 行中文注释），
5 次连跑失败 1 次、15 次连跑失败 6 次，全部是 `I4` 违例。
调试发现 `$env:MH_SCRIPT_SEED` 始终为空。
Token 级证据：`L3` 注释 token 把 `$env:MH_SCRIPT_SEED = '42'` 一起吞了。
修复：`515674d`，文件改 UTF-8 with BOM。

**规则**：

- **含非 ASCII 字符的 `.ps1` 必须存为 UTF-8 with BOM**。
- 纯 ASCII 的 `.ps1` 可无 BOM，安全。
- 新增 `.ps1` 且含中文时，落盘即加 BOM，不要等踩坑。

**检查方法**：

```powershell
# 首 3 字节
[System.IO.File]::ReadAllBytes('path\to\script.ps1')[0..2]   # 期望 0xEF 0xBB 0xBF

# Token 级：确认无注释吞行
$errs = $null
$tokens = [System.Management.Automation.PSParser]::Tokenize(
    [System.IO.File]::ReadAllText('path\to\script.ps1'), [ref]$errs)
$errs.Count                                                    # 期望 0
$tokens | Where-Object { $_.Type -eq 'Comment' } | ForEach-Object {
    "L{0} ({1} lines): {2}" -f $_.StartLine, ($_.EndLine - $_.StartLine + 1), $_.Content
}
# 每个 Comment token 必须只跨 1 行（EndLine == StartLine）
```

**当前状态**：`scripts/` 下全部含中文的 `.ps1` 均已 BOM=True。

---

## E2：`.ps1` 行尾不统一，改动时必须保持原样

**现象**：用 `Set-Content` 或文本编辑器保存 `.ps1`，行尾被隐式统一为 CRLF 或 LF，
导致 `git diff` 显示整个文件重写。

**根因**：仓库中 `.ps1` 存在两种行尾（`run_all.ps1` 为 CRLF，
`run_script_acceptance.ps1` 为 LF），无统一约定。`core.autocrlf=true`
在工作区与索引间做转换，容易误判。

**规则**：修改 `.ps1` 时**保持该文件原有行尾**，不做统一。写入用：

```powershell
$text = [System.IO.File]::ReadAllText($path)
# 修改 $text
[System.IO.File]::WriteAllText($path, $text, [System.Text.UTF8Encoding]::new($true))
```

`WriteAllText` 按字符串原样写出，不改变 `\r\n` / `\n`。

---

## E3：`// \` 注释末尾反斜杠会触发行拼接

**现象**：C++ 注释末尾若是反斜杠 `\`，编译器会把下一行**拼进注释**，吞掉代码。

**根因**：C++ 预处理器在词法分析前做行拼接（line splicing），
反斜杠 + 换行被视为续行。

**规则**：注释末尾不要以 `\` 结束。若要写路径，改用 `/` 或加空格。

---

## E4：`GetTickCount64` 与 `Sleep` 精度 15.6ms

**现象**：脚本按键落点呈 33ms 梳状，`mod N` 分布不均匀。

**根因**：`GetTickCount64` 分辨率 15.6ms（未调 `timeBeginPeriod` 时），
`Sleep()` 同理。这两者共同掩盖了真实抖动。

**规则**：

- 高精度时间戳用 **QPC**（`QueryPerformanceCounter`）。
- 高精度等待用 **`CreateWaitableTimerEx`**。

**相关 commit**：`851a374`（recorder 换 QPC）、`810a715`（高精度定时器）。

---

## E5：`Set-Content -Encoding UTF8` 会加 BOM

**现象**：用 `Set-Content -Encoding UTF8` 写 `.md` / `.cpp`，文件被加上 BOM，
污染仓库（`git diff` 首行异常，部分工具不识别）。

**根因**：PowerShell 5.1 的 `-Encoding UTF8` 等价于 `UTF8 with BOM`。

**规则**：

- 写无 BOM 文件用：
  `[System.IO.File]::WriteAllText($path, $text, [System.Text.UTF8Encoding]::new($false))`
- 写 BOM 文件用：
  `[System.IO.File]::WriteAllText($path, $text, [System.Text.UTF8Encoding]::new($true))`
- **禁止** `Set-Content -Encoding UTF8`。

---

## E6：`findstr` 中文匹配失败

**现象**：`findstr /C:"中文"` 在 UTF-8 文件上匹配不到。

**根因**：`findstr` 按系统代码页解码，与 UTF-8 文件编码不匹配。

**规则**：搜中文用 ASCII 关键字（如搜 `MH_SCRIPT_SEED` 而非搜 `种子`），
或用 PowerShell `Select-String -Encoding UTF8`。

---

## E7：PowerShell 双引号内 `@{u}` 被解析为哈希表

**现象**：`git log --oneline origin/master.."@{u}"` 报

```text
fatal: ambiguous argument 'dQA=': unknown revision or path not in the working tree.
```

**根因**：PowerShell 把双引号内的 `@{u}` 当作**哈希表字面量**求值——
`@` 是哈希表起始符，`{u}` 被当键名处理，字符串被求值成 `dQA=` 之类
无意义内容再传给 git，于是 git 收到一个不存在的 revision。

**首次踩坑**：`b0d2a0c` v3 收尾包（2026-09-30），push 后核对
`origin/master..@{u}` 时触发；当次为纯引号问题，git 本身正常。

**规则**：

- 要引用 upstream 就用**单引号**：`git rev-parse '@{u}'`。
- 或直接写目标名，绕开该语法：`git rev-parse origin/master`。
- 凡 PowerShell 中含 `@` / `$` / `{}` 的字面量传给外部命令，一律**单引号**。

---

## E8：用陈旧 `origin/master` 引用估算「本地领先数」

**现象**：`git rev-list --count origin/master..HEAD` 得到 **33**，
据此外报「本地领先 33、push 将发布 33 个提交」；
实际 `git push` 输出的范围是 `f85acf6..b0d2a0c`，**新发布只有 9 个**
（远程 tip 早已是 `f85acf6`，本地引用却还停在 `eda8543`）。

**根因**：`origin/master` 是**上次 fetch 时的快照**，别人/别的机器 push 过之后
本地不会自动更新。用它算 ahead/behind **只在刚 fetch 之后可信**。

**首次踩坑**：`b0d2a0c` v3 收尾包（2026-09-30），**误导过一次 push 影响面判断**
（已据此在 push 前贴出核对、push 后用 fetch 复验，实际未造成错误操作）。

**规则**：

- 涉及「领先 / 落后多少」「这次 push 会发布多少」的判断前，**先 `git fetch origin`**，
  再 `git rev-list --count`。
- 交叉验证三者是否一致：
  `git rev-parse HEAD` / `git rev-parse origin/master` / `git rev-parse '@{u}'`（见 E7）。
- push 的**实际影响面以 push 输出里的 `old..new` 范围为准**，不以本地估算为准。

---

## E9：浏览器访问 `localhost:6601` 返回 502（须用 `127.0.0.1`）

**现象**：`core.exe` 起遥测服务器后，浏览器打开 `http://localhost:6601`
反复返回 `502 Bad Gateway`；改用 `http://127.0.0.1:6601` 立即 200。

**根因**：`telemetry_server.cpp` 用 `svr.listen("127.0.0.1", port)` 只绑 **IPv4**；
`localhost` 在现代浏览器 / 系统里可能优先解析为 **IPv6 `::1`**，或经系统代理转发，
落到无监听的 IPv6 回环或代理上，表现为 502。

**首次踩坑**：`05f65bc` 之后的 UI 实测（2026-10-05），
用于确认几何 v3 前端形状时触发。

**规则**：

- 访问遥测页面一律用 `http://127.0.0.1:6601`，不用 `localhost`。
- 若必须支持 `localhost`，需把服务器改为同时绑 IPv4 + IPv6
  （`svr.listen("0.0.0.0", port)` 或双栈）。
- 文档 / 提示里写地址时明确写 `127.0.0.1`。

---

**文档完**。
