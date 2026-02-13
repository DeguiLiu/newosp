/**
 * @file serial_transport.hpp
 * @brief Industrial-grade serial port transport with framed protocol, CRC-CCITT,
 *        sequence tracking, optional ACK/retransmit, and health monitoring.
 *
 * Compliant with:
 * - MISRA C++ (stdint types, explicit casts, no magic numbers)
 * - IEC 61508 (health monitoring, error rate tracking, watchdog)
 * - Google C++ Style (naming conventions, documentation)
 *
 * Pure POSIX implementation (termios + open/read/write). Zero external dependencies.
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_SERIAL_TRANSPORT_HPP_
#define OSP_SERIAL_TRANSPORT_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <cstdint>
#include <cstring>

#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/time.h>
#endif

namespace osp {

// ============================================================================
// Serial Error
// ============================================================================

enum class SerialError : uint8_t {
  kOpenFailed,
  kConfigFailed,
  kSendFailed,
  kRecvFailed,
  kCrcError,
  kTimeout,
  kFrameOversize,
  kPortNotOpen,
  kAckTimeout,
  kRateLimitExceeded,
  kHealthCheckFailed,
};

// ============================================================================
// Serial Port Health Status (IEC 61508)
// ============================================================================

/// @brief Port health state for industrial safety monitoring
enum class SerialPortHealth : uint8_t {
  kHealthy = 0U,    ///< Normal operation
  kDegraded = 1U,   ///< Error rate above warning threshold
  kFailed = 2U,     ///< Error rate above critical threshold or no communication
};

// ============================================================================
// Serial Frame Constants (MISRA C++: Named constants, no magic numbers)
// ============================================================================

static constexpr uint16_t kSerialSyncWord   = 0xAA55U;
static constexpr uint16_t kSerialMagic      = 0x4F53U;  // "OS"
static constexpr uint16_t kSerialAckMagic   = 0x4F41U;  // "OA"
static constexpr uint8_t  kSerialTailByte   = 0x0DU;
static constexpr uint8_t  kSerialSyncByte1  = 0xAAU;
static constexpr uint8_t  kSerialSyncByte2  = 0x55U;

/// Header: sync(2) + magic(2) + msg_len(2) + seq(2) + type(2) = 10 bytes
static constexpr uint32_t kSerialHeaderSize = 10U;
/// Trailer: crc16(2) + tail(1) = 3 bytes
static constexpr uint32_t kSerialTrailerSize = 3U;
/// Minimum frame: header + trailer, no payload
static constexpr uint32_t kSerialMinFrameSize = kSerialHeaderSize + kSerialTrailerSize;

/// ACK frame: sync(2) + ack_magic(2) + ack_seq(2) + crc16(2) = 8 bytes
static constexpr uint32_t kSerialAckFrameSize = 8U;

/// CRC field size in bytes
static constexpr uint32_t kSerialCrcSize = 2U;

/// Default maximum frame size
#ifndef OSP_SERIAL_MAX_FRAME_SIZE
#define OSP_SERIAL_MAX_FRAME_SIZE 1024U
#endif

// Compile-time validation of frame size constraints (MISRA C++: static_assert)
static_assert(OSP_SERIAL_MAX_FRAME_SIZE >= kSerialMinFrameSize,
              "OSP_SERIAL_MAX_FRAME_SIZE must be >= kSerialMinFrameSize");
static_assert(OSP_SERIAL_MAX_FRAME_SIZE <= 65535U,
              "OSP_SERIAL_MAX_FRAME_SIZE must fit in uint16_t msg_len field");

// ============================================================================
// Reliability Config
// ============================================================================

struct ReliabilityConfig {
  bool enable_ack = false;
  uint32_t ack_timeout_ms = 100U;
  uint8_t max_retries = 3U;
  bool enable_seq_check = true;
};

// ============================================================================
// Serial Config (IEC 61508: Health monitoring and rate limiting)
// ============================================================================

/// @brief Configuration for serial port transport with industrial safety features
struct SerialConfig {
  char port_name[64] = "/dev/ttyS0";
  uint32_t baud_rate = 115200U;
  uint8_t data_bits = 8U;
  uint8_t stop_bits = 1U;
  uint8_t parity = 0U;              // 0=None, 1=Odd, 2=Even
  uint8_t flow_control = 0U;        // 0=None, 1=HW(RTS/CTS), 2=SW(XON/XOFF)
  uint32_t inter_byte_timeout_ms = 50U;
  uint32_t frame_max_size = OSP_SERIAL_MAX_FRAME_SIZE;
  ReliabilityConfig reliability;

  // IEC 61508: Health monitoring parameters
  uint32_t watchdog_timeout_ms = 5000U;     ///< Max time without successful rx/tx
  uint32_t error_rate_window = 100U;        ///< Window size for error rate calculation
  uint32_t degraded_error_threshold = 10U;  ///< Errors per window for degraded state
  uint32_t failed_error_threshold = 30U;    ///< Errors per window for failed state

  // Rate limiting (prevent bus flooding)
  uint32_t max_frames_per_second = 1000U;   ///< 0 = unlimited
  uint32_t write_retry_count = 3U;          ///< Max retries for WriteAll
  uint32_t write_retry_delay_us = 1000U;    ///< Backoff delay between retries
};

// Compile-time validation of config struct
static_assert(sizeof(SerialConfig) < 256U, "SerialConfig should be compact");
static_assert(sizeof(ReliabilityConfig) < 32U, "ReliabilityConfig should be compact");

// ============================================================================
// Serial Statistics (IEC 61508: Error tracking)
// ============================================================================

/// @brief Statistics for monitoring serial port health and performance
struct SerialStatistics {
  uint64_t frames_sent = 0U;
  uint64_t frames_received = 0U;
  uint64_t bytes_sent = 0U;
  uint64_t bytes_received = 0U;
  uint64_t crc_errors = 0U;
  uint64_t sync_errors = 0U;
  uint64_t timeout_errors = 0U;
  uint64_t oversize_errors = 0U;
  uint64_t seq_gaps = 0U;
  uint64_t retransmits = 0U;
  uint64_t ack_timeouts = 0U;
  uint64_t rate_limit_drops = 0U;       ///< Frames dropped due to rate limiting
  uint64_t write_retries = 0U;          ///< Total write retry attempts
};

// ============================================================================
// Rx Callback
// ============================================================================

/// @brief Callback invoked when a complete frame is received
/// @param payload Pointer to payload data (may be nullptr if size is 0)
/// @param size Payload size in bytes
/// @param type_index Message type index
/// @param seq Sequence number
/// @param ctx User context pointer
using SerialRxCallback = void (*)(const void* payload, uint32_t size,
                                   uint16_t type_index, uint16_t seq, void* ctx);

// ============================================================================
// CRC-CCITT (0x1021) Lookup Table
// ============================================================================

/// @brief CRC-CCITT calculator with polynomial 0x1021
class Crc16Ccitt {
 public:
  /// @brief Calculate CRC-CCITT for given data
  /// @param data Pointer to data buffer
  /// @param size Size of data in bytes
  /// @return CRC-CCITT value
  static uint16_t Calculate(const void* data, uint32_t size) noexcept {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint16_t crc = 0xFFFFU;
    for (uint32_t i = 0U; i < size; ++i) {
      const uint8_t index = static_cast<uint8_t>((crc >> 8) ^ p[i]);
      crc = static_cast<uint16_t>((crc << 8) ^ kTable[index]);
    }
    return crc;
  }

 private:
  static constexpr uint16_t kTable[256] = {
    0x0000U, 0x1021U, 0x2042U, 0x3063U, 0x4084U, 0x50A5U, 0x60C6U, 0x70E7U,
    0x8108U, 0x9129U, 0xA14AU, 0xB16BU, 0xC18CU, 0xD1ADU, 0xE1CEU, 0xF1EFU,
    0x1231U, 0x0210U, 0x3273U, 0x2252U, 0x52B5U, 0x4294U, 0x72F7U, 0x62D6U,
    0x9339U, 0x8318U, 0xB37BU, 0xA35AU, 0xD3BDU, 0xC39CU, 0xF3FFU, 0xE3DEU,
    0x2462U, 0x3443U, 0x0420U, 0x1401U, 0x64E6U, 0x74C7U, 0x44A4U, 0x5485U,
    0xA56AU, 0xB54BU, 0x8528U, 0x9509U, 0xE5EEU, 0xF5CFU, 0xC5ACU, 0xD58DU,
    0x3653U, 0x2672U, 0x1611U, 0x0630U, 0x76D7U, 0x66F6U, 0x5695U, 0x46B4U,
    0xB75BU, 0xA77AU, 0x9719U, 0x8738U, 0xF7DFU, 0xE7FEU, 0xD79DU, 0xC7BCU,
    0x48C4U, 0x58E5U, 0x6886U, 0x78A7U, 0x0840U, 0x1861U, 0x2802U, 0x3823U,
    0xC9CCU, 0xD9EDU, 0xE98EU, 0xF9AFU, 0x8948U, 0x9969U, 0xA90AU, 0xB92BU,
    0x5AF5U, 0x4AD4U, 0x7AB7U, 0x6A96U, 0x1A71U, 0x0A50U, 0x3A33U, 0x2A12U,
    0xDBFDU, 0xCBDCU, 0xFBBFU, 0xEB9EU, 0x9B79U, 0x8B58U, 0xBB3BU, 0xAB1AU,
    0x6CA6U, 0x7C87U, 0x4CE4U, 0x5CC5U, 0x2C22U, 0x3C03U, 0x0C60U, 0x1C41U,
    0xEDAEU, 0xFD8FU, 0xCDECU, 0xDDCDU, 0xAD2AU, 0xBD0BU, 0x8D68U, 0x9D49U,
    0x7E97U, 0x6EB6U, 0x5ED5U, 0x4EF4U, 0x3E13U, 0x2E32U, 0x1E51U, 0x0E70U,
    0xFF9FU, 0xEFBEU, 0xDFDDU, 0xCFFCU, 0xBF1BU, 0xAF3AU, 0x9F59U, 0x8F78U,
    0x9188U, 0x81A9U, 0xB1CAU, 0xA1EBU, 0xD10CU, 0xC12DU, 0xF14EU, 0xE16FU,
    0x1080U, 0x00A1U, 0x30C2U, 0x20E3U, 0x5004U, 0x4025U, 0x7046U, 0x6067U,
    0x83B9U, 0x9398U, 0xA3FBU, 0xB3DAU, 0xC33DU, 0xD31CU, 0xE37FU, 0xF35EU,
    0x02B1U, 0x1290U, 0x22F3U, 0x32D2U, 0x4235U, 0x5214U, 0x6277U, 0x7256U,
    0xB5EAU, 0xA5CBU, 0x95A8U, 0x8589U, 0xF56EU, 0xE54FU, 0xD52CU, 0xC50DU,
    0x34E2U, 0x24C3U, 0x14A0U, 0x0481U, 0x7466U, 0x6447U, 0x5424U, 0x4405U,
    0xA7DBU, 0xB7FAU, 0x8799U, 0x97B8U, 0xE75FU, 0xF77EU, 0xC71DU, 0xD73CU,
    0x26D3U, 0x36F2U, 0x0691U, 0x16B0U, 0x6657U, 0x7676U, 0x4615U, 0x5634U,
    0xD94CU, 0xC96DU, 0xF90EU, 0xE92FU, 0x99C8U, 0x89E9U, 0xB98AU, 0xA9ABU,
    0x5844U, 0x4865U, 0x7806U, 0x6827U, 0x18C0U, 0x08E1U, 0x3882U, 0x28A3U,
    0xCB7DU, 0xDB5CU, 0xEB3FU, 0xFB1EU, 0x8BF9U, 0x9BD8U, 0xABBBU, 0xBB9AU,
    0x4A75U, 0x5A54U, 0x6A37U, 0x7A16U, 0x0AF1U, 0x1AD0U, 0x2AB3U, 0x3A92U,
    0xFD2EU, 0xED0FU, 0xDD6CU, 0xCD4DU, 0xBDAAU, 0xAD8BU, 0x9DE8U, 0x8DC9U,
    0x7C26U, 0x6C07U, 0x5C64U, 0x4C45U, 0x3CA2U, 0x2C83U, 0x1CE0U, 0x0CC1U,
    0xEF1FU, 0xFF3EU, 0xCF5DU, 0xDF7CU, 0xAF9BU, 0xBFBAU, 0x8FD9U, 0x9FF8U,
    0x6E17U, 0x7E36U, 0x4E55U, 0x5E74U, 0x2E93U, 0x3EB2U, 0x0ED1U, 0x1EF0U,
  };
};

// Out-of-line constexpr definition (C++17 inline variable via constexpr)
constexpr uint16_t Crc16Ccitt::kTable[256];

// ============================================================================
// SerialTransport
// ============================================================================

/// @brief Industrial-grade serial transport with health monitoring and rate limiting
class SerialTransport {
 public:
  /// @brief Construct serial transport with given configuration
  /// @param cfg Serial port configuration
  explicit SerialTransport(const SerialConfig& cfg) noexcept
      : cfg_(cfg), fd_(-1), tx_seq_(0U), rx_expected_seq_(0U),
        rx_state_(RxState::kIdle), rx_pos_(0U),
        rx_cb_(nullptr), rx_ctx_(nullptr), stats_{},
        last_byte_time_ms_(0U), last_tx_time_ms_(0U), last_rx_time_ms_(0U),
        frame_count_window_start_ms_(0U), frames_sent_in_window_(0U),
        error_count_in_window_(0U) {}

  ~SerialTransport() { Close(); }

  // Non-copyable, non-movable
  SerialTransport(const SerialTransport&) = delete;
  SerialTransport& operator=(const SerialTransport&) = delete;

  // ------------------------------------------------------------------
  // Open / Close / IsOpen
  // ------------------------------------------------------------------

  /// @brief Open the serial port
  /// @return Success or error code
  expected<void, SerialError> Open() noexcept {
#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)
    if (fd_ >= 0) {
      return expected<void, SerialError>::success();
    }

    fd_ = ::open(cfg_.port_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
      return expected<void, SerialError>::error(SerialError::kOpenFailed);
    }

    auto r = ConfigurePort();
    if (!r) {
      ::close(fd_);
      fd_ = -1;
      return r;
    }

    // Initialize health monitoring timestamps
    const uint64_t now = NowMs();
    last_tx_time_ms_ = now;
    last_rx_time_ms_ = now;
    frame_count_window_start_ms_ = now;

    return expected<void, SerialError>::success();
#else
    return expected<void, SerialError>::error(SerialError::kOpenFailed);
#endif
  }

  /// @brief Close the serial port and reset state
  void Close() noexcept {
#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
#endif
    ResetRxState();
  }

  /// @brief Check if port is open
  /// @return true if port is open
  bool IsOpen() const noexcept { return fd_ >= 0; }

  // ------------------------------------------------------------------
  // Send
  // ------------------------------------------------------------------

  /// @brief Send a frame with type and payload
  /// @param type_index Message type index
  /// @param payload Pointer to payload data (may be nullptr if size is 0)
  /// @param size Payload size in bytes
  /// @return Success or error code
  expected<void, SerialError> Send(uint16_t type_index,
                                    const void* payload,
                                    uint32_t size) noexcept {
    if (fd_ < 0) {
      return expected<void, SerialError>::error(SerialError::kPortNotOpen);
    }

    // Rate limiting check
    if (cfg_.max_frames_per_second > 0U) {
      const uint64_t now = NowMs();
      const uint64_t window_duration_ms = 1000U;

      if (now - frame_count_window_start_ms_ >= window_duration_ms) {
        // Reset window
        frame_count_window_start_ms_ = now;
        frames_sent_in_window_ = 0U;
      }

      if (frames_sent_in_window_ >= cfg_.max_frames_per_second) {
        ++stats_.rate_limit_drops;
        return expected<void, SerialError>::error(SerialError::kRateLimitExceeded);
      }
    }

    // msg_len = header size (10) + payload + crc(2), NOT including tail
    const uint32_t msg_len = kSerialHeaderSize + size + kSerialCrcSize;
    const uint32_t total = msg_len + 1U;  // + tail byte

    if (total > cfg_.frame_max_size) {
      return expected<void, SerialError>::error(SerialError::kFrameOversize);
    }

    // Build frame into stack buffer
    uint8_t buf[OSP_SERIAL_MAX_FRAME_SIZE];
    uint32_t pos = 0U;

    // sync word (fixed byte order: 0xAA first, 0x55 second)
    buf[pos] = kSerialSyncByte1;
    ++pos;
    buf[pos] = kSerialSyncByte2;
    ++pos;

    // magic
    WriteLE16(buf + pos, kSerialMagic);
    pos += 2U;

    // msg_len
    WriteLE16(buf + pos, static_cast<uint16_t>(msg_len));
    pos += 2U;

    // seq (handle wraparound explicitly)
    const uint16_t seq = tx_seq_;
    tx_seq_ = static_cast<uint16_t>(tx_seq_ + 1U);  // Explicit wraparound
    WriteLE16(buf + pos, seq);
    pos += 2U;

    // type
    WriteLE16(buf + pos, type_index);
    pos += 2U;

    // payload
    if (size > 0U && payload != nullptr) {
      std::memcpy(buf + pos, payload, size);
      pos += size;
    }

    // CRC over everything from sync to end of payload
    const uint16_t crc = Crc16Ccitt::Calculate(buf, pos);
    WriteLE16(buf + pos, crc);
    pos += 2U;

    // tail
    buf[pos] = kSerialTailByte;
    ++pos;

    const expected<void, SerialError> result = WriteAll(buf, pos, seq);

    if (result.has_value()) {
      ++frames_sent_in_window_;
      last_tx_time_ms_ = NowMs();
    }

    return result;
  }

  // ------------------------------------------------------------------
  // Poll (non-blocking receive + decode)
  // ------------------------------------------------------------------

  /// @brief Poll for incoming data and decode frames
  /// @return Number of complete frames decoded
  uint32_t Poll() noexcept {
    if (fd_ < 0) {
      return 0U;
    }

    uint8_t tmp[256];
    uint32_t frames_done = 0U;

    for (;;) {
      const ssize_t n = ::read(fd_, tmp, sizeof(tmp));
      if (n <= 0) {
        break;
      }

      const uint64_t now = NowMs();
      stats_.bytes_received += static_cast<uint64_t>(n);

      for (ssize_t i = 0; i < n; ++i) {
        // Inter-byte timeout check
        if (rx_state_ != RxState::kIdle && last_byte_time_ms_ != 0U) {
          if (now - last_byte_time_ms_ > cfg_.inter_byte_timeout_ms) {
            ++stats_.timeout_errors;
            IncrementErrorCount();
            ResetRxState();
          }
        }
        last_byte_time_ms_ = now;

        if (FeedByte(tmp[i])) {
          ++frames_done;
          last_rx_time_ms_ = now;
        }
      }
    }
    return frames_done;
  }

  // ------------------------------------------------------------------
  // Callback
  // ------------------------------------------------------------------

  /// @brief Set receive callback
  /// @param cb Callback function pointer
  /// @param ctx User context pointer
  void SetRxCallback(SerialRxCallback cb, void* ctx = nullptr) noexcept {
    rx_cb_ = cb;
    rx_ctx_ = ctx;
  }

  /// @brief Get file descriptor for integration with event loops
  /// @return File descriptor or -1 if not open
  int GetFd() const noexcept { return fd_; }

  // ------------------------------------------------------------------
  // Statistics
  // ------------------------------------------------------------------

  /// @brief Get current statistics
  /// @return Statistics structure
  SerialStatistics GetStatistics() const noexcept { return stats_; }

  /// @brief Reset all statistics counters
  void ResetStatistics() noexcept {
    std::memset(&stats_, 0, sizeof(stats_));
  }

  // ------------------------------------------------------------------
  // Health Monitoring (IEC 61508)
  // ------------------------------------------------------------------

  /// @brief Check port health status
  /// @return Health status enum
  SerialPortHealth GetHealth() const noexcept {
    const uint64_t now = NowMs();

    // Check watchdog timeout
    const uint64_t time_since_tx = now - last_tx_time_ms_;
    const uint64_t time_since_rx = now - last_rx_time_ms_;
    const uint64_t max_idle = (time_since_tx > time_since_rx) ? time_since_tx : time_since_rx;

    if (max_idle > cfg_.watchdog_timeout_ms) {
      return SerialPortHealth::kFailed;
    }

    // Check error rate
    if (error_count_in_window_ >= cfg_.failed_error_threshold) {
      return SerialPortHealth::kFailed;
    }

    if (error_count_in_window_ >= cfg_.degraded_error_threshold) {
      return SerialPortHealth::kDegraded;
    }

    return SerialPortHealth::kHealthy;
  }

  /// @brief Check if port is healthy
  /// @return true if health status is kHealthy
  bool IsHealthy() const noexcept {
    return GetHealth() == SerialPortHealth::kHealthy;
  }

 private:
  // ------------------------------------------------------------------
  // Receive state machine
  // ------------------------------------------------------------------

  enum class RxState : uint8_t {
    kIdle,
    kWaitSync2,
    kWaitHeader,
    kWaitPayload,
    kWaitCrc,
  };

  /// @brief Reset receive state machine
  void ResetRxState() noexcept {
    rx_state_ = RxState::kIdle;
    rx_pos_ = 0U;
  }

  /// @brief Increment error count for health monitoring
  void IncrementErrorCount() noexcept {
    ++error_count_in_window_;

    // Reset window if needed
    const uint64_t now = NowMs();
    const uint64_t window_duration_ms = 10000U;  // 10 second window

    if (now - frame_count_window_start_ms_ >= window_duration_ms) {
      error_count_in_window_ = 1U;  // Current error
      frame_count_window_start_ms_ = now;
    }
  }

  /// @brief Feed one byte into the state machine
  /// @param byte Input byte
  /// @return true if a complete frame was delivered to the callback
  bool FeedByte(uint8_t byte) noexcept {
    // MISRA C++: All switch statements must have default case
    switch (rx_state_) {
      case RxState::kIdle:
        if (byte == kSerialSyncByte1) {
          rx_buf_[0] = byte;
          rx_pos_ = 1U;
          rx_state_ = RxState::kWaitSync2;
        } else {
          ++stats_.sync_errors;
          IncrementErrorCount();
        }
        return false;

      case RxState::kWaitSync2:
        if (byte == kSerialSyncByte2) {
          rx_buf_[rx_pos_] = byte;
          ++rx_pos_;
          rx_state_ = RxState::kWaitHeader;
        } else {
          ++stats_.sync_errors;
          IncrementErrorCount();
          ResetRxState();
          // Re-check if this byte is start of new sync
          if (byte == kSerialSyncByte1) {
            rx_buf_[0] = byte;
            rx_pos_ = 1U;
            rx_state_ = RxState::kWaitSync2;
          }
        }
        return false;

      case RxState::kWaitHeader:
        rx_buf_[rx_pos_] = byte;
        ++rx_pos_;
        // We need full header (10 bytes)
        if (rx_pos_ >= kSerialHeaderSize) {
          // Validate magic
          const uint16_t magic = ReadLE16(rx_buf_ + 2U);
          if (magic == kSerialAckMagic) {
            // ACK frame path: need 8 bytes total
            if (rx_pos_ >= kSerialAckFrameSize) {
              HandleAckFrame();
              ResetRxState();
            } else {
              // Continue collecting ACK bytes
              rx_state_ = RxState::kWaitHeader;
            }
            return false;
          }
          if (magic != kSerialMagic) {
            ++stats_.sync_errors;
            IncrementErrorCount();
            ResetRxState();
            return false;
          }
          // Parse msg_len
          rx_msg_len_ = ReadLE16(rx_buf_ + 4U);
          // Sanity check
          const uint32_t total = static_cast<uint32_t>(rx_msg_len_) + 1U; // +tail
          const uint32_t min_msg_len = kSerialHeaderSize + kSerialCrcSize;
          if (total > cfg_.frame_max_size || rx_msg_len_ < min_msg_len) {
            ++stats_.oversize_errors;
            IncrementErrorCount();
            ResetRxState();
            return false;
          }
          // How many more bytes for payload?
          // msg_len includes header(10) + payload + crc(2)
          // payload_size = msg_len - 10 - 2
          rx_payload_size_ = static_cast<uint16_t>(rx_msg_len_ - kSerialHeaderSize - kSerialCrcSize);
          if (rx_payload_size_ > 0U) {
            rx_state_ = RxState::kWaitPayload;
          } else {
            rx_state_ = RxState::kWaitCrc;
          }
        }
        return false;

      case RxState::kWaitPayload: {
        rx_buf_[rx_pos_] = byte;
        ++rx_pos_;
        const uint32_t payload_collected = rx_pos_ - kSerialHeaderSize;
        if (payload_collected >= static_cast<uint32_t>(rx_payload_size_)) {
          rx_state_ = RxState::kWaitCrc;
        }
        return false;
      }

      case RxState::kWaitCrc: {
        rx_buf_[rx_pos_] = byte;
        ++rx_pos_;
        // We need: header + payload + crc(2) + tail(1)
        const uint32_t expected_total = static_cast<uint32_t>(rx_msg_len_) + 1U;
        if (rx_pos_ >= expected_total) {
          return ValidateAndDeliver();
        }
        return false;
      }

      default:
        // MISRA C++: Handle unexpected state
        OSP_ASSERT(false && "Invalid RxState");
        ResetRxState();
        return false;
    }
  }

  /// @brief Validate CRC and deliver frame to callback
  /// @return true if frame was valid and delivered
  bool ValidateAndDeliver() noexcept {
    // Check tail byte
    const uint32_t tail_pos = static_cast<uint32_t>(rx_msg_len_);
    if (rx_buf_[tail_pos] != kSerialTailByte) {
      ++stats_.sync_errors;
      IncrementErrorCount();
      ResetRxState();
      return false;
    }

    // CRC check: over bytes [0 .. msg_len-2) i.e. everything before CRC
    const uint32_t crc_data_len = static_cast<uint32_t>(rx_msg_len_) - kSerialCrcSize;
    const uint16_t computed = Crc16Ccitt::Calculate(rx_buf_, crc_data_len);
    const uint16_t received = ReadLE16(rx_buf_ + crc_data_len);

    if (computed != received) {
      ++stats_.crc_errors;
      IncrementErrorCount();
      ResetRxState();
      return false;
    }

    // Extract fields
    const uint16_t seq = ReadLE16(rx_buf_ + 6U);
    const uint16_t type_index = ReadLE16(rx_buf_ + 8U);
    const uint8_t* payload_ptr = rx_buf_ + kSerialHeaderSize;

    // Sequence gap detection (handle wraparound)
    if (cfg_.reliability.enable_seq_check && stats_.frames_received > 0U) {
      const uint16_t expected_seq = rx_expected_seq_;
      if (seq != expected_seq) {
        // Calculate gap with wraparound handling
        uint16_t gap;
        if (seq > expected_seq) {
          gap = static_cast<uint16_t>(seq - expected_seq);
        } else {
          // Wraparound case: seq wrapped around to 0
          gap = static_cast<uint16_t>((65536U - expected_seq) + seq);
        }
        stats_.seq_gaps += gap;
      }
    }
    rx_expected_seq_ = static_cast<uint16_t>(seq + 1U);  // Explicit wraparound

    ++stats_.frames_received;

    // Send ACK if enabled
    if (cfg_.reliability.enable_ack) {
      SendAck(seq);
    }

    // Deliver to callback
    if (rx_cb_ != nullptr) {
      rx_cb_(payload_ptr, static_cast<uint32_t>(rx_payload_size_), type_index, seq, rx_ctx_);
    }

    ResetRxState();
    return true;
  }

  /// @brief Handle received ACK frame
  void HandleAckFrame() noexcept {
    // ACK frame: sync(2) + ack_magic(2) + ack_seq(2) + crc(2)
    // Validate CRC over first 6 bytes
    const uint16_t computed = Crc16Ccitt::Calculate(rx_buf_, 6U);
    const uint16_t received = ReadLE16(rx_buf_ + 6U);
    if (computed != received) {
      ++stats_.crc_errors;
      IncrementErrorCount();
      return;
    }
    // ACK received - in a full implementation we'd mark the seq as acknowledged
    // For now, just count it
    (void)ReadLE16(rx_buf_ + 4U);  // ack_seq
  }

  /// @brief Send ACK frame for received sequence number
  /// @param seq Sequence number to acknowledge
  void SendAck(uint16_t seq) noexcept {
    uint8_t ack[kSerialAckFrameSize];
    ack[0] = kSerialSyncByte1;
    ack[1] = kSerialSyncByte2;
    WriteLE16(ack + 2U, kSerialAckMagic);
    WriteLE16(ack + 4U, seq);
    const uint16_t crc = Crc16Ccitt::Calculate(ack, 6U);
    WriteLE16(ack + 6U, crc);
    // Best-effort write
    if (fd_ >= 0) {
      (void)::write(fd_, ack, kSerialAckFrameSize);
    }
  }

  // ------------------------------------------------------------------
  // Port configuration (POSIX termios)
  // ------------------------------------------------------------------

  /// @brief Configure serial port parameters
  /// @return Success or error code
  expected<void, SerialError> ConfigurePort() noexcept {
#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)
    struct termios tio;
    std::memset(&tio, 0, sizeof(tio));

    if (::tcgetattr(fd_, &tio) != 0) {
      return expected<void, SerialError>::error(SerialError::kConfigFailed);
    }

    // Raw mode
    tio.c_iflag &= static_cast<tcflag_t>(~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR |
                      IGNCR | ICRNL | IXON | IXOFF | IXANY));
    tio.c_oflag &= static_cast<tcflag_t>(~OPOST);
    tio.c_lflag &= static_cast<tcflag_t>(~(ECHO | ECHONL | ICANON | ISIG | IEXTEN));
    tio.c_cflag &= static_cast<tcflag_t>(~(CSIZE | PARENB | PARODD | CSTOPB));
    tio.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD);

    // Data bits (MISRA C++: All switch statements must have default case)
    switch (cfg_.data_bits) {
      case 5U:
        tio.c_cflag |= CS5;
        break;
      case 6U:
        tio.c_cflag |= CS6;
        break;
      case 7U:
        tio.c_cflag |= CS7;
        break;
      case 8U:
        tio.c_cflag |= CS8;
        break;
      default:
        tio.c_cflag |= CS8;
        break;
    }

    // Parity (MISRA C++: Explicit comparison)
    if (cfg_.parity == 1U) {       // Odd
      tio.c_cflag |= static_cast<tcflag_t>(PARENB | PARODD);
    } else if (cfg_.parity == 2U) { // Even
      tio.c_cflag |= PARENB;
    } else {
      // No parity (default)
    }

    // Stop bits
    if (cfg_.stop_bits == 2U) {
      tio.c_cflag |= CSTOPB;
    }

    // Flow control
    if (cfg_.flow_control == 1U) {
#ifdef CRTSCTS
      tio.c_cflag |= CRTSCTS;
#endif
    } else if (cfg_.flow_control == 2U) {
      tio.c_iflag |= static_cast<tcflag_t>(IXON | IXOFF);
    } else {
      // No flow control (default)
    }

    // Non-blocking: VMIN=0, VTIME=0
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    // Baud rate
    const speed_t speed = BaudToSpeed(cfg_.baud_rate);
    ::cfsetispeed(&tio, speed);
    ::cfsetospeed(&tio, speed);

    if (::tcsetattr(fd_, TCSANOW, &tio) != 0) {
      return expected<void, SerialError>::error(SerialError::kConfigFailed);
    }

    // Flush
    ::tcflush(fd_, TCIOFLUSH);

    return expected<void, SerialError>::success();
#else
    return expected<void, SerialError>::error(SerialError::kConfigFailed);
#endif
  }

  /// @brief Convert baud rate to termios speed_t
  /// @param baud Baud rate value
  /// @return termios speed constant
  static speed_t BaudToSpeed(uint32_t baud) noexcept {
    // MISRA C++: All switch statements must have default case
    switch (baud) {
      case 9600U:
        return B9600;
      case 19200U:
        return B19200;
      case 38400U:
        return B38400;
      case 57600U:
        return B57600;
      case 115200U:
        return B115200;
      case 230400U:
        return B230400;
#ifdef B460800
      case 460800U:
        return B460800;
#endif
#ifdef B921600
      case 921600U:
        return B921600;
#endif
      default:
        return B115200;
    }
  }

  // ------------------------------------------------------------------
  // Write helpers (with retry and backoff)
  // ------------------------------------------------------------------

  /// @brief Write all data with retry and backoff
  /// @param data Pointer to data buffer
  /// @param len Length of data in bytes
  /// @param seq Sequence number (for future ACK/retry logic)
  /// @return Success or error code
  expected<void, SerialError> WriteAll(const uint8_t* data, uint32_t len,
                                        uint16_t /*seq*/) noexcept {
    OSP_ASSERT(data != nullptr);
    OSP_ASSERT(len > 0U);

    uint32_t written = 0U;
    uint32_t retry_count = 0U;

    while (written < len) {
      const ssize_t n = ::write(fd_, data + written, len - written);
      if (n < 0) {
        const int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK) {
          // Non-blocking fd would block, retry with backoff
          if (retry_count >= cfg_.write_retry_count) {
            return expected<void, SerialError>::error(SerialError::kSendFailed);
          }
          ++retry_count;
          ++stats_.write_retries;
          ::usleep(cfg_.write_retry_delay_us);
          continue;
        }
        // Other error
        return expected<void, SerialError>::error(SerialError::kSendFailed);
      }
      written += static_cast<uint32_t>(n);
      retry_count = 0U;  // Reset retry count on successful write
    }

    stats_.bytes_sent += len;
    ++stats_.frames_sent;
    return expected<void, SerialError>::success();
  }

  // ------------------------------------------------------------------
  // Byte-order helpers (little-endian)
  // ------------------------------------------------------------------

  /// @brief Write 16-bit value in little-endian format
  /// @param p Pointer to output buffer
  /// @param v Value to write
  static void WriteLE16(uint8_t* p, uint16_t v) noexcept {
    OSP_ASSERT(p != nullptr);
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
  }

  /// @brief Read 16-bit value in little-endian format
  /// @param p Pointer to input buffer
  /// @return 16-bit value
  static uint16_t ReadLE16(const uint8_t* p) noexcept {
    OSP_ASSERT(p != nullptr);
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8));
  }

  // ------------------------------------------------------------------
  // Timestamp helper
  // ------------------------------------------------------------------

  /// @brief Get current time in milliseconds
  /// @return Timestamp in milliseconds
  static uint64_t NowMs() noexcept {
    struct timeval tv;
    ::gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000U +
           static_cast<uint64_t>(tv.tv_usec) / 1000U;
  }

  // ------------------------------------------------------------------
  // Data members (Google C++ Style: snake_case_ with trailing underscore)
  // ------------------------------------------------------------------

  SerialConfig cfg_;
  int fd_;
  uint16_t tx_seq_;
  uint16_t rx_expected_seq_;

  RxState rx_state_;
  uint32_t rx_pos_;
  uint16_t rx_msg_len_;
  uint16_t rx_payload_size_;
  uint8_t rx_buf_[OSP_SERIAL_MAX_FRAME_SIZE];

  SerialRxCallback rx_cb_;
  void* rx_ctx_;

  SerialStatistics stats_;

  // Health monitoring timestamps
  uint64_t last_byte_time_ms_;
  uint64_t last_tx_time_ms_;
  uint64_t last_rx_time_ms_;

  // Rate limiting state
  uint64_t frame_count_window_start_ms_;
  uint32_t frames_sent_in_window_;

  // Error rate tracking
  uint32_t error_count_in_window_;
};

}  // namespace osp

#endif  // OSP_SERIAL_TRANSPORT_HPP_
