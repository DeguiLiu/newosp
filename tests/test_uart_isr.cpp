/**
 * @file test_uart_isr.cpp
 * @brief Catch2 tests for ota::IsrPushChunked / ota::DrainRing.
 *
 * Exercises the interrupt-style feeder against a real osp::SpscRingbuffer:
 * chunked push preserves byte order, wake fires once per chunk, short writes
 * are counted as drops, and DrainRing empties the ring through the sink.
 */

#include "osp/spsc_ringbuffer.hpp"
#include "serial_ota/uart_isr.hpp"

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

namespace {

using Ring = osp::SpscRingbuffer<uint8_t, 64>;

}  // namespace

TEST_CASE("IsrPushChunked: preserves byte order and wakes once per chunk", "[uart_isr]") {
  Ring ring;
  const uint8_t in[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  uint32_t wakes = 0U;

  const size_t dropped = ota::IsrPushChunked(ring, in, 10U, 4U, [&wakes]() { ++wakes; });

  REQUIRE(dropped == 0U);
  REQUIRE(wakes == 3U);  // ceil(10 / 4)

  uint8_t out[10] = {};
  REQUIRE(ring.PopBatch(out, 10U) == 10U);
  for (size_t i = 0U; i < 10U; ++i) {
    REQUIRE(out[i] == in[i]);
  }
}

TEST_CASE("IsrPushChunked: exact multiple of chunk size", "[uart_isr]") {
  Ring ring;
  const uint8_t in[8] = {10, 20, 30, 40, 50, 60, 70, 80};
  uint32_t wakes = 0U;

  const size_t dropped = ota::IsrPushChunked(ring, in, 8U, 4U, [&wakes]() { ++wakes; });

  REQUIRE(dropped == 0U);
  REQUIRE(wakes == 2U);
  REQUIRE(ring.Size() == 8U);
}

TEST_CASE("IsrPushChunked: counts dropped bytes when the ring overflows", "[uart_isr]") {
  Ring ring;

  // Fill the ring to leave room for only 8 more bytes.
  uint8_t fill[56] = {};
  REQUIRE(ring.PushBatch(fill, 56U) == 56U);
  REQUIRE(ring.Available() == 8U);

  const uint8_t in[16] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22,
                          0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0x00};
  uint32_t wakes = 0U;

  const size_t dropped = ota::IsrPushChunked(ring, in, 16U, 4U, [&wakes]() { ++wakes; });

  REQUIRE(dropped == 8U);  // 16 - 8 available
  REQUIRE(wakes == 4U);    // ceil(16 / 4): every interrupt, even empty writes
  REQUIRE(ring.IsFull());

  uint8_t head[8] = {};
  REQUIRE(ring.Discard(56U) == 56U);  // skip the pre-fill bytes
  REQUIRE(ring.PopBatch(head, 8U) == 8U);
  for (size_t i = 0U; i < 8U; ++i) {
    REQUIRE(head[i] == in[i]);  // the first 8 source bytes survived
  }
}

TEST_CASE("DrainRing: empties the ring through the sink", "[uart_isr]") {
  Ring ring;
  const uint8_t in[30] = {};
  REQUIRE(ring.PushBatch(in, 30U) == 30U);

  uint8_t buf[16];
  uint32_t sink_calls = 0U;
  size_t sink_bytes = 0U;

  const size_t total = ota::DrainRing(ring, buf, sizeof(buf), [&](const uint8_t*, size_t n) {
    ++sink_calls;
    sink_bytes += n;
  });

  REQUIRE(total == 30U);
  REQUIRE(sink_bytes == 30U);
  REQUIRE(sink_calls >= 2U);  // 30 over a 16-byte scratch buffer = 2 batches
  REQUIRE(ring.IsEmpty());
}

TEST_CASE("DrainRing: empty ring drains zero and never calls the sink", "[uart_isr]") {
  Ring ring;
  uint8_t buf[16];
  uint32_t sink_calls = 0U;

  const size_t total = ota::DrainRing(ring, buf, sizeof(buf), [&](const uint8_t*, size_t) { ++sink_calls; });

  REQUIRE(total == 0U);
  REQUIRE(sink_calls == 0U);
}
