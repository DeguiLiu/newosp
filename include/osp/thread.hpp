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
 * @file osp/thread.hpp
 * @brief osp::Thread - cross-platform thread abstraction.
 * std::thread on Linux, rt_thread_create on RT-Thread. RT-Thread join() uses a
 * semaphore because the idle thread reclaims exited TCBs (no rt_thread_delete).
 */

#ifndef OSP_THREAD_HPP_
#define OSP_THREAD_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <cstdint>

#include <utility>

#if defined(OSP_PLATFORM_RTTHREAD)
#include <rtthread.h>
#else
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <thread>

#if defined(OSP_PLATFORM_LINUX)
#include <sys/syscall.h>
#include <unistd.h>
#endif
#endif

namespace osp {

// ============================================================================
// ThreadOptions
// ============================================================================

/// SBO buffer for the entry callable. Fits the largest capture shape used in
/// the codebase (service async handler, ~32B) with headroom. StartImpl()
/// static_asserts that the callable fits, so oversized captures fail at compile
/// time on both platforms.
static constexpr size_t kThreadCallableBufSize = 6U * sizeof(void*);

/// Fixed-function entry type (void() signature) used on both platforms.
using ThreadCallable = osp::FixedFunction<void(), kThreadCallableBufSize>;

/**
 * @brief Thread creation parameters.
 * Fields a platform does not support are ignored rather than rejected.
 */
struct ThreadOptions {
  osp::FixedString<16> name{"osp"};  ///< RT-Thread truncates to RT_NAME_MAX (8).
  uint32_t stack_size{0U};           ///< Bytes; 0 = platform default (RT-Thread: 8KB).
  int32_t priority{0};               ///< Linux: >0 -> SCHED_FIFO(1..99), <0 -> SCHED_IDLE, 0 -> default.
                                     ///< RT-Thread: 0 -> mid-range, positive -> 0..RT_THREAD_PRIORITY_MAX-1.
#ifdef __linux__
  const cpu_set_t* cpu_set{nullptr};  ///< Optional CPU affinity mask (Linux only).
  size_t cpu_set_size{0U};
#endif
};

// ============================================================================
// Thread
// ============================================================================

/**
 * @brief Portable thread: std::thread subset plus name/stack/priority options.
 * Join a joinable Thread before destruction, as with std::thread. Debug builds
 * assert this in the destructor; release builds leave it undefined behavior.
 */
class Thread {
 public:
  Thread() noexcept = default;
  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;
  Thread(Thread&& other) noexcept;
  Thread& operator=(Thread&& other) noexcept;
  ~Thread() noexcept;

  /** @brief Start with default options. Returns false if already joinable or creation failed. */
  template <typename F>
  bool Start(F&& fn) noexcept {
    return StartImpl(ThreadOptions{}, std::forward<F>(fn));
  }

  /** @brief Start a callable (lambda / functor / free function) with options. */
  template <typename F>
  bool Start(const ThreadOptions& opts, F&& fn) noexcept {
    return StartImpl(opts, std::forward<F>(fn));
  }

  /** @brief Start a member function, e.g. Start(opts, &C::Loop, this, i). */
  template <typename R, typename C, typename... Args>
  bool Start(const ThreadOptions& opts, R (C::*method)(Args...), C* obj, Args... args) noexcept {
    return StartImpl(opts, [method, obj, args...]() { (obj->*method)(args...); });
  }

  /** @brief Block until the thread finishes. No-op when not joinable. */
  void join() noexcept;

  /** @brief True while a started thread has not been joined yet. */
  [[nodiscard]] bool joinable() const noexcept;

  void swap(Thread& other) noexcept;

  /**
   * @brief Stable numeric id of the calling thread.
   * Linux: kernel tid (SYS_gettid); RT-Thread: thread handle. Unique per live
   * thread.
   */
  static uintptr_t CurrentThreadId() noexcept;

 private:
  template <typename F>
  bool StartImpl(const ThreadOptions& opts, F&& fn) noexcept;

#if !defined(OSP_PLATFORM_RTTHREAD)
  static void ApplyOptions(const ThreadOptions& opts) noexcept;
#endif

#if defined(OSP_PLATFORM_RTTHREAD)
  static constexpr uint32_t kDefaultStackSize = 8192U;
  static constexpr uint32_t kDefaultTimeSlice = 10U;

  /// Heap-owned entry block: keeps the callable alive independent of the Thread
  /// object's address, so moving a running Thread is safe on RT-Thread.
  struct RtThreadParam {
    ThreadCallable fn;
    rt_sem_t join_sem{nullptr};
  };

  static void RtEntry(void* param) noexcept;
  RtThreadParam* rt_param_{nullptr};
  rt_thread_t rt_handle_{nullptr};
#else
  std::thread native_;
#endif
};

// ============================================================================
// Mutex
// ============================================================================

/**
 * @brief BasicLockable mutex, usable with std::lock_guard / unique_lock.
 * Host: std::mutex. RT-Thread: rt_mutex with priority inheritance.
 */
class Mutex {
 public:
  Mutex() noexcept {
#if defined(OSP_PLATFORM_RTTHREAD)
    native_ = rt_mutex_create("osp", RT_IPC_FLAG_PRIO);
#endif
  }

  ~Mutex() {
#if defined(OSP_PLATFORM_RTTHREAD)
    if (native_ != nullptr) {
      rt_mutex_delete(native_);
    }
#endif
  }

  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;
  Mutex(Mutex&&) = delete;
  Mutex& operator=(Mutex&&) = delete;

  void lock() noexcept {
#if defined(OSP_PLATFORM_RTTHREAD)
    if (native_ != nullptr) {
      rt_mutex_take(native_, RT_WAITING_FOREVER);
    }
#else
    native_.lock();
#endif
  }

  void unlock() noexcept {
#if defined(OSP_PLATFORM_RTTHREAD)
    if (native_ != nullptr) {
      rt_mutex_release(native_);
    }
#else
    native_.unlock();
#endif
  }

  bool try_lock() noexcept {
#if defined(OSP_PLATFORM_RTTHREAD)
    if (native_ == nullptr) {
      return false;
    }
    return rt_mutex_take(native_, RT_WAITING_NO) == RT_EOK;
#else
    return native_.try_lock();
#endif
  }

 private:
#if defined(OSP_PLATFORM_RTTHREAD)
  rt_mutex_t native_{nullptr};
#else
  std::mutex native_;
#endif
};

// ============================================================================
// Inline definitions
// ============================================================================

#if !defined(OSP_PLATFORM_RTTHREAD)

inline void Thread::ApplyOptions(const ThreadOptions& opts) noexcept {
  if (opts.priority > 0) {
    struct sched_param param{};
    param.sched_priority = (opts.priority > 99) ? 99 : opts.priority;
    (void)pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
  } else if (opts.priority < 0) {
    struct sched_param param{};
    param.sched_priority = 0;
    (void)pthread_setschedparam(pthread_self(), SCHED_IDLE, &param);
  }
#if defined(OSP_PLATFORM_LINUX)
  if (opts.cpu_set != nullptr && opts.cpu_set_size > 0U) {
    (void)pthread_setaffinity_np(pthread_self(), opts.cpu_set_size, opts.cpu_set);
  }
#endif
}

#endif  // !OSP_PLATFORM_RTTHREAD

#if defined(OSP_PLATFORM_RTTHREAD)

namespace detail {

inline rt_uint8_t ClampRtPriority(int32_t priority) noexcept {
  const int32_t max_prio = static_cast<int32_t>(RT_THREAD_PRIORITY_MAX);
  if (priority <= 0) {
    // Mid-range default; a worker pool at priority 0 would starve other threads.
    return static_cast<rt_uint8_t>(max_prio / 2);
  }
  if (priority >= max_prio) {
    return static_cast<rt_uint8_t>(max_prio - 1);
  }
  return static_cast<rt_uint8_t>(priority);
}

}  // namespace detail

inline void Thread::RtEntry(void* param) noexcept {
  RtThreadParam* p = static_cast<RtThreadParam*>(param);
  p->fn();
  rt_sem_release(p->join_sem);
}

#endif  // OSP_PLATFORM_RTTHREAD

template <typename F>
inline bool Thread::StartImpl(const ThreadOptions& opts, F&& fn) noexcept {
  if (joinable()) {
    return false;
  }
  using Decay = typename std::decay<F>::type;
  static_assert(sizeof(Decay) <= kThreadCallableBufSize, "Thread callable exceeds SBO buffer size");
#if defined(OSP_PLATFORM_RTTHREAD)
  void* mem = rt_malloc(sizeof(RtThreadParam));
  if (mem == nullptr) {
    return false;
  }
  RtThreadParam* param = ::new (mem) RtThreadParam();
  param->fn = ThreadCallable(std::forward<F>(fn));
  param->join_sem = rt_sem_create(opts.name.c_str(), 0, RT_IPC_FLAG_PRIO);
  if (param->join_sem == nullptr) {
    param->fn.~ThreadCallable();
    rt_free(param);
    return false;
  }
  const rt_uint32_t stack = (opts.stack_size > 0U) ? static_cast<rt_uint32_t>(opts.stack_size) : kDefaultStackSize;
  rt_thread_t handle = rt_thread_create(opts.name.c_str(), &Thread::RtEntry, param, stack,
                                        detail::ClampRtPriority(opts.priority), kDefaultTimeSlice);
  if (handle == nullptr) {
    rt_sem_delete(param->join_sem);
    param->fn.~ThreadCallable();
    rt_free(param);
    return false;
  }
  if (rt_thread_startup(handle) != RT_EOK) {
    (void)rt_thread_delete(handle);
    rt_sem_delete(param->join_sem);
    param->fn.~ThreadCallable();
    rt_free(param);
    return false;
  }
  rt_param_ = param;
  rt_handle_ = handle;
  return true;
#else
  native_ = std::thread([opts, fn = Decay(std::forward<F>(fn))]() mutable {
    ApplyOptions(opts);
    fn();
  });
  return true;
#endif
}

inline Thread::Thread(Thread&& other) noexcept {
#if defined(OSP_PLATFORM_RTTHREAD)
  rt_param_ = other.rt_param_;
  rt_handle_ = other.rt_handle_;
  other.rt_param_ = nullptr;
  other.rt_handle_ = nullptr;
#else
  native_ = std::move(other.native_);
#endif
}

inline Thread& Thread::operator=(Thread&& other) noexcept {
  if (this != &other) {
    OSP_ASSERT(!joinable());
#if defined(OSP_PLATFORM_RTTHREAD)
    rt_param_ = other.rt_param_;
    rt_handle_ = other.rt_handle_;
    other.rt_param_ = nullptr;
    other.rt_handle_ = nullptr;
#else
    native_ = std::move(other.native_);
#endif
  }
  return *this;
}

inline Thread::~Thread() noexcept {
  OSP_ASSERT(!joinable());
}

inline void Thread::join() noexcept {
#if defined(OSP_PLATFORM_RTTHREAD)
  if (rt_param_ == nullptr) {
    return;
  }
  rt_sem_take(rt_param_->join_sem, RT_WAITING_FOREVER);
  rt_sem_delete(rt_param_->join_sem);
  rt_param_->fn.~ThreadCallable();
  rt_free(rt_param_);
  rt_param_ = nullptr;
  rt_handle_ = nullptr;  // TCB already reclaimed by the idle thread; never dereference.
#else
  if (native_.joinable()) {
    native_.join();
  }
#endif
}

inline bool Thread::joinable() const noexcept {
#if defined(OSP_PLATFORM_RTTHREAD)
  return rt_param_ != nullptr;
#else
  return native_.joinable();
#endif
}

inline void Thread::swap(Thread& other) noexcept {
#if defined(OSP_PLATFORM_RTTHREAD)
  std::swap(rt_param_, other.rt_param_);
  std::swap(rt_handle_, other.rt_handle_);
#else
  native_.swap(other.native_);
#endif
}

inline uintptr_t Thread::CurrentThreadId() noexcept {
#if defined(OSP_PLATFORM_RTTHREAD)
  return reinterpret_cast<uintptr_t>(rt_thread_self());
#elif defined(OSP_PLATFORM_LINUX)
  return static_cast<uintptr_t>(::syscall(SYS_gettid));
#else
  return static_cast<uintptr_t>(pthread_self());
#endif
}

}  // namespace osp

#endif  // OSP_THREAD_HPP_
