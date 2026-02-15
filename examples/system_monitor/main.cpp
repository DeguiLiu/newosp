/**
 * MIT License
 *
 * Copyright (c) 2024 liudegui
 */

/**
 * @file system_monitor_demo.cpp
 * @brief Demonstrates SystemMonitor + YAML config (via config.hpp).
 *
 * Shows: YAML configuration loading, threshold setup, CPU/memory/disk
 * monitoring, state-change alerts, FaultReporter integration pattern.
 *
 * Build: cmake -DOSP_CONFIG_YAML=ON -DOSP_BUILD_EXAMPLES=ON ..
 * Run:   ./osp_system_monitor_demo [config.yaml]
 */

#include "osp/config.hpp"
#include "osp/log.hpp"
#include "osp/system_monitor.hpp"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <thread>

// Default config path (relative to working directory)
static constexpr const char* kDefaultConfigPath = "system_monitor.yaml";

// Alert callback: print to stderr
static void OnAlert(const osp::SystemSnapshot& /*snap*/,
                    const char* msg, void* /*ctx*/) {
  std::fprintf(stderr, "  ** ALERT: %s\n", msg);
}

int main(int argc, char* argv[]) {
  std::printf("=== newosp SystemMonitor + YAML Config Demo ===\n\n");

  // ---- 1. Load YAML configuration ----
  const char* config_path = (argc > 1) ? argv[1] : kDefaultConfigPath;

#ifdef OSP_CONFIG_YAML_ENABLED
  osp::Config<osp::YamlBackend> cfg;
#else
  // Fallback: use IniBackend if YAML not enabled (won't parse YAML)
  osp::Config<osp::IniBackend> cfg;
  std::printf("NOTE: YAML support not enabled (OSP_CONFIG_YAML=OFF).\n");
  std::printf("      Using hardcoded defaults.\n\n");
#endif

  bool config_loaded = false;
  auto result = cfg.LoadFile(config_path);
  if (result.has_value()) {
    std::printf("Loaded config: %s\n", config_path);
    config_loaded = true;
  } else {
    std::printf("Config file not found: %s (using defaults)\n", config_path);
  }

  // ---- 2. Read thresholds from config (with defaults) ----
  osp::AlertThresholds thresholds;
  thresholds.cpu_percent = static_cast<uint32_t>(
      cfg.GetInt("thresholds", "cpu_percent", 90));
  thresholds.cpu_temp_mc = cfg.GetInt("thresholds", "cpu_temp_celsius", 85) * 1000;
  thresholds.memory_percent = static_cast<uint32_t>(
      cfg.GetInt("thresholds", "memory_percent", 90));
  thresholds.disk_percent = static_cast<uint32_t>(
      cfg.GetInt("thresholds", "disk_percent", 90));

  // Read sampling parameters
  int32_t interval_ms = cfg.GetInt("sampling", "interval_ms", 1000);
  int32_t sample_count = cfg.GetInt("sampling", "count", 5);
  bool alert_stderr = cfg.GetBool("logging", "alert_to_stderr", true);

  std::printf("\nConfiguration:\n");
  std::printf("  thresholds.cpu_percent:    %" PRIu32 "%%\n",
              thresholds.cpu_percent);
  std::printf("  thresholds.cpu_temp:       %d.%d C\n",
              thresholds.cpu_temp_mc / 1000,
              (thresholds.cpu_temp_mc % 1000) / 100);
  std::printf("  thresholds.memory_percent: %" PRIu32 "%%\n",
              thresholds.memory_percent);
  std::printf("  thresholds.disk_percent:   %" PRIu32 "%%\n",
              thresholds.disk_percent);
  std::printf("  sampling.interval_ms:      %" PRId32 "\n", interval_ms);
  std::printf("  sampling.count:            %" PRId32 "\n", sample_count);
  std::printf("  logging.alert_to_stderr:   %s\n",
              alert_stderr ? "true" : "false");

  // ---- 3. Create and configure SystemMonitor ----
  osp::SystemMonitor<4> monitor;
  monitor.SetThresholds(thresholds);

  if (alert_stderr) {
    monitor.SetAlertCallback(OnAlert, nullptr);
  }

  // Read disk paths from config
  for (int32_t i = 0; i < 4; ++i) {
    char key[16];
    std::snprintf(key, sizeof(key), "path_%d", i);
    const char* path = cfg.GetString("disk_paths", key, "");
    if (path[0] != '\0') {
      if (monitor.AddDiskPath(path)) {
        std::printf("  disk_path[%d]: %s\n", i, path);
      }
    }
  }

  // Fallback: if no disk paths configured, monitor root
  if (monitor.DiskPathCount() == 0) {
    monitor.AddDiskPath("/");
    std::printf("  disk_path[0]: / (default)\n");
  }

  // ---- 4. FaultReporter integration pattern (commented) ----
  // osp::FaultCollector<8, 32> collector;
  // monitor.SetFaultReporter([](uint16_t idx, uint32_t det,
  //     uint8_t pri, void* c) {
  //   static_cast<osp::FaultCollector<8,32>*>(c)->ReportFault(
  //       idx, det, static_cast<osp::FaultPriority>(pri));
  // }, &collector);

  // ---- 5. Sampling loop ----
  std::printf("\n%-6s %6s %6s %6s %6s %8s | %8s %8s %5s | %s\n",
              "Sample", "CPU%", "User%", "Sys%", "IOW%", "Temp",
              "MemTotal", "MemUsed", "Mem%", "Disk");
  std::printf("------ ------ ------ ------ ------ -------- | "
              "-------- -------- ----- | ----\n");

  for (int32_t i = 0; i < sample_count; ++i) {
    auto snap = monitor.Sample();

    // Format temperature
    char temp_str[16];
    if (snap.cpu.temperature_mc >= 0) {
      std::snprintf(temp_str, sizeof(temp_str), "%d.%dC",
                    snap.cpu.temperature_mc / 1000,
                    (snap.cpu.temperature_mc % 1000) / 100);
    } else {
      std::snprintf(temp_str, sizeof(temp_str), "N/A");
    }

    // Format disk info
    char disk_str[128] = {};
    uint32_t off = 0;
    for (uint32_t d = 0; d < monitor.DiskPathCount(); ++d) {
      const auto& ds = monitor.GetDiskSnapshot(d);
      char dkey[16];
      std::snprintf(dkey, sizeof(dkey), "path_%u", d);
      const char* dpath = cfg.GetString("disk_paths", dkey, "?");
      if (dpath[0] == '\0') dpath = "?";
      int written = std::snprintf(disk_str + off, sizeof(disk_str) - off,
                                  "%s:%" PRIu32 "%% ", dpath,
                                  ds.used_percent);
      if (written > 0) {
        off += static_cast<uint32_t>(written);
      }
    }

    std::printf("#%-5" PRId32 " %5" PRIu32 "%% %5" PRIu32 "%% %5" PRIu32
                "%% %5" PRIu32 "%% %8s | %5" PRIu64 "MB %5" PRIu64
                "MB %4" PRIu32 "%% | %s\n",
                i + 1,
                snap.cpu.total_percent, snap.cpu.user_percent,
                snap.cpu.system_percent, snap.cpu.iowait_percent,
                temp_str,
                snap.memory.total_kb / 1024, snap.memory.used_kb / 1024,
                snap.memory.used_percent,
                disk_str);

    if (i < sample_count - 1) {
      std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
  }

  // ---- 6. Summary ----
  const auto& last = monitor.LastSnapshot();
  std::printf("\nFinal: CPU %" PRIu32 "%%  Mem %" PRIu32
              "%%  Disks %" PRIu32 "\n",
              last.cpu.total_percent, last.memory.used_percent,
              monitor.DiskPathCount());

  if (config_loaded) {
    std::printf("Config source: %s\n", config_path);
  }
  std::printf("Demo completed.\n");
  return 0;
}
