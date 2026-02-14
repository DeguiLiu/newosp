# newosp API 参考: 调度与状态层

本文档覆盖 newosp 调度与状态管理模块的公共 API。

---

## hsm.hpp - 层次状态机

**概述**: 轻量级零堆分配层次状态机，支持 LCA 转换算法和 guard 条件。

**头文件**: `include/osp/hsm.hpp`

**依赖**: `platform.hpp`

### 配置宏

| 宏名 | 默认值 | 说明 |
|------|--------|------|
| `OSP_HSM_MAX_DEPTH` | 32 | 状态层次最大深度 |

### Event

事件结构体，传递给状态处理函数。

```cpp
struct Event {
  uint32_t id;        // 事件 ID
  const void* data;   // 可选负载指针
};
```

### TransitionResult

状态处理函数返回值。

| 枚举值 | 说明 |
|--------|------|
| `kHandled` | 事件已处理 |
| `kUnhandled` | 事件未处理，向父状态冒泡 |
| `kTransition` | 请求状态转换 |

### StateConfig\<Context\>

状态配置结构体。

**模板参数**: `Context` - 用户上下文类型

**成员**:

| 成员 | 类型 | 说明 |
|------|------|------|
| `name` | `const char*` | 状态名（静态生命周期） |
| `parent_index` | `int32_t` | 父状态索引，-1 表示根 |
| `handler` | `HandlerFn` | 事件处理函数 |
| `on_entry` | `EntryFn` | 进入动作（可为 nullptr） |
| `on_exit` | `ExitFn` | 退出动作（可为 nullptr） |
| `guard` | `GuardFn` | 守卫条件（可为 nullptr） |

### StateMachine\<Context, MaxStates\>

层次状态机模板类。

**模板参数**:
- `Context`: 用户上下文类型
- `MaxStates`: 最大状态数（默认 16）

**构造函数**:

```cpp
explicit StateMachine(Context& ctx) noexcept;
```

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|----------|------|------------|
| `int32_t AddState(const StateConfig<Context>&)` | 添加状态，返回状态索引 | 构建阶段 |
| `void SetInitialState(int32_t state_index)` | 设置初始状态 | 构建阶段 |
| `void Start()` | 启动状态机，进入初始状态 | 构建阶段 |
| `void Dispatch(const Event&)` | 分发事件 | 运行时 |
| `TransitionResult RequestTransition(int32_t target)` | 请求转换到目标状态 | 处理函数内 |
| `int32_t CurrentState() const` | 获取当前状态索引 | 线程安全 |
| `const char* CurrentStateName() const` | 获取当前状态名 | 线程安全 |
| `bool IsInState(int32_t state_index) const` | 检查是否在指定状态或其子状态 | 线程安全 |

**使用示例**:

```cpp
struct MyCtx { int value; };
MyCtx ctx{};
osp::StateMachine<MyCtx, 8> sm(ctx);
auto s0 = sm.AddState({"Idle", -1, handler_fn, nullptr, nullptr, nullptr});
sm.SetInitialState(s0);
sm.Start();
sm.Dispatch({EVENT_ID, nullptr});
```

---

## bt.hpp - 行为树

**概述**: 扁平数组存储的行为树，索引引用，零堆分配。

**头文件**: `include/osp/bt.hpp`

**依赖**: `platform.hpp`

### 配置宏

| 宏名 | 默认值 | 说明 |
|------|--------|------|
| `OSP_BT_MAX_CHILDREN` | 8 | 复合节点最大子节点数 |
| `OSP_BT_MAX_NODES` | 32 | 行为树最大节点数 |

### NodeStatus

节点执行状态。

| 枚举值 | 说明 |
|--------|------|
| `kSuccess` | 成功 |
| `kFailure` | 失败 |
| `kRunning` | 运行中 |
| `kIdle` | 未执行 |

### NodeType

节点类型。

| 枚举值 | 说明 |
|--------|------|
| `kAction` | 动作叶节点 |
| `kCondition` | 条件叶节点 |
| `kSequence` | 序列复合节点（AND 逻辑） |
| `kSelector` | 选择复合节点（OR 逻辑） |
| `kParallel` | 并行复合节点 |
| `kInverter` | 反转装饰器 |
| `kRepeat` | 重复装饰器 |

### BehaviorTree\<Context, MaxNodes\>

行为树模板类。

**模板参数**:
- `Context`: 用户上下文类型
- `MaxNodes`: 最大节点数（默认 32）

**构造函数**:

```cpp
explicit BehaviorTree(Context& ctx, const char* name = "bt") noexcept;
```

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|----------|------|------------|
| `int32_t AddAction(const char* name, TickFn fn, int32_t parent = -1)` | 添加动作节点 | 构建阶段 |
| `int32_t AddCondition(const char* name, TickFn fn, int32_t parent = -1)` | 添加条件节点 | 构建阶段 |
| `int32_t AddSequence(const char* name, int32_t parent = -1)` | 添加序列节点 | 构建阶段 |
| `int32_t AddSelector(const char* name, int32_t parent = -1)` | 添加选择节点 | 构建阶段 |
| `int32_t AddParallel(const char* name, uint32_t threshold, int32_t parent = -1)` | 添加并行节点 | 构建阶段 |
| `void SetRoot(int32_t node_index)` | 设置根节点 | 构建阶段 |
| `NodeStatus Tick()` | 执行一次 tick | 运行时 |
| `uint32_t NodeCount() const` | 获取节点数 | 线程安全 |

**使用示例**:

```cpp
BehaviorTree<MyCtx> tree(ctx, "patrol");
auto root = tree.AddSequence("root");
tree.AddAction("move", move_fn, root);
tree.SetRoot(root);
tree.Tick();
```

---

## executor.hpp - 调度器

**概述**: 单线程/静态/CPU 绑定/实时调度器，驱动 AsyncBus 消息处理。

**头文件**: `include/osp/executor.hpp`

**依赖**: `platform.hpp`, `node.hpp`

### 配置宏

| 宏名 | 默认值 | 说明 |
|------|--------|------|
| `OSP_EXECUTOR_MAX_NODES` | 16 | 调度器最大节点数 |

### SingleThreadExecutor\<PayloadVariant\>

单线程调度器，阻塞式轮询。

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|----------|------|------------|
| `bool AddNode(Node<PayloadVariant>&)` | 添加节点 | Spin 前 |
| `bool RemoveNode(const Node<PayloadVariant>&)` | 移除节点 | Spin 前 |
| `void Spin()` | 阻塞式循环处理消息 | 单线程 |
| `uint32_t SpinOnce()` | 处理一批消息后返回 | 单线程 |
| `void Stop()` | 停止调度器 | 线程安全 |
| `bool IsRunning() const` | 检查是否运行中 | 线程安全 |

### StaticExecutor\<PayloadVariant\>

后台线程调度器。

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|----------|------|------------|
| `bool AddNode(Node<PayloadVariant>&)` | 添加节点 | Start 前 |
| `void Start()` | 启动后台线程 | 线程安全 |
| `void Stop()` | 停止后台线程 | 线程安全 |
| `void SetHeartbeat(ThreadHeartbeat*)` | 设置心跳监控 | Start 前 |

### PinnedExecutor\<PayloadVariant\>

CPU 绑定调度器。

**构造函数**:

```cpp
explicit PinnedExecutor(int32_t cpu_core) noexcept;
```

**公共方法**: 同 `StaticExecutor`

### RealtimeConfig

实时调度配置。

**成员**:

| 成员 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `sched_policy` | `int32_t` | 0 | 调度策略（0=OTHER, 1=FIFO, 2=RR） |
| `sched_priority` | `int32_t` | 0 | 优先级（FIFO: 1-99） |
| `lock_memory` | `bool` | false | 是否锁定内存（mlockall） |
| `stack_size` | `uint32_t` | 0 | 栈大小（0=默认） |
| `cpu_affinity` | `int32_t` | -1 | CPU 绑定（-1=不绑定） |

### RealtimeExecutor\<PayloadVariant\>

实时调度器（Linux SCHED_FIFO/RR）。

**构造函数**:

```cpp
explicit RealtimeExecutor(const RealtimeConfig& cfg) noexcept;
```

**公共方法**: 同 `StaticExecutor`，额外提供 `const RealtimeConfig& GetConfig() const`

**使用示例**:

```cpp
RealtimeConfig cfg;
cfg.sched_policy = 1;  // SCHED_FIFO
cfg.sched_priority = 50;
cfg.cpu_affinity = 2;
RealtimeExecutor<MyPayload> exec(cfg);
exec.AddNode(node);
exec.Start();
```

---

## timer.hpp - 定时器调度

**概述**: 周期性和一次性定时器任务调度器，后台线程驱动。

**头文件**: `include/osp/timer.hpp`

**依赖**: `platform.hpp`, `vocabulary.hpp`

### TimerScheduler\<MaxTasks\>

定时器调度器模板类。

**模板参数**: `MaxTasks` - 最大任务槽数（默认 16）

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|----------|------|------------|
| `expected<TimerTaskId, TimerError> Add(uint32_t period_ms, TimerTaskFn fn, void* ctx = nullptr)` | 添加周期任务 | 线程安全 |
| `expected<TimerTaskId, TimerError> AddOneShot(uint32_t delay_ms, TimerTaskFn fn, void* ctx = nullptr)` | 添加一次性任务 | 线程安全 |
| `expected<void, TimerError> Remove(TimerTaskId task_id)` | 移除任务 | 线程安全 |
| `expected<void, TimerError> Start()` | 启动调度线程 | 线程安全 |
| `void Stop()` | 停止调度线程 | 线程安全 |
| `bool IsRunning() const` | 检查是否运行中 | 线程安全 |
| `uint32_t TaskCount() const` | 获取活跃任务数 | 线程安全 |
| `void SetHeartbeat(ThreadHeartbeat*)` | 设置心跳监控 | Start 前 |

**使用示例**:

```cpp
osp::TimerScheduler<8> sched;
auto task_id = sched.Add(1000, my_callback);
sched.Start();
// ... 运行 ...
sched.Stop();
```

---

## lifecycle_node.hpp - 生命周期节点

**概述**: HSM 驱动的层次生命周期状态机节点。

**头文件**: `include/osp/lifecycle_node.hpp`

**依赖**: `hsm.hpp`, `node.hpp`, `fault_collector.hpp`

### LifecycleState

粗粒度生命周期状态（向后兼容）。

| 枚举值 | 说明 |
|--------|------|
| `kUnconfigured` | 未配置 |
| `kInactive` | 已配置但未激活 |
| `kActive` | 运行中 |
| `kFinalized` | 终止状态 |

### LifecycleDetailedState

细粒度层次状态。

| 枚举值 | 说明 |
|--------|------|
| `kAlive` | 根状态 |
| `kUnconfigured` | 未配置父状态 |
| `kInitializing` | 初始化中 |
| `kWaitingConfig` | 等待配置 |
| `kConfigured` | 已配置父状态 |
| `kInactive` | 未激活父状态 |
| `kStandby` | 待命 |
| `kPaused` | 暂停 |
| `kActive` | 激活父状态 |
| `kStarting` | 启动中 |
| `kRunning` | 正常运行 |
| `kDegraded` | 降级运行 |
| `kError` | 错误父状态 |
| `kRecoverable` | 可恢复错误 |
| `kFatal` | 致命错误 |
| `kFinalized` | 终止 |

### LifecycleNode\<PayloadVariant\>

生命周期节点模板类。

**构造函数**:

```cpp
explicit LifecycleNode(const char* name, uint32_t id = 0) noexcept;
```

**回调注册**:

| 方法签名 | 说明 |
|----------|------|
| `void SetOnConfigure(LifecycleCallback cb)` | 设置配置回调 |
| `void SetOnActivate(LifecycleCallback cb)` | 设置激活回调 |
| `void SetOnDeactivate(LifecycleCallback cb)` | 设置停用回调 |
| `void SetOnCleanup(LifecycleCallback cb)` | 设置清理回调 |
| `void SetOnShutdown(LifecycleShutdownCallback cb)` | 设置关停回调 |
| `void SetFaultReporter(FaultReporter reporter)` | 设置故障报告器 |

**状态查询**:

| 方法签名 | 说明 | 线程安全性 |
|----------|------|------------|
| `LifecycleState GetState() const` | 获取粗粒度状态 | 线程安全 |
| `LifecycleDetailedState GetDetailedState() const` | 获取细粒度状态 | 线程安全 |
| `const char* DetailedStateName() const` | 获取状态名 | 线程安全 |
| `bool IsInDetailedState(LifecycleDetailedState) const` | 检查是否在指定状态 | 线程安全 |

**状态转换（向后兼容）**:

| 方法签名 | 说明 | 线程安全性 |
|----------|------|------------|
| `expected<void, LifecycleError> Configure()` | Unconfigured → Inactive | 单线程 |
| `expected<void, LifecycleError> Activate()` | Inactive → Active | 单线程 |
| `expected<void, LifecycleError> Deactivate()` | Active → Inactive | 单线程 |
| `expected<void, LifecycleError> Cleanup()` | Inactive → Unconfigured | 单线程 |
| `expected<void, LifecycleError> Shutdown()` | Any → Finalized | 单线程 |

**扩展转换**:

| 方法签名 | 说明 | 线程安全性 |
|----------|------|------------|
| `expected<void, LifecycleError> Pause()` | Running → Paused | 单线程 |
| `expected<void, LifecycleError> Resume()` | Paused → Running | 单线程 |
| `expected<void, LifecycleError> MarkDegraded()` | Running → Degraded | 单线程 |
| `expected<void, LifecycleError> ClearDegraded()` | Degraded → Running | 单线程 |
| `expected<void, LifecycleError> TriggerError()` | Any → Recoverable | 单线程 |
| `expected<void, LifecycleError> TriggerFatalError()` | Any → Fatal | 单线程 |
| `expected<void, LifecycleError> Recover()` | Recoverable → WaitingConfig | 单线程 |

**使用示例**:

```cpp
LifecycleNode<MyPayload> node("sensor", 1);
node.SetOnConfigure([]() { return true; });
node.SetOnActivate([]() { return true; });
node.Configure();
node.Activate();
```

---

## qos.hpp - QoS 配置

**概述**: 发布订阅通信的服务质量策略配置。

**头文件**: `include/osp/qos.hpp`

**依赖**: `platform.hpp`

### ReliabilityPolicy

可靠性策略。

| 枚举值 | 说明 |
|--------|------|
| `kBestEffort` | 尽力而为（UDP 语义） |
| `kReliable` | 可靠传输（TCP 语义） |

### HistoryPolicy

历史深度策略。

| 枚举值 | 说明 |
|--------|------|
| `kKeepLast` | 保留最后 N 条消息 |
| `kKeepAll` | 保留所有消息（直到队列满） |

### DurabilityPolicy

持久性策略。

| 枚举值 | 说明 |
|--------|------|
| `kVolatile` | 易失性（无后加入支持） |
| `kTransientLocal` | 瞬态本地（后加入者获取缓存消息） |

### QosProfile

QoS 配置结构体。

**成员**:

| 成员 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `reliability` | `ReliabilityPolicy` | `kBestEffort` | 可靠性策略 |
| `history` | `HistoryPolicy` | `kKeepLast` | 历史策略 |
| `durability` | `DurabilityPolicy` | `kVolatile` | 持久性策略 |
| `history_depth` | `uint32_t` | 10 | KeepLast 模式消息数 |
| `deadline_ms` | `uint32_t` | 0 | 消息超时丢弃（0=无限制） |
| `lifespan_ms` | `uint32_t` | 0 | 消息生命周期（0=无限制） |

### 预定义 QoS 配置

| 常量 | 说明 |
|------|------|
| `QosSensorData` | 传感器数据：尽力而为，保留最后 1 条 |
| `QosControlCommand` | 控制命令：可靠，保留所有，100ms 超时 |
| `QosSystemDefault` | 系统默认：尽力而为，保留最后 10 条 |
| `QosReliableSensor` | 可靠传感器：可靠，保留最后 5 条，1s 生命周期 |

### 辅助函数

| 函数签名 | 说明 |
|----------|------|
| `constexpr bool IsCompatible(const QosProfile& pub, const QosProfile& sub)` | 检查发布者和订阅者 QoS 兼容性 |
| `constexpr bool IsDeadlineExpired(const QosProfile&, uint32_t elapsed_ms)` | 检查消息是否超时 |
| `constexpr bool IsLifespanExpired(const QosProfile&, uint32_t elapsed_ms)` | 检查消息是否过期 |

---

## shutdown.hpp - 优雅关停

**概述**: POSIX 信号驱动的优雅关停管理器，LIFO 回调执行。

**头文件**: `include/osp/shutdown.hpp`

**依赖**: `platform.hpp`, `vocabulary.hpp`

### ShutdownManager

优雅关停管理器（单例模式）。

**构造函数**:

```cpp
explicit ShutdownManager(uint32_t max_callbacks = 16) noexcept;
```

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|----------|------|------------|
| `bool IsValid() const` | 检查实例是否有效 | 线程安全 |
| `expected<void, ShutdownError> Register(ShutdownFn fn)` | 注册关停回调 | 线程安全 |
| `expected<void, ShutdownError> InstallSignalHandlers()` | 安装 SIGINT/SIGTERM 处理器 | 单次调用 |
| `void Quit(int signo = 0)` | 手动触发关停 | 线程安全 |
| `void WaitForShutdown()` | 阻塞等待关停信号 | 单线程 |
| `bool IsShutdownRequested() const` | 检查是否已请求关停 | 线程安全 |

**使用示例**:

```cpp
osp::ShutdownManager mgr;
mgr.Register([](int) { cleanup(); });
mgr.InstallSignalHandlers();
mgr.WaitForShutdown();
```

---

## shell.hpp - 调试 Shell

**概述**: Telnet 远程调试 Shell，支持命令注册和自动补全。

**头文件**: `include/osp/shell.hpp`

**依赖**: `platform.hpp`, `vocabulary.hpp`

### DebugShell

Telnet 调试服务器。

**配置结构体**:

```cpp
struct Config {
  uint16_t port;              // TCP 监听端口（默认 5090）
  uint32_t max_connections;   // 最大并发会话数（默认 2）
  const char* prompt;         // 提示符（默认 "osp> "）
};
```

**构造函数**:

```cpp
explicit DebugShell(const Config& cfg = Config{}) noexcept;
```

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|----------|------|------------|
| `expected<void, ShellError> Start()` | 启动 telnet 服务器 | 线程安全 |
| `void Stop()` | 停止服务器并关闭所有会话 | 线程安全 |
| `bool IsRunning() const` | 检查是否运行中 | 线程安全 |
| `static int Printf(const char* fmt, ...)` | 向当前会话输出（命令回调内使用） | 命令上下文 |
| `void SetHeartbeat(ThreadHeartbeat*)` | 设置心跳监控 | Start 前 |

### 命令注册宏

```cpp
OSP_SHELL_CMD(cmd_function, "description")
```

**使用示例**:

```cpp
static int reboot(int argc, char* argv[]) {
  osp::DebugShell::Printf("Rebooting...\n");
  return 0;
}
OSP_SHELL_CMD(reboot, "Reboot the system");

osp::DebugShell::Config cfg;
cfg.port = 5090;
osp::DebugShell shell(cfg);
shell.Start();
```

---

**文档版本**: 2024-02-14
**项目**: newosp (C++17 header-only 嵌入式基础设施库)
