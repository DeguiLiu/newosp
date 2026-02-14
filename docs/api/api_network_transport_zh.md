# newosp API 参考: 网络与传输层

本文档覆盖 newosp 项目的网络层和传输层模块公共 API。

---

## 1. socket.hpp - POSIX 套接字封装

**概述**: POSIX 套接字 RAII 抽象，提供 TCP/UDP/Unix 域套接字的类型安全封装。

**头文件**: `include/osp/socket.hpp`

**依赖**: `platform.hpp`, `vocabulary.hpp`

**平台**: Linux / macOS

### 错误枚举

#### SocketError

| 值 | 说明 |
|---|---|
| `kInvalidFd` | 无效文件描述符 |
| `kBindFailed` | 绑定失败 |
| `kListenFailed` | 监听失败 |
| `kConnectFailed` | 连接失败 |
| `kSendFailed` | 发送失败 |
| `kRecvFailed` | 接收失败 |
| `kAcceptFailed` | 接受连接失败 |
| `kAlreadyClosed` | 已关闭 |
| `kSetOptFailed` | 设置选项失败 |
| `kPathTooLong` | 路径过长 |

### SocketAddress

**概述**: IPv4 套接字地址封装 (sockaddr_in)。

#### 静态工厂方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `static expected<SocketAddress, SocketError> FromIpv4(const char* ip, uint16_t port)` | 从点分十进制 IP 和端口创建地址 | 线程安全 |

#### 公共方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `const sockaddr* Raw() const` | 返回底层 sockaddr 指针 | 线程安全 |
| `socklen_t Size() const` | 返回地址结构大小 | 线程安全 |
| `uint16_t Port() const` | 返回主机字节序端口号 | 线程安全 |

### TcpSocket

**概述**: RAII TCP 流套接字，可移动但不可复制。

#### 静态工厂方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `static expected<TcpSocket, SocketError> Create()` | 创建 TCP 套接字 | 线程安全 |

#### 公共方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `expected<void, SocketError> Connect(const SocketAddress& addr)` | 连接到远程地址 | 非线程安全 |
| `expected<int32_t, SocketError> Send(const void* data, size_t len)` | 发送数据 | 非线程安全 |
| `expected<int32_t, SocketError> Recv(void* buf, size_t len)` | 接收数据 | 非线程安全 |
| `expected<void, SocketError> SetNonBlocking(bool enable)` | 设置非阻塞模式 | 非线程安全 |
| `expected<void, SocketError> SetReuseAddr(bool enable)` | 设置 SO_REUSEADDR | 非线程安全 |
| `expected<void, SocketError> SetNoDelay(bool enable)` | 设置 TCP_NODELAY (禁用 Nagle) | 非线程安全 |
| `void Close()` | 关闭套接字 (幂等) | 非线程安全 |
| `int32_t Fd() const` | 返回文件描述符 | 线程安全 |
| `bool IsValid() const` | 检查套接字是否有效 | 线程安全 |

### UdpSocket

**概述**: RAII UDP 数据报套接字。

#### 静态工厂方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `static expected<UdpSocket, SocketError> Create()` | 创建 UDP 套接字 | 线程安全 |

#### 公共方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `expected<void, SocketError> Bind(const SocketAddress& addr)` | 绑定到本地地址 | 非线程安全 |
| `expected<int32_t, SocketError> SendTo(const void* data, size_t len, const SocketAddress& dest)` | 发送数据报到目标地址 | 非线程安全 |
| `expected<int32_t, SocketError> RecvFrom(void* buf, size_t len, SocketAddress& src)` | 接收数据报并获取源地址 | 非线程安全 |
| `void Close()` | 关闭套接字 | 非线程安全 |
| `int32_t Fd() const` | 返回文件描述符 | 线程安全 |
| `bool IsValid() const` | 检查套接字是否有效 | 线程安全 |

### TcpListener

**概述**: RAII TCP 监听器 (服务端)。

#### 静态工厂方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `static expected<TcpListener, SocketError> Create()` | 创建 TCP 监听器 | 线程安全 |

#### 公共方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `expected<void, SocketError> Bind(const SocketAddress& addr)` | 绑定到本地地址 | 非线程安全 |
| `expected<void, SocketError> Listen(int32_t backlog = 128)` | 开始监听 | 非线程安全 |
| `expected<TcpSocket, SocketError> Accept()` | 接受连接 | 非线程安全 |
| `expected<TcpSocket, SocketError> Accept(SocketAddress& client_addr)` | 接受连接并获取客户端地址 | 非线程安全 |
| `void Close()` | 关闭监听器 | 非线程安全 |
| `int32_t Fd() const` | 返回文件描述符 | 线程安全 |
| `bool IsValid() const` | 检查监听器是否有效 | 线程安全 |

### UnixAddress

**概述**: Unix 域套接字地址封装 (sockaddr_un)。

#### 静态工厂方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `static expected<UnixAddress, SocketError> FromPath(const char* path)` | 从文件系统路径创建地址 | 线程安全 |

#### 公共方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `const char* Path() const` | 返回套接字路径 | 线程安全 |
| `const sockaddr* Raw() const` | 返回底层 sockaddr 指针 | 线程安全 |
| `socklen_t Size() const` | 返回地址结构大小 | 线程安全 |

### UnixSocket / UnixListener

**概述**: Unix 域套接字的流式套接字和监听器，API 与 TcpSocket/TcpListener 类似。

**使用示例**:

```cpp
// TCP 客户端
auto addr = SocketAddress::FromIpv4("127.0.0.1", 8080).value();
auto sock = TcpSocket::Create().value();
sock.Connect(addr);
sock.Send("hello", 5);

// TCP 服务端
auto listener = TcpListener::Create().value();
listener.Bind(SocketAddress::FromIpv4("0.0.0.0", 8080).value());
listener.Listen();
auto client = listener.Accept().value();
```

---

## 2. connection.hpp - 连接池管理

**概述**: 固定容量连接池，用于管理网络连接的生命周期和状态。

**头文件**: `include/osp/connection.hpp`

**依赖**: `platform.hpp`, `vocabulary.hpp`

### 错误枚举

#### ConnectionError

| 值 | 说明 |
|---|---|
| `kPoolFull` | 连接池已满 |
| `kNotFound` | 连接未找到 |
| `kInvalidId` | 无效连接 ID |
| `kAlreadyExists` | 连接已存在 |
| `kTimeout` | 超时 |

### ConnectionId

**概述**: 强类型连接标识符。

| 方法签名 | 说明 |
|---|---|
| `static ConnectionId Invalid()` | 返回无效 ID |
| `bool IsValid() const` | 检查 ID 是否有效 |

### ConnectionState

**概述**: 连接状态枚举。

| 值 | 说明 |
|---|---|
| `kIdle` | 空闲 |
| `kConnecting` | 连接中 |
| `kConnected` | 已连接 |
| `kDisconnecting` | 断开中 |
| `kClosed` | 已关闭 |

### ConnectionInfo

**概述**: 连接元数据结构。

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | `ConnectionId` | 连接 ID |
| `state` | `ConnectionState` | 连接状态 |
| `created_time_us` | `uint64_t` | 创建时间戳 (微秒) |
| `last_active_us` | `uint64_t` | 最后活跃时间戳 |
| `remote_ip` | `uint32_t` | IPv4 地址 (主机字节序) |
| `remote_port` | `uint16_t` | 远程端口 |
| `label` | `char[32]` | 可选标签 |

### ConnectionPool<MaxConnections>

**概述**: 固定容量连接池，栈分配，零堆分配。

**模板参数**:
- `MaxConnections`: 最大连接数 (默认 32)

#### 配置宏

| 宏 | 默认值 | 说明 |
|---|---|---|
| `OSP_CONNECTION_POOL_MAX` | 32 | 默认最大连接数 |

#### 公共方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `expected<ConnectionId, ConnectionError> Add(uint32_t remote_ip, uint16_t remote_port, const char* label = nullptr)` | 添加连接 | 非线程安全 |
| `expected<void, ConnectionError> Remove(ConnectionId id)` | 移除连接 | 非线程安全 |
| `expected<void, ConnectionError> SetState(ConnectionId id, ConnectionState state)` | 设置连接状态 | 非线程安全 |
| `expected<void, ConnectionError> Touch(ConnectionId id)` | 更新最后活跃时间 | 非线程安全 |
| `const ConnectionInfo* Find(ConnectionId id) const` | 查找连接 (只读) | 非线程安全 |
| `ConnectionInfo* Find(ConnectionId id)` | 查找连接 (可写) | 非线程安全 |
| `template<typename Func> void ForEach(Func&& func) const` | 遍历所有活跃连接 | 非线程安全 |
| `uint32_t RemoveTimedOut(uint64_t timeout_us)` | 移除超时连接 | 非线程安全 |
| `uint32_t Count() const` | 返回活跃连接数 | 线程安全 |
| `uint32_t Capacity() const` | 返回最大容量 | 线程安全 |
| `bool IsFull() const` | 检查是否已满 | 线程安全 |
| `bool IsEmpty() const` | 检查是否为空 | 线程安全 |
| `void Clear()` | 清空所有连接 | 非线程安全 |

**使用示例**:

```cpp
ConnectionPool<64> pool;
auto id = pool.Add(0x7F000001, 8080, "client1").value();
pool.SetState(id, ConnectionState::kConnected);
pool.Touch(id);
pool.RemoveTimedOut(5000000);  // 5 秒超时
```

---

## 3. io_poller.hpp - I/O 事件轮询

**概述**: 统一 I/O 事件轮询抽象，封装 epoll (Linux) 和 kqueue (macOS)。

**头文件**: `include/osp/io_poller.hpp`

**依赖**: `platform.hpp`, `vocabulary.hpp`

**平台**: Linux (epoll) / macOS (kqueue)

### 错误枚举

#### PollerError

| 值 | 说明 |
|---|---|
| `kCreateFailed` | 创建失败 |
| `kAddFailed` | 添加 fd 失败 |
| `kModifyFailed` | 修改 fd 失败 |
| `kRemoveFailed` | 移除 fd 失败 |
| `kWaitFailed` | 等待失败 |

### IoEvent

**概述**: I/O 事件类型位掩码。

| 值 | 说明 |
|---|---|
| `kReadable` | 可读 (0x01) |
| `kWritable` | 可写 (0x02) |
| `kError` | 错误 (0x04) |
| `kHangup` | 挂断 (0x08) |

### PollResult

**概述**: 轮询结果结构。

| 字段 | 类型 | 说明 |
|---|---|---|
| `fd` | `int32_t` | 文件描述符 |
| `events` | `uint8_t` | 事件位掩码 (IoEvent) |

### IoPoller

**概述**: I/O 事件轮询器，可移动但不可复制。

#### 配置宏

| 宏 | 默认值 | 说明 |
|---|---|---|
| `OSP_IO_POLLER_MAX_EVENTS` | 64 | 单次 Wait 最大事件数 |

#### 公共方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `bool IsValid() const` | 检查轮询器是否有效 | 线程安全 |
| `int32_t Fd() const` | 返回轮询器文件描述符 | 线程安全 |
| `expected<void, PollerError> Add(int32_t fd, uint8_t events)` | 添加 fd 监听 | 非线程安全 |
| `expected<void, PollerError> Modify(int32_t fd, uint8_t events)` | 修改 fd 监听事件 | 非线程安全 |
| `expected<void, PollerError> Remove(int32_t fd)` | 移除 fd 监听 | 非线程安全 |
| `expected<uint32_t, PollerError> Wait(PollResult* results, uint32_t max_results, int32_t timeout_ms = -1)` | 等待事件 (外部缓冲) | 非线程安全 |
| `expected<uint32_t, PollerError> Wait(int32_t timeout_ms = -1)` | 等待事件 (内部缓冲) | 非线程安全 |
| `const PollResult* Results() const` | 访问上次 Wait 结果 | 非线程安全 |

**使用示例**:

```cpp
IoPoller poller;
poller.Add(sock_fd, IoEvent::kReadable | IoEvent::kWritable);
auto count = poller.Wait(1000).value();  // 1 秒超时
for (uint32_t i = 0; i < count; ++i) {
  const auto& r = poller.Results()[i];
  if (r.events & IoEvent::kReadable) { /* 处理可读 */ }
}
```


---

## 4. net.hpp - sockpp 集成层

**概述**: sockpp 库的薄封装，提供 osp::expected 错误处理。

**头文件**: `include/osp/net.hpp`

**依赖**: `vocabulary.hpp`, sockpp (仅在 OSP_HAS_SOCKPP 定义时可用)

**平台**: Linux / macOS (需要 sockpp)

### NetError

| 值 | 说明 |
|---|---|
| `kConnectFailed` | 连接失败 |
| `kBindFailed` | 绑定失败 |
| `kListenFailed` | 监听失败 |
| `kAcceptFailed` | 接受连接失败 |
| `kSendFailed` | 发送失败 |
| `kRecvFailed` | 接收失败 |
| `kTimeout` | 超时 |
| `kClosed` | 已关闭 |
| `kInvalidAddress` | 无效地址 |

### TcpClient

**概述**: TCP 客户端连接封装 (sockpp::tcp_socket)。

#### 静态工厂方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `static expected<TcpClient, NetError> Connect(const char* host, uint16_t port, int32_t timeout_ms = 5000)` | 连接到远程服务器 | 线程安全 |

#### 公共方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `expected<size_t, NetError> Send(const void* data, size_t len)` | 发送数据 | 非线程安全 |
| `expected<size_t, NetError> Recv(void* buf, size_t len)` | 接收数据 | 非线程安全 |
| `int Fd() const` | 返回文件描述符 | 线程安全 |
| `bool IsOpen() const` | 检查连接是否打开 | 线程安全 |
| `void Close()` | 关闭连接 | 非线程安全 |

### TcpServer

**概述**: TCP 服务端监听器封装 (sockpp::tcp_acceptor)。

#### 静态工厂方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `static expected<TcpServer, NetError> Listen(uint16_t port, int32_t backlog = 16)` | 创建监听器 | 线程安全 |

#### 公共方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `expected<TcpClient, NetError> Accept()` | 接受连接 (阻塞) | 非线程安全 |
| `int Fd() const` | 返回文件描述符 | 线程安全 |
| `bool IsOpen() const` | 检查是否监听中 | 线程安全 |
| `void Close()` | 关闭监听器 | 非线程安全 |

### UdpPeer

**概述**: UDP 套接字封装 (sockpp::udp_socket)。

#### 静态工厂方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `static expected<UdpPeer, NetError> Bind(uint16_t port)` | 绑定到端口 | 线程安全 |
| `static expected<UdpPeer, NetError> Create()` | 创建未绑定套接字 | 线程安全 |

#### 公共方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `expected<size_t, NetError> SendTo(const void* data, size_t len, const char* host, uint16_t port)` | 发送数据报 | 非线程安全 |
| `expected<size_t, NetError> RecvFrom(void* buf, size_t len, char* from_host, size_t host_len, uint16_t* from_port)` | 接收数据报 | 非线程安全 |
| `int Fd() const` | 返回文件描述符 | 线程安全 |
| `void Close()` | 关闭套接字 | 非线程安全 |

**使用示例**:

```cpp
auto client = net::TcpClient::Connect("127.0.0.1", 8080).value();
client.Send("hello", 5);
```

---

## 5. transport.hpp - 网络传输层

**概述**: 透明网络传输层，提供帧化 TCP 传输、序列化、NetworkNode 集成。

**头文件**: `include/osp/transport.hpp`

**依赖**: `platform.hpp`, `vocabulary.hpp`, `socket.hpp`, `bus.hpp`, `node.hpp`, `spsc_ringbuffer.hpp`

**平台**: Linux / macOS

### 错误枚举

#### TransportError

| 值 | 说明 |
|---|---|
| `kConnectionFailed` | 连接失败 |
| `kBindFailed` | 绑定失败 |
| `kSendFailed` | 发送失败 |
| `kRecvFailed` | 接收失败 |
| `kSerializationError` | 序列化错误 |
| `kInvalidFrame` | 无效帧 |
| `kBufferFull` | 缓冲区满 |
| `kNotConnected` | 未连接 |

### Endpoint

**概述**: 网络端点描述符 (host + port)。

| 字段 | 类型 | 说明 |
|---|---|---|
| `host` | `FixedString<63>` | 主机地址 |
| `port` | `uint16_t` | 端口号 |

| 方法签名 | 说明 |
|---|---|
| `static Endpoint FromString(const char* addr, uint16_t p)` | 从字符串创建端点 |

### 帧协议常量

| 常量 | 值 | 说明 |
|---|---|---|
| `kFrameMagicV0` | 0x4F535000 | v0 魔数 ("OSP\0") |
| `kFrameMagicV1` | 0x4F535001 | v1 魔数 ("OSP\1") |
| `kFrameMagic` | `kFrameMagicV0` | 默认魔数 |

### FrameHeader

**概述**: v0 帧头 (14 字节)。

| 字段 | 类型 | 说明 |
|---|---|---|
| `magic` | `uint32_t` | 魔数 |
| `length` | `uint32_t` | 负载长度 |
| `type_index` | `uint16_t` | 变体类型索引 |
| `sender_id` | `uint32_t` | 发送者 ID |

### FrameHeaderV1

**概述**: v1 帧头 (26 字节)，增加序列号和时间戳。

| 字段 | 类型 | 说明 |
|---|---|---|
| `magic` | `uint32_t` | 魔数 (0x4F535001) |
| `length` | `uint32_t` | 负载长度 |
| `type_index` | `uint16_t` | 变体类型索引 |
| `sender_id` | `uint32_t` | 发送者 ID |
| `seq_num` | `uint32_t` | 序列号 |
| `timestamp_ns` | `uint64_t` | 时间戳 (纳秒) |

### Serializer<T>

**概述**: 默认 POD 序列化器 (memcpy)，可为自定义类型特化。

#### 静态方法

| 方法签名 | 说明 |
|---|---|
| `static uint32_t Serialize(const T& obj, void* buf, uint32_t buf_size)` | 序列化对象到缓冲区 |
| `static bool Deserialize(const void* buf, uint32_t size, T& out)` | 从缓冲区反序列化 |

### SequenceTracker

**概述**: 序列号跟踪器，用于丢包/乱序检测。

#### 公共方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `bool Track(uint32_t seq_num)` | 跟踪序列号 | 非线程安全 |
| `void Reset()` | 重置计数器 | 非线程安全 |
| `uint64_t TotalReceived() const` | 总接收包数 | 线程安全 |
| `uint64_t LostCount() const` | 丢包数 | 线程安全 |
| `uint64_t ReorderedCount() const` | 乱序包数 | 线程安全 |
| `uint64_t DuplicateCount() const` | 重复包数 | 线程安全 |

### FrameCodec

**概述**: 帧头编解码器 (手动序列化)。

#### 静态方法

| 方法签名 | 说明 |
|---|---|
| `static uint32_t EncodeHeader(const FrameHeader& hdr, uint8_t* buf, uint32_t buf_size)` | 编码 v0 帧头 |
| `static bool DecodeHeader(const uint8_t* buf, uint32_t buf_size, FrameHeader& hdr)` | 解码 v0 帧头 |
| `static uint32_t EncodeHeaderV1(const FrameHeaderV1& hdr, uint8_t* buf, uint32_t buf_size)` | 编码 v1 帧头 |
| `static bool DecodeHeaderV1(const uint8_t* buf, uint32_t buf_size, FrameHeaderV1& hdr)` | 解码 v1 帧头 |
| `static uint8_t DetectVersion(const uint8_t* buf, uint32_t buf_size)` | 检测帧版本 |

#### 常量

| 常量 | 值 | 说明 |
|---|---|---|
| `kHeaderSize` | 14 | v0 帧头大小 |
| `kHeaderSizeV1` | 26 | v1 帧头大小 |

### TcpTransport

**概述**: 帧化 TCP 连接管理器，支持客户端和服务端模式。

#### 配置宏

| 宏 | 默认值 | 说明 |
|---|---|---|
| `OSP_TRANSPORT_MAX_FRAME_SIZE` | 4096 | 最大帧大小 |
| `OSP_TRANSPORT_RECV_RING_DEPTH` | 32 | 接收环形缓冲深度 |

#### 公共方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `expected<void, TransportError> Connect(const Endpoint& ep)` | 连接到远程端点 (客户端) | 非线程安全 |
| `void AcceptFrom(TcpSocket&& sock)` | 接受已连接套接字 (服务端) | 非线程安全 |
| `expected<void, TransportError> SendFrame(uint16_t type_index, uint32_t sender_id, const void* payload, uint32_t payload_len)` | 发送 v0 帧 | 非线程安全 |
| `expected<void, TransportError> SendFrameV1(uint16_t type_index, uint32_t sender_id, uint32_t seq_num, uint64_t timestamp_ns, const void* payload, uint32_t payload_len)` | 发送 v1 帧 | 非线程安全 |
| `expected<uint32_t, TransportError> RecvFrame(FrameHeader& hdr, void* payload_buf, uint32_t buf_size)` | 接收 v0 帧 (阻塞) | 非线程安全 |
| `expected<uint32_t, TransportError> RecvFrameAuto(FrameHeaderV1& hdr_v1, void* payload_buf, uint32_t buf_size)` | 接收帧 (自动检测版本) | 非线程安全 |
| `bool IsConnected() const` | 检查连接状态 | 线程安全 |
| `void Close()` | 关闭连接 | 非线程安全 |

### NetworkNode<PayloadVariant>

**概述**: 扩展 Node，支持透明远程 Pub/Sub over TCP。

**模板参数**:
- `PayloadVariant`: 消息负载变体类型 (std::variant<...>)

#### 公共方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `template<typename T> expected<void, TransportError> AdvertiseTo(const Endpoint& ep)` | 向远程端点发布类型 T 消息 | 非线程安全 |
| `template<typename T> expected<void, TransportError> SubscribeFrom(const Endpoint& ep)` | 从远程端点订阅类型 T 消息 | 非线程安全 |
| `uint32_t ReceiveToBuffer()` | 接收帧到环形缓冲 (I/O 线程) | 非线程安全 |
| `uint32_t DispatchFromBuffer()` | 从缓冲分发消息 (调度线程) | 非线程安全 |
| `uint32_t ProcessRemote()` | 处理远程消息 (单线程) | 非线程安全 |
| `expected<void, TransportError> Listen(uint16_t port)` | 监听端口 | 非线程安全 |
| `expected<void, TransportError> AcceptOne()` | 接受一个连接 | 非线程安全 |
| `int ListenerFd() const` | 返回监听器 fd | 线程安全 |
| `bool IsListening() const` | 检查是否监听中 | 线程安全 |
| `uint32_t RemotePublisherCount() const` | 远程发布者数量 | 线程安全 |
| `uint32_t RemoteSubscriberCount() const` | 远程订阅者数量 | 线程安全 |
| `uint64_t RecvRingDrops() const` | 接收环丢包数 | 线程安全 |

**使用示例**:

```cpp
using Payload = std::variant<SensorData, ControlCmd>;
NetworkNode<Payload> node("remote_node", 1);
node.AdvertiseTo<SensorData>(Endpoint::FromString("192.168.1.100", 9000));
node.ProcessRemote();  // 轮询远程消息
```

---

## 6. shm_transport.hpp - 共享内存 IPC

**概述**: 共享内存 IPC 传输，提供无锁 MPSC 环形缓冲和命名通道。

**头文件**: `include/osp/shm_transport.hpp`

**依赖**: `platform.hpp`, `vocabulary.hpp`

**平台**: Linux (POSIX shm_open/mmap)

### 错误枚举

#### ShmError

| 值 | 说明 |
|---|---|
| `kCreateFailed` | 创建失败 |
| `kOpenFailed` | 打开失败 |
| `kMapFailed` | 映射失败 |
| `kFull` | 缓冲区满 |
| `kEmpty` | 缓冲区空 |
| `kTimeout` | 超时 |
| `kClosed` | 已关闭 |

### 配置宏

| 宏 | 默认值 | 说明 |
|---|---|---|
| `OSP_SHM_SLOT_SIZE` | 4096 | 槽大小 (字节) |
| `OSP_SHM_SLOT_COUNT` | 256 | 槽数量 (必须是 2 的幂) |
| `OSP_SHM_CHANNEL_NAME_MAX` | 64 | 通道名最大长度 |

### SharedMemorySegment

**概述**: POSIX 共享内存段 RAII 封装 (shm_open/mmap)。

#### 静态工厂方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `static expected<SharedMemorySegment, ShmError> Create(const char* name, uint32_t size)` | 创建共享内存段 | 线程安全 |
| `static expected<SharedMemorySegment, ShmError> CreateOrReplace(const char* name, uint32_t size)` | 创建或替换共享内存段 | 线程安全 |
| `static expected<SharedMemorySegment, ShmError> Open(const char* name)` | 打开已存在的共享内存段 | 线程安全 |

#### 公共方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `void Unlink()` | 标记删除 (shm_unlink) | 非线程安全 |
| `void* Data()` | 返回数据指针 | 线程安全 |
| `const void* Data() const` | 返回只读数据指针 | 线程安全 |
| `uint32_t Size() const` | 返回大小 | 线程安全 |
| `const char* Name() const` | 返回名称 | 线程安全 |

### ShmRingBuffer<SlotSize, SlotCount>

**概述**: 无锁 MPSC 环形缓冲，用于共享内存。

**模板参数**:
- `SlotSize`: 槽大小 (默认 4096)
- `SlotCount`: 槽数量 (默认 256，必须是 2 的幂)

#### 静态方法

| 方法签名 | 说明 |
|---|---|
| `static ShmRingBuffer* InitAt(void* shm_addr)` | 在共享内存地址初始化 |
| `static ShmRingBuffer* AttachAt(void* shm_addr)` | 附加到已存在的缓冲 |
| `static constexpr uint32_t Size()` | 返回所需大小 |

#### 公共方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `bool TryPush(const void* data, uint32_t size)` | 尝试推送数据 (非阻塞) | 多生产者安全 |
| `bool TryPop(void* data, uint32_t& size)` | 尝试弹出数据 (非阻塞) | 单消费者 |
| `uint32_t Depth() const` | 返回当前深度 (近似) | 线程安全 |

### ShmChannel<SlotSize, SlotCount>

**概述**: 命名共享内存通道，结合 SharedMemorySegment + ShmRingBuffer。

**模板参数**:
- `SlotSize`: 槽大小 (默认 4096)
- `SlotCount`: 槽数量 (默认 256)

#### 静态工厂方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `static expected<ShmChannel, ShmError> CreateWriter(const char* name)` | 创建写端点 | 线程安全 |
| `static expected<ShmChannel, ShmError> CreateOrReplaceWriter(const char* name)` | 创建或替换写端点 | 线程安全 |
| `static expected<ShmChannel, ShmError> OpenReader(const char* name)` | 打开读端点 | 线程安全 |

#### 公共方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `expected<void, ShmError> Write(const void* data, uint32_t size)` | 写入数据 | 多写者安全 |
| `expected<void, ShmError> Read(void* data, uint32_t& size)` | 读取数据 | 单读者 |
| `expected<void, ShmError> WaitReadable(uint32_t timeout_ms)` | 等待可读 (轮询) | 单读者 |
| `void Notify()` | 通知读者 (空操作) | 线程安全 |
| `void Unlink()` | 删除通道 (写端点) | 非线程安全 |
| `uint32_t Depth() const` | 返回深度 | 线程安全 |

**使用示例**:

```cpp
// 写端
auto writer = ShmChannel<>::CreateWriter("my_channel").value();
writer.Write("hello", 5);

// 读端
auto reader = ShmChannel<>::OpenReader("my_channel").value();
uint8_t buf[4096];
uint32_t size;
reader.WaitReadable(1000);
reader.Read(buf, size);
```


---

## 7. serial_transport.hpp - 工业级串口传输

**概述**: 工业级串口传输，支持帧化协议、CRC-CCITT、序列跟踪、ACK/重传、健康监控。

**头文件**: `include/osp/serial_transport.hpp`

**依赖**: `platform.hpp`, `vocabulary.hpp`, `spsc_ringbuffer.hpp`

**平台**: Linux / macOS (POSIX termios)

**标准合规**: MISRA C++, IEC 61508

### 错误枚举

#### SerialError

| 值 | 说明 |
|---|---|
| `kOpenFailed` | 打开失败 |
| `kConfigFailed` | 配置失败 |
| `kSendFailed` | 发送失败 |
| `kRecvFailed` | 接收失败 |
| `kCrcError` | CRC 错误 |
| `kTimeout` | 超时 |
| `kFrameOversize` | 帧过大 |
| `kPortNotOpen` | 端口未打开 |
| `kAckTimeout` | ACK 超时 |
| `kRateLimitExceeded` | 速率限制超出 |
| `kHealthCheckFailed` | 健康检查失败 |

### SerialPortHealth

**概述**: 端口健康状态 (IEC 61508)。

| 值 | 说明 |
|---|---|
| `kHealthy` | 正常运行 |
| `kDegraded` | 错误率超过警告阈值 |
| `kFailed` | 错误率超过临界阈值或无通信 |

### 帧协议常量

| 常量 | 值 | 说明 |
|---|---|---|
| `kSerialSyncWord` | 0xAA55 | 同步字 |
| `kSerialMagic` | 0x4F53 | 魔数 ("OS") |
| `kSerialAckMagic` | 0x4F41 | ACK 魔数 ("OA") |
| `kSerialTailByte` | 0x0D | 尾字节 |
| `kSerialHeaderSize` | 10 | 帧头大小 |
| `kSerialTrailerSize` | 3 | 帧尾大小 (CRC + tail) |
| `kSerialMinFrameSize` | 13 | 最小帧大小 |
| `kSerialAckFrameSize` | 8 | ACK 帧大小 |

### 配置宏

| 宏 | 默认值 | 说明 |
|---|---|---|
| `OSP_SERIAL_MAX_FRAME_SIZE` | 1024 | 最大帧大小 |
| `OSP_SERIAL_RX_RING_SIZE` | 4096 | 接收环形缓冲大小 |

### ReliabilityConfig

**概述**: 可靠性配置结构。

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `enable_ack` | `bool` | false | 启用 ACK |
| `ack_timeout_ms` | `uint32_t` | 100 | ACK 超时 (毫秒) |
| `max_retries` | `uint8_t` | 3 | 最大重试次数 |
| `enable_seq_check` | `bool` | true | 启用序列检查 |

### SerialConfig

**概述**: 串口配置结构 (IEC 61508 健康监控)。

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `port_name` | `FixedString<63>` | "/dev/ttyS0" | 端口名 |
| `baud_rate` | `uint32_t` | 115200 | 波特率 |
| `data_bits` | `uint8_t` | 8 | 数据位 |
| `stop_bits` | `uint8_t` | 1 | 停止位 |
| `parity` | `uint8_t` | 0 | 校验 (0=None, 1=Odd, 2=Even) |
| `flow_control` | `uint8_t` | 0 | 流控 (0=None, 1=HW, 2=SW) |
| `inter_byte_timeout_ms` | `uint32_t` | 50 | 字节间超时 |
| `frame_max_size` | `uint32_t` | 1024 | 最大帧大小 |
| `reliability` | `ReliabilityConfig` | - | 可靠性配置 |
| `watchdog_timeout_ms` | `uint32_t` | 5000 | 看门狗超时 |
| `error_rate_window` | `uint32_t` | 100 | 错误率窗口大小 |
| `degraded_error_threshold` | `uint32_t` | 10 | 降级错误阈值 |
| `failed_error_threshold` | `uint32_t` | 30 | 失败错误阈值 |
| `max_frames_per_second` | `uint32_t` | 1000 | 最大帧率 (0=无限制) |
| `write_retry_count` | `uint32_t` | 3 | 写重试次数 |
| `write_retry_delay_us` | `uint32_t` | 1000 | 写重试延迟 (微秒) |

### SerialStatistics

**概述**: 统计信息结构 (IEC 61508 错误跟踪)。

| 字段 | 类型 | 说明 |
|---|---|---|
| `frames_sent` | `uint64_t` | 发送帧数 |
| `frames_received` | `uint64_t` | 接收帧数 |
| `bytes_sent` | `uint64_t` | 发送字节数 |
| `bytes_received` | `uint64_t` | 接收字节数 |
| `crc_errors` | `uint64_t` | CRC 错误数 |
| `sync_errors` | `uint64_t` | 同步错误数 |
| `timeout_errors` | `uint64_t` | 超时错误数 |
| `oversize_errors` | `uint64_t` | 过大帧错误数 |
| `seq_gaps` | `uint64_t` | 序列间隙数 |
| `retransmits` | `uint64_t` | 重传次数 |
| `ack_timeouts` | `uint64_t` | ACK 超时数 |
| `rate_limit_drops` | `uint64_t` | 速率限制丢弃数 |
| `write_retries` | `uint64_t` | 写重试次数 |

### SerialRxCallback

**概述**: 接收回调函数类型。

```cpp
using SerialRxCallback = void (*)(const void* payload, uint32_t size,
                                   uint16_t type_index, uint16_t seq, void* ctx);
```

### Crc16Ccitt

**概述**: CRC-CCITT 计算器 (多项式 0x1021)。

#### 静态方法

| 方法签名 | 说明 |
|---|---|
| `static uint16_t Calculate(const void* data, uint32_t size)` | 计算 CRC-CCITT |

### SerialTransport

**概述**: 工业级串口传输，支持健康监控和速率限制。

#### 构造函数

| 方法签名 | 说明 |
|---|---|
| `explicit SerialTransport(const SerialConfig& cfg)` | 构造串口传输 |

#### 公共方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `expected<void, SerialError> Open()` | 打开串口 | 非线程安全 |
| `void Close()` | 关闭串口 | 非线程安全 |
| `bool IsOpen() const` | 检查是否打开 | 线程安全 |
| `expected<void, SerialError> Send(uint16_t type_index, const void* payload, uint32_t size)` | 发送帧 | 非线程安全 |
| `uint32_t Poll()` | 轮询接收 (单线程) | 非线程安全 |
| `uint32_t ReadToRing()` | 读取到环形缓冲 (I/O 线程) | 非线程安全 |
| `uint32_t ParseFromRing()` | 从环形缓冲解析 (解析线程) | 非线程安全 |
| `void SetRxCallback(SerialRxCallback cb, void* ctx = nullptr)` | 设置接收回调 | 非线程安全 |
| `int GetFd() const` | 返回文件描述符 | 线程安全 |
| `SerialStatistics GetStatistics() const` | 获取统计信息 | 线程安全 |
| `void ResetStatistics()` | 重置统计信息 | 非线程安全 |
| `SerialPortHealth GetHealth() const` | 获取健康状态 | 线程安全 |
| `bool IsHealthy() const` | 检查是否健康 | 线程安全 |

**使用示例**:

```cpp
SerialConfig cfg;
cfg.port_name = "/dev/ttyUSB0";
cfg.baud_rate = 115200;
SerialTransport serial(cfg);
serial.Open();
serial.SetRxCallback([](const void* data, uint32_t size, uint16_t type, uint16_t seq, void* ctx) {
  // 处理接收数据
}, nullptr);
serial.Send(1, "hello", 5);
serial.Poll();
```

---

## 8. transport_factory.hpp - 传输工厂

**概述**: 自动传输选择 (inproc/shm/tcp/unix)，提供传输配置和解析。

**头文件**: `include/osp/transport_factory.hpp`

**依赖**: `platform.hpp`, `vocabulary.hpp`

### TransportType

**概述**: 传输类型枚举。

| 值 | 说明 |
|---|---|
| `kInproc` | 进程内 (同进程，无锁队列) |
| `kShm` | 共享内存 (本地主机，零拷贝) |
| `kTcp` | TCP 套接字 (远程主机，网络) |
| `kUnix` | Unix 域套接字 (本地主机，流式) |
| `kAuto` | 自动检测 |

### TransportConfig

**概述**: 传输配置结构。

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `type` | `TransportType` | `kAuto` | 传输类型 |
| `remote_host` | `FixedString<63>` | "127.0.0.1" | 远程主机 |
| `remote_port` | `uint16_t` | 0 | 远程端口 |
| `shm_channel_name` | `FixedString<63>` | "" | 共享内存通道名 |
| `shm_slot_size` | `uint32_t` | 4096 | 共享内存槽大小 |
| `shm_slot_count` | `uint32_t` | 256 | 共享内存槽数量 |
| `unix_path` | `FixedString<107>` | "" | Unix 域套接字路径 |

### TransportFactory

**概述**: 传输工厂，提供自动类型检测。

#### 静态方法

| 方法签名 | 说明 |
|---|---|
| `static TransportType DetectBestTransport(const TransportConfig& cfg)` | 检测最佳传输类型 |
| `static const char* TransportTypeName(TransportType type)` | 返回类型名称字符串 |

**检测逻辑**:
- 如果 `remote_host` 是 "127.0.0.1" 或 "localhost" 且 `unix_path` 非空 → `kUnix`
- 如果 `remote_host` 是 "127.0.0.1" 或 "localhost" 且 `shm_channel_name` 非空 → `kShm`
- 如果 `remote_host` 是 "127.0.0.1" 或 "localhost" 且 `shm_channel_name` 为空 → `kInproc`
- 否则 → `kTcp`

### TransportSelector<PayloadVariant>

**概述**: 传输选择器，管理配置和解析。

**模板参数**:
- `PayloadVariant`: 总线负载变体类型 (用于类型安全)

#### 公共方法

| 方法签名 | 说明 | 线程安全性 |
|---|---|---|
| `void Configure(const TransportConfig& cfg)` | 配置并解析类型 | 非线程安全 |
| `TransportType ResolvedType() const` | 返回解析后的类型 | 线程安全 |
| `bool IsLocal() const` | 检查是否本地传输 | 线程安全 |
| `bool IsRemote() const` | 检查是否远程传输 | 线程安全 |
| `const TransportConfig& Config() const` | 返回配置 | 线程安全 |

**使用示例**:

```cpp
TransportConfig cfg;
cfg.type = TransportType::kAuto;
cfg.remote_host = "127.0.0.1";
cfg.shm_channel_name = "my_channel";

auto type = TransportFactory::DetectBestTransport(cfg);
// type == TransportType::kShm

TransportSelector<MyPayload> selector;
selector.Configure(cfg);
if (selector.IsLocal()) {
  // 使用本地传输
}
```

---

## 总结

本文档覆盖了 newosp 项目网络与传输层的 8 个核心模块：

1. **socket.hpp**: POSIX 套接字 RAII 封装 (TCP/UDP/Unix)
2. **connection.hpp**: 固定容量连接池管理
3. **io_poller.hpp**: 统一 I/O 事件轮询 (epoll/kqueue)
4. **net.hpp**: sockpp 集成层 (可选)
5. **transport.hpp**: 帧化 TCP 传输 + NetworkNode
6. **shm_transport.hpp**: 共享内存 IPC (无锁 MPSC)
7. **serial_transport.hpp**: 工业级串口传输 (IEC 61508)
8. **transport_factory.hpp**: 自动传输选择

所有模块均为 header-only，兼容 `-fno-exceptions -fno-rtti`，遵循 MISRA C++ 和 Google C++ Style Guide。
