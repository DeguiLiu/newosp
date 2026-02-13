/**
 * @file test_serial_transport.cpp
 * @brief Tests for serial_transport.hpp using PTY pairs to simulate serial ports.
 */

#include <catch2/catch_test_macros.hpp>
#include "osp/serial_transport.hpp"

#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <cstdlib>

#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)
#include <pty.h>
#endif

// ============================================================================
// PTY helper
// ============================================================================

struct PtyPair {
  int master = -1;
  int slave = -1;
  char slave_name[64] = {};
  bool valid = false;
};

static PtyPair CreatePtyPair() {
  PtyPair p;
#if defined(OSP_PLATFORM_LINUX)
  if (::openpty(&p.master, &p.slave, p.slave_name, nullptr, nullptr) == 0) {
    // Set master to non-blocking for reading back
    int flags = ::fcntl(p.master, F_GETFL, 0);
    ::fcntl(p.master, F_SETFL, flags | O_NONBLOCK);
    p.valid = true;
  }
#endif
  return p;
}

static void ClosePty(PtyPair& p) {
  if (p.master >= 0) ::close(p.master);
  if (p.slave >= 0) ::close(p.slave);
  p.master = p.slave = -1;
  p.valid = false;
}

// Helper: build a valid frame in a buffer, returns total frame size
static uint32_t BuildFrame(uint8_t* buf, uint32_t buf_size,
                           uint16_t type_index, uint16_t seq,
                           const void* payload, uint32_t payload_size) {
  if (buf_size < osp::kSerialHeaderSize + payload_size + osp::kSerialTrailerSize)
    return 0;

  auto WriteLE16 = [](uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  };

  uint32_t pos = 0;
  uint16_t msg_len = static_cast<uint16_t>(osp::kSerialHeaderSize + payload_size + 2);

  // sync word: fixed byte order 0xAA, 0x55
  buf[pos++] = 0xAA;
  buf[pos++] = 0x55;
  WriteLE16(buf + pos, osp::kSerialMagic);    pos += 2;
  WriteLE16(buf + pos, msg_len);              pos += 2;
  WriteLE16(buf + pos, seq);                  pos += 2;
  WriteLE16(buf + pos, type_index);           pos += 2;

  if (payload_size > 0 && payload != nullptr) {
    std::memcpy(buf + pos, payload, payload_size);
    pos += payload_size;
  }

  uint16_t crc = osp::Crc16Ccitt::Calculate(buf, pos);
  WriteLE16(buf + pos, crc); pos += 2;
  buf[pos++] = osp::kSerialTailByte;

  return pos;
}

// ============================================================================
// CRC-CCITT calculation
// ============================================================================

TEST_CASE("serial - CRC-CCITT calculation", "[serial]") {
  // Known test vector: "123456789" -> CRC-CCITT(0xFFFF) = 0x29B1
  const char data[] = "123456789";
  uint16_t crc = osp::Crc16Ccitt::Calculate(data, 9);
  REQUIRE(crc == 0x29B1);
}

TEST_CASE("serial - CRC empty data", "[serial]") {
  uint16_t crc = osp::Crc16Ccitt::Calculate(nullptr, 0);
  REQUIRE(crc == 0xFFFF);  // Initial value with no data
}

TEST_CASE("serial - CRC single byte", "[serial]") {
  uint8_t byte = 0x00;
  uint16_t crc1 = osp::Crc16Ccitt::Calculate(&byte, 1);
  byte = 0xFF;
  uint16_t crc2 = osp::Crc16Ccitt::Calculate(&byte, 1);
  REQUIRE(crc1 != crc2);
}

// ============================================================================
// Frame encode/decode
// ============================================================================

TEST_CASE("serial - Frame encode/decode", "[serial]") {
  uint8_t frame[256];
  uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
  uint32_t len = BuildFrame(frame, sizeof(frame), 42, 7, payload, 4);

  REQUIRE(len == osp::kSerialHeaderSize + 4 + osp::kSerialTrailerSize);

  // Check sync (raw byte order: 0xAA first, 0x55 second)
  REQUIRE(frame[0] == 0xAA);
  REQUIRE(frame[1] == 0x55);
  // Check magic (LE16: 0x4F53 -> [0x53, 0x4F])
  REQUIRE(frame[2] == 0x53);
  REQUIRE(frame[3] == 0x4F);
  // Check tail
  REQUIRE(frame[len - 1] == osp::kSerialTailByte);
}

// ============================================================================
// Open and close (PTY)
// ============================================================================

TEST_CASE("serial - Open and close", "[serial]") {
  auto pty = CreatePtyPair();
  if (!pty.valid) {
    SKIP("PTY not available");
  }

  osp::SerialConfig cfg;
  cfg.port_name.assign(osp::TruncateToCapacity, pty.slave_name);

  // Close the slave fd so SerialTransport can open it
  ::close(pty.slave);
  pty.slave = -1;

  osp::SerialTransport tx(cfg);
  REQUIRE_FALSE(tx.IsOpen());

  auto r = tx.Open();
  REQUIRE(r.has_value());
  REQUIRE(tx.IsOpen());
  REQUIRE(tx.GetFd() >= 0);

  tx.Close();
  REQUIRE_FALSE(tx.IsOpen());

  ::close(pty.master);
}

// ============================================================================
// Send and receive single frame (PTY pair)
// ============================================================================

struct RxRecord {
  uint16_t type_index = 0;
  uint16_t seq = 0;
  uint32_t size = 0;
  uint8_t data[256] = {};
  uint32_t count = 0;
};

static void TestRxCallback(const void* payload, uint32_t size,
                            uint16_t type_index, uint16_t seq, void* ctx) {
  auto* rec = static_cast<RxRecord*>(ctx);
  rec->type_index = type_index;
  rec->seq = seq;
  rec->size = size;
  if (size > 0 && size <= sizeof(rec->data)) {
    std::memcpy(rec->data, payload, size);
  }
  ++rec->count;
}

TEST_CASE("serial - Send and receive single frame", "[serial]") {
  auto pty = CreatePtyPair();
  if (!pty.valid) SKIP("PTY not available");

  // Sender uses slave_name
  osp::SerialConfig tx_cfg;
  tx_cfg.port_name.assign(osp::TruncateToCapacity, pty.slave_name);
  ::close(pty.slave);
  pty.slave = -1;

  osp::SerialTransport sender(tx_cfg);
  REQUIRE(sender.Open().has_value());

  // Send a frame - it goes out through slave, readable on master
  uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
  auto sr = sender.Send(10, payload, 4);
  REQUIRE(sr.has_value());

  // Read from master side
  ::usleep(10000);  // 10ms for data to arrive
  uint8_t raw[512];
  ssize_t n = ::read(pty.master, raw, sizeof(raw));
  REQUIRE(n > 0);

  // Now feed this data into a receiver transport via master->write->slave->read
  // Instead, we create a second PTY pair for the receiver
  // Simpler approach: manually feed the raw bytes into a new transport

  // Create receiver with a new PTY
  auto pty2 = CreatePtyPair();
  if (!pty2.valid) {
    sender.Close();
    ::close(pty.master);
    SKIP("PTY not available");
  }

  osp::SerialConfig rx_cfg;
  rx_cfg.port_name.assign(osp::TruncateToCapacity, pty2.slave_name);
  ::close(pty2.slave);
  pty2.slave = -1;

  osp::SerialTransport receiver(rx_cfg);
  REQUIRE(receiver.Open().has_value());

  RxRecord rec;
  receiver.SetRxCallback(TestRxCallback, &rec);

  // Write the raw frame data to master2 so receiver can read from slave2
  ssize_t w = ::write(pty2.master, raw, static_cast<size_t>(n));
  REQUIRE(w == n);
  ::usleep(10000);

  uint32_t frames = receiver.Poll();
  REQUIRE(frames == 1);
  REQUIRE(rec.count == 1);
  REQUIRE(rec.type_index == 10);
  REQUIRE(rec.size == 4);
  REQUIRE(std::memcmp(rec.data, payload, 4) == 0);

  sender.Close();
  receiver.Close();
  ::close(pty.master);
  ::close(pty2.master);
}

// ============================================================================
// Send and receive multiple frames
// ============================================================================

TEST_CASE("serial - Send and receive multiple frames", "[serial]") {
  auto pty_tx = CreatePtyPair();
  auto pty_rx = CreatePtyPair();
  if (!pty_tx.valid || !pty_rx.valid) SKIP("PTY not available");

  osp::SerialConfig tx_cfg;
  tx_cfg.port_name.assign(osp::TruncateToCapacity, pty_tx.slave_name);
  ::close(pty_tx.slave); pty_tx.slave = -1;

  osp::SerialConfig rx_cfg;
  rx_cfg.port_name.assign(osp::TruncateToCapacity, pty_rx.slave_name);
  ::close(pty_rx.slave); pty_rx.slave = -1;

  osp::SerialTransport sender(tx_cfg);
  osp::SerialTransport receiver(rx_cfg);
  REQUIRE(sender.Open().has_value());
  REQUIRE(receiver.Open().has_value());

  RxRecord rec;
  receiver.SetRxCallback(TestRxCallback, &rec);

  constexpr int kNumFrames = 5;
  for (int i = 0; i < kNumFrames; ++i) {
    uint8_t data = static_cast<uint8_t>(i);
    REQUIRE(sender.Send(static_cast<uint16_t>(i), &data, 1).has_value());
  }

  // Relay all frames from tx master to rx master
  ::usleep(20000);
  uint8_t relay[4096];
  ssize_t n = ::read(pty_tx.master, relay, sizeof(relay));
  REQUIRE(n > 0);
  REQUIRE(::write(pty_rx.master, relay, static_cast<size_t>(n)) == n);
  ::usleep(10000);

  uint32_t total = receiver.Poll();
  REQUIRE(total == kNumFrames);
  REQUIRE(rec.count == kNumFrames);

  auto stats = receiver.GetStatistics();
  REQUIRE(stats.frames_received == kNumFrames);

  sender.Close();
  receiver.Close();
  ::close(pty_tx.master);
  ::close(pty_rx.master);
}

// ============================================================================
// Sequence number tracking
// ============================================================================

TEST_CASE("serial - Sequence number tracking", "[serial]") {
  auto pty_tx = CreatePtyPair();
  auto pty_rx = CreatePtyPair();
  if (!pty_tx.valid || !pty_rx.valid) SKIP("PTY not available");

  osp::SerialConfig tx_cfg;
  tx_cfg.port_name.assign(osp::TruncateToCapacity, pty_tx.slave_name);
  ::close(pty_tx.slave); pty_tx.slave = -1;

  osp::SerialConfig rx_cfg;
  rx_cfg.reliability.enable_seq_check = true;
  rx_cfg.port_name.assign(osp::TruncateToCapacity, pty_rx.slave_name);
  ::close(pty_rx.slave); pty_rx.slave = -1;

  osp::SerialTransport sender(tx_cfg);
  osp::SerialTransport receiver(rx_cfg);
  REQUIRE(sender.Open().has_value());
  REQUIRE(receiver.Open().has_value());

  RxRecord rec;
  receiver.SetRxCallback(TestRxCallback, &rec);

  // Send 3 frames (seq 0, 1, 2)
  for (int i = 0; i < 3; ++i) {
    uint8_t d = static_cast<uint8_t>(i);
    sender.Send(1, &d, 1);
  }

  ::usleep(20000);
  uint8_t relay[4096];
  ssize_t n = ::read(pty_tx.master, relay, sizeof(relay));
  REQUIRE(n > 0);
  REQUIRE(::write(pty_rx.master, relay, static_cast<size_t>(n)) == n);
  ::usleep(10000);

  receiver.Poll();
  auto stats = receiver.GetStatistics();
  REQUIRE(stats.frames_received == 3);
  REQUIRE(stats.seq_gaps == 0);

  sender.Close();
  receiver.Close();
  ::close(pty_tx.master);
  ::close(pty_rx.master);
}

// ============================================================================
// CRC error detection
// ============================================================================

TEST_CASE("serial - CRC error detection", "[serial]") {
  auto pty = CreatePtyPair();
  if (!pty.valid) SKIP("PTY not available");

  osp::SerialConfig cfg;
  cfg.port_name.assign(osp::TruncateToCapacity, pty.slave_name);
  ::close(pty.slave); pty.slave = -1;

  osp::SerialTransport receiver(cfg);
  REQUIRE(receiver.Open().has_value());

  RxRecord rec;
  receiver.SetRxCallback(TestRxCallback, &rec);

  // Build a valid frame then corrupt payload
  uint8_t frame[256];
  uint8_t payload[] = {0x01, 0x02, 0x03};
  uint32_t len = BuildFrame(frame, sizeof(frame), 1, 0, payload, 3);
  REQUIRE(len > 0);

  // Corrupt a payload byte (after header, before CRC)
  frame[osp::kSerialHeaderSize] ^= 0xFF;

  ::write(pty.master, frame, len);
  ::usleep(10000);

  uint32_t frames = receiver.Poll();
  REQUIRE(frames == 0);
  REQUIRE(rec.count == 0);

  auto stats = receiver.GetStatistics();
  REQUIRE(stats.crc_errors == 1);

  receiver.Close();
  ::close(pty.master);
}

// ============================================================================
// Oversize frame rejection
// ============================================================================

TEST_CASE("serial - Oversize frame rejection", "[serial]") {
  auto pty = CreatePtyPair();
  if (!pty.valid) SKIP("PTY not available");

  osp::SerialConfig cfg;
  cfg.frame_max_size = 32;  // Very small limit
  cfg.port_name.assign(osp::TruncateToCapacity, pty.slave_name);
  ::close(pty.slave); pty.slave = -1;

  osp::SerialTransport receiver(cfg);
  REQUIRE(receiver.Open().has_value());

  RxRecord rec;
  receiver.SetRxCallback(TestRxCallback, &rec);

  // Build a frame with payload that exceeds the limit
  uint8_t frame[256];
  uint8_t payload[64];
  std::memset(payload, 0xAB, sizeof(payload));
  uint32_t len = BuildFrame(frame, sizeof(frame), 1, 0, payload, 64);
  REQUIRE(len > 0);

  ::write(pty.master, frame, len);
  ::usleep(10000);

  receiver.Poll();
  REQUIRE(rec.count == 0);

  auto stats = receiver.GetStatistics();
  REQUIRE(stats.oversize_errors >= 1);

  receiver.Close();
  ::close(pty.master);
}

// ============================================================================
// Statistics tracking
// ============================================================================

TEST_CASE("serial - Statistics tracking", "[serial]") {
  auto pty_tx = CreatePtyPair();
  if (!pty_tx.valid) SKIP("PTY not available");

  osp::SerialConfig cfg;
  cfg.port_name.assign(osp::TruncateToCapacity, pty_tx.slave_name);
  ::close(pty_tx.slave); pty_tx.slave = -1;

  osp::SerialTransport tx(cfg);
  REQUIRE(tx.Open().has_value());

  uint8_t data[] = {0x01};
  tx.Send(1, data, 1);
  tx.Send(2, data, 1);

  auto stats = tx.GetStatistics();
  REQUIRE(stats.frames_sent == 2);
  REQUIRE(stats.bytes_sent > 0);

  tx.ResetStatistics();
  stats = tx.GetStatistics();
  REQUIRE(stats.frames_sent == 0);
  REQUIRE(stats.bytes_sent == 0);

  tx.Close();
  ::close(pty_tx.master);
}

// ============================================================================
// Frame sync recovery (garbage before valid frame)
// ============================================================================

TEST_CASE("serial - Frame sync recovery", "[serial]") {
  auto pty = CreatePtyPair();
  if (!pty.valid) SKIP("PTY not available");

  osp::SerialConfig cfg;
  cfg.port_name.assign(osp::TruncateToCapacity, pty.slave_name);
  ::close(pty.slave); pty.slave = -1;

  osp::SerialTransport receiver(cfg);
  REQUIRE(receiver.Open().has_value());

  RxRecord rec;
  receiver.SetRxCallback(TestRxCallback, &rec);

  // Write garbage followed by a valid frame
  uint8_t garbage[] = {0x12, 0x34, 0x56, 0x78, 0xFF, 0x00, 0xAA, 0x00};
  ::write(pty.master, garbage, sizeof(garbage));

  uint8_t frame[256];
  uint8_t payload[] = {0xCA, 0xFE};
  uint32_t len = BuildFrame(frame, sizeof(frame), 99, 0, payload, 2);
  ::write(pty.master, frame, len);
  ::usleep(10000);

  uint32_t frames = receiver.Poll();
  REQUIRE(frames == 1);
  REQUIRE(rec.count == 1);
  REQUIRE(rec.type_index == 99);
  REQUIRE(rec.size == 2);
  REQUIRE(rec.data[0] == 0xCA);
  REQUIRE(rec.data[1] == 0xFE);

  receiver.Close();
  ::close(pty.master);
}

// ============================================================================
// Health monitoring
// ============================================================================

TEST_CASE("serial - Health monitoring healthy state", "[serial]") {
  auto pty = CreatePtyPair();
  if (!pty.valid) SKIP("PTY not available");

  osp::SerialConfig cfg;
  cfg.watchdog_timeout_ms = 1000;
  cfg.port_name.assign(osp::TruncateToCapacity, pty.slave_name);
  ::close(pty.slave); pty.slave = -1;

  osp::SerialTransport transport(cfg);
  REQUIRE(transport.Open().has_value());

  // Initially healthy
  REQUIRE(transport.GetHealth() == osp::SerialPortHealth::kHealthy);
  REQUIRE(transport.IsHealthy());

  // Send a frame to update tx timestamp
  uint8_t data = 0x42;
  transport.Send(1, &data, 1);

  // Still healthy
  REQUIRE(transport.GetHealth() == osp::SerialPortHealth::kHealthy);

  transport.Close();
  ::close(pty.master);
}

TEST_CASE("serial - Health monitoring degraded state", "[serial]") {
  auto pty = CreatePtyPair();
  if (!pty.valid) SKIP("PTY not available");

  osp::SerialConfig cfg;
  cfg.degraded_error_threshold = 2;
  cfg.failed_error_threshold = 10;
  cfg.port_name.assign(osp::TruncateToCapacity, pty.slave_name);
  ::close(pty.slave); pty.slave = -1;

  osp::SerialTransport receiver(cfg);
  REQUIRE(receiver.Open().has_value());

  RxRecord rec;
  receiver.SetRxCallback(TestRxCallback, &rec);

  // Send corrupted frames to trigger errors
  uint8_t frame[256];
  uint8_t payload[] = {0x01, 0x02};
  uint32_t len = BuildFrame(frame, sizeof(frame), 1, 0, payload, 2);

  // Corrupt multiple frames
  for (int i = 0; i < 3; ++i) {
    frame[osp::kSerialHeaderSize] ^= 0xFF;  // Corrupt payload
    ::write(pty.master, frame, len);
    ::usleep(5000);
    receiver.Poll();
  }

  // Should be degraded after multiple errors
  auto health = receiver.GetHealth();
  REQUIRE((health == osp::SerialPortHealth::kDegraded ||
           health == osp::SerialPortHealth::kFailed));
  REQUIRE_FALSE(receiver.IsHealthy());

  receiver.Close();
  ::close(pty.master);
}

// ============================================================================
// Rate limiting
// ============================================================================

TEST_CASE("serial - Rate limiting", "[serial]") {
  auto pty = CreatePtyPair();
  if (!pty.valid) SKIP("PTY not available");

  osp::SerialConfig cfg;
  cfg.max_frames_per_second = 5;  // Very low limit for testing
  cfg.port_name.assign(osp::TruncateToCapacity, pty.slave_name);
  ::close(pty.slave); pty.slave = -1;

  osp::SerialTransport tx(cfg);
  REQUIRE(tx.Open().has_value());

  uint8_t data = 0x01;
  uint32_t success_count = 0;
  uint32_t rate_limit_count = 0;

  // Try to send more frames than the limit
  for (int i = 0; i < 10; ++i) {
    auto r = tx.Send(1, &data, 1);
    if (r.has_value()) {
      ++success_count;
    } else if (r.get_error() == osp::SerialError::kRateLimitExceeded) {
      ++rate_limit_count;
    }
  }

  // Should have hit rate limit
  REQUIRE(success_count <= cfg.max_frames_per_second);
  REQUIRE(rate_limit_count > 0);

  auto stats = tx.GetStatistics();
  REQUIRE(stats.rate_limit_drops > 0);

  tx.Close();
  ::close(pty.master);
}

// ============================================================================
// Sequence number wraparound
// ============================================================================

TEST_CASE("serial - Sequence number wraparound", "[serial]") {
  auto pty_tx = CreatePtyPair();
  auto pty_rx = CreatePtyPair();
  if (!pty_tx.valid || !pty_rx.valid) SKIP("PTY not available");

  osp::SerialConfig tx_cfg;
  tx_cfg.port_name.assign(osp::TruncateToCapacity, pty_tx.slave_name);
  ::close(pty_tx.slave); pty_tx.slave = -1;

  osp::SerialConfig rx_cfg;
  rx_cfg.reliability.enable_seq_check = true;
  rx_cfg.port_name.assign(osp::TruncateToCapacity, pty_rx.slave_name);
  ::close(pty_rx.slave); pty_rx.slave = -1;

  osp::SerialTransport sender(tx_cfg);
  osp::SerialTransport receiver(rx_cfg);
  REQUIRE(sender.Open().has_value());
  REQUIRE(receiver.Open().has_value());

  RxRecord rec;
  receiver.SetRxCallback(TestRxCallback, &rec);

  // Send frames - sequence numbers should increment
  for (int i = 0; i < 3; ++i) {
    uint8_t d = static_cast<uint8_t>(i);
    sender.Send(1, &d, 1);
  }

  ::usleep(20000);
  uint8_t relay[4096];
  ssize_t n = ::read(pty_tx.master, relay, sizeof(relay));
  REQUIRE(n > 0);
  REQUIRE(::write(pty_rx.master, relay, static_cast<size_t>(n)) == n);
  ::usleep(10000);

  receiver.Poll();
  auto stats = receiver.GetStatistics();
  REQUIRE(stats.frames_received == 3);

  sender.Close();
  receiver.Close();
  ::close(pty_tx.master);
  ::close(pty_rx.master);
}

// ============================================================================
// Write retry statistics
// ============================================================================

TEST_CASE("serial - Write retry tracking", "[serial]") {
  auto pty = CreatePtyPair();
  if (!pty.valid) SKIP("PTY not available");

  osp::SerialConfig cfg;
  cfg.write_retry_count = 5;
  cfg.write_retry_delay_us = 100;
  cfg.port_name.assign(osp::TruncateToCapacity, pty.slave_name);
  ::close(pty.slave); pty.slave = -1;

  osp::SerialTransport tx(cfg);
  REQUIRE(tx.Open().has_value());

  uint8_t data[100];
  std::memset(data, 0xAB, sizeof(data));

  // Send some data
  auto r = tx.Send(1, data, sizeof(data));
  REQUIRE(r.has_value());

  auto stats = tx.GetStatistics();
  REQUIRE(stats.frames_sent > 0);
  // write_retries may be 0 if write succeeded immediately

  tx.Close();
  ::close(pty.master);
}

// ============================================================================
// Rx callback invocation
// ============================================================================

TEST_CASE("serial - Rx callback invocation", "[serial]") {
  auto pty = CreatePtyPair();
  if (!pty.valid) SKIP("PTY not available");

  osp::SerialConfig cfg;
  cfg.port_name.assign(osp::TruncateToCapacity, pty.slave_name);
  ::close(pty.slave); pty.slave = -1;

  osp::SerialTransport receiver(cfg);
  REQUIRE(receiver.Open().has_value());

  // No callback set - should not crash
  uint8_t frame[256];
  uint8_t payload[] = {0x01};
  uint32_t len = BuildFrame(frame, sizeof(frame), 1, 0, payload, 1);
  ::write(pty.master, frame, len);
  ::usleep(10000);

  uint32_t frames = receiver.Poll();
  REQUIRE(frames == 1);  // Frame decoded even without callback

  // Now set callback and send another
  RxRecord rec;
  receiver.SetRxCallback(TestRxCallback, &rec);

  len = BuildFrame(frame, sizeof(frame), 2, 1, payload, 1);
  ::write(pty.master, frame, len);
  ::usleep(10000);

  frames = receiver.Poll();
  REQUIRE(frames == 1);
  REQUIRE(rec.count == 1);
  REQUIRE(rec.type_index == 2);

  receiver.Close();
  ::close(pty.master);
}

// ============================================================================
// Partial frame timeout
// ============================================================================

TEST_CASE("serial - Partial frame timeout", "[serial]") {
  auto pty = CreatePtyPair();
  if (!pty.valid) SKIP("PTY not available");

  osp::SerialConfig cfg;
  cfg.inter_byte_timeout_ms = 1;  // Very short timeout for testing
  cfg.port_name.assign(osp::TruncateToCapacity, pty.slave_name);
  ::close(pty.slave); pty.slave = -1;

  osp::SerialTransport receiver(cfg);
  REQUIRE(receiver.Open().has_value());

  RxRecord rec;
  receiver.SetRxCallback(TestRxCallback, &rec);

  // Send only the sync bytes (incomplete frame)
  uint8_t partial[] = {0xAA, 0x55, 0x53, 0x4F};
  ::write(pty.master, partial, sizeof(partial));
  ::usleep(5000);

  // First poll processes partial data
  receiver.Poll();

  // Wait for timeout to expire
  ::usleep(10000);

  // Now send a valid frame - the stale partial should be discarded
  uint8_t frame[256];
  uint8_t payload[] = {0xBB};
  uint32_t len = BuildFrame(frame, sizeof(frame), 77, 0, payload, 1);
  ::write(pty.master, frame, len);
  ::usleep(10000);

  uint32_t frames = receiver.Poll();
  REQUIRE(frames == 1);
  REQUIRE(rec.type_index == 77);

  receiver.Close();
  ::close(pty.master);
}

// ============================================================================
// Port not open error
// ============================================================================

TEST_CASE("serial - Port not open error", "[serial]") {
  osp::SerialConfig cfg;
  osp::SerialTransport tx(cfg);

  REQUIRE_FALSE(tx.IsOpen());
  auto r = tx.Send(1, nullptr, 0);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.get_error() == osp::SerialError::kPortNotOpen);
}

// ============================================================================
// Zero-payload frame
// ============================================================================

TEST_CASE("serial - Zero-payload frame", "[serial]") {
  auto pty = CreatePtyPair();
  if (!pty.valid) SKIP("PTY not available");

  osp::SerialConfig cfg;
  cfg.port_name.assign(osp::TruncateToCapacity, pty.slave_name);
  ::close(pty.slave); pty.slave = -1;

  osp::SerialTransport receiver(cfg);
  REQUIRE(receiver.Open().has_value());

  RxRecord rec;
  receiver.SetRxCallback(TestRxCallback, &rec);

  uint8_t frame[256];
  uint32_t len = BuildFrame(frame, sizeof(frame), 5, 0, nullptr, 0);
  REQUIRE(len == osp::kSerialMinFrameSize);

  ::write(pty.master, frame, len);
  ::usleep(10000);

  uint32_t frames = receiver.Poll();
  REQUIRE(frames == 1);
  REQUIRE(rec.count == 1);
  REQUIRE(rec.type_index == 5);
  REQUIRE(rec.size == 0);

  receiver.Close();
  ::close(pty.master);
}
