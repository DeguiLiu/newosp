# newosp API 参考: 数据流处理层

本文档覆盖 newosp 项目的数据流处理相关模块的公共 API，包括通用数据分发器、共享内存环形缓冲和管道架构。

> **相关模块**: 进程内 SPSC 环形缓冲 (`spsc_ringbuffer.hpp`) 详见 [基础层文档](api_foundation_core_zh.md#8-spsc_ringbufferhpp---spsc-环形缓冲)。本文档重点覆盖共享内存 IPC 传输。

---

## 1. data_dispatcher.hpp - 通用数据分发器

**概述**: 工业级数据分发框架，支持多阶段管道、CAS lock-free 设计、可插拔存储策略（进程内/共享内存）和通知策略。

**头文件**: `include/osp/data_dispatcher.hpp`

**依赖**: `platform.hpp`, `vocabulary.hpp`, `fault_collector.hpp`

**特性**:
- Header-only, C++17
- 兼容 `-fno-exceptions -fno-rtti`
- CAS lock-free 数据块分配
- 静态 DAG 管道（最多 8 阶段，16 条边）

### 错误枚举

#### JobPoolError

| 值 | 说明 |
|---|---|
| `kPoolExhausted` | 内存池满（背压） |
| `kInvalidBlockId` | 数据块 ID 超出范围 |
| `kBlockNotReady` | 数据块未处于预期状态 |
| `kBlockTimeout` | 处理超时 |
| `kPipelineFull` | 阶段/边数超出限制 |
| `kNoEntryStage` | 未配置入口阶段 |
| `kInvalidStage` | 无效的阶段索引 |

### 数据块状态枚举

#### BlockState

| 值 | 说明 |
|---|---|
| `kFree` | 空闲（可分配） |
| `kAllocated` | 已分配（用户填充数据） |
| `kReady` | 就绪（等待分发） |
| `kProcessing` | 处理中 |
| `kDone` | 完成（可回收） |

### 编译期配置宏

| 宏名 | 默认值 | 说明 |
|------|--------|------|
| `OSP_JOB_POOL_MAGIC` | `0x4A4F4250` | 内存池幻数 |
| `OSP_JOB_BLOCK_ALIGN` | `64` | 数据块对齐字节数 |
| `OSP_JOB_MAX_STAGES` | `8` | 最大管道阶段数 |
| `OSP_JOB_MAX_EDGES` | `16` | 最大管道边数 |
| `OSP_JOB_MAX_CONSUMERS` | `8` | 单个阶段最大消费者数 |

### 模板类: DataDispatcher

```cpp
template <typename StorePolicy, typename NotifyPolicy,
          size_t MaxStages, size_t MaxEdges>
class DataDispatcher;
```

**模板参数**:
- `StorePolicy`: 存储后端（`detail::InProcStore` 或 `detail::ShmStore`）
- `NotifyPolicy`: 通知策略（`DirectNotify` 或 `ShmNotify`）
- `MaxStages`: 最大阶段数（≤ `OSP_JOB_MAX_STAGES`）
- `MaxEdges`: 最大边数（≤ `OSP_JOB_MAX_EDGES`）

**主要方法**:

| 方法签名 | 返回类型 | 说明 | 线程安全性 |
|---------|---------|------|-----------|
| `Init()` | `void` | 初始化分发器 | 单线程 |
| `AllocBlock()` | `expected<uint32_t, JobPoolError>` | 分配数据块 | CAS lock-free |
| `SubmitBlock(id)` | `expected<void, JobPoolError>` | 提交数据块进入管道 | CAS lock-free |
| `GetBlock(id)` | `void*` | 获取数据块指针 | CAS lock-free |
| `FreeBlock(id)` | `expected<void, JobPoolError>` | 释放数据块 | CAS lock-free |
| `SetupPipeline(...)` | `expected<void, JobPoolError>` | 配置多阶段管道 | 单线程 |
| `ProcessStage(stage_idx)` | `void` | 处理指定阶段的所有就绪块 | 多线程 |

### 存储策略

#### InProcStore

进程内存储，适合单进程场景。

```cpp
detail::InProcStore<BlockSize, MaxBlocks>
```

**配置宏**:
- `OSP_JOB_POOL_SIZE`: 内存池总大小（字节）

#### ShmStore

共享内存存储，适合多进程 IPC。

```cpp
detail::ShmStore<BlockSize, MaxBlocks>
```

**特性**:
- POSIX 共享内存 (`/dev/shm` on Linux)
- 大页支持（巨页）
- 跨进程访问

### 通知策略

#### DirectNotify

直接函数调用，适合单进程高频率场景。

#### ShmNotify

futex 系统调用，适合多进程低延迟通知。

---

## 2. shm_transport.hpp - 共享内存传输

**概述**: POSIX 共享内存 IPC 传输，支持 SPSC 字节环、SPMC 多消费者、futex 通知和大页支持。

**头文件**: `include/osp/shm_transport.hpp`

**依赖**: `platform.hpp`, `vocabulary.hpp`

**平台**: Linux 专用 (futex, `/dev/shm`)

**特性**:
- Header-only, C++17
- Lock-free SPSC/SPMC 字节环
- futex 系统调用低延迟通知
- 大页（HugePage）支持
- ARM 内存序加固（acquire/release fences）

### 编译期配置宏

| 宏名 | 默认值 | 说明 |
|------|--------|------|
| `OSP_SHM_SLOT_SIZE` | `4096` | 每个数据块大小（字节） |
| `OSP_SHM_SLOT_COUNT` | `256` | 最大数据块数量 |
| `OSP_SHM_CHANNEL_NAME_MAX` | `64` | 通道名称最大长度 |
| `OSP_SHM_BYTE_RING_CAPACITY` | `1MB` | 字节环缓冲容量 |

### 类: ShmSpscByteRing

SPSC（单生产者-单消费者）共享内存字节环，用于大消息传输（激光雷达点云、视频帧）。

```cpp
class ShmSpscByteRing;
```

**主要方法**:

| 方法签名 | 返回类型 | 说明 | 线程安全性 |
|---------|---------|------|-----------|
| `Create(name, capacity)` | `expected<ShmSpscByteRing, int>` | 创建或附加共享内存 | 单次 |
| `Unlink(name)` | `void` | 删除共享内存文件 | 后清理 |
| `Write(data, size)` | `size_t` | 写入数据 | SPSC 写入端 |
| `Read(buf, size)` | `size_t` | 读取数据 | SPSC 读取端 |
| `Available()` const | `size_t` | 可读字节数 | 读取端 |
| `Capacity()` const | `size_t` | 环形缓冲容量 | 常数 |
| `WaitNotification(timeout_ms)` | `bool` | 等待写入端通知 | futex 阻塞 |
| `NotifyReader()` | `void` | 唤醒读取端 | futex 唤醒 |

**字节级序列化示例**:

```cpp
auto ring_res = ShmSpscByteRing::Create("lidar_channel", 64*1024*1024);
if (!ring_res) return;
auto& ring = *ring_res;

// 写入端
LidarFrame frame{...};
auto buf = reinterpret_cast<const char*>(&frame);
ring.Write(buf, sizeof(frame));
ring.NotifyReader();

// 读取端
char buf[sizeof(LidarFrame)];
while (ring.Read(buf, sizeof(buf)) == 0) {
  ring.WaitNotification(1000);  // 1s timeout
}
LidarFrame* rx = reinterpret_cast<LidarFrame*>(buf);
```

### 类: ShmSpmcByteChannel

SPMC（单生产者-多消费者）字节通道，多个消费者可独立读取同一消息。

```cpp
class ShmSpmcByteChannel;
```

**主要方法**:

| 方法签名 | 返回类型 | 说明 | 线程安全性 |
|---------|---------|------|-----------|
| `Create(name, max_consumers)` | `expected<ShmSpmcByteChannel, int>` | 创建多消费者通道 | 单次 |
| `RegisterConsumer(consumer_id)` | `expected<void, int>` | 注册消费者 | 注册端 |
| `PublishBlock(data, size)` | `expected<void, int>` | 发布数据块 | 生产端 |
| `ReadBlock(consumer_id, buf, size)` | `size_t` | 消费者读取块 | SPMC 消费端 |
| `CommitRead(consumer_id)` | `void` | 消费者确认已读 | 消费端 |

**多播场景示例**:

```cpp
// 创建通道支持最多 4 个消费者
auto ch_res = ShmSpmcByteChannel::Create("video_stream", 4);
auto& channel = *ch_res;

// 消费者 0 和 1 各自注册
channel.RegisterConsumer(0);
channel.RegisterConsumer(1);

// 生产端发布视频帧
VideoFrame frame{...};
channel.PublishBlock(&frame, sizeof(frame));

// 消费者 0 读取
char buf0[sizeof(VideoFrame)];
channel.ReadBlock(0, buf0, sizeof(buf0));
channel.CommitRead(0);

// 消费者 1 独立读取同一消息
char buf1[sizeof(VideoFrame)];
channel.ReadBlock(1, buf1, sizeof(buf1));
channel.CommitRead(1);
```

### 类: ShmMemory

POSIX 共享内存 RAII 管理器。

```cpp
class ShmMemory;
```

**主要方法**:

| 方法签名 | 返回类型 | 说明 |
|---------|---------|------|
| `Create(name, size)` | `expected<ShmMemory, int>` | 创建或附加 |
| `Unlink(name)` | `void` | 删除 SHM 文件 |
| `Get()` const | `void*` | 获取内存指针 |
| `Size()` const | `size_t` | 大小 |
| `Resize(new_size)` | `expected<void, int>` | 调整大小 |

### futex 通知机制

所有通知基于 Linux futex 系统调用，支持：

- **低延迟唤醒**: 微秒级延迟
- **功耗优化**: 等待线程原子休眠
- **超时机制**: 防止无限等待
- **多任务友好**: 不占用 CPU 自旋

---

## 3. Pipeline - 管道构建辅助

**概述**: 静态管道 DAG 构建工具，用于多阶段数据处理链。

**包含于**: `data_dispatcher.hpp`

**特性**:
- 编译期 DAG 验证
- 最多 8 个处理阶段
- 最多 16 条数据流边

### 类: Pipeline

```cpp
template <size_t MaxStages, size_t MaxEdges>
class Pipeline;
```

**主要方法**:

| 方法签名 | 返回类型 | 说明 |
|---------|---------|------|
| `AddStage(name, handler)` | `expected<uint8_t, JobPoolError>` | 添加处理阶段 |
| `AddEdge(from_stage, to_stage)` | `expected<void, JobPoolError>` | 添加数据流边 |
| `Validate()` | `expected<void, JobPoolError>` | 验证 DAG 合法性 |
| `StageCount()` const | `size_t` | 返回阶段数 |

**三阶段典型拓扑**:

```
入口阶段 → 处理阶段 → 输出阶段
  (0)        (1)        (2)
```

对应代码：

```cpp
Pipeline<8, 16> pipeline;
auto stage0 = pipeline.AddStage("Ingress", IngressHandler);
auto stage1 = pipeline.AddStage("Process", ProcessHandler);
auto stage2 = pipeline.AddStage("Egress", EgressHandler);

pipeline.AddEdge(*stage0, *stage1);
pipeline.AddEdge(*stage1, *stage2);
pipeline.Validate();
```

---

## 线程安全性总结

| 模块 | 操作 | 线程安全 | 备注 |
|------|------|---------|------|
| DataDispatcher | AllocBlock/SubmitBlock/FreeBlock | CAS lock-free | 多线程安全 |
| DataDispatcher | SetupPipeline | 否 | 初始化时调用 |
| DataDispatcher | ProcessStage | 是 | 支持多线程处理 |
| SpscRingBuffer | Push/Pop | SPSC | 严格单生产单消费 |
| ShmSpscByteRing | Write/Read | SPSC | 严格单生产单消费 |
| ShmSpmcByteChannel | Publish/Read | SPMC | 支持 1:N 多播 |
| Pipeline | AddStage/AddEdge | 否 | 初始化阶段 |

---

## 性能指标

### 基准测试 (Intel Xeon, 8 cores)

| 操作 | 吞吐量 | 延迟 (p99) | 备注 |
|------|--------|-----------|------|
| DataDispatcher::AllocBlock | 12M ops/s | 50ns | CAS lock-free |
| SpscRingBuffer::Push | 150M ops/s | 10ns | 批量 1000 元素 |
| ShmSpscByteRing::Write | 2GB/s | 1µs | 4KB blocks |
| ShmSpmcByteChannel::Publish | 500M msgs/s | 500ns | 每消费者 |

---

## 编译示例

### 启用大页支持

```cmake
# 在 CMakeLists.txt 中
add_definitions(-DOSP_SHM_USE_HUGE_PAGES)
```

### 自定义配置

```cpp
#define OSP_SHM_BYTE_RING_CAPACITY (2 * 1024 * 1024)  // 2 MB
#define OSP_JOB_MAX_STAGES 16  // 最多 16 个阶段
#include "osp/data_dispatcher.hpp"
```

---

## 相关设计文档

- [数据分发器设计](../design_data_dispatcher_zh.md) - DataDispatcher 架构、设计决策、扩展性
- [Job Pool 和 Pipeline](../design_job_pool_zh.md) - 多阶段管道、状态机、示例应用
- [SHM IPC 性能](../benchmark_report_zh.md) - 共享内存 vs TCP vs UDS 基准对比

