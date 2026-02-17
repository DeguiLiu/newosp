// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// common.hpp -- Shared definitions for data-dispatcher demo.
//
// Defines the LiDAR point cloud frame format, DataDispatcher pool
// configuration, notification message, and POSIX shm helpers.

#ifndef EXAMPLES_DATA_DISPATCHER_COMMON_HPP_
#define EXAMPLES_DATA_DISPATCHER_COMMON_HPP_

#include <cstdint>
#include <cstring>
#include <type_traits>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// LiDAR point cloud frame format
// ---------------------------------------------------------------------------

struct LidarPoint {
  float x;
  float y;
  float z;
  uint8_t intensity;
  uint8_t ring;
  uint8_t pad[2];
};
static_assert(sizeof(LidarPoint) == 16, "LidarPoint must be 16 bytes");

struct LidarFrame {
  uint32_t magic;       // 0x4C494441 ('LIDA')
  uint32_t seq_num;
  uint32_t point_count;
  uint32_t timestamp_ms;
};
static_assert(sizeof(LidarFrame) == 16, "LidarFrame header must be 16 bytes");

static constexpr uint32_t kLidarMagic = 0x4C494441u;
static constexpr uint32_t kPointsPerFrame = 1000;
static constexpr uint32_t kFrameDataSize =
    sizeof(LidarFrame) + kPointsPerFrame * sizeof(LidarPoint);
static constexpr uint32_t kProduceFps = 10;
static constexpr uint32_t kProduceIntervalMs = 1000 / kProduceFps;

// ---------------------------------------------------------------------------
// DataDispatcher pool configuration
// ---------------------------------------------------------------------------

static constexpr uint32_t kPoolMaxBlocks = 32;

// ---------------------------------------------------------------------------
// InterProc shared memory configuration
// ---------------------------------------------------------------------------

static constexpr const char* kPoolShmName = "/osp_dd_pool";
static constexpr const char* kNotifyChannelName = "osp_dd_notify";
static constexpr uint32_t kNotifyCapacity = 4096;
static constexpr uint32_t kNotifyMaxConsumers = 4;
static constexpr uint16_t kDefaultShellPort = 9600;

// ---------------------------------------------------------------------------
// Notification message
// ---------------------------------------------------------------------------

struct NotifyMsg {
  uint32_t block_id;
  uint32_t payload_size;
};
static_assert(sizeof(NotifyMsg) == 8, "NotifyMsg must be 8 bytes");
static_assert(std::is_trivially_copyable<NotifyMsg>::value,
              "NotifyMsg must be trivially copyable");

// ---------------------------------------------------------------------------
// Fill / validate LiDAR frame
// ---------------------------------------------------------------------------

inline void FillLidarFrame(uint8_t* buf, uint32_t seq, uint32_t ts_ms) {
  auto* hdr = reinterpret_cast<LidarFrame*>(buf);
  hdr->magic = kLidarMagic;
  hdr->seq_num = seq;
  hdr->point_count = kPointsPerFrame;
  hdr->timestamp_ms = ts_ms;
  auto* pts = reinterpret_cast<LidarPoint*>(buf + sizeof(LidarFrame));
  for (uint32_t i = 0; i < kPointsPerFrame; ++i) {
    float angle = static_cast<float>(i) * 0.36f;
    pts[i].x = 10.0f + static_cast<float>(seq % 10) + angle * 0.01f;
    pts[i].y = 5.0f + static_cast<float>(i % 100) * 0.1f;
    pts[i].z = 1.5f + static_cast<float>(i % 50) * 0.02f;
    pts[i].intensity = static_cast<uint8_t>((seq + i) & 0xFF);
    pts[i].ring = static_cast<uint8_t>(i % 16);
    pts[i].pad[0] = 0;
    pts[i].pad[1] = 0;
  }
}

inline bool ValidateLidarFrame(const uint8_t* buf, uint32_t len) {
  if (len < sizeof(LidarFrame)) return false;
  auto* hdr = reinterpret_cast<const LidarFrame*>(buf);
  if (hdr->magic != kLidarMagic) return false;
  if (hdr->point_count == 0 || hdr->point_count > 100000) return false;
  uint32_t expected =
      sizeof(LidarFrame) + hdr->point_count * sizeof(LidarPoint);
  return len >= expected;
}

// ---------------------------------------------------------------------------
// POSIX shared memory helpers (for DataDispatcher<ShmStore> pool region)
// ---------------------------------------------------------------------------

inline void* CreatePoolShm(const char* name, uint32_t size) {
  shm_unlink(name);
  int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
  if (fd < 0) return nullptr;
  if (ftruncate(fd, size) < 0) { close(fd); return nullptr; }
  void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  return (ptr == MAP_FAILED) ? nullptr : ptr;
}

inline void* OpenPoolShm(const char* name, uint32_t size) {
  int fd = shm_open(name, O_RDWR, 0666);
  if (fd < 0) return nullptr;
  void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  return (ptr == MAP_FAILED) ? nullptr : ptr;
}

inline void ClosePoolShm(void* ptr, uint32_t size) {
  if (ptr != nullptr) munmap(ptr, size);
}

inline void UnlinkPoolShm(const char* name) { shm_unlink(name); }

#endif  // EXAMPLES_DATA_DISPATCHER_COMMON_HPP_
