/**
 * MIT License
 *
 * Copyright (c) 2024 liudegui
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file system_monitor.hpp
 * @brief Header-only Linux system health monitor for embedded platforms.
 *
 * Monitors CPU utilization, CPU temperature, memory usage, and disk usage by
 * reading /proc and /sys filesystems. Zero heap allocation, compatible with
 * -fno-exceptions -fno-rtti.
 *
 * Design:
 * - POD snapshots for zero-copy data access.
 * - Stack-only parsing of /proc/stat, /proc/meminfo, /sys/class/thermal.
 * - Delta-based CPU utilization (requires two samples with interval).
 * - State-change alert pattern (only fire on threshold crossing).
 * - Function pointer callbacks for alert notifications.
 * - Template parameter for max disk paths (default 4).
 * - Static SampleTick() for TimerScheduler integration.
 *
 * Typical usage:
 *
 *   #include "osp/system_monitor.hpp"
 *   #include "osp/timer.hpp"
 *
 *   osp::SystemMonitor<4> monitor;
 *
 *   // Configure thresholds
 *   osp::AlertThresholds thresholds;
 *   thresholds.cpu_percent = 85;
 *   thresholds.cpu_temp_mc = 80000;  // 80°C
 *   thresholds.memory_percent = 90;
 *   thresholds.disk_percent = 95;
 *   monitor.SetThresholds(thresholds);
 *
 *   // Set alert callback
 *   monitor.SetAlertCallback([](const osp::SystemSnapshot& snap,
 *                                const char* msg, void* ctx) {
 *     fprintf(stderr, "ALERT: %s\n", msg);
 *   }, nullptr);
 *
 *   // Add disk paths to monitor
 *   monitor.AddDiskPath("/");
 *   monitor.AddDiskPath("/tmp");
 *
 *   // Integrate with timer (sample every 1 second)
 *   osp::TimerScheduler scheduler;
 *   scheduler.AddTimer(1000, osp::SystemMonitor<4>::SampleTick, &monitor);
 *
 *   // Or manual sampling in main loop
 *   while (running) {
 *     auto snap = monitor.Sample();
 *     printf("CPU: %u%%, Mem: %u%%\n", snap.cpu.total_percent,
 *            snap.memory.used_percent);
 *     std::this_thread::sleep_for(std::chrono::seconds(1));
 *   }
 *
 * Linux-only: All monitoring functions return zeros on non-Linux platforms.
 */

#ifndef OSP_SYSTEM_MONITOR_HPP_
#define OSP_SYSTEM_MONITOR_HPP_

#include "osp/platform.hpp"

#if defined(OSP_PLATFORM_LINUX)

#include "osp/log.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace osp {

// Forward declaration for FaultReporter integration (optional)
// Modules that include fault_collector.hpp before this header get full support.
// Otherwise, SetFaultReporter() is still available but the caller must provide
// the FaultReporter struct definition.

// ============================================================================
// Fault indices for SystemMonitor (used with FaultReporter)
// ============================================================================

/// Predefined fault indices for SystemMonitor alerts.
/// Users can override by passing custom indices to SetFaultIndices().
struct SystemMonitorFaultIndex {
  uint16_t cpu_overload = 0;     ///< CPU usage exceeded threshold
  uint16_t cpu_overheat = 1;     ///< CPU temperature exceeded threshold
  uint16_t memory_overload = 2;  ///< Memory usage exceeded threshold
  uint16_t disk_full = 3;        ///< Disk usage exceeded threshold
};

// ============================================================================
// Data Structures (all POD, zero-copy snapshots)
// ============================================================================

/**
 * @brief CPU utilization snapshot (per-core + total).
 *
 * Percentages are in range [0, 100]. Temperature is in milli-Celsius
 * (e.g., 45000 = 45.0°C), or -1 if unavailable.
 */
struct CpuSnapshot {
  uint32_t total_percent;   ///< Overall CPU usage (0-100).
  uint32_t user_percent;    ///< User-space usage (0-100).
  uint32_t system_percent;  ///< Kernel-space usage (0-100).
  uint32_t iowait_percent;  ///< I/O wait percentage (0-100).
  int32_t temperature_mc;   ///< CPU temperature in milli-Celsius (-1 if N/A).
};

/**
 * @brief Memory usage snapshot.
 *
 * All sizes in kilobytes (kB). Percentage is in range [0, 100].
 */
struct MemorySnapshot {
  uint64_t total_kb;      ///< Total physical memory (kB).
  uint64_t available_kb;  ///< Available memory (kB).
  uint64_t used_kb;       ///< Used memory (kB).
  uint32_t used_percent;  ///< Usage percentage (0-100).
};

/**
 * @brief Disk usage snapshot for a single mount point.
 *
 * Sizes in bytes. Percentage is in range [0, 100].
 */
struct DiskSnapshot {
  uint64_t total_bytes;      ///< Total space (bytes).
  uint64_t available_bytes;  ///< Available space (bytes).
  uint32_t used_percent;     ///< Usage percentage (0-100).
};

/**
 * @brief Combined system health snapshot.
 *
 * Includes CPU, memory, and timestamp. Disk snapshots are accessed separately
 * via GetDiskSnapshot().
 */
struct SystemSnapshot {
  CpuSnapshot cpu;        ///< CPU metrics.
  MemorySnapshot memory;  ///< Memory metrics.
  uint64_t timestamp_us;  ///< Monotonic timestamp (SteadyNowUs).
};

// ============================================================================
// Alert configuration
// ============================================================================

/**
 * @brief Alert thresholds for system health monitoring.
 *
 * Alerts fire when metrics cross thresholds (state-change pattern).
 */
struct AlertThresholds {
  uint32_t cpu_percent = 90;     ///< CPU usage alert threshold (0-100).
  int32_t cpu_temp_mc = 85000;   ///< CPU temp alert (85°C in milli-Celsius).
  uint32_t memory_percent = 90;  ///< Memory usage alert threshold (0-100).
  uint32_t disk_percent = 90;    ///< Disk usage alert threshold (0-100).
};

// ============================================================================
// SystemMonitor class template
// ============================================================================

/**
 * @brief Header-only Linux system health monitor.
 *
 * Monitors CPU, memory, disk, and temperature. Zero heap allocation in hot
 * path. Compatible with -fno-exceptions -fno-rtti.
 *
 * @tparam MaxDiskPaths  Maximum number of monitored disk paths (default 4).
 */
template <uint32_t MaxDiskPaths = 4U>
class SystemMonitor final {
 public:
  /**
   * @brief Alert callback signature.
   *
   * Invoked when a metric crosses its threshold. Message is a stack-allocated
   * string describing the alert.
   *
   * @param snap     Current system snapshot.
   * @param alert_msg  Alert message (stack buffer, valid only during call).
   * @param ctx      User context pointer.
   */
  using AlertCallback = void (*)(const SystemSnapshot& snap, const char* alert_msg, void* ctx);

  SystemMonitor() noexcept = default;
  ~SystemMonitor() noexcept = default;

  // Non-copyable, non-movable
  SystemMonitor(const SystemMonitor&) = delete;
  SystemMonitor& operator=(const SystemMonitor&) = delete;

  // -- Configuration (call before first Sample) --

  /**
   * @brief Set alert thresholds.
   *
   * @param t  Threshold configuration.
   */
  void SetThresholds(const AlertThresholds& t) noexcept { thresholds_ = t; }

  /**
   * @brief Set alert callback.
   *
   * @param fn   Callback function pointer.
   * @param ctx  User context pointer (passed to callback).
   */
  void SetAlertCallback(AlertCallback fn, void* ctx = nullptr) noexcept {
    alert_fn_ = fn;
    alert_ctx_ = ctx;
  }

  // -- FaultReporter integration (optional) --

  /// Function pointer type matching FaultReporter::fn signature.
  using FaultReportFn = void (*)(uint16_t fault_index, uint32_t detail, uint8_t priority, void* ctx);

  /**
   * @brief Set fault reporter for threshold-crossing events.
   *
   * When a metric crosses its threshold, the monitor reports a fault via
   * this function pointer (in addition to the AlertCallback and log).
   * This integrates with osp::FaultCollector through the FaultReporter
   * injection pattern (see fault_collector.hpp).
   *
   * @param fn   Fault report function (nullptr = disabled).
   * @param ctx  User context (typically FaultCollector*).
   *
   * Wiring example:
   * @code
   *   osp::FaultCollector<8, 32> collector;
   *   monitor.SetFaultReporter([](uint16_t idx, uint32_t det,
   *       uint8_t pri, void* c) {
   *     static_cast<osp::FaultCollector<8,32>*>(c)->ReportFault(
   *         idx, det, static_cast<osp::FaultPriority>(pri));
   *   }, &collector);
   * @endcode
   */
  void SetFaultReporter(FaultReportFn fn, void* ctx = nullptr) noexcept {
    fault_fn_ = fn;
    fault_ctx_ = ctx;
  }

  /**
   * @brief Set custom fault indices for each alert type.
   *
   * Default: cpu_overload=0, cpu_overheat=1, memory_overload=2, disk_full=3.
   */
  void SetFaultIndices(const SystemMonitorFaultIndex& idx) noexcept { fault_idx_ = idx; }

  /**
   * @brief Add a disk path to monitor (e.g., "/", "/tmp", "/userdata").
   *
   * @param path  Mount point path (copied to internal buffer, max 63 chars).
   * @return true if added, false if MaxDiskPaths reached or path is nullptr.
   */
  bool AddDiskPath(const char* path) noexcept {
    if (path == nullptr || disk_count_ >= MaxDiskPaths) {
      return false;
    }
    auto& slot = disk_slots_[disk_count_];
    std::strncpy(slot.path, path, sizeof(slot.path) - 1);
    slot.path[sizeof(slot.path) - 1] = '\0';
    slot.active = true;
    ++disk_count_;
    return true;
  }

  // -- Sampling (call periodically from timer or main loop) --

  /**
   * @brief Sample all metrics and check thresholds.
   *
   * This is the main entry point. Call from timer callback or main loop.
   * First call initializes baseline (CPU delta requires two samples).
   *
   * @return Current SystemSnapshot.
   */
  SystemSnapshot Sample() noexcept {
    last_snapshot_.cpu = ReadCpu();
    last_snapshot_.memory = ReadMemory();
    last_snapshot_.timestamp_us = SteadyNowUs();

    // Sample all disk paths
    for (uint32_t i = 0; i < disk_count_; ++i) {
      if (disk_slots_[i].active) {
        disk_slots_[i].snapshot = ReadDisk(disk_slots_[i].path);
      }
    }

    CheckAlerts(last_snapshot_);
    return last_snapshot_;
  }

  // -- Individual readers (can be called standalone) --

  /**
   * @brief Read current CPU utilization.
   *
   * Requires two calls with interval for delta calculation. First call
   * returns zeros (no baseline yet). Temperature is always read.
   *
   * @return CpuSnapshot with current metrics.
   */
  CpuSnapshot ReadCpu() noexcept {
    CpuSnapshot snap{};
    snap.temperature_mc = ReadCpuTemperature();

    CpuJiffies curr{};
    if (!ReadCpuJiffies(curr)) {
      return snap;  // Parse failed, return zeros
    }

    if (first_sample_) {
      prev_jiffies_ = curr;
      first_sample_ = false;
      return snap;  // No delta yet
    }

    // Calculate delta
    const uint64_t total_delta = curr.total - prev_jiffies_.total;
    const uint64_t busy_delta = curr.busy - prev_jiffies_.busy;
    const uint64_t user_delta = curr.user - prev_jiffies_.user;
    const uint64_t system_delta = curr.system - prev_jiffies_.system;
    const uint64_t iowait_delta = curr.iowait - prev_jiffies_.iowait;

    if (OSP_LIKELY(total_delta > 0)) {
      snap.total_percent = static_cast<uint32_t>((busy_delta * 100ULL) / total_delta);
      snap.user_percent = static_cast<uint32_t>((user_delta * 100ULL) / total_delta);
      snap.system_percent = static_cast<uint32_t>((system_delta * 100ULL) / total_delta);
      snap.iowait_percent = static_cast<uint32_t>((iowait_delta * 100ULL) / total_delta);

      // Clamp to [0, 100]
      snap.total_percent = (snap.total_percent > 100) ? 100 : snap.total_percent;
      snap.user_percent = (snap.user_percent > 100) ? 100 : snap.user_percent;
      snap.system_percent = (snap.system_percent > 100) ? 100 : snap.system_percent;
      snap.iowait_percent = (snap.iowait_percent > 100) ? 100 : snap.iowait_percent;
    }

    prev_jiffies_ = curr;

    return snap;
  }

  /**
   * @brief Read current memory usage.
   *
   * Parses /proc/meminfo for MemTotal and MemAvailable.
   *
   * @return MemorySnapshot with current metrics.
   */
  static MemorySnapshot ReadMemory() noexcept {
    MemorySnapshot snap{};

    const int fd = ::open("/proc/meminfo", O_RDONLY);
    if (fd < 0) {
      return snap;
    }

    char buf[512];
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);

    if (n <= 0) {
      return snap;
    }
    buf[n] = '\0';

    // Parse line by line
    bool found_total = false;
    bool found_available = false;
    const char* line = buf;
    while (*line != '\0' && (!found_total || !found_available)) {
      if (std::strncmp(line, "MemTotal:", 9) == 0) {
        std::sscanf(line + 9, " %" SCNu64, &snap.total_kb);
        found_total = true;
      } else if (std::strncmp(line, "MemAvailable:", 13) == 0) {
        std::sscanf(line + 13, " %" SCNu64, &snap.available_kb);
        found_available = true;
      }
      // Move to next line
      while (*line != '\0' && *line != '\n') {
        ++line;
      }
      if (*line == '\n') {
        ++line;
      }
    }

    if (found_total && found_available && snap.total_kb > 0) {
      snap.used_kb = snap.total_kb - snap.available_kb;
      snap.used_percent = static_cast<uint32_t>((snap.used_kb * 100ULL) / snap.total_kb);
      snap.used_percent = (snap.used_percent > 100) ? 100 : snap.used_percent;
    }

    return snap;
  }

  /**
   * @brief Read disk usage for a specific path.
   *
   * Uses statvfs() syscall.
   *
   * @param path  Mount point path.
   * @return DiskSnapshot with current metrics.
   */
  static DiskSnapshot ReadDisk(const char* path) noexcept {
    DiskSnapshot snap{};

    if (path == nullptr) {
      return snap;
    }

    struct statvfs st;
    if (::statvfs(path, &st) != 0) {
      return snap;
    }

    snap.total_bytes = st.f_blocks * st.f_frsize;
    snap.available_bytes = st.f_bavail * st.f_frsize;

    if (snap.total_bytes > 0) {
      const uint64_t used_bytes = snap.total_bytes - snap.available_bytes;
      snap.used_percent = static_cast<uint32_t>((used_bytes * 100ULL) / snap.total_bytes);
      snap.used_percent = (snap.used_percent > 100) ? 100 : snap.used_percent;
    }

    return snap;
  }

  /**
   * @brief Read CPU temperature in milli-Celsius.
   *
   * Reads /sys/class/thermal/thermal_zone0/temp.
   *
   * @return Temperature in milli-Celsius (e.g., 45000 = 45.0°C), or -1 if N/A.
   */
  static int32_t ReadCpuTemperature() noexcept {
    const int fd = ::open("/sys/class/thermal/thermal_zone0/temp", O_RDONLY);
    if (fd < 0) {
      return -1;
    }

    char buf[32];
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);

    if (n <= 0) {
      return -1;
    }
    buf[n] = '\0';

    int32_t temp_mc = 0;
    if (std::sscanf(buf, "%d", &temp_mc) == 1) {
      return temp_mc;
    }
    return -1;
  }

  // -- Query --

  /**
   * @brief Get last sampled snapshot (no I/O).
   *
   * @return Reference to last SystemSnapshot.
   */
  const SystemSnapshot& LastSnapshot() const noexcept { return last_snapshot_; }

  /**
   * @brief Get disk snapshot by index.
   *
   * @param index  Disk slot index [0, DiskPathCount()).
   * @return Reference to DiskSnapshot (zeros if index out of range).
   */
  const DiskSnapshot& GetDiskSnapshot(uint32_t index) const noexcept {
    static const DiskSnapshot kZero{};
    if (index >= disk_count_) {
      return kZero;
    }
    return disk_slots_[index].snapshot;
  }

  /**
   * @brief Number of monitored disk paths.
   *
   * @return Active disk path count.
   */
  uint32_t DiskPathCount() const noexcept { return disk_count_; }

  // -- Static callback for TimerScheduler integration --

  /**
   * @brief Static callback for timer integration.
   *
   * Usage:
   *   scheduler.AddTimer(1000, SystemMonitor<4>::SampleTick, &monitor);
   *
   * @param ctx  Pointer to SystemMonitor instance.
   */
  static void SampleTick(void* ctx) noexcept {
    if (ctx != nullptr) {
      static_cast<SystemMonitor*>(ctx)->Sample();
    }
  }

 private:
  // CPU jiffies for delta calculation
  struct CpuJiffies {
    uint64_t user = 0;
    uint64_t nice = 0;
    uint64_t system = 0;
    uint64_t idle = 0;
    uint64_t iowait = 0;
    uint64_t irq = 0;
    uint64_t softirq = 0;
    uint64_t steal = 0;
    uint64_t total = 0;
    uint64_t busy = 0;
  };

  // Disk monitoring slot
  struct DiskSlot {
    char path[64] = {};
    DiskSnapshot snapshot = {};
    uint32_t prev_used_percent = 0;
    bool active = false;
  };

  // Internal helpers

  /**
   * @brief Read CPU jiffies from /proc/stat.
   *
   * Parses first line: "cpu user nice system idle iowait irq softirq steal".
   *
   * @param out  Output CpuJiffies structure.
   * @return true if parse succeeded, false otherwise.
   */
  bool ReadCpuJiffies(CpuJiffies& out) noexcept {
    const int fd = ::open("/proc/stat", O_RDONLY);
    if (fd < 0) {
      return false;
    }

    char buf[256];
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);

    if (n <= 0) {
      return false;
    }
    buf[n] = '\0';

    // Parse first line: "cpu user nice system idle iowait irq softirq steal"
    const int parsed = std::sscanf(
        buf, "cpu %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
        &out.user, &out.nice, &out.system, &out.idle, &out.iowait, &out.irq, &out.softirq, &out.steal);

    if (parsed < 4) {
      return false;  // At least user, nice, system, idle required
    }

    out.total = out.user + out.nice + out.system + out.idle + out.iowait + out.irq + out.softirq + out.steal;
    out.busy = out.total - out.idle - out.iowait;

    return true;
  }

  /**
   * @brief Check thresholds and fire alerts on state change.
   *
   * Uses state-change pattern: only fire when crossing threshold, not every
   * sample. Logs warnings and invokes callback.
   *
   * @param snap  Current system snapshot.
   */
  void CheckAlerts(const SystemSnapshot& snap) noexcept {
    char msg[128];

    // CPU usage alert
    if (StateCrossed(prev_cpu_percent_, snap.cpu.total_percent, thresholds_.cpu_percent)) {
      const bool exceeded = snap.cpu.total_percent >= thresholds_.cpu_percent;
      std::snprintf(msg, sizeof(msg), "CPU usage %s threshold: %u%% (limit %u%%)", exceeded ? "exceeded" : "below",
                    snap.cpu.total_percent, thresholds_.cpu_percent);
      OSP_LOG_WARN("SysMon", "%s", msg);
      if (alert_fn_ != nullptr) {
        alert_fn_(snap, msg, alert_ctx_);
      }
      if (exceeded) {
        ReportFault(fault_idx_.cpu_overload, snap.cpu.total_percent,
                    2U);  // kMedium
      }
    }
    prev_cpu_percent_ = snap.cpu.total_percent;

    // CPU temperature alert
    if (snap.cpu.temperature_mc >= 0) {
      if (StateCrossed(prev_cpu_temp_mc_, snap.cpu.temperature_mc, thresholds_.cpu_temp_mc)) {
        const bool exceeded = snap.cpu.temperature_mc >= thresholds_.cpu_temp_mc;
        std::snprintf(msg, sizeof(msg), "CPU temperature %s threshold: %d.%d C (limit %d.%d C)",
                      exceeded ? "exceeded" : "below", snap.cpu.temperature_mc / 1000,
                      (snap.cpu.temperature_mc % 1000) / 100, thresholds_.cpu_temp_mc / 1000,
                      (thresholds_.cpu_temp_mc % 1000) / 100);
        OSP_LOG_WARN("SysMon", "%s", msg);
        if (alert_fn_ != nullptr) {
          alert_fn_(snap, msg, alert_ctx_);
        }
        if (exceeded) {
          ReportFault(fault_idx_.cpu_overheat, static_cast<uint32_t>(snap.cpu.temperature_mc),
                      1U);  // kHigh
        }
      }
      prev_cpu_temp_mc_ = snap.cpu.temperature_mc;
    }

    // Memory usage alert
    if (StateCrossed(prev_mem_percent_, snap.memory.used_percent, thresholds_.memory_percent)) {
      const bool exceeded = snap.memory.used_percent >= thresholds_.memory_percent;
      std::snprintf(msg, sizeof(msg), "Memory usage %s threshold: %u%% (limit %u%%)", exceeded ? "exceeded" : "below",
                    snap.memory.used_percent, thresholds_.memory_percent);
      OSP_LOG_WARN("SysMon", "%s", msg);
      if (alert_fn_ != nullptr) {
        alert_fn_(snap, msg, alert_ctx_);
      }
      if (exceeded) {
        ReportFault(fault_idx_.memory_overload, snap.memory.used_percent,
                    1U);  // kHigh
      }
    }
    prev_mem_percent_ = snap.memory.used_percent;

    // Disk usage alerts
    for (uint32_t i = 0; i < disk_count_; ++i) {
      auto& slot = disk_slots_[i];
      if (!slot.active) {
        continue;
      }
      if (StateCrossed(slot.prev_used_percent, slot.snapshot.used_percent, thresholds_.disk_percent)) {
        const bool exceeded = slot.snapshot.used_percent >= thresholds_.disk_percent;
        std::snprintf(msg, sizeof(msg), "Disk usage %s threshold on %s: %u%% (limit %u%%)",
                      exceeded ? "exceeded" : "below", slot.path, slot.snapshot.used_percent, thresholds_.disk_percent);
        OSP_LOG_WARN("SysMon", "%s", msg);
        if (alert_fn_ != nullptr) {
          alert_fn_(snap, msg, alert_ctx_);
        }
        if (exceeded) {
          ReportFault(fault_idx_.disk_full, (i << 16U) | slot.snapshot.used_percent,
                      1U);  // kHigh, detail encodes disk index + percent
        }
      }
      slot.prev_used_percent = slot.snapshot.used_percent;
    }
  }

  /// Report a fault via the injected FaultReporter (no-op if not wired).
  void ReportFault(uint16_t fault_index, uint32_t detail, uint8_t priority) noexcept {
    if (fault_fn_ != nullptr) {
      fault_fn_(fault_index, detail, priority, fault_ctx_);
    }
  }

  /**
   * @brief Check if a value crossed a threshold (state-change pattern).
   *
   * Returns true if:
   * - prev < threshold AND curr >= threshold (crossing up), OR
   * - prev >= threshold AND curr < threshold (crossing down)
   *
   * @param prev       Previous value.
   * @param curr       Current value.
   * @param threshold  Threshold value.
   * @return true if state changed, false otherwise.
   */
  static constexpr bool StateCrossed(uint32_t prev, uint32_t curr, uint32_t threshold) noexcept {
    return ((prev < threshold && curr >= threshold) || (prev >= threshold && curr < threshold));
  }

  /**
   * @brief Check if a value crossed a threshold (int32_t overload).
   */
  static constexpr bool StateCrossed(int32_t prev, int32_t curr, int32_t threshold) noexcept {
    return ((prev < threshold && curr >= threshold) || (prev >= threshold && curr < threshold));
  }

  // State
  AlertThresholds thresholds_{};
  AlertCallback alert_fn_{nullptr};
  void* alert_ctx_{nullptr};
  FaultReportFn fault_fn_{nullptr};
  void* fault_ctx_{nullptr};
  SystemMonitorFaultIndex fault_idx_{};
  SystemSnapshot last_snapshot_{};
  CpuJiffies prev_jiffies_{};
  bool first_sample_{true};
  DiskSlot disk_slots_[MaxDiskPaths]{};
  uint32_t disk_count_{0};

  // Previous values for state-change detection
  uint32_t prev_cpu_percent_{0};
  int32_t prev_cpu_temp_mc_{-1};
  uint32_t prev_mem_percent_{0};
};

}  // namespace osp

#endif  // defined(OSP_PLATFORM_LINUX)

#endif  // OSP_SYSTEM_MONITOR_HPP_
