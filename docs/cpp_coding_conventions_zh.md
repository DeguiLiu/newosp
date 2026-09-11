# newosp C++ 编码规约（中文版）

本文档是 **newosp 仓库** C++ 代码的完整编码规约，**改编自 coact 仓库的同名文档**（`coact/docs/cpp_coding_conventions_zh.md`）。视角是现代 C++17——不是从 C/MISRA 移植过来的 C-with-classes，而是编译期确定、低拷贝、低分支、静态多态优先的表达。目标读者：在本仓库编写或评审 C++ 代码的工程师与自动化审查代理。每条规约写成可判定形式——评审时对每条回答"是/否"即可。

**关键前提：coact 的规约不自动适用于 newosp。** coact 面向 RT-Thread 单核 MCU，强调无锁单核临界区、业务零堆、ISP 管线与 Active Object 运行时；newosp 是面向嵌入式 Linux（并兼容 RT-Thread / macOS / Windows）的头文件库，定位是节点 / 服务 / 传输 / 事件总线框架。凡 coact 特化且 newosp 没有对应机制的条目，本文不改写为规则，而是显式标注「newosp 无对应机制」或改写到 newosp 的真实等价物。评审时不得因为"coact 文档这么写"就要求 newosp 代码具备它不存在的机制。

规约锚点分两层：

- 框架层：`include/osp/` 头文件（`hsm.hpp`、`hsm_table.hpp`、`vocabulary.hpp`、`mem_pool.hpp`、`spsc_ringbuffer.hpp`、`bus.hpp`、`event_loop.hpp`、`breaker.hpp`、`watchdog.hpp`、`timer.hpp`、`semaphore.hpp`、`thread.hpp`、`platform.hpp`、`config.hpp`、`log.hpp`、`async_log.hpp`、`node.hpp`、`static_node.hpp`、`node_manager.hpp`、`service.hpp`、`service_hsm.hpp`、`data_dispatcher.hpp`、`data_fusion.hpp`、`transport.hpp`、`qos.hpp` 等）；
- 示例层：`examples/`（`hsm_protocol_demo.cpp`、`node_manager_hsm_demo.cpp`、`hsm_bt_combo_demo.cpp` 等带自验证的示例）。

`expected` / `FixedString` / `FixedVector` / `FixedFunction` / `NewType` / `ScopeGuard` 的统一实现入口是 `vocabulary.hpp`——**newosp 没有 coact 的 `expected.hpp` 兼容转发头**。落点标注到文件与类名/函数名，不标注行号（行号随重构漂移）；抽象规约配以最小通用片段自证。

适用范围：`include/osp/`、`examples/`、`tests/` 下全部 C++ 代码。C 接口（RT-Thread / POSIX）约束单独收口在 7.2 节，正文不展开。

红线速查（详见各章）：禁止异常逃逸；禁止裸 `int/long/char`；禁止裸 `enum` 与新增业务 `#define` 常量；禁止裸指针代替引用/句柄；禁止动态分配池 / 工厂模式 / 过度抽象层；禁止跨线程共享可变状态不经同步或消息传递；禁止在锁内执行回调或系统调用；禁止未定界递归；禁止固定 sleep 排空；禁止为用而用任何 C++17 特性；三个相似才抽基类；组合优先于继承，静态多态优先于运行时多态。

## 1. 总则

### 1.1 语言与标准

- **C++17**，由 `target_compile_features(osp INTERFACE cxx_std_17)` 锁定（CMakeLists.txt）。禁止使用 C++20 特性（concepts、ranges、`<format>` 等）。
- 理由：newosp 是头文件库，须在 RT-Thread 工具链与 Linux host 工具链上同时稳定编译；同理，核心代码须能在 `-fno-exceptions -fno-rtti` 可选模式（CMake 选项）下构建。C++17 是两条工具链的共同稳定基线。
- 可用的 C++17 核心能力以第 5 章的使用规约为准；语言特性本身允许不等于任何场景都该用。

### 1.2 平台约束

- 所有 C++ 代码必须同时可在 **RT-Thread 目标机** 与 **Linux / macOS host** 编译运行（`platform.hpp` 以 `OSP_PLATFORM_RTTHREAD` / `OSP_PLATFORM_LINUX` / `OSP_PLATFORM_MACOS` 探测）。平台差异只允许经两类手段隔离：
  - 编译期平台宏：`OSP_PLATFORM_*`（platform.hpp），仅用于选择后端；
  - 后端策略模板：`ThreadT<Ops>` 注入线程后端（`PosixThreadOps` / `RtThreadOps` / `Win32ThreadOps`，thread.hpp）、`IoPoller` 注入 epoll / kqueue / poll 后端（io_poller.hpp）、`SleepStrategy` 注入空闲策略（executor.hpp）。
- **平台 `#ifdef` 只允许出现在平台边界头**（`platform.hpp`、`thread.hpp`、`io_poller.hpp`、`executor.hpp`、`log.hpp`、`shm_transport.hpp`）；业务、协议、节点、服务代码不得出现平台分支。
- newosp **没有** coact 的 PAL 抽象层（`pal.hpp` / `pal_posix.hpp` / `pal_rtthread.hpp`）与 `Runtime<Config, PalT, Profile>` 注入点，新代码不得引用。也没有 `RttSingleCoreProfile` / `HostSmpProfile` 这类并发 Profile——单核 / SMP 语义差异由实际同步原语（`Mutex` / `std::atomic` / 无锁环）承担，而不是模板 Profile。

### 1.3 现代 C++17 基调

本仓库的 C++ 有四条基调，全文档各章都是它们的具体化：

- **编译期确定、运行期少分配**：容量与契约一律模板参数或 `static_assert`——`StateMachine<Context, MaxStates>`、`TableHsm<Context, MaxStates, MaxTransitions>`、`FixedPool<BlockSize, MaxBlocks>`、`ObjectPool<T, MaxObjects>`、`Pipeline<MaxStages, MaxEdges>`、`ServiceRegistry<MaxServices>`。
- **低拷贝 / 零拷贝**：热路径零堆（`AsyncBus` 头注释明确 "zero heap allocation in hot path"）；大块数据用 `block_id` 描述符 + 引用计数（data_dispatcher.hpp），不随消息拷贝；交接用移动语义（`SpscRingbuffer::Push(T&&)`、move-only 的 `FixedFunction` / `expected`）。
- **低分支**：`if constexpr` 编译期分流（`std::is_trivially_copyable<T>` 的 memcpy 路径，vocabulary.hpp）+ 表驱动（HSM 转移表，hsm_table.hpp）+ `std::variant` 访问取代 if 链。
- **边界清晰**：每个跨模块结构体在定义处用 `static_assert` 声明类型契约；违约编译失败而非现场崩溃。

### 1.4 语言与注释语言

- 代码、注释、commit message 用英文；面向人的文档用中文（本文件、`docs/*_zh.md`、`README_zh.md`、各 `examples/*/README.md`）。
- **禁止在代码或代码注释中夹杂中文**。newosp 框架头与示例源文件已全英文；存量个例（如 `tests/test_data_dispatcher.cpp` 的一处中文注释）为历史遗留，新增代码不再扩展此风格。

## 2. 类型与内存安全

### 2.1 类型纪律（现代 C++ 表达）

- **固定宽度整型**：优先 `<cstdint>` 体系（`uint8_t / uint16_t / uint32_t / int32_t` 等），禁裸 `int / long / char / unsigned`；宽度契约用 `static_assert` 钉在定义处，不靠人肉记忆。
- **强类型代替弱转换**：领域枚举用 `enum class X : 底层类型`（禁 `#define` 常量、禁裸 `enum`）；语义 id 用 `NewType<T, Tag>`（vocabulary.hpp）而不是裸整数；可判空的语义类型用 `explicit operator bool()` 或 `not_null<T>`（vocabulary.hpp），不返回裸 int / 指针；错误用 `expected<V, E>` 或错误码枚举，不返回裸 int。
  - 落点：`enum class Level : uint8_t`（log.hpp）、`enum class BreakerLevel : uint8_t`（breaker.hpp）、`enum class MessagePriority : uint8_t`（bus.hpp）、`enum class TransitionKind : uint8_t`（hsm_table.hpp）、`enum class TransitionResult : uint8_t`（hsm.hpp）；`NewType` / `SessionId` / `TimerTaskId`（vocabulary.hpp）、`WatchdogSlotId`（watchdog.hpp）、`SubscriptionHandle`（bus.hpp）、`ConnectionId`（connection.hpp）；`explicit operator bool()`（vocabulary.hpp 的 expected / optional / FixedFunction）；`not_null<T>`、`function_ref<Sig>`（vocabulary.hpp）。
  - 纠正：coact 文档里的 `Expected`（大写）与 `expected.hpp` 在 newosp 不存在；newosp 的类型名是小写 `expected`，定义在 `vocabulary.hpp`。
- **隐式转换显式标注**：任何跨宽度 / 跨符号赋值必须写 `static_cast<目标类型>(...)`。
  - 落点：`static constexpr uint32_t kQueueDepth = static_cast<uint32_t>(OSP_BUS_QUEUE_DEPTH);`（bus.hpp）等框架层一致使用。
- 常量左侧（Yoda 比较）作为**新增代码**的统一写法：`0 == x` / `nullptr == p`。存量代码两种写法并存，评审以改动 diff 为准。
  - 落点：`0 == (events & ...)`（node_manager.hpp）、`0 == n`（config.hpp）、`nullptr != self`（shutdown.hpp）。

### 2.2 健壮性基础（源自 MISRA 精神的现代等价物）

MISRA C:2012 在本仓库不再逐条适用；它的精神已翻译成 C++17 表达：

- **复合语句纪律**：`if` / `for` / `while` / `do-while` 的分支体一律用 `{}` 包裹，即使单语句——防悬挂 else 与后续插入语句时的作用域漂移。
- **禁 `goto`**：跨作用域跳转禁用；控制流清理由 RAII（析构保证）与 HSM 转移拓扑承担。
- **禁未定界递归**：递归深度必须在编译期可界定，并在代码处声明上界；深度不可静态界定的递归禁止。受控例外：`Pipeline::ExecuteStage` 是同步 DAG 执行递归，深度上限为 `MaxStages`，且有明确注释（"Maximum recursion depth equals the longest chain in the DAG"，data_dispatcher.hpp）。树 / 组合遍历优先展平为固定深度循环。
- **`switch` 完整**：每个 `switch` 必须带 `default` 标签或穷举 + 兜底返回；`enum class` 的穷举交编译器检查，`default` 作兜底而非吞掉遗漏。
  - 落点：`LevelTag` / `Level` 分派（log.hpp）。
- **RAII 取代手工配对**：`malloc/free`、`lock/unlock`、进入/恢复等成对操作整体包成 guard 对象，任意退出路径（含提前 return）自动配对；裸配对调用被禁用。
  - 落点：`ScopeGuard` + `OSP_SCOPE_EXIT`（拷贝 `= delete`，vocabulary.hpp）；`std::lock_guard<osp::Mutex>`（fault_collector.hpp）；`WatchdogGuard`（watchdog.hpp）；`DirGuard` / `PipeGuard`（process.hpp）。

### 2.3 内存策略：热路径零堆、定容优先

- **禁止动态分配池 / 工厂模式**。需要"多形态行为"时用模板参数化（CRTP / Policy 静态多态）或 `constexpr` / 静态数据表；确需运行期擦除的少量场景用 const 函数指针表，但首选模板。
  - 落点：`JobHandler` 函数指针表（`StageConfig`，data_dispatcher.hpp）；总线回调经 `FixedFunction` 小缓冲擦除（bus.hpp 的 `CallbackType`）。
- **热路径零堆、定容优先**。newosp 面向嵌入式 Linux（不是 MCU），**允许**在冷路径 / 解析层使用 `std::string` / `std::vector`（如 config.hpp、inicpp.hpp、toml.hpp、process.hpp）；但数据 / 事件 / 日志热路径必须零堆，存储优先编译期容量类型或内联 / 调用方存储。
  - 落点：`AsyncBus` "zero heap allocation in hot path"（bus.hpp）；`FixedVector<T, Capacity>` / `FixedString<Capacity>` / `FixedFunction<Sig, BufferSize>`（vocabulary.hpp）；`SpscRingbuffer<T, BufferSize>` 内联 `std::array`（spsc_ringbuffer.hpp）；`async_log.hpp` 每线程 SPSC + 定长 `LogEntry`。
  - 纠正 coact 的"业务代码零堆"：newosp 无此强制；强制的是**热路径**零堆，冷路径允许标准容器。
- **侵入式空闲链代替堆容器**：`FixedPool` 在块首 4 字节内嵌 `next` 索引，配合 tagged 头无锁回收（mem_pool.hpp）。
  - 落点：`FixedPool<BlockSize, MaxBlocks>` 的内联 `alignas(std::max_align_t) uint8_t storage_[...]` + `std::atomic<uint32_t> free_head_`（mem_pool.hpp，注释明确 "Inline storage -- zero heap allocation"）。
- **减少裸指针 → 引用 / 值语义句柄**：所有权清晰的传参用引用（`const T&`）；跨线程 / 跨边界的可空句柄用值语义 id，不用裸指针 + 注释描述谁拥有。
  - 落点：`Publisher<T, PayloadVariant>` 持总线引用 + sender_id（node.hpp）；`SubscriptionHandle`（bus.hpp）；`ConnectionId`（connection.hpp）；`NewType` id（vocabulary.hpp）。
- **newosp 无 `EventPool`**（coact 那种由调用方提供存储块、池只做管理的机制）。newosp 的池自带内联存储（`FixedPool` / `ObjectPool`），或由调用方在 `Pipeline` / `DataDispatcher` 上层管理块；不得引用 `EventPool`。

### 2.4 placement new 与对象生命周期纪律

- **placement new 用于固定 / 复用存储的就地构造；若类型非平凡析构，必须显式配对析构**——由容器在析构 / 赋值 / 回收路径调用 `~T()`。这是 coact"placement new 仅用于平凡可析构类型"在 newosp 的等价物：newosp 的支持面更宽（允许非平凡析构），代价是析构配对的纪律。
  - 落点：`ObjectPool<T>::Create` 的 `::new (mem) T(...)` 与 `Destroy` / 析构中的 `obj->~T()`（mem_pool.hpp）；`expected<V, E>` 的 placement new 与 `reinterpret_cast<V*>(&storage_)->~V()`（vocabulary.hpp）；`FixedVector` 的 `::new (&storage_[...]) T(...)`（vocabulary.hpp）。
- **原始存储的 newosp 既有形态是 `alignas(T) uint8_t[]` / `std::aligned_storage` + placement new + 显式转型**，不是 coact 的 `std::byte[]` + `std::launder`；newosp 核心代码不使用 `std::launder`（仅第三方 toml.hpp 使用）。
  - 落点：`FixedVector` 的 `alignas(T) uint8_t storage_[sizeof(T) * Capacity]`（vocabulary.hpp）；`FixedPool` 的 `alignas(std::max_align_t) uint8_t storage_[...]`（mem_pool.hpp）；`hsm_storage_`（app.hpp / lifecycle_node.hpp / node_manager_hsm.hpp）。
  - 保留规则：原始存储必须经 placement new 构造后访问，禁止裸类型双关。`std::byte` + `std::launder` 是更严格的写法，但 newosp 无先例，作为改进方向而非既有规约；不得声称 newosp 已采用。
- **跨边界结构体的布局契约在定义处 `static_assert`**：`is_trivially_copyable` / `is_standard_layout` / `is_trivially_destructible`。违约编译失败而非现场崩溃。
  - 落点：`LogEntry`（async_log.hpp）、`RecvFrameSlot`（transport.hpp）、`DataBlock` / `ConsumerSlot`（data_dispatcher.hpp）、`Service` 的 Request / Response（service.hpp）、共享内存 `Slot`（shm_transport.hpp）。
- **move 语义即所有权语言**：跨线程 / 跨槽位交接用 `T&&` + `std::move`；失败路径**不得消费调用者的值**（先确认容量再 move）。
  - 落点：`SpscRingbuffer::PushImpl` 先判满返回 false，再做 `data_buff_[...] = std::forward<U>(data)`（spsc_ringbuffer.hpp）；`FixedFunction` 拷贝 `= delete`、仅可 move（vocabulary.hpp）；`expected` move 构造 / 赋值（vocabulary.hpp）。

### 2.5 栈开销控制

零堆的另一面是**栈成为主要的运行期伸缩空间**，而嵌入式线程栈是编译期定容的稀缺资源。栈开销按"每一帧多大 × 最深几帧"两个维度控制，任何一处放大都必须能被静态界定。

- **定容线程栈是硬预算**：线程栈大小在 `ThreadOptions` / RT-Thread 线程参数处一次性定死（thread.hpp），业务代码不得假设有富余。
  - 落点：`ThreadOptions`（thread.hpp）；`RtThreadOps` 的 RT-Thread 线程参数（thread.hpp）。
- **大对象不入栈帧**：MB 级存储 / 查找表用 `static` / 内联成员 / 编译期容量容器，绝不作为大局部变量出现在函数栈上；确需局部的小容器用编译期容量（模板参数）。热路径批量缓冲用固定大小栈数组并声明上界。
  - 落点：`async_log.hpp` WriterLoop 的 `LogEntry batch[N]` 固定批量缓冲（注释明确 "on its thread stack"）；`FixedPool` / `AsyncBus` 的环形存储为内联成员而非栈对象。
- **按引用传大结构，按值传描述符 / 标量**：`sizeof` 超过若干字节的结构体一律 `const T&` 入参；小 POD（`NewType` id、枚举、`MessageHeader`）按值传，避免多一层间接。
- **事件 / 消息只带描述符或小载荷**：大块数据经 `DataDispatcher` 的 `block_id` 管理，处理函数栈帧里不会出现大缓冲（承 3.1）。
- **禁未定界递归与深调用链**：递归在定容栈上不可静态界定（同 2.2）；`Pipeline` 的同步递归深度 ≤ `MaxStages`，须一并核算栈预算。热路径调用深度应可被人工核验到"最深一条链 × 单帧上限 ≤ 栈预算"。
- **小缓冲类型擦除留在调用者栈，不外溢堆**：需要可调用对象承载捕获时，用固定内联缓冲的 `FixedFunction<Sig, BufferSize>`（默认 `2 * sizeof(void*)`），超出缓冲即编译失败（`static_assert(sizeof(Decay) <= BufferSize)`）而非静默回落堆——把栈占用显式化、有界化（vocabulary.hpp）。`expected` / `FixedString` / `FixedVector` 同族"内联、零分配"。

## 3. 线程与并发

### 3.1 事件 / 消息平面

- 组件之间通过消息总线或事件队列通信，**禁止跨线程裸访问共享可变状态**：
  - `AsyncBus<PayloadVariant>::Publish` / `Subscribe`（bus.hpp）——lock-free MPSC、按优先级的准入控制、热路径零堆；
  - `EventQueue<T, Depth>::TryPush` / `TryPop`（hsm.hpp）——固定容量无锁 MPSC，把事件从任意生产者线程汇入单一分发线程（HSM 自身无需 mutex）。
  - 落点：`Node` / `Publisher` / `StaticNode` 均经 `AsyncBus` 收发（node.hpp、static_node.hpp）；`WorkerPool` 经 `AsyncBus::PublishWithPriority` 提交（worker_pool.hpp）。
- 非消费者线程（worker、I/O 线程）与消费者的**唯一耦合是消息 / 事件平面**；不得直接读对方内部字段。
- **大块数据不随消息拷贝**：消息 / 事件携带 `block_id` / 描述符 / `Event{ id, data }`（hsm.hpp），真实字节留在拥有者或 `DataDispatcher` 块存储中。
  - 落点：`DataDispatcher::Release(block_id, generation)` 的块生命周期（data_dispatcher.hpp）；`Event` 定义（hsm.hpp）。
- 纠正：coact 的 Active Object（AO）、`alloc_typed`、`EventPool`、`coordinator()`、DDR / 像素数据平面在 newosp 均无对应物；newosp 的等价物是事件总线 + 节点 / 服务 + `DataDispatcher` 块引用。

### 3.2 同步与锁

- newosp **没有** coact 的 `L1 Singleton → L2 Context → L3 Device` 命名锁层级。通用规则：多锁场景必须在头注释声明获取顺序，禁止反向获取；单锁 / 无锁结构优先。
  - 落点：`SpscRingbuffer`（单生产者 / 单消费者无锁，spsc_ringbuffer.hpp）、`AsyncBus`（无锁 MPSC，bus.hpp）、`EventQueue`（无锁 MPSC，hsm.hpp）。
- **最弱足够原则**：若数据已被互斥机制（单消费者序列化、单写者、消息平面）覆盖，**不加锁**，且必须写注释论证为什么不需要锁。
- **回调 / 系统调用不得在锁内执行**：采用 collect-release-execute——锁内收集 / 更新，锁外执行回调或阻塞操作。
  - 落点：`EventLoop` 的 "Hooks run outside the mutex (collect-release-execute)"（event_loop.hpp）；`Discovery` 的 collect-release-execute（discovery.hpp）；`NodeManager` 的 "send outside it so a blocked TCP send does not hold mutex_"（node_manager.hpp）；`Service::Stop` 锁内收集线程、锁外 join（service.hpp）。
- 全局可变状态（若有）必须**单写者** + 注释声明它不是黑板；黑板（多生产者 + 轮询消费者）仅在明确需求下允许。该通用内核保留；coact 的"模拟硬件寄存器全局"在 newosp 无对应实例。
- 跨线程枚举 / 标志用 `std::atomic` 且必须断言 `is_always_lock_free`（libatomic 回退是隐藏的锁 / 堆依赖，构建期必须失败）。
  - 落点：`static_assert(std::atomic<bool>::is_always_lock_free, ...)`（event_loop.hpp）；`std::atomic<uint32_t>`（breaker.hpp）；`std::atomic<uint8_t> / <uint32_t>`（data_dispatcher.hpp）；共享内存原子（shm_transport.hpp）。

### 3.3 Worker 交接与停机

- **交接点满则返回失败，不阻塞、不排队**；调用侧计数丢弃。丢弃是诚实的计数器，不是隐藏的停顿。
  - 落点：`SpscRingbuffer::Push` 满返回 false（spsc_ringbuffer.hpp）；`EventQueue::TryPush` 满返回 false（hsm.hpp）；`AsyncBus` 队列满 → `BusError::kQueueFull` + `BusStatistics.messages_dropped` + `BusErrorCallback`（bus.hpp）。
- **优先级准入即背压**：`MessagePriority::kLow / kMedium / kHigh` 分别在队列 ≥ 60% / 80% / 99% 时丢弃（bus.hpp）；过载降级另见 `Breaker`（breaker.hpp）、`BackpressureLevel`（vocabulary.hpp）、`DataDispatcher` 背压回调（data_dispatcher.hpp）。
- **drain-on-stop**：停机先排空在途任务再退出；与"忙则丢弃"语义并存时**两者都要注释写明差异**。
  - 落点：`WorkerPool::Shutdown`（"stop accepting jobs, drain queues, join all threads"）、`FlushAndPause` / `Resume`、`StopAll`（worker_pool.hpp）。
- 关键区只含少量 store，**绝不把硬件 / 系统调用延迟圈进锁内**（承 3.2 collect-release-execute）。

## 4. 函数与控制流

### 4.1 return 预算

- **单个函数的 return 语句不超过 5 个**；能不提前 return 就不提前 return。错误路径可提前返回，但超过 5 个 return 说明函数职责过多，应拆分。

### 4.2 guard / entry / exit / action 分层

newosp 的两套 HSM 都在类型系统里显式分层，评审直接看签名即可：

- `StateConfig<Context>`：`HandlerFn` / `EntryFn` / `ExitFn` / `GuardFn`（hsm.hpp）；
- `StateDef<Context>`（`on_entry` / `on_exit`）+ `TransitionDef<Context>`（`action` / `guard`）（hsm_table.hpp）。

- **guard 是纯函数**：只读 ctx 与 event、无副作用、返回 bool。newosp 的函数指针签名即约束——`GuardFn = bool (*)(const Context&, const Event&)`（hsm.hpp），hsm_table 的 guard 亦为谓词。
- **entry 只做进入动作**：进入状态时发出的资源获取 / 命令下发必须放在 entry action，**不得放在 transition action**（transition action 在状态切换前执行，从那里自提交事件会与拓扑竞争）。
- **exit 只做清理**：退出状态时的资源释放 / 计数收尾。
- **action 是事件响应**：transition action 只做"本弧的业务效果 + 更新状态"，控制流归 HSM 拓扑。

### 4.3 状态机表驱动

- 行为一律**静态表驱动**，禁止在 action 里 if-else 模拟状态机。
  - 落点：`TableHsm<Context, MaxStates, MaxTransitions>` + 调用方持有的 `StateDef[]` / `TransitionDef[]`，表通常 `constexpr`、无拷贝无堆（hsm_table.hpp）；`StateMachine<Context, MaxStates>`（hsm.hpp）。
- 拒绝路径必须显式：非法 (状态, 事件) 对落到显式 reject 弧 / 根 reject（计数 + trace），禁止静默丢弃。
  - 落点：`Dispatch` 在层级冒泡后应用匹配转移，否则落根 reject 弧（hsm_table.hpp）；`TransitionKind::kSelf` 承载自环 / 重入（hsm_table.hpp）。

### 4.4 事件生命周期与可观测性

- newosp 的 `Event` 是值类型（`{ uint32_t id; const void* data; }`，hsm.hpp），**没有引用计数**；**引用计数的块生命周期在 `DataDispatcher`**：`Alloc()` 取块、`Release(block_id, generation)` 归还，归零回收。业务只提交不手动回收，并以可验证方式断言零泄漏。
  - 落点：`DataDispatcher::Release` 与 `BlockState` 生命周期（data_dispatcher.hpp）。
  - 纠正：coact 的 `Event::ref_ctr` / `EventPool::used()` 在 newosp 不存在。
- 组件静态属性（容量、策略、handler）一律走模板参数 / 编译期常量，不走构造参数或运行期 setter。
  - 落点：`StaticNode<PayloadVariant, Handler>` 编译期绑定 handler（static_node.hpp）；容量模板参数（`StateMachine` / `ServiceRegistry` / `Pipeline`）。
  - 纠正：coact 的 `Ao<Context, HsmT, Traits>` / `AoTrait` 在 newosp 无对应物。
- 运行期可观测性来自专用组件，不往业务代码里加散装打印探针：
  - `SystemMonitor<MaxDiskPaths>` 周期快照（system_monitor.hpp）；
  - `FaultCollector<...>` 故障环形缓冲与统计（fault_collector.hpp）；
  - `ThreadWatchdog<MaxThreads>` + `ThreadHeartbeat`（watchdog.hpp、platform.hpp）。
- HSM 转移种类用对：`TransitionKind::kInternal`（只跑 action）、`kSelf`（自环 / 拒收重入）、`kExternal`（LCA 离开再入目标，边界状态重跑 entry/exit）（hsm_table.hpp）。
- 排空用事件驱动条件，**禁止固定 sleep 赌时序**；空闲等待由后端按最近到期时间阻塞。
  - 落点：`EventLoop`（"sleeps until the next event is due instead of polling at a fixed interval"，event_loop.hpp）；worker drain 见 3.3。
- **跨线程唤醒只能经单一线程入口**，不得从消费者上下文自提交竞争闩锁。
  - 落点：`EventLoop::Wake()` 是唯一跨线程入口，`OnWake` 钩子由派生类在 Run 线程处理（event_loop.hpp）。
- 组件 / 容量上限由配置约束，合并同类项以达标。
  - 落点：`OSP_EXECUTOR_MAX_NODES`、`OSP_APP_MAX_INSTANCES`、`OSP_NODE_MANAGER_MAX_NODES`、`OSP_MAX_NODE_SUBSCRIPTIONS`（opt.hpp）。

## 5. C++17 特性使用规约

每特性三段式：何时用 / 红线 / 代码落点。

### 5.1 `if constexpr`

- **何时用**：同一模板需要按编译期条件实例化不同路径，且不需要的那条路径根本不该被实例化。
- **红线**：禁止用 `if constexpr` 包裹恒真 / 恒假条件来"预留未来分支"；运行期才知道的条件必须用普通 `if`。
- **落点**：`if constexpr (std::is_trivially_copyable<T>::value)` 分流 memcpy / 逐元素两条路径（vocabulary.hpp）；`if constexpr (sizeof...(Rest) > 0)`（config.hpp）。

### 5.2 `constexpr` / `inline constexpr`

- **何时用**：模式表、容量、调参常量、枚举名表——所有"数据即契约"的常量集合。头文件内自由常量用 `inline constexpr` 避免 ODR；类内常量用 `static constexpr`。编译期确定的值一旦被 `static_assert` 或模板消费，即锁进二进制。
- **红线**：不为 constexpr 而 constexpr（运行期才确定的值就用普通变量）；禁止把大表写成 constexpr 但从未被编译期消费又声称收益。
- **落点**：`inline constexpr uint32_t kInstanceHsmMaxStates`（app.hpp）、`inline constexpr uint32_t kHeartbeatMagic`（node_manager.hpp）、`inline constexpr uint16_t kSerialSyncWord`（serial_transport.hpp）；类内 `static constexpr uint32_t kMaxEntries / kMaxKeyLen / kMaxValueLen`（config.hpp）；`static constexpr size_t kCacheLineSize`（platform.hpp）。
- newosp 既有的编译期调优开关是 `opt.hpp` 的 `#define OSP_*`（lwIP opt.h 风格，如 `OSP_BUS_QUEUE_DEPTH`、`OSP_HSM_MAX_DEPTH`）；**新增业务常量不得用 `#define`**，应用 `kPascalCase` 常量。

### 5.3 `enum class` + 底层类型

- **何时用**：一切新枚举；跨边界 / 进消息的枚举必须带底层类型。
- **红线**：禁止裸 `enum`（newosp 头文件现无裸 enum，保持）。
- **落点**：`Level : uint8_t`（log.hpp）、`BreakerLevel : uint8_t`（breaker.hpp）、`MessagePriority : uint8_t`（bus.hpp）、`TransitionKind : uint8_t`（hsm_table.hpp）、`TransitionResult : uint8_t`（hsm.hpp）。

### 5.4 `static_assert` 类型萃取

- **何时用**：凡是"该类型必须满足 X"的假设，一律在定义处或模板内断言（`is_standard_layout` / `is_trivially_copyable` / `is_trivially_destructible` / `atomic<T>::is_always_lock_free` / `is_same`）。违约必须编译失败，不许到现场才炸。
- **红线**：禁止断言显然为真的平凡事实凑数。
- **落点**：`is_trivially_copyable`（async_log.hpp、transport.hpp、service.hpp）；`is_trivially_destructible`（data_dispatcher.hpp）；`is_standard_layout`（shm_transport.hpp）；`is_always_lock_free`（event_loop.hpp、breaker.hpp、data_dispatcher.hpp）；`FixedFunction` 的 `static_assert(sizeof(Decay) <= BufferSize)`（vocabulary.hpp）。

### 5.5 `[[nodiscard]]` / `noexcept` / `explicit`

- **何时用**：
  - `[[nodiscard]]`：返回值承载错误 / 所有权 / 关键结果的函数（忽略即 bug）；newosp 在函数级使用。
  - `noexcept`：不抛函数（move、纯查询、guard、静态策略）——本库核心以 `-fno-exceptions` 可编译为准，`noexcept` 是接口契约而非优化提示；
  - `explicit`：单参构造与转换运算符（`explicit operator bool()` 让"有值"判断不会静默变 int）。
- **红线**：不整文件机械标注；`[[nodiscard]]` 用于"忽略它必然是错"的场景。
- **落点**：`[[nodiscard]] expected<void, LoopError> AddFd(...) noexcept`（event_loop.hpp）；`[[nodiscard]] bool has_value()` 与 `explicit operator bool()`（vocabulary.hpp）；`noexcept` 遍布 guard / 静态钩子 / policy（vocabulary.hpp、bus.hpp、event_loop.hpp 等）。
- 纠正：coact 的 `class [[nodiscard]] Expected final` 整类标注在 newosp 不存在——newosp 的 `expected` 类本身未整类标注。

### 5.6 `std::exchange`

- **何时用**：仅当需要**"取旧值 + 置新值"一体的原子语义交接**——即旧值确实被消费，且置新值是交接的一部分。
- **红线**：单纯赋值不得硬改成 `std::exchange`；不消费返回值时写 `static_cast<void>(std::exchange(...))` 并保留注释。
- **落点**：**newosp 核心头文件暂无 `std::exchange` 用例**（仅第三方 toml.hpp 使用）。本条为保留规约：若引入 `std::exchange`，按上述判定；newosp 既有的所有权交接用 `std::move`（`SpscRingbuffer::Push(T&&)`、move-only 的 `FixedFunction`）。

### 5.7 placement new + 对齐存储

- **何时用**：在原始内存（池块、内联缓冲、复用槽位）中就地构造，见 2.4。对齐由 `alignas(T)` / `alignas(std::max_align_t)` 在**存储声明处**保证，不在使用处补救。
- **红线**：非平凡析构类型进复用内存必须显式配对析构；禁止未经构造直接访问原始存储。
- **落点**：`FixedVector` 的 `alignas(T) uint8_t storage_[...]`（vocabulary.hpp）；`FixedPool` 的 `alignas(std::max_align_t) uint8_t storage_[...]`（mem_pool.hpp）；`ObjectPool` 的 `::new (mem) T(...)`（mem_pool.hpp）。
- 说明：newosp **无** `std::byte[] + std::launder` 既有形态；不得把 `std::launder` 当成 newosp 的当规约。

### 5.8 `std::move` / 右值引用（值类别即所有权）

- **何时用**：跨线程 / 跨槽位交接对象时以 `T&&` 参数 + `std::move` 表达"源从此失效"；失败路径不得消费调用者的值（先查容量再 move）。
- **红线**：move 后的源对象禁止再读；禁止对 const 对象强行 `const_cast` 后 move；复制成本可忽略的标量 / 描述符不必 move（过度 move 与漏 move 同罪）。
- **落点**：`SpscRingbuffer::Push(T&&)` / `PushImpl` 先判满再 move（spsc_ringbuffer.hpp）；`FixedFunction` 拷贝 `= delete`、仅可 move（vocabulary.hpp）；`expected` 的 move 构造 / 赋值（vocabulary.hpp）。

### 5.9 特性使用总红线

- **不为用而用**：任何特性必须能回答"不用它会怎样"。
- **三个相似才抽基类**：出现第三处结构相似时才提取（CRTP / Policy / 宏），两处时容忍重复。
- **禁过度设计**：helper / util / 抽象层最小化；宁可局部直白，不要全局优雅。
- 若新增压缩 HSM 表的宏，必须保持"表即数据"（只拼表项，不嵌控制流），用后 `#undef`。
- 纠正：coact 的 `COACT_HSM_STATES` / `COACT_HSM_TRANS` 宏压缩表在 newosp 不存在；newosp 的 `TableHsm` 使用调用方提供的 `StateDef[]` / `TransitionDef[]` 数组，无宏表压缩先例。

## 6. 设计模式使用边界

四个允许的模式，各自有明确的准入条件与红线。通用红线：**每个模式引入前必须能指出"三个相似实例"或等价的复用证据**；临界情况需注释说明。

### 6.1 CRTP（骨架 + 钩子）

- **准入**：一个骨架类承载固定生命周期 / 流程，各派生类只提供少量钩子；需要编译期分发、拒绝 vtable。
- **红线**：钩子数量失控（>7 个）说明骨架在猜未来，退回普通函数组合；CRTP 基类不得持有 per-instance 状态（静态钩子风格时）。
- **落点**：`EventLoop<Derived, MaxFds, MaxTimers>`——派生实现 `OnFd` / `OnTimer` / `OnWake` 钩子，`static_cast<Derived*>(this)` 编译期分发，无函数指针擦除、无 vtable（event_loop.hpp）；`NodeManager : public EventLoop<NodeManager<MaxNodes>, ...>`（node_manager.hpp）。

### 6.2 策略（Policy，算法族参数化）

- **准入**：同一算法骨架 × 可替换的无状态算法，策略是**只有静态方法 / 无状态方法**的 struct——定制点风格。
- **红线**：策略禁止携带状态（有状态策略改用 CRTP 或独立类）；策略方法必须 `noexcept`（可 `constexpr`）。
- **落点**：`ThreadT<Ops>` 的 `PosixThreadOps` / `RtThreadOps` / `Win32ThreadOps`（thread.hpp）；`SleepStrategy` 的 `YieldSleepStrategy` / `PreciseSleepStrategy` + `StaticExecutor<PayloadVariant, SleepStrategy>`（executor.hpp）；`IoPoller` 后端策略（io_poller.hpp）。
- 纠正：coact 的 `RttSingleCoreProfile` / `HostSmpProfile` / `policy.hpp` 在 newosp 不存在。

### 6.3 命令（延迟执行 / 顺序契约）

- **准入**：(a) 操作需要携带自身身份 / 参数延迟投递执行；或 (b) 操作序列的**顺序本身是契约**，必须以数据表形式固化可审。
- **红线**：命令对象必须自包含（自带 tag / 参数，不依赖调用点上下文）；禁止把命令表当成变相 if 链。
- **落点**：`MessageEnvelope<PayloadVariant>` 自带 header（sender / priority / timestamp）延迟投递（bus.hpp）；`Pipeline<MaxStages, MaxEdges>` + `StageConfig` / `JobHandler` 静态 DAG，初始化后运行期不可变（data_dispatcher.hpp）。

### 6.4 组合（固定集合传播）

- **准入**：需要向固定成员集合传播同一操作（广播 / 订阅 / init / deinit）。
- **红线**：**编译期固定成员**；成员集合运行期不可变；禁止未定界递归，受控定深递归须声明上界（见 2.2）。
- **落点**：`AsyncBus` 一对多订阅广播，`SubscriptionHandle` 标识订阅（bus.hpp）；`ServiceRegistry<MaxServices>`（service.hpp）；`FusedSubscription<PayloadVariant, MsgTypes...>` 编译期订阅集合 + `TimeSynchronizer`（data_fusion.hpp）。
- 纠正：coact 的 `AoRegistry` 在 newosp 不存在；`Pipeline::ExecuteStage` 是受控定深递归的例外，见 2.2。

### 6.5 模式选择决策表

| 场景特征 | 选 CRTP | 选策略 | 选命令 | 选组合 |
|---|---|---|---|---|
| 差异是**钩子方法集合**（流程同、多处不同行为） | 是（`EventLoop`） | — | — | — |
| 差异是**一个无状态算法 / 后端** | — | 是（`ThreadT<Ops>`、`SleepStrategy`） | — | — |
| 需要**延迟到别的线程 / 状态执行** | — | — | 是（`MessageEnvelope`） | — |
| **顺序本身是契约**、要以数据表审计 | — | — | 是（`Pipeline` / `StageConfig`） | — |
| 需要向**固定集合**广播 / 传播 | — | — | — | 是（`AsyncBus` 订阅） |
| 需要 vtable 运行期多态 | 否——用 CRTP | 否——用 Policy | — | — |
| 需要"is-a"类层次继承 | 否——组合 + 钩子代替 | — | — | — |

两条总序：**静态多态（CRTP/Policy）优先于运行时多态（vtable）；组合优先于继承**——继承只出现在 CRTP 的"骨架 + 钩子"形态里，且基类不持有派生专属状态。

无法对号入座时：先写两个直接的普通函数 / struct，等第三个相似实例出现再回到本表。

## 7. 风格与注释

- **newosp 的风格以其 `.clang-format` 与 `docs/coding_standards_zh.md` 为准（Google 基线）：2 空格缩进、Attach 花括号、120 列、指针左对齐、命名空间不缩进、include 排序（主头文件 > 项目头文件 > C 封装 > C++ 标准库）**。
  - 注意：coact 原文档的 **Allman 花括号 / 4 空格不适用于 newosp**，不得据以评审。这是本文与 coact 版差异最大的一处。
- 注释英文，`/* */` 块注释与 `//` 行注释皆可；文件头 `@file` / `@brief` 一句话定位 + MIT 许可证头（newosp 多数头为完整 MIT 文本，部分用 `SPDX-License-Identifier: MIT`）。
- **决策注释义务**：反直觉的选择（不加锁、丢弃语义、单槽深度、锁外执行、`block_id` 引用计数）必须在代码处写明"为什么"，且注释要能被下一个人单独读懂。
  - 落点：`NodeManager` 的 "send outside it so a blocked TCP send does not hold mutex_"；`EventLoop` 的 "Hooks run outside the mutex"；`SpscRingbuffer` 的单生产者 / 单消费者契约（spsc_ringbuffer.hpp 头注释）。
- 命名（`docs/coding_standards_zh.md`，与 coact 版基本一致，补充 newosp 细节）：
  - 类型 / 类 / 公有函数 `PascalCase`（`AsyncBus`、`Publish`）；
  - 变量 / 成员 `snake_case`，成员常带尾下划线（`sender_id_`）；
  - 常量 / 枚举值 `kPascalCase`（`kCacheLineSize`、`kSuccess`）；
  - HSM 事件枚举 `k<Module>Evt<Name>`（`kSvcEvtStart`、`kDiscEvtNodeFound`）；
  - 宏 `OSP_UPPER_CASE`（`OSP_LOG_INFO`、`OSP_ASSERT`）；
  - 模板参数 `PascalCase`（`PayloadVariant`、`BufferSize`）。
- **RAII 装饰器**：成对操作（进入 / 退出必须同时发生）包成 guard 对象，拷贝 / 赋值 `= delete`；显式 `Start/Stop`、`Init/Deinit` 生命周期用于跨事件边界的长寿命资源（配对语义写头注释，见 3.3 drain-on-stop）。
  - 落点：`ScopeGuard`（拷贝 `= delete`，vocabulary.hpp）；`WatchdogGuard`（watchdog.hpp）；`DirGuard` / `PipeGuard`（process.hpp）。

### 7.1 错误处理：`expected` 与错误码

- **异常核心策略**：核心 / 热路径禁用异常逃逸（以 `-fno-exceptions` 可编译为准）；错误用值语义返回。第三方解析器（toml / inicpp）可能内部抛异常，**必须在 `config.hpp` 边界 `try/catch` 转为 `ConfigError`**，异常不得逃逸到框架 / 业务接口。
  - 落点：`config.hpp` 的 `try / catch (const std::exception&)` → `expected<void, ConfigError>::error(...)`。
- 简单场景：bool / 错误码枚举（`ConfigError`（vocabulary.hpp）、`TimerError`、`SemaphoreError`、`WorkerPoolError`、`JobPoolError`、`NodeError`、`ServiceError` 等）。
- 值或错误二选一：`expected<V, E>`（小写，include/osp/vocabulary.hpp）——`success() / error()` 工厂、`expected<void,E>` 特化、支持 move-only `V`、固定内联存储、零堆、兼容 `-fno-exceptions`。
  - 落点：`expected<void, LoopError>` / `expected<uint32_t, LoopError>`（event_loop.hpp）；`expected<RegResult, WatchdogError>`（watchdog.hpp）；`expected<void, ConfigError>`（config.hpp 的 `Config<Backends...>`）。
  - 纠正：coact 的 `Expected` / `expected.hpp` / 整类 `[[nodiscard]]` 不适用于 newosp。
- 错误路径不得静默：返回值被消费或被计数（丢包计数 `messages_dropped`、背压级别、fault 计数），无"丢弃返回值且无注释"的调用点。

### 7.2 平台 / C 接口边界

以下条目**只适用于**平台边界头（`platform.hpp`、`thread.hpp`）及直接对接 RT-Thread / OS C API 的代码，不进入业务 / 协议其余部分：

- RT-Thread C API 隔离在 `platform.hpp`（`OSP_ASSERT` 用 `rt_kprintf`）与 `thread.hpp`（`RtThreadOps` 用 `rt_malloc` / `rt_free`）；`rt_malloc` 必须配对 `rt_free` 且释放后置空 / 防重复释放。
- `rt_kprintf` 仅允许 `%d %u %x %s %lu`（禁 `%llu / %zu / %f`），`size_t` 显式转 `(unsigned long)`。
- 文件 I/O 首选 POSIX `open / read / close`（config.hpp 解析路径即此）；`std::fopen` 存量见 config.hpp，不作为新增代码范式；格式化用 `snprintf`（禁 `sprintf`；头文件现无 `sprintf`）。
- host 侧示例可用 `std::printf`；面向 RT-Thread 打印通道时按上一条约束。
  - 纠正：coact 的 `pal_rtthread.hpp` 与 `diag/log_rtthread.hpp`（coact 侧路径）在 newosp 均不存在。

### 7.3 其他

- **自验证**：示例程序结尾必须断言全部不变量并以退出码给出结论（可被 CI 门控）。
  - 落点：`examples/hsm_protocol_demo.cpp`、`examples/node_manager_hsm_demo.cpp`、`examples/hsm_bt_combo_demo.cpp` 均输出 `RESULT: PASS/FAIL` 并返回退出码。

## 8. 检查清单（review checklist）

逐项打勾；任一"否"即 review 不通过。标注「无对应」的条目为保留的通用规则，newosp 无该机制但有等价物时按等价物判定。

### 总则

1. [ ] 仅使用 C++17 特性，未引入 C++20 语法 / 库？
2. [ ] 无平台 `#ifdef` 分支出现在平台边界头之外（业务 / 协议代码无平台分支）？
3. [ ] 热路径零堆分配（无 `new` / `malloc`）；存储为定容 / 内联 / 调用方持有（冷路径解析层可合理使用标准容器）？
4. [ ] 代码与注释为英文，无中文混入？
5. [ ] `rt_kprintf` / POSIX I/O / `rt_malloc` 等 C 接口只出现在平台边界（7.2 收口），未渗入业务代码？

### 类型与内存

6. [ ] 无裸 `int/long/char`；全部 `<cstdint>` 固定宽度整型？
7. [ ] 宽度 / 符号转换处均有显式 `static_cast`？
8. [ ] 新增 / 改动代码中比较表达式常量在左（`0 == x`、`nullptr == p`）？
9. [ ] 新枚举均为 `enum class` + 底层类型；无裸 `enum`，无新增业务 `#define` 常量（`opt.hpp` 既有调优宏除外）？
10. [ ] 单语句分支也带 `{}`；全文件无 `goto`、无未定界递归（受控定深递归须声明上界）？
11. [ ] `switch` 有 `default` 或穷举 + 兜底返回？
12. [ ] 无动态分配池 / 工厂模式；行为多态走模板（CRTP/Policy）或定容静态池（`FixedPool`/`ObjectPool`），确需擦除时用函数表并说明理由？
13. [ ] 跨边界结构体在定义处有 `is_standard_layout` / `is_trivially_copyable` / `is_trivially_destructible` 断言？
14. [ ] placement new 仅用于固定 / 复用存储；非平凡析构类型有显式配对的析构调用？
15. [ ] 原始存储声明为 `alignas(T) uint8_t[]` / `std::aligned_storage`（newosp 形态），无裸 `char` 双关，未虚称 `std::launder` 规约？
16. [ ] 跨边界连接用值语义 id（`NewType` / `SubscriptionHandle` / `ConnectionId` 等）或引用，非裸指针 + 所有权注释？
17. [ ] 成对操作已包成 RAII guard（拷贝 `= delete`），无裸 lock/unlock 配对调用？
18. [ ] 新增 / 改动的调用路径已核算"最深帧 × 单帧上限 ≤ 线程栈预算"，无大对象入栈？

### 线程与并发

19. [ ] 组件间仅经总线 / 事件队列通信，无共享可变状态跨线程裸访问？
20. [ ] 大块数据留在 owner / `DataDispatcher`，消息 / 事件只带 `block_id` / 描述符（零拷贝）？
21. [ ] 多锁场景已在注释声明获取顺序、无反向获取（newosp 无 coact 的 L1/L2/L3 命名层级）？
22. [ ] 每处"不加锁"的决定都有注释论证（最弱足够原则）？
23. [ ] 跨线程标志 / 枚举为 `std::atomic` 且断言 `is_always_lock_free`？
24. [ ] worker 交接为定容环 / 单槽、忙则拒绝 + 计数，无阻塞排队？
25. [ ] `stop()` 语义（drain vs 丢弃）已声明且被注释论证？
26. [ ] 关键区内无回调 / 系统调用 / 长操作（collect-release-execute）？

### 函数与控制流

27. [ ] 每个函数 return 数 ≤ 5？
28. [ ] guard 均为纯函数（只读、`noexcept`、无副作用）？
29. [ ] entry / exit / action 按 `StateConfig` / `StateDef` / `TransitionDef` 分层，进入动作未错放进 transition action？
30. [ ] 状态机为静态表驱动（`TableHsm` / `StateDef[]` / `TransitionDef[]`），非法 (状态, 事件) 有显式 reject 弧？
31. [ ] 事件 / 块只 submit 不手动回收；`DataDispatcher` 块经 `Release` 归零，零泄漏可验证？
32. [ ] 组件属性走模板参数 / 编译期常量；运行期观测走 `SystemMonitor` / `FaultCollector` / `Watchdog`，无散装打印探针？
33. [ ] 排空逻辑用事件驱动条件（drain / join、最近到期唤醒），无固定 sleep 赌时序？

### C++17 特性

34. [ ] `if constexpr` 只用于"未选分支不该被实例化"的场景？
35. [ ] 常量表为 `constexpr` / `inline constexpr`，且非为 constexpr 而 constexpr；无新增业务 `#define` 常量？
36. [ ] `[[nodiscard]]` / `noexcept` / `explicit` 按语义使用，未机械全标？
37. [ ] `std::exchange`（若使用）仅用于"取旧+置新"一体交接；弃返回值处写 `static_cast<void>`（newosp 核心暂无用例）？
38. [ ] 跨槽位 / 跨线程交接用 `T&&` + `std::move`；失败路径不消费调用者的值；move 后源不再读？

### 设计模式

39. [ ] 每个模式（CRTP/策略/命令/组合）的引入满足第 6 章准入条件，可指出三个相似实例或等价证据？
40. [ ] CRTP 钩子面 ≤ 7 个且基类未滥用状态？
41. [ ] 策略无状态、方法 `noexcept`？
42. [ ] 命令对象自包含；顺序契约以数据表固化而非 if 链？
43. [ ] 组合为编译期固定集合；遍历迭代化，受控递归已声明深度上界？
44. [ ] 静态多态优先于 vtable；组合优先于继承，未引入非必要类层次？

### 风格

45. [ ] 符合 newosp `.clang-format`（2 空格 / Attach / 120 列 / 指针左对齐）与第 7 章命名约定（非 coact 的 Allman / 4 空格）？
46. [ ] 反直觉决策处均有"为什么"注释？
47. [ ] 错误路径有消费或计数（`expected` / 错误码被处理），无静默丢弃返回值？
48. [ ] （示例程序）结尾自验证不变量并以退出码给出结论？

（完）
