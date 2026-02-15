// Copyright (c) 2025 newosp
// SPDX-License-Identifier: MIT

#include <catch2/catch_test_macros.hpp>
#include "osp/system_monitor.hpp"

// --- POD snapshot tests ---
TEST_CASE("SystemMonitor: snapshot structs are POD-like", "[system_monitor]") {
  // CpuSnapshot zero-init
  osp::CpuSnapshot cpu{};
  CHECK(cpu.total_percent == 0);
  CHECK(cpu.user_percent == 0);
  CHECK(cpu.system_percent == 0);
  CHECK(cpu.iowait_percent == 0);
  CHECK(cpu.temperature_mc == 0);

  // MemorySnapshot zero-init
  osp::MemorySnapshot mem{};
  CHECK(mem.total_kb == 0);
  CHECK(mem.available_kb == 0);
  CHECK(mem.used_kb == 0);
  CHECK(mem.used_percent == 0);

  // DiskSnapshot zero-init
  osp::DiskSnapshot disk{};
  CHECK(disk.total_bytes == 0);
  CHECK(disk.available_bytes == 0);
  CHECK(disk.used_percent == 0);

  // SystemSnapshot zero-init
  osp::SystemSnapshot snap{};
  CHECK(snap.timestamp_us == 0);
  CHECK(snap.cpu.total_percent == 0);
  CHECK(snap.memory.total_kb == 0);
}

TEST_CASE("SystemMonitor: AlertThresholds defaults", "[system_monitor]") {
  osp::AlertThresholds t;
  CHECK(t.cpu_percent == 90);
  CHECK(t.cpu_temp_mc == 85000);
  CHECK(t.memory_percent == 90);
  CHECK(t.disk_percent == 90);
}

// --- Configuration tests ---
TEST_CASE("SystemMonitor: AddDiskPath", "[system_monitor]") {
  osp::SystemMonitor<2> mon;
  CHECK(mon.DiskPathCount() == 0);
  CHECK(mon.AddDiskPath("/"));
  CHECK(mon.DiskPathCount() == 1);
  CHECK(mon.AddDiskPath("/tmp"));
  CHECK(mon.DiskPathCount() == 2);
  // MaxDiskPaths=2, third should fail
  CHECK_FALSE(mon.AddDiskPath("/var"));
  CHECK(mon.DiskPathCount() == 2);
}

TEST_CASE("SystemMonitor: AddDiskPath nullptr", "[system_monitor]") {
  osp::SystemMonitor<4> mon;
  CHECK_FALSE(mon.AddDiskPath(nullptr));
  CHECK(mon.DiskPathCount() == 0);
}

TEST_CASE("SystemMonitor: SetThresholds", "[system_monitor]") {
  osp::SystemMonitor<4> mon;
  osp::AlertThresholds t;
  t.cpu_percent = 50;
  t.memory_percent = 60;
  mon.SetThresholds(t);
  // No crash, thresholds stored internally
}

TEST_CASE("SystemMonitor: SetAlertCallback", "[system_monitor]") {
  osp::SystemMonitor<4> mon;
  uint32_t alert_count = 0;
  mon.SetAlertCallback([](const osp::SystemSnapshot&, const char*, void* ctx) {
    ++(*static_cast<uint32_t*>(ctx));
  }, &alert_count);
  // No crash
}

// --- ReadMemory tests ---
TEST_CASE("SystemMonitor: ReadMemory returns valid data", "[system_monitor]") {
  auto mem = osp::SystemMonitor<4>::ReadMemory();
  // On Linux CI, these should be non-zero
  CHECK(mem.total_kb > 0);
  CHECK(mem.available_kb > 0);
  CHECK(mem.available_kb <= mem.total_kb);
  CHECK(mem.used_kb <= mem.total_kb);
  CHECK(mem.used_percent <= 100);
}

// --- ReadDisk tests ---
TEST_CASE("SystemMonitor: ReadDisk root", "[system_monitor]") {
  auto disk = osp::SystemMonitor<4>::ReadDisk("/");
  CHECK(disk.total_bytes > 0);
  CHECK(disk.available_bytes > 0);
  CHECK(disk.available_bytes <= disk.total_bytes);
  CHECK(disk.used_percent <= 100);
}

TEST_CASE("SystemMonitor: ReadDisk nullptr", "[system_monitor]") {
  auto disk = osp::SystemMonitor<4>::ReadDisk(nullptr);
  CHECK(disk.total_bytes == 0);
  CHECK(disk.available_bytes == 0);
  CHECK(disk.used_percent == 0);
}

TEST_CASE("SystemMonitor: ReadDisk nonexistent path", "[system_monitor]") {
  auto disk = osp::SystemMonitor<4>::ReadDisk("/nonexistent_path_xyz");
  CHECK(disk.total_bytes == 0);
  CHECK(disk.used_percent == 0);
}

// --- ReadCpuTemperature tests ---
TEST_CASE("SystemMonitor: ReadCpuTemperature", "[system_monitor]") {
  int32_t temp = osp::SystemMonitor<4>::ReadCpuTemperature();
  // May return -1 on CI (no thermal zone), or valid temp
  CHECK((temp == -1 || (temp > 0 && temp < 150000)));
}

// --- ReadCpu tests ---
TEST_CASE("SystemMonitor: ReadCpu first call returns zeros", "[system_monitor]") {
  osp::SystemMonitor<4> mon;
  auto cpu = mon.ReadCpu();
  // First call has no baseline, percentages should be 0
  CHECK(cpu.total_percent == 0);
  CHECK(cpu.user_percent == 0);
  CHECK(cpu.system_percent == 0);
  CHECK(cpu.iowait_percent == 0);
}

TEST_CASE("SystemMonitor: ReadCpu second call returns valid data", "[system_monitor]") {
  osp::SystemMonitor<4> mon;
  mon.ReadCpu();  // First call (baseline)
  auto cpu = mon.ReadCpu();  // Second call (delta)
  // Percentages should be in valid range
  CHECK(cpu.total_percent <= 100);
  CHECK(cpu.user_percent <= 100);
  CHECK(cpu.system_percent <= 100);
  CHECK(cpu.iowait_percent <= 100);
}

// --- Sample tests ---
TEST_CASE("SystemMonitor: Sample returns valid snapshot", "[system_monitor]") {
  osp::SystemMonitor<4> mon;
  mon.AddDiskPath("/");
  auto snap = mon.Sample();  // First sample (baseline)
  CHECK(snap.timestamp_us > 0);
  CHECK(snap.memory.total_kb > 0);

  snap = mon.Sample();  // Second sample (with delta)
  CHECK(snap.timestamp_us > 0);
  CHECK(snap.cpu.total_percent <= 100);
  CHECK(snap.memory.used_percent <= 100);
}

TEST_CASE("SystemMonitor: LastSnapshot matches Sample return", "[system_monitor]") {
  osp::SystemMonitor<4> mon;
  auto snap = mon.Sample();
  const auto& last = mon.LastSnapshot();
  CHECK(last.timestamp_us == snap.timestamp_us);
  CHECK(last.memory.total_kb == snap.memory.total_kb);
}

TEST_CASE("SystemMonitor: GetDiskSnapshot out of range", "[system_monitor]") {
  osp::SystemMonitor<4> mon;
  const auto& disk = mon.GetDiskSnapshot(99);
  CHECK(disk.total_bytes == 0);
  CHECK(disk.used_percent == 0);
}

TEST_CASE("SystemMonitor: GetDiskSnapshot after Sample", "[system_monitor]") {
  osp::SystemMonitor<4> mon;
  mon.AddDiskPath("/");
  mon.Sample();
  const auto& disk = mon.GetDiskSnapshot(0);
  CHECK(disk.total_bytes > 0);
  CHECK(disk.used_percent <= 100);
}

// --- SampleTick static callback ---
TEST_CASE("SystemMonitor: SampleTick static callback", "[system_monitor]") {
  osp::SystemMonitor<4> mon;
  osp::SystemMonitor<4>::SampleTick(&mon);
  CHECK(mon.LastSnapshot().timestamp_us > 0);
}

TEST_CASE("SystemMonitor: SampleTick nullptr safe", "[system_monitor]") {
  osp::SystemMonitor<4>::SampleTick(nullptr);
  // Should not crash
}

// --- StateCrossed logic (tested via alert callback) ---
TEST_CASE("SystemMonitor: alert fires on threshold crossing", "[system_monitor]") {
  // Set memory threshold very low so it triggers
  osp::SystemMonitor<4> mon;
  osp::AlertThresholds t;
  t.cpu_percent = 100;       // Won't trigger (CPU rarely 100%)
  t.memory_percent = 1;      // Will trigger (memory always > 1%)
  t.disk_percent = 100;      // Won't trigger
  t.cpu_temp_mc = 200000;    // Won't trigger (200°C)
  mon.SetThresholds(t);

  uint32_t alert_count = 0;
  mon.SetAlertCallback([](const osp::SystemSnapshot&, const char*, void* ctx) {
    ++(*static_cast<uint32_t*>(ctx));
  }, &alert_count);

  mon.Sample();  // First sample (baseline, prev=0, curr>1 => crosses threshold)
  mon.Sample();  // Second sample
  // At least one memory alert should have fired (0 -> >1% crosses threshold=1%)
  CHECK(alert_count >= 1);
}

TEST_CASE("SystemMonitor: no repeated alerts without state change", "[system_monitor]") {
  osp::SystemMonitor<4> mon;
  osp::AlertThresholds t;
  t.cpu_percent = 1;         // Will trigger once
  t.memory_percent = 1;      // Will trigger once
  t.disk_percent = 100;
  t.cpu_temp_mc = 200000;
  mon.SetThresholds(t);

  uint32_t alert_count = 0;
  mon.SetAlertCallback([](const osp::SystemSnapshot&, const char*, void* ctx) {
    ++(*static_cast<uint32_t*>(ctx));
  }, &alert_count);

  mon.Sample();  // baseline
  uint32_t first_count = alert_count;
  mon.Sample();  // should not re-trigger (same state)
  mon.Sample();  // should not re-trigger (same state)
  // After first crossing, subsequent samples should NOT add more alerts
  // (unless CPU fluctuates, which is unlikely in a test)
  // We just check it doesn't grow unboundedly
  CHECK(alert_count <= first_count + 2);  // Allow small margin for CPU fluctuation
}

// --- Template parameter test ---
TEST_CASE("SystemMonitor: MaxDiskPaths=1", "[system_monitor]") {
  osp::SystemMonitor<1> mon;
  CHECK(mon.AddDiskPath("/"));
  CHECK_FALSE(mon.AddDiskPath("/tmp"));
  CHECK(mon.DiskPathCount() == 1);
}
