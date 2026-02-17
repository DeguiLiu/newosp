# Data Visitor Dispatcher Demo -- 数据分发与流水线

## 概述

本示例演示 newosp 的两层数据分发架构:

1. **进程内 JobPool 流水线** (`pipeline_demo.cpp`): 使用 `DataDispatcher` + `Pipeline` 实现 DAG 拓扑，数据块通过引用计数共享，最后一个消费者完成后自动回收。
2. **跨进程 SPMC 共享内存** (`producer.cpp` + `visitor_*.cpp`): 使用 `ShmSpmcByteChannel` 实现一写多读的零拷贝传输。

两种模式共享相同的 LiDAR 数据格式 (`common.hpp`)，消费者处理逻辑一致。

参考项目: [data-visitor-dispatcher](https://gitee.com/liudegui/data-visitor-dispatcher)

## 架构

### 模式 1: 进程内 JobPool 流水线

```
DataDispatcher<16016, 32>
  +-- JobPool (32 blocks x 16016B, lock-free alloc/release)
  +-- Pipeline (static DAG)
  |
  |   entry (no-op) --+--> logging (帧统计/序号检测)
  |                    +--> fusion  (包围盒计算)
  |
  +-- FaultCollector (超时/背压上报)
  +-- Backpressure callback (free < 4 时告警)
```

数据块生命周期: `Alloc -> Fill -> Submit(refcount=1) -> entry(AddRef 2, Release 1) -> logging(Release) + fusion(Release) -> 回收`

### 模式 2: 跨进程 SPMC 共享内存

```
                    POSIX 共享内存
                 /osp_lidar_spmc (256KB)
                 ShmSpmcByteRing (SPMC)
                        |
    +-------------------+-------------------+
    |                   |                   |
Producer           Visitor-Logging    Visitor-Fusion
(10 Hz LiDAR)     (帧统计/日志)      (障碍物检测)
    |
    +--- Monitor (telnet Shell)
    |
Launcher (进程管理器)
```

## 组件依赖关系

| 组件 | 头文件 | 用途 | 使用方 |
|------|--------|------|--------|
| `DataDispatcher` | `job_pool.hpp` | JobPool + Pipeline 胶水层 | PL |
| `JobPool` | `job_pool.hpp` | Lock-free 数据块池 (引用计数) | PL |
| `Pipeline` | `job_pool.hpp` | 静态 DAG 拓扑 (fan-out/serial) | PL |
| `ShmSpmcByteChannel` | `shm_transport.hpp` | SPMC 命名通道 (1写N读) | P, VL, VF, M |
| `StateMachine<Ctx, N>` | `hsm.hpp` | HSM 驱动生命周期 | P, VL, VF |
| `TimerScheduler<4>` | `timer.hpp` | 周期性统计 | P, VL, VF, PL |
| `ThreadWatchdog<4>` | `watchdog.hpp` | 线程看门狗 | P, PL |
| `FaultCollector<4,16>` | `fault_collector.hpp` | 故障上报 | P, PL |
| `DebugShell` | `shell.hpp` | Telnet 调试 Shell | M |
| `ShutdownManager` | `shutdown.hpp` | 信号安全关停 | All |
| `Subprocess` | `process.hpp` | 子进程管理 | L |

PL=Pipeline, P=Producer, VL=Visitor-Logging, VF=Visitor-Fusion, M=Monitor, L=Launcher

## 数据格式

```
+---------------------------------------------+
| LidarFrame Header (16 bytes)                |
|   uint32_t magic      = 0x4C494441 ('LIDA') |
|   uint32_t seq_num    = 递增序号              |
|   uint32_t point_count = 1000                |
|   uint32_t timestamp_ms                      |
+---------------------------------------------+
| LidarPoint[1000] (16000 bytes)              |
|   float x, y, z                             |
|   uint8_t intensity, ring, pad[2]           |
+---------------------------------------------+
```

帧大小: 16016 bytes

## 构建与运行

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOSP_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)

# === 模式 1: 进程内 JobPool 流水线 ===
./build/examples/data_visitor_dispatcher/osp_dvd_pipeline 200

# === 模式 2: 跨进程 SPMC ===
# 方式 A: launcher 一键启动
./build/examples/data_visitor_dispatcher/osp_dvd_launcher --frames 200

# 方式 B: 手动启动各进程
./build/examples/data_visitor_dispatcher/osp_dvd_producer osp_lidar_spmc 500
./build/examples/data_visitor_dispatcher/osp_dvd_visitor_logging osp_lidar_spmc
./build/examples/data_visitor_dispatcher/osp_dvd_visitor_fusion osp_lidar_spmc
./build/examples/data_visitor_dispatcher/osp_dvd_monitor osp_lidar_spmc 9600
telnet localhost 9600
```

## Pipeline Demo 输出示例

```
[Pipeline] starting: frames=50 block_size=16016 pool_size=32
[Pipeline] producing 50 frames at 10 Hz...
[Logging] frame #10 seq=9 points=1000 ts=900ms
[Logging] frame #20 seq=19 points=1000 ts=1902ms
[Pipeline] stats: logged=20 fused=20 gaps=0 free=32 alloc=0 fps=10.0
...
[Pipeline] === Summary ===
[Pipeline]   Frames produced:  50
[Pipeline]   Alloc failures:   0
[Pipeline]   Logging received: 50 (gaps=0)
[Pipeline]   Fusion processed: 50
[Pipeline]   Pool: free=32 alloc=0
[Pipeline]   Avg FPS: 10.0
[Pipeline]   BBox: x=[10.0,22.6] y=[5.0,14.9] z=[1.5,2.5]
```

## Shell 调试命令 (跨进程模式)

通过 `telnet localhost 9600` 连接 monitor 进程:

| 命令 | 说明 |
|------|------|
| `help` | 列出所有命令 |
| `dvd_status` | 通道状态 (容量、消费者数、可读字节) |
| `dvd_stats` | 累积统计 (帧数、FPS、平均大小) |
| `dvd_peek` | 读取下一帧头部 |
| `dvd_reset` | 重置统计 |
| `dvd_config` | 通道配置 |

## 文件说明

| 文件 | 说明 | 模式 |
|------|------|------|
| `common.hpp` | LiDAR 帧格式 + 通道配置 | 共用 |
| `pipeline_demo.cpp` | JobPool 流水线 (进程内 fan-out) | 进程内 |
| `producer.cpp` | HSM 驱动的帧生产者 | 跨进程 |
| `visitor_logging.cpp` | HSM 驱动的日志订阅者 | 跨进程 |
| `visitor_fusion.cpp` | HSM 驱动的融合订阅者 | 跨进程 |
| `monitor.cpp` | Shell 调试监控 | 跨进程 |
| `launcher.cpp` | 进程管理器 | 跨进程 |

## 两种模式对比

| 维度 | JobPool Pipeline (进程内) | SPMC Channel (跨进程) |
|------|--------------------------|----------------------|
| 数据共享 | 引用计数零拷贝 | Ring buffer 独立拷贝 |
| 释放时机 | 最后一个消费者 Release | 消费者读完即推进 tail |
| 流水线 | DAG (fan-out + serial) | 无 (各消费者独立) |
| 背压 | 池满 + 阈值回调 | Ring full 阻塞写入 |
| 超时检测 | 每块 deadline + 扫描 | 无 |
| 故障隔离 | 无 (同进程) | 有 (进程级隔离) |
| 适用场景 | 复杂流水线 + 生命周期管理 | 简单 1:N 分发 + 故障隔离 |
