// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// common.hpp -- Shared definitions for data-visitor-dispatcher demo.
//
// Defines the LiDAR point cloud frame format, channel configuration,
// and statistics structures shared between producer, visitors, and monitor.

#ifndef EXAMPLES_DATA_VISITOR_DISPATCHER_COMMON_HPP_
#define EXAMPLES_DATA_VISITOR_DISPATCHER_COMMON_HPP_

#include <cstdint>
#include <type_traits>

// ---------------------------------------------------------------------------
// LiDAR point cloud frame format
// ---------------------------------------------------------------------------

struct LidarPoint {
  float x;
  float y;
  float z;
  uint8_t intensity;
  uint8_t ring;        // Laser ring index
  uint8_t pad[2];
};
static_assert(sizeof(LidarPoint) == 16, "LidarPoint must be 16 bytes");

struct LidarFrame {
  uint32_t magic;       // 0x4C494441 ('LIDA')
  uint32_t seq_num;
  uint32_t point_count;
  uint32_t timestamp_ms;
  // Followed by point_count * LidarPoint
};
static_assert(sizeof(LidarFrame) == 16, "LidarFrame header must be 16 bytes");

static constexpr uint32_t kLidarMagic = 0x4C494441u;  // 'LIDA'

// Simulated LiDAR config
static constexpr uint32_t kPointsPerFrame = 1000;   // 1000 points per scan
static constexpr uint32_t kFrameDataSize = sizeof(LidarFrame) + kPointsPerFrame * sizeof(LidarPoint);
static constexpr uint32_t kProduceFps = 10;          // 10 Hz scan rate
static constexpr uint32_t kProduceIntervalMs = 1000 / kProduceFps;

// ---------------------------------------------------------------------------
// SPMC channel configuration
// ---------------------------------------------------------------------------

static constexpr uint32_t kSpmcCapacity = 256 * 1024;  // 256 KB ring buffer
static constexpr uint32_t kSpmcMaxConsumers = 4;
static constexpr const char* kDefaultChannelName = "osp_lidar_spmc";
static constexpr uint16_t kDefaultShellPort = 9600;

// ---------------------------------------------------------------------------
// Visitor statistics (for monitor display)
// ---------------------------------------------------------------------------

struct VisitorStats {
  uint64_t timestamp_us;
  uint32_t frames_received;
  uint32_t frames_dropped;
  uint32_t last_seq;
  float fps;
  uint8_t visitor_id;
  uint8_t pad[3];
};
static_assert(std::is_trivially_copyable<VisitorStats>::value,
              "VisitorStats must be trivially copyable");

// ---------------------------------------------------------------------------
// Fill a simulated LiDAR frame
// ---------------------------------------------------------------------------

inline void FillLidarFrame(uint8_t* buf, uint32_t seq, uint32_t timestamp_ms) {
  auto* hdr = reinterpret_cast<LidarFrame*>(buf);
  hdr->magic = kLidarMagic;
  hdr->seq_num = seq;
  hdr->point_count = kPointsPerFrame;
  hdr->timestamp_ms = timestamp_ms;

  auto* points = reinterpret_cast<LidarPoint*>(buf + sizeof(LidarFrame));
  for (uint32_t i = 0; i < kPointsPerFrame; ++i) {
    float angle = static_cast<float>(i) * 0.36f;  // 360 deg / 1000 points
    points[i].x = 10.0f + static_cast<float>(seq % 10) + angle * 0.01f;
    points[i].y = 5.0f + static_cast<float>(i % 100) * 0.1f;
    points[i].z = 1.5f + static_cast<float>(i % 50) * 0.02f;
    points[i].intensity = static_cast<uint8_t>((seq + i) & 0xFF);
    points[i].ring = static_cast<uint8_t>(i % 16);
    points[i].pad[0] = 0;
    points[i].pad[1] = 0;
  }
}

// ---------------------------------------------------------------------------
// Validate a LiDAR frame
// ---------------------------------------------------------------------------

inline bool ValidateLidarFrame(const uint8_t* buf, uint32_t len) {
  if (len < sizeof(LidarFrame)) return false;
  auto* hdr = reinterpret_cast<const LidarFrame*>(buf);
  if (hdr->magic != kLidarMagic) return false;
  if (hdr->point_count == 0 || hdr->point_count > 100000) return false;
  uint32_t expected_size = sizeof(LidarFrame) + hdr->point_count * sizeof(LidarPoint);
  return len >= expected_size;
}

#endif  // EXAMPLES_DATA_VISITOR_DISPATCHER_COMMON_HPP_
