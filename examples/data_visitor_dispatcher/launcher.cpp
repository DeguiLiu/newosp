// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// launcher -- Process manager for data-visitor-dispatcher demo.
//
// Demonstrates: Subprocess spawn, health monitoring, graceful shutdown.
//
// Starts: producer, visitor_logging, visitor_fusion, monitor.
// Supervisor loop: health check every 1s, optional auto-restart.
// SIGINT -> graceful shutdown all children.
//
// Usage: ./osp_dvd_launcher [--restart] [--channel name] [--frames N]
//                           [--port P]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

#include "osp/log.hpp"
#include "osp/process.hpp"
#include "osp/shutdown.hpp"

#include "common.hpp"

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
struct LauncherConfig {
  const char* channel_name = kDefaultChannelName;
  uint16_t shell_port = kDefaultShellPort;
  uint32_t frame_count = 500;
  bool auto_restart = false;
};

static LauncherConfig g_config;

// ---------------------------------------------------------------------------
// Child slot management
// ---------------------------------------------------------------------------
static constexpr uint32_t kMaxChildren = 4;

struct ChildSlot {
  osp::Subprocess proc;
  const char* label = nullptr;
  const char* const* argv = nullptr;
  uint32_t restarts = 0;
  bool critical = false;  // If critical child dies, shut down all
};

static ChildSlot g_children[kMaxChildren];
static uint32_t g_num_children = 0;

// ---------------------------------------------------------------------------
// Spawn a child
// ---------------------------------------------------------------------------
static bool SpawnChild(uint32_t idx) {
  if (idx >= g_num_children) return false;

  osp::SubprocessConfig cfg;
  cfg.argv = g_children[idx].argv;
  cfg.capture_stdout = true;
  cfg.merge_stderr = true;

  auto result = g_children[idx].proc.Start(cfg);
  if (result == osp::ProcessResult::kSuccess) {
    OSP_LOG_INFO("Launcher", "[%s] started, pid=%d",
                 g_children[idx].label,
                 static_cast<int>(g_children[idx].proc.GetPid()));
    return true;
  }
  OSP_LOG_ERROR("Launcher", "[%s] failed to start", g_children[idx].label);
  return false;
}

// ---------------------------------------------------------------------------
// Drain stdout from children
// ---------------------------------------------------------------------------
static void DrainChildOutput() {
  char buf[512];
  for (uint32_t i = 0; i < g_num_children; ++i) {
    if (g_children[i].proc.GetPid() <= 0) continue;
    int n = g_children[i].proc.ReadStdout(buf, sizeof(buf) - 1);
    while (n > 0) {
      buf[n] = '\0';
      // Remove trailing newline for cleaner log
      if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
      if (buf[0] != '\0') {
        OSP_LOG_INFO(g_children[i].label, "%s", buf);
      }
      n = g_children[i].proc.ReadStdout(buf, sizeof(buf) - 1);
    }
  }
}

// ---------------------------------------------------------------------------
// Health check
// ---------------------------------------------------------------------------
static bool HealthCheck(osp::ShutdownManager& shutdown) {
  for (uint32_t i = 0; i < g_num_children; ++i) {
    pid_t pid = g_children[i].proc.GetPid();
    if (pid <= 0) continue;

    if (!osp::IsProcessAlive(pid)) {
      auto wr = g_children[i].proc.Wait(0);
      if (wr.exited) {
        OSP_LOG_WARN("Launcher", "[%s] exited with code %d",
                     g_children[i].label, wr.exit_code);
      } else if (wr.signaled) {
        OSP_LOG_WARN("Launcher", "[%s] killed by signal %d",
                     g_children[i].label, wr.term_signal);
      }

      if (g_children[i].critical && !g_config.auto_restart) {
        OSP_LOG_ERROR("Launcher", "[%s] is critical, shutting down all",
                      g_children[i].label);
        return false;  // Trigger shutdown
      }

      if (g_config.auto_restart && !shutdown.IsShutdownRequested()) {
        ++g_children[i].restarts;
        OSP_LOG_INFO("Launcher", "[%s] restarting (attempt %u)...",
                     g_children[i].label, g_children[i].restarts);
        SpawnChild(i);
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Stop all children
// ---------------------------------------------------------------------------
static void StopAll() {
  OSP_LOG_INFO("Launcher", "stopping all children...");

  // SIGTERM first (reverse order)
  for (int32_t i = static_cast<int32_t>(g_num_children) - 1; i >= 0; --i) {
    pid_t pid = g_children[i].proc.GetPid();
    if (pid > 0 && osp::IsProcessAlive(pid)) {
      OSP_LOG_INFO("Launcher", "[%s] sending SIGTERM (pid=%d)",
                   g_children[i].label, static_cast<int>(pid));
      kill(pid, SIGTERM);
    }
  }

  // Wait up to 2s for graceful exit
  for (uint32_t attempt = 0; attempt < 20; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool all_dead = true;
    for (uint32_t i = 0; i < g_num_children; ++i) {
      pid_t pid = g_children[i].proc.GetPid();
      if (pid > 0 && osp::IsProcessAlive(pid)) {
        all_dead = false;
        break;
      }
    }
    if (all_dead) break;
  }

  // SIGKILL remaining
  for (uint32_t i = 0; i < g_num_children; ++i) {
    pid_t pid = g_children[i].proc.GetPid();
    if (pid > 0 && osp::IsProcessAlive(pid)) {
      OSP_LOG_WARN("Launcher", "[%s] force killing (pid=%d)",
                   g_children[i].label, static_cast<int>(pid));
      kill(pid, SIGKILL);
    }
  }

  // Collect exit status
  for (uint32_t i = 0; i < g_num_children; ++i) {
    pid_t pid = g_children[i].proc.GetPid();
    if (pid > 0) {
      auto wr = g_children[i].proc.Wait(1000);
      if (wr.exited) {
        OSP_LOG_INFO("Launcher", "[%s] exited with code %d",
                     g_children[i].label, wr.exit_code);
      } else if (wr.signaled) {
        OSP_LOG_INFO("Launcher", "[%s] killed by signal %d",
                     g_children[i].label, wr.term_signal);
      }
    }
  }

  OSP_LOG_INFO("Launcher", "all children stopped");
}

// ---------------------------------------------------------------------------
// Parse args
// ---------------------------------------------------------------------------
static void ParseArgs(int argc, char* argv[]) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--restart") == 0) {
      g_config.auto_restart = true;
    } else if (std::strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
      g_config.channel_name = argv[++i];
    } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      g_config.frame_count = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      g_config.shell_port = static_cast<uint16_t>(std::atoi(argv[++i]));
    }
  }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  ParseArgs(argc, argv);

  OSP_LOG_INFO("Launcher", "starting data-visitor-dispatcher");
  OSP_LOG_INFO("Launcher", "  channel: %s", g_config.channel_name);
  OSP_LOG_INFO("Launcher", "  shell port: %u", g_config.shell_port);
  OSP_LOG_INFO("Launcher", "  frames: %u", g_config.frame_count);
  OSP_LOG_INFO("Launcher", "  auto-restart: %s",
               g_config.auto_restart ? "yes" : "no");

  osp::ShutdownManager shutdown;
  shutdown.Register([](int) {});

  // Build argv arrays for children
  char frames_str[16];
  std::snprintf(frames_str, sizeof(frames_str), "%u", g_config.frame_count);
  char port_str[16];
  std::snprintf(port_str, sizeof(port_str), "%u", g_config.shell_port);

  // Producer
  static const char* producer_argv[] = {
      "./osp_dvd_producer", nullptr, nullptr, nullptr};
  producer_argv[1] = g_config.channel_name;
  producer_argv[2] = frames_str;

  // Visitor logging
  static const char* vlog_argv[] = {
      "./osp_dvd_visitor_logging", nullptr, nullptr};
  vlog_argv[1] = g_config.channel_name;

  // Visitor fusion
  static const char* vfus_argv[] = {
      "./osp_dvd_visitor_fusion", nullptr, nullptr};
  vfus_argv[1] = g_config.channel_name;

  // Monitor
  static const char* mon_argv[] = {
      "./osp_dvd_monitor", nullptr, nullptr, nullptr};
  mon_argv[1] = g_config.channel_name;
  mon_argv[2] = port_str;

  // Register children
  g_children[0] = {osp::Subprocess{}, "Producer", producer_argv, 0, true};
  g_children[1] = {osp::Subprocess{}, "Logging", vlog_argv, 0, false};
  g_children[2] = {osp::Subprocess{}, "Fusion", vfus_argv, 0, false};
  g_children[3] = {osp::Subprocess{}, "Monitor", mon_argv, 0, false};
  g_num_children = 4;

  // Start all children
  OSP_LOG_INFO("Launcher", "starting %u children...", g_num_children);
  for (uint32_t i = 0; i < g_num_children; ++i) {
    if (!SpawnChild(i)) {
      OSP_LOG_ERROR("Launcher", "failed to start [%s], aborting",
                    g_children[i].label);
      StopAll();
      return 1;
    }
    // Small delay between spawns
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  OSP_LOG_INFO("Launcher", "all children started, entering supervisor loop");

  // Supervisor loop
  while (!shutdown.IsShutdownRequested()) {
    DrainChildOutput();

    if (!HealthCheck(shutdown)) {
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  OSP_LOG_INFO("Launcher", "shutdown requested");
  StopAll();
  OSP_LOG_INFO("Launcher", "launcher exiting");
  return 0;
}
