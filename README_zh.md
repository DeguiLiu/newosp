# newosp

[![CI](https://github.com/DeguiLiu/newosp/actions/workflows/ci.yml/badge.svg)](https://github.com/DeguiLiu/newosp/actions/workflows/ci.yml)

现代 C++17 纯头文件嵌入式基础设施库，面向 ARM-Linux 平台，从 OSP (Open Streaming Platform) 代码库 (~140k LOC) 提取并现代化重构。34 个头文件，527 测试用例，ASan/TSan/UBSan 全部通过。

## 特性

- **零全局状态**: 所有状态封装在对象中 (RAII)
- **栈优先分配**: 固定容量容器，热路径零堆分配
- **兼容 `-fno-exceptions -fno-rtti`**: 专为嵌入式 ARM-Linux 设计
- **类型安全错误处理**: `expected<V,E>` 和 `optional<T>` 词汇类型
- **纯头文件**: 单一 CMake INTERFACE 库，C++17 标准
- **无锁消息传递**: 基于 MPSC 环形缓冲区的消息总线，支持优先级准入控制
- **模板化设计模式**: 标签分发、变参模板、CRTP、编译期组合

## 模块

### 基础层

| 模块 | 说明 |
|------|------|
| `platform.hpp` | 平台/架构检测、编译器提示、`OSP_ASSERT` 宏 |
| `vocabulary.hpp` | `expected`、`optional`、`FixedVector`、`FixedString`、`FixedFunction`、`function_ref`、`not_null`、`NewType`、`ScopeGuard` |
| `config.hpp` | 多格式配置解析器 (INI/JSON/YAML)，基于模板的后端分发 |
| `log.hpp` | 日志宏，编译期级别过滤 (stderr 后端) |

### 核心层

| 模块 | 说明 |
|------|------|
| `timer.hpp` | 基于 `std::chrono::steady_clock` 的定时任务调度器 |
| `shell.hpp` | 远程调试 Shell (telnet)，支持 TAB 补全、命令历史、`OSP_SHELL_CMD` 注册 |
| `mem_pool.hpp` | 固定块内存池 (`FixedPool<BlockSize, MaxBlocks>`)，嵌入式空闲链表 |
| `shutdown.hpp` | 异步信号安全的优雅关停，LIFO 回调链 + `pipe(2)` 唤醒 |
| `bus.hpp` | 无锁 MPSC 消息总线 (`AsyncBus<PayloadVariant>`)，基于类型的路由 |
| `node.hpp` | 轻量级发布/订阅节点抽象 (`Node<PayloadVariant>`)，灵感来自 ROS2/CyberRT |
| `worker_pool.hpp` | 多工作线程池，基于 AsyncBus + SPSC 每工作线程队列 |
| `executor.hpp` | 调度器 (Single/Static/Pinned + Realtime SCHED_FIFO/DEADLINE) |
| `hsm.hpp` | 层次状态机 (LCA-based transitions, 嵌套状态) |
| `bt.hpp` | 行为树 (Sequence/Fallback/Parallel 组合节点) |

### 网络层

| 模块 | 说明 |
|------|------|
| `socket.hpp` | TCP/UDP RAII 封装 (基于 sockpp) |
| `connection.hpp` | 连接池管理 (自动重连、心跳) |
| `io_poller.hpp` | epoll 事件循环 (边缘触发 + 超时) |
| `transport.hpp` | 透明网络传输 (TCP/UDP 帧协议) |
| `semaphore.hpp` | 轻量信号量 (futex-based) |
| `shm_transport.hpp` | 共享内存 IPC (无锁 SPSC 环形缓冲区) |
| `data_fusion.hpp` | 多源数据融合 (时间对齐、插值) |
| `discovery.hpp` | 节点发现 (UDP 多播 + 静态配置) |
| `service.hpp` | RPC 服务 (请求-响应模式) |
| `node_manager.hpp` | 节点管理 + 心跳监控 |
| `node_manager_hsm.hpp` | HSM 驱动的节点连接管理 (Connected/Suspect/Disconnected) |
| `service_hsm.hpp` | HSM 驱动的服务生命周期 (Idle/Listening/Active/Error) |
| `discovery_hsm.hpp` | HSM 驱动的节点发现流程 (Idle/Announcing/Stable/Degraded) |

### 高级特性

| 模块 | 说明 |
|------|------|
| `lifecycle_node.hpp` | 生命周期节点 (Unconfigured → Inactive → Active → Finalized) |
| `qos.hpp` | QoS 服务质量配置 (Reliability/History/Deadline/Lifespan) |
| `serial_transport.hpp` | 工业串口传输 (CRC-CCITT, PTY 测试, IEC 61508) |
| `app.hpp` | Application/Instance 两层模型 (兼容原始 OSP) |
| `post.hpp` | 统一投递 (本地/远程/广播 + 同步消息) |
| `transport_factory.hpp` | 自动传输选择 (inproc/shm/tcp) |
| `net.hpp` | 网络层封装 (地址解析、套接字选项) |

## 架构

```
┌─────────────────────────────────────────────────────────────────┐
│  应用层: App/Instance, LifecycleNode, QoS                       │
├─────────────────────────────────────────────────────────────────┤
│  服务层: Service (RPC), Discovery, NodeManager                  │
├─────────────────────────────────────────────────────────────────┤
│  传输层: Transport (TCP/UDP), ShmTransport, SerialTransport     │
│          TransportFactory (inproc/shm/tcp 自动选择)             │
├─────────────────────────────────────────────────────────────────┤
│  网络层: Socket, Connection, IoPoller (epoll), Net              │
├─────────────────────────────────────────────────────────────────┤
│  消息层: AsyncBus (MPSC), Node (Pub/Sub), Post (统一投递)       │
├─────────────────────────────────────────────────────────────────┤
│  调度层: Executor (Realtime), WorkerPool, HSM, BT               │
├─────────────────────────────────────────────────────────────────┤
│  基础层: Config, Log, Timer, Shell, MemPool, Shutdown           │
│          Platform, Vocabulary (expected/optional/Fixed*)         │
└─────────────────────────────────────────────────────────────────┘
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

- 示例程序: `examples/` 目录 (12 个示例)，参见 [examples/README.md](examples/README.md)
- 单元测试: `tests/` 目录，527 测试用例，参见 [tests/README.md](tests/README.md)
- 代码生成: `tools/ospgen.py` (YAML → C++ 头文件)，参见 `defs/` 目录

## 设计模式

本库使用基于模板的现代 C++ 模式，替代传统虚函数 OOP:

- **标签分发 + 模板特化**: 配置后端选择 (INI/JSON/YAML)
- **变参模板 + `if constexpr`**: `Config<Backends...>` 编译期组合
- **CRTP**: 无虚函数的可扩展 Shell 命令
- **SBO 回调**: `FixedFunction<Sig, Cap>` 零堆分配
- **无锁 MPSC**: `AsyncBus` 基于序列号的环形缓冲区 + CAS 发布
- **基于类型的路由**: `std::variant` + `VariantIndex<T>` 编译期分发

## 许可证

Apache-2.0
