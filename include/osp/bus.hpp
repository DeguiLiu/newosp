/**
 * @file bus.hpp
 * @brief Lock-free MPSC message bus with priority-based admission control.
 *
 * Adapted from MCCC (Message-Centric Component Communication) by liudegui.
 * Provides type-safe publish/subscribe with zero heap allocation in hot path.
 *
 * @tparam PayloadVariant A std::variant<...> of user-defined message types.
 *
 * Usage:
 *   struct SensorData { float temp; };
 *   struct MotorCmd   { int speed; };
 *   using MyPayload = std::variant<SensorData, MotorCmd>;
 *   using MyBus = osp::AsyncBus<MyPayload>;
 *
 *   MyBus::Instance().Subscribe<SensorData>([](const auto& env) { ... });
 *   MyBus::Instance().Publish(SensorData{25.0f}, 1);
 */

#ifndef OSP_BUS_HPP_
#define OSP_BUS_HPP_

#include "osp/platform.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <variant>

namespace osp {

// ============================================================================
// Overloaded visitor pattern (C++17)
// ============================================================================

template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};

template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

// ============================================================================
// Compile-time Configuration
// ============================================================================

#ifndef OSP_BUS_QUEUE_DEPTH
#define OSP_BUS_QUEUE_DEPTH 4096U
#endif

#ifndef OSP_BUS_MAX_MESSAGE_TYPES
#define OSP_BUS_MAX_MESSAGE_TYPES 8U
#endif

#ifndef OSP_BUS_MAX_CALLBACKS_PER_TYPE
#define OSP_BUS_MAX_CALLBACKS_PER_TYPE 16U
#endif

#ifndef OSP_BUS_BATCH_SIZE
#define OSP_BUS_BATCH_SIZE 256U
#endif

// ============================================================================
// Message Priority
// ============================================================================

enum class MessagePriority : uint8_t {
  kLow = 0,     /** Dropped when queue >= 60% full */
  kMedium = 1,  /** Dropped when queue >= 80% full */
  kHigh = 2     /** Dropped when queue >= 99% full */
};

// ============================================================================
// Message Header
// ============================================================================

struct MessageHeader {
  uint64_t msg_id;
  uint64_t timestamp_us;
  uint32_t sender_id;
  MessagePriority priority;

  MessageHeader() noexcept
      : msg_id(0), timestamp_us(0), sender_id(0),
        priority(MessagePriority::kMedium) {}

  MessageHeader(uint64_t id, uint64_t ts, uint32_t sender,
                MessagePriority prio) noexcept
      : msg_id(id), timestamp_us(ts), sender_id(sender), priority(prio) {}
};

// ============================================================================
// Message Envelope
// ============================================================================

template <typename PayloadVariant>
struct MessageEnvelope {
  MessageHeader header;
  PayloadVariant payload;

  MessageEnvelope() noexcept : header(), payload() {}

  MessageEnvelope(const MessageHeader& hdr, PayloadVariant&& pl) noexcept
      : header(hdr), payload(std::move(pl)) {}

  MessageEnvelope(MessageEnvelope&&) noexcept = default;
  MessageEnvelope& operator=(MessageEnvelope&&) noexcept = default;
  MessageEnvelope(const MessageEnvelope&) = default;
  MessageEnvelope& operator=(const MessageEnvelope&) = default;
};

// ============================================================================
// Bus Error
// ============================================================================

enum class BusError : uint8_t {
  kQueueFull = 0,
  kOverflowDetected,
  kCallbacksFull
};

using BusErrorCallback = void (*)(BusError, uint64_t);

// ============================================================================
// Subscription Handle
// ============================================================================

struct SubscriptionHandle {
  uint32_t type_index;
  uint32_t callback_id;

  bool IsValid() const noexcept { return callback_id != UINT32_MAX; }

  static SubscriptionHandle Invalid() noexcept { return {0, UINT32_MAX}; }
};

// ============================================================================
// Bus Statistics
// ============================================================================

struct alignas(osp::kCacheLineSize) BusStatistics {
  std::atomic<uint64_t> messages_published{0};
  std::atomic<uint64_t> messages_dropped{0};
  std::atomic<uint64_t> messages_processed{0};
  std::atomic<uint64_t> admission_rechecks{0};

  void Reset() noexcept {
    messages_published.store(0, std::memory_order_relaxed);
    messages_dropped.store(0, std::memory_order_relaxed);
    messages_processed.store(0, std::memory_order_relaxed);
    admission_rechecks.store(0, std::memory_order_relaxed);
  }
};

struct BusStatisticsSnapshot {
  uint64_t messages_published;
  uint64_t messages_dropped;
  uint64_t messages_processed;
  uint64_t admission_rechecks;
};

// ============================================================================
// Backpressure Level
// ============================================================================

enum class BackpressureLevel : uint8_t {
  kNormal = 0,    /**< < 75% full */
  kWarning = 1,   /**< 75-90% full */
  kCritical = 2,  /**< 90-100% full */
  kFull = 3       /**< 100% full */
};

// ============================================================================
// Compile-time Variant Index Helper
// ============================================================================

namespace detail {

template <typename T, size_t I, typename Variant>
struct VariantIndexImpl;

template <typename T, size_t I>
struct VariantIndexImpl<T, I, std::variant<>> {
  static constexpr size_t value = static_cast<size_t>(-1);
};

template <typename T, size_t I, typename First, typename... Rest>
struct VariantIndexImpl<T, I, std::variant<First, Rest...>> {
  static constexpr size_t value =
      std::is_same<T, First>::value
          ? I
          : VariantIndexImpl<T, I + 1, std::variant<Rest...>>::value;
};

}  // namespace detail

template <typename T, typename Variant>
struct VariantIndex;

template <typename T, typename... Types>
struct VariantIndex<T, std::variant<Types...>> {
  static constexpr size_t value =
      detail::VariantIndexImpl<T, 0, std::variant<Types...>>::value;
  static_assert(value != static_cast<size_t>(-1),
                "Type not found in PayloadVariant");
};

// ============================================================================
// AsyncBus<PayloadVariant> - Lock-free MPSC Message Bus
// ============================================================================

/**
 * @brief Lock-free MPSC message bus with priority-based admission control.
 *
 * Singleton per PayloadVariant type. Thread-safe for multiple producers and
 * a single consumer. Uses a pre-allocated ring buffer for zero-copy message
 * delivery in steady state.
 *
 * @tparam PayloadVariant A std::variant<...> of user-defined message types.
 */
template <typename PayloadVariant>
class AsyncBus {
 public:
  using EnvelopeType = MessageEnvelope<PayloadVariant>;
  using CallbackType = std::function<void(const EnvelopeType&)>;

  static constexpr uint32_t kQueueDepth =
      static_cast<uint32_t>(OSP_BUS_QUEUE_DEPTH);
  static constexpr uint32_t kBufferMask = kQueueDepth - 1;
  static constexpr uint32_t kBatchSize =
      static_cast<uint32_t>(OSP_BUS_BATCH_SIZE);

  static constexpr uint32_t kLowThreshold = (kQueueDepth * 60) / 100;
  static constexpr uint32_t kMediumThreshold = (kQueueDepth * 80) / 100;
  static constexpr uint32_t kHighThreshold = (kQueueDepth * 99) / 100;

  static constexpr uint64_t kMsgIdWrapThreshold =
      UINT64_MAX - 10000;

  static_assert((kQueueDepth & (kQueueDepth - 1)) == 0,
                "Queue depth must be power of 2");

  /** @brief Meyer's singleton - one bus per PayloadVariant type */
  static AsyncBus& Instance() noexcept {
    static AsyncBus instance;
    return instance;
  }

  // ======================== Error Callback ========================

  void SetErrorCallback(BusErrorCallback callback) noexcept {
    error_callback_.store(callback, std::memory_order_release);
  }

  // ======================== Publish API ========================

  /**
   * @brief Publish a message with default (MEDIUM) priority.
   * @param payload The message payload (moved).
   * @param sender_id Sender identifier for tracing.
   * @return true if published, false if dropped.
   */
  bool Publish(PayloadVariant&& payload, uint32_t sender_id) noexcept {
    return PublishInternal(std::move(payload), sender_id,
                           GetTimestampUs(), MessagePriority::kMedium);
  }

  /**
   * @brief Publish a message with specified priority.
   */
  bool PublishWithPriority(PayloadVariant&& payload, uint32_t sender_id,
                            MessagePriority priority) noexcept {
    return PublishInternal(std::move(payload), sender_id,
                           GetTimestampUs(), priority);
  }

  /**
   * @brief Publish with pre-computed timestamp (for latency-sensitive paths).
   */
  bool PublishFast(PayloadVariant&& payload, uint32_t sender_id,
                    uint64_t timestamp_us) noexcept {
    return PublishInternal(std::move(payload), sender_id,
                           timestamp_us, MessagePriority::kMedium);
  }

  // ======================== Subscribe API ========================

  /**
   * @brief Subscribe to messages of type T.
   *
   * The callback receives the full envelope (header + payload).
   *
   * @tparam T The message type to subscribe to (must be in PayloadVariant).
   * @tparam Func Callable with signature void(const EnvelopeType&).
   * @return Valid handle on success, Invalid handle if callbacks are full.
   */
  template <typename T, typename Func>
  SubscriptionHandle Subscribe(Func&& func) noexcept {
    constexpr size_t type_idx =
        VariantIndex<T, PayloadVariant>::value;
    static_assert(type_idx < OSP_BUS_MAX_MESSAGE_TYPES,
                  "Type index exceeds OSP_BUS_MAX_MESSAGE_TYPES");

    std::unique_lock<std::shared_mutex> lock(callback_mutex_);

    CallbackSlot& slot = callback_table_[type_idx];
    uint32_t callback_id = next_callback_id_++;

    for (uint32_t i = 0; i < OSP_BUS_MAX_CALLBACKS_PER_TYPE; ++i) {
      if (!slot.entries[i].active) {
        slot.entries[i].id = callback_id;
        slot.entries[i].callback = CallbackType(std::forward<Func>(func));
        slot.entries[i].active = true;
        ++slot.count;
        return SubscriptionHandle{static_cast<uint32_t>(type_idx), callback_id};
      }
    }

    return SubscriptionHandle::Invalid();
  }

  /**
   * @brief Unsubscribe a previously registered callback.
   * @param handle The subscription handle returned by Subscribe.
   * @return true if the callback was found and removed.
   */
  bool Unsubscribe(const SubscriptionHandle& handle) noexcept {
    if (!handle.IsValid()) return false;
    if (handle.type_index >= OSP_BUS_MAX_MESSAGE_TYPES) return false;

    CallbackType old_callback;
    {
      std::unique_lock<std::shared_mutex> lock(callback_mutex_);

      CallbackSlot& slot = callback_table_[handle.type_index];
      for (uint32_t i = 0; i < OSP_BUS_MAX_CALLBACKS_PER_TYPE; ++i) {
        if (slot.entries[i].active &&
            slot.entries[i].id == handle.callback_id) {
          slot.entries[i].active = false;
          old_callback = std::move(slot.entries[i].callback);
          slot.entries[i].callback = nullptr;
          --slot.count;
          break;
        }
      }
    }
    // old_callback destroyed outside lock (safe re-entrancy)
    return static_cast<bool>(old_callback);
  }

  // ======================== Processing API ========================

  /**
   * @brief Process up to kBatchSize pending messages.
   *
   * Must be called from a single consumer thread. Dispatches messages
   * to registered callbacks.
   *
   * @return Number of messages processed.
   */
  uint32_t ProcessBatch() noexcept {
    uint32_t processed = 0;
    uint32_t cons_pos = consumer_pos_.load(std::memory_order_relaxed);

    for (uint32_t i = 0; i < kBatchSize; ++i) {
      RingBufferNode& node = ring_buffer_[cons_pos & kBufferMask];

      uint32_t expected_seq = cons_pos + 1;
      uint32_t seq = node.sequence.load(std::memory_order_acquire);

      if (seq != expected_seq) break;

      DispatchMessage(node.envelope);

      node.sequence.store(cons_pos + kQueueDepth,
                          std::memory_order_release);
      ++cons_pos;
      ++processed;
    }

    if (processed > 0) {
      consumer_pos_.store(cons_pos, std::memory_order_relaxed);
      stats_.messages_processed.fetch_add(processed,
                                          std::memory_order_relaxed);
    }
    return processed;
  }

  // ======================== Query API ========================

  /** @brief Current number of pending messages in the queue. */
  uint32_t Depth() const noexcept {
    uint32_t prod = producer_pos_.load(std::memory_order_acquire);
    uint32_t cons = consumer_pos_.load(std::memory_order_acquire);
    return prod - cons;
  }

  /** @brief Queue utilization as percentage (0-100). */
  uint32_t UtilizationPercent() const noexcept {
    return (Depth() * 100) / kQueueDepth;
  }

  /** @brief Current backpressure level based on queue depth. */
  BackpressureLevel GetBackpressureLevel() const noexcept {
    uint32_t depth = Depth();
    if (depth >= kQueueDepth) return BackpressureLevel::kFull;
    if (depth >= (kQueueDepth * 90) / 100) return BackpressureLevel::kCritical;
    if (depth >= (kQueueDepth * 75) / 100) return BackpressureLevel::kWarning;
    return BackpressureLevel::kNormal;
  }

  /** @brief Get a snapshot of bus statistics. */
  BusStatisticsSnapshot GetStatistics() const noexcept {
    return BusStatisticsSnapshot{
        stats_.messages_published.load(std::memory_order_relaxed),
        stats_.messages_dropped.load(std::memory_order_relaxed),
        stats_.messages_processed.load(std::memory_order_relaxed),
        stats_.admission_rechecks.load(std::memory_order_relaxed)};
  }

  /** @brief Reset all statistics counters. */
  void ResetStatistics() noexcept { stats_.Reset(); }

  /**
   * @brief Reset the entire bus state (for testing only).
   *
   * Clears all subscriptions, resets queue positions and statistics.
   * NOT thread-safe - must be called when no other threads use the bus.
   */
  void Reset() noexcept {
    // Clear all subscriptions
    {
      std::unique_lock<std::shared_mutex> lock(callback_mutex_);
      for (auto& slot : callback_table_) {
        for (auto& entry : slot.entries) {
          entry.active = false;
          entry.callback = nullptr;
        }
        slot.count = 0;
      }
    }

    // Reset ring buffer sequences
    for (uint32_t i = 0; i < kQueueDepth; ++i) {
      ring_buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }
    producer_pos_.store(0, std::memory_order_relaxed);
    cached_consumer_pos_.store(0, std::memory_order_relaxed);
    consumer_pos_.store(0, std::memory_order_relaxed);
    next_msg_id_.store(1, std::memory_order_relaxed);
    next_callback_id_ = 1;
    stats_.Reset();
    error_callback_.store(nullptr, std::memory_order_relaxed);
  }

 private:
  // ======================== Ring Buffer Node ========================

  struct alignas(osp::kCacheLineSize) RingBufferNode {
    std::atomic<uint32_t> sequence{0};
    EnvelopeType envelope;
  };

  // ======================== Callback Storage ========================

  struct CallbackEntry {
    uint32_t id{0};
    CallbackType callback{nullptr};
    bool active{false};
  };

  struct CallbackSlot {
    std::array<CallbackEntry, OSP_BUS_MAX_CALLBACKS_PER_TYPE> entries{};
    uint32_t count{0};
  };

  // ======================== Constructor ========================

  AsyncBus() noexcept
      : producer_pos_(0),
        cached_consumer_pos_(0),
        consumer_pos_(0),
        next_msg_id_(1),
        next_callback_id_(1) {
    for (uint32_t i = 0; i < kQueueDepth; ++i) {
      ring_buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  ~AsyncBus() = default;
  AsyncBus(const AsyncBus&) = delete;
  AsyncBus& operator=(const AsyncBus&) = delete;
  AsyncBus(AsyncBus&&) = delete;
  AsyncBus& operator=(AsyncBus&&) = delete;

  // ======================== Internal Helpers ========================

  uint32_t GetThresholdForPriority(MessagePriority priority) const noexcept {
    switch (priority) {
      case MessagePriority::kHigh:
        return kHighThreshold;
      case MessagePriority::kMedium:
        return kMediumThreshold;
      case MessagePriority::kLow:
      default:
        return kLowThreshold;
    }
  }

  bool PublishInternal(PayloadVariant&& payload, uint32_t sender_id,
                        uint64_t timestamp_us,
                        MessagePriority priority) noexcept {
    // Message ID overflow check
    uint64_t current_id = next_msg_id_.load(std::memory_order_relaxed);
    if (OSP_UNLIKELY(current_id >= kMsgIdWrapThreshold)) {
      ReportError(BusError::kOverflowDetected, current_id);
      return false;
    }

    // Admission control with cached consumer position
    uint32_t threshold = GetThresholdForPriority(priority);
    uint32_t prod = producer_pos_.load(std::memory_order_relaxed);
    uint32_t cached_cons =
        cached_consumer_pos_.load(std::memory_order_relaxed);
    uint32_t estimated_depth = prod - cached_cons;

    if (estimated_depth >= threshold) {
      // Re-read real consumer position
      uint32_t real_cons =
          consumer_pos_.load(std::memory_order_acquire);
      cached_consumer_pos_.store(real_cons, std::memory_order_relaxed);
      stats_.admission_rechecks.fetch_add(1, std::memory_order_relaxed);

      uint32_t real_depth = prod - real_cons;
      if (real_depth >= threshold) {
        stats_.messages_dropped.fetch_add(1, std::memory_order_relaxed);
        ReportError(BusError::kQueueFull, current_id);
        return false;
      }
    }

    // CAS loop to claim a producer slot (MPSC)
    uint32_t prod_pos;
    RingBufferNode* target;

    do {
      prod_pos = producer_pos_.load(std::memory_order_relaxed);
      target = &ring_buffer_[prod_pos & kBufferMask];

      uint32_t seq = target->sequence.load(std::memory_order_acquire);
      if (seq != prod_pos) {
        stats_.messages_dropped.fetch_add(1, std::memory_order_relaxed);
        ReportError(BusError::kQueueFull, current_id);
        return false;
      }
    } while (!producer_pos_.compare_exchange_weak(
        prod_pos, prod_pos + 1,
        std::memory_order_acq_rel, std::memory_order_relaxed));

    // Fill envelope
    uint64_t msg_id =
        next_msg_id_.fetch_add(1, std::memory_order_relaxed);
    target->envelope.header =
        MessageHeader{msg_id, timestamp_us, sender_id, priority};
    target->envelope.payload = std::move(payload);

    // Publish (make visible to consumer)
    target->sequence.store(prod_pos + 1, std::memory_order_release);

    stats_.messages_published.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  void DispatchMessage(const EnvelopeType& envelope) noexcept {
    size_t type_idx = envelope.payload.index();
    if (type_idx >= OSP_BUS_MAX_MESSAGE_TYPES) return;

    std::shared_lock<std::shared_mutex> lock(callback_mutex_);

    const CallbackSlot& slot = callback_table_[type_idx];
    if (slot.count == 0) return;

    for (uint32_t i = 0; i < OSP_BUS_MAX_CALLBACKS_PER_TYPE; ++i) {
      if (slot.entries[i].active) {
        slot.entries[i].callback(envelope);
      }
    }
  }

  void ReportError(BusError error, uint64_t msg_id) const noexcept {
    BusErrorCallback cb =
        error_callback_.load(std::memory_order_acquire);
    if (cb != nullptr) {
      cb(error, msg_id);
    }
  }

  static uint64_t GetTimestampUs() noexcept {
    auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch());
    return static_cast<uint64_t>(us.count());
  }

  // ======================== Data Members ========================
  // Aligned to prevent false sharing between producer and consumer.

  alignas(osp::kCacheLineSize)
      std::array<RingBufferNode, kQueueDepth> ring_buffer_;

  alignas(osp::kCacheLineSize) std::atomic<uint32_t> producer_pos_;
  std::atomic<uint32_t> cached_consumer_pos_;

  alignas(osp::kCacheLineSize) std::atomic<uint32_t> consumer_pos_;

  alignas(osp::kCacheLineSize) std::atomic<uint64_t> next_msg_id_;
  uint32_t next_callback_id_;

  BusStatistics stats_;
  std::array<CallbackSlot, OSP_BUS_MAX_MESSAGE_TYPES> callback_table_;
  mutable std::shared_mutex callback_mutex_;
  std::atomic<BusErrorCallback> error_callback_{nullptr};
};

}  // namespace osp

#endif  // OSP_BUS_HPP_
