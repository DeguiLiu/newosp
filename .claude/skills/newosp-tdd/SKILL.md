---
name: newosp-tdd
description: newosp 工程 TDD 工作流（RED-GREEN-REFACTOR）。铁律：没有先看到测试 RED 就不写实现代码。覆盖死锁/UAF/越界类并发缺陷的测试设计技巧。当修复 bug、加功能、重构本工程代码时使用。
---

# newosp TDD 工作流

## 铁律

```
NO PRODUCTION CODE WITHOUT A FAILING TEST FIRST
```
未先看到测试失败（RED），不写实现（GREEN）。测试必须失败在"正确的原因"（功能缺失/缺陷存在），而非语法错误。

## 循环

1. **RED**：写一个最小测试，精确描述期望行为。运行它，确认失败。
2. **GREEN**：写最小实现让它通过。不要添加测试之外的功能。
3. **REFACTOR**：清理重复/命名/结构，保持测试绿。

## 本工程测试技巧

### 死锁类缺陷（如回调内退订）
无法让 Catch2 超时。用独立线程 + 2 秒轮询 + `std::_Exit(1)`：
```cpp
std::atomic<bool> done{false};
std::thread consumer([&bus, &done]() { bus.ProcessBatch(); done.store(true, std::memory_order_release); });
auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
  std::this_thread::yield();
if (!done.load(std::memory_order_acquire)) std::_Exit(1);  // 死锁，不可恢复
consumer.join();
```
注意：lambda 按值捕获 handle 时 handle 尚未赋值（注册需要回调、回调需要 handle 的鸡生蛋问题），用指针间接填充。

### UAF / 越界缺陷（栈对象悬垂、数组越界）
普通构建可能不崩（UB 未定义）。用 ASan 验证：
- 栈 use-after-return：`ASAN_OPTIONS=detect_stack_use_after_return=1`
- 编译命令见 newosp-build skill 的 ASan 节

### 并发缺陷（数据竞争）
TSan 构建：`-fsanitize=thread`。注意 TSan 与 `fork()` 不兼容（Catch2 里 fork 测试需 `#if defined(__SANITIZE_THREAD__)` 跳过）。

## 快速验证

```bash
cd /home/dgliu/newosp
cmake --build build -j$(nproc) --target osp_tests && ./build/tests/osp_tests "<TestName>"
```

## 本工程已知缺陷模式（修复时重点验证）

- 异步边界：超时后消息仍被消费、回调内重入
- 锁内执行用户回调（collect-release-execute 违反）
- 生命周期：析构不退订、槽位复用 ABA、对象在途销毁
- 无锁：uint32 回绕、msg_len+4 溢出、跨进程原子无 static_assert
- NDEBUG 下 `OSP_ASSERT` 失效 → 运行时校验缺失
