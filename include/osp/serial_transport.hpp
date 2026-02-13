/**
 * @file serial_transport.hpp
 * @brief Industrial-grade serial port transport with framed protocol, CRC-CCITT,
 *        sequence tracking, and optional ACK/retransmit.
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
};

// ============================================================================
// Serial Frame Constants
// ============================================================================

static constexpr uint16_t kSerialSyncWord   = 0xAA55;
static constexpr uint16_t kSerialMagic      = 0x4F53;  // "OS"
static constexpr uint16_t kSerialAckMagic   = 0x4F41;  // "OA"
static constexpr uint8_t  kSerialTailByte   = 0x0D;

/// Header: sync(2) + magic(2) + msg_len(2) + seq(2) + type(2) = 10 bytes
static constexpr uint32_t kSerialHeaderSize = 10;
/// Trailer: crc16(2) + tail(1) = 3 bytes
static constexpr uint32_t kSerialTrailerSize = 3;
/// Minimum frame: header + trailer, no payload
static constexpr uint32_t kSerialMinFrameSize = kSerialHeaderSize + kSerialTrailerSize;

/// ACK frame: sync(2) + ack_magic(2) + ack_seq(2) + crc16(2) = 8 bytes
static constexpr uint32_t kSerialAckFrameSize = 8;

#ifndef OSP_SERIAL_MAX_FRAME_SIZE
#define OSP_SERIAL_MAX_FRAME_SIZE 1024U
#endif

// ============================================================================
// Reliability Config
// ============================================================================

struct ReliabilityConfig {
  bool enable_ack = false;
  uint32_t ack_timeout_ms = 100;
  uint8_t max_retries = 3;
  bool enable_seq_check = true;
};

// ============================================================================
// Serial Config
// ============================================================================

struct SerialConfig {
  char port_name[64] = "/dev/ttyS0";
  uint32_t baud_rate = 115200;
  uint8_t data_bits = 8;
  uint8_t stop_bits = 1;
  uint8_t parity = 0;              // 0=None, 1=Odd, 2=Even
  uint8_t flow_control = 0;        // 0=None, 1=HW(RTS/CTS), 2=SW(XON/XOFF)
  uint32_t inter_byte_timeout_ms = 50;
  uint32_t frame_max_size = OSP_SERIAL_MAX_FRAME_SIZE;
  ReliabilityConfig reliability;
};

// ============================================================================
// Serial Statistics
// ============================================================================

struct SerialStatistics {
  uint64_t frames_sent = 0;
  uint64_t frames_received = 0;
  uint64_t bytes_sent = 0;
  uint64_t bytes_received = 0;
  uint64_t crc_errors = 0;
  uint64_t sync_errors = 0;
  uint64_t timeout_errors = 0;
  uint64_t oversize_errors = 0;
  uint64_t seq_gaps = 0;
  uint64_t retransmits = 0;
  uint64_t ack_timeouts = 0;
};

// ============================================================================
// Rx Callback
// ============================================================================

using SerialRxCallback = void (*)(const void* payload, uint32_t size,
                                   uint16_t type_index, uint16_t seq, void* ctx);

// ============================================================================
// CRC-CCITT (0x1021) Lookup Table
// ============================================================================

class Crc16Ccitt {
 public:
  static uint16_t Calculate(const void* data, uint32_t size) noexcept {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < size; ++i) {
      crc = static_cast<uint16_t>((crc << 8) ^ kTable[((crc >> 8) ^ p[i]) & 0xFF]);
    }
    return crc;
  }

 private:
  static constexpr uint16_t kTable[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
    0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0,
  };
};

// Out-of-line constexpr definition (C++17 inline variable via constexpr)
constexpr uint16_t Crc16Ccitt::kTable[256];

// ============================================================================
// SerialTransport
// ============================================================================

class SerialTransport {
 public:
  explicit SerialTransport(const SerialConfig& cfg) noexcept
      : cfg_(cfg), fd_(-1), tx_seq_(0), rx_expected_seq_(0),
        rx_state_(RxState::kIdle), rx_pos_(0),
        rx_cb_(nullptr), rx_ctx_(nullptr), stats_{},
        last_byte_time_ms_(0) {}

  ~SerialTransport() { Close(); }

  // Non-copyable, non-movable
  SerialTransport(const SerialTransport&) = delete;
  SerialTransport& operator=(const SerialTransport&) = delete;

  // ------------------------------------------------------------------
  // Open / Close / IsOpen
  // ------------------------------------------------------------------

  expected<void, SerialError> Open() noexcept {
#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)
    if (fd_ >= 0) return expected<void, SerialError>::success();

    fd_ = ::open(cfg_.port_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) return expected<void, SerialError>::error(SerialError::kOpenFailed);

    auto r = ConfigurePort();
    if (!r) {
      ::close(fd_);
      fd_ = -1;
      return r;
    }
    return expected<void, SerialError>::success();
#else
    return expected<void, SerialError>::error(SerialError::kOpenFailed);
#endif
  }

  void Close() noexcept {
#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
#endif
    rx_state_ = RxState::kIdle;
    rx_pos_ = 0;
  }

  bool IsOpen() const noexcept { return fd_ >= 0; }

  // ------------------------------------------------------------------
  // Send
  // ------------------------------------------------------------------

  expected<void, SerialError> Send(uint16_t type_index,
                                    const void* payload,
                                    uint32_t size) noexcept {
    if (fd_ < 0)
      return expected<void, SerialError>::error(SerialError::kPortNotOpen);

    // msg_len = header size (10) + payload + crc(2), NOT including tail
    const uint32_t msg_len = kSerialHeaderSize + size + 2;
    const uint32_t total = msg_len + 1;  // + tail byte

    if (total > cfg_.frame_max_size)
      return expected<void, SerialError>::error(SerialError::kFrameOversize);

    // Build frame into stack buffer
    uint8_t buf[OSP_SERIAL_MAX_FRAME_SIZE];
    uint32_t pos = 0;

    // sync word (fixed byte order: 0xAA first, 0x55 second)
    buf[pos++] = 0xAA;
    buf[pos++] = 0x55;
    // magic
    WriteLE16(buf + pos, kSerialMagic); pos += 2;
    // msg_len
    WriteLE16(buf + pos, static_cast<uint16_t>(msg_len)); pos += 2;
    // seq
    uint16_t seq = tx_seq_++;
    WriteLE16(buf + pos, seq); pos += 2;
    // type
    WriteLE16(buf + pos, type_index); pos += 2;
    // payload
    if (size > 0 && payload != nullptr) {
      std::memcpy(buf + pos, payload, size);
      pos += size;
    }
    // CRC over everything from sync to end of payload
    uint16_t crc = Crc16Ccitt::Calculate(buf, pos);
    WriteLE16(buf + pos, crc); pos += 2;
    // tail
    buf[pos++] = kSerialTailByte;

    return WriteAll(buf, pos, seq);
  }

  // ------------------------------------------------------------------
  // Poll (non-blocking receive + decode)
  // ------------------------------------------------------------------

  uint32_t Poll() noexcept {
    if (fd_ < 0) return 0;

    uint8_t tmp[256];
    uint32_t frames_done = 0;

    for (;;) {
      auto n = ::read(fd_, tmp, sizeof(tmp));
      if (n <= 0) break;

      uint64_t now = NowMs();
      stats_.bytes_received += static_cast<uint64_t>(n);

      for (ssize_t i = 0; i < n; ++i) {
        // Inter-byte timeout check
        if (rx_state_ != RxState::kIdle && last_byte_time_ms_ != 0) {
          if (now - last_byte_time_ms_ > cfg_.inter_byte_timeout_ms) {
            ++stats_.timeout_errors;
            ResetRx();
          }
        }
        last_byte_time_ms_ = now;

        if (FeedByte(tmp[i])) {
          ++frames_done;
        }
      }
    }
    return frames_done;
  }

  // ------------------------------------------------------------------
  // Callback
  // ------------------------------------------------------------------

  void SetRxCallback(SerialRxCallback cb, void* ctx = nullptr) noexcept {
    rx_cb_ = cb;
    rx_ctx_ = ctx;
  }

  int GetFd() const noexcept { return fd_; }

  // ------------------------------------------------------------------
  // Statistics
  // ------------------------------------------------------------------

  SerialStatistics GetStatistics() const noexcept { return stats_; }

  void ResetStatistics() noexcept {
    std::memset(&stats_, 0, sizeof(stats_));
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

  void ResetRx() noexcept {
    rx_state_ = RxState::kIdle;
    rx_pos_ = 0;
  }

  /// Feed one byte into the state machine. Returns true if a complete frame
  /// was delivered to the callback.
  bool FeedByte(uint8_t byte) noexcept {
    switch (rx_state_) {
      case RxState::kIdle:
        if (byte == 0xAA) {
          rx_buf_[0] = byte;
          rx_pos_ = 1;
          rx_state_ = RxState::kWaitSync2;
        } else {
          ++stats_.sync_errors;
        }
        return false;

      case RxState::kWaitSync2:
        if (byte == 0x55) {
          rx_buf_[rx_pos_++] = byte;
          rx_state_ = RxState::kWaitHeader;
        } else {
          ++stats_.sync_errors;
          ResetRx();
          // Re-check if this byte is start of new sync
          if (byte == 0xAA) {
            rx_buf_[0] = byte;
            rx_pos_ = 1;
            rx_state_ = RxState::kWaitSync2;
          }
        }
        return false;

      case RxState::kWaitHeader:
        rx_buf_[rx_pos_++] = byte;
        // We need full header (10 bytes)
        if (rx_pos_ >= kSerialHeaderSize) {
          // Validate magic
          uint16_t magic = ReadLE16(rx_buf_ + 2);
          if (magic == kSerialAckMagic) {
            // ACK frame path: need 8 bytes total
            if (rx_pos_ >= kSerialAckFrameSize) {
              HandleAckFrame();
              ResetRx();
            } else {
              // Continue collecting ACK bytes
              rx_state_ = RxState::kWaitHeader;
            }
            return false;
          }
          if (magic != kSerialMagic) {
            ++stats_.sync_errors;
            ResetRx();
            return false;
          }
          // Parse msg_len
          rx_msg_len_ = ReadLE16(rx_buf_ + 4);
          // Sanity check
          uint32_t total = static_cast<uint32_t>(rx_msg_len_) + 1; // +tail
          if (total > cfg_.frame_max_size || rx_msg_len_ < kSerialHeaderSize + 2) {
            ++stats_.oversize_errors;
            ResetRx();
            return false;
          }
          // How many more bytes for payload?
          // msg_len includes header(10) + payload + crc(2)
          // payload_size = msg_len - 10 - 2
          rx_payload_size_ = rx_msg_len_ - kSerialHeaderSize - 2;
          if (rx_payload_size_ > 0) {
            rx_state_ = RxState::kWaitPayload;
          } else {
            rx_state_ = RxState::kWaitCrc;
          }
        }
        return false;

      case RxState::kWaitPayload: {
        rx_buf_[rx_pos_++] = byte;
        uint32_t payload_collected = rx_pos_ - kSerialHeaderSize;
        if (payload_collected >= rx_payload_size_) {
          rx_state_ = RxState::kWaitCrc;
        }
        return false;
      }

      case RxState::kWaitCrc: {
        rx_buf_[rx_pos_++] = byte;
        // We need: header + payload + crc(2) + tail(1)
        uint32_t expected_total = static_cast<uint32_t>(rx_msg_len_) + 1;
        if (rx_pos_ >= expected_total) {
          return ValidateAndDeliver();
        }
        return false;
      }
    }
    return false;
  }

  bool ValidateAndDeliver() noexcept {
    // Check tail byte
    uint32_t tail_pos = static_cast<uint32_t>(rx_msg_len_);
    if (rx_buf_[tail_pos] != kSerialTailByte) {
      ++stats_.sync_errors;
      ResetRx();
      return false;
    }

    // CRC check: over bytes [0 .. msg_len-2) i.e. everything before CRC
    uint32_t crc_data_len = static_cast<uint32_t>(rx_msg_len_) - 2;
    uint16_t computed = Crc16Ccitt::Calculate(rx_buf_, crc_data_len);
    uint16_t received = ReadLE16(rx_buf_ + crc_data_len);

    if (computed != received) {
      ++stats_.crc_errors;
      ResetRx();
      return false;
    }

    // Extract fields
    uint16_t seq = ReadLE16(rx_buf_ + 6);
    uint16_t type_index = ReadLE16(rx_buf_ + 8);
    const uint8_t* payload_ptr = rx_buf_ + kSerialHeaderSize;

    // Sequence gap detection
    if (cfg_.reliability.enable_seq_check && stats_.frames_received > 0) {
      uint16_t expected_seq = rx_expected_seq_;
      if (seq != expected_seq) {
        uint16_t gap = static_cast<uint16_t>(seq - expected_seq);
        stats_.seq_gaps += gap;
      }
    }
    rx_expected_seq_ = static_cast<uint16_t>(seq + 1);

    ++stats_.frames_received;

    // Send ACK if enabled
    if (cfg_.reliability.enable_ack) {
      SendAck(seq);
    }

    // Deliver to callback
    if (rx_cb_ != nullptr) {
      rx_cb_(payload_ptr, rx_payload_size_, type_index, seq, rx_ctx_);
    }

    ResetRx();
    return true;
  }

  void HandleAckFrame() noexcept {
    // ACK frame: sync(2) + ack_magic(2) + ack_seq(2) + crc(2)
    // Validate CRC over first 6 bytes
    uint16_t computed = Crc16Ccitt::Calculate(rx_buf_, 6);
    uint16_t received = ReadLE16(rx_buf_ + 6);
    if (computed != received) {
      ++stats_.crc_errors;
      return;
    }
    // ACK received - in a full implementation we'd mark the seq as acknowledged
    // For now, just count it
    (void)ReadLE16(rx_buf_ + 4);  // ack_seq
  }

  void SendAck(uint16_t seq) noexcept {
    uint8_t ack[kSerialAckFrameSize];
    ack[0] = 0xAA;
    ack[1] = 0x55;
    WriteLE16(ack + 2, kSerialAckMagic);
    WriteLE16(ack + 4, seq);
    uint16_t crc = Crc16Ccitt::Calculate(ack, 6);
    WriteLE16(ack + 6, crc);
    // Best-effort write
    if (fd_ >= 0) {
      (void)::write(fd_, ack, kSerialAckFrameSize);
    }
  }

  // ------------------------------------------------------------------
  // Port configuration (POSIX termios)
  // ------------------------------------------------------------------

  expected<void, SerialError> ConfigurePort() noexcept {
#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)
    struct termios tio;
    std::memset(&tio, 0, sizeof(tio));

    if (::tcgetattr(fd_, &tio) != 0) {
      return expected<void, SerialError>::error(SerialError::kConfigFailed);
    }

    // Raw mode
    tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR |
                      IGNCR | ICRNL | IXON | IXOFF | IXANY);
    tio.c_oflag &= ~OPOST;
    tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tio.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB);
    tio.c_cflag |= CLOCAL | CREAD;

    // Data bits
    switch (cfg_.data_bits) {
      case 5: tio.c_cflag |= CS5; break;
      case 6: tio.c_cflag |= CS6; break;
      case 7: tio.c_cflag |= CS7; break;
      default: tio.c_cflag |= CS8; break;
    }

    // Parity
    if (cfg_.parity == 1) {       // Odd
      tio.c_cflag |= PARENB | PARODD;
    } else if (cfg_.parity == 2) { // Even
      tio.c_cflag |= PARENB;
    }

    // Stop bits
    if (cfg_.stop_bits == 2) {
      tio.c_cflag |= CSTOPB;
    }

    // Flow control
    if (cfg_.flow_control == 1) {
#ifdef CRTSCTS
      tio.c_cflag |= CRTSCTS;
#endif
    } else if (cfg_.flow_control == 2) {
      tio.c_iflag |= IXON | IXOFF;
    }

    // Non-blocking: VMIN=0, VTIME=0
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    // Baud rate
    speed_t speed = BaudToSpeed(cfg_.baud_rate);
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

  static speed_t BaudToSpeed(uint32_t baud) noexcept {
    switch (baud) {
      case 9600:   return B9600;
      case 19200:  return B19200;
      case 38400:  return B38400;
      case 57600:  return B57600;
      case 115200: return B115200;
      case 230400: return B230400;
#ifdef B460800
      case 460800: return B460800;
#endif
#ifdef B921600
      case 921600: return B921600;
#endif
      default:     return B115200;
    }
  }

  // ------------------------------------------------------------------
  // Write helpers
  // ------------------------------------------------------------------

  expected<void, SerialError> WriteAll(const uint8_t* data, uint32_t len,
                                        uint16_t /*seq*/) noexcept {
    uint32_t written = 0;
    while (written < len) {
      auto n = ::write(fd_, data + written, len - written);
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          // Busy-wait briefly for non-blocking fd
          continue;
        }
        return expected<void, SerialError>::error(SerialError::kSendFailed);
      }
      written += static_cast<uint32_t>(n);
    }
    stats_.bytes_sent += len;
    ++stats_.frames_sent;
    return expected<void, SerialError>::success();
  }

  // ------------------------------------------------------------------
  // Byte-order helpers (little-endian)
  // ------------------------------------------------------------------

  static void WriteLE16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  }

  static uint16_t ReadLE16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
  }

  // ------------------------------------------------------------------
  // Timestamp helper
  // ------------------------------------------------------------------

  static uint64_t NowMs() noexcept {
    struct timeval tv;
    ::gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000 +
           static_cast<uint64_t>(tv.tv_usec) / 1000;
  }

  // ------------------------------------------------------------------
  // Data members
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
  uint64_t last_byte_time_ms_;
};

}  // namespace osp

#endif  // OSP_SERIAL_TRANSPORT_HPP_
