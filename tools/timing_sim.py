#!/usr/bin/env python3
"""
newosp 低速嵌入式平台时序模拟器

针对硬件场景 (CPU 100 MHz / DDR 400 MHz / 无 L2 / FULL BUS) 估算基础库
各操作在真实嵌入式平台上的周期/纳秒开销，识别潜在瓶颈并给出设计建议。

硬件参数:
  CPU   100 MHz   (10 ns/cycle)
  TIM   10 MHz    (慢定时器, 100 ns/cycle)
  DDR   400 MHz   (FULL BUS, ECC disabled, 1600 Mbps)
  AXI   400/180/180 MHz, APB 50 MHz
  Cache: 无 L2 (假设 L1 I/D 各 32KB, ~1 cycle 命中)

方法:
  对每个基础操作, 列出典型指令流, 用 cycle 计数模型估算:
  - L1 命中: 1 cycle
  - DDR 访问: ~20-40 cycles (DDR 400MHz, 未命中缓存)
  - 原子 RMW (ldrex/strex 循环): ~5-15 cycles/次 (独占访问串行化)
  - 系统调用 (clock_gettime/yield): ~300-1000 cycles (含 syscall 开销)
"""

from dataclasses import dataclass

# ============================ 硬件模型 ============================
@dataclass
class HwModel:
    cpu_mhz: int = 100
    ddr_mhz: int = 400
    has_l2: bool = False
    l1_hit_cycles: int = 1
    ddr_access_cycles: int = 30      # 未命中缓存读 64B 线
    atomic_rmw_cycles: int = 10      # ldrex/strex 独占访问
    syscall_cycles: int = 500        # clock_gettime/yield/sched_yield
    memcpy_per_word_cycles: int = 2  # 连续突发拷贝

    @property
    def ns_per_cycle(self) -> float:
        return 1000.0 / self.cpu_mhz

    def cycles_to_ns(self, cycles: int) -> float:
        return cycles * self.ns_per_cycle


# ============================ 操作模拟 ============================
@dataclass
class OpEstimate:
    name: str
    cycles: int
    ns: float
    bottleneck: str
    note: str


def sim_bus_publish(hw: HwModel) -> OpEstimate:
    """AsyncBus::Publish: msg_id load + admission check + CAS + store"""
    # 关键路径: 2x relaxed load (prod/cached_cons) + 1x acquire load + 1x CAS + 1x store
    cycles = (
        2 * hw.l1_hit_cycles      # prod, cached_cons
        + hw.atomic_rmw_cycles    # CAS (ldrex/strex 循环, 无竞争首轮成功)
        + hw.atomic_rmw_cycles    # next_msg_id fetch_add
        + 1 * hw.l1_hit_cycles    # sequence store
        + 1 * hw.l1_hit_cycles    # header store
        + 2 * hw.memcpy_per_word_cycles  # envelope 拷贝 (小消息 8B)
    )
    return OpEstimate(
        "bus.Publish (小消息, 无竞争)",
        cycles, hw.cycles_to_ns(cycles),
        "原子 RMW 独占访问",
        "CAS 在无竞争时约 10 cycles; 多核共享 cache line 会放大",
    )


def sim_bus_process(hw: HwModel) -> OpEstimate:
    """ProcessBatch: sequence load + SharedSpinLock reader + dispatch"""
    cycles = (
        1 * hw.l1_hit_cycles       # sequence acquire load
        + 1 * hw.l1_hit_cycles     # lock_shared CAS (state 0->1)
        + hw.l1_hit_cycles         # callback table read
        + hw.l1_hit_cycles         # FixedFunction invoker call
        + 1 * hw.l1_hit_cycles     # sequence release store
        + hw.l1_hit_cycles         # unlock_shared (state 1->0)
    )
    return OpEstimate(
        "bus.ProcessBatch (每条消息)",
        cycles, hw.cycles_to_ns(cycles),
        "SharedSpinLock reader CAS",
        "无竞争时 reader lock 约 1-2 cycles, 热路径可接受",
    )


def sim_spsc_push(hw: HwModel) -> OpEstimate:
    """spsc_ringbuffer.Push: head/tail check + store + release"""
    cycles = (
        1 * hw.l1_hit_cycles       # head load
        + 1 * hw.l1_hit_cycles     # tail acquire load
        + 2 * hw.memcpy_per_word_cycles  # data store (8B)
        + 1 * hw.l1_hit_cycles     # head release store
    )
    return OpEstimate(
        "spsc.Push (SPSC, 无竞争)",
        cycles, hw.cycles_to_ns(cycles),
        "纯 L1 命中",
        "wait-free, 无锁; 是基础库最优秀的操作",
    )


def sim_spinlock_contended(hw: HwModel) -> OpEstimate:
    """SpinLock::lock (有竞争, 进入 backoff)"""
    # 有竞争: 首轮 CAS 失败 + backoff 循环 (1..64 CpuRelax) + yield
    cycles = (
        hw.atomic_rmw_cycles       # 首轮 CAS 失败
        + sum(1 << i for i in range(6))  # CpuRelax 1+2+4+8+16+32 = 63
        + hw.l1_hit_cycles * 63    # 每次 relax 1 cycle
        + hw.syscall_cycles        # yield (达到阈值 64 后)
    )
    return OpEstimate(
        "SpinLock::lock (有竞争, backoff+yield)",
        cycles, hw.cycles_to_ns(cycles),
        "yield 系统调用",
        "backoff 先自旋 ~63 cycles 再 yield; 单核下 yield 才能让持锁线程跑",
    )


def sim_spinlock_uncontended(hw: HwModel) -> OpEstimate:
    """SpinLock::lock (无竞争, 首轮成功)"""
    cycles = hw.atomic_rmw_cycles  # 首轮 test_and_set 成功
    return OpEstimate(
        "SpinLock::lock (无竞争)",
        cycles, hw.cycles_to_ns(cycles),
        "test_and_set",
        "yield 不触发, 零额外开销 (阈值 64 保护)",
    )


def sim_clock_syscall(hw: HwModel) -> OpEstimate:
    """SteadyNowUs 最坏情况: 内核禁用 vDSO, 走真实 syscall"""
    cycles = hw.syscall_cycles + 1
    return OpEstimate(
        "SteadyNowUs (最坏: 无 vDSO, 真 syscall)",
        cycles, hw.cycles_to_ns(cycles),
        "系统调用 ~500 cycles",
        "嵌入式内核常禁用 vDSO; 此时每次取时钟 5us, 热路径不可接受",
    )


def sim_clock_vdso(hw: HwModel) -> OpEstimate:
    """SteadyNowUs: vDSO 路径 (host 实测 22ns / 2.2 cycles @1GHz 等效)"""
    # host 实测: steady_clock::now 25.3ns, clock_gettime(MONOTONIC) 22.4ns
    # 折算到 100MHz: vDSO 内部有 64 位乘法+移位做频率换算, 约 40 cycles
    cycles = 40
    return OpEstimate(
        "SteadyNowUs (vDSO, host 实测 22ns)",
        cycles, hw.cycles_to_ns(cycles),
        "vDSO read-only page",
        "glibc 默认走 vDSO; 100MHz 上仍需 ~40 cycles 做频率换算",
    )


def sim_clock_coarse(hw: HwModel) -> OpEstimate:
    """CoarseNowUs: CLOCK_MONOTONIC_COARSE (host 实测 6ns)"""
    # host 实测 6.0ns vs SteadyNowNs 21.9ns (3.6x)
    # COARSE 直接读内核维护的 jiffies 快照, 无频率换算乘法
    cycles = 11
    return OpEstimate(
        "CoarseNowUs (COARSE, host 实测 6ns)",
        cycles, hw.cycles_to_ns(cycles),
        "纯内存读, 无乘法换算",
        "分辨率 ~4ms; 适用于消息时间戳/日志/心跳, 不适用于延迟测量",
    )


def sim_fixedvector_copy(hw: HwModel) -> OpEstimate:
    """FixedVector 拷贝 16 个 8B 元素: 逐元素 vs memcpy"""
    n, elem = 16, 8
    per_element = hw.l1_hit_cycles + hw.memcpy_per_word_cycles  # placement-new + store
    elementwise = n * per_element
    bulk = n * hw.memcpy_per_word_cycles + hw.l1_hit_cycles
    return OpEstimate(
        f"FixedVector 拷贝 {n}x{elem}B: memcpy vs 逐元素",
        bulk, hw.cycles_to_ns(bulk),
        f"memcpy 省 {elementwise-bulk} cycles",
        f"逐元素: {elementwise} cycles, memcpy: {bulk} cycles ({100*(elementwise-bulk)/elementwise:.0f}% 省)",
    )


def sim_async_log(hw: HwModel) -> OpEstimate:
    """async_log: 每条日志 (timestamp + vsnprintf + spsc push)"""
    # LogEntry 320B = 5 cache line; vsnprintf 是真正主导项
    vsnprintf_cycles = 800          # 格式化 + 边界检查, 保守估计
    entry_copy = 320 // 4 * hw.memcpy_per_word_cycles  # 320B push 进 SPSC
    cycles = 11 + vsnprintf_cycles + entry_copy + 2 * hw.l1_hit_cycles
    return OpEstimate(
        "async_log 每条 (CoarseNowNs + vsnprintf)",
        cycles, hw.cycles_to_ns(cycles),
        "vsnprintf 格式化",
        "时钟已降到 11 cycles; 现在 vsnprintf(~800) 和 320B 拷贝(~160) 主导",
    )


# ============================ 运行 ============================
def main() -> None:
    hw = HwModel()

    print("=" * 78)
    print("newosp 基础库时序模拟 — 硬件: CPU 100MHz / DDR 400MHz / 无 L2")
    print("=" * 78)
    print(f"  CPU 100MHz = 10ns/cycle | DDR 访问 ~{hw.ddr_access_cycles} cycles"
          f" | 原子RMW ~{hw.atomic_rmw_cycles} cycles | syscall ~{hw.syscall_cycles} cycles")
    print("-" * 78)
    print(f"{'操作':<46}{'cycles':>8}{'ns':>10}   瓶颈")
    print("-" * 78)

    ops = [
        sim_bus_publish(hw),
        sim_bus_process(hw),
        sim_spsc_push(hw),
        sim_spinlock_uncontended(hw),
        sim_spinlock_contended(hw),
        sim_clock_syscall(hw),
        sim_clock_vdso(hw),
        sim_clock_coarse(hw),
        sim_async_log(hw),
        sim_fixedvector_copy(hw),
    ]

    for op in ops:
        print(f"{op.name:<46}{op.cycles:>8}{op.ns:>10.0f}   {op.bottleneck}")
        if op.note:
            print(f"{'':<46}         (注: {op.note})")

    print("-" * 78)
    print("\n== host 实测校准 (x86_64, glibc vDSO 可用) ==")
    print("  steady_clock::now()            25.3 ns")
    print("  clock_gettime(MONOTONIC)       22.4 ns   <- vDSO, 非 syscall")
    print("  clock_gettime(MONOTONIC_COARSE) 8.7 ns   <- 无频率换算乘法")
    print("  结论: host 上时钟从未走 syscall; 但嵌入式内核常禁用 vDSO,")
    print("        此时同一份代码退化到 ~5us/次, 相差 200 倍。")
    print("\n== 关键发现 ==")
    print("1. 时钟是唯一会因平台配置退化 200 倍的操作")
    print("   - vDSO 可用: 22ns; vDSO 禁用: 5000ns")
    print("   - 用 COARSE 可把最坏情况也压住 (内核 jiffies 快照, 无换算)")
    print("2. bus.Publish 无竞争 28 cycles = 280ns, 时钟不再主导")
    print("   - 改用 CoarseNowUs 后 timestamp 仅 11 cycles (原 500)")
    print("3. async_log 真正瓶颈是 vsnprintf(~800 cycles), 不是时钟")
    print("   - 320B LogEntry 拷贝 ~160 cycles 也不可忽略")
    print("4. SpinLock yield 阈值 64: 无竞争零开销, 单核下 yield 必要")
    print("5. 无 L2 cache: DDR 未命中 ~30 cycles (300ns)")
    print("   - 环形缓冲应保持 L1 常驻 (小 slot), 避免 DDR 回写")
    print("\n== 已实施 ==")
    print("A. [完成] 新增 CoarseNowUs/CoarseNowNs (CLOCK_MONOTONIC_COARSE)")
    print("B. [完成] bus.Publish 系列改用 CoarseNowUs (消息时间戳无需 us 精度)")
    print("C. [完成] async_log 改用 CoarseNowNs (实测 22ns -> 6ns)")
    print("D. [保留] SpinLock yield 阈值 / spsc slot L1 对齐 / FixedVector memcpy")
    print("\n== 未采纳 ==")
    print("E. 内联汇编读 CNTPCT_EL0: 特权指令 + 硬编码频率, 牺牲可移植性")
    print("F. 抽 vsnprintf 到非内联 helper 减体积: 实测 -Os 下 GCC 已自动 outline,")
    print("   20 调用点仅差 29 字节, 无收益")
    print("\n== 剩余风险 (需真机验证) ==")
    print("G. SteadyNowUs 仍用于延迟测量/超时判断, 这些路径必须保持高精度")
    print("H. COARSE 分辨率 ~4ms: 若目标内核 CONFIG_HZ=100 则为 10ms")


if __name__ == "__main__":
    main()
