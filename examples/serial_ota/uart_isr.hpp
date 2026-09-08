/**
 * @file uart_isr.hpp
 * @brief Interrupt-style feeder helpers for simulated UART channels.
 *
 * Mirrors the libev C++17 uart_ring_hsm example: a producer "ISR" delivers a
 * frame into an SPSC ring in small chunks (one interrupt each), poking the
 * event loop after every chunk, and a consumer drains the ring into a parser.
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 */

#ifndef SERIAL_OTA_UART_ISR_HPP_
#define SERIAL_OTA_UART_ISR_HPP_

#include <cstddef>
#include <cstdint>

namespace ota {

/**
 * @brief Push a byte buffer into an SPSC ring in small "interrupt" chunks.
 *
 * @tparam Ring    An SPSC ring buffer exposing PushBatch(const T*, size_t).
 * @tparam WakeFn  A callable invoked after every chunk (including a chunk
 *                 that could not be written); the loop wakeup / ev_async poke.
 *
 * @param ring        The producer-owned ring.
 * @param data        Source bytes.
 * @param len         Number of source bytes.
 * @param chunk_size  Maximum bytes per "interrupt" (must be > 0).
 * @param wake        Wakeup callback, called once per chunk.
 *
 * @return Number of bytes dropped because the ring had no room.
 */
template <typename Ring, typename WakeFn>
size_t IsrPushChunked(Ring& ring, const uint8_t* data, size_t len, size_t chunk_size, WakeFn&& wake) noexcept {
  size_t dropped = 0U;
  size_t off = 0U;

  while (off < len) {
    const size_t remaining = len - off;
    const size_t n = (remaining > chunk_size) ? chunk_size : remaining;

    const size_t written = ring.PushBatch(data + off, n);
    if (written < n) {
      dropped += (n - written);
    }

    off += n;
    wake();
  }

  return dropped;
}

/**
 * @brief Drain an SPSC ring into a sink callback until it is empty.
 *
 * @tparam Ring  An SPSC ring buffer exposing PopBatch(T*, size_t).
 * @tparam Sink  A callable invoked with (const uint8_t*, size_t) per batch.
 *
 * @param ring     The consumer-owned ring.
 * @param buf      Scatch buffer reused between batches.
 * @param buf_cap  Capacity of @p buf in bytes.
 * @param sink     The parser feed callback.
 *
 * @return Total number of bytes drained.
 */
template <typename Ring, typename Sink>
size_t DrainRing(Ring& ring, uint8_t* buf, size_t buf_cap, Sink&& sink) noexcept {
  size_t total = 0U;

  for (;;) {
    const size_t got = ring.PopBatch(buf, buf_cap);
    if (0U == got) {
      break;
    }
    sink(buf, got);
    total += got;
  }

  return total;
}

}  // namespace ota

#endif  // SERIAL_OTA_UART_ISR_HPP_
