# newosp AI Knowledge Base

本目录存放 newosp 项目的 AI 可读知识，用于辅助 AI 工具理解项目结构和设计决策。

## 项目概述

newosp 是面向工业级嵌入式平台 (ARM-Linux) 的 C++17 header-only 基础设施库。

- 目标平台: ARM-Linux (GCC / Clang)
- 编译标志: `-fno-exceptions -fno-rtti` (可选)
- 核心原则: 零堆分配热路径、编译期分发、RAII 资源管理

## 模块清单 (42 个头文件)

### 基础层
| 模块 | 头文件 | 职责 |
|------|--------|------|
| Platform | platform.hpp | 平台检测、OSP_ASSERT、编译器提示 |
| Vocabulary | vocabulary.hpp | expected, optional, FixedFunction, FixedVector, FixedString, ScopeGuard |
| SPSC | spsc_ringbuffer.hpp | Lock-free wait-free SPSC 环形缓冲 |

### 核心层
| 模块 | 头文件 | 职责 |
|------|--------|------|
| Config | config.hpp | INI/JSON/YAML 配置解析 |
| Log | log.hpp | stderr 日志宏 |
| Timer | timer.hpp | 定时器调度 |
| Shell | shell.hpp | 调试 Shell (TCP/stdin/UART 多后端) |
| MemPool | mem_pool.hpp | 固定大小内存池 |
| Shutdown | shutdown.hpp | pipe(2) 唤醒 + LIFO 回调 |
| Bus | bus.hpp | 无锁 MPSC 消息总线 |
| Node | node.hpp | Pub/Sub 节点 |
| StaticNode | static_node.hpp | 编译期 Handler 绑定节点 |
| WorkerPool | worker_pool.hpp | 工作线程池 |
| Executor | executor.hpp | 调度器 (Single/Static/Pinned/Realtime) |
| HSM | hsm.hpp | 层次状态机 |
| BT | bt.hpp | 行为树 |
| Semaphore | semaphore.hpp | 信号量 |

### 网络层
| 模块 | 头文件 | 职责 |
|------|--------|------|
| Socket | socket.hpp | TCP/UDP 封装 (sockpp) |
| Connection | connection.hpp | 连接管理 |
| IoPoller | io_poller.hpp | epoll 事件循环 |
| Transport | transport.hpp | 网络传输 (v0/v1 帧) |
| ShmTransport | shm_transport.hpp | 共享内存 IPC |
| DataFusion | data_fusion.hpp | 多源数据融合 |
| Discovery | discovery.hpp | 节点发现 (静态+多播) |
| Service | service.hpp | RPC 服务 |
| NodeManager | node_manager.hpp | 节点管理+心跳 |
| TransportFactory | transport_factory.hpp | 自动传输选择 |

### 应用层
| 模块 | 头文件 | 职责 |
|------|--------|------|
| App | app.hpp | Application/Instance 两层模型 |
| Post | post.hpp | 统一投递 + OspSendAndWait |
| QoS | qos.hpp | QoS 配置 |
| LifecycleNode | lifecycle_node.hpp | 生命周期节点 |

### 诊断层
| 模块 | 头文件 | 职责 |
|------|--------|------|
| ShellCommands | shell_commands.hpp | 14 个内置诊断命令桥接 |
| Watchdog | watchdog.hpp | 线程看门狗 |
| FaultCollector | fault_collector.hpp | 故障收集器 |
| SystemMonitor | system_monitor.hpp | CPU/内存/磁盘监控 |
| Process | process.hpp | 进程管理 |

## Shell 多后端架构 (v0.3.1)

shell.hpp 提供 3 种调试 shell 后端，共享同一命令注册表和 Printf 路由:

| 后端 | 类 | 场景 |
|------|-----|------|
| TCP telnet | `DebugShell` | 有网络环境，telnet 远程调试 |
| stdin/stdout | `ConsoleShell` | 无网络，SSH 或终端直连 |
| UART serial | `UartShell` | 串口调试，开发板早期阶段 |

I/O 抽象通过函数指针 (`ShellWriteFn`/`ShellReadFn`) 实现，非虚接口。

## 关键设计决策

1. **无锁 MPSC Bus**: CAS-based sequence, 模板参数化 QueueDepth/BatchSize
2. **FNV-1a Topic 路由**: Node 使用 32-bit hash 进行 topic 匹配
3. **FixedFunction SBO**: 零 std::function，Small Buffer Optimization 回调
4. **HSM LCA 转换**: 层次状态机使用最低公共祖先算法
5. **Shell 函数指针 I/O**: 所有后端通过 read_fn/write_fn 抽象，无虚调用

## 测试覆盖

- 框架: Catch2 v3.5.2
- 正常模式: 1066+ tests
- -fno-exceptions 模式: 393 tests
- Sanitizer: ASan + UBSan + TSan
