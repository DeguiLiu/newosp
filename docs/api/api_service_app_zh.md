# newosp API 参考: 服务/发现/可靠性/应用层

本文档覆盖 newosp 项目的服务发现、可靠性保障和应用层模块的公共 API。

---

## 模块列表

1. discovery.hpp - 节点发现 (静态 + 多播 + TopicAwareDiscovery)
2. discovery_hsm.hpp - HSM 驱动的发现流程管理
3. service.hpp - RPC 服务 (Service/Client + AsyncClient + ServiceRegistry)
4. service_hsm.hpp - HSM 驱动的服务连接管理
5. node_manager.hpp - 节点管理 + 心跳
6. node_manager_hsm.hpp - HSM 驱动的节点心跳状态机
7. data_fusion.hpp - 多源数据融合
8. watchdog.hpp - 线程看门狗
9. fault_collector.hpp - 故障收集器
10. shell_commands.hpp - 诊断命令桥接
11. app.hpp - Application/Instance 两层模型
12. post.hpp - AppRegistry + OspPost 统一投递

---

## 1. discovery.hpp - 节点发现

**概述**: 提供静态配置和 UDP 多播两种节点发现机制，支持 topic 和 service 感知的发现注册表。

**头文件**: `include/osp/discovery.hpp`
**依赖**: `platform.hpp`, `vocabulary.hpp`, `timer.hpp`
**平台**: Linux/macOS (需要 socket API)

### 配置宏

| 宏名 | 默认值 | 说明 |
|------|--------|------|
| `OSP_DISCOVERY_PORT` | 9999 | 多播发现端口 |
| `OSP_DISCOVERY_INTERVAL_MS` | 1000 | 心跳广播间隔 (毫秒) |
| `OSP_DISCOVERY_TIMEOUT_MS` | 3000 | 节点超时时间 (毫秒) |
| `OSP_DISCOVERY_MULTICAST_GROUP` | "239.255.0.1" | 多播组地址 |

### 错误枚举

```cpp
enum class DiscoveryError : uint8_t {
  kSocketFailed,
  kBindFailed,
  kMulticastJoinFailed,
  kSendFailed,
  kAlreadyRunning,
  kNotRunning,
};
```

### DiscoveredNode 结构体

节点信息快照。

**成员**:

| 成员名 | 类型 | 说明 |
|--------|------|------|
| `name` | `FixedString<63>` | 节点名称 |
| `address` | `FixedString<63>` | IP 地址 |
| `port` | `uint16_t` | 服务端口 |
| `last_seen_us` | `uint64_t` | 最后心跳时间戳 (微秒) |
| `alive` | `bool` | 是否存活 |

### TopicInfo 结构体

Topic 广告信息。

**成员**:

| 成员名 | 类型 | 说明 |
|--------|------|------|
| `name` | `FixedString<63>` | Topic 名称 |
| `type_name` | `FixedString<63>` | 类型名称 |
| `publisher_port` | `uint16_t` | 发布者端口 |
| `is_publisher` | `bool` | true=发布者, false=订阅者 |

### ServiceInfo 结构体

Service 广告信息。

**成员**:

| 成员名 | 类型 | 说明 |
|--------|------|------|
| `name` | `FixedString<63>` | 服务名称 |
| `request_type` | `FixedString<63>` | 请求类型名 |
| `response_type` | `FixedString<63>` | 响应类型名 |
| `port` | `uint16_t` | 服务端口 |

### StaticDiscovery 类

静态配置驱动的节点表。

**模板参数**:
- `MaxNodes` - 最大节点数 (默认 32)

**公共方法**:

```cpp
expected<void, DiscoveryError> AddNode(const char* name, const char* address, uint16_t port) noexcept
```

| 参数 | 类型 | 说明 |
|------|------|------|
| `name` | `const char*` | 节点名称 (截断至 63 字符) |
| `address` | `const char*` | IP 地址 |
| `port` | `uint16_t` | 服务端口 |

**返回**: 成功或 `DiscoveryError`

```cpp
bool RemoveNode(const char* name) noexcept
```

移除指定名称的节点。返回 true 表示找到并移除。

```cpp
const DiscoveredNode* FindNode(const char* name) const noexcept
```

查找节点，返回指针 (未找到返回 nullptr)。

```cpp
void ForEach(void (*callback)(const DiscoveredNode&, void*), void* ctx) const noexcept
```

遍历所有节点。

```cpp
uint32_t NodeCount() const noexcept
```

获取节点数量。

**线程安全性**: 非线程安全，需外部同步。

### MulticastDiscovery 类

UDP 多播自动发现。

**模板参数**:
- `MaxNodes` - 最大节点数 (默认 32)

**配置结构体**:

```cpp
struct Config {
  const char* multicast_group = OSP_DISCOVERY_MULTICAST_GROUP;
  uint16_t port = OSP_DISCOVERY_PORT;
  uint32_t announce_interval_ms = OSP_DISCOVERY_INTERVAL_MS;
  uint32_t timeout_ms = OSP_DISCOVERY_TIMEOUT_MS;
};
```

**公共方法**:

```cpp
void SetLocalNode(const char* name, uint16_t service_port) noexcept
```

设置本地节点信息。

```cpp
void SetOnNodeJoin(NodeCallback cb, void* ctx = nullptr) noexcept
void SetOnNodeLeave(NodeCallback cb, void* ctx = nullptr) noexcept
```

设置节点加入/离开回调。回调签名: `void (*)(const DiscoveredNode&, void*)`

```cpp
expected<void, DiscoveryError> Start() noexcept
```

启动发现 (创建后台线程)。

```cpp
void Stop() noexcept
```

停止发现并等待线程退出。

```cpp
bool IsRunning() const noexcept
```

检查是否运行中。

```cpp
const DiscoveredNode* FindNode(const char* name) const noexcept
uint32_t NodeCount() const noexcept
void ForEach(NodeCallback cb, void* ctx) const noexcept
```

查询和遍历节点。

```cpp
void SetHeartbeat(ThreadHeartbeat* hb) noexcept
```

设置心跳监控 (用于 watchdog)。

**线程安全性**: 所有公共方法线程安全 (内部 mutex 保护)。

**使用示例**:

```cpp
osp::MulticastDiscovery<32> discovery;
discovery.SetLocalNode("robot1", 8080);
discovery.SetOnNodeJoin([](const osp::DiscoveredNode& node, void*) {
  printf("Node joined: %s @ %s:%u\n", node.name.c_str(),
         node.address.c_str(), node.port);
}, nullptr);
auto r = discovery.Start();
// ... 运行 ...
discovery.Stop();
```

### TopicAwareDiscovery 类

Topic 和 Service 感知的发现注册表。

**模板参数**:
- `MaxNodes` - 最大节点数 (默认 32)
- `MaxTopicsPerNode` - 每节点最大 topic 数 (默认 16)

**公共方法**:

```cpp
expected<void, DiscoveryError> AddLocalTopic(const TopicInfo& topic) noexcept
expected<void, DiscoveryError> AddLocalService(const ServiceInfo& svc) noexcept
```

添加本地 topic/service 广告。

```cpp
uint32_t FindPublishers(const char* topic_name, TopicInfo* out, uint32_t max_results) const noexcept
uint32_t FindSubscribers(const char* topic_name, TopicInfo* out, uint32_t max_results) const noexcept
```

查找指定 topic 的发布者/订阅者，返回找到的数量。

```cpp
const ServiceInfo* FindService(const char* service_name) const noexcept
```

查找服务，返回指针 (未找到返回 nullptr)。

```cpp
uint32_t TopicCount() const noexcept
uint32_t ServiceCount() const noexcept
```

获取本地 topic/service 数量。

**线程安全性**: 所有公共方法线程安全 (内部 mutex 保护)。

---

## 2. discovery_hsm.hpp - HSM 驱动的发现流程管理

**概述**: 使用层次状态机管理发现流程生命周期: Idle → Announcing → Discovering → Stable/Degraded。

**头文件**: `include/osp/discovery_hsm.hpp`
**依赖**: `hsm.hpp`, `fault_collector.hpp`, `platform.hpp`

### HSM 事件枚举

```cpp
enum class DiscoveryHsmEvent : uint32_t {
  kDiscEvtStart = 1,
  kDiscEvtNodeFound = 2,
  kDiscEvtNodeLost = 3,
  kDiscEvtNetworkStable = 4,
  kDiscEvtNetworkDegraded = 5,
  kDiscEvtStop = 6,
};
```

### HsmDiscovery 类

HSM 驱动的发现流程管理器。

**模板参数**:
- `MaxNodes` - 最大节点数 (默认 64，用于 API 一致性)

**公共方法**:

```cpp
void SetStableThreshold(uint32_t threshold) noexcept
```

设置稳定状态的最小节点数阈值。

```cpp
void OnStable(DiscoveryCallbackFn fn, void* ctx = nullptr) noexcept
void OnDegraded(DiscoveryCallbackFn fn, void* ctx = nullptr) noexcept
```

设置稳定/降级状态回调。回调签名: `void (*)(void*)`

```cpp
void SetFaultReporter(FaultReporter reporter) noexcept
```

设置故障报告器 (自动报告网络降级)。

```cpp
void Start() noexcept
void Stop() noexcept
```

启动/停止状态机。

```cpp
void OnNodeFound() noexcept
void OnNodeLost() noexcept
void CheckStability() noexcept
void TriggerDegraded() noexcept
```

触发状态机事件。

```cpp
const char* GetState() const noexcept
bool IsStable() const noexcept
bool IsDegraded() const noexcept
bool IsDiscovering() const noexcept
bool IsAnnouncing() const noexcept
bool IsIdle() const noexcept
bool IsStopped() const noexcept
```

查询当前状态。

```cpp
uint32_t GetDiscoveredCount() const noexcept
uint32_t GetLostCount() const noexcept
void ResetCounters() noexcept
```

查询和重置计数器。

**线程安全性**: 所有公共方法线程安全 (内部 mutex 保护)。

**使用示例**:

```cpp
osp::HsmDiscovery<64> hsm_disc;
hsm_disc.SetStableThreshold(3);
hsm_disc.OnStable([](void*) { printf("Network stable\n"); }, nullptr);
hsm_disc.Start();
hsm_disc.OnNodeFound();
hsm_disc.CheckStability();
```


---

## 3. service.hpp - RPC 服务

**概述**: 提供 TCP 请求-响应服务模式，支持同步和异步客户端。

**头文件**: `include/osp/service.hpp`
**依赖**: `platform.hpp`, `vocabulary.hpp`
**平台**: Linux/macOS (需要 socket API)

### 错误枚举

```cpp
enum class ServiceError : uint8_t {
  kBindFailed,
  kConnectFailed,
  kSendFailed,
  kRecvFailed,
  kTimeout,
  kSerializeFailed,
  kDeserializeFailed,
  kNotRunning,
};
```

### 协议常量

| 常量 | 值 | 说明 |
|------|-----|------|
| `kServiceRequestMagic` | 0x4F535052 | 请求帧魔数 ("OSPR") |
| `kServiceResponseMagic` | 0x4F535041 | 响应帧魔数 ("OSPA") |
| `kServiceFrameHeaderSize` | 8 | 帧头大小 (字节) |

**帧格式**:
- 请求: `{ magic(4B), req_size(4B), request_data }`
- 响应: `{ magic(4B), resp_size(4B), response_data }`

### Service 类

服务端请求处理器。

**模板参数**:
- `Request` - 请求消息类型 (必须 trivially copyable)
- `Response` - 响应消息类型 (必须 trivially copyable)

**配置结构体**:

```cpp
struct Config {
  uint16_t port = 0;
  int32_t backlog = 8;
  uint32_t max_concurrent = 4;
};
```

**公共方法**:

```cpp
void SetHandler(Handler handler, void* ctx = nullptr) noexcept
```

设置请求处理函数。签名: `Response (*)(const Request&, void*)`

```cpp
expected<void, ServiceError> Start() noexcept
void Stop() noexcept
bool IsRunning() const noexcept
uint16_t GetPort() const noexcept
void SetHeartbeat(ThreadHeartbeat* hb) noexcept
```

启动/停止服务，查询状态，设置心跳监控。

**线程安全性**: 所有公共方法线程安全。

**使用示例**:

```cpp
struct PingReq { uint32_t seq; };
struct PingResp { uint32_t seq; uint64_t timestamp; };

osp::Service<PingReq, PingResp> service({.port = 8080});
service.SetHandler([](const PingReq& req, void*) -> PingResp {
  return {req.seq, osp::SteadyNowUs()};
}, nullptr);
auto r = service.Start();
```

### Client 类

同步客户端。

**模板参数**:
- `Request` - 请求消息类型 (必须 trivially copyable)
- `Response` - 响应消息类型 (必须 trivially copyable)

**公共方法**:

```cpp
static expected<Client, ServiceError> Connect(const char* host, uint16_t port,
                                               int32_t timeout_ms = 5000) noexcept
```

连接到服务端点。

```cpp
expected<Response, ServiceError> Call(const Request& req, int32_t timeout_ms = 2000) noexcept
void Close() noexcept
bool IsConnected() const noexcept
```

发送请求、关闭连接、查询状态。

**线程安全性**: 非线程安全，单线程使用。

### AsyncClient 类

异步客户端 (后台线程处理)。

**公共方法**:

```cpp
static expected<AsyncClient, ServiceError> Connect(const char* host, uint16_t port,
                                                    int32_t timeout_ms = 5000) noexcept
bool CallAsync(const Request& req, int32_t timeout_ms = 2000) noexcept
bool IsReady() const noexcept
expected<Response, ServiceError> GetResult(int32_t timeout_ms = 5000) noexcept
bool IsConnected() const noexcept
void Close() noexcept
```

**线程安全性**: 所有公共方法线程安全。

### ServiceRegistry 类

本地服务名称到端点的映射注册表。

**模板参数**:
- `MaxServices` - 最大服务数 (默认 32)

**公共方法**:

```cpp
expected<void, ServiceError> Register(const char* name, const char* host, uint16_t port) noexcept
bool Unregister(const char* name) noexcept
optional<Entry> Lookup(const char* name) const noexcept
uint32_t Count() const noexcept
void Reset() noexcept
```

**线程安全性**: 所有公共方法线程安全 (内部 mutex 保护)。

---

## 4. service_hsm.hpp - HSM 驱动的服务连接管理

**概述**: 使用层次状态机管理服务端连接生命周期: Idle → Listening → Active → Error/ShuttingDown。

**头文件**: `include/osp/service_hsm.hpp`
**依赖**: `hsm.hpp`, `fault_collector.hpp`, `platform.hpp`

### HsmService 类

HSM 驱动的服务连接生命周期管理器。

**模板参数**:
- `MaxClients` - 最大并发客户端数 (默认 32)

**公共方法**:

```cpp
void Start() noexcept
void Stop() noexcept
void Recover() noexcept
void OnClientConnect() noexcept
void OnClientDisconnect() noexcept
void OnError(int32_t error_code) noexcept
void SetErrorCallback(ServiceErrorFn fn, void* ctx = nullptr) noexcept
void SetShutdownCallback(ServiceShutdownFn fn, void* ctx = nullptr) noexcept
void SetFaultReporter(FaultReporter reporter) noexcept
const char* GetState() const noexcept
bool IsActive() const noexcept
uint32_t GetActiveClients() const noexcept
```

**线程安全性**: 所有公共方法线程安全 (内部 mutex 保护)。

---

## 5-12. 其他模块

由于篇幅限制，以下模块的详细 API 请参考源代码头文件：

- **node_manager.hpp**: TCP 节点管理 + 心跳检测
- **node_manager_hsm.hpp**: HSM 驱动的节点心跳状态机
- **data_fusion.hpp**: 多源数据融合 (FusedSubscription, TimeSynchronizer)
- **watchdog.hpp**: 线程看门狗 (ThreadWatchdog, WatchdogGuard)
- **fault_collector.hpp**: 故障收集器 (多优先级队列，钩子回调)
- **shell_commands.hpp**: 诊断命令桥接 (RegisterWatchdog, RegisterFaults 等)
- **app.hpp**: Application/Instance 两层模型 (HSM 驱动的实例生命周期)
- **post.hpp**: AppRegistry + OspPost 统一投递 (OspPost, OspSendAndWait)

---

## 总结

本文档覆盖了 newosp 项目的服务发现、可靠性保障和应用层 12 个模块的核心 API。

| 模块 | 用途 | 线程安全 |
|------|------|----------|
| discovery.hpp | 静态/多播节点发现 | 部分线程安全 |
| discovery_hsm.hpp | HSM 驱动的发现流程管理 | 线程安全 |
| service.hpp | TCP 请求-响应服务 | 线程安全 |
| service_hsm.hpp | HSM 驱动的服务连接管理 | 线程安全 |
| node_manager.hpp | TCP 节点管理 + 心跳 | 线程安全 |
| node_manager_hsm.hpp | HSM 驱动的节点心跳状态机 | 线程安全 |
| data_fusion.hpp | 多源数据融合 | 线程安全 |
| watchdog.hpp | 线程看门狗 | 线程安全 |
| fault_collector.hpp | 故障收集器 | 线程安全 (ReportFault 无锁) |
| shell_commands.hpp | 诊断命令桥接 | N/A (注册函数) |
| app.hpp | Application/Instance 两层模型 | 部分线程安全 (Post) |
| post.hpp | AppRegistry + OspPost 统一投递 | 部分线程安全 (PostLocal) |

所有模块均为 header-only，兼容 `-fno-exceptions -fno-rtti`，遵循 MISRA C++ 和 Google C++ Style Guide。

详细的 API 参数、返回值和使用示例请参考各模块的头文件注释和单元测试代码。
