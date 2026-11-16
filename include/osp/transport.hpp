/**
 * MIT License
 *
 * Copyright (c) 2024 liudegui
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file transport.hpp
 * @brief Transparent network transport layer for local/remote node communication.
 *
 * Inspired by ZeroMQ patterns. Provides framed TCP transport with manual
 * serialization, type-safe message forwarding, and NetworkNode integration.
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_TRANSPORT_HPP_
#define OSP_TRANSPORT_HPP_

#include "osp/bus.hpp"
#include "osp/crc32.hpp"
#include "osp/node.hpp"
#include "osp/platform.hpp"
#include "osp/socket.hpp"
#include "osp/span.hpp"
#include "osp/spsc_ringbuffer.hpp"
#include "osp/vocabulary.hpp"

#include <cstdint>
#include <cstring>

#include <type_traits>
#include <variant>

#if OSP_HAS_NETWORK

namespace osp {

// ============================================================================
// Transport Error
// ============================================================================

enum class TransportError : uint8_t {
  kConnectionFailed,
  kBindFailed,
  kSendFailed,
  kRecvFailed,
  kSerializationError,
  kInvalidFrame,
  kBufferFull,
  kNotConnected,
  kWouldBlock,  ///< Kernel send/recv buffer full (EAGAIN). Transient error.
  kPayloadTooLarge  ///< Send payload exceeds OSP_TRANSPORT_MAX_FRAME_SIZE.
};

// ============================================================================
// Endpoint
// ============================================================================

/**
 * @brief Network endpoint descriptor (host + port).
 */
struct Endpoint {
  FixedString<63> host;
  uint16_t port;

  /**
   * @brief Create an Endpoint from a host string and port number.
   * @param addr Host address string (e.g. "127.0.0.1")
   * @param p Port number in host byte order
   * @return Endpoint with the host and port populated.
   */
  static Endpoint FromString(const char* addr, uint16_t p) noexcept {
    Endpoint ep;
    ep.host.assign(TruncateToCapacity, addr);
    ep.port = p;
    return ep;
  }
};

// ============================================================================
// Socket Transport Type
// ============================================================================

enum class SocketTransportType : uint8_t { kTcp = 0, kUdp = 1 };

// ============================================================================
// Frame Protocol Constants
// ============================================================================

/**
 * Frame wire format v0 (14-byte header):
 * +--------+--------+----------+----------+------------------+
 * | magic  | length | type_idx | sender   | payload          |
 * | 4 byte | 4 byte | 2 byte   | 4 byte   | variable         |
 * +--------+--------+----------+----------+------------------+
 *
 * Frame wire format v1 (26-byte header):
 * +--------+--------+----------+----------+----------+-----------+----------+
 * | magic  | length | type_idx | sender   | seq_num  | timestamp | payload  |
 * | 4 byte | 4 byte | 2 byte   | 4 byte   | 4 byte   | 8 byte    | variable |
 * +--------+--------+----------+----------+----------+-----------+----------+
 */
inline constexpr uint32_t kFrameMagicV0 = 0x4F535000;   ///< v0 magic ("OSP\0")
inline constexpr uint32_t kFrameMagicV1 = 0x4F535001;   ///< v1 magic ("OSP\1")
inline constexpr uint32_t kFrameMagicV2 = 0x4F535002;   ///< v2 magic ("OSP\2")
inline constexpr uint32_t kFrameMagic = kFrameMagicV0;  ///< Default (backward compat)

/**
 * @brief Frame header for the wire protocol (v0).
 *
 * Not packed as a struct due to platform padding differences.
 * Use FrameCodec for manual serialization to/from wire format.
 */
struct FrameHeader {
  uint32_t magic;
  uint32_t length;      ///< Payload length in bytes.
  uint16_t type_index;  ///< Variant index of the payload type.
  uint32_t sender_id;   ///< Sender node identifier.
};

/**
 * @brief Extended frame header for the wire protocol (v1).
 *
 * Adds sequence number and timestamp fields for loss/reorder detection.
 */
struct FrameHeaderV1 {
  uint32_t magic;         ///< 0x4F535001
  uint32_t length;        ///< Payload length in bytes.
  uint16_t type_index;    ///< Variant index of the payload type.
  uint32_t sender_id;     ///< Sender node identifier.
  uint32_t seq_num;       ///< Monotonically increasing sequence number.
  uint64_t timestamp_ns;  ///< steady_clock nanoseconds.
};

/**
 * @brief v2 frame header: v1 fields plus a CRC32 covering header (except
 *        magic/crc) and payload. Detects corruption; does not recover order.
 */
struct FrameHeaderV2 {
  uint32_t magic;         ///< 0x4F535002
  uint32_t length;        ///< Payload length in bytes.
  uint16_t type_index;    ///< Variant index of the payload type.
  uint32_t sender_id;     ///< Sender node identifier.
  uint32_t seq_num;       ///< Monotonically increasing sequence number.
  uint64_t timestamp_ns;  ///< steady_clock nanoseconds.
  uint32_t crc32;         ///< IEEE 802.3 CRC of header[4..crc) + payload.
};

// ============================================================================
// Serializer<T> - Default POD serialization
// ============================================================================

/**
 * @brief Default serializer for trivially copyable types (memcpy).
 *
 * Specialize this template for non-trivially-copyable types that need
 * custom wire-format encoding.
 *
 * @tparam T The type to serialize/deserialize.
 */
template <typename T>
struct Serializer {
  static_assert(std::is_trivially_copyable<T>::value, "Default Serializer requires trivially copyable type");

  /**
   * @brief Serialize an object into a buffer.
   * @param obj The object to serialize.
   * @param buf Output buffer.
   * @param buf_size Size of the output buffer in bytes.
   * @return Number of bytes written, or 0 on failure.
   */
  static uint32_t Serialize(const T& obj, void* buf, uint32_t buf_size) noexcept {
    if (buf_size < sizeof(T))
      return 0;
    std::memcpy(buf, &obj, sizeof(T));
    return static_cast<uint32_t>(sizeof(T));
  }

  /**
   * @brief Deserialize an object from a buffer.
   * @param buf Input buffer.
   * @param size Size of the input data in bytes.
   * @param out Output object.
   * @return true on success, false if the buffer is too small.
   */
  static bool Deserialize(const void* buf, uint32_t size, T& out) noexcept {
    if (size < sizeof(T))
      return false;
    std::memcpy(&out, buf, sizeof(T));
    return true;
  }
};

// ============================================================================
// Timestamp Utility
// ============================================================================
// Note: SteadyNowNs() is provided by platform.hpp

// ============================================================================
// SequenceTracker - Sequence Number Tracking
// ============================================================================

/**
 * @brief Tracks sequence numbers for loss/reorder detection.
 *
 * Monitors incoming sequence numbers to detect packet loss, reordering,
 * and duplicates. Assumes monotonically increasing sequence numbers.
 *
 * Detection only: a gap advances expected_seq_ and returns true. A late
 * packet (reorder/duplicate) returns false and is not buffered or delivered.
 * Callers that need recovery must retry at a higher layer.
 */
class SequenceTracker {
 public:
  SequenceTracker() noexcept
      : expected_seq_(0), total_received_(0), lost_count_(0), reordered_count_(0), duplicate_count_(0) {}

  /**
   * @brief Process a received sequence number.
   * @param seq_num The sequence number to track.
   * @return true if in-order, false if reordered or duplicate.
   */
  bool Track(uint32_t seq_num) noexcept {
    ++total_received_;

    // Mod-2^32 wraparound-aware comparison (TCP-style): `diff` is the signed
    // distance from the expected sequence. A small positive diff means the
    // packet is ahead (possibly with loss in the gap); a large diff (>= 2^31)
    // means the packet is behind (reorder/duplicate). This keeps the tracker
    // correct across the UINT32_MAX -> 0 boundary instead of misclassifying a
    // legitimately wrapped stream as a permanent duplicate.
    const uint32_t diff = static_cast<uint32_t>(seq_num - expected_seq_);
    if (diff == 0) {
      // In-order
      ++expected_seq_;
      return true;
    } else if (diff < (1U << 31)) {
      // Gap detected (packet loss): seq_num is ahead of expected.
      lost_count_ += diff;
      expected_seq_ = seq_num + 1;
      return true;
    } else {
      // seq_num is behind expected: reorder or duplicate.
      const uint32_t gap = static_cast<uint32_t>(expected_seq_ - seq_num);
      if (gap <= 1000) {
        // Within reorder window
        ++reordered_count_;
      } else {
        // Too old, likely duplicate
        ++duplicate_count_;
      }
      return false;
    }
  }

  /**
   * @brief Reset all tracking counters.
   */
  void Reset() noexcept {
    expected_seq_ = 0;
    total_received_ = 0;
    lost_count_ = 0;
    reordered_count_ = 0;
    duplicate_count_ = 0;
  }

  /**
   * @brief Set the expected sequence number.
   *
   * Useful for diagnostics and for recovering the tracker to a known point
   * after a peer re-synchronizes (and for test scaffolding).
   */
  void SetExpected(uint32_t seq) noexcept { expected_seq_ = seq; }

  /** @brief Get total number of packets received. */
  uint64_t TotalReceived() const noexcept { return total_received_; }

  /** @brief Get number of lost packets detected. */
  uint64_t LostCount() const noexcept { return lost_count_; }

  /** @brief Get number of reordered packets detected. */
  uint64_t ReorderedCount() const noexcept { return reordered_count_; }

  /** @brief Get number of duplicate packets detected. */
  uint64_t DuplicateCount() const noexcept { return duplicate_count_; }

 private:
  uint32_t expected_seq_;
  uint64_t total_received_;
  uint64_t lost_count_;
  uint64_t reordered_count_;
  uint64_t duplicate_count_;
};

// ============================================================================
// FrameCodec - Manual Header Serialization
// ============================================================================

/// Compile-time receive ring depth per remote subscriber (must be power of 2).

/// @brief Trivially copyable frame slot for SPSC receive ring buffer.
///
/// Stores a complete received frame (header + payload) for deferred dispatch.
/// Used by NetworkNode::ReceiveToBuffer() / DispatchFromBuffer() to decouple
/// TCP I/O from message deserialization and bus publishing.
struct RecvFrameSlot {
  FrameHeader header;
  uint8_t payload[OSP_TRANSPORT_MAX_FRAME_SIZE];
  uint32_t payload_len;
};
static_assert(std::is_trivially_copyable<RecvFrameSlot>::value,
              "RecvFrameSlot must be trivially copyable for SPSC memcpy path");

/**
 * @brief Encodes and decodes frame headers to/from wire format.
 *
 * Uses manual memcpy-based serialization to avoid struct packing issues.
 * Little-endian byte order (native on ARM and x86).
 */
class FrameCodec {
 public:
  static constexpr uint32_t kHeaderSize = 14;    // v0: 4+4+2+4
  static constexpr uint32_t kHeaderSizeV1 = 26;  // v1: 14 + 4 + 8
  static constexpr uint32_t kHeaderSizeV2 = 30;  // v1 + crc32

  /**
   * @brief Encode a FrameHeader (v0) into a byte buffer.
   * @param hdr The header to encode.
   * @param buf Output buffer (must be at least kHeaderSize bytes).
   * @param buf_size Size of the output buffer.
   * @return Number of bytes written (kHeaderSize), or 0 on failure.
   */
  static uint32_t EncodeHeader(const FrameHeader& hdr, uint8_t* buf, uint32_t buf_size) noexcept {
    if (buf_size < kHeaderSize)
      return 0;
    std::memcpy(buf + 0, &hdr.magic, 4);
    std::memcpy(buf + 4, &hdr.length, 4);
    std::memcpy(buf + 8, &hdr.type_index, 2);
    std::memcpy(buf + 10, &hdr.sender_id, 4);
    return kHeaderSize;
  }

  /**
   * @brief Decode a FrameHeader (v0) from a byte buffer.
   * @param buf Input buffer (must be at least kHeaderSize bytes).
   * @param buf_size Size of the input buffer.
   * @param hdr Output header.
   * @return true on success, false if the buffer is too small.
   */
  static bool DecodeHeader(const uint8_t* buf, uint32_t buf_size, FrameHeader& hdr) noexcept {
    if (buf_size < kHeaderSize)
      return false;
    std::memcpy(&hdr.magic, buf + 0, 4);
    std::memcpy(&hdr.length, buf + 4, 4);
    std::memcpy(&hdr.type_index, buf + 8, 2);
    std::memcpy(&hdr.sender_id, buf + 10, 4);
    return true;
  }

  /**
   * @brief Encode a FrameHeaderV1 into a byte buffer.
   * @param hdr The v1 header to encode.
   * @param buf Output buffer (must be at least kHeaderSizeV1 bytes).
   * @param buf_size Size of the output buffer.
   * @return Number of bytes written (kHeaderSizeV1), or 0 on failure.
   */
  static uint32_t EncodeHeaderV1(const FrameHeaderV1& hdr, uint8_t* buf, uint32_t buf_size) noexcept {
    if (buf_size < kHeaderSizeV1)
      return 0;
    std::memcpy(buf + 0, &hdr.magic, 4);
    std::memcpy(buf + 4, &hdr.length, 4);
    std::memcpy(buf + 8, &hdr.type_index, 2);
    std::memcpy(buf + 10, &hdr.sender_id, 4);
    std::memcpy(buf + 14, &hdr.seq_num, 4);
    std::memcpy(buf + 18, &hdr.timestamp_ns, 8);
    return kHeaderSizeV1;
  }

  /**
   * @brief Decode a FrameHeaderV1 from a byte buffer.
   * @param buf Input buffer (must be at least kHeaderSizeV1 bytes).
   * @param buf_size Size of the input buffer.
   * @param hdr Output v1 header.
   * @return true on success, false if the buffer is too small.
   */
  static bool DecodeHeaderV1(const uint8_t* buf, uint32_t buf_size, FrameHeaderV1& hdr) noexcept {
    if (buf_size < kHeaderSizeV1)
      return false;
    std::memcpy(&hdr.magic, buf + 0, 4);
    std::memcpy(&hdr.length, buf + 4, 4);
    std::memcpy(&hdr.type_index, buf + 8, 2);
    std::memcpy(&hdr.sender_id, buf + 10, 4);
    std::memcpy(&hdr.seq_num, buf + 14, 4);
    std::memcpy(&hdr.timestamp_ns, buf + 18, 8);
    return true;
  }

  /**
   * @brief Detect frame version from magic field.
   * @param buf Input buffer (must be at least 4 bytes).
   * @param buf_size Size of the input buffer.
   * @return 0 for v0, 1 for v1, 2 for v2, UINT8_MAX for unknown/invalid.
   */
  static uint8_t DetectVersion(const uint8_t* buf, uint32_t buf_size) noexcept {
    if (buf_size < 4)
      return UINT8_MAX;
    uint32_t magic;
    std::memcpy(&magic, buf, 4);
    if (magic == kFrameMagicV0)
      return 0;
    if (magic == kFrameMagicV1)
      return 1;
    if (magic == kFrameMagicV2)
      return 2;
    return UINT8_MAX;
  }

  static uint32_t EncodeHeaderV2(const FrameHeaderV2& hdr, uint8_t* buf, uint32_t buf_size) noexcept {
    if (buf_size < kHeaderSizeV2)
      return 0;
    std::memcpy(buf + 0, &hdr.magic, 4);
    std::memcpy(buf + 4, &hdr.length, 4);
    std::memcpy(buf + 8, &hdr.type_index, 2);
    std::memcpy(buf + 10, &hdr.sender_id, 4);
    std::memcpy(buf + 14, &hdr.seq_num, 4);
    std::memcpy(buf + 18, &hdr.timestamp_ns, 8);
    std::memcpy(buf + 26, &hdr.crc32, 4);
    return kHeaderSizeV2;
  }

  static bool DecodeHeaderV2(const uint8_t* buf, uint32_t buf_size, FrameHeaderV2& hdr) noexcept {
    if (buf_size < kHeaderSizeV2)
      return false;
    std::memcpy(&hdr.magic, buf + 0, 4);
    std::memcpy(&hdr.length, buf + 4, 4);
    std::memcpy(&hdr.type_index, buf + 8, 2);
    std::memcpy(&hdr.sender_id, buf + 10, 4);
    std::memcpy(&hdr.seq_num, buf + 14, 4);
    std::memcpy(&hdr.timestamp_ns, buf + 18, 8);
    std::memcpy(&hdr.crc32, buf + 26, 4);
    return true;
  }

  /// CRC32 over header bytes after magic and before crc, plus payload.
  static uint32_t Crc32HeaderAndPayload(const uint8_t* hdr, uint32_t hdr_size, const void* payload,
                                        uint32_t payload_len) noexcept {
    uint32_t crc = Crc32Update(0xFFFFFFFFu, hdr + 4, hdr_size - 8U);
    if (payload_len > 0U && payload != nullptr) {
      crc = Crc32Update(crc, static_cast<const uint8_t*>(payload), payload_len);
    }
    return crc ^ 0xFFFFFFFFu;
  }
};

// ============================================================================
// TcpTransport - Framed TCP Connection
// ============================================================================

#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)

/**
 * @brief Manages a single TCP connection for sending/receiving framed messages.
 *
 * Supports both client mode (Connect) and server mode (AcceptFrom).
 * Provides reliable, ordered message framing over a TCP stream.
 */
class TcpTransport {
 public:
  TcpTransport() noexcept : connected_(false) {}

  ~TcpTransport() { Close(); }

  // Move-only
  TcpTransport(TcpTransport&& other) noexcept
      : socket_(static_cast<TcpSocket&&>(other.socket_)),
        connected_(other.connected_),
        send_version_(other.send_version_),
        integrity_unchecked_(other.integrity_unchecked_),
        crc_error_count_(other.crc_error_count_),
        inject_send_budget_(other.inject_send_budget_),
        inject_recv_budget_(other.inject_recv_budget_) {
    other.connected_ = false;
  }

  TcpTransport& operator=(TcpTransport&& other) noexcept {
    if (this != &other) {
      Close();
      socket_ = static_cast<TcpSocket&&>(other.socket_);
      connected_ = other.connected_;
      send_version_ = other.send_version_;
      integrity_unchecked_ = other.integrity_unchecked_;
      crc_error_count_ = other.crc_error_count_;
      inject_send_budget_ = other.inject_send_budget_;
      inject_recv_budget_ = other.inject_recv_budget_;
      other.connected_ = false;
    }
    return *this;
  }

  TcpTransport(const TcpTransport&) = delete;
  TcpTransport& operator=(const TcpTransport&) = delete;

  /**
   * @brief Connect to a remote endpoint (client mode).
   * @param ep The remote endpoint to connect to.
   * @return Success or TransportError.
   */
  expected<void, TransportError> Connect(const Endpoint& ep) noexcept {
    auto sock_r = TcpSocket::Create();
    if (!sock_r.has_value()) {
      return expected<void, TransportError>::error(TransportError::kConnectionFailed);
    }
    socket_ = static_cast<TcpSocket&&>(sock_r.value());

    auto addr_r = SocketAddress::FromIpv4(ep.host.c_str(), ep.port);
    if (!addr_r.has_value()) {
      socket_.Close();
      return expected<void, TransportError>::error(TransportError::kConnectionFailed);
    }

    auto conn_r = socket_.Connect(addr_r.value());
    if (!conn_r.has_value()) {
      socket_.Close();
      return expected<void, TransportError>::error(TransportError::kConnectionFailed);
    }

    // Disable Nagle's algorithm for low-latency framing
    static_cast<void>(socket_.SetNoDelay(true));
    connected_ = true;
    return expected<void, TransportError>::success();
  }

  /**
   * @brief Accept ownership of an already-connected socket (server mode).
   * @param sock A connected TcpSocket (moved in).
   */
  void AcceptFrom(TcpSocket&& sock) noexcept {
    Close();
    socket_ = static_cast<TcpSocket&&>(sock);
    if (socket_.IsValid()) {
      static_cast<void>(socket_.SetNoDelay(true));
      connected_ = true;
    }
  }

  /**
   * @brief Send a framed message over the connection.
   * @param type_index Variant type index of the payload.
   * @param sender_id Sender node identifier.
   * @param payload Pointer to the serialized payload data.
   * @param payload_len Length of the payload in bytes.
   * @return Success or TransportError.
   */
  expected<void, TransportError> SendFrame(uint16_t type_index, uint32_t sender_id, const void* payload,
                                           uint32_t payload_len) noexcept {
    if (!connected_) {
      return expected<void, TransportError>::error(TransportError::kNotConnected);
    }

    if (payload_len > OSP_TRANSPORT_MAX_FRAME_SIZE) {
      return expected<void, TransportError>::error(TransportError::kPayloadTooLarge);
    }

    // Encode header
    FrameHeader hdr;
    hdr.magic = kFrameMagic;
    hdr.length = payload_len;
    hdr.type_index = type_index;
    hdr.sender_id = sender_id;

    uint8_t hdr_buf[FrameCodec::kHeaderSize];
    FrameCodec::EncodeHeader(hdr, hdr_buf, sizeof(hdr_buf));
    return SendFrameBytes(hdr_buf, FrameCodec::kHeaderSize, payload, payload_len);
  }

  /**
   * @brief Send a framed message from a byte view.
   * @param type_index Variant type index of the payload.
   * @param sender_id Sender node identifier.
   * @param payload Serialized payload bytes; the view size is the frame length.
   * @return Success or TransportError.
   */
  expected<void, TransportError> SendFrame(uint16_t type_index, uint32_t sender_id, byte_span payload) noexcept {
    if (payload.size() > OSP_TRANSPORT_MAX_FRAME_SIZE) {
      return expected<void, TransportError>::error(TransportError::kPayloadTooLarge);
    }
    return SendFrame(type_index, sender_id, payload.data(), payload.size_u32());
  }

  /**
   * @brief Receive a framed message (blocking).
   * @param hdr Output frame header.
   * @param payload_buf Buffer to receive the payload into.
   * @param buf_size Size of the payload buffer.
   * @return Number of payload bytes received, or TransportError.
   */
  expected<uint32_t, TransportError> RecvFrame(FrameHeader& hdr, void* payload_buf, uint32_t buf_size) noexcept {
    if (!connected_) {
      return expected<uint32_t, TransportError>::error(TransportError::kNotConnected);
    }

    uint8_t hdr_buf[FrameCodec::kHeaderSize];
    auto r = RecvAll(hdr_buf, FrameCodec::kHeaderSize, false);
    if (!r.has_value())
      return expected<uint32_t, TransportError>::error(r.get_error());

    if (!FrameCodec::DecodeHeader(hdr_buf, FrameCodec::kHeaderSize, hdr) || hdr.magic != kFrameMagic) {
      connected_ = false;
      return expected<uint32_t, TransportError>::error(TransportError::kInvalidFrame);
    }

    return RecvDeclaredPayload(hdr.length, payload_buf, buf_size);
  }

  /**
   * @brief Receive a framed message into a writable byte view.
   * @param hdr Output frame header.
   * @param payload_buf View to receive the payload into; its size is the limit.
   * @return Number of payload bytes received, or TransportError.
   */
  expected<uint32_t, TransportError> RecvFrame(FrameHeader& hdr, writable_byte_span payload_buf) noexcept {
    return RecvFrame(hdr, payload_buf.data(), payload_buf.size_u32());
  }

  /**
   * @brief Send a framed message with v1 header (includes seq_num and timestamp).
   * @param type_index Variant type index of the payload.
   * @param sender_id Sender node identifier.
   * @param seq_num Sequence number.
   * @param timestamp_ns Timestamp in nanoseconds.
   * @param payload Pointer to the serialized payload data.
   * @param payload_len Length of the payload in bytes.
   * @return Success or TransportError.
   */
  expected<void, TransportError> SendFrameV1(uint16_t type_index, uint32_t sender_id, uint32_t seq_num,
                                             uint64_t timestamp_ns, const void* payload,
                                             uint32_t payload_len) noexcept {
    if (!connected_) {
      return expected<void, TransportError>::error(TransportError::kNotConnected);
    }

    if (payload_len > OSP_TRANSPORT_MAX_FRAME_SIZE) {
      return expected<void, TransportError>::error(TransportError::kPayloadTooLarge);
    }

    // Encode v1 header
    FrameHeaderV1 hdr;
    hdr.magic = kFrameMagicV1;
    hdr.length = payload_len;
    hdr.type_index = type_index;
    hdr.sender_id = sender_id;
    hdr.seq_num = seq_num;
    hdr.timestamp_ns = timestamp_ns;

    uint8_t hdr_buf[FrameCodec::kHeaderSizeV1];
    FrameCodec::EncodeHeaderV1(hdr, hdr_buf, sizeof(hdr_buf));
    return SendFrameBytes(hdr_buf, FrameCodec::kHeaderSizeV1, payload, payload_len);
  }

  /**
   * @brief Send a v1 framed message from a byte view.
   * @param type_index Variant type index of the payload.
   * @param sender_id Sender node identifier.
   * @param seq_num Sequence number.
   * @param timestamp_ns Timestamp in nanoseconds.
   * @param payload Serialized payload bytes; the view size is the frame length.
   * @return Success or TransportError.
   */
  expected<void, TransportError> SendFrameV1(uint16_t type_index, uint32_t sender_id, uint32_t seq_num,
                                             uint64_t timestamp_ns, byte_span payload) noexcept {
    if (payload.size() > OSP_TRANSPORT_MAX_FRAME_SIZE) {
      return expected<void, TransportError>::error(TransportError::kPayloadTooLarge);
    }
    return SendFrameV1(type_index, sender_id, seq_num, timestamp_ns, payload.data(), payload.size_u32());
  }

  expected<void, TransportError> SendFrameV2(uint16_t type_index, uint32_t sender_id, uint32_t seq_num,
                                             uint64_t timestamp_ns, const void* payload,
                                             uint32_t payload_len) noexcept {
    if (!connected_) {
      return expected<void, TransportError>::error(TransportError::kNotConnected);
    }
    if (payload_len > OSP_TRANSPORT_MAX_FRAME_SIZE) {
      return expected<void, TransportError>::error(TransportError::kPayloadTooLarge);
    }

    FrameHeaderV2 hdr;
    hdr.magic = kFrameMagicV2;
    hdr.length = payload_len;
    hdr.type_index = type_index;
    hdr.sender_id = sender_id;
    hdr.seq_num = seq_num;
    hdr.timestamp_ns = timestamp_ns;
    hdr.crc32 = 0;

    uint8_t hdr_buf[FrameCodec::kHeaderSizeV2];
    FrameCodec::EncodeHeaderV2(hdr, hdr_buf, sizeof(hdr_buf));
    hdr.crc32 = FrameCodec::Crc32HeaderAndPayload(hdr_buf, FrameCodec::kHeaderSizeV2, payload, payload_len);
    std::memcpy(hdr_buf + 26, &hdr.crc32, 4);
    return SendFrameBytes(hdr_buf, FrameCodec::kHeaderSizeV2, payload, payload_len);
  }

  expected<void, TransportError> SendConfigured(uint16_t type_index, uint32_t sender_id, uint32_t seq_num,
                                                uint64_t timestamp_ns, const void* payload,
                                                uint32_t payload_len) noexcept {
    if (send_version_ == 2U) {
      return SendFrameV2(type_index, sender_id, seq_num, timestamp_ns, payload, payload_len);
    }
    if (send_version_ == 1U) {
      return SendFrameV1(type_index, sender_id, seq_num, timestamp_ns, payload, payload_len);
    }
    return SendFrame(type_index, sender_id, payload, payload_len);
  }

  void SetSendVersion(uint8_t version) noexcept { send_version_ = version; }
  uint8_t SendVersion() const noexcept { return send_version_; }
  uint64_t IntegrityUncheckedCount() const noexcept { return integrity_unchecked_; }
  uint64_t CrcErrorCount() const noexcept { return crc_error_count_; }
  void TestSetSendBudget(uint32_t n) noexcept { inject_send_budget_ = n; }
  void TestSetRecvBudget(uint32_t n) noexcept { inject_recv_budget_ = n; }

  /**
   * @brief Receive a framed message with auto-detection (v0, v1, or v2).
   * @param hdr_v1 Output v1 header (seq_num and timestamp_ns are 0 for v0 frames).
   * @param payload_buf Buffer to receive the payload into.
   * @param buf_size Size of the payload buffer.
   * @return Number of payload bytes received, or TransportError.
   */
  expected<uint32_t, TransportError> RecvFrameAuto(FrameHeaderV1& hdr_v1, void* payload_buf,
                                                   uint32_t buf_size) noexcept {
    if (!connected_) {
      return expected<uint32_t, TransportError>::error(TransportError::kNotConnected);
    }

    uint8_t magic_buf[4];
    auto r = RecvAll(magic_buf, 4, false);
    if (!r.has_value())
      return expected<uint32_t, TransportError>::error(r.get_error());

    const uint8_t version = FrameCodec::DetectVersion(magic_buf, 4);
    if (version == UINT8_MAX) {
      connected_ = false;
      return expected<uint32_t, TransportError>::error(TransportError::kInvalidFrame);
    }

    if (version == 0) {
      uint8_t hdr_buf[FrameCodec::kHeaderSize];
      std::memcpy(hdr_buf, magic_buf, 4);
      auto r2 = RecvAll(hdr_buf + 4, FrameCodec::kHeaderSize - 4, true);
      if (!r2.has_value())
        return expected<uint32_t, TransportError>::error(r2.get_error());

      FrameHeader hdr;
      if (!FrameCodec::DecodeHeader(hdr_buf, FrameCodec::kHeaderSize, hdr) || hdr.magic != kFrameMagicV0) {
        connected_ = false;
        return expected<uint32_t, TransportError>::error(TransportError::kInvalidFrame);
      }

      auto pr = RecvDeclaredPayload(hdr.length, payload_buf, buf_size);
      if (!pr.has_value())
        return pr;

      ++integrity_unchecked_;
      hdr_v1.magic = hdr.magic;
      hdr_v1.length = hdr.length;
      hdr_v1.type_index = hdr.type_index;
      hdr_v1.sender_id = hdr.sender_id;
      hdr_v1.seq_num = 0;
      hdr_v1.timestamp_ns = 0;
      return expected<uint32_t, TransportError>::success(hdr.length);
    }

    if (version == 1) {
      uint8_t hdr_buf[FrameCodec::kHeaderSizeV1];
      std::memcpy(hdr_buf, magic_buf, 4);
      auto r2 = RecvAll(hdr_buf + 4, FrameCodec::kHeaderSizeV1 - 4, true);
      if (!r2.has_value())
        return expected<uint32_t, TransportError>::error(r2.get_error());

      if (!FrameCodec::DecodeHeaderV1(hdr_buf, FrameCodec::kHeaderSizeV1, hdr_v1) || hdr_v1.magic != kFrameMagicV1) {
        connected_ = false;
        return expected<uint32_t, TransportError>::error(TransportError::kInvalidFrame);
      }

      auto pr = RecvDeclaredPayload(hdr_v1.length, payload_buf, buf_size);
      if (!pr.has_value())
        return pr;

      ++integrity_unchecked_;
      return expected<uint32_t, TransportError>::success(hdr_v1.length);
    }

    uint8_t hdr_buf[FrameCodec::kHeaderSizeV2];
    std::memcpy(hdr_buf, magic_buf, 4);
    auto r2 = RecvAll(hdr_buf + 4, FrameCodec::kHeaderSizeV2 - 4, true);
    if (!r2.has_value())
      return expected<uint32_t, TransportError>::error(r2.get_error());

    FrameHeaderV2 hdr_v2;
    if (!FrameCodec::DecodeHeaderV2(hdr_buf, FrameCodec::kHeaderSizeV2, hdr_v2) || hdr_v2.magic != kFrameMagicV2) {
      connected_ = false;
      return expected<uint32_t, TransportError>::error(TransportError::kInvalidFrame);
    }

    auto pr = RecvDeclaredPayload(hdr_v2.length, payload_buf, buf_size);
    if (!pr.has_value())
      return pr;

    const uint32_t got = FrameCodec::Crc32HeaderAndPayload(hdr_buf, FrameCodec::kHeaderSizeV2, payload_buf,
                                                           hdr_v2.length);
    if (got != hdr_v2.crc32) {
      connected_ = false;
      ++crc_error_count_;
      return expected<uint32_t, TransportError>::error(TransportError::kInvalidFrame);
    }

    hdr_v1.magic = hdr_v2.magic;
    hdr_v1.length = hdr_v2.length;
    hdr_v1.type_index = hdr_v2.type_index;
    hdr_v1.sender_id = hdr_v2.sender_id;
    hdr_v1.seq_num = hdr_v2.seq_num;
    hdr_v1.timestamp_ns = hdr_v2.timestamp_ns;
    return expected<uint32_t, TransportError>::success(hdr_v2.length);
  }

  /** @brief Check if the transport is connected. */
  bool IsConnected() const noexcept { return connected_; }

  /** @brief Close the transport connection. */
  void Close() noexcept {
    socket_.Close();
    connected_ = false;
  }

 private:
  expected<void, TransportError> SendFrameBytes(const uint8_t* hdr, uint32_t hdr_len, const void* payload,
                                                uint32_t payload_len) noexcept {
    auto r = SendAll(hdr, hdr_len, false);
    if (!r.has_value())
      return r;
    if (payload_len > 0U && payload != nullptr) {
      return SendAll(payload, payload_len, true);
    }
    return expected<void, TransportError>::success();
  }

  expected<uint32_t, TransportError> RecvDeclaredPayload(uint32_t length, void* payload_buf,
                                                         uint32_t buf_size) noexcept {
    if (length > OSP_TRANSPORT_MAX_FRAME_SIZE) {
      connected_ = false;
      return expected<uint32_t, TransportError>::error(TransportError::kInvalidFrame);
    }
    if (length > buf_size) {
      // Caller buffer too small: keep the connection and report kBufferFull so
      // the caller can retry with a larger buffer (pre-refactor behavior).
      return expected<uint32_t, TransportError>::error(TransportError::kBufferFull);
    }
    if (length > 0U) {
      auto pr = RecvAll(payload_buf, length, true);
      if (!pr.has_value())
        return expected<uint32_t, TransportError>::error(pr.get_error());
    }
    return expected<uint32_t, TransportError>::success(length);
  }

  expected<void, TransportError> SendAll(const void* data, uint32_t len) noexcept { return SendAll(data, len, false); }

  /**
   * @brief Send exactly len bytes. If this call or the current frame has already
   *        put any byte on the wire, kWouldBlock becomes kSendFailed and the
   *        connection is closed so the stream cannot desynchronize.
   */
  expected<void, TransportError> SendAll(const void* data, uint32_t len, bool frame_started) noexcept {
    static constexpr uint32_t kMaxEagainRetries = 16;
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    uint32_t remaining = len;
    uint32_t eagain_count = 0;
    uint32_t sent_this_call = 0;
    while (remaining > 0) {
      uint32_t want = remaining;
      if (inject_send_budget_ != 0xFFFFFFFFu) {
        if (inject_send_budget_ == 0U) {
          if (sent_this_call > 0U || frame_started) {
            connected_ = false;
            return expected<void, TransportError>::error(TransportError::kSendFailed);
          }
          return expected<void, TransportError>::error(TransportError::kWouldBlock);
        }
        if (want > inject_send_budget_)
          want = inject_send_budget_;
      }
      auto r = socket_.Send(ptr, want);
      if (!r.has_value()) {
        if (r.get_error() == SocketError::kWouldBlock) {
          if (++eagain_count > kMaxEagainRetries) {
            if (sent_this_call > 0U || frame_started) {
              connected_ = false;
              return expected<void, TransportError>::error(TransportError::kSendFailed);
            }
            return expected<void, TransportError>::error(TransportError::kWouldBlock);
          }
          osp::ThreadYield();
          continue;
        }
        connected_ = false;
        return expected<void, TransportError>::error(TransportError::kSendFailed);
      }
      if (r.value() == 0) {
        connected_ = false;
        return expected<void, TransportError>::error(TransportError::kSendFailed);
      }
      uint32_t sent = static_cast<uint32_t>(r.value());
      if (inject_send_budget_ != 0xFFFFFFFFu)
        inject_send_budget_ -= sent;
      ptr += sent;
      remaining -= sent;
      sent_this_call += sent;
      eagain_count = 0;
    }
    return expected<void, TransportError>::success();
  }

  expected<void, TransportError> RecvAll(void* data, uint32_t len) noexcept { return RecvAll(data, len, false); }

  expected<void, TransportError> RecvAll(void* data, uint32_t len, bool frame_started) noexcept {
    static constexpr uint32_t kMaxEagainRetries = 16;
    uint8_t* ptr = static_cast<uint8_t*>(data);
    uint32_t remaining = len;
    uint32_t eagain_count = 0;
    uint32_t recvd_this_call = 0;
    while (remaining > 0) {
      uint32_t want = remaining;
      if (inject_recv_budget_ != 0xFFFFFFFFu) {
        if (inject_recv_budget_ == 0U) {
          if (recvd_this_call > 0U || frame_started) {
            connected_ = false;
            return expected<void, TransportError>::error(TransportError::kRecvFailed);
          }
          return expected<void, TransportError>::error(TransportError::kWouldBlock);
        }
        if (want > inject_recv_budget_)
          want = inject_recv_budget_;
      }
      auto r = socket_.Recv(ptr, want);
      if (!r.has_value()) {
        if (r.get_error() == SocketError::kWouldBlock) {
          if (++eagain_count > kMaxEagainRetries) {
            if (recvd_this_call > 0U || frame_started) {
              connected_ = false;
              return expected<void, TransportError>::error(TransportError::kRecvFailed);
            }
            return expected<void, TransportError>::error(TransportError::kWouldBlock);
          }
          osp::ThreadYield();
          continue;
        }
        connected_ = false;
        return expected<void, TransportError>::error(TransportError::kRecvFailed);
      }
      if (r.value() == 0) {
        connected_ = false;
        return expected<void, TransportError>::error(TransportError::kRecvFailed);
      }
      uint32_t received = static_cast<uint32_t>(r.value());
      if (inject_recv_budget_ != 0xFFFFFFFFu)
        inject_recv_budget_ -= received;
      ptr += received;
      remaining -= received;
      recvd_this_call += received;
      eagain_count = 0;
    }
    return expected<void, TransportError>::success();
  }

  expected<void, TransportError> SendAll(byte_span data) noexcept { return SendAll(data.data(), data.size_u32()); }

  expected<void, TransportError> RecvAll(writable_byte_span data) noexcept {
    return RecvAll(data.data(), data.size_u32());
  }

  TcpSocket socket_;
  bool connected_;
  uint8_t send_version_{0};
  uint64_t integrity_unchecked_{0};
  uint64_t crc_error_count_{0};
  uint32_t inject_send_budget_{0xFFFFFFFFu};
  uint32_t inject_recv_budget_{0xFFFFFFFFu};
};



// ============================================================================
// NetworkNode<PayloadVariant> - Node with Remote Pub/Sub
// ============================================================================

/**
 * @brief Extends Node with transparent remote publish/subscribe over TCP.
 *
 * Allows nodes on different machines to exchange typed messages over the
 * network. Uses TcpTransport for framing and Serializer<T> for wire encoding.
 *
 * @tparam PayloadVariant A std::variant<...> of user-defined message types.
 */
template <typename PayloadVariant>
class NetworkNode : public Node<PayloadVariant> {
 public:
  using BusType = AsyncBus<PayloadVariant>;
  using EnvelopeType = MessageEnvelope<PayloadVariant>;

  /**
   * @brief Construct a named network node.
   * @param name Node name (truncated to kNodeNameMaxLen chars).
   * @param id Unique sender ID for message tracing.
   */
  explicit NetworkNode(const char* name, uint32_t id = 0) noexcept : Node<PayloadVariant>(name, id), listening_(false) {
    for (uint32_t i = 0; i < kMaxRemoteConnections; ++i) {
      publishers_[i].active.store(false, std::memory_order_relaxed);
      publishers_[i].type_index = 0;
      subscribers_[i].active.store(false, std::memory_order_relaxed);
      subscribers_[i].type_index = 0;
    }
  }

  ~NetworkNode() {
    for (uint32_t i = 0; i < kMaxRemoteConnections; ++i) {
      if (publishers_[i].active.load(std::memory_order_acquire)) {
        publishers_[i].transport.Close();
        publishers_[i].active.store(false, std::memory_order_relaxed);
      }
      if (subscribers_[i].active.load(std::memory_order_acquire)) {
        subscribers_[i].transport.Close();
        subscribers_[i].active.store(false, std::memory_order_relaxed);
      }
    }
    listener_.Close();
  }

  NetworkNode(const NetworkNode&) = delete;
  NetworkNode& operator=(const NetworkNode&) = delete;
  NetworkNode(NetworkNode&&) = delete;
  NetworkNode& operator=(NetworkNode&&) = delete;

  /**
   * @brief Advertise messages of type T to a remote endpoint.
   *
   * Connects to the remote endpoint and registers a local subscription
   * that forwards matching messages over the TCP transport.
   *
   * @tparam T The message type to forward remotely.
   * @param ep The remote endpoint to connect to.
   * @return Success or TransportError.
   */
  template <typename T>
  expected<void, TransportError> AdvertiseTo(const Endpoint& ep) noexcept {
    if (pub_count_.load(std::memory_order_relaxed) >= kMaxRemoteConnections) {
      return expected<void, TransportError>::error(TransportError::kBufferFull);
    }

    constexpr uint16_t type_idx = static_cast<uint16_t>(VariantIndex<T, PayloadVariant>::value);

    // Find a free slot
    uint32_t slot = kMaxRemoteConnections;
    for (uint32_t i = 0; i < kMaxRemoteConnections; ++i) {
      if (!publishers_[i].active.load(std::memory_order_acquire)) {
        slot = i;
        break;
      }
    }
    if (slot >= kMaxRemoteConnections) {
      return expected<void, TransportError>::error(TransportError::kBufferFull);
    }

    auto r = publishers_[slot].transport.Connect(ep);
    if (!r.has_value())
      return r;

    publishers_[slot].type_index = type_idx;
    publishers_[slot].next_seq = 0;
    publishers_[slot].transport.SetSendVersion(send_version_);
    const uint32_t generation = publishers_[slot].generation.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    publishers_[slot].active.store(true, std::memory_order_release);
    pub_count_.fetch_add(1U, std::memory_order_relaxed);

    RemotePublisher* pub_ptr = &publishers_[slot];
    uint32_t sender_id = this->Id();
    std::atomic<uint32_t>* pub_count = &pub_count_;

    auto sub_r = this->template Subscribe<T>([pub_ptr, generation, sender_id, pub_count](
                                                 const T& data, const MessageHeader& /*hdr*/) {
      // A slot reused by a later AdvertiseTo must not revive this callback.
      if (!pub_ptr->active.load(std::memory_order_acquire) ||
          pub_ptr->generation.load(std::memory_order_acquire) != generation) {
        return;
      }

      uint8_t payload_buf[OSP_TRANSPORT_MAX_FRAME_SIZE];
      uint32_t len = Serializer<T>::Serialize(data, payload_buf, sizeof(payload_buf));
      if (len == 0)
        return;

      const uint32_t seq = pub_ptr->next_seq;
      pub_ptr->next_seq = seq + 1U;
      auto send_r = pub_ptr->transport.SendConfigured(pub_ptr->type_index, sender_id, seq, SteadyNowNs(), payload_buf,
                                                      len);
      if (!send_r.has_value()) {
        pub_ptr->transport.Close();
        bool was_active = true;
        if (pub_ptr->active.compare_exchange_strong(was_active, false, std::memory_order_release,
                                                    std::memory_order_relaxed)) {
          pub_count->fetch_sub(1U, std::memory_order_relaxed);
        }
      }
    });

    if (!sub_r.has_value()) {
      publishers_[slot].transport.Close();
      publishers_[slot].active.store(false, std::memory_order_release);
      pub_count_.fetch_sub(1U, std::memory_order_relaxed);
      return expected<void, TransportError>::error(TransportError::kConnectionFailed);
    }

    return expected<void, TransportError>::success();
  }

  /**
   * @brief Subscribe to messages of type T from a remote endpoint.
   *
   * Connects to the remote endpoint. Use ProcessRemote() to poll for
   * incoming messages and inject them into the local bus.
   *
   * @tparam T The message type to receive remotely.
   * @param ep The remote endpoint to connect to.
   * @return Success or TransportError.
   */
  template <typename T>
  expected<void, TransportError> SubscribeFrom(const Endpoint& ep) noexcept {
    if (sub_count_.load(std::memory_order_relaxed) >= kMaxRemoteConnections) {
      return expected<void, TransportError>::error(TransportError::kBufferFull);
    }

    constexpr uint16_t type_idx = static_cast<uint16_t>(VariantIndex<T, PayloadVariant>::value);

    // Find a free slot
    uint32_t slot = kMaxRemoteConnections;
    for (uint32_t i = 0; i < kMaxRemoteConnections; ++i) {
      if (!subscribers_[i].active.load(std::memory_order_acquire)) {
        slot = i;
        break;
      }
    }
    if (slot >= kMaxRemoteConnections) {
      return expected<void, TransportError>::error(TransportError::kBufferFull);
    }

    auto r = subscribers_[slot].transport.Connect(ep);
    if (!r.has_value())
      return r;

    subscribers_[slot].type_index = type_idx;
    subscribers_[slot].active.store(true, std::memory_order_release);
    sub_count_.fetch_add(1U, std::memory_order_relaxed);

    return expected<void, TransportError>::success();
  }

  /**
   * @brief Poll all remote subscribers and receive frames into ring buffers.
   *
   * Call from the I/O thread. Receives at most one frame per subscriber per
   * call. Disconnected subscribers are automatically deactivated.
   * When a subscriber's recv_ring is full, the frame is dropped and
   * recv_ring_drops_ is incremented.
   *
   * @return Number of frames buffered.
   */
  uint32_t ReceiveToBuffer() noexcept {
    uint32_t buffered = 0;

    for (uint32_t i = 0; i < kMaxRemoteConnections; ++i) {
      if (!subscribers_[i].active.load(std::memory_order_acquire))
        continue;
      if (!subscribers_[i].transport.IsConnected()) {
        subscribers_[i].active.store(false, std::memory_order_release);
        sub_count_.fetch_sub(1U, std::memory_order_relaxed);
        continue;
      }

      RecvFrameSlot slot;
      FrameHeaderV1 hdr_v1{};
      auto r = subscribers_[i].transport.RecvFrameAuto(hdr_v1, slot.payload, sizeof(slot.payload));
      if (!r.has_value()) {
        if (r.get_error() == TransportError::kWouldBlock)
          continue;
        subscribers_[i].transport.Close();
        subscribers_[i].active.store(false, std::memory_order_release);
        sub_count_.fetch_sub(1U, std::memory_order_relaxed);
        continue;
      }

      slot.header.magic = hdr_v1.magic;
      slot.header.length = hdr_v1.length;
      slot.header.type_index = hdr_v1.type_index;
      slot.header.sender_id = hdr_v1.sender_id;
      slot.payload_len = r.value();
      if (subscribers_[i].recv_ring.Push(slot)) {
        ++buffered;
      } else {
        // Ring buffer full -- frame is dropped.
        recv_ring_drops_.fetch_add(1U, std::memory_order_relaxed);
      }
    }

    return buffered;
  }

  /**
   * @brief Dispatch all buffered frames from ring buffers to the local bus.
   *
   * Call from the dispatch thread. Drains each subscriber's recv_ring and
   * deserializes + publishes each frame.
   *
   * @return Number of messages dispatched.
   */
  uint32_t DispatchFromBuffer() noexcept {
    uint32_t processed = 0;

    for (uint32_t i = 0; i < kMaxRemoteConnections; ++i) {
      if (!subscribers_[i].active.load(std::memory_order_acquire))
        continue;

      RecvFrameSlot slot;
      while (subscribers_[i].recv_ring.Pop(slot)) {
        if (DeserializeAndPublish(slot.header.type_index, slot.header.sender_id, slot.payload, slot.payload_len)) {
          ++processed;
        }
      }
    }

    return processed;
  }

  /**
   * @brief Process incoming data from all remote subscribers (single-threaded).
   *
   * Convenience method that calls ReceiveToBuffer() + DispatchFromBuffer()
   * sequentially. For multi-threaded use, call them separately from
   * different threads.
   *
   * @return Number of messages processed from remote sources.
   */
  uint32_t ProcessRemote() noexcept {
    ReceiveToBuffer();
    return DispatchFromBuffer();
  }

  /**
   * @brief Start listening for incoming connections on the given port.
   * @param port TCP port to listen on (0 for OS-assigned).
   * @return Success or TransportError.
   */
  expected<void, TransportError> Listen(uint16_t port) noexcept {
    auto listener_r = TcpListener::Create();
    if (!listener_r.has_value()) {
      return expected<void, TransportError>::error(TransportError::kBindFailed);
    }
    listener_ = static_cast<TcpListener&&>(listener_r.value());

    // Set SO_REUSEADDR
    int32_t opt = 1;
    (void)socket_api::SetSockOpt(listener_.Fd(), SOL_SOCKET, SO_REUSEADDR, &opt, static_cast<socklen_t>(sizeof(opt)));

    auto addr_r = SocketAddress::FromIpv4("0.0.0.0", port);
    if (!addr_r.has_value()) {
      listener_.Close();
      return expected<void, TransportError>::error(TransportError::kBindFailed);
    }

    auto bind_r = listener_.Bind(addr_r.value());
    if (!bind_r.has_value()) {
      listener_.Close();
      return expected<void, TransportError>::error(TransportError::kBindFailed);
    }

    auto listen_r = listener_.Listen(8);
    if (!listen_r.has_value()) {
      listener_.Close();
      return expected<void, TransportError>::error(TransportError::kBindFailed);
    }

    listening_ = true;
    return expected<void, TransportError>::success();
  }

  /**
   * @brief Accept one incoming connection and store it as a subscriber.
   * @return Success or TransportError.
   */
  expected<void, TransportError> AcceptOne() noexcept {
    if (!listening_ || !listener_.IsValid()) {
      return expected<void, TransportError>::error(TransportError::kNotConnected);
    }

    auto accept_r = listener_.Accept();
    if (!accept_r.has_value()) {
      return expected<void, TransportError>::error(TransportError::kConnectionFailed);
    }

    // Find a free subscriber slot
    for (uint32_t i = 0; i < kMaxRemoteConnections; ++i) {
      if (!subscribers_[i].active.load(std::memory_order_acquire)) {
        subscribers_[i].transport.AcceptFrom(static_cast<TcpSocket&&>(accept_r.value()));
        subscribers_[i].type_index = 0;  // Will be determined by frame data
        subscribers_[i].active.store(true, std::memory_order_release);
        sub_count_.fetch_add(1U, std::memory_order_relaxed);
        return expected<void, TransportError>::success();
      }
    }

    // No free slots
    return expected<void, TransportError>::error(TransportError::kBufferFull);
  }

  /** @brief Get the listener's file descriptor (for getsockname). */
  int ListenerFd() const noexcept { return listener_.Fd(); }

  /** @brief Check if the node is listening for connections. */
  bool IsListening() const noexcept { return listening_; }

  /** @brief Get the number of active remote publishers. */
  uint32_t RemotePublisherCount() const noexcept { return pub_count_.load(std::memory_order_relaxed); }

  /** @brief Get the number of active remote subscribers. */
  uint32_t RemoteSubscriberCount() const noexcept { return sub_count_.load(std::memory_order_relaxed); }

  /** @brief Get the number of frames dropped due to full recv_ring. */
  uint64_t RecvRingDrops() const noexcept { return recv_ring_drops_.load(std::memory_order_relaxed); }

  void SetSendVersion(uint8_t version) noexcept {
    send_version_ = version;
    for (uint32_t i = 0; i < kMaxRemoteConnections; ++i) {
      publishers_[i].transport.SetSendVersion(version);
    }
  }

 private:
  static constexpr uint32_t kMaxRemoteConnections = 8;

  struct RemotePublisher {
    TcpTransport transport;
    uint16_t type_index;
    uint32_t next_seq{0};
    std::atomic<uint32_t> generation{0};
    std::atomic<bool> active{false};
  };

  struct RemoteSubscriber {
    TcpTransport transport;
    SpscRingbuffer<RecvFrameSlot, OSP_TRANSPORT_RECV_RING_DEPTH> recv_ring;
    uint16_t type_index;
    std::atomic<bool> active{false};
  };

  /**
   * @brief Deserialize payload and publish to local bus based on type_index.
   *
   * Uses a compile-time visitor over the variant alternatives to find the
   * matching type for deserialization.
   */
  bool DeserializeAndPublish(uint16_t type_index, uint32_t sender_id, const void* data, uint32_t len) noexcept {
    return DeserializeAtIndex<0>(type_index, sender_id, data, len);
  }

  /**
   * @brief Recursive template to match type_index to a variant alternative.
   */
  template <size_t I>
  typename std::enable_if<(I < std::variant_size<PayloadVariant>::value), bool>::type DeserializeAtIndex(
      uint16_t type_index, uint32_t sender_id, const void* data, uint32_t len) noexcept {
    if (type_index == static_cast<uint16_t>(I)) {
      using T = typename std::variant_alternative<I, PayloadVariant>::type;
      T obj;
      if (!Serializer<T>::Deserialize(data, len, obj))
        return false;
      BusType::Instance().Publish(PayloadVariant(obj), sender_id);
      return true;
    }
    return DeserializeAtIndex<I + 1>(type_index, sender_id, data, len);
  }

  template <size_t I>
  typename std::enable_if<(I >= std::variant_size<PayloadVariant>::value), bool>::type DeserializeAtIndex(
      uint16_t /*type_index*/, uint32_t /*sender_id*/, const void* /*data*/, uint32_t /*len*/) noexcept {
    return false;
  }

  RemotePublisher publishers_[kMaxRemoteConnections];
  RemoteSubscriber subscribers_[kMaxRemoteConnections];
  std::atomic<uint32_t> pub_count_{0};
  std::atomic<uint32_t> sub_count_{0};
  std::atomic<uint64_t> recv_ring_drops_{0};
  TcpListener listener_;
  bool listening_;
  uint8_t send_version_{0};
};

#endif  // defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)

}  // namespace osp

#endif  // OSP_HAS_NETWORK

#endif  // OSP_TRANSPORT_HPP_
