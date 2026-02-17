# DataDispatcher: 统一数据分发架构设计

> 版本: 3.0
> 日期: 2026-02-17
> 状态: 已实现 (Phase 0-5 完成)
> 前身: design_job_pool_zh.md (方案对比与最终设计)

---

## 目录

1. [设计目标](#1-设计目标)
2. [设计约束](#2-设计约束)
3. [现状分析: InProc vs InterProc 差异](#3-现状分析-inproc-vs-interproc-差异)
4. [架构决策: StorePolicy + NotifyPolicy 双策略模板](#4-架构决策-storepolicy--notifypolicy-双策略模板)
5. [统一 API](#5-统一-api)
6. [核心数据结构](#6-核心数据结构)
7. [StorePolicy 实现](#7-storepolicy-实现)
8. [NotifyPolicy 实现](#8-notifypolicy-实现)
9. [DataDispatcher 完整 API](#9-datadispatcher-完整-api)
10. [数据块生命周期与状态机](#10-数据块生命周期与状态机)
11. [Pipeline DAG 拓扑](#11-pipeline-dag-拓扑)
12. [跨进程架构](#12-跨进程架构)
13. [进程监管与崩溃恢复](#13-进程监管与崩溃恢复)
14. [与现有模块的复用关系](#14-与现有模块的复用关系)
15. [资源预算](#15-资源预算)
16. [编译期配置](#16-编译期配置)
17. [错误码](#17-错误码)
18. [实施状态](#18-实施状态)
19. [文件清单](#19-文件清单)

---

## 1. 设计目标

统一 InProc (`data_dispatcher.hpp` 嵌入式数组) 和 InterProc (POSIX 共享内存) 两套数据分发机制，让业务代码用同一套 API 完成 1:N 数据分发，不感知底层是线程间共享块还是跨进程共享内存。

```
业务代码 (统一 API):
  Alloc -> GetWritable -> Submit -> GetReadable -> Release
           |                           |
     +-----+-----+              +-----+-----+
     | InProc    |              | InterProc  |
     | 栈/静态   |              | mmap shm   |
     | 嵌入数组  |              | 共享内存   |
     +-----------+              +------------+
```

核心需求:
- 消费者对数据来源无感知 (进程内/跨进程统一接口)
- 数据块共享内存 + 引用计数，最后一个消费者完成后才释放
- 支持并行扇出 (fan-out) 和串行流水线 (pipeline) 两种 DAG 拓扑
- 固定大小内存池，预分配，零运行时堆分配
- 背压机制 + FaultCollector 异常上报
- 超时检测 + 强制回收

### 1.1 不在范围内

- N:1 通信 (AsyncBus + Node.Subscribe / 多条 ShmChannel + DataFusion) -- 语义不同
- 1:1 流式传输 (ShmSpscByteRing) -- 字节流语义，不适合块池模型
- 现有 Ring Buffer 系列 -- 保持不变，与 DataDispatcher 正交共存

---

## 2. 设计约束

| 编号 | 约束 | 来源 |
|------|------|------|
| C1 | 可以破坏现有 API，允许重构合并 | 需求方确认 |
| C2 | InterProc 采用共享内存 DataBlock 零拷贝 + block_id 通知 | 需求 |
| C3 | 保留: 引用计数、DAG Pipeline、背压、故障上报、超时检测 | 需求 |
| C4 | 编译期确定部署模式，不需要运行时切换 | 需求 |
| C5 | 零运行时开销 (编译期分发) | 嵌入式原则 |
| C6 | Header-only, C++17, `-fno-exceptions -fno-rtti` 兼容 | 项目规范 |
| C7 | 支持 1:N、N:1、1:1 三种通信模式 | 分析确认 |

---

## 3. 现状分析: InProc vs InterProc 差异

对 InProc (嵌入数组) 和 InterProc (共享内存) 逐项比对:

| 维度 | InProc (InProcStore) | InterProc (ShmStore) | 差异程度 |
|------|------------------|--------------------|----------|
| **块存储** | `uint8_t storage_[]` 嵌入数组 | `void* shm_base_` mmap 区域 | 核心差异 |
| **池元数据** | 成员 `atomic<uint32_t>` | shm header 内 `atomic<uint32_t>` | 核心差异 |
| **Submit 通知** | `Pipeline::Execute()` 同步 | push `block_id` + ShmNotify | 核心差异 |
| `GetBlock(id)` | `&storage_[id * stride]` | `shm_base + header + id * stride` | 1 行 |
| `Init()` | 零初始化数组 | `shm_open` + `mmap` + 初始化 header | 差异较大 |
| **Alloc()** | CAS free_head | **完全相同** | 0 行 |
| **Release()** | CAS refcount + Recycle | **完全相同** | 0 行 |
| **Recycle()** | CAS push to free list | **完全相同** | 0 行 |
| **ScanTimeout()** | CAS reclaim loop | **完全相同** | 0 行 |
| **ForceCleanup()** | CAS reclaim loop | **完全相同** | 0 行 |
| DataBlock 布局 | 44B header + payload | **完全相同** | 0 行 |
| Pipeline | 支持 DAG 拓扑 | 同结构 | 0 行 |

**结论: 核心差异仅 3 处** -- 块存储位置、池元数据位置、Submit 后通知路径。CAS 无锁逻辑 (~120 行) 完全相同。

### 3.1 CAS 无锁逻辑的复杂度与 Bug 历史

```
Alloc():       CAS free_head loop + 状态初始化          ~25 行
Release():     CAS refcount loop + Recycle 触发         ~20 行
Recycle():     CAS push to free_head                    ~15 行
ScanTimeout(): 遍历 + CAS refcount claim + Recycle     ~30 行
ForceCleanup():遍历 + predicate + CAS claim + Recycle  ~25 行
AddRef():      fetch_add                                 ~5 行
                                                  合计 ~120 行
```

Code review 中，这 120 行发现并修复了 3 个线程安全 bug:
- `ScanTimeout` 先 `SetState(kTimeout)` 再 CAS，导致与 `Release` 双重回收
- `ForceCleanup` 使用 `fetch_sub` 导致下溢
- `Release` 使用 `fetch_sub` 在 ScanTimeout 已回收时触发 assert 失败

修复方案: **CAS-first-then-setState** -- 先 CAS 成功 claim refcount，再设置状态，防止竞态回收。

**这是选择双策略模板方案的关键理由**: 绝不将这 120 行危险 CAS 代码复制为两份。

---

## 4. 架构决策: StorePolicy + NotifyPolicy 双策略模板

### 4.1 方案选择

曾评估两个方案:
- **方案 A: StorePolicy + NotifyPolicy 双策略模板** -- CAS 逻辑 1 份
- **方案 B: 两个独立具体类** -- CAS 逻辑 2 份 (复制)

**选择方案 A**。核心理由: CAS 无锁逻辑是 bug 密度最高的区域，方案 A 只维护 1 份，修 bug 改 1 处。

| 维度 | 方案 A (已选) | 方案 B |
|------|-------------|--------|
| CAS 逻辑份数 | **1 份** | 2 份 (重复) |
| 修 bug 改动点 | **1 处** | 2 处 (必须同步) |
| 总代码量 | ~1000 行 | ~1150 行 |
| 新增抽象 | StorePolicy (5 方法) + NotifyPolicy (1 方法) | 无 |
| API 一致性 | 完全一致 (同一份代码) | 需人工保持一致 |

### 4.2 架构总览

```
DataDispatcher<StorePolicy, NotifyPolicy, MaxStages, MaxEdges>
|
+-- StorePolicy (数据块存储)
|   +-- InProcStore<BlockSize, MaxBlocks>   -- 嵌入式数组 + 成员 atomic
|   +-- ShmStore<BlockSize, MaxBlocks>      -- mmap 指针 + header atomic
|
+-- NotifyPolicy (消费者通知)
|   +-- DirectNotify     -- 空操作 (Pipeline 内部驱动)
|   +-- ShmNotify        -- 函数指针回调 (push block_id)
|
+-- Pipeline<MaxStages, MaxEdges>  -- DAG 拓扑 (两种模式共享)
+-- FaultReporter                  -- 故障上报 (两种模式共享)
+-- BackpressureFn                 -- 背压回调 (两种模式共享)
```

### 4.3 StorePolicy Concept (C++17 隐式约束)

```cpp
// StorePolicy 必须提供:
//   void             Init() noexcept;                               // InProcStore
//   void             Init(void* shm_base, uint32_t size) noexcept;  // ShmStore Creator
//   void             Attach(void* shm_base) noexcept;               // ShmStore Opener
//   DataBlock*       GetBlock(uint32_t id) noexcept;
//   const DataBlock* GetBlock(uint32_t id) const noexcept;
//   std::atomic<uint32_t>& FreeHead() noexcept;
//   std::atomic<uint32_t>& FreeCount() noexcept;
//   std::atomic<uint32_t>& AllocCount() noexcept;
//   static constexpr uint32_t Capacity() noexcept;
//   static constexpr uint32_t PayloadCapacity() noexcept;
//   static constexpr uint32_t kBlockStride;
//   static constexpr uint32_t kHeaderAlignedSize;
```

### 4.4 NotifyPolicy Concept

```cpp
// NotifyPolicy 必须提供:
//   void OnSubmit(uint32_t block_id, uint32_t payload_size) noexcept;
```

---

## 5. 统一 API

### 5.1 统一动词表

| 阶段 | 统一动词 | 含义 | 旧命名 |
|------|---------|------|--------|
| 分配 | `Alloc()` | 从池中分配一个数据块 | `AllocBlock()` |
| 填充 | `GetWritable(block_id)` | 获取可写 payload 指针 | `GetBlockPayload()` |
| 提交 | `Submit(block_id, size)` | 标记数据就绪并通知消费者 | `SubmitBlock()` |
| 消费 | `GetReadable(block_id)` | 获取只读 payload 指针 | `GetBlockPayloadReadOnly()` |
| 释放 | `Release(block_id)` | 递减引用计数 | `ReleaseBlock()` |

### 5.2 Producer 基本流程

```cpp
auto bid = disp.Alloc();
uint8_t* p = disp.GetWritable(bid.value());
FillFrame(p, frame_data, frame_size);
disp.Submit(bid.value(), frame_size);
```

### 5.3 Consumer 基本流程

```cpp
const uint8_t* data = disp.GetReadable(block_id);
uint32_t len = disp.GetPayloadSize(block_id);
ProcessFrame(data, len);
disp.Release(block_id);
```

---

## 6. 核心数据结构

### 6.1 DataBlock -- 共享数据块

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
  // payload 紧随其后 (通过 kHeaderAlignedSize 偏移访问)
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
+20   alloc_time_us (8B)
+28   deadline_us   (8B)
+36   next_free     (4B)
+40   reserved      (4B)
+44   [padding to cache line boundary]
+64   payload[BlockSize]
```

### 6.2 BlockState 枚举

```cpp
enum class BlockState : uint8_t {
  kFree       = 0,   // 在 free list 中
  kAllocated  = 1,   // 已分配，生产者填充中
  kReady      = 2,   // 数据就绪，等待消费
  kProcessing = 3,   // 至少一个消费者正在处理
  kTimeout    = 5,   // 超时 (ScanTimeout 标记)
  kError      = 6,   // 异常 (ForceCleanup 标记)
};
```

### 6.3 StageConfig -- DAG 节点配置

```cpp
using StageHandler = void (*)(const uint8_t* data, uint32_t len,
                              uint32_t block_id, void* ctx);

struct StageConfig {
  const char* name;          // Stage 名称 (调试用)
  StageHandler handler;      // 处理函数 (nullptr = pass-through)
  void* handler_ctx;         // 处理函数上下文
  uint32_t timeout_ms;       // 单块处理超时 (0 = 使用全局默认)
};
```

---

## 7. StorePolicy 实现

### 7.1 InProcStore

嵌入式数组存储，适用于进程内模式。

```cpp
template <uint32_t BlockSize, uint32_t MaxBlocks>
struct InProcStore {
    static constexpr uint32_t kBlockStride = /* aligned(header_size + BlockSize) */;

    void Init() noexcept;                      // 初始化 free list + atomic 计数器
    DataBlock* GetBlock(uint32_t id) noexcept;  // &storage_[id * kBlockStride]

    std::atomic<uint32_t>& FreeHead() noexcept;
    std::atomic<uint32_t>& FreeCount() noexcept;
    std::atomic<uint32_t>& AllocCount() noexcept;

    // 误调用 Attach / Init(void*, uint32_t) 时编译报错
    template <typename T = void>
    void Attach(void*) noexcept { static_assert(sizeof(T) == 0, "Use ShmStore"); }
    template <typename T = void>
    void Init(void*, uint32_t) noexcept { static_assert(sizeof(T) == 0, "Use ShmStore"); }

private:
    alignas(OSP_JOB_BLOCK_ALIGN) uint8_t storage_[kBlockStride * MaxBlocks];
    alignas(kCacheLineSize) std::atomic<uint32_t> free_head_{0};
    alignas(kCacheLineSize) std::atomic<uint32_t> free_count_{MaxBlocks};
    alignas(kCacheLineSize) std::atomic<uint32_t> alloc_count_{0};
};
```

### 7.2 ShmStore

共享内存存储，适用于跨进程模式。

```cpp
namespace detail {
struct ShmPoolHeader {
    uint32_t magic;        // 0x4A4F4250 ('JOBP')
    uint32_t version;      // 1
    uint32_t block_size;   // BlockSize 模板参数
    uint32_t max_blocks;   // MaxBlocks 模板参数
    uint32_t block_stride; // kBlockStride
    uint32_t total_size;   // RequiredShmSize()
    uint32_t reserved[2];
    alignas(64) std::atomic<uint32_t> free_head;
    alignas(64) std::atomic<uint32_t> free_count;
    alignas(64) std::atomic<uint32_t> alloc_count;
};
}

template <uint32_t BlockSize, uint32_t MaxBlocks>
struct ShmStore {
    static constexpr uint32_t kShmHeaderSize =
        (sizeof(ShmPoolHeader) + OSP_JOB_BLOCK_ALIGN - 1) & ~(OSP_JOB_BLOCK_ALIGN - 1);

    /// Creator 路径: 初始化 header + free list
    void Init(void* shm_base, uint32_t shm_size) noexcept;

    /// Opener 路径: 验证 magic/version/params, 不初始化
    void Attach(void* shm_base) noexcept;

    DataBlock* GetBlock(uint32_t id) noexcept;
    std::atomic<uint32_t>& FreeHead() noexcept;   // header_->free_head
    std::atomic<uint32_t>& FreeCount() noexcept;   // header_->free_count
    std::atomic<uint32_t>& AllocCount() noexcept;  // header_->alloc_count

    static constexpr uint32_t RequiredShmSize() noexcept;

    // 误调用 Init() 无参版本时编译报错
    template <typename T = void>
    void Init() noexcept { static_assert(sizeof(T) == 0, "Use Init(shm_base, size)"); }

private:
    ShmPoolHeader* header_{nullptr};
    uint8_t* blocks_base_{nullptr};
};
```

SHM 内存布局:
```
+0                    ShmPoolHeader (cache-line aligned atomics)
+kShmHeaderSize       DataBlock[0] (header + payload)
+kShmHeaderSize+S     DataBlock[1]
...
+kShmHeaderSize+N*S   DataBlock[MaxBlocks-1]
```

### 7.3 Attach 安全校验

```cpp
void Attach(void* shm_base) noexcept {
    header_ = static_cast<ShmPoolHeader*>(shm_base);
    std::atomic_thread_fence(std::memory_order_acquire);
    OSP_ASSERT(header_->magic == OSP_JOB_POOL_MAGIC);
    OSP_ASSERT(header_->version == 1);
    OSP_ASSERT(header_->block_size == BlockSize);
    OSP_ASSERT(header_->max_blocks == MaxBlocks);
    blocks_base_ = static_cast<uint8_t*>(shm_base) + kShmHeaderSize;
}
```

---

## 8. NotifyPolicy 实现

### 8.1 DirectNotify (进程内)

```cpp
struct DirectNotify {
    void OnSubmit(uint32_t, uint32_t) noexcept {
        // 空操作 -- Pipeline::Execute() 由 DataDispatcher::Submit() 直接驱动
    }
};
```

### 8.2 ShmNotify (跨进程)

```cpp
struct ShmNotify {
    using NotifyFn = void (*)(uint32_t block_id, uint32_t payload_size, void* ctx);
    NotifyFn fn{nullptr};
    void* ctx{nullptr};

    void OnSubmit(uint32_t block_id, uint32_t payload_size) noexcept {
        if (fn != nullptr) fn(block_id, payload_size, ctx);
    }
};
```

典型用法: ShmNotify 回调向 ShmSpmcByteChannel 写入 8 字节 `{block_id, payload_size}`:

```cpp
auto& notify = disp.GetNotify();
notify.fn = [](uint32_t bid, uint32_t size, void* ctx) {
    auto* ch = static_cast<osp::ShmSpmcByteChannel*>(ctx);
    NotifyMsg msg{bid, size};
    ch->Write(reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
};
notify.ctx = &notify_channel;
```

---

## 9. DataDispatcher 完整 API

```cpp
template <typename Store,
          typename Notify = DirectNotify,
          uint32_t MaxStages = OSP_JOB_MAX_STAGES,
          uint32_t MaxEdges = OSP_JOB_MAX_EDGES>
class DataDispatcher {
public:
    // ====== 初始化 ======
    void Init(const Config& cfg) noexcept;                               // InProc
    void Init(const Config& cfg, void* shm_base, uint32_t size) noexcept; // InterProc Creator
    void Attach(void* shm_base) noexcept;                                 // InterProc Opener
    void Attach(const Config& cfg, void* shm_base) noexcept;              // Opener + Config

    // ====== Producer API ======
    expected<uint32_t, JobPoolError> Alloc() noexcept;
    uint8_t* GetWritable(uint32_t block_id) noexcept;
    void Submit(uint32_t block_id, uint32_t payload_size) noexcept;
    void Submit(uint32_t block_id, uint32_t size, uint32_t consumer_count) noexcept;

    // ====== Consumer API ======
    const uint8_t* GetReadable(uint32_t block_id) const noexcept;
    uint32_t GetPayloadSize(uint32_t block_id) const noexcept;
    bool Release(uint32_t block_id) noexcept;
    void AddRef(uint32_t block_id, uint32_t count) noexcept;

    // ====== Pipeline 配置 ======
    int32_t AddStage(const StageConfig& cfg) noexcept;
    bool AddEdge(int32_t from, int32_t to) noexcept;
    void SetEntryStage(int32_t id) noexcept;

    // ====== Backpressure + Fault ======
    void SetBackpressureCallback(BackpressureFn cb, void* ctx) noexcept;
    void SetFaultReporter(const FaultReporter& r, uint16_t slot) noexcept;

    // ====== Notify 配置 ======
    Notify& GetNotify() noexcept;

    // ====== 监控 ======
    uint32_t ScanTimeout() noexcept;
    uint32_t ForceCleanup(bool (*pred)(uint32_t, void*), void* ctx) noexcept;
    uint32_t FreeBlocks() const noexcept;
    uint32_t AllocBlocks() const noexcept;
    BlockState GetBlockState(uint32_t id) const noexcept;
    static constexpr uint32_t Capacity() noexcept;

    // ====== Consumer 管理 (ShmStore only, SFINAE-guarded) ======
    int32_t RegisterConsumer(uint32_t pid) noexcept;
    void UnregisterConsumer(int32_t slot_id) noexcept;
    void ConsumerHeartbeat(int32_t slot_id) noexcept;
    void TrackBlockHold(int32_t consumer_id, uint32_t block_id) noexcept;
    void TrackBlockRelease(int32_t consumer_id, uint32_t block_id) noexcept;
    uint32_t CleanupDeadConsumers() noexcept;
};
```

### 9.1 Consumer 管理 API (ShmStore only)

| 方法 | 说明 | 可用条件 |
|------|------|----------|
| `RegisterConsumer(pid)` | 注册消费者进程，返回 slot index (-1 = 满) | ShmStore only |
| `UnregisterConsumer(slot_id)` | 注销消费者 | ShmStore only |
| `ConsumerHeartbeat(slot_id)` | 更新心跳时间戳 | ShmStore only |
| `TrackBlockHold(consumer_id, block_id)` | 标记持有的数据块 (bitmap set) | ShmStore only |
| `TrackBlockRelease(consumer_id, block_id)` | 清除持有标记 (bitmap clear) | ShmStore only |
| `CleanupDeadConsumers()` | 回收死亡消费者持有的数据块 | ShmStore only |

### 9.2 便利别名

```cpp
// 进程内
template <uint32_t BS, uint32_t MB, uint32_t MS = 8, uint32_t ME = 16>
using InProcDispatcher = DataDispatcher<InProcStore<BS, MB>, DirectNotify, MS, ME>;

// 跨进程
template <uint32_t BS, uint32_t MB, uint32_t MS = 8, uint32_t ME = 16>
using ShmDispatcher = DataDispatcher<ShmStore<BS, MB>, ShmNotify, MS, ME>;
```

---

## 10. 数据块生命周期与状态机

### 10.1 状态转换图

```
Alloc()          Submit()         Pipeline开始       最后一个Release()
  |                |                |                    |
  v                v                v                    v
kFree --> kAllocated --> kReady --> kProcessing -------> kFree (Recycle)
                                       |
                                       +-- ScanTimeout --> kTimeout --> kFree
                                       +-- ForceCleanup -> kError   --> kFree
```

### 10.2 constexpr 状态转换表

BlockState 转换通过 `detail::kBlockStateTransition[7][7]` 编译期表校验:

| From \ To | Free | Alloc | Ready | Proc | Done | Timeout | Error |
|-----------|------|-------|-------|------|------|---------|-------|
| Free      |      | yes   |       |      |      |         |       |
| Allocated | yes  |       | yes   |      |      |         |       |
| Ready     | yes  |       |       | yes  |      | yes     | yes   |
| Processing| yes  |       |       |      |      | yes     | yes   |
| Done      | yes  |       |       |      |      |         |       |
| Timeout   | yes  |       |       |      |      |         |       |
| Error     | yes  |       |       |      |      |         |       |

- `Allocated -> Free`: 生产者放弃未提交的块
- `Ready -> Free`: 直接回收 (ScanTimeout/ForceCleanup/单引用释放)
- Debug builds: `SetState()` 内部 `OSP_ASSERT(IsValidBlockTransition(from, to))`
- Release builds: 零开销 (assert 被优化掉)

### 10.3 引用计数规则

```
Submit(block_id, size):
  InProc:   refcount = pipeline.EntryFanOut()  // DAG 入口的后继数量
  InterProc: refcount = consumer_count          // 用户显式传入

Pipeline fan-out (A -> B, A -> C):
  Submit: refcount = 2
  B Release: refcount 2 -> 1 (不回收)
  C Release: refcount 1 -> 0 -> Recycle

Pipeline serial (A -> B -> C):
  Submit: refcount = 1
  A 完成: AddRef(1) for B, Release own -> refcount 保持 1
  B 完成: AddRef(1) for C, Release own -> refcount 保持 1
  C 完成: Release -> refcount 0 -> Recycle
```

### 10.4 CAS 安全模式: CAS-first-then-setState

ScanTimeout 和 ForceCleanup 中的关键模式:

```cpp
// 正确: 先 CAS claim refcount, 再设状态
uint32_t cur = blk->refcount.load(std::memory_order_acquire);
bool claimed = false;
while (cur > 0) {
    if (blk->refcount.compare_exchange_weak(
            cur, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
        claimed = true;
        break;
    }
}
if (claimed) {
    blk->SetState(BlockState::kTimeout);  // 设状态在 CAS 之后
    Recycle(block_id);
}
```

错误模式 (已修复):
```cpp
// 错误: 先设状态, 再 CAS -- Race condition!
blk->SetState(BlockState::kTimeout);  // Release 看到 kTimeout 但 refcount > 0
// 此时 Release 可能 CAS 成功 refcount -> 0 -> Recycle (kFree)
// 然后 ScanTimeout 的 CAS 失败, 但 SetState 已经覆盖了 kFree -> 数据损坏
```

### 10.5 背压机制

```
Alloc():
  free_count = FreeCount()
  if free_count < backpressure_threshold:
    bp_callback_(free_count, bp_ctx_)  // 通知生产者
    fault_reporter_.Report(fault_slot_, free_count, kLow)
  if free_count == 0:
    return kPoolExhausted  // 生产者决定: 丢帧 or 等待
```

---

## 11. Pipeline DAG 拓扑

### 11.1 Pipeline 结构

```cpp
template <uint32_t MaxStages = 8, uint32_t MaxEdges = 16>
class Pipeline {
public:
    int32_t AddStage(const StageConfig& cfg) noexcept;
    bool AddEdge(int32_t from, int32_t to) noexcept;
    void SetEntryStage(int32_t id) noexcept;
    uint32_t EntryFanOut() const noexcept;

    template <typename PoolLike>
    void Execute(PoolLike& pool, uint32_t block_id) noexcept;
};
```

### 11.2 DAG 拓扑示例

```
4-stage DAG (示例: data_dispatcher/pipeline_demo.cpp):

  entry (pass-through)
    |
  preprocess (intensity filter)
    |
    +-- logging (seq check + stats)
    +-- fusion  (bounding box)
```

### 11.3 InterProc 模式: Pipeline + NotifyStage

InterProc 模式下，NotifyStage 可作为 Pipeline 叶子节点:

```
Producer 配置:
  Stage[crc] -> Stage[compress] -> Stage[notify] (叶子)

Submit() -> Pipeline::Execute()
  -> Stage[crc]: 数据校验
  -> Stage[compress]: 本地预处理
  -> Stage[notify]: AddRef(num_consumers) + ShmNotify.OnSubmit(block_id)
                    handler 返回后 Pipeline Release -> refcount 不为 0

Consumer 进程: 收到 block_id -> GetReadable -> 处理 -> Release
```

纯 1:N 通知 (无预处理) 场景下，可跳过 Pipeline 直接在 Submit 中触发 ShmNotify。

---

## 12. 跨进程架构

### 12.1 双共享内存段

```
         ShmStore Pool (/osp_dd_pool)
   32 blocks x 16016B, CAS-based refcount
                    |
+-------------------+-------------------+
|                   |                   |
Producer         Consumer-Logging  Consumer-Fusion
DataDispatcher   DataDispatcher    DataDispatcher
<ShmStore,Notify>  <ShmStore>        <ShmStore>
|                   |                   |
+-- ShmNotify --> ShmSpmcByteChannel <--+
|   (8B NotifyMsg: block_id + payload_size)
|
Monitor (DataDispatcher<ShmStore> + DebugShell)
|
Launcher (process manager)
```

两个共享内存段:
1. **Pool 段**: 大块共享内存 (`ShmStore`)，存放 DataBlock 数组，所有进程 mmap 同一段
2. **通知段**: 小型 `ShmSpmcByteChannel`，仅传输 8 字节 `{block_id, payload_size}`

### 12.2 Data Flow

1. Producer: `Alloc -> GetWritable -> FillFrame -> Submit(block_id, size, consumer_count)`
2. Submit 设置 `refcount = consumer_count`，ShmNotify 推送 NotifyMsg 到 SPMC channel
3. Consumer: 接收 NotifyMsg -> `GetReadable(block_id)` (零拷贝指针) -> 处理 -> `Release(block_id)`
4. Block 在 `refcount -> 0` 时由最后一个 `Release()` 调用回收

### 12.3 Creator / Opener 双路径

```cpp
// Creator (Producer 进程):
void* shm = CreatePoolShm(name, ShmStore::RequiredShmSize());
disp.Init(cfg, shm, ShmStore::RequiredShmSize());
// 退出前 UnlinkPoolShm(name)

// Opener (Consumer 进程):
void* shm = OpenPoolShm(name, ShmStore::RequiredShmSize());
disp.Attach(shm);
// 退出前 ClosePoolShm(shm, size) (不 Unlink)
```

---

## 13. 进程监管与崩溃恢复

### 13.1 监管架构

```
Launcher (process.hpp)
  |
  +-- 进程健康: IsProcessAlive() @ 500ms
  +-- 崩溃处理: 标记 consumer_slot.active = 0
  +-- 优雅关停: SIGTERM -> 2s -> SIGKILL
  |
  v
各子进程内部:
  +-- ThreadWatchdog: 线程心跳 @ 1Hz, 5s 超时
  +-- Timer: ScanTimeout() @ 1Hz, 检测卡死数据块
  +-- FaultCollector: 上报 kPoolExhausted / kJobTimeout
  +-- ShutdownManager: SIGTERM -> drain -> release -> exit
```

### 13.2 ConsumerSlot 结构 (共享内存中)

ConsumerSlot 数组位于共享内存中，紧跟 DataBlock 数组之后。ShmStore 要求 `MaxBlocks <= 64`，因为 `holding_mask` 使用 `uint64_t` 位图追踪持有的块。

```cpp
struct ConsumerSlot {
    std::atomic<uint32_t> active;         // 0=空闲, 1=活跃
    std::atomic<uint32_t> pid;            // Consumer 进程 PID
    std::atomic<uint64_t> heartbeat_us;   // 最后一次心跳
    std::atomic<uint64_t> holding_mask;   // 持有的 block bitmap (MaxBlocks <= 64)
};
```

DataDispatcher 通过 SFINAE 在 ShmStore 模式下启用消费者管理方法:

| 方法 | 说明 |
|------|------|
| `RegisterConsumer(pid)` | CAS 原子竞争 `active` 字段认领空闲 slot，返回 slot index (-1 = 满) |
| `UnregisterConsumer(slot_id)` | 注销消费者，清除 active 和 holding_mask |
| `ConsumerHeartbeat(slot_id)` | 更新心跳时间戳 |
| `TrackBlockHold(consumer_id, block_id)` | 在 holding_mask 中置位 (bitmap set) |
| `TrackBlockRelease(consumer_id, block_id)` | 在 holding_mask 中清位 (bitmap clear) |
| `CleanupDeadConsumers()` | 回收死亡消费者持有的数据块 (见 13.3) |

这些方法仅在 `StorePolicy = ShmStore` 时可见 (SFINAE guard)，InProcStore 模式下调用会产生编译错误。

### 13.3 崩溃恢复流程 (CleanupDeadConsumers)

```
1. Launcher: IsProcessAlive(consumer_pid) == false
2. Launcher: consumer_slots[cid].active.store(0)
3. CleanupDeadConsumers() (Producer 或 Monitor 周期调用):
   for each slot where active == 0 && holding_mask != 0:
     mask = holding_mask.exchange(0)       // 原子取走 mask
     while mask != 0:
       bit = CTZ(mask)                     // 最低置位位
       CAS refcount -= 1                   // 递减持有 block 的引用计数
       if refcount -> 0: Recycle(bit)      // 引用归零则回收
       mask &= ~(1ULL << bit)
4. FaultCollector 上报 kConsumerCrash
```

### 13.4 双重兜底

| 机制 | 作用 | 触发条件 |
|------|------|----------|
| holding_mask 精确回收 | 只释放崩溃 Consumer 持有的 block | consumer active=0 且 mask!=0 |
| deadline_us 超时回收 | 兜底: 即使 mask 不准也能回收 | block 超时未释放 |

---

## 14. 与现有模块的复用关系

DataDispatcher 不是独立系统，而是 newosp 现有模块的组合层:

| 需求 | 现有模块 | 复用方式 | 新增代码 |
|------|----------|----------|----------|
| 数据块内存池 | MemPool 设计 | 复用 free list 设计 | DataBlock + Store |
| 消费者生命周期 | HSM | HSM 驱动状态 (Connecting/Receiving/Stalled/Done) | 零 |
| 异常上报 | FaultCollector | 直接复用 | 零 |
| 数据就绪通知 (跨进程) | ShmSpmcByteChannel | 发 8B NotifyMsg (非完整帧) | 零 |
| 节点心跳监控 | ThreadWatchdog | 各进程内线程监控 | 零 |
| Worker 执行 | Pipeline | Stage 处理 DAG | Pipeline 类 |
| 进程管理 | Subprocess (process.hpp) | Launcher 启动/停止 | 零 |
| 定时统计 | TimerScheduler | 周期性 ScanTimeout + stats | 零 |
| 优雅关停 | ShutdownManager | drain -> release -> exit | 零 |
| 调试诊断 | DebugShell | dd_status/dd_scan/dd_blocks 命令 | ~50 行 |

---

## 15. 资源预算

以 LiDAR 场景 (BlockSize=16016, MaxBlocks=32) 为例:

### 15.1 内存占用

| 组件 | InProc | InterProc (shm) | 说明 |
|------|--------|-----------------|------|
| Pool Header | 内嵌 (~192B) | ShmPoolHeader (~256B) | 3 个 cache-line-aligned atomic |
| DataBlock[32] | 512KB | 512KB | 32 x (64B hdr + 16016B payload, aligned) |
| Pipeline (8 stages) | ~512B | ~512B | 两种模式相同 |
| 通知通道 | 0 | ~4KB | ShmSpmcByteChannel |
| **总计** | ~513KB | ~517KB | 几乎无差异 |

### 15.2 编译产物

| 指标 | 值 |
|------|-----|
| 头文件大小 | ~1000 行 (data_dispatcher.hpp) |
| 模板实例化 | 仅实例化所用的 Store/Notify 组合 |
| 运行时开销 | 零 (编译期分发, 无虚函数) |

---

## 16. 编译期配置

| 宏 | 默认值 | 说明 |
|----|--------|------|
| `OSP_JOB_POOL_MAGIC` | `0x4A4F4250` | 共享内存 magic number |
| `OSP_JOB_BLOCK_ALIGN` | 64 | DataBlock 对齐 (cache line) |
| `OSP_JOB_MAX_STAGES` | 8 | Pipeline 最大 stage 数 |
| `OSP_JOB_MAX_EDGES` | 16 | Pipeline 最大边数 |
| `OSP_JOB_MAX_CONSUMERS` | 8 | 最大 ConsumerSlot 数 (ShmStore) |

---

## 17. 错误码

```cpp
enum class JobPoolError : uint8_t {
  kPoolExhausted = 0,  // 内存池满 (背压)
  kInvalidBlockId,     // block_id 越界
  kBlockNotReady,      // 数据块未就绪
  kBlockTimeout,       // 处理超时
  kPipelineFull,       // stage/edge 数量超限
  kNoEntryStage,       // 未配置入口 stage
  kInvalidStage,       // 无效 stage 索引
};
```

---

## 18. 实施状态

### 18.1 已完成

| 阶段 | 内容 | 状态 |
|------|------|------|
| Phase 0 | API 命名统一 (Alloc/GetWritable/Submit/GetReadable/Release) | 已完成 |
| Phase 1 | 核心重构: InProcStore + DirectNotify + DataDispatcher 模板化 | 已完成 |
| Phase 2 | ShmStore + ShmNotify + 跨进程 demo | 已完成 |
| Phase 3 | ConsumerSlot + per-consumer bitmap + 崩溃回收 | 已完成 |
| Phase 4 | 集成测试 (fork + kill + 验证 block 回收) | 已完成 |
| Phase 5 | BlockState constexpr 转换表 + debug assert | 已完成 |

### 18.2 示例程序

| 文件 | 说明 | 模式 |
|------|------|------|
| `pipeline_demo.cpp` | 4-stage DAG pipeline (InProcStore) | InProc |
| `unified_api_demo.cpp` | Store-agnostic API 演示 | InProc |
| `producer.cpp` | HSM-driven producer (ShmStore + ShmNotify) | Cross-Process |
| `consumer_logging.cpp` | HSM-driven logging consumer (ShmStore) | Cross-Process |
| `consumer_fusion.cpp` | HSM-driven fusion consumer (ShmStore) | Cross-Process |
| `monitor.cpp` | Pool inspector + DebugShell (ShmStore) | Cross-Process |
| `launcher.cpp` | Process manager (spawn/health/restart) | Cross-Process |

---

## 19. 文件清单

| 文件 | 说明 | 行数 |
|------|------|------|
| `include/osp/data_dispatcher.hpp` | DataBlock + Pipeline + InProcStore + ShmStore + ConsumerSlot + DataDispatcher | ~1280 |
| `include/osp/job_pool.hpp` | 向后兼容重定向 (#include data_dispatcher.hpp) | ~10 |
| `tests/test_data_dispatcher.cpp` | 单元测试 + ShmStore + fork 跨进程测试 | ~1280 |
| `examples/data_dispatcher/` | InProc + Cross-Process demo (7 个源文件) | ~800 |
| `docs/design_data_dispatcher_zh.md` | 本文档 | ~850 |
