# examples 现代设计模式重构 — 执行计划

本文件是 `docs/design_examples_refactor_zh.md`（设计文档）的执行版，吸收并行调研结论与三项决策，分阶段落地。

## 1. 决策汇总

| 决策点 | 结论 | 影响 |
|---|---|---|
| §4.3 静态 HSM 表（StateDef[]/TransitionDef[]） | **先补框架再改** | 新增核心组件 `osp::TableHsm`；迁移 parser/ClientSm/FtSm |
| EventLoop 改造力度 | **仅改真实轮询点**（`while+sleep` 主循环 / 独立 TimerScheduler 线程） | net_stress 3 文件 + serial_ota main loop；data_dispatcher ScanTimeout、streaming_protocol 心跳保留 |
| data_dispatcher Breaker 接入 | **新增 Breaker 接入** | 把 `backpressure_threshold` 触发接 `Breaker::OnWatermarkViolation`，L2/Safe 拒非关键路径 |

## 2. 最终范围

### 跳过（假设不成立）

- `serial_ota` §6.1.1 `FrameParser` 去重 — 单类实例化两次无重复。
- `streaming_protocol` §6.4.1 消息路由命令表 — 已是 `StaticNode`+`std::visit` 编译期派发无 if 链。
- `shm_ipc` §6.5.2 `sleep` → `LightSemaphore` — sleep 为节流/backoff；`LightSemaphore` 是进程内 `sem_t` 不可跨进程。

### 实施清单

| # | 项目 | 性质 | 依赖 |
|---|---|---|---|
| F1 | 新增 `osp::TableHsm<Context, MaxStates, MaxTransitions>`（StateDef[]/TransitionDef[]/TransitionKind） | 新核心组件 | — |
| M1 | `serial_ota/parser.hpp` 9 状态平坦 → TableHsm | 迁移 | F1 |
| M2 | `net_stress/client_sm.hpp` 7 状态（Root+Connected+Idle/Running）→ TableHsm | 迁移 | F1 |
| M3 | `net_stress/file_transfer.hpp` 8 状态（Root+Transferring+Sending/WaitingAck/Retrying + Complete/Failed）→ TableHsm | 迁移 | F1 |
| E1 | `net_stress/client.cpp` 3 timer + `while+sleep` 主循环 → `EventLoop<ClientLoop>` | 改造 | — |
| E2 | `net_stress/server.cpp` 1 timer + 主循环 → `EventLoop<ServerLoop>` | 改造 | — |
| E3 | `net_stress/monitor.cpp` 1 timer + 主循环 → `EventLoop<MonitorLoop>` | 改造 | — |
| E4 | `serial_ota/main.cpp` TimerScheduler 后台线程 + `while(g_ota_running)` 主循环 → `EventLoop<OtaLoop>` | 改造 | — |
| B1 | `net_stress/server.cpp` 3 handler（Handshake/Echo/FileTransfer）加 Breaker 闸门 | 改造 | — |
| G1 | `client_gateway/main.cpp` 5 phase 硬编码顺序 → `constexpr` 命令表 + Ctx | 改造 | — |
| G2 | `include/osp/worker_pool.hpp` `FlushAndPause` 内部 `ThreadSleepUs(100)` 忙等 → 计数驱动（`outstanding_ = dispatched_ - processed_`） | 核心改造 | — |
| D1 | `data_dispatcher` Breaker 接入：把 `backpressure_threshold` 触发送 `Breaker::OnWatermarkViolation`；L2/Safe 时拒非关键路径 | 改造 | — |

## 3. 框架设计：`osp::TableHsm`

### 3.1 API

新增 `include/osp/hsm_table.hpp`，与 `include/osp/hsm.hpp` 的 `StateMachine` 并存（按 coact 迁移策略），不破坏现有 `HsmDiscovery`/`HsmService`/`HsmNodeManager`。

```cpp
namespace osp {

enum class TransitionKind : uint8_t {
  kExternal,  // 沿 LCA 退出源、进入目标
  kInternal,  // 不 exit/entry，跑 action，停留在当前状态
  kSelf       // exit + 重新 entry
};

template <typename Context>
struct StateDef {
  const char* name;
  int32_t parent;                // -1 = root
  void (*on_entry)(Context&);
  void (*on_exit)(Context&);
};

template <typename Context>
struct TransitionDef {
  int32_t from;                  // 源状态
  uint32_t event;                // 事件 ID
  int32_t to;                    // 目标状态
  TransitionKind kind;
  void (*action)(Context& ctx, const void* data);  // 副作用，可为 null
  bool (*guard)(Context& ctx, const void* data);   // 条件转移谓词，可为 null
};

template <typename Context, uint32_t MaxStates = 16, uint32_t MaxTransitions = 64>
class TableHsm {
 public:
  TableHsm(Context& ctx, const StateDef<Context>* states, uint32_t state_count,
           const TransitionDef<Context>* transitions, uint32_t trans_count) noexcept;
  void SetInitialState(int32_t state_index) noexcept;
  void Start() noexcept;
  void Dispatch(const Event& event) noexcept;
  bool ForceTransition(int32_t target) noexcept;
  int32_t CurrentState() const noexcept;
  const char* CurrentStateName() const noexcept;
  bool IsInState(int32_t) const noexcept;
 private:
  // 复用 StateMachine 的 LCA/entry/exit 算法思路（depth 归一化 + 同步上溯）
};

}
```

### 3.2 派发语义

1. 给定 `(current_state, event.id)`，线性扫描 `TransitionDef[]` 找匹配。
2. 命中条件：`from==state && event==event.id && (guard==nullptr || guard(ctx,data)==true)`；同 `(from, event)` 多行按序扫描，首个 guard 通过或无 guard 者胜出（guarded 行排前，无 guard 兜底排后）。
3. 命中后先跑 `action(ctx, event.data)`（副作用），再按 `kind` 执行转移（kInternal 停留；kSelf exit+re-entry；kExternal LCA 路径）。guard 在 action **之前**求值，读转移前状态。
4. 未命中：冒泡到父状态重试；冒泡到根仍未命中 → 拒绝弧（停留、无副作用、无转移）。
5. action 是副作用函数，**禁止在 action 内做转移决策**（§4.3 红线）；条件转移用 guard 谓词表达，目标态在表行内固定。guard 读转移前状态，若判定依赖「即将写入的字节」需用 `+1` 补偿（如 `payload_index + 1U >= expected_len`）。

### 3.3 约束

- header-only、C++17、`-fno-exceptions -fno-rtti` 兼容。
- `const` 指针传入表（表本身在调用方为 `constexpr`/`const`），实例不拷贝表。
- `MaxTransitions` 编译期上限；线性扫描在小规模（< 64）下不输哈希，零额外依赖。
- 不引入动态分配、虚函数、std::function。

## 4. 迁移设计要点

### 4.1 parser.hpp（M1）

平坦 9 状态。`TransitionDef` 表按现有 handler 一对一映射：
- `(state, kEvtByte)` → `action=记录/统计副作用, to=next_or_self, kind=kExternal/kInternal`
- `(state, kEvtReset|kEvtTimeout)` → `to=Idle, kind=kExternal`（统一规整现有 9 处 reset 分支）

条件转移用 guard 谓词表达（目标态在表行内固定）：header 判定（`GuardIsHeader`）、长度合法（`GuardLenValid`）、有无 payload（`GuardHasPayload`）、payload 满（`GuardPayloadDone`，`+1` 补偿）、tail/CRC 判定（`GuardNotTail`/`GuardTailBadCrc`/`GuardCrcOk`）。非 header 字节走 `kInternal` 自环 + `ActSyncError`（等价原文「停留 idle」）。增量 CRC 用 `Crc16Update` 逐字节折叠，避免整帧重算。

### 4.2 client_sm.hpp / file_transfer.hpp（M2/M3）

层次结构（Root + 子状态）。`StateDef[].parent` 表达；`TransitionDef` 表平铺所有 `(state, event)`。冒泡到父状态由 TableHsm 内置提供。`Running`/`Sending` 的 RPC 副作用放入对应 transition 的 `action`；`kInternal` 表达"停留 + 跑 action"。

FtSm 的 `WaitingAck` 空壳状态删除（worker 调研建议，与"7/8 状态"规模说法一致，去除保留 future async 噪声）。

### 4.3 EventLoop 改造（E1–E4）

参考 `HsmService : public EventLoop<HsmService<MaxClients>, 1, 1>`（`service_hsm.hpp:222`）模式：
- 新建 `class ClientLoop : public EventLoop<ClientLoop>` 局部骨架
- `OnTimer(uint32_t id)` 内 `switch(id)` 分派到原 `TickCallback`/`StatsCallback`/`ErrorRateCheckCallback` 内部逻辑
- 主循环 `loop.Run()` 替代 `while (g_running) sleep_for(100ms)`
- EventLoop 受 `#if OSP_HAS_NETWORK` 守护，无网络平台保留原 TimerScheduler（按目标平台 CMake 配置确认）

### 4.4 Breaker 闸门（B1）

`server.cpp` 3 个 handler 入口加 `breaker_.DirectAllowed()` / `DropNonCritical()` 闸门。`Service::Handler` 签名 `Response(*)(const Request&, void* ctx)` 函数指针限制 → handler 内部 `static` 命名空间持 `Breaker<100>` 实例，通过 `osp::Service::GetHandlerCtx()` 关联。`OnDispatchCycle` 每 N request 调一次；`OnDirectTimeout` 由 handler 内 2s 超时计；`OnWatermarkViolation` 由 `active_workers_/max_concurrent` 比值算百分比。短路响应借 `accepted=0` 字段上报。

### 4.5 client_gateway 5 phase 命令表（G1）

```cpp
struct PhaseCmd {
  void (*run)(GatewayCtx&);   // 编译期函数指针
  uint32_t count;              // 重复次数或 sentinel
};
struct GatewayCtx { osp::Node<Payload>& gateway; osp::WorkerPool<Payload>& pool; ... };

static constexpr PhaseCmd kPhases[] = {
  { &PublishConn, kNumClients },
  { &SubmitDataBatch, kNumClients * kMsgsPerClient },
  { &PublishHb, kNumClients },
  { &FlushAndEmitResult, 0 },  // Phase 4 复合
  { &PublishDisc, kNumClients },
};
for (const auto& p : kPhases) for (uint32_t i = 0; i < max(1u, p.count); ++i) p.run(ctx);
```

`GatewayCtx` 在运行时填入 `gateway&/pool&` 引用；`kPhases` 是 `constexpr`（函数指针 + count 编译期可知）。phase 间 `sleep_for(50ms)` 留作现状（worker §6.6.2 建议"不纳入"，边界外）。

### 4.6 FlushAndPause 计数驱动（G2）

`include/osp/worker_pool.hpp`：新增 `std::atomic<uint64_t> outstanding_`；worker 每完成一条 `fetch_sub`；`dispatched_` 入队时 `fetch_add`。`FlushAndPause` 改为 `while (outstanding_.load() != 0) { if (timeout) break; osp::ThreadSleepUs(100); }`（保留 100µs 作为最低频唤醒，不再每 tick 检查 bus depth + 每 worker queue，复杂度从 O(bus+workers) 降到 O(1)）。`examples/client_gateway/main.cpp:184` 是唯一 example 调用点；影响面仅 `WorkerPool::FlushAndPause` 行为契约（唤醒延迟从 tick 周期变为"完成即信号"）。

### 4.7 data_dispatcher Breaker 接入（D1）

`examples/data_dispatcher` 当前 0 处 Breaker 引用。新增：
- `launcher.cpp` / `producer.cpp` / `pipeline_demo.cpp` 持有 `osp::Breaker<100>` 实例。
- `backpressure_threshold` 触发时 `breaker_.OnWatermarkViolation()`；`cfg` 提供 `non_critical_mask` 字段标记非关键消息。
- consumer 入口查 `breaker_.DropNonCritical()` 决定是否拒入。
- 不改 `DataDispatcher<StorePolicy>` 模板签名（避免影响 net_stress 同步路径）；Breaker 持有在 demo 层。

## 5. 执行顺序与验证

按依赖排序，每个阶段独立编译 + 运行验证：

| 阶段 | 内容 | 验证项 | 状态 |
|---|---|---|---|
| **P1 F1** | 新增 `include/osp/hsm_table.hpp` + `tests/test_hsm_table.cpp`（TDD: RED→GREEN），加 `tests/CMakeLists.txt` | `cmake --build build --target test_hsm_table` 通过；覆盖：平坦/层次转移、LCA、内部/外部/自转移、动作、拒绝弧 | 已完成 |
| **P2 M1** | `serial_ota/parser.hpp` 迁移到 TableHsm | `cmake --build build --target osp_serial_ota_demo`；运行 demo 对比改前 stats（`frames_received/sync_errors/crc_errors/tail_errors/length_errors`） | 已完成 |
| **P3 M2** | `net_stress/client_sm.hpp` 迁移 | `cmake --build build --target osp_net_stress_client`；运行 launcher 跑通 handshake→echo→disconnect 序列，stats 不退化 | 已完成 |
| **P4 M3** | `net_stress/file_transfer.hpp` 迁移（含删 `WaitingAck`） | 同一 launcher，文件传输 `chunks_ok/chunks_retried` 与改前一致 | 已完成 |
| **P5 E1–E3** | net_stress client/server/monitor EventLoop 化 | 3 target 编译；运行 launcher 全流程；`while+sleep` 主循环消除 | 已完成 |
| **P6 E4** | serial_ota main loop EventLoop 化 + ISR 中断模拟（`uart_isr.hpp`） | `osp_serial_ota_demo` 编译；运行；FW/Flash CRC 匹配，35 帧 | 已完成 |
| **P7 B1** | net_stress server Breaker 闸门 | `osp_net_stress_server` 编译；模拟过载场景验证降级路径 | 未开始 |
| **P8 G1+G2** | client_gateway 5 phase 命令表 + `FlushAndPause` 计数驱动 | `osp_client_demo` 编译；运行；`pool: processed == dispatched`；`git diff --check` | 未开始 |
| **P9 D1** | data_dispatcher Breaker 接入 | `osp_dd_*` 6 个 target 编译；运行；过载场景下非关键消息被拒 | 未开始 |

计划外新增（本轮追加）：

| 项目 | 内容 | 状态 |
|---|---|---|
| D2 | 3 个 HSM demo 迁移 TableHsm + EventLoop + 自校验（`node_manager_hsm_demo` / `hsm_protocol_demo` / `hsm_bt_combo_demo`），仿 libev 自校验 demo（`RESULT: PASS/FAIL` + exit code） | 已完成 |
| D3 | 核心代码资源泄露/竞争/优雅释放/健壮性评估（10 文件全覆盖） | 已完成 |

P1 完成后向用户报告框架测试覆盖与 API，再继续 P2。P2–P4 串行（同属 HSM 迁移，可一次性 review 三个迁移）。P5–P6 EventLoop 改造完成后向用户报告。P7–P9 完成后向用户报告。

## 6. 已知风险与决策点

1. **parser 迁移功能等价（已闭环）**：`HandleIdle` 非 header 字节原文停留 idle；表驱动用 `kInternal` 自环 + `ActSyncError`（不跳 LenLo），与原文「停留 idle」严格等价，非 header 字节判定用 `GuardIsHeader` 守卫。已运行 demo 验证 stats 一致。
2. **FtSm `WaitingAck` 删除**：原文就是 `kUnhandled` 空壳，删除不改变行为；与"7/8 状态"规模说法一致（迁后 7 状态）。
3. **EventLoop 在 `OSP_HAS_NETWORK=0` 平台**：保留原 TimerScheduler 主循环作为 fallback；改造仅在 `OSP_HAS_NETWORK=1` 编译路径生效。
4. **B1 Breaker 持有方式**：`Service::Handler` 函数指针 + `void* ctx` 限制下用 static 实例；为最小侵入接受此限制。
5. **G2 `WorkerPool::FlushAndPause` 行为变化**：从 100µs 周期 tick 改为 outstanding==0 立即返回；调用方需无 timeout bug（已有 `kFlushTimeout` 路径不变）。
6. **D1 Breaker 持有在 demo 层**：不改 `DataDispatcher` 模板签名，net_stress Breaker 路径独立。后续若需 DataDispatcher 内置，可迭代到框架层。

## 7. 不在本计划范围

- 装饰器模式在 transport 的进一步组合（设计文档 §3 已判定 transport 已组合）。
- `coact` 静态 HSM 完整语义（guard、条件表、区域状态）。
- 新平台 / 工具链支持。
