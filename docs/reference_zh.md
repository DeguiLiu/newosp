# newosp 开发参考手册

> 从 [design_zh.md](design_zh.md) 拆分
> 版本: 1.0
> 日期: 2026-02-14

---

## 1. 编译期配置汇总

| 宏 | 默认值 | 模块 | 说明 |
|----|--------|------|------|
| `OSP_LOG_MIN_LEVEL` | 0/1 | log.hpp | 编译期日志级别 |
| `OSP_CONFIG_MAX_FILE_SIZE` | 8192 | config.hpp | 最大配置文件大小 |
| `OSP_BUS_QUEUE_DEPTH` | 4096 | bus.hpp | 环形缓冲区大小 |
| `OSP_BUS_MAX_MESSAGE_TYPES` | 8 | bus.hpp | 最大消息类型数 |
| `OSP_BUS_MAX_CALLBACKS_PER_TYPE` | 16 | bus.hpp | 每类型最大订阅 |
| `OSP_BUS_BATCH_SIZE` | 256 | bus.hpp | 单次批量处理上限 |
| `OSP_MAX_NODE_SUBSCRIPTIONS` | 16 | node.hpp | 每节点最大订阅 |
| `OSP_IO_POLLER_MAX_EVENTS` | 64 | io_poller.hpp | 单次 Wait 最大事件数 |
| `OSP_BT_MAX_NODES` | 32 | bt.hpp | 行为树最大节点数 |
| `OSP_BT_MAX_CHILDREN` | 8 | bt.hpp | 复合节点最大子节点数 |
| `OSP_EXECUTOR_MAX_NODES` | 16 | executor.hpp | Executor 最大节点数 |
| `OSP_TRANSPORT_MAX_FRAME_SIZE` | 4096 | transport.hpp | 最大帧大小 |
| `OSP_CONNECTION_POOL_CAPACITY` | 32 | connection.hpp | 连接池默认容量 |
| `OSP_SHM_SLOT_SIZE` | 4096 | shm_transport.hpp | 共享内存 slot 大小 |
| `OSP_SHM_SLOT_COUNT` | 256 | shm_transport.hpp | 共享内存 slot 数量 |
| `OSP_DISCOVERY_PORT` | 9999 | discovery.hpp | 多播发现端口 |
| `OSP_HEARTBEAT_INTERVAL_MS` | 1000 | node_manager.hpp | 心跳间隔 |
| `OSP_HEARTBEAT_TIMEOUT_COUNT` | 3 | node_manager.hpp | 心跳超时次数 |
| `OSP_SERIAL_FRAME_MAX_SIZE` | 1024 | serial_transport.hpp | 串口最大帧大小 |
| `OSP_SERIAL_CRC_ENABLED` | 1 | serial_transport.hpp | CRC16 校验开关 |
| `OSP_WORKER_QUEUE_DEPTH` | 1024 | worker_pool.hpp | 工作线程 SPSC 队列深度 |
| `OSP_SERIAL_RX_RING_SIZE` | 4096 | serial_transport.hpp | 串口接收环形缓冲 |
| `OSP_TRANSPORT_RECV_RING_DEPTH` | 32 | transport.hpp | 网络帧接收缓冲深度 |

## 2. 线程安全性总结

| 模块 | 线程安全保证 |
|------|-------------|
| platform | N/A (纯宏) |
| vocabulary | 局部对象，无共享 |
| config | 加载后只读 |
| log | fprintf 原子写 |
| timer | mutex 保护所有公有方法 |
| shell | 注册表 mutex + 会话线程隔离 |
| mem_pool | mutex 保护 alloc/free |
| shutdown | 原子标志 + async-signal-safe pipe |
| bus | 无锁 MPSC + SharedMutex 订阅 |
| node | 发布线程安全; SpinOnce 单消费者 |
| worker_pool | 原子标志 + CV + SPSC 无锁 |
| spsc_ringbuffer | 单生产者单消费者 (SPSC 约定) |
| socket | 单线程使用，fd 不可共享 |
| io_poller | 单线程 (事件循环线程) |
| connection | mutex 保护连接池操作 |
| transport | TcpTransport 单线程; NetworkNode 继承 Node 线程安全性 |
| shm_transport | ShmRingBuffer 无锁 CAS; ShmChannel 单写多读 |
| serial_transport | NATIVE_SYNC 单线程; IoPoller 事件循环线程安全 |
| hsm | 单线程 Dispatch; guard/handler 不可重入 |
| bt | 单线程 Tick; 叶节点回调不可重入 |
| executor | 内部线程安全; Stop 可跨线程调用 |
| data_fusion | 继承 Bus 订阅线程安全性 |
| discovery | 内部线程安全; 回调在发现线程执行 |
| service | handler 在服务线程执行; Client::Call 可跨线程 |
| app | Application::Post 线程安全; Instance::OnMessage 单线程 |
| post | OspPost 线程安全; OspSendAndWait 阻塞调用线程 |
| node_manager | 内部 mutex 保护; 回调在心跳线程执行 |
| node_manager_hsm | mutex 保护; 每连接独立 HSM 单线程 Dispatch |
| service_hsm | mutex 保护; HSM 单线程 Dispatch |
| discovery_hsm | mutex 保护; HSM 单线程 Dispatch |
| watchdog | Register/Unregister mutex; Beat() 无锁 atomic; Check() 单线程 |
| fault_collector | ReportFault 无锁 MPSC; 消费者独立线程 |
| net | 单线程使用 (同 sockpp) |
| transport_factory | 无状态静态方法; 线程安全 |
