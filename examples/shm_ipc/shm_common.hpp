// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// shm_common.hpp -- Shared definitions for SHM IPC demo.
//
// Frame format, statistics snapshot (trivially_copyable for SpscRingbuffer),
// fault codes, and channel configuration constants.

#ifndef EXAMPLES_SHM_IPC_SHM_COMMON_HPP_
#define EXAMPLES_SHM_IPC_SHM_COMMON_HPP_

#include <cstdint>
#include <type_traits>

// ---------------------------------------------------------------------------
// Frame format
// ---------------------------------------------------------------------------
struct FrameHeader {
  uint32_t magic;
  uint32_t seq_num;
  uint32_t width;
  uint32_t height;
};
static_assert(sizeof(FrameHeader) == 16, "FrameHeader must be 16 bytes");

static constexpr uint32_t kMagic      = 0x4652414Du;  // 'FRAM'
static constexpr uint32_t kWidth      = 320;
static constexpr uint32_t kHeight     = 240;
static constexpr uint32_t kPixelBytes = kWidth * kHeight;
static constexpr uint32_t kFrameSize  = sizeof(FrameHeader) + kPixelBytes;

// ---------------------------------------------------------------------------
// SHM channel configuration
// ---------------------------------------------------------------------------
static constexpr uint32_t kSlotSize   = 81920;  // > kFrameSize (76816)
static constexpr uint32_t kSlotCount  = 16;

// ---------------------------------------------------------------------------
// Statistics snapshot (trivially_copyable for SpscRingbuffer)
// ---------------------------------------------------------------------------
struct ShmStats {
  uint64_t timestamp_us;     // When this snapshot was taken
  uint32_t frames_ok;        // Frames successfully sent/received
  uint32_t frames_bad;       // Frames failed validation (consumer only)
  uint32_t dropped;          // Frames dropped due to ring full (producer)
  uint32_t gaps;             // Sequence gaps detected (consumer)
  uint32_t stall_count;      // Stall events
  uint32_t queue_depth;      // Current ring buffer depth
  float    fps;              // Frames per second (last interval)
  float    mbps;             // Throughput MB/s (last interval)
  uint8_t  role;             // 0 = producer, 1 = consumer
  uint8_t  pad[3];           // Alignment padding
};
static_assert(std::is_trivially_copyable<ShmStats>::value,
              "ShmStats must be trivially copyable for SpscRingbuffer");
static_assert(sizeof(ShmStats) == 48, "ShmStats size check");

// ---------------------------------------------------------------------------
// Fault codes
// ---------------------------------------------------------------------------
namespace FaultCode {
  static constexpr uint16_t kThreadDeath    = 0U;   // Watchdog timeout
  static constexpr uint16_t kRingFull       = 1U;   // Producer ring full
  static constexpr uint16_t kFrameInvalid   = 2U;   // Consumer validation fail
  static constexpr uint16_t kStall          = 3U;   // No data timeout
  static constexpr uint16_t kConnectFail    = 4U;   // Channel open failed
}  // namespace FaultCode

// ---------------------------------------------------------------------------
// SpscRingbuffer stats channel config
// ---------------------------------------------------------------------------
static constexpr uint32_t kStatsRingSize = 16;  // Power of 2

#endif  // EXAMPLES_SHM_IPC_SHM_COMMON_HPP_
