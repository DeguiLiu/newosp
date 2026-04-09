/**
 * @file test_mem_pool.cpp
 * @brief Tests for mem_pool.hpp
 */

#include "osp/mem_pool.hpp"

#include <cstring>

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <thread>

TEST_CASE("FixedPool basic alloc/free", "[mem_pool]") {
  osp::FixedPool<32, 4> pool;
  REQUIRE(pool.FreeCount() == 4);
  REQUIRE(pool.UsedCount() == 0);
  REQUIRE(pool.Capacity() == 4);

  void* p1 = pool.Allocate();
  REQUIRE(p1 != nullptr);
  REQUIRE(pool.UsedCount() == 1);
  REQUIRE(pool.FreeCount() == 3);

  pool.Free(p1);
  REQUIRE(pool.UsedCount() == 0);
  REQUIRE(pool.FreeCount() == 4);
}

TEST_CASE("FixedPool exhaust and recover", "[mem_pool]") {
  osp::FixedPool<16, 2> pool;

  void* p1 = pool.Allocate();
  void* p2 = pool.Allocate();
  REQUIRE(p1 != nullptr);
  REQUIRE(p2 != nullptr);

  void* p3 = pool.Allocate();
  REQUIRE(p3 == nullptr);  // exhausted

  pool.Free(p1);
  void* p4 = pool.Allocate();
  REQUIRE(p4 != nullptr);  // recovered
}

TEST_CASE("FixedPool AllocateChecked", "[mem_pool]") {
  osp::FixedPool<16, 1> pool;

  auto r1 = pool.AllocateChecked();
  REQUIRE(r1.has_value());
  REQUIRE(r1.value() != nullptr);

  auto r2 = pool.AllocateChecked();
  REQUIRE(!r2.has_value());
  REQUIRE(r2.get_error() == osp::MemPoolError::kPoolExhausted);

  pool.Free(r1.value());
}

TEST_CASE("FixedPool OwnsPointer", "[mem_pool]") {
  osp::FixedPool<32, 4> pool;
  void* p = pool.Allocate();
  REQUIRE(pool.OwnsPointer(p));

  int stack_var = 0;
  REQUIRE(!pool.OwnsPointer(&stack_var));

  pool.Free(p);
}

TEST_CASE("FixedPool static constexpr queries", "[mem_pool]") {
  REQUIRE(osp::FixedPool<64, 8>::Capacity() == 8);
  REQUIRE(osp::FixedPool<64, 8>::BlockSizeValue() == 64);
  REQUIRE(osp::FixedPool<64, 8>::AlignedBlockSize() >= 64);
}

TEST_CASE("ObjectPool Create and Destroy", "[mem_pool]") {
  struct Widget {
    int x;
    int y;
    Widget(int a, int b) : x(a), y(b) {}
  };

  osp::ObjectPool<Widget, 4> pool;
  Widget* w = pool.Create(10, 20);
  REQUIRE(w != nullptr);
  REQUIRE(w->x == 10);
  REQUIRE(w->y == 20);
  REQUIRE(pool.UsedCount() == 1);

  pool.Destroy(w);
  REQUIRE(pool.UsedCount() == 0);
}

TEST_CASE("ObjectPool CreateChecked", "[mem_pool]") {
  osp::ObjectPool<int, 1> pool;
  auto r1 = pool.CreateChecked(42);
  REQUIRE(r1.has_value());
  REQUIRE(*r1.value() == 42);

  auto r2 = pool.CreateChecked(99);
  REQUIRE(!r2.has_value());
  REQUIRE(r2.get_error() == osp::MemPoolError::kPoolExhausted);

  pool.Destroy(r1.value());
}

TEST_CASE("ObjectPool with non-trivial type", "[mem_pool]") {
  osp::ObjectPool<std::string, 4> pool;
  auto* s = pool.Create(std::string("hello world"));
  REQUIRE(s != nullptr);
  REQUIRE(*s == "hello world");

  pool.Destroy(s);
  REQUIRE(pool.UsedCount() == 0);
}

TEST_CASE("ObjectPool exhaust", "[mem_pool]") {
  osp::ObjectPool<int, 2> pool;
  int* a = pool.Create(1);
  int* b = pool.Create(2);
  int* c = pool.Create(3);

  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  REQUIRE(c == nullptr);

  pool.Destroy(a);
  pool.Destroy(b);
}

TEST_CASE("ObjectPool OwnsPointer", "[mem_pool]") {
  osp::ObjectPool<int, 4> pool;
  int* p = pool.Create(42);
  REQUIRE(pool.OwnsPointer(p));

  int stack_var = 0;
  REQUIRE(!pool.OwnsPointer(&stack_var));

  pool.Destroy(p);
}

TEST_CASE("ObjectPool concurrent Create/Destroy", "[mem_pool]") {
  struct CounterWidget {
    explicit CounterWidget(std::atomic<uint32_t>* live_counter_in) : live_counter(live_counter_in) {
      live_counter->fetch_add(1U, std::memory_order_relaxed);
    }

    ~CounterWidget() { live_counter->fetch_sub(1U, std::memory_order_relaxed); }

    std::atomic<uint32_t>* live_counter;
  };

  static constexpr uint32_t kPoolCapacity = 16U;
  static constexpr uint32_t kThreadCount = 4U;
  static constexpr uint32_t kIterationsPerThread = 2000U;

  osp::ObjectPool<CounterWidget, kPoolCapacity> pool;
  std::atomic<uint32_t> live_counter(0U);
  std::atomic<uint32_t> success_count(0U);
  std::array<std::thread, kThreadCount> workers;

  for (uint32_t i = 0U; i < kThreadCount; ++i) {
    workers[i] = std::thread([&pool, &live_counter, &success_count]() {
      for (uint32_t iter = 0U; iter < kIterationsPerThread; ++iter) {
        CounterWidget* obj = pool.Create(&live_counter);
        if (obj != nullptr) {
          success_count.fetch_add(1U, std::memory_order_relaxed);
          pool.Destroy(obj);
        }
      }
    });
  }

  for (uint32_t i = 0U; i < kThreadCount; ++i) {
    workers[i].join();
  }

  REQUIRE(live_counter.load(std::memory_order_relaxed) == 0U);
  REQUIRE(pool.UsedCount() == 0U);
  REQUIRE(pool.FreeCount() == kPoolCapacity);
  REQUIRE(success_count.load(std::memory_order_relaxed) > 0U);
}
