# newosp 最小安全优化方案

**结论先行**：先做 P0-a（P0-1/P0-2/P0-3 + 编译期失败可见性），关闭“故障丢失、监督失效、配置静默”三条最高风险路径；P0-b（P0-4/P0-5）涉及 ABI/API 与迁移，必须单独提交。P1 做失败可见性和可分析性；P2 不要混进同一个提交。

**实施状态（2026-10-30）**：P0-1、P0-2、P0-3、P0-5 已完成；P0-4 已完成（布局头、attach 校验、owner 存活检查、force-replace 分离、裸 `AttachAt()` 删除）。P0-4 是 ABI 变更，必须单独版本发布。

## 1. 目标

把 newosp 从“按功能安全思路写的基础设施库”推进到“可分析、可验证的安全相关组件”。

最小优化只做三件事：

1. 让已经检测到的故障不丢、不静默。
2. 让配置失败、监督失效、共享边界不一致变成可见故障。
3. 让所有权和时间上界进入类型或明确的 API 约定。

约束不变：

- header-only，C++17。
- `-fno-exceptions -fno-rtti` 可编译。
- 热路径不引入堆分配。
- 容量继续编译期固定，不引入运行时扩容。
- 优先 additive API；必须破坏时给迁移说明。

## 2. 非目标

本方案不承担以下工作：

- 不完成 ASIL/SSIL 认证、HARA 或完整 SFMEA；但安全需求、SFMEA、追溯和安全手册必须作为并行工作流启动，不能作为 P2 延后。
- 不把 newosp 重构成 iceoryx 的 `Sample<T>` 全量所有权模型。
- 不引入通用 `std::function`、动态容器或异常。
- 不实现完整 AUTOSAR E2E；只做 CRC、计数器、超时三件套的最小组合。
- 不追求所有模块的 wait-free；先保证失败可见和时间上界可分析。

## 3. 优先级总览

| 级别 | 编号 | 改动 | 目的 | 工作量 |
| --- | --- | --- | --- | --- |
| P0 | P0-1 | critical 故障先锁存，再入队 | 故障不因队列满而完全消失 | S |
| P0 | P0-2 | Watchdog 启动可检查、恢复需连续健康 | 监督者本身可被监督 | S |
| P0 | P0-3 | 实时配置失败返回错误 | 不再静默降级调度属性 | S |
| P0 | P0-4 | 共享内存布局头与 attach 校验 | 防止错误布局和静默替换 | L（核心已实施） |
| P0 | P0-5 | DataDispatcher 块守卫与超时回收边界 | 防止 use-after-release | L（已实施） |
| P1 | P1-1 | `expected`/关键 API 增加 nodiscard | 失败路径不能被静默忽略 | S |
| P1 | P1-2 | 串口 ACK/健康/轮询预算收口 | 删除未实现的安全承诺 | M |
| P1 | P1-3 | AsyncBus 安全 profile 与分级丢弃统计 | 降低最坏路径复杂度 | S |
| P1 | P1-4 | Executor 支持注入 bus | 修复单例 bus 与多实例矛盾 | S |
| P1 | P1-5 | Lifecycle 并发保护与恢复守卫 | 状态转换不靠调用者串行化 | M |
| P1 | P1-6 | Pipeline handler 返回失败 | 处理失败可进入故障路径 | M |
| P2 | P2-1 | 网络/共享内存 E2E CRC | 端到端内容完整性 | L |
| P2 | P2-2 | 64 位 ABA 计数或速率上界 | 消除 16 位 tag 回绕假设 | M |
| P2 | P2-3 | FaultCollector 去 mutex / 去逐条唤醒 | 缩短故障上报路径 | M |
| P2 | P2-4 | 覆盖率、故障注入、SFMEA、安全手册 | 形成证据链 | L |

### 3.1 改动落点

| 编号 | 文件 | 主要位置 |
| --- | --- | --- |
| P0-1 | `include/osp/fault_collector.hpp` | `ReportFault()`、`SetFaultActive()`、`ClearAllFaults()` |
| P0-2 | `include/osp/watchdog.hpp` | `StartAutoCheck()`、`Check()`、`Slot` |
| P0-3 | `include/osp/executor.hpp` | `RealtimeConfig`、`RealtimeExecutor::Start()`、`ApplyRealtimeConfig()` |
| P0-4 | `include/osp/shm_transport.hpp` | `SharedMemorySegment`、`ShmRingBuffer::InitAt/AttachChecked`、`ShmChannel` |
| P0-5 | `include/osp/data_dispatcher.hpp` | `Alloc()`、`Submit()`、`ScanTimeout()`、`StageConfig` |
| P1-1 | `include/osp/platform.hpp`、`include/osp/vocabulary.hpp` | `OSP_NODISCARD`、`expected`、`optional` |
| P1-2 | `include/osp/serial_transport.hpp` | `ReliabilityConfig`、`HandleAckFrame()`、`Poll()`、健康统计 |
| P1-3 | `include/osp/bus.hpp`、`include/osp/opt.hpp`、`CMakeLists.txt` | `BusStatistics`、`OSP_SAFETY_PROFILE`、eviction 扫描 |
| P1-4 | `include/osp/executor.hpp` | 四个 executor 的 `bus_` 成员和构造函数 |
| P1-5 | `include/osp/lifecycle_node.hpp` | `Trigger()`、`Recover()`、`LifecycleError` |
| P1-6 | `include/osp/data_dispatcher.hpp` | `JobHandlerEx`、`StageConfig`、`ExecuteStage()` |

## 4. P0：先闭环

### P0-1 FaultCollector：critical 故障先锁存

现状：

- `FaultPriority::kCritical` 注释写 `never dropped`，文档写“永不丢弃”。
- `ReportFault()` 先做优先级准入，再 `TryPush()`，最后才 `SetFaultActive()`。
- 队列物理满时 `TryPush()` 失败，critical 故障被拒绝，且不会进入 active 位图。

改动：

- critical 故障在准入之前调用 `SetFaultActive(fault_index)`。
- 任何准入失败或入队失败都置位一个锁存标志 `fault_overflow_`，并提供只读查询与清除接口。
- `ClearAllFaults()` 同时清除 `fault_overflow_`。
- 把 `kCritical` 的注释改为“优先准入；仍受物理容量约束；溢出会锁存”。

代码示意：

```cpp
// fault_collector.hpp
expected<void, FaultCollectorError> ReportFault(uint16_t fault_index, uint32_t detail,
                                                FaultPriority priority) noexcept {
  const bool critical = (priority == FaultPriority::kCritical);
  if (critical) {
    SetFaultActive(fault_index);  // latch first: fault state must survive queue loss
  }

  // Admission failure path also stores fault_overflow_ before returning.
  // ... admission and TryPush as before ...

  if (OSP_UNLIKELY(!queue.TryPush(entry))) {
    stats_dropped_[pri].fetch_add(1U, std::memory_order_relaxed);
    stats_total_dropped_.fetch_add(1U, std::memory_order_relaxed);
    fault_overflow_.store(true, std::memory_order_release);
    if (overflow_cb_) {
      overflow_cb_(fault_index, priority);
    }
    breaker_.OnOverflow();
    return expected<void, FaultCollectorError>::error(FaultCollectorError::kQueueFull);
  }
  // ...
}
```

新增成员与接口：

```cpp
std::atomic<bool> fault_overflow_{false};
bool IsFaultOverflow() const noexcept;
void ClearFaultOverflow() noexcept;
uint32_t ConsumerCycles() const noexcept;
```

已实现说明：`ConsumerCycles()` 只在消费者循环完成一整轮后递增；它用于区分
“消费者线程存在”与“消费者仍在推进”。`Start(ThreadHeartbeat*)` 提供一步完成
heartbeat 绑定的入口，`IsConsumerMonitored()` 用于启动期检查。

验收：

- `FaultCollector<16, 8>`，消费者不启动，上报 10 个不同 `fault_index` 的 critical 故障。
- `ActiveFaultCount() == 10`，`IsFaultOverflow() == true`。
- 队列仍只接受 8 条，`total_dropped >= 2`。
- `ClearAllFaults()` 后 `ActiveFaultCount() == 0`，`IsFaultOverflow() == false`。

风险：

- 故障位会保持到显式 `ClearFault()`/`ClearAllFaults()`；这是 fail-safe 行为，需要在文档中写明。
- 被拒绝的 critical 故障不会执行 hook；应用必须通过 active 位图或溢出标志发现它。

### P0-2 Watchdog：启动可检查，恢复需连续健康

现状：

- `StartAutoCheck()` 返回 `void`，线程启动失败只复位内部标志。
- 没有 `IsAutoCheckRunning()`，调用者无法确认看门狗是否真的在跑。
- 一次心跳就会清除 `timed_out` 并触发 `on_recovered`。
- auto-check 线程不注册自己，检查回调在检查线程执行且没有时间约束。

改动：

- `StartAutoCheck()` 返回 `expected<void, WatchdogError>`，新增 `kThreadStartFailed`。
- 新增 `bool IsAutoCheckRunning() const noexcept`。
- 每个 slot 增加 `uint8_t healthy_beats`；连续 `kRecoverBeats` 次检查都看到新心跳后，才清除 `timed_out` 并调用 `on_recovered`。
- `Check()` 的 timeout/recover 回调仍只做短动作；文档明确“回调不得阻塞，故障处置应投递到 FaultCollector”。

代码示意：

```cpp
expected<void, WatchdogError> StartAutoCheck(uint32_t interval_ms) noexcept;
bool IsAutoCheckRunning() const noexcept;
uint32_t CheckCycles() const noexcept;

static constexpr uint8_t kRecoverBeats = 3U;
```

已实现说明：`CheckCycles()` 只在 `Check()` 完整返回后递增；如果检查回调阻塞，
`IsAutoCheckRunning()` 仍为 true 但 `CheckCycles()` 停止推进，外部可据此区分
“线程存在”和“检查在推进”。`Register(name, timeout_ms, recover_beats)` 支持
per-slot 恢复窗口，默认 `kRecoverBeats`。

验收：

- 线程创建失败时返回 `kThreadStartFailed`，`IsAutoCheckRunning() == false`。
- 强制某个 slot 超时后，一次心跳不能让 `IsTimedOut()` 变 false。
- 连续 3 次健康检查后恢复，`on_recovered` 只调用一次；如果检查间隔大于
  timeout/3，单次心跳会在恢复窗口内再次超时，这是预期行为。

风险：

- `TimedOutCount()` 会比原来保持更久；这是有意的恢复滞回。
- 检查回调仍然运行在检查线程；需要应用保证回调不阻塞。

### P0-3 RealtimeExecutor：配置失败返回错误

现状：

- `RealtimeExecutor::Start()` 返回 `void`。
- `mlockall`、CPU 绑定、`pthread_setschedparam` 失败只写 stderr。
- 调用者无法知道任务是否真的以 `SCHED_FIFO` 运行。

改动：

- 新增 `ExecutorError` 和 `StartChecked()`：

```cpp
enum class ExecutorError : uint8_t {
  kNone = 0,
  kAlreadyRunning,
  kThreadStartFailed,
  kMlockFailed,
  kAffinityInvalid,
  kAffinityFailed,
  kSchedFailed,
  kConfigTimeout,
  kUnsupported,
};

expected<void, ExecutorError> StartChecked() noexcept;
ExecutorError GetLastError() const noexcept;
```

已实现说明：`StartChecked()` 在 `require_realtime == true` 时等待调度线程完成
配置并返回首个失败原因；`Start()` 仍保留兼容签名。`GetLastError()` 在
`require_realtime == false` 时也可用于观察“尽力配置”的实际结果。

- `RealtimeConfig` 增加 `bool require_realtime = false`。
- `require_realtime == true` 时，任一配置失败都不启动调度线程，返回对应错误。
- `require_realtime == false` 时保留现有“尽力配置”行为，但记录 `last_error_`。
- `cpu_affinity >= CPU_SETSIZE` 在配置前直接返回 `kAffinityInvalid`，不再静默忽略。

验收：

- 非法 `cpu_affinity` 返回 `kAffinityInvalid`。
- 无权限设置 `SCHED_FIFO` 且 `require_realtime == true` 时返回 `kSchedFailed`，`IsRunning() == false`。
- `require_realtime == false` 时仍可启动，但 `GetLastError()` 能反映失败原因。

风险：

- 需要区分“配置失败但可用”和“配置失败必须停机”的项目策略；用 `require_realtime` 表达。
- 现有调用点忽略返回值仍可编译；建议在安全 profile 中要求检查 `StartChecked()`。

### P0-4 共享内存：布局头与 attach 校验

现状：

- `CreateOrReplace()` 无条件 `shm_unlink` 再 `Create`，可能替换仍在运行的写者。
- `AttachAt()` 直接 `static_cast`，没有 magic、version、大小或布局检查。
- 没有 epoch，旧读者会继续读旧段，新读者读新段。

改动：

- 在每个共享内存段起始位置放一个固定布局头：

```cpp
struct ShmLayoutHeader {
  uint32_t magic;        // 0x4F53484D ("OSHM")
  uint16_t version;      // layout version
  uint16_t header_size;
  uint32_t kind;         // slot ring / SPSC byte ring / SPMC byte ring
  uint32_t slot_size;
  uint32_t slot_count;
  uint32_t total_size;
  uint32_t layout_hash;
  uint64_t epoch;
  int32_t owner_pid;
  uint32_t flags;
  uint8_t reserved[16];
};
```

- `InitAt()` 写入头；`ValidateLayout()` 校验 magic、version、`SlotSize`、`SlotCount`、`total_size`。
- `OpenReader()` 在 attach 前调用 `ValidateLayout()`，失败返回 `ShmError::kLayoutMismatch`。
- 复用现有 `Create()`（已使用 `O_CREAT | O_EXCL`）作为安全路径，不再新增语义重复的 `CreateIfAbsent()`。
- `CreateOrReplace()`/`CreateOrReplaceWriter()` 先检查旧 owner 是否存活，存活则返回 `kOwnerAlive`；新增 `ForceReplace()`/`ForceReplaceWriter()` 作为显式强制替换路径。
- 新增 `uint64_t Epoch() const noexcept`；恢复流程用它判断是否发生了段替换。
- `layout_hash` 必须定义为固定的 offsetof/sizeof/endianness 算法，不能只 hash 结构体大小。
- `owner_pid` 使用 Linux `pidfd_open`（不可用时回退 `kill(pid, 0)`）做存活确认；PID 复用/namespace 的残余风险仍需在板级说明。
- 删除公开裸 `AttachAt()`；唯一 attach 路径是 `AttachChecked()`，channel 的 `OpenReader()` 负责调用。

已实现说明：`ShmLayoutHeader` 为 64 字节，包含 magic/version/kind/
slot_size/slot_count/total_size/layout_hash/epoch/owner_pid。`ShmRingBuffer`、
`ShmSpscByteRing`、`ShmSpmcByteRing` 都在段首写入该头；channel 的 `OpenReader()`
走 `AttachChecked()`，布局不一致返回 `kLayoutMismatch`。`SharedMemorySegment::Create()`
在已有同名段时返回 `kAlreadyExists`；`CreateOrReplace()` 拒绝活 owner（`kOwnerAlive`），
`ForceReplace()` 是显式强制路径。`ShmChannel`/`ShmByteChannel`/`ShmSpmcByteChannel`
均提供 `ForceReplaceWriter()`；段对象和 channel 都暴露 `Epoch()`。
裸 `AttachAt()` 已删除；没有校验的 attach 路径不再存在。

验收：

- 第二个写者使用 `Create()` 创建同名段时返回 `kAlreadyExists`。
- 活 owner 存在时 `CreateOrReplace()` 返回 `kOwnerAlive`；`ForceReplace()` 才覆盖。
- 用错误 `SlotCount` 的 reader attach 时返回 `kLayoutMismatch`。
- `CreateOrReplace` 重建后 epoch 改变；旧 reader 仍映射旧段，应用可据此发现分裂。

风险：

- 段布局变化，需要提升 ABI/version，所有使用方重新编译。
- 旧 reader 不会自动迁移；epoch 只提供检测，不提供迁移。

### P0-5 DataDispatcher：块守卫与超时回收边界

现状：

- `Alloc()` 返回裸 `uint32_t`，`GetWritable()`/`Release()` 是裸接口。
- `Alloc()` 结果可以忽略，块会泄漏到超时扫描。
- `ScanTimeout()` 会把 `kProcessing` 的块引用计数清零并回收；handler 仍在运行时可能被复用。
- `StageConfig::timeout_ms` 从未使用。

改动：

- 新增 move-only `BlockGuard`：

```cpp
class BlockGuard {
 public:
  ~BlockGuard() noexcept { Reset(); }
  BlockGuard(BlockGuard&& other) noexcept;
  BlockGuard& operator=(BlockGuard&& other) noexcept;
  BlockGuard(const BlockGuard&) = delete;

  uint8_t* Writable() noexcept;
  const uint8_t* Readable() const noexcept;
  uint32_t Id() const noexcept;
  uint32_t Generation() const noexcept;
  void Disarm() noexcept;

 private:
  void Reset() noexcept;
  DataDispatcher* dispatcher_{nullptr};
  uint32_t block_id_{0U};
  uint32_t generation_{0U};
};
```

- 新增 `expected<BlockGuard, JobPoolError> AllocGuard() noexcept`。
- 新增 `expected<void, JobPoolError> Submit(BlockGuard&& guard, uint32_t payload_size) noexcept`：成功才 `Disarm()`；失败时 guard 保持所有权并在析构时归还。
- `ScanTimeout()` 只回收 `kReady`；`kProcessing` 只标记 `kTimeout`，不重置引用计数、不回收，等待 handler `Release()` 或显式 `ForceCleanup()`。
- `CleanupDeadConsumers()` 对 `kProcessing` 块也只标记、不直接回收；heartbeat 过期不等于进程死亡（`SIGSTOP`/调度饥饿仍可能恢复）。
- 删除未使用的 `StageConfig::timeout_ms`，或改为明确注释“保留，未实现”。

已实现说明：`BlockGuard` 通过 `Abandon()` 归还“已分配但未 Submit”的块，
通过 generation-checked `Release()` 归还已提交的块；`Recycle()` 现在对同一块
幂等，避免 Release/ScanTimeout/ForceCleanup 竞争导致重复入空闲链表。

验收：

- `BlockGuard` 离开作用域自动归还块，`FreeBlocks()` 恢复。
- `kProcessing` 块超时后 `ScanTimeout()` 不回收；`ForceCleanup()` 才回收。
- heartbeat 过期但消费者被 `SIGSTOP` 时，`CleanupDeadConsumers()` 不误回收。
- `ForceCleanup()` 覆盖 `kTimeout` 块，作为确认 owner 已死亡后的显式出口。
- 过期 generation 的 `Release()` 返回 false，不减少新持有者的引用计数。

风险：

- `kProcessing` 泄漏会变成显式故障，而不是静默 use-after-free；需要监控 `FreeBlocks()` 和超时计数。
- 现有裸 `Alloc()`/`Release()` 保留，但文档标记为 advanced/unsafe，逐步迁移到 guard。

## 5. P1：让失败可见、可分析

### P1-1 `expected` 与关键 API 增加 nodiscard

改动：

- 在 `platform.hpp` 新增 `OSP_NODISCARD` 宏；`OSP_STRICT_NODISCARD` 开启时展开为 `[[nodiscard]]`。
- 先用于 `expected`、`optional` 和关键 API，而不是一次性改全部返回值。
- `Publish()`、`Alloc()`、`ReportFault()`、`Start()`、`StartAutoCheck()`、`Register()` 加 `OSP_NODISCARD`。
- 增加 `OSP_STRICT_NODISCARD` 编译选项；安全 profile 默认开启，普通构建先观察。
- `expected::value()`/`get_error()` 在状态错误时使用显式 `if (!has_value_) { OSP_ASSERT(false); std::abort(); }`，不能只写 `OSP_ASSERT`（`NDEBUG` 下会被编译掉）。

验收：

- 在 `-Werror=unused-result` 下，忽略 `ReportFault()` 或 `Alloc()` 返回值会编译失败。
- 错误状态调用 `value()` 在 Debug 和 Release 都终止，而不是继续执行。

风险：

- 会暴露大量现有调用点；先开 `OSP_STRICT_NODISCARD`，分批修复，再默认开启。

### P1-2 串口：删除未实现的 ACK 承诺

现状：

- 配置有 `enable_ack`、`ack_timeout_ms`、`max_retries`。
- `HandleAckFrame()` 是空实现；`retransmits`/`ack_timeouts` 从不递增。
- 序号缺口只统计，帧仍然交付。

改动：

- 首选：删除 `enable_ack`、`ack_timeout_ms`、`max_retries`、`retransmits`、`ack_timeouts`，或标记 `[[deprecated]]` 并在 README 写明“仅接收 ACK 帧，不做重传”。
- 在 `serial_transport.hpp` 内定义 `SerialFaultReportFn`（priority 用 `uint8_t`），增加 `SetFaultReporter(SerialFaultReportFn fn, void* ctx)`。这样 Foundation 层不直接依赖 `fault_collector.hpp`。在 CRC 错误、健康状态从 `kHealthy` 变为 `kDegraded`/`kFailed` 时上报一次，避免每字节上报。
- `IncrementErrorCount()` 使用 `cfg_.error_rate_window`，不再硬编码 10 秒。
- 打开端口时用当前时间初始化 `last_tx_time_ms_`/`last_rx_time_ms_`，避免启动即 `kFailed`，或在文档写明启动语义。
- `Poll()` 增加 `max_bytes` 预算参数或 `rx_budget_bytes` 配置，达到预算立即返回。

验收：

- 配置中不再存在“启用重传”的假承诺。
- CRC 错误后 `FaultCollector` 能收到一条故障，而不是只有统计计数。
- PTY 持续灌数据时，`Poll()` 在预算字节后返回。

风险：

- 删除字段是 API 破坏；先 deprecated，再移除。

### P1-3 AsyncBus：安全 profile 与分级丢弃统计

改动：

- 增加 CMake 选项 `OSP_SAFETY_PROFILE=ON`，至少包含：

```text
OSP_BUS_EVICTION=0
OSP_STRICT_NODISCARD=1
```

- 安全 profile 下高优先级发布不做 O(N) 淘汰扫描；队列满直接返回 false，由调用者上报故障。
- 增加按优先级统计的 `messages_dropped_by_priority`，便于 SFMEA 对应到具体通道。
- 如果保留 eviction，增加 `OSP_BUS_EVICT_SCAN_MAX`（默认 64），超过上限放弃淘汰并计数。

验收：

- 安全 profile 下队列满时 `Publish()` 返回 false，不发生长扫描。
- 统计能区分低/中/高优先级的丢弃数量。

风险：

- 关闭 eviction 会改变高优先级消息的准入行为；这是用“简单、可分析”换“尽力保高优先级”。

### P1-4 Executor 支持注入 bus

改动：

- 四个 executor 增加 `BusType* bus_`，默认 `&BusType::Instance()`。
- 增加构造函数重载接收 `BusType&`。
- `SpinOnce()`/`DispatchLoop()` 使用 `bus_->ProcessBatch()`。

验收：

- 两个 `AsyncBus` 实例、两个 `SingleThreadExecutor`，各自只消费自己的消息。
- 现有默认构造行为不变。

风险：

- `Node` 必须绑定到同一个 bus；文档要写明生命周期和绑定关系。

### P1-5 Lifecycle：并发保护与恢复守卫

改动：

- 增加 `LifecycleError::kBusy` 和 `LifecycleError::kRecoverConditionFailed`。
- `Trigger()` 用 `std::atomic_flag transition_busy_` 做非阻塞互斥；并发触发返回 `kBusy`，不持锁执行用户回调。
- 增加 `SetRecoverGuard(bool (*)(void*), void*)`；`Recover()` 在状态检查后调用守卫，失败则留在 `Recoverable` 并返回 `kRecoverConditionFailed`。

验收：

- 两个线程同时 `Activate()`，一个成功，一个返回 `kBusy`。
- 恢复守卫返回 false 时状态不变，返回错误。

风险：

- `kBusy` 是新错误码；调用者需要处理。

### P1-6 Pipeline handler 返回失败

改动：

- 保留现有 `JobHandler`，新增 `JobHandlerEx`：`bool (*)(const uint8_t*, uint32_t, uint32_t, void*)`。
- `StageConfig` 增加 `handler_ex`；两者只能设置一个。
- `ExecuteStage()` 在 `handler_ex` 返回 false 时：`SetState(kError)`、上报 fault、释放当前 stage 的引用、停止后继 stage，不再执行正常 fan-out。

验收：

- handler 返回 false 后，块进入 `kError` 并被回收，后继 stage 不执行。
- `FaultReporter` 收到对应故障。

风险：

- 新增函数指针字段会增加 `StageConfig` 大小；仍为固定布局，无堆。

### P1-7 QoS 与文档口径

- `QosProfile::deadline_ms`/`lifespan_ms` 目前只是声明；在实现前，文档和 shell 输出必须标注“advisory only”。
- 或者先在 `Node` 层增加 `Subscribe(max_age_us, ...)`，在回调前检查 `header.timestamp_us`；不要假装总线已经执行 QoS。
- README 修正以下过度声明：

```text
connection.hpp 的 auto-reconnect/heartbeat
serial_transport.hpp 的 IEC 61508
Zero global state
kCritical 永不丢弃
Tests 1295 / 500 / 1442 三个口径
```

## 6. P2：暂缓，不要混进最小改动

| 编号 | 内容 | 暂缓原因 |
| --- | --- | --- |
| P2-1 | 网络与共享内存 E2E CRC | 需要先完成布局头、版本和 wire format 设计 |
| P2-2 | 64 位 ABA 计数 | 32 位 MCU 上不一定 lock-free，需要平台分析 |
| P2-3 | FaultCollector 去 mutex、去逐条 `sem_post` | 需要 lock-free recent ring 或 seqlock |
| P2-4 | 覆盖率、故障注入、SFMEA、安全手册 | 是独立的安全流程工作，不是库内小改 |

注意：如果事件/共享内存承载安全相关数据，P2-1 不能停留在 P2；P2-4 是安全论证
的前置工作流，不是“代码改完再说”的收尾项。代码加固可以与这些工作并行，但在
它们完成前不能宣称任何 ASIL/SSIL 结论。

## 7. 实施顺序

1. 文档修正与安全假设（零代码风险）。
2. P0-a：P0-1、P0-2、P0-3（不改 ABI 的故障路径加固）。
3. P0-5：BlockGuard、Submit 结果、ScanTimeout/CleanupDeadConsumers 边界。
4. P1-1：nodiscard 与 fail-closed accessor（先以 OSP_STRICT_NODISCARD 观察）。
5. P0-4 已完成；后续只在版本迁移说明中记录 ABI 变更。
6. P1-2 至 P1-6：串口、总线 profile、executor、lifecycle、pipeline。
7. 并行工作流：安全需求、SFMEA/FTA、故障注入、覆盖率、安全手册。
8. 全量测试、ASan/TSan/UBSan、文档复核。

每一步都必须附带对应测试；没有测试的改动不进入下一步。

## 8. 验收矩阵

| 改动 | 测试 | 通过标准 |
| --- | --- | --- |
| P0-1 | 消费者不启动，10 个 critical 故障 | 全部 active；溢出标志为真；清除后归零 |
| P0-2 | 线程启动失败、超时后单次心跳、连续心跳 | 返回错误；单次不恢复；连续 3 次恢复 |
| P0-3 | 非法 affinity、无权限 SCHED_FIFO | 返回对应错误；`IsRunning()` 符合策略 |
| P0-4 | 同名二次创建、错误布局 attach、force 重建 | `kAlreadyExists`、`kLayoutMismatch`、epoch 变化 |
| P0-5 | guard 析构、processing 超时、force cleanup | 自动归还；不误回收；显式清理成功 |
| P1-1 | `-Werror=unused-result` 编译检查 | 忽略返回值编译失败 |
| P1-2 | PTY CRC 错误、持续灌数据 | 故障上报；Poll 有预算上界 |
| P1-3 | 安全 profile 满队列发布 | 返回 false；无长扫描；分级统计正确 |
| P1-4 | 两个 bus + 两个 executor | 各自只消费自己的消息 |
| P1-5 | 并发 Trigger、恢复守卫失败 | 一个成功一个 `kBusy`；失败保持状态 |
| P1-6 | handler 返回 false | 后继不执行；故障上报；块回收 |

## 9. 兼容性与回滚

| 改动 | 兼容性 | 回滚方式 |
| --- | --- | --- |
| P0-1 | 行为增强，API additive | 移除先锁存和溢出标志即可 |
| P0-2 | 返回值从 void 变 expected，源码兼容 | 恢复 void 和单次恢复 |
| P0-3 | 新增 `StartChecked()`，旧 `Start()` 保留 | 不使用新接口 |
| P0-4 | 共享内存布局变化，需 version bump | 旧 API 保留一个版本；布局版本单独升级 |
| P0-5 | 新增 guard，裸接口保留 | 不使用 guard，恢复旧 ScanTimeout |
| P1-1 | nodiscard 可能破坏下游构建 | 关闭 `OSP_STRICT_NODISCARD` |
| P1-2 | 删除/弃用 ACK 字段 | 保留 deprecated 字段一个版本 |

## 10. 最小改动的边界

以下事情不做，避免把“最小优化”变成重构：

- 不新增 `SafetyManager`、`HealthService` 之类的抽象层。
- 不把 `FaultCollector` 改成全局单例。
- 不把所有容器替换成 `FixedVector`；只修热路径和边界。
- 不把 `DataDispatcher` 改成完整 iceoryx 模型；只加 `BlockGuard` 和安全回收边界。
- 不把 `Breaker` 改成调度器；只把它的输出接到现有降级点。
- 不在没有故障注入和覆盖率之前宣称任何 ASIL/SSIL 结论。

## 11. 完成标准

最小优化完成的标志不是“代码能编译”，而是：

1. P0 的每个失败路径都有可复现测试。
2. critical 故障、配置失败、布局不匹配、超时回收都能被观察到。
3. `expected` 和关键 API 的失败不能被静默忽略。
4. README 和设计文档不再包含未实现的承诺。
5. ASan/TSan/UBSan 和 `-fno-exceptions` 构建全部通过。

满足以上条件后，newosp 仍然不是“经过认证的功能安全组件”，但它会成为一个**可以被安全分析、可以被故障注入、可以被证据追溯**的基础设施库。

## 12. 评审基线

- 代码基线：newosp HEAD `96bbbb6`，评审时工作树另有未提交修改（`serial_transport.hpp`、`transport.hpp` 等），实施前应先固定一个干净提交。
- 本文只描述最小改动，不替代 SFMEA、FTA、DFA、安全手册或任何 ASIL/SSIL 结论。
- 每项改动的验收测试应与代码同一提交，测试报告绑定 commit、配置、平台和编译器版本。
