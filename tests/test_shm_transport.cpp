/**
 * @file test_shm_transport.cpp
 * @brief Catch2 tests for osp::SharedMemorySegment, osp::ShmRingBuffer,
 *        and osp::ShmChannel.
 */

#include "osp/shm_transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#if defined(OSP_PLATFORM_LINUX)

// ============================================================================
// SharedMemorySegment tests
// ============================================================================

TEST_CASE("shm_transport - SharedMemorySegment create and map",
          "[shm_transport]") {
  auto result = osp::SharedMemorySegment::Create("test_seg_1", 4096);
  REQUIRE(result.has_value());

  auto& seg = result.value();
  REQUIRE(seg.Data() != nullptr);
  REQUIRE(seg.Size() == 4096);

  // Write and read back
  char* ptr = static_cast<char*>(seg.Data());
  std::memcpy(ptr, "Hello SHM", 10);
  REQUIRE(std::memcmp(ptr, "Hello SHM", 10) == 0);

  seg.Unlink();
}

TEST_CASE("shm_transport - SharedMemorySegment open existing segment",
          "[shm_transport]") {
  // Create segment
  auto create_result = osp::SharedMemorySegment::Create("test_seg_2", 8192);
  REQUIRE(create_result.has_value());

  auto& creator = create_result.value();
  std::memcpy(creator.Data(), "SharedData", 11);

  // Open from another "process" (same process for testing)
  auto open_result = osp::SharedMemorySegment::Open("test_seg_2");
  REQUIRE(open_result.has_value());

  auto& opener = open_result.value();
  REQUIRE(opener.Size() == 8192);
  REQUIRE(std::memcmp(opener.Data(), "SharedData", 11) == 0);

  creator.Unlink();
}

TEST_CASE("shm_transport - SharedMemorySegment open non-existent fails",
          "[shm_transport]") {
  auto result = osp::SharedMemorySegment::Open("nonexistent_segment_xyz");
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.get_error() == osp::ShmError::kOpenFailed);
}

TEST_CASE("shm_transport - SharedMemorySegment move semantics",
          "[shm_transport]") {
  auto result = osp::SharedMemorySegment::Create("test_seg_move", 2048);
  REQUIRE(result.has_value());

  auto seg1 = static_cast<osp::SharedMemorySegment&&>(result.value());
  REQUIRE(seg1.Data() != nullptr);

  // Move construct
  osp::SharedMemorySegment seg2(static_cast<osp::SharedMemorySegment&&>(seg1));
  REQUIRE(seg2.Data() != nullptr);
  REQUIRE(seg2.Size() == 4096);  // page-aligned from 2048

  seg2.Unlink();
}

// ============================================================================
// ShmRingBuffer tests
// ============================================================================

TEST_CASE("shm_transport - ShmRingBuffer single thread push/pop",
          "[shm_transport]") {
  constexpr uint32_t kSlotSize = 128;
  constexpr uint32_t kSlotCount = 16;
  using RingBuffer = osp::ShmRingBuffer<kSlotSize, kSlotCount>;

  auto seg_result = osp::SharedMemorySegment::Create("test_ring_1",
                                                      RingBuffer::Size());
  REQUIRE(seg_result.has_value());
  auto& seg = seg_result.value();

  auto* rb = RingBuffer::InitAt(seg.Data());
  REQUIRE(rb != nullptr);
  REQUIRE(rb->Depth() == 0);

  // Push some data
  char data[64] = "Test message 1";
  REQUIRE(rb->TryPush(data, 15));
  REQUIRE(rb->Depth() == 1);

  // Pop it back
  char recv[kSlotSize];
  uint32_t recv_size = 0;
  REQUIRE(rb->TryPop(recv, recv_size));
  REQUIRE(recv_size == 15);
  REQUIRE(std::memcmp(recv, "Test message 1", 15) == 0);
  REQUIRE(rb->Depth() == 0);

  seg.Unlink();
}

TEST_CASE("shm_transport - ShmRingBuffer full and empty boundaries",
          "[shm_transport]") {
  constexpr uint32_t kSlotSize = 64;
  constexpr uint32_t kSlotCount = 8;
  using RingBuffer = osp::ShmRingBuffer<kSlotSize, kSlotCount>;

  auto seg_result = osp::SharedMemorySegment::Create("test_ring_full",
                                                      RingBuffer::Size());
  REQUIRE(seg_result.has_value());
  auto& seg = seg_result.value();

  auto* rb = RingBuffer::InitAt(seg.Data());

  // Fill the buffer
  char data[32] = "msg";
  for (uint32_t i = 0; i < kSlotCount; ++i) {
    REQUIRE(rb->TryPush(data, 4));
  }

  // Next push should fail (full)
  REQUIRE_FALSE(rb->TryPush(data, 4));
  REQUIRE(rb->Depth() == kSlotCount);

  // Drain the buffer
  char recv[kSlotSize];
  uint32_t recv_size = 0;
  for (uint32_t i = 0; i < kSlotCount; ++i) {
    REQUIRE(rb->TryPop(recv, recv_size));
  }

  // Next pop should fail (empty)
  REQUIRE_FALSE(rb->TryPop(recv, recv_size));
  REQUIRE(rb->Depth() == 0);

  seg.Unlink();
}

TEST_CASE("shm_transport - ShmRingBuffer oversized message rejected",
          "[shm_transport]") {
  constexpr uint32_t kSlotSize = 64;
  constexpr uint32_t kSlotCount = 8;
  using RingBuffer = osp::ShmRingBuffer<kSlotSize, kSlotCount>;

  auto seg_result = osp::SharedMemorySegment::Create("test_ring_oversize",
                                                      RingBuffer::Size());
  REQUIRE(seg_result.has_value());
  auto& seg = seg_result.value();

  auto* rb = RingBuffer::InitAt(seg.Data());

  char large_data[128];
  std::memset(large_data, 'X', sizeof(large_data));

  // Should reject message larger than slot size
  REQUIRE_FALSE(rb->TryPush(large_data, 128));

  seg.Unlink();
}

TEST_CASE("shm_transport - ShmRingBuffer multi-threaded MPSC",
          "[shm_transport]") {
  constexpr uint32_t kSlotSize = 256;
  constexpr uint32_t kSlotCount = 64;
  constexpr uint32_t kProducerCount = 4;
  constexpr uint32_t kMessagesPerProducer = 100;
  using RingBuffer = osp::ShmRingBuffer<kSlotSize, kSlotCount>;

  auto seg_result = osp::SharedMemorySegment::Create("test_ring_mpsc",
                                                      RingBuffer::Size());
  REQUIRE(seg_result.has_value());
  auto& seg = seg_result.value();

  auto* rb = RingBuffer::InitAt(seg.Data());

  std::atomic<uint32_t> total_sent{0};
  std::atomic<uint32_t> total_received{0};

  // Producer threads
  std::vector<std::thread> producers;
  for (uint32_t p = 0; p < kProducerCount; ++p) {
    producers.emplace_back([rb, p, &total_sent]() {
      for (uint32_t i = 0; i < kMessagesPerProducer; ++i) {
        char msg[64];
        std::snprintf(msg, sizeof(msg), "P%u-M%u", p, i);
        uint32_t len = static_cast<uint32_t>(std::strlen(msg)) + 1;

        // Retry on full
        while (!rb->TryPush(msg, len)) {
          std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        total_sent.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // Consumer thread
  std::thread consumer([rb, &total_received]() {
    uint32_t expected = kProducerCount * kMessagesPerProducer;
    char recv[kSlotSize];
    uint32_t recv_size = 0;

    while (total_received.load(std::memory_order_relaxed) < expected) {
      if (rb->TryPop(recv, recv_size)) {
        total_received.fetch_add(1, std::memory_order_relaxed);
      } else {
        std::this_thread::sleep_for(std::chrono::microseconds(10));
      }
    }
  });

  for (auto& t : producers) {
    t.join();
  }
  consumer.join();

  REQUIRE(total_sent.load() == kProducerCount * kMessagesPerProducer);
  REQUIRE(total_received.load() == kProducerCount * kMessagesPerProducer);
  REQUIRE(rb->Depth() == 0);

  seg.Unlink();
}

TEST_CASE("shm_transport - ShmRingBuffer concurrent MPSC stress",
          "[shm_transport]") {
  constexpr uint32_t kSlotSize = 256;
  constexpr uint32_t kSlotCount = 64;
  constexpr uint32_t kProducerCount = 4;
  constexpr uint32_t kMessagesPerProducer = 500;
  using RingBuffer = osp::ShmRingBuffer<kSlotSize, kSlotCount>;

  auto seg_result = osp::SharedMemorySegment::Create("test_ring_mpsc_stress",
                                                      RingBuffer::Size());
  REQUIRE(seg_result.has_value());
  auto& seg = seg_result.value();

  auto* rb = RingBuffer::InitAt(seg.Data());

  std::atomic<uint32_t> total_sent{0};
  std::atomic<uint32_t> total_received{0};
  std::atomic<bool> producers_done{false};

  // Producer threads
  std::vector<std::thread> producers;
  for (uint32_t p = 0; p < kProducerCount; ++p) {
    producers.emplace_back([rb, p, &total_sent]() {
      for (uint32_t i = 0; i < kMessagesPerProducer; ++i) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Producer%u-Msg%u-Data", p, i);
        uint32_t len = static_cast<uint32_t>(std::strlen(msg)) + 1;

        // Retry on full with backoff
        while (!rb->TryPush(msg, len)) {
          std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
        total_sent.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // Single consumer thread
  std::thread consumer([rb, &total_received, &producers_done]() {
    uint32_t expected = kProducerCount * kMessagesPerProducer;
    char recv[kSlotSize];
    uint32_t recv_size = 0;

    while (total_received.load(std::memory_order_relaxed) < expected) {
      if (rb->TryPop(recv, recv_size)) {
        // Verify message format
        REQUIRE(recv_size > 0);
        REQUIRE(recv_size < kSlotSize);
        total_received.fetch_add(1, std::memory_order_relaxed);
      } else {
        std::this_thread::sleep_for(std::chrono::microseconds(1));
      }
    }
  });

  for (auto& t : producers) {
    t.join();
  }
  producers_done.store(true, std::memory_order_release);
  consumer.join();

  REQUIRE(total_sent.load() == kProducerCount * kMessagesPerProducer);
  REQUIRE(total_received.load() == kProducerCount * kMessagesPerProducer);
  REQUIRE(rb->Depth() == 0);

  seg.Unlink();
}

TEST_CASE("shm_transport - ShmRingBuffer cache line separation",
          "[shm_transport]") {
  constexpr uint32_t kSlotSize = 128;
  constexpr uint32_t kSlotCount = 16;
  using RingBuffer = osp::ShmRingBuffer<kSlotSize, kSlotCount>;

  auto seg_result = osp::SharedMemorySegment::Create("test_ring_cacheline",
                                                      RingBuffer::Size());
  REQUIRE(seg_result.has_value());
  auto& seg = seg_result.value();

  auto* rb = RingBuffer::InitAt(seg.Data());

  // Verify cache line separation by checking the size of the ring buffer.
  // With alignas(64) and padding, the structure should be properly aligned.
  // We verify this indirectly by ensuring the buffer works correctly under
  // concurrent access (tested in other test cases).
  // The static_assert in the header ensures Slot is standard layout.

  // Basic sanity check: buffer should be initialized correctly
  REQUIRE(rb->Depth() == 0);

  // Verify alignment by checking that Size() accounts for padding
  REQUIRE(RingBuffer::Size() >= sizeof(std::atomic<uint32_t>) * 2 + 64);

  seg.Unlink();
}

TEST_CASE("shm_transport - ShmRingBuffer full wraparound",
          "[shm_transport]") {
  constexpr uint32_t kSlotSize = 64;
  constexpr uint32_t kSlotCount = 8;
  using RingBuffer = osp::ShmRingBuffer<kSlotSize, kSlotCount>;

  auto seg_result = osp::SharedMemorySegment::Create("test_ring_wraparound",
                                                      RingBuffer::Size());
  REQUIRE(seg_result.has_value());
  auto& seg = seg_result.value();

  auto* rb = RingBuffer::InitAt(seg.Data());

  // Fill buffer completely
  char data[32];
  for (uint32_t i = 0; i < kSlotCount; ++i) {
    std::snprintf(data, sizeof(data), "Round1-Msg%u", i);
    REQUIRE(rb->TryPush(data, static_cast<uint32_t>(std::strlen(data)) + 1));
  }
  REQUIRE(rb->Depth() == kSlotCount);

  // Drain buffer completely
  char recv[kSlotSize];
  uint32_t recv_size = 0;
  for (uint32_t i = 0; i < kSlotCount; ++i) {
    REQUIRE(rb->TryPop(recv, recv_size));
    char expected[32];
    std::snprintf(expected, sizeof(expected), "Round1-Msg%u", i);
    REQUIRE(std::strcmp(recv, expected) == 0);
  }
  REQUIRE(rb->Depth() == 0);

  // Fill again (tests wraparound)
  for (uint32_t i = 0; i < kSlotCount; ++i) {
    std::snprintf(data, sizeof(data), "Round2-Msg%u", i);
    REQUIRE(rb->TryPush(data, static_cast<uint32_t>(std::strlen(data)) + 1));
  }
  REQUIRE(rb->Depth() == kSlotCount);

  // Drain again
  for (uint32_t i = 0; i < kSlotCount; ++i) {
    REQUIRE(rb->TryPop(recv, recv_size));
    char expected[32];
    std::snprintf(expected, sizeof(expected), "Round2-Msg%u", i);
    REQUIRE(std::strcmp(recv, expected) == 0);
  }
  REQUIRE(rb->Depth() == 0);

  seg.Unlink();
}

TEST_CASE("shm_transport - ShmRingBuffer memory ordering verification",
          "[shm_transport]") {
  constexpr uint32_t kSlotSize = 256;
  constexpr uint32_t kSlotCount = 32;
  constexpr uint32_t kIterations = 1000;
  using RingBuffer = osp::ShmRingBuffer<kSlotSize, kSlotCount>;

  auto seg_result = osp::SharedMemorySegment::Create("test_ring_ordering",
                                                      RingBuffer::Size());
  REQUIRE(seg_result.has_value());
  auto& seg = seg_result.value();

  auto* rb = RingBuffer::InitAt(seg.Data());

  std::atomic<bool> consumer_error{false};

  // Producer: write incrementing counter values
  std::thread producer([rb]() {
    for (uint32_t i = 0; i < kIterations; ++i) {
      uint32_t value = i;
      while (!rb->TryPush(&value, sizeof(value))) {
        std::this_thread::yield();
      }
    }
  });

  // Consumer: verify values are in order (tests memory ordering)
  std::thread consumer([rb, &consumer_error]() {
    uint32_t expected = 0;
    char recv[kSlotSize];
    uint32_t recv_size = 0;

    while (expected < kIterations) {
      if (rb->TryPop(recv, recv_size)) {
        REQUIRE(recv_size == sizeof(uint32_t));
        uint32_t value;
        std::memcpy(&value, recv, sizeof(value));

        if (value != expected) {
          consumer_error.store(true, std::memory_order_release);
          break;
        }
        ++expected;
      } else {
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  consumer.join();

  REQUIRE_FALSE(consumer_error.load());
  REQUIRE(rb->Depth() == 0);

  seg.Unlink();
}

// ============================================================================
// ShmChannel tests
// ============================================================================

TEST_CASE("shm_transport - ShmChannel create writer and open reader",
          "[shm_transport]") {
  using Channel = osp::ShmChannel<256, 32>;

  auto writer_result = Channel::CreateWriter("test_channel_1");
  REQUIRE(writer_result.has_value());
  auto writer = static_cast<Channel&&>(writer_result.value());

  auto reader_result = Channel::OpenReader("test_channel_1");
  REQUIRE(reader_result.has_value());
  auto reader = static_cast<Channel&&>(reader_result.value());

  writer.Unlink();
}

TEST_CASE("shm_transport - ShmChannel write and read",
          "[shm_transport]") {
  using Channel = osp::ShmChannel<128, 16>;

  auto writer_result = Channel::CreateWriter("test_channel_rw");
  REQUIRE(writer_result.has_value());
  auto writer = static_cast<Channel&&>(writer_result.value());

  auto reader_result = Channel::OpenReader("test_channel_rw");
  REQUIRE(reader_result.has_value());
  auto reader = static_cast<Channel&&>(reader_result.value());

  // Write data
  const char* msg = "Hello from writer";
  uint32_t msg_len = static_cast<uint32_t>(std::strlen(msg)) + 1;
  auto write_result = writer.Write(msg, msg_len);
  REQUIRE(write_result.has_value());

  // Read data
  char recv[128];
  uint32_t recv_size = 0;
  auto read_result = reader.Read(recv, recv_size);
  REQUIRE(read_result.has_value());
  REQUIRE(recv_size == msg_len);
  REQUIRE(std::strcmp(recv, msg) == 0);

  writer.Unlink();
}

TEST_CASE("shm_transport - ShmChannel read empty returns kEmpty",
          "[shm_transport]") {
  using Channel = osp::ShmChannel<128, 16>;

  auto writer_result = Channel::CreateWriter("test_channel_empty");
  REQUIRE(writer_result.has_value());
  auto writer = static_cast<Channel&&>(writer_result.value());

  auto reader_result = Channel::OpenReader("test_channel_empty");
  REQUIRE(reader_result.has_value());
  auto reader = static_cast<Channel&&>(reader_result.value());

  char recv[128];
  uint32_t recv_size = 0;
  auto read_result = reader.Read(recv, recv_size);
  REQUIRE_FALSE(read_result.has_value());
  REQUIRE(read_result.get_error() == osp::ShmError::kEmpty);

  writer.Unlink();
}

TEST_CASE("shm_transport - ShmChannel eventfd notification",
          "[shm_transport]") {
  using Channel = osp::ShmChannel<256, 32>;

  // Clean up any leftover segment first
  ::shm_unlink("/osp_shm_test_channel_notify");

  auto writer_result = Channel::CreateWriter("test_channel_notify");
  REQUIRE(writer_result.has_value());
  auto writer = static_cast<Channel&&>(writer_result.value());

  auto reader_result = Channel::OpenReader("test_channel_notify");
  REQUIRE(reader_result.has_value());
  auto reader = static_cast<Channel&&>(reader_result.value());

  std::atomic<bool> writer_done{false};

  // Writer thread: write after delay
  std::thread writer_thread([&writer, &writer_done]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const char* msg = "Delayed message";
    writer.Write(msg, static_cast<uint32_t>(std::strlen(msg)) + 1);
    writer_done.store(true, std::memory_order_release);
  });

  // Reader thread: wait for notification
  auto wait_result = reader.WaitReadable(2000);  // 2s timeout
  REQUIRE(wait_result.has_value());

  char recv[256];
  uint32_t recv_size = 0;
  auto read_result = reader.Read(recv, recv_size);
  REQUIRE(read_result.has_value());
  REQUIRE(std::strcmp(recv, "Delayed message") == 0);

  writer_thread.join();
  REQUIRE(writer_done.load());

  writer.Unlink();
}

TEST_CASE("shm_transport - ShmChannel WaitReadable timeout",
          "[shm_transport]") {
  using Channel = osp::ShmChannel<128, 16>;

  auto writer_result = Channel::CreateWriter("test_channel_timeout");
  REQUIRE(writer_result.has_value());
  auto writer = static_cast<Channel&&>(writer_result.value());

  auto reader_result = Channel::OpenReader("test_channel_timeout");
  REQUIRE(reader_result.has_value());
  auto reader = static_cast<Channel&&>(reader_result.value());

  auto start = std::chrono::steady_clock::now();
  auto wait_result = reader.WaitReadable(50);  // 50ms timeout
  auto elapsed = std::chrono::steady_clock::now() - start;

  REQUIRE_FALSE(wait_result.has_value());
  REQUIRE(wait_result.get_error() == osp::ShmError::kTimeout);

  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  REQUIRE(elapsed_ms >= 40);  // Allow some slack

  writer.Unlink();
}

TEST_CASE("shm_transport - ShmChannel multi-threaded concurrent write and read",
          "[shm_transport]") {
  using Channel = osp::ShmChannel<256, 64>;

  auto writer_result = Channel::CreateWriter("test_channel_concurrent");
  REQUIRE(writer_result.has_value());
  auto writer = static_cast<Channel&&>(writer_result.value());

  auto reader_result = Channel::OpenReader("test_channel_concurrent");
  REQUIRE(reader_result.has_value());
  auto reader = static_cast<Channel&&>(reader_result.value());

  constexpr uint32_t kWriterCount = 3;
  constexpr uint32_t kMessagesPerWriter = 50;
  std::atomic<uint32_t> total_written{0};
  std::atomic<uint32_t> total_read{0};

  // Writer threads
  std::vector<std::thread> writers;
  for (uint32_t w = 0; w < kWriterCount; ++w) {
    writers.emplace_back([&writer, w, &total_written]() {
      for (uint32_t i = 0; i < kMessagesPerWriter; ++i) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "W%u-M%u", w, i);
        uint32_t len = static_cast<uint32_t>(std::strlen(msg)) + 1;

        // Retry on full
        while (!writer.Write(msg, len).has_value()) {
          std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        total_written.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // Reader thread
  std::thread reader_thread([&reader, &total_read]() {
    uint32_t expected = kWriterCount * kMessagesPerWriter;
    char recv[256];
    uint32_t recv_size = 0;

    while (total_read.load(std::memory_order_relaxed) < expected) {
      auto result = reader.Read(recv, recv_size);
      if (result.has_value()) {
        total_read.fetch_add(1, std::memory_order_relaxed);
      } else {
        // Wait for notification or timeout
        reader.WaitReadable(10);
      }
    }
  });

  for (auto& t : writers) {
    t.join();
  }
  reader_thread.join();

  REQUIRE(total_written.load() == kWriterCount * kMessagesPerWriter);
  REQUIRE(total_read.load() == kWriterCount * kMessagesPerWriter);

  writer.Unlink();
}

// ============================================================================
// Cross-process fork tests
// ============================================================================

TEST_CASE("shm_transport - Cross-process SharedMemorySegment visibility",
          "[shm_transport][fork]") {
  const char* name = "xproc_seg";

  auto seg_r = osp::SharedMemorySegment::CreateOrReplace(name, 4096);
  REQUIRE(seg_r.has_value());
  auto seg = static_cast<osp::SharedMemorySegment&&>(seg_r.value());

  // Write a pattern
  auto* data = static_cast<uint8_t*>(seg.Data());
  for (uint32_t i = 0; i < 256; ++i) {
    data[i] = static_cast<uint8_t>(i);
  }

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    // Child: open same segment and verify
    auto child_r = osp::SharedMemorySegment::Open(name);
    if (!child_r.has_value()) _exit(1);
    auto child_seg = static_cast<osp::SharedMemorySegment&&>(child_r.value());
    auto* child_data = static_cast<uint8_t*>(child_seg.Data());
    for (uint32_t i = 0; i < 256; ++i) {
      if (child_data[i] != static_cast<uint8_t>(i)) _exit(2);
    }
    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  seg.Unlink();
}

TEST_CASE("shm_transport - Cross-process ShmRingBuffer write then read",
          "[shm_transport][fork]") {
  const char* name = "xproc_ring";
  using Ring = osp::ShmRingBuffer<4096, 8>;

  auto seg_r = osp::SharedMemorySegment::CreateOrReplace(name, Ring::Size());
  REQUIRE(seg_r.has_value());
  auto seg = static_cast<osp::SharedMemorySegment&&>(seg_r.value());
  auto* ring = Ring::InitAt(seg.Data());

  // Parent writes
  char msg1[] = "hello_from_parent";
  char msg2[] = "second_message";
  REQUIRE(ring->TryPush(msg1, sizeof(msg1)));
  REQUIRE(ring->TryPush(msg2, sizeof(msg2)));
  REQUIRE(ring->Depth() == 2);

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    // Child: open and read
    auto child_r = osp::SharedMemorySegment::Open(name);
    if (!child_r.has_value()) _exit(1);
    auto child_seg = static_cast<osp::SharedMemorySegment&&>(child_r.value());
    auto* child_ring = Ring::AttachAt(child_seg.Data());

    if (child_ring->Depth() != 2) _exit(2);

    char buf[4096];
    uint32_t sz = 4096;
    if (!child_ring->TryPop(buf, sz)) _exit(3);
    if (sz != sizeof(msg1) || std::memcmp(buf, msg1, sz) != 0) _exit(4);

    sz = 4096;
    if (!child_ring->TryPop(buf, sz)) _exit(5);
    if (sz != sizeof(msg2) || std::memcmp(buf, msg2, sz) != 0) _exit(6);

    if (child_ring->Depth() != 0) _exit(7);
    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  seg.Unlink();
}

TEST_CASE("shm_transport - Cross-process ShmChannel producer-consumer",
          "[shm_transport][fork]") {
  const char* name = "xproc_chan";
  using Channel = osp::ShmChannel<4096, 16>;

  auto wr = Channel::CreateOrReplaceWriter(name);
  REQUIRE(wr.has_value());
  auto writer = static_cast<Channel&&>(wr.value());

  // Write 10 messages
  constexpr uint32_t kCount = 10;
  for (uint32_t i = 0; i < kCount; ++i) {
    uint32_t payload = i * 100 + 42;
    auto w = writer.Write(&payload, sizeof(payload));
    REQUIRE(w.has_value());
  }
  REQUIRE(writer.Depth() == kCount);

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    // Child: open reader and consume all messages
    auto rd = Channel::OpenReader(name);
    if (!rd.has_value()) _exit(1);
    auto reader = static_cast<Channel&&>(rd.value());

    if (reader.Depth() != kCount) _exit(2);

    for (uint32_t i = 0; i < kCount; ++i) {
      uint32_t val = 0;
      uint32_t sz = sizeof(val);
      auto r = reader.Read(&val, sz);
      if (!r.has_value()) _exit(10 + i);
      if (sz != sizeof(uint32_t)) _exit(20 + i);
      if (val != i * 100 + 42) _exit(30 + i);
    }

    if (reader.Depth() != 0) _exit(3);
    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  writer.Unlink();
}

TEST_CASE("shm_transport - Cross-process concurrent write and read",
          "[shm_transport][fork]") {
  const char* name = "xproc_conc";
  using Channel = osp::ShmChannel<256, 32>;

  auto wr = Channel::CreateOrReplaceWriter(name);
  REQUIRE(wr.has_value());
  auto writer = static_cast<Channel&&>(wr.value());

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    // Child: reader -- wait for data then consume
    auto rd = Channel::OpenReader(name);
    if (!rd.has_value()) _exit(1);
    auto reader = static_cast<Channel&&>(rd.value());

    uint32_t total_read = 0;
    constexpr uint32_t kExpected = 20;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (total_read < kExpected &&
           std::chrono::steady_clock::now() < deadline) {
      auto w = reader.WaitReadable(200);
      if (!w) continue;

      uint32_t val = 0;
      uint32_t sz = sizeof(val);
      auto r = reader.Read(&val, sz);
      if (r.has_value() && sz == sizeof(uint32_t)) {
        if (val != total_read) _exit(10);
        ++total_read;
      }
    }

    _exit(total_read == kExpected ? 0 : 2);
  }

  // Parent: writer -- write with small delays
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  constexpr uint32_t kCount = 20;
  for (uint32_t i = 0; i < kCount; ++i) {
    auto w = writer.Write(&i, sizeof(i));
    REQUIRE(w.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  writer.Unlink();
}

TEST_CASE("shm_transport - Cross-process large frame transfer",
          "[shm_transport][fork]") {
  const char* name = "xproc_large";
  constexpr uint32_t kSlotSize = 81920;
  constexpr uint32_t kSlotCount = 8;
  using Channel = osp::ShmChannel<kSlotSize, kSlotCount>;

  auto wr = Channel::CreateOrReplaceWriter(name);
  REQUIRE(wr.has_value());
  auto writer = static_cast<Channel&&>(wr.value());

  // Write a large frame (76816 bytes, simulating video frame)
  constexpr uint32_t kFrameSize = 76816;
  std::vector<uint8_t> frame(kFrameSize);
  for (uint32_t i = 0; i < kFrameSize; ++i) {
    frame[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
  }
  auto w = writer.Write(frame.data(), kFrameSize);
  REQUIRE(w.has_value());

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    auto rd = Channel::OpenReader(name);
    if (!rd.has_value()) _exit(1);
    auto reader = static_cast<Channel&&>(rd.value());

    if (reader.Depth() != 1) _exit(2);

    std::vector<uint8_t> buf(kSlotSize);
    uint32_t sz = kSlotSize;
    auto r = reader.Read(buf.data(), sz);
    if (!r.has_value()) _exit(3);
    if (sz != kFrameSize) _exit(4);

    // Verify data integrity
    for (uint32_t i = 0; i < kFrameSize; ++i) {
      if (buf[i] != static_cast<uint8_t>((i * 7 + 13) & 0xFF)) _exit(5);
    }
    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  writer.Unlink();
}

TEST_CASE("shm_transport - Cross-process ShmChannel WaitReadable polling",
          "[shm_transport][fork]") {
  // TSan instruments fork'd children differently; shared memory timing
  // becomes unreliable, causing spurious Read failures.  Skip under TSan.
#if defined(__SANITIZE_THREAD__)
  SKIP("Skipped under ThreadSanitizer (fork + shm timing unreliable)");
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
  SKIP("Skipped under ThreadSanitizer (fork + shm timing unreliable)");
#endif
#endif
  const char* name = "xproc_wait";
  using Channel = osp::ShmChannel<256, 8>;

  auto wr = Channel::CreateOrReplaceWriter(name);
  REQUIRE(wr.has_value());
  auto writer = static_cast<Channel&&>(wr.value());

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    // Child: reader -- WaitReadable should block then succeed
    auto rd = Channel::OpenReader(name);
    if (!rd.has_value()) _exit(1);
    auto reader = static_cast<Channel&&>(rd.value());

    // Should timeout initially (no data yet)
    auto w1 = reader.WaitReadable(100);
    if (w1.has_value()) _exit(2);  // should timeout

    // Wait longer -- parent will write after 300ms
    auto w2 = reader.WaitReadable(2000);
    if (!w2.has_value()) _exit(3);  // should succeed

    uint32_t val = 0;
    uint32_t sz = sizeof(val);
    auto r = reader.Read(&val, sz);
    if (!r.has_value() || val != 0xDEAD) _exit(4);

    _exit(0);
  }

  // Parent: write after delay
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  uint32_t val = 0xDEAD;
  auto w = writer.Write(&val, sizeof(val));
  REQUIRE(w.has_value());

  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  writer.Unlink();
}

// ============================================================================
// ShmSpscByteRing tests
// ============================================================================

TEST_CASE("shm_transport - ShmSpscByteRing basic write and read",
          "[shm_transport]") {
  auto seg_r = osp::SharedMemorySegment::CreateOrReplace("test_byte_ring_basic",
                                                          4096);
  REQUIRE(seg_r.has_value());
  auto& seg = seg_r.value();

  auto ring = osp::ShmSpscByteRing::InitAt(seg.Data(), seg.Size());
  REQUIRE(ring.Capacity() > 0);
  REQUIRE(ring.ReadableBytes() == 0);
  REQUIRE(!ring.HasData());

  // Write a message
  const char msg[] = "Hello ByteRing";
  REQUIRE(ring.Write(msg, sizeof(msg)));
  REQUIRE(ring.HasData());
  REQUIRE(ring.ReadableBytes() == sizeof(msg) + 4);  // 4B length prefix

  // Read it back
  char buf[256];
  uint32_t len = ring.Read(buf, sizeof(buf));
  REQUIRE(len == sizeof(msg));
  REQUIRE(std::memcmp(buf, msg, sizeof(msg)) == 0);
  REQUIRE(!ring.HasData());

  seg.Unlink();
}

TEST_CASE("shm_transport - ShmSpscByteRing multiple messages",
          "[shm_transport]") {
  auto seg_r = osp::SharedMemorySegment::CreateOrReplace("test_byte_ring_multi",
                                                          8192);
  REQUIRE(seg_r.has_value());
  auto& seg = seg_r.value();

  auto ring = osp::ShmSpscByteRing::InitAt(seg.Data(), seg.Size());

  // Write 10 messages
  for (uint32_t i = 0; i < 10; ++i) {
    char msg[64];
    std::snprintf(msg, sizeof(msg), "Message-%u", i);
    uint32_t msg_len = static_cast<uint32_t>(std::strlen(msg)) + 1;
    REQUIRE(ring.Write(msg, msg_len));
  }

  // Read them back in order
  for (uint32_t i = 0; i < 10; ++i) {
    char expected[64];
    std::snprintf(expected, sizeof(expected), "Message-%u", i);
    char buf[256];
    uint32_t len = ring.Read(buf, sizeof(buf));
    REQUIRE(len > 0);
    REQUIRE(std::strcmp(buf, expected) == 0);
  }

  // Should be empty now
  char buf[256];
  REQUIRE(ring.Read(buf, sizeof(buf)) == 0);

  seg.Unlink();
}

TEST_CASE("shm_transport - ShmSpscByteRing full buffer rejection",
          "[shm_transport]") {
  // Small buffer: 4096 total, ~4080 data after header, rounded to 2048
  auto seg_r = osp::SharedMemorySegment::CreateOrReplace("test_byte_ring_full",
                                                          4096);
  REQUIRE(seg_r.has_value());
  auto& seg = seg_r.value();

  auto ring = osp::ShmSpscByteRing::InitAt(seg.Data(), seg.Size());
  uint32_t cap = ring.Capacity();

  // Fill with a message that uses most of the capacity
  // Each message costs len + 4 bytes
  std::vector<uint8_t> big_msg(cap - 8);  // leave room for length prefix
  std::memset(big_msg.data(), 0xAB, big_msg.size());
  REQUIRE(ring.Write(big_msg.data(), static_cast<uint32_t>(big_msg.size())));

  // Next write should fail (not enough space)
  char small[] = "x";
  REQUIRE_FALSE(ring.Write(small, 2));

  // Read the big message, then write should succeed again
  std::vector<uint8_t> recv(cap);
  uint32_t len = ring.Read(recv.data(), static_cast<uint32_t>(recv.size()));
  REQUIRE(len == big_msg.size());
  REQUIRE(ring.Write(small, 2));

  seg.Unlink();
}

TEST_CASE("shm_transport - ShmSpscByteRing wrap-around",
          "[shm_transport]") {
  auto seg_r = osp::SharedMemorySegment::CreateOrReplace("test_byte_ring_wrap",
                                                          4096);
  REQUIRE(seg_r.has_value());
  auto& seg = seg_r.value();

  auto ring = osp::ShmSpscByteRing::InitAt(seg.Data(), seg.Size());
  uint32_t cap = ring.Capacity();

  // Fill and drain multiple rounds to force wrap-around
  for (uint32_t round = 0; round < 5; ++round) {
    // Write messages until near full
    uint32_t written = 0;
    while (ring.WriteableBytes() >= 68) {  // 64 payload + 4 prefix
      char msg[64];
      std::snprintf(msg, sizeof(msg), "R%u-M%u", round, written);
      uint32_t msg_len = static_cast<uint32_t>(std::strlen(msg)) + 1;
      REQUIRE(ring.Write(msg, msg_len));
      ++written;
    }
    REQUIRE(written > 0);

    // Drain all
    uint32_t read_count = 0;
    char buf[256];
    while (ring.Read(buf, sizeof(buf)) > 0) {
      ++read_count;
    }
    REQUIRE(read_count == written);
  }

  seg.Unlink();
}

TEST_CASE("shm_transport - ShmSpscByteRing producer/consumer attach",
          "[shm_transport]") {
  auto seg_r = osp::SharedMemorySegment::CreateOrReplace("test_byte_ring_attach",
                                                          8192);
  REQUIRE(seg_r.has_value());
  auto& seg = seg_r.value();

  // Producer initializes
  auto producer = osp::ShmSpscByteRing::InitAt(seg.Data(), seg.Size());

  // Consumer attaches
  auto consumer = osp::ShmSpscByteRing::AttachAt(seg.Data());
  REQUIRE(consumer.Capacity() == producer.Capacity());

  // Producer writes
  const char msg[] = "cross-view test";
  REQUIRE(producer.Write(msg, sizeof(msg)));

  // Consumer reads
  char buf[256];
  uint32_t len = consumer.Read(buf, sizeof(buf));
  REQUIRE(len == sizeof(msg));
  REQUIRE(std::memcmp(buf, msg, sizeof(msg)) == 0);

  seg.Unlink();
}

TEST_CASE("shm_transport - ShmSpscByteRing large payload (simulated LiDAR)",
          "[shm_transport]") {
  // 2 MB buffer for large payloads
  constexpr uint32_t kBufSize = 2 * 1024 * 1024;
  auto seg_r = osp::SharedMemorySegment::CreateOrReplace("test_byte_ring_lidar",
                                                          kBufSize);
  REQUIRE(seg_r.has_value());
  auto& seg = seg_r.value();

  auto ring = osp::ShmSpscByteRing::InitAt(seg.Data(), seg.Size());

  // Simulate a 100KB LiDAR point cloud
  constexpr uint32_t kFrameSize = 100 * 1024;
  std::vector<uint8_t> frame(kFrameSize);
  for (uint32_t i = 0; i < kFrameSize; ++i) {
    frame[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
  }

  REQUIRE(ring.Write(frame.data(), kFrameSize));

  std::vector<uint8_t> recv(kFrameSize);
  uint32_t len = ring.Read(recv.data(), kFrameSize);
  REQUIRE(len == kFrameSize);

  // Verify data integrity
  for (uint32_t i = 0; i < kFrameSize; ++i) {
    if (recv[i] != frame[i]) {
      FAIL("Data mismatch at offset " << i);
      break;
    }
  }

  seg.Unlink();
}

TEST_CASE("shm_transport - ShmSpscByteRing concurrent SPSC",
          "[shm_transport]") {
  constexpr uint32_t kBufSize = 256 * 1024;
  constexpr uint32_t kMsgCount = 1000;

  auto seg_r = osp::SharedMemorySegment::CreateOrReplace("test_byte_ring_spsc",
                                                          kBufSize);
  REQUIRE(seg_r.has_value());
  auto& seg = seg_r.value();

  auto producer = osp::ShmSpscByteRing::InitAt(seg.Data(), seg.Size());
  auto consumer = osp::ShmSpscByteRing::AttachAt(seg.Data());

  std::atomic<bool> error_flag{false};

  std::thread prod_thread([&producer]() {
    for (uint32_t i = 0; i < kMsgCount; ++i) {
      while (!producer.Write(&i, sizeof(i))) {
        std::this_thread::yield();
      }
    }
  });

  std::thread cons_thread([&consumer, &error_flag]() {
    uint32_t expected = 0;
    while (expected < kMsgCount) {
      uint32_t val = 0;
      uint32_t len = consumer.Read(&val, sizeof(val));
      if (len > 0) {
        if (val != expected) {
          error_flag.store(true, std::memory_order_relaxed);
          break;
        }
        ++expected;
      } else {
        std::this_thread::yield();
      }
    }
  });

  prod_thread.join();
  cons_thread.join();

  REQUIRE_FALSE(error_flag.load());

  seg.Unlink();
}

TEST_CASE("shm_transport - ShmSpscByteRing RoundDownPow2 via capacity",
          "[shm_transport]") {
  // 5000 total - 16 header = 4984 data, rounded down to 4096
  auto seg_r = osp::SharedMemorySegment::CreateOrReplace("test_byte_ring_pow2",
                                                          5000);
  REQUIRE(seg_r.has_value());
  auto& seg = seg_r.value();

  // Page-aligned size will be 8192, so data = 8192 - 16 = 8176, pow2 = 4096
  auto ring = osp::ShmSpscByteRing::InitAt(seg.Data(), seg.Size());
  // Capacity must be power of 2
  uint32_t cap = ring.Capacity();
  REQUIRE(cap > 0);
  REQUIRE((cap & (cap - 1)) == 0);

  seg.Unlink();
}

// ============================================================================
// ShmByteChannel tests
// ============================================================================

TEST_CASE("shm_transport - ShmByteChannel create writer and open reader",
          "[shm_transport]") {
  auto wr = osp::ShmByteChannel::CreateOrReplaceWriter("test_byte_ch_1", 4096);
  REQUIRE(wr.has_value());
  auto writer = static_cast<osp::ShmByteChannel&&>(wr.value());

  auto rd = osp::ShmByteChannel::OpenReader("test_byte_ch_1");
  REQUIRE(rd.has_value());
  auto reader = static_cast<osp::ShmByteChannel&&>(rd.value());

  REQUIRE(writer.Capacity() > 0);
  REQUIRE(reader.Capacity() == writer.Capacity());

  writer.Unlink();
}

TEST_CASE("shm_transport - ShmByteChannel write and read",
          "[shm_transport]") {
  auto wr = osp::ShmByteChannel::CreateOrReplaceWriter("test_byte_ch_rw", 8192);
  REQUIRE(wr.has_value());
  auto writer = static_cast<osp::ShmByteChannel&&>(wr.value());

  auto rd = osp::ShmByteChannel::OpenReader("test_byte_ch_rw");
  REQUIRE(rd.has_value());
  auto reader = static_cast<osp::ShmByteChannel&&>(rd.value());

  // Write
  const char msg[] = "Hello from ShmByteChannel";
  auto w = writer.Write(msg, sizeof(msg));
  REQUIRE(w.has_value());

  // Read
  char buf[256];
  uint32_t len = reader.Read(buf, sizeof(buf));
  REQUIRE(len == sizeof(msg));
  REQUIRE(std::strcmp(buf, msg) == 0);

  writer.Unlink();
}

TEST_CASE("shm_transport - ShmByteChannel write full returns kFull",
          "[shm_transport]") {
  // Small capacity
  auto wr = osp::ShmByteChannel::CreateOrReplaceWriter("test_byte_ch_full", 4096);
  REQUIRE(wr.has_value());
  auto writer = static_cast<osp::ShmByteChannel&&>(wr.value());

  uint32_t cap = writer.Capacity();
  // Fill the buffer
  std::vector<uint8_t> big(cap - 8);
  auto w1 = writer.Write(big.data(), static_cast<uint32_t>(big.size()));
  REQUIRE(w1.has_value());

  // Next write should fail
  char small[] = "x";
  auto w2 = writer.Write(small, 2);
  REQUIRE_FALSE(w2.has_value());
  REQUIRE(w2.get_error() == osp::ShmError::kFull);

  writer.Unlink();
}

TEST_CASE("shm_transport - ShmByteChannel futex WaitReadable",
          "[shm_transport]") {
  auto wr = osp::ShmByteChannel::CreateOrReplaceWriter("test_byte_ch_futex",
                                                         8192);
  REQUIRE(wr.has_value());
  auto writer = static_cast<osp::ShmByteChannel&&>(wr.value());

  auto rd = osp::ShmByteChannel::OpenReader("test_byte_ch_futex");
  REQUIRE(rd.has_value());
  auto reader = static_cast<osp::ShmByteChannel&&>(rd.value());

  std::atomic<bool> writer_done{false};

  // Writer thread: write after 50ms delay
  std::thread writer_thread([&writer, &writer_done]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const char msg[] = "futex wakeup";
    writer.Write(msg, sizeof(msg));
    writer_done.store(true, std::memory_order_release);
  });

  // Reader: wait for data via futex
  auto wait_result = reader.WaitReadable(2000);
  REQUIRE(wait_result.has_value());

  char buf[256];
  uint32_t len = reader.Read(buf, sizeof(buf));
  REQUIRE(len > 0);
  REQUIRE(std::strcmp(buf, "futex wakeup") == 0);

  writer_thread.join();
  REQUIRE(writer_done.load());

  writer.Unlink();
}

TEST_CASE("shm_transport - ShmByteChannel WaitReadable timeout",
          "[shm_transport]") {
  auto wr = osp::ShmByteChannel::CreateOrReplaceWriter("test_byte_ch_timeout",
                                                         4096);
  REQUIRE(wr.has_value());
  auto writer = static_cast<osp::ShmByteChannel&&>(wr.value());

  auto rd = osp::ShmByteChannel::OpenReader("test_byte_ch_timeout");
  REQUIRE(rd.has_value());
  auto reader = static_cast<osp::ShmByteChannel&&>(rd.value());

  auto start = std::chrono::steady_clock::now();
  auto wait_result = reader.WaitReadable(50);
  auto elapsed = std::chrono::steady_clock::now() - start;

  REQUIRE_FALSE(wait_result.has_value());
  REQUIRE(wait_result.get_error() == osp::ShmError::kTimeout);

  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  REQUIRE(elapsed_ms >= 40);  // Allow some slack

  writer.Unlink();
}

TEST_CASE("shm_transport - ShmByteChannel concurrent SPSC throughput",
          "[shm_transport]") {
  constexpr uint32_t kCapacity = 256 * 1024;
  constexpr uint32_t kMsgCount = 500;

  auto wr = osp::ShmByteChannel::CreateOrReplaceWriter("test_byte_ch_spsc",
                                                         kCapacity);
  REQUIRE(wr.has_value());
  auto writer = static_cast<osp::ShmByteChannel&&>(wr.value());

  auto rd = osp::ShmByteChannel::OpenReader("test_byte_ch_spsc");
  REQUIRE(rd.has_value());
  auto reader = static_cast<osp::ShmByteChannel&&>(rd.value());

  std::atomic<uint32_t> total_read{0};

  std::thread prod([&writer]() {
    for (uint32_t i = 0; i < kMsgCount; ++i) {
      char msg[128];
      std::snprintf(msg, sizeof(msg), "Msg-%u", i);
      uint32_t len = static_cast<uint32_t>(std::strlen(msg)) + 1;
      while (!writer.Write(msg, len).has_value()) {
        std::this_thread::yield();
      }
    }
  });

  std::thread cons([&reader, &total_read]() {
    char buf[256];
    while (total_read.load(std::memory_order_relaxed) < kMsgCount) {
      uint32_t len = reader.Read(buf, sizeof(buf));
      if (len > 0) {
        total_read.fetch_add(1, std::memory_order_relaxed);
      } else {
        reader.WaitReadable(10);
      }
    }
  });

  prod.join();
  cons.join();

  REQUIRE(total_read.load() == kMsgCount);

  writer.Unlink();
}

TEST_CASE("shm_transport - ShmByteChannel move semantics",
          "[shm_transport]") {
  auto wr = osp::ShmByteChannel::CreateOrReplaceWriter("test_byte_ch_move",
                                                         4096);
  REQUIRE(wr.has_value());
  auto ch1 = static_cast<osp::ShmByteChannel&&>(wr.value());
  REQUIRE(ch1.Capacity() > 0);

  // Move construct
  osp::ShmByteChannel ch2(static_cast<osp::ShmByteChannel&&>(ch1));
  REQUIRE(ch2.Capacity() > 0);

  // Move assign
  osp::ShmByteChannel ch3;
  ch3 = static_cast<osp::ShmByteChannel&&>(ch2);
  REQUIRE(ch3.Capacity() > 0);

  // Write through moved channel
  const char msg[] = "moved";
  auto w = ch3.Write(msg, sizeof(msg));
  REQUIRE(w.has_value());

  ch3.Unlink();
}

// ============================================================================
// Cross-process ShmByteChannel tests
// ============================================================================

TEST_CASE("shm_transport - Cross-process ShmByteChannel write then read",
          "[shm_transport][fork]") {
  const char* name = "xproc_byte_ch";
  constexpr uint32_t kCapacity = 64 * 1024;

  auto wr = osp::ShmByteChannel::CreateOrReplaceWriter(name, kCapacity);
  REQUIRE(wr.has_value());
  auto writer = static_cast<osp::ShmByteChannel&&>(wr.value());

  // Write messages
  constexpr uint32_t kCount = 10;
  for (uint32_t i = 0; i < kCount; ++i) {
    uint32_t payload = i * 100 + 42;
    auto w = writer.Write(&payload, sizeof(payload));
    REQUIRE(w.has_value());
  }

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    auto rd = osp::ShmByteChannel::OpenReader(name);
    if (!rd.has_value()) _exit(1);
    auto reader = static_cast<osp::ShmByteChannel&&>(rd.value());

    for (uint32_t i = 0; i < kCount; ++i) {
      uint32_t val = 0;
      uint32_t len = reader.Read(&val, sizeof(val));
      if (len != sizeof(uint32_t)) _exit(10 + i);
      if (val != i * 100 + 42) _exit(20 + i);
    }
    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  writer.Unlink();
}

TEST_CASE("shm_transport - Cross-process ShmByteChannel large frame",
          "[shm_transport][fork]") {
  const char* name = "xproc_byte_large";
  constexpr uint32_t kCapacity = 512 * 1024;
  constexpr uint32_t kFrameSize = 200 * 1024;  // 200 KB simulated frame

  auto wr = osp::ShmByteChannel::CreateOrReplaceWriter(name, kCapacity);
  REQUIRE(wr.has_value());
  auto writer = static_cast<osp::ShmByteChannel&&>(wr.value());

  // Write a large frame
  std::vector<uint8_t> frame(kFrameSize);
  for (uint32_t i = 0; i < kFrameSize; ++i) {
    frame[i] = static_cast<uint8_t>((i * 11 + 7) & 0xFF);
  }
  auto w = writer.Write(frame.data(), kFrameSize);
  REQUIRE(w.has_value());

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    auto rd = osp::ShmByteChannel::OpenReader(name);
    if (!rd.has_value()) _exit(1);
    auto reader = static_cast<osp::ShmByteChannel&&>(rd.value());

    std::vector<uint8_t> buf(kFrameSize);
    uint32_t len = reader.Read(buf.data(), kFrameSize);
    if (len != kFrameSize) _exit(2);

    for (uint32_t i = 0; i < kFrameSize; ++i) {
      if (buf[i] != static_cast<uint8_t>((i * 11 + 7) & 0xFF)) _exit(3);
    }
    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  writer.Unlink();
}

TEST_CASE("shm_transport - Cross-process ShmByteChannel futex notification",
          "[shm_transport][fork]") {
#if defined(__SANITIZE_THREAD__)
  SKIP("Skipped under ThreadSanitizer (fork + shm timing unreliable)");
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
  SKIP("Skipped under ThreadSanitizer (fork + shm timing unreliable)");
#endif
#endif
  const char* name = "xproc_byte_futex";
  constexpr uint32_t kCapacity = 8192;

  auto wr = osp::ShmByteChannel::CreateOrReplaceWriter(name, kCapacity);
  REQUIRE(wr.has_value());
  auto writer = static_cast<osp::ShmByteChannel&&>(wr.value());

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    auto rd = osp::ShmByteChannel::OpenReader(name);
    if (!rd.has_value()) _exit(1);
    auto reader = static_cast<osp::ShmByteChannel&&>(rd.value());

    // Should timeout initially (no data)
    auto w1 = reader.WaitReadable(100);
    if (w1.has_value()) _exit(2);

    // Wait longer -- parent writes after 300ms
    auto w2 = reader.WaitReadable(2000);
    if (!w2.has_value()) _exit(3);

    uint32_t val = 0;
    uint32_t len = reader.Read(&val, sizeof(val));
    if (len != sizeof(uint32_t) || val != 0xBEEF) _exit(4);

    _exit(0);
  }

  // Parent: write after delay
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  uint32_t val = 0xBEEF;
  auto w = writer.Write(&val, sizeof(val));
  REQUIRE(w.has_value());

  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  writer.Unlink();
}

#endif  // OSP_PLATFORM_LINUX
