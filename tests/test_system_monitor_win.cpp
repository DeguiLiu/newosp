/**
 * @file test_system_monitor_win.cpp
 * @brief Catch2 tests for osp::system_monitor.hpp on the Windows branch
 *        (GlobalMemoryStatusEx / GetSystemTimes / GetDiskFreeSpaceExW).
 *
 * CPU temperature is documented unavailable (-1) on Windows. Guarded by
 * OSP_PLATFORM_WINDOWS.
 */

#include "osp/system_monitor.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

#if defined(OSP_PLATFORM_WINDOWS)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

/// @brief Drive root of the current working directory, e.g. "C:\\".
bool CurrentDriveRoot(char (&root)[4]) {
  char cwd[MAX_PATH];
  const DWORD n = ::GetCurrentDirectoryA(MAX_PATH, cwd);
  if (n == 0) {
    return false;
  }
  root[0] = cwd[0];
  root[1] = ':';
  root[2] = '\\';
  root[3] = '\0';
  return true;
}

}  // namespace

// ============================================================================
// Memory
// ============================================================================

TEST_CASE("system_monitor_win - ReadMemory returns plausible values", "[system_monitor][windows]") {
  const auto mem = osp::SystemMonitor<4>::ReadMemory();
  REQUIRE(mem.total_kb > 0);
  REQUIRE(mem.available_kb > 0);
  REQUIRE(mem.used_kb > 0);
  REQUIRE(mem.used_percent <= 100);
}

// ============================================================================
// Disk
// ============================================================================

TEST_CASE("system_monitor_win - ReadDisk on current drive", "[system_monitor][windows]") {
  char root[4];
  REQUIRE(CurrentDriveRoot(root));

  const auto disk = osp::SystemMonitor<4>::ReadDisk(root);
  REQUIRE(disk.total_bytes > 0);
  REQUIRE(disk.available_bytes > 0);
  REQUIRE(disk.used_percent <= 100);
}

TEST_CASE("system_monitor_win - ReadDisk null path returns zeros", "[system_monitor][windows]") {
  const auto disk = osp::SystemMonitor<4>::ReadDisk(nullptr);
  REQUIRE(disk.total_bytes == 0);
  REQUIRE(disk.used_percent == 0);
}

// ============================================================================
// CPU
// ============================================================================

TEST_CASE("system_monitor_win - ReadCpuTemperature is -1 on Windows", "[system_monitor][windows]") {
  REQUIRE(osp::SystemMonitor<4>::ReadCpuTemperature() == -1);
}

TEST_CASE("system_monitor_win - ReadCpu two-sample delta", "[system_monitor][windows]") {
  osp::SystemMonitor<4> mon;

  // First sample establishes the baseline and reports zero percent.
  const auto c1 = mon.ReadCpu();
  REQUIRE(c1.total_percent == 0);

  osp::ThreadSleepUs(static_cast<uint64_t>(20) * 1000u);

  // Second sample computes a delta; values must be within [0, 100].
  const auto c2 = mon.ReadCpu();
  REQUIRE(c2.total_percent <= 100);
  REQUIRE(c2.user_percent <= 100);
  REQUIRE(c2.system_percent <= 100);
}

// ============================================================================
// Sample / aggregate
// ============================================================================

TEST_CASE("system_monitor_win - Sample runs and fills memory", "[system_monitor][windows]") {
  osp::SystemMonitor<4> mon;
  REQUIRE(mon.AddDiskPath("C:\\"));
  REQUIRE(mon.DiskPathCount() == 1);

  const auto snap = mon.Sample();
  REQUIRE(snap.memory.total_kb > 0);
  REQUIRE(snap.timestamp_us > 0);
}

#else  // OSP_PLATFORM_WINDOWS

TEST_CASE("system_monitor_win - Windows system monitor tests are Windows-only",
          "[system_monitor][windows]") {
  SKIP("system_monitor_win tests only run on Windows");
}

#endif  // OSP_PLATFORM_WINDOWS
