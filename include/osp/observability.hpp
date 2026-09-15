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
 * @file osp/observability.hpp
 * @brief Lightweight runtime observability primitives for the hcs_* diagnostic
 *        shell snapshots (and for a Python diagnostic panel to consume).
 *
 * These are the data sources behind the `hcs_threads` / `hcs_queues` /
 * `hcs_frames` / `hcs_latency` commands registered by shell_commands.hpp.
 * Modules stay free of any shell dependency: they only fill the counters /
 * registries below, and shell_commands.hpp (a bridge file) prints them.
 *
 * Types:
 *  - ThreadRegistry  : named threads + heartbeat + priority (thread liveness).
 *  - QueueCounters   : lock-free queue counters (depth/peak/submit/coalesce/
 *                      reject/full) + named QueueMonitor set.
 *  - FrameStageSet   : per-stage frame counters (fps / drop / last frame id).
 *  - LatencyTracker  : P50/P95/P99/max + last-10s window histogram.
 *
 * All types are header-only, stack/static allocation only, and compatible
 * with -fno-exceptions -fno-rtti. Counters use relaxed atomics; they are
 * diagnostics, not correctness-critical synchronization. Snapshot methods are
 * cold path (1 Hz polling / shell), never per-frame.
 */

#ifndef OSP_OBSERVABILITY_HPP_
#define OSP_OBSERVABILITY_HPP_

#include "osp/platform.hpp"
#include "osp/thread.hpp"
#include "osp/vocabulary.hpp"

#include <cstdint>

#include <atomic>

namespace osp {

// ============================================================================
// QueueCounters - lock-free queue statistics accumulator
// ============================================================================

/**
 * @brief Reusable, cache-line-padded queue counter set.
 *
 * One instance per observable queue. Producers call the On*() hooks from the
 * hot path (relaxed atomics only); consumers call GetSnapshot() on the cold
 * path (shell / 1 Hz status poll).
 *
 * `full` is the explicit fault signal: it means a job was dropped because the
 * queue could not accept it (as opposed to `rejected`, which is a policy-level
 * admission rejection such as "paused" or "busy").
 */
class QueueCounters final {
 public:
  struct Snapshot {
    uint64_t depth{0U};       ///< Current queue depth (last observed).
    uint64_t peak_depth{0U};  ///< Peak depth since last Reset().
    uint64_t submitted{0U};   ///< Total jobs accepted.
    uint64_t coalesced{0U};   ///< Total jobs merged into an in-flight job.
    uint64_t rejected{0U};    ///< Total jobs refused by admission policy.
    uint64_t full{0U};        ///< Total jobs dropped because the queue was full.
  };

  /// @brief A job was accepted into the queue.
  void OnSubmitted() noexcept { submitted_.fetch_add(1U, std::memory_order_relaxed); }

  /// @brief A job was coalesced into a still-pending job (e.g. slider updates).
  void OnCoalesced() noexcept { coalesced_.fetch_add(1U, std::memory_order_relaxed); }

  /// @brief A job was refused by admission policy (paused/busy/backpressure).
  void OnRejected() noexcept { rejected_.fetch_add(1U, std::memory_order_relaxed); }

  /// @brief A job was dropped because the queue was full (fault signal).
  void OnFull() noexcept { full_.fetch_add(1U, std::memory_order_relaxed); }

  /// @brief Record the current depth; updates the peak via CAS.
  void OnDepth(uint64_t depth) noexcept {
    depth_.store(depth, std::memory_order_relaxed);
    uint64_t peak = peak_depth_.load(std::memory_order_relaxed);
    while (depth > peak && !peak_depth_.compare_exchange_weak(peak, depth, std::memory_order_relaxed,
                                                             std::memory_order_relaxed)) {
    }
  }

  Snapshot GetSnapshot() const noexcept {
    Snapshot s;
    s.depth = depth_.load(std::memory_order_relaxed);
    s.peak_depth = peak_depth_.load(std::memory_order_relaxed);
    s.submitted = submitted_.load(std::memory_order_relaxed);
    s.coalesced = coalesced_.load(std::memory_order_relaxed);
    s.rejected = rejected_.load(std::memory_order_relaxed);
    s.full = full_.load(std::memory_order_relaxed);
    return s;
  }

  void Reset() noexcept {
    depth_.store(0U, std::memory_order_relaxed);
    peak_depth_.store(0U, std::memory_order_relaxed);
    submitted_.store(0U, std::memory_order_relaxed);
    coalesced_.store(0U, std::memory_order_relaxed);
    rejected_.store(0U, std::memory_order_relaxed);
    full_.store(0U, std::memory_order_relaxed);
  }

 private:
  alignas(kCacheLineSize) std::atomic<uint64_t> submitted_{0U};
  alignas(kCacheLineSize) std::atomic<uint64_t> coalesced_{0U};
  alignas(kCacheLineSize) std::atomic<uint64_t> rejected_{0U};
  alignas(kCacheLineSize) std::atomic<uint64_t> full_{0U};
  alignas(kCacheLineSize) std::atomic<uint64_t> depth_{0U};
  alignas(kCacheLineSize) std::atomic<uint64_t> peak_depth_{0U};
};

// ============================================================================
// QueueStrategy - admission/eviction policy label (for hcs_queues output)
// ============================================================================

enum class QueueStrategy : uint8_t {
  kFifo = 0,          ///< FIFO; oldest job first.
  kCoalesceLatest = 1,  ///< Merge duplicate keys, keep the newest value.
  kDropNewest = 2,    ///< On full, reject the incoming job.
  kDropOldest = 3,    ///< On full, evict the oldest job.
};

inline const char* QueueStrategyName(QueueStrategy s) noexcept {
  switch (s) {
    case QueueStrategy::kFifo:
      return "fifo";
    case QueueStrategy::kCoalesceLatest:
      return "coalesce-latest";
    case QueueStrategy::kDropNewest:
      return "drop-newest";
    case QueueStrategy::kDropOldest:
      return "drop-oldest";
    default:
      return "unknown";
  }
}

// ============================================================================
// QueueMonitor - named set of QueueCounters (hcs_queues data source)
// ============================================================================

/**
 * @brief Fixed-capacity set of named queues with per-queue counters and a
 *        strategy label. The application adds its queues once at wiring time,
 *        then calls Counter(i) from the producer hot path.
 */
template <uint32_t MaxQueues = 16>
class QueueMonitor final {
  static_assert(MaxQueues > 0U, "MaxQueues must be > 0");

 public:
  static constexpr int32_t kInvalidIndex = -1;

  struct Snapshot {
    FixedString<24> name;
    QueueStrategy strategy{QueueStrategy::kFifo};
    QueueCounters::Snapshot counters{};
  };

  /// @brief Add a named queue. Returns its index, or kInvalidIndex when full.
  /// @pre Call before concurrent Counter()/ForEach() use (wiring phase).
  int32_t AddQueue(const char* name, QueueStrategy strategy = QueueStrategy::kFifo) noexcept {
    const uint32_t idx = count_.load(std::memory_order_relaxed);
    if (idx >= MaxQueues) {
      return kInvalidIndex;
    }
    slots_[idx].name.assign(TruncateToCapacity, name);
    slots_[idx].strategy = strategy;
    count_.store(idx + 1U, std::memory_order_release);
    return static_cast<int32_t>(idx);
  }

  /// @brief Access the counters of queue @p index (for producer On*() calls).
  QueueCounters& Counter(uint32_t index) noexcept { return slots_[index].counters; }

  uint32_t Count() const noexcept { return count_.load(std::memory_order_acquire); }

  /// @brief Invoke @p fn(const Snapshot&) for each queue. Snapshot is cold path.
  template <typename Fn>
  void ForEach(Fn&& fn) const noexcept {
    const uint32_t n = count_.load(std::memory_order_acquire);
    for (uint32_t i = 0U; i < n; ++i) {
      Snapshot s;
      s.name = slots_[i].name;
      s.strategy = slots_[i].strategy;
      s.counters = slots_[i].counters.GetSnapshot();
      fn(s);
    }
  }

  void Reset() noexcept {
    const uint32_t n = count_.load(std::memory_order_acquire);
    for (uint32_t i = 0U; i < n; ++i) {
      slots_[i].counters.Reset();
    }
  }

 private:
  struct Slot {
    FixedString<24> name;
    QueueStrategy strategy{QueueStrategy::kFifo};
    QueueCounters counters;
  };

  Slot slots_[MaxQueues]{};
  std::atomic<uint32_t> count_{0U};
};

// ============================================================================
// ThreadRegistry - named thread liveness registry (hcs_threads data source)
// ============================================================================

/**
 * @brief Registry of monitored threads for the hcs_threads diagnostic.
 *
 * The application registers each worker thread once with its name, priority and
 * a ThreadHeartbeat* (may be nullptr for threads without a beat). The
 * ThreadHeartbeat* may be the same object the ThreadWatchdog watches, so the
 * watchdog reports faults while this registry reports ages/priorities.
 */
template <uint32_t MaxThreads = 32>
class ThreadRegistry final {
  static_assert(MaxThreads > 0U, "MaxThreads must be > 0");

 public:
  struct Snapshot {
    FixedString<32> name;
    int32_t priority{0};
    bool has_heartbeat{false};
    bool timed_out{false};
    uint64_t last_beat_us{0U};
    uint64_t age_us{0U};  ///< now - last_beat (0 if no heartbeat).
  };

  /// @brief Register a thread. Returns slot id, or -1 when full.
  /// @param timeout_ms  0 = no timeout (never reported stale); else stale when
  ///                    heartbeat age exceeds this.
  int32_t Register(const char* name, ThreadHeartbeat* heartbeat, int32_t priority = 0,
                   uint32_t timeout_ms = 0U) noexcept {
    std::lock_guard<osp::Mutex> lock(mutex_);
    for (uint32_t i = 0U; i < MaxThreads; ++i) {
      if (entries_[i].active) {
        continue;
      }
      entries_[i].name.assign(TruncateToCapacity, name);
      entries_[i].heartbeat = heartbeat;
      entries_[i].priority = priority;
      entries_[i].timeout_us = static_cast<uint64_t>(timeout_ms) * 1000ULL;
      entries_[i].active = true;
      return static_cast<int32_t>(i);
    }
    return -1;
  }

  void Unregister(int32_t slot_id) noexcept {
    if (slot_id < 0 || static_cast<uint32_t>(slot_id) >= MaxThreads) {
      return;
    }
    std::lock_guard<osp::Mutex> lock(mutex_);
    entries_[static_cast<uint32_t>(slot_id)].active = false;
  }

  uint32_t ActiveCount() const noexcept {
    std::lock_guard<osp::Mutex> lock(mutex_);
    uint32_t n = 0U;
    for (uint32_t i = 0U; i < MaxThreads; ++i) {
      if (entries_[i].active) {
        ++n;
      }
    }
    return n;
  }

  /// @brief Invoke @p fn(const Snapshot&) for each active thread.
  /// Collects under the mutex, then invokes callbacks outside it
  /// (collect-release-execute).
  template <typename Fn>
  void ForEach(Fn&& fn) const noexcept {
    const uint64_t now = SteadyNowUs();

    Snapshot pending[MaxThreads];
    uint32_t n = 0U;
    {
      std::lock_guard<osp::Mutex> lock(mutex_);
      for (uint32_t i = 0U; i < MaxThreads; ++i) {
        const Entry& e = entries_[i];
        if (!e.active) {
          continue;
        }
        Snapshot& s = pending[n++];
        s.name = e.name;
        s.priority = e.priority;
        s.has_heartbeat = (e.heartbeat != nullptr);
        if (e.heartbeat != nullptr) {
          s.last_beat_us = e.heartbeat->LastBeatUs();
          s.age_us = (now > s.last_beat_us) ? (now - s.last_beat_us) : 0U;
          s.timed_out = (e.timeout_us > 0U) && (s.age_us > e.timeout_us);
        } else {
          s.last_beat_us = 0U;
          s.age_us = 0U;
          s.timed_out = false;
        }
      }
    }

    for (uint32_t i = 0U; i < n; ++i) {
      fn(pending[i]);
    }
  }

 private:
  struct Entry {
    FixedString<32> name;
    ThreadHeartbeat* heartbeat{nullptr};
    int32_t priority{0};
    uint64_t timeout_us{0U};
    bool active{false};
  };

  Entry entries_[MaxThreads]{};
  mutable osp::Mutex mutex_;
};

// ============================================================================
// FrameStageSet - per-stage frame counters (hcs_frames data source)
// ============================================================================

/**
 * @brief Counters for one processing stage of the frame pipeline.
 *
 * Each stage records the latest frame id it saw, total frames, and total
 * drops. `fps` is computed over a rolling ~1 second window at snapshot time
 * (cold path); it is approximate under concurrent Mark*() calls.
 */
template <uint32_t MaxStages = 16>
class FrameStageSet final {
  static_assert(MaxStages > 0U, "MaxStages must be > 0");

 public:
  static constexpr uint64_t kWindowUs = 1000000ULL;  ///< fps window (1 s).

  struct StageSnapshot {
    FixedString<24> name;
    uint64_t last_frame_id{0U};
    uint64_t total_frames{0U};
    uint64_t total_dropped{0U};
    double fps{0.0};
  };

  /// @brief Add a stage. Returns its index, or -1 when full.
  /// @pre Call before concurrent Mark*() use (wiring phase).
  int32_t AddStage(const char* name) noexcept {
    const uint32_t idx = count_.load(std::memory_order_relaxed);
    if (idx >= MaxStages) {
      return -1;
    }
    stages_[idx].name.assign(TruncateToCapacity, name);
    count_.store(idx + 1U, std::memory_order_release);
    return static_cast<int32_t>(idx);
  }

  /// @brief Record that stage @p stage processed frame @p frame_id.
  void MarkFrame(uint32_t stage, uint64_t frame_id) noexcept {
    if (stage >= count_.load(std::memory_order_acquire)) {
      return;
    }
    Stage& s = stages_[stage];
    s.last_frame_id.store(frame_id, std::memory_order_relaxed);
    s.total_frames.fetch_add(1U, std::memory_order_relaxed);

    uint64_t zero = 0U;
    s.window_start_us.compare_exchange_strong(zero, SteadyNowUs(), std::memory_order_relaxed,
                                              std::memory_order_relaxed);
    s.window_frames.fetch_add(1U, std::memory_order_relaxed);
  }

  /// @brief Record that stage @p stage dropped @p count frames.
  void MarkDrop(uint32_t stage, uint32_t count = 1U) noexcept {
    if (stage >= count_.load(std::memory_order_acquire)) {
      return;
    }
    stages_[stage].total_dropped.fetch_add(count, std::memory_order_relaxed);
  }

  uint32_t StageCount() const noexcept { return count_.load(std::memory_order_acquire); }

  /// @brief Invoke @p fn(const StageSnapshot&) for each stage (cold path).
  /// Rolls the per-stage fps window when it has elapsed.
  template <typename Fn>
  void ForEachSnapshot(Fn&& fn) noexcept {
    const uint64_t now = SteadyNowUs();
    const uint32_t n = count_.load(std::memory_order_acquire);
    for (uint32_t i = 0U; i < n; ++i) {
      Stage& s = stages_[i];
      StageSnapshot snap;
      snap.name = s.name;
      snap.last_frame_id = s.last_frame_id.load(std::memory_order_relaxed);
      snap.total_frames = s.total_frames.load(std::memory_order_relaxed);
      snap.total_dropped = s.total_dropped.load(std::memory_order_relaxed);

      const uint64_t start = s.window_start_us.load(std::memory_order_relaxed);
      const uint64_t frames = s.window_frames.load(std::memory_order_relaxed);
      const uint64_t elapsed = (now > start && start != 0U) ? (now - start) : 0U;
      snap.fps = (elapsed >= kWindowUs && frames > 0U)
                     ? static_cast<double>(frames) * 1e6 / static_cast<double>(elapsed)
                     : 0.0;

      // Roll the window: only one caller wins the CAS; the loser keeps the old
      // window for one more snapshot. Approximate by design (diagnostic only).
      if (elapsed >= kWindowUs) {
        uint64_t expected = start;
        if (s.window_start_us.compare_exchange_strong(expected, now, std::memory_order_relaxed,
                                                      std::memory_order_relaxed)) {
          s.window_frames.store(0U, std::memory_order_relaxed);
        }
      }

      fn(snap);
    }
  }

  void Reset() noexcept {
    const uint32_t n = count_.load(std::memory_order_acquire);
    for (uint32_t i = 0U; i < n; ++i) {
      Stage& s = stages_[i];
      s.last_frame_id.store(0U, std::memory_order_relaxed);
      s.total_frames.store(0U, std::memory_order_relaxed);
      s.total_dropped.store(0U, std::memory_order_relaxed);
      s.window_frames.store(0U, std::memory_order_relaxed);
      s.window_start_us.store(0U, std::memory_order_relaxed);
    }
  }

 private:
  struct Stage {
    FixedString<24> name;
    std::atomic<uint64_t> last_frame_id{0U};
    std::atomic<uint64_t> total_frames{0U};
    std::atomic<uint64_t> total_dropped{0U};
    std::atomic<uint64_t> window_frames{0U};
    std::atomic<uint64_t> window_start_us{0U};
  };

  Stage stages_[MaxStages]{};
  std::atomic<uint32_t> count_{0U};
};

// ============================================================================
// LatencyTracker - percentile histogram + last-10s window (hcs_latency)
// ============================================================================

namespace observability_detail {

/// @brief floor(log2(v)) for a non-zero value (portable, no intrinsics).
inline uint32_t FloorLog2(uint64_t v) noexcept {
  uint32_t b = 0U;
  while (v >= 2U) {
    v >>= 1U;
    ++b;
  }
  return b;
}

}  // namespace observability_detail

/**
 * @brief Latency statistics: all-time P50/P95/P99/max + a rolling last-10s
 *        window summary.
 *
 * Percentiles use a fixed log2 histogram (64 buckets, one per power of two),
 * walked on the cold-path snapshot. The recent window keeps the last
 * kWindowSamples (timestamp + value) in a ring buffer.
 *
 * @note Record() is intended for a single writer thread (e.g. a stats
 *       collector). GetSnapshot() may run concurrently and returns a relaxed
 *       diagnostic approximation.
 */
template <uint32_t kWindowSamples = 256>
class LatencyTracker final {
  static_assert(kWindowSamples > 0U, "kWindowSamples must be > 0");

 public:
  static constexpr uint32_t kBuckets = 64U;
  static constexpr uint64_t kWindowUs = 10000000ULL;  ///< Recent window: 10 s.

  struct Snapshot {
    uint64_t count{0U};
    uint64_t min_us{0U};
    uint64_t max_us{0U};
    uint64_t avg_us{0U};
    uint64_t p50_us{0U};
    uint64_t p95_us{0U};
    uint64_t p99_us{0U};
    uint64_t window_count{0U};
    uint64_t window_min_us{0U};
    uint64_t window_max_us{0U};
    uint64_t window_avg_us{0U};
  };

  /// @brief Record one latency sample (microseconds).
  void Record(uint64_t latency_us) noexcept {
    const uint64_t prev = count_.fetch_add(1U, std::memory_order_relaxed);
    sum_.fetch_add(latency_us, std::memory_order_relaxed);
    if (prev == 0U) {
      min_.store(latency_us, std::memory_order_relaxed);
      max_.store(latency_us, std::memory_order_relaxed);
    } else {
      uint64_t cur = min_.load(std::memory_order_relaxed);
      while (latency_us < cur && !min_.compare_exchange_weak(cur, latency_us, std::memory_order_relaxed,
                                                            std::memory_order_relaxed)) {
      }
      cur = max_.load(std::memory_order_relaxed);
      while (latency_us > cur && !max_.compare_exchange_weak(cur, latency_us, std::memory_order_relaxed,
                                                            std::memory_order_relaxed)) {
      }
    }

    const uint32_t bucket = observability_detail::FloorLog2(latency_us);
    if (bucket < kBuckets) {
      buckets_[bucket].fetch_add(1U, std::memory_order_relaxed);
    }

    // Recent-window ring buffer.
    const uint32_t idx = window_head_.fetch_add(1U, std::memory_order_relaxed) % kWindowSamples;
    window_[idx].timestamp_us = SteadyNowUs();
    window_[idx].value_us = latency_us;
  }

  Snapshot GetSnapshot() const noexcept {
    Snapshot s;
    s.count = count_.load(std::memory_order_relaxed);
    if (s.count == 0U) {
      return s;
    }
    s.min_us = min_.load(std::memory_order_relaxed);
    s.max_us = max_.load(std::memory_order_relaxed);
    s.avg_us = sum_.load(std::memory_order_relaxed) / s.count;
    s.p50_us = Percentile(s.count * 50U / 100U);
    s.p95_us = Percentile(s.count * 95U / 100U);
    s.p99_us = Percentile(s.count * 99U / 100U);

    // Recent window: iterate the last kWindowSamples entries in ring order and
    // keep those within the last 10 s.
    const uint64_t now = SteadyNowUs();
    const uint32_t head = window_head_.load(std::memory_order_relaxed);
    const uint32_t n = (head < kWindowSamples) ? head : kWindowSamples;
    uint64_t wcount = 0U;
    uint64_t wsum = 0U;
    uint64_t wmin = UINT64_MAX;
    uint64_t wmax = 0U;
    for (uint32_t i = 0U; i < n; ++i) {
      const uint32_t idx = (head - 1U - i) % kWindowSamples;
      const WindowSample& e = window_[idx];
      if ((now - e.timestamp_us) > kWindowUs) {
        continue;
      }
      ++wcount;
      wsum += e.value_us;
      if (e.value_us < wmin) {
        wmin = e.value_us;
      }
      if (e.value_us > wmax) {
        wmax = e.value_us;
      }
    }
    if (wcount > 0U) {
      s.window_count = wcount;
      s.window_min_us = wmin;
      s.window_max_us = wmax;
      s.window_avg_us = wsum / wcount;
    }
    return s;
  }

  void Reset() noexcept {
    count_.store(0U, std::memory_order_relaxed);
    sum_.store(0U, std::memory_order_relaxed);
    min_.store(0U, std::memory_order_relaxed);
    max_.store(0U, std::memory_order_relaxed);
    for (uint32_t i = 0U; i < kBuckets; ++i) {
      buckets_[i].store(0U, std::memory_order_relaxed);
    }
    window_head_.store(0U, std::memory_order_relaxed);
  }

 private:
  /// @brief Histogram percentile: return the bucket midpoint of the rank,
  ///        clamped to the observed maximum so percentiles never overshoot.
  uint64_t Percentile(uint64_t rank) const noexcept {
    const uint64_t total = count_.load(std::memory_order_relaxed);
    if (total == 0U || rank >= total) {
      return max_.load(std::memory_order_relaxed);
    }
    const uint64_t max_val = max_.load(std::memory_order_relaxed);
    uint64_t seen = 0U;
    for (uint32_t i = 0U; i < kBuckets; ++i) {
      seen += buckets_[i].load(std::memory_order_relaxed);
      if (seen > rank) {
        const uint64_t lo = (i == 0U) ? 0U : (1ULL << i);
        const uint64_t hi = (i >= 63U) ? lo : (1ULL << (i + 1U));
        const uint64_t mid = lo + (hi - lo) / 2U;
        return (mid > max_val) ? max_val : mid;
      }
    }
    return max_val;
  }

  struct WindowSample {
    uint64_t timestamp_us{0U};
    uint64_t value_us{0U};
  };

  alignas(kCacheLineSize) std::atomic<uint64_t> count_{0U};
  alignas(kCacheLineSize) std::atomic<uint64_t> sum_{0U};
  alignas(kCacheLineSize) std::atomic<uint64_t> min_{0U};
  alignas(kCacheLineSize) std::atomic<uint64_t> max_{0U};
  std::atomic<uint64_t> buckets_[kBuckets]{};
  std::atomic<uint32_t> window_head_{0U};
  WindowSample window_[kWindowSamples]{};
};

}  // namespace osp

#endif  // OSP_OBSERVABILITY_HPP_
