# 阶段 8：会话记录

## 一、目标

为后续调试、回归测试、性能分析提供统一的会话记录基础设施。

## 二、设计取舍

参考 Python 项目的 session_store.py（Parquet），C++ 版本简化：

| Python | C++ | 理由 |
|---|---|---|
| Parquet | JSONL | C++ 引 pyarrow 代价大；JSONL 人可读、grep 友好 |
| 4 张表分开 | 单文件 + type 字段 | 单次运行几 MB 可接受，读分析方便 |
| 队列 + 后台线程 | 一样 | 主循环不被磁盘 IO 拖 |
| 会话目录 + 时间戳 | 一样 | 多次运行不混淆 |
| 7 天清理 | 一样 | 防磁盘堆积 |

## 三、输出格式

每行一个 JSON 对象，字段紧凑：

  {"t":123456,"type":"det","frame":42,"cls":0,"id":3,"conf":0.85,"cx":0.48,"cy":0.55,"w":0.02,"h":0.06}
  {"t":123456,"type":"dec","frame":42,"me_valid":1,"me_fx":0.48,"me_fy":0.58,"out":[{"kind":3,"a":39,"b":1}]}
  {"t":123456,"type":"hum","frame":42,"kind":3,"a":73,"b":1}
  {"t":123456,"type":"err","level":"ERR","msg":"..."}

t = GetTickCount64() 毫秒
type = det | dec | hum | err
frame = 帧号

## 三之二、JSON 实现

用 nlohmann/json（header-only，单头文件）。
存放位置：third_party/nlohmann/json.hpp
CMake 里把 third_party 加到 include 路径。

不自己手写 JSON 拼接（转义/浮点格式易错）。

## 四、会话目录

  <record_dir>/session_<YYYYMMDD_HHMMSS>/
    meta.json
    events.jsonl

meta.json：版本、配置、开始时间、ABI 版本。

启动时清理超过 7 天的 session_* 目录。

## 五、命令行参数（不是 config）

record 不是 config 键，而是命令行参数：

  core.exe --record <会话根目录>

- `--record <路径>`：启动时开启记录，会话目录建在指定路径下。
- 未提供 `--record` 时不记录，行为与之前一致。
- 记录路径由 main.cpp 解析，不经过 LoadConfigFile。
- 启动失败（无法创建目录）打印 `[错误] 记录启动失败: <path>`，程序继续运行（只是不记录），不退出。

## 六、架构

新增 src/core/recorder.h / .cpp：

  class Recorder {
  public:
      Recorder();
      ~Recorder();

      // 启动。返回 false 表示无法创建文件（不阻塞程序运行，只是不记录）。
      bool Start(const std::string& record_dir);

      // 停止并 flush。幂等。
      void Stop();

      // 以下方法线程安全，热路径调用，只入队不写盘。
      void RecordDetection(uint64_t frame, const core_detection& d);
      void RecordDecision(uint64_t frame, bool me_valid,
                          float me_fx, float me_fy,
                          const core_decision* dec);
      void RecordHuman(uint64_t frame, const core_action& a);
      void RecordError(const char* msg);

  private:
      struct Impl;
      Impl* impl_;
  };

内部：
- 队列（mutex + deque）
- 后台线程：从队列取，拼 JSON 行，写文件
- 每 2 秒或队列累积 1000 条时 flush
- Stop() 时排空队列
- 队列上限 10000 条。满了丢弃最旧并记录一条 [recorder] queue overflow 到 stderr。

### 生命周期

main.cpp 里用 unique_ptr + 自定义 deleter 保证任何 return 路径都触发 Stop：

  struct RecorderDeleter {
      void operator()(Recorder* r) const {
          if (r) { r->Stop(); delete r; }
      }
  };
  std::unique_ptr<Recorder, RecorderDeleter> recorder;

理由：main 里有多条 return 路径（capture 失败 / decide 失败 / execute 失败），
必须保证每条路径都 flush 日志。

## 七、main.cpp 集成

main 开头：
  if (config.record) {
      recorder.Start(config.record_dir);
  }

ON 5 帧循环里：
- capture 之后、policy.decide 之前记帧号
- policy.decide 之后：对每个 det 调 RecordDetection
- script_host.GetDecision 之后：调 RecordDecision（me 状态从 ScriptHost / 脚本内部获取）
- 人工事件 pop 之后：对每个事件调 RecordHuman
- LogPrintf 有 ERROR 级别的，调 RecordError（可选，暂缓）

main 结尾不需要显式调用 Stop（RAII deleter 自动触发，见第六节）。

## 八、不做

- Parquet
- 崩溃捕获 hook
- 日志分级
- 串口帧记录（后续加）

## 九、验收

- config 打开 record 后，会话目录生成
- events.jsonl 每行是合法 JSON
- 主循环帧率不受影响（record=1 与 record=0 对比）
- 强杀进程时，已 flush 的记录保留

## 十、一句话原则

只入队不写盘；简单格式；会话隔离。
