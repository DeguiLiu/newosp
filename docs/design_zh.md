# newosp 架构设计文档

> 版本: 2.1
> 日期: 2026-02-17
> 状态: 活跃演进中

---

## 目录

1. [项目概述](#1-项目概述)
2. [设计目标与约束](#2-设计目标与约束)
3. [系统架构](#3-系统架构)
4. [基础层](#4-基础层)
5. [核心通信层](#5-核心通信层)
6. [状态机与行为树](#6-状态机与行为树)
7. [网络与传输层](#7-网络与传输层)
8. [服务与发现层](#8-服务与发现层)
9. [应用层](#9-应用层)
10. [可靠性组件](#10-可靠性组件)
11. [跨模块交互](#11-跨模块交互)
12. [资源预算](#12-资源预算)
13. [编码规范与质量保障](#13-编码规范与质量保障)
14. [附录](#14-附录)

> 变更日志 (Phase A-T 实施记录、P0 架构调整历史) 见 [changelog_zh.md](changelog_zh.md)
> 示例设计文档见 [examples_zh.md](examples_zh.md)

---

## 1. 项目概述

### 1.1 定位

newosp 是一个面向 ARM-Linux 工业级嵌入式平台的现代 C++17 纯头文件基础设施库。提供嵌入式系统开发所需的核心基础能力: 配置管理、日志 (含异步日志)、定时调度、远程调试、内存池、消息总线、节点通信、工作线程池、共享内存 IPC、网络传输、串口通信、层次状态机、行为树、QoS 服务质量、生命周期节点、代码生成。

### 1.2 适用场景

| 场景 | 关键需求 |
|------|----------|
| 视频监控服务器 | Socket 抽象、定时调度、日志、状态机 |
| IoT 网关 | 多协议接入、消息总线、工作线程池 |
| 机器人中间件 | 高性能 SPSC 缓冲、节点通信、传感器融合 |
| 工业边缘计算 | 零拷贝 Socket I/O、流量控制、CPU 监控 |
| 嵌入式多媒体 | RTP/RTSP 封装、周期调度、配置管理 |

### 1.3 技术栈

- **语言**: C++17 (兼容 `-fno-exceptions -fno-rtti`)
- **构建**: CMake >= 3.14, FetchContent 自动管理依赖
- **测试**: Catch2 v3.5.2, 1153 test cases, ASan/TSan/UBSan clean
- **代码规范**: Google C++ Style (clang-format v21), cpplint, clang-tidy, MISRA C++
- **CI**: GitHub Actions (Ubuntu, GCC x Debug/Release, Sanitizers)

---

## 2. 设计目标与约束

### 2.1 核心设计原则

| 原则 | 说明 |
|------|------|
| 零全局状态 | 所有状态封装在对象中，RAII 管理生命周期 |
| 栈优先分配 | 固定容量容器，热路径禁止堆分配 |
| 无锁或最小锁 | MPSC 无锁总线，SPSC 无锁队列，SharedMutex 读写分离 |
| 编译期分发 | 模板特化、标签分发、`if constexpr` 替代虚函数 |
| 类型安全 | `expected<V,E>` 错误处理，`NewType<T,Tag>` 强类型，`std::variant` 消息路由 |
| 嵌入式友好 | 兼容 `-fno-exceptions -fno-rtti`，固定宽度整数，缓存行对齐 |

### 2.2 硬件约束

```
目标平台:    ARM Cortex-A 系列 (Linux 4.x+)
编译器:      GCC >= 7, Clang >= 5
内存预算:    典型 ~100KB 静态 + <10KB 堆
线程预算:    典型 4-8 线程 (定时器1 + Shell 3 + 异步日志 1 + 工作线程 N)
缓存行:     64 字节 (ARM 标准)
```

### 2.3 依赖管理

| 依赖 | 版本 | 用途 | 条件 |
|------|------|------|------|
| inih | r58 | INI 配置解析 | `OSP_CONFIG_INI=ON` |
| nlohmann/json | v3.11.3 | JSON 配置解析 | `OSP_CONFIG_JSON=ON` |
| fkYAML | v0.4.0 | YAML 配置解析 | `OSP_CONFIG_YAML=ON` |
| sockpp | master (DeguiLiu fork) | RAII Socket 封装 | `OSP_WITH_SOCKPP=ON` |
| CSerialPort | v4.3.1 | 串口通信 | `OSP_WITH_SERIAL=ON` |
| Catch2 | v3.5.2 | 单元测试 | `OSP_BUILD_TESTS=ON` |

**CMake 构建模式**:

| 模式 | 命令 | 说明 |
|------|------|------|
| 全模块 | `cmake -B build -DOSP_BUILD_TESTS=ON` | 默认，包含所有模块 |
| 无网络 | `cmake -B build -DOSP_BUILD_TESTS=ON -DOSP_WITH_NETWORK=OFF` | 排除网络依赖模块，适合无网络栈平台 |
| 无异常 | `cmake -B build -DOSP_BUILD_TESTS=ON -DOSP_NO_EXCEPTIONS=ON` | `-fno-exceptions` 编译 |

### 2.4 开发分级

| 优先级 | 含义 | 开发原则 |
|--------|------|---------|
| P0 | 架构基础 | 不完成 P0 不启动 P1；设计文档同步更新 |
| P1 | 功能扩展 | P0 全部完成后启动；兼容性和功能覆盖优先 |
| P2 | 高级特性 | P1 基本完成后启动；可按需裁剪 |

---

## 3. 系统架构

### 3.1 层次结构

```
                      Application Layer
                 (Protocol / Client / Business)
    ============================================
    |                                          |
    |          newosp Infrastructure           |
    |                                          |
    |  ┌─────────────────────────────────────┐ |
    |  |  §10 可靠性组件                      | |
    |  |  Watchdog + FaultCollector           | |
    |  |  + FaultReporter 注入               | |
    |  └──────────────┬──────────────────────┘ |
    |                 |                        |
    |  ┌──────────────┴──────────────────────┐ |
    |  |  §9 应用层                           | |
    |  |  App/Instance(HSM) + Post           | |
    |  |  LifecycleNode(HSM) + QoS           | |
    |  └──────────────┬──────────────────────┘ |
    |                 |                        |
    |  ┌──────────────┴──────────────────────┐ |
    |  |  §8 服务与发现层                     | |
    |  |  Service(RPC) + Discovery           | |
    |  |  NodeManager + *_hsm.hpp            | |
    |  └──────────────┬──────────────────────┘ |
    |                 |                        |
    |  ┌──────────────┴──────────────────────┐ |
    |  |  §7 网络与传输层                     | |
    |  |  Transport (TCP/UDP/SHM/Serial)     | |
    |  |  TransportFactory + IoPoller        | |
    |  └──────────────┬──────────────────────┘ |
    |                 |                        |
    |  ┌──────────────┴──────────────────────┐ |
    |  |  §6 状态机与行为树                   | |
    |  |  HSM + BT                           | |
    |  └──────────────┬──────────────────────┘ |
    |                 |                        |
    |  ┌──────────────┴──────────────────────┐ |
    |  |  §5 核心通信层                       | |
    |  |  AsyncBus (MPSC) + Node (Pub/Sub)   | |
    |  |  WorkerPool + SpscRingbuffer        | |
    |  |  Executor + Semaphore               | |
    |  └──────────────┬──────────────────────┘ |
    |                 |                        |
    |  ┌──────────────┴──────────────────────┐ |
    |  |  §4 基础层                           | |
    |  |  Platform + Vocabulary              | |
    |  |  Config + Log + Timer + Shell       | |
    |  |  MemPool + Shutdown                 | |
    |  └─────────────────────────────────────┘ |
    ============================================
                   POSIX / Linux Kernel
```

### 3.2 模块依赖关系

```
platform.hpp  ──────────────────  (无依赖，纯宏)
    |
vocabulary.hpp  ────────────────  (依赖 platform.hpp)
    |
    ├── config.hpp     (依赖 vocabulary: expected, optional, FixedString)
    ├── log.hpp        (独立，纯 C stdlib)
    ├── async_log.hpp   (依赖 log.hpp + platform.hpp + spsc_ringbuffer.hpp)
    ├── timer.hpp      (依赖 vocabulary: expected, NewType)
    ├── shell.hpp      (依赖 vocabulary: function_ref, FixedFunction, expected)
    ├── mem_pool.hpp   (依赖 vocabulary: expected)
    ├── shutdown.hpp   (依赖 vocabulary: expected)
    |
    ├── bus.hpp        (依赖 platform: kCacheLineSize)
    ├── node.hpp       (依赖 vocabulary + bus.hpp)
    ├── static_node.hpp (依赖 node.hpp + bus.hpp)
    ├── worker_pool.hpp (依赖 vocabulary + bus.hpp + spsc_ringbuffer.hpp)
    ├── spsc_ringbuffer.hpp (依赖 platform: kCacheLineSize)
    ├── hsm.hpp        (依赖 platform)
    ├── bt.hpp         (依赖 platform)
    |
    ├── net.hpp        (依赖 vocabulary + sockpp)
    ├── transport.hpp  (依赖 vocabulary + spsc_ringbuffer + net)
    ├── shm_transport.hpp (依赖 vocabulary + platform)
    ├── serial_transport.hpp (依赖 vocabulary + spsc_ringbuffer + CSerialPort)
    ├── transport_factory.hpp (依赖 platform + vocabulary)
    |
    ├── node_manager_hsm.hpp  (依赖 hsm + fault_collector)
    ├── service_hsm.hpp       (依赖 hsm + fault_collector)
    ├── discovery_hsm.hpp     (依赖 hsm + fault_collector)
    |
    ├── lifecycle_node.hpp (依赖 hsm + fault_collector + node)
    ├── qos.hpp            (依赖 platform)
    |
    ├── watchdog.hpp       (依赖 platform: ThreadHeartbeat)
    ├── fault_collector.hpp (依赖 platform + vocabulary)
    └── system_monitor.hpp (依赖 platform)
```

### 3.3 数据流

```
                        ┌──────────┐
                        │ Producer │ (任意线程)
                        └────┬─────┘
                             │ Publish() [lock-free CAS]
                             v
                    ┌────────────────┐     ┌─────────────────┐
                    │   AsyncBus     │     │  NetworkTransport│
                    │ (MPSC Ring)    │◄────│  (TCP/UDP)       │
                    │ 4096 slots     │     │  远程节点消息     │
                    └───────┬────────┘     └─────────────────┘
                            │ ProcessBatch() [单消费者]
                   ┌────────┼────────┐
                   v        v        v
              ┌────────┐ ┌────────┐ ┌────────┐
              │ Node 0 │ │ Node 1 │ │ Node 2 │  (类型路由)
              └────────┘ └────────┘ └────────┘
                   |
           ┌───────┴───────┐
           v               v
    ┌────────────┐  ┌────────────┐
    │ Worker[0]  │  │ Worker[1]  │  (SPSC 队列)
    │ SpscRing   │  │ SpscRing   │
    └────────────┘  └────────────┘
```

### 3.4 模块总览 (41 个头文件)

> 网络依赖标记: 无标记 = 始终可用; `[N]` = 需要 `OSP_HAS_NETWORK`; `[L]` = 仅 Linux

| 层 | 模块 | 头文件 | 说明 |
|----|------|--------|------|
| 基础层 | Platform | platform.hpp | 平台检测、OSP_ASSERT、OSP_HAS_NETWORK、ThreadHeartbeat |
| 基础层 | Vocabulary | vocabulary.hpp | expected, FixedString/Vector, FixedFunction, ScopeGuard |
| 基础层 | Config | config.hpp | INI/JSON/YAML 多格式配置 |
| 基础层 | Log | log.hpp | stderr 日志宏 |
| 基础层 | AsyncLog | async_log.hpp | 异步日志 (Per-Thread SPSC, 分级路由, 背压丢弃上报) |
| 基础层 | Timer | timer.hpp | 定时调度器 |
| 基础层 | Shell | shell.hpp | 多后端调试 Shell (DebugShell `[N]`, ConsoleShell, UartShell) |
| 基础层 | MemPool | mem_pool.hpp | 固定块内存池 |
| 基础层 | Shutdown | shutdown.hpp | 优雅关停 |
| 核心通信 | AsyncBus | bus.hpp | 无锁 MPSC 消息总线 |
| 核心通信 | Node | node.hpp | Pub/Sub 节点 |
| 核心通信 | StaticNode | static_node.hpp | 编译期 Handler 绑定节点 (零间接调用) |
| 核心通信 | WorkerPool | worker_pool.hpp | 多工作线程池 |
| 核心通信 | SpscRingbuffer | spsc_ringbuffer.hpp | Lock-free SPSC 环形缓冲区 |
| 核心通信 | Executor | executor.hpp | 调度器 (Single/Static/Pinned/Realtime) |
| 核心通信 | Semaphore | semaphore.hpp | 信号量 |
| 核心通信 | DataFusion | data_fusion.hpp | 多源数据融合 |
| 状态机 | HSM | hsm.hpp | 层次状态机 |
| 行为树 | BT | bt.hpp | 行为树 |
| 网络传输 | Socket | socket.hpp | TCP/UDP 封装 `[N]` |
| 网络传输 | IoPoller | io_poller.hpp | I/O 多路复用 (epoll/kqueue/poll) `[N]` |
| 网络传输 | Connection | connection.hpp | 连接管理 |
| 网络传输 | Transport | transport.hpp | 透明网络传输 `[N]` |
| 网络传输 | ShmTransport | shm_transport.hpp | 共享内存 IPC (MPSC slot + SPSC/SPMC byte-stream + futex) `[N]` |
| 网络传输 | SerialTransport | serial_transport.hpp | 串口传输 |
| 网络传输 | Net | net.hpp | sockpp 集成层 `[N]` |
| 网络传输 | TransportFactory | transport_factory.hpp | 自动传输选择 `[N]` |
| 服务发现 | Discovery | discovery.hpp | 节点发现 `[N]` |
| 服务发现 | Service | service.hpp | RPC 服务 `[N]` |
| 服务发现 | NodeManager | node_manager.hpp | 节点管理 `[N]` |
| 服务发现 | NodeManagerHsm | node_manager_hsm.hpp | HSM 驱动节点管理 |
| 服务发现 | ServiceHsm | service_hsm.hpp | HSM 驱动服务生命周期 |
| 服务发现 | DiscoveryHsm | discovery_hsm.hpp | HSM 驱动节点发现 |
| 应用层 | App | app.hpp | Application/Instance 模型 |
| 应用层 | Post | post.hpp | 统一消息投递 |
| 应用层 | QoS | qos.hpp | 服务质量配置 |
| 应用层 | LifecycleNode | lifecycle_node.hpp | 生命周期节点 |
| 可靠性 | Watchdog | watchdog.hpp | 线程看门狗 |
| 可靠性 | FaultCollector | fault_collector.hpp | 故障收集器 |
| 可靠性 | ShellCommands | shell_commands.hpp | 内置诊断 Shell 命令桥接 |
| 可靠性 | SystemMonitor | system_monitor.hpp | 系统健康监控 `[L]` |
| 可靠性 | Process | process.hpp | 进程管理 `[L]` |

---

## 4. 基础层

### 4.1 platform.hpp -- 平台检测与编译器提示

| 组件 | 说明 |
|------|------|
| `OSP_PLATFORM_LINUX/MACOS/WINDOWS` | 平台检测宏 |
| `OSP_ARCH_ARM/X86` | 架构检测宏 |
| `OSP_HAS_NETWORK` | 网络可用性宏 (BSD socket 检测，见下文) |
| `kCacheLineSize` | 缓存行大小常量 (64) |
| `OSP_LIKELY(x)` / `OSP_UNLIKELY(x)` | 分支预测提示 |
| `OSP_ASSERT(cond)` | 调试断言 (NDEBUG 下编译消除) |
| `SteadyNowUs()` / `SteadyNowNs()` | 单调时钟微秒/纳秒 |
| `ThreadHeartbeat` | 原子心跳结构 (供 Watchdog 使用) |

纯宏 + constexpr 实现，零运行时开销。

**OSP_HAS_NETWORK -- 网络条件编译**:

通过 `__has_include(<sys/socket.h>)` 自动检测 BSD socket 可用性。CMake 层可通过 `-DOSP_WITH_NETWORK=OFF` 强制关闭 (设置 `OSP_HAS_NETWORK=0`)。默认在 Linux/macOS 上为 ON，无网络栈的裸机/RTOS 平台上为 OFF。

基于此宏，所有模块分为两类:

| 类别 | 模块 | 说明 |
|------|------|------|
| 无网络依赖 (27 个) | platform, vocabulary, spsc_ringbuffer, log, async_log, timer, shell (ConsoleShell/UartShell), mem_pool, shutdown, bus, node, static_node, worker_pool, executor, hsm, bt, semaphore, app, post, qos, lifecycle_node, serial_transport, data_fusion, connection, node_manager_hsm, service_hsm, discovery_hsm, fault_collector, watchdog, shell_commands | 始终可用 |
| 网络依赖 (11 个, `#if OSP_HAS_NETWORK`) | socket, io_poller, transport, discovery, service, node_manager, transport_factory, shm_transport, net, DebugShell (shell.hpp 中 TCP telnet 后端) | 需 BSD socket |
| Linux 专用 (`#if OSP_PLATFORM_LINUX`) | process, system_monitor | 依赖 /proc 文件系统 |

---

### 4.2 vocabulary.hpp -- 词汇类型

零堆分配的基础类型，替代异常和动态容器。

**错误处理**:

| 类型 | 说明 |
|------|------|
| `expected<V, E>` | 轻量级 error-or-value 类型 |
| `optional<T>` | 可空值包装器 |
| `and_then(exp, F)` / `or_else(exp, F)` | 单子链式调用 |

**固定容量容器**:

| 类型 | 说明 |
|------|------|
| `FixedVector<T, Cap>` | 栈分配定长向量 |
| `FixedString<Cap>` | 固定容量字符串 |
| `FixedFunction<Sig, BufSize>` | SBO 回调 (默认 2\*sizeof(void\*)); const-qualified `operator()`; 支持 `nullptr_t` 构造/赋值; 编译期 `static_assert` 拒绝超限可调用体 |
| `function_ref<Sig>` | 非拥有型函数引用 (2 指针) |

**类型安全工具**: `not_null<T>`, `NewType<T, Tag>`, `ScopeGuard`, `OSP_SCOPE_EXIT(...)`

**错误枚举**:

```cpp
enum class ConfigError : uint8_t { kFileNotFound, kParseError, kFormatNotSupported, kBufferFull };
enum class TimerError  : uint8_t { kSlotsFull, kInvalidPeriod, kNotRunning, kAlreadyRunning };
enum class ShellError  : uint8_t { kRegistryFull, kDuplicateName, kPortInUse, kNotRunning,
                                   kAlreadyRunning, kAuthFailed, kDeviceOpenFailed };
enum class MemPoolError: uint8_t { kPoolExhausted, kInvalidPointer };
```

---

### 4.3 config.hpp -- 模板化多格式配置

统一接口解析 INI/JSON/YAML 配置文件。标签分发 + 模板特化，编译期选择后端。

```
Config<Backends...> ── if constexpr 分发 ──> IniBackend / JsonBackend / YamlBackend
```

```cpp
osp::MultiConfig cfg;
auto r = cfg.LoadFile("app.yaml");  // expected<void, ConfigError>
int32_t port = cfg.GetInt("network", "port", 8080);
auto opt = cfg.FindBool("debug", "verbose");  // optional<bool>
```

---

### 4.4 log.hpp -- 轻量日志

printf 风格线程安全日志，支持编译期和运行时级别过滤。

级别: `kDebug(0) < kInfo(1) < kWarn(2) < kError(3) < kFatal(4) < kOff(5)`

```cpp
OSP_LOG_INFO("sensor", "temp=%.1f from %u", t, id);
// 输出: [2026-02-14 14:30:00.123] [INFO] [sensor] temp=25.0 from 1
```

- `OSP_LOG_MIN_LEVEL` 编译期裁剪 (Release 默认 kInfo)
- 栈缓冲区 (ts 32B + msg 512B)，零堆分配
- POSIX `fprintf` 保证原子写

---

### 4.4a async_log.hpp -- 异步日志

基于 Per-Thread SPSC 环形缓冲的异步日志后端，零配置自动接管 `log.hpp` 宏。

```cpp
#include "osp/async_log.hpp"  // 自动接管 OSP_LOG_DEBUG/INFO/WARN

OSP_LOG_INFO("Sensor", "temp=%.1f", 25.0);  // 异步 SPSC, ~200-300ns
OSP_LOG_ERROR("Sensor", "fault!");           // 同步 fprintf, 崩溃安全
// 首次调用自动启动写线程, 进程退出自动 drain
```

**分级路由**: ERROR/FATAL 走同步 `fprintf` (崩溃安全); DEBUG/INFO/WARN 走异步 SPSC 环形缓冲 (不阻塞业务线程)。

**核心数据结构**:

| 结构 | 大小 | 说明 |
|------|------|------|
| `LogEntry` | 320B | trivially_copyable, 5 cache lines, memcpy 批处理 |
| `LogBuffer` (per-thread) | 80KB | `SpscRingbuffer<LogEntry, 256>` + slot 管理 |
| `AsyncLogContext` | ~2KB | 全局单例, 原子计数器, sink 指针 |

**线程模型**:
- 生产者: 业务线程, `thread_local` 快路径 (~1ns), CAS 首次注册
- 消费者: 后台写线程, round-robin 轮询, `PopBatch(32)` 批量消费
- 退避策略: 三阶段自适应 (spin → yield → sleep 50us)

**背压与丢弃上报**:
- 队列满时丢弃非关键日志 (不阻塞生产者)
- 定时 stderr 上报丢弃统计 (默认 10s 间隔)
- `GetAsyncStats()` API 可集成 Shell 诊断命令

**编译期配置**:

| 宏 | 默认值 | 说明 |
|----|--------|------|
| `OSP_ASYNC_LOG_QUEUE_DEPTH` | 256 | 每线程 SPSC 深度 |
| `OSP_ASYNC_LOG_MAX_THREADS` | 8 | 最大并发日志线程数 |
| `OSP_ASYNC_LOG_DROP_REPORT_INTERVAL_S` | 10 | 丢弃上报间隔 (秒) |
| `OSP_LOG_SYNC_ONLY` | 未定义 | 定义后禁用异步路径 |

> 详细设计见 [design_async_log_zh.md](design_async_log_zh.md)

---

### 4.5 timer.hpp -- 定时调度器

后台线程驱动的周期/单次任务调度，支持策略化时基。

```cpp
// 默认: SteadyTickSource (steady_clock)
osp::TimerScheduler<16> sched;
auto r = sched.Add(1000, heartbeat_fn, ctx);  // expected<TimerTaskId, TimerError>
sched.Start();
sched.Remove(r.value());
sched.Stop();

// ManualTickSource: 外部 tick 驱动 (硬件定时器/确定性测试)
osp::ManualTickSource::SetTickPeriodNs(1000000);  // 1ms/tick
osp::TimerScheduler<8, osp::ManualTickSource> tick_sched;
tick_sched.Add(100, my_callback);  // 100ms 周期
osp::ManualTickSource::Tick();     // ISR 或测试循环中推进

// NsToNextTask: 精确休眠计算 (借鉴 ztask)
uint64_t ns = sched.NsToNextTask();  // UINT64_MAX=无任务, 0=已过期
```

**TickSource 策略** (编译期模板参数):

| 策略 | 时钟源 | 适用场景 |
|------|--------|----------|
| `SteadyTickSource` (默认) | `steady_clock` | 生产环境 |
| `ManualTickSource` | 外部 `Tick()` 驱动 | 硬件定时器 ISR / 确定性测试 / 仿真 |

- 预分配任务槽数组，自动追赶错过的周期
- `TimerTaskId` 基于 `NewType<uint32_t, Tag>` 强类型 ID
- `NsToNextTask()`: 返回距下一任务触发的纳秒数，用于精确休眠/低功耗
- `PreciseSleep()`: Linux 使用 `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` 无漂移休眠

---

### 4.6 shell.hpp -- 多后端调试 Shell

三后端调试 Shell: TCP telnet / 本地 Console / UART 串口，共享行编辑和命令执行逻辑。

**后端可用性**: ConsoleShell 和 UartShell 始终可用 (依赖 termios); DebugShell (TCP telnet) 需要 `OSP_HAS_NETWORK`。

```
        ┌─────────────────────────────────────────────────────┐
        │                  共享层 (detail::)                   │
        │  ProcessByte ← FilterIac ← ESC FSM ← History       │
        │  TabComplete   ExecuteLine   SessionWrite            │
        │  WriteFn/ReadFn 函数指针抽象                          │
        └────────┬──────────────┬──────────────┬──────────────┘
                 │              │              │
        ┌────────▼───┐  ┌──────▼─────┐  ┌────▼────────┐
        │ DebugShell  │  │ConsoleShell│  │  UartShell   │
        │ TCP telnet  │  │stdin/stdout│  │ /dev/ttyS*   │
        │ IAC 过滤    │  │ termios raw│  │ baudrate cfg │
        │ 多连接 auth │  │ poll(100ms)│  │ poll(200ms)  │
        └─────────────┘  └────────────┘  └──────────────┘
```

**后端 I/O 抽象**:

```cpp
using ShellWriteFn = ssize_t (*)(int fd, const void* buf, size_t len);
using ShellReadFn  = ssize_t (*)(int fd, void* buf, size_t len);
// TCP: send/recv + MSG_NOSIGNAL     Console/UART: write/read (POSIX)
```

**字节处理流水线** (每字节经过):

```
raw byte ──> [FilterIac] ──> [skip_next_lf] ──> [ESC FSM] ──> [字符分类]
              (telnet 专用)    (\r\n 去重)        (方向键)      (Enter/BS/Tab/Ctrl/打印)
```

**IAC 协议过滤** (4 状态, telnet 专用):
- kNormal: 正常字节透传; 0xFF -> kIac
- kIac: WILL/WONT/DO/DONT (0xFB-0xFE) -> kNego; SB (0xFA) -> kSub; IAC IAC -> literal 0xFF
- kNego: 消耗 option byte -> kNormal
- kSub: 等待 IAC SE 结束子协商 -> kNormal

**ESC 序列解析** (3 状态):
- kNone: 0x1B -> kEsc
- kEsc: '[' -> kBracket; 其他 -> kNone
- kBracket: 'A' = HistoryUp, 'B' = HistoryDown, 'C'/'D' = 预留 -> kNone

**历史记录**: 环形缓冲 `history[16][256]`，head/count/browse 索引，跳过连续重复。

**认证** (DebugShell 可选): username/password 配置，3 次失败断开，星号掩码密码输入。

```cpp
// TCP telnet (多连接 + 认证)
osp::DebugShell::Config cfg;
cfg.port = 5090;
cfg.username = "admin";    // nullptr = 无认证
cfg.password = "secret";
osp::DebugShell shell(cfg);
shell.Start();

// 本地 Console (stdin/stdout raw mode)
osp::ConsoleShell console;
console.Start();           // 异步, 或 console.Run() 同步阻塞

// UART 串口
osp::UartShell::Config ucfg;
ucfg.device = "/dev/ttyS0";
ucfg.baudrate = 115200;
osp::UartShell uart(ucfg);
uart.Start();
```

**编译期配置宏**:

| 宏 | 默认值 | 说明 |
|----|--------|------|
| `OSP_SHELL_LINE_BUF_SIZE` | 256 | 行缓冲大小 (字节) |
| `OSP_SHELL_HISTORY_SIZE` | 16 | 历史记录条数 |
| `OSP_SHELL_MAX_ARGS` | 16 | 命令最大参数数 |

**线程安全**:
- 注册表: mutex 保护
- 会话线程: 各自独立 ShellSession，无共享可变状态
- Stop(): `shutdown(SHUT_RDWR)` 安全唤醒 recv() 阻塞的会话线程
- AcceptLoop: `poll()` + 200ms 超时，Stop() 时 close(listen_fd_) 即可退出
- `[[nodiscard]]` 标注所有有返回值意义的公有函数
- `ScopeGuard` / `OSP_SCOPE_EXIT` 管理 fd 生命周期

---

### 4.7 mem_pool.hpp -- 固定块内存池

O(1) 分配/释放的嵌入式内存池。

| 层级 | 类型 | 说明 |
|------|------|------|
| 底层 | `FixedPool<BlockSize, MaxBlocks>` | 原始块分配，嵌入式空闲链表 |
| 上层 | `ObjectPool<T, MaxObjects>` | 类型安全包装，placement new/delete |

```cpp
osp::ObjectPool<Packet, 256> pool;
auto r = pool.CreateChecked(args...);  // expected<Packet*, MemPoolError>
pool.Destroy(ptr);
```

内联存储 (全栈分配)，`mutex` 保护线程安全。

---

### 4.8 shutdown.hpp -- 优雅关停

进程信号处理与 LIFO 回调执行。

```
SIGINT/SIGTERM ──> 信号处理器 (写 pipe, async-signal-safe)
                          |
              WaitForShutdown() (读 pipe 阻塞)
                          |
              执行回调链 (LIFO 顺序) ──> 进程退出
```

- `sigaction(2)` + `pipe(2)` 异步信号安全唤醒
- LIFO 回调保证后注册的先执行 (嵌套清理)

---

## 5. 核心通信层

### 5.1 bus.hpp -- 无锁 MPSC 消息总线

核心消息传递基础设施，支持多生产者单消费者无锁发布。

```
Producer 0 ─┐
Producer 1 ──┼── CAS Publish ──> Ring Buffer ──> ProcessBatch() ──> Type-Based Dispatch
Producer 2 ─┘   (模板参数化)     (sequence-based)   (批量消费)      (variant + FixedFunction SBO)
```

**模板参数化**:

```cpp
template <typename PayloadVariant,
          uint32_t QueueDepth = 4096,
          uint32_t BatchSize = 256>
class AsyncBus;

using LightBus = AsyncBus<Payload, 256, 64>;    // ~20KB (嵌入式低内存)
using HighBus = AsyncBus<Payload, 4096, 256>;   // ~320KB (高吞吐)
```

**关键特性**:

| 特性 | 说明 |
|------|------|
| 无锁发布 | CAS 循环抢占生产者位置 |
| 优先级准入控制 | LOW 60% / MEDIUM 80% / HIGH 99% 阈值 |
| 批量处理 | `ProcessBatch()` 每轮最多 BatchSize 条消息 |
| 编译期访问者分发 | `ProcessBatchWith<Visitor>` 绕过回调表直接 `std::visit`，可内联 |
| 背压感知 | Normal/Warning/Critical/Full 四级 |
| 缓存行分离 | 生产者/消费者计数器分属不同缓存行 |
| 零堆分配回调 | `FixedFunction` SBO 替代 `std::function`，编译期拒绝超限捕获 |

**variant 内存优化策略**:

| 策略 | 说明 | 适用场景 |
|------|------|---------|
| 控制 variant 类型大小 | 大消息用指针/ID 间接引用 | 推荐，零改动 |
| 分离总线 | 不同大小的消息使用不同 AsyncBus 实例 | 子系统隔离 |
| MemPool 间接 | 大消息存入 ObjectPool，variant 存 `PoolHandle<T>` | 混合大小消息 |

**零堆分配回调 (FixedFunction SBO)**:

订阅回调使用 `FixedFunction<void(const Envelope&), 4*sizeof(void*)>` 替代 `std::function`，彻底消除热路径堆分配:

```cpp
// bus.hpp -- 订阅回调类型定义
static constexpr size_t kCallbackBufSize = 4 * sizeof(void*);  // 32B (64-bit)
using CallbackType = FixedFunction<void(const EnvelopeType&), kCallbackBufSize>;
```

| 特性 | 说明 |
|------|------|
| SBO 内联存储 | 32 字节缓冲区，容纳含 1-2 个捕获变量的 lambda |
| 编译期大小校验 | `static_assert(sizeof(Decay) <= BufferSize)` 拒绝超限可调用体 |
| const 正确性 | `operator()` 为 const 限定，匹配 Envelope 的 const 引用语义 |
| 零堆分配 | 整个代码库不再使用 `std::function`，与 worker_pool.hpp 保持一致 |

**单消费者设计取舍**: 消费侧无锁、无竞争，延迟确定性好。消息处理回调应轻量 (<10us)，重计算应转发到 WorkerPool。如需多消费者，可按消息类型分片到多个 AsyncBus 实例。

---

### 5.2 node.hpp -- 轻量 Pub/Sub 节点

受 ROS2/CyberRT 启发的轻量节点通信抽象。

```cpp
// 依赖注入总线实例 (推荐)
osp::AsyncBus<Payload, 1024> my_bus;
osp::Node<Payload> sensor("sensor", 1, my_bus);

// 纯类型路由 (默认，编译期确定)
sensor.Subscribe<SensorData>([](const SensorData& d, const osp::MessageHeader& h) {
    OSP_LOG_INFO("sensor", "temp=%.1f from sender %u", d.temp, h.sender_id);
});

// 混合路由: 类型 + 主题名 (区分同类型不同语义)
sensor.Subscribe<SensorData>("imu/temperature", on_imu_temp);
sensor.Subscribe<SensorData>("env/temperature", on_env_temp);

sensor.Publish(SensorData{25.0f});
sensor.Publish(SensorData{18.0f}, "env/temperature");
sensor.SpinOnce();
```

- 类型路由为主，主题路由为辅 (FNV-1a 32-bit hash)
- RAII 自动退订，总线依赖注入 (支持多总线实例隔离)
- `Publisher<T>` 轻量发布器 (无 RAII 清理)

---

### 5.3 static_node.hpp -- 编译期 Handler 绑定节点

`Node` 的零开销替代方案。Handler 类型作为模板参数，编译器可内联分发调用、生成直接跳转表。

**设计动机**: `Node` 通过 `FixedFunction` (SBO 类型擦除) 存储回调，分发时仍有函数指针间接调用。嵌入式系统中约 80% 的消息处理逻辑在编译期确定 (传感器数据总是交给同一个处理函数)，`std::function`/`FixedFunction` 的运行时灵活性完全多余。

**双模式分发**:

StaticNode 的 `SpinOnce()` 根据是否调用 `Start()` 自动选择分发模式:

| 模式 | 触发条件 | 分发路径 | 适用场景 |
|------|---------|---------|---------|
| 回调模式 | 调用 `Start()` | `ProcessBatch()` -> 回调表 -> FixedFunction | 多订阅者共享同一 Bus |
| 直接分发 | 不调用 `Start()` | `ProcessBatchWith(handler_)` -> `std::visit` | 单消费者 (MPSC) |

```cpp
// Handler 协议: 为关注的消息类型定义 operator()，模板 catch-all 忽略其余类型
struct StreamHandler {
  ProtocolState* state;

  void operator()(const StreamCommand& cmd, const osp::MessageHeader&) {
    ++state->stream_count;  // 编译器可内联到 Bus 分发路径中
  }
  void operator()(const StreamData& sd, const osp::MessageHeader&) {
    ++state->stream_count;
  }
  template <typename T>
  void operator()(const T&, const osp::MessageHeader&) {}  // 无关类型: 零开销
};

// 直接分发模式 (推荐: 零开销，15x 快于回调模式)
osp::StaticNode<Payload, StreamHandler> ctrl("ctrl", 3, StreamHandler{&state});
ctrl.SpinOnce();  // 不调用 Start()，自动使用 ProcessBatchWith

// 回调模式 (兼容多订阅者)
osp::StaticNode<Payload, StreamHandler> ctrl2("ctrl2", 4, StreamHandler{&state});
ctrl2.Start();     // 注册到回调表
ctrl2.SpinOnce();  // 使用 ProcessBatch
ctrl2.Stop();      // RAII 退订
```

**消除的开销 (直接分发 vs 回调模式)**:

| 开销项 | 回调模式 (ProcessBatch) | 直接分发 (ProcessBatchWith) |
|--------|:---:|:---:|
| SharedSpinLock 读锁 | 有 | **无** |
| callback_table_ 遍历 | 有 | **无** |
| FixedFunction 间接调用 | 有 | **无** |
| `std::visit` 编译期跳转表 | 无 | 有 (可内联) |

**性能对比** (1000 SmallMsg, P50):

| 模式 | ns/msg | 加速比 |
|------|-------:|-------:|
| Direct (ProcessBatchWith) | ~2 | **15x** |
| Callback (ProcessBatch) | ~30 | 1x |
| Node (FixedFunction) | ~29 | ~1x |

**与 Node 对比**:

| 特性 | Node | StaticNode |
|------|------|-----------|
| 回调存储 | `FixedFunction` (SBO 类型擦除) | Handler 模板参数 (类型已知) |
| 分发方式 | 函数指针间接调用 | 编译期直接调用 (可内联) |
| 订阅灵活性 | 运行时逐个注册 | `Start()` 一次性注册所有 variant 类型 |
| 零开销直接分发 | 不支持 | `ProcessBatchWith` (不调用 Start) |
| 用作 `void*` 上下文 | 可以 (timer callback 等) | 不推荐 (模板类型不便擦除) |
| 适用场景 | 运行时动态订阅、跨编译单元 | 编译期确定的固定处理逻辑 |

**实际应用**: `streaming_protocol` 示例中 3 个服务端节点 (registrar, heartbeat_monitor, stream_controller) 使用 StaticNode 直接分发模式，客户端节点保留 Node (需作为 timer context 指针)。

---

### 5.4 worker_pool.hpp -- 多工作线程池

基于 AsyncBus + SpscRingbuffer 的多工作线程池。

```
Submit(job) ──> AsyncBus::Publish()  [无锁 MPSC]
                       |
               DispatcherThread  (ProcessBatch 循环, round-robin)
                       |
            ┌──────────┼──────────┐
            v          v          v
      Worker[0]    Worker[1]  Worker[N-1]
      SpscRingbuffer (编译期深度 OSP_WORKER_QUEUE_DEPTH=1024)
```

```cpp
osp::WorkerPoolConfig cfg{.name = "processor", .worker_num = 4};
osp::WorkerPool<Payload> pool(cfg);
pool.RegisterHandler<SensorData>(process_sensor);
pool.Start();
pool.Submit(SensorData{25.0f});
pool.Shutdown();
```

- 二级排队: MPSC 总线 (入口) + SpscRingbuffer 每工作线程 (分发)
- `FlushAndPause()` / `Resume()` 支持优雅排空
- Linux CPU 亲和性 + 线程优先级

---

### 5.5 spsc_ringbuffer.hpp -- Lock-free SPSC 环形缓冲区

从 [DeguiLiu/ringbuffer](https://github.com/DeguiLiu/ringbuffer) v2.0.0 适配。

```cpp
template <typename T, size_t BufferSize = 16, bool FakeTSO = false, typename IndexT = size_t>
class SpscRingbuffer;
```

**核心特性**:

| 特性 | 说明 |
|------|------|
| Lock-free / Wait-free | 单生产者单消费者，无锁无等待 |
| 编译期双路径 | `if constexpr`: trivially_copyable 走 memcpy，否则走 move |
| 批量操作 | PushBatch / PopBatch，环形 wrap-around 自动处理 |
| 零拷贝查看 | Peek() 返回指针，At(n) 随机访问，Discard(n) 跳过 |
| FakeTSO 模式 | 单核 MCU 或 x86 TSO 下使用 relaxed ordering |
| 缓存行对齐 | head/tail 分别对齐到 kCacheLineSize，避免 false sharing |

**内存序策略**:

| FakeTSO | Acquire | Release | 适用场景 |
|---------|---------|---------|----------|
| false | `memory_order_acquire` | `memory_order_release` | 多核 ARM/RISC-V |
| true | `memory_order_relaxed` | `memory_order_relaxed` | 单核 MCU / x86 TSO |

**跨层集成**:

| 集成点 | 模块 | 元素类型 | 容量宏 |
|--------|------|---------|--------|
| 工作线程池 | worker_pool.hpp | `MessageEnvelope<PayloadVariant>` | `OSP_WORKER_QUEUE_DEPTH` (1024) |
| 串口字节缓冲 | serial_transport.hpp | `uint8_t` | `OSP_SERIAL_RX_RING_SIZE` (4096) |
| 网络帧缓冲 | transport.hpp | `RecvFrameSlot` | `OSP_TRANSPORT_RECV_RING_DEPTH` (32) |
| 统计通道 | shm_ipc 示例 | `ShmStats` (48B) | 16 |

---

### 5.6 executor.hpp -- 调度器

借鉴 ROS2 Executor 和 CyberRT Choreography Scheduler。

| 模式 | 说明 |
|------|------|
| `SingleThreadExecutor` | 单线程轮询所有节点 |
| `StaticExecutor<PV, Sleep>` | 固定节点-线程映射 (确定性调度) |
| `PinnedExecutor<PV, Sleep>` | 每节点绑定 CPU 核心 + 实时优先级 |
| `RealtimeExecutor<PV, Sleep>` | SCHED_FIFO + mlockall + 优先级队列 |

**SleepStrategy 策略** (编译期模板参数，默认 `YieldSleepStrategy` 保持向后兼容):

| 策略 | 空闲行为 | 适用场景 |
|------|----------|----------|
| `YieldSleepStrategy` (默认) | `std::this_thread::yield()` | 通用工作负载 |
| `PreciseSleepStrategy` | `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` | 嵌入式低功耗、周期性任务 |

```cpp
// 向后兼容: 默认 yield
osp::StaticExecutor<Payload> exec;

// 精确休眠: 1ms 默认, 100us~10ms 范围
osp::PreciseSleepStrategy sleep(1000000ULL, 100000ULL, 10000000ULL);
osp::StaticExecutor<Payload, osp::PreciseSleepStrategy> exec(sleep);

// 支持外部设置唤醒点
sleep.SetNextWakeup(osp::SteadyNowNs() + 10000000ULL);  // 10ms 后唤醒
```

**实时调度配置**:

```cpp
struct RealtimeConfig {
  int sched_policy = SCHED_OTHER;   // SCHED_OTHER / SCHED_FIFO / SCHED_RR
  int sched_priority = 0;           // SCHED_FIFO: 1-99
  bool lock_memory = false;         // mlockall(MCL_CURRENT | MCL_FUTURE)
  uint32_t stack_size = 0;          // 0=系统默认
  int cpu_affinity = -1;            // -1=不绑定, >=0=绑定到指定核心
};
```

---

### 5.7 其他核心组件

**semaphore.hpp**: `LightSemaphore` (futex-based) / `BinarySemaphore` / `PosixSemaphore`。12 test cases。

**data_fusion.hpp**: 借鉴 CyberRT DataFusion，多消息对齐触发。`FusedSubscription<MsgTypes...>` 当所有类型各收到至少一条时触发回调。10 test cases。

---

## 6. 状态机与行为树

### 6.1 hsm.hpp -- 层次状态机

集成 hsm-cpp，事件驱动状态管理。

```
StateMachine<Context, MaxStates>
  ├── Off
  ├── On (parent)
  │   ├── Idle
  │   ├── Running
  │   └── Paused
  └── Error

Event dispatch bubbles up to parent.
LCA-based entry/exit path.
Guard conditions control transitions.
```

**与消息总线集成**: 总线消息 -> Event(id) -> StateMachine::Dispatch()

**关键特性**:
- 编译期 MaxStates 模板参数，零堆分配
- LCA (Lowest Common Ancestor) 算法自动处理 entry/exit 路径
- Guard 条件控制状态转换合法性
- 单线程 Dispatch，handler 不可重入

---

### 6.2 bt.hpp -- 行为树

集成 bt-cpp，异步任务编排。

```
    Sequence
    ├── Condition: check_connection()
    ├── Action: send_heartbeat()     [kRunning -> kSuccess]
    └── Selector
        ├── Action: process_data()
        └── Action: fallback_handler()
```

- 扁平数组存储，索引引用 (非指针)，缓存友好
- 编译期 `OSP_BT_MAX_NODES` (32) / `OSP_BT_MAX_CHILDREN` (8)
- 叶节点通过 WorkerPool 提交异步任务

---

## 7. 网络与传输层

### 7.1 socket.hpp -- Socket 抽象层

RAII Socket 包装，替代裸 POSIX fd 操作。

```cpp
class TcpSocket {
  static expected<TcpSocket, SocketError> Connect(const char* host, uint16_t port);
  expected<size_t, SocketError> Send(const void* data, size_t len);
  expected<size_t, SocketError> Recv(void* buf, size_t len);
  void Close() noexcept;
};
class UdpSocket { /* SendTo/RecvFrom */ };
class TcpListener { /* accept loop */ };
```

---

### 7.2 io_poller.hpp -- I/O 多路复用

epoll (Linux) / kqueue (macOS) / poll (其他 POSIX) 统一抽象，`#if OSP_HAS_NETWORK` 条件编译。

| 平台 | 后端 |
|------|------|
| Linux | epoll (默认) |
| macOS | kqueue |
| 其他 POSIX | poll() 回退 |

```cpp
class IoPoller {
  expected<void, IoError> Add(int fd, uint32_t events, void* ctx);
  expected<uint32_t, IoError> Wait(IoEvent* events, uint32_t max, int timeout_ms);
};
```

---

### 7.3 connection.hpp -- 连接池

固定容量连接管理。`ConnectionPool<MaxConns>` 提供 Add/Remove/ForEach 接口。

---

### 7.4 transport.hpp -- 透明网络传输层

使 Node 屏蔽本地进程和远程进程的差异。

**架构**:

```
Node::Publish(msg)
       |
TransportFactory::Route()
       |
  ┌────┼────┬────┬────┐
  v    v    v    v    v
inproc  shm  tcp  udp  serial
```

**消息帧格式** (v1, 26 字节):

```
┌─────────┬─────────┬────────┬─────────┬──────────┬────────────┬─────────┐
│magic(4B)│len(4B)  │type(2B)│sender(4B)│seq_num(4B)│timestamp(8B)│payload │
│0x4F5350 │total    │variant │node id  │单调递增   │steady_clock│N bytes │
│00       │bytes    │index   │         │           │纳秒        │        │
└─────────┴─────────┴────────┴─────────┴──────────┴────────────┴─────────┘
```

- `seq_num`: 每发送端单调递增，检测丢包/乱序/重复
- `timestamp`: steady_clock 纳秒，端到端延迟分析和 DataFusion 时间对齐

**NetworkNode**: 继承 Node，透明远程发布/订阅。

```cpp
osp::NetworkNode<Payload> sensor("sensor", 1);
sensor.AdvertiseTo<SensorData>({"tcp://0.0.0.0:9001"});
sensor.Publish(SensorData{25.0f});  // 本地 + 远程同时发布
```

**TCP 发送: short write 与 EAGAIN 处理** (v0.4.1):

`TcpTransport::SendAll()` 采用同步循环发送，处理 TCP short write (部分写入)。
v0.4.1 新增 EAGAIN/EWOULDBLOCK 区分:

- `SocketError::kWouldBlock`: 内核发送缓冲区满，瞬态错误
- `SendAll()` 对 EAGAIN 进行有限重试 (yield + 最多 16 次)，重试耗尽返回 `TransportError::kWouldBlock` 而**不断开连接**
- 致命错误 (EPIPE, ECONNRESET 等) 仍返回 `kSendFailed` 并标记连接断开

**已知限制**: SendAll 是同步阻塞的，重试期间调用线程被阻塞。高吞吐 TCP
场景 (如远程传感器流) 应考虑用户态发送缓冲 (SPSC 字节环形缓冲 + EPOLLOUT
事件驱动异步刷写) 替代同步重试。当前方案适用于 newosp 主要场景: 同机
shm_transport 优先，TCP 作为远程低频备选。

---

### 7.5 shm_transport.hpp -- 共享内存 IPC

借鉴 cpp-ipc 的共享内存无锁队列、ROS2 的零拷贝设计和 cpp_py_shmbuf 的 SPSC 字节流环形缓冲区。

提供三种互补的环形缓冲区:

| 类型 | 模式 | 适用场景 | 消息格式 |
|------|------|----------|----------|
| ShmRingBuffer | MPSC slot-based | 小消息、多生产者 (控制指令、状态上报) | 固定 slot (SlotSize) |
| ShmSpscByteRing | SPSC byte-stream | 大变长载荷 (LiDAR 点云、视频帧) | [4B 长度前缀][payload] |
| ShmSpmcByteRing | SPMC byte-stream | 数据分发 (1 写 N 读, 传感器广播) | [4B 长度前缀][payload] |

**架构**:

```
Process A                          Process B
┌──────────────┐                  ┌──────────────┐
│ ShmChannel   │  MPSC slot-based │ ShmChannel   │
│ (writer)     │  (小消息/多生产者)│ (reader)     │
└──────┬───────┘                  └──────┬───────┘
       │          Shared Memory          │
       v     /osp_shm_<name>             v
  ┌────────────────────────────────────────┐
  │ ShmRingBuffer (CAS-based MPSC)         │
  │ [slot0][slot1]...[slotN]               │
  │ prod_pos_ (atomic, cache-line aligned) │
  │ cons_pos_ (atomic, cache-line aligned) │
  └────────────────────────────────────────┘

Process A                          Process B
┌────────────────┐                ┌────────────────┐
│ ShmByteChannel │  SPSC byte     │ ShmByteChannel │
│ (writer)       │  (大载荷/futex)│ (reader)       │
└──────┬─────────┘                └──────┬─────────┘
       │          Shared Memory          │
       v     /osp_shm_<name>             v
  ┌────────────────────────────────────────┐
  │ ShmSpscByteRing (SPSC byte-stream)     │
  │ [head(4)][tail(4)][capacity(4)][rsv(4)]│
  │ [data area: circular buffer]           │
  │ futex(head) for wakeup notification    │
  └────────────────────────────────────────┘
```

**核心接口**:

```cpp
// 共享内存段 RAII 封装 (BuildName/MmapWithFallback/PageAlign 内部去重)
class SharedMemorySegment {
  static expected<SharedMemorySegment, ShmError> Create(const char* name, uint32_t size);
  static expected<SharedMemorySegment, ShmError> CreateOrReplace(const char* name, uint32_t size);
  static expected<SharedMemorySegment, ShmError> Open(const char* name);
  void Unlink() noexcept;
};

// MPSC slot-based 环形缓冲区 (小消息、多生产者)
template <uint32_t SlotSize, uint32_t SlotCount>
class ShmRingBuffer {
  static ShmRingBuffer* InitAt(void* shm_addr);
  static ShmRingBuffer* AttachAt(void* shm_addr);
  bool TryPush(const void* data, uint32_t size);
  bool TryPop(void* data, uint32_t& size);
  uint32_t Depth() const noexcept;
};

// SPSC 字节流环形缓冲区 (大变长载荷: LiDAR/视频帧)
class ShmSpscByteRing {
  static ShmSpscByteRing InitAt(void* shm_base, uint32_t total_size);
  static ShmSpscByteRing AttachAt(void* shm_base);
  bool Write(const void* data, uint32_t len);     // [4B len][payload]
  uint32_t Read(void* out, uint32_t max_len);      // 返回 payload 长度
  uint32_t ReadableBytes() const noexcept;
  uint32_t WriteableBytes() const noexcept;
};

// MPSC slot-based 命名通道 (polling 等待)
template <uint32_t SlotSize, uint32_t SlotCount>
class ShmChannel {
  static expected<ShmChannel, ShmError> CreateWriter(const char* name);
  static expected<ShmChannel, ShmError> CreateOrReplaceWriter(const char* name);
  static expected<ShmChannel, ShmError> OpenReader(const char* name);
  expected<void, ShmError> Write(const void* data, uint32_t size);
  expected<void, ShmError> Read(void* data, uint32_t& size);
  expected<void, ShmError> WaitReadable(uint32_t timeout_ms);
};

// SPSC 字节流命名通道 (futex 低延迟唤醒)
class ShmByteChannel {
  static expected<ShmByteChannel, ShmError> CreateWriter(const char* name, uint32_t capacity);
  static expected<ShmByteChannel, ShmError> CreateOrReplaceWriter(const char* name, uint32_t capacity);
  static expected<ShmByteChannel, ShmError> OpenReader(const char* name);
  expected<void, ShmError> Write(const void* data, uint32_t size);  // 自动 futex wake
  uint32_t Read(void* data, uint32_t max_len);
  expected<void, ShmError> WaitReadable(uint32_t timeout_ms);       // futex wait
};

// SPMC 字节流环形缓冲区 (1 写 N 读, 数据分发)
class ShmSpmcByteRing {
  static ShmSpmcByteRing InitAt(void* shm_base, uint32_t total_size, uint32_t max_consumers);
  static ShmSpmcByteRing AttachAt(void* shm_base);
  int32_t RegisterConsumer();                       // 返回 consumer_id, -1 = 满
  void UnregisterConsumer(int32_t consumer_id);
  bool Write(const void* data, uint32_t len);       // 检查最慢消费者
  uint32_t Read(int32_t consumer_id, void* out, uint32_t max_len);
  uint32_t ConsumerCount() const noexcept;
};

// SPMC 字节流命名通道 (futex 广播唤醒)
class ShmSpmcByteChannel {
  static expected<ShmSpmcByteChannel, ShmError> CreateWriter(const char* name, uint32_t capacity, uint32_t max_consumers);
  static expected<ShmSpmcByteChannel, ShmError> CreateOrReplaceWriter(const char* name, uint32_t capacity, uint32_t max_consumers);
  static expected<ShmSpmcByteChannel, ShmError> OpenReader(const char* name);  // 自动 RegisterConsumer
  expected<void, ShmError> Write(const void* data, uint32_t size);  // 自动 futex wake ALL
  uint32_t Read(void* data, uint32_t max_len);
  expected<void, ShmError> WaitReadable(uint32_t timeout_ms);
  uint32_t ConsumerCount() const noexcept;
  int32_t ConsumerId() const noexcept;
};
```

**ShmSpscByteRing 设计要点**:

- 单调递增 head/tail 索引 + power-of-2 bitmask 取模，无 buf_full 标志竞态
- 16 字节 POD header (head, tail, capacity, reserved)，跨语言兼容 (Python ctypes 可直接读写)
- 消息格式: `[4B LE length][payload]`，支持变长消息，不浪费 slot 尾部空间
- `atomic_thread_fence(acquire/release)` 保证跨进程可见性

**Futex 通知机制 (ShmByteChannel)**:

ShmByteChannel 使用 Linux futex 替代 polling，将等待延迟从 ms 级降到 us 级:

| 操作 | 机制 | 说明 |
|------|------|------|
| Writer.Write() | futex(FUTEX_WAKE) | 写入后自动唤醒等待的 reader |
| Reader.WaitReadable() | futex(FUTEX_WAIT) | 在 head 字段上等待，head 变化即唤醒 |
| Reader fast path | 直接检查 HasData() | 有数据时不进入 futex 系统调用 |

futex 地址直接指向共享内存中的 head 字段，无需额外 fd (eventfd 无法跨进程共享)。

**ARM 弱内存模型注意事项**:

| 操作 | memory_order | 说明 |
|------|-------------|------|
| 生产者写入 slot 数据 | 普通写 (非原子) | 数据填充阶段 |
| 生产者推进 `prod_pos_` | `release` | 确保 slot 数据对消费者可见 |
| 消费者读取 `prod_pos_` | `acquire` | 与生产者 release 配对 |
| 消费者读取 slot 数据 | 普通读 (非原子) | acquire 之后保证可见 |
| `ref_count` 递减 | `acq_rel` | 最后一个 Release 需要看到所有写入 |
| ShmSpscByteRing head/tail | `release`/`acquire` fence | 跨进程 SPSC 可见性 |

避免默认 `seq_cst`，显式使用 `acquire/release` 配对，在 ARM 上可获得 3-5 倍性能提升。

**Huge Pages 支持 (可选)**:

定义 `OSP_SHM_HUGE_PAGES` 宏后，`SharedMemorySegment::Create()` 和 `Open()` 的 `mmap()` 调用携带 `MAP_HUGETLB` 标志，减少大段共享内存的 TLB miss。若系统未配置 huge pages 或权限不足，自动回退到普通 `MAP_SHARED`，无需调用方处理。启用前需确保内核预留了足够的 huge pages (`/proc/sys/vm/nr_hugepages`)。

**崩溃恢复: CreateOrReplace 模式**:

工业场景中进程可能因 SIGKILL 或断电异常退出，残留 `/dev/shm/osp_shm_*` 文件。`CreateOrReplace` 先 `shm_unlink` 清除残留，再执行标准 `Create`:

```cpp
// MPSC slot-based
auto result = ShmChannel<4096, 256>::CreateOrReplaceWriter("cmd_ch");

// SPSC byte-stream (8 MB buffer for LiDAR point clouds)
auto result = ShmByteChannel::CreateOrReplaceWriter("lidar_ch", 8 * 1024 * 1024);

// SPMC data distribution (1 writer, up to 4 consumers)
auto result = ShmSpmcByteChannel::CreateOrReplaceWriter("lidar_spmc", 256 * 1024, 4);
```

**ShmSpmcByteRing 设计要点 (SPMC 数据分发)**:

SPMC 模式解决传感器数据一对多分发场景 (如 LiDAR 点云同时发送给日志、融合、可视化模块):

```
Producer                    Shared Memory                  Consumers
┌──────────┐          /osp_shm_<name>              ┌──────────────┐
│ Writer   │    ┌──────────────────────────┐       │ Consumer 0   │
│ (1 个)   │───>│ ShmSpmcByteRing          │──────>│ (Logging)    │
│          │    │ head (producer 写位置)    │       └──────────────┘
└──────────┘    │ tails[0..N] (各消费者读位置)│     ┌──────────────┐
                │ active[0..N] (CAS 注册)  │──────>│ Consumer 1   │
                │ [data area: ring buffer] │       │ (Fusion)     │
                │ futex(head) wake ALL     │       └──────────────┘
                └──────────────────────────┘       ┌──────────────┐
                                             ──────>│ Consumer 2   │
                                                   │ (Visualize)  │
                                                   └──────────────┘
```

- 每个消费者通过 CAS 原子操作注册独立的 tail 指针，互不干扰
- Producer 写入时检查最慢消费者的 tail，防止覆盖未读数据
- futex WAKE ALL 广播唤醒所有等待的消费者
- 消费者动态注册/注销，OpenReader 自动分配 consumer_id
- 编译期宏 `OSP_SHM_SPMC_MAX_CONSUMERS` 控制最大消费者数 (默认 8)

| 维度 | ShmByteChannel (SPSC) | ShmSpmcByteChannel (SPMC) |
|------|----------------------|--------------------------|
| 消费者数 | 1 | 1..N (默认最多 8) |
| 写入检查 | head - tail | head - slowest_tail |
| 通知机制 | futex wake 1 | futex wake ALL |
| 消费者注册 | 无 | CAS atomic 注册 |
| Header 大小 | 16 bytes | 动态 (取决于 max_consumers) |
| 适用场景 | 点对点传输 | 数据分发 (1:N) |

---

### 7.6 serial_transport.hpp -- 串口传输

集成 CSerialPort 实现工业级串口通信。

```
Board A                              Board B
┌──────────────┐    UART/RS-485    ┌──────────────┐
│ SerialTransport                  │ SerialTransport
│ (NATIVE_SYNC)│  ──────────────>  │ (NATIVE_SYNC)│
└──────┬───────┘    /dev/ttyS1     └──────┬───────┘
       └──── IoPoller (epoll) ────────────┘
```

**帧格式** (扩展网络帧，增加 CRC):

```
┌─────────┬─────────┬────────┬─────────┬─────────┬──────────┐
│magic(4B)│len(4B)  │type(2B)│sender(4B)│payload  │crc16(2B) │
└─────────┴─────────┴────────┴─────────┴─────────┴──────────┘
```

- NATIVE_SYNC 模式: 无堆分配、无线程、兼容 `-fno-exceptions -fno-rtti`
- 串口 fd 直接加入 IoPoller 统一事件循环
- SpscRingbuffer<uint8_t, 4096> 解耦 I/O 读取与帧解析状态机
- 帧同步状态机 (kWaitMagic/kWaitHeader/kWaitPayload/kWaitCrc) + CRC16 校验

---

### 7.7 net.hpp -- 网络层封装

sockpp 集成层，提供 `osp::expected` 错误处理的网络接口。

| 类型 | 说明 |
|------|------|
| `TcpClient` | TCP 客户端连接 (sockpp::tcp_connector) |
| `TcpServer` | TCP 服务端监听 (sockpp::tcp_acceptor) |
| `UdpPeer` | UDP 收发 (sockpp::udp_socket) |

条件编译: 仅在 `OSP_HAS_SOCKPP` 定义时可用。

---

### 7.8 transport_factory.hpp -- 自动传输选择

借鉴 CyberRT，根据配置自动选择传输方式。

```
TransportFactory::Detect(cfg)
       |
       ├── 有 SHM channel name ──> kShm
       ├── localhost ──> kShm
       └── 远程地址 ──> kTcp
```

---

## 8. 服务与发现层

### 8.1 discovery.hpp -- 节点发现

借鉴 ROS2 DDS 和 CyberRT 拓扑发现。

```cpp
// UDP 多播自动发现
class MulticastDiscovery {
  void Announce(const char* node_name, const Endpoint& ep);
  void SetOnNodeJoin(void(*cb)(const char* name, const Endpoint& ep));
  void SetOnNodeLeave(void(*cb)(const char* name));
  void Start();
  void Stop();
};
```

支持 TopicAwareDiscovery (按主题名发现)。

---

### 8.2 service.hpp -- RPC 服务

借鉴 ROS2 Service/Client 模式。

```cpp
template <typename Request, typename Response>
class Service {
  void SetHandler(function_ref<Response(const Request&)> handler);
  void Start(const Endpoint& ep);
};

template <typename Request, typename Response>
class Client {
  expected<Response, ServiceError> Call(const Request& req, int timeout_ms);
};
```

支持 AsyncClient + ServiceRegistry 服务注册表。

---

### 8.3 node_manager.hpp -- 节点管理器

兼容原始 OSP 的 ospnodeman，管理 TCP/SHM 连接、心跳检测、断开通知。

```cpp
class NodeManager {
  expected<uint16_t, NodeError> CreateListener(uint16_t port);
  expected<uint16_t, NodeError> Connect(const char* host, uint16_t port);
  void OnDisconnect(void(*cb)(uint16_t node_id));
  void Start();
  void Stop();
};
```

---

### 8.4 node_manager_hsm.hpp -- HSM 驱动节点管理

为每个 TCP/SHM 连接提供独立的 HSM 状态机。

```
Connected ──(心跳超时)──> Suspect ──(超时次数达阈值)──> Disconnected
    ^                       |
    └───(心跳恢复)──────────┘
```

- 每连接独立 HSM 实例 (最多 64 个)
- 内置 FaultReporter: 心跳超时报 kFaultHeartbeatTimeout (kHigh)，断连报 kFaultDisconnected (kCritical)
- 15 test cases, ASan/TSan/UBSan clean

---

### 8.5 service_hsm.hpp -- HSM 驱动服务生命周期

管理 Service 服务端连接生命周期。

```
Idle ──(Start)──> Listening ──(ClientConnected)──> Active
                                                     |
                                              (Error) |  (Stop)
                                                     v      v
                                                   Error  ShuttingDown
```

- 可选 FaultReporter: Error 状态入口自动报 kFaultServiceError (kHigh)
- 10 test cases

---

### 8.6 discovery_hsm.hpp -- HSM 驱动节点发现

管理节点发现流程。

```
Idle ──(Start)──> Announcing ──(NodeFound)──> Discovering
                                                  |
                                    (达到阈值)     |  (节点丢失)
                                                  v      v
                                               Stable  Degraded
```

- 可选 FaultReporter: Degraded 状态入口自动报 kFaultNetworkDegraded (kMedium)
- 可配置稳定阈值 (`stable_threshold`)
- 10 test cases

---

## 9. 应用层

### 9.1 app.hpp -- 应用-实例模型

兼容原始 OSP 的 CApp/CInstance 两层架构，采用编译期分发替代虚函数。

设计决策:
- Instance 基类无 virtual: OnMessage() 为非虚空实现，派生类通过名称隐藏 (name hiding) 覆盖
- Application 模板参数化: `Application<InstanceImpl, MaxInstances>` 编译期绑定具体类型
- ObjectPool 零堆分配: 所有 Instance 内存由 `ObjectPool<InstanceImpl, MaxInstances>` 管理
- HSM 消息分发: `BeginMessage()`/`EndMessage()` public wrapper 驱动 Processing 状态转换

```cpp
// 全局实例 ID 编解码
constexpr uint32_t MakeIID(uint16_t app_id, uint16_t ins_id);
constexpr uint16_t GetAppId(uint32_t iid);
constexpr uint16_t GetInsId(uint32_t iid);

// 实例基类 (HSM 驱动, 11 状态, 非虚)
class Instance {
  void OnMessage(uint16_t event, const void* data, uint32_t len) noexcept; // 非虚, 派生类隐藏
  void BeginMessage() noexcept;   // HSM: Idle -> Processing
  void EndMessage() noexcept;     // HSM: Processing -> Idle
  void Initialize() noexcept;     // HSM: Created -> Idle
  uint16_t CurState() const noexcept;
};

// 应用 (ObjectPool 管理实例, 零堆分配)
template <typename InstanceImpl, uint16_t MaxInstances = 64>
class Application {
  static_assert(std::is_base_of<Instance, InstanceImpl>::value, "...");
  expected<uint16_t, AppError> CreateInstance();   // ObjectPool::Create()
  expected<void, AppError> DestroyInstance(uint16_t ins_id); // ObjectPool::Destroy()
  bool Post(uint16_t ins_id, uint16_t event, const void* data, uint32_t len);
  bool ProcessOne() noexcept;  // BeginMessage -> OnMessage -> EndMessage
};
```

---

### 9.2 post.hpp -- 统一消息投递

兼容原始 OSP 的 OspPost() 统一投递接口。

```cpp
// 统一投递 (自动路由: 本地/进程间/跨机器)
bool OspPost(uint32_t dst_iid, uint16_t event,
             const void* data, uint32_t len, uint16_t dst_node = 0);

// 同步请求-响应
expected<uint32_t, PostError> OspSendAndWait(
    uint32_t dst_iid, uint16_t event,
    const void* data, uint32_t len,
    void* ack_buf, uint32_t ack_buf_size,
    uint16_t dst_node = 0, int timeout_ms = 2000);
```

AppRegistry 内部使用 `FixedVector<AppEntry, N>` 管理注册表。

---

### 9.3 qos.hpp -- QoS 服务质量

借鉴 ROS2 QoS 策略，精简为 5 个核心参数。

```cpp
struct QosProfile {
  Reliability reliability = Reliability::kBestEffort;
  HistoryPolicy history = HistoryPolicy::kKeepLast;
  uint16_t depth = 10;
  uint32_t deadline_ms = 0;      // 0=不检测
  uint32_t lifespan_ms = 0;      // 0=永不过期
};

// 预定义 QoS (constexpr，零开销)
inline constexpr QosProfile kQosSensorData{
    Reliability::kBestEffort, HistoryPolicy::kKeepLast, 5, 0, 100};
inline constexpr QosProfile kQosControlCommand{
    Reliability::kReliable, HistoryPolicy::kKeepLast, 20, 50, 0};
inline constexpr QosProfile kQosSystemStatus{
    Reliability::kReliable, HistoryPolicy::kKeepLast, 1, 1000, 0};
```

**QoS 对 Transport 选择的影响**:

| 条件 | 传输方式 |
|------|---------|
| Reliable + 跨机器 | TCP |
| BestEffort + 跨机器 | UDP |
| Reliable + 串口 | Serial + ACK/重传 |
| BestEffort + 串口 | Serial (仅 CRC) |

**兼容性矩阵**: Reliable 发布者兼容所有订阅者; BestEffort 发布者不兼容 Reliable 订阅者。

---

### 9.4 lifecycle_node.hpp -- 生命周期节点

借鉴 ROS2 Lifecycle Node，HSM 驱动的 16 状态层次结构。

**状态机**:

```
                    ┌──────────────────────────────────────────┐
                    v                                          │
  [Unconfigured] ──configure──> [Inactive] ──activate──> [Active]
        ^                           |                       |
        └────────cleanup────────────┘                       │
        │                                                   │
        └──────────────────shutdown──────────────────────────┘
        v                                                   v
  [Finalized] <─────────────────────────────────────────────┘
```

**HSM 层次结构** (16 状态):

```
Root
├── Operational
│   ├── Unconfigured
│   ├── Configuring (过渡)
│   ├── Inactive (Idle / Ready)
│   ├── Activating (过渡)
│   ├── Active (Running / Degraded)
│   ├── Deactivating (过渡)
│   └── CleaningUp (过渡)
├── Error (Recoverable / Fatal)
├── ShuttingDown (过渡)
└── Finalized
```

**核心接口**:

```cpp
template <typename PayloadVariant>
class LifecycleNode : public Node<PayloadVariant> {
  CallbackReturn Configure();    // Unconfigured -> Inactive
  CallbackReturn Activate();     // Inactive -> Active
  CallbackReturn Deactivate();   // Active -> Inactive
  CallbackReturn Cleanup();      // Inactive -> Unconfigured
  CallbackReturn Shutdown();     // Any -> Finalized
  LifecycleState CurrentState() const noexcept;

 protected:
  virtual CallbackReturn on_configure()  { return CallbackReturn::kSuccess; }
  virtual CallbackReturn on_activate()   { return CallbackReturn::kSuccess; }
  virtual CallbackReturn on_deactivate() { return CallbackReturn::kSuccess; }
  virtual CallbackReturn on_cleanup()    { return CallbackReturn::kSuccess; }
  virtual CallbackReturn on_shutdown()   { return CallbackReturn::kSuccess; }
  virtual CallbackReturn on_error(LifecycleState previous_state);
};
```

- 内置 FaultReporter: configure/activate 失败报 kHigh，cleanup 失败报 kMedium
- Degraded 子状态支持运行时降级
- Error 层次 (Recoverable/Fatal) 区分可恢复和不可恢复错误
- 与 Executor 集成: `ConfigureAll()` / `ActivateAll()` 批量生命周期管理

---

### 9.5 代码生成工具 (ospgen v2)

YAML 驱动的编译期代码生成，零运行时开销。v2 新增: standalone enum class、sizeof 断言、event-message 编译期绑定、字段描述/范围约束、Validate()、Dump()、字节序转换、packed struct、版本号、deprecated 标记。

| 类型 | YAML 定义 | 生成内容 |
|------|----------|---------|
| 消息/事件 | `defs/*.yaml` | enum class、事件枚举 (含 desc)、POD 结构体 (Validate/Dump)、`std::variant`、`static_assert` (trivially_copyable + sizeof)、EventMessage/MessageEvent 编译期绑定 |
| 节点拓扑 | `defs/topology.yaml` | `constexpr` 节点 ID/名称/订阅计数常量 (含 desc) |

```bash
python3 tools/ospgen.py --input defs/protocol_messages.yaml \
  --output generated/osp/protocol_messages.hpp
```

CMake 集成: `OSP_CODEGEN=ON` 时通过 `add_custom_command` + `DEPENDS` 增量生成。

> 详细设计见 [design_codegen_zh.md](design_codegen_zh.md)

---

## 10. 可靠性组件

### 10.1 watchdog.hpp -- 线程看门狗

newosp 共有 8 个模块创建 `std::thread`，可产生 15+ 个线程。ThreadWatchdog 提供零堆分配的线程存活检测。

**架构**:

```
platform.hpp (ThreadHeartbeat)  <--  所有模块 (Beat only)
platform.hpp (ThreadHeartbeat)  <--  watchdog.hpp (Check/Register)
watchdog.hpp  <--  应用层 (wiring)
```

```
+------------------+     Beat()      +------------------+
| Monitored Thread | -------------> | ThreadHeartbeat   |
| (timer/worker/   |  atomic store  | (in platform.hpp) |
|  executor/...)   |                +------------------+
+------------------+                        ^
                                            | read last_beat_us
        Check() tick                        |
        (from TimerScheduler        +------------------+
         or user poll)              | ThreadWatchdog   |
              |                     | <MaxThreads=32>  |
              v                     | - slots_[N]      |
     Compare last_beat_us           | - on_timeout_    |
     vs now - timeout_us            | - on_recovered_  |
                                    +------------------+
```

**核心 API**:

```cpp
template <uint32_t MaxThreads = 32>
class ThreadWatchdog {
  expected<RegResult, WatchdogError> Register(const char* name, uint32_t timeout_ms);
  expected<void, WatchdogError> Unregister(WatchdogSlotId id);
  uint32_t Check() noexcept;  // 返回超时线程数
  void SetOnTimeout(TimeoutCallback fn, void* ctx);
  void SetOnRecovered(RecoverCallback fn, void* ctx);
  void StartAutoCheck(uint32_t interval_ms);  // 备路自检线程
  void StopAutoCheck();
};
```

**RAII 守卫**: `WatchdogGuard` 构造时 Register，析构时 Unregister。

**需要集成的模块**:

| 模块 | 线程 | 建议超时 |
|------|------|----------|
| TimerScheduler | ScheduleLoop | 2x max(period_ms) |
| WorkerPool | DispatcherLoop, WorkerLoop | 2x dispatch_interval |
| Executor | DispatchLoop | 2x batch_interval |
| NodeManager | HeartbeatLoop | 2x heartbeat_interval |
| DebugShell | AcceptLoop, SessionLoop | 30s |
| Service | AcceptLoop, HandleConnection | 30s |

**与 TimerScheduler 集成**: 推荐双路驱动 -- timer 主路 (高频 500ms) + auto-check 备路 (低频 1000ms)。即使 TimerScheduler 卡死，备路仍能触发超时检测。

**资源预算**: ThreadWatchdog<32> ~2 KB; Beat() 热路径 1 次 atomic store + 1 次 clock_gettime; 额外线程 0 (可选 1 备路)。

---

### 10.2 fault_collector.hpp -- 故障收集器

从 fccu_linux_demo (C + QPC active object) 重写为 C++17 header-only 模块。

**核心特性**:

| 特性 | 实现 |
|------|------|
| 多优先级队列 | 4 级独立 MPSC RingBuffer (kCritical/kHigh/kMedium/kLow) |
| 优先级准入 | 队列 60%/80%/99% 阈值，kCritical 永不丢弃 |
| 消费者线程 | 独立线程，条件变量 + 10ms 超时唤醒 |
| Hook 决策 | kHandled / kEscalate / kDefer / kShutdown |
| 活跃位图 | atomic<uint64_t> + popcount，O(1) 查询 |
| Watchdog 集成 | SetConsumerHeartbeat(ThreadHeartbeat*) |
| 零堆分配 | 编译期模板参数，FixedFunction SBO |

```cpp
template <uint32_t MaxFaults = 64U, uint32_t QueueDepth = 256U, uint32_t HookBufSize = 32U>
class FaultCollector;
```

**资源预算**: 默认 (64, 256) ~36 KB; 精简 (16, 64) ~9.6 KB。38 test cases。

---

### 10.3 FaultReporter 注入模式

16 字节 POD 结构，作为零开销故障上报注入点:

```cpp
using FaultReportFn = void (*)(uint16_t fault_index, uint32_t detail,
                               FaultPriority priority, void* ctx);
struct FaultReporter {
  FaultReportFn fn = nullptr;
  void* ctx = nullptr;
  void Report(uint16_t fault_index, uint32_t detail,
              FaultPriority priority) const noexcept {
    if (fn != nullptr) { fn(fault_index, detail, priority, ctx); }
  }
};
```

**内置 vs 可选注入**:

| 模块 | 方式 | 自动上报场景 |
|------|------|-------------|
| lifecycle_node.hpp | 内置 | configure/activate/deactivate/cleanup 失败 |
| node_manager_hsm.hpp | 内置 | 心跳超时 (kHigh)、断连 (kCritical) |
| service_hsm.hpp | 可选 `SetFaultReporter()` | Error 状态入口 |
| discovery_hsm.hpp | 可选 `SetFaultReporter()` | Degraded 状态入口 |

核心模块内置 FaultReporter 是因为它们是故障关键路径; 服务模块可选注入保持灵活性。`fn == nullptr` 时 `Report()` 为空操作，零运行时开销。

---

### 10.4 Watchdog + FaultCollector 集成模式

两个模块职责正交，组合后形成完整的故障检测-收集-处理链路:

| 模块 | 职责 | 检测范围 |
|------|------|----------|
| ThreadWatchdog | 发现问题 | 线程挂死、死锁、消费者线程卡住 |
| FaultCollector | 处理问题 | 业务级故障分级上报、hook 决策、统计与背压 |

```mermaid
graph LR
    WD[ThreadWatchdog] -->|timeout 回调| APP[应用层 wiring]
    APP -->|ReportFault| FC[FaultCollector]
    FC -->|Beat| HB[ThreadHeartbeat]
    WD -->|Check last_beat_us| HB
```

**设计原则**:
- 不把 Watchdog 嵌入 FaultCollector 内部 (违反单一职责)
- 不创建 FaultWatchdogAdapter 抽象层 (没有产品需求支撑)
- 不给 Watchdog 加 FaultCollector 依赖 (保持依赖方向简洁)

**应用层集成示例**:

```cpp
struct FaultSystem {
  osp::ThreadWatchdog<32> watchdog;
  osp::FaultCollector<64, 256> collector;

  void Init() {
    // Watchdog 超时 -> 上报故障到 FaultCollector
    watchdog.SetOnTimeout([](uint32_t slot_id, const char*, void* ctx) {
      auto* fc = static_cast<osp::FaultCollector<64, 256>*>(ctx);
      fc->ReportFault(kFaultThreadDeath, slot_id, osp::FaultPriority::kCritical);
    }, &collector);

    // FaultCollector 消费者线程也被 Watchdog 监控
    auto reg = watchdog.Register("fault_consumer", 5000);
    if (reg.has_value()) {
      collector.SetConsumerHeartbeat(reg.value().heartbeat);
    }

    // 双路驱动
    watchdog.StartAutoCheck(1000);
    collector.Start();
  }
};
```

### 10.5 shell_commands.hpp -- 内置诊断 Shell 命令

零侵入桥接文件，将各模块运行时状态暴露为 telnet shell 命令。模块本身不依赖 shell.hpp。

**依赖方向**:

```
shell.hpp (OSP_SHELL_CMD, DebugShell::Printf)
    ^
    |
shell_commands.hpp (桥接层，按需 include)
    ^
    |
各模块头文件 (bus.hpp, watchdog.hpp, ...)
```

**命令列表**:

| 命令 | 注册函数 | 模块 | 输出内容 |
|------|---------|------|----------|
| `osp_watchdog` | `RegisterWatchdog()` | watchdog.hpp | slot 状态、超时配置、最后心跳 |
| `osp_faults` | `RegisterFaults()` | fault_collector.hpp | 统计、队列使用率、最近故障 |
| `osp_bus` | `RegisterBusStats()` | bus.hpp | 发布/丢弃/背压统计 |
| `osp_pool` | `RegisterWorkerPool()` | worker_pool.hpp | 分发/处理/队列满统计 |
| `osp_transport` | `RegisterTransport()` | transport.hpp | 丢包/重排/重复统计 |
| `osp_serial` | `RegisterSerial()` | serial_transport.hpp | 帧/字节/错误全量统计 |
| `osp_nodes` | `RegisterHsmNodes()` | node_manager_hsm.hpp | HSM 节点状态、心跳、丢失计数 |
| `osp_nodes_basic` | `RegisterNodeManager()` | node_manager.hpp | 基础节点连接状态 |
| `osp_service` | `RegisterServiceHsm()` | service_hsm.hpp | 服务 HSM 状态 |
| `osp_discovery` | `RegisterDiscoveryHsm()` | discovery_hsm.hpp | 发现 HSM 状态、丢失节点数 |
| `osp_lifecycle` | `RegisterLifecycle()` | lifecycle_node.hpp | 生命周期状态 |
| `osp_qos` | `RegisterQos()` | qos.hpp | QoS 配置各字段 |
| `osp_mempool` | `RegisterMemPool()` | mem_pool.hpp | 容量/已用/空闲 |

**实现模式**: 模板函数 + 静态局部变量捕获对象指针，编译期绑定，无虚函数:

```cpp
template <typename WatchdogType>
inline void RegisterWatchdog(WatchdogType& wd) {
  static WatchdogType* s_wd = &wd;
  static auto cmd = [](int, char*[]) -> int {
    // 使用 s_wd 访问模块状态，DebugShell::Printf 输出
    return 0;
  };
  osp::detail::GlobalCmdRegistry::Instance().Register(
      "osp_watchdog", +cmd, "Show thread watchdog status");
}
```

**新增诊断查询接口** (为 shell_commands 补充):

| 模块 | 新增接口 | 说明 |
|------|---------|------|
| watchdog.hpp | `WatchdogSlotInfo` + `ForEachSlot(Fn)` | 遍历活跃 slot 快照 |
| fault_collector.hpp | `QueueUsageInfo` + `QueueUsage(pri)` | 单队列使用率 |
| fault_collector.hpp | `RecentFaultInfo` + `ForEachRecent(Fn, max)` | 遍历最近 N 条故障 |
| node_manager_hsm.hpp | `HsmNodeInfo` + `ForEachNode(Fn)` | 遍历活跃节点快照 |

**资源开销**: 每个 Register 函数 1 个 static 指针 (8B)，每个命令占用 GlobalCmdRegistry 1 个 slot，运行时仅在 telnet 执行命令时触发。

---

### 10.6 system_monitor.hpp -- 系统健康监控

Linux 专用系统健康监控模块 (`#if defined(OSP_PLATFORM_LINUX)`)，通过 /proc 和 /sys 文件系统采集 CPU、内存、磁盘、温度指标。

**设计要点**:

| 特性 | 实现 |
|------|------|
| 数据采集 | POSIX open/read/close 解析 /proc/stat, /proc/meminfo, /sys/class/thermal |
| CPU 利用率 | jiffies 差分计算 (需两次采样) |
| 告警模式 | 状态变化检测 (StateCrossed)，仅在跨越阈值时触发 |
| 回调类型 | 函数指针 `AlertCallback = void(*)(const SystemSnapshot&, const char*, void*)` |
| 磁盘监控 | statvfs() 系统调用，模板参数 MaxDiskPaths (默认 4) |
| 零堆分配 | 栈缓冲区解析，POD 快照结构体 |
| Timer 集成 | 静态 `SampleTick(void*)` 方法 |

**数据结构**:

```
CpuSnapshot     -- total/user/system/iowait 百分比 + 温度 (milli-Celsius)
MemorySnapshot  -- total/available/used (kB) + 百分比
DiskSnapshot    -- total/available (bytes) + 百分比
SystemSnapshot  -- CpuSnapshot + MemorySnapshot + timestamp_us
AlertThresholds -- cpu/temp/memory/disk 阈值配置
```

**典型用法**:

```cpp
osp::SystemMonitor<4> monitor;

// 配置阈值
osp::AlertThresholds thresholds;
thresholds.cpu_percent = 85;
thresholds.memory_percent = 90;
monitor.SetThresholds(thresholds);

// 设置告警回调
monitor.SetAlertCallback([](const osp::SystemSnapshot& snap,
                            const char* msg, void*) {
  fprintf(stderr, "ALERT: %s\n", msg);
}, nullptr);

// 添加磁盘监控路径
monitor.AddDiskPath("/");
monitor.AddDiskPath("/tmp");

// 定时采样 (1 秒间隔)
scheduler.AddTimer(1000, osp::SystemMonitor<4>::SampleTick, &monitor);
```

**与 guardian 项目的关系**: 从 guardian 守护进程的 SystemCheck 模块提炼而来，去除了产品硬编码和全局状态，改为模板化 header-only 设计，符合 newosp 零堆分配和函数指针回调的架构风格。

---

### 10.7 process.hpp -- 进程管理

Linux 专用进程管理模块 (`#if defined(OSP_PLATFORM_LINUX)`)，提供进程发现、控制、子进程 spawn 和管道输出捕获。

**设计要点**:

| 特性 | 实现 |
|------|------|
| 进程发现 | /proc/[pid]/comm 精确匹配 + /proc/[pid]/cmdline basename 回退 |
| 进程控制 | SIGSTOP/SIGCONT/SIGKILL + 重试机制 |
| 状态检查 | /proc/[pid]/status 解析 State 字段 (R/S/D/T/Z) |
| 子进程 spawn | fork + execvp, setsid 信号隔离, 信号重置 |
| 管道捕获 | stdout/stderr 独立或合并捕获, 非阻塞读取 |
| 超时等待 | waitpid + WNOHANG 轮询, 可配置超时 |
| RAII | Subprocess 析构自动 kill + waitpid, PipeGuard/DirGuard |
| 零 std::regex | 手写 /proc 解析, 无正则引擎开销 |
| 便捷接口 | RunCommand() 一行调用捕获命令输出 |

**核心类型**:

```
ProcessResult    -- kSuccess/kNotFound/kFailed/kWaitError
SubprocessConfig -- argv, working_dir, capture_stdout/stderr, merge_stderr
WaitResult       -- exited, exit_code, signaled, term_signal, timed_out
Subprocess       -- RAII 子进程句柄 (Start/Wait/Signal/ReadStdout/ReadStderr)
```

**典型用法**:

```cpp
// 1. 查找并冻结进程
pid_t pid;
osp::FindPidByName("my_daemon", pid);
osp::FreezeProcess(pid);
// ... 资源调度 ...
osp::ResumeProcess(pid);

// 2. 子进程 spawn + 输出捕获
const char* argv[] = {"ls", "-la", nullptr};
osp::Subprocess proc;
osp::SubprocessConfig cfg;
cfg.argv = argv;
cfg.capture_stdout = true;
proc.Start(cfg);
std::string output = proc.ReadAllStdout();
auto wr = proc.Wait();

// 3. 一行命令执行
std::string out; int code;
osp::RunCommand(argv, out, code);
```

**TOCTOU 说明**: FindPidByName 返回的 PID 可能在查找后失效 (进程退出或 PID 复用)。Linux 5.1+ 可用 pidfd_open(2) 消除此竞态，当前版本通过文档标注提醒调用者。

**灵感来源**: reproc (跨平台子进程库)、subprocess.h (单头文件 C 库)、TinyProcessLibrary。

---

## 11. 跨模块交互

### 11.1 典型启动序列

```cpp
// 1. 加载配置
osp::MultiConfig cfg;
cfg.LoadFile("app.yaml");

// 2. 初始化日志
osp::log::SetLevel(osp::log::Level::kInfo);

// 3. 启动定时器
osp::TimerScheduler timer(16);
timer.Add(1000, heartbeat_fn, nullptr);
timer.Start();

// 4. 启动调试 Shell
osp::DebugShell shell({.port = cfg.GetInt("shell", "port", 5090)});
shell.Start();

// 5. 创建通信节点
osp::Node<Payload> controller("ctrl", 1);
controller.Subscribe<SensorData>(on_sensor);
controller.Start();

// 6. 启动工作线程池
osp::WorkerPoolConfig wp_cfg{.worker_num = 4};
osp::WorkerPool<Payload> pool(wp_cfg);
pool.RegisterHandler<MotorCmd>(execute_motor);
pool.Start();

// 7. 注册关停
osp::ShutdownManager shutdown;
shutdown.Register([](int) { /* cleanup */ });
shutdown.InstallSignalHandlers();
shutdown.WaitForShutdown();
```

### 11.2 消息流转链路

```
Timer 回调 ──> Node::Publish(SensorData)
                    |
         AsyncBus (MPSC, 无锁)
                    |
         Node::SpinOnce() / WorkerPool::DispatcherThread
                    |
         ┌──────────┼──────────┐
         v          v          v
    Subscriber   Worker[0]  Worker[1]
    (回调函数)   (SPSC)     (SPSC)
```

### 11.3 错误传播路径

```
底层操作失败
    |
    v
expected<V, E> 返回
    |
    ├── .has_value() 检查 ──> and_then() 链式处理
    |
    └── .get_error() 获取错误码
            |
            v
        OSP_LOG_ERROR() 记录
            |
            v
        上层策略决定 (重试 / 降级 / 关停)
            |
            v
        FaultReporter.Report() (可选，自动上报到 FaultCollector)
```

---

## 12. 资源预算

### 12.1 已实现模块

| 模块 | 静态内存 | 堆内存 | 线程数 |
|------|----------|--------|--------|
| Config (128 entries) | ~48 KB | 0 | 0 |
| TimerScheduler(16) | ~1 KB | ~1 KB | 1 |
| DebugShell(2 conn) | ~9 KB | ~4 KB | 3 |
| FixedPool<256,64> | 16 KB | 0 | 0 |
| ShutdownManager | <256 B | 0 | 0 |
| AsyncBus(4096) | ~320 KB | 0 | 0 |
| Node (per node) | ~1 KB | 0 | 0 |
| WorkerPool(4 workers) | ~64 KB | ~16 KB | 5 |
| StateMachine<Ctx,16> | ~1 KB | 0 | 0 |
| BehaviorTree<Ctx,32> | ~4 KB | 0 | 0 |
| Executor (各类型) | <1 KB | 0 | 1 |
| TcpTransport (per conn) | ~256 B | 0 | 0 |
| ShmRingBuffer (256x4KB) | ~1 MB (共享内存) | 0 | 0 |
| ShmChannel (per channel) | ~256 B | 0 | 0 |
| ShmSpscByteRing (1MB default) | ~1 MB (共享内存) | 0 | 0 |
| ShmByteChannel (per channel) | ~256 B | 0 | 0 |
| SerialTransport (per port) | ~2 KB | 0 | 0 |
| MulticastDiscovery | ~2 KB | ~1 KB | 1 |
| Service/Client (per service) | ~512 B | 0 | 0 |
| Application<Impl, 64> | ~8 KB + sizeof(Impl)*64 | 0 | 1 |
| NodeManager (64 nodes) | ~4 KB | ~2 KB | 2 |
| NodeManagerHsm (64 nodes) | ~8 KB | 0 | 1 |
| QosProfile (per topic) | ~16 B | 0 | 0 |
| LifecycleNode (per node) | ~1 KB | 0 | 0 |
| ThreadWatchdog<32> | ~2 KB | 0 | 0-1 |
| FaultCollector<64,256> | ~36 KB | 0 | 1 |
| SpscRingbuffer<T,N> | sizeof(T)*N + 128B | 0 | 0 |
| ConnectionPool<32> | ~4 KB | 0 | 0 |
| IoPoller (64 fds) | ~2 KB | <1 KB | 0 |
| LightSemaphore | <128 B | 0 | 0 |
| TransportFactory | 0 (无状态) | 0 | 0 |
| AsyncLog (8 threads) | ~652 KB | 0 | 1 |

> 实测性能数据 (吞吐、延迟、sizeof、运行时 RSS) 见 [benchmark_report_zh.md](benchmark_report_zh.md)

---

## 13. 编码规范与质量保障

### 13.1 代码风格

基于 Google C++ Style Guide，使用 `.clang-format` 和 `CPPLINT.cfg`:

- 缩进 2 空格，行宽 120，Attach 花括号
- 指针左对齐 (`int* ptr`)，命名空间不缩进
- Include 排序: 主头文件 > 项目头文件 > C 封装 > C++ 标准库

### 13.2 命名约定

| 元素 | 风格 | 示例 |
|------|------|------|
| 类/结构体 | PascalCase | `AsyncBus`, `TimerScheduler` |
| 函数 (公有) | PascalCase | `Publish()`, `ProcessBatch()` |
| 变量/成员 | snake_case / snake_case_ | `sender_id`, `running_` |
| 常量/枚举值 | kPascalCase | `kCacheLineSize`, `kSuccess` |
| HSM 事件枚举 | k\<Module\>Evt\<Name\> | `kSvcEvtStart`, `kDiscEvtNodeFound` |
| 宏 | OSP_UPPER_CASE | `OSP_LOG_INFO`, `OSP_ASSERT` |
| 模板参数 | PascalCase | `PayloadVariant`, `BlockSize` |

### 13.3 CI 流水线

| 阶段 | 内容 |
|------|------|
| build-and-test | Ubuntu, Debug + Release |
| build-with-options | `-fno-exceptions -fno-rtti` 兼容性 |
| sanitizers | ASan, TSan, UBSan |
| code-quality | clang-format (v21) + cpplint + clang-tidy |

### 13.4 测试策略

- 框架: Catch2 v3.5.2
- 每模块独立测试文件: `test_<module>.cpp`
- 覆盖目标: 基础 API + 边界条件 + 多线程场景
- Sanitizer 验证: 所有测试在 ASan/TSan/UBSan 下通过
- 当前: 1153 test cases (全模块), 798 test cases (无异常模式)
- 构建模式: 正常模式 (全模块) / 无网络模式 (`-DOSP_WITH_NETWORK=OFF`) / 无异常模式 (`-DOSP_NO_EXCEPTIONS=ON`)

---

## 14. 附录

### 14.1 借鉴的设计模式来源

| 模式 | 来源 | 应用模块 |
|------|------|----------|
| 标签分发 + 模板特化 | -- | config.hpp 后端选择 |
| 变参模板 + if constexpr | -- | Config<Backends...> 编译期组合 |
| CRTP | -- | Shell 命令扩展 |
| SBO 回调 | iceoryx | FixedFunction (bus/worker_pool/shell/fault_collector 全覆盖，零 std::function) |
| 无锁 MPSC 环形缓冲 | LMAX Disruptor | AsyncBus |
| 优先级准入控制 | MCCC | AsyncBus 背压 |
| SpinLock + 指数退避 | eventpp | 低延迟锁 |
| 批量预取 | eventpp | 回调遍历优化 |
| SharedMutex 读写分离 | eventpp | 订阅管理 |
| LCA 层次转换 | hsm-cpp | 状态机 entry/exit 路径 |
| 位图并行追踪 | bt-cpp | 行为树 Parallel 节点 |
| ROS2 CallbackGroup | ROS2 rclcpp | Executor 分组调度 |
| CyberRT DataFusion | CyberRT | FusedSubscription 多源对齐 |
| 二级排队 (MPSC+SPSC) | consumer-producer | WorkerPool |
| Per-Thread SPSC 异步日志 | spdlog + 自研 | async_log.hpp wait-free 热路径 |
| 共享内存无锁队列 | cpp-ipc | ShmRingBuffer 进程间通信 |
| 零拷贝 LoanedMessage | ROS2 Fast DDS | ShmTransport 共享内存发布 |
| 自动传输选择 | CyberRT | TransportFactory 路由策略 |
| eventfd 轻量通知 | Linux kernel | ShmChannel 消费者唤醒 |
| Service/Client RPC | ROS2 rclcpp | service.hpp 请求-响应 |
| UDP 多播发现 | ROS2 DDS | discovery.hpp 节点自动发现 |
| HSM 驱动实例生命周期 | hsm-cpp + 原始 OSP | app.hpp Instance 11 状态, BeginMessage/EndMessage |
| HSM 驱动生命周期节点 | ROS2 Lifecycle + hsm-cpp | lifecycle_node.hpp 16 状态 |
| App-Instance 两层模型 | 原始 OSP | app.hpp 编译期分发 + ObjectPool 零堆分配 |
| 全局实例 ID (IID) | 原始 OSP | post.hpp MAKEIID 寻址 |
| NATIVE_SYNC 串口 | CSerialPort | serial_transport.hpp |
| 帧同步状态机 + CRC16 | 工业通信协议 | serial_transport.hpp |
| HSM 驱动生命周期 | ROS2 + hsm-cpp | service_hsm/discovery_hsm/node_manager_hsm |
| YAML 编译期代码生成 | ROS2 msg/srv codegen | ospgen.py |
| FaultReporter POD 注入 | 内部演进 | fault_collector.hpp + 上层模块 |

### 14.2 编译期配置汇总

| 宏 | 默认值 | 模块 | 说明 |
|----|--------|------|------|
| `OSP_HAS_NETWORK` | 自动检测 | platform.hpp | 网络可用性 (`__has_include(<sys/socket.h>)`); CMake: `-DOSP_WITH_NETWORK=OFF` 强制关闭 |
| `OSP_LOG_MIN_LEVEL` | 0/1 | log.hpp | 编译期日志级别 |
| `OSP_ASYNC_LOG_QUEUE_DEPTH` | 256 | async_log.hpp | 每线程 SPSC 队列深度 |
| `OSP_ASYNC_LOG_MAX_THREADS` | 8 | async_log.hpp | 最大并发日志线程数 |
| `OSP_ASYNC_LOG_DROP_REPORT_INTERVAL_S` | 10 | async_log.hpp | 丢弃统计上报间隔 (秒) |
| `OSP_LOG_SYNC_ONLY` | 未定义 | async_log.hpp | 禁用异步路径 |
| `OSP_CONFIG_MAX_FILE_SIZE` | 8192 | config.hpp | 最大配置文件大小 |
| `OSP_BUS_QUEUE_DEPTH` | 4096 | bus.hpp | 环形缓冲区大小 |
| `OSP_BUS_MAX_MESSAGE_TYPES` | 8 | bus.hpp | 最大消息类型数 |
| `OSP_BUS_MAX_CALLBACKS_PER_TYPE` | 16 | bus.hpp | 每类型最大订阅 |
| `OSP_BUS_BATCH_SIZE` | 256 | bus.hpp | 单次批量处理上限 |
| `OSP_MAX_NODE_SUBSCRIPTIONS` | 16 | node.hpp | 每节点最大订阅 |
| `OSP_MAX_STATIC_NODE_SUBSCRIPTIONS` | 16 | static_node.hpp | StaticNode 最大订阅 |
| `OSP_IO_POLLER_MAX_EVENTS` | 64 | io_poller.hpp | 单次 Wait 最大事件数 |
| `OSP_BT_MAX_NODES` | 32 | bt.hpp | 行为树最大节点数 |
| `OSP_BT_MAX_CHILDREN` | 8 | bt.hpp | 复合节点最大子节点数 |
| `OSP_EXECUTOR_MAX_NODES` | 16 | executor.hpp | Executor 最大节点数 |
| `OSP_TRANSPORT_MAX_FRAME_SIZE` | 4096 | transport.hpp | 最大帧大小 |
| `OSP_CONNECTION_POOL_CAPACITY` | 32 | connection.hpp | 连接池默认容量 |
| `OSP_SHM_SLOT_SIZE` | 4096 | shm_transport.hpp | MPSC 共享内存 slot 大小 |
| `OSP_SHM_SLOT_COUNT` | 256 | shm_transport.hpp | MPSC 共享内存 slot 数量 |
| `OSP_SHM_BYTE_RING_CAPACITY` | 1048576 (1 MB) | shm_transport.hpp | SPSC 字节流默认容量 |
| `OSP_SHM_HUGE_PAGES` | 未定义 | shm_transport.hpp | 定义后 mmap 使用 MAP_HUGETLB (透明回退) |
| `OSP_SHM_SPMC_MAX_CONSUMERS` | 8 | shm_transport.hpp | SPMC 环形缓冲最大消费者数 |
| `OSP_DISCOVERY_PORT` | 9999 | discovery.hpp | 多播发现端口 |
| `OSP_HEARTBEAT_INTERVAL_MS` | 1000 | node_manager.hpp | 心跳间隔 |
| `OSP_HEARTBEAT_TIMEOUT_COUNT` | 3 | node_manager.hpp | 心跳超时次数 |
| `OSP_SERIAL_FRAME_MAX_SIZE` | 1024 | serial_transport.hpp | 串口最大帧大小 |
| `OSP_SERIAL_CRC_ENABLED` | 1 | serial_transport.hpp | CRC16 校验开关 |
| `OSP_WORKER_QUEUE_DEPTH` | 1024 | worker_pool.hpp | 工作线程 SPSC 队列深度 |
| `OSP_SERIAL_RX_RING_SIZE` | 4096 | serial_transport.hpp | 串口接收环形缓冲 |
| `OSP_TRANSPORT_RECV_RING_DEPTH` | 32 | transport.hpp | 网络帧接收缓冲深度 |

### 14.3 线程安全性总结

| 模块 | 线程安全保证 |
|------|-------------|
| platform | N/A (纯宏) |
| vocabulary | 局部对象，无共享 |
| config | 加载后只读 |
| log | fprintf 原子写 |
| async_log | wait-free SPSC 生产者; 单消费者写线程; CAS 线程注册 |
| timer | mutex 保护所有公有方法 |
| shell | 注册表 mutex + 会话线程隔离 + shutdown(SHUT_RDWR) 安全关停 |
| mem_pool | mutex 保护 alloc/free |
| shutdown | 原子标志 + async-signal-safe pipe |
| bus | 无锁 MPSC + SharedMutex 订阅 |
| node | 发布线程安全; SpinOnce 单消费者 |
| worker_pool | 原子标志 + CV + SPSC 无锁 |
| spsc_ringbuffer | 单生产者单消费者 (SPSC 约定) |
| socket | 单线程使用，fd 不可共享 |
| io_poller | 单线程 (事件循环线程) |
| connection | mutex 保护连接池操作 |
| transport | TcpTransport 单线程; NetworkNode 继承 Node 线程安全性 |
| shm_transport | ShmRingBuffer 无锁 CAS (MPSC); ShmSpscByteRing 无锁 SPSC; ShmByteChannel futex 通知 |
| serial_transport | NATIVE_SYNC 单线程; IoPoller 事件循环线程安全 |
| hsm | 单线程 Dispatch; guard/handler 不可重入 |
| bt | 单线程 Tick; 叶节点回调不可重入 |
| executor | 内部线程安全; Stop 可跨线程调用 |
| data_fusion | 继承 Bus 订阅线程安全性 |
| discovery | 内部线程安全; 回调在发现线程执行 |
| service | handler 在服务线程执行; Client::Call 可跨线程 |
| app | Application::Post 线程安全 (mutex); ProcessOne 单消费者; Instance::OnMessage 单线程 |
| post | OspPost 线程安全; OspSendAndWait 阻塞调用线程 |
| node_manager | 内部 mutex 保护; 回调在心跳线程执行 |
| node_manager_hsm | mutex 保护; 每连接独立 HSM 单线程 Dispatch |
| service_hsm | mutex 保护; HSM 单线程 Dispatch |
| discovery_hsm | mutex 保护; HSM 单线程 Dispatch |
| watchdog | Register/Unregister mutex; Beat() 无锁 atomic; Check() 单线程 |
| fault_collector | ReportFault 无锁 MPSC; 消费者独立线程 |
| net | 单线程使用 (同 sockpp) |
| transport_factory | 无状态静态方法; 线程安全 |
