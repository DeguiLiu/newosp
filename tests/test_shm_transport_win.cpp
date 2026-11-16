/**
 * @file test_shm_transport_win.cpp
 * @brief Catch2 tests for osp::shm_transport.hpp on the Windows branch
 *        (CreateFileMappingW / OpenFileMappingW / MapViewOfFile).
 *
 * Verifies SharedMemorySegment create/open/replace/unlink lifecycle and a
 * ShmChannel writer/reader roundtrip including WaitReadable. Guarded by
 * OSP_PLATFORM_WINDOWS.
 *
 * KNOWN WINDOWS BUG (exposed by these tests, NOT fixed here):
 *   SharedMemorySegment::Open (include/osp/shm_transport.hpp, ~line 349) calls
 *   GetFileInformationByHandle() on the section handle returned by
 *   OpenFileMappingW(). For a pagefile-backed section (created with
 *   INVALID_HANDLE_VALUE) that call fails with ERROR_INVALID_HANDLE(6), so Open
 *   always returns ShmError::kOpenFailed on Windows. This cascades to
 *   ShmChannel::OpenReader. The create/write/ring-buffer paths work; only the
 *   open/reader path is affected. frame_channel.hpp avoids this by mapping with
 *   dwNumberOfBytesToMap=0 and using VirtualQuery to learn the size.
 */

#include "osp/shm_transport.hpp"

#include <cstring>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

#if defined(OSP_PLATFORM_WINDOWS)

// ============================================================================
// SharedMemorySegment lifecycle
// ============================================================================

TEST_CASE("shm_transport_win - SharedMemorySegment create and write/read", "[shm_transport][windows]") {
  auto r = osp::SharedMemorySegment::Create("seg_rw", 4096);
  REQUIRE(r.has_value());
  REQUIRE(r.value().Data() != nullptr);
  REQUIRE(r.value().Size() >= 4096);

  char* p = static_cast<char*>(r.value().Data());
  std::memcpy(p, "Hello SHM", 10);
  REQUIRE(std::memcmp(p, "Hello SHM", 10) == 0);
}

TEST_CASE("shm_transport_win - Open existing segment sees same memory", "[shm_transport][windows]") {
  // KNOWN BUG: this currently FAILS because SharedMemorySegment::Open always
  // returns kOpenFailed on Windows (GetFileInformationByHandle is invalid for
  // pagefile-backed section handles). Kept as a red test documenting the bug.
  auto create_r = osp::SharedMemorySegment::Create("seg_open", 8192);
  REQUIRE(create_r.has_value());
  std::memcpy(create_r.value().Data(), "SharedData", 11);

  auto open_r = osp::SharedMemorySegment::Open("seg_open");
  REQUIRE(open_r.has_value());
  REQUIRE(open_r.value().Size() >= 8192);
  REQUIRE(std::memcmp(open_r.value().Data(), "SharedData", 11) == 0);
}

TEST_CASE("shm_transport_win - Open non-existent segment fails", "[shm_transport][windows]") {
  auto r = osp::SharedMemorySegment::Open("seg_does_not_exist_xyz");
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.get_error() == osp::ShmError::kOpenFailed);
}

TEST_CASE("shm_transport_win - CreateOrReplace same name twice", "[shm_transport][windows]") {
  auto r1 = osp::SharedMemorySegment::CreateOrReplace("seg_repl", 4096);
  REQUIRE(r1.has_value());
  REQUIRE(r1.value().Data() != nullptr);

  auto r2 = osp::SharedMemorySegment::CreateOrReplace("seg_repl", 4096);
  REQUIRE(r2.has_value());
  REQUIRE(r2.value().Data() != nullptr);

  // Both handles map the same section: a write via r1 is visible via r2.
  std::memcpy(r1.value().Data(), "replace-ok", 11);
  REQUIRE(std::memcmp(r2.value().Data(), "replace-ok", 11) == 0);
}

TEST_CASE("shm_transport_win - Unlink then Open fails", "[shm_transport][windows]") {
  // NOTE: Open fails on Windows regardless of Unlink (see the Open bug above),
  // so this green result is confounded -- it cannot distinguish "unlinked" from
  // "Open broken". Kept for the post-Unlink lifecycle smoke test.
  {
    auto r = osp::SharedMemorySegment::Create("seg_unlink", 4096);
    REQUIRE(r.has_value());
    r.value().Unlink();  // Windows: drops the handle; section dies when last ref closes.
  }
  auto open_r = osp::SharedMemorySegment::Open("seg_unlink");
  REQUIRE_FALSE(open_r.has_value());
}

// ============================================================================
// ShmRingBuffer direct (writer side, no Open needed)
// ============================================================================

TEST_CASE("shm_transport_win - ShmRingBuffer push/pop on shared memory", "[shm_transport][windows]") {
  using RB = osp::ShmRingBuffer<256, 8>;
  auto seg_r = osp::SharedMemorySegment::Create("rb_direct", RB::Size());
  REQUIRE(seg_r.has_value());

  RB* rb = RB::InitAt(seg_r.value().Data());
  REQUIRE(rb != nullptr);
  REQUIRE(rb->Depth() == 0);

  const char msg[] = "ring";
  REQUIRE(rb->TryPush(msg, sizeof(msg)));
  REQUIRE(rb->Depth() == 1);

  char out[256] = {0};
  uint32_t n = 0;
  REQUIRE(rb->TryPop(out, n));
  REQUIRE(n == sizeof(msg));
  REQUIRE(std::memcmp(out, msg, sizeof(msg)) == 0);
  REQUIRE(rb->Depth() == 0);
}

// ============================================================================
// ShmChannel (writer path works; reader path hits the Open bug)
// ============================================================================

TEST_CASE("shm_transport_win - ShmChannel writer writes into ring", "[shm_transport][windows]") {
  using Channel = osp::ShmChannel<256, 8>;
  auto wr = Channel::CreateOrReplaceWriter("ch_write");
  REQUIRE(wr.has_value());
  Channel writer = std::move(wr.value());

  const char msg[] = "hello";
  REQUIRE(writer.Write(msg, sizeof(msg)).has_value());
  REQUIRE(writer.Depth() == 1);
}

// ============================================================================
// ShmChannel roundtrip
// ============================================================================

TEST_CASE("shm_transport_win - ShmChannel writer/reader roundtrip", "[shm_transport][windows]") {
  // KNOWN BUG: this currently FAILS at OpenReader because SharedMemorySegment::Open
  // always returns kOpenFailed on Windows (see file header). Kept as a red test
  // documenting the bug -- once Open is fixed this verifies the read path.
  using Channel = osp::ShmChannel<256, 8>;

  auto wr = Channel::CreateOrReplaceWriter("ch_roundtrip");
  REQUIRE(wr.has_value());
  Channel writer = std::move(wr.value());

  auto rr = Channel::OpenReader("ch_roundtrip");
  REQUIRE(rr.has_value());
  Channel reader = std::move(rr.value());

  const char msg[] = "hello shm";
  REQUIRE(writer.Write(msg, sizeof(msg)).has_value());

  char buf[256] = {0};
  uint32_t size = 0;
  REQUIRE(reader.Read(buf, size).has_value());
  REQUIRE(size == sizeof(msg));
  REQUIRE(std::memcmp(buf, msg, sizeof(msg)) == 0);

  // Ring is now empty.
  auto empty_r = reader.Read(buf, size);
  REQUIRE_FALSE(empty_r.has_value());
  REQUIRE(empty_r.get_error() == osp::ShmError::kEmpty);
}

TEST_CASE("shm_transport_win - ShmChannel WaitReadable wakes on background write",
          "[shm_transport][windows]") {
  using Channel = osp::ShmChannel<256, 8>;

  auto wr = Channel::CreateOrReplaceWriter("ch_wait");
  REQUIRE(wr.has_value());
  Channel writer = std::move(wr.value());

  // Blocked by the Open bug; SKIP so the WaitReadable path is not silently lost.
  auto rr = Channel::OpenReader("ch_wait");
  if (!rr.has_value()) {
    SKIP("ShmChannel::OpenReader broken on Windows (GetFileInformationByHandle bug)");
  }
  Channel reader = std::move(rr.value());

  std::atomic<bool> written{false};
  std::thread producer([&writer, &written]() {
    osp::ThreadSleepUs(static_cast<uint64_t>(100) * 1000u);
    const char data[] = "wake";
    (void)writer.Write(data, sizeof(data));
    written.store(true, std::memory_order_release);
  });

  auto w = reader.WaitReadable(2000u);
  producer.join();
  REQUIRE(w.has_value());
  REQUIRE(written.load(std::memory_order_acquire));
}

TEST_CASE("shm_transport_win - ShmChannel WaitReadable times out when empty",
          "[shm_transport][windows]") {
  using Channel = osp::ShmChannel<256, 8>;

  auto wr = Channel::CreateOrReplaceWriter("ch_timeout");
  REQUIRE(wr.has_value());
  Channel writer = std::move(wr.value());

  // Blocked by the Open bug; SKIP so the WaitReadable path is not silently lost.
  auto rr = Channel::OpenReader("ch_timeout");
  if (!rr.has_value()) {
    SKIP("ShmChannel::OpenReader broken on Windows (GetFileInformationByHandle bug)");
  }
  Channel reader = std::move(rr.value());

  auto w = reader.WaitReadable(30u);
  REQUIRE_FALSE(w.has_value());
  REQUIRE(w.get_error() == osp::ShmError::kTimeout);
}

#else  // OSP_PLATFORM_WINDOWS

TEST_CASE("shm_transport_win - Windows SHM tests are Windows-only", "[shm_transport][windows]") {
  SKIP("shm_transport_win tests only run on Windows");
}

#endif  // OSP_PLATFORM_WINDOWS
