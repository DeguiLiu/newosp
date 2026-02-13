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
#include <thread>
#include <type_traits>
#include <variant>

namespace osp {

// ============================================================================
// FNV-1a Hash Function
// ============================================================================

/**
 * @brief Compute FNV-1a 32-bit hash of a null-terminated string.
 *
 * Used for topic-based routing. Returns 0 for nullptr input.
 */
constexpr uint32_t Fnv1a32(const char* str) noexcept {
  if (str == nullptr) return 0;
  uint32_t hash = 2166136261u;
  while (*str) {
    hash ^= static_cast<uint32_t>(*str++);
    hash *= 16777619u;
  }
  return hash;
}

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
  uint32_t topic_hash;  // 0 = no topic filter, non-zero = FNV-1a hash
  MessagePriority priority;

  MessageHeader() noexcept
      : msg_id(0), timestamp_us(0), sender_id(0), topic_hash(0),
        priority(MessagePriority::kMedium) {}

  MessageHeader(uint64_t id, uint64_t ts, uint32_t sender,
                MessagePriority prio) noexcept
      : msg_id(id), timestamp_us(ts), sender_id(sender), topic_hash(0), priority(prio) {}

  MessageHeader(uint64_t id, uint64_t ts, uint32_t sender,
                uint32_t topic, MessagePriority prio) noexcept
      : msg_id(id), timestamp_us(ts), sender_id(sender), topic_hash(topic),
        priority(prio) {}
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

// ============================================================================
// SpinLock with Exponential Backoff (osp::detail)
// ============================================================================

namespace detail {

/**
 * @brief Lightweight exclusive spin lock with exponential backoff.
 *
 * Based on std::atomic_flag. Uses architecture-specific pause/yield hints
 * to reduce contention on the cache line. Zero heap allocation, trivially
 * constructible.
 */
class SpinLock {
 public:
  SpinLock() noexcept = default;

  void lock() noexcept {
    uint32_t backoff = 1;
    while (flag_.test_and_set(std::memory_order_acquire)) {
      for (uint32_t i = 0; i < backoff; ++i) {
        CpuRelax();
      }
      if (backoff < kMaxBackoff) {
        backoff <<= 1;
      }
    }
  }

  void unlock() noexcept { flag_.clear(std::memory_order_release); }

  bool try_lock() noexcept {
    return !flag_.test_and_set(std::memory_order_acquire);
  }

 private:
  static constexpr uint32_t kMaxBackoff = 1024;

  static void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
  }

  std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

/**
 * @brief Reader-writer spin lock using atomic counter with exponential backoff.
 *
 * Readers increment a shared counter; a writer sets a flag and waits for all
 * readers to drain. Designed for short critical sections where
 * std::shared_mutex overhead dominates actual work (e.g., callback dispatch).
 *
 * State encoding:
 *   state_ >= 0  : number of active readers (0 = unlocked)
 *   state_ == -1 : writer holds exclusive lock
 */
class SharedSpinLock {
 public:
  SharedSpinLock() noexcept = default;

  /** @brief Acquire shared (reader) lock. */
  void lock_shared() noexcept {
    uint32_t backoff = 1;
    for (;;) {
      int32_t state = state_.load(std::memory_order_relaxed);
      // Only acquire if no writer is active (state >= 0)
      if (state >= 0 &&
          state_.compare_exchange_weak(state, state + 1,
                                       std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
        return;
      }
      Backoff(backoff);
    }
  }

  /** @brief Release shared (reader) lock. */
  void unlock_shared() noexcept {
    state_.fetch_sub(1, std::memory_order_release);
  }

  /** @brief Acquire exclusive (writer) lock. */
  void lock() noexcept {
    uint32_t backoff = 1;
    // Step 1: Acquire writer flag (transition 0 -> -1)
    for (;;) {
      int32_t expected = 0;
      if (state_.compare_exchange_weak(expected, kWriterActive,
                                       std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
        return;
      }
      Backoff(backoff);
    }
  }

  /** @brief Release exclusive (writer) lock. */
  void unlock() noexcept {
    state_.store(0, std::memory_order_release);
  }

 private:
  static constexpr int32_t kWriterActive = -1;
  static constexpr uint32_t kMaxBackoff = 1024;

  static void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
  }

  static void Backoff(uint32_t& backoff) noexcept {
    for (uint32_t i = 0; i < backoff; ++i) {
      CpuRelax();
    }
    if (backoff < kMaxBackoff) {
      backoff <<= 1;
    }
  }

  std::atomic<int32_t> state_{0};
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
                           GetTimestampUs(), MessagePriority::kMedium, 0);
  }

  /**
   * @brief Publish a message with specified priority.
   */
  bool PublishWithPriority(PayloadVariant&& payload, uint32_t sender_id,
                            MessagePriority priority) noexcept {
    return PublishInternal(std::move(payload), sender_id,
                           GetTimestampUs(), priority, 0);
  }

  /**
   * @brief Publish with pre-computed timestamp (for latency-sensitive paths).
   */
  bool PublishFast(PayloadVariant&& payload, uint32_t sender_id,
                    uint64_t timestamp_us) noexcept {
    return PublishInternal(std::move(payload), sender_id,
                           timestamp_us, MessagePriority::kMedium, 0);
  }

  /**
   * @brief Publish a message with topic.
   * @param payload The message payload (moved).
   * @param sender_id Sender identifier for tracing.
   * @param topic Topic string (null-terminated).
   * @return true if published, false if dropped.
   */
  bool PublishTopic(PayloadVariant&& payload, uint32_t sender_id,
                     const char* topic) noexcept {
    uint32_t topic_hash = Fnv1a32(topic);
    return PublishInternal(std::move(payload), sender_id,
                           GetTimestampUs(), MessagePriority::kMedium, topic_hash);
  }

  /**
   * @brief Publish a message with topic and priority.
   */
  bool PublishTopicWithPriority(PayloadVariant&& payload, uint32_t sender_id,
                                  const char* topic, MessagePriority priority) noexcept {
    uint32_t topic_hash = Fnv1a32(topic);
    return PublishInternal(std::move(payload), sender_id,
                           GetTimestampUs(), priority, topic_hash);
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

    callback_spin_lock_.lock();

    CallbackSlot& slot = callback_table_[type_idx];
    uint32_t callback_id = next_callback_id_++;

    for (uint32_t i = 0; i < OSP_BUS_MAX_CALLBACKS_PER_TYPE; ++i) {
      if (!slot.entries[i].active) {
        slot.entries[i].id = callback_id;
        slot.entries[i].callback = CallbackType(std::forward<Func>(func));
        slot.entries[i].active = true;
        ++slot.count;
        SubscriptionHandle handle{static_cast<uint32_t>(type_idx), callback_id};
        callback_spin_lock_.unlock();
        return handle;
      }
    }

    callback_spin_lock_.unlock();
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
      callback_spin_lock_.lock();

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

      callback_spin_lock_.unlock();
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

      // Prefetch next ring buffer slot for reduced cache miss latency
#ifdef __GNUC__
      if (i + 1 < kBatchSize) {
        __builtin_prefetch(
            &ring_buffer_[(cons_pos + 1) & kBufferMask], 0, 1);
      }
#endif

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
      callback_spin_lock_.lock();
      for (auto& slot : callback_table_) {
        for (auto& entry : slot.entries) {
          entry.active = false;
          entry.callback = nullptr;
        }
        slot.count = 0;
      }
      callback_spin_lock_.unlock();
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
                        MessagePriority priority, uint32_t topic_hash) noexcept {
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
        MessageHeader{msg_id, timestamp_us, sender_id, topic_hash, priority};
    target->envelope.payload = std::move(payload);

    // Publish (make visible to consumer)
    target->sequence.store(prod_pos + 1, std::memory_order_release);

    stats_.messages_published.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  void DispatchMessage(const EnvelopeType& envelope) noexcept {
    size_t type_idx = envelope.payload.index();
    if (type_idx >= OSP_BUS_MAX_MESSAGE_TYPES) return;

    callback_spin_lock_.lock_shared();

    const CallbackSlot& slot = callback_table_[type_idx];
    if (slot.count == 0) {
      callback_spin_lock_.unlock_shared();
      return;
    }

    for (uint32_t i = 0; i < OSP_BUS_MAX_CALLBACKS_PER_TYPE; ++i) {
      if (slot.entries[i].active) {
        slot.entries[i].callback(envelope);
      }
    }

    callback_spin_lock_.unlock_shared();
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
  mutable detail::SharedSpinLock callback_spin_lock_;
  std::atomic<BusErrorCallback> error_callback_{nullptr};
};

}  // namespace osp

#endif  // OSP_BUS_HPP_
