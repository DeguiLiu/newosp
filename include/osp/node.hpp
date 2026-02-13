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

enum class NodeError : uint8_t {
  kAlreadyStarted = 0,
  kNotStarted,
  kSubscriptionFull,
  kPublishFailed
};

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

  Publisher(AsyncBus<PayloadVariant>& bus, uint32_t sender_id) noexcept
      : bus_(&bus), sender_id_(sender_id) {}

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
    return bus_->PublishWithPriority(
        PayloadVariant(std::move(msg)), sender_id_, priority);
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
 * The bus is a global singleton per PayloadVariant type. All nodes sharing the
 * same PayloadVariant communicate through the same bus. Message routing is
 * type-based (not topic-based): all subscribers of type T receive all messages
 * of type T, regardless of which node published them.
 *
 * @tparam PayloadVariant A std::variant<...> of user-defined message types.
 */
template <typename PayloadVariant>
class Node {
 public:
  using BusType = AsyncBus<PayloadVariant>;
  using EnvelopeType = MessageEnvelope<PayloadVariant>;

  /**
   * @brief Construct a named node.
   * @param name Node name (truncated to kNodeNameMaxLen chars).
   * @param id Unique sender ID for message tracing.
   */
  explicit Node(const char* name, uint32_t id = 0) noexcept
      : id_(id), handle_count_(0), started_(false) {
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
    if (!started_ && handle_count_ == 0) return;

    for (uint32_t i = 0; i < handle_count_; ++i) {
      BusType::Instance().Unsubscribe(handles_[i]);
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
    return BusType::Instance().Publish(
        PayloadVariant(std::forward<T>(msg)), id_);
  }

  /**
   * @brief Publish with explicit priority.
   */
  template <typename T>
  bool PublishWithPriority(T&& msg, MessagePriority priority) noexcept {
    return BusType::Instance().PublishWithPriority(
        PayloadVariant(std::forward<T>(msg)), id_, priority);
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
        BusType::Instance().template Subscribe<T>(
            [cb = std::forward<Func>(callback)](
                const EnvelopeType& env) noexcept {
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
        BusType::Instance().template Subscribe<T>(
            [cb = std::forward<Func>(callback)](
                const EnvelopeType& env) noexcept {
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
  uint32_t SpinOnce() noexcept {
    return BusType::Instance().ProcessBatch();
  }

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
    return Publisher<T, PayloadVariant>(BusType::Instance(), id_);
  }

 private:
  char name_[kNodeNameMaxLen + 1];
  uint32_t id_;
  SubscriptionHandle handles_[OSP_MAX_NODE_SUBSCRIPTIONS];
  uint32_t handle_count_;
  bool started_;
};

}  // namespace osp

#endif  // OSP_NODE_HPP_
