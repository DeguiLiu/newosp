// Compile-time compatibility check: newosp's RT-Thread layer vs real RT-Thread
// headers. Compiled with -c only (never linked/run); instantiation of every
// RT-Thread-facing method catches API/signature drift. Intended to run from
// .ai/check-rtthread-compat.sh against a real RT-Thread source tree.
//
// Example:
//   g++ -std=c++17 -c .ai/rtthread_compat_check.cpp
//       -I <rt-thread>/include -I <dir-with-rtconfig.h>
//       -I include -DOSP_PLATFORM_RTTHREAD=1 -DOSP_NET_BACKEND=2

#include "osp/semaphore.hpp"
#include "osp/thread.hpp"

#include <cstdint>

namespace {

void ExercisePlatform() noexcept {
  (void)osp::SteadyNowUs();
  (void)osp::SteadyNowNs();
  (void)osp::CoarseNowUs();
  (void)osp::CoarseNowNs();
  osp::ThreadSleepUs(1000U);
  osp::ThreadYield();
  osp::CpuRelax();
}

void ExerciseSemaphore() noexcept {
  osp::RtSemaphore sem(0U);
  sem.Signal();
  sem.Post();
  (void)sem.TryWait();
  (void)sem.WaitFor(1000U);
  (void)sem.Count();
  (void)sem.IsValid();

  osp::Semaphore alias(1U);
  alias.Signal();
  (void)alias.TryWait();
  (void)alias.WaitFor(1U);
}

void ExerciseMutex() noexcept {
  osp::Mutex m;
  m.lock();
  (void)m.try_lock();
  m.unlock();
}

struct Loop {
  void Run() noexcept { osp::ThreadYield(); }
};

void ExerciseThread() noexcept {
  osp::Thread t;
  (void)t.Start([]() { osp::ThreadYield(); });

  osp::ThreadOptions opts;
  opts.name = "wk";
  opts.stack_size = 4096U;
  opts.priority = 5;

  osp::Thread t2;
  (void)t2.Start(opts, []() { osp::ThreadSleepUs(1000U); });

  osp::Thread t3;
  Loop loop;
  (void)t3.Start(opts, &Loop::Run, &loop);

  (void)t.joinable();
  (void)osp::Thread::CurrentThreadId();
  t2.swap(t);
  if (t.joinable()) {
    t.join();
  }
}

}  // namespace

int main() {
  ExercisePlatform();
  ExerciseSemaphore();
  ExerciseMutex();
  ExerciseThread();
  return 0;
}
