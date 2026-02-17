// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// monitor.cpp -- DebugShell monitor for SPMC data distribution channel.
//
// Demonstrates:
//   1. ShmSpmcByteChannel::OpenReader for non-intrusive observation
//   2. DebugShell telnet interface for runtime inspection
//   3. Polling thread for channel state sampling
//   4. Statistics accumulation (frames, fps, avg size)
//   5. Frame header validation and display
//   6. Graceful shutdown via ShutdownManager
//
// Usage:
//   ./osp_dvd_monitor [channel_name] [shell_port] [--console]
//
// Shell commands:
//   dvd_status  - Show channel status (capacity, consumers, readable bytes)
//   dvd_stats   - Show accumulated statistics (frames, fps, avg size)
//   dvd_peek    - Read and display next frame header (consumes frame)
//   dvd_reset   - Reset statistics counters
//   dvd_config  - Show channel configuration
//
// newosp components used:
//   - osp::ShmSpmcByteChannel  -- SPMC shared memory channel
//   - osp::DebugShell          -- telnet shell interface
//   - osp::ShutdownManager     -- graceful SIGINT handling
//   - osp::log                 -- structured logging

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "osp/log.hpp"
#include "osp/platform.hpp"
#include "osp/shell.hpp"
#include "osp/shm_transport.hpp"
#include "osp/shutdown.hpp"
#include "common.hpp"

using Channel = osp::ShmSpmcByteChannel;

// ---------------------------------------------------------------------------
// Monitor state
// ---------------------------------------------------------------------------
struct MonitorState {
  const char* channel_name = kDefaultChannelName;
  Channel reader;
  bool channel_open = false;

  // Polling stats (updated by polling thread)
  std::atomic<uint32_t> readable_bytes{0};
  std::atomic<uint32_t> consumer_count{0};
  std::atomic<uint32_t> poll_count{0};

  // Accumulated statistics
  std::atomic<uint64_t> frames_seen{0};
  std::atomic<uint64_t> total_frame_size{0};
  std::atomic<uint64_t> last_frame_time_us{0};
  std::atomic<uint64_t> first_frame_time_us{0};
  std::atomic<uint32_t> last_seq{0};
  std::atomic<uint32_t> validation_errors{0};
};

static MonitorState g_state;  // NOLINT(cert-err58-cpp)
static osp::ShutdownManager* g_shutdown = nullptr;  // NOLINT(cert-err58-cpp)

// ---------------------------------------------------------------------------
// Polling thread -- samples channel state every 100ms
// ---------------------------------------------------------------------------
static void PollingThread() {
  OSP_LOG_INFO("MONITOR", "Polling thread started");

  while (!g_shutdown->IsShutdownRequested()) {
    if (g_state.channel_open) {
      uint32_t readable = g_state.reader.ReadableBytes();
      uint32_t consumers = g_state.reader.ConsumerCount();
      g_state.readable_bytes.store(readable, std::memory_order_relaxed);
      g_state.consumer_count.store(consumers, std::memory_order_relaxed);
      g_state.poll_count.fetch_add(1, std::memory_order_relaxed);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  OSP_LOG_INFO("MONITOR", "Polling thread stopped");
}

// ---------------------------------------------------------------------------
// Shell commands
// ---------------------------------------------------------------------------

static int dvd_status(int /*argc*/, char* /*argv*/[]) {
  uint32_t readable = g_state.readable_bytes.load(std::memory_order_relaxed);
  uint32_t consumers = g_state.consumer_count.load(std::memory_order_relaxed);
  uint32_t polls = g_state.poll_count.load(std::memory_order_relaxed);
  uint32_t capacity = g_state.channel_open ? g_state.reader.Capacity() : 0;

  osp::DebugShell::Printf(
      "  channel:       %s\r\n"
      "  open:          %s\r\n"
      "  capacity:      %u bytes\r\n"
      "  readable:      %u bytes\r\n"
      "  consumers:     %u / %u\r\n"
      "  polls:         %u\r\n",
      g_state.channel_name,
      g_state.channel_open ? "yes" : "no",
      capacity,
      readable,
      consumers,
      kSpmcMaxConsumers,
      polls);
  return 0;
}
OSP_SHELL_CMD(dvd_status, "Show SPMC channel status");

static int dvd_stats(int /*argc*/, char* /*argv*/[]) {
  uint64_t frames = g_state.frames_seen.load(std::memory_order_relaxed);
  uint64_t total_size = g_state.total_frame_size.load(std::memory_order_relaxed);
  uint64_t first_time = g_state.first_frame_time_us.load(std::memory_order_relaxed);
  uint64_t last_time = g_state.last_frame_time_us.load(std::memory_order_relaxed);
  uint32_t last_seq = g_state.last_seq.load(std::memory_order_relaxed);
  uint32_t errors = g_state.validation_errors.load(std::memory_order_relaxed);

  double avg_size = (frames > 0) ? static_cast<double>(total_size) / frames : 0.0;
  double fps = 0.0;
  if (frames > 1 && last_time > first_time) {
    double duration_sec = static_cast<double>(last_time - first_time) / 1e6;
    fps = static_cast<double>(frames - 1) / duration_sec;
  }

  osp::DebugShell::Printf(
      "  frames seen:      %llu\r\n"
      "  avg frame size:   %.1f bytes\r\n"
      "  fps:              %.2f\r\n"
      "  last seq:         %u\r\n"
      "  validation errs:  %u\r\n",
      static_cast<unsigned long long>(frames),
      avg_size,
      fps,
      last_seq,
      errors);
  return 0;
}
OSP_SHELL_CMD(dvd_stats, "Show accumulated statistics");

static int dvd_peek(int /*argc*/, char* /*argv*/[]) {
  if (!g_state.channel_open) {
    osp::DebugShell::Printf("  channel not open\r\n");
    return 1;
  }

  uint8_t buf[kFrameDataSize];
  uint32_t len = g_state.reader.Read(buf, sizeof(buf));

  if (len == 0) {
    osp::DebugShell::Printf("  no data available\r\n");
    return 1;
  }

  if (!ValidateLidarFrame(buf, len)) {
    osp::DebugShell::Printf("  validation failed (len=%u)\r\n", len);
    g_state.validation_errors.fetch_add(1, std::memory_order_relaxed);
    return 1;
  }

  auto* hdr = reinterpret_cast<const LidarFrame*>(buf);
  osp::DebugShell::Printf(
      "  magic:       0x%08X\r\n"
      "  seq_num:     %u\r\n"
      "  point_count: %u\r\n"
      "  timestamp:   %u ms\r\n"
      "  frame_size:  %u bytes\r\n",
      hdr->magic,
      hdr->seq_num,
      hdr->point_count,
      hdr->timestamp_ms,
      len);

  // Update stats
  uint64_t now_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());

  uint64_t frames = g_state.frames_seen.fetch_add(1, std::memory_order_relaxed);
  if (frames == 0) {
    g_state.first_frame_time_us.store(now_us, std::memory_order_relaxed);
  }
  g_state.last_frame_time_us.store(now_us, std::memory_order_relaxed);
  g_state.total_frame_size.fetch_add(len, std::memory_order_relaxed);
  g_state.last_seq.store(hdr->seq_num, std::memory_order_relaxed);

  return 0;
}
OSP_SHELL_CMD(dvd_peek, "Read and display next frame header");

static int dvd_reset(int /*argc*/, char* /*argv*/[]) {
  g_state.frames_seen.store(0, std::memory_order_relaxed);
  g_state.total_frame_size.store(0, std::memory_order_relaxed);
  g_state.first_frame_time_us.store(0, std::memory_order_relaxed);
  g_state.last_frame_time_us.store(0, std::memory_order_relaxed);
  g_state.last_seq.store(0, std::memory_order_relaxed);
  g_state.validation_errors.store(0, std::memory_order_relaxed);
  g_state.poll_count.store(0, std::memory_order_relaxed);

  osp::DebugShell::Printf("  statistics reset\r\n");
  return 0;
}
OSP_SHELL_CMD(dvd_reset, "Reset statistics counters");

static int dvd_config(int /*argc*/, char* /*argv*/[]) {
  osp::DebugShell::Printf(
      "  channel name:     %s\r\n"
      "  capacity:         %u bytes\r\n"
      "  max consumers:    %u\r\n"
      "  frame size:       %u bytes\r\n"
      "  points per frame: %u\r\n"
      "  produce fps:      %u Hz\r\n",
      g_state.channel_name,
      kSpmcCapacity,
      kSpmcMaxConsumers,
      kFrameDataSize,
      kPointsPerFrame,
      kProduceFps);
  return 0;
}
OSP_SHELL_CMD(dvd_config, "Show channel configuration");

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
  // Parse command line
  const char* channel_name = kDefaultChannelName;
  uint16_t shell_port = kDefaultShellPort;
  bool console_mode = false;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--console") == 0) {
      console_mode = true;
    } else if (std::strcmp(argv[i], "--help") == 0) {
      std::printf(
          "Usage: %s [channel_name] [shell_port] [--console]\n"
          "\n"
          "Options:\n"
          "  channel_name  SPMC channel name (default: %s)\n"
          "  shell_port    DebugShell port (default: %u)\n"
          "  --console     Use stdin/stdout instead of telnet\n"
          "\n"
          "Shell commands:\n"
          "  dvd_status  - Show channel status\n"
          "  dvd_stats   - Show accumulated statistics\n"
          "  dvd_peek    - Read and display next frame header\n"
          "  dvd_reset   - Reset statistics counters\n"
          "  dvd_config  - Show channel configuration\n",
          argv[0], kDefaultChannelName, kDefaultShellPort);
      return 0;
    } else if (i == 1) {
      channel_name = argv[i];
    } else if (i == 2) {
      shell_port = static_cast<uint16_t>(std::atoi(argv[i]));
    }
  }

  g_state.channel_name = channel_name;

  OSP_LOG_INFO("MONITOR", "Starting data-visitor-dispatcher monitor");
  OSP_LOG_INFO("MONITOR", "  channel: %s", channel_name);
  OSP_LOG_INFO("MONITOR", "  shell port: %u", shell_port);
  OSP_LOG_INFO("MONITOR", "  console mode: %s", console_mode ? "yes" : "no");

  // Setup shutdown handler
  osp::ShutdownManager shutdown;
  g_shutdown = &shutdown;
  auto install_result = shutdown.InstallSignalHandlers();
  if (!install_result) {
    OSP_LOG_ERROR("MONITOR", "Failed to install signal handlers");
    return 1;
  }

  // Open SPMC channel reader
  auto result = Channel::OpenReader(channel_name);
  if (!result) {
    OSP_LOG_ERROR("MONITOR", "Failed to open SPMC reader: %d", static_cast<int>(result.get_error()));
    return 1;
  }
  g_state.reader = static_cast<Channel&&>(result.value());
  g_state.channel_open = true;
  OSP_LOG_INFO("MONITOR", "SPMC reader opened successfully");

  // Start polling thread
  std::thread poll_thread(PollingThread);

  // Start DebugShell
  osp::ConsoleShell console_shell({});
  osp::DebugShell::Config tcp_cfg;
  tcp_cfg.port = shell_port;
  tcp_cfg.max_connections = 4;
  osp::DebugShell tcp_shell(tcp_cfg);

  if (console_mode) {
    OSP_LOG_INFO("MONITOR", "Starting console shell");
    auto start_result = console_shell.Start();
    if (!start_result) {
      OSP_LOG_ERROR("MONITOR", "Failed to start console shell");
      poll_thread.join();
      return 1;
    }
  } else {
    OSP_LOG_INFO("MONITOR", "Starting telnet shell on port %u", shell_port);
    auto start_result = tcp_shell.Start();
    if (!start_result) {
      OSP_LOG_ERROR("MONITOR", "Failed to start DebugShell");
      poll_thread.join();
      return 1;
    }
  }

  // Main loop
  OSP_LOG_INFO("MONITOR", "Monitor running (Ctrl+C to stop)");
  while (!shutdown.IsShutdownRequested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Cleanup
  OSP_LOG_INFO("MONITOR", "Shutting down...");
  if (console_mode) {
    console_shell.Stop();
  } else {
    tcp_shell.Stop();
  }
  poll_thread.join();

  OSP_LOG_INFO("MONITOR", "Monitor stopped");
  return 0;
}
