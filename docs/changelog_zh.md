# newosp 变更日志

本文档记录 newosp 项目的架构调整历史和实施阶段。

---

## 最新变更

### 2026-02-14: inicpp 集成优化

**变更内容**:
- 移除 `examples/inicpp/` 子目录 (已有顶层 `inicpp_demo.cpp`)
- 合并 `docs/inicpp_api_zh.md` 到 `docs/api/api_foundation_core_zh.md` (新增第 4 节)
- 统一 API 文档结构: inicpp 作为基础层模块与其他模块并列
- 修正 `examples/inicpp_demo.cpp` 头文件引用: `#include "osp/inicpp.h"` → `#include "osp/inicpp.hpp"`

**影响模块**: examples/CMakeLists.txt, docs/api/api_foundation_core_zh.md, examples/inicpp_demo.cpp

---

## 1. P0 架构调整记录

本节列出当前架构设计中已识别的 7 项 P0 优先级调整项，这些调整针对工业级嵌入式场景的关键需求，将在后续实施阶段逐步落地。

### P0-1: AsyncBus 单消费者瓶颈 + variant 内存膨胀

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

### P0-2: Node 全局单例 Bus + 纯类型路由局限

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

### P0-3: Executor 缺少实时调度支持

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

### P0-4: Transport 帧缺少 seq_num 和 timestamp

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

### P0-5: ShmRingBuffer ARM 弱内存序

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

### P0-6: 缺少 QoS 配置层

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

### P0-7: 缺少 Lifecycle Node

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

## 2. 实施阶段

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
38. **worker_pool.hpp -- SpscRingbuffer**: 替换 SpscQueue，编译期容量 (OSP_WORKER_QUEUE_DEPTH)，batch ops / Peek / PushFromCallback
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
128. **性能基准回归**: 全传输路径 (inproc/shm/tcp) ��吐量/延迟基线

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

### Phase R: HSM 驱动模块 + 代码生成 (已实现, 575 tests)

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

### Phase S: 基础组件统一采用 + MISRA 合规 (已实现, 575 tests)

> 上层模块 (应用层/服务层/传输层) 全面采用基础层组件，消除重复实现。
> 同时完成 HSM 集成到 Instance 和 LifecycleNode，以及 MISRA C++ 合规修复。

159. **char[] → FixedString**: app.hpp name_, node_manager.hpp remote_host, service.hpp Entry name/host, discovery.hpp DiscoveredNode/TopicInfo/ServiceInfo, transport.hpp Endpoint host, serial_transport.hpp port_name, transport_factory.hpp TransportConfig
160. **固定数组+active → FixedVector**: post.hpp AppRegistry entries_, service.hpp ServiceRegistry entries_
161. **chrono/gettimeofday → SteadyNowUs/SteadyNowNs**: node_manager_hsm.hpp MonitorLoop, service.hpp AsyncClient::GetResult, serial_transport.hpp (移除 NowMs+gettimeofday), transport.hpp (移除 SteadyClockNs)
162. **CopyString 辅助函数删除**: discovery.hpp 两处 CopyString 替换为 FixedString::assign(TruncateToCapacity, ...)
163. **Instance HSM 集成**: 11 状态层次结构 (Idle/Running/Processing/Paused/Error/Fatal 等)
164. **LifecycleNode HSM 重构**: 16 状态层次结构 (含 Degraded/Recoverable/Fatal 子状态)
165. **MISRA C++ 合规**: enum class 替代裸枚举, inline constexpr 替代 #define, static_cast<void> 弃值标注
166. **测试更新**: test_transport.cpp, test_transport_factory.cpp, test_service.cpp, test_discovery.cpp, test_serial_transport.cpp 适配 FixedString API

### Phase T: 崩溃恢复 + 组件集成 + SPSC 环形缓冲区 (已实现, 758 tests)

> shm_transport 崩溃恢复 (CreateOrReplace)、SpscRingbuffer 引入、
> shm_ipc 示例全面重写集成 Watchdog + FaultCollector + SpscRingbuffer。

167. **shm_transport.hpp -- CreateOrReplace**: SharedMemorySegment::CreateOrReplace 容忍残留 /dev/shm 文件 (shm_unlink 后 Create)
168. **shm_transport.hpp -- CreateOrReplaceWriter**: ShmChannel::CreateOrReplaceWriter 委托 CreateOrReplace，崩溃后重启无需手动清理
169. **test_shm_transport.cpp**: 6 个 fork 跨进程测试改用 CreateOrReplace，消除残留文件导致的测试失败
170. **spsc_ringbuffer.hpp**: 从 DeguiLiu/ringbuffer v2.0.0 适配，lock-free wait-free SPSC，支持 trivially_copyable (memcpy) 和非 trivially_copyable (move) 双路径
171. **shm_common.hpp**: 共享定义 -- FrameHeader、ShmStats (48B trivially_copyable)、FaultCode、通道配置常量
172. **shm_producer.cpp**: HSM 8 状态 + CreateOrReplaceWriter + ThreadWatchdog + FaultCollector + SpscRingbuffer 统计推送
173. **shm_consumer.cpp**: HSM 8 状态 + ThreadWatchdog + FaultCollector + SpscRingbuffer 统计推送 + 帧完整性校验
174. **shm_monitor.cpp**: DebugShell + SpscRingbuffer 统计消费 + 6 个 Shell 命令 (shm_status/stats/latest/peek/config/reset)
175. **design_zh.md 6.25.3**: 修复 StartAutoCheck 示例 (C 函数指针回调 + kFaultThreadDeath 映射)
176. **design_zh.md 6.9**: 补充 CreateOrReplace API 文档 + shm_ipc 三进程架构说明
177. **design_zh.md 6.26**: 新增 SpscRingbuffer 章节 (特性/API/内存序/资源预算)
178. **spsc_ringbuffer.hpp -- 跨层集成**: 泛化支持非 trivially_copyable 类型 (if constexpr 双路径)，集成到 worker_pool / serial_transport / transport
179. **worker_pool.hpp -- SpscQueue 替换**: 删除 detail::SpscQueue (~80行)，改用 SpscRingbuffer<EnvelopeType, OSP_WORKER_QUEUE_DEPTH>
180. **serial_transport.hpp -- rx_ring_**: 新增 ReadToRing/ParseFromRing，SpscRingbuffer<uint8_t, 4096> 字节缓冲
181. **transport.hpp -- RecvFrameSlot**: 新增 RecvFrameSlot + ReceiveToBuffer/DispatchFromBuffer，SpscRingbuffer per-subscriber 帧缓冲
182. **test_spsc_ringbuffer.cpp**: 新增 5 个 non-trivially-copyable 测试 (variant Push/Pop, PushBatch/PopBatch, 并发 SPSC, MoveTracker, MessageEnvelope)

---

## 3. 原始 OSP 功能覆盖对照表

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

---
