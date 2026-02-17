// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// launcher -- Process manager for data-dispatcher demo.
//
// Starts: producer, consumer_logging, consumer_fusion, monitor.
// Supervisor loop: health check every 500ms, optional auto-restart.
//
// Usage: ./osp_dd_launcher [--restart] [--frames N] [--port P]

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
  uint16_t shell_port = kDefaultShellPort;
  uint32_t frame_count = 500;
  bool auto_restart = false;
};

static LauncherConfig g_config;

// ---------------------------------------------------------------------------
// Child management
// ---------------------------------------------------------------------------

static constexpr uint32_t kMaxChildren = 4;

struct ChildSlot {
  osp::Subprocess proc;
  const char* label = nullptr;
  const char* const* argv = nullptr;
  uint32_t restarts = 0;
  bool critical = false;
};

static ChildSlot g_children[kMaxChildren];
static uint32_t g_num_children = 0;

static bool SpawnChild(uint32_t idx) {
  if (idx >= g_num_children) return false;
  osp::SubprocessConfig cfg;
  cfg.argv = g_children[idx].argv;
  cfg.capture_stdout = true;
  cfg.merge_stderr = true;
  auto result = g_children[idx].proc.Start(cfg);
  if (result == osp::ProcessResult::kSuccess) {
    OSP_LOG_INFO("Launcher", "[%s] started pid=%d",
                 g_children[idx].label,
                 static_cast<int>(g_children[idx].proc.GetPid()));
    return true;
  }
  OSP_LOG_ERROR("Launcher", "[%s] failed to start", g_children[idx].label);
  return false;
}

static void DrainOutput() {
  char buf[512];
  for (uint32_t i = 0; i < g_num_children; ++i) {
    if (g_children[i].proc.GetPid() <= 0) continue;
    int n = g_children[i].proc.ReadStdout(buf, sizeof(buf) - 1);
    while (n > 0) {
      buf[n] = '\0';
      if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
      if (buf[0] != '\0') OSP_LOG_INFO(g_children[i].label, "%s", buf);
      n = g_children[i].proc.ReadStdout(buf, sizeof(buf) - 1);
    }
  }
}

static bool HealthCheck(osp::ShutdownManager& shutdown) {
  for (uint32_t i = 0; i < g_num_children; ++i) {
    pid_t pid = g_children[i].proc.GetPid();
    if (pid <= 0) continue;
    if (!osp::IsProcessAlive(pid)) {
      auto wr = g_children[i].proc.Wait(0);
      if (wr.exited) {
        OSP_LOG_WARN("Launcher", "[%s] exited code=%d",
                     g_children[i].label, wr.exit_code);
      } else if (wr.signaled) {
        OSP_LOG_WARN("Launcher", "[%s] signal=%d",
                     g_children[i].label, wr.term_signal);
      }
      if (g_children[i].critical && !g_config.auto_restart) {
        OSP_LOG_ERROR("Launcher", "[%s] critical, shutting down",
                      g_children[i].label);
        return false;
      }
      if (g_config.auto_restart && !shutdown.IsShutdownRequested()) {
        ++g_children[i].restarts;
        OSP_LOG_INFO("Launcher", "[%s] restarting (#%u)...",
                     g_children[i].label, g_children[i].restarts);
        SpawnChild(i);
      }
    }
  }
  return true;
}

static void StopAll() {
  OSP_LOG_INFO("Launcher", "stopping all children...");
  for (int32_t i = static_cast<int32_t>(g_num_children) - 1; i >= 0; --i) {
    pid_t pid = g_children[i].proc.GetPid();
    if (pid > 0 && osp::IsProcessAlive(pid)) {
      kill(pid, SIGTERM);
    }
  }
  for (uint32_t attempt = 0; attempt < 20; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool all_dead = true;
    for (uint32_t i = 0; i < g_num_children; ++i) {
      pid_t pid = g_children[i].proc.GetPid();
      if (pid > 0 && osp::IsProcessAlive(pid)) { all_dead = false; break; }
    }
    if (all_dead) break;
  }
  for (uint32_t i = 0; i < g_num_children; ++i) {
    pid_t pid = g_children[i].proc.GetPid();
    if (pid > 0 && osp::IsProcessAlive(pid)) {
      OSP_LOG_WARN("Launcher", "[%s] force kill", g_children[i].label);
      kill(pid, SIGKILL);
    }
  }
  for (uint32_t i = 0; i < g_num_children; ++i) {
    if (g_children[i].proc.GetPid() > 0) g_children[i].proc.Wait(1000);
  }
  OSP_LOG_INFO("Launcher", "all children stopped");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--restart") == 0) {
      g_config.auto_restart = true;
    } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      g_config.frame_count = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      g_config.shell_port = static_cast<uint16_t>(std::atoi(argv[++i]));
    }
  }

  OSP_LOG_INFO("Launcher", "starting data-dispatcher demo");
  OSP_LOG_INFO("Launcher", "  frames: %u  auto-restart: %s",
               g_config.frame_count,
               g_config.auto_restart ? "yes" : "no");

  osp::ShutdownManager shutdown;
  shutdown.Register([](int) {});

  char frames_str[16];
  std::snprintf(frames_str, sizeof(frames_str), "%u", g_config.frame_count);
  char port_str[16];
  std::snprintf(port_str, sizeof(port_str), "%u", g_config.shell_port);

  static const char* prod_argv[] = {
      "./osp_dd_producer", "--frames", nullptr, nullptr};
  prod_argv[2] = frames_str;

  static const char* clog_argv[] = {"./osp_dd_consumer_logging", nullptr};
  static const char* cfus_argv[] = {"./osp_dd_consumer_fusion", nullptr};

  static const char* mon_argv[] = {
      "./osp_dd_monitor", "--port", nullptr, nullptr};
  mon_argv[2] = port_str;

  g_children[0] = {osp::Subprocess{}, "Producer", prod_argv, 0, true};
  g_children[1] = {osp::Subprocess{}, "Logging", clog_argv, 0, false};
  g_children[2] = {osp::Subprocess{}, "Fusion", cfus_argv, 0, false};
  g_children[3] = {osp::Subprocess{}, "Monitor", mon_argv, 0, false};
  g_num_children = 4;

  for (uint32_t i = 0; i < g_num_children; ++i) {
    if (!SpawnChild(i)) {
      StopAll();
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  OSP_LOG_INFO("Launcher", "all children started, supervisor loop");

  while (!shutdown.IsShutdownRequested()) {
    DrainOutput();
    if (!HealthCheck(shutdown)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  StopAll();
  return 0;
}
