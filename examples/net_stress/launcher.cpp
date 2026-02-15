// Copyright (c) 2024 liudegui. MIT License.
//
// net_stress launcher -- supervised process management for net_stress demo.
//
// Demonstrates:
//   1. Subprocess spawn with stdout capture (osp::Subprocess)
//   2. Supervisor pattern: auto-restart crashed children
//   3. Process freeze/resume for resource throttling
//   4. RunCommand for system info collection
//   5. Health monitoring via IsProcessAlive + ReadProcessState
//   6. Graceful shutdown: SIGINT -> stop all children -> exit
//   7. Shared INI config (net_stress.ini) with server/client/monitor
//
// Usage:
//   ./osp_net_stress_launcher [--config net_stress.ini] [--restart]
//
// The launcher reads net_stress.ini (shared with server/client/monitor),
// starts all processes, then enters a supervisor loop.
// Press Ctrl+C to shut down all children.
//
// newosp components used:
//   - osp::Config (IniBackend)  -- INI configuration (shared with all processes)
//   - osp::Subprocess           -- child process spawn + pipe capture
//   - osp::RunCommand           -- system info collection
//   - osp::IsProcessAlive       -- health check
//   - osp::ReadProcessState     -- state inspection
//   - osp::FreezeProcess        -- resource throttling demo
//   - osp::ResumeProcess        -- resume after throttle
//   - osp::log                  -- structured logging
//   - osp::ShutdownManager      -- graceful SIGINT handling

#include "osp/log.hpp"
#include "osp/process.hpp"
#include "osp/shutdown.hpp"
#include "protocol.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace net_stress;

// ============================================================================
// Launcher-specific config (extends NetStressConfig)
// ============================================================================

static constexpr uint32_t kMaxChildren = 8;
static constexpr uint32_t kHealthCheckMs = 1000;
static constexpr uint32_t kThrottleDemoSec = 3;

// ============================================================================
// Child slot management
// ============================================================================

struct ChildSlot {
  osp::Subprocess proc;
  const char* label;
  const char* const* argv;
  uint32_t restart_count;
  bool should_run;
};

static ChildSlot g_children[kMaxChildren];
static uint32_t g_num_children = 0;

static uint32_t AddChild(const char* label, const char* const* argv) {
  if (g_num_children >= kMaxChildren) return UINT32_MAX;
  uint32_t idx = g_num_children++;
  g_children[idx].label = label;
  g_children[idx].argv = argv;
  g_children[idx].restart_count = 0;
  g_children[idx].should_run = true;
  return idx;
}

static osp::ProcessResult StartChild(uint32_t idx) {
  osp::SubprocessConfig cfg;
  cfg.argv = g_children[idx].argv;
  cfg.capture_stdout = true;
  cfg.merge_stderr = true;

  auto r = g_children[idx].proc.Start(cfg);
  if (r == osp::ProcessResult::kSuccess) {
    OSP_LOG_INFO("LAUNCHER", "[%s] started, pid=%d",
                 g_children[idx].label, g_children[idx].proc.GetPid());
  } else {
    OSP_LOG_ERROR("LAUNCHER", "[%s] failed to start", g_children[idx].label);
  }
  return r;
}

// ============================================================================
// Output drain -- read and log child stdout
// ============================================================================

static void DrainOutput(uint32_t idx) {
  char buf[512];
  int n = g_children[idx].proc.ReadStdout(buf, sizeof(buf) - 1);
  while (n > 0) {
    buf[n] = '\0';
    if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
    if (buf[0] != '\0') {
      OSP_LOG_INFO(g_children[idx].label, "%s", buf);
    }
    n = g_children[idx].proc.ReadStdout(buf, sizeof(buf) - 1);
  }
}

// ============================================================================
// System info collection via RunCommand
// ============================================================================

static void PrintSystemInfo() {
  OSP_LOG_INFO("LAUNCHER", "=== System Information ===");

  const char* uname_argv[] = {"uname", "-srm", nullptr};
  std::string output;
  int code;
  if (osp::RunCommand(uname_argv, output, code) == osp::ProcessResult::kSuccess
      && code == 0) {
    if (!output.empty() && output.back() == '\n') output.pop_back();
    OSP_LOG_INFO("LAUNCHER", "Kernel: %s", output.c_str());
  }

  const char* nproc_argv[] = {"nproc", nullptr};
  if (osp::RunCommand(nproc_argv, output, code) == osp::ProcessResult::kSuccess
      && code == 0) {
    if (!output.empty() && output.back() == '\n') output.pop_back();
    OSP_LOG_INFO("LAUNCHER", "CPUs: %s", output.c_str());
  }

  const char* free_argv[] = {"free", "-h", "--si", nullptr};
  if (osp::RunCommand(free_argv, output, code) == osp::ProcessResult::kSuccess
      && code == 0) {
    size_t pos = output.find('\n');
    if (pos != std::string::npos) {
      size_t pos2 = output.find('\n', pos + 1);
      if (pos2 != std::string::npos) {
        OSP_LOG_INFO("LAUNCHER", "Memory: %s",
                     output.substr(pos + 1, pos2 - pos - 1).c_str());
      }
    }
  }

  OSP_LOG_INFO("LAUNCHER", "=========================");
}

// ============================================================================
// Throttle demo -- freeze/resume a client briefly
// ============================================================================

static void ThrottleDemo(uint32_t client_idx) {
  pid_t pid = g_children[client_idx].proc.GetPid();
  if (pid <= 0 || !g_children[client_idx].proc.IsRunning()) return;

  OSP_LOG_INFO("LAUNCHER", "[%s] throttle: freezing pid=%d for %us",
               g_children[client_idx].label, pid, kThrottleDemoSec);

  auto r = osp::FreezeProcess(pid);
  if (r != osp::ProcessResult::kSuccess) {
    OSP_LOG_WARN("LAUNCHER", "[%s] freeze failed", g_children[client_idx].label);
    return;
  }

  char state = osp::ReadProcessState(pid);
  OSP_LOG_INFO("LAUNCHER", "[%s] state after freeze: '%c'",
               g_children[client_idx].label, state);

  std::this_thread::sleep_for(std::chrono::seconds(kThrottleDemoSec));

  r = osp::ResumeProcess(pid);
  state = osp::ReadProcessState(pid);
  OSP_LOG_INFO("LAUNCHER", "[%s] resumed, state: '%c'",
               g_children[client_idx].label, state);
}

// ============================================================================
// Print loaded config
// ============================================================================

static void PrintConfig(const NetStressConfig& c, bool auto_restart) {
  OSP_LOG_INFO("LAUNCHER", "=== Configuration ===");
  OSP_LOG_INFO("LAUNCHER", "  auto_restart:     %s", auto_restart ? "yes" : "no");
  OSP_LOG_INFO("LAUNCHER", "  server_hs_port:   %u", c.server_hs_port);
  OSP_LOG_INFO("LAUNCHER", "  server_echo_port: %u", c.server_echo_port);
  OSP_LOG_INFO("LAUNCHER", "  server_shell:     %u", c.server_shell_port);
  OSP_LOG_INFO("LAUNCHER", "  client_num:       %u", c.client_num);
  OSP_LOG_INFO("LAUNCHER", "  client_interval:  %ums", c.client_interval_ms);
  OSP_LOG_INFO("LAUNCHER", "  monitor_probe:    %ums", c.monitor_probe_ms);
  OSP_LOG_INFO("LAUNCHER", "  monitor_shell:    %u", c.monitor_shell_port);
  OSP_LOG_INFO("LAUNCHER", "=====================");
}

// ============================================================================
// Supervisor loop
// ============================================================================

static void SupervisorLoop(bool auto_restart,
                           osp::ShutdownManager& shutdown) {
  uint32_t tick = 0;
  bool throttle_done = false;

  while (!shutdown.IsShutdownRequested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kHealthCheckMs));
    ++tick;

    // Drain output from all children
    for (uint32_t i = 0; i < g_num_children; ++i) {
      DrainOutput(i);
    }

    // Health check + auto-restart
    for (uint32_t i = 0; i < g_num_children; ++i) {
      if (!g_children[i].should_run) continue;

      if (!g_children[i].proc.IsRunning()) {
        auto wr = g_children[i].proc.Wait(100);
        if (wr.exited) {
          OSP_LOG_WARN("LAUNCHER", "[%s] exited with code %d",
                       g_children[i].label, wr.exit_code);
        } else if (wr.signaled) {
          OSP_LOG_WARN("LAUNCHER", "[%s] killed by signal %d",
                       g_children[i].label, wr.term_signal);
        }

        if (auto_restart) {
          g_children[i].restart_count++;
          OSP_LOG_INFO("LAUNCHER", "[%s] restarting (#%u)",
                       g_children[i].label, g_children[i].restart_count);
          StartChild(i);
        } else {
          g_children[i].should_run = false;
        }
      }
    }

    // Throttle demo: freeze/resume first client after 5 ticks
    if (!throttle_done && tick >= 5 && g_num_children > 2) {
      for (uint32_t i = 0; i < g_num_children; ++i) {
        if (std::strstr(g_children[i].label, "client") != nullptr) {
          ThrottleDemo(i);
          throttle_done = true;
          break;
        }
      }
    }

    // Periodic status every 10 ticks
    if (tick % 10 == 0) {
      OSP_LOG_INFO("LAUNCHER", "--- Status (tick=%u) ---", tick);
      for (uint32_t i = 0; i < g_num_children; ++i) {
        pid_t pid = g_children[i].proc.GetPid();
        bool alive = g_children[i].proc.IsRunning();
        char state = alive ? osp::ReadProcessState(pid) : '-';
        OSP_LOG_INFO("LAUNCHER", "  [%s] pid=%d alive=%d state='%c' restarts=%u",
                     g_children[i].label, pid, alive, state,
                     g_children[i].restart_count);
      }
    }
  }
}

// ============================================================================
// Shutdown: stop all children gracefully
// ============================================================================

static void StopAllChildren() {
  OSP_LOG_INFO("LAUNCHER", "Stopping all children...");

  for (uint32_t i = 0; i < g_num_children; ++i) {
    g_children[i].should_run = false;
    if (g_children[i].proc.IsRunning()) {
      OSP_LOG_INFO("LAUNCHER", "[%s] killing pid=%d",
                   g_children[i].label, g_children[i].proc.GetPid());
      g_children[i].proc.Signal(SIGKILL);
    }
  }

  for (uint32_t i = 0; i < g_num_children; ++i) {
    if (g_children[i].proc.GetPid() > 0) {
      auto wr = g_children[i].proc.Wait(2000);
      if (wr.timed_out) {
        OSP_LOG_WARN("LAUNCHER", "[%s] did not exit in time",
                     g_children[i].label);
      } else {
        OSP_LOG_INFO("LAUNCHER", "[%s] stopped", g_children[i].label);
      }
    }
  }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
  osp::log::Init();
  osp::log::SetLevel(osp::log::Level::kInfo);

  // --- Parse launcher-specific args ---
  bool auto_restart = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--restart") == 0) {
      auto_restart = true;
    } else if (std::strcmp(argv[i], "--help") == 0) {
      std::printf(
          "Usage: %s [--config net_stress.ini] [--restart]\n"
          "\n"
          "  --config FILE   INI config file (default: net_stress.ini)\n"
          "  --restart       Auto-restart crashed children\n"
          "  --help          Show this help\n",
          argv[0]);
      return 0;
    }
  }

  OSP_LOG_INFO("LAUNCHER", "=== Net Stress Launcher ===");

  // --- Load shared INI config (same as server/client/monitor) ---
  NetStressConfig cfg;
  const char* ini_path = FindConfigArg(argc, argv);
  LoadConfig(ini_path ? ini_path : "net_stress.ini", cfg);

  PrintConfig(cfg, auto_restart);
  PrintSystemInfo();

  // --- Shutdown manager ---
  osp::ShutdownManager shutdown;
  shutdown.InstallSignalHandlers();
  OSP_LOG_INFO("LAUNCHER", "Press Ctrl+C to shut down all processes.");

  // --- Build argv arrays for children ---
  // All children share the same --config path
  const char* config_arg = ini_path ? ini_path : "net_stress.ini";

  // Server: ./osp_net_stress_server --config <ini>
  static const char* server_argv[4];
  server_argv[0] = "./osp_net_stress_server";
  server_argv[1] = "--config";
  server_argv[2] = config_arg;
  server_argv[3] = nullptr;
  AddChild("server", server_argv);

  // Monitor: ./osp_net_stress_monitor --config <ini> 127.0.0.1
  static const char* monitor_argv[6];
  monitor_argv[0] = "./osp_net_stress_monitor";
  monitor_argv[1] = "--config";
  monitor_argv[2] = config_arg;
  monitor_argv[3] = "127.0.0.1";
  monitor_argv[4] = nullptr;
  AddChild("monitor", monitor_argv);

  // Clients: ./osp_net_stress_client --config <ini> 127.0.0.1 <num_clients>
  static char client_num_buf[8];
  snprintf(client_num_buf, sizeof(client_num_buf), "%u", cfg.client_num);
  static const char* client_argv[7];
  client_argv[0] = "./osp_net_stress_client";
  client_argv[1] = "--config";
  client_argv[2] = config_arg;
  client_argv[3] = "127.0.0.1";
  client_argv[4] = client_num_buf;
  client_argv[5] = nullptr;
  AddChild("client", client_argv);

  // --- Start all children ---
  OSP_LOG_INFO("LAUNCHER", "Starting %u children...", g_num_children);
  for (uint32_t i = 0; i < g_num_children; ++i) {
    StartChild(i);
    // Server starts first, wait for port binding
    if (i == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }

  // --- Supervisor loop ---
  SupervisorLoop(auto_restart, shutdown);

  // --- Cleanup ---
  StopAllChildren();

  OSP_LOG_INFO("LAUNCHER", "All children stopped. Launcher exit.");
  return 0;
}
