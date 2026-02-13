# Serial Communication Demo

## 概述

工业级串口通信示例，展示了可靠的串口传输与行为树（BT）和层次状态机（HSM）的集成。

## 功能特性

### 架构设计

- **节点 A (传感器节点)**: 使用 HSM 管理生命周期
  - 状态: Unconfigured → Calibrating → Running → Error
  - 周期性发送传感器数据（温度、压力）
  - 接收控制命令

- **节点 B (控制器节点)**: 使用 BT 实现决策逻辑
  - 行为树: Sequence(CheckSensorValid → EvaluateThreshold → SelectAction → SendCommand)
  - 根据传感器数据做出控制决策
  - 发送控制命令

### 通信特性

- **可靠传输**: CRC-CCITT 校验 + ACK 确认机制
- **帧协议**: 同步字 + 魔数 + 长度 + 序列号 + 类型 + 载荷 + CRC + 尾字节
- **配置**: 115200 波特率, 8N1, 最多 3 次重传
- **测试方式**: PTY 伪终端对，无需真实硬件

### 核心组件

1. **SerialTransport**: 工业级串口传输层
   - CRC-CCITT 帧校验
   - 序列号跟踪
   - ACK/重传机制
   - 统计信息收集

2. **HSM (Hierarchical State Machine)**: 传感器状态管理
   - 配置 → 校准 → 运行 → 错误处理
   - 事件驱动状态转换
   - 层次化状态组织

3. **BT (Behavior Tree)**: 控制器决策逻辑
   - 条件检查（传感器数据有效性）
   - 阈值评估（温度范围判断）
   - 动作选择和执行

4. **Timer**: 周期性任务调度
   - 500ms 周期发送传感器数据
   - 非阻塞定时器实现

## 消息类型

### SensorData (类型 ID: 1)
```cpp
struct SensorData {
  float temperature;      // 温度 (°C)
  float pressure;         // 压力 (hPa)
  uint64_t timestamp_us;  // 时间戳 (微秒)
};
```

### ControlCommand (类型 ID: 2)
```cpp
struct ControlCommand {
  uint32_t mode;          // 模式: 0=正常, 1=降温, 2=加热
  float target_value;     // 目标值
};
```

## 构建和运行

### 构建
```bash
cd /tmp/newosp
cmake -B build -DOSP_BUILD_EXAMPLES=ON
cmake --build build --target serial_demo
```

### 运行
```bash
./build/examples/serial_demo
```

### 预期输出
```
=== Serial Communication Demo ===
PTY A: /dev/pts/X
PTY B: /dev/pts/Y
Serial transports opened
Starting demo loop (5 seconds)...
[Sensor] Configuring sensor...
[Sensor] Calibration complete
[Sensor] Sensor running
[Controller] Received sensor data: temp=30.3, pressure=1016.6
[Controller] Sent command: mode=1, target=20.0
...
Demo complete

=== Statistics ===
Node A (Sensor):
  Frames sent:     11
  Frames received: 0
  Bytes sent:      319
  Bytes received:  164
  CRC errors:      0
  Retransmits:     0

Node B (Controller):
  Frames sent:     4
  Frames received: 10
  Bytes sent:      84
  Bytes received:  290
  CRC errors:      0
  Retransmits:     0
```

## 技术细节

### HSM 状态转换
```
Unconfigured --[kEventConfigure]--> Calibrating
Calibrating --[kEventCalibrateDone]--> Running
Running --[kEventError]--> Error
Error --[kEventRecover]--> Running
```

### BT 决策树
```
Sequence
├── CheckSensorValid (条件)
├── EvaluateThreshold (动作)
│   ├── temp > 28.0°C → mode=1 (降温)
│   ├── temp < 20.0°C → mode=2 (加热)
│   └── 其他 → mode=0 (正常)
├── SelectAction (动作)
└── SendCommand (动作)
```

### 串口帧格式
```
[Sync:2][Magic:2][Len:2][Seq:2][Type:2][Payload:N][CRC:2][Tail:1]
```

- Sync: 0xAA55 (固定同步字)
- Magic: 0x4F53 ("OS")
- Len: 消息长度（包含头部和 CRC）
- Seq: 序列号（用于检测丢包）
- Type: 消息类型 ID
- Payload: 实际数据
- CRC: CRC-CCITT 校验和
- Tail: 0x0D (帧尾标记)

## 设计要点

### 嵌入式友好
- 固定宽度整数类型 (uint32_t, uint16_t)
- 栈分配，热路径无堆分配
- 兼容 `-fno-exceptions -fno-rtti`
- Google C++ 代码风格

### 可靠性
- CRC-CCITT 校验确保数据完整性
- ACK 机制确保消息送达
- 序列号检测丢包和乱序
- 重传机制处理传输失败

### 可扩展性
- 消息类型可扩展（通过类型 ID）
- HSM 状态可扩展（添加新状态和事件）
- BT 节点可扩展（添加新条件和动作）

## 参考

- 原始 OSP 项目: ospdemo (网络通信示例)
- 本示例: 串口版本，适用于嵌入式和工业场景
- 相关头文件:
  - `osp/serial_transport.hpp` - 串口传输层
  - `osp/hsm.hpp` - 层次状态机
  - `osp/bt.hpp` - 行为树
  - `osp/timer.hpp` - 定时器调度器
  - `osp/log.hpp` - 日志系统
