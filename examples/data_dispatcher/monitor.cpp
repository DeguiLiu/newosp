// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// monitor -- DataDispatcher<ShmStore> pool inspector + DebugShell.
//
// Demonstrates: ShmStore Attach (read-only inspection), DebugShell,
//               pool state polling, ScanTimeout from monitor process.
//
// Usage: ./osp_dd_monitor [--channel name] [--port P] [--console]

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "osp/data_dispatcher.hpp"
#include "osp/log.hpp"
#include "osp/platform.hpp"
#include "osp/shell.hpp"
#include "osp/shm_transport.hpp"
#include "osp/shutdown.hpp"
#include "common.hpp"

using Store = osp::ShmStore<kFrameDataSize, kPoolMaxBlocks>;
using Disp = osp::DataDispatcher<Store>;

// ---------------------------------------------------------------------------
// Monitor state
// ---------------------------------------------------------------------------

struct MonitorState {
  const char* pool_name = kPoolShmName;
  const char* notify_name = kNotifyChannelName;

  Disp disp;
  void* pool_shm = nullptr;
  uint32_t pool_size = 0;
  bool pool_attached = false;

  osp::ShmSpmcByteChannel notify_channel;
  bool notify_open = false;

  std::atomic<uint32_t> poll_count{0};
  std::atomic<uint64_t> frames_seen{0};
  std::atomic<uint32_t> last_seq{0};
};

static MonitorState g_state;
static osp::ShutdownManager* g_shutdown = nullptr;

// ---------------------------------------------------------------------------
// Polling thread
// ---------------------------------------------------------------------------

static void PollingThread() {
  OSP_LOG_INFO("Monitor", "polling thread started");
  while (!g_shutdown->IsShutdownRequested()) {
    g_state.poll_count.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

// ---------------------------------------------------------------------------
// Shell commands
// ---------------------------------------------------------------------------

static int dd_status(int, char*[]) {
  if (!g_state.pool_attached) {
    osp::DebugShell::Printf("  pool not attached\r\n");
    return 1;
  }
  osp::DebugShell::Printf(
      "  pool:      %s\r\n"
      "  capacity:  %u blocks x %u bytes\r\n"
      "  free:      %u\r\n"
      "  allocated: %u\r\n"
      "  polls:     %u\r\n",
      g_state.pool_name,
      kPoolMaxBlocks, kFrameDataSize,
      g_state.disp.FreeBlocks(),
      g_state.disp.AllocBlocks(),
      g_state.poll_count.load(std::memory_order_relaxed));
  return 0;
}
OSP_SHELL_CMD(dd_status, "Show DataDispatcher pool status");

static int dd_scan(int, char*[]) {
  if (!g_state.pool_attached) {
    osp::DebugShell::Printf("  pool not attached\r\n");
    return 1;
  }
  uint32_t t = g_state.disp.ScanTimeout();
  osp::DebugShell::Printf("  ScanTimeout: %u blocks reclaimed\r\n", t);
  return 0;
}
OSP_SHELL_CMD(dd_scan, "Run ScanTimeout on pool");

static int dd_blocks(int, char*[]) {
  if (!g_state.pool_attached) {
    osp::DebugShell::Printf("  pool not attached\r\n");
    return 1;
  }
  for (uint32_t i = 0; i < kPoolMaxBlocks; ++i) {
    auto st = g_state.disp.GetBlockState(i);
    if (st != osp::BlockState::kFree) {
      osp::DebugShell::Printf("  block[%u] state=%u\r\n",
                               i, static_cast<unsigned>(st));
    }
  }
  return 0;
}
OSP_SHELL_CMD(dd_blocks, "List non-free blocks");

static int dd_notify(int, char*[]) {
  if (!g_state.notify_open) {
    osp::DebugShell::Printf("  notify channel not open\r\n");
    return 1;
  }
  osp::DebugShell::Printf(
      "  notify channel: %s\r\n"
      "  consumers:      %u\r\n"
      "  readable:       %u bytes\r\n",
      g_state.notify_name,
      g_state.notify_channel.ConsumerCount(),
      g_state.notify_channel.ReadableBytes());
  return 0;
}
OSP_SHELL_CMD(dd_notify, "Show notification channel status");

static int dd_peek(int, char*[]) {
  if (!g_state.notify_open) {
    osp::DebugShell::Printf("  notify channel not open\r\n");
    return 1;
  }
  NotifyMsg msg;
  uint32_t len = g_state.notify_channel.Read(
      reinterpret_cast<uint8_t*>(&msg), sizeof(msg));
  if (len != sizeof(msg)) {
    osp::DebugShell::Printf("  no notification available\r\n");
    return 1;
  }
  osp::DebugShell::Printf("  block_id=%u payload_size=%u\r\n",
                           msg.block_id, msg.payload_size);
  if (g_state.pool_attached && msg.block_id < kPoolMaxBlocks) {
    const uint8_t* data = g_state.disp.GetReadable(msg.block_id);
    if (ValidateLidarFrame(data, msg.payload_size)) {
      auto* hdr = reinterpret_cast<const LidarFrame*>(data);
      osp::DebugShell::Printf("  seq=%u pts=%u ts=%u ms\r\n",
                               hdr->seq_num, hdr->point_count,
                               hdr->timestamp_ms);
    }
    g_state.disp.Release(msg.block_id);
  }
  return 0;
}
OSP_SHELL_CMD(dd_peek, "Peek next notification and display frame header");

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
  const char* pool_name = kPoolShmName;
  const char* notify_name = kNotifyChannelName;
  uint16_t shell_port = kDefaultShellPort;
  bool console_mode = false;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--console") == 0) {
      console_mode = true;
    } else if (std::strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
      pool_name = argv[++i];
    } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      shell_port = static_cast<uint16_t>(std::atoi(argv[++i]));
    }
  }

  g_state.pool_name = pool_name;
  g_state.notify_name = notify_name;
  g_state.pool_size = Store::RequiredShmSize();

  OSP_LOG_INFO("Monitor", "starting: pool='%s' port=%u", pool_name, shell_port);

  osp::ShutdownManager shutdown;
  g_shutdown = &shutdown;
  shutdown.InstallSignalHandlers();

  // Attach to pool shm
  g_state.pool_shm = OpenPoolShm(pool_name, g_state.pool_size);
  if (g_state.pool_shm != nullptr) {
    g_state.disp.Attach(g_state.pool_shm);
    g_state.pool_attached = true;
    OSP_LOG_INFO("Monitor", "pool attached: free=%u alloc=%u",
                 g_state.disp.FreeBlocks(), g_state.disp.AllocBlocks());
  } else {
    OSP_LOG_WARN("Monitor", "pool shm not available yet");
  }

  // Open notify channel
  auto ch = osp::ShmSpmcByteChannel::OpenReader(notify_name);
  if (ch.has_value()) {
    g_state.notify_channel = static_cast<osp::ShmSpmcByteChannel&&>(ch.value());
    g_state.notify_open = true;
    OSP_LOG_INFO("Monitor", "notify channel opened");
  } else {
    OSP_LOG_WARN("Monitor", "notify channel not available yet");
  }

  std::thread poll_thread(PollingThread);

  osp::ConsoleShell console_shell({});
  osp::DebugShell::Config tcp_cfg;
  tcp_cfg.port = shell_port;
  tcp_cfg.max_connections = 4;
  osp::DebugShell tcp_shell(tcp_cfg);

  if (console_mode) {
    (void)console_shell.Start();
  } else {
    OSP_LOG_INFO("Monitor", "telnet shell on port %u", shell_port);
    (void)tcp_shell.Start();
  }

  while (!shutdown.IsShutdownRequested()) {
    // Retry attach if not yet connected
    if (!g_state.pool_attached) {
      g_state.pool_shm = OpenPoolShm(pool_name, g_state.pool_size);
      if (g_state.pool_shm != nullptr) {
        g_state.disp.Attach(g_state.pool_shm);
        g_state.pool_attached = true;
        OSP_LOG_INFO("Monitor", "pool attached (late)");
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  if (console_mode) console_shell.Stop(); else tcp_shell.Stop();
  poll_thread.join();
  if (g_state.pool_shm != nullptr)
    ClosePoolShm(g_state.pool_shm, g_state.pool_size);
  OSP_LOG_INFO("Monitor", "stopped");
  return 0;
}
