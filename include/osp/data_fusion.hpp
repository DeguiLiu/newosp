/**
 * @file data_fusion.hpp
 * @brief Multi-message alignment trigger and time-synchronized data fusion.
 *
 * Inspired by CyberRT DataFusion. Provides FusedSubscription for triggering
 * a callback when all subscribed message types have been received, and
 * TimeSynchronizer for timestamp-aligned fusion within a time window.
 *
 * @tparam PayloadVariant A std::variant<...> of user-defined message types.
 * @tparam MsgTypes The subset of types to fuse.
 */

#ifndef OSP_DATA_FUSION_HPP_
#define OSP_DATA_FUSION_HPP_

#include "osp/platform.hpp"
#include "osp/bus.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace osp {

// ============================================================================
// Data Fusion Error
// ============================================================================

enum class DataFusionError : uint8_t {
  kCallbackNotSet = 0,
  kAlreadyActive,
  kNotActive,
  kWindowTimeout
};

// ============================================================================
// Detail: compile-time helpers for parameter pack iteration
// ============================================================================

namespace detail {

/// Index of type T within the parameter pack Types...
template <typename T, typename... Types>
struct PackIndex;

template <typename T, typename First, typename... Rest>
struct PackIndex<T, First, Rest...> {
  static constexpr size_t value =
      std::is_same<T, First>::value ? 0 : 1 + PackIndex<T, Rest...>::value;
};

template <typename T>
struct PackIndex<T> {
  static constexpr size_t value = 0;
  // Will cause static_assert failure via tuple index bounds if T not found.
};

/// Subscribe helper: subscribes to each MsgType on the bus and stores handles.
template <typename PayloadVariant, typename... MsgTypes>
struct SubscribeHelper;

template <typename PayloadVariant>
struct SubscribeHelper<PayloadVariant> {
  template <typename Self>
  static void DoSubscribe(Self* /*self*/,
                           AsyncBus<PayloadVariant>& /*bus*/,
                           SubscriptionHandle* /*handles*/) {}
};

template <typename PayloadVariant, typename First, typename... Rest>
struct SubscribeHelper<PayloadVariant, First, Rest...> {
  template <typename Self>
  static void DoSubscribe(Self* self,
                           AsyncBus<PayloadVariant>& bus,
                           SubscriptionHandle* handles) {
    constexpr size_t idx = PackIndex<First, First, Rest...>::value;
    // The handle index for First is: total_count - remaining_count
    constexpr size_t handle_idx = sizeof...(Rest) == 0
        ? (self->kNumTypes - 1)
        : (self->kNumTypes - 1 - sizeof...(Rest));
    (void)idx;

    handles[handle_idx] = bus.template Subscribe<First>(
        [self](const MessageEnvelope<PayloadVariant>& env) {
          self->template OnMessage<First>(env);
        });

    SubscribeHelper<PayloadVariant, Rest...>::DoSubscribe(
        self, bus, handles);
  }
};

}  // namespace detail

// ============================================================================
// FusedSubscription<PayloadVariant, MsgTypes...>
// ============================================================================

/**
 * @brief Multi-message alignment trigger.
 *
 * Subscribes to multiple message types on an AsyncBus. When all MsgTypes
 * have been received at least once, fires the callback with a tuple of the
 * latest values. Auto-resets after firing so it can trigger again.
 *
 * Thread-safe: messages may arrive from ProcessBatch on different threads.
 *
 * @tparam PayloadVariant The bus's std::variant payload type.
 * @tparam MsgTypes The message types to fuse (must all be in PayloadVariant).
 */
template <typename PayloadVariant, typename... MsgTypes>
class FusedSubscription {
 public:
  static constexpr size_t kNumTypes = sizeof...(MsgTypes);
  static_assert(kNumTypes >= 2,
                "FusedSubscription requires at least 2 message types");

  using FusedTuple = std::tuple<MsgTypes...>;
  using Callback = void (*)(const FusedTuple&);

  FusedSubscription() noexcept = default;
  ~FusedSubscription() = default;

  // Non-copyable, non-movable (holds subscription state)
  FusedSubscription(const FusedSubscription&) = delete;
  FusedSubscription& operator=(const FusedSubscription&) = delete;
  FusedSubscription(FusedSubscription&&) = delete;
  FusedSubscription& operator=(FusedSubscription&&) = delete;

  /**
   * @brief Set the fusion callback.
   * @param cb Function pointer called when all types have been received.
   */
  void SetCallback(Callback cb) noexcept { callback_ = cb; }

  /**
   * @brief Start listening on the bus for all MsgTypes.
   * @return DataFusionError on failure, or no error (returns true on success).
   */
  bool Activate(AsyncBus<PayloadVariant>& bus,
                DataFusionError* err = nullptr) noexcept {
    if (callback_ == nullptr) {
      if (err) *err = DataFusionError::kCallbackNotSet;
      return false;
    }
    if (active_.load(std::memory_order_acquire)) {
      if (err) *err = DataFusionError::kAlreadyActive;
      return false;
    }

    SubscribeAll(bus, std::index_sequence_for<MsgTypes...>{});

    active_.store(true, std::memory_order_release);
    return true;
  }

  /**
   * @brief Stop listening on the bus.
   */
  bool Deactivate(AsyncBus<PayloadVariant>& bus,
                  DataFusionError* err = nullptr) noexcept {
    if (!active_.load(std::memory_order_acquire)) {
      if (err) *err = DataFusionError::kNotActive;
      return false;
    }

    for (uint32_t i = 0; i < kNumTypes; ++i) {
      if (handles_[i].IsValid()) {
        bus.Unsubscribe(handles_[i]);
        handles_[i] = SubscriptionHandle::Invalid();
      }
    }

    active_.store(false, std::memory_order_release);
    return true;
  }

  /** @brief Whether the subscription is currently active. */
  bool IsActive() const noexcept {
    return active_.load(std::memory_order_acquire);
  }

  /** @brief Number of times the fusion callback has fired. */
  uint32_t FireCount() const noexcept {
    return fire_count_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Called internally when a message of type T arrives.
   * Template method invoked by the bus callback.
   */
  template <typename T>
  void OnMessage(const MessageEnvelope<PayloadVariant>& env) noexcept {
    constexpr size_t idx = detail::PackIndex<T, MsgTypes...>::value;
    static_assert(idx < kNumTypes, "Type not in MsgTypes pack");

    std::lock_guard<std::mutex> lock(mutex_);

    // Store the latest value
    std::get<idx>(data_) = std::get<T>(env.payload);
    received_[idx] = true;

    // Check if all types have been received
    TryFire();
  }

 protected:
  void TryFire() noexcept {
    for (uint32_t i = 0; i < kNumTypes; ++i) {
      if (!received_[i]) return;
    }

    // All received -- fire callback and reset
    if (callback_ != nullptr) {
      callback_(data_);
    }
    fire_count_.fetch_add(1, std::memory_order_relaxed);

    // Auto-reset for next cycle
    for (uint32_t i = 0; i < kNumTypes; ++i) {
      received_[i] = false;
    }
  }

  /** @brief Store timestamp from envelope header for the given type index. */
  void StoreTimestamp(uint32_t idx,
                      const MessageEnvelope<PayloadVariant>& env) noexcept {
    timestamps_[idx] = env.header.timestamp_us;
  }

 private:
  template <size_t... Is>
  void SubscribeAll(AsyncBus<PayloadVariant>& bus,
                    std::index_sequence<Is...>) noexcept {
    // Use fold expression to subscribe to each type
    (SubscribeOne<Is>(bus), ...);
  }

  template <size_t I>
  void SubscribeOne(AsyncBus<PayloadVariant>& bus) noexcept {
    using MsgType = std::tuple_element_t<I, FusedTuple>;
    handles_[I] = bus.template Subscribe<MsgType>(
        [this](const MessageEnvelope<PayloadVariant>& env) {
          this->template OnMessage<MsgType>(env);
        });
  }

 protected:
  Callback callback_{nullptr};
  std::atomic<bool> active_{false};
  std::atomic<uint32_t> fire_count_{0};
  std::mutex mutex_;

  FusedTuple data_{};
  std::array<bool, kNumTypes> received_{};
  std::array<uint64_t, kNumTypes> timestamps_{};
  std::array<SubscriptionHandle, kNumTypes> handles_{};

  // Initialize handles to invalid
  struct HandleInit {
    static void Init(std::array<SubscriptionHandle, kNumTypes>& h) {
      for (auto& handle : h) {
        handle = SubscriptionHandle::Invalid();
      }
    }
  };
};

// ============================================================================
// TimeSynchronizer<PayloadVariant, MsgTypes...>
// ============================================================================

/**
 * @brief Timestamp-aligned data fusion.
 *
 * Similar to FusedSubscription but only fires if all messages arrive within
 * a configurable time window (microseconds). Uses the timestamp_us from
 * MessageHeader for timing. Drops stale data if window is exceeded.
 *
 * @tparam PayloadVariant The bus's std::variant payload type.
 * @tparam MsgTypes The message types to fuse.
 */
template <typename PayloadVariant, typename... MsgTypes>
class TimeSynchronizer {
 public:
  static constexpr size_t kNumTypes = sizeof...(MsgTypes);
  static_assert(kNumTypes >= 2,
                "TimeSynchronizer requires at least 2 message types");

  using FusedTuple = std::tuple<MsgTypes...>;
  using Callback = void (*)(const FusedTuple&);

  TimeSynchronizer() noexcept {
    for (auto& h : handles_) {
      h = SubscriptionHandle::Invalid();
    }
  }

  ~TimeSynchronizer() = default;

  TimeSynchronizer(const TimeSynchronizer&) = delete;
  TimeSynchronizer& operator=(const TimeSynchronizer&) = delete;
  TimeSynchronizer(TimeSynchronizer&&) = delete;
  TimeSynchronizer& operator=(TimeSynchronizer&&) = delete;

  /** @brief Set the fusion callback. */
  void SetCallback(Callback cb) noexcept { callback_ = cb; }

  /** @brief Set the time window in microseconds. */
  void SetTimeWindow(uint64_t window_us) noexcept {
    window_us_ = window_us;
  }

  /** @brief Get the current time window. */
  uint64_t GetTimeWindow() const noexcept { return window_us_; }

  /**
   * @brief Start listening on the bus.
   */
  bool Activate(AsyncBus<PayloadVariant>& bus,
                DataFusionError* err = nullptr) noexcept {
    if (callback_ == nullptr) {
      if (err) *err = DataFusionError::kCallbackNotSet;
      return false;
    }
    if (active_.load(std::memory_order_acquire)) {
      if (err) *err = DataFusionError::kAlreadyActive;
      return false;
    }

    SubscribeAll(bus, std::index_sequence_for<MsgTypes...>{});
    active_.store(true, std::memory_order_release);
    return true;
  }

  /**
   * @brief Stop listening on the bus.
   */
  bool Deactivate(AsyncBus<PayloadVariant>& bus,
                  DataFusionError* err = nullptr) noexcept {
    if (!active_.load(std::memory_order_acquire)) {
      if (err) *err = DataFusionError::kNotActive;
      return false;
    }

    for (uint32_t i = 0; i < kNumTypes; ++i) {
      if (handles_[i].IsValid()) {
        bus.Unsubscribe(handles_[i]);
        handles_[i] = SubscriptionHandle::Invalid();
      }
    }

    active_.store(false, std::memory_order_release);
    return true;
  }

  /** @brief Whether the synchronizer is currently active. */
  bool IsActive() const noexcept {
    return active_.load(std::memory_order_acquire);
  }

  /** @brief Number of times the fusion callback has fired. */
  uint32_t FireCount() const noexcept {
    return fire_count_.load(std::memory_order_relaxed);
  }

  /** @brief Number of times data was dropped due to window timeout. */
  uint32_t TimeoutCount() const noexcept {
    return timeout_count_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Called internally when a message of type T arrives.
   */
  template <typename T>
  void OnMessage(const MessageEnvelope<PayloadVariant>& env) noexcept {
    constexpr size_t idx = detail::PackIndex<T, MsgTypes...>::value;
    static_assert(idx < kNumTypes, "Type not in MsgTypes pack");

    std::lock_guard<std::mutex> lock(mutex_);

    // Store the latest value and timestamp
    std::get<idx>(data_) = std::get<T>(env.payload);
    timestamps_[idx] = env.header.timestamp_us;
    received_[idx] = true;

    // Check if any existing data is stale relative to the new message
    DropStaleData(env.header.timestamp_us);

    // Try to fire if all types received and within window
    TryFire();
  }

 private:
  template <size_t... Is>
  void SubscribeAll(AsyncBus<PayloadVariant>& bus,
                    std::index_sequence<Is...>) noexcept {
    (SubscribeOne<Is>(bus), ...);
  }

  template <size_t I>
  void SubscribeOne(AsyncBus<PayloadVariant>& bus) noexcept {
    using MsgType = std::tuple_element_t<I, FusedTuple>;
    handles_[I] = bus.template Subscribe<MsgType>(
        [this](const MessageEnvelope<PayloadVariant>& env) {
          this->template OnMessage<MsgType>(env);
        });
  }

  /** @brief Drop any received data that is outside the time window. */
  void DropStaleData(uint64_t current_ts) noexcept {
    for (uint32_t i = 0; i < kNumTypes; ++i) {
      if (received_[i]) {
        uint64_t diff = (current_ts >= timestamps_[i])
                            ? (current_ts - timestamps_[i])
                            : (timestamps_[i] - current_ts);
        if (diff > window_us_) {
          received_[i] = false;
          timeout_count_.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
  }

  /** @brief Try to fire the callback if all types received within window. */
  void TryFire() noexcept {
    for (uint32_t i = 0; i < kNumTypes; ++i) {
      if (!received_[i]) return;
    }

    // Verify all timestamps are within the window
    uint64_t min_ts = timestamps_[0];
    uint64_t max_ts = timestamps_[0];
    for (uint32_t i = 1; i < kNumTypes; ++i) {
      if (timestamps_[i] < min_ts) min_ts = timestamps_[i];
      if (timestamps_[i] > max_ts) max_ts = timestamps_[i];
    }

    if ((max_ts - min_ts) > window_us_) {
      // Window exceeded -- mark the oldest as stale and drop
      for (uint32_t i = 0; i < kNumTypes; ++i) {
        if (timestamps_[i] == min_ts) {
          received_[i] = false;
          timeout_count_.fetch_add(1, std::memory_order_relaxed);
          break;
        }
      }
      return;
    }

    // All within window -- fire
    if (callback_ != nullptr) {
      callback_(data_);
    }
    fire_count_.fetch_add(1, std::memory_order_relaxed);

    // Auto-reset
    for (uint32_t i = 0; i < kNumTypes; ++i) {
      received_[i] = false;
    }
  }

  Callback callback_{nullptr};
  uint64_t window_us_{0};
  std::atomic<bool> active_{false};
  std::atomic<uint32_t> fire_count_{0};
  std::atomic<uint32_t> timeout_count_{0};
  std::mutex mutex_;

  FusedTuple data_{};
  std::array<bool, kNumTypes> received_{};
  std::array<uint64_t, kNumTypes> timestamps_{};
  std::array<SubscriptionHandle, kNumTypes> handles_{};
};

}  // namespace osp

#endif  // OSP_DATA_FUSION_HPP_
