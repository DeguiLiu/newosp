# Shell 内置诊断命令设计

> 从属于 [design_zh.md](design_zh.md) §4.6 shell.hpp 扩展
> 版本: 1.0
> 日期: 2026-02-14
> 状态: 设计中

---

## 1. 概述

### 1.1 问题

当前 shell 诊断命令仅在 examples 中通过 `OSP_SHELL_CMD` 手动注册，核心库头文件本身不提供内置诊断能力。每个新项目都需要重复编写相同的统计打印代码。

### 1.2 方案

新建独立桥接文件 `shell_commands.hpp`，零侵入地将各模块的运行时状态暴露为 shell 命令。

设计原则:
- 不修改现有模块头文件的公共接口
- 用户按需 include，不需要 shell 的场景零开销
- 函数模板接收对象引用，编译期绑定，无虚函数

### 1.3 依赖方向

```
shell.hpp (OSP_SHELL_CMD, DebugShell::Printf)
    ^
    |
shell_commands.hpp (桥接层，按需 include)
    ^
    |
各模块头文件 (bus.hpp, watchdog.hpp, ...)
```

shell_commands.hpp 依赖 shell.hpp + 各模块头文件。各模块本身不依赖 shell.hpp。

---

## 2. 命令规划

### 2.1 按架构层分组

命令名采用 `osp_<module>` 前缀，避免与用户命令冲突。

#### 应用层 (App Layer)

| 命令 | 模块 | 现有查询接口 | 输出内容 |
|------|------|-------------|----------|
| `osp_lifecycle` | lifecycle_node.hpp | `GetState()`, `DetailedStateName()` | 节点名、粗粒度状态、详细状态名 |
| `osp_qos` | qos.hpp | 纯数据结构 | 打印 QosProfile 各字段 (需用户传入 profile) |

#### 服务层 (Service Layer)

| 命令 | 模块 | 现有查询接口 | 输出内容 |
|------|------|-------------|----------|
| `osp_nodes` | node_manager_hsm.hpp | `GetNodeState()`, `NodeCount()` | 各节点 HSM 状态 (Connected/Suspect/Disconnected) |
| `osp_nodes_basic` | node_manager.hpp | `ForEach()`, `NodeCount()` | 各节点连接状态、心跳时间 |
| `osp_service` | service_hsm.hpp | `GetState()` | 服务 HSM 状态 |
| `osp_discovery` | discovery_hsm.hpp | `GetState()`, `GetLostCount()` | 发现 HSM 状态、丢失节点数 |

#### 网络传输层 (Network Transport Layer)

| 命令 | 模块 | 现有查询接口 | 输出内容 |
|------|------|-------------|----------|
| `osp_transport` | transport.hpp | `SequenceTracker::LostCount()`, `FrameCount()` | 帧计数、丢包数、丢包率 |
| `osp_serial` | serial_transport.hpp | `GetStatistics()` | frames/bytes/errors 全量统计 |

#### 通信层 (酌情添加)

bus 和 worker_pool 是模板类 (`AsyncBus<PayloadVariant>`, `WorkerPool<PayloadVariant>`)，实例化类型因项目而异，且通常作为内部组件被上层模块封装。是否注册取决于用户是否直接持有这些对象的引用。对于简单项目可以注册；对于多 Bus 实例的复杂项目，建议在应用层自行编写更有针对性的命令。

| 命令 | 模块 | 现有查询接口 | 输出内容 |
|------|------|-------------|----------|
| `osp_bus` | bus.hpp | `GetStatistics()` | publish/drop/overflow/backpressure |
| `osp_pool` | worker_pool.hpp | `GetStats()` | dispatched/processed/queue_full + bus stats |

#### 可靠性层

| 命令 | 模块 | 现有查询接口 | 输出内容 |
|------|------|-------------|----------|
| `osp_watchdog` | watchdog.hpp | `ActiveCount()`, `TimedOutCount()` | 各 slot 名称/超时配置/最后心跳/是否超时 |
| `osp_faults` | fault_collector.hpp | `GetStatistics()` | total/dropped/per-severity + 最近 N 条故障 |

#### 基础层

| 命令 | 模块 | 现有查询接口 | 输出内容 |
|------|------|-------------|----------|
| `osp_mempool` | mem_pool.hpp | `DumpState()` | 容量/已用/空闲 |

### 2.2 需要补充的查询接口

部分模块缺少遍历/快照接口，需要先补充:

| 模块 | 需要新增 | 说明 |
|------|---------|------|
| watchdog.hpp | `ForEachSlot(Fn)` | 遍历活跃 slot，回调传入 name/timeout_us/last_beat_us/timed_out |
| fault_collector.hpp | `ForEachRecent(Fn, max_count)` | 遍历最近 N 条故障记录 |
| fault_collector.hpp | `QueueUsage(priority) -> {size, capacity}` | 单队列使用率查询 |
| node_manager_hsm.hpp | `ForEachNode(Fn)` | 遍历活跃节点，回调传入 node_id/state_name/last_heartbeat |

---

## 3. 接口设计

### 3.1 注册模式

采用模板函数 + 静态局部变量捕获对象指针的方式:

```cpp
// shell_commands.hpp

namespace osp {
namespace shell_cmd {

/// Register bus statistics command.
/// @param bus  Reference to AsyncBus instance (must outlive shell).
template <typename BusType>
inline void RegisterBusStats(BusType& bus) {
  static BusType* s_bus = &bus;
  static auto cmd = [](int /*argc*/, char* /*argv*/[]) -> int {
    auto stats = s_bus->GetStatistics();
    DebugShell::Printf("  published:  %llu\r\n", stats.published);
    DebugShell::Printf("  consumed:   %llu\r\n", stats.consumed);
    DebugShell::Printf("  dropped:    %llu\r\n", stats.dropped);
    DebugShell::Printf("  overflow:   %llu\r\n", stats.overflow);
    return 0;
  };
  detail::GlobalCmdRegistry::Instance().Register(
      "osp_bus", +cmd, "Show AsyncBus statistics");
}

}  // namespace shell_cmd
}  // namespace osp
```

### 3.2 批量注册便捷函数

```cpp
/// Register all available diagnostic commands at once.
/// Pass nullptr for modules not in use.
template <typename BusType>
inline void RegisterAll(
    BusType* bus,
    ThreadWatchdog<>* watchdog,
    FaultCollector<>* faults,
    // ... other modules
) {
  if (bus) RegisterBusStats(*bus);
  if (watchdog) RegisterWatchdog(*watchdog);
  if (faults) RegisterFaults(*faults);
  // ...
}
```

### 3.3 使用示例

```cpp
#include "osp/shell.hpp"
#include "osp/shell_commands.hpp"
#include "osp/bus.hpp"
#include "osp/watchdog.hpp"

int main() {
  osp::AsyncBus<Payload> bus;
  osp::ThreadWatchdog<32> watchdog;

  // 按需注册
  osp::shell_cmd::RegisterBusStats(bus);
  osp::shell_cmd::RegisterWatchdog(watchdog);

  // 或批量注册
  // osp::shell_cmd::RegisterAll(&bus, &watchdog, nullptr, ...);

  osp::DebugShell shell({.port = 5090});
  shell.Start();
  // telnet localhost 5090 -> help -> osp_bus / osp_watchdog
}
```

---

## 4. 实现计划

### Phase 1: 补充查询接口 (P0)

| 任务 | 文件 | 说明 |
|------|------|------|
| 4.1 | watchdog.hpp | 新增 `ForEachSlot(Fn)` 遍历接口 |
| 4.2 | fault_collector.hpp | 新增 `ForEachRecent(Fn, max)` + `QueueUsage(pri)` |
| 4.3 | node_manager_hsm.hpp | 新增 `ForEachNode(Fn)` 遍历接口 |
| 4.4 | 对应 test 文件 | 新增遍历接口的单元测试 |

### Phase 2: shell_commands.hpp 实现 (P0)

| 任务 | 说明 |
|------|------|
| 4.5 | 创建 `include/osp/shell_commands.hpp` |
| 4.6 | 实现 15 个 Register 函数 (按 §2.1 表格) |
| 4.7 | 实现 `RegisterAll()` 批量注册 |
| 4.8 | `test_shell_commands.cpp` 单元测试 |

### Phase 3: 文档更新 (P0)

| 任务 | 说明 |
|------|------|
| 4.9 | 更新 design_zh.md 新增 shell_commands 章节 |
| 4.10 | 更新 README/README_zh 模块表 |

---

## 5. 输出格式规范

所有命令输出遵循统一格式，便于脚本解析。以下为各命令的实际输出示例 (基于 shell_commands.hpp 实现):

### 5.1 可靠性层

```
osp> osp_watchdog
[osp_watchdog] ThreadWatchdog (3/32 active, 0 timed out)
  [0] timer_scheduler   timeout=2000ms  last_beat=12ms_ago  OK
  [1] worker_dispatch   timeout=5000ms  last_beat=3ms_ago   OK
  [2] fault_consumer    timeout=5000ms  last_beat=8ms_ago   OK

osp> osp_faults
[osp_faults] FaultCollector Statistics
  total_reported:  42
  total_processed: 42
  total_dropped:   0
  Critical   reported=2  dropped=0
  High       reported=5  dropped=0
  Medium     reported=15  dropped=0
  Low        reported=20  dropped=0
  queue_usage: Critical=0/256 High=0/256 Medium=0/256 Low=0/256
  recent faults:
    [0] fault=0 detail=42 pri=High ts=1707900000000us
    [1] fault=1 detail=100 pri=Medium ts=1707899999000us
```

### 5.2 通信层

```
osp> osp_bus
[osp_bus] AsyncBus Statistics
  published:     12345
  processed:     12340
  dropped:           5
  rechecks:          0
  backpressure:  Normal

osp> osp_pool
[osp_pool] WorkerPool Statistics
  dispatched:      5000
  processed:       5000
  queue_full:         0
  bus_published:   5000
  bus_dropped:        0
```

### 5.3 网络传输层

```
osp> osp_transport
[osp_transport] SequenceTracker
  total_received:  10000
  lost:                3
  reordered:           1
  duplicates:          0
  loss_rate:       0.03%

osp> osp_serial
[osp_serial] SerialTransport Statistics
  frames_sent:      1000
  frames_received:   998
  bytes_sent:      64000
  bytes_received:  63872
  crc_errors:          2
  sync_errors:         0
  timeout_errors:      0
  seq_gaps:            0
  retransmits:         2
  ack_timeouts:        0
  rate_limit_drops:    0
```

### 5.4 服务层

```
osp> osp_nodes
[osp_nodes] HsmNodeManager (2 active)
  node_id=1  state=Connected      last_hb=50ms_ago  missed=0
  node_id=2  state=Suspect        last_hb=3200ms_ago  missed=2

osp> osp_nodes_basic
[osp_nodes_basic] NodeManager (3 active)
  node_id=1  [listener]
  node_id=2  remote=192.168.1.10:8080
  node_id=3  remote=192.168.1.11:8080

osp> osp_service
[osp_service] HsmService
  state: Active

osp> osp_discovery
[osp_discovery] HsmDiscovery
  state: Stable
  lost_count: 0
```

### 5.5 应用层

```
osp> osp_lifecycle
[osp_lifecycle] LifecycleNode
  state: Active (Running)

osp> osp_qos
[osp_qos] QosProfile 'sensor'
  reliability:   Reliable
  history:       KeepLast
  durability:    Volatile
  history_depth: 10
  deadline_ms:   100
  lifespan_ms:   5000
```

### 5.6 基础层

```
osp> osp_mempool
[osp_mempool] sensor_pool
  capacity: 64
  used:     12
  free:     52
```

---

## 6. 资源预算

| 项目 | 开销 |
|------|------|
| shell_commands.hpp 编译开销 | 仅在 include 时生效，不影响不使用的模块 |
| 静态局部指针 | 每个 Register 函数 1 个 static 指针 (8B) |
| 命令注册表 slot | 每个命令占用 GlobalCmdRegistry 1 个 slot (最多 64) |
| 运行时开销 | 仅在 telnet 执行命令时触发，热路径零开销 |
