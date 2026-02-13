# SHM IPC Demo -- 共享内存进程间通信示例

## 概述

本示例演示 newosp 共享内存组件 (`ShmChannel` / `ShmRingBuffer` / `SharedMemorySegment`)
的跨进程通信能力，模拟工业场景中的视频帧流式传输 (producer-consumer 模式)。

参考项目: [cpp_py_shmbuf_sample](https://gitee.com/liudegui/cpp_py_shmbuf_sample)

## 组件依赖关系

```mermaid
graph TB
    subgraph "newosp 基础层"
        platform["platform.hpp<br/>SteadyNowUs()"]
        log["log.hpp<br/>OSP_LOG_*"]
        vocabulary["vocabulary.hpp<br/>expected&lt;V,E&gt;"]
    end

    subgraph "newosp 核心层"
        hsm["hsm.hpp<br/>StateMachine&lt;Ctx, 8&gt;"]
        timer["timer.hpp<br/>TimerScheduler&lt;4&gt;"]
        mempool["mem_pool.hpp<br/>FixedPool&lt;S, N&gt;"]
        shutdown["shutdown.hpp<br/>ShutdownManager"]
        shell["shell.hpp<br/>DebugShell + OSP_SHELL_CMD"]
    end

    subgraph "newosp 传输层"
        shm["shm_transport.hpp<br/>ShmChannel&lt;81920, 16&gt;"]
        ring["ShmRingBuffer<br/>无锁 SPSC"]
        seg["SharedMemorySegment<br/>POSIX shm RAII"]
    end

    shm --> ring
    shm --> seg
    shm --> vocabulary
    ring --> platform
    timer --> platform
    hsm --> log

    subgraph "示例程序"
        producer["shm_producer<br/>8-state HSM"]
        consumer["shm_consumer<br/>8-state HSM"]
        monitor["shm_monitor<br/>5 Shell 命令"]
    end

    producer --> hsm
    producer --> shm
    producer --> timer
    producer --> mempool
    producer --> shutdown
    producer --> log
    producer --> platform

    consumer --> hsm
    consumer --> shm
    consumer --> timer
    consumer --> mempool
    consumer --> shutdown
    consumer --> log
    consumer --> platform

    monitor --> shm
    monitor --> shell
    monitor --> shutdown
    monitor --> log
```

## 进程交互架构

```mermaid
graph LR
    subgraph "进程 1: shm_producer"
        P_HSM["HSM<br/>8 states"]
        P_Timer["TimerScheduler&lt;4&gt;<br/>2s 统计"]
        P_Pool["FixedPool&lt;80KB, 4&gt;<br/>帧缓冲"]
        P_Shut["ShutdownManager<br/>SIGINT/SIGTERM"]
    end

    subgraph "POSIX 共享内存"
        SHM["/osp_shm_frame_ch<br/>ShmRingBuffer&lt;80KB, 16&gt;<br/>无锁 SPSC"]
    end

    subgraph "进程 2: shm_consumer"
        C_HSM["HSM<br/>8 states"]
        C_Timer["TimerScheduler&lt;4&gt;<br/>3s 统计"]
        C_Pool["FixedPool&lt;80KB, 4&gt;<br/>接收缓冲"]
        C_Shut["ShutdownManager<br/>SIGINT/SIGTERM"]
    end

    subgraph "进程 3: shm_monitor (可选)"
        M_Shell["DebugShell<br/>telnet :9527"]
        M_Poll["轮询线程<br/>100ms 采样"]
    end

    P_HSM -->|"Write(frame, 76KB)"| SHM
    SHM -->|"Read(buf, size)"| C_HSM
    SHM -.->|"Depth() 只读"| M_Poll
    M_Shell -.->|"shm_status<br/>shm_peek"| SHM

    Telnet["telnet client"] -->|"TCP :9527"| M_Shell
```

## 使用的 newosp 组件

| 组件 | 头文件 | 用途 | 使用方 |
|------|--------|------|--------|
| `ShmChannel<81920, 16>` | `shm_transport.hpp` | 命名共享内存通道 (CreateWriter / OpenReader) | P, C, M |
| `ShmRingBuffer` | `shm_transport.hpp` | 无锁 SPSC 环形缓冲区 (ShmChannel 内部) | 间接 |
| `SharedMemorySegment` | `shm_transport.hpp` | POSIX shm_open/mmap RAII 封装 (ShmChannel 内部) | 间接 |
| `StateMachine<Ctx, 8>` | `hsm.hpp` | 层次状态机驱动 producer/consumer 生命周期 | P, C |
| `FixedPool<81920, 4>` | `mem_pool.hpp` | 帧缓冲区固定块分配 (零堆分配) | P, C |
| `TimerScheduler<4>` | `timer.hpp` | 周期性统计吞吐量 (2s/3s 间隔) | P, C |
| `SteadyNowUs()` | `platform.hpp` | 统一时间戳 (FPS 计算、stall 检测) | P, C |
| `DebugShell` | `shell.hpp` | Telnet 调试 Shell (OSP_SHELL_CMD 注册) | M |
| `ShutdownManager` | `shutdown.hpp` | 信号安全优雅关停 (SIGINT/SIGTERM) | P, C, M |
| `expected<V, E>` | `vocabulary.hpp` | 错误处理 (CreateWriter/OpenReader 返回值) | P, C, M |
| `OSP_LOG_*` | `log.hpp` | 分级日志输出 | P, C, M |

P = Producer, C = Consumer, M = Monitor

## Producer HSM 状态转换

```mermaid
stateDiagram-v2
    state Operational {
        [*] --> Init

        state Init {
            [*] --> [*] : 创建 ShmChannel + 分配 FixedPool
        }

        Init --> Streaming : kEvtInitDone
        Init --> Error : kEvtInitFail

        state Running {
            Streaming --> Paused : kEvtRingFull (ring 满)
            Streaming --> Throttled : kEvtRingFull (连续 >= 3 次)
            Paused --> Streaming : kEvtRingAvail (ring 有空位)
            Throttled --> Streaming : kEvtThrottleEnd (ring < 50%)
            Streaming --> Streaming : kEvtFrameSent
        }

        Running --> Done : kEvtLimitReached

        state Error {
            [*] --> [*] : 等待 1s 重试
        }

        Error --> Init : kEvtRetry (< 3 次)
        Error --> Done : kEvtRetry (>= 3 次)
    }

    Operational --> Done : kEvtShutdown

    state Done {
        [*] --> [*] : Free(frame) + Unlink(channel)
    }
```

## Consumer HSM 状态转换

```mermaid
stateDiagram-v2
    state Operational {
        [*] --> Connecting

        state Connecting {
            [*] --> [*] : OpenReader 重试 (200ms 间隔, 最多 50 次)
        }

        Connecting --> Receiving : kEvtConnected
        Connecting --> Error : kEvtConnectFail (>= 50 次)

        state Running {
            Receiving --> Validating : kEvtFrameReady
            Validating --> Receiving : kEvtFrameValid (校验通过)
            Validating --> Receiving : kEvtFrameInvalid (校验失败, 计数)
            Receiving --> Stalled : kEvtTimeout (> 3s 无数据)
            Stalled --> Receiving : kEvtDataResumed
        }

        state Error {
            [*] --> [*] : 等待 1s 重试
        }

        Error --> Connecting : kEvtRetry (< 3 次)
        Error --> Done : kEvtRetry (>= 3 次)
    }

    Operational --> Done : kEvtShutdown

    state Done {
        [*] --> [*] : Free(recv_buf) + 输出统计
    }
```

## 数据格式

```
┌─────────────────────────────────────────────┐
│ FrameHeader (16 bytes)                      │
│   uint32_t magic      = 0x4652414D ('FRAM') │
│   uint32_t seq_num    = 递增序号              │
│   uint32_t width      = 320                  │
│   uint32_t height     = 240                  │
├─────────────────────────────────────────────┤
│ Pixel data (width * height = 76800 bytes)   │
│   每字节 = (seq_num + offset) & 0xFF        │
└─────────────────────────────────────────────┘
```

帧大小: 76816 bytes, ShmChannel 配置: `<81920, 16>` (slot 80KB x 16)

## 时间戳统一

所有时间测量统一使用 `osp::SteadyNowUs()` (platform.hpp)，不直接使用 `std::chrono::steady_clock`。
与 `design_timer_unification_zh.md` Phase 1 保持一致。

| 用途 | 调用位置 | 说明 |
|------|----------|------|
| FPS 计算 | producer 主循环 | 每 100 帧计算一次 |
| FPS / MB/s 计算 | consumer OnValidating | 每 100 帧计算一次 |
| Stall 检测 | consumer 主循环 | `(now_us - last_frame_us) / 1000 > 3000ms` |
| Pause 时间戳 | producer OnEnterPaused | 记录背压开始时间 |

## Shell 调试命令

通过 `telnet localhost 9527` 连接 monitor 进程:

| 命令 | 说明 |
|------|------|
| `help` | 列出所有命令 |
| `shm_status` | 显示通道状态、队列深度 |
| `shm_stats` | 显示队列填充率、平均深度 |
| `shm_reset` | 重置统计计数器 |
| `shm_peek` | 读取并显示下一帧 header (消费帧) |
| `shm_config` | 显示通道配置参数 |

## 构建与运行

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOSP_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)

# 终端 1: 启动 producer (1000 帧)
./build/examples/shm_ipc/osp_shm_producer frame_ch 1000

# 终端 2: 启动 consumer
./build/examples/shm_ipc/osp_shm_consumer frame_ch

# 终端 3: 启动 monitor (可选)
./build/examples/shm_ipc/osp_shm_monitor frame_ch 9527

# 终端 4: telnet 调试
telnet localhost 9527
```

## 文件说明

| 文件 | 说明 | 使用组件 |
|------|------|----------|
| `shm_producer.cpp` | HSM 驱动的帧生产者 (8 状态) | HSM, ShmChannel, Timer, FixedPool, Shutdown, Log, SteadyNowUs |
| `shm_consumer.cpp` | HSM 驱动的帧消费者 (8 状态) | HSM, ShmChannel, Timer, FixedPool, Shutdown, Log, SteadyNowUs |
| `shm_monitor.cpp` | Shell 调试监控 (5 个 telnet 命令) | ShmChannel, DebugShell, Shutdown, Log |
| `CMakeLists.txt` | 构建配置 (链接 osp, Threads, rt) | -- |

## 线程安全

| 组件 | 线程模型 | 说明 |
|------|----------|------|
| `ShmRingBuffer` | 无锁 CAS | 单写单读 (SPSC)，跨进程安全 |
| `ShmChannel` | 单写多读 | 写端和读端分属不同进程 |
| `DebugShell` | 内部 mutex | 命令注册表保护，会话线程隔离 |
| `TimerScheduler<4>` | 收集-释放-执行 | 锁内收集到期任务，锁外执行回调 |
| `FixedPool<80KB, 4>` | 内部 mutex | Allocate/Free 线程安全 |
| `ShutdownManager` | atomic flag | IsShutdownRequested() 无锁读取 |

## 设计要点

1. HSM entry action 中禁止调用 `sm->Dispatch()` (避免嵌套转换)，
   初始化和验证逻辑放在主循环中执行
2. `TimerScheduler<4>` 使用模板参数指定容量，零堆分配
3. `FixedPool<81920, 4>` 预分配帧缓冲，热路径无 malloc
4. 时间戳统一使用 `osp::SteadyNowUs()`，不引入 `steady_clock::time_point`
5. Producer 背压策略: 连续 3 次 ring full 后进入 Throttled 降速状态
6. Consumer 帧校验: 逐字节验证 `(seq_num + offset) & 0xFF` 模式
