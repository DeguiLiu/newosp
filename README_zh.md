# newosp

[![CI](https://github.com/DeguiLiu/newosp/actions/workflows/ci.yml/badge.svg)](https://github.com/DeguiLiu/newosp/actions/workflows/ci.yml)

现代 C++17 纯头文件嵌入式基础设施库，面向 ARM-Linux 平台，从 OSP (Open Streaming Platform) 代码库 (~140k LOC) 提取并现代化重构。37 个头文件，758 测试用例，ASan/TSan/UBSan 全部通过。

## 特性

- **零全局状态**: 所有状态封装在对象中 (RAII)
- **栈优先分配**: 固定容量容器，热路径零堆分配
- **兼容 `-fno-exceptions -fno-rtti`**: 专为嵌入式 ARM-Linux 设计
- **类型安全错误处理**: `expected<V,E>` 和 `optional<T>` 词汇类型
- **纯头文件**: 单一 CMake INTERFACE 库，C++17 标准
- **无锁消息传递**: 基于 MPSC 环形缓冲区的消息总线，支持优先级准入控制
- **模板化设计模式**: 标签分发、变参模板、CRTP、编译期组合

## 模块

### 基础层 (8 个)

| 模块 | 说明 |
|------|------|
| `platform.hpp` | 平台/架构检测、编译器提示、`OSP_ASSERT` 宏、`SteadyNowUs` 时基 |
| `vocabulary.hpp` | `expected`、`optional`、`FixedVector`、`FixedString`、`FixedFunction`、`function_ref`、`not_null`、`NewType`、`ScopeGuard` |
| `config.hpp` | 多格式配置解析器 (INI/JSON/YAML)，基于模板的后端分发 |
| `log.hpp` | 日志宏，编译期级别过滤 (stderr 后端) |
| `timer.hpp` | 基于 `std::chrono::steady_clock` 的定时任务调度器 |
| `shell.hpp` | 远程调试 Shell (telnet)，支持 TAB 补全、命令历史、`OSP_SHELL_CMD` 注册 |
| `mem_pool.hpp` | 固定块内存池 (`FixedPool<BlockSize, MaxBlocks>`)，嵌入式空闲链表 |
| `shutdown.hpp` | 异步信号安全的优雅关停，LIFO 回调链 + `pipe(2)` 唤醒 |

### 核心通信层 (7 个)

| 模块 | 说明 |
|------|------|
| `bus.hpp` | 无锁 MPSC 消息总线 (`AsyncBus<PayloadVariant>`)，CAS 发布，topic 路由 |
| `node.hpp` | 轻量级发布/订阅节点 (`Node<PayloadVariant>`)，Bus 依赖注入，FNV-1a topic hash |
| `worker_pool.hpp` | 多工作线程池，AsyncBus + SPSC 每工作线程队列，AdaptiveBackoff |
| `spsc_ringbuffer.hpp` | 无锁 wait-free SPSC 环形缓冲 (trivially_copyable, 批量操作, FakeTSO) |
| `executor.hpp` | 调度器 (Single/Static/Pinned + RealtimeExecutor SCHED_FIFO) |
| `semaphore.hpp` | 轻量信号量 (futex-based LightSemaphore/PosixSemaphore) |
| `data_fusion.hpp` | 多源数据融合 (时间对齐、插值) |

### 状态机与行为树 (2 个)

| 模块 | 说明 |
|------|------|
| `hsm.hpp` | 层次状态机 (LCA 转换、guard 条件、零堆分配) |
| `bt.hpp` | 行为树 (Sequence/Fallback/Parallel，扁平数组存储，缓存友好) |

### 网络与传输层 (8 个)

| 模块 | 说明 |
|------|------|
| `socket.hpp` | TCP/UDP RAII 封装 (基于 sockpp) |
| `io_poller.hpp` | epoll 事件循环 (边缘触发 + 超时) |
| `connection.hpp` | 连接池管理 (自动重连、心跳) |
| `transport.hpp` | 网络传输 (v0/v1 帧协议, SequenceTracker) |
| `shm_transport.hpp` | 共享内存 IPC (无锁 SPSC, ARM 内存序, CreateOrReplace 崩溃恢复) |
| `serial_transport.hpp` | 工业串口传输 (CRC-CCITT, PTY 测试, IEC 61508) |
| `net.hpp` | 网络层封装 (地址解析、套接字选项) |
| `transport_factory.hpp` | 自动传输选择 (inproc/shm/tcp) |

### 服务与发现层 (6 个)

| 模块 | 说明 |
|------|------|
| `service.hpp` | RPC 服务 (请求-响应, ServiceRegistry, AsyncClient) |
| `discovery.hpp` | 节点发现 (UDP 多播 + 静态配置, TopicAwareDiscovery) |
| `node_manager.hpp` | 节点管理 + 心跳监控 |
| `node_manager_hsm.hpp` | HSM 驱动节点心跳状态机 (Connected/Suspect/Disconnected) |
| `service_hsm.hpp` | HSM 驱动服务生命周期 (Idle/Listening/Active/Error/ShuttingDown) |
| `discovery_hsm.hpp` | HSM 驱动发现流程 (Idle/Announcing/Discovering/Stable/Degraded) |

### 应用层 (4 个)

| 模块 | 说明 |
|------|------|
| `app.hpp` | Application/Instance 两层模型 (MakeIID, HSM 驱动) |
| `post.hpp` | 统一投递 (AppRegistry + OspPost + OspSendAndWait) |
| `qos.hpp` | QoS 服务质量配置 (Reliability/History/Deadline/Lifespan) |
| `lifecycle_node.hpp` | 生命周期节点 (Unconfigured/Inactive/Active/Finalized, HSM 驱动) |

### 可靠性层 (2 个)

| 模块 | 说明 |
|------|------|
| `watchdog.hpp` | 软件看门狗 (截止时间监控、超时回调) |
| `fault_collector.hpp` | 故障收集与上报 (FaultReporter POD 注入, 环形缓冲) |
| `shell_commands.hpp` | 内置诊断 Shell 命令桥接 (零侵入, 15 个 Register 函数) |

## 架构

### 七层架构

```mermaid
graph TB
    subgraph 应用层
        A1[app.hpp<br/>Application/Instance]
        A2[post.hpp<br/>统一投递]
        A3[qos.hpp<br/>QoS配置]
        A4[lifecycle_node.hpp<br/>生命周期节点]
    end

    subgraph 服务与发现层
        S1[service.hpp<br/>RPC服务]
        S2[discovery.hpp<br/>节点发现]
        S3[node_manager.hpp<br/>节点管理]
        S4[*_hsm.hpp<br/>HSM驱动]
    end

    subgraph 传输层
        T1[transport.hpp<br/>网络传输]
        T2[shm_transport.hpp<br/>共享内存IPC]
        T3[serial_transport.hpp<br/>串口传输]
        T4[transport_factory.hpp<br/>传输选择]
        T5[data_fusion.hpp<br/>数据融合]
    end

    subgraph 网络层
        N1[socket.hpp<br/>TCP/UDP封装]
        N2[connection.hpp<br/>连接管理]
        N3[io_poller.hpp<br/>epoll事件循环]
        N4[net.hpp<br/>网络工具]
    end

    subgraph 核心通信层
        C1[bus.hpp<br/>MPSC消息总线]
        C2[node.hpp<br/>Pub/Sub节点]
        C3[worker_pool.hpp<br/>工作线程池]
        C4[spsc_ringbuffer.hpp<br/>SPSC环形缓冲]
    end

    subgraph 调度与状态层
        E1[executor.hpp<br/>调度器]
        E2[hsm.hpp<br/>层次状态机]
        E3[bt.hpp<br/>行为树]
        E4[semaphore.hpp<br/>信号量]
    end

    subgraph 可靠性层
        R1[watchdog.hpp<br/>看门狗]
        R2[fault_collector.hpp<br/>故障收集]
        R3[shell_commands.hpp<br/>诊断命令]
    end

    subgraph 基础层
        B1[platform.hpp<br/>平台检测]
        B2[vocabulary.hpp<br/>词汇类型]
        B3[config.hpp<br/>配置解析]
        B4[log.hpp<br/>日志]
        B5[timer.hpp<br/>定时器]
        B6[shell.hpp<br/>调试Shell]
        B7[mem_pool.hpp<br/>内存池]
        B8[shutdown.hpp<br/>优雅关停]
    end

    应用层 --> 服务与发现层
    服务与发现层 --> 传输层
    传输层 --> 网络层
    服务与发现层 --> 核心通信层
    核心通信层 --> 调度与状态层
    应用层 --> 可靠性层
    可靠性层 --> 基础层
    调度与状态层 --> 基础层
    网络层 --> 基础层
```

### 关键模块依赖

```mermaid
graph LR
    lifecycle_node[lifecycle_node.hpp] --> hsm[hsm.hpp]
    lifecycle_node --> node[node.hpp]

    app[app.hpp] --> hsm
    app --> post[post.hpp]

    node --> bus[bus.hpp]
    bus --> spsc[spsc_ringbuffer.hpp]

    worker_pool[worker_pool.hpp] --> spsc
    worker_pool --> bus

    service[service.hpp] --> transport[transport.hpp]
    discovery[discovery.hpp] --> socket[socket.hpp]
    node_manager[node_manager.hpp] --> connection[connection.hpp]

    transport --> socket
    shm_transport[shm_transport.hpp] --> spsc
    serial_transport[serial_transport.hpp] --> vocabulary[vocabulary.hpp]

    connection --> io_poller[io_poller.hpp]
    io_poller --> socket

    executor[executor.hpp] --> platform[platform.hpp]
    watchdog[watchdog.hpp] --> platform

    hsm --> vocabulary
    bt[bt.hpp] --> vocabulary

    socket --> net[net.hpp]
    net --> platform

    config[config.hpp] --> vocabulary
    log[log.hpp] --> platform
    timer[timer.hpp] --> platform
    shell[shell.hpp] --> vocabulary
    mem_pool[mem_pool.hpp] --> platform
    shutdown[shutdown.hpp] --> vocabulary
    fault_collector[fault_collector.hpp] --> vocabulary
    shell_commands[shell_commands.hpp] --> shell
    shell_commands --> watchdog
    shell_commands --> fault_collector
    shell_commands --> node_manager_hsm
    shell_commands --> bus
```

## 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### 启用所有配置后端

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
    -DOSP_CONFIG_INI=ON \
    -DOSP_CONFIG_JSON=ON \
    -DOSP_CONFIG_YAML=ON
cmake --build build -j$(nproc)
```

### CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `OSP_BUILD_TESTS` | ON | 构建测试套件 (Catch2 v3.5.2) |
| `OSP_BUILD_EXAMPLES` | OFF | 构建示例程序 |
| `OSP_CONFIG_INI` | ON | 启用 INI 配置后端 (inih) |
| `OSP_CONFIG_JSON` | OFF | 启用 JSON 配置后端 (nlohmann/json) |
| `OSP_CONFIG_YAML` | OFF | 启用 YAML 配置后端 (fkYAML) |
| `OSP_NO_EXCEPTIONS` | OFF | 禁用异常 (`-fno-exceptions`) |
| `OSP_WITH_SOCKPP` | ON | 启用 sockpp 网络库 (socket/transport) |
| `OSP_CODEGEN` | OFF | 启用 YAML → C++ 代码生成 (需要 Python3 + PyYAML + Jinja2) |

## 快速开始

```cpp
#include "osp/config.hpp"
#include "osp/bus.hpp"
#include "osp/node.hpp"
#include "osp/log.hpp"

// 多格式配置
osp::MultiConfig cfg;
cfg.LoadFile("app.yaml");
int32_t port = cfg.GetInt("network", "port", 8080);

// 基于类型的发布/订阅消息传递
struct SensorData { float temp; };
struct MotorCmd { int speed; };
using Payload = std::variant<SensorData, MotorCmd>;

osp::Node<Payload> sensor("sensor", 1);
sensor.Subscribe<SensorData>([](const SensorData& d, const auto& h) {
    OSP_LOG_INFO("sensor", "temp=%.1f from sender %u", d.temp, h.sender_id);
});
sensor.Publish(SensorData{25.0f});
sensor.SpinOnce();
```

## CI 流水线

| 任务 | 说明 |
|------|------|
| **build-and-test** | Ubuntu, Debug + Release |
| **build-with-options** | `-fno-exceptions -fno-rtti` 兼容性验证 |
| **sanitizers** | AddressSanitizer、ThreadSanitizer、UBSan |
| **code-quality** | clang-format、cpplint |

## 环境要求

- CMake >= 3.14
- C++17 编译器 (GCC >= 7, Clang >= 5)
- Linux (ARM-Linux 嵌入式平台)

## 第三方依赖

所有依赖通过 CMake FetchContent 自动获取:

| 库 | 版本 | 用途 | 条件 |
|----|------|------|------|
| [inih](https://github.com/benhoyt/inih) | r58 | INI 配置解析 | `OSP_CONFIG_INI=ON` |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.11.3 | JSON 配置解析 | `OSP_CONFIG_JSON=ON` |
| [fkYAML](https://github.com/fktn-k/fkYAML) | v0.4.0 | YAML 配置解析 | `OSP_CONFIG_YAML=ON` |
| [sockpp](https://github.com/fpagliughi/sockpp) | v1.0.0 | TCP/UDP 套接字封装 | `OSP_WITH_SOCKPP=ON` |
| [Catch2](https://github.com/catchorg/Catch2) | v3.5.2 | 单元测试 | `OSP_BUILD_TESTS=ON` |

## 示例和测试

- **示例程序**: `examples/` 目录，15 个示例 (13 个单文件 + 2 个多文件应用)
  - 单文件示例: `basic_demo.cpp`, `protocol_demo.cpp`, `client_demo.cpp`, `priority_demo.cpp`, `benchmark.cpp`, `serial_demo.cpp`, `realtime_executor_demo.cpp`, `node_manager_hsm_demo.cpp`, `hsm_bt_combo_demo.cpp`, `bt_patrol_demo.cpp`, `hsm_protocol_demo.cpp`, `watchdog_demo.cpp`, `fault_collector_demo.cpp`
  - 多文件应用: `shm_ipc/` (共享内存 IPC 演示), `client_gateway/` (多节点客户端网关)
  - 性能基准: `examples/benchmarks/` (串口、TCP、SHM、Bus 大 payload 吞吐测试)
  - 详见 [docs/examples_zh.md](docs/examples_zh.md)

- **单元测试**: `tests/` 目录，758 测试用例，ASan/TSan/UBSan 全部通过
  - 详见 [tests/README.md](tests/README.md)

- **代码生成**: `tools/ospgen.py` (YAML → C++ 头文件)
  - 定义文件: `defs/` 目录

## 文档

- [架构设计](docs/design_zh.md) - 系统架构、模块设计、资源预算
- [编码规范](docs/coding_standards_zh.md) - 代码风格、命名约定、CI、测试策略
- [开发参考](docs/reference_zh.md) - 编译期配置汇总、线程安全性总结
- [Shell 命令设计](docs/design_shell_commands_zh.md) - 内置诊断命令规划
- [串口集成设计](docs/cserialport_integration_analysis.md) - CSerialPort 集成方案
- [代码生成设计](docs/design_codegen_zh.md) - ospgen YAML → C++ 代码生成
- [性能基准报告](docs/benchmark_report_zh.md) - 吞吐、延迟、内存占用实测数据
- [激光雷达性能评估](docs/performance_analysis_lidar_zh.md) - 工业激光雷达场景适配分析
- [变更日志](docs/changelog_zh.md) - P0 调整 + Phase 实施记录
- [示例指南](docs/examples_zh.md) - 示例用途与架构映射

## 设计模式

本库使用基于模板的现代 C++ 模式，替代传统虚函数 OOP:

- **标签分发 + 模板特化**: 配置后端选择 (INI/JSON/YAML)
- **变参模板 + `if constexpr`**: `Config<Backends...>` 编译期组合
- **CRTP**: 无虚函数的可扩展 Shell 命令
- **SBO 回调**: `FixedFunction<Sig, Cap>` 零堆分配
- **无锁 MPSC**: `AsyncBus` 基于序列号的环形缓冲区 + CAS 发布
- **基于类型的路由**: `std::variant` + `VariantIndex<T>` 编译期分发
- **基础组件复用**: 上层模块统一使用 FixedString/FixedVector/SteadyNowUs，零重复实现

## 许可证

Apache-2.0
