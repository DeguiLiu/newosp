/**
 * @file test_thread.cpp
 * @brief Tests for osp::Thread (cross-platform thread abstraction).
 *
 * The API mirrors std::thread naming (join()/joinable()) so migrations keep
 * call sites almost unchanged. Thread creation is a cold path; the callable is
 * stored via SBO (FixedFunction) so no heap allocation is required.
 */

#include "osp/thread.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <thread>

// ============================================================================
// Helpers
// ============================================================================

namespace {

template <typename Pred>
bool WaitUntil(Pred pred, uint64_t timeout_ms = 3000U) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!pred() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  return pred();
}

template <typename T>
bool WaitForValue(const std::atomic<T>& flag, T expected, uint64_t timeout_ms = 3000U) {
  return WaitUntil([&] { return flag.load(std::memory_order_acquire) == expected; }, timeout_ms);
}

}  // namespace

// ============================================================================
// Construction / lifecycle
// ============================================================================

TEST_CASE("Thread default construct is not joinable", "[thread]") {
  osp::Thread t;
  REQUIRE_FALSE(t.joinable());
}

TEST_CASE("Thread runs a lambda and joins", "[thread]") {
  std::atomic<bool> ran{false};
  osp::Thread t;
  REQUIRE(t.Start([&ran]() { ran.store(true, std::memory_order_release); }));
  REQUIRE(t.joinable());
  REQUIRE(WaitForValue(ran, true));
  t.join();
  REQUIRE_FALSE(t.joinable());
}

TEST_CASE("Thread join on not-joinable is a no-op", "[thread]") {
  osp::Thread t;
  t.join();  // must not crash
  REQUIRE_FALSE(t.joinable());
}

TEST_CASE("Thread Start while joinable returns false", "[thread]") {
  osp::Thread t;
  REQUIRE(t.Start([&]() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }));
  REQUIRE(t.joinable());
  REQUIRE_FALSE(t.Start([&]() {}));  // second Start rejected
  t.join();
  REQUIRE_FALSE(t.joinable());
}

TEST_CASE("Thread runs a member function pointer", "[thread]") {
  struct Runner {
    std::atomic<int>* out;
    void Loop(int x) { out->store(x, std::memory_order_release); }
  };

  std::atomic<int> result{0};
  Runner r{&result};
  osp::Thread t;
  REQUIRE(t.Start(osp::ThreadOptions{}, &Runner::Loop, &r, 42));
  REQUIRE(WaitForValue(result, 42));
  t.join();
  REQUIRE(result.load() == 42);
}

// ============================================================================
// Move semantics
// ============================================================================

TEST_CASE("Thread move construction transfers the running thread", "[thread]") {
  osp::Thread t;
  REQUIRE(t.Start([&]() { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }));
  REQUIRE(t.joinable());

  osp::Thread t2(std::move(t));
  REQUIRE_FALSE(t.joinable());
  REQUIRE(t2.joinable());
  t2.join();
  REQUIRE_FALSE(t2.joinable());
}

TEST_CASE("Thread move assignment transfers the running thread", "[thread]") {
  osp::Thread a;
  REQUIRE(a.Start([&]() { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }));
  REQUIRE(a.joinable());

  osp::Thread b;
  b = std::move(a);
  REQUIRE_FALSE(a.joinable());
  REQUIRE(b.joinable());
  b.join();
  REQUIRE_FALSE(b.joinable());
}

TEST_CASE("Thread swap exchanges handles", "[thread]") {
  osp::Thread a;
  osp::Thread b;
  REQUIRE(a.Start([&]() { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }));
  REQUIRE_FALSE(b.joinable());

  a.swap(b);
  REQUIRE_FALSE(a.joinable());
  REQUIRE(b.joinable());
  b.join();
  REQUIRE_FALSE(b.joinable());
}

// ============================================================================
// Thread-local identity / options
// ============================================================================

TEST_CASE("Thread CurrentThreadId differs from main thread", "[thread]") {
  const uintptr_t main_id = osp::Thread::CurrentThreadId();
  std::atomic<uintptr_t> worker_id{0U};
  std::atomic<bool> done{false};

  osp::Thread t;
  REQUIRE(t.Start([&worker_id, &done]() {
    worker_id.store(osp::Thread::CurrentThreadId(), std::memory_order_release);
    done.store(true, std::memory_order_release);
  }));
  REQUIRE(WaitForValue(done, true));
  t.join();
  REQUIRE(worker_id.load(std::memory_order_acquire) != main_id);
}

TEST_CASE("Thread accepts ThreadOptions and still runs", "[thread]") {
  std::atomic<bool> ran{false};
  osp::ThreadOptions opts;
  opts.name = "tst";
  opts.priority = 0;

  osp::Thread t;
  REQUIRE(t.Start(opts, [&ran]() { ran.store(true, std::memory_order_release); }));
  REQUIRE(WaitForValue(ran, true));
  t.join();
}

// ============================================================================
// SBO / fixed storage usage (worker_pool alignment)
// ============================================================================

TEST_CASE("Thread supports a moderately large capture (SBO upper bound)", "[thread]") {
  auto shared = std::make_shared<uint32_t>(7U);
  const int client_fd = 3;
  std::atomic<int> got{-1};

  // ~40B of captures: shared_ptr(16B) + Token(16B) + int(4B) + ref(8B).
  // Close to the largest capture shape used in the codebase (service async
  // handler), exercising the SBO bound of the FixedFunction entry buffer.
  struct Token {
    void* a;
    uint64_t b;
  };
  Token token{nullptr, 5U};

  osp::Thread t;
  REQUIRE(t.Start([shared, token, client_fd, &got]() {
    got.store(client_fd + static_cast<int>(*shared) + static_cast<int>(token.b), std::memory_order_release);
  }));
  REQUIRE(WaitForValue(got, 15));
  t.join();
  REQUIRE(got.load() == 15);
}

TEST_CASE("FixedVector of Thread runs multiple workers", "[thread]") {
  osp::FixedVector<osp::Thread, 4> threads;
  std::atomic<int> counter{0};

  for (int i = 0; i < 4; ++i) {
    REQUIRE(threads.emplace_back());
    REQUIRE(threads.back().Start([&counter]() { counter.fetch_add(1, std::memory_order_release); }));
  }

  REQUIRE(WaitForValue(counter, 4));
  for (auto& t : threads) {
    t.join();
  }
  REQUIRE(counter.load() == 4);
}

// ============================================================================
// Mutex
// ============================================================================

TEST_CASE("Mutex basic lock/unlock and try_lock", "[thread]") {
  osp::Mutex mtx;
  mtx.lock();
  REQUIRE_FALSE(mtx.try_lock());
  mtx.unlock();
  REQUIRE(mtx.try_lock());
  mtx.unlock();
}

TEST_CASE("Mutex works with std::lock_guard", "[thread]") {
  osp::Mutex mtx;
  int counter = 0;
  {
    std::lock_guard<osp::Mutex> lock(mtx);
    ++counter;
  }
  REQUIRE(counter == 1);
}

TEST_CASE("Mutex serializes concurrent increments", "[thread]") {
  osp::Mutex mtx;
  int counter = 0;
  osp::FixedVector<osp::Thread, 4> threads;

  for (int i = 0; i < 4; ++i) {
    REQUIRE(threads.emplace_back());
    REQUIRE(threads.back().Start([&mtx, &counter]() {
      for (int j = 0; j < 1000; ++j) {
        std::lock_guard<osp::Mutex> lock(mtx);
        ++counter;
      }
    }));
  }

  for (auto& t : threads) {
    t.join();
  }
  REQUIRE(counter == 4000);
}
