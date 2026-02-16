# newosp Shell 命令参考手册

本文档列出 newosp 调试 Shell 支持的所有命令、参数格式和使用示例。

Shell 通过 telnet 连接访问（默认端口 2323），支持 TAB 补全、历史记录（上下箭头）和 IAC 协议。

---

## 目录

- [内置命令](#内置命令)
- [运行时控制命令](#运行时控制命令)
  - [osp_log -- 日志级别控制](#osp_log----日志级别控制)
  - [osp_config -- 配置查看与修改](#osp_config----配置查看与修改)
  - [osp_bus -- 消息总线统计与控制](#osp_bus----消息总线统计与控制)
  - [osp_lifecycle -- 生命周期节点状态与转换](#osp_lifecycle----生命周期节点状态与转换)
- [只读诊断命令](#只读诊断命令)
  - [osp_watchdog -- 线程看门狗状态](#osp_watchdog----线程看门狗状态)
  - [osp_faults -- 故障收集器统计](#osp_faults----故障收集器统计)
  - [osp_pool -- 工作线程池统计](#osp_pool----工作线程池统计)
  - [osp_nodes -- HSM 节点管理器状态](#osp_nodes----hsm-节点管理器状态)
  - [osp_nodes_basic -- 基础节点管理器状态](#osp_nodes_basic----基础节点管理器状态)
  - [osp_service -- 服务 HSM 状态](#osp_service----服务-hsm-状态)
  - [osp_discovery -- 发现 HSM 状态](#osp_discovery----发现-hsm-状态)
  - [osp_transport -- 传输序列追踪](#osp_transport----传输序列追踪)
  - [osp_serial -- 串口传输统计](#osp_serial----串口传输统计)
  - [osp_qos -- QoS 配置查看](#osp_qos----qos-配置查看)
  - [osp_mempool -- 内存池使用情况](#osp_mempool----内存池使用情况)
  - [osp_app -- 应用实例池状态](#osp_app----应用实例池状态)
  - [osp_sysmon -- 系统健康监控](#osp_sysmon----系统健康监控)
- [参数解析工具 API](#参数解析工具-api)
- [自定义命令注册](#自定义命令注册)

---

## 内置命令

| 命令 | 说明 |
|------|------|
| `help` | 列出所有已注册命令及简要描述 |

`help` 命令在 Shell 启动时自动注册，无需用户代码调用。

```
osp> help
  help             - List all commands
  osp_log          - Log level display and control
  osp_config       - Runtime config view and modification
  osp_bus          - AsyncBus statistics and control
  osp_lifecycle    - Lifecycle node state and transitions
  osp_watchdog     - Show thread watchdog status
  osp_faults       - Show fault collector statistics
  ...
```

---

## 运行时控制命令

以下命令支持子命令分发，可通过参数执行运行时控制操作。
所有修改仅影响内存，重启后恢复原值。

### osp_log -- 日志级别控制

**注册方式:**
```cpp
osp::shell_cmd::RegisterLog();
```

**命令格式:**
```
osp_log                          # 显示当前日志级别
osp_log status                   # 同上
osp_log level <level>            # 设置日志级别
osp_log help                     # 显示子命令帮助
```

**参数:**

| 子命令 | 参数 | 说明 |
|--------|------|------|
| (无) / `status` | -- | 显示当前日志级别名称和数值 |
| `level` | `<0-5\|debug\|info\|warn\|error\|fatal\|off>` | 设置日志级别，支持数字或名称（大小写无关） |
| `help` | -- | 列出所有子命令 |

**级别对照表:**

| 数值 | 名称 | 说明 |
|------|------|------|
| 0 | debug | 调试信息 |
| 1 | info | 一般信息 |
| 2 | warn | 警告 |
| 3 | error | 错误 |
| 4 | fatal | 致命错误 |
| 5 | off | 关闭日志 |

**使用示例:**
```
osp> osp_log
[osp_log] level: INFO (1)

osp> osp_log level debug
[osp_log] level set to DEBUG

osp> osp_log level 3
[osp_log] level set to ERROR

osp> osp_log level 99
Invalid level: 99 (0-5)
```

---

### osp_config -- 配置查看与修改

**注册方式:**
```cpp
osp::ConfigStore config;
config.ParseFile("config.ini");
osp::shell_cmd::RegisterConfig(config);
```

**命令格式:**
```
osp_config                       # 列出所有配置条目
osp_config list                  # 同上
osp_config list <section>        # 列出指定 section 的条目
osp_config get <section> <key>   # 获取配置值
osp_config set <section> <key> <value>  # 设置配置值（仅内存）
osp_config help                  # 显示子命令帮助
```

**参数:**

| 子命令 | 参数 | 说明 |
|--------|------|------|
| (无) / `list` | `[section]` | 列出所有配置条目，可选按 section 过滤（大小写无关） |
| `get` | `<section> <key>` | 获取指定键的值，不存在时返回 "not found" |
| `set` | `<section> <key> <value>` | 设置配置值，仅修改内存，重启后丢失 |
| `help` | -- | 列出所有子命令 |

**使用示例:**
```
osp> osp_config
[osp_config] all entries (3):
  [net] port = 8080
  [net] host = 192.168.1.100
  [log] level = 3

osp> osp_config list net
[osp_config] entries in [net]:
  [net] port = 8080
  [net] host = 192.168.1.100

osp> osp_config get net port
[net] port = 8080

osp> osp_config set net port 9090
[net] port = 9090 (set)

osp> osp_config get net nonexistent
[net] nonexistent: not found
```

---

### osp_bus -- 消息总线统计与控制

**注册方式:**
```cpp
auto& bus = osp::AsyncBus<MyPayload>::Instance();
osp::shell_cmd::RegisterBusStats(bus);
```

**命令格式:**
```
osp_bus                          # 显示总线统计信息
osp_bus status                   # 同上
osp_bus reset                    # 重置所有计数器
osp_bus help                     # 显示子命令帮助
```

**参数:**

| 子命令 | 参数 | 说明 |
|--------|------|------|
| (无) / `status` | -- | 显示发布/处理/丢弃消息数、背压级别 |
| `reset` | -- | 重置所有统计计数器为零 |
| `help` | -- | 列出所有子命令 |

**使用示例:**
```
osp> osp_bus
[osp_bus] AsyncBus Statistics
  published:     12450
  processed:     12448
  dropped:       2
  rechecks:      0
  backpressure:  Normal

osp> osp_bus reset
[osp_bus] Statistics reset.

osp> osp_bus status
[osp_bus] AsyncBus Statistics
  published:     0
  processed:     0
  dropped:       0
  rechecks:      0
  backpressure:  Normal
```

---

### osp_lifecycle -- 生命周期节点状态与转换

**注册方式:**
```cpp
osp::LifecycleNode<MyPayload> node("my_node", 1);
osp::shell_cmd::RegisterLifecycle(node);
```

**命令格式:**
```
osp_lifecycle                    # 显示当前状态
osp_lifecycle status             # 同上
osp_lifecycle configure          # 触发 Configure 转换
osp_lifecycle activate           # 触发 Activate 转换
osp_lifecycle deactivate         # 触发 Deactivate 转换
osp_lifecycle cleanup            # 触发 Cleanup 转换
osp_lifecycle shutdown           # 触发 Shutdown 转换
osp_lifecycle help               # 显示子命令帮助
```

**参数:**

| 子命令 | 参数 | 说明 |
|--------|------|------|
| (无) / `status` | -- | 显示当前生命周期状态 |
| `configure` | -- | Unconfigured -> Inactive |
| `activate` | -- | Inactive -> Active |
| `deactivate` | -- | Active -> Inactive |
| `cleanup` | -- | Inactive -> Unconfigured |
| `shutdown` | -- | 任意状态 -> Finalized |
| `help` | -- | 列出所有子命令 |

**状态机转换图:**
```
Unconfigured --configure--> Inactive --activate--> Active
     ^                         |                     |
     |                         |                     |
     +------cleanup-----------+    <--deactivate---+
     |                                              |
     +--shutdown--> Finalized <------shutdown-------+
```

**错误类型:**

| 错误 | 含义 |
|------|------|
| InvalidTransition | 当前状态不允许该转换 |
| CallbackFailed | 转换回调执行失败 |
| AlreadyFinalized | 节点已终态，不可再转换 |

**使用示例:**
```
osp> osp_lifecycle
[osp_lifecycle] LifecycleNode
  state: Unconfigured (unconfigured)

osp> osp_lifecycle configure
[osp_lifecycle] Configure OK.

osp> osp_lifecycle activate
[osp_lifecycle] Activate OK.

osp> osp_lifecycle status
[osp_lifecycle] LifecycleNode
  state: Active (active)

osp> osp_lifecycle configure
[osp_lifecycle] Configure failed: InvalidTransition
```

---

## 只读诊断命令

以下命令为只读状态查询，不接受额外参数。

### osp_watchdog -- 线程看门狗状态

**注册方式:**
```cpp
osp::ThreadWatchdog<8> wd;
osp::shell_cmd::RegisterWatchdog(wd);
```

**输出示例:**
```
osp> osp_watchdog
[osp_watchdog] ThreadWatchdog (3/8 active, 0 timed out)
  [0] main_loop            timeout=1000ms  last_beat=12ms_ago  OK
  [1] sensor_thread        timeout=500ms   last_beat=45ms_ago  OK
  [2] comm_thread           timeout=2000ms  last_beat=1501ms_ago  TIMEOUT
```

**输出字段:**

| 字段 | 说明 |
|------|------|
| active/capacity | 活跃线程数 / 总容量 |
| timed out | 超时线程数 |
| slot_id | 槽位编号 |
| name | 线程名称 |
| timeout | 超时阈值（ms） |
| last_beat | 距上次心跳时间（ms） |
| OK / TIMEOUT | 当前状态 |

---

### osp_faults -- 故障收集器统计

**注册方式:**
```cpp
osp::FaultCollector<16, 32> fc;
osp::shell_cmd::RegisterFaults(fc);
```

**输出示例:**
```
osp> osp_faults
[osp_faults] FaultCollector Statistics
  total_reported:  156
  total_processed: 156
  total_dropped:   0
  Critical   reported=2   dropped=0
  High       reported=12  dropped=0
  Medium     reported=45  dropped=0
  Low        reported=97  dropped=0
  queue_usage: Critical=0/16 High=0/16 Medium=0/16 Low=0/16
  recent faults:
    [0] fault=3 detail=42 pri=High ts=1234567890us
    [1] fault=7 detail=0 pri=Low ts=1234567800us
```

---

### osp_pool -- 工作线程池统计

**注册方式:**
```cpp
osp::WorkerPool<MyPayload> pool(cfg);
osp::shell_cmd::RegisterWorkerPool(pool);
```

**输出示例:**
```
osp> osp_pool
[osp_pool] WorkerPool Statistics
  dispatched:      1024
  processed:       1024
  queue_full:      0
  bus_published:   1024
  bus_dropped:     0
```

---

### osp_nodes -- HSM 节点管理器状态

**注册方式:**
```cpp
osp::HsmNodeManager<8> mgr;
osp::shell_cmd::RegisterHsmNodes(mgr);
```

**输出示例:**
```
osp> osp_nodes
[osp_nodes] HsmNodeManager (3 active)
  node_id=1   state=Connected      last_hb=120ms_ago  missed=0
  node_id=2   state=Connected      last_hb=250ms_ago  missed=0
  node_id=5   state=Suspect        last_hb=3200ms_ago  missed=3
```

---

### osp_nodes_basic -- 基础节点管理器状态

**注册方式:**
```cpp
osp::NodeManager mgr;
osp::shell_cmd::RegisterNodeManager(mgr);
```

**输出示例:**
```
osp> osp_nodes_basic
[osp_nodes_basic] NodeManager (2 active)
  node_id=1  [listener]
  node_id=2  remote=192.168.1.100:8080
```

---

### osp_service -- 服务 HSM 状态

**注册方式:**
```cpp
osp::HsmService svc;
osp::shell_cmd::RegisterServiceHsm(svc);
```

**输出示例:**
```
osp> osp_service
[osp_service] HsmService
  state: Active
```

---

### osp_discovery -- 发现 HSM 状态

**注册方式:**
```cpp
osp::HsmDiscovery disc;
osp::shell_cmd::RegisterDiscoveryHsm(disc);
```

**输出示例:**
```
osp> osp_discovery
[osp_discovery] HsmDiscovery
  state: Stable
  lost_count: 0
```

---

### osp_transport -- 传输序列追踪

**注册方式:**
```cpp
osp::SequenceTracker tracker;
osp::shell_cmd::RegisterTransport(tracker);
```

需要编译选项 `OSP_WITH_NETWORK=ON`。

**输出示例:**
```
osp> osp_transport
[osp_transport] SequenceTracker
  total_received:  50000
  lost:            3
  reordered:       1
  duplicates:      0
  loss_rate:       0.00%
```

---

### osp_serial -- 串口传输统计

**注册方式:**
```cpp
osp::SerialTransport serial(scfg);
osp::shell_cmd::RegisterSerial(serial);
```

**输出示例:**
```
osp> osp_serial
[osp_serial] SerialTransport Statistics
  frames_sent:      1200
  frames_received:  1198
  bytes_sent:       48000
  bytes_received:   47920
  crc_errors:       2
  sync_errors:      0
  timeout_errors:   0
  seq_gaps:         0
  retransmits:      2
  ack_timeouts:     0
  rate_limit_drops: 0
```

---

### osp_qos -- QoS 配置查看

**注册方式:**
```cpp
osp::shell_cmd::RegisterQos(osp::QosSensorData, "sensor");
```

**输出示例:**
```
osp> osp_qos
[osp_qos] QosProfile 'sensor'
  reliability:   BestEffort
  history:       KeepLast
  durability:    Volatile
  history_depth: 1
  deadline_ms:   100
  lifespan_ms:   500
```

---

### osp_mempool -- 内存池使用情况

**注册方式:**
```cpp
osp::FixedPool<64, 16> pool;
osp::shell_cmd::RegisterMemPool(pool, "sensor_pool");
```

**输出示例:**
```
osp> osp_mempool
[osp_mempool] sensor_pool
  capacity: 16
  used:     3
  free:     13
```

---

### osp_app -- 应用实例池状态

**注册方式:**
```cpp
osp::Application app;
osp::shell_cmd::RegisterApp(app);
```

**输出示例:**
```
osp> osp_app
[osp_app] Application 'my_app' (id=1)
  instances:    4
  pending_msgs: 12
```

---

### osp_sysmon -- 系统健康监控

**注册方式:**
```cpp
osp::SystemMonitor<2> mon;
osp::shell_cmd::RegisterSystemMonitor(mon);
```

**输出示例:**
```
osp> osp_sysmon
[osp_sysmon] SystemMonitor
  CPU:  total=15%  user=10%  sys=5%  iowait=0%
  Temp: 42.3 C
  Mem:  total=1048576kB  avail=524288kB  used=50%
  Disk[0]: total=16106127360B  avail=8053063680B  used=50%
  Disk[1]: total=1073741824B  avail=536870912B  used=50%
```

---

## 参数解析工具 API

Shell 提供以下工具函数，用于自定义命令的参数解析。定义在 `osp/shell.hpp` 中。

### ShellParseInt

```cpp
[[nodiscard]] optional<int32_t> ShellParseInt(const char* str) noexcept;
```

将字符串解析为有符号 32 位整数（十进制）。拒绝 null、空串、尾部非数字字符和溢出。

### ShellParseUint

```cpp
[[nodiscard]] optional<uint32_t> ShellParseUint(const char* str) noexcept;
```

将字符串解析为无符号 32 位整数（十进制）。额外拒绝前导 `-`。

### ShellParseBool

```cpp
[[nodiscard]] optional<bool> ShellParseBool(const char* str) noexcept;
```

解析布尔值。大小写无关。

| 返回 true | `true`, `1`, `yes`, `on` |
|-----------|--------------------------|
| 返回 false | `false`, `0`, `no`, `off` |

### ShellArgCheck

```cpp
[[nodiscard]] bool ShellArgCheck(int argc, int min_argc, const char* usage) noexcept;
```

检查 argc 是否满足最小参数数量。不足时自动打印 `Usage: <usage>` 并返回 false。

### ShellDispatch

```cpp
int ShellDispatch(int argc, char* argv[],
                  const ShellSubCmd* table, uint32_t count,
                  ShellCmdFn default_fn = nullptr) noexcept;
```

子命令分发器。行为：

- `argc <= 1`（无子命令）：调用 `default_fn`（若非 null），否则打印帮助
- `argv[1] == "help"`：打印格式化子命令帮助表
- `argv[1]` 匹配子命令：调用 `handler(argc-1, argv+1)`（argv 左移）
- 未匹配：打印错误 + 提示 help

### ShellSubCmd 结构体

```cpp
struct ShellSubCmd {
  const char* name;       // 子命令名
  const char* args_desc;  // 参数描述 (nullptr=无参数)
  const char* help;       // 帮助文本
  ShellCmdFn handler;     // 处理函数
};
```

---

## 自定义命令注册

### 方式一：OSP_SHELL_CMD 宏

适用于全局函数：

```cpp
int my_custom_cmd(int argc, char* argv[]) {
    osp::ShellPrintf("Hello from custom command!\r\n");
    return 0;
}
OSP_SHELL_CMD(my_custom_cmd, "My custom diagnostic command");
```

### 方式二：GlobalCmdRegistry 手动注册

适用于需要捕获上下文的场景：

```cpp
osp::detail::GlobalCmdRegistry::Instance().Register(
    "my_cmd", my_handler, "Description");
```

### 方式三：带子命令分发的命令

使用 `ShellDispatch` 实现子命令路由：

```cpp
static int sub_status(int argc, char* argv[]) {
    osp::ShellPrintf("Status: OK\r\n");
    return 0;
}

static int sub_reset(int argc, char* argv[]) {
    // reset logic...
    osp::ShellPrintf("Reset done.\r\n");
    return 0;
}

static const osp::ShellSubCmd kSubs[] = {
    {"status", nullptr,       "Show status",    sub_status},
    {"reset",  nullptr,       "Reset counters", sub_reset},
};

static int my_cmd(int argc, char* argv[]) {
    return osp::ShellDispatch(argc, argv, kSubs, 2U, sub_status);
}
OSP_SHELL_CMD(my_cmd, "My module control");
```

使用效果：
```
osp> my_cmd              # 调用默认 handler (sub_status)
osp> my_cmd status       # 显式调用 status
osp> my_cmd reset        # 调用 reset
osp> my_cmd help         # 自动生成帮助
osp> my_cmd bogus        # "Unknown subcommand: bogus (try 'my_cmd help')"
```

---

## 命令总览

| 命令 | 类型 | 子命令 | 说明 |
|------|------|--------|------|
| `help` | 内置 | -- | 列出所有命令 |
| `osp_log` | 控制 | `status`, `level` | 日志级别查看与设置 |
| `osp_config` | 控制 | `list`, `get`, `set` | 配置查看与运行时修改 |
| `osp_bus` | 控制 | `status`, `reset` | 消息总线统计与重置 |
| `osp_lifecycle` | 控制 | `status`, `configure`, `activate`, `deactivate`, `cleanup`, `shutdown` | 生命周期状态机控制 |
| `osp_watchdog` | 诊断 | -- | 线程看门狗 |
| `osp_faults` | 诊断 | -- | 故障收集器 |
| `osp_pool` | 诊断 | -- | 工作线程池 |
| `osp_nodes` | 诊断 | -- | HSM 节点管理器 |
| `osp_nodes_basic` | 诊断 | -- | 基础节点管理器 |
| `osp_service` | 诊断 | -- | 服务 HSM |
| `osp_discovery` | 诊断 | -- | 发现 HSM |
| `osp_transport` | 诊断 | -- | 传输序列追踪 |
| `osp_serial` | 诊断 | -- | 串口传输 |
| `osp_qos` | 诊断 | -- | QoS 配置 |
| `osp_mempool` | 诊断 | -- | 内存池 |
| `osp_app` | 诊断 | -- | 应用实例池 |
| `osp_sysmon` | 诊断 | -- | 系统健康监控 |
