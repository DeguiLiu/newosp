// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// shm_monitor -- DebugShell monitor for SHM IPC channels.
//
// Provides telnet-accessible shell commands to inspect ShmChannel state,
// throughput statistics, and queue depth in real time.
//
// Usage: ./osp_shm_monitor [channel_name] [shell_port]

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

// ---------------------------------------------------------------------------
// Frame format (must match producer/consumer)
// ---------------------------------------------------------------------------
struct FrameHeader {
  uint32_t magic;
  uint32_t seq_num;
  uint32_t width;
  uint32_t height;
};

static constexpr uint32_t kMagic     = 0x4652414Du;
static constexpr uint32_t kSlotSize  = 81920;
static constexpr uint32_t kSlotCount = 16;

using Channel = osp::ShmChannel<kSlotSize, kSlotCount>;

// ---------------------------------------------------------------------------
// Global monitor state (accessed by shell commands + polling thread)
// ---------------------------------------------------------------------------
struct MonitorState {
  const char* channel_name = "frame_ch";
  Channel channel;
  bool channel_open = false;

  std::atomic<uint32_t> poll_depth{0};
  std::atomic<uint32_t> poll_count{0};
  std::atomic<uint64_t> total_depth_sum{0};
  std::atomic<double> avg_depth{0.0};
};

static MonitorState g_state;  // NOLINT(cert-err58-cpp)

// ---------------------------------------------------------------------------
// Shell commands
// ---------------------------------------------------------------------------

static int shm_status(int /*argc*/, char* /*argv*/[]) {
  osp::DebugShell::Printf(
      "  channel:    %s\r\n"
      "  open:       %s\r\n"
      "  depth:      %u / %u slots\r\n"
      "  avg depth:  %.1f\r\n"
      "  polls:      %u\r\n",
      g_state.channel_name,
      g_state.channel_open ? "yes" : "no",
      g_state.poll_depth.load(std::memory_order_relaxed), kSlotCount,
      g_state.avg_depth.load(std::memory_order_relaxed),
      g_state.poll_count.load(std::memory_order_relaxed));
  return 0;
}
OSP_SHELL_CMD(shm_status, "Show ShmChannel queue depth and state");

static int shm_stats(int /*argc*/, char* /*argv*/[]) {
  uint32_t depth = g_state.poll_depth.load(std::memory_order_relaxed);
  double avg = g_state.avg_depth.load(std::memory_order_relaxed);
  double fill_pct = (kSlotCount > 0)
      ? (static_cast<double>(depth) / kSlotCount * 100.0)
      : 0.0;

  osp::DebugShell::Printf(
      "  queue fill:   %.1f%% (%u/%u)\r\n"
      "  avg depth:    %.2f\r\n"
      "  poll cycles:  %u\r\n"
      "  slot size:    %u bytes\r\n"
      "  slot count:   %u\r\n"
      "  capacity:     %u KB\r\n",
      fill_pct, depth, kSlotCount,
      avg,
      g_state.poll_count.load(std::memory_order_relaxed),
      kSlotSize,
      kSlotCount,
      (kSlotSize * kSlotCount) / 1024);
  return 0;
}
OSP_SHELL_CMD(shm_stats, "Show throughput and queue statistics");

static int shm_reset(int /*argc*/, char* /*argv*/[]) {
  g_state.poll_count.store(0, std::memory_order_relaxed);
  g_state.total_depth_sum.store(0, std::memory_order_relaxed);
  g_state.avg_depth.store(0.0, std::memory_order_relaxed);
  osp::DebugShell::Printf("  stats reset.\r\n");
  return 0;
}
OSP_SHELL_CMD(shm_reset, "Reset statistics counters");

static int shm_peek(int /*argc*/, char* /*argv*/[]) {
  if (!g_state.channel_open) {
    osp::DebugShell::Printf("  channel not open.\r\n");
    return 1;
  }

  uint32_t depth = g_state.channel.Depth();
  if (depth == 0) {
    osp::DebugShell::Printf("  queue empty.\r\n");
    return 0;
  }

  uint8_t buf[kSlotSize];
  uint32_t size = kSlotSize;
  auto rd = g_state.channel.Read(buf, size);
  if (!rd) {
    osp::DebugShell::Printf("  read failed.\r\n");
    return 1;
  }

  if (size >= sizeof(FrameHeader)) {
    const auto* hdr = reinterpret_cast<const FrameHeader*>(buf);
    osp::DebugShell::Printf(
        "  magic:    0x%08X %s\r\n"
        "  seq_num:  %u\r\n"
        "  width:    %u\r\n"
        "  height:   %u\r\n"
        "  size:     %u bytes\r\n"
        "  (note: frame consumed from queue)\r\n",
        hdr->magic,
        (hdr->magic == kMagic) ? "(OK)" : "(BAD)",
        hdr->seq_num,
        hdr->width,
        hdr->height,
        size);
  } else {
    osp::DebugShell::Printf("  frame too small: %u bytes\r\n", size);
  }
  return 0;
}
OSP_SHELL_CMD(shm_peek, "Read and display next frame header (consumes frame)");

static int shm_config(int /*argc*/, char* /*argv*/[]) {
  osp::DebugShell::Printf(
      "  channel name: %s\r\n"
      "  slot size:    %u bytes\r\n"
      "  slot count:   %u\r\n"
      "  ring capacity: %u KB\r\n"
      "  frame magic:  0x%08X\r\n",
      g_state.channel_name,
      kSlotSize,
      kSlotCount,
      (kSlotSize * kSlotCount) / 1024,
      kMagic);
  return 0;
}
OSP_SHELL_CMD(shm_config, "Show SHM channel configuration");

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  const char* channel_name = (argc > 1) ? argv[1] : "frame_ch";
  uint16_t shell_port = (argc > 2)
      ? static_cast<uint16_t>(std::atoi(argv[2]))
      : 9527;

  g_state.channel_name = channel_name;

  osp::ShutdownManager shutdown;
  shutdown.InstallSignalHandlers();

  // Start DebugShell
  osp::DebugShell::Config shell_cfg;
  shell_cfg.port = shell_port;
  shell_cfg.max_connections = 4;
  osp::DebugShell shell(shell_cfg);

  auto shell_r = shell.Start();
  if (!shell_r) {
    OSP_LOG_ERROR("monitor", "shell start failed on port %u", shell_port);
    return 1;
  }
  OSP_LOG_INFO("monitor", "shell started on port %u (telnet localhost %u)",
               shell_port, shell_port);

  // Try to open channel as reader (retry loop)
  OSP_LOG_INFO("monitor", "waiting for channel %s ...", channel_name);
  while (!shutdown.IsShutdownRequested()) {
    auto result = Channel::OpenReader(channel_name);
    if (result) {
      g_state.channel = static_cast<Channel&&>(result.value());
      g_state.channel_open = true;
      OSP_LOG_INFO("monitor", "channel %s opened", channel_name);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  // Polling loop: sample queue depth periodically
  while (!shutdown.IsShutdownRequested()) {
    if (g_state.channel_open) {
      uint32_t depth = g_state.channel.Depth();
      g_state.poll_depth.store(depth, std::memory_order_relaxed);

      uint32_t count =
          g_state.poll_count.fetch_add(1, std::memory_order_relaxed) + 1;
      uint64_t sum =
          g_state.total_depth_sum.fetch_add(depth, std::memory_order_relaxed)
          + depth;
      double avg = (count > 0) ? static_cast<double>(sum) / count : 0.0;
      g_state.avg_depth.store(avg, std::memory_order_relaxed);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  shell.Stop();
  OSP_LOG_INFO("monitor", "shutdown complete");
  return 0;
}
