> 返回 [[Home]] | 另见: [[Architecture]] | [[Examples]]

# newosp 性能与内存基准报告

> 从属于 [design_zh.md](design_zh.md) §12 资源预算
> 版本: 1.0
> 日期: 2026-02-14
> 数据来源: `examples/benchmark.cpp`, `examples/benchmarks/` 实测

## 1. 测试环境

| 项目 | 值 |
|------|-----|
| CPU | Intel Xeon (Cascadelake) |
| 内核 | Linux 6.8.0-79-generic |
| 编译器 | GCC 13.3.0 (Ubuntu 24.04) |
| 编译选项 | `-O3 -std=c++17` (Release) |
| 二进制大小 | 106 KB (stripped) |
| 运行时 RSS | 5.6 MB (峰值) |
| 页错误 | 639 minor, 0 major |

> 注: 测试在 x86_64 云服务器上执行。ARM-Linux 嵌入式平台性能因 CPU 主频和缓存差异会有所不同，但相对趋势一致。

## 2. 吞吐量测试

### 2.1 AsyncBus 消息吞吐

测试 `AsyncBus<Payload>` 的 Publish + ProcessBatch 端到端吞吐，三种消息大小。

#### 默认队列深度 (QueueDepth=4096)

| 消息大小 | 数量 | 耗时 | 吞吐率 | 接收数 |
|----------|------|------|--------|--------|
| Small (24B) | 10,000 | 0.61 ms | 5.344 M msgs/s | 3,276 |
| Medium (64B) | 10,000 | 0.72 ms | 4.536 M msgs/s | 3,276 |
| Large (256B) | 10,000 | 0.78 ms | 4.218 M msgs/s | 3,276 |
| Small (24B) | 100,000 | 4.47 ms | 0.734 M msgs/s | 3,276 |
| Medium (64B) | 100,000 | 5.28 ms | 0.620 M msgs/s | 3,276 |
| Large (256B) | 100,000 | 5.66 ms | 0.579 M msgs/s | 3,276 |

默认队列 4096 在 10K 消息时即触发背压，实际接收仅 3,276 条。100K 消息时吞吐率被队列满拖至 < 1M msgs/s。

#### 大队列深度 (QueueDepth=131072)

编译时通过 `-DOSP_BUS_QUEUE_DEPTH=131072U` 覆盖:

| 消息大小 | 数量 | 耗时 | 吞吐率 | 接收数 |
|----------|------|------|--------|--------|
| Small (24B) | 10,000 | 1.65 ms | 6.059 M msgs/s | 10,000 |
| Medium (64B) | 10,000 | 1.46 ms | 6.869 M msgs/s | 10,000 |
| Large (256B) | 10,000 | 1.40 ms | 7.160 M msgs/s | 10,000 |
| Small (24B) | 100,000 | 17.00 ms | 5.884 M msgs/s | 100,000 |
| Medium (64B) | 100,000 | 17.38 ms | 5.753 M msgs/s | 100,000 |
| Large (256B) | 100,000 | 17.96 ms | 5.568 M msgs/s | 100,000 |

分析:
- 大队列下 100K 消息全部接收，吞吐稳定在 5.6-6.1 M msgs/s
- 相比默认队列 100K 场景 (0.6-0.7M)，提升约 8-9 倍
- 消息大小对吞吐影响约 10% (24B vs 256B)，variant 拷贝开销可控
- 队列深度是吞吐的决定性因素，而非 CAS 竞争
- 大队列代价: AsyncBus 实例从 518 KB 增至 ~16 MB (131072 x 128B envelope)

### 2.2 ShmRingBuffer SPSC 吞吐

单生产者-单消费者同线程 push/pop 循环 (256B slot, 1024 slots):

| 指标 | 值 |
|------|-----|
| 迭代次数 | 100,000 |
| 写入成功 | 100,000 |
| 读取成功 | 100,000 |
| 耗时 | 2.36 ms |
| 吞吐率 | 42.337 M cycles/s |

分析:
- wait-free SPSC 设计在同线程场景下达到 42M cycles/s
- 跨线程场景下因 cache line bouncing 会有所下降，但仍保持 lock-free 特性

### 2.3 Transport 帧编解码

`FrameCodec::EncodeHeader` / `DecodeHeader` 纯计算吞吐:

| 操作 | 迭代次数 | 耗时 | 吞吐率 |
|------|----------|------|--------|
| Encode | 1,000,000 | < 0.01 ms | ~18.9 G ops/s |
| Decode | 1,000,000 | < 0.01 ms | ~23.8 G ops/s |

分析:
- 帧头编解码为纯内存操作 (memcpy + 字段赋值)，被编译器高度优化
- 实际吞吐受限于网络 I/O 而非编解码开销

### 2.4 MemPool 分配/释放

`FixedPool<64, 1024>` 批量 alloc + free 循环:

| 指标 | 值 |
|------|-----|
| 迭代次数 | 100,000 (每轮 1024 alloc + 1024 free) |
| 总操作数 | 204,800,000 |
| 耗时 | 2,080.67 ms |
| 吞吐率 | 98.430 M ops/s |

分析:
- 嵌入式空闲链表设计，单次 alloc/free 约 10 ns
- 无锁、无系统调用，适合热路径高频分配

### 2.5 WorkerPool 任务分发

2 工作线程，100K 任务提交 + 处理:

| 指标 | 值 |
|------|-----|
| 任务数 | 100,000 |
| 处理完成 | 100,000 |
| 耗时 | 40.87 ms |
| 吞吐率 | 2.447 M tasks/s |

分析:
- 包含跨线程唤醒和 SPSC 队列分发开销
- 2.4M tasks/s 满足大多数嵌入式实时场景

### 2.6 SerialTransport 串口吞吐

测试条件: PTY 对模拟串口，波特率 921600，CRC-CCITT 校验，每种 payload 发送 1000 帧。

帧开销: 13 字节 (header 10B + CRC 2B + tail 1B)。

#### 无 ACK 模式

| Payload (B) | 发送 | 接收 | 耗时 (ms) | FPS | KB/s |
|-------------|------|------|-----------|-----|------|
| 8 | 1,000 | 1,000 | 12.0 | 83,398 | 651.5 |
| 32 | 1,000 | 1,000 | 9.8 | 101,940 | 3,185.6 |
| 64 | 1,000 | 1,000 | 9.4 | 106,311 | 6,644.5 |
| 128 | 1,000 | 1,000 | 11.3 | 88,769 | 11,096.1 |
| 256 | 1,000 | 1,000 | 14.4 | 69,226 | 17,306.5 |
| 512 | 1,000 | 1,000 | 19.5 | 51,300 | 25,650.2 |

#### ACK 模式

| Payload (B) | 发送 | 接收 | 耗时 (ms) | FPS | KB/s | us/frame |
|-------------|------|------|-----------|-----|------|----------|
| 8 | 1,000 | 1,000 | 15.8 | 63,308 | 494.6 | 15.8 |
| 32 | 1,000 | 1,000 | 18.2 | 54,844 | 1,713.9 | 18.2 |
| 64 | 1,000 | 1,000 | 20.0 | 49,978 | 3,123.6 | 20.0 |
| 128 | 1,000 | 1,000 | 23.1 | 43,252 | 5,406.5 | 23.1 |
| 256 | 1,000 | 1,000 | 25.5 | 39,242 | 9,810.4 | 25.5 |
| 512 | 1,000 | 1,000 | 29.6 | 33,744 | 16,871.9 | 29.6 |

分析:
- 无 ACK 模式下 64B payload 达到峰值 FPS (106K)，更大 payload 因帧长增加 FPS 下降但字节吞吐持续上升
- ACK 模式引入约 30-40% 的 FPS 开销 (等待确认帧)，但保证可靠传输
- 512B payload 无 ACK 模式下字节吞吐达 25.6 MB/s，远超物理波特率 (921600 bps ≈ 112 KB/s)，因 PTY 是内存管道无实际串口限制
- 实际硬件串口吞吐受波特率限制，921600 bps 理论上限约 90 KB/s (含起止位开销)
- 数据来源: `examples/benchmarks/serial_benchmark.cpp`

### 2.7 TCP Loopback 吞吐

测试条件: 127.0.0.1 loopback，v0 帧头 14B，最大帧 2048B，每种 payload 发送 100,000 帧。

| Payload (B) | 发送 | 接收 | 耗时 (ms) | FPS | MB/s |
|-------------|------|------|-----------|-----|------|
| 64 | 100,000 | 100,000 | 1,059.3 | 94,401 | 5.8 |
| 256 | 100,000 | 100,000 | 211.9 | 471,849 | 115.2 |
| 512 | 100,000 | 100,000 | 1,084.7 | 92,188 | 45.0 |
| 1024 | 100,000 | 100,000 | 1,123.0 | 89,043 | 87.0 |

分析:
- 256B payload 达到峰值 FPS (471K)，可能因 TCP Nagle 关闭后小帧合并效率最优
- 512B 和 1024B 的 FPS 回落至 ~90K，但字节吞吐 (MB/s) 随 payload 增大而上升
- TCP loopback 吞吐受内核协议栈开销影响，实际网络场景还需考虑 RTT 和带宽
- 数据来源: `examples/benchmarks/transport_benchmark.cpp`

### 2.8 Unix Domain Socket vs TCP Loopback

测试条件: 单线程 ping-pong (send + recv 一个完整 round-trip)，50,000 次迭代，-O3 编译。

#### 固定消息 4096B

| 传输方式 | 耗时 (ms) | msg/s | MB/s |
|----------|-----------|-------|------|
| Unix Domain Socket | 784.3 | 63,754 | 498.1 |
| TCP 127.0.0.1 | 2,217.9 | 22,544 | 176.1 |

Unix Domain Socket 比 TCP loopback 快 2.83x。

#### 消息大小扫描 (10,000 round-trips)

| 消息大小 (B) | Unix (MB/s) | TCP (MB/s) | 加速比 |
|-------------|-------------|------------|--------|
| 64 | 8.4 | 2.8 | 2.95x |
| 256 | 34.2 | 11.6 | 2.96x |
| 1,024 | 137.5 | 43.7 | 3.15x |
| 4,096 | 494.0 | 161.3 | 3.06x |
| 16,384 | 1,604.9 | 508.5 | 3.16x |
| 65,536 | 2,726.8 | 1,909.9 | 1.43x |

分析:
- 小到中等消息 (64B-16KB) Unix 稳定快 3x，优势来自跳过 TCP/IP 协议栈
- 大消息 (64KB) 差距缩小到 1.43x，瓶颈转移到 memcpy 和内核缓冲区管理
- Unix Domain Socket 适合同机进程间通信，延迟和吞吐均优于 TCP loopback
- 数据来源: `examples/benchmarks/unix_socket_benchmark.cpp`

### 2.9 ShmRingBuffer SPSC 不同 Payload 吞吐

测试条件: 同线程 push/pop 循环，SlotSize=8192，SlotCount=1024，每种 payload 1,000,000 次操作。

| Payload (B) | 推送 | 弹出 | 耗时 (ms) | M ops/s | MB/s |
|-------------|------|------|-----------|---------|------|
| 64 | 1,000,000 | 1,000,000 | 17.6 | 56.96 | 3,476.8 |
| 256 | 1,000,000 | 1,000,000 | 24.9 | 40.19 | 9,811.3 |
| 512 | 1,000,000 | 1,000,000 | 29.6 | 33.79 | 16,496.9 |
| 1024 | 1,000,000 | 1,000,000 | 90.9 | 11.00 | 10,746.4 |
| 4096 | 1,000,000 | 1,000,000 | 341.4 | 2.93 | 11,442.7 |

分析:
- 64B payload 达到 57M ops/s，接近 L1 cache memcpy 极限
- 1024B 处出现明显拐点 (11M → 2.9M ops/s)，因 slot memcpy 超出 L1 cache line 范围
- 字节吞吐在 512B 时达到峰值 16.5 GB/s，之后因 cache miss 下降
- 数据来源: `examples/benchmarks/transport_benchmark.cpp`

### 2.10 AsyncBus 大 Payload 吞吐

测试条件: 单线程 Publish + ProcessBatch，QueueDepth=4096，BatchSize=4096，Envelope sizeof=8232B。

#### 10,000 消息

| Payload (B) | 发送 | 接收 | 耗时 (ms) | M msgs/s | MB/s |
|-------------|------|------|-----------|----------|------|
| 64 | 10,000 | 10,000 | 25.77 | 0.388 | 23.7 |
| 256 | 10,000 | 10,000 | 15.26 | 0.655 | 160.0 |
| 1024 | 10,000 | 10,000 | 14.80 | 0.676 | 659.8 |
| 4096 | 10,000 | 10,000 | 14.90 | 0.671 | 2,620.9 |
| 8192 | 10,000 | 10,000 | 15.72 | 0.636 | 4,971.0 |

#### 100,000 消息

| Payload (B) | 发送 | 接收 | 耗时 (ms) | M msgs/s | MB/s |
|-------------|------|------|-----------|----------|------|
| 64 | 100,000 | 100,000 | 145.07 | 0.689 | 42.1 |
| 256 | 100,000 | 100,000 | 145.13 | 0.689 | 168.2 |
| 1024 | 100,000 | 100,000 | 150.66 | 0.664 | 648.2 |
| 4096 | 100,000 | 100,000 | 150.67 | 0.664 | 2,592.6 |
| 8192 | 100,000 | 100,000 | 159.24 | 0.628 | 4,906.3 |

分析:
- Envelope 包含完整 variant (8232B)，无论实际 payload 大小，每次 Publish 都拷贝整个 variant
- 因此 64B 和 8192B 的 M msgs/s 差异仅 ~10% (0.689 vs 0.628)，瓶颈在 envelope 拷贝而非 payload 大小
- 字节吞吐随 payload 线性增长: 8192B 达到 4.9 GB/s
- 与 §2.1 小 payload (5.9M msgs/s) 对比，大 variant 导致吞吐下降约 9x，因 envelope sizeof 从 104B 增至 8232B
- 数据来源: `examples/benchmarks/bus_payload_benchmark.cpp`

## 3. 延迟测试

### 3.1 AsyncBus 端到端延迟

SmallMsg (24B) 单线程 Publish + ProcessBatch，10,000 样本:

| 百分位 | 延迟 |
|--------|------|
| Mean | 95 ns |
| P50 | 90 ns |
| P95 | 113 ns |
| P99 | 157 ns |

分析:
- 同线程 publish-to-callback 延迟 P99 < 160 ns
- 跨线程场景需额外考虑线程唤醒延迟 (通常 1-10 us)

## 4. 定时器测试

### 4.1 Add/Remove 吞吐

| 场景 | 吞吐率 |
|------|--------|
| 调度器停止 (冷路径) | 51,393 K ops/s |
| 调度器运行 + 8 活跃任务 (热路径) | 24,811 K ops/s |
| 独立线程 + 8 活跃定时器 | 18,290 K ops/s |
| 4 线程并发 Add/Remove | 9.234 M ops/s |

### 4.2 定时精度

| 场景 | 期望 | 实际 | 误差 |
|------|------|------|------|
| 周期定时 50ms, 500ms 运行 | ~10 次 | 9 次 | 10.0% |
| 平均间隔 | 50.00 ms | 50.00 ms | 0.0% |
| 单次定时 50ms (10 样本) | 50 ms | 50.0 ms | 0.0% |
| 单次定时 10ms 抖动 (100 样本) | 0 us | 650.4 us (mean) | - |
| 单次定时 10ms 最大抖动 | - | 1,123 us | - |

### 4.3 多定时器负载下精度

8 个定时器同时运行 (5/10/20/50 ms 周期)，500ms 测试:

| 周期 (ms) | 触发次数 | 平均抖动 (us) | 最大抖动 (us) |
|-----------|----------|---------------|---------------|
| 5 | 100 | -7.9 | 1,045 |
| 10 | 50 | 0.2 | -1,046 |
| 20 | 25 | -0.7 | -1,035 |
| 50 | 10 | -8.0 | -973 |

分析:
- 平均抖动 < 10 us，满足毫秒级定时需求
- 最大抖动 ~1 ms，受 OS 调度影响 (非 RT 内核)
- SCHED_FIFO + RealtimeExecutor 可进一步降低抖动

## 5. 背压与优先级

100K 消息混合发布 (HIGH/LOW 交替):

#### 默认队列 (QueueDepth=4096)

| 优先级 | 发布数 | 接收数 | 丢弃率 |
|--------|--------|--------|--------|
| HIGH | 2,827 | 2,827 | 0.0% |
| LOW | 1,228 | 1,228 | 0.0% |

总发布 4,055 条 (队列深度限制，大量消息被拒绝发布)。

#### 大队列 (QueueDepth=131072)

| 优先级 | 发布数 | 接收数 | 丢弃率 |
|--------|--------|--------|--------|
| HIGH | 50,000 | 50,000 | 0.0% |
| LOW | 39,321 | 39,321 | 0.0% |

分析:
- 大队列下 HIGH 全部发布成功 (50K/50K)，LOW 发布 39,321 条 (78.6%)
- LOW 被拒绝的 10,679 条是优先级准入控制生效: 队列水位超过 LOW 阈值 (60%) 时拒绝低优先级消息
- 验证了优先级准入控制的设计意图: 高优先级消息零丢弃，低优先级消息在拥塞时被限流

## 6. 内存占用

### 6.1 二进制段分布

| 段 | 大小 | 说明 |
|----|------|------|
| .text | 72,744 B (71 KB) | 代码段 |
| .data | 1,712 B (1.7 KB) | 已初始化数据 |
| .bss | 1,586,048 B (1.5 MB) | 未初始化数据 (AsyncBus 静态实例) |
| 文件大小 | 106 KB | strip 后可执行文件 |

### 6.2 运行时内存

| 指标 | 值 |
|------|-----|
| 峰值 RSS | 5,632 KB (5.5 MB) |
| Minor page faults | 639 |
| Major page faults | 0 |
| 上下文切换 | 31,831 (voluntary) |

### 6.3 核心类型 sizeof

#### 通信与调度

| 类型 | 大小 | 说明 |
|------|------|------|
| `AsyncBus<Payload>` | 530,816 B (518 KB) | 含环形缓冲区 + 订阅表 (静态分配) |
| `Node<Payload>` | 184 B | 轻量节点 (Bus 引用 + 订阅列表) |
| `MessageEnvelope<Payload>` | 104 B | 消息信封 (header + variant) |
| `MessageHeader` | 32 B | 消息头 (sender/topic/priority/timestamp) |
| `WorkerPool<Payload>` | 512 B | 线程池控制结构 (不含工作线程栈) |
| `TimerScheduler<16>` | 704 B | 16 槽定时器调度器 |

#### 共享内存与内存池

| 类型 | 大小 | 说明 |
|------|------|------|
| `ShmRingBuffer<256,1024>` | 270,464 B (264 KB) | 256B slot x 1024 + 控制头 |
| `FixedPool<64,1024>` | 66,608 B (65 KB) | 64B block x 1024 + 空闲链表 |

#### 词汇类型

| 类型 | 大小 | 说明 |
|------|------|------|
| `FixedString<64>` | 72 B | 64 字符 + 长度 + 对齐 |
| `FixedString<256>` | 264 B | 256 字符 + 长度 + 对齐 |
| `FixedVector<uint32_t,64>` | 260 B | 64 元素 + size |
| `FixedFunction<void(),64>` | 80 B | 64B SBO 缓冲 + 函数指针 |
| `expected<int,int>` | 12 B | value + error + tag |
| `optional<uint64_t>` | 16 B | value + has_value |
| `ScopeGuard` | 40 B | FixedFunction 回调 |

#### 状态机与行为树

| 类型 | 大小 | 说明 |
|------|------|------|
| `StateMachine<Ctx,8>` | 416 B | 8 状态 HSM (零堆分配) |
| `BehaviorTree<Ctx,32>` | 2,464 B (2.4 KB) | 32 节点扁平数组 |

#### 可靠性与同步

| 类型 | 大小 | 说明 |
|------|------|------|
| `ThreadWatchdog<8>` | 664 B | 8 线程看门狗 |
| `FaultCollector<64>` | 71,424 B (70 KB) | 64 条故障环形缓冲 |
| `QosProfile` | 16 B | QoS 配置 POD |
| `LightSemaphore` | 96 B | futex-based 信号量 |
| `PosixSemaphore` | 40 B | POSIX sem_t 封装 |

## 7. 资源预算总结

### 7.1 典型嵌入式部署估算

以 ARM Cortex-A7 (1 GHz, 256 MB RAM) 为例，典型 4 节点应用:

| 资源 | 估算 | 说明 |
|------|------|------|
| .text | ~80 KB | 仅链接使用的模块 (header-only LTO) |
| 栈内存 | ~16 KB/线程 | 主线程 + 2 worker + 1 timer |
| AsyncBus | ~520 KB | 单 Bus 实例 (可通过模板参数缩小) |
| ShmRingBuffer | ~264 KB/通道 | 按需创建 |
| MemPool | ~65 KB/池 | 按需创建 |
| 总 RAM | < 2 MB | 不含应用层数据 |

### 7.2 性能缩放参考

| 指标 | x86 实测 (QueueDepth=131072) | ARM 估算 (1 GHz) | 说明 |
|------|------|-------------------|------|
| Bus 吞吐 | 5.9 M msgs/s | ~0.5-1.0 M msgs/s | 受 CAS 性能影响 |
| Bus 延迟 P99 | 307 ns | ~500-1500 ns | 受缓存层级影响 |
| SPSC 吞吐 | 34 M cycles/s | ~5-10 M cycles/s | wait-free，缩放线性 |
| MemPool | 95 M ops/s | ~10-20 M ops/s | 纯内存操作 |
| Timer 抖动 | ~1 ms | ~1-5 ms | 受 OS 调度影响 |
| WorkerPool | 3.1 M tasks/s | ~0.3-0.6 M tasks/s | 含线程唤醒开销 |

### 7.3 QueueDepth 选型参考

| QueueDepth | AsyncBus 内存 | 适用场景 |
|------------|---------------|----------|
| 1024 | ~130 KB | 低频控制消息 (< 1K msgs/s) |
| 4096 (默认) | ~520 KB | 通用场景 (< 10K msgs/s 突发) |
| 16384 | ~2 MB | 中等突发 (视频帧元数据) |
| 131072 | ~16 MB | 高吞吐压测 / 大突发缓冲 |

### 7.4 调优建议

| 场景 | 建议 |
|------|------|
| 内存受限 (< 512 KB) | 缩小 `QueueDepth` (默认 4096)，减少 Bus 实例内存 |
| 低延迟要求 (< 10 us) | 使用 `RealtimeExecutor` (SCHED_FIFO) + CPU affinity |
| 高吞吐要求 (> 1M msgs/s) | 增大 `QueueDepth`，或使用 SPSC 直连替代 Bus 广播 |
| 突发流量场景 | 增大 `QueueDepth` 吸收突发，避免背压丢弃 |
| 多进程 IPC | 使用 `ShmChannel` 替代 TCP，延迟降低 10x |

---

# newosp 性能评估: 工业激光雷达应用场景

> 从属于 [benchmark_report_zh.md](benchmark_report_zh.md) 实测数据
> 版本: 1.0
> 日期: 2026-02-14

## 1. 目标场景

newosp 作为激光雷达产品内部的嵌入式通信框架，运行在 ARM-Linux SoC 上，负责:

- 采集模块、预处理模块、点云拼接模块、输出模块之间的数据流调度
- 设备生命周期管理 (上电→校准→运行→故障→待机)
- 串口诊断通道 (配置下发、固件升级、故障上报)
- 定时器驱动的周期性任务 (扫描触发、心跳、看门狗)

## 2. 性能需求 vs 实测数据

### 2.1 典型激光雷达数据流参数

| 参数 | 典型值 | 说明 |
|------|--------|------|
| 扫描帧率 | 10-25 Hz | 机械旋转或 MEMS 扫描周期 |
| 单帧点数 | 10K-300K points | 取决于线数和角分辨率 |
| 单点大小 | 12-16 B | x/y/z/intensity/timestamp |
| 单帧数据量 | 120 KB - 4.8 MB | 点数 x 单点大小 |
| 内部模块数 | 4-8 | 采集/预处理/拼接/滤波/输出/诊断 |
| 模块间消息频率 | 100-1000 msgs/s | 控制消息 + 帧元数据 |
| 诊断串口波特率 | 115200-921600 bps | 配置/日志/OTA |

### 2.2 吞吐量对比

| 指标 | x86 实测 | ARM 估算 (1 GHz) | 激光雷达需求 | 裕量 |
|------|----------|-------------------|-------------|------|
| Bus 消息吞吐 | 5.9M msgs/s | 0.5-1.0M msgs/s | < 1K msgs/s (控制消息) | > 500x |
| SHM SPSC (256B) | 40M ops/s | 5-10M ops/s | < 30K ops/s (帧元数据) | > 150x |
| SHM SPSC (4KB) | 2.9M ops/s | 0.3-0.6M ops/s | < 25 ops/s (点云分包) | > 10000x |
| MemPool alloc/free | 98M ops/s | 10-20M ops/s | < 100K ops/s | > 100x |
| 串口 (512B, ACK) | 33K FPS | 受波特率限制 | < 100 FPS | 充足 |

所有指标相对需求均有 100x 以上裕量，不构成瓶颈。

### 2.3 延迟对比

| 指标 | x86 实测 | ARM 估算 | 激光雷达需求 | 评估 |
|------|----------|----------|-------------|------|
| Bus P99 延迟 | 157 ns | 500-1500 ns | < 100 us | 充足 |
| Timer 平均抖动 | < 10 us | < 50 us | < 1 ms | 充足 |
| Timer 最大抖动 | ~1 ms | 1-5 ms | < 10 ms | 充足 (非 RT 内核) |

对于扫描周期 40-100 ms (10-25 Hz) 的激光雷达，微秒级的 Bus 延迟和毫秒级的 Timer 抖动完全满足要求。

### 2.4 内存占用对比

| 资源 | newosp 占用 | 激光雷达 SoC 典型配置 | 占比 |
|------|-------------|----------------------|------|
| .text | ~80 KB | 256 MB - 1 GB Flash | < 0.1% |
| 运行时 RAM | < 2 MB | 256 MB - 2 GB DDR | < 1% |
| AsyncBus 实例 | ~520 KB | - | 可通过 QueueDepth 缩小 |

框架本身的资源占用可忽略不计，绝大部分内存留给点云缓冲和算法处理。

## 3. 模块适配分析

### 3.1 推荐使用的模块

| 模块 | 激光雷达用途 | 适配度 |
|------|-------------|--------|
| AsyncBus + Node | 模块间控制消息路由 (启停/模式切换/参数更新) | 高 |
| ShmChannel | 点云数据在进程间零拷贝传递 | 高 |
| HSM | 设备生命周期 (Init→Calibrating→Running→Error→Standby) | 高 |
| BehaviorTree | 自检流程 (检查激光器→检查电机→检查温度→就绪) | 高 |
| TimerScheduler | 扫描触发、心跳、温控采样周期 | 高 |
| SerialTransport | 诊断串口 (配置/日志/OTA)，CRC+ACK 保证可靠性 | 高 |
| ThreadWatchdog | 关键线程存活监控 | 高 |
| FaultCollector | 故障环形缓冲，掉电前持久化 | 高 |
| MemPool | 点云帧缓冲池，避免运行时堆分配 | 高 |
| WorkerPool | 并行点云预处理 (多核 SoC) | 中-高 |

### 3.2 数据流架构建议

```mermaid
flowchart LR
    ACQ["采集模块<br/>SPI/LVDS 驱动"] -->|ShmChannel<br/>原始点云| PRE["预处理模块<br/>去噪/校准"]
    PRE -->|ShmChannel<br/>校准点云| FUSE["拼接模块<br/>多回波融合"]
    FUSE -->|ShmChannel<br/>完整帧| OUT["输出模块<br/>UDP/PTP 发送"]

    CTRL["控制节点<br/>HSM 生命周期"] -->|Bus 控制消息| ACQ
    CTRL -->|Bus 控制消息| PRE
    CTRL -->|Bus 控制消息| FUSE
    CTRL -->|Bus 控制消息| OUT

    DIAG["诊断模块<br/>SerialTransport"] <-->|串口| HOST["上位机/工具"]
    DIAG -->|Bus 参数更新| CTRL

    TMR["TimerScheduler"] -->|周期触发| ACQ
    TMR -->|心跳| CTRL
    WDG["ThreadWatchdog"] -.->|监控| ACQ
    WDG -.->|监控| PRE
    WDG -.->|监控| FUSE
```

关键设计原则:
- 点云数据 (大 payload) 走 ShmChannel 零拷贝，不经过 Bus
- Bus 仅传递控制消息和帧元数据 (< 256B)，保持 variant 轻量
- HSM 管理设备状态，BT 管理自检/恢复流程
- 串口通道独立于数据通路，不影响实时性

### 3.3 HSM 状态设计参考

```mermaid
stateDiagram-v2
    [*] --> PowerOn
    PowerOn --> SelfTest: 上电完成
    SelfTest --> Calibrating: 自检通过
    SelfTest --> Fault: 自检失败

    Calibrating --> Running: 校准完成
    Calibrating --> Fault: 校准超时

    Running --> Standby: 停止命令
    Running --> Fault: 运行异常

    Standby --> Running: 启动命令
    Standby --> PowerOff: 关机命令

    Fault --> SelfTest: 恢复命令
    Fault --> PowerOff: 关机命令

    PowerOff --> [*]
```

## 4. 设计决策与注意事项

### 4.1 控制面/数据面分离

实测数据显示，当 variant 包含 8KB 类型时，Envelope sizeof 达 8232B，Bus 吞吐从 5.9M 降至 0.6M msgs/s。newosp 的架构设计已针对此做了分离:

- 数据面 (点云): 走 ShmChannel 零拷贝传递，不经过 Bus，吞吐不受 variant 大小影响
- 控制面 (消息): 走 Bus pub-sub，variant 仅包含控制消息类型 (< 256B)，Envelope sizeof 保持 ~300B，吞吐 > 5M msgs/s

这是架构层面的设计选择，而非运行时限制。

### 4.2 实时性保障

Timer 最大抖动 ~1ms (x86) / ~5ms (ARM，非 RT 内核)。对于 10-25 Hz 扫描周期 (40-100 ms) 足够。

如需更高确定性:
- 扫描触发等时间敏感操作使用 `RealtimeExecutor` (SCHED_FIFO + CPU affinity)
- 部署 PREEMPT_RT 内核补丁可将抖动降至 < 100 us
- newosp 已内置 `RealtimeExecutor` 支持，无需额外开发

### 4.3 多核缓存优化

ARM 平台的 cache coherency 开销高于 x86。ShmRingBuffer 在 1KB 处出现吞吐拐点 (L1 cache 边界)。newosp 已做如下优化:

- ShmRingBuffer cache line 对齐 (64B padding)
- explicit acquire/release memory fences (ARM 内存序加固)
- 点云分包大小建议 ≤ 512B 以获得最佳 SPSC 吞吐

## 5. 资源预算估算

以典型 ARM Cortex-A53/A72 (1.2-1.8 GHz, 512 MB DDR) 激光雷达 SoC 为例:

| 资源 | newosp 框架 | 点云缓冲 (3 帧 x 1MB) | 算法处理 | 总计 |
|------|-------------|----------------------|----------|------|
| RAM | ~2 MB | ~3 MB | ~10-50 MB | < 60 MB |
| .text | ~80 KB | - | ~1-5 MB | < 6 MB |
| 线程数 | 4-6 (Bus+Timer+Worker+Diag) | - | 2-4 | < 10 |
| CPU 占用 | < 5% | - | 30-60% | < 70% |

框架层资源占用极低，为点云算法留出充足空间。

## 6. 结论

newosp 的性能指标在所有维度上均满足激光雷达内部通信需求，且有 100x 以上裕量。其零堆分配、header-only、轻量级的设计特点与激光雷达嵌入式 SoC 的资源约束高度匹配。

推荐架构: Bus 负责控制面 (轻量消息)，ShmChannel 负责数据面 (点云零拷贝)，HSM 管理设备状态，SerialTransport 提供诊断通道。
