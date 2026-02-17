# JobPool: 工业级共享数据块流水线

## 1. 设计目标

为 newosp 提供统一的数据分发与流水线执行框架，满足工业嵌入式场景的以下需求:

- 消费者 (Visitor/Worker) 对数据来源无感知 (进程内/跨进程统一接口)
- 数据块共享内存 + 引用计数，最后一个消费者完成后才释放
- 支持并行扇出 (fan-out) 和串行流水线 (pipeline) 两种 DAG 拓扑
- 固定大小内存池，预分配，零运行时堆分配
- 背压机制 + FaultCollector 异常上报
- 超时检测 + Watchdog 集成
- 复用现有 WorkerPool 执行模型

## 2. 架构总览

```
                    SharedMemory (POSIX shm 或进程内 buffer)
                    ┌─────────────────────────────────────┐
                    │ JobPoolHeader                        │
                    │   pool_capacity, block_size          │
                    │   alloc_head (atomic, lock-free)     │
                    │   free_list[] (embedded free list)   │
                    │                                      │
                    │ DataBlock[0] { refcount, state,      │
                    │                deadline_us, fault_id, │
                    │                payload[block_size] }  │
                    │ DataBlock[1] ...                      │
                    │ DataBlock[N-1] ...                    │
                    └─────────────────────────────────────┘
                              │
                    ┌─────────┴──────────┐
                    │   DAG Pipeline     │
                    │   (静态拓扑)        │
                    │                    │
                    │  Stage A ──┬──→ Stage B ──→ Stage D
                    │            └──→ Stage C
                    └────────────────────┘
                              │
                    ┌─────────┴──────────┐
                    │   WorkerPool       │
                    │   (复用现有模块)    │
                    │   Worker[0..N-1]   │
                    └────────────────────┘
```

## 3. 核心数据结构

### 3.1 DataBlock -- 共享数据块

```cpp
struct DataBlock {
  std::atomic<uint32_t> refcount;    // 引用计数 (跨进程 atomic)
  std::atomic<uint8_t>  state;       // BlockState 枚举
  uint8_t  pad[3];
  uint32_t block_id;                 // 池内索引
  uint32_t payload_size;             // 实际数据大小
  uint32_t fault_id;                 // 关联的 fault code (0 = 无异常)
  uint64_t alloc_time_us;            // 分配时间戳
  uint64_t deadline_us;              // 超时截止时间 (0 = 无超时)
  uint32_t next_free;                // 嵌入式 free list 指针
  uint32_t reserved;
  // alignas(16) uint8_t payload[];  // 柔性数组 (实际通过偏移计算)
};

enum class BlockState : uint8_t {
  kFree       = 0,   // 在 free list 中
  kAllocated  = 1,   // 已分配，生产者填充中
  kReady      = 2,   // 数据就绪，等待消费
  kProcessing = 3,   // 至少一个消费者正在处理
  kDone       = 4,   // 所有消费者完成，待回收
  kTimeout    = 5,   // 超时未完成
  kError      = 6,   // 处理异常
};
```

内存布局 (每个 DataBlock):
```
+0    refcount      (4B, atomic)
+4    state         (1B, atomic)
+5    pad           (3B)
+8    block_id      (4B)
+12   payload_size  (4B)
+16   fault_id      (4B)
+20   alloc_time_us (8B, 不对齐但可接受)
+28   deadline_us   (8B)
+36   next_free     (4B)
+40   reserved      (4B)
+44   [padding to 48B or 64B cache line]
+48/64 payload[block_size]
```

### 3.2 JobPoolHeader -- 池头部 (共享内存首部)

```cpp
struct JobPoolHeader {
  uint32_t magic;                    // 0x4A4F4250 ('JOBP')
  uint32_t version;                  // 1
  uint32_t block_size;               // 每块 payload 大小
  uint32_t block_count;              // 总块数
  uint32_t block_stride;             // sizeof(DataBlock) + block_size (对齐后)
  std::atomic<uint32_t> free_head;   // free list 头 (CAS)
  std::atomic<uint32_t> free_count;  // 可用块数
  std::atomic<uint32_t> alloc_count; // 已分配块数
  uint32_t reserved[8];
};
```

### 3.3 StageConfig -- DAG 节点配置

```cpp
using JobHandler = void (*)(const uint8_t* data, uint32_t len, void* ctx);

struct StageConfig {
  const char* name;                  // Stage 名称 (调试用)
  JobHandler handler;                // 处理函数
  void* handler_ctx;                 // 处理函数上下文
  uint32_t timeout_ms;               // 单块处理超时 (0 = 无超时)
  uint32_t worker_affinity;          // 绑定到哪个 WorkerPool worker (-1 = 任意)
};
```

## 4. 核心类

### 4.1 JobPool -- 共享内存数据块池

```cpp
template <uint32_t BlockSize, uint32_t MaxBlocks>
class JobPool {
 public:
  // --- 初始化 ---
  // 进程内: 在栈/堆上初始化
  void InitLocal();
  // 跨进程: 在共享内存上初始化 (生产者调用)
  static expected<JobPool, JobPoolError> CreateShared(
      const char* name, uint32_t block_size, uint32_t block_count);
  // 跨进程: 附加到已有共享内存 (消费者调用)
  static expected<JobPool, JobPoolError> AttachShared(const char* name);

  // --- 生产者 API ---
  // 分配一个数据块 (CAS lock-free)
  expected<uint32_t, JobPoolError> Alloc();
  // 获取数据块 payload 指针 (用于填充数据)
  uint8_t* GetPayload(uint32_t block_id);
  // 提交数据块到 pipeline (设置 refcount = fan_out_count, state = kReady)
  void Submit(uint32_t block_id, uint32_t payload_size);

  // --- 消费者 API ---
  // 获取只读 payload 指针
  const uint8_t* GetPayloadReadOnly(uint32_t block_id) const;
  // 消费者完成处理，递减引用计数
  // 返回 true 表示自己是最后一个消费者 (refcount 降为 0)
  bool Release(uint32_t block_id);

  // --- 查询 ---
  uint32_t FreeCount() const;
  uint32_t AllocCount() const;
  BlockState GetState(uint32_t block_id) const;
  uint32_t Capacity() const { return MaxBlocks; }
  uint32_t BlockSize() const { return BlockSize; }

  // --- 异常检测 ---
  // 扫描所有已分配块，检测超时
  // 返回超时块数量，通过 callback 上报每个超时块
  uint32_t ScanTimeout(void (*on_timeout)(uint32_t block_id, void* ctx),
                       void* ctx);

 private:
  JobPoolHeader* header_ = nullptr;
  uint8_t* blocks_base_ = nullptr;  // DataBlock[0] 起始地址
  // shm 相关
  int shm_fd_ = -1;
  bool is_owner_ = false;
};
```

### 4.2 Pipeline -- 静态 DAG 流水线

```cpp
template <uint32_t MaxStages = 8, uint32_t MaxEdges = 16>
class Pipeline {
 public:
  // --- 构建 (初始化时调用，运行时不变) ---
  int32_t AddStage(const StageConfig& cfg);
  void AddEdge(int32_t from, int32_t to);  // from 完成后触发 to
  void SetEntryStage(int32_t stage_id);     // 入口 stage

  // --- 绑定执行器 ---
  template <typename PayloadVariant>
  void BindWorkerPool(WorkerPool<PayloadVariant>* pool);

  // --- 提交数据块进入 pipeline ---
  void Submit(uint32_t block_id);

  // --- 查询 ---
  uint32_t StageCount() const;
  const char* StageName(int32_t stage_id) const;

 private:
  struct StageSlot {
    StageConfig config;
    // 出边列表 (fan-out)
    int32_t successors[MaxEdges];
    uint32_t successor_count = 0;
  };

  StageSlot stages_[MaxStages];
  uint32_t stage_count_ = 0;
  int32_t entry_stage_ = -1;

  // 内部: stage 完成后的回调
  void OnStageComplete(int32_t stage_id, uint32_t block_id);
};
```

### 4.3 DataDispatcher -- 统一分发器 (面向用户的顶层 API)

```cpp
template <uint32_t BlockSize, uint32_t MaxBlocks,
          uint32_t MaxStages = 8, uint32_t MaxEdges = 16>
class DataDispatcher {
 public:
  struct Config {
    const char* name;                // 通道名称
    bool shared_memory;              // true = 跨进程, false = 进程内
    uint32_t timeout_ms;             // 全局超时 (0 = 无)
    uint32_t backpressure_threshold; // 背压阈值 (可用块 < 此值时触发)
  };

  // --- 初始化 ---
  expected<void, JobPoolError> Init(const Config& cfg);

  // --- 生产者 ---
  expected<uint32_t, JobPoolError> AllocBlock();
  uint8_t* GetBlockPayload(uint32_t block_id);
  void SubmitBlock(uint32_t block_id, uint32_t size);

  // --- Pipeline 配置 ---
  int32_t AddStage(const StageConfig& cfg);
  void AddEdge(int32_t from, int32_t to);
  void SetEntryStage(int32_t stage_id);

  // --- 绑定执行器 ---
  template <typename PayloadVariant>
  void BindWorkerPool(WorkerPool<PayloadVariant>* pool);

  // --- 背压回调 ---
  void SetBackpressureCallback(void (*cb)(uint32_t free_count, void* ctx),
                               void* ctx);

  // --- Fault 集成 ---
  template <uint32_t FC, uint32_t FM>
  void BindFaultCollector(FaultCollector<FC, FM>* collector,
                          uint32_t fault_slot);

  // --- 监控 ---
  uint32_t ScanTimeout();
  uint32_t FreeBlocks() const;
  uint32_t AllocBlocks() const;

 private:
  JobPool<BlockSize, MaxBlocks> pool_;
  Pipeline<MaxStages, MaxEdges> pipeline_;
  Config cfg_;
  // Fault
  void* fault_collector_ = nullptr;
  uint32_t fault_slot_ = 0;
  // Backpressure
  void (*bp_callback_)(uint32_t, void*) = nullptr;
  void* bp_ctx_ = nullptr;
};
```

## 5. 生命周期流程

### 5.1 数据块生命周期

```
Alloc()          Submit()         Stage完成          最后一个Release()
  │                │                │                    │
  ▼                ▼                ▼                    ▼
kFree ──→ kAllocated ──→ kReady ──→ kProcessing ──→ kDone ──→ kFree
                                       │                       ▲
                                       ├── 超时 ──→ kTimeout ──┘
                                       └── 异常 ──→ kError ────┘
```

### 5.2 引用计数规则

```
Submit(block_id):
  refcount = fan_out_count  // 入口 stage 的后继数量
  state = kReady

Stage[i] 完成:
  for each successor[j]:
    refcount.fetch_add(1)   // 为每个后继增加引用
  refcount.fetch_sub(1)     // 自己完成，减引用
  if refcount == 0:
    state = kDone → 回收到 free list

特殊情况:
  fan-out (A → B, A → C):
    Submit 时 refcount = 2 (B 和 C)
    B 完成: refcount 1→0? 否 → 不回收
    C 完成: refcount 0 → 回收

  pipeline (A → B → C):
    Submit 时 refcount = 1 (只有 B)
    A 完成: 触发 B, refcount 保持 1 (A 减 1, B 加 1)
    B 完成: 触发 C, refcount 保持 1
    C 完成: refcount 1→0 → 回收
```

### 5.3 背压机制

```
AllocBlock():
  free_count = pool_.FreeCount()
  if free_count < backpressure_threshold:
    bp_callback_(free_count, bp_ctx_)  // 通知生产者
    fault_collector_->ReportFault(fault_slot_, block_id, kLow)
  if free_count == 0:
    return kPoolExhausted  // 生产者决定: 丢帧 or 等待
```

### 5.4 超时检测

```
ScanTimeout() -- 由 Watchdog 或 Timer 定期调用:
  for each block where state == kProcessing:
    if SteadyNowUs() > block.deadline_us:
      block.state = kTimeout
      fault_collector_->ReportFault(fault_slot_, block_id, kHigh)
      // 强制回收 (refcount 置 0)
      block.refcount = 0
      RecycleBlock(block_id)
```

## 6. 进程内 vs 跨进程

### 6.1 进程内模式

```cpp
DataDispatcher<16016, 32> dispatcher;
DataDispatcher<16016, 32>::Config cfg;
cfg.name = "lidar";
cfg.shared_memory = false;  // 进程内
cfg.backpressure_threshold = 4;
dispatcher.Init(cfg);

// 添加 stage
auto s_log = dispatcher.AddStage({"logging", LogHandler, &log_ctx, 100, 0});
auto s_fus = dispatcher.AddStage({"fusion", FusionHandler, &fus_ctx, 200, 1});
dispatcher.SetEntryStage(s_log);  // 入口
dispatcher.AddEdge(s_log, s_fus); // log → fusion (串行)
// 或: 两个都是入口的后继 (并行 fan-out)

dispatcher.BindWorkerPool(&worker_pool);

// 生产
auto bid = dispatcher.AllocBlock();
FillLidarFrame(dispatcher.GetBlockPayload(bid.value()), seq, ts);
dispatcher.SubmitBlock(bid.value(), kFrameDataSize);
// Worker 自动执行 stage，最后一个完成后自动回收
```

内存布局: `JobPool` 的 `blocks_base_` 指向栈上或堆上的 `alignas(64) uint8_t storage[]`。

### 6.2 跨进程模式

```cpp
// 生产者进程
DataDispatcher<16016, 32> producer_disp;
DataDispatcher<16016, 32>::Config cfg;
cfg.name = "osp_lidar_jobs";
cfg.shared_memory = true;  // POSIX shm
producer_disp.Init(cfg);

auto bid = producer_disp.AllocBlock();
FillLidarFrame(producer_disp.GetBlockPayload(bid.value()), seq, ts);
producer_disp.SubmitBlock(bid.value(), kFrameDataSize);

// 消费者进程 (各自 Attach)
DataDispatcher<16016, 32> consumer_disp;
consumer_disp.AttachShared("osp_lidar_jobs");
// 消费者通过 block_id 直接访问共享内存中的 payload (零拷贝)
const uint8_t* data = consumer_disp.GetBlockPayloadReadOnly(block_id);
ProcessFrame(data, len);
consumer_disp.ReleaseBlock(block_id);  // 最后一个释放时自动回收
```

跨进程时，`DataBlock.refcount` 在共享内存中，所有进程通过 `atomic` 操作共享。

### 6.3 通知机制

- 进程内: `OspPost(visitor_iid, kEvtBlockReady, &block_id, 4)` 通过 AsyncBus 投递
- 跨进程: `ShmSpmcByteChannel.Write(&block_id, 4)` 只发 4 字节 block_id (非完整数据帧)
  - 消费者 `WaitReadable()` + `Read()` 收到 block_id
  - 通过同一块 POSIX shm 的 SharedBlockPool 直接访问数据 (零拷贝)
  - SPMC 通道容量极小 (4B × N blocks)，几乎无内存开销

## 7. 与现有模块的深度复用

JobPool 不是独立系统，而是 newosp 现有模块的组合层。核心原则: 只新增共享内存数据块管理和 DAG 编排，其余全部复用。

### 7.1 复用映射表

| 需求 | 现有模块 | 复用方式 | 新增代码 |
|------|----------|----------|----------|
| 数据块内存池 | `MemPool` (FixedPool) | 复用嵌入式 free list 设计，扩展到共享内存上 | SharedBlockPool |
| 消费者生命周期 | `LifecycleNode` | 每个 Visitor 就是一个 LifecycleNode | 零 |
| | | on_configure: 注册到 Pipeline | |
| | | on_activate: 开始接收数据 | |
| | | on_deactivate: 暂停接收 | |
| | | on_cleanup: 释放所有持有的 block | |
| 启动编排 | `BehaviorTree` | Sequence/Parallel 编排模块启动顺序 | 零 |
| | | Condition 检查前置依赖就绪 | |
| 异常上报 | `FaultCollector` | 直接复用，不新建 | 零 |
| | | kPoolExhausted / kJobTimeout / kConsumerCrash | |
| 数据就绪通知 (进程内) | `OspPost` / `AppRegistry` | 数据块就绪后 OspPost 通知消费者 | 零 |
| 数据就绪通知 (跨进程) | `ShmSpmcByteChannel` | 通过 SPMC 通道发送 block_id (4B) | 零 |
| | | 消费者收到 block_id 后直接访问共享内存 | |
| 节点心跳监控 | `NodeManagerHsm` | 跨进程消费者 = Node，心跳检测复用 | 零 |
| | | Connected/Suspect/Disconnected 状态机 | |
| Worker 执行 | `WorkerPool` | Stage 处理函数通过 Submit() 分发 | 零 |
| 消费者状态管理 | `HSM` | Connecting/Processing/Overloaded/Done | 零 |
| 多源融合 | `DataFusion` | 多传感器时间对齐触发 | 零 |
| 进程管理 | `Subprocess` (process.hpp) | Launcher 启动/停止/重启消费者进程 | 零 |
| 线程看门狗 | `ThreadWatchdog` | 各进程内线程心跳监控 | 零 |
| 定时统计 | `TimerScheduler` | 定期统计 free/alloc count, throughput | 零 |
| 优雅关停 | `ShutdownManager` | drain pipeline → release blocks → exit | 零 |
| 调试诊断 | `DebugShell` + `shell_commands.hpp` | job_status/job_stats/job_timeout 命令 | ~50 行 |

### 7.2 新增代码量评估

| 组件 | 说明 | 预估行数 |
|------|------|----------|
| `SharedBlockPool` | 共享内存上的 FixedPool + atomic refcount | ~200 |
| `Pipeline` | 静态 DAG 拓扑 (stage + edge + dispatch) | ~150 |
| `DataDispatcher` | 胶水层: 组合 BlockPool + Pipeline + 通知 | ~150 |
| Shell 命令 | job_status/job_stats/job_timeout | ~50 |
| 总计 | | ~550 |

对比: 如果不复用现有模块，从零实现需要 ~2000+ 行。

### 7.3 跨进程通知: OspPost vs SPMC 通道

进程内和跨进程使用不同的通知机制，但消费者接口统一:

```
进程内:
  Producer Submit(block_id)
    → OspPost(visitor_iid, kEvtBlockReady, &block_id, 4)
    → LifecycleNode 的 Bus 回调收到 block_id
    → 通过 SharedBlockPool 直接访问数据 (零拷贝)

跨进程:
  Producer Submit(block_id)
    → ShmSpmcByteChannel.Write(&block_id, 4)  // 只发 4 字节 block_id
    → 消费者进程 Read() 收到 block_id
    → 通过 SharedBlockPool (同一块 POSIX shm) 直接访问数据 (零拷贝)
```

关键: SPMC 通道不再传输完整数据帧 (16KB)，只传输 block_id (4B)。实际数据在共享内存的 BlockPool 中，所有进程 mmap 同一段内存，真正的零拷贝。

### 7.4 消费者 = LifecycleNode

```cpp
// 消费者不需要知道数据来自进程内还是跨进程
struct FusionVisitor {
  osp::LifecycleNode node;

  bool on_configure() {
    // 注册到 Pipeline 的 "fusion" stage
    dispatcher.RegisterConsumer("fusion", getpid());
    return true;
  }

  bool on_activate() {
    // 开始接收数据块通知
    node.Subscribe<BlockReadyEvent>([this](const BlockReadyEvent& evt) {
      const uint8_t* data = pool.GetPayloadReadOnly(evt.block_id);
      ProcessFrame(data, evt.payload_size);
      pool.Release(evt.block_id);  // 最后一个释放时自动回收
    });
    return true;
  }

  bool on_deactivate() {
    // 暂停接收
    return true;
  }

  bool on_cleanup() {
    // 释放所有持有的 block
    dispatcher.UnregisterConsumer(getpid());
    return true;
  }
};
```

## 8. 跨进程监管 (Process + Watchdog)

跨进程模式下，每个消费者是独立进程，需要 Launcher 通过 `process.hpp` 和 `watchdog.hpp` 进行双层监管:

### 8.1 进程级监管 (Launcher)

```
Launcher (父进程)
  │
  ├── Subprocess: Producer     ← process.hpp 管理
  ├── Subprocess: VisitorLog   ← IsProcessAlive() 健康检查
  ├── Subprocess: VisitorFus   ← 崩溃后可 auto-restart
  └── Subprocess: Monitor      ← SIGTERM → 2s → SIGKILL
```

Launcher 职责:
- 通过 `osp::Subprocess` 启动/停止各进程
- 1s 周期 `IsProcessAlive()` 检测进程存活
- 关键进程 (Producer) 崩溃 → 全局 shutdown
- 非关键进程 (Visitor) 崩溃 → 可选 auto-restart
- 崩溃进程的 DataBlock 引用计数需要清理 (见 8.3)

### 8.2 数据块级监管 (Watchdog + Timer)

每个进程内部，Watchdog 监控数据块处理是否卡死:

```cpp
// Producer 进程
ctx.watchdog.Register("producer_main", 5000);  // 5s 超时

// Timer 定期扫描超时数据块
ctx.timer.Add(1000, [](void* arg) {
  auto* disp = static_cast<DataDispatcher*>(arg);
  uint32_t timeout_count = disp->ScanTimeout();
  if (timeout_count > 0) {
    // FaultCollector 上报
    fault_collector.ReportFault(kFaultJobTimeout, timeout_count,
                                osp::FaultPriority::kHigh);
  }
}, &dispatcher);
```

双层超时检测:
- **进程级**: Watchdog 检测线程心跳，5s 无心跳 → 进程异常
- **数据块级**: Timer 扫描 `DataBlock.deadline_us`，单块超时 → 强制回收 + Fault 上报

### 8.3 进程崩溃时的引用计数清理

消费者进程崩溃后，其持有的 DataBlock 引用计数不会自动递减，导致数据块泄漏。解决方案:

```
方案: writer_pid + consumer_pid 追踪

JobPoolHeader 中增加:
  consumer_pids[MaxConsumers]: atomic<uint32_t>  // 注册的消费者 PID

Launcher 检测到进程崩溃后:
  1. 遍历 consumer_pids[]，找到崩溃进程的 PID
  2. 遍历所有 state == kProcessing 的 DataBlock
  3. 对崩溃进程持有的块执行 ForceRelease():
     - refcount.fetch_sub(1)
     - 如果降为 0 → 回收到 free list
  4. 清除 consumer_pids[] 中的条目
  5. FaultCollector 上报 kFaultConsumerCrash
```

```cpp
// Launcher 中的崩溃处理
void OnChildCrash(uint32_t child_pid) {
  OSP_LOG_ERROR("Launcher", "child pid=%u crashed, cleaning up blocks",
                child_pid);
  uint32_t cleaned = dispatcher.CleanupConsumer(child_pid);
  OSP_LOG_WARN("Launcher", "cleaned %u orphaned blocks", cleaned);
  fault_collector.ReportFault(kFaultConsumerCrash, child_pid,
                              osp::FaultPriority::kHigh);
}
```

### 8.4 监管架构总览

```
Launcher (process.hpp)
  │
  ├── 进程健康: IsProcessAlive() @ 1Hz
  ├── 崩溃清理: CleanupConsumer(pid)
  ├── 优雅关停: SIGTERM → drain → SIGKILL
  │
  ▼
各子进程内部:
  ├── ThreadWatchdog: 线程心跳 @ 1Hz, 5s 超时
  ├── Timer: ScanTimeout() @ 1Hz, 检测卡死数据块
  ├── FaultCollector: 上报 kPoolExhausted / kJobTimeout / kConsumerCrash
  └── ShutdownManager: SIGTERM → drain pipeline → release blocks → exit
```

## 9. 与 BT 的集成 (启动编排)

工业系统中各模块的启动顺序有依赖关系，用行为树编排:

```
Sequence (系统启动)
├── Action: InitJobPool("osp_lidar_jobs")
├── Action: InitPipeline(stages, edges)
├── Action: StartWorkerPool(4 workers)
├── Parallel (启动消费者)
│   ├── Action: StartLoggingVisitor()
│   └── Action: StartFusionVisitor()
├── Action: StartProducer()
└── Action: StartMonitor()
```

Pipeline 的 DAG 拓扑在 `InitPipeline` 阶段静态配置，运行时不变。BT 负责确保依赖顺序正确。

## 10. 资源预算

以 LiDAR 场景为例 (BlockSize=16016, MaxBlocks=32):

| 资源 | 用量 | 说明 |
|------|------|------|
| JobPoolHeader | 64B | 池头部元数据 |
| DataBlock header | 64B × 32 = 2KB | 每块控制信息 (对齐到 cache line) |
| DataBlock payload | 16016B × 32 = 500KB | 数据区 |
| Pipeline | ~512B | 8 stages × 64B |
| 总计 | ~503KB | 共享内存段大小 |

对比当前 SPMC 方案 (256KB ring buffer): JobPool 多用约 250KB，但提供了引用计数、超时检测、DAG 编排能力。

## 11. 编译期配置

| 宏 | 默认值 | 说明 |
|----|--------|------|
| `OSP_JOB_POOL_MAGIC` | `0x4A4F4250` | 共享内存 magic number |
| `OSP_JOB_BLOCK_ALIGN` | 64 | DataBlock 对齐 (cache line) |
| `OSP_JOB_MAX_STAGES` | 8 | Pipeline 最大 stage 数 |
| `OSP_JOB_MAX_EDGES` | 16 | Pipeline 最大边数 |

## 12. 错误码

```cpp
enum class JobPoolError : uint8_t {
  kPoolExhausted = 0,  // 内存池满 (背压)
  kInvalidBlockId,     // block_id 越界
  kBlockNotReady,      // 数据块未就绪
  kBlockTimeout,       // 处理超时
  kShmCreateFailed,    // 共享内存创建失败
  kShmAttachFailed,    // 共享内存附加失败
  kMagicMismatch,      // magic 不匹配
  kVersionMismatch,    // 版本不匹配
  kPipelineFull,       // stage/edge 数量超限
};
```

## 13. 文件清单

| 文件 | 说明 | 预估行数 |
|------|------|----------|
| `include/osp/job_pool.hpp` | JobPool + Pipeline + DataDispatcher | ~600 |
| `tests/test_job_pool.cpp` | 单元测试 (~20 cases) | ~500 |
| `examples/data_visitor_dispatcher/` | 改造 demo 支持进程内/跨进程 | 修改现有文件 |
| `docs/design_zh.md` | 更新 JobPool 模块文档 | 追加 |

## 14. 与 SPMC 的关系

两者定位不同，共存互补:

| 维度 | ShmSpmcByteChannel | DataDispatcher (JobPool) |
|------|--------------------|-----------------------|
| 抽象层级 | 字节流传输 | 数据块生命周期管理 |
| 数据共享 | 每消费者独立拷贝 | 引用计数零拷贝共享 |
| 释放时机 | 消费者读完即推进 tail | 最后一个消费者 Release |
| 流水线 | 无 | DAG (fan-out + pipeline) |
| 背压 | ring full 阻塞写入 | 池满 + 阈值回调 |
| 超时检测 | 无 | 每块 deadline + 扫描 |
| 适用场景 | 简单 1:N 分发 | 复杂流水线 + 生命周期管理 |
| 复杂度 | 低 | 中 |

简单场景 (纯分发，消费者独立处理) 用 SPMC。复杂场景 (多 stage 流水线，共享数据，超时检测) 用 JobPool。
