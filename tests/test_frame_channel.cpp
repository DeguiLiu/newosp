/**
 * @file test_frame_channel.cpp
 * @brief Catch2 tests for osp::FrameChannel (zero-copy slot-based frame channel).
 *
 * Verifies the Windows branch (CreateFileMappingW / MapViewOfFile) behavior:
 * producer/consumer roundtrip, fast-forward (drop-intermediate) semantics,
 * CRC validation, and the kFull back-pressure boundary. All TEST_CASEs are
 * guarded by OSP_PLATFORM_WINDOWS so Linux CI is unaffected.
 */

#include "osp/frame_channel.hpp"

#include <cstring>

#include <catch2/catch_test_macros.hpp>

#if defined(OSP_PLATFORM_WINDOWS)

// ============================================================================
// Geometry: slot_size for a 64-byte payload, 2 slots
// ============================================================================

namespace {

constexpr uint32_t kPayloadBytes = 64u;
constexpr uint32_t kSlotSize = osp::FrameChannel::RequiredSlotSize(kPayloadBytes);  // 128
constexpr osp::FrameMeta kMeta{8u, 8u, osp::FrameDtype::kUint8};                    // 8*8*1 = 64

void FillPayload(uint8_t* out, uint32_t value) {
  std::memset(out, static_cast<int>(value), kPayloadBytes);
}

}  // namespace

// ============================================================================
// Producer/consumer roundtrip (same process, two endpoints)
// ============================================================================

TEST_CASE("frame_channel - producer/consumer roundtrip preserves payload", "[frame_channel][windows]") {
  auto pr = osp::FrameChannel::OpenProducer("fc_roundtrip", kSlotSize, 2u);
  REQUIRE(pr.has_value());
  osp::FrameChannel producer = std::move(pr.value());
  REQUIRE(producer.IsWriter());
  REQUIRE(producer.NumSlots() == 2u);
  REQUIRE(producer.FrameCapacity() >= kPayloadBytes);

  uint8_t payload[kPayloadBytes];
  FillPayload(payload, 0x5A);

  auto wr = producer.WriteFrame(kMeta, payload, kPayloadBytes);
  REQUIRE(wr.has_value());
  REQUIRE(producer.WriteIndex() == 1u);

  auto cr = osp::FrameChannel::OpenConsumer("fc_roundtrip");
  REQUIRE(cr.has_value());
  osp::FrameChannel consumer = std::move(cr.value());
  REQUIRE_FALSE(consumer.IsWriter());

  auto rr = consumer.ReadLatestFrame();
  REQUIRE(rr.has_value());
  const osp::FrameView view = rr.value();
  REQUIRE(view.frame_no == 0u);
  REQUIRE(view.width == kMeta.width);
  REQUIRE(view.height == kMeta.height);
  REQUIRE(view.dtype == static_cast<uint32_t>(osp::FrameDtype::kUint8));
  REQUIRE(view.payload_size == kPayloadBytes);
  REQUIRE(view.crc32 == osp::FrameCrc32(payload, kPayloadBytes));
  REQUIRE(std::memcmp(view.data, payload, kPayloadBytes) == 0);

  // No newer frame: second read must report kEmpty.
  auto rr2 = consumer.ReadLatestFrame();
  REQUIRE_FALSE(rr2.has_value());
  REQUIRE(rr2.get_error() == osp::FrameChannelError::kEmpty);
}

// ============================================================================
// Fast-forward: writing many frames, consumer reads only the latest
// ============================================================================

TEST_CASE("frame_channel - consumer fast-forwards to latest frame", "[frame_channel][windows]") {
  constexpr uint32_t kNumSlots = 8u;
  auto pr = osp::FrameChannel::OpenProducer("fc_fastfwd", kSlotSize, kNumSlots);
  REQUIRE(pr.has_value());
  osp::FrameChannel producer = std::move(pr.value());

  for (uint32_t f = 0; f < 5u; ++f) {
    uint8_t payload[kPayloadBytes];
    FillPayload(payload, f + 1u);
    REQUIRE(producer.WriteFrame(kMeta, payload, kPayloadBytes).has_value());
  }
  REQUIRE(producer.WriteIndex() == 5u);

  auto cr = osp::FrameChannel::OpenConsumer("fc_fastfwd");
  REQUIRE(cr.has_value());
  osp::FrameChannel consumer = std::move(cr.value());
  REQUIRE(consumer.HasNewFrame());

  // First read must return frame 4 (the latest), dropping 0..3.
  auto rr = consumer.ReadLatestFrame();
  REQUIRE(rr.has_value());
  const osp::FrameView view = rr.value();
  REQUIRE(view.frame_no == 4u);

  uint8_t expected[kPayloadBytes];
  FillPayload(expected, 5u);  // frame 4 payload = value 5
  REQUIRE(std::memcmp(view.data, expected, kPayloadBytes) == 0);

  // Consumer has now acked frame 4; producer can keep going.
  uint8_t payload[kPayloadBytes];
  FillPayload(payload, 9u);
  REQUIRE(producer.WriteFrame(kMeta, payload, kPayloadBytes).has_value());

  auto rr2 = consumer.ReadLatestFrame();
  REQUIRE(rr2.has_value());
  REQUIRE(rr2.value().frame_no == 5u);
  REQUIRE(std::memcmp(rr2.value().data, payload, kPayloadBytes) == 0);
}

// ============================================================================
// CRC: corrupting the published payload makes the consumer reject the frame
// ============================================================================

TEST_CASE("frame_channel - corrupted payload detected via CRC", "[frame_channel][windows]") {
  constexpr uint32_t kNumSlots = 4u;
  auto pr = osp::FrameChannel::OpenProducer("fc_crc", kSlotSize, kNumSlots);
  REQUIRE(pr.has_value());
  osp::FrameChannel producer = std::move(pr.value());

  uint8_t payload[kPayloadBytes];
  FillPayload(payload, 0xAB);
  REQUIRE(producer.WriteFrame(kMeta, payload, kPayloadBytes).has_value());

  // Open a raw second mapping into the same section and flip one payload byte.
  auto seg_r = osp::FrameShmSegment::Open("fc_crc");
  REQUIRE(seg_r.has_value());
  uint8_t* raw = static_cast<uint8_t*>(seg_r.value().Data());
  const uint32_t slot0_data_offset = osp::kFrameSegmentHeaderBytes + osp::kFrameSlotHeaderBytes;
  raw[slot0_data_offset] ^= 0xFFu;  // corrupt first payload byte

  auto cr = osp::FrameChannel::OpenConsumer("fc_crc");
  REQUIRE(cr.has_value());
  osp::FrameChannel consumer = std::move(cr.value());
  auto rr = consumer.ReadLatestFrame();
  REQUIRE_FALSE(rr.has_value());
  REQUIRE(rr.get_error() == osp::FrameChannelError::kCorrupt);
}

// ============================================================================
// kFull back-pressure: producer blocked when consumer is too far behind
// ============================================================================

TEST_CASE("frame_channel - full buffer reports kFull", "[frame_channel][windows]") {
  constexpr uint32_t kNumSlots = 4u;
  auto pr = osp::FrameChannel::OpenProducer("fc_full", kSlotSize, kNumSlots);
  REQUIRE(pr.has_value());
  osp::FrameChannel producer = std::move(pr.value());

  uint8_t payload[kPayloadBytes];
  FillPayload(payload, 1u);

  // Exactly kNumSlots in-flight writes succeed (frames 0..3, wi -> 4).
  for (uint32_t i = 0; i < kNumSlots; ++i) {
    REQUIRE(producer.WriteFrame(kMeta, payload, kPayloadBytes).has_value());
  }
  // 5th write must be rejected: the producer would clobber the consumer's view.
  auto w5 = producer.WriteFrame(kMeta, payload, kPayloadBytes);
  REQUIRE_FALSE(w5.has_value());
  REQUIRE(w5.get_error() == osp::FrameChannelError::kFull);

  // A consumer that acks the latest frame releases the back-pressure.
  auto cr = osp::FrameChannel::OpenConsumer("fc_full");
  REQUIRE(cr.has_value());
  osp::FrameChannel consumer = std::move(cr.value());
  auto rr = consumer.ReadLatestFrame();
  REQUIRE(rr.has_value());

  REQUIRE(producer.WriteFrame(kMeta, payload, kPayloadBytes).has_value());
}

// ============================================================================
// WaitReadable timeout (Windows polling branch)
// ============================================================================

TEST_CASE("frame_channel - WaitReadable times out on Windows poller", "[frame_channel][windows]") {
  auto pr = osp::FrameChannel::OpenProducer("fc_wait", kSlotSize, 4u);
  REQUIRE(pr.has_value());
  osp::FrameChannel producer = std::move(pr.value());

  auto cr = osp::FrameChannel::OpenConsumer("fc_wait");
  REQUIRE(cr.has_value());
  osp::FrameChannel consumer = std::move(cr.value());

  // Nothing written: polling WaitReadable must time out.
  auto w = consumer.WaitReadable(30u);
  REQUIRE_FALSE(w.has_value());
  REQUIRE(w.get_error() == osp::FrameChannelError::kTimeout);

  // A frame published by the producer makes WaitReadable succeed.
  uint8_t payload[kPayloadBytes];
  FillPayload(payload, 7u);
  REQUIRE(producer.WriteFrame(kMeta, payload, kPayloadBytes).has_value());
  auto w2 = consumer.WaitReadable(2000u);
  REQUIRE(w2.has_value());
}

#else  // OSP_PLATFORM_WINDOWS

TEST_CASE("frame_channel - Windows tests are Windows-only", "[frame_channel][windows]") {
  SKIP("frame_channel Windows tests only run on Windows");
}

#endif  // OSP_PLATFORM_WINDOWS
