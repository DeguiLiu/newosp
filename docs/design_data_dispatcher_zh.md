# DataDispatcher: 统一数据分发架构设计

> 版本: 3.0
> 日期: 2026-02-17
> 状态: 已实现 (Phase 0-5 完成)

---

## 目录

1. [设计目标](#1-设计目标)
2. [设计约束](#2-设计约束)
3. [架构决策: StorePolicy + NotifyPolicy 双策略模板](#3-架构决策-storepolicy--notifypolicy-双策略模板)
4. [统一 API](#4-统一-api)
5. [核心数据结构](#5-核心数据结构)
6. [StorePolicy 实现](#6-storepolicy-实现)
7. [NotifyPolicy 实现](#7-notifypolicy-实现)
8. [DataDispatcher 完整 API](#8-datadispatcher-完整-api)

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

## 3. 架构决策: StorePolicy + NotifyPolicy 双策略模板

### 3.1 方案选择

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

### 3.2 架构总览

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

### 3.3 StorePolicy Concept (C++17 隐式约束)

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

### 3.4 NotifyPolicy Concept

```cpp
// NotifyPolicy 必须提供:
//   void OnSubmit(uint32_t block_id, uint32_t payload_size) noexcept;
```

---

## 4. 统一 API

### 4.1 统一动词表

| 阶段 | 统一动词 | 含义 | 旧命名 |
|------|---------|------|--------|
| 分配 | `Alloc()` | 从池中分配一个数据块 | `AllocBlock()` |
| 填充 | `GetWritable(block_id)` | 获取可写 payload 指针 | `GetBlockPayload()` |
| 提交 | `Submit(block_id, size)` | 标记数据就绪并通知消费者 | `SubmitBlock()` |
| 消费 | `GetReadable(block_id)` | 获取只读 payload 指针 | `GetBlockPayloadReadOnly()` |
| 释放 | `Release(block_id)` | 递减引用计数 | `ReleaseBlock()` |

### 4.2 Producer 基本流程

```cpp
auto bid = disp.Alloc();
uint8_t* p = disp.GetWritable(bid.value());
FillFrame(p, frame_data, frame_size);
disp.Submit(bid.value(), frame_size);
```

### 4.3 Consumer 基本流程

```cpp
const uint8_t* data = disp.GetReadable(block_id);
uint32_t len = disp.GetPayloadSize(block_id);
ProcessFrame(data, len);
disp.Release(block_id);
```

---

## 5. 核心数据结构

### 5.1 DataBlock -- 共享数据块

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

### 5.2 BlockState 枚举

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

### 5.3 StageConfig -- DAG 节点配置

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

## 6. StorePolicy 实现

### 6.1 InProcStore

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

### 6.2 ShmStore

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

### 6.3 Attach 安全校验

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

## 7. NotifyPolicy 实现

### 7.1 DirectNotify (进程内)

```cpp
struct DirectNotify {
    void OnSubmit(uint32_t, uint32_t) noexcept {
        // 空操作 -- Pipeline::Execute() 由 DataDispatcher::Submit() 直接驱动
    }
};
```

### 7.2 ShmNotify (跨进程)

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

## 8. DataDispatcher 完整 API

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

### 8.1 Consumer 管理 API (ShmStore only)

| 方法 | 说明 | 可用条件 |
|------|------|----------|
| `RegisterConsumer(pid)` | 注册消费者进程，返回 slot index (-1 = 满) | ShmStore only |
| `UnregisterConsumer(slot_id)` | 注销消费者 | ShmStore only |
| `ConsumerHeartbeat(slot_id)` | 更新心跳时间戳 | ShmStore only |
| `TrackBlockHold(consumer_id, block_id)` | 标记持有的数据块 (bitmap set) | ShmStore only |
| `TrackBlockRelease(consumer_id, block_id)` | 清除持有标记 (bitmap clear) | ShmStore only |
| `CleanupDeadConsumers()` | 回收死亡消费者持有的数据块 | ShmStore only |

### 8.2 便利别名

```cpp
// 进程内
template <uint32_t BS, uint32_t MB, uint32_t MS = 8, uint32_t ME = 16>
using InProcDispatcher = DataDispatcher<InProcStore<BS, MB>, DirectNotify, MS, ME>;

// 跨进程
template <uint32_t BS, uint32_t MB, uint32_t MS = 8, uint32_t ME = 16>
using ShmDispatcher = DataDispatcher<ShmStore<BS, MB>, ShmNotify, MS, ME>;
```

---

