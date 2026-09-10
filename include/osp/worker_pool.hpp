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
 * @file osp/worker_pool.hpp
 * @brief WorkerPool - Multi-worker thread pool built on osp::AsyncBus.
 *
 * Architecture:
 *   Submit() -> osp::AsyncBus::Publish() (lock-free MPSC)
 *                    |
 *              DispatcherThread (ProcessBatch loop)
 *                    | round-robin
 *              Worker[0..N-1] SPSC Queue -> WorkerThread -> Handler
 *
 * Features:
 * - Lock-free ingress via MPSC ring buffer
 * - Lock-free SPSC per-worker queues for dispatch
 * - Function pointer handlers (no std::function in worker hot path)
 * - Priority-based admission control (osp::MessagePriority)
 * - FlushAndPause / Resume for graceful draining
 * - Synchronous execution mode (SubmitSync)
 * - Thread priority and CPU affinity support (Linux)
 * - -fno-exceptions -fno-rtti compatible
 *
 * @tparam PayloadVariant A std::variant<...> of user-defined message types.
 *
 * Usage:
 *   struct TaskA { int id; };
 *   struct TaskB { float value; };
 *   using MyPayload = std::variant<TaskA, TaskB>;
 *
 *   osp::WorkerPoolConfig cfg;
 *   cfg.name = "demo";
 *   cfg.worker_num = 4;
 *
 *   osp::WorkerPool<MyPayload> pool(cfg);
 *   pool.RegisterHandler<TaskA>([](const TaskA& t, const osp::MessageHeader&) { ... });
 *   pool.Start();
 *   pool.Submit(TaskA{1});
 *   pool.Shutdown();
 */

#ifndef OSP_WORKER_POOL_HPP_
#define OSP_WORKER_POOL_HPP_

#include "osp/bus.hpp"
#include "osp/semaphore.hpp"
#include "osp/spsc_ringbuffer.hpp"
#include "osp/thread.hpp"
#include "osp/vocabulary.hpp"

#include <cstdint>

#include <array>
#include <atomic>
#include <variant>

#ifdef __linux__
#include <sched.h>
#endif

namespace osp {

// ============================================================================
// Configuration
// ============================================================================

/**
 * @brief WorkerPool configuration.
 *
 * Worker queue depth is now compile-time via OSP_WORKER_QUEUE_DEPTH macro.
 */
struct WorkerPoolConfig {
  osp::FixedString<32> name{"pool"};
  uint32_t worker_num{1U};
  int32_t priority{0};
#ifdef __linux__
  uint32_t cpu_set_size{0U};
  const cpu_set_t* cpu_set{nullptr};
#endif
};

// ============================================================================
// WorkerPool Error Codes
// ============================================================================

enum class WorkerPoolError {
  kFlushTimeout,     ///< FlushAndPause timed out waiting for workers
  kWorkerUnhealthy,  ///< One or more worker threads are dead
};

// ============================================================================
// WorkerPool Statistics
// ============================================================================

struct WorkerPoolStats {
  uint64_t dispatched{0U};
  uint64_t processed{0U};
  uint64_t worker_queue_full{0U};
  osp::BusStatisticsSnapshot bus_stats{};
};

// ============================================================================
// WorkerPool
// ============================================================================

/**
 * @brief Multi-worker thread pool built on osp::AsyncBus.
 *
 * @tparam PayloadVariant A std::variant<...> of user-defined message types.
 */
template <typename PayloadVariant>
class WorkerPool {
 public:
  using BusType = osp::AsyncBus<PayloadVariant>;
  using EnvelopeType = osp::MessageEnvelope<PayloadVariant>;

  static constexpr uint32_t kMaxTypes = std::variant_size_v<PayloadVariant>;
  static constexpr uint32_t kMaxWorkers = OSP_WORKER_POOL_MAX_WORKERS;

  /** @brief Callback invoked when a job is dropped because all worker queues are full. */
  using OverflowCallback = void (*)(void* ctx);

  explicit WorkerPool(const WorkerPoolConfig& cfg) noexcept
      : name_(cfg.name),
        worker_num_(cfg.worker_num > 0U ? (cfg.worker_num <= kMaxWorkers ? cfg.worker_num : kMaxWorkers) : 1U),
#ifdef __linux__
        cpu_set_size_(cfg.cpu_set_size),
        cpu_set_(cfg.cpu_set),
#endif
        priority_(cfg.priority) {
  }

  ~WorkerPool() noexcept {
    if (running_.load(std::memory_order_acquire)) {
      Shutdown();
    }
  }

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;
  WorkerPool(WorkerPool&&) = delete;
  WorkerPool& operator=(WorkerPool&&) = delete;

  // ======================== Handler Registration ========================

  /**
   * @brief Register a handler for message type T (function pointer overload).
   *
   * Must be called before Start(). Handler is invoked in worker threads.
   *
   * @tparam T Message type (must be in PayloadVariant)
   * @param handler Function pointer: void(const T&, const MessageHeader&)
   */
  template <typename T>
  void RegisterHandler(void (*handler)(const T&, const osp::MessageHeader&)) noexcept {
    constexpr size_t idx = osp::VariantIndex<T, PayloadVariant>::value;
    static_assert(idx < kMaxTypes, "Type not in PayloadVariant");
    auto fn_ptr = handler;
    handlers_[idx] = HandlerFn([fn_ptr](const EnvelopeType& env) {
      const T* data = std::get_if<T>(&env.payload);
      if (data != nullptr) {
        fn_ptr(*data, env.header);
      }
    });
  }

  /**
   * @brief Register a callable handler for message type T.
   *
   * Accepts lambdas (with captures), function objects, etc.
   * Must be called before Start(). Handler is invoked in worker threads.
   *
   * @tparam T    Message type (must be in PayloadVariant)
   * @tparam Func Callable: void(const T&, const MessageHeader&)
   */
  template <typename T, typename Func,
            typename = typename std::enable_if<
                !std::is_convertible<Func, void (*)(const T&, const osp::MessageHeader&)>::value>::type>
  void RegisterHandler(Func&& func) noexcept {
    constexpr size_t idx = osp::VariantIndex<T, PayloadVariant>::value;
    static_assert(idx < kMaxTypes, "Type not in PayloadVariant");
    handlers_[idx] = HandlerFn([f = static_cast<Func&&>(func)](const EnvelopeType& env) {
      const T* data = std::get_if<T>(&env.payload);
      if (data != nullptr) {
        f(*data, env.header);
      }
    });
  }

  // ======================== Lifecycle ========================

  /**
   * @brief Start dispatcher and worker threads.
   *
   * Subscribes to all registered message types on the bus,
   * then starts the dispatcher thread and N worker threads.
   */
  void Start() noexcept {
    if (running_.load(std::memory_order_acquire)) {
      return;
    }
    running_.store(true, std::memory_order_release);
    shutdown_.store(false, std::memory_order_release);

    for (uint32_t i = 0U; i < worker_num_; ++i) {
      workers_.emplace_back();
    }

    SubscribeAll(static_cast<PayloadVariant*>(nullptr));

    if (!StartDispatcher() || !StartWorkers()) {
      // Thread creation failed (e.g. RT-Thread heap exhausted): stop whatever
      // started and roll the pool back to the not-running state.
      shutdown_.store(true, std::memory_order_release);
      StopAll();
      running_.store(false, std::memory_order_release);
    }
  }

  /**
   * @brief Shutdown: stop accepting jobs, drain queues, join all threads.
   *
   * Handles dead worker threads gracefully - only joins threads that are still joinable.
   */
  void Shutdown() noexcept {
    if (!running_.load(std::memory_order_acquire)) {
      return;
    }

    shutdown_.store(true, std::memory_order_release);
    StopAll();
    running_.store(false, std::memory_order_release);
  }

  /**
   * @brief Flush all pending work and pause accepting new jobs.
   *
   * Blocks until all dispatched jobs are processed by workers, or timeout is reached.
   *
   * @param timeout_ms Maximum time to wait in milliseconds (default 5000ms)
   * @return expected<void, WorkerPoolError> - kFlushTimeout if timeout exceeded
   */
  osp::expected<void, WorkerPoolError> FlushAndPause(uint32_t timeout_ms = 5000U) noexcept {
    paused_.store(true, std::memory_order_release);

    const uint64_t start_us = osp::SteadyNowUs();
    const uint64_t timeout_us = static_cast<uint64_t>(timeout_ms) * 1000ULL;

    // Wait until bus is drained, all worker queues empty, and all dispatched jobs processed
    while (true) {
      bool bus_empty = (BusType::Instance().Depth() == 0U);
      bool workers_empty = true;
      for (uint32_t i = 0U; i < worker_num_; ++i) {
        if (!workers_[i].queue.IsEmpty()) {
          workers_empty = false;
          break;
        }
      }
      uint64_t disp = dispatched_.load(std::memory_order_acquire);
      uint64_t proc = processed_.load(std::memory_order_acquire);
      if (bus_empty && workers_empty && disp == proc) {
        return osp::expected<void, WorkerPoolError>::success();
      }

      // Check timeout
      if ((osp::SteadyNowUs() - start_us) >= timeout_us) {
        return osp::expected<void, WorkerPoolError>::error(WorkerPoolError::kFlushTimeout);
      }

      osp::ThreadSleepUs(100);
    }
  }

  /**
   * @brief Resume accepting jobs after FlushAndPause.
   */
  void Resume() noexcept { paused_.store(false, std::memory_order_release); }

  // ======================== Submit API ========================

  /**
   * @brief Submit a job asynchronously via the bus (lock-free).
   *
   * @tparam T Message type
   * @param payload Message payload
   * @param priority Message priority (default kMedium)
   * @return true if published to bus, false if paused or bus rejected
   */
  template <typename T>
  bool Submit(T&& payload, osp::MessagePriority priority = osp::MessagePriority::kMedium) noexcept {
    if (paused_.load(std::memory_order_acquire)) {
      return false;
    }
    return BusType::Instance().PublishWithPriority(PayloadVariant(std::forward<T>(payload)), 0U, priority);
  }

  /**
   * @brief Execute a job synchronously in the caller's thread.
   *
   * Bypasses the bus and worker threads entirely.
   *
   * @tparam T Message type
   * @param payload Message payload
   * @return true if handler exists and was invoked, false otherwise
   */
  template <typename T>
  bool SubmitSync(T&& payload) noexcept {
    constexpr size_t idx = osp::VariantIndex<T, PayloadVariant>::value;
    if (!handlers_[idx]) {
      return false;
    }
    osp::MessageHeader header(0U, SteadyNowUs(), 0U, osp::MessagePriority::kHigh);
    PayloadVariant pv(std::forward<T>(payload));
    EnvelopeType env(header, std::move(pv));
    handlers_[idx](env);
    return true;
  }

  // ======================== Query ========================

  WorkerPoolStats GetStats() const noexcept {
    WorkerPoolStats s;
    s.dispatched = dispatched_.load(std::memory_order_relaxed);
    s.processed = processed_.load(std::memory_order_relaxed);
    s.worker_queue_full = worker_queue_full_.load(std::memory_order_relaxed);
    s.bus_stats = BusType::Instance().GetStatistics();
    return s;
  }

  uint32_t WorkerCount() const noexcept { return worker_num_; }

  bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }

  bool IsPaused() const noexcept { return paused_.load(std::memory_order_acquire); }

  /**
   * @brief Check if all worker threads are healthy (still joinable).
   *
   * A worker thread becomes unjoinable if it has terminated unexpectedly
   * (e.g., due to a crash in a callback).
   *
   * @return expected<void, WorkerPoolError> - kWorkerUnhealthy if any worker is dead
   */
  osp::expected<void, WorkerPoolError> IsHealthy() const noexcept {
    if (!running_.load(std::memory_order_acquire)) {
      return osp::expected<void, WorkerPoolError>::success();  // Not started yet or already shut down - not an error
    }

    for (const auto& t : worker_threads_) {
      if (!t.joinable()) {
        return osp::expected<void, WorkerPoolError>::error(WorkerPoolError::kWorkerUnhealthy);
      }
    }

    if (!dispatcher_thread_.joinable() && running_.load(std::memory_order_acquire)) {
      return osp::expected<void, WorkerPoolError>::error(WorkerPoolError::kWorkerUnhealthy);
    }

    return osp::expected<void, WorkerPoolError>::success();
  }

  /**
   * @brief Set heartbeat for external watchdog monitoring of dispatcher thread.
   *
   * The dispatcher thread will call hb->Beat() each iteration.
   * Worker threads can be monitored by setting worker heartbeats via
   * SetWorkerHeartbeat() after Start().
   */
  void SetHeartbeat(ThreadHeartbeat* hb) noexcept { heartbeat_ = hb; }

  /**
   * @brief Set a callback invoked when a job cannot be dispatched because all
   *        worker queues are full.
   *
   * The callback runs on the dispatcher thread. This lets callers observe
   * (and recover from) dropped jobs instead of silently losing them.
   *
   * @param cb  Callback: void(void* ctx). nullptr clears the callback.
   * @param ctx User context passed to the callback.
   */
  void SetOnOverflow(OverflowCallback cb, void* ctx = nullptr) noexcept {
    overflow_cb_.store(cb, std::memory_order_release);
    overflow_ctx_.store(ctx, std::memory_order_release);
  }

 private:
  /// SBO buffer for handler callable (fits lambda with 1-2 captures).
  static constexpr size_t kHandlerBufSize = 4 * sizeof(void*);
  using HandlerFn = osp::FixedFunction<void(const EnvelopeType&), kHandlerBufSize>;

  void DispatchEnvelope(const EnvelopeType& env) noexcept {
    const size_t idx = env.payload.index();
    if (idx < kMaxTypes && handlers_[idx]) {
      handlers_[idx](env);
    }
  }

  // ======================== Bus subscription ========================

  template <typename T>
  void MaybeSubscribe() noexcept {
    constexpr size_t idx = osp::VariantIndex<T, PayloadVariant>::value;
    if (!handlers_[idx]) {
      return;
    }
    auto handle =
        BusType::Instance().template Subscribe<T>([this](const EnvelopeType& env) noexcept { DispatchToWorker(env); });
    if (handle.IsValid()) {
      subscription_handles_.push_back(handle);
    }
  }

  template <typename... Types>
  void SubscribeAllImpl(std::variant<Types...>* /*tag*/) noexcept {
    (MaybeSubscribe<Types>(), ...);
  }

  void SubscribeAll(PayloadVariant* tag) noexcept { SubscribeAllImpl(tag); }

  // ======================== Worker dispatch ========================

  void DispatchToWorker(const EnvelopeType& env) noexcept {
    const uint32_t start = next_worker_.fetch_add(1U, std::memory_order_relaxed) % worker_num_;
    for (uint32_t i = 0U; i < worker_num_; ++i) {
      const uint32_t wid = (start + i) % worker_num_;
      if (workers_[wid].queue.Push(env)) {
        dispatched_.fetch_add(1U, std::memory_order_release);
        workers_[wid].wakeup.Signal();
        return;
      }
    }
    worker_queue_full_.fetch_add(1U, std::memory_order_relaxed);
    // The task could not be delivered to any worker: report the overflow so
    // callers can observe dropped jobs instead of silently losing them.
    OverflowCallback cb = overflow_cb_.load(std::memory_order_acquire);
    if (cb != nullptr) {
      cb(overflow_ctx_.load(std::memory_order_acquire));
    }
  }

  // ======================== Dispatcher thread ========================

  void DispatcherLoop() noexcept {
    osp::AdaptiveBackoff backoff;

    detail::BeatLoop(
        heartbeat_, [this]() { return !shutdown_.load(std::memory_order_acquire); },
        [&backoff, this]() {
          uint32_t count = BusType::Instance().ProcessBatch();
          if (count > 0U) {
            backoff.Reset();
          } else {
            backoff.Wait();
          }
        });

    // Final drain
    for (uint32_t round = 0U; round < 10U; ++round) {
      if (BusType::Instance().ProcessBatch() == 0U) {
        break;
      }
    }
  }

  // ======================== Worker thread ========================

  void WorkerLoop(uint32_t worker_id) noexcept {
    WorkerContext& ctx = workers_[worker_id];
    EnvelopeType env;
    osp::AdaptiveBackoff backoff;

    while (!shutdown_.load(std::memory_order_acquire)) {
      if (ctx.queue.Pop(env)) {
        DispatchEnvelope(env);
        processed_.fetch_add(1U, std::memory_order_release);
        backoff.Reset();
        continue;
      }

      // Adaptive spin before the timed wait
      if (backoff.InSpinPhase()) {
        backoff.Wait();
        continue;
      }

      // 1ms timed wait; the semaphore is a wake-up hint, the SPSC queue is the
      // source of truth, so a stale signal only costs one extra empty Pop.
      ctx.wakeup.WaitFor(1000);
      backoff.Reset();
    }

    // Drain remaining
    while (ctx.queue.Pop(env)) {
      DispatchEnvelope(env);
      processed_.fetch_add(1U, std::memory_order_release);
    }
  }

  // ======================== Lifecycle helpers ========================

  ThreadOptions MakeThreadOptions(const char* name) const noexcept {
    ThreadOptions opts;
    opts.name.assign(osp::TruncateToCapacity, name);
    opts.priority = priority_;
#ifdef __linux__
    opts.cpu_set = cpu_set_;
    opts.cpu_set_size = cpu_set_size_;
#endif
    return opts;
  }

  bool StartDispatcher() noexcept {
    return dispatcher_thread_.Start(MakeThreadOptions("disp"), [this]() { DispatcherLoop(); });
  }

  bool StartWorkers() noexcept {
    for (uint32_t i = 0U; i < worker_num_; ++i) {
      if (!worker_threads_.emplace_back()) {
        return false;  // FixedVector full (worker_num_ is clamped to kMaxWorkers)
      }
      if (!worker_threads_.back().Start(MakeThreadOptions("wkr"), [this, i]() { WorkerLoop(i); })) {
        return false;
      }
    }
    return true;
  }

  void UnsubscribeAll() noexcept {
    for (uint32_t i = 0U; i < subscription_handles_.size(); ++i) {
      BusType::Instance().Unsubscribe(subscription_handles_[i]);
    }
    subscription_handles_.clear();
  }

  void StopAll() noexcept {
    // Wake up all workers (1ms WaitFor also times out on its own).
    for (uint32_t i = 0U; i < workers_.size(); ++i) {
      workers_[i].wakeup.Signal();
    }

    // Join dispatcher thread if still alive
    if (dispatcher_thread_.joinable()) {
      dispatcher_thread_.join();
    }

    // Join worker threads (skip if already dead)
    for (uint32_t i = 0U; i < worker_threads_.size(); ++i) {
      if (worker_threads_[i].joinable()) {
        worker_threads_[i].join();
      }
    }

    UnsubscribeAll();
    workers_.clear();
    worker_threads_.clear();
  }

  // ======================== Worker context ========================

  struct WorkerContext {
    osp::SpscRingbuffer<EnvelopeType, OSP_WORKER_QUEUE_DEPTH> queue;
    osp::Semaphore wakeup{0};

    WorkerContext() noexcept = default;
    WorkerContext(const WorkerContext&) = delete;
    WorkerContext& operator=(const WorkerContext&) = delete;
    WorkerContext(WorkerContext&&) = delete;
    WorkerContext& operator=(WorkerContext&&) = delete;
  };

  // ======================== Data members ========================

  osp::FixedString<32> name_;
  const uint32_t worker_num_;
#ifdef __linux__
  const uint32_t cpu_set_size_{0U};
  const cpu_set_t* cpu_set_{nullptr};
#endif
  const int32_t priority_;

  std::array<HandlerFn, kMaxTypes> handlers_;

  std::atomic<bool> running_{false};
  std::atomic<bool> shutdown_{false};
  std::atomic<bool> paused_{false};

  alignas(osp::kCacheLineSize) std::atomic<uint64_t> dispatched_{0U};
  alignas(osp::kCacheLineSize) std::atomic<uint64_t> processed_{0U};
  alignas(osp::kCacheLineSize) std::atomic<uint64_t> worker_queue_full_{0U};
  alignas(osp::kCacheLineSize) std::atomic<uint32_t> next_worker_{0U};

  FixedVector<WorkerContext, kMaxWorkers> workers_;
  FixedVector<osp::Thread, kMaxWorkers> worker_threads_;
  osp::Thread dispatcher_thread_;
  FixedVector<osp::SubscriptionHandle, kMaxTypes> subscription_handles_;
  ThreadHeartbeat* heartbeat_{nullptr};  ///< Dispatcher thread heartbeat.
  std::atomic<OverflowCallback> overflow_cb_{nullptr};
  std::atomic<void*> overflow_ctx_{nullptr};
};

}  // namespace osp

#endif  // OSP_WORKER_POOL_HPP_
