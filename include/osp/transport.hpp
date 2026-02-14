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

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"
#include "osp/socket.hpp"
#include "osp/bus.hpp"
#include "osp/node.hpp"
#include "osp/spsc_ringbuffer.hpp"

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <variant>

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
  kNotConnected
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

enum class SocketTransportType : uint8_t {
  kTcp = 0,
  kUdp = 1
};

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
inline constexpr uint32_t kFrameMagicV0 = 0x4F535000;  ///< v0 magic ("OSP\0")
inline constexpr uint32_t kFrameMagicV1 = 0x4F535001;  ///< v1 magic ("OSP\1")
inline constexpr uint32_t kFrameMagic = kFrameMagicV0; ///< Default (backward compat)

/**
 * @brief Frame header for the wire protocol (v0).
 *
 * Not packed as a struct due to platform padding differences.
 * Use FrameCodec for manual serialization to/from wire format.
 */
struct FrameHeader {
  uint32_t magic;
  uint32_t length;       ///< Payload length in bytes.
  uint16_t type_index;   ///< Variant index of the payload type.
  uint32_t sender_id;    ///< Sender node identifier.
};

/**
 * @brief Extended frame header for the wire protocol (v1).
 *
 * Adds sequence number and timestamp fields for loss/reorder detection.
 */
struct FrameHeaderV1 {
  uint32_t magic;        ///< 0x4F535001
  uint32_t length;       ///< Payload length in bytes.
  uint16_t type_index;   ///< Variant index of the payload type.
  uint32_t sender_id;    ///< Sender node identifier.
  uint32_t seq_num;      ///< Monotonically increasing sequence number.
  uint64_t timestamp_ns; ///< steady_clock nanoseconds.
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
  static_assert(std::is_trivially_copyable<T>::value,
                "Default Serializer requires trivially copyable type");

  /**
   * @brief Serialize an object into a buffer.
   * @param obj The object to serialize.
   * @param buf Output buffer.
   * @param buf_size Size of the output buffer in bytes.
   * @return Number of bytes written, or 0 on failure.
   */
  static uint32_t Serialize(const T& obj, void* buf,
                             uint32_t buf_size) noexcept {
    if (buf_size < sizeof(T)) return 0;
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
    if (size < sizeof(T)) return false;
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
 */
class SequenceTracker {
 public:
  SequenceTracker() noexcept
      : expected_seq_(0),
        total_received_(0),
        lost_count_(0),
        reordered_count_(0),
        duplicate_count_(0) {}

  /**
   * @brief Process a received sequence number.
   * @param seq_num The sequence number to track.
   * @return true if in-order, false if reordered or duplicate.
   */
  bool Track(uint32_t seq_num) noexcept {
    ++total_received_;

    if (seq_num == expected_seq_) {
      // In-order
      ++expected_seq_;
      return true;
    } else if (seq_num > expected_seq_) {
      // Gap detected (packet loss)
      lost_count_ += (seq_num - expected_seq_);
      expected_seq_ = seq_num + 1;
      return true;
    } else {
      // seq_num < expected_seq_: reorder or duplicate
      uint32_t gap = expected_seq_ - seq_num;
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

#ifndef OSP_TRANSPORT_MAX_FRAME_SIZE
#define OSP_TRANSPORT_MAX_FRAME_SIZE 4096U
#endif

/// Compile-time receive ring depth per remote subscriber (must be power of 2).
#ifndef OSP_TRANSPORT_RECV_RING_DEPTH
#define OSP_TRANSPORT_RECV_RING_DEPTH 32U
#endif

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

  /**
   * @brief Encode a FrameHeader (v0) into a byte buffer.
   * @param hdr The header to encode.
   * @param buf Output buffer (must be at least kHeaderSize bytes).
   * @param buf_size Size of the output buffer.
   * @return Number of bytes written (kHeaderSize), or 0 on failure.
   */
  static uint32_t EncodeHeader(const FrameHeader& hdr, uint8_t* buf,
                                uint32_t buf_size) noexcept {
    if (buf_size < kHeaderSize) return 0;
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
  static bool DecodeHeader(const uint8_t* buf, uint32_t buf_size,
                            FrameHeader& hdr) noexcept {
    if (buf_size < kHeaderSize) return false;
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
  static uint32_t EncodeHeaderV1(const FrameHeaderV1& hdr, uint8_t* buf,
                                  uint32_t buf_size) noexcept {
    if (buf_size < kHeaderSizeV1) return 0;
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
  static bool DecodeHeaderV1(const uint8_t* buf, uint32_t buf_size,
                              FrameHeaderV1& hdr) noexcept {
    if (buf_size < kHeaderSizeV1) return false;
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
   * @return 0 for v0, 1 for v1, UINT8_MAX for unknown/invalid.
   */
  static uint8_t DetectVersion(const uint8_t* buf, uint32_t buf_size) noexcept {
    if (buf_size < 4) return UINT8_MAX;
    uint32_t magic;
    std::memcpy(&magic, buf, 4);
    if (magic == kFrameMagicV0) return 0;
    if (magic == kFrameMagicV1) return 1;
    return UINT8_MAX;
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
        connected_(other.connected_) {
    other.connected_ = false;
  }

  TcpTransport& operator=(TcpTransport&& other) noexcept {
    if (this != &other) {
      Close();
      socket_ = static_cast<TcpSocket&&>(other.socket_);
      connected_ = other.connected_;
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
      return expected<void, TransportError>::error(
          TransportError::kConnectionFailed);
    }
    socket_ = static_cast<TcpSocket&&>(sock_r.value());

    auto addr_r = SocketAddress::FromIpv4(ep.host.c_str(), ep.port);
    if (!addr_r.has_value()) {
      socket_.Close();
      return expected<void, TransportError>::error(
          TransportError::kConnectionFailed);
    }

    auto conn_r = socket_.Connect(addr_r.value());
    if (!conn_r.has_value()) {
      socket_.Close();
      return expected<void, TransportError>::error(
          TransportError::kConnectionFailed);
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
  expected<void, TransportError> SendFrame(uint16_t type_index,
                                            uint32_t sender_id,
                                            const void* payload,
                                            uint32_t payload_len) noexcept {
    if (!connected_) {
      return expected<void, TransportError>::error(
          TransportError::kNotConnected);
    }

    if (payload_len > OSP_TRANSPORT_MAX_FRAME_SIZE) {
      return expected<void, TransportError>::error(
          TransportError::kBufferFull);
    }

    // Encode header
    FrameHeader hdr;
    hdr.magic = kFrameMagic;
    hdr.length = payload_len;
    hdr.type_index = type_index;
    hdr.sender_id = sender_id;

    uint8_t hdr_buf[FrameCodec::kHeaderSize];
    FrameCodec::EncodeHeader(hdr, hdr_buf, sizeof(hdr_buf));

    // Send header
    auto r = SendAll(hdr_buf, FrameCodec::kHeaderSize);
    if (!r.has_value()) return r;

    // Send payload
    if (payload_len > 0 && payload != nullptr) {
      r = SendAll(payload, payload_len);
      if (!r.has_value()) return r;
    }

    return expected<void, TransportError>::success();
  }

  /**
   * @brief Receive a framed message (blocking).
   * @param hdr Output frame header.
   * @param payload_buf Buffer to receive the payload into.
   * @param buf_size Size of the payload buffer.
   * @return Number of payload bytes received, or TransportError.
   */
  expected<uint32_t, TransportError> RecvFrame(
      FrameHeader& hdr, void* payload_buf, uint32_t buf_size) noexcept {
    if (!connected_) {
      return expected<uint32_t, TransportError>::error(
          TransportError::kNotConnected);
    }

    // Receive header
    uint8_t hdr_buf[FrameCodec::kHeaderSize];
    auto r = RecvAll(hdr_buf, FrameCodec::kHeaderSize);
    if (!r.has_value()) return expected<uint32_t, TransportError>::error(
        r.get_error());

    if (!FrameCodec::DecodeHeader(hdr_buf, FrameCodec::kHeaderSize, hdr)) {
      return expected<uint32_t, TransportError>::error(
          TransportError::kInvalidFrame);
    }

    // Validate magic
    if (hdr.magic != kFrameMagic) {
      return expected<uint32_t, TransportError>::error(
          TransportError::kInvalidFrame);
    }

    // Validate payload size
    if (hdr.length > buf_size) {
      return expected<uint32_t, TransportError>::error(
          TransportError::kBufferFull);
    }

    // Receive payload
    if (hdr.length > 0) {
      auto pr = RecvAll(payload_buf, hdr.length);
      if (!pr.has_value()) return expected<uint32_t, TransportError>::error(
          pr.get_error());
    }

    return expected<uint32_t, TransportError>::success(hdr.length);
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
  expected<void, TransportError> SendFrameV1(uint16_t type_index,
                                              uint32_t sender_id,
                                              uint32_t seq_num,
                                              uint64_t timestamp_ns,
                                              const void* payload,
                                              uint32_t payload_len) noexcept {
    if (!connected_) {
      return expected<void, TransportError>::error(
          TransportError::kNotConnected);
    }

    if (payload_len > OSP_TRANSPORT_MAX_FRAME_SIZE) {
      return expected<void, TransportError>::error(
          TransportError::kBufferFull);
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

    // Send header
    auto r = SendAll(hdr_buf, FrameCodec::kHeaderSizeV1);
    if (!r.has_value()) return r;

    // Send payload
    if (payload_len > 0 && payload != nullptr) {
      r = SendAll(payload, payload_len);
      if (!r.has_value()) return r;
    }

    return expected<void, TransportError>::success();
  }

  /**
   * @brief Receive a framed message with auto-detection (handles both v0 and v1).
   * @param hdr_v1 Output v1 header (seq_num and timestamp_ns are 0 for v0 frames).
   * @param payload_buf Buffer to receive the payload into.
   * @param buf_size Size of the payload buffer.
   * @return Number of payload bytes received, or TransportError.
   */
  expected<uint32_t, TransportError> RecvFrameAuto(
      FrameHeaderV1& hdr_v1, void* payload_buf, uint32_t buf_size) noexcept {
    if (!connected_) {
      return expected<uint32_t, TransportError>::error(
          TransportError::kNotConnected);
    }

    // Peek at first 4 bytes to detect version
    uint8_t magic_buf[4];
    auto r = RecvAll(magic_buf, 4);
    if (!r.has_value()) return expected<uint32_t, TransportError>::error(
        r.get_error());

    uint8_t version = FrameCodec::DetectVersion(magic_buf, 4);

    if (version == 0) {
      // v0 frame: read remaining 10 bytes of header
      uint8_t hdr_buf[FrameCodec::kHeaderSize];
      std::memcpy(hdr_buf, magic_buf, 4);
      auto r2 = RecvAll(hdr_buf + 4, FrameCodec::kHeaderSize - 4);
      if (!r2.has_value()) return expected<uint32_t, TransportError>::error(
          r2.get_error());

      FrameHeader hdr;
      if (!FrameCodec::DecodeHeader(hdr_buf, FrameCodec::kHeaderSize, hdr)) {
        return expected<uint32_t, TransportError>::error(
            TransportError::kInvalidFrame);
      }

      // Validate magic
      if (hdr.magic != kFrameMagicV0) {
        return expected<uint32_t, TransportError>::error(
            TransportError::kInvalidFrame);
      }

      // Validate payload size
      if (hdr.length > buf_size) {
        return expected<uint32_t, TransportError>::error(
            TransportError::kBufferFull);
      }

      // Receive payload
      if (hdr.length > 0) {
        auto pr = RecvAll(payload_buf, hdr.length);
        if (!pr.has_value()) return expected<uint32_t, TransportError>::error(
            pr.get_error());
      }

      // Convert to v1 header (seq_num and timestamp_ns are 0)
      hdr_v1.magic = hdr.magic;
      hdr_v1.length = hdr.length;
      hdr_v1.type_index = hdr.type_index;
      hdr_v1.sender_id = hdr.sender_id;
      hdr_v1.seq_num = 0;
      hdr_v1.timestamp_ns = 0;

      return expected<uint32_t, TransportError>::success(hdr.length);

    } else if (version == 1) {
      // v1 frame: read remaining 22 bytes of header
      uint8_t hdr_buf[FrameCodec::kHeaderSizeV1];
      std::memcpy(hdr_buf, magic_buf, 4);
      auto r2 = RecvAll(hdr_buf + 4, FrameCodec::kHeaderSizeV1 - 4);
      if (!r2.has_value()) return expected<uint32_t, TransportError>::error(
          r2.get_error());

      if (!FrameCodec::DecodeHeaderV1(hdr_buf, FrameCodec::kHeaderSizeV1, hdr_v1)) {
        return expected<uint32_t, TransportError>::error(
            TransportError::kInvalidFrame);
      }

      // Validate magic
      if (hdr_v1.magic != kFrameMagicV1) {
        return expected<uint32_t, TransportError>::error(
            TransportError::kInvalidFrame);
      }

      // Validate payload size
      if (hdr_v1.length > buf_size) {
        return expected<uint32_t, TransportError>::error(
            TransportError::kBufferFull);
      }

      // Receive payload
      if (hdr_v1.length > 0) {
        auto pr = RecvAll(payload_buf, hdr_v1.length);
        if (!pr.has_value()) return expected<uint32_t, TransportError>::error(
            pr.get_error());
      }

      return expected<uint32_t, TransportError>::success(hdr_v1.length);

    } else {
      // Unknown version
      return expected<uint32_t, TransportError>::error(
          TransportError::kInvalidFrame);
    }
  }

  /** @brief Check if the transport is connected. */
  bool IsConnected() const noexcept { return connected_; }

  /** @brief Close the transport connection. */
  void Close() noexcept {
    socket_.Close();
    connected_ = false;
  }

 private:
  /**
   * @brief Send exactly len bytes over the socket.
   * @param data Pointer to the data to send.
   * @param len Number of bytes to send.
   * @return Success or TransportError::kSendFailed.
   */
  expected<void, TransportError> SendAll(const void* data,
                                          uint32_t len) noexcept {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    uint32_t remaining = len;
    while (remaining > 0) {
      auto r = socket_.Send(ptr, remaining);
      if (!r.has_value()) {
        connected_ = false;
        return expected<void, TransportError>::error(
            TransportError::kSendFailed);
      }
      if (r.value() == 0) {
        connected_ = false;
        return expected<void, TransportError>::error(
            TransportError::kSendFailed);
      }
      uint32_t sent = static_cast<uint32_t>(r.value());
      ptr += sent;
      remaining -= sent;
    }
    return expected<void, TransportError>::success();
  }

  /**
   * @brief Receive exactly len bytes from the socket.
   * @param data Pointer to the receive buffer.
   * @param len Number of bytes to receive.
   * @return Success or TransportError::kRecvFailed.
   */
  expected<void, TransportError> RecvAll(void* data, uint32_t len) noexcept {
    uint8_t* ptr = static_cast<uint8_t*>(data);
    uint32_t remaining = len;
    while (remaining > 0) {
      auto r = socket_.Recv(ptr, remaining);
      if (!r.has_value()) {
        connected_ = false;
        return expected<void, TransportError>::error(
            TransportError::kRecvFailed);
      }
      if (r.value() == 0) {
        // Connection closed by peer
        connected_ = false;
        return expected<void, TransportError>::error(
            TransportError::kRecvFailed);
      }
      uint32_t received = static_cast<uint32_t>(r.value());
      ptr += received;
      remaining -= received;
    }
    return expected<void, TransportError>::success();
  }

  TcpSocket socket_;
  bool connected_;
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
  explicit NetworkNode(const char* name, uint32_t id = 0) noexcept
      : Node<PayloadVariant>(name, id),
        pub_count_(0),
        sub_count_(0),
        recv_ring_drops_(0),
        listening_(false) {
    for (uint32_t i = 0; i < kMaxRemoteConnections; ++i) {
      publishers_[i].active = false;
      publishers_[i].type_index = 0;
      subscribers_[i].active = false;
      subscribers_[i].type_index = 0;
    }
  }

  ~NetworkNode() {
    for (uint32_t i = 0; i < kMaxRemoteConnections; ++i) {
      if (publishers_[i].active) {
        publishers_[i].transport.Close();
        publishers_[i].active = false;
      }
      if (subscribers_[i].active) {
        subscribers_[i].transport.Close();
        subscribers_[i].active = false;
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
    if (pub_count_ >= kMaxRemoteConnections) {
      return expected<void, TransportError>::error(
          TransportError::kBufferFull);
    }

    constexpr uint16_t type_idx = static_cast<uint16_t>(
        VariantIndex<T, PayloadVariant>::value);

    // Find a free slot
    uint32_t slot = kMaxRemoteConnections;
    for (uint32_t i = 0; i < kMaxRemoteConnections; ++i) {
      if (!publishers_[i].active) {
        slot = i;
        break;
      }
    }
    if (slot >= kMaxRemoteConnections) {
      return expected<void, TransportError>::error(
          TransportError::kBufferFull);
    }

    auto r = publishers_[slot].transport.Connect(ep);
    if (!r.has_value()) return r;

    publishers_[slot].type_index = type_idx;
    publishers_[slot].active = true;
    ++pub_count_;

    // Subscribe locally to type T: forward matching messages over transport.
    // We capture the slot index to send through the correct transport.
    RemotePublisher* pub_ptr = &publishers_[slot];
    uint32_t sender_id = this->Id();

    auto sub_r = this->template Subscribe<T>(
        [pub_ptr, sender_id](const T& data, const MessageHeader& /*hdr*/) {
          if (!pub_ptr->active) return;

          uint8_t payload_buf[OSP_TRANSPORT_MAX_FRAME_SIZE];
          uint32_t len = Serializer<T>::Serialize(
              data, payload_buf, sizeof(payload_buf));
          if (len == 0) return;

          static_cast<void>(pub_ptr->transport.SendFrame(
              pub_ptr->type_index, sender_id, payload_buf, len));
        });

    if (!sub_r.has_value()) {
      publishers_[slot].transport.Close();
      publishers_[slot].active = false;
      --pub_count_;
      return expected<void, TransportError>::error(
          TransportError::kConnectionFailed);
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
    if (sub_count_ >= kMaxRemoteConnections) {
      return expected<void, TransportError>::error(
          TransportError::kBufferFull);
    }

    constexpr uint16_t type_idx = static_cast<uint16_t>(
        VariantIndex<T, PayloadVariant>::value);

    // Find a free slot
    uint32_t slot = kMaxRemoteConnections;
    for (uint32_t i = 0; i < kMaxRemoteConnections; ++i) {
      if (!subscribers_[i].active) {
        slot = i;
        break;
      }
    }
    if (slot >= kMaxRemoteConnections) {
      return expected<void, TransportError>::error(
          TransportError::kBufferFull);
    }

    auto r = subscribers_[slot].transport.Connect(ep);
    if (!r.has_value()) return r;

    subscribers_[slot].type_index = type_idx;
    subscribers_[slot].active = true;
    ++sub_count_;

    return expected<void, TransportError>::success();
  }

  /**
   * @brief Process remote subscriber connections (call periodically).
   *
   * For each active remote subscriber, attempts a blocking receive of one
   * frame, deserializes the payload, and publishes it to the local bus.
   *
   * NOTE: For non-blocking behavior, set sockets to non-blocking mode
   * before calling this method.
   *
   * @return Number of messages processed from remote sources.
   */
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
      if (!subscribers_[i].active) continue;
      if (!subscribers_[i].transport.IsConnected()) {
        subscribers_[i].active = false;
        --sub_count_;
        continue;
      }

      RecvFrameSlot slot;
      auto r = subscribers_[i].transport.RecvFrame(
          slot.header, slot.payload, sizeof(slot.payload));
      if (!r.has_value()) continue;

      slot.payload_len = r.value();
      if (subscribers_[i].recv_ring.Push(slot)) {
        ++buffered;
      } else {
        // Ring buffer full -- frame is dropped.
        ++recv_ring_drops_;
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
      if (!subscribers_[i].active) continue;

      RecvFrameSlot slot;
      while (subscribers_[i].recv_ring.Pop(slot)) {
        if (DeserializeAndPublish(slot.header.type_index, slot.header.sender_id,
                                   slot.payload, slot.payload_len)) {
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
      return expected<void, TransportError>::error(
          TransportError::kBindFailed);
    }
    listener_ = static_cast<TcpListener&&>(listener_r.value());

    // Set SO_REUSEADDR
    int32_t opt = 1;
    ::setsockopt(listener_.Fd(), SOL_SOCKET, SO_REUSEADDR, &opt,
                 static_cast<socklen_t>(sizeof(opt)));

    auto addr_r = SocketAddress::FromIpv4("0.0.0.0", port);
    if (!addr_r.has_value()) {
      listener_.Close();
      return expected<void, TransportError>::error(
          TransportError::kBindFailed);
    }

    auto bind_r = listener_.Bind(addr_r.value());
    if (!bind_r.has_value()) {
      listener_.Close();
      return expected<void, TransportError>::error(
          TransportError::kBindFailed);
    }

    auto listen_r = listener_.Listen(8);
    if (!listen_r.has_value()) {
      listener_.Close();
      return expected<void, TransportError>::error(
          TransportError::kBindFailed);
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
      return expected<void, TransportError>::error(
          TransportError::kNotConnected);
    }

    auto accept_r = listener_.Accept();
    if (!accept_r.has_value()) {
      return expected<void, TransportError>::error(
          TransportError::kConnectionFailed);
    }

    // Find a free subscriber slot
    for (uint32_t i = 0; i < kMaxRemoteConnections; ++i) {
      if (!subscribers_[i].active) {
        subscribers_[i].transport.AcceptFrom(
            static_cast<TcpSocket&&>(accept_r.value()));
        subscribers_[i].type_index = 0;  // Will be determined by frame data
        subscribers_[i].active = true;
        ++sub_count_;
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
  uint32_t RemotePublisherCount() const noexcept { return pub_count_; }

  /** @brief Get the number of active remote subscribers. */
  uint32_t RemoteSubscriberCount() const noexcept { return sub_count_; }

  /** @brief Get the number of frames dropped due to full recv_ring. */
  uint64_t RecvRingDrops() const noexcept { return recv_ring_drops_; }

 private:
  static constexpr uint32_t kMaxRemoteConnections = 8;

  struct RemotePublisher {
    TcpTransport transport;
    uint16_t type_index;
    bool active;
  };

  struct RemoteSubscriber {
    TcpTransport transport;
    SpscRingbuffer<RecvFrameSlot, OSP_TRANSPORT_RECV_RING_DEPTH> recv_ring;
    uint16_t type_index;
    bool active;
  };

  /**
   * @brief Deserialize payload and publish to local bus based on type_index.
   *
   * Uses a compile-time visitor over the variant alternatives to find the
   * matching type for deserialization.
   */
  bool DeserializeAndPublish(uint16_t type_index, uint32_t sender_id,
                              const void* data, uint32_t len) noexcept {
    return DeserializeAtIndex<0>(type_index, sender_id, data, len);
  }

  /**
   * @brief Recursive template to match type_index to a variant alternative.
   */
  template <size_t I>
  typename std::enable_if<(I < std::variant_size<PayloadVariant>::value),
                          bool>::type
  DeserializeAtIndex(uint16_t type_index, uint32_t sender_id,
                      const void* data, uint32_t len) noexcept {
    if (type_index == static_cast<uint16_t>(I)) {
      using T = typename std::variant_alternative<I, PayloadVariant>::type;
      T obj;
      if (!Serializer<T>::Deserialize(data, len, obj)) return false;
      BusType::Instance().Publish(PayloadVariant(obj), sender_id);
      return true;
    }
    return DeserializeAtIndex<I + 1>(type_index, sender_id, data, len);
  }

  template <size_t I>
  typename std::enable_if<(I >= std::variant_size<PayloadVariant>::value),
                          bool>::type
  DeserializeAtIndex(uint16_t /*type_index*/, uint32_t /*sender_id*/,
                      const void* /*data*/, uint32_t /*len*/) noexcept {
    return false;
  }

  RemotePublisher publishers_[kMaxRemoteConnections];
  RemoteSubscriber subscribers_[kMaxRemoteConnections];
  uint32_t pub_count_;
  uint32_t sub_count_;
  uint64_t recv_ring_drops_;
  TcpListener listener_;
  bool listening_;
};

#endif  // defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)

}  // namespace osp

#endif  // OSP_TRANSPORT_HPP_
