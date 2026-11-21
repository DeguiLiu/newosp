# newosp 变更日志

只记录每版核心变更；设计与细节见 `docs/design_zh.md` 及各专题文档。

## 最新变更

### 2026-11-21: v0.9.5 Shell 会话安全、Socket 组播与 CI 修复

- 认证：修复 telnet CRLF 导致密码为空；失败退避 250 ms；会话槽复用清零 `authenticated/admin`。
- IAC：新增 `kSubIac` 状态，子协商 payload 不再泄漏为命令输入。
- 权限：`ShellCmdFlags::kAdmin`，顶层命令与子命令均可标记；`Config::admin_username/admin_password`；本地 Console/UART 为 admin。
- 可用性：`help <cmd>` 显示子命令用法与 `[admin]`、`help <prefix>` 前缀检索、`?` 别名、`history`、未知命令近似建议、子命令 TAB 补全、命令别名注册。
- 配置：`DebugShell::Config::bind_ip`、`OSP_SHELL_MAX_COMMANDS`。
- Socket：`SocketAddress` 增加 IPv4 字节访问器；`UdpSocket` 增加 `JoinMulticastGroup`/`LeaveMulticastGroup` 与 `SO_REUSEADDR`/`SO_REUSEPORT` 选项。
- 修复：release 构建下日志未使用参数告警；`ShmRingBuffer` 布局常量与头文件格式；`[shell][privilege]` 用例改为自注册命令，适配 ctest 逐用例进程隔离。
- 验证：shell 相关 4 个测试文件 59/22/6/24 例全部通过；uds_over_tcp 27 例通过。

### 2026-11-16: v0.9.4 共享内存安全与传输层加固（源码不兼容）

- shm 布局 v2；`slot_state`+generation+pid 回收；`ReadChecked`/`WriteChecked`/`Reopen`/`ResetAfterQuiesce`；`WaitWritable`/`WaitConsumers`；`Health`/`FaultReporter`；租约适配器与 `RevokeConsumer`。
- 删除按 id 的 SPMC 接口，`ShmSpmcByteChannel::Read` 返回 `expected`；升级需停机删除 v1 段。
- transport 帧 v2 CRC32 灰度；缓冲不足恢复 `kBufferFull` 且不断连；修复 `AdvertiseTo` 槽位复用导致的重复投递；串口 GCRA 零突发（行为变更）。
- 验证：全量 1411 例 0 失败；`[shm][safety]` 48 例，ASan 48、TSan 44；租约基准 224.4us vs 73.6us。

### 2026-11-15: v0.9.3 Executor bus 注入与 CI 转绿
- executor 支持注入 bus；TOML 嵌套表/数组补齐；Windows/MSVC 与 CI 修复。

### 2026-11-15: v0.9.2 测试构建修复与 span 视图
- 修复测试构建与重复用例名；网络/串口/帧接口增加 `byte_span` 重载。

### 2026-11-15: v0.9.1 AsyncBus 有界、可检测、可修复
- 有界淘汰扫描、分级丢弃统计、`OSP_SAFETY_PROFILE`。

### 2026-10-30: v0.9.0 安全关键路径 fail-closed
- 故障锁存、看门狗自检、实时配置失败可见、shm 布局校验、块守卫。

### 2026-09-10: v0.8.1 / v0.8.0
- v0.8.1 BeatLoop 循环骨架；v0.8.0 TOML 配置后端与 FileBackedParser 骨架。

### 2026-09-09: v0.7.1
- OnFd `user_data` 下沉，修复 fd 复用竞态。

### 2026-09-08: v0.7.0
- 表驱动 HSM、事件循环、熔断，examples 重构。

### 2026-08-09: v0.6.3
- AsyncBus 高优先级驱逐（TDD）。

### 2026-08-04: v0.5.4
- 稳定性、类型安全与构建改进。

### 2026-02-14: inicpp 集成
- 配置解析集成与优化。

## 早期里程碑

- P0-1..P0-7：AsyncBus 单消费者与内存、Node 单例、实时调度、帧序号、ARM 内存序、QoS、Lifecycle。
- Phase A-L：核心组件、通信层、示例与文档、Socket/网络/连接管理、HSM 与 Executor。
- Phase M：共享内存 IPC；Phase N：sockpp 集成。
- Phase O-Q：QoS/Lifecycle/实时调度、串口与统一投递、节点发现与服务。
- Phase R-S：HSM 代码生成、组件统一与 MISRA 合规。
- Phase T：崩溃恢复、组件集成与 SPSC 环形缓冲。
