# newosp 异步日志模块概要设计

## 1. 概述

### 1.1 背景

newosp 当前日志模块 (`log.hpp`) 使用同步 `fprintf(stderr, ...)` 输出日志。同步写入在以下场景存在问题:

- 工业传感器突发大量日志 (故障诊断、状态切换)
- `fprintf` I/O 系统调用阻塞调用线程 (~1-3us/条)
- 影响实时业务线程的执行时序

### 1.2 设计目标

| 目标 | 描述 |
|------|------|
| 分级路由 | ERROR/FATAL 同步写 (崩溃安全); DEBUG/INFO/WARN 异步写 (不阻塞) |
| 零竞争热路径 | 生产者 (业务线程) wait-free, ~200-300ns |
| 背压丢弃 + 主动上报 | 队列满时丢弃非关键日志, 定时向 stderr 上报丢弃统计 |
| 零侵入集成 | 包含 `async_log.hpp` 自动接管宏, 不使用则完全不影响 |
| 可替换 Sink | 函数指针 + context, 支持 stderr / 文件 / 自定义输出 |
| 编译约束兼容 | -fno-exceptions -fno-rtti, C++17, 零堆分配 |

### 1.3 非目标

- 日志文件轮转 (由外部 sink 实现)
- 结构化日志 (JSON/protobuf 序列化)
- 网络日志传输 (syslog/gRPC)

---

## 2. 架构设计

### 2.1 整体架构

```
                    +--- Per-Thread SPSC ---+
 Thread 0 ------->| RingBuffer<LogEntry,N> |--+
                    +----------------------+  |
                    +--- Per-Thread SPSC ---+  |     +----------------+
 Thread 1 ------->| RingBuffer<LogEntry,N> |--+---->| Writer Thread  |---> Sink
                    +----------------------+  |     | (round-robin   |    (stderr/
                    +--- Per-Thread SPSC ---+  |     |  poll + batch) |     file/
 Thread N ------->| RingBuffer<LogEntry,N> |--+     +----------------+     custom)
                    +----------------------+

 ERROR/FATAL ---------------------------------> fprintf(stderr) [sync, crash-safe]
```

### 2.2 方案选型

| 方案 | 生产者延迟 | 缓存行为 | 代码复用 | 内存 |
|------|-----------|---------|---------|------|
| A: MPSC (AsyncBus 式 CAS) | CAS 重试 ~50-100ns | 共享 producer_pos_ 跨核弹跳 | 需适配 AsyncBus | N * 320B |
| **B: Per-Thread SPSC** | **Wait-free ~10-20ns** | **每线程独立缓存行** | **复用 SpscRingbuffer** | **MaxThreads * N * 320B** |
| C: Thread-Local 累积 + 定期刷写 | 累积阶段零开销 | 好 | 复杂生命周期管理 | 不可控 |

**选择方案 B**: 热路径 wait-free (零竞争), 直接复用 `SpscRingbuffer<LogEntry, N>`, 嵌入式场景线程数固定 (2-8), 内存开销可接受且编译期可控。

### 2.3 分级路由策略

```
OSP_LOG_XXX(category, fmt, ...)
     |
     v  编译期级别过滤 (OSP_LOG_MIN_LEVEL)
AsyncLogWrite(level, ...)
     |
     +-- level >= ERROR?  ----yes----> LogWriteVa() [sync fprintf + fflush]
     |                                 崩溃安全: FATAL 写完后 abort()
     +-- async 未启动?   ----yes----> LogWriteVa() [sync fallback]
     |
     +-- AcquireLogBuffer()
     |     |
     |     +-- thread_local 快路径 (~1ns, 已注册)
     |     +-- CAS 首次注册 (仅一次)
     |     +-- 所有 slot 已满? -> sync_fallbacks++ -> LogWriteVa()
     |
     +-- vsnprintf(entry.message, 256, fmt, args)  [~100-200ns]
     |
     +-- buf->queue.Push(entry)  [wait-free SPSC, ~10-20ns]
           |
           +-- 队列满? -> entries_dropped++ [不阻塞, 计数上报]
```

---

## 3. 核心数据结构

### 3.1 LogEntry (320B)

```cpp
struct LogEntry {
  uint64_t timestamp_ns;    //  8B  CLOCK_MONOTONIC 单调时钟
  uint32_t wallclock_sec;   //  4B  挂钟秒 (time_t)
  uint16_t wallclock_ms;    //  2B  挂钟毫秒
  Level    level;           //  1B  日志级别枚举
  uint8_t  padding0;        //  1B  对齐填充
  char     category[16];    // 16B  日志分类 (null-terminated)
  char     message[256];    //256B  格式化消息 (vsnprintf)
  char     file[24];        // 24B  源文件名 (basename, 截断)
  uint32_t line;            //  4B  源代码行号
  uint32_t thread_id;       //  4B  系统线程 ID (cached)
};                          // 合计 320B = 5 cache lines
```

设计要点:
- `trivially_copyable`: SpscRingbuffer 使用 memcpy 批处理路径
- 320B 固定大小: 避免动态分配, 支持数组连续存储
- `message[256]`: 调用线程格式化, 避免 va_list 跨线程传递

### 3.2 LogBuffer (Per-Thread)

```cpp
struct LogBuffer {
  SpscRingbuffer<LogEntry, OSP_ASYNC_LOG_QUEUE_DEPTH> queue;
  std::atomic<bool> active{false};  // slot 占用标记
  uint32_t thread_id{0};            // 绑定的系统线程 ID
};
```

### 3.3 AsyncLogContext (全局单例)

```cpp
struct AsyncLogContext {
  LogBuffer buffers[OSP_ASYNC_LOG_MAX_THREADS];  // 静态 slot 数组
  std::atomic<bool> running{false};
  std::atomic<bool> shutdown{false};
  LogSinkFn sink{nullptr};
  void* sink_context{nullptr};
  std::thread writer_thread;

  // 统计计数器 (独立缓存行, 避免伪共享)
  alignas(64) std::atomic<uint64_t> entries_written{0};
  alignas(64) std::atomic<uint64_t> entries_dropped{0};
  alignas(64) std::atomic<uint64_t> sync_fallbacks{0};
};
```

---

## 4. 线程模型

### 4.1 线程注册

```
首次调用 AsyncLogWrite() 的线程:
  1. 检查 thread_local tls_cleanup.buf (快路径, ~1ns)
  2. 若为空, CAS 遍历 buffers[0..MAX_THREADS-1]:
     - compare_exchange_strong(false -> true)
     - 成功: 记录 thread_id, 设置 tls_cleanup.buf
     - 失败: 尝试下一个 slot
  3. 全部 slot 已占用: 返回 nullptr, 调用者 fallback 到同步写入

线程退出:
  ~TlsCleanup() -> buf->active.store(false, release)
  slot 自动释放, 可被后续线程复用
```

### 4.2 后台写线程 (WriterLoop)

```
WriterLoop:
  初始化: resolve sink (default StderrSink), 初始化 LogBackoff

  循环 (while !shutdown):
    total_popped = 0
    for each buffer slot:
      if !active && queue.IsEmpty(): skip
      n = queue.PopBatch(batch, 32)
      if n > 0:
        sink(batch, n, ctx)
        entries_written += n
        total_popped += n

    if total_popped > 0:
      backoff.Reset()
    else:
      backoff.Wait()  // spin -> yield -> sleep(50us)

    // 定时丢弃上报 (每 N 秒)
    if 到达上报时间点:
      delta_drops = entries_dropped - last_reported_drops
      if delta_drops > 0:
        fprintf(stderr, "[AsyncLog] WARN: %llu entries dropped ...")

  // Shutdown drain (最多 10 轮)
  for round in 0..9:
    drain all buffers -> sink
    if nothing drained: break

  // 最终丢弃上报
  if 有未报告的丢弃:
    fprintf(stderr, "[AsyncLog] WARN: %llu entries dropped since last report")
```

### 4.3 AdaptiveBackoff (三阶段退避)

```
LogBackoff:
  Phase 1 (spin_count < 6):   指数退避 CPU pause/yield, 1/2/4/8/16/32 次
  Phase 2 (spin_count < 10):  std::this_thread::yield() x 4
  Phase 3 (spin_count >= 10): std::this_thread::sleep_for(50us)
```

| 阶段 | 延迟 | 适用场景 |
|------|------|---------|
| Spin (pause/yield) | ~10-100ns | 高频日志突发, 写线程快速响应 |
| Yield | ~1us | 中等负载, 让出 CPU 时间片 |
| Sleep (50us) | 50us | 空闲期, 最小化 CPU 占用 |

---

## 5. 丢弃统计与上报机制

### 5.1 背压策略

队列满时, `Push()` 返回 false, 日志被丢弃:
- `entries_dropped` 原子计数器 +1
- 不阻塞调用线程 (wait-free 保证)
- 丢弃的是非关键日志 (ERROR/FATAL 走同步路径, 不受影响)

### 5.2 定时上报

写线程每 `OSP_ASYNC_LOG_DROP_REPORT_INTERVAL_S` 秒 (默认 10s) 检查一次:

```
[AsyncLog] WARN: 42 entries dropped in last 10s (total: written=10000 dropped=42 fallbacks=0)
```

- 直接写 stderr (同步, 崩溃安全)
- 仅在有新增丢弃时输出, 无丢弃不输出
- Shutdown 时上报剩余未报告的丢弃

### 5.3 Shell 查询接口

通过 `GetAsyncStats()` 暴露统计数据, 可集成 shell 诊断命令:

```cpp
auto stats = osp::log::GetAsyncStats();
// stats.entries_written   -- 已写入 sink 的总条数
// stats.entries_dropped   -- 因队列满丢弃的总条数
// stats.sync_fallbacks    -- 回退同步写入的总次数
```

Shell 命令示例 (通过 shell_commands.hpp 桥接):
```
newosp> osp_log_stats
AsyncLog: written=10342 dropped=17 fallbacks=0 enabled=true
```

---

## 6. 公共 API

### 6.1 生命周期 (自动管理)

异步日志的生命周期由框架自动管理, 用户无需手动调用:

- **自动启动**: 首次调用 `OSP_LOG_INFO/WARN/DEBUG` 时, `AsyncLogWrite()` 内部通过 CAS 原子自启动 writer thread
- **自动停止**: `atexit(StopAsync)` 确保进程退出前 drain 所有缓冲
- **强制同步模式**: 定义 `OSP_LOG_SYNC_ONLY` 宏后, 所有日志走同步路径, 不启动写线程

| 函数 | 签名 | 说明 |
|------|------|------|
| `StartAsync` | `void StartAsync(const AsyncLogConfig& = {}) noexcept` | 自动调用; 仅需自定义 sink 时手动调用 |
| `StopAsync` | `void StopAsync() noexcept` | 自动调用 (atexit); 通常不需手动调用 |
| `IsAsyncEnabled` | `bool IsAsyncEnabled() noexcept` | 查询运行状态 |
| `SetSink` | `void SetSink(LogSinkFn fn, void* ctx = nullptr) noexcept` | 设置输出 sink |
| `GetAsyncStats` | `AsyncLogStats GetAsyncStats() noexcept` | 获取运行时统计 |
| `ResetAsyncStats` | `void ResetAsyncStats() noexcept` | 重置统计计数器 |

### 6.2 Sink 函数签名

```cpp
using LogSinkFn = void (*)(const LogEntry* entries, uint32_t count, void* context);
```

- 函数指针 + context (非 virtual, 零开销)
- 批量接口: 一次传入多条 entry, 减少调用开销
- 默认: `StderrSink` (fprintf 到 stderr)

### 6.3 宏接口 (零侵入)

包含 `async_log.hpp` 后, 自动重定义以下宏:

| 宏 | 路由 |
|----|------|
| `OSP_LOG_DEBUG(cat, fmt, ...)` | AsyncLogWrite (异步 SPSC) |
| `OSP_LOG_INFO(cat, fmt, ...)` | AsyncLogWrite (异步 SPSC) |
| `OSP_LOG_WARN(cat, fmt, ...)` | AsyncLogWrite (异步 SPSC) |
| `OSP_LOG_ERROR(cat, fmt, ...)` | LogWrite (同步 fprintf, 不变) |
| `OSP_LOG_FATAL(cat, fmt, ...)` | LogWrite (同步 fprintf + abort, 不变) |

不包含 `async_log.hpp` 的编译单元完全不受影响。

---

## 7. 编译期配置

| 宏 | 默认值 | 说明 |
|----|--------|------|
| `OSP_ASYNC_LOG_QUEUE_DEPTH` | 256 | 每线程 SPSC 队列深度 |
| `OSP_ASYNC_LOG_MAX_THREADS` | 8 | 最大并发日志线程数 |
| `OSP_ASYNC_LOG_DROP_REPORT_INTERVAL_S` | 10 | 丢弃统计上报间隔 (秒, 0=禁用) |
| `OSP_LOG_MIN_LEVEL` | 0 (Debug) / 1 (Release) | 编译期最低日志级别 |
| `OSP_LOG_SYNC_ONLY` | 未定义 | 定义后禁用异步路径, 所有日志同步写 |

---

## 8. 资源预算

| 资源 | 数值 | 计算 |
|------|------|------|
| SPSC 缓冲 (per-thread) | 80KB | 256 * 320B = 81,920B |
| 总 SPSC 缓冲 | 640KB | 8 threads * 80KB |
| AsyncLogContext 管理开销 | ~2KB | 原子计数器 + 指针 + flags |
| 写线程栈上 batch 缓冲 | 10KB | 32 * 320B = 10,240B |
| 写线程 | +1 | 低优先级后台线程 |
| **最坏总内存** | **~652KB** | 编译期可控 |

### 8.1 延迟预算

| 路径 | 延迟 | 说明 |
|------|------|------|
| 编译期过滤 | 0ns | 宏展开为空 |
| 运行时级别过滤 | ~5ns | 一次原子 load + 比较 |
| thread_local 快路径 | ~1ns | 已注册线程 |
| vsnprintf 格式化 | ~100-200ns | 256B 缓冲, 调用线程执行 |
| SPSC Push | ~10-20ns | wait-free, memcpy 路径 |
| **异步热路径总计** | **~200-300ns** | vs 同步 ~1-3us |
| 同步路径 (ERROR/FATAL) | ~1-3us | fprintf + fflush, 崩溃安全 |

---

## 9. 依赖关系

```
async_log.hpp
  |-- log.hpp         (日志级别枚举, LogWrite, LogWriteVa, 宏定义)
  |-- platform.hpp    (kCacheLineSize, OSP_PLATFORM_LINUX 检测)
  |-- spsc_ringbuffer.hpp (SPSC 环形缓冲, wait-free, memcpy batch)
  |-- <thread>        (std::thread, writer_thread)
  |-- <atomic>        (统计计数器, slot 管理)
  |-- <chrono>        (AdaptiveBackoff, 定时上报)
  |-- <cinttypes>     (PRIu64 格式化宏)
```

不依赖:
- `worker_pool.hpp` (AdaptiveBackoff 内联拷贝, 避免引入 AsyncBus 链)
- `bus.hpp` (无消息总线依赖)
- `vocabulary.hpp` (无 FixedFunction/FixedString 依赖)

---

## 10. 文件清单

| 文件 | 类型 | 行数 | 说明 |
|------|------|------|------|
| `include/osp/async_log.hpp` | 新建 | ~680 | 异步日志核心实现 |
| `include/osp/log.hpp` | 修改 | +40 | 添加 `LogWriteVa()` va_list 变体 |
| `tests/test_async_log.cpp` | 新建 | ~340 | 10 个 Catch2 单元测试 |
| `tests/CMakeLists.txt` | 修改 | +1 | 添加测试文件 |
| `docs/design_async_log_zh.md` | 新建 | - | 本文档 |

---

## 11. 测试覆盖

10 个 Catch2 测试用例:

| 测试 | 覆盖场景 |
|------|---------|
| LogEntry is trivially copyable | static_assert POD 属性 |
| LogEntry size is 320 bytes | static_assert 布局大小 |
| basic async write and drain | INFO/WARN 异步写入, 验证 sink 接收 |
| ERROR bypasses async path | ERROR 走同步路径, 不进入 async sink |
| fallback to sync when not started | 未启动异步时自动回退同步写入 |
| drop policy on full queue | 队列满丢弃, entries_dropped > 0 |
| multi-thread concurrent logging | 4 线程 x 50 条并发, 零丢弃 |
| thread registration and deregistration | 线程退出释放 slot, 新线程复用 |
| custom sink receives correct data | 验证 entry 字段完整性 (level, category, message, file, line, tid, timestamp) |
| graceful shutdown drains all entries | 100 条突发后立即 Stop, 全部 drain |
| stats counters are accurate | written/dropped/fallbacks 计数准确, Reset 归零 |
| StartAsync is idempotent | 重复 Start/Stop 安全, 无崩溃 |

全部通过: ASan + UBSan + TSan, Debug + Release, -fno-exceptions 构建。

---

## 12. 使用示例

### 12.1 基本使用 (零配置)

```cpp
#include "osp/async_log.hpp"  // 自动接管 OSP_LOG_XXX 宏

int main() {
    // 无需手动调用 StartAsync/StopAsync
    // 首次 log 调用自动启动 writer thread, 进程退出自动 drain

    OSP_LOG_INFO("Main", "system started, version=%d", 1);
    OSP_LOG_WARN("Sensor", "temperature=%.1f exceeds threshold", 85.3);
    OSP_LOG_ERROR("Sensor", "hardware fault detected");  // 同步写, 崩溃安全

    return 0;  // atexit 自动调用 StopAsync(), drain 所有缓冲
}
```

### 12.2 强制同步模式

```cpp
// CMakeLists.txt 或编译参数中定义:
// target_compile_definitions(my_target PRIVATE OSP_LOG_SYNC_ONLY)
#define OSP_LOG_SYNC_ONLY
#include "osp/async_log.hpp"

// 所有 OSP_LOG_* 宏保持同步写入, 不启动后台线程
OSP_LOG_INFO("Main", "this goes to stderr synchronously");
```

### 12.3 自定义 Sink (高级用法)

```cpp
void FileSink(const osp::log::LogEntry* entries, uint32_t count, void* ctx) {
    FILE* fp = static_cast<FILE*>(ctx);
    for (uint32_t i = 0; i < count; ++i) {
        fprintf(fp, "[%s] %s\n",
                osp::log::detail::LevelTag(entries[i].level),
                entries[i].message);
    }
    fflush(fp);
}

int main() {
    FILE* logfile = fopen("/var/log/app.log", "a");
    osp::log::AsyncLogConfig cfg;
    cfg.sink = FileSink;
    cfg.sink_context = logfile;
    osp::log::StartAsync(cfg);  // 提前配置自定义 sink
    // 后续 log 调用使用 FileSink
    // ...
    // atexit 自动 StopAsync + drain
    // 注意: logfile 需在 atexit 之后仍有效, 或手动 StopAsync 后 fclose
}
```

### 12.3 统计查询

```cpp
auto stats = osp::log::GetAsyncStats();
printf("written=%lu dropped=%lu fallbacks=%lu\n",
       stats.entries_written, stats.entries_dropped, stats.sync_fallbacks);
```

---

## 13. 设计决策记录

### Q1: 为什么在调用线程执行 vsnprintf?

va_list 中的参数 (栈上临时变量、寄存器值) 在函数返回后失效, 不能跨线程传递。必须在调用线程完成格式化 (~100-200ns), 将结果存入 `message[256]` 后再入队。

### Q2: 为什么不复用 worker_pool.hpp 的 AdaptiveBackoff?

`worker_pool.hpp` 依赖 `bus.hpp` (AsyncBus), 引入会拉入整个消息总线链。日志模块作为基础设施应最小依赖, 因此内联拷贝了 ~30 行的 `LogBackoff` 类。

### Q3: 为什么选择丢弃而非阻塞?

工业嵌入式系统中, 业务线程的实时性优先于日志完整性。队列满时阻塞生产者会导致:
- 控制回路超时
- 看门狗触发复位
- 传感器数据丢失 (比丢日志更严重)

丢弃的日志通过定时上报机制 (`entries_dropped` 计数) 让运维感知。

### Q4: thread_local 生命周期如何管理?

`TlsCleanup` RAII 对象绑定到 `thread_local` 存储:
- 线程退出时, 析构函数自动将 `active` 置 false
- slot 释放后可被新线程 CAS 抢占复用
- 不依赖 `pthread_key_create` 等平台 API

### Q5: 写线程停止策略?

`StopAsync()` 流程:
1. `shutdown.store(true)` -- 通知写线程退出主循环
2. 写线程执行最多 10 轮 final drain, 确保缓冲中的 entry 全部写出
3. 写线程退出前输出最终丢弃统计
4. `writer_thread.join()` -- 主线程等待写线程结束
5. `running.store(false)` -- 标记已停止

### Q6: 为什么选择 auto-start + atexit 而非手动 Start/Stop?

日志是基础设施, 用户不应关心其后台线程的生命周期:
- **auto-start**: 首次 `AsyncLogWrite()` 调用时 CAS 原子自启动, 线程安全
- **atexit(StopAsync)**: 进程退出前自动 drain, 无需用户干预
- **OSP_LOG_SYNC_ONLY**: 编译期宏禁用异步路径, 用于调试或无后台线程的平台

### Q7: 为什么选 Per-Thread SPSC 而非 MPSC?

| 维度 | MPSC (CAS) | Per-Thread SPSC |
|------|-----------|-----------------|
| 生产者延迟 | CAS 重试 ~50-100ns | wait-free ~10-20ns |
| 缓存行为 | 共享 tail 跨核弹跳 | 每线程独立, 零 false sharing |
| 额外复杂度 | committed 标志 | 无 |
| 内存 | 1 * N * entry_size | MaxThreads * N * entry_size |
| 适用场景 | 线程数多/动态 | **线程数固定 (嵌入式 2-8)** |

嵌入式场景线程数少且固定, SPSC 的 wait-free 确定性延迟更适合实时系统。

---

## 14. 版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| v0.3.2 | 2026-02-16 | 初始实现: Per-Thread SPSC 异步日志 + 丢弃统计定时上报 |
| v0.3.2+ | 2026-02-16 | 增强: auto-start + atexit + OSP_LOG_SYNC_ONLY |
