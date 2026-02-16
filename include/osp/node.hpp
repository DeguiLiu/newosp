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
 * @file node.hpp
 * @brief Lightweight node abstraction for component communication.
 *
 * Inspired by ROS2 Node and CyberRT Component patterns.
 * Provides named node identity, typed publish/subscribe, RAII subscription
 * cleanup, and simple event-loop integration.
 *
 * Usage:
 *   struct SensorData { float temp; };
 *   struct MotorCmd   { int speed; };
 *   using MyPayload = std::variant<SensorData, MotorCmd>;
 *
 *   osp::Node<MyPayload> sensor("sensor_node", 1);
 *   sensor.Subscribe<SensorData>([](const SensorData& d, const auto& h) {
 *       printf("temp=%f from sender %u\n", d.temp, h.sender_id);
 *   });
 *   sensor.Publish(SensorData{25.0f});
 *   sensor.SpinOnce();
 */

#ifndef OSP_NODE_HPP_
#define OSP_NODE_HPP_

#include "osp/bus.hpp"
#include "osp/vocabulary.hpp"

#include <cstdint>
#include <cstring>

namespace osp {

// ============================================================================
// Node Error
// ============================================================================

enum class NodeError : uint8_t { kAlreadyStarted = 0, kNotStarted, kSubscriptionFull, kPublishFailed };

// ============================================================================
// Node Configuration Constants
// ============================================================================

#ifndef OSP_MAX_NODE_SUBSCRIPTIONS
#define OSP_MAX_NODE_SUBSCRIPTIONS 16U
#endif

static constexpr uint32_t kNodeNameMaxLen = 31;

// ============================================================================
// Publisher<T, PayloadVariant>
// ============================================================================

/**
 * @brief Typed message publisher bound to a specific node's sender ID.
 *
 * Lightweight wrapper around AsyncBus::Publish. Does not own any resource
 * (no RAII cleanup needed). Lifetime must not exceed the bus singleton.
 *
 * @tparam T The concrete message type to publish.
 * @tparam PayloadVariant The variant type used by the bus.
 */
template <typename T, typename PayloadVariant>
class Publisher {
 public:
  Publisher() noexcept : bus_(nullptr), sender_id_(0) {}

  Publisher(AsyncBus<PayloadVariant>& bus, uint32_t sender_id) noexcept : bus_(&bus), sender_id_(sender_id) {}

  /** @brief Publish a message with default (MEDIUM) priority. */
  bool Publish(const T& msg) noexcept {
    OSP_ASSERT(bus_ != nullptr);
    return bus_->Publish(PayloadVariant(msg), sender_id_);
  }

  /** @brief Publish an rvalue message with default (MEDIUM) priority. */
  bool Publish(T&& msg) noexcept {
    OSP_ASSERT(bus_ != nullptr);
    return bus_->Publish(PayloadVariant(std::move(msg)), sender_id_);
  }

  /** @brief Publish with explicit priority. */
  bool PublishWithPriority(const T& msg, MessagePriority priority) noexcept {
    OSP_ASSERT(bus_ != nullptr);
    return bus_->PublishWithPriority(PayloadVariant(msg), sender_id_, priority);
  }

  /** @brief Publish rvalue with explicit priority. */
  bool PublishWithPriority(T&& msg, MessagePriority priority) noexcept {
    OSP_ASSERT(bus_ != nullptr);
    return bus_->PublishWithPriority(PayloadVariant(std::move(msg)), sender_id_, priority);
  }

  /** @brief Check if this publisher is bound to a bus. */
  bool IsValid() const noexcept { return bus_ != nullptr; }

 private:
  AsyncBus<PayloadVariant>* bus_;
  uint32_t sender_id_;
};

// ============================================================================
// Node<PayloadVariant>
// ============================================================================

/**
 * @brief Lightweight communication node with typed pub/sub.
 *
 * Each node has a unique name and sender ID. Subscriptions are automatically
 * cleaned up when the node is destroyed (RAII).
 *
 * The bus can be injected via constructor, or defaults to the global singleton
 * per PayloadVariant type. All nodes sharing the same bus communicate through
 * it. Message routing is type-based by default, or topic-based when topics are
 * specified.
 *
 * @tparam PayloadVariant A std::variant<...> of user-defined message types.
 */
template <typename PayloadVariant>
class Node {
 public:
  using BusType = AsyncBus<PayloadVariant>;
  using EnvelopeType = MessageEnvelope<PayloadVariant>;

  /**
   * @brief Construct a named node with global singleton bus.
   * @param name Node name (truncated to kNodeNameMaxLen chars).
   * @param id Unique sender ID for message tracing.
   */
  explicit Node(const char* name, uint32_t id = 0) noexcept
      : bus_ptr_(&BusType::Instance()), id_(id), handle_count_(0), started_(false) {
    InitName(name);
  }

  /**
   * @brief Construct a named node with injected bus.
   * @param name Node name (truncated to kNodeNameMaxLen chars).
   * @param id Unique sender ID for message tracing.
   * @param bus Reference to the bus instance to use.
   */
  Node(const char* name, uint32_t id, BusType& bus) noexcept
      : bus_ptr_(&bus), id_(id), handle_count_(0), started_(false) {
    InitName(name);
  }

  /** @brief Destructor - unsubscribes all registered callbacks. */
  ~Node() noexcept { Stop(); }

  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;
  Node(Node&&) = delete;
  Node& operator=(Node&&) = delete;

  // ======================== Accessors ========================

  /** @brief Get the node name. */
  const char* Name() const noexcept { return name_; }

  /** @brief Get the node sender ID. */
  uint32_t Id() const noexcept { return id_; }

  /** @brief Check if the node has been started. */
  bool IsStarted() const noexcept { return started_; }

  /** @brief Get the number of active subscriptions. */
  uint32_t SubscriptionCount() const noexcept { return handle_count_; }

  // ======================== Lifecycle ========================

  /**
   * @brief Start the node.
   * @return Success or kAlreadyStarted error.
   */
  expected<void, NodeError> Start() noexcept {
    if (started_) {
      return expected<void, NodeError>::error(NodeError::kAlreadyStarted);
    }
    started_ = true;
    return expected<void, NodeError>::success();
  }

  /**
   * @brief Stop the node and unsubscribe all callbacks.
   */
  void Stop() noexcept {
    if (!started_ && handle_count_ == 0)
      return;

    for (uint32_t i = 0; i < handle_count_; ++i) {
      bus_ptr_->Unsubscribe(handles_[i]);
    }
    handle_count_ = 0;
    started_ = false;
  }

  // ======================== Publish ========================

  /**
   * @brief Publish a typed message through the bus.
   *
   * The message is published with the node's sender ID and default
   * (MEDIUM) priority.
   *
   * @tparam T The message type (must be one of the PayloadVariant alternatives).
   * @param msg The message to publish.
   * @return true if published successfully, false if dropped.
   */
  template <typename T>
  bool Publish(T&& msg) noexcept {
    return bus_ptr_->Publish(PayloadVariant(std::forward<T>(msg)), id_);
  }

  /**
   * @brief Publish with explicit priority.
   */
  template <typename T>
  bool PublishWithPriority(T&& msg, MessagePriority priority) noexcept {
    return bus_ptr_->PublishWithPriority(PayloadVariant(std::forward<T>(msg)), id_, priority);
  }

  /**
   * @brief Publish a typed message with topic.
   *
   * The message is published with the node's sender ID, topic, and default
   * (MEDIUM) priority. Only subscribers that subscribed to the same topic
   * will receive this message.
   *
   * @tparam T The message type (must be one of the PayloadVariant alternatives).
   * @param msg The message to publish.
   * @param topic Topic string (null-terminated).
   * @return true if published successfully, false if dropped.
   */
  template <typename T>
  bool Publish(T&& msg, const char* topic) noexcept {
    return bus_ptr_->PublishTopic(PayloadVariant(std::forward<T>(msg)), id_, topic);
  }

  /**
   * @brief Publish with topic and explicit priority.
   */
  template <typename T>
  bool PublishWithPriority(T&& msg, const char* topic, MessagePriority priority) noexcept {
    return bus_ptr_->PublishTopicWithPriority(PayloadVariant(std::forward<T>(msg)), id_, topic, priority);
  }

  // ======================== Subscribe ========================

  /**
   * @brief Subscribe to messages of type T.
   *
   * The callback receives the typed message data and the message header.
   * Signature: void(const T& data, const MessageHeader& header)
   *
   * The subscription is tracked by this node and will be automatically
   * unsubscribed when the node is destroyed or Stop() is called.
   *
   * @tparam T The message type to subscribe to.
   * @tparam Func Callable with signature void(const T&, const MessageHeader&).
   * @return Success or kSubscriptionFull error.
   */
  template <typename T, typename Func>
  expected<void, NodeError> Subscribe(Func&& callback) noexcept {
    if (handle_count_ >= OSP_MAX_NODE_SUBSCRIPTIONS) {
      return expected<void, NodeError>::error(NodeError::kSubscriptionFull);
    }

    SubscriptionHandle handle =
        bus_ptr_->template Subscribe<T>([cb = std::forward<Func>(callback)](const EnvelopeType& env) noexcept {
          const T* data = std::get_if<T>(&env.payload);
          if (data != nullptr) {
            cb(*data, env.header);
          }
        });

    if (!handle.IsValid()) {
      return expected<void, NodeError>::error(NodeError::kSubscriptionFull);
    }

    handles_[handle_count_++] = handle;
    return expected<void, NodeError>::success();
  }

  /**
   * @brief Subscribe to messages of type T with topic filter.
   *
   * The callback receives the typed message data and the message header.
   * Only messages published with the matching topic will be delivered.
   *
   * @tparam T The message type to subscribe to.
   * @tparam Func Callable with signature void(const T&, const MessageHeader&).
   * @param topic Topic string (null-terminated).
   * @return Success or kSubscriptionFull error.
   */
  template <typename T, typename Func>
  expected<void, NodeError> Subscribe(const char* topic, Func&& callback) noexcept {
    if (handle_count_ >= OSP_MAX_NODE_SUBSCRIPTIONS) {
      return expected<void, NodeError>::error(NodeError::kSubscriptionFull);
    }

    uint32_t topic_hash = Fnv1a32(topic);

    SubscriptionHandle handle = bus_ptr_->template Subscribe<T>(
        [cb = std::forward<Func>(callback), topic_hash](const EnvelopeType& env) noexcept {
          // Topic filter: only deliver if topic_hash matches
          if (env.header.topic_hash != topic_hash) {
            return;
          }
          const T* data = std::get_if<T>(&env.payload);
          if (data != nullptr) {
            cb(*data, env.header);
          }
        });

    if (!handle.IsValid()) {
      return expected<void, NodeError>::error(NodeError::kSubscriptionFull);
    }

    handles_[handle_count_++] = handle;
    return expected<void, NodeError>::success();
  }

  /**
   * @brief Subscribe with a simple callback (data only, no header).
   *
   * Signature: void(const T& data)
   */
  template <typename T, typename Func>
  expected<void, NodeError> SubscribeSimple(Func&& callback) noexcept {
    if (handle_count_ >= OSP_MAX_NODE_SUBSCRIPTIONS) {
      return expected<void, NodeError>::error(NodeError::kSubscriptionFull);
    }

    SubscriptionHandle handle =
        bus_ptr_->template Subscribe<T>([cb = std::forward<Func>(callback)](const EnvelopeType& env) noexcept {
          const T* data = std::get_if<T>(&env.payload);
          if (data != nullptr) {
            cb(*data);
          }
        });

    if (!handle.IsValid()) {
      return expected<void, NodeError>::error(NodeError::kSubscriptionFull);
    }

    handles_[handle_count_++] = handle;
    return expected<void, NodeError>::success();
  }

  // ======================== Processing ========================

  /**
   * @brief Process pending messages from the bus.
   *
   * Drains the bus queue by calling ProcessBatch(). This dispatches
   * messages to all registered callbacks (across all nodes using the
   * same bus).
   *
   * NOTE: Only ONE thread should call SpinOnce/Spin at a time (MPSC
   * consumer constraint). Typically a single "main loop" thread.
   *
   * @return Number of messages processed.
   */
  uint32_t SpinOnce() noexcept { return bus_ptr_->ProcessBatch(); }

  // ======================== Publisher Factory ========================

  /**
   * @brief Create a typed publisher bound to this node's sender ID.
   *
   * The returned publisher is lightweight and does not own any resource.
   * Its lifetime should not exceed the node's lifetime.
   *
   * @tparam T The message type to publish.
   * @return A Publisher<T> instance.
   */
  template <typename T>
  Publisher<T, PayloadVariant> CreatePublisher() noexcept {
    return Publisher<T, PayloadVariant>(*bus_ptr_, id_);
  }

 private:
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

  BusType* bus_ptr_;
  char name_[kNodeNameMaxLen + 1];
  uint32_t id_;
  SubscriptionHandle handles_[OSP_MAX_NODE_SUBSCRIPTIONS];
  uint32_t handle_count_;
  bool started_;
};

}  // namespace osp

#endif  // OSP_NODE_HPP_
