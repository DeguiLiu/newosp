// serial_benchmark.cpp -- Serial transport throughput benchmark
//
// Measures throughput for different payload sizes over PTY pairs.
// Tests: 8B, 32B, 64B, 128B, 256B, 512B, 900B payloads
// Modes: with/without ACK, with/without CRC verification

#include "osp/serial_transport.hpp"
#include "osp/platform.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#if defined(OSP_PLATFORM_LINUX)
#include <fcntl.h>
#include <pty.h>
#include <unistd.h>
#endif

using Clock = std::chrono::high_resolution_clock;

// -- PTY helpers -------------------------------------------------------------

struct PtyPair {
  int master_a = -1;
  int master_b = -1;
  char slave_a_name[64]{};
  char slave_b_name[64]{};
  bool valid = false;
};

#if defined(OSP_PLATFORM_LINUX)
static PtyPair CreatePtyPair() {
  PtyPair p{};
  int slave_a, slave_b;
  if (::openpty(&p.master_a, &slave_a, p.slave_a_name, nullptr, nullptr) != 0)
    return p;
  if (::openpty(&p.master_b, &slave_b, p.slave_b_name, nullptr, nullptr) != 0) {
    ::close(p.master_a);
    ::close(slave_a);
    return p;
  }
  ::fcntl(p.master_a, F_SETFL, ::fcntl(p.master_a, F_GETFL, 0) | O_NONBLOCK);
  ::fcntl(p.master_b, F_SETFL, ::fcntl(p.master_b, F_GETFL, 0) | O_NONBLOCK);
  ::close(slave_a);
  ::close(slave_b);
  p.valid = true;
  return p;
}

static void ClosePty(PtyPair& p) {
  if (p.master_a >= 0) ::close(p.master_a);
  if (p.master_b >= 0) ::close(p.master_b);
  p.master_a = p.master_b = -1;
  p.valid = false;
}

// Cross-connect: forward data between two PTY masters (bidirectional)
static void CrossConnectOnce(int master_a, int master_b) {
  uint8_t buf[4096];
  ssize_t n = ::read(master_a, buf, sizeof(buf));
  if (n > 0) {
    ::write(master_b, buf, static_cast<size_t>(n));
  }
  n = ::read(master_b, buf, sizeof(buf));
  if (n > 0) {
    ::write(master_a, buf, static_cast<size_t>(n));
  }
}
#endif

// -- Benchmark ---------------------------------------------------------------

struct BenchResult {
  uint32_t payload_size;
  uint32_t sent;
  uint32_t received;
  double elapsed_ms;
  double throughput_fps;   // frames per second
  double throughput_kbps;  // kilobytes per second (payload only)
  double avg_frame_us;     // average time per frame
};

#if defined(OSP_PLATFORM_LINUX)
static BenchResult RunSerialBench(uint32_t payload_size, uint32_t count,
                                  bool enable_ack) {
  BenchResult result{};
  result.payload_size = payload_size;

  auto pty = CreatePtyPair();
  if (!pty.valid) {
    printf("  [ERROR] Failed to create PTY pair\n");
    return result;
  }

  // Sender config
  osp::SerialConfig tx_cfg;
  tx_cfg.port_name = pty.slave_a_name;
  tx_cfg.baud_rate = 921600U;
  tx_cfg.max_frames_per_second = 0U;  // unlimited
  tx_cfg.reliability.enable_ack = enable_ack;
  tx_cfg.reliability.ack_timeout_ms = 50U;
  tx_cfg.reliability.max_retries = 3U;

  // Receiver config
  osp::SerialConfig rx_cfg;
  rx_cfg.port_name = pty.slave_b_name;
  rx_cfg.baud_rate = 921600U;
  rx_cfg.max_frames_per_second = 0U;
  rx_cfg.reliability.enable_ack = enable_ack;

  osp::SerialTransport sender(tx_cfg);
  osp::SerialTransport receiver(rx_cfg);

  auto r1 = sender.Open();
  auto r2 = receiver.Open();
  if (!r1 || !r2) {
    printf("  [ERROR] Failed to open serial ports\n");
    ClosePty(pty);
    return result;
  }

  // Receive counter
  std::atomic<uint32_t> rx_count{0};
  receiver.SetRxCallback(
      [](const void*, uint32_t, uint16_t, uint16_t, void* ctx) {
        static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(
            1, std::memory_order_relaxed);
      },
      &rx_count);

  // Prepare payload
  std::vector<uint8_t> payload(payload_size, 0xAB);

  // Cross-connect thread
  std::atomic<bool> stop{false};
  std::thread bridge([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      CrossConnectOnce(pty.master_a, pty.master_b);
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  });

  // Warm up
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Send + poll loop
  auto t0 = Clock::now();
  uint32_t tx_ok = 0;
  for (uint32_t i = 0; i < count; ++i) {
    auto sr = sender.Send(1, payload.data(), payload_size);
    if (sr) ++tx_ok;

    // Poll receiver to process incoming data
    for (int p = 0; p < 10; ++p) {
      receiver.Poll();
      if (enable_ack) sender.Poll();  // process ACKs
    }
  }

  // Drain remaining
  for (int drain = 0; drain < 500; ++drain) {
    receiver.Poll();
    if (enable_ack) sender.Poll();
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    if (rx_count.load(std::memory_order_relaxed) >= tx_ok) break;
  }
  auto t1 = Clock::now();

  stop.store(true, std::memory_order_relaxed);
  bridge.join();

  double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  uint32_t rx = rx_count.load();

  result.sent = tx_ok;
  result.received = rx;
  result.elapsed_ms = elapsed_ms;
  result.throughput_fps = (elapsed_ms > 0) ? (rx / (elapsed_ms / 1000.0)) : 0;
  result.throughput_kbps =
      (elapsed_ms > 0)
          ? (static_cast<double>(rx) * payload_size / 1024.0 / (elapsed_ms / 1000.0))
          : 0;
  result.avg_frame_us = (rx > 0) ? (elapsed_ms * 1000.0 / rx) : 0;

  sender.Close();
  receiver.Close();
  ClosePty(pty);

  return result;
}
#endif

// -- Main --------------------------------------------------------------------

int main() {
  printf("newosp Serial Transport Benchmark\n");
  printf("==================================\n");
  printf("Transport: PTY pair, baud=921600, CRC-CCITT\n\n");

#if defined(OSP_PLATFORM_LINUX)
  static constexpr uint32_t kPayloadSizes[] = {8, 32, 64, 128, 256, 512};
  static constexpr uint32_t kCount = 1000;

  // --- Without ACK ---
  printf("=== Throughput (no ACK, %u frames per size) ===\n", kCount);
  printf("  %-10s  %6s  %6s  %10s  %10s  %10s\n",
         "Payload", "Sent", "Recv", "Time(ms)", "FPS", "KB/s");
  printf("  ---------------------------------------------------------------\n");

  for (uint32_t sz : kPayloadSizes) {
    auto r = RunSerialBench(sz, kCount, false);
    printf("  %-10u  %6u  %6u  %10.1f  %10.0f  %10.1f\n",
           r.payload_size, r.sent, r.received, r.elapsed_ms,
           r.throughput_fps, r.throughput_kbps);
  }

  printf("\n");

  // --- With ACK ---
  printf("=== Throughput (ACK enabled, %u frames per size) ===\n", kCount);
  printf("  %-10s  %6s  %6s  %10s  %10s  %10s  %10s\n",
         "Payload", "Sent", "Recv", "Time(ms)", "FPS", "KB/s", "us/frame");
  printf("  --------------------------------------------------------------------------\n");

  for (uint32_t sz : kPayloadSizes) {
    auto r = RunSerialBench(sz, kCount, true);
    printf("  %-10u  %6u  %6u  %10.1f  %10.0f  %10.1f  %10.1f\n",
           r.payload_size, r.sent, r.received, r.elapsed_ms,
           r.throughput_fps, r.throughput_kbps, r.avg_frame_us);
  }

  printf("\nFrame overhead: %u bytes (header %u + CRC %u + tail 1)\n",
         osp::kSerialHeaderSize + osp::kSerialCrcSize + 1U,
         osp::kSerialHeaderSize, osp::kSerialCrcSize);

#else
  printf("Serial benchmark requires Linux (PTY support).\n");
#endif

  printf("\nDone.\n");
  return 0;
}
