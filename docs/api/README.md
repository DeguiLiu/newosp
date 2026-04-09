# newosp API 参考文档索引

本目录包含 newosp 所有公共 API 的详细参考文档，按功能模块分类。

## 📚 文档目录

### 1. [基础层与核心通信](api_foundation_core_zh.md)

覆盖最基础的设施和核心通信模块，包括：

- **platform.hpp** - 平台检测、编译器优化、断言
- **vocabulary.hpp** - expected, optional, FixedFunction, FixedVector, FixedString
- **spsc_ringbuffer.hpp** - Lock-free SPSC 环形缓冲
- **log.hpp** - 高性能日志系统
- **timer.hpp** - 定时器调度
- **bus.hpp** - 无锁 MPSC 消息总线
- **node.hpp** - 轻量级 Pub/Sub 节点
- **worker_pool.hpp** - 工作线程池

**适用场景**: 单进程基础设施、高频消息传递、线程管理

---

### 2. [网络与传输层](api_network_transport_zh.md)

覆盖网络 IPC 和数据传输，包括：

- **socket.hpp** - TCP/UDP/Unix 域套接字 RAII 封装
- **connection.hpp** - 连接管理
- **io_poller.hpp** - epoll 事件循环
- **transport.hpp** - 透明网络传输（v0/v1 帧）
- **discovery.hpp** - 节点发现（静态/多播）
- **service.hpp** - RPC 服务框架
- **serial_transport.hpp** - 工业级串口传输

**适用场景**: 多进程通信、远程传输、网络服务、工业串口

---

### 3. [数据流处理层](api_data_flow_zh.md) ⭐ **新增**

覆盖高性能数据处理管道，包括：

- **data_dispatcher.hpp** - 通用多阶段数据分发器，支持 InProc/SHM 双后端
- **shm_transport.hpp** - POSIX 共享内存 IPC（SPSC/SPMC 字节环，futex 通知）
- **Pipeline** - 静态 DAG 管道构建工具

**特性**:
- CAS lock-free 数据块分配
- 字节级零拷贝传输（LiDAR、视频）
- futex 低延迟多进程通知
- 大页（HugePage）支持
- ARM 内存序加固

**适用场景**: 激光雷达点云处理、视频流管道、多进程高频数据融合、工业实时系统

**性能指标**:
- DataDispatcher: 12M blocks/s, 50ns 延迟
- SHM 字节环: 2GB/s, 1µs 延迟
- futex 通知: 500ns 唤醒延迟

---

### 4. [状态机与调度器](api_state_scheduler_zh.md)

覆盖状态管理和任务调度，包括：

- **executor.hpp** - 调度器（Single/Static/Pinned/Realtime）
- **hsm.hpp** - 层次状态机（LCA 转换、guard 条件）
- **bt.hpp** - 行为树（Sequence/Selector/Decorator）
- **lifecycle_node.hpp** - 生命周期节点状态机

**适用场景**: 复杂状态机、实时调度、行为规划、应用生命周期管理

---

### 5. [服务与应用层](api_service_app_zh.md)

覆盖高层应用框架，包括：

- **app.hpp** - Application/Instance 双层模型
- **post.hpp** - 统一消息投递 API
- **qos.hpp** - QoS 配置（可靠性、历史、截止时间）
- **node_manager.hpp** - 节点管理与心跳
- **node_manager_hsm.hpp** - HSM 驱动的节点心跳状态机

**适用场景**: 分布式应用、QoS 保证、节点心跳监控、应用容器

---

## 🎯 快速导航

### 按使用场景

| 场景 | 推荐模块 | 文档 |
|------|---------|------|
| 单进程消息队列 | bus.hpp + SpscRingBuffer | [基础层](api_foundation_core_zh.md) |
| 多进程 IPC | shm_transport.hpp + service.hpp | [数据流](api_data_flow_zh.md) / [网络](api_network_transport_zh.md) |
| 激光雷达处理 | DataDispatcher + ShmSpscByteRing | [数据流](api_data_flow_zh.md) |
| 视频帧管道 | ShmSpmcByteChannel + Pipeline | [数据流](api_data_flow_zh.md) |
| 工业串口通信 | serial_transport.hpp + HSM | [网络](api_network_transport_zh.md) / [状态机](api_state_scheduler_zh.md) |
| 实时调度 | RealtimeExecutor + HSM | [状态机](api_state_scheduler_zh.md) |
| 分布式服务 | service.hpp + discovery.hpp | [网络](api_network_transport_zh.md) |
| 应用生命周期 | app.hpp + lifecycle_node.hpp | [服务](api_service_app_zh.md) |

### 按性能要求

| 要求 | 推荐策略 |
|------|---------|
| 最低延迟（<1µs） | SpscRingBuffer（进程内）、DataDispatcher CAS lock-free |
| 高吞吐量（>100M ops/s） | SpscRingBuffer 批量操作、Bus 批处理 |
| 多进程低延迟 | ShmSpscByteRing + futex 通知 |
| 大数据流（GB/s） | ShmSpmcByteChannel 零拷贝 |
| 实时约束 | RealtimeExecutor + SCHED_FIFO |

### 按线程模型

| 模型 | 推荐模块 |
|------|---------|
| SPSC（单生单消） | SpscRingBuffer, ShmSpscByteRing |
| MPSC（多生单消） | Bus, Service Client |
| SPMC（单生多消） | ShmSpmcByteChannel, Node Subscription |
| MPMC（多生多消） | Service + discovery |

---

## 📖 模块关系图

```
┌─────────────────────────────────────────────────────────┐
│           Application Framework (app.hpp)               │
│  Instance | AppRegistry | Lifecycle | QoS Profile      │
└──────────────────┬──────────────────────────────────────┘
                   │
┌──────────────────┴──────────────────────────────────────┐
│         Service & Discovery Layer                       │
│  Service | Client | Discovery | NodeManager | HSM       │
└──────────────┬───────────────────┬──────────────────────┘
               │                   │
        ┌──────┴─────┐      ┌──────┴──────┐
        │             │      │             │
    ┌───▼──┐   ┌─────▼──┐ ┌─┴───────┐  ┌─┴──────────┐
    │TCP   │   │ Serial │ │ Shared  │  │ Behavior  │
    │UDP   │   │ Port   │ │ Memory  │  │ Tree/HSM  │
    │UDS   │   │ (CRC)  │ │ (futex) │  │           │
    └───┬──┘   └────┬──┘ └────┬────┘  └─┬──────────┘
        │           │         │        │
    ┌───▴────────────┴─────────┴────────┴──────────────────┐
    │           Data Flow & Dispatch Layer                 │
    │  DataDispatcher | SHM Ring Buffers | Pipeline | BT   │
    └─────────┬────────────────────────────────┬───────────┘
              │                                │
        ┌─────▴─────────┐             ┌────────┴────────┐
        │               │             │                 │
    ┌───▼────┐    ┌─────▼──┐    ┌────▼──┐         ┌─────▼──┐
    │InProc  │    │  SHM   │    │ Poll  │ epoll   │ Timer  │
    │Store   │    │ Store  │    │       │ select  │ Signal │
    └────┬───┘    └────┬──┘    └────┬──┘         └─────┬──┘
         │             │            │                 │
         │   ┌─────────┴────────────┴─────────────────┘
         │   │
    ┌────▴───┴────────────────────────────────────────────┐
    │        Core Communication & Threading               │
    │  Bus | Node | Worker Pool | Executor | SpscRing    │
    └──────┬──────────────────────────┬───────────────────┘
           │                          │
    ┌──────▴──────────────────────────▴──────────────────┐
    │          Foundation Layer                          │
    │ Platform | Vocabulary | Log | Shutdown | Memory   │
    └────────────────────────────────────────────────────┘
```

---

## 🔗 相关文档

- **[设计文档](../design_zh.md)** - 完整架构、模块设计、决策理由
- **[数据分发设计](../design_data_dispatcher_zh.md)** - DataDispatcher 深度剖析
- **[性能基准](../benchmark_report_zh.md)** - 实测性能数据
- **[示例代码](../../examples/README.md)** - 完整工作示例
- **[测试文档](../../tests/README.md)** - 单元测试覆盖

---

## 🚀 版本历史

| 版本 | 发布日期 | 主要增加 |
|------|---------|---------|
| v0.5.2 | 2026-02-18 | DataDispatcher、ShmSpscByteRing、ShmSpmcByteChannel |
| v0.5.0 | 2026-02-17 | Job Pool、Pipeline、SHM 传输 |
| v0.4.3 | 2026-02-17 | Lifecycle Node、生命周期管理 |

---

## 📝 使用指南

### 从这里开始

1. **新手入门**: 从 [基础层](api_foundation_core_zh.md) 开始了解核心概念
2. **构建第一个应用**: 参考 [服务层](api_service_app_zh.md) 和示例代码
3. **性能优化**: 根据 [数据流层](api_data_flow_zh.md) 选择合适的零拷贝传输
4. **复杂状态机**: 学习 [状态机与调度器](api_state_scheduler_zh.md)

### 查找特定 API

使用浏览器搜索功能（Ctrl+F）在各文档中查找模块名或函数名。

### 报告文档问题

若发现文档错误或不清楚的地方，请在 GitHub Issues 中反馈。

