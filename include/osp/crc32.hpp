/**
 * MIT License
 *
 * Copyright (c) 2024 liudegui
 */

/**
 * @file crc32.hpp
 * @brief IEEE 802.3 / zlib CRC-32 (poly 0xEDB88320).
 *
 * Shared by TcpTransport frame integrity and FrameChannel slot checks.
 * Byte-for-byte match with Python zlib.crc32.
 */

#ifndef OSP_CRC32_HPP_
#define OSP_CRC32_HPP_

#include <cstdint>

namespace osp {

namespace crc32_detail {

constexpr uint32_t kCrc32Poly = 0xEDB88320u;

constexpr uint32_t Crc32TableEntry(uint32_t i) noexcept {
  uint32_t c = i;
  for (uint32_t k = 0; k < 8; ++k) {
    c = (c & 1u) ? (kCrc32Poly ^ (c >> 1u)) : (c >> 1u);
  }
  return c;
}

struct Crc32Slice8Table {
  uint32_t v[8][256];
  constexpr Crc32Slice8Table() noexcept : v{} {
    for (uint32_t i = 0; i < 256; ++i) {
      v[0][i] = Crc32TableEntry(i);
    }
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = v[0][i];
      for (uint32_t j = 1; j < 8; ++j) {
        c = v[0][c & 0xFFu] ^ (c >> 8u);
        v[j][i] = c;
      }
    }
  }
};

inline uint32_t Crc32UpdateRaw(uint32_t crc, const uint8_t* data, uint32_t len) noexcept {
  static constexpr Crc32Slice8Table kTable{};
  while (len >= 8u) {
    const uint32_t b0 = data[0];
    const uint32_t b1 = data[1];
    const uint32_t b2 = data[2];
    const uint32_t b3 = data[3];
    const uint32_t x = crc ^ (b0 | (b1 << 8u) | (b2 << 16u) | (b3 << 24u));
    crc = kTable.v[7][x & 0xFFu] ^ kTable.v[6][(x >> 8u) & 0xFFu] ^ kTable.v[5][(x >> 16u) & 0xFFu] ^
          kTable.v[4][(x >> 24u) & 0xFFu] ^ kTable.v[3][data[4]] ^ kTable.v[2][data[5]] ^ kTable.v[1][data[6]] ^
          kTable.v[0][data[7]];
    data += 8;
    len -= 8u;
  }
  while (len > 0u) {
    crc = kTable.v[0][(crc ^ data[0]) & 0xFFu] ^ (crc >> 8u);
    ++data;
    --len;
  }
  return crc;
}

}  // namespace crc32_detail

/// @brief Continue a CRC-32 over [data, data+len). `crc` is the running value after init 0xFFFFFFFF.
inline uint32_t Crc32Update(uint32_t crc, const uint8_t* data, uint32_t len) noexcept {
  if (len == 0U || data == nullptr) {
    return crc;
  }
  return crc32_detail::Crc32UpdateRaw(crc, data, len);
}

/// @brief CRC-32 (zlib-compatible) over [data, data+len).
inline uint32_t Crc32(const uint8_t* data, uint32_t len) noexcept {
  return Crc32Update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

/// @brief Alias kept for FrameChannel call sites.
inline uint32_t FrameCrc32(const uint8_t* data, uint32_t len) noexcept { return Crc32(data, len); }

}  // namespace osp

#endif  // OSP_CRC32_HPP_
