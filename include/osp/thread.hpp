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
 * std::thread on Linux/Windows, rt_thread_create on RT-Thread. RT-Thread join()
 * uses a semaphore because the idle thread reclaims exited TCBs (no
 * rt_thread_delete). Platform actions are selected at compile time through the
 * ops policy (template parameter), keeping the public osp::Thread API intact.
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
#include <thread>

#if defined(OSP_PLATFORM_WINDOWS)
// Win32 supplies GetCurrentThreadId/SetThreadPriority/SetThreadAffinityMask,
// which std::thread does not expose. NOMINMAX keeps windows.h from defining the
// min/max macros that would break <algorithm> in headers included afterwards.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>

#if defined(OSP_PLATFORM_LINUX)
#include <sys/syscall.h>
#include <unistd.h>
#endif
#endif  // OSP_PLATFORM_WINDOWS
#endif  // OSP_PLATFORM_RTTHREAD

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
                                     ///< Windows: >0 -> THREAD_PRIORITY_TIME_CRITICAL, <0 -> THREAD_PRIORITY_IDLE,
                                     ///< 0 -> default.
#ifdef __linux__
  const cpu_set_t* cpu_set{nullptr};  ///< Optional CPU affinity mask (Linux only).
  size_t cpu_set_size{0U};
#endif
#if defined(OSP_PLATFORM_WINDOWS)
  uint64_t cpu_set{0U};  ///< Optional processor affinity bitmask (Win32 only); 0 = no affinity.
#endif
};

// ============================================================================
// Thread platform strategies (ops policy)
// ============================================================================

#if !defined(OSP_PLATFORM_RTTHREAD) && !defined(OSP_PLATFORM_WINDOWS)

/**
 * @brief POSIX thread strategy (Linux/macOS).
 * Creation via std::thread; scheduling/affinity via pthread_*.
 */
struct PosixThreadOps {
  PosixThreadOps() noexcept = default;
  PosixThreadOps(const PosixThreadOps&) = delete;
  PosixThreadOps& operator=(const PosixThreadOps&) = delete;
  PosixThreadOps(PosixThreadOps&& other) noexcept = default;
  PosixThreadOps& operator=(PosixThreadOps&& other) noexcept = default;

  template <typename F>
  bool Start(const ThreadOptions& opts, F&& fn) noexcept {
    using Decay = typename std::decay<F>::type;
    native_ = std::thread([opts, fn = Decay(std::forward<F>(fn))]() mutable {
      ApplyOptions(opts);
      fn();
    });
    return true;
  }

  void Join() noexcept {
    if (native_.joinable()) {
      native_.join();
    }
  }

  [[nodiscard]] bool Joinable() const noexcept { return native_.joinable(); }

  void Swap(PosixThreadOps& other) noexcept { native_.swap(other.native_); }

  /**
   * @brief Stable numeric id of the calling thread.
   * Linux: kernel tid (SYS_gettid); other POSIX: pthread_self(). Unique per
   * live thread.
   */
  static uintptr_t CurrentThreadId() noexcept {
#if defined(OSP_PLATFORM_LINUX)
    return static_cast<uintptr_t>(::syscall(SYS_gettid));
#else
    return static_cast<uintptr_t>(pthread_self());
#endif
  }

  static void ApplyOptions(const ThreadOptions& opts) noexcept {
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

 private:
  std::thread native_;
};

#elif defined(OSP_PLATFORM_RTTHREAD)

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

/**
 * @brief RT-Thread thread strategy.
 * Heap-owned entry block keeps the callable alive independent of the Thread
 * object's address, so moving a running Thread is safe on RT-Thread. join()
 * waits on a semaphore released by the entry trampoline.
 */
struct RtThreadOps {
  RtThreadOps() noexcept = default;
  RtThreadOps(const RtThreadOps&) = delete;
  RtThreadOps& operator=(const RtThreadOps&) = delete;

  RtThreadOps(RtThreadOps&& other) noexcept {
    rt_param_ = other.rt_param_;
    rt_handle_ = other.rt_handle_;
    other.rt_param_ = nullptr;
    other.rt_handle_ = nullptr;
  }

  RtThreadOps& operator=(RtThreadOps&& other) noexcept {
    if (this != &other) {
      rt_param_ = other.rt_param_;
      rt_handle_ = other.rt_handle_;
      other.rt_param_ = nullptr;
      other.rt_handle_ = nullptr;
    }
    return *this;
  }

  template <typename F>
  bool Start(const ThreadOptions& opts, F&& fn) noexcept {
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
    rt_thread_t handle = rt_thread_create(opts.name.c_str(), &RtThreadOps::RtEntry, param, stack,
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
  }

  void Join() noexcept {
    if (rt_param_ == nullptr) {
      return;
    }
    rt_sem_take(rt_param_->join_sem, RT_WAITING_FOREVER);
    rt_sem_delete(rt_param_->join_sem);
    rt_param_->fn.~ThreadCallable();
    rt_free(rt_param_);
    rt_param_ = nullptr;
    rt_handle_ = nullptr;  // TCB already reclaimed by the idle thread; never dereference.
  }

  [[nodiscard]] bool Joinable() const noexcept { return rt_param_ != nullptr; }

  void Swap(RtThreadOps& other) noexcept {
    std::swap(rt_param_, other.rt_param_);
    std::swap(rt_handle_, other.rt_handle_);
  }

  /** @brief Calling thread handle as a stable numeric id. */
  static uintptr_t CurrentThreadId() noexcept { return reinterpret_cast<uintptr_t>(rt_thread_self()); }

 private:
  static constexpr uint32_t kDefaultStackSize = 8192U;
  static constexpr uint32_t kDefaultTimeSlice = 10U;

  struct RtThreadParam {
    ThreadCallable fn;
    rt_sem_t join_sem{nullptr};
  };

  static void RtEntry(void* param) noexcept {
    RtThreadParam* p = static_cast<RtThreadParam*>(param);
    p->fn();
    rt_sem_release(p->join_sem);
  }

  RtThreadParam* rt_param_{nullptr};
  rt_thread_t rt_handle_{nullptr};
};

#elif defined(OSP_PLATFORM_WINDOWS)

/**
 * @brief Win32 thread strategy.
 * Thread creation is delegated entirely to std::thread; Win32 is used only for
 * the operations std::thread does not expose (thread id, priority, affinity).
 *
 * priority mapping (see ThreadOptions):
 *   priority > 0 -> THREAD_PRIORITY_TIME_CRITICAL (analogue of SCHED_FIFO)
 *   priority < 0 -> THREAD_PRIORITY_IDLE          (analogue of SCHED_IDLE)
 *   priority == 0 -> leave at THREAD_PRIORITY_NORMAL (default)
 * Affinity: opts.cpu_set, when nonzero, is passed verbatim to
 * SetThreadAffinityMask as a processor bitmask.
 */
struct Win32ThreadOps {
  Win32ThreadOps() noexcept = default;
  Win32ThreadOps(const Win32ThreadOps&) = delete;
  Win32ThreadOps& operator=(const Win32ThreadOps&) = delete;
  Win32ThreadOps(Win32ThreadOps&& other) noexcept = default;
  Win32ThreadOps& operator=(Win32ThreadOps&& other) noexcept = default;

  template <typename F>
  bool Start(const ThreadOptions& opts, F&& fn) noexcept {
    using Decay = typename std::decay<F>::type;
    native_ = std::thread([opts, fn = Decay(std::forward<F>(fn))]() mutable {
      ApplyOptions(opts);
      fn();
    });
    return true;
  }

  void Join() noexcept {
    if (native_.joinable()) {
      native_.join();
    }
  }

  [[nodiscard]] bool Joinable() const noexcept { return native_.joinable(); }

  void Swap(Win32ThreadOps& other) noexcept { native_.swap(other.native_); }

  /** @brief Stable numeric id of the calling thread (Win32 thread id). */
  static uintptr_t CurrentThreadId() noexcept { return static_cast<uintptr_t>(::GetCurrentThreadId()); }

  static void ApplyOptions(const ThreadOptions& opts) noexcept {
    if (opts.priority > 0) {
      (void)::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    } else if (opts.priority < 0) {
      (void)::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_IDLE);
    }
    if (opts.cpu_set != 0U) {
      (void)::SetThreadAffinityMask(::GetCurrentThread(), static_cast<DWORD_PTR>(opts.cpu_set));
    }
  }

 private:
  std::thread native_;
};

#endif  // platform strategy selection

// Default ops for the host platform; osp::Thread is an alias over it.
#if defined(OSP_PLATFORM_RTTHREAD)
#define OSP_DEFAULT_THREAD_OPS ::osp::RtThreadOps
#elif defined(OSP_PLATFORM_WINDOWS)
#define OSP_DEFAULT_THREAD_OPS ::osp::Win32ThreadOps
#else
#define OSP_DEFAULT_THREAD_OPS ::osp::PosixThreadOps
#endif

// ============================================================================
// Thread
// ============================================================================

/**
 * @brief Portable thread: std::thread subset plus name/stack/priority options.
 * Join a joinable Thread before destruction, as with std::thread. Debug builds
 * assert this in the destructor; release builds leave it undefined behavior.
 *
 * @tparam Ops Platform strategy (PosixThreadOps / RtThreadOps / Win32ThreadOps).
 */
template <typename Ops>
class ThreadT {
 public:
  ThreadT() noexcept = default;
  ThreadT(const ThreadT&) = delete;
  ThreadT& operator=(const ThreadT&) = delete;
  ThreadT(ThreadT&& other) noexcept : ops_(std::move(other.ops_)) {}

  ThreadT& operator=(ThreadT&& other) noexcept {
    if (this != &other) {
      OSP_ASSERT(!joinable());
      ops_ = std::move(other.ops_);
    }
    return *this;
  }

  ~ThreadT() noexcept { OSP_ASSERT(!joinable()); }

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
  void join() noexcept { ops_.Join(); }

  /** @brief True while a started thread has not been joined yet. */
  [[nodiscard]] bool joinable() const noexcept { return ops_.Joinable(); }

  void swap(ThreadT& other) noexcept { ops_.Swap(other.ops_); }

  /**
   * @brief Stable numeric id of the calling thread.
   * Linux: kernel tid (SYS_gettid); RT-Thread: thread handle; Windows: Win32
   * thread id. Unique per live thread.
   */
  static uintptr_t CurrentThreadId() noexcept { return Ops::CurrentThreadId(); }

 private:
  template <typename F>
  bool StartImpl(const ThreadOptions& opts, F&& fn) noexcept {
    if (joinable()) {
      return false;
    }
    using Decay = typename std::decay<F>::type;
    static_assert(sizeof(Decay) <= kThreadCallableBufSize, "Thread callable exceeds SBO buffer size");
    return ops_.Start(opts, std::forward<F>(fn));
  }

  Ops ops_;
};

/// Public thread type: name and API preserved, backed by the platform ops.
using Thread = ThreadT<OSP_DEFAULT_THREAD_OPS>;

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

}  // namespace osp

#endif  // OSP_THREAD_HPP_
