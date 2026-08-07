/**
 * @file rtthread.h
 * @brief Host test stub for the RT-Thread kernel API used by osp headers.
 * pthread-backed shim so OSP_PLATFORM_RTTHREAD branches compile and run.
 * Covers only the symbols include/osp actually calls.
 */

#ifndef OSP_TEST_RTTHREAD_STUB_H_
#define OSP_TEST_RTTHREAD_STUB_H_

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

// ============================================================================
// Types
// ============================================================================

typedef uint8_t rt_uint8_t;
typedef uint32_t rt_uint32_t;
typedef int32_t rt_int32_t;
typedef uint32_t rt_tick_t;
typedef int rt_err_t;
typedef long rt_size_t;

typedef struct rt_mutex_impl* rt_mutex_t;
typedef struct rt_semaphore_impl* rt_sem_t;
typedef struct rt_thread_impl* rt_thread_t;

// ============================================================================
// Constants
// ============================================================================

#define RT_EOK 0
#define RT_ETIMEOUT 7
#define RT_ENOMEM 8
#define RT_EINVAL 10
#define RT_WAITING_FOREVER 0xFFFFFFFF
#define RT_WAITING_NO 0
#define RT_IPC_FLAG_PRIO 0x01
#define RT_THREAD_PRIORITY_MAX 256
#define RT_TICK_PER_SECOND 1000

// ============================================================================
// Mutex (recursive, matching RT-Thread reentrant rt_mutex)
// ============================================================================

struct rt_mutex_impl {
  pthread_mutex_t handle;
};

inline rt_mutex_t rt_mutex_create(const char*, rt_uint8_t) {
  rt_mutex_t m = static_cast<rt_mutex_t>(std::malloc(sizeof(rt_mutex_impl)));
  if (m == nullptr) {
    return nullptr;
  }
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&m->handle, &attr);
  pthread_mutexattr_destroy(&attr);
  return m;
}

inline rt_err_t rt_mutex_delete(rt_mutex_t m) {
  if (m == nullptr) {
    return -RT_EINVAL;
  }
  pthread_mutex_destroy(&m->handle);
  std::free(m);
  return RT_EOK;
}

inline rt_err_t rt_mutex_take(rt_mutex_t m, rt_int32_t time) {
  if (m == nullptr) {
    return -RT_EINVAL;
  }
  if (time == RT_WAITING_NO) {
    const int rc = pthread_mutex_trylock(&m->handle);
    return (rc == 0) ? RT_EOK : -RT_ETIMEOUT;
  }
  const int rc = pthread_mutex_lock(&m->handle);
  return (rc == 0) ? RT_EOK : -RT_ETIMEOUT;
}

inline rt_err_t rt_mutex_release(rt_mutex_t m) {
  if (m == nullptr) {
    return -RT_EINVAL;
  }
  pthread_mutex_unlock(&m->handle);
  return RT_EOK;
}

// ============================================================================
// Semaphore (counting, condvar-backed)
// ============================================================================

struct rt_semaphore_impl {
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  uint32_t count;
};

inline rt_sem_t rt_sem_create(const char*, rt_uint32_t value, rt_uint8_t) {
  rt_sem_t s = static_cast<rt_sem_t>(std::malloc(sizeof(rt_semaphore_impl)));
  if (s == nullptr) {
    return nullptr;
  }
  pthread_mutex_init(&s->mtx, nullptr);
  pthread_cond_init(&s->cond, nullptr);
  s->count = value;
  return s;
}

inline rt_err_t rt_sem_delete(rt_sem_t s) {
  if (s == nullptr) {
    return -RT_EINVAL;
  }
  pthread_mutex_destroy(&s->mtx);
  pthread_cond_destroy(&s->cond);
  std::free(s);
  return RT_EOK;
}

inline rt_err_t rt_sem_take(rt_sem_t s, rt_int32_t time) {
  if (s == nullptr) {
    return -RT_EINVAL;
  }
  pthread_mutex_lock(&s->mtx);
  if (time == RT_WAITING_NO) {
    const int acquired = (s->count > 0U) ? RT_EOK : -RT_ETIMEOUT;
    if (acquired == RT_EOK) {
      --s->count;
    }
    pthread_mutex_unlock(&s->mtx);
    return acquired;
  }
  if (time != static_cast<rt_int32_t>(RT_WAITING_FOREVER)) {
    // Bounded wait: 1 tick == 1 ms (RT_TICK_PER_SECOND == 1000).
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += time / 1000;
    deadline.tv_nsec += static_cast<long>(time % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_sec += 1;
      deadline.tv_nsec -= 1000000000L;
    }
    while (s->count == 0U && pthread_cond_timedwait(&s->cond, &s->mtx, &deadline) != ETIMEDOUT) {}
    const int acquired = (s->count > 0U) ? RT_EOK : -RT_ETIMEOUT;
    if (acquired == RT_EOK) {
      --s->count;
    }
    pthread_mutex_unlock(&s->mtx);
    return acquired;
  }
  while (s->count == 0U) {
    (void)pthread_cond_wait(&s->cond, &s->mtx);
  }
  --s->count;
  pthread_mutex_unlock(&s->mtx);
  return RT_EOK;
}

inline rt_err_t rt_sem_release(rt_sem_t s) {
  if (s == nullptr) {
    return -RT_EINVAL;
  }
  pthread_mutex_lock(&s->mtx);
  ++s->count;
  pthread_cond_signal(&s->cond);
  pthread_mutex_unlock(&s->mtx);
  return RT_EOK;
}

// ============================================================================
// Thread
// ============================================================================

struct rt_thread_impl {
  pthread_t tid;
  void (*entry)(void*);
  void* param;
};

inline thread_local rt_thread_t tls_self = nullptr;

inline void* RtStubThreadRun(void* arg) {
  rt_thread_t t = static_cast<rt_thread_t>(arg);
  tls_self = t;
  // osp::Thread never calls rt_thread_delete (join is semaphore-based, like
  // RT-Thread idle-thread reclaim), so detach to let the OS free resources.
  (void)pthread_detach(pthread_self());
  t->entry(t->param);
  std::free(t);
  return nullptr;
}

inline rt_thread_t rt_thread_create(const char*, void (*entry)(void*), void* param, rt_uint32_t, rt_uint8_t,
                                    rt_uint32_t) {
  rt_thread_t t = static_cast<rt_thread_t>(std::malloc(sizeof(rt_thread_impl)));
  if (t == nullptr) {
    return nullptr;
  }
  t->entry = entry;
  t->param = param;
  t->tid = pthread_t{};
  return t;
}

inline rt_err_t rt_thread_startup(rt_thread_t t) {
  if (t == nullptr) {
    return -RT_EINVAL;
  }
  if (pthread_create(&t->tid, nullptr, &RtStubThreadRun, t) != 0) {
    return -RT_ENOMEM;  // Caller frees via rt_thread_delete.
  }
  return RT_EOK;
}

inline rt_err_t rt_thread_delete(rt_thread_t t) {
  if (t == nullptr) {
    return -RT_EINVAL;
  }
  std::free(t);
  return RT_EOK;
}

inline rt_thread_t rt_thread_self() {
  return tls_self;
}

// ============================================================================
// Time / yield / memory / console
// ============================================================================

inline void rt_thread_mdelay(rt_uint32_t ms) {
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(ms / 1000U);
  ts.tv_nsec = static_cast<long>(ms % 1000U) * 1000000L;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

inline void rt_thread_delay(rt_tick_t tick) {
  rt_thread_mdelay(static_cast<rt_uint32_t>(tick));
}

inline void rt_thread_yield() {
  (void)::sched_yield();
}

inline rt_tick_t rt_tick_get() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<rt_tick_t>(static_cast<uint64_t>(ts.tv_sec) * 1000U +
                                static_cast<uint64_t>(ts.tv_nsec) / 1000000U);
}

inline void* rt_malloc(rt_size_t size) {
  return std::malloc(static_cast<std::size_t>(size));
}

inline void rt_free(void* ptr) {
  std::free(ptr);
}

inline int rt_kprintf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  return n;
}

#endif  // OSP_TEST_RTTHREAD_STUB_H_
