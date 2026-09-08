# coact 设计引入 newosp 计划

## 1. 结论与背景

coact 与 newosp 同属事件驱动基础设施库，但目标平台不同：coact 面向 RT-Thread MCU 单核，newosp 面向嵌入式 Linux 多线程。本计划回答一个问题：coact 的哪些设计值得引入 newosp，哪些必须留在 MCU 侧。

核心结论：**值得引入的是「平台无关的纯 C++ 设计」——静态 HSM 表、编译期配置、背压熔断、词汇类型升级；不该引入的是「MCU 特化」——无锁单核临界区、零堆强制、16 位原子门槛。** 同时 newosp 自身存在若干锁窗口过大、忙等轮询的问题，与 coact 无关，应一并修复。

## 2. 场景差异约束

| 维度 | coact（RT-Thread MCU 单核） | newosp（嵌入式 Linux） |
|---|---|---|
| 并发模型 | 单 Dispatcher 线程 + AO 事件排队 | 多线程 + mutex/条件变量 |
| 同步原语 | 无锁 CAS / irq-mask 临界区 | std::mutex / std::thread / futex 信号量 |
| 内存 | 业务零堆，定容静态存储 | 允许 std::string / std::vector |
| 平台分支 | PAL + Profile 模板参数，业务无 #ifdef | #ifdef 平台分支（Linux/macOS/lwIP） |

依据此表：coact 中与「单核、无锁、零堆」绑定的设计不可照搬；与「类型系统、状态机、配置、可靠性」绑定的设计可移植。

## 3. 五维调研结论

调研由 5 个并行只读 worker 完成，关键结论已抽样验证。

| 维度 | 核心结论 |
|---|---|
| 无锁结构 | newosp 数据路径已无锁（SpscRingbuffer/AsyncBus 为 lock-free），mutex 集中在阻塞原语与回调注册表。可借鉴 `std::launder`、`is_always_lock_free` 断言、`try_push_observed` 合并返回 |
| 事件派发 | newosp 存在锁窗口过大的真实问题：`service.hpp:221` 锁内 `join()`、`node_manager.hpp:462` 锁内 TCP `SendHeartbeat` |
| 编译期设计 | coact 的静态 HSM 转移表、`TransitionKind`、`NewType` 默认构造/`raw()`、`ScopeGuard` 模板化、`DefaultConfig` 常量块均平台无关、可直接引入 |
| 可靠性/背压 | newosp 缺过载降级、老化、背压消费方；coact 的 5 状态 `Breaker` + 80/50 滞回水位 + cooldown 不可跳过语义最值得移植 |
| 平台抽象 | newosp 的 `IoPoller`/`LightSemaphore`/时钟/线程可 Policy 化；头文件选择、后端实现、架构特定代码的 `#ifdef` 应保留 |

## 4. 设计模式落点

| 模式 | newosp 落点 | 是否来自 coact |
|---|---|---|
| 策略（Policy） | `IoPoller<Backend>`、`LightSemaphore<SemaphoreOps>`、`ClockOps` | 是（coact Profile/ClockOps） |
| 状态 | 静态 HSM 转移表替代 `StateMachine::Dispatch` 的运行时 enum if-链 | 是（coact `StateDef[]`+`TransitionDef[]`） |
| 命令 | bus / app message 的 handler if-链改为表驱动 | 是（coact `Event.signal` + constexpr 表） |
| CRTP AOP | 编译期 `Trace` Policy（默认 NullTrace 零开销）注入日志/监控 | 是（coact Trace） |
| 装饰器 | `transport.hpp` v0/v1 帧协议、`SequenceTracker`、`serial_transport` CRC：编译期模板装饰器 `CrcTransport<Inner>`，零虚函数 | 否（newosp 自身场景，coact 无对应实例） |

## 5. 不引入的 MCU 特化

1. `SingleCoreCriticalRing` 与 irq-mask CS 注入：SMP Linux 上 irq-mask 无意义。
2. `RttSingleCoreProfile` 的「单核内 alloc/reclaim 不交错」假设：SMP 上为假，会损坏池。
3. 16 位原子 / `0x7FFFU` 容量硬门槛：newosp 的 `IndexT=size_t` 更灵活，照搬反而退化。
4. 业务代码零堆强制：newosp 面向 Linux，合理使用标准容器不必为无堆而无堆。

## 6. 分阶段执行计划

### 阶段 1：锁 bug 修复（优先级最高，与 coact 无关）

目标：消除锁内阻塞/系统调用，改为「锁内收集、锁外执行」。

| 位置 | 问题 | 修法 |
|---|---|---|
| `service.hpp:221-225` | `Stop()` 持 `threads_mutex_` 时 `join()` 所有 worker | 锁内收集 thread 到临时数组，锁外 join |
| `service.hpp:238-247` | `GetPort()` 持锁调 `GetSockName` 系统调用 | 锁内取 fd 副本，锁外调系统调用 |
| `service.hpp:274,308` | `ReapFinishedWorkers()` 锁内 `join()` | 锁内收集，锁外 join |
| `service.hpp:328-341` | `AcceptLoop` 锁内 `thread.Start()` | 锁内预留槽位，锁外启动 |
| `node_manager.hpp:462-467` | 心跳路径锁内 TCP `SendHeartbeat` | 锁内收集节点快照，锁外发送 |
| `node_manager.hpp:482-488` | 独立心跳线程同款锁内发送 | 同上 |

验证：`-Wall -Wextra -Werror` 编译通过，`tests/` 冒烟测试通过。

### 阶段 2：可靠性机制引入（背压/熔断）

目标：补齐 newosp 缺失的过载降级与老化机制。

1. 引入 5 状态 `Breaker`（Normal → BrokenL1 → BrokenL2 → Safe → Recovering），状态打包进单个 `std::atomic<uint32_t>`，`static_assert` lock-free。
2. 引入 80/50 滞回水位采样 + N 样本持久性。
3. cooldown 不可跳过语义：单次合格样本只清超时计数，不跳冷却。
4. 提供 `drop_non_critical()` / `safe_events_only()` 分级查询供策略层消费。

落点：新增 `include/osp/breaker.hpp`，接入 `fault_collector.hpp` 的 `BackpressureLevel` 作为消费方。

### 阶段 3：词汇类型升级（低风险小改动）

1. `NewType` 增加 constexpr 默认构造、`raw()` 访问器、`explicit operator bool()`、`static_assert(sizeof)`。
2. `ScopeGuard` 改为 `template <typename Cleanup>`，去掉内部 `FixedFunction<void()>` 的一层间接。
3. 新增 `DefaultConfig` 常量块，收拢散落的魔法数字（`OSP_HSM_MAX_DEPTH`、`OSP_CONFIG_MAX_FILE_SIZE`）。

### 阶段 4：较大重构（按需分模块推进）

1. 静态 HSM 转移表：新增 `StateDef`/`TransitionDef` POD 对 + `TransitionKind{External,Internal,Self}`，与现有 `StateMachine` 并存，逐步迁移调用方。
2. `IoPoller<Backend>` Policy 化：`EpollPolicy`/`KqueuePolicy`/`PollPolicy`，`#ifdef` 只保留在后端选择边界。
3. `transport` 装饰器化：`CrcTransport<Inner>`、`FramedTransport<Inner>`，保持零虚函数。
4. 编译期 `Trace` Policy 注入日志/监控切面。

## 7. 验证方式

- 每个阶段独立提交，提交前 `-Wall -Wextra -Werror` 编译通过。
- 阶段 1 以 `tests/` 冒烟测试 + 新增并发场景用例验证无死锁/无竞态（TSan 可选用）。
- 阶段 2 以单测覆盖 Breaker 状态转移矩阵与滞回/cooldown 语义。
- 阶段 3、4 保持既有示例（examples/）全部可编译运行，行为不变。
