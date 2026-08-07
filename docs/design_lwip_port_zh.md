# lwIP 移植评估

> 结论：**可行，非零改动**。lwIP 的 BSD socket 兼容层能覆盖约 80% 的 POSIX 依赖，硬缺口：AF_UNIX、epoll、sockpp。推荐"OSP 网络后端抽象"（方案 B），改动收敛在 socket.hpp / io_poller.hpp。
>
> 目标环境：**RT-Thread + lwIP**（RT-Thread SAL/netdev 把 lwIP 作后端协议栈，`SAL_USING_POSIX` 时提供标准 POSIX socket 名字）。网络模块以嵌入式友好为默认目标。

## 1. 现状 POSIX 依赖面

| 模块 | 依赖 | 被谁使用 |
|---|---|---|
| socket.hpp | ::socket/connect/send/recv/bind/listen/accept/fcntl、AF_INET/AF_UNIX、MSG_NOSIGNAL、TCP_NODELAY、O_NONBLOCK | transport、node_manager |
| io_poller.hpp | epoll/kqueue/poll | 当前无任何模块使用 |
| net.hpp | sockpp（第三方，异常型）| 核心路径不编译 |
| discovery.hpp | 裸 POSIX UDP 组播（IP_ADD_MEMBERSHIP）| 服务发现 |
| service.hpp | ::socket/accept/recv/send + 独立 accept 线程 | 跨节点服务调用 |
| shell.hpp | TCP telnet（IAC 协商）| 调试 shell |

关键观察：阻塞线程模型（每连接一线程）与 lwIP 多线程 socket 天然兼容，无需改事件驱动；io_poller 无调用方，非阻塞项。

## 2. RT-Thread + lwIP 兼容性对照

对照面为 RT-Thread SAL + lwIP。

| POSIX | lwIP/SAL 状态 |
|---|---|
| socket/bind/listen/accept/connect/send/recv/sendto/recvfrom/close | 直接兼容 |
| select | 兼容（lwip_select，level-triggered）|
| setsockopt(SO_REUSEADDR/TCP_NODELAY/IP_ADD_MEMBERSHIP) | 支持（组播需 LWIP_IGMP）|
| errno、多线程阻塞 | 支持（需 LWIP_TCPIP_CORE_LOCKING）|
| fcntl(F_SETFL O_NONBLOCK) | lwip_fcntl 仅支持 O_NONBLOCK，需适配 |
| MSG_NOSIGNAL | 无信号概念，已收拢为 kSendNoSignal |
| inet_pton | 非 socket API，需 ipaddr_aton |
| AF_UNIX | 不支持，本地传输改 shm_transport |
| epoll | 无，改 select/poll |
| sockpp | 无法迁移，OSP_WITH_SOCKPP=OFF |

## 3. 移植方案对比

- **A lwIP 兼容宏**：LWIP_COMPAT_SOCKETS=1 直接同名，改 4 点。改动小，但条件编译散落、AF_UNIX 需业务侧屏蔽。
- **B OSP 后端抽象（推荐）**：`OSP_NET_BACKEND`（kLinux/kRtThread/kNone）收拢到 socket.hpp/io_poller.hpp，业务层零改动，加新后端成本低。
- **C 平台胶水层**：newosp 零改动，但需自研 AF_UNIX/epoll 模拟，成本高且不可复用。

推荐 B：AF_UNIX、epoll 两大缺口决定必须有明确后端边界。

## 4. 风险与限制

1. AF_UNIX 不可替代：本地通信依赖 shm_transport（Linux mmap，需移植 RT-Thread）。
2. lwIP 多线程正确性依赖 tcpip_thread + CORE_LOCKING 配置。
3. 组播依赖 LWIP_IGMP。
4. io_poller 换 select 后无 edge-triggered 语义（当前无调用方，风险可控）。
5. sockpp 后端必须关闭。
6. host 测试三条路径可验证后端：lwIP unixsim / RT-Thread QEMU（最贴近）/ lwIP 自带单测。

lwIP 后端测试已集成在 `tests/`（TCP/UDP 回环，自写 loopback netif 免 TAP/root），以外部库链接启用：

```bash
cmake -B build_lwip -DOSP_WITH_LWIP=ON \
  -DOSP_LWIP_INCLUDE_DIR="<lwip>/src/include;<lwip>/contrib/ports/unix/port/include;<lwip>/contrib/ports/unix/lib;<lwip>/contrib" \
  -DOSP_LWIP_LIBRARY=<liblwip>
```

lwIP 源码获取：github（codeload + 代理）2.2.0，或 gitee `mirrors/lwip`（2.1.x）+ `mirrors/lwip-contrib`；库需以 `LWIP_COMPAT_SOCKETS=0`、`LWIP_NETIF_LOOPBACK=1` 编译。后端适配层 `socket_api`（Linux 调系统函数、lwIP 调 `lwip_*`，无宏依赖）。

## 5. 嵌入式友好优化清单

| # | 优化项 |
|---|---|
| 1 | sockpp 默认 OFF |
| 2 | io_poller 去 EPOLLET（水平触发）|
| 3 | MSG_NOSIGNAL 收拢 kSendNoSignal |
| 4 | OSP_NET_BACKEND 后端宏 |
| 5 | inet_pton 地址解析抽象 |
| 6 | 连接超时 SetNonBlocking 可移植 |
| 7 | 单线程 select 事件驱动 |

## 6. 待决策点

1. RT-Thread 版本 + SAL_USING_POSIX 是否开启（决定 rt_* 还是标准 POSIX 名字）。
2. `<sys/socket.h>` 可用性：OSP_HAS_NETWORK 依赖 `__has_include`，RT-Thread 需确认或按平台强制。
3. 本地传输策略：shm_transport 在 RT-Thread 的移植。
4. platform.hpp 加 OSP_PLATFORM_RTTHREAD，确认 std::thread/chrono 在 RT-Thread 工具链可用性（网络层之外的全局问题）。
5. io_poller 在 RT-Thread 走 poll/select 后端。
