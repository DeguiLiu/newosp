# DataDispatcher 示例 -- 统一数据分发

## 概述

本示例演示统一的 `DataDispatcher<StorePolicy, NotifyPolicy>` API 在两种部署模式下的使用，使用相同的 Alloc/Submit/GetReadable/Release 操作：

1. **进程内流水线** (`pipeline_demo.cpp`): `DataDispatcher<InProcStore>` 实现 4 阶段 DAG
   (entry -> preprocess -> logging + fusion)。引用计数块，自动回收。
2. **跨进程零拷贝** (`producer.cpp` + `consumer_*.cpp`): `DataDispatcher<ShmStore, ShmNotify>`
   基于 POSIX 共享内存。通过 `GetReadable` 零拷贝访问数据，跨进程管理引用计数。
3. **统一 API 演示** (`unified_api_demo.cpp`): 模板函数证明 Store 无关代码 --
   相同的 `Alloc/GetWritable/Submit` 序列适用于任何 StorePolicy。

所有模式共享相同的激光雷达帧格式 (`common.hpp`)。

## 架构

### 模式 1: 进程内流水线 (DataDispatcher<InProcStore>)

```
InProcDispatcher<16016, 32>
  +-- InProcStore (32 blocks x 16016B, 无锁 CAS 分配/释放)
  +-- Pipeline (静态 DAG, 4 阶段)
  |
  |   entry (透传)
  |     |
  |   preprocess (强度过滤)
  |     |
  |     +-- logging (序列号检查 + 统计)
  |     +-- fusion  (边界框)
  |
  +-- FaultCollector (超时/背压上报)
  +-- ScanTimeout (周期性陈旧块回收)
  +-- ForceCleanup (运行结束块恢复)
```

### 模式 2: 跨进程 (DataDispatcher<ShmStore, ShmNotify>)

```
               ShmStore Pool (/osp_dd_pool)
         32 blocks x 16016B, 基于 CAS 的引用计数
                        |
    +-------------------+-------------------+
    |                   |                   |
Producer            Consumer-Logging   Consumer-Fusion
DataDispatcher      DataDispatcher     DataDispatcher
<ShmStore,ShmNotify>  <ShmStore>         <ShmStore>
    |                   |                   |
    +-- ShmNotify ---> ShmSpmcByteChannel <--+
    |   (8B NotifyMsg: block_id + payload_size)
    |
Monitor (DataDispatcher<ShmStore> + DebugShell)
    +-- ConsumerSlot[8] (每消费者 holding_mask 用于崩溃恢复)
    |
Launcher (进程管理器)
```

数据流:
1. Producer: `Alloc -> GetWritable -> FillFrame -> Submit(block_id, size, consumer_count)`
2. Submit 设置 refcount = consumer_count, ShmNotify 推送 `{block_id, payload_size}` 到 SPMC 通道
3. Consumer: 接收 NotifyMsg -> `GetReadable(block_id)` (零拷贝指针) -> 处理 -> `Release(block_id)`
4. 当 refcount 达到 0 时回收块 (最后一个消费者 Release)

### 统一 API 接口

两种模式使用相同的 DataDispatcher 方法：

| 方法 | 描述 |
|------|------|
| `Alloc()` | 分配空闲块 (返回 block_id) |
| `GetWritable(id)` | 获取块数据的可写指针 |
| `Submit(id, size)` | 提交块到流水线 / 设置引用计数 |
| `GetReadable(id)` | 获取只读指针 (零拷贝) |
| `Release(id)` | 递减引用计数，为 0 时回收 |
| `ScanTimeout()` | 回收超时块 |
| `ForceCleanup(pred, ctx)` | 强制释放匹配谓词的块 |
| `RegisterConsumer(pid)` | 注册消费者进程，返回槽索引 (仅 ShmStore) |
| `UnregisterConsumer(id)` | 注销消费者 (仅 ShmStore) |
| `TrackBlockHold(cid, bid)` | 标记块被消费者持有 (位图) (仅 ShmStore) |
| `TrackBlockRelease(cid, bid)` | 清除块持有 (位图) (仅 ShmStore) |
| `CleanupDeadConsumers()` | 回收崩溃消费者的块 (仅 ShmStore) |
| `FreeBlocks()` / `AllocBlocks()` | 池占用查询 |

## 组件依赖

| 组件 | 头文件 | 用途 | 使用者 |
|------|--------|------|--------|
| `DataDispatcher` | `data_dispatcher.hpp` | 统一块池 + 流水线 | All |
| `InProcStore` | `data_dispatcher.hpp` | 进程内块存储 | PL, U |
| `ShmStore` | `data_dispatcher.hpp` | 跨进程共享内存块存储 | P, CL, CF, M |
| `ShmNotify` | `data_dispatcher.hpp` | Submit 通知回调 | P |
| `ShmSpmcByteChannel` | `shm_transport.hpp` | SPMC 通知通道 | P, CL, CF, M |
| `StateMachine<Ctx, N>` | `hsm.hpp` | HSM 生命周期管理 | P, CL, CF |
| `TimerScheduler<4>` | `timer.hpp` | 周期性统计 / ScanTimeout | P, CL, CF, PL |
| `ThreadWatchdog<4>` | `watchdog.hpp` | 线程看门狗 | P, PL |
| `FaultCollector<4,16>` | `fault_collector.hpp` | 故障上报 | P, PL |
| `DebugShell` | `shell.hpp` | Telnet 调试 Shell | M |
| `ShutdownManager` | `shutdown.hpp` | 信号安全关闭 | All |
| `Subprocess` | `process.hpp` | 子进程管理 | L |

PL=Pipeline, U=Unified, P=Producer, CL=Consumer-Logging, CF=Consumer-Fusion, M=Monitor, L=Launcher

## 数据格式

```
+---------------------------------------------+
| LidarFrame Header (16 bytes)                |
|   uint32_t magic      = 0x4C494441 ('LIDA') |
|   uint32_t seq_num    = incremental          |
|   uint32_t point_count = 1000                |
|   uint32_t timestamp_ms                      |
+---------------------------------------------+
| LidarPoint[1000] (16000 bytes)              |
|   float x, y, z                             |
|   uint8_t intensity, ring, pad[2]           |
+---------------------------------------------+
```

帧大小: 16016 字节

## 构建与运行

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOSP_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)

# === 模式 1: 进程内流水线 ===
./build/examples/data_dispatcher/osp_dd_pipeline 200

# === 模式 2: 跨进程 (ShmStore) ===
# 选项 A: launcher (启动所有进程)
./build/examples/data_dispatcher/osp_dd_launcher --frames 200

# 选项 B: 手动启动
./build/examples/data_dispatcher/osp_dd_producer --frames 500 &
./build/examples/data_dispatcher/osp_dd_consumer_logging &
./build/examples/data_dispatcher/osp_dd_consumer_fusion &
./build/examples/data_dispatcher/osp_dd_monitor --port 9600 &
telnet localhost 9600

# === 统一 API 演示 ===
./build/examples/data_dispatcher/osp_dd_unified 50
```

## Shell 命令 (跨进程模式)

通过 `telnet localhost 9600` 连接到 monitor 进程：

| 命令 | 描述 |
|------|------|
| `help` | 列出所有命令 |
| `dd_status` | 池状态 (容量, 空闲/已分配块) |
| `dd_scan` | 在池上运行 ScanTimeout |
| `dd_blocks` | 列出非空闲块及其状态 |
| `dd_notify` | 通知通道状态 (消费者, 可读字节) |
| `dd_peek` | 读取下一个通知并显示帧头 |

## 文件

| 文件 | 描述 | 模式 |
|------|------|------|
| `common.hpp` | 激光雷达帧格式 + 池/共享内存配置 + POSIX 辅助函数 | 共享 |
| `pipeline_demo.cpp` | 4 阶段 DAG 流水线 (InProcStore) | 进程内 |
| `unified_api_demo.cpp` | Store 无关 API 演示 | 进程内 |
| `producer.cpp` | HSM 驱动的生产者 (ShmStore + ShmNotify) | 跨进程 |
| `consumer_logging.cpp` | HSM 驱动的日志消费者 (ShmStore) | 跨进程 |
| `consumer_fusion.cpp` | HSM 驱动的融合消费者 (ShmStore) | 跨进程 |
| `monitor.cpp` | 池检查器 + DebugShell (ShmStore) | 跨进程 |
| `launcher.cpp` | 进程管理器 (spawn/health/restart) | 跨进程 |

## 模式对比

| 维度 | 进程内流水线 | 跨进程 ShmStore |
|------|-------------|----------------|
| 数据共享 | 引用计数零拷贝 (同地址空间) | 引用计数零拷贝 (共享内存) |
| 释放 | 最后一个消费者 Release 回收块 | 最后一个消费者 Release 回收块 |
| 流水线 | DAG (串行 + 扇出) | 扁平 (独立消费者) |
| 背压 | 池阈值回调 | 生产者检查 FreeBlocks |
| 超时 | 每块截止时间 + ScanTimeout | 每块截止时间 + ScanTimeout |
| 故障隔离 | 无 (同进程) | 进程级隔离 |
| 通知 | 流水线自动分发 | ShmSpmcByteChannel (8B NotifyMsg) |
| 使用场景 | 复杂多阶段处理 | 分布式传感器数据分发 |
| 崩溃恢复 | N/A (同进程) | ConsumerSlot holding_mask + CleanupDeadConsumers |
