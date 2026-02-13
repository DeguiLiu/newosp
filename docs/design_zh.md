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

newosp 是一个面向 ARM-Linux 工业级嵌入式平台的现代 C++17 纯头文件基础设施库。提供嵌入式系统开发所需的核心基础能力: 配置管理、日志、定时调度、远程调试、内存池、消息总线、节点通信、工作线程池、共享内存 IPC、网络传输、串口通信、层次状态机、行为树、QoS 服务质量、生命周期节点、代码生成。

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
- **CI**: GitHub Actions (Ubuntu, GCC x Debug/Release, Sanitizers)
- **代码生成**: Python3 + PyYAML + Jinja2 (YAML → C++ 头文件)

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
| sockpp | master (DeguiLiu fork) | RAII Socket 封装 | `OSP_WITH_SOCKPP=ON` |
| CSerialPort | v4.3.1 | 串口通信 | `OSP_WITH_SERIAL=ON` |
| Catch2 | v3.5.2 | 单元测试 | `OSP_BUILD_TESTS=ON` |

### 2.4 开发分级与优先级原则

所有模块和任务按 P0 -> P1 -> P2 三级优先级开发和测试，严格按优先级顺序推进:

| 优先级 | 含义 | 开发原则 | 对应 Phase |
|--------|------|---------|-----------|
| P0 | 架构基础，必须先行 | 不完成 P0 不启动 P1；设计文档同步更新 | Phase A-N (已完成), Phase O (已实现) |
| P1 | 功能扩展，核心可用 | P0 全部完成后启动；兼容性和功能覆盖优先 | Phase P (已实现) |
| P2 | 高级特性，锦上添花 | P1 基本完成后启动；可按需裁剪 | Phase Q (已实现) |

关键约束:
- 设计文档 (design_zh.md) 的及时更新永远是 P0 级别，任何架构调整必须先更新文档再编码
- 每个 Phase 完成后必须通过全量 Sanitizer 验证 (ASan + TSan + UBSan)
- P0 模块的接口变更需要评估对已完成模块的影响

### 2.5 P0 架构调整

本节列出当前架构设计中已识别的 7 项 P0 优先级调整项，这些调整针对工业级嵌入式场景的关键需求，将在后续实施阶段逐步落地。

#### P0-1: AsyncBus 单消费者瓶颈 + variant 内存膨胀

**优先级**: P0 | **状态**: 已实现

**问题描述**:
- 当前 AsyncBus 采用 MPSC (多生产者单消费者) 架构，单消费者 `ProcessBatch()` 串行处理所有消息类型，在高频消息场景下成为吞吐瓶颈
- `std::variant` 按最大类型分配内存，当 variant 中类型大小差异悬殊时 (如 4B 心跳 vs 1KB 帧头)，小消息浪费严重

**解决方案**:
- 模板参数化 Bus 容量和消息类型，支持不同场景定制化配置
- 大消息/小消息分离通道: 不同大小的消息使用不同 AsyncBus 实例，避免内存浪费
- 支持多消费者 (MPMC) 可选模式: 按消息类型分片到多个消费者线程，突破单消费者瓶颈

**设计要点**:
```cpp
// 模板参数化容量和批量大小
template <typename PayloadVariant,
          uint32_t QueueDepth = 4096,
          uint32_t BatchSize = 256>
class AsyncBus { /* ... */ };

// 嵌入式低内存场景
using LightBus = AsyncBus<SmallPayload, 256, 64>;

// 高吞吐场景
using HighBus = AsyncBus<LargePayload, 4096, 256>;

// 多消费者模式 (可选)
template <typename PayloadVariant, uint32_t ConsumerCount = 1>
class MpmcBus { /* 按类型哈希分片到多个消费者 */ };
```

**影响模块**: bus.hpp, node.hpp, worker_pool.hpp

---

#### P0-2: Node 全局单例 Bus + 纯类型路由局限

**优先级**: P0 | **状态**: 已实现

**问题描述**:
- 当前 Node 使用全局单例 Bus，不同 Node 无法隔离，子系统间耦合严重
- 纯类型路由无法区分同类型不同语义的消息 (如 IMU 温度 vs 环境温度，均为 `SensorData` 类型)

**解决方案**:
- Bus 依赖注入而非全局单例: 构造时传入 Bus 引用，支持多总线实例隔离子系统
- 混合 topic+type 路由: 结合 topic 字符串哈希 + 类型索引，运行时按 topic 过滤

**设计要点**:
```cpp
// 方式 1: 使用默认全局总线 (向后兼容)
osp::Node<Payload> sensor("sensor", 1);

// 方式 2: 依赖注入总线实例 (推荐)
osp::AsyncBus<Payload, 1024> my_bus;
osp::Node<Payload> sensor("sensor", 1, my_bus);

// 纯类型路由 (默认)
sensor.Subscribe<SensorData>(on_sensor);

// 混合路由: 类型 + 主题名
sensor.Subscribe<SensorData>("imu/temperature", on_imu_temp);
sensor.Subscribe<SensorData>("env/temperature", on_env_temp);
sensor.Publish(SensorData{25.0f}, "imu/temperature");
```

**影响模块**: node.hpp, bus.hpp

---

#### P0-3: Executor 缺少实时调度支持

**优先级**: P0 | **状态**: 已实现

**问题描述**:
- 当前 Executor 使用普通线程 (`SCHED_OTHER`)，无法满足工业实时性要求
- 缺少内存锁定、CPU 亲和性、优先级调度等实时特性

**解决方案**:
- 增加 `RealtimeConfig` 结构体，支持 `SCHED_FIFO` / `SCHED_DEADLINE` 调度策略
- `mlockall()` 锁定内存，避免页面交换导致的不确定延迟
- CPU 亲和性绑定，关键节点绑定到隔离核心 (配合 `isolcpus` 内核参数)

**设计要点**:
```cpp
namespace osp {

// 实时调度配置 (借鉴 CyberRT Processor)
struct RealtimeConfig {
  int sched_policy = SCHED_OTHER;   // SCHED_OTHER / SCHED_FIFO / SCHED_RR
  int sched_priority = 0;           // SCHED_FIFO: 1-99, 越大越高
  bool lock_memory = false;         // mlockall(MCL_CURRENT | MCL_FUTURE)
  uint32_t stack_size = 0;          // 0=系统默认, 非零=预分配栈
  int cpu_affinity = -1;            // -1=不绑定, >=0=绑定到指定 CPU 核心
};

// 实时调度器
template <typename PayloadVariant>
class RealtimeExecutor {
 public:
  explicit RealtimeExecutor(const RealtimeConfig& cfg);
  void AddNode(Node<PayloadVariant>& node, int priority = 0);
  void AddPeriodicNode(Node<PayloadVariant>& node,
                       uint32_t period_ms, int priority = 0);
  void Spin();
  void Stop();
};

}  // namespace osp
```

**影响模块**: executor.hpp

---

#### P0-4: Transport 帧缺少 seq_num 和 timestamp

**优先级**: P0 | **状态**: 已实现

**问题描述**:
- 网络传输帧头缺少序列号和时间戳，无法检测丢帧和测量延迟
- UDP 和串口场景下无法检测乱序/重复/丢包
- 数据融合场景 (DataFusion) 依赖时间戳对齐多源消息

**解决方案**:
- 扩展帧头增加 `seq_num (4B)` + `timestamp (8B)`
- 保持向后兼容: 通过 magic 版本号区分新旧帧格式

**设计要点**:
```
新帧格式 (26 字节帧头):
┌─────────────┬─────────────┬──────────┬───────────┬───────────┬──────────────┬──────────┐
│ magic (4B)  │ msg_len (4B)│ type (2B)│ sender(4B)│ seq_num(4B)│ timestamp(8B)│ payload  │
│ 0x4F535001  │ total bytes │ variant  │ node id   │ 单调递增   │ steady_clock │ N bytes  │
│ (版本 v1)   │             │ index    │           │            │ 纳秒         │          │
└─────────────┴─────────────┴──────────┴───────────┴───────────┴──────────────┴──────────┘

旧帧格式 (14 字节帧头, 向后兼容):
┌─────────────┬─────────────┬──────────┬───────────┬──────────┐
│ magic (4B)  │ msg_len (4B)│ type (2B)│ sender(4B)│ payload  │
│ 0x4F535000  │ total bytes │ variant  │ node id   │ N bytes  │
│ (版本 v0)   │             │ index    │           │          │
└─────────────┴─────────────┴──────────┴───────────┴──────────┘
```

**新增字段说明**:
- `seq_num (4B)`: 每个发送端单调递增，用于检测丢包/乱序/重复 (UDP 和串口场景必需)
- `timestamp (8B)`: `steady_clock` 纳秒时间戳，用于端到端延迟分析和 DataFusion 时间对齐

**影响模块**: transport.hpp, serial_transport.hpp

---

#### P0-5: ShmRingBuffer ARM 弱内存序

**优先级**: P0 | **状态**: 已实现

**问题描述**:
- ARM 是弱内存序 (weakly-ordered) 架构，与 x86 的 TSO (Total Store Order) 不同
- 当前共享内存实现可能存在可见性问题: 生产者写入的数据在消费者侧不可见
- 默认 `memory_order_seq_cst` 在 ARM 上开销是 `acquire/release` 的 3-5 倍

**解决方案**:
- 显式标注 memory_order: 生产者 `release`，消费者 `acquire`
- `uint64_t` 序列号避免 ABA 问题 (64 位序列号在 4GHz 频率下需 ~146 年才会回绕)
- LoanedMessage 引用计数使用原子操作 (`acq_rel` 递减，`relaxed` 递增)

**设计要点**:
```cpp
// 生产者: 写入数据后 release 推进位置
void TryPush(const void* data, uint32_t size) {
    auto pos = prod_pos_.load(std::memory_order_relaxed);
    // ... CAS 竞争 slot ...
    memcpy(&slots_[pos % SlotCount], data, size);
    prod_pos_.store(pos + 1, std::memory_order_release);  // release: 数据可见
}

// 消费者: acquire 读取位置后读数据
bool TryPop(void* data, uint32_t& size) {
    auto prod = prod_pos_.load(std::memory_order_acquire);  // acquire: 配对 release
    auto cons = cons_pos_.load(std::memory_order_relaxed);
    if (cons >= prod) return false;
    memcpy(data, &slots_[cons % SlotCount], size);
    cons_pos_.store(cons + 1, std::memory_order_release);
    return true;
}

// LoanedMessage::Release: 最后一个订阅者释放 slot
void Release() noexcept {
    if (header_->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // 最后一个订阅者，回收 slot
    }
}
```

**性能对比 (ARM Cortex-A72)**:

| memory_order | 指令 | 开销 (相对 relaxed) |
|-------------|------|-------------------|
| `relaxed` | 普通 load/store | 1x |
| `acquire` | `ldar` (load-acquire) | ~1.2x |
| `release` | `stlr` (store-release) | ~1.2x |
| `seq_cst` | `ldar` + `stlr` + barrier | ~3-5x |

**影响模块**: shm_transport.hpp

---

#### P0-6: 缺少 QoS 配置层

**优先级**: P0 | **状态**: 已实现

**问题描述**:
- 无法配置消息传输的可靠性、历史深度、截止时间等质量属性
- 不同场景对消息传输的需求差异大 (如控制指令需要可靠传输，传感器数据可容忍丢失)

**解决方案**:
- 增加 `QosProfile` 结构体，借鉴 ROS2 QoS 策略
- 支持可靠性 (BestEffort/Reliable)、历史深度 (KeepLast/KeepAll)、截止时间 (Deadline)、生命周期 (Lifespan)

**设计要点**:
```cpp
namespace osp {

// QoS 可靠性策略
enum class ReliabilityPolicy : uint8_t {
  kBestEffort,  // 尽力而为，允许丢失 (UDP 语义)
  kReliable     // 可靠传输，保证送达 (TCP 语义 + 重传)
};

// QoS 历史深度策略
enum class HistoryPolicy : uint8_t {
  kKeepLast,    // 保留最近 N 条消息
  kKeepAll      // 保留所有消息 (直到队列满)
};

// QoS 配置
struct QosProfile {
  ReliabilityPolicy reliability = ReliabilityPolicy::kBestEffort;
  HistoryPolicy history = HistoryPolicy::kKeepLast;
  uint32_t history_depth = 10;      // KeepLast 模式下保留的消息数
  uint32_t deadline_ms = 0;         // 0=无截止时间，>0=消息超时丢弃
  uint32_t lifespan_ms = 0;         // 0=无生命周期，>0=消息过期丢弃
};

// 预定义 QoS 配置
constexpr QosProfile QosSensorData{
  .reliability = ReliabilityPolicy::kBestEffort,
  .history = HistoryPolicy::kKeepLast,
  .history_depth = 1  // 只保留最新值
};

constexpr QosProfile QosControlCommand{
  .reliability = ReliabilityPolicy::kReliable,
  .history = HistoryPolicy::kKeepAll,
  .deadline_ms = 100  // 100ms 内必须送达
};

// Node 订阅时指定 QoS
template <typename T>
void Subscribe(const char* topic, Callback<T> cb, const QosProfile& qos);

}  // namespace osp
```

**影响模块**: node.hpp, transport.hpp, bus.hpp

---

#### P0-7: 缺少 Lifecycle Node

**优先级**: P0 | **状态**: 已实现

**问题描述**:
- 节点缺少标准化的生命周期管理 (类似 ROS2 Lifecycle Node)
- 无法统一管理节点的配置、激活、停用、清理等阶段
- 复杂系统中节点启动顺序和依赖关系难以管理

**解决方案**:
- 增加 `LifecycleNode` 状态机: Unconfigured → Inactive → Active → Finalized
- 支持 `configure` / `activate` / `deactivate` / `cleanup` 回调
- 借鉴 ROS2 Lifecycle Node 的状态转换模型

**设计要点**:
```cpp
namespace osp {

// 生命周期状态
enum class LifecycleState : uint8_t {
  kUnconfigured,  // 未配置 (初始状态)
  kInactive,      // 已配置但未激活
  kActive,        // 激活运行中
  kFinalized      // 已终止 (终态)
};

// 生命周期转换
enum class LifecycleTransition : uint8_t {
  kConfigure,     // Unconfigured → Inactive
  kActivate,      // Inactive → Active
  kDeactivate,    // Active → Inactive
  kCleanup,       // Inactive → Unconfigured
  kShutdown       // Any → Finalized
};

// 生命周期节点
template <typename PayloadVariant>
class LifecycleNode : public Node<PayloadVariant> {
 public:
  // 状态转换回调 (返回 true 表示成功)
  virtual bool OnConfigure() { return true; }
  virtual bool OnActivate() { return true; }
  virtual bool OnDeactivate() { return true; }
  virtual bool OnCleanup() { return true; }
  virtual void OnShutdown() {}

  // 触发状态转换
  expected<void, LifecycleError> Trigger(LifecycleTransition transition);

  LifecycleState GetState() const noexcept;
};

}  // namespace osp
```

**状态转换图**:
```
    Unconfigured
         |
         | configure()
         v
      Inactive  ←──────┐
         |             |
         | activate()  | deactivate()
         v             |
       Active  ────────┘
         |
         | shutdown()
         v
      Finalized
```

**影响模块**: node.hpp

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
    ├── worker_pool.hpp (依赖 vocabulary + bus.hpp)
    ├── hsm.hpp        (依赖 platform)
    ├── bt.hpp         (依赖 platform)
    ├── net.hpp        (依赖 vocabulary + sockpp)
    ├── transport_factory.hpp (依赖 platform + vocabulary)
    ├── node_manager_hsm.hpp  (依赖 hsm + platform + vocabulary)
    ├── service_hsm.hpp       (依赖 hsm + platform)
    └── discovery_hsm.hpp     (依赖 hsm + platform)
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

**P0 架构调整参考**: 本模块涉及以下 P0 调整项:
- [P0-1: AsyncBus 单消费者瓶颈 + variant 内存膨胀](#p0-1-asyncbus-单消费者瓶颈--variant-内存膨胀) -- 模板参数化容量、大小消息分离、多消费者模式
- [P0-2: Node 全局单例 Bus + 纯类型路由局限](#p0-2-node-全局单例-bus--纯类型路由局限) -- Bus 依赖注入、混合 topic+type 路由

**架构**:

```
Producer 0 ─┐
Producer 1 ──┼── CAS Publish ──> ┌────────────────┐
Producer 2 ─┘                    │ Ring Buffer    │
                                 │ (模板参数化)    │
                                 │ sequence-based  │
                                 └───────┬────────┘
                                         │ ProcessBatch()
                                         v
                                 Type-Based Dispatch
                                 (std::variant + VariantIndex<T>)
```

**模板参数化** (P0 调整):

```cpp
// 队列深度和批量大小作为模板参数，适配不同场景
template <typename PayloadVariant,
          uint32_t QueueDepth = 4096,
          uint32_t BatchSize = 256>
class AsyncBus {
    static_assert((QueueDepth & (QueueDepth - 1)) == 0, "QueueDepth must be power of 2");
    // ...
};

// 嵌入式低内存场景
using LightBus = AsyncBus<Payload, 256, 64>;    // ~20KB

// 高吞吐场景
using HighBus = AsyncBus<Payload, 4096, 256>;   // ~320KB
```

**variant 内存优化说明**:

每个 slot 占用 `sizeof(PayloadVariant)` 字节 (最大类型 size + 对齐)。当 variant 中类型大小差异悬殊时 (如 4B 心跳 vs 1KB 帧头)，小消息浪费严重。应对策略:

| 策略 | 说明 | 适用场景 |
|------|------|---------|
| 控制 variant 类型大小 | 大消息用指针/ID 间接引用，variant 只存元数据 | 推荐，零改动 |
| 分离总线 | 不同大小的消息使用不同 AsyncBus 实例 | 子系统隔离 |
| MemPool 间接 | 大消息存入 ObjectPool，variant 存 `PoolHandle<T>` | 混合大小消息 |

**关键特性**:

| 特性 | 说明 |
|------|------|
| 无锁发布 | CAS 循环抢占生产者位置 |
| 优先级准入控制 | LOW 60% / MEDIUM 80% / HIGH 99% 阈值 |
| 批量处理 | `ProcessBatch()` 每轮最多 BatchSize 条消息 |
| 背压感知 | Normal/Warning/Critical/Full 四级 |
| 缓存行分离 | 生产者/消费者计数器分属不同缓存行 |

**编译期配置**:

```cpp
#define OSP_BUS_QUEUE_DEPTH            4096   // 环形缓冲区默认大小 (2^N, 模板可覆盖)
#define OSP_BUS_MAX_MESSAGE_TYPES      8      // std::variant 最大类型数
#define OSP_BUS_MAX_CALLBACKS_PER_TYPE 16     // 每类型最大订阅数
#define OSP_BUS_BATCH_SIZE             256    // 单次处理批量默认上限 (模板可覆盖)
```

**单消费者设计取舍**:

单消费者 `ProcessBatch()` 是有意的设计选择，而非疏忽:
- 优势: 消费侧无锁、无竞争，延迟确定性好
- 局限: 所有消息类型串行分发，高频消息可能阻塞低频消息
- 适用边界: 消息处理回调应轻量 (<10us)，重计算应转发到 WorkerPool
- 扩展方案: 如需多消费者，可按消息类型分片到多个 AsyncBus 实例

**设计模式** (借鉴 eventpp):
- 序列号环形缓冲区 (类似 Disruptor)
- 准入控制带缓存消费者位置 recheck (减少 atomic load)
- `SharedMutex` 保护回调注册 (读写分离)

---

### 4.10 node.hpp -- 轻量 Pub/Sub 节点

**职责**: 受 ROS2/CyberRT 启发的轻量节点通信抽象。

**P0 架构调整参考**: 本模块涉及以下 P0 调整项:
- [P0-2: Node 全局单例 Bus + 纯类型路由局限](#p0-2-node-全局单例-bus--纯类型路由局限) -- Bus 依赖注入、混合 topic+type 路由
- [P0-6: 缺少 QoS 配置层](#p0-6-缺少-qos-配置层) -- QosProfile 配置可靠性、历史深度、截止时间
- [P0-7: 缺少 Lifecycle Node](#p0-7-缺少-lifecycle-node) -- LifecycleNode 状态机管理

**接口**:

```cpp
struct SensorData { float temp; };
struct MotorCmd { int speed; };
using Payload = std::variant<SensorData, MotorCmd>;

// 方式 1: 使用默认全局总线 (简单场景，向后兼容)
osp::Node<Payload> sensor("sensor", 1);

// 方式 2: 依赖注入总线实例 (P0 调整，推荐)
osp::AsyncBus<Payload, 1024> my_bus;
osp::Node<Payload> sensor("sensor", 1, my_bus);

// 纯类型路由 (默认，编译期确定)
sensor.Subscribe<SensorData>([](const SensorData& d, const osp::MessageHeader& h) {
    OSP_LOG_INFO("sensor", "temp=%.1f from sender %u", d.temp, h.sender_id);
});

// 混合路由: 类型 + 主题名 (P0 调整，区分同类型不同语义)
sensor.Subscribe<SensorData>("imu/temperature", on_imu_temp);
sensor.Subscribe<SensorData>("env/temperature", on_env_temp);

sensor.Publish(SensorData{25.0f});
sensor.Publish(SensorData{18.0f}, "env/temperature");  // 带主题名发布
sensor.SpinOnce();  // 消费总线中的消息
```

**设计决策**:
- 类型路由为主，主题路由为辅 (类型安全 + 语义灵活)
  - 纯类型路由: 编译期确定分发，零开销，适合单一语义消息
  - 混合路由: `Subscribe<T>(topic, cb)`，运行时按 topic 过滤，适合同类型多源场景
  - 借鉴: CyberRT 的 channel name + ROS2 的 topic name
- RAII 自动退订 (析构时清理所有订阅)
- 总线依赖注入 (P0 调整):
  - 构造时可传入 Bus 引用，支持多总线实例隔离子系统
  - 不传则使用全局单例总线 (向后兼容)
  - 优势: 可独立测试、子系统隔离、多租户共存
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

## 6. 扩展模块设计

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

**P0 架构调整参考**: 本模块涉及以下 P0 调整项:
- [P0-4: Transport 帧缺少 seq_num 和 timestamp](#p0-4-transport-帧缺少-seq_num-和-timestamp) -- 扩展帧头增加序列号和时间戳
- [P0-6: 缺少 QoS 配置层](#p0-6-缺少-qos-配置层) -- QosProfile 配置传输质量属性

**设计理念** (借鉴 ZeroMQ / ROS2 / CyberRT / cpp-ipc):

| 特性 | 说明 |
|------|------|
| 位置透明 | 本地/远程节点使用相同的 Pub/Sub API |
| 传输可插拔 | inproc (本地 AsyncBus) / shm (共享内存) / tcp / udp / unix domain socket |
| 零拷贝本地路径 | 进程内走 AsyncBus，进程间走 ShmTransport LoanedMessage |
| 自动发现 | 多播或配置文件驱动的节点发现 |
| 自动传输选择 | 借鉴 CyberRT，根据拓扑自动选择 inproc/shm/tcp |
| 序列化可定制 | 默认 memcpy (POD 类型)，可扩展 protobuf/flatbuffers |

**架构**:

```
                        Application Layer
                              |
                    Node<Payload>::Publish(msg)
                              |
                    TransportFactory::Route()
                              |
              ┌───────────────┼───────────────┬───────────────┐
              |               |               |               |
        LocalTransport   ShmTransport    RemoteTransport  SerialTransport
        (AsyncBus,       (共享内存,       (TCP/UDP +       (UART/RS-485,
         zero-copy)       LoanedMessage)   serialize)       CSerialPort)
              |               |               |               |
        本地 Subscriber  进程间 Subscriber  网络 I/O 线程   串口 I/O 线程
                              |               |               |
                        ShmChannel       IoPoller (epoll/kqueue)
                        (eventfd 通知)         |               |
                                        ┌─────┴─────┐         |
                                        |           |         |
                                   TcpTransport UdpTransport  |
                                   (sockpp)     (sockpp)      |
                                                              |
                                                        SerialTransport
                                                        (CSerialPort)
```

**传输策略接口**:

```cpp
namespace osp {

// 传输层基础类型
enum class TransportType : uint8_t { kInproc, kShm, kTcp, kUdp, kUnix, kSerial };

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

**消息帧格式** (P0 调整: 增加序列号和时间戳):

```
┌─────────────┬─────────────┬──────────┬───────────┬───────────┬──────────────┬──────────┐
│ magic (4B)  │ msg_len (4B)│ type (2B)│ sender(4B)│ seq_num(4B)│ timestamp(8B)│ payload  │
│ 0x4F535000  │ total bytes │ variant  │ node id   │ 单调递增   │ steady_clock │ N bytes  │
│             │             │ index    │           │            │ 纳秒         │          │
└─────────────┴─────────────┴──────────┴───────────┴───────────┴──────────────┴──────────┘
帧头总计: 26 字节 (旧 14 字节)
```

新增字段说明:
- `seq_num (4B)`: 每个发送端单调递增，用于检测丢包/乱序/重复 (UDP 和串口场景必需)
- `timestamp (8B)`: `steady_clock` 纳秒时间戳，用于端到端延迟分析和 DataFusion 时间对齐 (借鉴 ROS2 message_filters 和 CyberRT DataFusion 的时间戳依赖)
- TCP 长连接可在握手后省略 magic (通过连接协商标志位控制，减少 4B/帧开销)

**实现策略**:
- Phase 1 (已完成): `TcpTransport` + `Serializer<POD>` 基础实现
- Phase 2 (已完成): `UdpTransport` + 分片重组
- Phase 3 (Phase M): `ShmTransport` + 共享内存无锁队列 + LoanedMessage 零拷贝
- Phase 4 (Phase N): sockpp 深度集成，替代裸 POSIX Socket 封装
- Phase 5 (Phase O): 节点自动发现 (multicast) + Service/Client RPC
- Phase 6 (未来): 可选 protobuf/flatbuffers 序列化后端

### 6.5 层次状态机 (P0, hsm.hpp) -- 已实现

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

### 6.6 行为树 (P1, bt.hpp) -- 已实现

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

### 6.7 Executor 调度器 (P0, executor.hpp)

**目标**: 借鉴 ROS2 Executor 和 CyberRT Choreography Scheduler，统一节点调度，支持实时调度策略。

**P0 架构调整参考**: 本模块涉及以下 P0 调整项:
- [P0-3: Executor 缺少实时调度支持](#p0-3-executor-缺少实时调度支持) -- RealtimeConfig、SCHED_FIFO、mlockall、CPU 亲和性

**实时调度配置** (P0 调整):

```cpp
namespace osp {

// 实时调度配置 (借鉴 CyberRT Processor)
struct RealtimeConfig {
  int sched_policy = SCHED_OTHER;   // SCHED_OTHER / SCHED_FIFO / SCHED_RR
  int sched_priority = 0;           // SCHED_FIFO: 1-99, 越大越高
  bool lock_memory = false;         // mlockall(MCL_CURRENT | MCL_FUTURE)
  uint32_t stack_size = 0;          // 0=系统默认, 非零=预分配栈 (避免缺页中断)
  int cpu_affinity = -1;            // -1=不绑定, >=0=绑定到指定 CPU 核心
};

}  // namespace osp
```

| 模式 | 说明 |
|------|------|
| `SingleThreadExecutor` | 单线程轮询所有节点 |
| `StaticExecutor` | 固定节点-线程映射 (确定性调度) |
| `PinnedExecutor` | 每节点绑定 CPU 核心 + 实时优先级 |
| `RealtimeExecutor` | SCHED_FIFO + mlockall + 优先级队列 (P0 新增) |

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

// P0 新增: 实时调度器 (借鉴 CyberRT Choreography)
template <typename PayloadVariant>
class RealtimeExecutor {
 public:
  explicit RealtimeExecutor(const RealtimeConfig& cfg);

  // 添加节点，指定优先级 (高优先级节点优先执行)
  void AddNode(Node<PayloadVariant>& node, int priority = 0);

  // 添加周期节点 (与 timer.hpp 集成)
  void AddPeriodicNode(Node<PayloadVariant>& node,
                       uint32_t period_ms, int priority = 0);

  void Spin();      // 阻塞运行 (SCHED_FIFO 线程)
  void SpinOnce();  // 单轮处理 (按优先级顺序)
  void Stop();
};

}  // namespace osp
```

**实时调度要点**:
- `SCHED_FIFO` + `mlockall`: 避免页面交换导致的不确定延迟
- CPU 亲和性: 关键节点绑定到隔离核心 (通过 `isolcpus` 内核参数)
- 优先级队列: 高优先级节点优先执行，借鉴 CyberRT ClassicContext 多级队列
- 周期调度: Executor 内部集成 Timer，支持固定周期触发节点 SpinOnce

### 6.8 数据融合 (P1, data_fusion.hpp) -- 已实现

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

### 6.9 共享内存 IPC (P0, shm_transport.hpp)

**目标**: 借鉴 [cpp-ipc](https://github.com/mutouyun/cpp-ipc) 的共享内存无锁队列和 [ROS2 loaned messages](https://www.eprosima.com/r-d-projects/rosin-project-ros2-shared-memory) 的零拷贝设计，实现同机器进程间高性能通信。

**P0 架构调整参考**: 本模块涉及以下 P0 调整项:
- [P0-5: ShmRingBuffer ARM 弱内存序](#p0-5-shmringbuffer-arm-弱内存序) -- 显式 memory_order、uint64_t 序列号、LoanedMessage 原子引用计数

**设计理念** (借鉴 cpp-ipc / ROS2 / CyberRT):

| 特性 | 说明 |
|------|------|
| 共享内存传输 | POSIX shm_open/mmap，避免内核态 Socket 开销 |
| 无锁队列 | CAS 环形缓冲区在共享内存中，多生产者安全 |
| 零拷贝 | LoanedMessage API，发布者直接写入共享内存段 |
| 命名通道 | 基于名称的通道发现，类似 cpp-ipc 的 channel |
| 轻量通知 | eventfd (Linux) 唤醒等待的消费者，热路径无系统调用 |

**架构**:

```
Process A                          Process B
┌──────────────┐                  ┌──────────────┐
│ Node::Publish│                  │ Node::Subscribe
│      |       │                  │      ^       │
│ ShmTransport │                  │ ShmTransport │
│      |       │                  │      |       │
│  ShmChannel  │                  │  ShmChannel  │
│   (writer)   │                  │   (reader)   │
└──────┬───────┘                  └──────┬───────┘
       │                                 │
       v          Shared Memory          v
  ┌────────────────────────────────────────┐
  │  /osp_channel_<name>                   │
  │  ┌──────────────────────────────────┐  │
  │  │ ShmRingBuffer (lock-free MPSC)   │  │
  │  │ [slot0][slot1][slot2]...[slotN]  │  │
  │  │  prod_pos_ (atomic)              │  │
  │  │  cons_pos_ (atomic)              │  │
  │  └──────────────────────────────────┘  │
  │  eventfd for wakeup notification       │
  └────────────────────────────────────────┘
```

**核心接口**:

```cpp
namespace osp {

// 共享内存段 RAII 封装
class SharedMemorySegment {
 public:
  static expected<SharedMemorySegment, ShmError> Create(
      const char* name, uint32_t size);
  static expected<SharedMemorySegment, ShmError> Open(const char* name);
  void* Data() noexcept;
  uint32_t Size() const noexcept;
  void Unlink() noexcept;  // 标记删除 (最后一个 close 时释放)
};

// 共享内存中的无锁环形缓冲区
template <uint32_t SlotSize, uint32_t SlotCount>
class ShmRingBuffer {
 public:
  // 在已有共享内存上构造 (placement)
  static ShmRingBuffer* InitAt(void* shm_addr);
  static ShmRingBuffer* AttachAt(void* shm_addr);

  bool TryPush(const void* data, uint32_t size);
  bool TryPop(void* data, uint32_t& size);
  uint32_t Depth() const noexcept;

 private:
  // P0 调整: 使用 uint64_t 序列号防止 ABA 问题
  // 64 位序列号在 4GHz 频率下需 ~146 年才会回绕
  alignas(64) std::atomic<uint64_t> prod_pos_{0};  // 缓存行对齐
  alignas(64) std::atomic<uint64_t> cons_pos_{0};
  // slot 索引 = pos % SlotCount
};

// 命名通道 (生产者/消费者端点)
class ShmChannel {
 public:
  static expected<ShmChannel, ShmError> CreateWriter(const char* name,
      uint32_t slot_size, uint32_t slot_count);
  static expected<ShmChannel, ShmError> OpenReader(const char* name);

  bool Write(const void* data, uint32_t size);
  bool Read(void* data, uint32_t& size);
  bool WaitReadable(int timeout_ms);  // eventfd 等待
  void Notify();                       // eventfd 唤醒
};

// 零拷贝 loaned message (仅限 POD 类型)
// P0 调整: 增加原子引用计数，支持多订阅者安全共享
template <typename T>
class LoanedMessage {
 public:
  T* Get() noexcept;             // 直接写入共享内存
  void Publish() noexcept;       // 提交 (标记 slot 可读，初始化 ref_count)
  void AddRef() noexcept;        // 订阅者获取时 +1 (memory_order_relaxed)
  void Release() noexcept;       // 订阅者消费完 -1，最后一个释放 slot
  uint32_t RefCount() const noexcept;
  // Non-copyable, movable

 private:
  // 引用计数位于 slot header，与数据共存于共享内存
  // Publish 时设置 ref_count = 订阅者数量
  // 每个订阅者消费完调用 Release()
  // ref_count 降为 0 时 slot 回收 (cons_pos_ 推进)
  struct SlotHeader {
    std::atomic<uint32_t> ref_count{0};
    uint32_t data_size;
  };
};

// 共享内存传输 (实现 Transport 接口)
template <typename PayloadVariant>
class ShmTransport {
 public:
  expected<void, ShmError> Bind(const char* channel_name);
  expected<void, ShmError> Connect(const char* channel_name);

  template <typename T>
  expected<LoanedMessage<T>, ShmError> Borrow();  // 零拷贝发布

  expected<void, ShmError> Send(const void* data, uint32_t size);
  void Poll(int timeout_ms);
  void Close() noexcept;
};

}  // namespace osp
```

**P0 调整: ARM 弱内存模型注意事项**:

ARM 是弱内存序 (weakly-ordered) 架构，与 x86 的 TSO (Total Store Order) 不同，编译器和 CPU 可能重排内存访问。ShmRingBuffer 在共享内存中跨进程使用，必须显式标注 memory_order:

| 操作 | memory_order | 说明 |
|------|-------------|------|
| 生产者写入 slot 数据 | 普通写 (非原子) | 数据填充阶段 |
| 生产者推进 `prod_pos_` | `memory_order_release` | 确保 slot 数据对消费者可见 |
| 消费者读取 `prod_pos_` | `memory_order_acquire` | 与生产者 release 配对，获取数据可见性 |
| 消费者读取 slot 数据 | 普通读 (非原子) | acquire 之后保证可见 |
| `ref_count` 递减 | `memory_order_acq_rel` | 最后一个 Release 需要看到所有订阅者的写入 |
| `ref_count` 递增 | `memory_order_relaxed` | 仅需原子性，无顺序要求 |

关键实现模式:

```cpp
// 生产者: 写入数据后 release 推进位置
void TryPush(const void* data, uint32_t size) {
    auto pos = prod_pos_.load(std::memory_order_relaxed);
    // ... CAS 竞争 slot ...
    memcpy(&slots_[pos % SlotCount], data, size);
    prod_pos_.store(pos + 1, std::memory_order_release);  // release: 数据可见
}

// 消费者: acquire 读取位置后读数据
bool TryPop(void* data, uint32_t& size) {
    auto prod = prod_pos_.load(std::memory_order_acquire);  // acquire: 配对 release
    auto cons = cons_pos_.load(std::memory_order_relaxed);
    if (cons >= prod) return false;  // 空
    memcpy(data, &slots_[cons % SlotCount], size);
    cons_pos_.store(cons + 1, std::memory_order_release);
    return true;
}

// LoanedMessage::Release: 最后一个订阅者释放 slot
void Release() noexcept {
    if (header_->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // 最后一个订阅者，回收 slot
        // acq_rel 确保所有订阅者的读取都已完成
    }
}
```

性能对比 (ARM Cortex-A72):

| memory_order | 指令 | 开销 (相对 relaxed) |
|-------------|------|-------------------|
| `relaxed` | 普通 load/store | 1x |
| `acquire` | `ldar` (load-acquire) | ~1.2x |
| `release` | `stlr` (store-release) | ~1.2x |
| `seq_cst` | `ldar` + `stlr` + barrier | ~3-5x |

结论: 避免默认 `seq_cst`，显式使用 `acquire/release` 配对，在 ARM 上可获得 3-5 倍性能提升。

**自动传输选择** (借鉴 CyberRT):

```
Node::Publish(msg)
       |
  TransportFactory::Route(sender, receiver)
       |
       ├── 同进程? ──> inproc (AsyncBus, 零拷贝指针传递)
       │
       ├── 同机器不同进程? ──> shm (ShmTransport, 共享内存无锁队列)
       │
       ├── 跨机器? ──> tcp/udp (TcpTransport, 序列化 + 网络 I/O)
       │
       └── 跨机器 (仅串口)? ──> serial (SerialTransport, CRC16 + 帧同步)
```

**与原始 OSP 的对比**:

| 特性 | 原始 OSP | newosp |
|------|----------|--------|
| 进程内通信 | 消息队列 (VOS) | AsyncBus (无锁 MPSC) |
| 进程间通信 | 仅 TCP Socket | 共享内存 (ShmTransport) + TCP/UDP |
| 跨机器通信 | TCP + 心跳 | TCP/UDP + 自动发现 |
| 零拷贝 | 无 | LoanedMessage (SHM) + inproc 指针传递 |
| 传输选择 | 手动配置 | 自动路由 (TransportFactory) |

### 6.10 节点发现与服务 (P2, discovery.hpp / service.hpp) -- 已实现

**目标**: 借鉴 ROS2 Service/Client 和 CyberRT 拓扑发现。

```cpp
namespace osp {

// UDP 多播自动发现
class MulticastDiscovery {
 public:
  void Announce(const char* node_name, const Endpoint& ep);
  void SetOnNodeJoin(void(*cb)(const char* name, const Endpoint& ep));
  void SetOnNodeLeave(void(*cb)(const char* name));
  void Start();
  void Stop();
};

// 请求-响应服务
template <typename Request, typename Response>
class Service {
 public:
  void SetHandler(function_ref<Response(const Request&)> handler);
  void Start(const Endpoint& ep);
};

template <typename Request, typename Response>
class Client {
 public:
  expected<Response, ServiceError> Call(const Request& req, int timeout_ms);
};

}  // namespace osp
```

### 6.11 应用-实例模型 (P1, app.hpp)

**目标**: 兼容原始 OSP 的 CApp/CInstance 两层架构，提供消息驱动的应用框架。

**与原始 OSP 的对应**:

| 原始 OSP | newosp | 说明 |
|----------|--------|------|
| CApp | Application | 拥有消息队列和实例池 |
| CInstance | Instance | 状态机驱动的逻辑实体 |
| MAKEIID(app,ins) | MakeIID(app,ins) | 全局实例 ID 编码 |
| InstanceEntry() | OnMessage() | 消息入口 (事件+状态分发) |
| DaemonInstanceEntry() | Daemon::OnMessage() | 守护实例 |
| NextState() | SetState() | 状态转换 |

```cpp
namespace osp {

// 全局实例 ID 编解码 (兼容原始 OSP)
constexpr uint32_t MakeIID(uint16_t app_id, uint16_t ins_id) {
  return (static_cast<uint32_t>(app_id) << 16) | ins_id;
}
constexpr uint16_t GetAppId(uint32_t iid) { return iid >> 16; }
constexpr uint16_t GetInsId(uint32_t iid) { return iid & 0xFFFF; }

// 特殊实例 ID
constexpr uint16_t kInsPending = 0;
constexpr uint16_t kInsDaemon  = 0xFFFC;
constexpr uint16_t kInsEach    = 0xFFFF;  // 广播

// 实例基类 (状态机驱动)
class Instance {
 public:
  virtual ~Instance() = default;
  virtual void OnMessage(uint16_t event, const void* data, uint32_t len) = 0;

  uint16_t CurState() const noexcept;
  void SetState(uint16_t state) noexcept;
  uint16_t InsId() const noexcept;
};

// 应用 (管理实例池 + 消息队列)
template <uint16_t MaxInstances = 256>
class Application {
 public:
  Application(uint16_t app_id, const char* name);

  // 注册实例工厂
  template <typename T>
  void RegisterInstanceFactory();

  // 创建/销毁实例
  expected<uint16_t, AppError> CreateInstance();
  expected<void, AppError> DestroyInstance(uint16_t ins_id);

  // 消息投递 (投递到本应用的消息队列)
  bool Post(uint16_t ins_id, uint16_t event, const void* data, uint32_t len);

  // 主循环 (从消息队列取消息，分发到实例)
  void Run();
  void Stop();

  uint16_t AppId() const noexcept;
};

}  // namespace osp
```

### 6.12 统一消息投递 (P1, post.hpp)

**目标**: 兼容原始 OSP 的 OspPost() 统一投递接口，自动路由本地/进程间/网络消息。

```cpp
namespace osp {

// 统一投递 (自动路由)
// 本地: 直接投递到 Application 消息队列
// 进程间: 通过 ShmTransport 投递
// 跨机器: 通过 TcpTransport 投递
bool OspPost(uint32_t dst_iid, uint16_t event,
             const void* data, uint32_t len,
             uint16_t dst_node = 0 /* 0=本地 */);

// 同步请求-响应 (替代原始 OspSend)
expected<uint32_t, PostError> OspSendAndWait(
    uint32_t dst_iid, uint16_t event,
    const void* data, uint32_t len,
    void* ack_buf, uint32_t ack_buf_size,
    uint16_t dst_node = 0,
    int timeout_ms = 2000);

}  // namespace osp
```

### 6.13 节点管理器 (P0, node_manager.hpp)

**目标**: 兼容原始 OSP 的 ospnodeman，管理 TCP/SHM 连接、心跳检测、断开通知。

```cpp
namespace osp {

struct NodeManagerConfig {
  uint16_t max_nodes = 64;
  uint32_t heartbeat_interval_ms = 1000;
  uint32_t heartbeat_timeout_count = 3;
};

class NodeManager {
 public:
  explicit NodeManager(const NodeManagerConfig& cfg);

  // 创建监听 (对应 OspCreateTcpNode)
  expected<uint16_t, NodeError> CreateListener(uint16_t port);

  // 连接远程节点 (对应 OspConnectTcpNode)
  expected<uint16_t, NodeError> Connect(const char* host, uint16_t port);

  // 断开节点 (对应 OspDisconnectTcpNode)
  expected<void, NodeError> Disconnect(uint16_t node_id);

  // 注册断开回调 (对应 OspNodeDiscCBReg)
  void OnDisconnect(void(*cb)(uint16_t node_id));

  // 获取节点信息
  bool IsConnected(uint16_t node_id) const;
  uint32_t NodeCount() const;

  void Start();
  void Stop();
};

}  // namespace osp
```

### 6.14 串口传输 (P1, serial_transport.hpp)

**目标**: 集成 [CSerialPort](https://github.com/itas109/CSerialPort) 实现工业级串口通信，作为 Transport 层的可插拔传输实现。适用于板间低速控制、传感器采集、备用通信通道等嵌入式场景。

**P0 架构调整参考**: 本模块涉及以下 P0 调整项:
- [P0-4: Transport 帧缺少 seq_num 和 timestamp](#p0-4-transport-帧缺少-seq_num-和-timestamp) -- 串口帧同样需要序列号和时间戳

**设计理念**:
- NATIVE_SYNC 模式: 无堆分配、无线程、兼容 `-fno-exceptions -fno-rtti`
- 串口 fd 直接加入 IoPoller (epoll) 统一事件循环
- 复用 newosp 消息帧格式 (magic + length + type + sender + payload)
- 帧同步状态机 + CRC16 校验保证数据完整性

**架构**:

```
Board A                              Board B
┌──────────────┐    UART/RS-485    ┌──────────────┐
│ Node::Publish│  ──────────────>  │ Node::Subscribe
│      |       │    /dev/ttyS1     │      ^       │
│ SerialTransport                  │ SerialTransport
│      |       │                   │      |       │
│ CSerialPort  │                   │ CSerialPort  │
│ (NATIVE_SYNC)│                   │ (NATIVE_SYNC)│
└──────┬───────┘                   └──────┬───────┘
       │                                  │
       └──── IoPoller (epoll) ────────────┘
             统一事件循环管理
```

**核心接口**:

```cpp
namespace osp {

// 串口配置
struct SerialConfig {
  char port_name[64];          // "/dev/ttyS0" 或 "/dev/ttyUSB0"
  uint32_t baud_rate;          // 9600 ~ 4000000
  uint8_t data_bits;           // 5/6/7/8
  uint8_t stop_bits;           // 1/2
  uint8_t parity;              // 0=None, 1=Odd, 2=Even
  uint8_t flow_control;        // 0=None, 1=Hardware(RTS/CTS), 2=Software(XON/XOFF)
  uint32_t read_timeout_ms;    // 读超时
  uint32_t frame_max_size;     // 最大帧大小 (默认 OSP_TRANSPORT_MAX_FRAME_SIZE)
  bool enable_crc;             // CRC16 校验 (默认 true)
};

// 串口传输 (实现 Transport 接口语义)
template <typename PayloadVariant>
class SerialTransport {
 public:
  explicit SerialTransport(const SerialConfig& cfg);

  // 打开串口 (串口无客户端/服务端区分)
  expected<void, SerialError> Open();

  // 发送帧 (复用 newosp 消息帧格式 + CRC16)
  expected<void, SerialError> Send(const void* data, uint32_t size);

  // 轮询接收 (帧同步状态机解析)
  void Poll(int timeout_ms);

  // 获取串口 fd (用于加入 IoPoller)
  int GetFd() const noexcept;

  void Close() noexcept;
  bool IsOpen() const noexcept;

 private:
  SerialConfig config_;

  // 接收状态机 (帧解析)
  enum class RxState : uint8_t { kWaitMagic, kWaitHeader, kWaitPayload, kWaitCrc };
  RxState rx_state_;
  uint8_t rx_buf_[OSP_TRANSPORT_MAX_FRAME_SIZE];
  uint32_t rx_pos_;
};

}  // namespace osp
```

**帧格式** (扩展网络帧，增加 CRC):

```
┌─────────────┬─────────────┬──────────┬───────────┬──────────┬──────────┐
│ magic (4B)  │ msg_len (4B)│ type (2B)│ sender(4B)│ payload  │ crc16(2B)│
│ 0x4F535000  │ total bytes │ variant  │ node id   │ N bytes  │ 校验和   │
└─────────────┴─────────────┴──────────┴───────────┴──────────┴──────────┘
```

**自动传输选择扩展**:

```
TransportFactory::Route(sender, receiver)
       |
       ├── 同进程? ──> inproc (AsyncBus)
       ├── 同机器不同进程? ──> shm (ShmTransport)
       ├── 跨机器 (有网络)? ──> tcp/udp (RemoteTransport)
       └── 跨机器 (仅串口)? ──> serial (SerialTransport)
```

**与 IoPoller 集成**:

```cpp
// NATIVE_SYNC 模式下，串口 fd 加入 epoll 统一事件循环
auto serial = SerialTransport<Payload>(serial_cfg);
serial.Open();
io_poller.Add(serial.GetFd(), EPOLLIN, &serial);

// 统一事件循环 (串口 + TCP + UDP 同一线程处理)
while (running) {
    auto n = io_poller.Wait(events, max_events, timeout_ms);
    for (uint32_t i = 0; i < n; ++i) {
        if (events[i].ctx == &serial) serial.Poll(0);
        else if (events[i].ctx == &tcp) tcp.Poll(0);
    }
}
```

**性能特征**:

| 波特率 | 理论吞吐 | 实际吞吐 (含帧头+CRC) | 适合的消息大小 |
|--------|---------|----------------------|--------------|
| 9600 | 960 B/s | ~800 B/s | <100 B |
| 115200 | 11.5 KB/s | ~10 KB/s | <1 KB |
| 921600 | 92 KB/s | ~80 KB/s | <4 KB |
| 4000000 | 400 KB/s | ~350 KB/s | <16 KB |

### 6.15 QoS 服务质量 (P0, qos.hpp)

**目标**: 借鉴 ROS2 QoS 策略和 CyberRT QoS 配置，提供简化但实用的服务质量控制。在嵌入式场景中，不同消息有不同的可靠性和实时性要求 (传感器数据可丢 vs 控制指令必达)，缺少 QoS 会导致系统无法区分关键消息和普通消息。

**设计理念**:

| 对比 | ROS2 DDS QoS | CyberRT QoS | newosp QoS |
|------|-------------|-------------|------------|
| 复杂度 | 22+ 策略参数 | 简化 (channel 级) | 精简 (5 个核心参数) |
| 可靠性 | Reliable / BestEffort | 依赖 SHM/TCP 选择 | Reliable / BestEffort |
| 历史 | KeepLast / KeepAll + depth | 固定 depth | KeepLast + depth |
| Deadline | 支持 | 不支持 | 支持 (超时回调) |
| Liveliness | 支持 (复杂) | 不支持 | 不支持 (由心跳层处理) |

**核心接口**:

```cpp
namespace osp {

// QoS 可靠性策略
enum class Reliability : uint8_t {
  kBestEffort,  // 尽力投递，允许丢失 (适合传感器数据、视频帧)
  kReliable     // 可靠投递，确保送达 (适合控制指令、状态变更)
};

// QoS 历史策略
enum class HistoryPolicy : uint8_t {
  kKeepLast,    // 仅保留最近 depth 条 (默认)
  kKeepAll      // 保留所有 (受限于队列容量)
};

// QoS 配置 (POD，可静态初始化)
struct QosProfile {
  Reliability reliability = Reliability::kBestEffort;
  HistoryPolicy history = HistoryPolicy::kKeepLast;
  uint16_t depth = 10;           // 历史深度 (KeepLast 模式)
  uint32_t deadline_ms = 0;      // 消息到达截止时间 (0=不检测)
  uint32_t lifespan_ms = 0;      // 消息有效期 (0=永不过期)
};

// 预定义 QoS 配置 (constexpr，零开销)
inline constexpr QosProfile kQosSensorData{
    Reliability::kBestEffort, HistoryPolicy::kKeepLast, 5, 0, 100
};
inline constexpr QosProfile kQosControlCommand{
    Reliability::kReliable, HistoryPolicy::kKeepLast, 20, 50, 0
};
inline constexpr QosProfile kQosSystemStatus{
    Reliability::kReliable, HistoryPolicy::kKeepLast, 1, 1000, 0
};
inline constexpr QosProfile kQosDefault{};

// Deadline 超时回调
using DeadlineMissedCallback = void(*)(const char* topic_name, uint32_t elapsed_ms);

}  // namespace osp
```

**与 Node/Transport 集成**:

```cpp
// 发布者指定 QoS
node.Advertise<SensorData>("lidar", kQosSensorData);
node.Advertise<ControlCmd>("motor_cmd", kQosControlCommand);

// 订阅者指定 QoS (与发布者 QoS 兼容性检查)
node.Subscribe<SensorData>("lidar", on_lidar, kQosSensorData);
node.Subscribe<ControlCmd>("motor_cmd", on_cmd, kQosControlCommand);

// Deadline 超时检测
node.SetDeadlineMissedCallback([](const char* topic, uint32_t elapsed_ms) {
    LOG_WARN("deadline missed: topic=%s elapsed=%u ms", topic, elapsed_ms);
});
```

**QoS 对 Transport 选择的影响**:

```
TransportFactory::Route(sender, receiver, qos)
       |
       ├── Reliable + 跨机器? ──> TCP (保证有序可靠)
       ├── BestEffort + 跨机器? ──> UDP (低延迟，允许丢包)
       ├── Reliable + 串口? ──> Serial + ACK/重传
       └── BestEffort + 串口? ──> Serial (仅 CRC 校验)
```

**QoS 兼容性矩阵** (发布者 vs 订阅者):

| 发布者 \ 订阅者 | BestEffort | Reliable |
|----------------|-----------|----------|
| BestEffort | 兼容 | 不兼容 (订阅者要求更高) |
| Reliable | 兼容 (降级) | 兼容 |

不兼容时 `Subscribe` 返回错误，避免运行时静默丢消息。

### 6.16 生命周期节点 (P0, lifecycle_node.hpp)

**目标**: 借鉴 ROS2 Lifecycle Node (managed node) 状态机，提供确定性的资源分配和释放。嵌入式系统需要精确控制模块的启停顺序、资源分配时机、故障恢复策略。

**设计理念**:

| 对比 | ROS2 Lifecycle | newosp Lifecycle |
|------|---------------|-----------------|
| 状态数 | 4 主状态 + 6 过渡状态 | 4 主状态 + 4 过渡回调 |
| 过渡触发 | Service 调用 (DDS) | 直接方法调用 (本地) |
| 错误处理 | 过渡状态 + ErrorProcessing | 回调返回值 + on_error |
| 复杂度 | 高 (DDS 依赖) | 低 (纯本地状态机) |

**状态机**:

```
                    ┌──────────────────────────────────────────┐
                    │                                          │
                    v                                          │
  [Unconfigured] ──configure──> [Inactive] ──activate──> [Active]
        ^                           |                       |
        │                           │                       │
        └────────cleanup────────────┘                       │
        │                                                   │
        └──────────────────shutdown──────────────────────────┘
        │                                                   │
        v                                                   v
  [Finalized] <─────────────────────────────────────────────┘
```

状态说明:

| 状态 | 说明 | 资源状态 |
|------|------|---------|
| Unconfigured | 初始状态，节点已创建但未配置 | 无资源分配 |
| Inactive | 已配置，资源已分配，但不处理消息 | Publisher/Subscriber 已创建 |
| Active | 正常工作，处理消息 | 全部活跃 |
| Finalized | 终态，节点即将销毁 | 全部释放 |

**核心接口**:

```cpp
namespace osp {

enum class LifecycleState : uint8_t {
  kUnconfigured,
  kInactive,
  kActive,
  kFinalized
};

// 过渡回调返回值
enum class CallbackReturn : uint8_t {
  kSuccess,   // 过渡成功
  kFailure,   // 过渡失败，回退到前一状态
  kError      // 严重错误，进入 on_error 处理
};

// 生命周期节点 (继承自 Node)
template <typename PayloadVariant>
class LifecycleNode : public Node<PayloadVariant> {
 public:
  using Node<PayloadVariant>::Node;  // 继承构造函数

  // 状态过渡触发
  CallbackReturn Configure();    // Unconfigured -> Inactive
  CallbackReturn Activate();     // Inactive -> Active
  CallbackReturn Deactivate();   // Active -> Inactive
  CallbackReturn Cleanup();      // Inactive -> Unconfigured
  CallbackReturn Shutdown();     // Any -> Finalized

  // 当前状态查询
  LifecycleState CurrentState() const noexcept;

 protected:
  // 子类重写: 过渡回调 (确定性资源管理)
  virtual CallbackReturn on_configure()  { return CallbackReturn::kSuccess; }
  virtual CallbackReturn on_activate()   { return CallbackReturn::kSuccess; }
  virtual CallbackReturn on_deactivate() { return CallbackReturn::kSuccess; }
  virtual CallbackReturn on_cleanup()    { return CallbackReturn::kSuccess; }
  virtual CallbackReturn on_shutdown()   { return CallbackReturn::kSuccess; }
  virtual CallbackReturn on_error(LifecycleState previous_state) {
    return CallbackReturn::kSuccess;
  }

 private:
  LifecycleState state_{LifecycleState::kUnconfigured};
};

}  // namespace osp
```

**使用示例**:

```cpp
class LidarNode : public LifecycleNode<SensorPayload> {
 protected:
  CallbackReturn on_configure() override {
    // 分配资源: 创建 publisher, 打开设备
    Advertise<PointCloud>("lidar_points", kQosSensorData);
    fd_ = open("/dev/lidar0", O_RDONLY);
    return fd_ >= 0 ? CallbackReturn::kSuccess : CallbackReturn::kFailure;
  }

  CallbackReturn on_activate() override {
    // 开始工作: 启动数据采集
    active_ = true;
    return CallbackReturn::kSuccess;
  }

  CallbackReturn on_deactivate() override {
    // 暂停工作: 停止采集，但保留资源
    active_ = false;
    return CallbackReturn::kSuccess;
  }

  CallbackReturn on_cleanup() override {
    // 释放资源: 关闭设备
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
    return CallbackReturn::kSuccess;
  }

 private:
  int fd_ = -1;
  bool active_ = false;
};

// 系统启动: 逐步初始化
LidarNode lidar(bus, "lidar", 1);
lidar.Configure();   // 分配资源
lidar.Activate();    // 开始工作

// 运行时降级 (如省电模式)
lidar.Deactivate();  // 暂停，保留资源
lidar.Activate();    // 恢复工作

// 系统关闭
lidar.Shutdown();    // 释放所有资源
```

**与 Executor 集成**:

```cpp
// RealtimeExecutor 管理 LifecycleNode 的批量启停
RealtimeExecutor executor(rt_cfg);
executor.AddNode(lidar);
executor.AddNode(camera);
executor.AddNode(motor);

// 按依赖顺序启动
executor.ConfigureAll();  // 所有节点 Configure
executor.ActivateAll();   // 所有节点 Activate
executor.Spin();          // 运行

// 故障恢复: 单个节点重启
motor.Deactivate();
motor.Cleanup();
motor.Configure();
motor.Activate();
```

### 6.17 HSM 驱动节点管理 (P0, node_manager_hsm.hpp) -- 已实现

**目标**: 为每个 TCP/SHM 连接提供独立的 HSM 状态机，管理 Connected → Suspect → Disconnected 生命周期。

**状态机**:

Connected ──(心跳超时)──> Suspect ──(超时次数达阈值)──> Disconnected
    ^                       |
    └───(心跳恢复)──────────┘

**事件枚举**: `kEvtHeartbeatReceived`, `kEvtHeartbeatTimeout`, `kEvtDisconnect`, `kEvtReconnect`

**关键特性**:
- 每连接独立 HSM 实例 (最多 64 个)
- Suspect 状态支持可配置超时次数
- 断开/恢复回调通知
- 15 test cases, ASan/TSan/UBSan clean

### 6.18 HSM 驱动服务生命周期 (P1, service_hsm.hpp) -- 已实现

**目标**: 管理 Service 服务端连接生命周期: Idle → Listening → Active → Error/ShuttingDown。

**状态机**:

Idle ──(Start)──> Listening ──(ClientConnected)──> Active
                                                     |
                                              (Error) |  (Stop)
                                                     v      v
                                                   Error  ShuttingDown
                                                     |
                                              (Recover)
                                                     v
                                                   Idle

**事件枚举**: `kSvcEvtStart`, `kSvcEvtClientConnected`, `kSvcEvtClientDisconnected`, `kSvcEvtError`, `kSvcEvtStop`, `kSvcEvtRecover`

**关键特性**:
- 直接持有 `StateMachine<ServiceHsmContext, 8>` 成员
- 错误回调 + 关停回调 (函数指针 + context)
- mutex 保护状态转换
- 10 test cases

### 6.19 HSM 驱动节点发现 (P1, discovery_hsm.hpp) -- 已实现

**目标**: 管理节点发现流程: Idle → Announcing → Discovering → Stable/Degraded。

**状态机**:

Idle ──(Start)──> Announcing ──(NodeFound)──> Discovering
                                                  |
                                    (达到阈值)     |  (节点丢失)
                                                  v      v
                                               Stable  Degraded
                                                  |      |
                                           (NodeLost)  (恢复)
                                                  v      v
                                              Degraded  Stable

**事件枚举**: `kDiscEvtStart`, `kDiscEvtNodeFound`, `kDiscEvtNodeLost`, `kDiscEvtNetworkStable`, `kDiscEvtNetworkDegraded`, `kDiscEvtStop`

**关键特性**:
- 直接持有 `StateMachine<DiscoveryHsmContext, 8>` 成员
- 可配置稳定阈值 (`stable_threshold`)
- 稳定/降级回调通知
- 10 test cases

### 6.20 网络层封装 (P0, net.hpp) -- 已实现

**目标**: sockpp 集成层，提供 `osp::expected` 错误处理的网络接口。

**核心类型**:

| 类型 | 说明 |
|------|------|
| `TcpClient` | TCP 客户端连接 (sockpp::tcp_connector 封装) |
| `TcpServer` | TCP 服务端监听 (sockpp::tcp_acceptor 封装) |
| `UdpPeer` | UDP 收发 (sockpp::udp_socket 封装) |
| `NetError` | 网络错误枚举 (ConnectFailed/BindFailed/SendFailed 等) |

**条件编译**: 仅在 `OSP_HAS_SOCKPP` 定义时可用。11 test cases。

### 6.21 传输工厂 (P0, transport_factory.hpp) -- 已实现

**目标**: 自动传输选择策略 (借鉴 CyberRT)，根据配置自动选择 inproc/shm/tcp。

**核心接口**:

```cpp
enum class TransportType : uint8_t { kInproc, kShm, kTcp, kAuto };

struct TransportConfig {
  TransportType type = TransportType::kAuto;
  char remote_host[64] = "127.0.0.1";
  uint16_t remote_port = 0;
  char shm_channel_name[64] = "";
};

class TransportFactory {
 public:
  static TransportType Detect(const TransportConfig& cfg);
  static bool IsLocalHost(const char* host);
};
```

**选择策略**: Auto 模式下: 有 SHM channel name → kShm; localhost → kShm; 远程地址 → kTcp。10 test cases。

### 6.22 代码生成工具 (ospgen)

**目标**: YAML 驱动的编译期代码生成，零运行时开销。

**工具链**: Python3 + PyYAML + Jinja2 → C++ 头文件

**生成目标**:

| 类型 | YAML 定义 | 生成内容 |
|------|----------|---------|
| 消息/事件 | `defs/*.yaml` (events + messages) | 事件枚举、POD 结构体、`std::variant` 类型、`static_assert` |
| 节点拓扑 | `defs/topology.yaml` (nodes) | `constexpr` 节点 ID/名称常量 |

**CMake 集成**: `OSP_CODEGEN=ON` 时通过 `add_custom_command` + `DEPENDS` 增量生成。

**使用示例**:

```bash
python3 tools/ospgen.py --input defs/protocol_messages.yaml \
  --output include/generated/ --templates tools/templates/
```

---

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
| TcpSocket (per conn) | ~128 B | 0 | 0 |
| IoPoller (64 fds) | ~2 KB | <1 KB | 0 |
| ConnectionPool<32> | ~4 KB | 0 | 0 |
| StateMachine<Ctx,16> | ~1 KB | 0 | 0 |
| BehaviorTree<Ctx,32> | ~4 KB | 0 | 0 |
| StaticExecutor | <1 KB | 0 | 1 |
| PinnedExecutor | <1 KB | 0 | 1 |
| TcpTransport (per conn) | ~256 B | 0 | 0 |
| FusedSubscription | <1 KB | 0 | 0 |
| TimeSynchronizer | <1 KB | 0 | 0 |
| LightSemaphore | <128 B | 0 | 0 |
| **典型总计** | **~466 KB** | **~22 KB** | **11** |

### 7.2 规划模块 (估算)

| 模块 | 静态内存 | 堆内存 | 线程数 |
|------|----------|--------|--------|
| ShmRingBuffer (per channel, 256 slots x 4KB) | ~1 MB (共享内存) | 0 | 0 |
| ShmChannel (per channel) | ~256 B | 0 | 0 |
| ShmTransport | ~1 KB | 0 | 0 |
| SerialTransport (per port) | ~2 KB | 0 | 0 |
| MulticastDiscovery | ~2 KB | ~1 KB | 1 |
| Service/Client (per service) | ~512 B | 0 | 0 |
| Application<256> | ~8 KB | 0 | 1 |
| Instance (per instance) | ~128 B | 0 | 0 |
| NodeManager (64 nodes) | ~4 KB | ~2 KB | 2 |
| QosProfile (per topic) | ~16 B | 0 | 0 |
| LifecycleNode (per node) | ~1 KB | 0 | 0 |
| RealtimeExecutor | <1 KB | 0 | 1 |
| NodeManagerHsm (64 nodes) | ~8 KB | 0 | 1 |
| HsmService (per service) | ~1 KB | 0 | 0 |
| HsmDiscovery | ~1 KB | 0 | 0 |
| TcpClient/TcpServer (net.hpp) | ~256 B | 0 | 0 |
| TransportFactory | 0 (无状态) | 0 | 0 |

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
| HSM 事件枚举 | k\<Module\>Evt\<Name\> | `kSvcEvtStart`, `kDiscEvtNodeFound`, `kEvtHeartbeatReceived` |
| 宏 | OSP_UPPER_CASE | `OSP_LOG_INFO`, `OSP_ASSERT` |
| 模板参数 | PascalCase | `PayloadVariant`, `BlockSize` |

### 8.3 CI 流水线

| 阶段 | 内容 |
|------|------|
| **build-and-test** | Ubuntu, Debug + Release |
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

### Phase F: 扩展模块 -- Socket 与 I/O (已完成)

57. **socket.hpp -- TcpSocket RAII**: fd 封装、Connect/Send/Recv/Close、move 语义
58. **socket.hpp -- UdpSocket**: 无连接 Socket、SendTo/RecvFrom
59. **socket.hpp -- TcpListener**: bind/listen/accept 循环、expected 接口
60. **test_socket.cpp**: 连接测试、收发验证、错误处理
61. **io_poller.hpp**: epoll (Linux) / kqueue (macOS) 统一抽象
62. **io_poller.hpp -- 事件循环集成**: Add/Modify/Remove/Wait 接口
63. **test_io_poller.cpp**: 多 fd 就绪、超时、边缘触发

### Phase G: 扩展模块 -- 透明网络传输 (已完成)

64. **transport.hpp -- Endpoint/TransportType**: 端点定义、传输类型枚举
65. **transport.hpp -- Serializer<T>**: POD memcpy 默认序列化、模板特化扩展点
66. **transport.hpp -- 消息帧协议**: magic + length + type_index + sender_id + payload
67. **transport.hpp -- TcpTransport**: TCP 传输实现、连接管理、分帧收发
68. **transport.hpp -- UdpTransport**: UDP 数据报传输、分片重组 (MTU 感知)
69. **transport.hpp -- NetworkNode**: 继承 Node，透明远程发布/订阅
70. **transport.hpp -- 本地+远程统一分发**: Publish 同时投递 AsyncBus + 远程端点
71. **test_transport.cpp**: 本地回环、TCP 收发、序列化正确性、帧解析
72. **transport.hpp -- 节点发现 (Phase 2)**: 多播自动发现、端点注册表

### Phase H: 扩展模块 -- 连接管理 (已完成)

73. **connection.hpp -- ConnectionPool**: 固定容量连接池、ConnectionId 强类型
74. **connection.hpp -- 连接生命周期**: Add/Remove/ForEach、超时清理
75. **test_connection.cpp**: 池满测试、ID 回收、遍历一致性

### Phase I: 扩展模块 -- 状态机与行为树 (已完成)

76. **hsm.hpp**: 集成 hsm-cpp State/StateMachine 模板
77. **hsm.hpp -- 消息总线桥接**: Bus 消息 -> Event(id) -> Dispatch()
78. **test_hsm.cpp**: 层次状态转换、LCA 路径、guard 条件
79. **bt.hpp**: 集成 bt-cpp Node/BehaviorTree 模板
80. **bt.hpp -- 异步任务集成**: 叶节点通过 WorkerPool 提交任务
81. **test_bt.cpp**: Sequence/Selector/Parallel 执行、异步完成

### Phase J: 扩展模块 -- Executor 调度 (已完成)

82. **executor.hpp -- SingleThreadExecutor**: 单线程轮询多节点
83. **executor.hpp -- StaticExecutor**: 固定节点-线程映射、确定性调度
84. **executor.hpp -- PinnedExecutor**: CPU 核心绑定、最低延迟
85. **test_executor.cpp**: 节点调度公平性、停止安全性

### Phase K: 扩展模块 -- 高级特性 (已完成)

86. **data_fusion.hpp -- FusedSubscription**: 多消息对齐触发、时间窗口
87. **data_fusion.hpp -- TimeSynchronizer**: 时间戳对齐策略
88. **test_data_fusion.cpp**: 多源融合、超时处理 (10 test cases)
89. **semaphore.hpp**: LightSemaphore/BinarySemaphore/PosixSemaphore (12 test cases)
90. **优化 bus.hpp**: SpinLock + SharedSpinLock 指数退避、批量预取 (__builtin_prefetch)
91. **优化 worker_pool.hpp**: AdaptiveBackoff 三阶段退避 (spin -> yield -> sleep)

### Phase L: 集成验证 (已完成)

92. **全模块集成测试**: 7 个跨模块场景 (Node+WorkerPool+Bus, HSM+Bus, BT+Node, FusedSubscription+Node, Executor+Node, ConnectionPool+Timer, Node+Timer+Bus)
93. **Sanitizer 全量验证**: ASan + UBSan + TSan 全部通过
    - **527 测试用例**: 34 个头文件全覆盖，含 18 个测试文件 + 1 个集成测试
94. **原始 OSP 代码分析**: osp_legacy_analysis.md -- 从 demo/test 入手的完整功能文档

### Phase M: 共享内存 IPC -- 进程间通信 (已完成, 269 tests, d2f5450)

> 借鉴 [cpp-ipc](https://github.com/mutouyun/cpp-ipc) 的共享内存无锁队列、
> [ROS2 Fast DDS SHM Transport](https://www.eprosima.com/index.php/products-all/tools/eprosima-shared-memory) 的零拷贝 loaned message、
> [CyberRT](https://developer.apollo.auto/cyber.html) 的自动传输选择策略。
>
> 原始 OSP 的节点间通信仅支持 TCP Socket，newosp 需要同时支持进程内 (inproc)、
> 进程间同机 (shm)、跨机器 (tcp/udp) 三种传输，并自动选择最优路径。

95. **shm_transport.hpp -- SharedMemorySegment**: POSIX shm_open/mmap RAII 封装、命名共享内存段
96. **shm_transport.hpp -- ShmRingBuffer**: 共享内存中的无锁 SPSC/MPSC 环形缓冲区 (借鉴 cpp-ipc 的 CAS 设计)
97. **shm_transport.hpp -- ShmChannel**: 命名通道抽象、生产者/消费者端点、waiter 通知 (eventfd/futex)
98. **shm_transport.hpp -- ShmTransport**: 实现 Transport 接口、Bind/Connect/Send/Poll
99. **shm_transport.hpp -- 零拷贝 LoanedMessage**: 借鉴 ROS2 loaned_messages API，发布者直接写入共享内存 (仅限 POD 类型)
100. **test_shm_transport.cpp**: 单进程 SHM 回环、多线程并发读写、通道创建/销毁
101. **transport.hpp -- TransportFactory**: 自动传输选择策略 (借鉴 CyberRT) -- 已完成 (transport_factory.hpp)
    - 同进程同 PayloadVariant: inproc (AsyncBus 零拷贝)
    - 同机器不同进程: shm (共享内存无锁队列)
    - 跨机器: tcp/udp (网络传输)
102. **transport.hpp -- TopicRegistry**: 基于主题名的端点注册表、支持类型路由 + 主题路由混合模式
103. **test_shm_ipc.cpp**: 跨进程通信集成测试 (fork + shm_channel)

### Phase N: sockpp 深度集成 -- 网络传输重构 (已完成, 280 tests, 25e7591)

> Fork [sockpp](https://github.com/fpagliughi/sockpp) 到 DeguiLiu/sockpp，
> 在 fork 分支上按嵌入式需求优化 (去除异常依赖、减少堆分配、添加 expected<> 接口)。
> socket.hpp / transport.hpp 的网络路径深度依赖 sockpp，替代当前的裸 POSIX 封装。

104. **Fork sockpp**: 创建 DeguiLiu/sockpp fork，建立 osp-embedded 分支
105. **sockpp 嵌入式适配**: -fno-exceptions 兼容、expected<> 错误返回、减少 std::string 使用
106. **CMake 集成**: FetchContent 从 fork 拉取 sockpp，作为 osp 的可选依赖
107. **socket.hpp 重构**: TcpSocket/UdpSocket/TcpListener 底层切换为 sockpp::tcp_socket 等
108. **transport.hpp 网络路径重构**: TcpTransport/UdpTransport 使用 sockpp 的 stream_socket/datagram_socket
109. **io_poller.hpp 集成**: sockpp 的 socket fd 与 IoPoller 无缝配合
110. **shell.hpp 统一**: DebugShell 的 TCP 监听切换为 sockpp (与 socket.hpp 共享依赖)
111. **test_sockpp_integration.cpp**: sockpp 集成回归测试

### Phase O: P0 架构加固 -- QoS / Lifecycle / 实时调度 / 节点管理 (已实现)

> P0 优先级。基于 ROS2 rclcpp、CyberRT、iceoryx、eCAL 等中间件的最佳实践，
> 针对嵌入式 ARM-Linux 实时场景，补齐 P0 级别的架构缺陷。
> 同时实现 P0 级别的节点管理器 (原始 OSP ospnodeman 核心功能)。

112. **qos.hpp -- QosProfile**: Reliability/HistoryPolicy/depth/deadline_ms/lifespan_ms 配置
113. **qos.hpp -- 预定义 QoS**: kQosSensorData/kQosControlCommand/kQosSystemStatus constexpr 配置
114. **qos.hpp -- QoS 兼容性检查**: 发布者/订阅者 QoS 匹配验证，不兼容时返回错误
115. **qos.hpp -- Deadline 检测**: 基于 TimerScheduler 的超时回调
116. **lifecycle_node.hpp -- LifecycleState**: Unconfigured/Inactive/Active/Finalized 状态枚举
117. **lifecycle_node.hpp -- LifecycleNode**: 继承 Node，on_configure/on_activate/on_deactivate/on_cleanup/on_shutdown 虚函数
118. **lifecycle_node.hpp -- 状态过渡验证**: 非法过渡返回错误 (如 Unconfigured -> Active)
119. **executor.hpp -- RealtimeConfig**: sched_policy/sched_priority/lock_memory/stack_size/cpu_affinity
120. **executor.hpp -- RealtimeExecutor**: SCHED_FIFO + mlockall + 优先级队列 + 周期调度
121. **executor.hpp -- Executor + LifecycleNode 集成**: ConfigureAll/ActivateAll 批量生命周期管理
122. **node_manager.hpp -- NodeManager**: TCP/SHM 连接管理、心跳检测、断开通知回调
123. **node_manager.hpp -- 心跳机制**: 可配置间隔和超时次数，断开时广播 OSP_DISCONNECT
124. **test_qos.cpp**: QoS 兼容性矩阵、Deadline 超时、Transport 选择影响
125. **test_lifecycle_node.cpp**: 状态过渡、非法过渡拒绝、资源分配/释放验证
126. **test_realtime_executor.cpp**: 优先级调度、周期精度、CPU 亲和性
127. **test_node_manager.cpp**: 连接建立、心跳超时、断开通知
128. **性能基准回归**: 全传输路径 (inproc/shm/tcp) 吞吐量/延迟基线

### Phase P: P1 功能扩展 -- OSP 兼容 / 串口 / 统一投递 (已实现)

> P1 优先级。确保 newosp 覆盖原始 OSP (osp_legacy_analysis.md) 的核心功能。
> 表现形式不同，但功能等价。仅支持 Linux 平台 (含 ARM-Linux)。

129. **app.hpp -- Application 应用抽象**: 对应原始 OSP 的 CApp，拥有消息队列和实例池
130. **app.hpp -- Instance 实例抽象**: 对应原始 OSP 的 CInstance，状态机驱动的逻辑实体
131. **app.hpp -- InstanceEntry 消息分发**: 事件+状态 二维分发表，替代 switch-case
132. **app.hpp -- MAKEIID/GETAPP/GETINS**: 全局实例 ID 编解码，兼容原始 OSP 寻址模型
133. **post.hpp -- OspPost 统一投递**: 本地/远程/广播三种投递方式，自动路由
134. **post.hpp -- 同步消息 (SendAndWait)**: 借鉴 ROS2 Service，替代原始 OspSend()
135. **serial_transport.hpp -- SerialTransport**: CSerialPort NATIVE_SYNC 集成、帧同步状态机、CRC16 校验
136. **serial_transport.hpp -- IoPoller 集成**: 串口 fd 加入 epoll 统一事件循环
137. **test_app.cpp**: Application/Instance 生命周期、消息分发、状态转换
138. **test_post.cpp**: 本地投递、远程投递、广播投递、同步消息
139. **test_serial_transport.cpp**: 串口回环测试、帧解析、CRC 校验

### Phase Q: P2 高级特性 -- 节点发现 / 服务 (已实现)

> P2 优先级。借鉴 ROS2 的 Service/Client 模式和 CyberRT 的拓扑自动发现。
> 这些特性对核心功能非必需，但对完整的分布式系统至关重要。

140. **discovery.hpp -- MulticastDiscovery**: UDP 多播节点自动发现、心跳保活
141. **discovery.hpp -- StaticDiscovery**: 配置文件驱动的静态端点表
142. **service.hpp -- Service/Client**: 请求-响应模式 (借鉴 ROS2 Service)
143. **test_discovery.cpp**: 多播发现、节点上下线通知
144. **test_service.cpp**: 请求-响应、超时处理、并发调用

### Phase R: HSM 驱动模块 + 代码生成 (已实现, 527 tests)

> HSM 驱动的高级状态管理模块，为 service、discovery、node_manager 提供独立的
> 状态机生命周期管理。同时引入 YAML 驱动的编译期代码生成工具 ospgen。

145. **node_manager_hsm.hpp**: HSM 驱动节点心跳状态机 (Connected/Suspect/Disconnected)
146. **service_hsm.hpp**: HSM 驱动服务生命周期 (Idle/Listening/Active/Error/ShuttingDown)
147. **discovery_hsm.hpp**: HSM 驱动节点发现流程 (Idle/Announcing/Discovering/Stable/Degraded)
148. **net.hpp**: sockpp 集成层 (TcpClient/TcpServer/UdpPeer + osp::expected)
149. **transport_factory.hpp**: 自动传输选择 (inproc/shm/tcp)
150. **tools/ospgen.py**: YAML → C++ 代码生成器 (PyYAML + Jinja2)
151. **tools/templates/**: Jinja2 模板 (messages.hpp.j2, topology.hpp.j2)
152. **defs/**: YAML 定义文件 (protocol_messages, sensor_messages, topology)
153. **examples/codegen_demo.cpp**: 代码生成 3 部分演示
154. **test_node_manager_hsm.cpp**: 15 test cases
155. **test_service_hsm.cpp**: 10 test cases
156. **test_discovery_hsm.cpp**: 10 test cases
157. **test_net.cpp**: 11 test cases
158. **test_transport_factory.cpp**: 10 test cases

---

### 原始 OSP 功能覆盖对照表

| 原始 OSP 功能 | newosp 对应 | 状态 | 备注 |
|--------------|------------|------|------|
| **ospvos -- 线程管理** | std::thread + worker_pool.hpp | 已完成 | C++17 标准线程 |
| **ospvos -- 信号量** | semaphore.hpp | 已完成 | LightSemaphore/PosixSemaphore |
| **ospvos -- 消息队列** | bus.hpp (AsyncBus) | 已完成 | 无锁 MPSC 替代 VOS 消息队列 |
| **ospvos -- Socket** | socket.hpp + sockpp (Phase N) | 已完成 | RAII 封装 |
| **ospvos -- 串口** | serial_transport.hpp (CSerialPort) | 已完成 | NATIVE_SYNC 模式，IoPoller 集成 |
| **osppost -- 本地投递** | bus.hpp Publish() | 已完成 | 类型路由替代 ID 路由 |
| **osppost -- 远程投递** | transport.hpp NetworkNode | 已完成 | TCP/UDP 透明传输 |
| **osppost -- 广播投递** | bus.hpp (所有订阅者) | 已完成 | 类型订阅天然广播 |
| **osppost -- 别名投递** | -- | 不实现 | 类型路由更安全，应用层可维护名称映射 |
| **osppost -- 同步消息** | post.hpp OspSendAndWait + service.hpp | 已完成 | ResponseChannel 阻塞等待回复，支持超时 |
| **ospnodeman -- TCP 连接** | connection.hpp + transport.hpp | 已完成 | ConnectionPool 管理 |
| **ospnodeman -- 心跳检测** | node_manager.hpp + node_manager_hsm.hpp | 已完成 | TCP 心跳 + HSM 状态机驱动 |
| **ospnodeman -- 断开通知** | node_manager.hpp + node_manager_hsm.hpp | 已完成 | 回调通知 + HSM Connected/Suspect/Disconnected |
| **ospnodeman -- HSM 状态机** | node_manager_hsm.hpp (Phase O) | 已完成 | 每连接独立 HSM，三状态生命周期管理 |
| **ospsch -- 调度器** | executor.hpp | 已完成 | Single/Static/Pinned 三种模式 |
| **ospsch -- 内存池** | mem_pool.hpp | 已完成 | FixedPool + ObjectPool |
| **osptimer -- 定时器** | timer.hpp | 已完成 | TimerScheduler 后台线程 |
| **osptimer -- 绝对定时器** | -- | 不实现 | 相对定时器 + 应用层调度 |
| **osplog -- 日志** | log.hpp | 已完成 | 编译期过滤 + stderr |
| **osplog -- 文件轮转** | -- | 未来扩展 | 可集成 zlog |
| **ospteleserver -- Telnet** | shell.hpp (DebugShell) | 已完成 | TCP Telnet + 命令注册 |
| **osptest -- 测试框架** | Catch2 v3.5.2 | 已完成 | 现代测试框架替代 |
| **CApp -- 应用** | app.hpp Application (Phase P) | 已完成 | 消息队列 + 实例池 + ResponseChannel |
| **CInstance -- 实例** | app.hpp Instance (Phase P) | 已完成 | 状态机驱动 + Reply() 同步回复 |
| **-- QoS 服务质量** | qos.hpp (Phase O) | 已完成 | 简化版 QoS，5 个核心参数 |
| **-- 生命周期节点** | lifecycle_node.hpp (Phase O) | 已完成 | 借鉴 ROS2 Lifecycle Node |
| **-- 实时调度** | executor.hpp RealtimeExecutor (Phase O) | 已完成 | SCHED_FIFO + mlockall + 优先级队列 |
| **CMessage -- 消息** | MessageEnvelope (bus.hpp) | 已完成 | header + variant payload |
| **COspStack -- 栈内存** | mem_pool.hpp FixedPool | 已完成 | 固定块分配 |
| **进程内通信** | AsyncBus (inproc, 零拷贝) | 已完成 | 无锁 MPSC |
| **进程间通信 (同机)** | ShmTransport (Phase M) | 已完成 | 共享内存无锁队列 |
| **网络间通信 (跨机)** | TcpTransport/UdpTransport | 已完成 | sockpp 重构 (Phase N) |
| **自动传输选择** | transport_factory.hpp | 已完成 | inproc/shm/tcp 自动路由 |
| **节点发现** | discovery.hpp (Phase Q) | 已完成 | UDP 多播 + 静态配置 |
| **字节序转换** | Serializer<T> (transport.hpp) | 已完成 | POD memcpy，可扩展 protobuf |
| **HSM 驱动服务生命周期** | service_hsm.hpp (Phase R) | 已完成 | Idle/Listening/Active/Error 四状态 |
| **HSM 驱动节点发现** | discovery_hsm.hpp (Phase R) | 已完成 | Idle/Announcing/Stable/Degraded 四状态 |
| **YAML 代码生成** | ospgen.py (tools/) | 已完成 | 消息/事件/拓扑编译期生成 |

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
| 共享内存无锁队列 | cpp-ipc | ShmRingBuffer 进程间通信 |
| 零拷贝 LoanedMessage | ROS2 Fast DDS | ShmTransport 共享内存发布 |
| 自动传输选择 | CyberRT | TransportFactory 路由策略 |
| 命名通道 IPC | cpp-ipc | ShmChannel 进程间命名管道 |
| eventfd 轻量通知 | Linux kernel | ShmChannel 消费者唤醒 |
| Service/Client RPC | ROS2 rclcpp | service.hpp 请求-响应 |
| UDP 多播发现 | ROS2 DDS | discovery.hpp 节点自动发现 |
| App-Instance 两层模型 | 原始 OSP | app.hpp 应用-实例架构 |
| 全局实例 ID (IID) | 原始 OSP | post.hpp MAKEIID 寻址 |
| 心跳检测 + 断开通知 | 原始 OSP | node_manager.hpp 连接管理 |
| sockpp RAII Socket | sockpp | socket.hpp/transport.hpp 网络层 |
| NATIVE_SYNC 串口 | CSerialPort | serial_transport.hpp 串口传输 |
| 帧同步状态机 + CRC16 | 工业通信协议 | serial_transport.hpp 可靠传输 |
| HSM 驱动生命周期 | ROS2 Lifecycle + hsm-cpp | service_hsm/discovery_hsm/node_manager_hsm |
| 每连接独立状态机 | 工业协议栈 | node_manager_hsm 连接管理 |
| YAML 编译期代码生成 | ROS2 msg/srv codegen | ospgen.py 消息/拓扑生成 |

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
| `OSP_IO_POLLER_MAX_EVENTS` | 64 | io_poller.hpp | 单次 Wait 最大事件数 |
| `OSP_BT_MAX_NODES` | 32 | bt.hpp | 行为树最大节点数 |
| `OSP_BT_MAX_CHILDREN` | 8 | bt.hpp | 复合节点最大子节点数 |
| `OSP_EXECUTOR_MAX_NODES` | 16 | executor.hpp | Executor 最大节点数 |
| `OSP_TRANSPORT_MAX_FRAME_SIZE` | 4096 | transport.hpp | 最大帧大小 |
| `OSP_CONNECTION_POOL_CAPACITY` | 32 | connection.hpp | 连接池默认容量 |
| `OSP_SHM_SLOT_SIZE` | 4096 | shm_transport.hpp | 共享内存 slot 大小 |
| `OSP_SHM_SLOT_COUNT` | 256 | shm_transport.hpp | 共享内存 slot 数量 |
| `OSP_SHM_CHANNEL_NAME_MAX` | 64 | shm_transport.hpp | 通道名最大长度 |
| `OSP_DISCOVERY_PORT` | 9999 | discovery.hpp | 多播发现端口 |
| `OSP_DISCOVERY_INTERVAL_MS` | 1000 | discovery.hpp | 心跳广播间隔 |
| `OSP_APP_MAX_INSTANCES` | 256 | app.hpp | 每应用最大实例数 |
| `OSP_APP_MAX_APPS` | 64 | app.hpp | 最大应用数 |
| `OSP_NODE_MANAGER_MAX_NODES` | 64 | node_manager.hpp | 最大节点数 |
| `OSP_HEARTBEAT_INTERVAL_MS` | 1000 | node_manager.hpp | 心跳间隔 |
| `OSP_HEARTBEAT_TIMEOUT_COUNT` | 3 | node_manager.hpp | 心跳超时次数 |
| `OSP_WITH_SERIAL` | OFF | serial_transport.hpp | 串口传输开关 |
| `OSP_SERIAL_FRAME_MAX_SIZE` | 1024 | serial_transport.hpp | 串口最大帧大小 |
| `OSP_SERIAL_CRC_ENABLED` | 1 | serial_transport.hpp | CRC16 校验开关 |

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
| socket | 单线程使用，fd 不可共享 |
| io_poller | 单线程 (事件循环线程) |
| connection | mutex 保护连接池操作 |
| transport | TcpTransport 单线程; NetworkNode 继承 Node 线程安全性 |
| hsm | 单线程 Dispatch; guard/handler 不可重入 |
| bt | 单线程 Tick; 叶节点回调不可重入 |
| executor | 内部线程安全; Stop 可跨线程调用 |
| data_fusion | FusedSubscription 继承 Bus 订阅线程安全性 |
| semaphore | LightSemaphore/PosixSemaphore 全线程安全 |
| shm_transport | ShmRingBuffer 无锁 CAS; ShmChannel 单写多读 |
| serial_transport | NATIVE_SYNC 单线程; IoPoller 事件循环线程安全 |
| discovery | 内部线程安全; 回调在发现线程执行 |
| service | Service handler 在服务线程执行; Client::Call 可跨线程 |
| app | Application::Post 线程安全; Instance::OnMessage 单线程 |
| post | OspPost 线程安全; OspSendAndWait 阻塞调用线程 |
| node_manager | 内部 mutex 保护; 回调在心跳线程执行 |
| node_manager_hsm | mutex 保护; 每连接独立 HSM 单线程 Dispatch |
| service_hsm | mutex 保护; HSM 单线程 Dispatch |
| discovery_hsm | mutex 保护; HSM 单线程 Dispatch |
| net | 单线程使用 (同 sockpp); fd 不可共享 |
| transport_factory | 无状态静态方法; 线程安全 |
