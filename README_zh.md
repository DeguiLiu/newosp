# newosp

[![CI](https://github.com/DeguiLiu/newosp/actions/workflows/ci.yml/badge.svg)](https://github.com/DeguiLiu/newosp/actions/workflows/ci.yml)

现代 C++14/17 纯头文件嵌入式基础设施库，面向 ARM-Linux 平台，从 OSP (Open Streaming Platform) 代码库 (~140k LOC) 提取并现代化重构。

## 特性

- **零全局状态**: 所有状态封装在对象中 (RAII)
- **栈优先分配**: 固定容量容器，热路径零堆分配
- **兼容 `-fno-exceptions -fno-rtti`**: 专为嵌入式 ARM-Linux 设计
- **类型安全错误处理**: `expected<V,E>` 和 `optional<T>` 词汇类型
- **纯头文件**: 单一 CMake INTERFACE 库，C++17 标准
- **无锁消息传递**: 基于 MPSC 环形缓冲区的消息总线，支持优先级准入控制
- **模板化设计模式**: 标签分发、变参模板、CRTP、编译期组合

## 模块

| 模块 | 说明 |
|------|------|
| `platform.hpp` | 平台/架构检测、编译器提示、`OSP_ASSERT` 宏 |
| `vocabulary.hpp` | `expected`、`optional`、`FixedVector`、`FixedString`、`FixedFunction`、`function_ref`、`not_null`、`NewType`、`ScopeGuard` |
| `config.hpp` | 多格式配置解析器 (INI/JSON/YAML)，基于模板的后端分发 |
| `log.hpp` | 日志宏，编译期级别过滤 (stderr 后端) |
| `timer.hpp` | 基于 `std::chrono::steady_clock` 的定时任务调度器 |
| `shell.hpp` | 远程调试 Shell (telnet)，支持 TAB 补全、命令历史、`OSP_SHELL_CMD` 注册 |
| `mem_pool.hpp` | 固定块内存池 (`FixedPool<BlockSize, MaxBlocks>`)，嵌入式空闲链表 |
| `shutdown.hpp` | 异步信号安全的优雅关停，LIFO 回调链 + `pipe(2)` 唤醒 |
| `bus.hpp` | 无锁 MPSC 消息总线 (`AsyncBus<PayloadVariant>`)，基于类型的路由 |
| `node.hpp` | 轻量级发布/订阅节点抽象 (`Node<PayloadVariant>`)，灵感来自 ROS2/CyberRT |
| `worker_pool.hpp` | 多工作线程池，基于 AsyncBus + SPSC 每工作线程队列 |

## 架构

```
                    Config (INI/JSON/YAML)
                    Log (stderr)
                    Timer (std::chrono)
                         |
   Submit ──> AsyncBus (无锁 MPSC 环形缓冲区)
                  |
            ProcessBatch / SpinOnce
                  |
         ┌───────┼───────┐
         v       v       v
      Node 0   Node 1   Node 2    (基于类型的发布/订阅)
         |
   WorkerPool (dispatcher -> SPSC -> worker threads)
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
| **build-and-test** | Ubuntu + macOS，Debug + Release |
| **build-with-options** | `-fno-exceptions -fno-rtti` 兼容性验证 |
| **sanitizers** | AddressSanitizer、ThreadSanitizer、UBSan |
| **code-quality** | clang-format、cpplint |

## 环境要求

- CMake >= 3.14
- C++17 编译器 (GCC >= 7, Clang >= 5)
- POSIX (Linux / macOS)

## 第三方依赖

所有依赖通过 CMake FetchContent 自动获取:

| 库 | 版本 | 用途 | 条件 |
|----|------|------|------|
| [inih](https://github.com/benhoyt/inih) | r58 | INI 配置解析 | `OSP_CONFIG_INI=ON` |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.11.3 | JSON 配置解析 | `OSP_CONFIG_JSON=ON` |
| [fkYAML](https://github.com/fktn-k/fkYAML) | v0.4.0 | YAML 配置解析 | `OSP_CONFIG_YAML=ON` |
| [Catch2](https://github.com/catchorg/Catch2) | v3.5.2 | 单元测试 | `OSP_BUILD_TESTS=ON` |

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
