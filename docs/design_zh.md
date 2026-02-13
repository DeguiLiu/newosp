# newosp 架构设计文档

> 版本: 1.0
> 日期: 2026-02-13
> 状态: 活跃演进中

---

## 目录

1. [项目概述](#1-项目概述)
2. [设计目标与约束](#2-设计目标与约束)
3. [系统架构](#3-系统架构)
4. [模块详细设计](#4-模块详细设计)
5. [跨模块交互](#5-跨模块交互)
6. [扩展模块规划](#6-扩展模块规划)
7. [资源预算](#7-资源预算)
8. [编码规范与质量保障](#8-编码规范与质量保障)
9. [实施步骤](#9-实施步骤)
10. [附录](#10-附录)

---

## 1. 项目概述

### 1.1 定位

newosp 是一个面向 ARM-Linux 嵌入式平台的现代 C++17 纯头文件基础设施库。提供嵌入式系统开发所需的核心基础能力: 配置管理、日志、定时调度、远程调试、内存池、消息总线、节点通信、工作线程池。

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
- **测试**: Catch2 v3.5.2
- **代码规范**: Google C++ Style (clang-format), cpplint
- **CI**: GitHub Actions (GCC/Clang x Debug/Release, Sanitizers)

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
线程预算:    典型 4-8 线程 (定时器1 + Shell 3 + 工作线程 N)
缓存行:     64 字节 (ARM 标准)
```

### 2.3 依赖管理

| 依赖 | 版本 | 用途 | 条件 |
|------|------|------|------|
| inih | r58 | INI 配置解析 | `OSP_CONFIG_INI=ON` |
| nlohmann/json | v3.11.3 | JSON 配置解析 | `OSP_CONFIG_JSON=ON` |
| fkYAML | v0.4.0 | YAML 配置解析 | `OSP_CONFIG_YAML=ON` |
| Catch2 | v3.5.2 | 单元测试 | `OSP_BUILD_TESTS=ON` |

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
    |  |     Network Transport Layer         | |
    |  |  NetworkNode (Pub/Sub 透明传输)     | |
    |  |  Transport (TCP/UDP/Unix/Inproc)    | |
    |  |  IoPoller (epoll/kqueue)            | |
    |  └──────────────┬──────────────────────┘ |
    |                 |                        |
    |  ┌──────────────┴──────────────────────┐ |
    |  |     Communication Layer             | |
    |  |  Node (Pub/Sub) + WorkerPool        | |
    |  |  AsyncBus (MPSC) + SPSC Queue       | |
    |  └──────────────┬──────────────────────┘ |
    |                 |                        |
    |  ┌──────────────┴──────────────────────┐ |
    |  |     Service Layer                   | |
    |  |  Config + Log + Timer + Shell       | |
    |  |  MemPool + Shutdown                 | |
    |  └──────────────┬──────────────────────┘ |
    |                 |                        |
    |  ┌──────────────┴──────────────────────┐ |
    |  |     Foundation Layer                | |
    |  |  Platform + Vocabulary              | |
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
    ├── timer.hpp      (依赖 vocabulary: expected, NewType)
    ├── shell.hpp      (依赖 vocabulary: function_ref, FixedFunction, expected)
    ├── mem_pool.hpp   (依赖 vocabulary: expected)
    ├── shutdown.hpp   (依赖 vocabulary: expected)
    |
    ├── bus.hpp        (依赖 platform: kCacheLineSize)
    ├── node.hpp       (依赖 vocabulary + bus.hpp)
    └── worker_pool.hpp (依赖 vocabulary + bus.hpp)
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
    │ SPSC Queue │  │ SPSC Queue │
    └────────────┘  └────────────┘
```

---

## 4. 模块详细设计

### 4.1 platform.hpp -- 平台检测与编译器提示

**职责**: 提供跨平台兼容的基础宏定义。

| 组件 | 说明 |
|------|------|
| `OSP_PLATFORM_LINUX/MACOS/WINDOWS` | 平台检测宏 |
| `OSP_ARCH_ARM/X86` | 架构检测宏 |
| `kCacheLineSize` | 缓存行大小常量 (64) |
| `OSP_LIKELY(x)` / `OSP_UNLIKELY(x)` | 分支预测提示 |
| `OSP_ASSERT(cond)` | 调试断言 (NDEBUG 下编译消除) |

**设计决策**: 纯宏实现，零运行时开销。

---

### 4.2 vocabulary.hpp -- 词汇类型

**职责**: 提供零堆分配的基础类型，替代异常和动态容器。

#### 4.2.1 错误处理

| 类型 | 说明 |
|------|------|
| `expected<V, E>` | 轻量级 error-or-value 类型 |
| `optional<T>` | 可空值包装器 |
| `and_then(exp, F)` | 单子链式调用 |
| `or_else(exp, F)` | 错误回调链 |

#### 4.2.2 固定容量容器

| 类型 | 说明 |
|------|------|
| `FixedVector<T, Cap>` | 栈分配定长向量 |
| `FixedString<Cap>` | 固定容量字符串 |
| `FixedFunction<Sig, BufSize>` | SBO 回调 (默认 2*sizeof(void*)) |
| `function_ref<Sig>` | 非拥有型函数引用 (2 指针) |

#### 4.2.3 类型安全工具

| 类型 | 说明 |
|------|------|
| `not_null<T>` | 非空指针包装 |
| `NewType<T, Tag>` | 强类型标签包装 |
| `ScopeGuard` | RAII 作用域守卫 |
| `OSP_SCOPE_EXIT(...)` | 作用域退出宏 |

#### 4.2.4 错误枚举

```cpp
enum class ConfigError : uint8_t { kFileNotFound, kParseError, kFormatNotSupported, kBufferFull };
enum class TimerError  : uint8_t { kSlotsFull, kInvalidPeriod, kNotRunning, kAlreadyRunning };
enum class ShellError  : uint8_t { kRegistryFull, kDuplicateName, kPortInUse, kNotRunning };
enum class MemPoolError: uint8_t { kPoolExhausted, kInvalidPointer };
```

**设计决策**:
- SBO 回调避免 `std::function` 的堆分配
- `expected<V,E>` 替代异常，保证 `-fno-exceptions` 兼容
- 所有容器容量在编译期确定，零动态分配

---

### 4.3 config.hpp -- 模板化多格式配置

**职责**: 统一接口解析 INI/JSON/YAML 配置文件。

**架构**:

```
┌──────────────┐
│ Config<Bs..> │ ◄── 用户接口 (多格式)
│ (variadic)   │
└──────┬───────┘
       │ if constexpr 分发
       v
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ ConfigParser │  │ ConfigParser │  │ ConfigParser │
│ <IniBackend> │  │ <JsonBackend>│  │ <YamlBackend>│
└──────┬───────┘  └──────┬───────┘  └──────┬───────┘
       │                 │                 │
       v                 v                 v
     inih         nlohmann/json         fkYAML
```

**关键接口**:

```cpp
osp::MultiConfig cfg;
auto r = cfg.LoadFile("app.yaml");  // expected<void, ConfigError>
int32_t port = cfg.GetInt("network", "port", 8080);
auto opt = cfg.FindBool("debug", "verbose");  // optional<bool>
```

**设计模式**: 标签分发 + 模板特化，编译期选择后端，零运行时多态开销。

**编译期开关**: `OSP_CONFIG_INI_ENABLED`, `OSP_CONFIG_JSON_ENABLED`, `OSP_CONFIG_YAML_ENABLED`

---

### 4.4 log.hpp -- 轻量日志

**职责**: printf 风格线程安全日志，支持编译期和运行时级别过滤。

**级别**: `kDebug(0) < kInfo(1) < kWarn(2) < kError(3) < kFatal(4) < kOff(5)`

**接口**:

```cpp
osp::log::SetLevel(osp::log::Level::kInfo);
OSP_LOG_DEBUG("sensor", "raw value: %d", raw);     // 编译期/运行时过滤
OSP_LOG_INFO("sensor",  "temp=%.1f from %u", t, id);
OSP_LOG_ERROR("bus",    "publish failed: queue full");
OSP_LOG_FATAL("main",   "unrecoverable error");     // 输出后 abort()
```

**输出格式**: `[2026-02-13 14:30:00.123] [INFO] [sensor] temp=25.0 from 1 [sensor.cpp:42]`

**设计决策**:
- stderr 后端，零依赖
- `OSP_LOG_MIN_LEVEL` 编译期裁剪 (Release 默认 kInfo)
- Debug 构建包含文件名:行号，Release 省略
- 栈缓冲区 (ts 32B + msg 512B)，零堆分配
- POSIX `fprintf` 保证原子写

---

### 4.5 timer.hpp -- 定时调度器

**职责**: 后台线程驱动的周期任务调度。

**接口**:

```cpp
osp::TimerScheduler scheduler(16);  // 最多 16 个任务
auto r = scheduler.Add(1000, heartbeat_fn, ctx);  // 1000ms 周期
// r: expected<TimerTaskId, TimerError>
scheduler.Start();
// ...
scheduler.Remove(r.value());
scheduler.Stop();
```

**关键实现**:
- `std::chrono::steady_clock` 单调时钟
- 预分配任务槽数组 (无动态扩容)
- 自动处理错过的周期 (追赶逻辑)
- 后台线程 1-10ms sleep 间隔
- `TimerTaskId` 基于 `NewType<uint32_t, Tag>` 的强类型 ID

---

### 4.6 shell.hpp -- 远程调试 Shell

**职责**: telnet 远程调试接口，支持命令注册、TAB 补全、历史记录。

**架构**:

```
┌──────────────┐  telnet 5090
│ DebugShell   │◄─────────────── 客户端
│ (TCP Server) │
└──────┬───────┘
       │ 会话线程
       v
┌──────────────┐
│ GlobalCmd    │
│ Registry     │  ◄── OSP_SHELL_CMD(name, desc) 静态注册
│ (Meyer单例)  │
└──────────────┘
```

**命令注册**:

```cpp
OSP_SHELL_CMD(stats, "Show system statistics") {
    osp::DebugShell::Printf("uptime: %u seconds\n", get_uptime());
    return 0;
}
```

**设计决策**:
- 原始 POSIX socket (无外部依赖)
- Meyer's 单例命令注册表 (最多 64 个命令)
- 线程局部 session 指针路由 `Printf()` 输出
- 128 字节行缓冲，支持退格、TAB 补全

---

### 4.7 mem_pool.hpp -- 固定块内存池

**职责**: O(1) 分配/释放的嵌入式内存池。

**两级抽象**:

| 层级 | 类型 | 说明 |
|------|------|------|
| 底层 | `FixedPool<BlockSize, MaxBlocks>` | 原始块分配，嵌入式空闲链表 |
| 上层 | `ObjectPool<T, MaxObjects>` | 类型安全包装，placement new/delete |

**接口**:

```cpp
osp::ObjectPool<Packet, 256> pool;
auto r = pool.CreateChecked(args...);  // expected<Packet*, MemPoolError>
pool.Destroy(ptr);
```

**设计决策**:
- 内联存储 (全栈分配)
- `std::memcpy` 替代 `reinterpret_cast` 避免严格别名违规
- `std::max_align_t` 对齐
- `mutex` 保护线程安全

---

### 4.8 shutdown.hpp -- 优雅关停

**职责**: 进程信号处理与 LIFO 回调执行。

**流程**:

```
SIGINT/SIGTERM ──> 信号处理器 (写 pipe, async-signal-safe)
                          |
                          v
              WaitForShutdown() (读 pipe 阻塞)
                          |
                          v
              执行回调链 (LIFO 顺序)
                          |
                          v
                    进程退出
```

**设计决策**:
- `sigaction(2)` + `pipe(2)` 异步信号安全唤醒
- 原子标志协调
- LIFO 回调保证后注册的先执行 (嵌套清理)
- 进程唯一实例保护

---

### 4.9 bus.hpp -- 无锁 MPSC 消息总线

**职责**: 核心消息传递基础设施，支持多生产者单消费者无锁发布。

**架构**:

```
Producer 0 ─┐
Producer 1 ──┼── CAS Publish ──> ┌────────────────┐
Producer 2 ─┘                    │ Ring Buffer    │
                                 │ (4096 slots)   │
                                 │ sequence-based  │
                                 └───────┬────────┘
                                         │ ProcessBatch()
                                         v
                                 Type-Based Dispatch
                                 (std::variant + VariantIndex<T>)
```

**关键特性**:

| 特性 | 说明 |
|------|------|
| 无锁发布 | CAS 循环抢占生产者位置 |
| 优先级准入控制 | LOW 60% / MEDIUM 80% / HIGH 99% 阈值 |
| 批量处理 | `ProcessBatch()` 每轮最多 256 条消息 |
| 背压感知 | Normal/Warning/Critical/Full 四级 |
| 缓存行分离 | 生产者/消费者计数器分属不同缓存行 |

**编译期配置**:

```cpp
#define OSP_BUS_QUEUE_DEPTH            4096   // 环形缓冲区大小 (2^N)
#define OSP_BUS_MAX_MESSAGE_TYPES      8      // std::variant 最大类型数
#define OSP_BUS_MAX_CALLBACKS_PER_TYPE 16     // 每类型最大订阅数
#define OSP_BUS_BATCH_SIZE             256    // 单次处理批量上限
```

**设计模式** (借鉴 eventpp):
- 序列号环形缓冲区 (类似 Disruptor)
- 准入控制带缓存消费者位置 recheck (减少 atomic load)
- `SharedMutex` 保护回调注册 (读写分离)

---

### 4.10 node.hpp -- 轻量 Pub/Sub 节点

**职责**: 受 ROS2/CyberRT 启发的轻量节点通信抽象。

**接口**:

```cpp
struct SensorData { float temp; };
struct MotorCmd { int speed; };
using Payload = std::variant<SensorData, MotorCmd>;

osp::Node<Payload> sensor("sensor", 1);
sensor.Subscribe<SensorData>([](const SensorData& d, const osp::MessageHeader& h) {
    OSP_LOG_INFO("sensor", "temp=%.1f from sender %u", d.temp, h.sender_id);
});
sensor.Publish(SensorData{25.0f});
sensor.SpinOnce();  // 消费总线中的消息
```

**设计决策**:
- 基于类型路由 (非主题路由)，编译期确定分发
- RAII 自动退订 (析构时清理所有订阅)
- 全局单例总线 (同一 `PayloadVariant` 共享)
- `Publisher<T>` 轻量发布器 (无 RAII 清理)

---

### 4.11 worker_pool.hpp -- 多工作线程池

**职责**: 基于 AsyncBus + SPSC 队列的多工作线程池。

**架构**:

```
Submit(job) ──> AsyncBus::Publish()  [无锁 MPSC]
                       |
               DispatcherThread  (ProcessBatch 循环)
                       |  round-robin
            ┌──────────┼──────────┐
            v          v          v
      Worker[0]    Worker[1]  Worker[N-1]
      SPSC Queue   SPSC Queue SPSC Queue  [无锁 SPSC]
            |          |          |
      HandlerFn   HandlerFn   HandlerFn
```

**接口**:

```cpp
osp::WorkerPoolConfig cfg{.name = "processor", .worker_num = 4};
osp::WorkerPool<Payload> pool(cfg);
pool.RegisterHandler<SensorData>(process_sensor);
pool.RegisterHandler<MotorCmd>(process_motor);
pool.Start();
pool.Submit(SensorData{25.0f});
pool.Shutdown();
```

**关键特性**:
- 二级排队: MPSC 总线 (入口) + SPSC 每工作线程队列 (分发)
- 类型擦除分发 (函数指针 + memcpy)
- `FlushAndPause()` / `Resume()` 支持优雅排空
- Linux CPU 亲和性 + 线程优先级
- 统计: dispatched, processed, worker_queue_full

---

## 5. 跨模块交互

### 5.1 典型启动序列

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

### 5.2 消息流转链路

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

### 5.3 错误传播路径

```
底层操作失败
    |
    v
expected<V, E> 返回
    |
    ├── 调用者 .has_value() 检查
    |       |
    |       v
    |   and_then() 链式处理
    |
    └── 调用者 .get_error() 获取错误码
            |
            v
        OSP_LOG_ERROR() 记录
            |
            v
        上层策略决定 (重试 / 降级 / 关停)
```

---

## 6. 扩展模块规划

### 6.1 Socket 抽象层 (P0, socket.hpp)

**目标**: RAII Socket 包装，替代裸 POSIX fd 操作。

```cpp
namespace osp {
class TcpSocket {
 public:
  static expected<TcpSocket, SocketError> Connect(const char* host, uint16_t port);
  expected<size_t, SocketError> Send(const void* data, size_t len);
  expected<size_t, SocketError> Recv(void* buf, size_t len);
  void Close() noexcept;
  int fd() const noexcept;
  // Non-copyable, movable
};

class UdpSocket { /* similar */ };
class TcpListener { /* accept loop */ };
}  // namespace osp
```

### 6.2 I/O 多路复用 (P0, io_poller.hpp)

**目标**: epoll/kqueue 统一抽象。

```cpp
namespace osp {
class IoPoller {
 public:
  expected<void, IoError> Add(int fd, uint32_t events, void* ctx);
  expected<void, IoError> Modify(int fd, uint32_t events);
  expected<void, IoError> Remove(int fd);
  expected<uint32_t, IoError> Wait(IoEvent* events, uint32_t max, int timeout_ms);
};
}  // namespace osp
```

### 6.3 连接池 (P1, connection.hpp)

**目标**: 固定容量连接管理。

```cpp
namespace osp {
template <uint32_t MaxConns>
class ConnectionPool {
 public:
  expected<ConnectionId, ConnError> Add(TcpSocket&& sock);
  expected<void, ConnError> Remove(ConnectionId id);
  void ForEach(function_ref<void(ConnectionId, TcpSocket&)> visitor);
};
}  // namespace osp
```

### 6.4 透明网络传输层 (P0, transport.hpp)

**目标**: 使 Node 屏蔽本地进程和远程进程的差异，类似 ZeroMQ 的透明传输模型。应用层代码使用统一的 `Publish()` / `Subscribe()` 接口，无需关心消息目标是本地 AsyncBus 还是远程节点。

**设计理念** (借鉴 ZeroMQ / ROS2 / CyberRT):

| 特性 | 说明 |
|------|------|
| 位置透明 | 本地/远程节点使用相同的 Pub/Sub API |
| 传输可插拔 | inproc (本地 AsyncBus) / tcp / udp / unix domain socket |
| 零拷贝本地路径 | 本地通信走 AsyncBus，不经过序列化 |
| 自动发现 | 多播或配置文件驱动的节点发现 |
| 序列化可定制 | 默认 memcpy (POD 类型)，可扩展 protobuf/flatbuffers |

**架构**:

```
                        Application Layer
                              |
                    Node<Payload>::Publish(msg)
                              |
                    ┌─────────┴─────────┐
                    |                   |
              LocalTransport       RemoteTransport
              (AsyncBus, zero-copy) (TCP/UDP + serialize)
                    |                   |
              本地 Subscriber      网络 I/O 线程
                                        |
                                  IoPoller (epoll/kqueue)
                                        |
                              ┌─────────┴─────────┐
                              |                   |
                         TcpTransport        UdpTransport
                         (可靠流)            (低延迟数据报)
```

**传输策略接口**:

```cpp
namespace osp {

// 传输层基础类型
enum class TransportType : uint8_t { kInproc, kTcp, kUdp, kUnix };

struct Endpoint {
  TransportType type;
  char address[64];   // e.g., "tcp://192.168.1.100:9000"
  uint16_t port;
};

// 消息序列化策略 (模板特化扩展)
template <typename T>
struct Serializer {
  // 默认: POD 类型直接 memcpy
  static uint32_t Serialize(const T& msg, void* buf, uint32_t buf_size) {
    static_assert(std::is_trivially_copyable_v<T>, "POD types use memcpy");
    std::memcpy(buf, &msg, sizeof(T));
    return sizeof(T);
  }
  static bool Deserialize(const void* buf, uint32_t size, T& msg) {
    if (size < sizeof(T)) return false;
    std::memcpy(&msg, buf, sizeof(T));
    return true;
  }
};

// 传输层抽象
template <typename PayloadVariant>
class Transport {
 public:
  // 绑定端点 (作为 Publisher 侧)
  expected<void, TransportError> Bind(const Endpoint& ep);

  // 连接端点 (作为 Subscriber 侧)
  expected<void, TransportError> Connect(const Endpoint& ep);

  // 发送序列化消息
  expected<void, TransportError> Send(const void* data, uint32_t size);

  // 接收并反序列化，投递到本地 AsyncBus
  void Poll(int timeout_ms);

  void Close() noexcept;
};

// 增强版 Node -- 支持透明网络传输
template <typename PayloadVariant>
class NetworkNode : public Node<PayloadVariant> {
 public:
  // 为指定消息类型添加远程发布端点
  template <typename T>
  expected<void, TransportError> AdvertiseTo(const Endpoint& ep);

  // 为指定消息类型添加远程订阅端点
  template <typename T>
  expected<void, TransportError> SubscribeFrom(const Endpoint& ep);
};
}  // namespace osp
```

**典型使用场景**:

```cpp
// Node A (本地进程 1): 传感器节点
osp::NetworkNode<Payload> sensor("sensor", 1);
sensor.AdvertiseTo<SensorData>({"tcp://0.0.0.0:9001"});  // 远程发布
sensor.Publish(SensorData{25.0f});  // 本地 + 远程同时发布

// Node B (远程进程 2): 控制器节点
osp::NetworkNode<Payload> controller("ctrl", 2);
controller.SubscribeFrom<SensorData>({"tcp://192.168.1.100:9001"});
controller.Subscribe<SensorData>(on_sensor);  // 统一回调，不区分来源
controller.SpinOnce();  // 本地 + 远程消息统一处理
```

**消息帧格式** (网络传输):

```
┌─────────────┬─────────────┬──────────┬───────────┬──────────┐
│ magic (4B)  │ msg_len (4B)│ type (2B)│ sender(4B)│ payload  │
│ 0x4F535000  │ total bytes │ variant  │ node id   │ N bytes  │
│             │             │ index    │           │          │
└─────────────┴─────────────┴──────────┴───────────┴──────────┘
```

**实现策略**:
- Phase 1: `TcpTransport` + `Serializer<POD>` 基础实现
- Phase 2: `UdpTransport` + 分片重组
- Phase 3: 节点自动发现 (multicast)
- Phase 4: 可选 protobuf/flatbuffers 序列化后端

### 6.5 层次状态机 (P1, hsm.hpp)

**目标**: 集成 hsm-cpp，事件驱动状态管理。

```
┌──────────────────────────────────┐
│ StateMachine<Context>            │
│  ├── Off                         │
│  ├── On (parent)                 │
│  │   ├── Idle                    │
│  │   ├── Running                 │
│  │   └── Paused                  │
│  └── Error                       │
│                                  │
│  Event dispatch bubbles up       │
│  LCA-based entry/exit path       │
└──────────────────────────────────┘
```

**与消息总线集成**: 总线消息 -> Event(id) -> StateMachine::Dispatch()

### 6.6 行为树 (P2, bt.hpp)

**目标**: 集成 bt-cpp，异步任务编排。

```
    Sequence
    ├── Condition: check_connection()
    ├── Action: send_heartbeat()     [kRunning -> kSuccess]
    └── Selector
        ├── Action: process_data()
        └── Action: fallback_handler()
```

**与工作线程池集成**: BT 叶节点通过 WorkerPool 提交异步任务。

### 6.7 Executor 调度器 (P1, executor.hpp)

**目标**: 借鉴 ROS2 Executor 模型，统一节点调度。

| 模式 | 说明 |
|------|------|
| `SingleThreadExecutor` | 单线程轮询所有节点 |
| `StaticExecutor` | 固定节点-线程映射 (确定性调度) |
| `PinnedExecutor` | 每节点绑定 CPU 核心 |

```cpp
namespace osp {
template <typename PayloadVariant>
class StaticExecutor {
 public:
  void AddNode(Node<PayloadVariant>& node);
  void Spin();      // 阻塞运行
  void SpinOnce();  // 单轮处理
  void Stop();
};
}  // namespace osp
```

### 6.8 数据融合 (P2, data_fusion.hpp)

**目标**: 借鉴 CyberRT DataFusion，多消息对齐触发。

```cpp
namespace osp {
template <typename... MsgTypes>
class FusedSubscription {
 public:
  using Callback = void(*)(const std::tuple<MsgTypes...>&);
  void SetCallback(Callback cb);
  void OnMessage(/* variant */);
  // 当所有 MsgTypes 各收到至少一条时触发回调
};
}  // namespace osp
```

---

## 7. 资源预算

### 7.1 已实现模块

| 模块 | 静态内存 | 堆内存 | 线程数 |
|------|----------|--------|--------|
| Config (128 entries) | ~48 KB | 0 | 0 |
| TimerScheduler(16) | ~1 KB | ~1 KB | 1 |
| DebugShell(2 conn) | ~1 KB | ~4 KB | 3 |
| FixedPool<256,64> | 16 KB | 0 | 0 |
| ShutdownManager | <256 B | 0 | 0 |
| AsyncBus(4096) | ~320 KB | 0 | 0 |
| Node (per node) | ~1 KB | 0 | 0 |
| WorkerPool(4 workers) | ~64 KB | ~16 KB | 5 |
| **典型总计** | **~450 KB** | **~21 KB** | **9** |

### 7.2 规划模块 (估算)

| 模块 | 静态内存 | 堆内存 | 线程数 |
|------|----------|--------|--------|
| TcpSocket (per conn) | ~128 B | 0 | 0 |
| IoPoller (64 fds) | ~2 KB | <1 KB | 0 |
| ConnectionPool<32> | ~4 KB | 0 | 0 |
| StateMachine | ~1 KB | <1 KB | 0 |
| BehaviorTree (32 nodes) | ~4 KB | 0 | 0 |
| StaticExecutor | <1 KB | 0 | N (节点数) |

---

## 8. 编码规范与质量保障

### 8.1 代码风格

基于 Google C++ Style Guide，使用 `.clang-format` 和 `CPPLINT.cfg` 配置:

- **缩进**: 2 空格，无 Tab
- **行宽**: 120 字符
- **花括号**: Attach 风格 (同行)
- **指针**: 左对齐 (`int* ptr`)
- **命名空间**: 不缩进
- **Include 排序**: 主头文件 > 项目头文件 > C 封装 > C++ 标准库

### 8.2 命名约定

| 元素 | 风格 | 示例 |
|------|------|------|
| 类/结构体 | PascalCase | `AsyncBus`, `TimerScheduler` |
| 函数 (公有) | PascalCase | `Publish()`, `ProcessBatch()` |
| 函数 (私有) | snake_case_ | `dispatch_to_worker_()` |
| 变量 | snake_case | `sender_id`, `queue_depth` |
| 成员变量 | snake_case_ | `running_`, `config_` |
| 常量 | kPascalCase | `kCacheLineSize`, `kMaxBlocks` |
| 枚举值 | kPascalCase | `kSuccess`, `kQueueFull` |
| 宏 | OSP_UPPER_CASE | `OSP_LOG_INFO`, `OSP_ASSERT` |
| 模板参数 | PascalCase | `PayloadVariant`, `BlockSize` |

### 8.3 CI 流水线

| 阶段 | 内容 |
|------|------|
| **build-and-test** | Ubuntu + macOS, Debug + Release |
| **build-with-options** | `-fno-exceptions -fno-rtti` 兼容性 |
| **sanitizers** | ASan, TSan, UBSan |
| **code-quality** | clang-format + cpplint |

### 8.4 测试策略

- **框架**: Catch2 v3.5.2
- **每模块独立测试文件**: `test_<module>.cpp`
- **覆盖目标**: 基础 API + 边界条件 + 多线程场景
- **Sanitizer 验证**: 所有测试在 ASan/TSan/UBSan 下通过

---

## 9. 实施步骤

### Phase A: 基础设施 (已完成)

1. **项目脚手架**: CMakeLists.txt, CI workflow, LICENSE, README.md
2. **platform.hpp**: 平台检测宏、架构检测、OSP_ASSERT、编译器提示
3. **test_platform.cpp**: 平台检测验证、断言测试
4. **vocabulary.hpp -- expected/optional**: 轻量错误处理类型、void 特化、move 语义
5. **vocabulary.hpp -- FixedFunction/function_ref**: SBO 回调 (2*sizeof(void*))、非拥有引用
6. **vocabulary.hpp -- FixedVector/FixedString**: 栈分配容器、容量管理、unordered erase
7. **vocabulary.hpp -- not_null/NewType/ScopeGuard**: 类型安全包装、RAII 守卫
8. **test_vocabulary.cpp**: expected 链式测试、容器边界测试、SBO 溢出测试
9. **config.hpp -- ConfigStore 基类**: 扁平 section+key=value 存储、类型化 getter
10. **config.hpp -- IniBackend**: inih 集成、FetchContent 拉取、ParseFile/ParseBuffer
11. **config.hpp -- JsonBackend**: nlohmann/json 特化、JSON 扁平化、条件编译
12. **config.hpp -- YamlBackend**: fkYAML 特化、YAML 扁平化、条件编译
13. **config.hpp -- Config<Backends...>**: 变参模板组合、if constexpr 分发、自动格式检测
14. **test_config.cpp**: INI/JSON/YAML 解析测试、格式检测、错误路径
15. **log.hpp**: 级别枚举、编译期过滤宏、运行时级别控制、stderr 输出
16. **test_log.cpp**: 级别过滤、格式验证、多线程安全
17. **timer.hpp**: TaskSlot 预分配、steady_clock 调度、后台线程、错过周期追赶
18. **test_timer.cpp**: 添加/移除任务、周期精度、并发操作
19. **shell.hpp -- GlobalCmdRegistry**: Meyer 单例、命令注册、TAB 补全算法
20. **shell.hpp -- DebugShell**: POSIX TCP socket、会话管理、线程局部路由
21. **OSP_SHELL_CMD 宏**: 静态自动注册机制
22. **test_shell.cpp**: 注册表测试、补全测试、会话模拟
23. **mem_pool.hpp -- FixedPool**: 嵌入式空闲链表、O(1) alloc/free、memcpy 安全
24. **mem_pool.hpp -- ObjectPool**: placement new 包装、CreateChecked expected 接口
25. **test_mem_pool.cpp**: 池耗尽测试、对齐验证、并发分配
26. **shutdown.hpp**: sigaction + pipe 异步唤醒、LIFO 回调链、进程唯一实例
27. **test_shutdown.cpp**: 手动 Quit 测试、回调顺序验证

### Phase B: 通信层 (已完成)

28. **bus.hpp -- 环形缓冲区**: power-of-2 容量、序列号索引、MessageEnvelope 存储
29. **bus.hpp -- CAS 发布**: 多生产者无锁 CAS 循环、原子序列号推进
30. **bus.hpp -- 优先级准入控制**: 三级阈值 (60/80/99%)、缓存消费者位置 recheck
31. **bus.hpp -- 类型路由分发**: VariantIndex<T> 编译期索引、variant visitor 分发
32. **bus.hpp -- 统计与背压**: 原子计数器、BackpressureLevel 四级、BusStatistics 快照
33. **test_bus.cpp**: MPSC 压力测试、优先级丢弃验证、背压级别、统计准确性
34. **node.hpp -- Node 生命周期**: 命名节点、RAII 订阅清理、Start/Stop 状态管理
35. **node.hpp -- 类型化发布/订阅**: Subscribe<T> 类型解包、SubscribeSimple 简化接口
36. **node.hpp -- Publisher 轻量发布器**: 非拥有引用、PublishWithPriority
37. **test_node.cpp**: 节点创建/销毁、多类型订阅、SpinOnce 消费
38. **worker_pool.hpp -- SpscQueue**: power-of-2 容量、缓存行对齐计数器、Lamport SPSC
39. **worker_pool.hpp -- DispatcherThread**: ProcessBatch 循环、round-robin 分发
40. **worker_pool.hpp -- WorkerThread**: SPSC 消费、CV 等待、CPU 亲和性
41. **worker_pool.hpp -- 类型擦除处理器**: 函数指针 + memcpy、RegisterHandler<T>
42. **worker_pool.hpp -- FlushAndPause/Resume**: 排空机制、暂停恢复
43. **test_worker_pool.cpp**: 基本分发、多类型处理、并发提交、flush 验证

### Phase C: 编码规范与配置 (已完成)

44. **.clang-format**: Google 基础 + C++17 标准、120 列、Attach 花括号
45. **CPPLINT.cfg**: 120 列、禁用 copyright/header_guard/c++11 等现代 C++ 不兼容检查
46. **.gitignore**: build/ 及编辑器临时文件
47. **README.md**: 英文项目文档、模块表格、架构图、快速开始
48. **README_zh.md**: 中文项目文档

### Phase D: 示例程序

49. **examples/CMakeLists.txt**: 示例构建配置、链接 osp 库
50. **examples/basic_demo.cpp**: 基础 Bus/Node 消息发布/订阅示例
51. **examples/protocol_demo.cpp**: 协议处理场景 -- 消息解析、状态机驱动、多类型分发
52. **examples/client_demo.cpp**: 客户端管理场景 -- 工作线程池、连接生命周期、心跳检测
53. **examples/priority_demo.cpp**: 优先级消息系统 -- 背压测试、丢弃策略验证
54. **examples/benchmark.cpp**: 性能基准 -- 吞吐量、延迟百分位、特性开销对比

### Phase E: 文档与架构

55. **docs/design_zh.md**: 本文档 -- 完整架构设计、模块详细设计、30+实施步骤
56. **更新 CI workflow**: 增加 examples 构建、cpplint 完整路径覆盖

### Phase F: 扩展模块 -- Socket 与 I/O (规划中)

57. **socket.hpp -- TcpSocket RAII**: fd 封装、Connect/Send/Recv/Close、move 语义
58. **socket.hpp -- UdpSocket**: 无连接 Socket、SendTo/RecvFrom
59. **socket.hpp -- TcpListener**: bind/listen/accept 循环、expected 接口
60. **test_socket.cpp**: 连接测试、收发验证、错误处理
61. **io_poller.hpp**: epoll (Linux) / kqueue (macOS) 统一抽象
62. **io_poller.hpp -- 事件循环集成**: Add/Modify/Remove/Wait 接口
63. **test_io_poller.cpp**: 多 fd 就绪、超时、边缘触发

### Phase G: 扩展模块 -- 透明网络传输 (规划中)

64. **transport.hpp -- Endpoint/TransportType**: 端点定义、传输类型枚举
65. **transport.hpp -- Serializer<T>**: POD memcpy 默认序列化、模板特化扩展点
66. **transport.hpp -- 消息帧协议**: magic + length + type_index + sender_id + payload
67. **transport.hpp -- TcpTransport**: TCP 传输实现、连接管理、分帧收发
68. **transport.hpp -- UdpTransport**: UDP 数据报传输、分片重组 (MTU 感知)
69. **transport.hpp -- NetworkNode**: 继承 Node，透明远程发布/订阅
70. **transport.hpp -- 本地+远程统一分发**: Publish 同时投递 AsyncBus + 远程端点
71. **test_transport.cpp**: 本地回环、TCP 收发、序列化正确性、帧解析
72. **transport.hpp -- 节点发现 (Phase 2)**: 多播自动发现、端点注册表

### Phase H: 扩展模块 -- 连接管理 (规划中)

73. **connection.hpp -- ConnectionPool**: 固定容量连接池、ConnectionId 强类型
74. **connection.hpp -- 连接生命周期**: Add/Remove/ForEach、超时清理
75. **test_connection.cpp**: 池满测试、ID 回收、遍历一致性

### Phase I: 扩展模块 -- 状态机与行为树 (规划中)

76. **hsm.hpp**: 集成 hsm-cpp State/StateMachine 模板
77. **hsm.hpp -- 消息总线桥接**: Bus 消息 -> Event(id) -> Dispatch()
78. **test_hsm.cpp**: 层次状态转换、LCA 路径、guard 条件
79. **bt.hpp**: 集成 bt-cpp Node/BehaviorTree 模板
80. **bt.hpp -- 异步任务集成**: 叶节点通过 WorkerPool 提交任务
81. **test_bt.cpp**: Sequence/Selector/Parallel 执行、异步完成

### Phase J: 扩展模块 -- Executor 调度 (规划中)

82. **executor.hpp -- SingleThreadExecutor**: 单线程轮询多节点
83. **executor.hpp -- StaticExecutor**: 固定节点-线程映射、确定性调度
84. **executor.hpp -- PinnedExecutor**: CPU 核心绑定、最低延迟
85. **test_executor.cpp**: 节点调度公平性、停止安全性

### Phase K: 扩展模块 -- 高级特性 (规划中)

86. **data_fusion.hpp -- FusedSubscription**: 多消息对齐触发、时间窗口
87. **data_fusion.hpp -- TimeSynchronizer**: 时间戳对齐策略
88. **test_data_fusion.cpp**: 多源融合、超时处理
89. **semaphore.hpp**: 计数信号量、POSIX sem 封装
90. **优化 bus.hpp**: 借鉴 eventpp SpinLock + 指数退避、批量预取
91. **优化 worker_pool.hpp**: 自适应退避 (spin -> yield -> futex)

### Phase L: 集成验证

92. **全模块集成测试**: 跨模块交互场景 (Node + Transport + WorkerPool)
93. **跨进程通信测试**: 两个进程通过 TcpTransport 通信
94. **Sanitizer 全量验证**: ASan + TSan + UBSan 全测试通过
95. **性能基准回归**: 吞吐量/延迟基线记录

---

## 10. 附录

### 10.1 借鉴的设计模式来源

| 模式 | 来源 | 应用模块 |
|------|------|----------|
| 标签分发 + 模板特化 | -- | config.hpp 后端选择 |
| 变参模板 + if constexpr | -- | Config<Backends...> 编译期组合 |
| CRTP | -- | Shell 命令扩展 |
| SBO 回调 | iceoryx | FixedFunction |
| 无锁 MPSC 环形缓冲 | LMAX Disruptor | AsyncBus |
| 优先级准入控制 | MCCC | AsyncBus 背压 |
| 基于类型的路由 | -- | std::variant + VariantIndex<T> |
| 策略模式 | eventpp | 线程策略、分配器策略 |
| SpinLock + 指数退避 | eventpp | 低延迟锁 |
| 批量预取 | eventpp | 回调遍历优化 |
| SharedMutex 读写分离 | eventpp | 订阅管理 |
| 池分配器 | eventpp | 节点池 |
| LCA 层次转换 | hsm-cpp | 状态机 entry/exit 路径 |
| 位图并行追踪 | bt-cpp | 行为树 Parallel 节点 |
| ROS2 CallbackGroup | ROS2 rclcpp | Executor 分组调度 |
| CyberRT DataFusion | CyberRT | FusedSubscription 多源对齐 |
| 二级排队 (MPSC+SPSC) | consumer-producer | WorkerPool |

### 10.2 编译期配置汇总

| 宏 | 默认值 | 模块 | 说明 |
|----|--------|------|------|
| `OSP_LOG_MIN_LEVEL` | 0 (Debug) / 1 (Release) | log.hpp | 编译期日志级别 |
| `OSP_CONFIG_MAX_FILE_SIZE` | 8192 | config.hpp | 最大配置文件大小 |
| `OSP_BUS_QUEUE_DEPTH` | 4096 | bus.hpp | 环形缓冲区大小 |
| `OSP_BUS_MAX_MESSAGE_TYPES` | 8 | bus.hpp | 最大消息类型数 |
| `OSP_BUS_MAX_CALLBACKS_PER_TYPE` | 16 | bus.hpp | 每类型最大订阅 |
| `OSP_BUS_BATCH_SIZE` | 256 | bus.hpp | 单次批量处理上限 |
| `OSP_MAX_NODE_SUBSCRIPTIONS` | 16 | node.hpp | 每节点最大订阅 |
| `OSP_CONFIG_INI_ENABLED` | (CMake) | config.hpp | INI 后端开关 |
| `OSP_CONFIG_JSON_ENABLED` | (CMake) | config.hpp | JSON 后端开关 |
| `OSP_CONFIG_YAML_ENABLED` | (CMake) | config.hpp | YAML 后端开关 |

### 10.3 线程安全性总结

| 模块 | 线程安全保证 |
|------|-------------|
| platform | N/A (纯宏) |
| vocabulary | 局部对象，无共享 |
| config | 加载后只读 |
| log | fprintf 原子写 |
| timer | mutex 保护所有公有方法 |
| shell | 注册表 mutex + 会话线程隔离 |
| mem_pool | mutex 保护 alloc/free |
| shutdown | 原子标志 + async-signal-safe pipe |
| bus | 无锁 MPSC + SharedMutex 订阅 |
| node | 发布线程安全; SpinOnce 单消费者 |
| worker_pool | 原子标志 + CV + SPSC 无锁 |
