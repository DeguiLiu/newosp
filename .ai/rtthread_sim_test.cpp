// Behavioral smoke test of newosp's RT-Thread layer on a REAL RT-Thread
// kernel (built as the bsp/simulator host process). This file provides main(),
// which runs in the RT-Thread main-thread context; the scheduler and board
// are bootstrapped by the kernel's entry()/rtthread_startup() before main runs.
// Linked against the simulator kernel objects -- see .ai/check-rtthread-compat.sh.

#include "osp/semaphore.hpp"
#include "osp/thread.hpp"

#include <cstdlib>

#include <rtthread.h>

static int g_failures = 0;

static void Check(bool cond, const char* what) {
  if (!cond) {
    rt_kprintf("  FAIL: %s\n", what);
    ++g_failures;
  }
}

int main(void) {
  rt_kprintf("\n=== newosp RT-Thread %d.%d.%d simulator test ===\n", RT_VERSION_MAJOR,
             RT_VERSION_MINOR, RT_VERSION_PATCH);

  {
    osp::Mutex m;
    m.lock();
    // RT-Thread 5.2.x mutex IS recursive: owner re-take increments hold
    // (src/ipc.c _rt_mutex_take, "if (mutex->owner == thread) mutex->hold++").
    bool reentrant = m.try_lock();
    if (reentrant) {
      m.unlock();  // drop the recursion level
    }
    m.unlock();
    Check(reentrant, "Mutex supports owner re-entrance");
  }

  {
    osp::RtSemaphore sem(0);
    osp::Thread t;
    bool started = t.Start([&sem]() { sem.Signal(); });
    Check(started, "Thread::Start succeeds");
    if (started) {
      bool got = sem.WaitFor(5000U);
      Check(got, "Thread signals semaphore within 5s");
      Check(sem.Count() == 0U, "Semaphore count drained");
      t.join();
      Check(!t.joinable(), "Thread joined, not joinable");
    }
  }

  {
    uintptr_t id = osp::Thread::CurrentThreadId();
    Check(id != 0U, "CurrentThreadId non-zero");
  }

  if (g_failures == 0) {
    rt_kprintf("=== newosp RT-Thread test: PASS ===\n");
    std::exit(0);
  }
  rt_kprintf("=== newosp RT-Thread test: FAIL (%d) ===\n", g_failures);
  std::exit(1);
}
