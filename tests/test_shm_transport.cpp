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
  REQUIRE(seg2.Size() == 2048);

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

#endif  // OSP_PLATFORM_LINUX
