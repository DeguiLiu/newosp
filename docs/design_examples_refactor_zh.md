# examples 现代设计模式重构计划

## 1. 结论与范围

本计划覆盖 6 个 examples 目录：`serial_ota`、`net_stress`、`data_dispatcher`、`streaming_protocol`、`shm_ipc`、`client_gateway`。

目标：用已落地的核心组件（`EventLoop`、`Breaker`）替换旧的轮询/回调写法，消除重复代码，把适合的场景改为表驱动/策略化。**不为模式而模式**，装饰器、命令模式中已判定不适用的场景不引入。

## 2. 已完成的核心改造（依赖前提）

| 组件 | 用途 | 状态 |
|---|---|---|
| `EventLoop<Derived>`（CRTP） | 统一事件循环：fd + 定时器 + 跨线程唤醒，超时=最近到期 | 已落地 |
| `TableHsm<Ctx, MaxStates, MaxTrans>` | 静态表 HSM（`StateDef[]`/`TransitionDef[]` + `guard` 条件转移） | 已落地，parser/ClientSm/FtSm/3 demo 迁移 |
| `Breaker<>` | 5 状态背压熔断，单 32 位原子 CAS | 已落地，接入 `fault_collector` |
| `detail::EventQueue<T, Depth>` | 无锁 MPSC 事件队列（sequence 号） | 已落地，HSM 类使用 |
| HSM 单线程化 | `HsmDiscovery`/`HsmService`/`HsmNodeManager` 事件入队 + Run 线程 Dispatch | 已落地 |

## 3. 重构原则

1. 不为了模式而模式：装饰器（transport 已组合）、命令模式替代 `FixedFunction` handler 的场景不适用。
2. 优先复用核心组件：examples 里手写的 `while + sleep` 轮询、定时器回调，凡能用 `EventLoop` 表达的，都改为事件循环。
3. 外科手术式修改：只改确有收益处，保持各 example 的演示意图不变。
4. 验证闭环：每个 example 改完可编译、可运行、行为与改前一致。
5. **多实例共享一个循环**：多实例 HSM（如 net_stress 的 `ClientSm x N`）禁止照搬「每实例一个 `EventLoop` 线程」，必须一个 Run 循环服务全部实例（对应 coact 的单 Dispatcher 服务所有 AO）。否则 N 个客户端 = N 个线程，违背单线程事件循环初衷。
6. **异步语义显式化**：凡把同步 API 改为「事件入队 + Run 线程 Dispatch」的，必须在类注释里写明「调用后状态未立即变化」，并同步检查调用方是否依赖同步语义。

## 4. 需要引入的设计模式

### 4.1 CRTP（骨架 + 钩子）

用途：处理器/状态机的编译期多态，替代虚函数与 `void*` 回调。

落点：serial_ota、net_stress 的处理器基类；`EventLoop<Derived>` 已是现成骨架。

红线：钩子方法不超过 7 个；基类不持有派生专属状态。

### 4.2 策略（Policy）

用途：同一算法骨架下可替换的无状态算法族，编译期选定。

落点：data_dispatcher 的 `StorePolicy`/`NotifyPolicy`（已有，保持）；协议常量与后端选择。

红线：策略禁止携带状态；方法 `noexcept`。

### 4.3 状态（静态 HSM 转移表）

用途：把「handler 返回 enum + if-else 解释」的运行时派发改为 `StateDef[]` + `TransitionDef[]` 静态表，编译期穷举。

落点：net_stress 的 `ClientSm`（7 状态）/`FtSm`（8 状态）、serial_ota 的 FrameParser，若存在 if-else 链则改。

红线：拒绝路径显式（非法 (状态, 事件) 冒泡到根落到 reject 弧），禁止在 action 里 if-else 模拟状态机；条件转移用 `TransitionDef.guard` 谓词在表行内表达（guard 在 action 前求值，读转移前状态），目标态固定。

### 4.4 命令（constexpr 表）

用途：把硬编码的顺序流程或消息路由 if-链改为 `constexpr` 命令表。

落点：client_gateway 的 5 个 phase、streaming_protocol 的消息路由（RegisterRequest/HeartbeatMsg/StreamCommand/StreamData → 各 Node）。

红线：命令对象自包含（自带参数）；禁止把命令表写成变相 if 链。

### 4.5 组合

用途：向固定成员集合传播操作，展平为固定数组循环。

落点：所有 example 的组件拼装（已有，保持）。

红线：编译期固定成员、禁递归。

## 5. 参考的规范

### 5.1 代码风格与命名：`docs/cpp_coding_conventions_zh.md` 第 7 章

newosp 仓库的统一编码规范，examples 重构严格遵循：

- 缩进 2 空格，行宽 120，Attach 花括号
- 指针左对齐（`int* ptr`），命名空间不缩进
- 命名：类/函数 PascalCase、变量 snake_case、常量 kPascalCase、宏 OSP_UPPER_CASE
- include 排序：主头文件 > 项目头文件 > C 封装 > C++ 标准库

### 5.2 类型纪律与设计模式：同上，第 2、4、5、6 章

作为类型纪律与设计模式准入条件的参考：

- 固定宽度整型（`<cstdint>`），禁裸 `int/long/char`
- Yoda 比较（常量在左）、单语句也带 `{}`、单函数 return 不超过 5
- 禁用异常、业务零堆、静态/栈存储优先
- 第 6 章设计模式准入条件（CRTP/策略/命令/组合的「何时用/红线」）
- `static_assert` 声明 ABI/布局契约

### 5.3 规范已合并

两份规范曾各自漂移：`coding_standards_zh.md` 为 2 空格 + Attach，而当时的 `cpp_coding_conventions_zh.md` 为 4 空格 + Allman。二者现已合并为 `docs/cpp_coding_conventions_zh.md` 一份，风格以该文第 7 章为准（`.ai/.clang-format`：2 空格 + Attach），examples 与仓库现有代码同此，不再存在两套风格。

## 6. 六个目录的改造点

### 6.1 serial_ota

1. 抽取 host/device 重复的 FrameParser 为共享模板（若两处逐字重复）。
2. `TimerScheduler` 进度/超时定时器 → `EventLoop::Schedule` + `OnTimer`。
3. 帧解析状态机若为 enum + if-else，改静态转移表（4.3）。

### 6.2 net_stress

1. `ClientSm`/`FtSm` 的 HSM 转移逻辑改静态表（4.3）。
2. 服务端背压接入 `Breaker`，过载降级而非崩溃。
3. echo/stats/watchdog 定时器 → `EventLoop` 统一调度。

### 6.3 data_dispatcher

1. `StorePolicy` 已是策略模式，保持不变。
2. 陈旧块回收（ScanTimeout）定时轮询 → `EventLoop` 定时器。
3. 确认 `BreakerDropsNonCritical` 在 demo 中被消费。

### 6.4 streaming_protocol

1. 消息路由 if-链 → constexpr 命令表（4.4）。
2. `TimerScheduler` 心跳 → `EventLoop` 定时器。

### 6.5 shm_ipc

1. `ShmRingBuffer` 无锁 SPSC 保持不变。
2. producer/consumer 的 `sleep` 忙等 → `LightSemaphore`/事件驱动。

### 6.6 client_gateway

1. 5 个 phase 的硬编码顺序 → constexpr 命令表（4.4）。
2. `WorkerPool` 完成等待若为 `sleep` 轮询，改事件/计数驱动。

## 7. 执行顺序与验证

执行顺序（改动最小、收益最明确优先）：

1. `client_gateway`：phase 表驱动（命令模式，纯重构）。
2. `streaming_protocol`：消息路由表 + EventLoop 定时器。
3. `shm_ipc`：轮询改事件驱动。
4. `serial_ota`：抽取重复 FrameParser + EventLoop 定时器。
5. `net_stress`：HSM 静态表 + Breaker 背压。
6. `data_dispatcher`：EventLoop 定时器 + 确认 Breaker 消费。

验证：

- 每个 example 编译通过（`cmake --build build --target <example>`），运行日志与改前对比。
- 涉及 HSM 静态表的对应单元测试全部通过。
- `git diff --check` 无空白错误。

## 8. 工具约束

- `minimax-worker` 仅只读，可用于并行调研各 example 的重复代码与 if-else 链（产出改造清单），不用于写代码。
- 代码修改由主 agent 顺序执行，「并行」体现在调研阶段。
