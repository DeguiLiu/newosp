# 大帧 shm IPC 传输现状

> 本文档描述共享内存 IPC 在大帧传输上的**当前实现状态**。核心事实：`ShmStore` 块池跨进程已是零拷贝；环形缓冲（`ShmRingBuffer`/`ShmSpscByteRing`/`ShmSpmcByteRing`）仍走 send/recv 双 memcpy，后者是**已知待改进**（见 §5）。
> 关联：`docs/design_data_dispatcher_zh.md`、`include/osp/data_dispatcher.hpp`、`include/osp/shm_transport.hpp`、`include/osp/opt.hpp`

## 1. 现状：块池单块跨进程零拷贝

`ShmStore`（`data_dispatcher.hpp`）提供跨进程共享数据块池，payload 传输全程**无拷贝**：

- `BlockSize` 是无上限模板参数（`template <uint32_t BlockSize, uint32_t MaxBlocks>`，`data_dispatcher.hpp:215`），单块容量按需指定，examples 已用 16016B 单块。
- `GetWritable(block_id)`（:802）/ `GetReadable(block_id)`（:843）返回块内裸指针；调用方在共享块上原地读写 payload，不存在 payload 拷贝点。
- 进程间同步用跨进程锁无关的 CAS + 引用计数（`DataBlock.refcount`），block_id 经通知通道送达消费端（`GetReadable(block_id, generation)` 代际重载已落地，:902，防陈旧 id 跨代串块）。
- 全仓库唯一 `memset` 是 shm 初始化清零（`data_dispatcher.hpp:365`），不在 payload 传输路径。

## 2. 现状：环形缓冲的 send/recv 双 memcpy

环形缓冲（`ShmSpscByteRing`/`ShmSpmcByteRing`）以字节流承载消息，send/recv 各一次 `memcpy`（`shm_transport.hpp`，write 路径 :366、read 路径 :405 附近）：

```cpp
// 写侧：把发送方缓冲拷贝进共享环
std::memcpy(target->data, data, size);
// 读侧：把共享环拷回接收方缓冲
std::memcpy(data, slot.data, size);
```

对 >15600B 的大帧，这两次 memcpy 是延时与缓存/内存带宽的主开销，且与帧长线性增长。

## 3. 替代方案与取舍

上一版方案的 G1 前提经评审不成立，已取消链式组帧：`BlockSize` 无上限，单块放大即可容纳大帧，无需 `struct pbuf` 式引用计数链。当前待改进点是 §2 的环形双 memcpy，替代路径为"大帧描述符 + 块池原地读"：

| 维度 | 块池路径（待改进目标） | 环形 memcpy（现状） |
|---|---|---|
| 大帧延迟 | 免 2 次 memcpy，缓存/DMA 友好 | 随帧长线性拷贝 |
| 小帧开销 | 描述符 + 池管理 | 最优 |
| 协议复杂度 | +1 描述符路径 | 无 |
| 生命周期 | 复用 refcount/超时/崩溃兜底 | 无需 |

## 4. 待改进（未实现）

按 §3 方向，环形缓冲"零拷贝 recv"**尚未实现**。落点为：环形缓冲识别描述符消息时返回共享块引用（经 `ShmStore` 与 bridge 连接）而非拷贝 payload。落地需处理 §6 风险。

## 5. 结论

- `ShmStore` 块池大帧跨进程**已零拷贝**，无需改造。
- 环形缓冲 send/recv **双 memcpy** 是唯一真实技术债，属待改进；消除方式为 §3 块池描述符路径，见 §4。

## 6. 风险与开放问题（供待改进落地时决策）

- **连接所有权**：块池由谁创建（bridge owner）、消费者如何取得 `consumer_id`/`holding_mask`（依赖 `ShmStore::ConsumerSlot`）。
- **描述符尺寸**：若引入更大描述符会破坏按字节背压（`consumer_fusion.cpp` 的 `5 * sizeof(NotifyMsg)`）——需保持 8B 或改按条数判定。
- **规模**：`MaxBlocks <= 64`（`holding_mask` 为 `uint64_t`）；大 `BlockSize` 时 shm 总量 = `stride × MaxBlocks` 需 mmap 供足。

## 关键文件

- `include/osp/data_dispatcher.hpp`: `BlockSize` 模板、`GetWritable/GetReadable/Release/Recycle`、`ConsumerSlot.holding_mask`
- `include/osp/shm_transport.hpp`: `ShmSpscByteRing`/`ShmSpmcByteRing` 的 send/recv 路径
- `include/osp/opt.hpp`: `OSP_SHM_SLOT_SIZE`（通知通道槽，非块容量）
