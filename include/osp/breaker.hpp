/**
 * MIT License
 *
 * Copyright (c) 2024 liudegui
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file breaker.hpp
 * @brief Per-component overload-degradation circuit breaker.
 *
 * Pure-logic five-state machine (Normal -> BrokenL1 -> BrokenL2 -> Safe ->
 * Recovering) packed into a single lock-free 32-bit atomic. No heap, no
 * platform dependency. Deploy one instance per component (AO, IO channel,
 * pipeline stage); feed it timeout/watermark/overflow/watchdog events from
 * the component's hot path and query the graded actions from the policy
 * layer.
 *
 * Degradation chain: consecutive direct timeouts -> BrokenL1 (direct revoked);
 * consecutive RTC timeouts, persistent high watermark or overflow ->
 * BrokenL2 (component quarantined); key-reserve exhaustion or watchdog ->
 * Safe. Recovery requires cooldown completion, sustained low watermark, and
 * N consecutive healthy probe windows - one good sample is not enough.
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_BREAKER_HPP_
#define OSP_BREAKER_HPP_

#include <cstdint>

#include <atomic>

namespace osp {

enum class BreakerLevel : uint8_t { Normal, BrokenL1, BrokenL2, Safe, Recovering };

/**
 * @brief Circuit breaker with graded degradation and cooldown-gated recovery.
 *
 * @tparam CooldownCycles Number of dispatch cycles a broken level must spend
 *                        cooling down before recovery may begin. Must fit in
 *                        16 bits.
 */
template <uint16_t CooldownCycles = 100U>
class Breaker {
 public:
  Breaker() noexcept;

  // -------------------------------------------------------------------------
  // Event inputs
  // -------------------------------------------------------------------------

  /** @brief Consecutive direct-path timeouts -> BrokenL1. */
  void OnDirectTimeout() noexcept;

  /** @brief Consecutive run-to-completion timeouts -> BrokenL2. */
  void OnRtcTimeout() noexcept;

  /** @brief One high-watermark sample (equivalently OnWatermark(81)). */
  void OnWatermarkViolation() noexcept;

  /** @brief Overflow of a bounded queue -> BrokenL2. */
  void OnOverflow() noexcept;

  /** @brief Critical capacity exhausted -> Safe. */
  void OnKeyReserveExhausted() noexcept;

  /** @brief Watchdog fired -> Safe. */
  void OnWatchdog() noexcept;

  /** @brief One dispatch cycle; advances cooldown and healthy windows. */
  void OnDispatchCycle() noexcept;

  /** @brief A controlled probe succeeded while Recovering. */
  void OnProbeSuccess() noexcept;

  /** @brief A controlled probe failed while Recovering -> BrokenL2. */
  void OnProbeFailure() noexcept;

  /** @brief External safety restore: Safe -> Recovering. */
  void OnExternalSafeRestore() noexcept;

  /**
   * @brief Qualifying call: clears the consecutive-timeout counters but does
   *        NOT touch cooldown - the recovery window is never skipped.
   */
  void OnRtcOk() noexcept;

  /** @brief Current watermark sample in percent (0..100). */
  void OnWatermark(uint8_t percent) noexcept;

  // -------------------------------------------------------------------------
  // Queries
  // -------------------------------------------------------------------------

  BreakerLevel Level() const noexcept;

  /** @brief True only in Normal; BrokenL1 revokes the direct path. */
  bool DirectAllowed() const noexcept;

  /** @brief True in Normal, or in Recovering after enough healthy windows. */
  bool HealthyWindowPassed() const noexcept;

  /** @brief L2/Safe: the policy layer should drop non-critical inputs. */
  bool DropNonCritical() const noexcept;

  /** @brief Safe: the policy layer should admit only safety events. */
  bool SafeEventsOnly() const noexcept;

  // Default thresholds.
  static constexpr uint8_t kDirectTimeoutThreshold = 3U;
  static constexpr uint8_t kRtcTimeoutThreshold = 3U;
  static constexpr uint8_t kWatermarkViolationPct = 80U;
  static constexpr uint8_t kLowWatermarkPct = 50U;
  static constexpr uint8_t kHighWatermarkPersist = 3U;
  static constexpr uint8_t kLowWatermarkPersist = 3U;
  static constexpr uint8_t kHealthyWindowsRequired = 3U;

 private:
  struct State {
    BreakerLevel level;
    uint16_t cooldown_remaining;
    uint8_t direct_timeout_consec;
    uint8_t rtc_timeout_consec;
    uint8_t high_watermark_consec;
    uint8_t low_watermark_consec;
    uint8_t healthy_window_count;
    bool probe_success;
  };

  // Single 32-bit word packs the whole state: level(3) + cooldown(16) +
  // three 2-bit counters + one 1-bit flag. Relaxed CAS linearizes every
  // transition; the Breaker publishes no external data.
  static constexpr uint32_t kLevelShift = 0U;
  static constexpr uint32_t kCooldownShift = 3U;
  static constexpr uint32_t kDirectTimeoutShift = 19U;
  static constexpr uint32_t kRtcTimeoutShift = 21U;
  static constexpr uint32_t kHighWatermarkShift = 23U;
  static constexpr uint32_t kLowWatermarkShift = 25U;
  static constexpr uint32_t kHealthyWindowShift = 27U;
  static constexpr uint32_t kProbeSuccessShift = 29U;
  static constexpr uint32_t kLevelMask = 0x7U;
  static constexpr uint32_t kCooldownMask = 0xffffU;
  static constexpr uint32_t kCounterMask = 0x3U;

  static uint32_t Pack(const State& state) noexcept;
  static State Unpack(uint32_t packed) noexcept;
  static void Increment(uint8_t& counter) noexcept;

  template <typename Transition>
  void Update(const Transition& transition) noexcept;

  void ResetMetrics(State& state) const noexcept;
  void EnterL1(State& state) const noexcept;
  void EnterL2(State& state) const noexcept;
  void EnterSafe(State& state) const noexcept;
  void EnterRecovering(State& state) const noexcept;

  std::atomic<uint32_t> state_;

  static_assert(std::atomic<uint32_t>::is_always_lock_free, "osp: Breaker requires lock-free 32-bit atomics");
  static_assert(CooldownCycles <= 0xffffU, "osp: Breaker CooldownCycles must fit in 16 bits");
};

// ============================================================================
// Inline Implementation
// ============================================================================

template <uint16_t CooldownCycles>
Breaker<CooldownCycles>::Breaker() noexcept : state_(Pack({BreakerLevel::Normal, 0U, 0U, 0U, 0U, 0U, 0U, false})) {}

template <uint16_t CooldownCycles>
uint32_t Breaker<CooldownCycles>::Pack(const State& state) noexcept {
  return (static_cast<uint32_t>(state.level) << kLevelShift) |
         (static_cast<uint32_t>(state.cooldown_remaining) << kCooldownShift) |
         (static_cast<uint32_t>(state.direct_timeout_consec) << kDirectTimeoutShift) |
         (static_cast<uint32_t>(state.rtc_timeout_consec) << kRtcTimeoutShift) |
         (static_cast<uint32_t>(state.high_watermark_consec) << kHighWatermarkShift) |
         (static_cast<uint32_t>(state.low_watermark_consec) << kLowWatermarkShift) |
         (static_cast<uint32_t>(state.healthy_window_count) << kHealthyWindowShift) |
         (static_cast<uint32_t>(state.probe_success) << kProbeSuccessShift);
}

template <uint16_t CooldownCycles>
typename Breaker<CooldownCycles>::State Breaker<CooldownCycles>::Unpack(uint32_t packed) noexcept {
  return {static_cast<BreakerLevel>((packed >> kLevelShift) & kLevelMask),
          static_cast<uint16_t>((packed >> kCooldownShift) & kCooldownMask),
          static_cast<uint8_t>((packed >> kDirectTimeoutShift) & kCounterMask),
          static_cast<uint8_t>((packed >> kRtcTimeoutShift) & kCounterMask),
          static_cast<uint8_t>((packed >> kHighWatermarkShift) & kCounterMask),
          static_cast<uint8_t>((packed >> kLowWatermarkShift) & kCounterMask),
          static_cast<uint8_t>((packed >> kHealthyWindowShift) & kCounterMask),
          0U != ((packed >> kProbeSuccessShift) & 0x1U)};
}

template <uint16_t CooldownCycles>
void Breaker<CooldownCycles>::Increment(uint8_t& counter) noexcept {
  if (counter < kCounterMask) {
    ++counter;
  }
}

template <uint16_t CooldownCycles>
template <typename Transition>
void Breaker<CooldownCycles>::Update(const Transition& transition) noexcept {
  uint32_t observed = state_.load(std::memory_order_relaxed);
  bool complete = false;
  while (!complete) {
    State next = Unpack(observed);
    transition(next);
    const uint32_t desired = Pack(next);
    complete = (desired == observed) ||
               state_.compare_exchange_weak(observed, desired, std::memory_order_relaxed, std::memory_order_relaxed);
  }
}

template <uint16_t CooldownCycles>
void Breaker<CooldownCycles>::ResetMetrics(State& state) const noexcept {
  state.direct_timeout_consec = 0U;
  state.rtc_timeout_consec = 0U;
  state.high_watermark_consec = 0U;
  state.low_watermark_consec = 0U;
  state.healthy_window_count = 0U;
  state.probe_success = false;
}

template <uint16_t CooldownCycles>
void Breaker<CooldownCycles>::EnterL1(State& state) const noexcept {
  state.level = BreakerLevel::BrokenL1;
  state.cooldown_remaining = CooldownCycles;
  ResetMetrics(state);
}

template <uint16_t CooldownCycles>
void Breaker<CooldownCycles>::EnterL2(State& state) const noexcept {
  state.level = BreakerLevel::BrokenL2;
  state.cooldown_remaining = CooldownCycles;
  ResetMetrics(state);
}

template <uint16_t CooldownCycles>
void Breaker<CooldownCycles>::EnterSafe(State& state) const noexcept {
  state.level = BreakerLevel::Safe;
  state.cooldown_remaining = CooldownCycles;
  ResetMetrics(state);
}

template <uint16_t CooldownCycles>
void Breaker<CooldownCycles>::EnterRecovering(State& state) const noexcept {
  state.level = BreakerLevel::Recovering;
  ResetMetrics(state);
  // cooldown_remaining is left untouched: from L1/L2 it is already 0;
  // from Safe it still counts down before healthy windows may advance.
}

template <uint16_t CooldownCycles>
void Breaker<CooldownCycles>::OnDirectTimeout() noexcept {
  Update([this](State& state) {
    Increment(state.direct_timeout_consec);
    switch (state.level) {
      case BreakerLevel::Normal:
        if (state.direct_timeout_consec >= kDirectTimeoutThreshold) {
          EnterL1(state);
        }
        break;
      case BreakerLevel::BrokenL1:
      case BreakerLevel::BrokenL2:
      case BreakerLevel::Safe:
        break;
      case BreakerLevel::Recovering:
        EnterL2(state);
        break;
      default:
        break;
    }
  });
}

template <uint16_t CooldownCycles>
void Breaker<CooldownCycles>::OnRtcTimeout() noexcept {
  Update([this](State& state) {
    Increment(state.rtc_timeout_consec);
    switch (state.level) {
      case BreakerLevel::Normal:
      case BreakerLevel::BrokenL1:
        if (state.rtc_timeout_consec >= kRtcTimeoutThreshold) {
          EnterL2(state);
        }
        break;
      case BreakerLevel::BrokenL2:
      case BreakerLevel::Safe:
        break;
      case BreakerLevel::Recovering:
        EnterL2(state);
        break;
      default:
        break;
    }
  });
}

template <uint16_t CooldownCycles>
void Breaker<CooldownCycles>::OnWatermarkViolation() noexcept {
  OnWatermark(static_cast<uint8_t>(kWatermarkViolationPct + 1U));
}

template <uint16_t CooldownCycles>
void Breaker<CooldownCycles>::OnWatermark(uint8_t percent) noexcept {
  Update([this, percent](State& state) {
    if (percent > kWatermarkViolationPct) {
      Increment(state.high_watermark_consec);
      state.low_watermark_consec = 0U;
    } else if (percent < kLowWatermarkPct) {
      Increment(state.low_watermark_consec);
      state.high_watermark_consec = 0U;
    } else {
      state.high_watermark_consec = 0U;
      state.low_watermark_consec = 0U;
    }

    switch (state.level) {
      case BreakerLevel::Normal:
        if (state.high_watermark_consec >= kHighWatermarkPersist) {
          EnterL2(state);
        }
        break;
      case BreakerLevel::BrokenL1:
        if (state.high_watermark_consec >= kHighWatermarkPersist) {
          EnterL2(state);
        } else if ((0U == state.cooldown_remaining) && (state.low_watermark_consec >= kLowWatermarkPersist)) {
          EnterRecovering(state);
        }
        break;
      case BreakerLevel::BrokenL2:
        if ((0U == state.cooldown_remaining) && (state.low_watermark_consec >= kLowWatermarkPersist)) {
          EnterRecovering(state);
        }
        break;
      case BreakerLevel::Safe:
        break;
      case BreakerLevel::Recovering:
        if (percent >= kLowWatermarkPct) {
          EnterL2(state);
        }
        break;
      default:
        break;
    }
  });
}

template <uint16_t CooldownCycles>
void Breaker<CooldownCycles>::OnOverflow() noexcept {
  Update([this](State& state) {
    switch (state.level) {
      case BreakerLevel::Normal:
      case BreakerLevel::BrokenL1:
      case BreakerLevel::Recovering:
        EnterL2(state);
        break;
      case BreakerLevel::BrokenL2:
      case BreakerLevel::Safe:
        break;
      default:
        break;
    }
  });
}

template <uint16_t CooldownCycles>
void Breaker<CooldownCycles>::OnKeyReserveExhausted() noexcept {
  Update([this](State& state) {
    if (BreakerLevel::Safe != state.level) {
      EnterSafe(state);
    }
  });
}

template <uint16_t CooldownCycles>
void Breaker<CooldownCycles>::OnWatchdog() noexcept {
  OnKeyReserveExhausted();
}

template <uint16_t CooldownCycles>
void Breaker<CooldownCycles>::OnDispatchCycle() noexcept {
  Update([](State& state) {
    if (state.cooldown_remaining > 0U) {
      --state.cooldown_remaining;
    }
    if ((BreakerLevel::Recovering == state.level) && (0U == state.cooldown_remaining) && state.probe_success) {
      Increment(state.healthy_window_count);
      state.probe_success = false;
      if (state.healthy_window_count >= kHealthyWindowsRequired) {
        state.level = BreakerLevel::Normal;
        state.healthy_window_count = 0U;
      }
    }
  });
}

template <uint16_t CooldownCycles>
void Breaker<CooldownCycles>::OnProbeSuccess() noexcept {
  Update([](State& state) {
    if (BreakerLevel::Recovering == state.level) {
      state.probe_success = true;
    }
  });
}

template <uint16_t CooldownCycles>
void Breaker<CooldownCycles>::OnProbeFailure() noexcept {
  Update([this](State& state) {
    if (BreakerLevel::Recovering == state.level) {
      EnterL2(state);
    }
  });
}

template <uint16_t CooldownCycles>
void Breaker<CooldownCycles>::OnExternalSafeRestore() noexcept {
  Update([this](State& state) {
    if (BreakerLevel::Safe == state.level) {
      EnterRecovering(state);
    }
  });
}

template <uint16_t CooldownCycles>
void Breaker<CooldownCycles>::OnRtcOk() noexcept {
  // A qualifying call clears the consecutive-timeout counters but does NOT
  // touch cooldown_remaining_: the recovery window is never skipped.
  Update([](State& state) {
    state.direct_timeout_consec = 0U;
    state.rtc_timeout_consec = 0U;
  });
}

template <uint16_t CooldownCycles>
BreakerLevel Breaker<CooldownCycles>::Level() const noexcept {
  return Unpack(state_.load(std::memory_order_relaxed)).level;
}

template <uint16_t CooldownCycles>
bool Breaker<CooldownCycles>::DirectAllowed() const noexcept {
  return (BreakerLevel::Normal == Level());
}

template <uint16_t CooldownCycles>
bool Breaker<CooldownCycles>::HealthyWindowPassed() const noexcept {
  const State state = Unpack(state_.load(std::memory_order_relaxed));
  if (BreakerLevel::Normal == state.level) {
    return true;
  }
  if (BreakerLevel::Recovering == state.level) {
    return (state.healthy_window_count >= kHealthyWindowsRequired);
  }
  return false;
}

template <uint16_t CooldownCycles>
bool Breaker<CooldownCycles>::DropNonCritical() const noexcept {
  const BreakerLevel current = Level();
  return (BreakerLevel::BrokenL2 == current) || (BreakerLevel::Safe == current);
}

template <uint16_t CooldownCycles>
bool Breaker<CooldownCycles>::SafeEventsOnly() const noexcept {
  return (BreakerLevel::Safe == Level());
}

}  // namespace osp

#endif  // OSP_BREAKER_HPP_
