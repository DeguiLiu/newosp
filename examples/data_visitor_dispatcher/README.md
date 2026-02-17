# Data Visitor Dispatcher Demo -- SPMC 共享内存数据分发

## 概述

本示例演示 newosp SPMC (Single-Producer Multi-Consumer) 共享内存组件
(`ShmSpmcByteChannel` / `ShmSpmcByteRing`) 的跨进程数据分发能力，
模拟工业场景中的 LiDAR 点云数据一对多分发。

参考项目: [data-visitor-dispatcher](https://gitee.com/liudegui/data-visitor-dispatcher)

## 架构

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
| `ShmSpmcByteChannel` | `shm_transport.hpp` | SPMC 命名通道 (1写N读) | P, VL, VF, M |
| `ShmSpmcByteRing` | `shm_transport.hpp` | 无锁 SPMC 环形缓冲 (内部) | 间接 |
| `StateMachine<Ctx, N>` | `hsm.hpp` | HSM 驱动生命周期 | P, VL, VF |
| `TimerScheduler<4>` | `timer.hpp` | 周期性统计 | P, VL, VF |
| `ThreadWatchdog<4>` | `watchdog.hpp` | 线程看门狗 | P |
| `FaultCollector<4,16>` | `fault_collector.hpp` | 故障上报 | P |
| `DebugShell` | `shell.hpp` | Telnet 调试 Shell | M |
| `ShutdownManager` | `shutdown.hpp` | 信号安全关停 | P, VL, VF, M, L |
| `Subprocess` | `process.hpp` | 子进程管理 | L |

P=Producer, VL=Visitor-Logging, VF=Visitor-Fusion, M=Monitor, L=Launcher

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

帧大小: 16016 bytes, SPMC 通道: 256KB ring buffer, 最多 4 个消费者

## Producer HSM 状态转换

```
Operational (root)
+-- Init       -- 创建 ShmSpmcByteChannel
+-- Running    -- 父状态 (处理 SHUTDOWN/LIMIT)
|   +-- Streaming -- 10 Hz 帧生产
|   +-- Paused    -- 背压 (ring full)
+-- Error      -- 可恢复错误, 1s 后重试
+-- Done       -- 清理退出
```

## Visitor HSM 状态转换

```
Connecting  -- 重试 OpenReader (200ms 间隔, 最多 50 次)
Receiving/Processing -- 读取帧, 处理数据
Stalled/Overloaded   -- 异常状态 (无数据/处理过慢)
Done        -- 输出最终统计
```

## Shell 调试命令

通过 `telnet localhost 9600` 连接 monitor 进程:

| 命令 | 说明 |
|------|------|
| `help` | 列出所有命令 |
| `dvd_status` | 通道状态 (容量、消费者数、可读字节) |
| `dvd_stats` | 累积统计 (帧数、FPS、平均大小) |
| `dvd_peek` | 读取下一帧头部 |
| `dvd_reset` | 重置统计 |
| `dvd_config` | 通道配置 |

## 构建与运行

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOSP_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)

# 方式 1: 使用 launcher 一键启动
./build/examples/data_visitor_dispatcher/osp_dvd_launcher --frames 200

# 方式 2: 手动启动各进程
# 终端 1: Producer (500 帧)
./build/examples/data_visitor_dispatcher/osp_dvd_producer osp_lidar_spmc 500

# 终端 2: Logging visitor
./build/examples/data_visitor_dispatcher/osp_dvd_visitor_logging osp_lidar_spmc

# 终端 3: Fusion visitor
./build/examples/data_visitor_dispatcher/osp_dvd_visitor_fusion osp_lidar_spmc

# 终端 4: Monitor (可选)
./build/examples/data_visitor_dispatcher/osp_dvd_monitor osp_lidar_spmc 9600

# 终端 5: Telnet 调试
telnet localhost 9600
```

## 文件说明

| 文件 | 说明 | 行数 |
|------|------|------|
| `common.hpp` | LiDAR 帧格式 + 通道配置 | ~110 |
| `producer.cpp` | HSM 驱动的帧生产者 | ~305 |
| `visitor_logging.cpp` | HSM 驱动的日志订阅者 | ~283 |
| `visitor_fusion.cpp` | HSM 驱动的融合订阅者 | ~312 |
| `monitor.cpp` | Shell 调试监控 | ~290 |
| `launcher.cpp` | 进程管理器 | ~289 |

## SPMC vs SPSC 对比

| 维度 | ShmByteChannel (SPSC) | ShmSpmcByteChannel (SPMC) |
|------|----------------------|--------------------------|
| 消费者数 | 1 | 1..N (默认最多 8) |
| 写入检查 | head - tail | head - slowest_tail |
| 通知机制 | futex wake 1 | futex wake ALL |
| 消费者注册 | 无 | CAS atomic 注册 |
| 适用场景 | 点对点传输 | 数据分发 (1:N) |
