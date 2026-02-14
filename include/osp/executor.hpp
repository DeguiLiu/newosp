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
 * @file executor.hpp
 * @brief Executor scheduling system for node lifecycle and message dispatch.
 *
 * Inspired by ROS2 executor patterns. Provides single-thread, static, and
 * CPU-pinned executor variants that manage Node<PayloadVariant> instances
 * and drive the AsyncBus message processing loop.
 *
 * Since AsyncBus is a singleton MPSC bus with a single consumer, only ONE
 * executor should be actively spinning (calling ProcessBatch) at a time for
 * a given PayloadVariant type.
 *
 * Usage:
 *   using MyPayload = std::variant<SensorData, MotorCmd>;
 *
 *   osp::Node<MyPayload> sensor("sensor", 1);
 *   osp::Node<MyPayload> motor("motor", 2);
 *
 *   osp::SingleThreadExecutor<MyPayload> exec;
 *   exec.AddNode(sensor);
 *   exec.AddNode(motor);
 *   exec.Spin();  // blocking until Stop()
 */

#ifndef OSP_EXECUTOR_HPP_
#define OSP_EXECUTOR_HPP_

#include "osp/platform.hpp"
#include "osp/node.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

#if defined(OSP_PLATFORM_LINUX)
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#endif

namespace osp {

// ============================================================================
// Configuration
// ============================================================================

#ifndef OSP_EXECUTOR_MAX_NODES
#define OSP_EXECUTOR_MAX_NODES 16U
#endif

// ============================================================================
// SingleThreadExecutor<PayloadVariant>
// ============================================================================

/**
 * @brief Single-threaded executor that polls all registered nodes in
 *        round-robin by driving the shared AsyncBus ProcessBatch loop.
 *
 * Spin() blocks on the calling thread. SpinOnce() performs a single batch
 * iteration and returns immediately.
 *
 * @tparam PayloadVariant A std::variant<...> of user-defined message types.
 */
template <typename PayloadVariant>
class SingleThreadExecutor {
 public:
  using BusType = AsyncBus<PayloadVariant>;

  SingleThreadExecutor() noexcept : node_count_(0), running_(false) {
    for (uint32_t i = 0; i < OSP_EXECUTOR_MAX_NODES; ++i) {
      nodes_[i] = nullptr;
    }
  }

  ~SingleThreadExecutor() noexcept { Stop(); }

  SingleThreadExecutor(const SingleThreadExecutor&) = delete;
  SingleThreadExecutor& operator=(const SingleThreadExecutor&) = delete;
  SingleThreadExecutor(SingleThreadExecutor&&) = delete;
  SingleThreadExecutor& operator=(SingleThreadExecutor&&) = delete;

  // ======================== Node Management ========================

  /**
   * @brief Add a node to this executor.
   *
   * Must be called before Spin(). The node's lifetime must exceed the
   * executor's active spinning period.
   *
   * @param node Reference to the node to manage.
   * @return true if added, false if the node array is full.
   */
  bool AddNode(Node<PayloadVariant>& node) noexcept {
    if (node_count_ >= OSP_EXECUTOR_MAX_NODES) {
      return false;
    }
    // Check for duplicates
    for (uint32_t i = 0; i < node_count_; ++i) {
      if (nodes_[i] == &node) {
        return false;
      }
    }
    nodes_[node_count_++] = &node;
    return true;
  }

  /**
   * @brief Remove a node from this executor.
   *
   * @param node Reference to the node to remove.
   * @return true if found and removed, false otherwise.
   */
  bool RemoveNode(const Node<PayloadVariant>& node) noexcept {
    for (uint32_t i = 0; i < node_count_; ++i) {
      if (nodes_[i] == &node) {
        // Shift remaining elements
        for (uint32_t j = i; j + 1 < node_count_; ++j) {
          nodes_[j] = nodes_[j + 1];
        }
        nodes_[--node_count_] = nullptr;
        return true;
      }
    }
    return false;
  }

  // ======================== Execution ========================

  /**
   * @brief Blocking spin loop - processes all nodes until Stop() is called.
   *
   * Drives the bus ProcessBatch in a tight loop with yield on idle.
   * Only one thread should call Spin() at a time.
   */
  void Spin() noexcept {
    running_.store(true, std::memory_order_release);
    while (running_.load(std::memory_order_relaxed)) {
      uint32_t processed = SpinOnce();
      if (processed == 0) {
        std::this_thread::yield();
      }
    }
  }

  /**
   * @brief Single iteration - process one batch of pending messages.
   *
   * @return Number of messages processed.
   */
  uint32_t SpinOnce() noexcept {
    return BusType::Instance().ProcessBatch();
  }

  /**
   * @brief Stop the executor.
   *
   * Sets the running flag to false. If Spin() is active on another thread,
   * it will exit after the current iteration completes.
   */
  void Stop() noexcept {
    running_.store(false, std::memory_order_release);
  }

  // ======================== Accessors ========================

  /** @brief Check if the executor is currently spinning. */
  bool IsRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

  /** @brief Get the number of registered nodes. */
  uint32_t NodeCount() const noexcept { return node_count_; }

 private:
  Node<PayloadVariant>* nodes_[OSP_EXECUTOR_MAX_NODES];
  uint32_t node_count_;
  std::atomic<bool> running_;
};

// ============================================================================
// StaticExecutor<PayloadVariant>
// ============================================================================

/**
 * @brief Static executor with a dedicated dispatcher thread.
 *
 * Nodes are pre-registered, and a single internal dispatcher thread drives
 * the AsyncBus ProcessBatch loop. Callbacks registered by all managed nodes
 * are invoked from the dispatcher thread context.
 *
 * This is suitable for scenarios where message processing should run on a
 * background thread separate from the main application thread.
 *
 * @tparam PayloadVariant A std::variant<...> of user-defined message types.
 */
template <typename PayloadVariant>
class StaticExecutor {
 public:
  using BusType = AsyncBus<PayloadVariant>;

  StaticExecutor() noexcept : node_count_(0), running_(false) {
    for (uint32_t i = 0; i < OSP_EXECUTOR_MAX_NODES; ++i) {
      nodes_[i] = nullptr;
    }
  }

  ~StaticExecutor() noexcept { Stop(); }

  StaticExecutor(const StaticExecutor&) = delete;
  StaticExecutor& operator=(const StaticExecutor&) = delete;
  StaticExecutor(StaticExecutor&&) = delete;
  StaticExecutor& operator=(StaticExecutor&&) = delete;

  // ======================== Node Management ========================

  /**
   * @brief Add a node to this executor.
   *
   * Must be called before Start(). The node's lifetime must exceed the
   * executor's active period.
   *
   * @param node Reference to the node to manage.
   * @return true if added, false if the node array is full.
   */
  bool AddNode(Node<PayloadVariant>& node) noexcept {
    if (node_count_ >= OSP_EXECUTOR_MAX_NODES) {
      return false;
    }
    for (uint32_t i = 0; i < node_count_; ++i) {
      if (nodes_[i] == &node) {
        return false;
      }
    }
    nodes_[node_count_++] = &node;
    return true;
  }

  /**
   * @brief Remove a node from this executor.
   *
   * Should not be called while the executor is running.
   *
   * @param node Reference to the node to remove.
   * @return true if found and removed, false otherwise.
   */
  bool RemoveNode(const Node<PayloadVariant>& node) noexcept {
    for (uint32_t i = 0; i < node_count_; ++i) {
      if (nodes_[i] == &node) {
        for (uint32_t j = i; j + 1 < node_count_; ++j) {
          nodes_[j] = nodes_[j + 1];
        }
        nodes_[--node_count_] = nullptr;
        return true;
      }
    }
    return false;
  }

  // ======================== Lifecycle ========================

  /**
   * @brief Start the dispatcher thread.
   *
   * The dispatcher thread will continuously call ProcessBatch() on the
   * shared AsyncBus, yielding when no messages are available.
   */
  void Start() noexcept {
    if (running_.load(std::memory_order_acquire)) {
      return;  // Already running
    }
    running_.store(true, std::memory_order_release);
    dispatch_thread_ = std::thread([this]() { DispatchLoop(); });
  }

  /**
   * @brief Stop the dispatcher thread and wait for it to finish.
   */
  void Stop() noexcept {
    running_.store(false, std::memory_order_release);
    if (dispatch_thread_.joinable()) {
      dispatch_thread_.join();
    }
  }

  // ======================== Accessors ========================

  /** @brief Check if the dispatcher thread is running. */
  bool IsRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

  /** @brief Get the number of registered nodes. */
  uint32_t NodeCount() const noexcept { return node_count_; }

  /** @brief Set heartbeat for external watchdog monitoring. */
  void SetHeartbeat(ThreadHeartbeat* hb) noexcept { heartbeat_ = hb; }

 private:
  /**
   * @brief Main dispatch loop for the background thread.
   *
   * Continuously processes pending messages. Yields when the bus is empty
   * to avoid busy-spinning.
   */
  void DispatchLoop() noexcept {
    while (running_.load(std::memory_order_relaxed)) {
      if (heartbeat_ != nullptr) { heartbeat_->Beat(); }
      uint32_t processed = BusType::Instance().ProcessBatch();
      if (processed == 0) {
        std::this_thread::yield();
      }
    }
  }

  Node<PayloadVariant>* nodes_[OSP_EXECUTOR_MAX_NODES];
  uint32_t node_count_;
  std::atomic<bool> running_;
  std::thread dispatch_thread_;
  ThreadHeartbeat* heartbeat_{nullptr};
};

// ============================================================================
// PinnedExecutor<PayloadVariant>
// ============================================================================

/**
 * @brief CPU-pinned executor with affinity-bound dispatcher thread.
 *
 * Like StaticExecutor, but pins the dispatcher thread to a specific CPU core
 * for deterministic scheduling and reduced cache migration. On platforms
 * without portable CPU affinity APIs (e.g. macOS), the pinning is skipped
 * with a log warning.
 *
 * @tparam PayloadVariant A std::variant<...> of user-defined message types.
 */
template <typename PayloadVariant>
class PinnedExecutor {
 public:
  using BusType = AsyncBus<PayloadVariant>;

  /**
   * @brief Construct a pinned executor bound to the specified CPU core.
   * @param cpu_core The CPU core index to pin the dispatcher thread to.
   */
  explicit PinnedExecutor(int32_t cpu_core) noexcept
      : node_count_(0), running_(false), cpu_core_(cpu_core) {
    for (uint32_t i = 0; i < OSP_EXECUTOR_MAX_NODES; ++i) {
      nodes_[i] = nullptr;
    }
  }

  ~PinnedExecutor() noexcept { Stop(); }

  PinnedExecutor(const PinnedExecutor&) = delete;
  PinnedExecutor& operator=(const PinnedExecutor&) = delete;
  PinnedExecutor(PinnedExecutor&&) = delete;
  PinnedExecutor& operator=(PinnedExecutor&&) = delete;

  // ======================== Node Management ========================

  /**
   * @brief Add a node to this executor.
   *
   * Must be called before Start(). The node's lifetime must exceed the
   * executor's active period.
   *
   * @param node Reference to the node to manage.
   * @return true if added, false if the node array is full.
   */
  bool AddNode(Node<PayloadVariant>& node) noexcept {
    if (node_count_ >= OSP_EXECUTOR_MAX_NODES) {
      return false;
    }
    for (uint32_t i = 0; i < node_count_; ++i) {
      if (nodes_[i] == &node) {
        return false;
      }
    }
    nodes_[node_count_++] = &node;
    return true;
  }

  // ======================== Lifecycle ========================

  /**
   * @brief Start the pinned dispatcher thread.
   *
   * Launches a background thread pinned to the configured CPU core
   * that continuously calls ProcessBatch() on the shared AsyncBus.
   */
  void Start() noexcept {
    if (running_.load(std::memory_order_acquire)) {
      return;  // Already running
    }
    running_.store(true, std::memory_order_release);
    dispatch_thread_ = std::thread([this]() {
      PinThread(cpu_core_);
      DispatchLoop();
    });
  }

  /**
   * @brief Stop the pinned dispatcher thread and wait for it to finish.
   */
  void Stop() noexcept {
    running_.store(false, std::memory_order_release);
    if (dispatch_thread_.joinable()) {
      dispatch_thread_.join();
    }
  }

  // ======================== Accessors ========================

  /** @brief Check if the dispatcher thread is running. */
  bool IsRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

  /** @brief Set heartbeat for external watchdog monitoring. */
  void SetHeartbeat(ThreadHeartbeat* hb) noexcept { heartbeat_ = hb; }

 private:
  /**
   * @brief Main dispatch loop for the pinned background thread.
   */
  void DispatchLoop() noexcept {
    while (running_.load(std::memory_order_relaxed)) {
      if (heartbeat_ != nullptr) { heartbeat_->Beat(); }
      uint32_t processed = BusType::Instance().ProcessBatch();
      if (processed == 0) {
        std::this_thread::yield();
      }
    }
  }

  /**
   * @brief Pin the current thread to the specified CPU core.
   *
   * On Linux: uses pthread_setaffinity_np with cpu_set_t.
   * On macOS: no portable API, logs a warning to stderr.
   * On other platforms: no-op.
   *
   * @param core CPU core index (0-based).
   */
  static void PinThread(int32_t core) noexcept {
#if defined(OSP_PLATFORM_LINUX)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    int32_t rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
      (void)std::fprintf(stderr,
                         "PinnedExecutor: failed to pin thread to core %d "
                         "(errno=%d)\n",
                         core, rc);
    }
#elif defined(OSP_PLATFORM_MACOS)
    (void)core;
    (void)std::fprintf(stderr,
                       "PinnedExecutor: CPU pinning not supported on macOS, "
                       "skipping core %d\n",
                       core);
#else
    (void)core;
#endif
  }

  Node<PayloadVariant>* nodes_[OSP_EXECUTOR_MAX_NODES];
  uint32_t node_count_;
  std::atomic<bool> running_;
  std::thread dispatch_thread_;
  int32_t cpu_core_;
  ThreadHeartbeat* heartbeat_{nullptr};
};

// ============================================================================
// RealtimeConfig
// ============================================================================

/**
 * @brief Realtime scheduling configuration (inspired by CyberRT Processor).
 *
 * Configures thread scheduling policy, priority, memory locking, stack size,
 * and CPU affinity for realtime execution contexts.
 */
struct RealtimeConfig {
  int32_t sched_policy = 0;       // SCHED_OTHER=0, SCHED_FIFO=1, SCHED_RR=2
  int32_t sched_priority = 0;     // SCHED_FIFO: 1-99, higher = more priority
  bool lock_memory = false;   // mlockall(MCL_CURRENT | MCL_FUTURE)
  uint32_t stack_size = 0;    // 0=system default, non-zero=preallocated stack
  int32_t cpu_affinity = -1;      // -1=no binding, >=0=bind to specific CPU core
};

// ============================================================================
// RealtimeExecutor<PayloadVariant>
// ============================================================================

/**
 * @brief Realtime executor with configurable scheduling policy, priority,
 *        memory locking, and CPU affinity.
 *
 * On Linux: applies SCHED_FIFO/SCHED_RR, mlockall, pthread_setaffinity_np,
 * and custom stack size as configured.
 * On non-Linux: falls back to normal thread with warnings logged to stderr.
 *
 * @tparam PayloadVariant A std::variant<...> of user-defined message types.
 */
template <typename PayloadVariant>
class RealtimeExecutor {
 public:
  using BusType = AsyncBus<PayloadVariant>;

  /**
   * @brief Construct a realtime executor with the specified configuration.
   * @param cfg Realtime configuration (scheduling, affinity, memory locking).
   */
  explicit RealtimeExecutor(const RealtimeConfig& cfg) noexcept
      : node_count_(0), running_(false), config_(cfg) {
    for (uint32_t i = 0; i < OSP_EXECUTOR_MAX_NODES; ++i) {
      nodes_[i] = nullptr;
    }
  }

  ~RealtimeExecutor() noexcept { Stop(); }

  RealtimeExecutor(const RealtimeExecutor&) = delete;
  RealtimeExecutor& operator=(const RealtimeExecutor&) = delete;
  RealtimeExecutor(RealtimeExecutor&&) = delete;
  RealtimeExecutor& operator=(RealtimeExecutor&&) = delete;

  // ======================== Node Management ========================

  /**
   * @brief Add a node to this executor.
   *
   * Must be called before Start(). The node's lifetime must exceed the
   * executor's active period.
   *
   * @param node Reference to the node to manage.
   * @return true if added, false if the node array is full or duplicate.
   */
  bool AddNode(Node<PayloadVariant>& node) noexcept {
    if (node_count_ >= OSP_EXECUTOR_MAX_NODES) {
      return false;
    }
    for (uint32_t i = 0; i < node_count_; ++i) {
      if (nodes_[i] == &node) {
        return false;
      }
    }
    nodes_[node_count_++] = &node;
    return true;
  }

  // ======================== Lifecycle ========================

  /**
   * @brief Start the realtime dispatcher thread.
   *
   * On Linux: applies sched_policy, sched_priority, cpu_affinity, mlockall,
   * and stack_size as configured.
   * On non-Linux: falls back to normal thread (logs warning).
   */
  void Start() noexcept {
    if (running_.load(std::memory_order_acquire)) {
      return;  // Already running
    }
    running_.store(true, std::memory_order_release);

#if defined(OSP_PLATFORM_LINUX)
    // If custom stack size is requested, use pthread_create directly
    if (config_.stack_size > 0) {
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, config_.stack_size);

      int32_t rc = pthread_create(&rt_thread_, &attr, &RealtimeExecutor::ThreadEntry, this);
      pthread_attr_destroy(&attr);

      if (rc != 0) {
        (void)std::fprintf(stderr,
                           "RealtimeExecutor: pthread_create failed (errno=%d)\n",
                           rc);
        running_.store(false, std::memory_order_release);
        return;
      }
      use_pthread_ = true;
    } else {
      dispatch_thread_ = std::thread([this]() {
        ApplyRealtimeConfig(config_);
        DispatchLoop();
      });
      use_pthread_ = false;
    }
#else
    (void)std::fprintf(stderr,
                       "RealtimeExecutor: realtime features not supported on "
                       "this platform, using normal thread\n");
    dispatch_thread_ = std::thread([this]() { DispatchLoop(); });
    use_pthread_ = false;
#endif
  }

  /**
   * @brief Stop the realtime dispatcher thread and wait for it to finish.
   */
  void Stop() noexcept {
    running_.store(false, std::memory_order_release);
#if defined(OSP_PLATFORM_LINUX)
    if (use_pthread_) {
      if (rt_thread_ != pthread_t{}) {
        pthread_join(rt_thread_, nullptr);
        rt_thread_ = pthread_t{};
      }
    } else {
      if (dispatch_thread_.joinable()) {
        dispatch_thread_.join();
      }
    }
#else
    if (dispatch_thread_.joinable()) {
      dispatch_thread_.join();
    }
#endif
    use_pthread_ = false;
  }

  // ======================== Accessors ========================

  /** @brief Check if the dispatcher thread is running. */
  bool IsRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

  /** @brief Get the number of registered nodes. */
  uint32_t NodeCount() const noexcept { return node_count_; }

  /** @brief Get the realtime configuration. */
  const RealtimeConfig& GetConfig() const noexcept { return config_; }

  /** @brief Set heartbeat for external watchdog monitoring. */
  void SetHeartbeat(ThreadHeartbeat* hb) noexcept { heartbeat_ = hb; }

 private:
  /**
   * @brief Main dispatch loop for the realtime background thread.
   */
  void DispatchLoop() noexcept {
    while (running_.load(std::memory_order_relaxed)) {
      if (heartbeat_ != nullptr) { heartbeat_->Beat(); }
      uint32_t processed = BusType::Instance().ProcessBatch();
      if (processed == 0) {
        std::this_thread::yield();
      }
    }
  }

  /**
   * @brief Apply realtime configuration to the current thread.
   *
   * On Linux: applies mlockall, CPU affinity, and scheduling policy/priority.
   * On other platforms: no-op.
   *
   * @param cfg Realtime configuration to apply.
   */
  static void ApplyRealtimeConfig(const RealtimeConfig& cfg) noexcept {
#if defined(OSP_PLATFORM_LINUX)
    // 1. Lock memory if requested
    if (cfg.lock_memory) {
      int32_t rc = mlockall(MCL_CURRENT | MCL_FUTURE);
      if (rc != 0) {
        (void)std::fprintf(stderr,
                           "RealtimeExecutor: mlockall failed (errno=%d)\n",
                           errno);
      }
    }

    // 2. Set CPU affinity if requested
    if (cfg.cpu_affinity >= 0) {
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      CPU_SET(cfg.cpu_affinity, &cpuset);
      int32_t rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
      if (rc != 0) {
        (void)std::fprintf(stderr,
                           "RealtimeExecutor: pthread_setaffinity_np failed "
                           "(errno=%d)\n",
                           rc);
      }
    }

    // 3. Set scheduling policy and priority if not SCHED_OTHER
    if (cfg.sched_policy != 0) {
      struct sched_param param;
      param.sched_priority = cfg.sched_priority;
      int32_t rc = pthread_setschedparam(pthread_self(), cfg.sched_policy, &param);
      if (rc != 0) {
        (void)std::fprintf(stderr,
                           "RealtimeExecutor: pthread_setschedparam failed "
                           "(policy=%d, priority=%d, errno=%d)\n",
                           cfg.sched_policy, cfg.sched_priority, rc);
      }
    }
#else
    (void)cfg;
#endif
  }

  static void* ThreadEntry(void* arg) {
    auto* self = static_cast<RealtimeExecutor*>(arg);
    self->ApplyRealtimeConfig(self->config_);
    self->DispatchLoop();
    return nullptr;
  }

  Node<PayloadVariant>* nodes_[OSP_EXECUTOR_MAX_NODES];
  uint32_t node_count_;
  std::atomic<bool> running_;
  std::thread dispatch_thread_;
  RealtimeConfig config_;
  ThreadHeartbeat* heartbeat_{nullptr};
#if defined(OSP_PLATFORM_LINUX)
  pthread_t rt_thread_{};
  bool use_pthread_{false};
#endif
};

}  // namespace osp

#endif  // OSP_EXECUTOR_HPP_
