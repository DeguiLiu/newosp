# DataDispatcher 统一架构: 最小可落地版本

> 版本: 2.0
> 日期: 2026-02-17
> 状态: 待评审
> 前置文档: [design_job_pool_zh.md](design_job_pool_zh.md)
> 合并来源: zh2 (主稿, StorePolicy + 对比矩阵) + zh3 (统一命名, NotifyPolicy, 用户视角)

---

## 目录

1. [问题定义](#1-问题定义)
2. [设计约束](#2-设计约束)
3. [现状分析](#3-现状分析)
4. [前导: 统一 API 命名](#4-前导-统一-api-命名)
5. [方案 A: StorePolicy + NotifyPolicy 双策略模板](#5-方案-a-storepolicy--notifypolicy-双策略模板)
6. [方案 B: 两个独立具体类](#6-方案-b-两个独立具体类)
7. [对比决策矩阵](#7-对比决策矩阵)
8. [关键设计决策](#8-关键设计决策)
9. [资源预算](#9-资源预算)
10. [实施计划](#10-实施计划)

---

## 1. 问题定义

### 1.1 核心需求

统一 InProc (`job_pool.hpp`) 和 InterProc (`shm_transport.hpp`) 两套数据分发机制，让业务代码用同一套 API 完成 1:N 数据分发，不感知底层是线程间共享块还是跨进程共享内存。

```
业务代码 (统一 API):
  Alloc -> GetWritable -> Submit -> GetReadable -> Release
           |                           |
     +-----+-----+              +-----+-----+
     | InProc    |              | InterProc  |
     | 栈/静态   |              | mmap shm   |
     | 数组      |              | 共享内存   |
     +-----------+              +------------+
```

### 1.2 驱动因素

当前 `job_pool.hpp` 的 `DataDispatcher` 仅支持进程内模式 (storage 是嵌入式数组)。`shm_transport.hpp` 的 SPMC Ring Buffer 是数据拷贝模式，不支持引用计数和 DAG Pipeline。两套机制各自独立，且 API 命名风格不一致 (`AllocBlock/GetBlockPayload` vs `Write/Read`)，缺乏统一的零拷贝数据分发层。

### 1.3 不在范围内

- N:1 通信 (AsyncBus + Node.Subscribe / 多条 ShmChannel + DataFusion) -- 语义不同，不强行统一
- 1:1 流式传输 (ShmSpscByteRing) -- 字节流语义，不适合块池模型
- 现有 Ring Buffer 系列 (ShmRingBuffer / ShmSpscByteRing / ShmSpmcByteRing) -- 保持不变，与新方案正交共存

---

## 2. 设计约束

| 编号 | 约束 | 来源 |
|------|------|------|
| C1 | 可以破坏现有 API，允许重构合并 job_pool.hpp | 需求方确认 |
| C2 | InterProc 采用共享内存 DataBlock 零拷贝 + block_id 通知 | 需求 |
| C3 | 保留: 引用计数、DAG Pipeline、背压、故障上报、超时检测 | 需求 |
| C4 | 编译期确定部署模式，不需要运行时切换 | 需求 |
| C5 | 先落地可用统一层，后续再做并发/性能增强 | 需求 |
| C6 | 零运行时开销 (编译期分发) | CLAUDE.md 嵌入式原则 |
| C7 | Header-only, C++17, `-fno-exceptions -fno-rtti` 兼容 | 项目规范 |
| C8 | 支持 1:N、N:1、1:1 三种通信模式 (N:1 通过多 Producer 并发 Alloc) | 分析确认 |

---

## 3. 现状分析

### 3.1 InProc 与 InterProc 的差异矩阵

对现有代码 (`job_pool.hpp:163-437`, `shm_transport.hpp:1102-1343`) 逐项比对:

| 维度 | InProc (JobPool) | InterProc (需新增) | 差异程度 |
|------|------------------|--------------------|----------|
| **块存储** | `uint8_t storage_[]` 嵌入数组 | `void* shm_base_` mmap 区域 | 核心差异 |
| **池元数据** | 成员 `atomic<uint32_t>` | shm header 内 `atomic<uint32_t>` | 核心差异 |
| **Submit 通知** | `Pipeline::Execute()` 同步 | push `block_id` + futex wake | 核心差异 |
| `GetBlock(id)` | `&storage_[id * stride]` | `shm_base + header + id * stride` | 1 行 |
| `Init()` | 零初始化数组 | `shm_open` + `mmap` + 初始化 header | 差异较大 |
| **Alloc()** | CAS free_head | **完全相同** | 0 行 |
| **Release()** | CAS refcount + Recycle | **完全相同** | 0 行 |
| **Recycle()** | CAS push to free list | **完全相同** | 0 行 |
| **ScanTimeout()** | CAS reclaim loop | **完全相同** | 0 行 |
| **ForceCleanup()** | CAS reclaim loop | **完全相同** | 0 行 |
| DataBlock 布局 | 44B header + payload | **完全相同** | 0 行 |
| Pipeline | 支持 DAG 拓扑 | 同结构 (执行位置不同) | 见 8.1 |
| BackpressureFn | 函数指针 + ctx | **完全相同** | 0 行 |
| FaultReporter | 函数指针 + ctx | **完全相同** | 0 行 |

结论: **核心差异 3 处** -- 块存储位置、池元数据位置、Submit 后通知路径。CAS 无锁逻辑 (~120 行) 完全相同。

### 3.2 CAS 无锁逻辑的复杂度与 Bug 历史

```
Alloc():       CAS free_head loop + 状态初始化          ~25 行
Release():     CAS refcount loop + Recycle 触发         ~20 行
Recycle():     CAS push to free_head                    ~15 行
ScanTimeout(): 遍历 + CAS refcount claim + Recycle     ~30 行
ForceCleanup():遍历 + predicate + CAS claim + Recycle  ~25 行
AddRef():      fetch_add                                 ~5 行
                                                  合计 ~120 行
```

上一次 code review 中，这 120 行发现并修复了 3 个线程安全 bug:
- `ScanTimeout` 直接 `store(0)` 导致与 `Release` 双重回收
- `ForceCleanup` 使用 `fetch_sub` 导致下溢
- `Release` 使用 `fetch_sub` 在 ScanTimeout 已回收时触发 assert 失败

**这是选择方案的关键考量因素**: 是否将这 120 行危险代码复制为两份。

---

## 4. 前导: 统一 API 命名

无论选择哪个方案，第一步统一 API 命名约定。这是低风险、高收益的前导改动，可独立提交。

### 4.1 统一动词表

| 阶段 | 统一动词 | 含义 | 当前命名 (待改) |
|------|---------|------|-----------------|
| 分配 | `Alloc()` | 从池中分配一个数据块 | `AllocBlock()` |
| 填充 | `GetWritable(block_id)` | 获取可写 payload 指针 | `GetBlockPayload()` |
| 提交 | `Submit(block_id, size)` | 标记数据就绪并通知消费者 | `SubmitBlock()` |
| 消费 | `GetReadable(block_id)` | 获取只读 payload 指针 | `GetBlockPayloadReadOnly()` |
| 释放 | `Release(block_id)` | 递减引用计数 | `ReleaseBlock()` |

同时保持:
- AsyncBus/Node 的 `Publish/Subscribe` 不变 (消息总线语义，非通道语义)
- ShmChannel 的 `Write/Read` 不变 (流模式，已符合约定)

### 4.2 改动范围

```diff
- expected<uint32_t, JobPoolError> AllocBlock() noexcept;
+ expected<uint32_t, JobPoolError> Alloc() noexcept;

- uint8_t* GetBlockPayload(uint32_t block_id) noexcept;
+ uint8_t* GetWritable(uint32_t block_id) noexcept;

- void SubmitBlock(uint32_t block_id, uint32_t payload_size) noexcept;
+ void Submit(uint32_t block_id, uint32_t payload_size) noexcept;

- const uint8_t* GetBlockPayloadReadOnly(uint32_t block_id) const noexcept;
+ const uint8_t* GetReadable(uint32_t block_id) const noexcept;

- bool ReleaseBlock(uint32_t block_id) noexcept;
+ bool Release(uint32_t block_id) noexcept;
```

影响: DataDispatcher 类 + test_job_pool.cpp + 相关 demo。约 100 行改动，零逻辑变更。

---

## 5. 方案 A: StorePolicy + NotifyPolicy 双策略模板

### 5.1 设计思路

将 3 处核心差异映射为 2 个策略参数:

- **StorePolicy**: 块存储位置 + 池元数据位置 (解决差异 1、2)
- **NotifyPolicy**: Submit 后通知消费者的路径 (解决差异 3)

CAS 无锁逻辑只写一份，通过 StorePolicy 的 5 个方法访问底层存储。

```
DataDispatcher<StorePolicy, NotifyPolicy, MaxStages, MaxEdges>
│
├── StorePolicy (数据块存储)
│   ├── InProcStore<BlockSize, MaxBlocks>   -- 嵌入式数组 + 成员 atomic
│   └── ShmStore<BlockSize, MaxBlocks>      -- mmap 指针 + header atomic
│
├── NotifyPolicy (消费者通知)
│   ├── DirectNotify     -- 同步: Pipeline::Execute (进程内)
│   └── ShmNotify        -- 跨进程: push block_id + futex wake
│
├── Pipeline<MaxStages, MaxEdges>  -- DAG 拓扑 (两种模式共享)
├── FaultReporter                  -- 故障上报 (两种模式共享)
└── BackpressureFn                 -- 背压回调 (两种模式共享)
```

### 5.2 StorePolicy Concept

```cpp
// Implicit concept (C++17, 通过 static_assert 约束)
//
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

### 5.3 NotifyPolicy Concept

```cpp
// NotifyPolicy 必须提供:
//   void OnSubmit(uint32_t block_id, uint32_t payload_size) noexcept;
//       -- Submit 完成后调用, 负责通知消费者
```

### 5.4 InProcStore

```cpp
template <uint32_t BlockSize, uint32_t MaxBlocks>
struct InProcStore {
    static constexpr uint32_t kBlockStride = /* aligned header + payload */;
    static constexpr uint32_t kHeaderAlignedSize = /* aligned DataBlock header */;

    void Init() noexcept {
        for (uint32_t i = 0; i < MaxBlocks; ++i) {
            DataBlock* blk = GetBlock(i);
            blk->Reset();
            blk->block_id = i;
            blk->next_free = (i + 1 < MaxBlocks) ? (i + 1) : kInvalidIndex;
        }
        free_head_.store(0, std::memory_order_relaxed);
        free_count_.store(MaxBlocks, std::memory_order_relaxed);
        alloc_count_.store(0, std::memory_order_relaxed);
    }

    // InProcStore 不支持 Attach, 误调用时编译报错
    // 实际约束: DataDispatcher::Attach() 通过 SFINAE 或文档约束
    // 仅对 ShmStore 有效; InProcStore 调用 Attach 触发 static_assert
    template <typename T = void>
    void Attach(void*) noexcept {
        static_assert(sizeof(T) == 0,
            "InProcStore does not support Attach(). "
            "Use ShmStore for inter-process mode.");
    }

    DataBlock* GetBlock(uint32_t id) noexcept {
        return reinterpret_cast<DataBlock*>(
            &storage_[static_cast<size_t>(id) * kBlockStride]);
    }
    const DataBlock* GetBlock(uint32_t id) const noexcept {
        return reinterpret_cast<const DataBlock*>(
            &storage_[static_cast<size_t>(id) * kBlockStride]);
    }

    std::atomic<uint32_t>& FreeHead() noexcept { return free_head_; }
    std::atomic<uint32_t>& FreeCount() noexcept { return free_count_; }
    std::atomic<uint32_t>& AllocCount() noexcept { return alloc_count_; }
    static constexpr uint32_t Capacity() noexcept { return MaxBlocks; }
    static constexpr uint32_t PayloadCapacity() noexcept { return BlockSize; }

private:
    alignas(OSP_JOB_BLOCK_ALIGN) uint8_t storage_[kBlockStride * MaxBlocks];
    alignas(kCacheLineSize) std::atomic<uint32_t> free_head_{0};
    alignas(kCacheLineSize) std::atomic<uint32_t> free_count_{MaxBlocks};
    alignas(kCacheLineSize) std::atomic<uint32_t> alloc_count_{0};
};
```

### 5.5 ShmStore

```cpp
template <uint32_t BlockSize, uint32_t MaxBlocks>
struct ShmStore {
    /// shm 布局: [ShmPoolHeader][DataBlock[0]]...[DataBlock[N-1]][ConsumerSlot[0]...]
    struct ShmPoolHeader {
        uint32_t magic;
        uint32_t version;
        uint32_t block_size;
        uint32_t block_count;
        uint32_t block_stride;
        uint32_t max_consumers;
        alignas(kCacheLineSize) std::atomic<uint32_t> free_head;
        alignas(kCacheLineSize) std::atomic<uint32_t> free_count;
        alignas(kCacheLineSize) std::atomic<uint32_t> alloc_count;
    };

    static constexpr uint32_t kBlockStride = /* same as InProcStore */;
    static constexpr uint32_t kHeaderAlignedSize = /* aligned DataBlock header */;
    static constexpr uint32_t kShmHeaderSize =
        (sizeof(ShmPoolHeader) + OSP_JOB_BLOCK_ALIGN - 1) & ~(OSP_JOB_BLOCK_ALIGN - 1);

    /// Creator: shm_open(O_CREAT) + mmap + 初始化 header + free list
    void Init(void* shm_base, uint32_t shm_size) noexcept {
        header_ = static_cast<ShmPoolHeader*>(shm_base);
        blocks_base_ = static_cast<uint8_t*>(shm_base) + kShmHeaderSize;
        header_->magic = OSP_JOB_POOL_MAGIC;
        header_->version = 1;
        header_->block_size = BlockSize;
        header_->block_count = MaxBlocks;
        header_->block_stride = kBlockStride;
        // free list 初始化 (逻辑同 InProcStore::Init)
        for (uint32_t i = 0; i < MaxBlocks; ++i) {
            DataBlock* blk = GetBlock(i);
            blk->Reset();
            blk->block_id = i;
            blk->next_free = (i + 1 < MaxBlocks) ? (i + 1) : kInvalidIndex;
        }
        header_->free_head.store(0, std::memory_order_relaxed);
        header_->free_count.store(MaxBlocks, std::memory_order_relaxed);
        header_->alloc_count.store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
    }

    /// Opener: shm_open(O_RDWR) + mmap, 不初始化, 验证 magic
    void Attach(void* shm_base) noexcept {
        header_ = static_cast<ShmPoolHeader*>(shm_base);
        blocks_base_ = static_cast<uint8_t*>(shm_base) + kShmHeaderSize;
        std::atomic_thread_fence(std::memory_order_acquire);
        OSP_ASSERT(header_->magic == OSP_JOB_POOL_MAGIC);
        OSP_ASSERT(header_->block_size == BlockSize);
        OSP_ASSERT(header_->block_count == MaxBlocks);
    }

    DataBlock* GetBlock(uint32_t id) noexcept {
        return reinterpret_cast<DataBlock*>(
            blocks_base_ + static_cast<size_t>(id) * kBlockStride);
    }
    const DataBlock* GetBlock(uint32_t id) const noexcept {
        return reinterpret_cast<const DataBlock*>(
            blocks_base_ + static_cast<size_t>(id) * kBlockStride);
    }

    std::atomic<uint32_t>& FreeHead() noexcept { return header_->free_head; }
    std::atomic<uint32_t>& FreeCount() noexcept { return header_->free_count; }
    std::atomic<uint32_t>& AllocCount() noexcept { return header_->alloc_count; }
    static constexpr uint32_t Capacity() noexcept { return MaxBlocks; }
    static constexpr uint32_t PayloadCapacity() noexcept { return BlockSize; }

    /// 计算所需 shm 总大小 (含 ConsumerSlot 数组)
    static constexpr uint32_t RequiredShmSize(
            uint32_t max_consumers = OSP_JOB_MAX_CONSUMERS) noexcept {
        uint32_t consumer_slots_size =
            (max_consumers * sizeof(ConsumerSlot) + OSP_JOB_BLOCK_ALIGN - 1)
            & ~(OSP_JOB_BLOCK_ALIGN - 1);
        return kShmHeaderSize + kBlockStride * MaxBlocks + consumer_slots_size;
    }

private:
    ShmPoolHeader* header_{nullptr};
    uint8_t* blocks_base_{nullptr};
};
```

### 5.6 DirectNotify + ShmNotify

```cpp
/// 进程内通知: 同步执行 Pipeline
struct DirectNotify {
    void OnSubmit(uint32_t /*block_id*/, uint32_t /*payload_size*/) noexcept {
        // 空操作 -- DataDispatcher::Submit() 内部直接调 pipeline_.Execute()
        // DirectNotify 仅作为占位, Pipeline 执行由 Dispatcher 驱动
    }
};

/// 跨进程通知: push block_id + futex wake
///
/// ShmNotify 持有通知通道的引用, 由用户在 Init 时注入。
/// 通道实现可以是 ShmSpmcByteChannel (多消费者) 或 pipe + futex (轻量)。
struct ShmNotify {
    using NotifyFn = void (*)(uint32_t block_id, uint32_t payload_size, void* ctx);
    NotifyFn fn{nullptr};
    void* ctx{nullptr};

    void OnSubmit(uint32_t block_id, uint32_t payload_size) noexcept {
        if (fn != nullptr) {
            fn(block_id, payload_size, ctx);
        }
    }
};
```

### 5.7 统一 DataDispatcher

```cpp
/// @tparam Store   InProcStore<BS,MB> 或 ShmStore<BS,MB>
/// @tparam Notify  DirectNotify 或 ShmNotify
template <typename Store,
          typename Notify = DirectNotify,
          uint32_t MaxStages = OSP_JOB_MAX_STAGES,
          uint32_t MaxEdges = OSP_JOB_MAX_EDGES>
class DataDispatcher {
public:
    using PipelineType = Pipeline<MaxStages, MaxEdges>;

    struct Config {
        const char* name{""};
        uint32_t default_timeout_ms{0};
        uint32_t backpressure_threshold{0};
    };

    // ====== 初始化 ======

    /// InProc: store_.Init() 内部零初始化
    void Init(const Config& cfg) noexcept {
        cfg_ = cfg;
        store_.Init();
    }

    /// InterProc Creator: store_.Init(shm_base, size)
    void Init(const Config& cfg, void* shm_base, uint32_t shm_size) noexcept {
        cfg_ = cfg;
        store_.Init(shm_base, shm_size);
    }

    /// InterProc Opener: store_.Attach(shm_base)
    void Attach(void* shm_base) noexcept {
        store_.Attach(shm_base);
    }

    // ====== Producer API ======

    expected<uint32_t, JobPoolError> Alloc() noexcept {
        // 背压检查
        uint32_t free = store_.FreeCount().load(std::memory_order_relaxed);
        if (cfg_.backpressure_threshold > 0 && free <= cfg_.backpressure_threshold) {
            if (bp_fn_ != nullptr) bp_fn_(free, bp_ctx_);
            if (fault_reporter_.fn != nullptr) {
                fault_reporter_.Report(fault_slot_, free, FaultPriority::kLow);
            }
        }
        // CAS free list (单份实现, 两种 Store 共享)
        uint32_t head = store_.FreeHead().load(std::memory_order_acquire);
        while (head != detail::kJobInvalidIndex) {
            DataBlock* blk = store_.GetBlock(head);
            uint32_t next = blk->next_free;
            if (store_.FreeHead().compare_exchange_weak(
                    head, next, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                store_.FreeCount().fetch_sub(1, std::memory_order_relaxed);
                store_.AllocCount().fetch_add(1, std::memory_order_relaxed);
                blk->SetState(BlockState::kAllocated);
                blk->alloc_time_us = SteadyNowUs();
                blk->payload_size = 0;
                blk->fault_id = 0;
                blk->deadline_us = 0;
                blk->refcount.store(0, std::memory_order_relaxed);
                return expected<uint32_t, JobPoolError>::success(head);
            }
        }
        return expected<uint32_t, JobPoolError>::error(JobPoolError::kPoolExhausted);
    }

    uint8_t* GetWritable(uint32_t block_id) noexcept {
        OSP_ASSERT(block_id < Store::Capacity());
        return reinterpret_cast<uint8_t*>(store_.GetBlock(block_id))
               + Store::kHeaderAlignedSize;
    }

    void Submit(uint32_t block_id, uint32_t payload_size) noexcept {
        OSP_ASSERT(block_id < Store::Capacity());
        DataBlock* blk = store_.GetBlock(block_id);
        blk->payload_size = payload_size;
        uint32_t timeout = cfg_.default_timeout_ms;
        if (timeout > 0) {
            blk->deadline_us = SteadyNowUs() + static_cast<uint64_t>(timeout) * 1000;
        }
        uint32_t refcount = pipeline_.EntryFanOut();
        blk->refcount.store(refcount, std::memory_order_release);
        blk->SetState(BlockState::kReady);
        // Pipeline 执行 (包含所有 Stage: 业务处理 + NotifyStage)
        // InProc: 业务 Stage 同步执行, 最后 Release 回收
        // InterProc: 业务预处理 Stage + NotifyStage (叶子) 触发跨进程通知
        pipeline_.Execute(*this, block_id);
    }

    // ====== Consumer API ======

    const uint8_t* GetReadable(uint32_t block_id) const noexcept {
        OSP_ASSERT(block_id < Store::Capacity());
        return reinterpret_cast<const uint8_t*>(store_.GetBlock(block_id))
               + Store::kHeaderAlignedSize;
    }

    uint32_t GetPayloadSize(uint32_t block_id) const noexcept {
        OSP_ASSERT(block_id < Store::Capacity());
        return store_.GetBlock(block_id)->payload_size;
    }

    bool Release(uint32_t block_id) noexcept {
        OSP_ASSERT(block_id < Store::Capacity());
        DataBlock* blk = store_.GetBlock(block_id);
        // CAS loop (单份实现, 防 ScanTimeout 并发回收)
        uint32_t cur = blk->refcount.load(std::memory_order_acquire);
        while (cur > 0) {
            if (blk->refcount.compare_exchange_weak(
                    cur, cur - 1, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                if (cur == 1) {
                    Recycle(block_id);
                    return true;
                }
                return false;
            }
        }
        return false;
    }

    void AddRef(uint32_t block_id, uint32_t count) noexcept {
        OSP_ASSERT(block_id < Store::Capacity());
        store_.GetBlock(block_id)->refcount.fetch_add(count, std::memory_order_release);
    }

    // ====== Pipeline 配置 ======

    int32_t AddStage(const StageConfig& cfg) noexcept { return pipeline_.AddStage(cfg); }
    bool AddEdge(int32_t from, int32_t to) noexcept { return pipeline_.AddEdge(from, to); }
    void SetEntryStage(int32_t id) noexcept { pipeline_.SetEntryStage(id); }

    // ====== Backpressure + Fault ======

    void SetBackpressureCallback(BackpressureFn cb, void* ctx) noexcept {
        bp_fn_ = cb; bp_ctx_ = ctx;
    }
    void SetFaultReporter(const FaultReporter& r, uint16_t slot) noexcept {
        fault_reporter_ = r; fault_slot_ = slot;
    }

    // ====== Notify 配置 (ShmNotify 模式) ======

    Notify& GetNotify() noexcept { return notify_; }

    // ====== Monitoring ======

    uint32_t ScanTimeout(void (*on_timeout)(uint32_t, void*), void* ctx) noexcept {
        // 同现有实现, CAS reclaim loop, 调 store_.GetBlock()
        uint32_t count = 0;
        uint64_t now = SteadyNowUs();
        for (uint32_t i = 0; i < Store::Capacity(); ++i) {
            DataBlock* blk = store_.GetBlock(i);
            auto st = blk->GetState();
            if (st == BlockState::kProcessing || st == BlockState::kReady) {
                if (blk->deadline_us != 0 && now > blk->deadline_us) {
                    blk->SetState(BlockState::kTimeout);
                    if (on_timeout) on_timeout(i, ctx);
                    uint32_t cur = blk->refcount.load(std::memory_order_acquire);
                    while (cur > 0) {
                        if (blk->refcount.compare_exchange_weak(
                                cur, 0, std::memory_order_acq_rel,
                                std::memory_order_acquire)) {
                            Recycle(i);
                            break;
                        }
                    }
                    ++count;
                }
            }
        }
        return count;
    }

    uint32_t ForceCleanup(bool (*pred)(uint32_t, void*), void* ctx) noexcept {
        // 同现有实现, CAS reclaim loop
        uint32_t count = 0;
        for (uint32_t i = 0; i < Store::Capacity(); ++i) {
            DataBlock* blk = store_.GetBlock(i);
            auto st = blk->GetState();
            if (st == BlockState::kProcessing || st == BlockState::kReady) {
                if (pred && pred(i, ctx)) {
                    blk->SetState(BlockState::kError);
                    uint32_t cur = blk->refcount.load(std::memory_order_acquire);
                    while (cur > 0) {
                        if (blk->refcount.compare_exchange_weak(
                                cur, 0, std::memory_order_acq_rel,
                                std::memory_order_acquire)) {
                            Recycle(i);
                            break;
                        }
                    }
                    ++count;
                }
            }
        }
        return count;
    }

    uint32_t FreeBlocks() const noexcept {
        return store_.FreeCount().load(std::memory_order_relaxed);
    }
    uint32_t AllocBlocks() const noexcept {
        return store_.AllocCount().load(std::memory_order_relaxed);
    }
    static constexpr uint32_t Capacity() noexcept { return Store::Capacity(); }

    // ====== Pipeline 需要的 pool-like 接口 (供 Pipeline::Execute 调用) ======

    const uint8_t* GetPayloadReadOnly(uint32_t id) const noexcept { return GetReadable(id); }

    // ====== Internals (testing) ======

    Store& GetStore() noexcept { return store_; }
    PipelineType& GetPipeline() noexcept { return pipeline_; }

private:
    void Recycle(uint32_t block_id) noexcept {
        DataBlock* blk = store_.GetBlock(block_id);
        blk->SetState(BlockState::kFree);
        store_.AllocCount().fetch_sub(1, std::memory_order_relaxed);
        uint32_t head = store_.FreeHead().load(std::memory_order_acquire);
        do {
            blk->next_free = head;
        } while (!store_.FreeHead().compare_exchange_weak(
            head, block_id, std::memory_order_acq_rel, std::memory_order_acquire));
        store_.FreeCount().fetch_add(1, std::memory_order_relaxed);
    }

    Store store_;
    Notify notify_;
    PipelineType pipeline_;
    Config cfg_;
    BackpressureFn bp_fn_{nullptr};
    void* bp_ctx_{nullptr};
    FaultReporter fault_reporter_;
    uint16_t fault_slot_{0};
};
```

### 5.8 用户代码示例

**进程内 1:N:**

```cpp
using Dispatcher = DataDispatcher<InProcStore<16016, 32>, DirectNotify>;
static Dispatcher disp;  // 512KB, 不要放栈上

Dispatcher::Config cfg;
cfg.name = "lidar";
cfg.backpressure_threshold = 4;
disp.Init(cfg);

// Pipeline 配置
auto s0 = disp.AddStage({"log", LogHandler, &log_ctx, 100});
auto s1 = disp.AddStage({"fusion", FusionHandler, &fus_ctx, 200});
disp.SetEntryStage(s0);
disp.AddEdge(s0, s1);

// 生产 (API 与跨进程完全一致)
auto bid = disp.Alloc();
uint8_t* p = disp.GetWritable(bid.value());
FillFrame(p, frame_data, frame_size);
disp.Submit(bid.value(), frame_size);
// Pipeline 同步执行, 最后一个 stage Release 后自动回收
```

**跨进程 1:N (Producer):**

```cpp
using Dispatcher = DataDispatcher<ShmStore<16016, 32>, ShmNotify>;
static Dispatcher disp;

// Creator: CreateOrReplace + Init
auto shm = SharedMemorySegment::CreateOrReplace(
    "lidar_pool", ShmStore<16016, 32>::RequiredShmSize());
Dispatcher::Config cfg;
cfg.name = "lidar";
cfg.default_timeout_ms = 500;
disp.Init(cfg, shm.value().Data(), shm.value().Size());

// 注入通知回调 (futex wake all consumers)
disp.GetNotify().fn = [](uint32_t bid, uint32_t size, void* ctx) {
    auto* ch = static_cast<ShmSpmcByteChannel*>(ctx);
    ch->Write(&bid, sizeof(bid));  // 4B block_id 通知
};
disp.GetNotify().ctx = &notify_channel;

// 生产 (API 与进程内完全一致)
auto bid = disp.Alloc();
uint8_t* p = disp.GetWritable(bid.value());
FillFrame(p, frame_data, frame_size);
disp.Submit(bid.value(), frame_size);
```

**跨进程 1:N (Consumer):**

```cpp
using Dispatcher = DataDispatcher<ShmStore<16016, 32>, ShmNotify>;
static Dispatcher disp;

// Opener: Open + Attach
auto shm = SharedMemorySegment::Open("lidar_pool");
disp.Attach(shm.value().Data());

// 消费循环
while (running) {
    // 等待 block_id 通知 (通过独立的 ShmSpmcByteChannel)
    uint32_t bid = 0;
    auto wait = notify_channel.WaitReadable(100);
    if (!wait.has_value()) continue;
    notify_channel.Read(&bid, sizeof(bid));

    // 零拷贝读取 (与进程内 API 相同)
    const uint8_t* data = disp.GetReadable(bid);
    uint32_t len = disp.GetPayloadSize(bid);
    ProcessFrame(data, len);
    disp.Release(bid);
}
```

### 5.9 文件结构

```
include/osp/
  job_pool.hpp    -- DataBlock, BlockState, Pipeline, BackpressureFn, FaultReporter,
                     InProcStore, ShmStore, DirectNotify, ShmNotify,
                     DataDispatcher<Store, Notify>
                     (单文件, ~1000 行)
```

---

## 6. 方案 B: 两个独立具体类

### 6.1 设计思路

不引入 StorePolicy/NotifyPolicy 抽象。`InProcDispatcher` 和 `ShmDispatcher` 各自完整实现全部逻辑，共享 DataBlock 布局、Pipeline、BackpressureFn、FaultReporter 定义。

```
job_pool.hpp
  +-- DataBlock, BlockState, Pipeline, BackpressureFn, FaultReporter (共享定义)
  +-- InProcDispatcher<BS, MB>  -- 嵌入数组 + 完整 CAS + Pipeline 同步
  +-- ShmDispatcher<BS, MB>     -- shm 指针 + 完整 CAS (复制) + OnSubmit 回调
```

### 6.2 InProcDispatcher

```cpp
template <uint32_t BlockSize, uint32_t MaxBlocks,
          uint32_t MaxStages = OSP_JOB_MAX_STAGES,
          uint32_t MaxEdges = OSP_JOB_MAX_EDGES>
class InProcDispatcher {
public:
    // 统一命名: Alloc, GetWritable, Submit, GetReadable, Release
    // 实现逻辑与当前 DataDispatcher + JobPool 完全相同
    // Pipeline 在 Submit 内同步执行

private:
    alignas(OSP_JOB_BLOCK_ALIGN) uint8_t storage_[kBlockStride * MaxBlocks];
    alignas(kCacheLineSize) std::atomic<uint32_t> free_head_{0};
    alignas(kCacheLineSize) std::atomic<uint32_t> free_count_{MaxBlocks};
    alignas(kCacheLineSize) std::atomic<uint32_t> alloc_count_{0};
    Pipeline<MaxStages, MaxEdges> pipeline_;
    Config cfg_;
    BackpressureFn bp_fn_{nullptr};
    void* bp_ctx_{nullptr};
    FaultReporter fault_reporter_;
    uint16_t fault_slot_{0};
};
```

### 6.3 ShmDispatcher

```cpp
template <uint32_t BlockSize, uint32_t MaxBlocks,
          uint32_t MaxStages = OSP_JOB_MAX_STAGES,
          uint32_t MaxEdges = OSP_JOB_MAX_EDGES>
class ShmDispatcher {
public:
    struct ShmPoolHeader { /* 同方案 A 的 ShmStore::ShmPoolHeader */ };

    void Init(const Config& cfg, void* shm_base, uint32_t shm_size) noexcept;  // Creator
    void Attach(void* shm_base) noexcept;                                       // Opener

    // 统一命名: Alloc, GetWritable, Submit, GetReadable, Release
    // CAS 逻辑与 InProcDispatcher 完全相同, 但操作 header_-> 指针

    expected<uint32_t, JobPoolError> Alloc() noexcept {
        // CAS free_head 逻辑 -- 与 InProcDispatcher::Alloc() 逻辑一致
        // 但 free_head 来自 header_->free_head
        uint32_t head = header_->free_head.load(std::memory_order_acquire);
        while (head != detail::kJobInvalidIndex) {
            DataBlock* blk = GetBlock(head);
            uint32_t next = blk->next_free;
            if (header_->free_head.compare_exchange_weak(
                    head, next, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                header_->free_count.fetch_sub(1, std::memory_order_relaxed);
                header_->alloc_count.fetch_add(1, std::memory_order_relaxed);
                blk->SetState(BlockState::kAllocated);
                blk->alloc_time_us = SteadyNowUs();
                blk->payload_size = 0;
                blk->fault_id = 0;
                blk->deadline_us = 0;
                blk->refcount.store(0, std::memory_order_relaxed);
                return expected<uint32_t, JobPoolError>::success(head);
            }
        }
        return expected<uint32_t, JobPoolError>::error(JobPoolError::kPoolExhausted);
    }

    // Release, Recycle, ScanTimeout, ForceCleanup
    // 与 InProcDispatcher 完全相同, 唯一差异: GetBlock() 和 atomic 引用来源

    // Submit 通知: 通过 OnSubmit 回调
    using OnSubmitFn = void (*)(uint32_t block_id, uint32_t size, void* ctx);
    void SetOnSubmit(OnSubmitFn fn, void* ctx) noexcept;

private:
    DataBlock* GetBlock(uint32_t id) noexcept {
        return reinterpret_cast<DataBlock*>(
            blocks_base_ + static_cast<size_t>(id) * kBlockStride);
    }

    ShmPoolHeader* header_{nullptr};
    uint8_t* blocks_base_{nullptr};
    Pipeline<MaxStages, MaxEdges> pipeline_;
    Config cfg_;
    // ... BackpressureFn, FaultReporter, OnSubmitFn ...
};
```

### 6.4 用户代码示例

```cpp
// ---- InProc ----
InProcDispatcher<16016, 32> disp;
disp.Init(cfg);
auto bid = disp.Alloc();
uint8_t* p = disp.GetWritable(bid.value());
FillFrame(p, data, size);
disp.Submit(bid.value(), size);

// ---- InterProc (Producer) ----
ShmDispatcher<16016, 32> producer;
auto shm = SharedMemorySegment::CreateOrReplace("lidar_pool",
               ShmDispatcher<16016, 32>::RequiredShmSize());
producer.Init(cfg, shm.value().Data(), shm.value().Size());
producer.SetOnSubmit(PushBlockIdFutex, &notifier);
auto bid = producer.Alloc();
producer.GetWritable(bid.value());
// ...
producer.Submit(bid.value(), size);

// ---- InterProc (Consumer) ----
ShmDispatcher<16016, 32> consumer;
auto shm = SharedMemorySegment::Open("lidar_pool");
consumer.Attach(shm.value().Data());
const uint8_t* data = consumer.GetReadable(block_id);
consumer.Release(block_id);
```

### 6.5 文件结构

```
include/osp/
  job_pool.hpp    -- DataBlock, BlockState, Pipeline, BackpressureFn, FaultReporter,
                     InProcDispatcher<BS, MB> (~450 行),
                     ShmDispatcher<BS, MB>    (~500 行, CAS 逻辑重复)
                     (单文件, ~1150 行)
```

---

## 7. 对比决策矩阵

### 7.1 工程维度

| 维度 | 方案 A (双策略模板) | 方案 B (两个独立类) |
|------|-------------------|-------------------|
| **CAS 逻辑份数** | **1 份 (~120 行)** | 2 份 (~240 行, 重复) |
| **修 bug 改动点** | **1 处** | 2 处 (必须同步) |
| **总代码量** | ~1000 行 | ~1150 行 |
| **新增抽象** | StorePolicy (5 方法) + NotifyPolicy (1 方法) | 无 |
| **阅读复杂度** | 需理解双策略模板参数 | 每个类自包含, 直接读 |
| **编译错误清晰度** | C++17 无 concept, 错误可能较长 | 直接, 无模板嵌套 |
| **零运行时开销** | 是 (编译期分发) | 是 (无虚函数) |
| **API 一致性** | 完全一致 (同一份代码) | 需人工保持一致 |

### 7.2 风险分析

| 风险场景 | 方案 A | 方案 B |
|----------|--------|--------|
| 修复 CAS 线程安全 bug | 改 1 处, 两种 Store 自动受益 | 改 2 处, 漏改一处 = 生产事故 |
| InProc/InterProc 行为不一致 | 不可能 (同一份代码) | 可能 (代码 drift) |
| 新增 Store 类型 (如 RTOS) | 实现 5 个方法 | 复制整个类 |
| 代码审查 CAS 正确性 | 审 1 份 | 审 2 份, 确认一致 |

### 7.3 项目原则检验

> "不为无产品需求文档支撑的扩展性增加抽象层"

StorePolicy + NotifyPolicy 不是为未来扩展设计的。StorePolicy 是为 **消除 120 行危险 CAS 代码的重复** (已有 3 个真实 bug 的区域)。NotifyPolicy 是为 **分离存储和通知两个正交关注点** (zh3 的改进)。两者都是解决当前已确认的两种部署模式的最小必要抽象。

> "拆文件解决协作问题, 不拆层级"

两个方案都合并在 `job_pool.hpp` 单文件内，不拆层级。

### 7.4 推荐结论

**推荐方案 A (StorePolicy + NotifyPolicy 双策略模板)**。

核心理由: CAS 无锁逻辑 (~120 行) 是整个模块中 bug 密度最高的区域 (上次 review 修了 3 个线程安全 bug)。方案 A 只维护 1 份，修 bug 改 1 处，两种 Store 自动受益。方案 B 维护 2 份，每次修 bug 必须同步改 2 处，漏改一处就是生产事故。

StorePolicy (5 个方法) + NotifyPolicy (1 个方法) 的抽象代价极小，映射的是 InProc/InterProc 之间的 3 处物理差异，不是预防性设计。代码量方案 A (~930 行) 低于方案 B (~1130 行)。

---

## 8. 关键设计决策

### 8.1 Pipeline 语义: InterProc 模式 (NotifyStage 叶子节点)

**决策: Pipeline 结构不变，InterProc 通知作为 Pipeline 的叶子 Stage 自然嵌入。Producer 端可配置本地预处理 Stage，NotifyStage 作为 DAG 最后一个节点触发跨进程通知。**

这是混合模式: Pipeline 同时承载本地预处理和跨进程通知，无需特殊分支逻辑。

#### 实现

```cpp
// NotifyStage: handler 内部先 AddRef 再通知, 防止 Pipeline Release 导致提前回收
struct NotifyCtx {
    ShmNotify* notify;
    DataDispatcher* disp;
    uint32_t num_consumers;   // 远程 Consumer 数量
};

void NotifyHandler(const uint8_t* data, uint32_t len, void* ctx) {
    auto* nc = static_cast<NotifyCtx*>(ctx);
    // block_id 从 DataBlock header 反算
    uint32_t block_id = /* offset calculation */;
    // 1. 先为远程 Consumer 增加引用计数
    nc->disp->AddRef(block_id, nc->num_consumers);
    // 2. 再通知 (Consumer 收到后可安全读取)
    nc->notify->OnSubmit(block_id, len);
    // 3. handler 返回后, Pipeline Release 本 Stage 的引用
    //    refcount 不会降为 0 (还有 num_consumers 个引用)
}
```

#### refcount 时序 (关键)

```
Submit: refcount = 1 (pipeline entry fan_out, 线性 Pipeline)

Pipeline::Execute:
  Stage[crc]:
    handler 执行 -> AddRef(1) for successor -> Release own -> refcount 仍为 1
  Stage[compress]:
    handler 执行 -> AddRef(1) for successor -> Release own -> refcount 仍为 1
  Stage[notify] (叶子):
    handler 执行:
      AddRef(block_id, 3)  // 3 个远程 Consumer -> refcount = 1 + 3 = 4
      push block_id 给 3 个 Consumer
    handler 返回 -> Pipeline Release -> refcount = 4 - 1 = 3

Consumer[0] Release: refcount 3 -> 2
Consumer[1] Release: refcount 2 -> 1
Consumer[2] Release: refcount 1 -> 0 -> Recycle  (最后一个远程 Consumer 回收)
```

错误模式 (不加 AddRef 的后果):
```
Stage[notify] handler: push block_id (不加 AddRef)
Pipeline Release: refcount 1 -> 0 -> Recycle!  // block 已回收
Consumer[0] GetReadable(bid) -> 读到已回收的 block -> UB
```

#### InProc (DirectNotify): Pipeline 全部是业务 Stage

```
Submit() -> Pipeline::Execute()
  -> Stage[log] -> Stage[fusion] -> 最后一个 Release -> 回收
```

#### InterProc (ShmNotify): NotifyStage 作为叶子节点

```
Submit() -> Pipeline::Execute()
  -> Stage[crc] -> Stage[compress] -> Stage[notify] (叶子)
                                         |
                                         v
                                    ShmNotify::OnSubmit(block_id)
                                         |
                                         v
                                    Consumer 进程收到 block_id
                                    -> GetReadable() -> 本地处理 -> Release()
```

#### 纯 1:N 通知 (无预处理): Pipeline 只有 NotifyStage

```
Producer 配置:
  auto s0 = disp.AddStage({"notify", NotifyHandler, &shm_notify, 0});
  disp.SetEntryStage(s0);
  // 无其他 stage, 无 edge

Submit() -> Pipeline::Execute()
  -> Stage[notify] (唯一 stage, 直接触发通知)
```

#### 优势

- Pipeline 代码零修改: NotifyStage 就是一个普通 Stage, 不需要特殊 if/else 判断模式
- DAG 拓扑自然表达预处理 + 通知的先后关系
- Consumer 端可以独立配置自己的本地 Pipeline (如果需要)
- refcount 语义一致: Pipeline 内部 Stage 通过标准 AddRef/Release 管理引用, NotifyStage 额外 AddRef(num_consumers) 为远程 Consumer 预留引用

#### 约束

- NotifyStage handler 必须在通知前调 `AddRef(block_id, num_consumers)`, 否则 Pipeline Release 会导致提前回收
- `num_consumers` 必须在 NotifyStage 构建时确定 (从 ShmPoolHeader 的 consumer_count 读取, 或配置时写死)

### 8.2 ForceCleanup 责任归属 + per-consumer bitmap

**决策: Producer 进程负责执行 ScanTimeout 和 ForceCleanup。Launcher/Monitor 负责崩溃检测和触发。通过 per-consumer holding_mask 精确追踪每个 Consumer 持有的 block。**

#### 8.2.1 职责分工

```
Launcher/Monitor: 检测 Consumer 进程存活 (IsProcessAlive @ 1Hz)
                  检测到崩溃 -> 设置 consumer_slot.active = 0
Producer:         定期执行 ScanTimeout() (Timer @ 1Hz)
                  扫描发现 dead consumer -> 根据 holding_mask 精确回收
```

理由:
- Producer 拥有 DataDispatcher 实例 (Creator), 是数据块的 owner
- ScanTimeout/ForceCleanup 的 CAS 操作在 Producer 地址空间内完成最安全
- Launcher 不需要 mmap 同一块共享内存，只需检测进程存活并标记 consumer_slot
- Monitor 进程做观测告警 (FaultCollector 上报), 不做主回收执行者

#### 8.2.2 ConsumerSlot 结构 (shm 中)

```cpp
struct ConsumerSlot {
    std::atomic<uint32_t> active;         // 0=空闲, 1=活跃
    std::atomic<uint32_t> pid;            // Consumer 进程 PID
    std::atomic<uint64_t> heartbeat_us;   // 最后一次心跳时间戳
    std::atomic<uint64_t> holding_mask;   // 持有的 block bitmap (bit i = 持有 block i)
};
// 限制: uint64_t holding_mask 最多支持 64 blocks
// LiDAR 场景 (MaxBlocks=32) 足够; 如需更多可扩展为 holding_mask[N]
```

shm 布局扩展:
```
[ShmPoolHeader][DataBlock[0]..DataBlock[N-1]][ConsumerSlot[0]..ConsumerSlot[MaxConsumers-1]]
```

#### 8.2.3 Consumer 生命周期

```cpp
// 注册 (Attach 后)
int32_t cid = disp.RegisterConsumer(getpid());
// cid = consumer_slot 索引, -1 表示已满

// 每次获取 block
uint32_t bid = recv_block_id();
const uint8_t* data = disp.GetReadable(bid);
// holding_mask |= (1ULL << bid)  -- 原子 OR

// 每次释放 block
disp.Release(bid);
// holding_mask &= ~(1ULL << bid)  -- 原子 AND
// heartbeat_us = SteadyNowUs()

// 退出 (正常关闭)
disp.UnregisterConsumer(cid);
// active = 0, holding_mask = 0
```

#### 8.2.4 崩溃检测到回收的完整路径

```
1. Launcher: IsProcessAlive(consumer_pid) == false
2. Launcher: consumer_slots[cid].active.store(0)
   (或 Producer 自行检测: heartbeat_us 超时 + !IsProcessAlive(pid))
3. Producer: ScanTimeout() / ForceCleanup() 扫描 consumer_slots
   for each slot where active == 0 && holding_mask != 0:
     uint64_t mask = slot.holding_mask.exchange(0);  // 原子取走
     while (mask != 0) {
       uint32_t bid = __builtin_ctzll(mask);  // 最低位 block_id
       mask &= mask - 1;                      // 清除最低位
       // CAS 减少 refcount
       DataBlock* blk = store_.GetBlock(bid);
       uint32_t cur = blk->refcount.load(std::memory_order_acquire);
       while (cur > 0) {
         if (blk->refcount.compare_exchange_weak(
                 cur, cur - 1, std::memory_order_acq_rel,
                 std::memory_order_acquire)) {
           if (cur == 1) Recycle(bid);
           break;
         }
       }
     }
4. Producer: FaultCollector 上报 kConsumerCrash
```

#### 8.2.5 双重兜底

| 机制 | 作用 | 触发条件 |
|------|------|----------|
| holding_mask 精确回收 | 只释放崩溃 Consumer 持有的 block | consumer active=0 且 mask!=0 |
| deadline_us 超时回收 | 兜底: 即使 mask 不准也能回收 | block 超时未释放 |

两者互不干扰: holding_mask 负责精确回收, deadline_us 是最后的安全网。

### 8.3 Shm 初始化: Creator / Opener 双路径

**决策: 明确 Creator(CreateOrReplace + Init) / Opener(Open + Attach) 双路径，复用现有 SharedMemorySegment RAII。**

```cpp
// Creator (Producer 进程):
auto seg = SharedMemorySegment::CreateOrReplace("lidar_pool",
               ShmStore<16016, 32>::RequiredShmSize());
disp.Init(cfg, seg.value().Data(), seg.value().Size());
// seg 的生命周期由 Producer 管理 (RAII, 析构时 munmap)
// Producer 退出前调 seg.Unlink() 清除 shm 文件

// Opener (Consumer 进程):
auto seg = SharedMemorySegment::Open("lidar_pool");
disp.Attach(seg.value().Data());
// seg 的生命周期由 Consumer 管理 (RAII, 析构时 munmap, 不 Unlink)
```

设计要点:
- `SharedMemorySegment` 是已有的 RAII wrapper (shm_transport.hpp:115-296), 直接复用
- `CreateOrReplace` 处理前一次崩溃残留的 stale shm (shm_unlink + 重建)
- `ShmStore` 不持有 `SharedMemorySegment`，只接收 `void*` 裸指针 -- shm 生命周期由调用方管理
- 这样 `ShmStore` 保持 POD-like (无 RAII 成员), 可以放在共享内存中或作为模板参数

Attach 时的安全校验:
```cpp
void Attach(void* shm_base) noexcept {
    header_ = static_cast<ShmPoolHeader*>(shm_base);
    std::atomic_thread_fence(std::memory_order_acquire);
    OSP_ASSERT(header_->magic == OSP_JOB_POOL_MAGIC);       // magic 校验
    OSP_ASSERT(header_->version == 1);                        // 版本兼容
    OSP_ASSERT(header_->block_size == BlockSize);             // 模板参数一致
    OSP_ASSERT(header_->block_count == MaxBlocks);            // 模板参数一致
    blocks_base_ = static_cast<uint8_t*>(shm_base) + kShmHeaderSize;
}
```

---

## 9. 资源预算

以 LiDAR 场景 (BlockSize=16016, MaxBlocks=32) 为例:

### 9.1 内存占用

| 组件 | InProc | InterProc (shm) | 说明 |
|------|--------|-----------------|------|
| Pool Header | 内嵌 (~192B) | ShmPoolHeader (~256B) | 3 个 cache-line-aligned atomic |
| DataBlock[32] | 512KB | 512KB | 32 x (64B hdr + 16016B payload, aligned) |
| ConsumerSlot[8] | 0 | ~256B | 8 x (4+4+8+8=24B, aligned to 32B) |
| Pipeline (8 stages) | ~512B | ~512B | 两种模式相同 |
| 通知通道 | 0 | ~128B | 4B x 32 block_id (ShmSpmcByteChannel) |
| **总计** | ~513KB | ~513KB | 几乎无差异 |

### 9.2 编译产物

两个方案编译产物体积无差异: 编译期选择，只实例化所用的 Store/Notify 组合。

| 指标 | 方案 A | 方案 B |
|------|--------|--------|
| 头文件大小 | ~1000 行 | ~1150 行 |
| 模板实例化 | `DataDispatcher<InProcStore<..>, DirectNotify>` | `InProcDispatcher<..>` |

---

## 10. 实施计划

### 10.1 阶段划分

```
Phase 0: API 命名统一 (前导, 可独立提交)
  - Alloc/GetWritable/Submit/GetReadable/Release 重命名
  - 同步更新 test_job_pool.cpp + 相关 demo
  - ~100 行改动, 零逻辑变更, 最低风险

Phase 1: 核心重构 (P0)
  - 方案 A: 提取 InProcStore + DirectNotify, DataDispatcher 模板化
  - 方案 B: 将现有代码整理为 InProcDispatcher
  - 确保现有 979+ tests 全部通过
  - ASan + UBSan + TSan clean

Phase 2: ShmStore + ShmNotify (P0)
  - ShmStore: ShmPoolHeader + Init/Attach + GetBlock
  - ShmNotify: OnSubmit 回调封装
  - 复用 SharedMemorySegment (CreateOrReplace + Open)
  - 新增测试: 单进程模拟 Creator/Opener/Release

Phase 3: 跨进程 ForceCleanup (P1)
  - Consumer PID 追踪 (ShmPoolHeader 扩展)
  - ScanTimeout + ForceCleanup 集成 IsProcessAlive
  - 崩溃清理测试 (fork + kill + 验证 block 回收)

Phase 4: 集成验证 (P1)
  - 跨进程 demo (Producer + Consumer 独立进程)
  - 压力测试: N:1 多 producer 并发 + 1:N 多 consumer 并发
  - 更新 design_job_pool_zh.md 和 design_zh.md
```

### 10.2 代码量预估

| 组件 | 方案 A | 方案 B |
|------|--------|--------|
| Phase 0: API 重命名 | ~100 行 | ~100 行 |
| Phase 1: 核心重构 | ~150 行 (提取 Policy) | ~0 行 (保留原样) |
| Phase 2: ShmStore + ShmNotify | ~200 行 | ~500 行 (含重复 CAS) |
| Phase 3: ForceCleanup | ~80 行 | ~80 行 |
| Phase 4: 测试 + demo | ~400 行 | ~450 行 |
| **总计** | **~930 行** | **~1130 行** |

### 10.3 风险缓解

| 风险 | 缓解措施 |
|------|----------|
| StorePolicy concept 编译错误不友好 | DataDispatcher 开头 static_assert 检查 5 个方法存在性 |
| ShmStore 跨进程 atomic 正确性 | `static_assert(atomic<uint32_t>::is_always_lock_free)` + ARM 实测 |
| Consumer 崩溃后 refcount 泄漏 | deadline_us 超时兜底 + ForceCleanup(IsProcessAlive) |
| 方案 B CAS 代码 drift | (仅方案 B) `// SYNC_WITH: InProcDispatcher::Alloc` 标记 |
