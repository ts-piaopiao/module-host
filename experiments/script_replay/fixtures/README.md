# Script Replay Fixtures

本目录存放脚本层回放验收用的 fixture（events.jsonl 格式）。

## 文件列表

| 文件 | 来源 | 帧数 | 用途 |
|---|---|---|---|
| `real_session_600f.jsonl` | 真实录制的 events.jsonl（2026-09-25，real_capture + yolo_policy）| 599 | 真实场景回归，含正面进带、贴脸后退等 |
| `turn_scene.jsonl` | **手工构造的合成场景**（PowerShell 生成）| 300 | 覆盖 ATTACK_TURN（背面进带触发转身），真实数据缺少此路径 |

## 格式

每行一个 JSON 对象，与 recorder 输出一致：
  {"t":毫秒,"frame":N,"type":"det","cls":0|1,"id":N,"conf":F,"cx":F,"cy":F,"w":F,"h":F}

## 注意事项

- turn_scene.jsonl 的 t 严格 +33ms/frame，target 的 cx 仅 0.2/0.6/0.8 三档（me 恒 0.5），conf 恒 0.95——是合成数据，不能作为"真实场景"证据
- real_session_600f.jsonl 无 dec/hum 行（recorder 从 det 摘出），缺少人工事件对照
