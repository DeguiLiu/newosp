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
 * @file static_node.hpp
 * @brief Compile-time handler binding node for zero-overhead message dispatch.
 *
 * StaticNode is a template-parameterized alternative to Node that eliminates
 * indirect call overhead by binding the handler type at compile time. The
 * compiler can generate a direct jump table and inline the handler calls.
 *
 * Handler protocol: The Handler type must provide operator() overloads for
 * each message type T it wants to handle:
 *   void operator()(const T&, const MessageHeader&)
 *
 * Usage:
 *   struct MyHandler {
 *     void operator()(const SensorData& d, const MessageHeader& h) { ... }
 *     void operator()(const MotorCmd& c, const MessageHeader& h) { ... }
 *   };
 *
 *   // Compile-time bound, zero overhead dispatch
 *   osp::StaticNode<Payload, MyHandler> node("sensor", 1, MyHandler{});
 *
 *   // With injected bus
 *   osp::StaticNode<Payload, MyHandler> node("sensor", 1, MyHandler{}, bus);
 */

#ifndef OSP_STATIC_NODE_HPP_
#define OSP_STATIC_NODE_HPP_

#include "osp/bus.hpp"
#include "osp/node.hpp"
#include "osp/vocabulary.hpp"

#include <cstdint>
#include <cstring>
#include <utility>

namespace osp {

// ============================================================================
// StaticNode Configuration Constants
// ============================================================================

#ifndef OSP_MAX_STATIC_NODE_SUBSCRIPTIONS
#define OSP_MAX_STATIC_NODE_SUBSCRIPTIONS 16U
#endif

// ============================================================================
// StaticNode<PayloadVariant, Handler>
// ============================================================================

/**
 * @brief Compile-time handler binding node for zero-overhead message dispatch.
 *
 * Unlike Node which stores callbacks via FixedFunction (SBO type erasure),
 * StaticNode takes the Handler as a template parameter. Each per-type
 * subscription callback captures a pointer to the Handler and calls
 * handler(data, header) directly. Since the Handler type is known at compile
 * time, the compiler can inline the call and generate a direct jump table.
 *
 * The bus integration is identical to Node: one subscription per variant
 * alternative type, with optional topic filtering. Lifecycle (Start/Stop)
 * and RAII unsubscription on destruction are fully supported.
 *
 * @tparam PayloadVariant A std::variant<...> of user-defined message types.
 * @tparam Handler A callable type with operator()(const T&, const MessageHeader&)
 *         for each type T in PayloadVariant that should be handled.
 */
template <typename PayloadVariant, typename Handler>
class StaticNode {
 public:
  using BusType = AsyncBus<PayloadVariant>;
  using EnvelopeType = MessageEnvelope<PayloadVariant>;

  /**
   * @brief Construct with global singleton bus (no topic filter).
   * @param name Node name (truncated to kNodeNameMaxLen chars).
   * @param id Unique sender ID for message tracing.
   * @param handler Handler instance (stored by value).
   */
  StaticNode(const char* name, uint32_t id, Handler handler) noexcept
      : bus_ptr_(&BusType::Instance()),
        handler_(static_cast<Handler&&>(handler)),
        id_(id),
        topic_hash_(0),
        handle_count_(0),
        started_(false) {
    InitName(name);
  }

  /**
   * @brief Construct with injected bus (no topic filter).
   * @param name Node name (truncated to kNodeNameMaxLen chars).
   * @param id Unique sender ID for message tracing.
   * @param handler Handler instance (stored by value).
   * @param bus Reference to the bus instance to use.
   */
  StaticNode(const char* name, uint32_t id, Handler handler,
             BusType& bus) noexcept
      : bus_ptr_(&bus),
        handler_(static_cast<Handler&&>(handler)),
        id_(id),
        topic_hash_(0),
        handle_count_(0),
        started_(false) {
    InitName(name);
  }

  /**
   * @brief Construct with global singleton bus and topic filter.
   * @param name Node name (truncated to kNodeNameMaxLen chars).
   * @param id Unique sender ID for message tracing.
   * @param handler Handler instance (stored by value).
   * @param topic Topic string for filtering (null-terminated).
   */
  StaticNode(const char* name, uint32_t id, Handler handler,
             const char* topic) noexcept
      : bus_ptr_(&BusType::Instance()),
        handler_(static_cast<Handler&&>(handler)),
        id_(id),
        topic_hash_(Fnv1a32(topic)),
        handle_count_(0),
        started_(false) {
    InitName(name);
  }

  /**
   * @brief Construct with injected bus and topic filter.
   * @param name Node name (truncated to kNodeNameMaxLen chars).
   * @param id Unique sender ID for message tracing.
   * @param handler Handler instance (stored by value).
   * @param topic Topic string for filtering (null-terminated).
   * @param bus Reference to the bus instance to use.
   */
  StaticNode(const char* name, uint32_t id, Handler handler,
             const char* topic, BusType& bus) noexcept
      : bus_ptr_(&bus),
        handler_(static_cast<Handler&&>(handler)),
        id_(id),
        topic_hash_(Fnv1a32(topic)),
        handle_count_(0),
        started_(false) {
    InitName(name);
  }

  /** @brief Destructor - unsubscribes all registered callbacks. */
  ~StaticNode() noexcept { Stop(); }

  StaticNode(const StaticNode&) = delete;
  StaticNode& operator=(const StaticNode&) = delete;
  StaticNode(StaticNode&&) = delete;
  StaticNode& operator=(StaticNode&&) = delete;

  // ======================== Accessors ========================

  /** @brief Get the node name. */
  const char* Name() const noexcept { return name_; }

  /** @brief Get the node sender ID. */
  uint32_t Id() const noexcept { return id_; }

  /** @brief Check if the node has been started. */
  bool IsStarted() const noexcept { return started_; }

  /** @brief Get the number of active subscriptions. */
  uint32_t SubscriptionCount() const noexcept { return handle_count_; }

  /** @brief Get a mutable reference to the handler. */
  Handler& GetHandler() noexcept { return handler_; }

  /** @brief Get a const reference to the handler. */
  const Handler& GetHandler() const noexcept { return handler_; }

  // ======================== Lifecycle ========================

  /**
   * @brief Start the node and subscribe to all variant types.
   *
   * Iterates over every alternative type in PayloadVariant and registers
   * a subscription callback that dispatches to the Handler. This is done
   * at compile time via recursive template instantiation.
   *
   * @return Success or NodeError.
   */
  expected<void, NodeError> Start() noexcept {
    if (started_) {
      return expected<void, NodeError>::error(NodeError::kAlreadyStarted);
    }

    // Subscribe for each type in the variant
    bool ok = SubscribeAll(
        std::make_index_sequence<std::variant_size_v<PayloadVariant>>{});
    if (!ok) {
      // Rollback any partial subscriptions
      UnsubscribeAll();
      return expected<void, NodeError>::error(NodeError::kSubscriptionFull);
    }

    started_ = true;
    return expected<void, NodeError>::success();
  }

  /**
   * @brief Stop the node and unsubscribe all callbacks.
   */
  void Stop() noexcept {
    if (!started_ && handle_count_ == 0) return;

    UnsubscribeAll();
    started_ = false;
  }

  // ======================== Publish ========================

  /**
   * @brief Publish a typed message through the bus.
   * @tparam T The message type (must be one of the PayloadVariant alternatives).
   * @param msg The message to publish.
   * @return true if published successfully, false if dropped.
   */
  template <typename T>
  bool Publish(T&& msg) noexcept {
    return bus_ptr_->Publish(
        PayloadVariant(std::forward<T>(msg)), id_);
  }

  /**
   * @brief Publish with explicit priority.
   */
  template <typename T>
  bool PublishWithPriority(T&& msg, MessagePriority priority) noexcept {
    return bus_ptr_->PublishWithPriority(
        PayloadVariant(std::forward<T>(msg)), id_, priority);
  }

  /**
   * @brief Publish a typed message with topic.
   * @tparam T The message type (must be one of the PayloadVariant alternatives).
   * @param msg The message to publish.
   * @param topic Topic string (null-terminated).
   * @return true if published successfully, false if dropped.
   */
  template <typename T>
  bool Publish(T&& msg, const char* topic) noexcept {
    return bus_ptr_->PublishTopic(
        PayloadVariant(std::forward<T>(msg)), id_, topic);
  }

  // ======================== Processing ========================

  /**
   * @brief Process pending messages from the bus.
   * @return Number of messages processed.
   */
  uint32_t SpinOnce() noexcept {
    return bus_ptr_->ProcessBatch();
  }

  // ======================== Publisher Factory ========================

  /**
   * @brief Create a typed publisher bound to this node's sender ID.
   * @tparam T The message type to publish.
   * @return A Publisher<T> instance.
   */
  template <typename T>
  Publisher<T, PayloadVariant> CreatePublisher() noexcept {
    return Publisher<T, PayloadVariant>(*bus_ptr_, id_);
  }

 private:
  // ======================== Subscribe Helpers ========================

  /**
   * @brief Subscribe for all variant alternative types via index sequence.
   * @return true if all subscriptions succeeded, false otherwise.
   */
  template <size_t... Is>
  bool SubscribeAll(std::index_sequence<Is...> /*seq*/) noexcept {
    // Fold expression: subscribe for each type index, short-circuit on failure
    return (SubscribeOne<Is>() && ...);
  }

  /**
   * @brief Subscribe for a single variant alternative at compile-time index I.
   *
   * The lambda captures a pointer to the Handler (known at compile time).
   * When invoked, it extracts the typed data via std::get_if and calls
   * handler(data, header) directly. Since Handler is a template parameter,
   * the compiler can inline the call entirely.
   *
   * @tparam I The variant alternative index.
   * @return true if subscription succeeded.
   */
  template <size_t I>
  bool SubscribeOne() noexcept {
    using T = std::variant_alternative_t<I, PayloadVariant>;

    if (handle_count_ >= OSP_MAX_STATIC_NODE_SUBSCRIPTIONS) {
      return false;
    }

    // Capture handler pointer and topic hash by value.
    // Handler* is stable because StaticNode is non-movable.
    Handler* handler_ptr = &handler_;
    uint32_t topic_hash = topic_hash_;

    SubscriptionHandle handle =
        bus_ptr_->template Subscribe<T>(
            [handler_ptr, topic_hash](const EnvelopeType& env) noexcept {
              // Topic filter: skip if topic_hash is set and does not match
              if (topic_hash != 0 && env.header.topic_hash != topic_hash) {
                return;
              }
              const T* data = std::get_if<T>(&env.payload);
              if (OSP_LIKELY(data != nullptr)) {
                (*handler_ptr)(*data, env.header);
              }
            });

    if (!handle.IsValid()) {
      return false;
    }

    handles_[handle_count_++] = handle;
    return true;
  }

  /**
   * @brief Unsubscribe all active subscriptions.
   */
  void UnsubscribeAll() noexcept {
    for (uint32_t i = 0; i < handle_count_; ++i) {
      bus_ptr_->Unsubscribe(handles_[i]);
    }
    handle_count_ = 0;
  }

  // ======================== Name Helper ========================

  void InitName(const char* name) noexcept {
    if (name != nullptr) {
      uint32_t i = 0;
      for (; i < kNodeNameMaxLen && name[i] != '\0'; ++i) {
        name_[i] = name[i];
      }
      name_[i] = '\0';
    } else {
      name_[0] = '\0';
    }
  }

  // ======================== Data Members ========================

  BusType* bus_ptr_;
  Handler handler_;
  char name_[kNodeNameMaxLen + 1];
  uint32_t id_;
  uint32_t topic_hash_;
  SubscriptionHandle handles_[OSP_MAX_STATIC_NODE_SUBSCRIPTIONS];
  uint32_t handle_count_;
  bool started_;
};

}  // namespace osp

#endif  // OSP_STATIC_NODE_HPP_
