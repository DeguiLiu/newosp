/**
 * @file test_spsc_ringbuffer.cpp
 * @brief Catch2 tests for osp::SpscRingbuffer.
 *
 * Covers: push/pop, batch ops, query API, callbacks, boundary conditions,
 * FakeTSO mode, custom index types, and concurrent SPSC correctness.
 */

#include <catch2/catch_test_macros.hpp>

#include "osp/spsc_ringbuffer.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "osp/bus.hpp"

// ============================================================================
// Basic Push / Pop
// ============================================================================

TEST_CASE("SpscRingbuffer: push and pop single element", "[spsc]") {
  osp::SpscRingbuffer<int, 8> rb;
  REQUIRE(rb.Push(42));

  int val = 0;
  REQUIRE(rb.Pop(val));
  REQUIRE(val == 42);
}

TEST_CASE("SpscRingbuffer: pop from empty returns false", "[spsc]") {
  osp::SpscRingbuffer<int, 4> rb;
  int val = 0;
  REQUIRE_FALSE(rb.Pop(val));
}

TEST_CASE("SpscRingbuffer: push to full returns false", "[spsc]") {
  osp::SpscRingbuffer<int, 4> rb;
  for (int i = 0; i < 4; ++i) {
    REQUIRE(rb.Push(i));
  }
  REQUIRE_FALSE(rb.Push(99));
}

TEST_CASE("SpscRingbuffer: FIFO order preserved", "[spsc]") {
  osp::SpscRingbuffer<int, 8> rb;
  for (int i = 0; i < 8; ++i) {
    REQUIRE(rb.Push(i * 10));
  }

  for (int i = 0; i < 8; ++i) {
    int val = -1;
    REQUIRE(rb.Pop(val));
    REQUIRE(val == i * 10);
  }
}

TEST_CASE("SpscRingbuffer: interleaved push/pop", "[spsc]") {
  osp::SpscRingbuffer<int, 4> rb;

  for (int round = 0; round < 100; ++round) {
    REQUIRE(rb.Push(round));
    int val = -1;
    REQUIRE(rb.Pop(val));
    REQUIRE(val == round);
  }
}

TEST_CASE("SpscRingbuffer: fill-drain-refill cycles", "[spsc]") {
  osp::SpscRingbuffer<int, 8> rb;

  for (int cycle = 0; cycle < 3; ++cycle) {
    for (int i = 0; i < 8; ++i) {
      REQUIRE(rb.Push(cycle * 100 + i));
    }
    REQUIRE(rb.IsFull());

    for (int i = 0; i < 8; ++i) {
      int val = -1;
      REQUIRE(rb.Pop(val));
      REQUIRE(val == cycle * 100 + i);
    }
    REQUIRE(rb.IsEmpty());
  }
}

// ============================================================================
// Batch Operations
// ============================================================================

TEST_CASE("SpscRingbuffer: PushBatch and PopBatch", "[spsc][batch]") {
  osp::SpscRingbuffer<int, 16> rb;
  int src[8] = {10, 20, 30, 40, 50, 60, 70, 80};

  REQUIRE(rb.PushBatch(src, 8) == 8);
  REQUIRE(rb.Size() == 8);

  int dst[8] = {};
  REQUIRE(rb.PopBatch(dst, 8) == 8);
  for (int i = 0; i < 8; ++i) {
    REQUIRE(dst[i] == (i + 1) * 10);
  }
}

TEST_CASE("SpscRingbuffer: PushBatch exceeding capacity", "[spsc][batch]") {
  osp::SpscRingbuffer<int, 16> rb;
  int src[24];
  for (int i = 0; i < 24; ++i) {
    src[i] = i;
  }

  size_t pushed = rb.PushBatch(src, 24);
  REQUIRE(pushed == 16);
  REQUIRE(rb.IsFull());
}

TEST_CASE("SpscRingbuffer: batch wrap-around correctness", "[spsc][batch]") {
  osp::SpscRingbuffer<int, 16> rb;

  // Fill 12, drain 12 to advance indices
  int tmp[12];
  for (int i = 0; i < 12; ++i) {
    tmp[i] = i;
  }
  REQUIRE(rb.PushBatch(tmp, 12) == 12);
  int discard[12];
  REQUIRE(rb.PopBatch(discard, 12) == 12);

  // Now head/tail are at 12. Push 10 elements that wrap around boundary
  int src[10];
  for (int i = 0; i < 10; ++i) {
    src[i] = 100 + i;
  }
  REQUIRE(rb.PushBatch(src, 10) == 10);

  int dst[10] = {};
  REQUIRE(rb.PopBatch(dst, 10) == 10);
  for (int i = 0; i < 10; ++i) {
    REQUIRE(dst[i] == 100 + i);
  }
}

// ============================================================================
// Query API
// ============================================================================

TEST_CASE("SpscRingbuffer: Size/Available/IsEmpty/IsFull", "[spsc][query]") {
  osp::SpscRingbuffer<int, 8> rb;

  REQUIRE(rb.IsEmpty());
  REQUIRE_FALSE(rb.IsFull());
  REQUIRE(rb.Size() == 0);
  REQUIRE(rb.Available() == 8);
  REQUIRE(rb.Capacity() == 8);

  for (int i = 0; i < 8; ++i) {
    rb.Push(i);
  }

  REQUIRE_FALSE(rb.IsEmpty());
  REQUIRE(rb.IsFull());
  REQUIRE(rb.Size() == 8);
  REQUIRE(rb.Available() == 0);
}

TEST_CASE("SpscRingbuffer: Peek does not consume", "[spsc][query]") {
  osp::SpscRingbuffer<int, 4> rb;
  REQUIRE(rb.Peek() == nullptr);

  rb.Push(77);
  int* p = rb.Peek();
  REQUIRE(p != nullptr);
  REQUIRE(*p == 77);
  REQUIRE(rb.Size() == 1);  // still there
}

TEST_CASE("SpscRingbuffer: At with bounds checking", "[spsc][query]") {
  osp::SpscRingbuffer<int, 8> rb;
  for (int i = 0; i < 5; ++i) {
    rb.Push(i * 10);
  }

  REQUIRE(*rb.At(0) == 0);
  REQUIRE(*rb.At(2) == 20);
  REQUIRE(*rb.At(4) == 40);
  REQUIRE(rb.At(5) == nullptr);  // out of range
  REQUIRE(rb.At(100) == nullptr);
}

TEST_CASE("SpscRingbuffer: operator[] unchecked access", "[spsc][query]") {
  osp::SpscRingbuffer<int, 8> rb;
  for (int i = 0; i < 4; ++i) {
    rb.Push(i + 1);
  }
  REQUIRE(rb[0] == 1);
  REQUIRE(rb[3] == 4);
}

TEST_CASE("SpscRingbuffer: Discard elements", "[spsc][query]") {
  osp::SpscRingbuffer<int, 8> rb;
  for (int i = 0; i < 6; ++i) {
    rb.Push(i);
  }

  REQUIRE(rb.Discard(3) == 3);
  REQUIRE(rb.Size() == 3);

  int val = -1;
  rb.Pop(val);
  REQUIRE(val == 3);  // first 3 discarded, next is 3
}

TEST_CASE("SpscRingbuffer: Discard more than available", "[spsc][query]") {
  osp::SpscRingbuffer<int, 4> rb;
  rb.Push(1);
  rb.Push(2);

  REQUIRE(rb.Discard(10) == 2);
  REQUIRE(rb.IsEmpty());
}

TEST_CASE("SpscRingbuffer: ProducerClear and ConsumerClear", "[spsc][query]") {
  osp::SpscRingbuffer<int, 8> rb;

  // Producer clear
  for (int i = 0; i < 4; ++i) {
    rb.Push(i);
  }
  rb.ProducerClear();
  REQUIRE(rb.IsEmpty());

  // Consumer clear
  for (int i = 0; i < 4; ++i) {
    rb.Push(i);
  }
  rb.ConsumerClear();
  REQUIRE(rb.IsEmpty());
}

// ============================================================================
// PushFromCallback
// ============================================================================

TEST_CASE("SpscRingbuffer: PushFromCallback", "[spsc][callback]") {
  osp::SpscRingbuffer<int, 4> rb;

  int counter = 0;
  REQUIRE(rb.PushFromCallback([&counter]() { return ++counter * 10; }));
  REQUIRE(rb.PushFromCallback([&counter]() { return ++counter * 10; }));

  int val = -1;
  rb.Pop(val);
  REQUIRE(val == 10);
  rb.Pop(val);
  REQUIRE(val == 20);
}

TEST_CASE("SpscRingbuffer: PushFromCallback skips when full", "[spsc][callback]") {
  osp::SpscRingbuffer<int, 2> rb;
  rb.Push(1);
  rb.Push(2);

  bool called = false;
  REQUIRE_FALSE(rb.PushFromCallback([&called]() {
    called = true;
    return 99;
  }));
  REQUIRE_FALSE(called);
}

// ============================================================================
// Boundary / Template Variants
// ============================================================================

TEST_CASE("SpscRingbuffer: minimum size buffer (2)", "[spsc][boundary]") {
  osp::SpscRingbuffer<int, 2> rb;
  REQUIRE(rb.Push(1));
  REQUIRE(rb.Push(2));
  REQUIRE_FALSE(rb.Push(3));

  int v = 0;
  REQUIRE(rb.Pop(v));
  REQUIRE(v == 1);
  REQUIRE(rb.Pop(v));
  REQUIRE(v == 2);
}

TEST_CASE("SpscRingbuffer: large buffer (4096)", "[spsc][boundary]") {
  osp::SpscRingbuffer<uint32_t, 4096> rb;

  for (uint32_t i = 0; i < 4096U; ++i) {
    REQUIRE(rb.Push(i));
  }
  REQUIRE(rb.IsFull());

  for (uint32_t i = 0; i < 4096U; ++i) {
    uint32_t v = 0;
    REQUIRE(rb.Pop(v));
    REQUIRE(v == i);
  }
}

TEST_CASE("SpscRingbuffer: custom IndexT uint16_t", "[spsc][boundary]") {
  osp::SpscRingbuffer<int, 64, false, uint16_t> rb;

  for (int i = 0; i < 64; ++i) {
    REQUIRE(rb.Push(i));
  }
  REQUIRE(rb.IsFull());

  for (int i = 0; i < 64; ++i) {
    int v = -1;
    REQUIRE(rb.Pop(v));
    REQUIRE(v == i);
  }
}

TEST_CASE("SpscRingbuffer: uint8_t index wrap stress test", "[spsc][boundary]") {
  // uint8_t max = 255, buffer size = 4. Index wraps around at 256.
  osp::SpscRingbuffer<int, 4, false, uint8_t> rb;

  for (int round = 0; round < 1000; ++round) {
    REQUIRE(rb.Push(round));
    int v = -1;
    REQUIRE(rb.Pop(v));
    REQUIRE(v == round);
  }
}

TEST_CASE("SpscRingbuffer: FakeTSO mode (relaxed ordering)", "[spsc][boundary]") {
  osp::SpscRingbuffer<int, 8, true> rb;

  for (int i = 0; i < 8; ++i) {
    REQUIRE(rb.Push(i * 3));
  }
  for (int i = 0; i < 8; ++i) {
    int v = -1;
    REQUIRE(rb.Pop(v));
    REQUIRE(v == i * 3);
  }
}

TEST_CASE("SpscRingbuffer: struct element type", "[spsc][boundary]") {
  struct Packet {
    uint32_t id;
    uint16_t len;
    uint8_t data[6];
  };
  static_assert(std::is_trivially_copyable<Packet>::value, "");

  osp::SpscRingbuffer<Packet, 8> rb;
  Packet p{};
  p.id = 42;
  p.len = 3;
  p.data[0] = 0xAA;
  p.data[1] = 0xBB;
  p.data[2] = 0xCC;

  REQUIRE(rb.Push(p));

  Packet out{};
  REQUIRE(rb.Pop(out));
  REQUIRE(out.id == 42);
  REQUIRE(out.len == 3);
  REQUIRE(out.data[0] == 0xAA);
  REQUIRE(out.data[2] == 0xCC);
}

// ============================================================================
// Concurrent SPSC Correctness
// ============================================================================

TEST_CASE("SpscRingbuffer: concurrent SPSC 1M elements", "[spsc][concurrent]") {
  constexpr size_t kCount = 1000000;
  osp::SpscRingbuffer<int, 1024> rb;

  std::atomic<bool> consumer_ok{true};

  std::thread producer([&rb, kCount]() {
    for (size_t i = 0; i < kCount; ++i) {
      while (!rb.Push(static_cast<int>(i))) {
        std::this_thread::yield();
      }
    }
  });

  std::thread consumer([&rb, &consumer_ok, kCount]() {
    for (size_t i = 0; i < kCount; ++i) {
      int val = -1;
      while (!rb.Pop(val)) {
        std::this_thread::yield();
      }
      if (val != static_cast<int>(i)) {
        consumer_ok.store(false, std::memory_order_relaxed);
        return;
      }
    }
  });

  producer.join();
  consumer.join();

  REQUIRE(consumer_ok.load());
  REQUIRE(rb.IsEmpty());
}

TEST_CASE("SpscRingbuffer: concurrent batch SPSC", "[spsc][concurrent]") {
  constexpr size_t kBatchCount = 100000;
  constexpr size_t kMaxBatch = 64;
  osp::SpscRingbuffer<uint32_t, 1024> rb;

  std::atomic<bool> ok{true};

  std::thread producer([&rb, kBatchCount, kMaxBatch]() {
    uint32_t seq = 0;
    uint32_t batch[kMaxBatch];
    for (size_t b = 0; b < kBatchCount; ++b) {
      size_t n = (b % kMaxBatch) + 1;
      for (size_t i = 0; i < n; ++i) {
        batch[i] = seq++;
      }
      size_t pushed = 0;
      while (pushed < n) {
        size_t r = rb.PushBatch(batch + pushed, n - pushed);
        pushed += r;
        if (r == 0) {
          std::this_thread::yield();
        }
      }
    }
  });

  std::thread consumer([&rb, &ok, kBatchCount, kMaxBatch]() {
    uint32_t expected = 0;
    uint32_t batch[kMaxBatch];
    for (size_t b = 0; b < kBatchCount; ++b) {
      size_t n = (b % kMaxBatch) + 1;
      size_t popped = 0;
      while (popped < n) {
        size_t r = rb.PopBatch(batch + popped, n - popped);
        popped += r;
        if (r == 0) {
          std::this_thread::yield();
        }
      }
      for (size_t i = 0; i < n; ++i) {
        if (batch[i] != expected++) {
          ok.store(false, std::memory_order_relaxed);
          return;
        }
      }
    }
  });

  producer.join();
  consumer.join();

  REQUIRE(ok.load());
  REQUIRE(rb.IsEmpty());
}

// ============================================================================
// Non-trivially Copyable Type Support
// ============================================================================

TEST_CASE("SpscRingbuffer: non-trivially copyable std::variant Push/Pop",
          "[spsc][nontrivial]") {
  using Var = std::variant<int, double, std::string>;
  static_assert(!std::is_trivially_copyable<Var>::value,
                "std::variant must not be trivially copyable for this test");

  osp::SpscRingbuffer<Var, 8> rb;

  // Push different variant alternatives
  REQUIRE(rb.Push(Var{42}));
  REQUIRE(rb.Push(Var{3.14}));
  REQUIRE(rb.Push(Var{std::string("hello")}));

  // Pop and verify correct type + value
  Var out;

  REQUIRE(rb.Pop(out));
  REQUIRE(std::holds_alternative<int>(out));
  REQUIRE(std::get<int>(out) == 42);

  REQUIRE(rb.Pop(out));
  REQUIRE(std::holds_alternative<double>(out));
  REQUIRE(std::get<double>(out) == 3.14);

  REQUIRE(rb.Pop(out));
  REQUIRE(std::holds_alternative<std::string>(out));
  REQUIRE(std::get<std::string>(out) == "hello");

  REQUIRE(rb.IsEmpty());
}

TEST_CASE("SpscRingbuffer: non-trivially copyable PushBatch/PopBatch",
          "[spsc][nontrivial]") {
  using Var = std::variant<int, std::string>;
  static_assert(!std::is_trivially_copyable<Var>::value,
                "std::variant<int, string> must not be trivially copyable");

  osp::SpscRingbuffer<Var, 8> rb;

  Var src[6] = {Var{10}, Var{std::string("a")}, Var{20},
                Var{std::string("b")}, Var{30}, Var{std::string("c")}};
  REQUIRE(rb.PushBatch(src, 6) == 6);
  REQUIRE(rb.Size() == 6);

  Var dst[6];
  REQUIRE(rb.PopBatch(dst, 6) == 6);

  REQUIRE(std::holds_alternative<int>(dst[0]));
  REQUIRE(std::get<int>(dst[0]) == 10);

  REQUIRE(std::holds_alternative<std::string>(dst[1]));
  REQUIRE(std::get<std::string>(dst[1]) == "a");

  REQUIRE(std::holds_alternative<int>(dst[2]));
  REQUIRE(std::get<int>(dst[2]) == 20);

  REQUIRE(std::holds_alternative<std::string>(dst[3]));
  REQUIRE(std::get<std::string>(dst[3]) == "b");

  REQUIRE(std::holds_alternative<int>(dst[4]));
  REQUIRE(std::get<int>(dst[4]) == 30);

  REQUIRE(std::holds_alternative<std::string>(dst[5]));
  REQUIRE(std::get<std::string>(dst[5]) == "c");

  REQUIRE(rb.IsEmpty());
}

TEST_CASE("SpscRingbuffer: non-trivially copyable concurrent SPSC",
          "[spsc][nontrivial][concurrent]") {
  using Var = std::variant<int, double>;
  constexpr size_t kCount = 100000;
  osp::SpscRingbuffer<Var, 256> rb;

  std::atomic<bool> consumer_ok{true};

  std::thread producer([&rb]() {
    for (size_t i = 0; i < kCount; ++i) {
      Var v;
      if (i % 2 == 0) {
        v = static_cast<int>(i);
      } else {
        v = static_cast<double>(i) * 0.5;
      }
      while (!rb.Push(std::move(v))) {
        std::this_thread::yield();
      }
    }
  });

  std::thread consumer([&rb, &consumer_ok]() {
    for (size_t i = 0; i < kCount; ++i) {
      Var val;
      while (!rb.Pop(val)) {
        std::this_thread::yield();
      }
      if (i % 2 == 0) {
        if (!std::holds_alternative<int>(val) ||
            std::get<int>(val) != static_cast<int>(i)) {
          consumer_ok.store(false, std::memory_order_relaxed);
          return;
        }
      } else {
        if (!std::holds_alternative<double>(val) ||
            std::get<double>(val) != static_cast<double>(i) * 0.5) {
          consumer_ok.store(false, std::memory_order_relaxed);
          return;
        }
      }
    }
  });

  producer.join();
  consumer.join();

  REQUIRE(consumer_ok.load());
  REQUIRE(rb.IsEmpty());
}

TEST_CASE("SpscRingbuffer: non-trivially copyable move semantics verification",
          "[spsc][nontrivial]") {
  struct MoveTracker {
    int val;
    int move_count;

    MoveTracker() noexcept : val(0), move_count(0) {}
    explicit MoveTracker(int v) noexcept : val(v), move_count(0) {}

    MoveTracker(const MoveTracker& o) noexcept
        : val(o.val), move_count(o.move_count) {}
    MoveTracker& operator=(const MoveTracker& o) noexcept {
      val = o.val;
      move_count = o.move_count;
      return *this;
    }

    MoveTracker(MoveTracker&& o) noexcept
        : val(o.val), move_count(o.move_count + 1) {
      o.val = -1;
    }
    MoveTracker& operator=(MoveTracker&& o) noexcept {
      val = o.val;
      move_count = o.move_count + 1;
      o.val = -1;
      return *this;
    }
  };

  static_assert(!std::is_trivially_copyable<MoveTracker>::value,
                "MoveTracker must not be trivially copyable");

  osp::SpscRingbuffer<MoveTracker, 4> rb;

  // Push by move: one move-assignment into the buffer slot
  MoveTracker src(42);
  REQUIRE(src.move_count == 0);
  REQUIRE(rb.Push(std::move(src)));
  REQUIRE(src.val == -1);  // source invalidated

  // Pop by move: one move-assignment out of the buffer slot
  MoveTracker dst;
  REQUIRE(rb.Pop(dst));
  REQUIRE(dst.val == 42);
  // Move count: 1 (Push move-assign into buffer) + 1 (Pop move-assign out) = 2
  REQUIRE(dst.move_count == 2);
}

TEST_CASE("SpscRingbuffer: MessageEnvelope compatibility",
          "[spsc][nontrivial]") {
  using Payload = std::variant<int, float, std::string>;
  using Envelope = osp::MessageEnvelope<Payload>;
  static_assert(!std::is_trivially_copyable<Envelope>::value,
                "MessageEnvelope<variant<...,string>> must not be trivially copyable");

  osp::SpscRingbuffer<Envelope, 8> rb;

  // Build an envelope with header and payload
  osp::MessageHeader hdr(123, 456789, 1, osp::MessagePriority::kHigh);
  Envelope env(hdr, Payload{3.14f});

  REQUIRE(rb.Push(std::move(env)));

  // Push a second envelope with a string payload
  osp::MessageHeader hdr2(456, 111222, 2, osp::MessagePriority::kLow);
  Envelope env2(hdr2, Payload{std::string("test_payload")});
  REQUIRE(rb.Push(std::move(env2)));

  Envelope out;
  REQUIRE(rb.Pop(out));

  // Verify header fields preserved
  REQUIRE(out.header.msg_id == 123);
  REQUIRE(out.header.timestamp_us == 456789);
  REQUIRE(out.header.sender_id == 1);
  REQUIRE(out.header.priority == osp::MessagePriority::kHigh);

  // Verify payload preserved
  REQUIRE(std::holds_alternative<float>(out.payload));
  REQUIRE(std::get<float>(out.payload) == 3.14f);

  // Pop second envelope and verify string payload
  Envelope out2;
  REQUIRE(rb.Pop(out2));

  REQUIRE(out2.header.msg_id == 456);
  REQUIRE(out2.header.timestamp_us == 111222);
  REQUIRE(out2.header.sender_id == 2);
  REQUIRE(out2.header.priority == osp::MessagePriority::kLow);

  REQUIRE(std::holds_alternative<std::string>(out2.payload));
  REQUIRE(std::get<std::string>(out2.payload) == "test_payload");

  REQUIRE(rb.IsEmpty());
}
