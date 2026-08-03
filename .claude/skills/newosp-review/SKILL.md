---
name: newosp-review
description: newosp 工程嵌入式中级代码审查清单。覆盖无锁并发、跨进程共享内存、生命周期、NDEBUG 运行时校验、回调执行上下文。当审查本工程改动、评估某模块实现质量时使用。
---

# newosp 代码审查清单

纯头文件 C++17 嵌入式库。审查重点是"边界"：异步边界、生命周期边界、编译期宏边界。

## 无锁并发（bus/spsc/shm）

- [ ] CAS 算法：序列号/位置回绕（uint32 取模）、队列满判定与 slot 占用等价性
- [ ] acquire/release 配对完整（ARM 弱内存序），无遗漏的 relaxed
- [ ] 跨进程共享内存：`std::atomic` 有 `is_always_lock_free` + `is_standard_layout` 静态断言；无裸 `reinterpret_cast` 于未构造对象
- [ ] FakeTSO 开关是否被绑定到单核/x86（多核 ARM 上 relaxed = UB）
- [ ] 锁自由引用计数/空闲链表是否防 ABA（代际计数）
- [ ] 消息长度字段来自不可信源时，`len + N` 溢出守卫（用 `msg_len > max` 而非 `msg_len + 4 > max`）

## 回调执行上下文（collect-release-execute）

- [ ] 用户回调是否在锁内执行？若是 → 回调内调 API 死锁（SharedSpinLock 升级/重入）
- [ ] 回调在消费者线程执行时：阻塞会拖垮其他优先级；回调内调 `Stop()` 会自 join 死锁
- [ ] 回调内调用本模块 API 是否被文档禁止/有锁保护

## 生命周期与 RAII

- [ ] 析构时是否主动退订/停线程（漏退订 → 回调捕获 this 的 UAF）
- [ ] 槽位/句柄复用是否有代际（旧指针喂活新槽 = 假阴性）
- [ ] 超时/异常路径是否留下悬垂指针（栈对象越过异步边界 = stack-use-after-return）
- [ ] 组件在 kShutdown/Stop 后是否可重启
- [ ] 同步等待超时后，队列中待消费消息对已失效状态的引用

## NDEBUG 下的运行时校验

- [ ] `OSP_ASSERT` 被 NDEBUG 编译掉后，关键路径是否有真正的运行时校验（如越界、空指针）
- [ ] `-fno-exceptions` 下错误处理是否退化为未初始化返回/`terminate`

## 类型与内存安全

- [ ] 固定宽度整数（禁裸 int/long）；`size_t` 转打印用 `(unsigned long)`
- [ ] 热路径零堆分配；堆分配仅限冷路径且判空
- [ ] 缓冲区边界：memcpy 长度来自配置/网络时校验
- [ ] 信号处理器内仅 async-signal-safe 函数

## 文档与实现一致性

- [ ] 头文件注释宣称的能力（"线程安全""零堆""多实例"）与实现一致
- [ ] 模板参数/宏默认值与文档资源预算一致

## 本工程已确认缺陷模式（新代码应避免）

1. **bus.hpp** 原 SharedSpinLock 不可重入 → 回调内退订自死锁（已修复：递归读锁+写锁升级）
2. **post.hpp** 栈 ResponseChannel 超时后消息仍引用 → stack-use-after-return（已修复：引用计数+堆分配）
3. **serial_transport** ACK 帧(8B) 被 header(10B) 门控 → 吞下一帧 sync（已修复：ACK 判定提前）
4. **hsm.hpp** RequestTransition 无边界校验 → NDEBUG OOB（已修复：非法 target 返回 kUnhandled）
5. **executor.hpp** 四类 AddNode/nodes_ 是死代码，Spin 只调 `Instance().ProcessBatch()`
6. **worker_pool.hpp** 全满静默丢任务 + FlushAndPause 误报成功
7. **data_fusion.hpp** FusedSubscription 析构不退订 → UAF
8. **watchdog.hpp** 槽位复用旧 ThreadHeartbeat* 污染
