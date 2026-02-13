# Serial OTA Demo -- 串口固件升级示例

## 概述

基于 newosp 组件实现的工业级串口固件升级 (OTA) 示例，参考 [uart_statemachine_ringbuffer_linux](https://gitee.com/liudegui/uart_statemachine_ringbuffer_linux) 的协议设计和分层架构。

本示例综合展示了 newosp 库中 10 个核心组件的协同工作。

## 组件集成架构

```mermaid
graph TB
    subgraph main["main.cpp -- 主控流程"]
        MAIN_LOOP["Main Loop<br/>BT tick + Bus process"]
    end

    subgraph host_side["Host 端 (升级工具)"]
        BT["BehaviorTree<br/>Sequence: start→chunks→end→verify"]
        HP["FrameParser<br/>StateMachine 9 states"]
    end

    subgraph device_side["Device 端 (目标设备)"]
        SM["StateMachine<br/>OTA: Idle→Erasing→Receiving→Verifying→Complete"]
        DP["FrameParser<br/>StateMachine 9 states"]
    end

    subgraph infra["基础设施层"]
        TIMER["TimerScheduler<br/>进度上报 250ms + 超时监控 500ms"]
        BUS["AsyncBus<br/>无锁 MPSC 事件通知"]
        SHELL["DebugShell<br/>6 条 telnet 调试命令"]
        LOG["log<br/>结构化日志"]
    end

    subgraph vocab["零堆分配容器 + RAII"]
        FV["FixedVector uint8_t 4096<br/>栈分配固件缓冲区"]
        FS["FixedString 32<br/>栈分配状态名称"]
        EXP["expected V E<br/>无异常错误处理"]
        SG["ScopeGuard / OSP_SCOPE_EXIT<br/>RAII 资源清理"]
    end

    MAIN_LOOP -->|tick| BT
    MAIN_LOOP -->|ProcessBatch| BUS
    BT -->|send frame| HP
    HP <-->|Direct Loopback| DP
    DP -->|dispatch cmd| SM
    SM -->|send ACK| DP

    TIMER -->|callback| BUS
    BUS -->|OtaProgressMsg<br/>OtaStateChangeMsg<br/>OtaCompleteMsg| LOG
    SHELL -->|query| BT
    SHELL -->|query| SM
    SHELL -->|query| BUS
    SHELL -->|query| TIMER

    BT -.->|firmware data| FV
    SM -.->|state name| FS
    TIMER -.->|Add result| EXP
    MAIN_LOOP -.->|cleanup| SG
```

## 组件交互时序

```mermaid
sequenceDiagram
    participant ML as Main Loop
    participant BT as BehaviorTree
    participant HP as Host Parser HSM
    participant DP as Device Parser HSM
    participant SM as Device SM
    participant TM as TimerScheduler
    participant BUS as AsyncBus

    Note over ML: OSP_SCOPE_EXIT 注册清理

    ML->>TM: Add(250ms, progress_cb)
    ML->>TM: Add(500ms, timeout_cb)
    TM-->>ML: expected TimerTaskId
    ML->>TM: Start()

    loop BT tick every 5ms
        ML->>BT: Tick()
        BT->>HP: send OTA frame
        HP->>DP: Direct Loopback
        DP->>SM: dispatch command
        SM->>DP: send ACK
        DP->>HP: Direct Loopback
        HP->>BT: ACK received
    end

    TM-->>BUS: Publish OtaProgressMsg
    ML->>BUS: ProcessBatch()
    BUS-->>ML: subscriber callback -> log

    Note over SM: state change
    SM-->>BUS: Publish OtaStateChangeMsg

    Note over BT: OTA complete
    BT-->>BUS: Publish OtaCompleteMsg
    ML->>TM: Stop()
    Note over ML: ScopeGuard 自动清理
```

## 10 个 osp 组件一览

| # | 组件 | 头文件 | 在本 Demo 中的用途 |
|---|------|--------|-------------------|
| 1 | `StateMachine` | osp/hsm.hpp | Device OTA 状态机 (5 states) + 双端 HSM 帧解析器 (9 states) |
| 2 | `BehaviorTree` | osp/bt.hpp | Host 升级流程: Sequence(start -> chunks -> end -> verify) |
| 3 | `TimerScheduler` | osp/timer.hpp | 进度上报 (250ms) + 超时监控 (500ms, 上限 30s) |
| 4 | `AsyncBus` | osp/bus.hpp | 无锁 MPSC 总线: OtaProgressMsg / OtaStateChangeMsg / OtaCompleteMsg |
| 5 | `FixedVector` | osp/vocabulary.hpp | `FixedVector<uint8_t, 4096>` 栈分配固件缓冲区，零堆分配 |
| 6 | `FixedString` | osp/vocabulary.hpp | `FixedString<32>` 栈分配状态名称，用于 Bus 事件 |
| 7 | `expected` | osp/vocabulary.hpp | Timer Add / Shell Start 的无异常错误处理 |
| 8 | `ScopeGuard` | osp/vocabulary.hpp | `OSP_SCOPE_EXIT` 自动清理 timer、g_host 指针 |
| 9 | `DebugShell` | osp/shell.hpp | 6 条 telnet 命令: ota_status / serial_stats / bus_stats / flash_dump / flash_crc / timer_info |
| 10 | `log` | osp/log.hpp | OSP_LOG_INFO / WARN / DEBUG 结构化日志 |

## 协议帧格式

兼容 uart_statemachine_ringbuffer_linux 的简单帧格式:

```
| 0xAA | LEN_LO | LEN_HI | CMD_CLASS | CMD | DATA[LEN-2] | CRC_LO | CRC_HI | 0x55 |
```

- 帧头: 0xAA, 帧尾: 0x55
- 长度: 2 字节小端序 (cmd_class + cmd + data 的总长度)
- CRC16-CCITT: constexpr 查表, 对 payload 部分校验

## OTA 命令定义

| 命令 | CMD | 方向 | 数据 |
|------|-----|------|------|
| OTA_START | 0x01 | Host -> Device | OtaStartReq (10B): total_size + start_addr + expected_crc |
| OTA_DATA | 0x02 | Host -> Device | OtaDataHdr (6B) + chunk_data[N] |
| OTA_END | 0x03 | Host -> Device | 无数据 |
| OTA_VERIFY | 0x04 | Host -> Device | 无数据 |
| OTA_ACK | 0x8x | Device -> Host | OtaAckResp (7B): status + received_size |
| VERIFY_RESP | 0x84 | Device -> Host | OtaVerifyResp (5B): status + calc_crc |

## 数据结构 (packed, 字节对齐)

```cpp
#pragma pack(push, 1)

struct OtaStartReq {        // 10 bytes
  uint32_t total_size;
  uint32_t start_addr;
  uint16_t expected_crc;
};

struct OtaDataHdr {         // 6 bytes
  uint32_t chunk_offset;
  uint16_t chunk_len;
};

struct OtaAckResp {         // 7 bytes
  uint8_t  cmd_class;
  uint8_t  cmd;
  uint8_t  status;
  uint32_t received_size;
};

struct OtaVerifyResp {      // 5 bytes
  uint8_t  cmd_class;
  uint8_t  cmd;
  uint8_t  status;
  uint16_t calc_crc;
};

#pragma pack(pop)
```

## OTA 状态机 (Device 端)

```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> Erasing : OTA_START
    Erasing --> Receiving : erase OK
    Erasing --> Error : erase failed
    Receiving --> Receiving : OTA_DATA write chunk
    Receiving --> Verifying : OTA_END size matches
    Receiving --> Error : OTA_END size mismatch
    Verifying --> Complete : OTA_VERIFY CRC match
    Verifying --> Error : OTA_VERIFY CRC mismatch
    Complete --> Idle : RESET
    Error --> Idle : RESET
```

## 行为树 (Host 端)

```mermaid
graph TD
    A[Sequence: ota_sequence] --> B[Action: send_start]
    A --> C[Action: send_chunks]
    A --> D[Action: send_end]
    A --> E[Action: send_verify]
```

每个 Action 使用 ActionPhase (kSend/kWait/kDone) 模式处理 BT Sequence 的 re-tick 行为:
- send_start: 发送 OTA_START, 等待 ACK
- send_chunks: 逐包发送 OTA_DATA (128B/chunk), 返回 kRunning 直到全部发完
- send_end: 发送 OTA_END, 等待 ACK
- send_verify: 发送 OTA_VERIFY, 等待校验结果

## Shell 调试命令

通过 `telnet localhost 5090` 连接，支持以下命令:

| 命令 | 说明 | 示例 |
|------|------|------|
| `help` | 列出所有命令 | `help` |
| `cmd_ota_status` | OTA 状态机和传输进度 | `cmd_ota_status` |
| `cmd_serial_stats` | 双端解析器统计 | `cmd_serial_stats` |
| `cmd_bus_stats` | 消息总线统计 | `cmd_bus_stats` |
| `cmd_flash_dump <addr> [len]` | 十六进制转储 Flash | `cmd_flash_dump 0x0 64` |
| `cmd_flash_crc <addr> <len>` | 计算 Flash CRC | `cmd_flash_crc 0x0 4096` |
| `cmd_timer_info` | 定时器配置信息 | `cmd_timer_info` |

## 参数配置

| 参数 | 值 | 说明 |
|------|-----|------|
| kFirmwareSize | 4096 | 模拟固件大小 |
| kChunkSize | 128 | 每包数据大小 |
| kFlashSize | 65536 | 模拟 Flash 大小 (64KB) |
| kFlashSectorSize | 4096 | Flash 扇区大小 |
| kFlashStartAddr | 0x0000 | Flash 写入起始地址 |
| Shell port | 5090 | Telnet 调试端口 |
| OTA tick | 5 ms | BT tick 间隔 |
| Progress report | 250 ms | 进度上报间隔 (TimerScheduler) |
| Timeout | 30000 ms | OTA 最大超时 |

## 编译运行

```bash
cmake -B build -DOSP_BUILD_EXAMPLES=ON
cmake --build build --target osp_serial_ota_demo
./build/examples/osp_serial_ota_demo
```

运行后可在另一个终端连接调试 shell:

```bash
telnet localhost 5090
```

## 典型输出

```
[INFO ] [OTA_MAIN] === Serial OTA Demo ===
[INFO ] [OTA_MAIN] Components: StateMachine + BehaviorTree + DebugShell + TimerScheduler + AsyncBus + vocabulary
[INFO ] [OTA_MAIN] Firmware: 4096 bytes, chunk: 128, addr: 0x0
[INFO ] [OTA_HOST] Starting OTA: size=4096 crc=0xE0B6 chunk=128
[INFO ] [OTA_HOST] START ACK received
[INFO ] [OTA_HOST] Progress: 4096/4096 (100%)
[INFO ] [OTA_DEV] CRC: expected=0xE0B6 calculated=0xE0B6
[INFO ] [OTA_HOST] Verify SUCCESS: CRC=0xE0B6
[INFO ] [OTA_MAIN] ========== Results ==========
[INFO ] [OTA_MAIN] BT ticks:     36
[INFO ] [OTA_MAIN] Host status:  SUCCESS
[INFO ] [OTA_MAIN] Bytes sent:   4096 / 4096
[INFO ] [OTA_MAIN] Device state: Complete
[INFO ] [OTA_MAIN] FW CRC=0xE0B6  Flash CRC=0xE0B6  MATCH
[INFO ] [OTA_MAIN] OTA upgrade completed successfully!
```

## 与参考项目的对应关系

| uart_statemachine_ringbuffer_linux | serial_ota_demo |
|-----------------------------------|-----------------|
| hsm_parser.c (C 状态机) | parser.hpp: osp::StateMachine (C++17 HSM) |
| cmd_handler.c (OTA 命令处理) | device.hpp: DeviceHandler + HSM state handlers |
| uart_protocol.h (帧格式) | protocol.hpp: packed structs + constexpr CRC |
| main.c (测试驱动) | host.hpp: osp::BehaviorTree (自动化流程) |
| printf 调试 | osp::DebugShell (telnet) + osp::log |
| -- | osp::TimerScheduler (定时调度) |
| -- | osp::AsyncBus (事件通知) |
| -- | osp::FixedVector/FixedString (零堆分配容器) |
| -- | osp::expected/ScopeGuard (RAII + 错误处理) |
