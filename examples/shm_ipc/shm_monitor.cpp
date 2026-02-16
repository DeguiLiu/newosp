// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// shm_monitor -- DebugShell monitor for SHM IPC channels.
//
// Demonstrates: DebugShell, SpscRingbuffer (stats consumption),
//               ShmChannel (queue depth polling), Shutdown, Log.
//
// The monitor reads ShmStats snapshots from SpscRingbuffer (pushed by
// producer/consumer) and exposes them via telnet shell commands.
// It also directly polls the ShmChannel queue depth for real-time view.
//
// In a real deployment, producer/consumer would share the SpscRingbuffer
// via shared memory or a named pipe. This demo uses in-process polling
// of the ShmChannel depth as the primary data source, with the
// SpscRingbuffer pattern shown for the shell "shm_latest" command.
//
// Usage: ./osp_shm_monitor [channel_name] [shell_port] [--console]

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
#include "osp/spsc_ringbuffer.hpp"

#include "shm_common.hpp"

using Channel = osp::ShmChannel<kSlotSize, kSlotCount>;

// ---------------------------------------------------------------------------
// Monitor state
// ---------------------------------------------------------------------------
struct MonitorState {
  const char* channel_name = "frame_ch";
  Channel channel;
  bool channel_open = false;

  // Polling stats (updated by monitor's own polling loop)
  std::atomic<uint32_t> poll_depth{0};
  std::atomic<uint32_t> poll_count{0};
  std::atomic<uint64_t> total_depth_sum{0};
  std::atomic<uint32_t> max_depth{0};
  std::atomic<uint32_t> min_depth{kSlotCount};

  // Latest stats snapshot from SpscRingbuffer (simulated in-process)
  // In production, this would be populated from shared memory
  osp::SpscRingbuffer<ShmStats, kStatsRingSize> stats_ring;
  ShmStats latest_producer_stats{};
  ShmStats latest_consumer_stats{};
  std::atomic<bool> has_producer_stats{false};
  std::atomic<bool> has_consumer_stats{false};
};

static MonitorState g_state;  // NOLINT(cert-err58-cpp)

// ---------------------------------------------------------------------------
// Drain stats ring and update latest snapshots
// ---------------------------------------------------------------------------
static void DrainStatsRing() {
  ShmStats s{};
  while (g_state.stats_ring.Pop(s)) {
    if (s.role == 0) {
      g_state.latest_producer_stats = s;
      g_state.has_producer_stats.store(true, std::memory_order_relaxed);
    } else {
      g_state.latest_consumer_stats = s;
      g_state.has_consumer_stats.store(true, std::memory_order_relaxed);
    }
  }
}

// ---------------------------------------------------------------------------
// Shell commands
// ---------------------------------------------------------------------------

static int shm_status(int /*argc*/, char* /*argv*/[]) {
  uint32_t depth = g_state.poll_depth.load(std::memory_order_relaxed);
  uint32_t polls = g_state.poll_count.load(std::memory_order_relaxed);
  double avg = (polls > 0)
      ? static_cast<double>(g_state.total_depth_sum.load(
            std::memory_order_relaxed)) / polls
      : 0.0;

  osp::DebugShell::Printf(
      "  channel:    %s\r\n"
      "  open:       %s\r\n"
      "  depth:      %u / %u slots\r\n"
      "  avg depth:  %.1f\r\n"
      "  min/max:    %u / %u\r\n"
      "  polls:      %u\r\n",
      g_state.channel_name,
      g_state.channel_open ? "yes" : "no",
      depth, kSlotCount,
      avg,
      g_state.min_depth.load(std::memory_order_relaxed),
      g_state.max_depth.load(std::memory_order_relaxed),
      polls);
  return 0;
}
OSP_SHELL_CMD(shm_status, "Show ShmChannel queue depth and state");

static int shm_stats(int /*argc*/, char* /*argv*/[]) {
  uint32_t depth = g_state.poll_depth.load(std::memory_order_relaxed);
  uint32_t polls = g_state.poll_count.load(std::memory_order_relaxed);
  double avg = (polls > 0)
      ? static_cast<double>(g_state.total_depth_sum.load(
            std::memory_order_relaxed)) / polls
      : 0.0;
  double fill_pct = (kSlotCount > 0)
      ? (static_cast<double>(depth) / kSlotCount * 100.0)
      : 0.0;

  osp::DebugShell::Printf(
      "  queue fill:   %.1f%% (%u/%u)\r\n"
      "  avg depth:    %.2f\r\n"
      "  min/max:      %u / %u\r\n"
      "  poll cycles:  %u\r\n"
      "  slot size:    %u bytes\r\n"
      "  slot count:   %u\r\n"
      "  capacity:     %u KB\r\n",
      fill_pct, depth, kSlotCount,
      avg,
      g_state.min_depth.load(std::memory_order_relaxed),
      g_state.max_depth.load(std::memory_order_relaxed),
      polls,
      kSlotSize,
      kSlotCount,
      (kSlotSize * kSlotCount) / 1024);
  return 0;
}
OSP_SHELL_CMD(shm_stats, "Show throughput and queue statistics");

static int shm_reset(int /*argc*/, char* /*argv*/[]) {
  g_state.poll_count.store(0, std::memory_order_relaxed);
  g_state.total_depth_sum.store(0, std::memory_order_relaxed);
  g_state.max_depth.store(0, std::memory_order_relaxed);
  g_state.min_depth.store(kSlotCount, std::memory_order_relaxed);
  osp::DebugShell::Printf("  stats reset.\r\n");
  return 0;
}
OSP_SHELL_CMD(shm_reset, "Reset statistics counters");

static int shm_latest(int /*argc*/, char* /*argv*/[]) {
  DrainStatsRing();

  if (g_state.has_producer_stats.load(std::memory_order_relaxed)) {
    const auto& p = g_state.latest_producer_stats;
    osp::DebugShell::Printf(
        "  [producer] frames=%u dropped=%u fps=%.1f mbps=%.1f depth=%u\r\n",
        p.frames_ok, p.dropped,
        static_cast<double>(p.fps), static_cast<double>(p.mbps),
        p.queue_depth);
  } else {
    osp::DebugShell::Printf("  [producer] no data yet\r\n");
  }

  if (g_state.has_consumer_stats.load(std::memory_order_relaxed)) {
    const auto& c = g_state.latest_consumer_stats;
    osp::DebugShell::Printf(
        "  [consumer] frames=%u bad=%u gaps=%u stalls=%u "
        "fps=%.1f mbps=%.1f\r\n",
        c.frames_ok, c.frames_bad, c.gaps, c.stall_count,
        static_cast<double>(c.fps), static_cast<double>(c.mbps));
  } else {
    osp::DebugShell::Printf("  [consumer] no data yet\r\n");
  }
  return 0;
}
OSP_SHELL_CMD(shm_latest, "Show latest producer/consumer stats from SpscRingbuffer");

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
        hdr->seq_num, hdr->width, hdr->height, size);
  } else {
    osp::DebugShell::Printf("  frame too small: %u bytes\r\n", size);
  }
  return 0;
}
OSP_SHELL_CMD(shm_peek, "Read and display next frame header (consumes frame)");

static int shm_config(int /*argc*/, char* /*argv*/[]) {
  osp::DebugShell::Printf(
      "  channel name:  %s\r\n"
      "  slot size:     %u bytes\r\n"
      "  slot count:    %u\r\n"
      "  ring capacity: %u KB\r\n"
      "  frame magic:   0x%08X\r\n"
      "  frame size:    %u bytes (%ux%u)\r\n"
      "  stats ring:    SpscRingbuffer<%u>\r\n",
      g_state.channel_name,
      kSlotSize, kSlotCount,
      (kSlotSize * kSlotCount) / 1024,
      kMagic,
      kFrameSize, kWidth, kHeight,
      kStatsRingSize);
  return 0;
}
OSP_SHELL_CMD(shm_config, "Show SHM channel and stats ring configuration");

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  const char* channel_name = (argc > 1) ? argv[1] : "frame_ch";
  uint16_t shell_port = (argc > 2)
      ? static_cast<uint16_t>(std::atoi(argv[2]))
      : 9527;

  // Detect --console flag
  bool use_console = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--console") == 0) {
      use_console = true;
    }
  }

  g_state.channel_name = channel_name;

  osp::ShutdownManager shutdown;
  shutdown.InstallSignalHandlers();

  // Create both shell types, start only one
  osp::ConsoleShell console_shell({});
  osp::DebugShell::Config tcp_cfg;
  tcp_cfg.port = shell_port;
  tcp_cfg.max_connections = 4;
  osp::DebugShell tcp_shell(tcp_cfg);

  if (use_console) {
    auto r = console_shell.Start();
    if (!r) {
      OSP_LOG_ERROR("monitor", "console shell start failed");
      return 1;
    }
    OSP_LOG_INFO("monitor", "console shell started (stdin/stdout)");
  } else {
    auto r = tcp_shell.Start();
    if (!r) {
      OSP_LOG_ERROR("monitor", "shell start failed on port %u", shell_port);
      return 1;
    }
    OSP_LOG_INFO("monitor", "shell started on port %u (telnet localhost %u)",
                 shell_port, shell_port);
  }
  OSP_LOG_INFO("monitor", "commands: shm_status, shm_stats, shm_latest, "
               "shm_peek, shm_config, shm_reset");

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

  // Polling loop: sample queue depth + drain stats ring
  while (!shutdown.IsShutdownRequested()) {
    if (g_state.channel_open) {
      uint32_t depth = g_state.channel.Depth();
      g_state.poll_depth.store(depth, std::memory_order_relaxed);

      uint32_t count =
          g_state.poll_count.fetch_add(1, std::memory_order_relaxed) + 1;
      g_state.total_depth_sum.fetch_add(depth, std::memory_order_relaxed);

      // Track min/max
      uint32_t cur_max = g_state.max_depth.load(std::memory_order_relaxed);
      while (depth > cur_max &&
             !g_state.max_depth.compare_exchange_weak(
                 cur_max, depth, std::memory_order_relaxed)) {}

      uint32_t cur_min = g_state.min_depth.load(std::memory_order_relaxed);
      while (depth < cur_min &&
             !g_state.min_depth.compare_exchange_weak(
                 cur_min, depth, std::memory_order_relaxed)) {}

      // Drain any stats pushed via SpscRingbuffer
      DrainStatsRing();

      // Periodic log
      if (count % 50 == 0) {
        double avg = static_cast<double>(
            g_state.total_depth_sum.load(std::memory_order_relaxed)) / count;
        OSP_LOG_INFO("monitor",
                     "depth=%u/%u avg=%.1f min=%u max=%u polls=%u",
                     depth, kSlotCount, avg,
                     g_state.min_depth.load(std::memory_order_relaxed),
                     g_state.max_depth.load(std::memory_order_relaxed),
                     count);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  if (use_console) {
    console_shell.Stop();
  } else {
    tcp_shell.Stop();
  }
  OSP_LOG_INFO("monitor", "shutdown complete");
  return 0;
}
