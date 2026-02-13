/**
 * @file fault_collector.hpp
 * @brief Header-only fault collection and control unit for OSP-CPP.
 *
 * Provides multi-priority fault reporting with lock-free MPSC queues,
 * independent consumer thread, priority admission control, active fault
 * bitmap, and user hook callbacks. Zero heap allocation on hot path.
 *
 * Compatible with -fno-exceptions -fno-rtti, C++17.
 *
 * Usage:
 *   #include "osp/fault_collector.hpp"
 *
 *   osp::FaultCollector<32, 128> collector;
 *   collector.RegisterFault(0U, 0x01010001U);
 *   collector.RegisterHook(0U, [](const osp::FaultEvent& e) {
 *       return osp::HookAction::kHandled;
 *   });
 *   collector.Start();
 *   collector.ReportFault(0U, 0xDEAD, osp::FaultPriority::kCritical);
 *   collector.Stop();
 */

#ifndef OSP_FAULT_COLLECTOR_HPP_
#define OSP_FAULT_COLLECTOR_HPP_

#include "osp/log.hpp"
#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <type_traits>

namespace osp {

// ============================================================================
// Enumerations
// ============================================================================

/// Fault priority levels (lower value = higher priority).
enum class FaultPriority : uint8_t {
  kCritical = 0U,  ///< System safety, never dropped
  kHigh     = 1U,  ///< Hardware fault, comm loss
  kMedium   = 2U,  ///< Performance degradation
  kLow      = 3U   ///< Diagnostics, statistics
};

/// Hook return action.
enum class HookAction : uint8_t {
  kHandled  = 0U,  ///< Fault resolved, clear active bit
  kEscalate = 1U,  ///< Re-enqueue at higher priority
  kDefer    = 2U,  ///< Keep active, process later
  kShutdown = 3U   ///< Request system shutdown
};

/// Error codes for FaultCollector operations.
enum class FaultCollectorError : uint8_t {
  kQueueFull      = 0U,
  kInvalidIndex   = 1U,
  kAlreadyStarted = 2U,
  kNotStarted     = 3U,
  kHookSlotFull   = 4U
};

// ============================================================================
// Data structures
// ============================================================================

/// Entry stored in the lock-free ring buffer (16 bytes).
struct FaultEntry {
  uint16_t fault_index;    ///< Fault point index (0 ~ MaxFaults-1)
  FaultPriority priority;  ///< Priority level
  uint8_t reserved;        ///< Alignment padding
  uint32_t detail;         ///< User-defined fault detail
  uint64_t timestamp_us;   ///< Monotonic timestamp (microseconds)
};

/// Event passed to user hook callbacks.
struct FaultEvent {
  uint16_t fault_index;       ///< Fault point index
  FaultPriority priority;     ///< Priority level
  uint32_t fault_code;        ///< Fault code from table
  uint32_t detail;            ///< User-defined detail
  uint64_t timestamp_us;      ///< Report timestamp
  uint32_t occurrence_count;  ///< Cumulative count
  bool is_first;              ///< First occurrence flag
};

/// Statistics snapshot (all counters are relaxed-atomic reads).
struct FaultStatistics {
  uint64_t total_reported;
  uint64_t total_processed;
  uint64_t total_dropped;
  std::array<uint64_t, 4> priority_reported;  ///< Per-priority reported counts
  std::array<uint64_t, 4> priority_dropped;   ///< Per-priority dropped counts
};

/// Queue usage snapshot for a single priority level.
struct QueueUsageInfo {
  uint32_t size;      ///< Current queue depth
  uint32_t capacity;  ///< Maximum queue depth
};

/// Recent fault record for diagnostic iteration.
struct RecentFaultInfo {
  uint16_t fault_index;       ///< Fault point index
  uint32_t detail;            ///< User-defined detail value
  FaultPriority priority;     ///< Fault priority level
  uint64_t timestamp_us;      ///< Steady-clock timestamp (microseconds)
};

// ============================================================================
// Lightweight fault reporting injection point (POD, 16 bytes)
// ============================================================================

/// Function pointer type for fault reporting.
/// @param fault_index  Fault point index (module-defined).
/// @param detail       User-defined detail value.
/// @param priority     Fault priority level.
/// @param ctx          User context pointer.
using FaultReportFn = void (*)(uint16_t fault_index, uint32_t detail,
                               FaultPriority priority, void* ctx);

/// Zero-overhead fault reporter injection point.
///
/// Modules store this as a plain member. When `fn` is nullptr (default),
/// no fault is reported and no code is generated on the hot path.
/// Application layer wires it to a FaultCollector instance at startup.
///
/// Usage in module:
/// @code
///   if (reporter_.fn != nullptr) {
///     reporter_.fn(kFaultIndex, detail, priority, reporter_.ctx);
///   }
/// @endcode
///
/// Wiring at application layer:
/// @code
///   node.SetFaultReporter({[](uint16_t idx, uint32_t det,
///       FaultPriority pri, void* c) {
///     static_cast<FaultCollector<8,32>*>(c)->ReportFault(idx, det, pri);
///   }, &collector});
/// @endcode
struct FaultReporter {
  FaultReportFn fn = nullptr;  ///< Report function (nullptr = disabled)
  void* ctx = nullptr;         ///< User context (typically FaultCollector*)

  /// Convenience: report a fault if reporter is wired.
  void Report(uint16_t fault_index, uint32_t detail,
              FaultPriority priority) const noexcept {
    if (fn != nullptr) {
      fn(fault_index, detail, priority, ctx);
    }
  }
};

// ============================================================================
// Internal: lock-free MPSC ring buffer (per-priority)
// ============================================================================

namespace detail {

static constexpr uint32_t kPriorityLevels = 4U;

template <uint32_t Depth>
class FaultRingBuffer {
  static_assert((Depth & (Depth - 1U)) == 0U, "Depth must be power of 2");

 public:
  FaultRingBuffer() noexcept {
    for (uint32_t i = 0U; i < Depth; ++i) {
      nodes_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  /// Try to enqueue (multi-producer safe). Returns false if full.
  bool TryPush(const FaultEntry& entry) noexcept {
    uint32_t pos = producer_pos_.load(std::memory_order_relaxed);
    for (;;) {
      auto& node = nodes_[pos & kMask];
      uint32_t seq = node.sequence.load(std::memory_order_acquire);
      int32_t diff = static_cast<int32_t>(seq) - static_cast<int32_t>(pos);
      if (diff == 0) {
        // Slot available, try to claim it
        if (producer_pos_.compare_exchange_weak(
                pos, pos + 1U, std::memory_order_relaxed)) {
          node.entry = entry;
          node.sequence.store(pos + 1U, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        // Queue full
        return false;
      } else {
        // Another producer claimed this slot, reload
        pos = producer_pos_.load(std::memory_order_relaxed);
      }
    }
  }

  /// Try to dequeue (single-consumer only). Returns false if empty.
  bool TryPop(FaultEntry& out) noexcept {
    uint32_t pos = consumer_pos_.load(std::memory_order_relaxed);
    auto& node = nodes_[pos & kMask];
    uint32_t seq = node.sequence.load(std::memory_order_acquire);
    int32_t diff = static_cast<int32_t>(seq) - static_cast<int32_t>(pos + 1U);
    if (diff < 0) {
      return false;  // Empty
    }
    out = node.entry;
    node.sequence.store(pos + Depth, std::memory_order_release);
    consumer_pos_.store(pos + 1U, std::memory_order_relaxed);
    return true;
  }

  /// Current queue depth (approximate).
  uint32_t Size() const noexcept {
    uint32_t prod = producer_pos_.load(std::memory_order_relaxed);
    uint32_t cons = consumer_pos_.load(std::memory_order_relaxed);
    return prod - cons;
  }

 private:
  static constexpr uint32_t kMask = Depth - 1U;

  struct alignas(kCacheLineSize) Node {
    std::atomic<uint32_t> sequence{0U};
    FaultEntry entry;
  };

  alignas(kCacheLineSize) std::array<Node, Depth> nodes_{};
  alignas(kCacheLineSize) std::atomic<uint32_t> producer_pos_{0U};
  alignas(kCacheLineSize) std::atomic<uint32_t> consumer_pos_{0U};
};

}  // namespace detail

// ============================================================================
// FaultCollector class template
// ============================================================================

/// @brief Fault collection and control unit with priority queues.
///
/// @tparam MaxFaults   Maximum number of fault points (1-256)
/// @tparam QueueDepth  Ring buffer depth per priority (power of 2)
/// @tparam HookBufSize FixedFunction SBO buffer size
template <uint32_t MaxFaults = 64U,
          uint32_t QueueDepth = 256U,
          uint32_t HookBufSize = 32U>
class FaultCollector {
  static_assert((QueueDepth & (QueueDepth - 1U)) == 0U,
                "QueueDepth must be power of 2");
  static_assert(MaxFaults >= 1U && MaxFaults <= 256U,
                "MaxFaults must be in [1, 256]");

 public:
  using HookFn = FixedFunction<HookAction(const FaultEvent&), HookBufSize>;
  using OverflowFn = FixedFunction<void(uint16_t, FaultPriority), HookBufSize>;
  using ShutdownFn = void (*)(void* ctx);

  FaultCollector() noexcept { ResetStatistics(); }

  ~FaultCollector() { Stop(); }

  FaultCollector(const FaultCollector&) = delete;
  FaultCollector& operator=(const FaultCollector&) = delete;

  // -- Configuration (call before Start) ------------------------------------

  /// Register a fault point in the table.
  expected<void, FaultCollectorError> RegisterFault(
      uint16_t fault_index,
      uint32_t fault_code,
      uint32_t attr = 0U,
      uint32_t err_threshold = 1U) noexcept {
    if (OSP_UNLIKELY(fault_index >= MaxFaults)) {
      return expected<void, FaultCollectorError>::error(
          FaultCollectorError::kInvalidIndex);
    }
    auto& entry = table_[fault_index];
    entry.fault_code = fault_code;
    entry.attr = attr;
    entry.err_threshold = err_threshold;
    entry.registered = true;
    return expected<void, FaultCollectorError>::success();
  }

  /// Register a user hook for a specific fault index.
  template <typename Func>
  expected<void, FaultCollectorError> RegisterHook(
      uint16_t fault_index, Func&& hook) noexcept {
    if (OSP_UNLIKELY(fault_index >= MaxFaults)) {
      return expected<void, FaultCollectorError>::error(
          FaultCollectorError::kInvalidIndex);
    }
    if (OSP_UNLIKELY(!table_[fault_index].registered)) {
      return expected<void, FaultCollectorError>::error(
          FaultCollectorError::kInvalidIndex);
    }
    table_[fault_index].hook = HookFn(static_cast<Func&&>(hook));
    return expected<void, FaultCollectorError>::success();
  }

  /// Set overflow callback (called when a fault is dropped).
  template <typename Func>
  void SetOverflowCallback(Func&& callback) noexcept {
    overflow_cb_ = OverflowFn(static_cast<Func&&>(callback));
  }

  /// Set default hook for faults without a specific hook.
  template <typename Func>
  void SetDefaultHook(Func&& hook) noexcept {
    default_hook_ = HookFn(static_cast<Func&&>(hook));
  }

  /// Set optional heartbeat for consumer thread monitoring.
  /// Must be called before Start(). The pointer must remain valid until Stop().
  void SetConsumerHeartbeat(ThreadHeartbeat* hb) noexcept {
    consumer_heartbeat_ = hb;
  }

  /// Set shutdown callback (invoked when a hook returns kShutdown).
  /// The callback is invoked from the consumer thread.
  void SetShutdownCallback(ShutdownFn fn, void* ctx = nullptr) noexcept {
    shutdown_fn_ = fn;
    shutdown_ctx_ = ctx;
  }

  // -- Lifecycle ------------------------------------------------------------

  /// Start the consumer thread.
  expected<void, FaultCollectorError> Start() noexcept {
    if (running_.load(std::memory_order_relaxed)) {
      return expected<void, FaultCollectorError>::error(
          FaultCollectorError::kAlreadyStarted);
    }
    running_.store(true, std::memory_order_release);
    consumer_thread_ = std::thread([this] { ConsumerLoop(); });
    OSP_LOG_INFO("FCCU", "Consumer thread started");
    return expected<void, FaultCollectorError>::success();
  }

  /// Stop the consumer thread (blocks until joined).
  void Stop() noexcept {
    if (!running_.load(std::memory_order_relaxed)) {
      return;
    }
    running_.store(false, std::memory_order_release);
    wake_cv_.notify_one();
    if (consumer_thread_.joinable()) {
      consumer_thread_.join();
    }
    OSP_LOG_INFO("FCCU", "Consumer thread stopped");
  }

  /// Check if consumer thread is running.
  bool IsRunning() const noexcept {
    return running_.load(std::memory_order_relaxed);
  }

  /// Check if a hook has requested system shutdown.
  bool IsShutdownRequested() const noexcept {
    return shutdown_requested_.load(std::memory_order_acquire);
  }

  // -- Reporting (thread-safe, hot path) ------------------------------------

  /// Report a fault (multi-producer safe).
  expected<void, FaultCollectorError> ReportFault(
      uint16_t fault_index,
      uint32_t detail = 0U,
      FaultPriority priority = FaultPriority::kMedium) noexcept {
    if (OSP_UNLIKELY(fault_index >= MaxFaults)) {
      return expected<void, FaultCollectorError>::error(
          FaultCollectorError::kInvalidIndex);
    }

    uint32_t pri = static_cast<uint32_t>(priority);
    auto& queue = queues_[pri];

    // Priority admission control
    if (OSP_UNLIKELY(!AdmitByPriority(pri, queue.Size()))) {
      stats_dropped_[pri].fetch_add(1U, std::memory_order_relaxed);
      stats_total_dropped_.fetch_add(1U, std::memory_order_relaxed);
      if (overflow_cb_) {
        overflow_cb_(fault_index, priority);
      }
      return expected<void, FaultCollectorError>::error(
          FaultCollectorError::kQueueFull);
    }

    FaultEntry entry{};
    entry.fault_index = fault_index;
    entry.priority = priority;
    entry.detail = detail;
    entry.timestamp_us = SteadyNowUs();

    if (OSP_UNLIKELY(!queue.TryPush(entry))) {
      stats_dropped_[pri].fetch_add(1U, std::memory_order_relaxed);
      stats_total_dropped_.fetch_add(1U, std::memory_order_relaxed);
      if (overflow_cb_) {
        overflow_cb_(fault_index, priority);
      }
      return expected<void, FaultCollectorError>::error(
          FaultCollectorError::kQueueFull);
    }

    // Update stats and active bitmap
    stats_reported_[pri].fetch_add(1U, std::memory_order_relaxed);
    stats_total_reported_.fetch_add(1U, std::memory_order_relaxed);
    SetFaultActive(fault_index);

    // Record to recent ring buffer
    {
      std::lock_guard<std::mutex> lock(recent_mutex_);
      auto& slot = recent_ring_[recent_head_];
      slot.fault_index = fault_index;
      slot.detail = detail;
      slot.priority = priority;
      slot.timestamp_us = entry.timestamp_us;
      recent_head_ = (recent_head_ + 1U) % kRecentRingSize;
      if (recent_count_ < kRecentRingSize) {
        ++recent_count_;
      }
    }

    // Wake consumer
    wake_cv_.notify_one();

    return expected<void, FaultCollectorError>::success();
  }

  // -- Query (thread-safe) --------------------------------------------------

  /// Number of currently active faults.
  uint32_t ActiveFaultCount() const noexcept {
    uint32_t count = 0U;
    for (uint32_t i = 0U; i < kBitmapWords; ++i) {
      uint64_t word = active_bitmap_[i].load(std::memory_order_relaxed);
      count += PopCount64(word);
    }
    return count;
  }

  /// Check if a specific fault is active.
  bool IsFaultActive(uint16_t fault_index) const noexcept {
    if (OSP_UNLIKELY(fault_index >= MaxFaults)) {
      return false;
    }
    uint32_t word_idx = fault_index / 64U;
    uint32_t bit_idx = fault_index % 64U;
    uint64_t word = active_bitmap_[word_idx].load(std::memory_order_relaxed);
    return (word & (1ULL << bit_idx)) != 0U;
  }

  /// Clear a specific fault (mark as resolved).
  void ClearFault(uint16_t fault_index) noexcept {
    if (OSP_UNLIKELY(fault_index >= MaxFaults)) {
      return;
    }
    ClearFaultActive(fault_index);
    OSP_LOG_DEBUG("FCCU", "Fault idx=%u cleared", fault_index);
  }

  /// Clear all active faults.
  void ClearAllFaults() noexcept {
    for (uint32_t i = 0U; i < kBitmapWords; ++i) {
      active_bitmap_[i].store(0U, std::memory_order_relaxed);
    }
    OSP_LOG_DEBUG("FCCU", "All faults cleared");
  }

  /// Get a snapshot of statistics.
  FaultStatistics GetStatistics() const noexcept {
    FaultStatistics s{};
    s.total_reported = stats_total_reported_.load(std::memory_order_relaxed);
    s.total_processed = stats_total_processed_.load(std::memory_order_relaxed);
    s.total_dropped = stats_total_dropped_.load(std::memory_order_relaxed);
    for (uint32_t i = 0U; i < detail::kPriorityLevels; ++i) {
      s.priority_reported[i] =
          stats_reported_[i].load(std::memory_order_relaxed);
      s.priority_dropped[i] =
          stats_dropped_[i].load(std::memory_order_relaxed);
    }
    return s;
  }

  /// Reset all statistics counters.
  void ResetStatistics() noexcept {
    stats_total_reported_.store(0U, std::memory_order_relaxed);
    stats_total_processed_.store(0U, std::memory_order_relaxed);
    stats_total_dropped_.store(0U, std::memory_order_relaxed);
    for (uint32_t i = 0U; i < detail::kPriorityLevels; ++i) {
      stats_reported_[i].store(0U, std::memory_order_relaxed);
      stats_dropped_[i].store(0U, std::memory_order_relaxed);
    }
  }

  /// Total queued items across all priority levels.
  uint32_t QueueDepthCurrent() const noexcept {
    uint32_t total = 0U;
    for (uint32_t i = 0U; i < detail::kPriorityLevels; ++i) {
      total += queues_[i].Size();
    }
    return total;
  }

  /// Worst-case backpressure level across all queues.
  BackpressureLevel GetBackpressureLevel() const noexcept {
    uint32_t max_usage = 0U;
    for (uint32_t i = 0U; i < detail::kPriorityLevels; ++i) {
      uint32_t usage = queues_[i].Size();
      if (usage > max_usage) {
        max_usage = usage;
      }
    }
    if (max_usage >= kHighThreshold) {
      return BackpressureLevel::kFull;
    }
    if (max_usage >= kMediumThreshold) {
      return BackpressureLevel::kCritical;
    }
    if (max_usage >= kLowThreshold) {
      return BackpressureLevel::kWarning;
    }
    return BackpressureLevel::kNormal;
  }

  /// Get queue usage for a specific priority level.
  QueueUsageInfo QueueUsage(FaultPriority pri) const noexcept {
    auto idx = static_cast<uint32_t>(pri);
    if (idx >= detail::kPriorityLevels) {
      return {0U, 0U};
    }
    return {queues_[idx].Size(), QueueDepth};
  }

  /// Iterate recent faults in reverse chronological order (newest first).
  /// Callback signature: bool(const RecentFaultInfo&) — return false to stop.
  /// Also accepts void(const RecentFaultInfo&) — iterates all entries.
  /// @param fn         Callback invoked for each recent fault.
  /// @param max_count  Maximum number of entries to visit (default: all).
  template <typename Fn>
  void ForEachRecent(Fn&& fn, uint32_t max_count = kRecentRingSize) const {
    std::lock_guard<std::mutex> lock(recent_mutex_);
    uint32_t n = (recent_count_ < max_count) ? recent_count_ : max_count;
    for (uint32_t i = 0U; i < n; ++i) {
      // Walk backwards from most recent
      uint32_t idx = (recent_head_ + kRecentRingSize - 1U - i) % kRecentRingSize;
      if constexpr (std::is_same_v<decltype(fn(recent_ring_[idx])), bool>) {
        if (!fn(recent_ring_[idx])) {
          break;
        }
      } else {
        fn(recent_ring_[idx]);
      }
    }
  }

 private:
  // -- Admission thresholds -------------------------------------------------

  static constexpr uint32_t kLowThreshold =
      (QueueDepth * 60U) / 100U;
  static constexpr uint32_t kMediumThreshold =
      (QueueDepth * 80U) / 100U;
  static constexpr uint32_t kHighThreshold =
      (QueueDepth * 99U) / 100U;

  /// Check if a fault at given priority should be admitted.
  bool AdmitByPriority(uint32_t pri, uint32_t current_depth) const noexcept {
    // kCritical: always admit if physically possible
    if (pri == static_cast<uint32_t>(FaultPriority::kCritical)) {
      return true;
    }
    if (pri == static_cast<uint32_t>(FaultPriority::kHigh)) {
      return current_depth < kHighThreshold;
    }
    if (pri == static_cast<uint32_t>(FaultPriority::kMedium)) {
      return current_depth < kMediumThreshold;
    }
    // kLow
    return current_depth < kLowThreshold;
  }

  // -- Bitmap operations ----------------------------------------------------

  static constexpr uint32_t kBitmapWords = (MaxFaults + 63U) / 64U;

  void SetFaultActive(uint16_t fault_index) noexcept {
    uint32_t word_idx = fault_index / 64U;
    uint32_t bit_idx = fault_index % 64U;
    active_bitmap_[word_idx].fetch_or(1ULL << bit_idx,
                                      std::memory_order_relaxed);
  }

  void ClearFaultActive(uint16_t fault_index) noexcept {
    uint32_t word_idx = fault_index / 64U;
    uint32_t bit_idx = fault_index % 64U;
    active_bitmap_[word_idx].fetch_and(~(1ULL << bit_idx),
                                       std::memory_order_relaxed);
  }

  static uint32_t PopCount64(uint64_t x) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<uint32_t>(__builtin_popcountll(x));
#else
    // Fallback: Hamming weight
    x = x - ((x >> 1U) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2U) & 0x3333333333333333ULL);
    x = (x + (x >> 4U)) & 0x0F0F0F0F0F0F0F0FULL;
    return static_cast<uint32_t>((x * 0x0101010101010101ULL) >> 56U);
#endif
  }

  // -- Consumer thread ------------------------------------------------------

  void ConsumerLoop() {
    OSP_LOG_DEBUG("FCCU", "Consumer loop entered");
    // Initialize heartbeat so watchdog sees us alive from the start
    if (consumer_heartbeat_ != nullptr) {
      consumer_heartbeat_->Beat();
    }
    while (running_.load(std::memory_order_acquire) &&
           !shutdown_requested_.load(std::memory_order_acquire)) {
      uint32_t processed = ProcessBatch();
      if (consumer_heartbeat_ != nullptr) {
        consumer_heartbeat_->Beat();
      }
      if (processed == 0U) {
        std::unique_lock<std::mutex> lock(wake_mutex_);
        wake_cv_.wait_for(lock, std::chrono::milliseconds(10), [this] {
          return !running_.load(std::memory_order_relaxed) ||
                 shutdown_requested_.load(std::memory_order_relaxed) ||
                 QueueDepthCurrent() > 0U;
        });
      }
    }
    // Drain remaining items before exit
    DrainAll();
    OSP_LOG_DEBUG("FCCU", "Consumer loop exited");
  }

  /// Process one batch: scan all priorities high-to-low.
  uint32_t ProcessBatch() noexcept {
    uint32_t total = 0U;
    for (uint32_t pri = 0U; pri < detail::kPriorityLevels; ++pri) {
      total += DrainQueue(pri);
    }
    return total;
  }

  /// Drain all items from a single priority queue.
  uint32_t DrainQueue(uint32_t pri) noexcept {
    uint32_t count = 0U;
    FaultEntry entry{};
    while (queues_[pri].TryPop(entry)) {
      ProcessEntry(entry);
      ++count;
    }
    return count;
  }

  /// Drain all queues (called on shutdown).
  void DrainAll() noexcept {
    for (uint32_t pri = 0U; pri < detail::kPriorityLevels; ++pri) {
      DrainQueue(pri);
    }
  }

  /// Process a single fault entry: build event, invoke hook.
  void ProcessEntry(const FaultEntry& entry) noexcept {
    uint16_t idx = entry.fault_index;
    if (OSP_UNLIKELY(idx >= MaxFaults)) {
      return;
    }

    auto& tbl = table_[idx];

    // Increment occurrence count
    uint32_t prev_count =
        tbl.occurrence_count.fetch_add(1U, std::memory_order_relaxed);

    // Build FaultEvent
    FaultEvent evt{};
    evt.fault_index = idx;
    evt.priority = entry.priority;
    evt.fault_code = tbl.fault_code;
    evt.detail = entry.detail;
    evt.timestamp_us = entry.timestamp_us;
    evt.occurrence_count = prev_count + 1U;
    evt.is_first = (prev_count == 0U);

    OSP_LOG_DEBUG("FCCU",
                  "Processing fault idx=%u code=0x%08x detail=0x%08x pri=%u",
                  idx, tbl.fault_code, entry.detail,
                  static_cast<uint32_t>(entry.priority));

    // Invoke hook
    HookAction action = HookAction::kHandled;
    if (tbl.hook) {
      action = tbl.hook(evt);
    } else if (default_hook_) {
      action = default_hook_(evt);
    }

    // Handle action
    switch (action) {
      case HookAction::kHandled:
        ClearFaultActive(idx);
        break;
      case HookAction::kEscalate:
        HandleEscalation(entry);
        break;
      case HookAction::kDefer:
        // Keep active, do nothing
        break;
      case HookAction::kShutdown:
        OSP_LOG_ERROR("FCCU",
                      "Shutdown requested by fault idx=%u code=0x%08x",
                      idx, tbl.fault_code);
        shutdown_requested_.store(true, std::memory_order_release);
        if (shutdown_fn_ != nullptr) {
          shutdown_fn_(shutdown_ctx_);
        }
        // Wake consumer loop so it exits promptly on shutdown
        wake_cv_.notify_one();
        break;
    }

    stats_total_processed_.fetch_add(1U, std::memory_order_relaxed);
  }

  /// Re-enqueue a fault at one level higher priority.
  void HandleEscalation(const FaultEntry& original) noexcept {
    uint32_t pri = static_cast<uint32_t>(original.priority);
    if (pri == 0U) {
      // Already kCritical, cannot escalate further
      OSP_LOG_WARN("FCCU",
                   "Fault idx=%u already at kCritical, cannot escalate",
                   original.fault_index);
      return;
    }

    FaultEntry escalated = original;
    escalated.priority = static_cast<FaultPriority>(pri - 1U);

    if (!queues_[pri - 1U].TryPush(escalated)) {
      OSP_LOG_WARN("FCCU",
                   "Escalation failed for fault idx=%u (queue full)",
                   original.fault_index);
    } else {
      OSP_LOG_INFO("FCCU", "Fault idx=%u escalated from pri=%u to pri=%u",
                   original.fault_index, pri, pri - 1U);
    }
  }

  // -- Fault table entry ----------------------------------------------------

  struct FaultTableEntry {
    uint32_t fault_code = 0U;
    uint32_t attr = 0U;
    uint32_t err_threshold = 1U;
    bool registered = false;
    std::atomic<uint32_t> occurrence_count{0U};
    HookFn hook;
  };

  // -- Member data ----------------------------------------------------------

  // Per-priority ring buffers
  std::array<detail::FaultRingBuffer<QueueDepth>,
             detail::kPriorityLevels> queues_;

  // Fault table
  std::array<FaultTableEntry, MaxFaults> table_{};

  // Active fault bitmap
  std::array<std::atomic<uint64_t>, kBitmapWords> active_bitmap_{};

  // Statistics (atomic counters)
  std::atomic<uint64_t> stats_total_reported_{0U};
  std::atomic<uint64_t> stats_total_processed_{0U};
  std::atomic<uint64_t> stats_total_dropped_{0U};
  std::array<std::atomic<uint64_t>, detail::kPriorityLevels> stats_reported_{};
  std::array<std::atomic<uint64_t>, detail::kPriorityLevels> stats_dropped_{};

  // Callbacks
  HookFn default_hook_;
  OverflowFn overflow_cb_;
  ShutdownFn shutdown_fn_{nullptr};
  void* shutdown_ctx_{nullptr};

  // Consumer thread
  std::atomic<bool> running_{false};
  std::atomic<bool> shutdown_requested_{false};
  ThreadHeartbeat* consumer_heartbeat_{nullptr};
  std::thread consumer_thread_;
  std::mutex wake_mutex_;
  std::condition_variable wake_cv_;

  // Recent fault ring buffer (diagnostic)
  static constexpr uint32_t kRecentRingSize = 16U;
  mutable std::mutex recent_mutex_;
  std::array<RecentFaultInfo, kRecentRingSize> recent_ring_{};
  uint32_t recent_head_{0U};
  uint32_t recent_count_{0U};
};

}  // namespace osp

#endif  // OSP_FAULT_COLLECTOR_HPP_
