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
 * @file shell_commands.hpp
 * @brief Built-in diagnostic shell commands for newosp modules.
 *
 * Zero-intrusion bridge: each module's runtime state is exposed as a telnet
 * shell command via template Register functions.  Modules themselves do NOT
 * depend on shell.hpp; only this file does.
 *
 * Usage:
 * @code
 *   #include "osp/shell_commands.hpp"
 *   osp::shell_cmd::RegisterWatchdog(watchdog);
 *   osp::shell_cmd::RegisterFaults(collector);
 * @endcode
 *
 * Compatible with -fno-exceptions -fno-rtti, C++17.
 */

#ifndef OSP_SHELL_COMMANDS_HPP_
#define OSP_SHELL_COMMANDS_HPP_

#include "osp/shell.hpp"

// Module headers for type access in diagnostic lambdas
#include "osp/bus.hpp"
#include "osp/config.hpp"
#include "osp/fault_collector.hpp"
#include "osp/lifecycle_node.hpp"
#include "osp/log.hpp"
#include "osp/node_manager_hsm.hpp"
#include "osp/watchdog.hpp"

#include <cinttypes>
#include <cstdio>

namespace osp {
namespace shell_cmd {

// ============================================================================
// Helpers
// ============================================================================

namespace detail {

inline const char* BackpressureName(BackpressureLevel lvl) noexcept {
  switch (lvl) {
    case BackpressureLevel::kNormal:
      return "Normal";
    case BackpressureLevel::kWarning:
      return "Warning";
    case BackpressureLevel::kCritical:
      return "Critical";
    case BackpressureLevel::kFull:
      return "Full";
    default:
      return "Unknown";
  }
}

inline const char* FaultPriorityName(uint32_t pri) noexcept {
  static const char* names[] = {"Critical", "High", "Medium", "Low"};
  return (pri < 4U) ? names[pri] : "Unknown";
}

}  // namespace detail

// ============================================================================
// Reliability Layer
// ============================================================================

/// Register osp_watchdog command.
template <typename WatchdogType>
inline void RegisterWatchdog(WatchdogType& wd) {
  static WatchdogType* s_wd = &wd;
  static auto cmd = [](int /*argc*/, char* /*argv*/[]) -> int {
    uint32_t active = s_wd->ActiveCount();
    uint32_t timed_out = s_wd->TimedOutCount();
    uint32_t cap = WatchdogType::Capacity();
    ShellPrintf("[osp_watchdog] ThreadWatchdog (%" PRIu32 "/%" PRIu32 " active, %" PRIu32 " timed out)\r\n", active,
                cap, timed_out);
    s_wd->ForEachSlot([](const WatchdogSlotInfo& info) {
      uint64_t now = SteadyNowUs();
      uint64_t ago = (now > info.last_beat_us) ? (now - info.last_beat_us) : 0U;
      ShellPrintf("  [%" PRIu32 "] %-20s timeout=%" PRIu64 "ms  last_beat=%" PRIu64 "ms_ago  %s\r\n", info.slot_id,
                  info.name, info.timeout_us / 1000U, ago / 1000U, info.timed_out ? "TIMEOUT" : "OK");
    });
    return 0;
  };
  (void)osp::detail::GlobalCmdRegistry::Instance().Register("osp_watchdog", +cmd, "Show thread watchdog status");
}

/// Register osp_faults command.
template <typename FaultCollectorType>
inline void RegisterFaults(FaultCollectorType& fc) {
  static FaultCollectorType* s_fc = &fc;
  static auto cmd = [](int /*argc*/, char* /*argv*/[]) -> int {
    auto stats = s_fc->GetStatistics();
    ShellPrintf("[osp_faults] FaultCollector Statistics\r\n");
    ShellPrintf("  total_reported:  %" PRIu64 "\r\n", stats.total_reported);
    ShellPrintf("  total_processed: %" PRIu64 "\r\n", stats.total_processed);
    ShellPrintf("  total_dropped:   %" PRIu64 "\r\n", stats.total_dropped);
    for (uint32_t i = 0U; i < 4U; ++i) {
      ShellPrintf("  %-10s reported=%" PRIu64 "  dropped=%" PRIu64 "\r\n", detail::FaultPriorityName(i),
                  stats.priority_reported[i], stats.priority_dropped[i]);
    }
    // Queue usage
    ShellPrintf("  queue_usage:");
    for (uint32_t i = 0U; i < 4U; ++i) {
      auto usage = s_fc->QueueUsage(static_cast<FaultPriority>(i));
      ShellPrintf(" %s=%" PRIu32 "/%" PRIu32, detail::FaultPriorityName(i), usage.size, usage.capacity);
    }
    ShellPrintf("\r\n");
    // Recent faults
    ShellPrintf("  recent faults:\r\n");
    uint32_t shown = 0U;
    s_fc->ForEachRecent(
        [&shown](const RecentFaultInfo& info) {
          ShellPrintf("    [%" PRIu32 "] fault=%" PRIu16 " detail=%" PRIu32 " pri=%s ts=%" PRIu64 "us\r\n", shown,
                      info.fault_index, info.detail, detail::FaultPriorityName(static_cast<uint32_t>(info.priority)),
                      info.timestamp_us);
          ++shown;
        },
        8U);
    if (shown == 0U) {
      ShellPrintf("    (none)\r\n");
    }
    return 0;
  };
  (void)osp::detail::GlobalCmdRegistry::Instance().Register("osp_faults", +cmd, "Show fault collector statistics");
}

// ============================================================================
// Communication Layer (optional)
// ============================================================================

/// Register osp_bus command (status + reset).
template <typename BusType>
inline void RegisterBusStats(BusType& bus) {
  static BusType* s_bus = &bus;

  static auto show_status = [](int /*argc*/, char* /*argv*/[]) -> int {
    auto stats = s_bus->GetStatistics();
    auto bp = s_bus->GetBackpressureLevel();
    ShellPrintf("[osp_bus] AsyncBus Statistics\r\n");
    ShellPrintf("  published:     %" PRIu64 "\r\n", stats.messages_published);
    ShellPrintf("  processed:     %" PRIu64 "\r\n", stats.messages_processed);
    ShellPrintf("  dropped:       %" PRIu64 "\r\n", stats.messages_dropped);
    ShellPrintf("  rechecks:      %" PRIu64 "\r\n", stats.admission_rechecks);
    ShellPrintf("  backpressure:  %s\r\n", detail::BackpressureName(bp));
    return 0;
  };

  static auto sub_status = [](int argc, char* argv[]) -> int { return (+show_status)(argc, argv); };

  static auto sub_reset = [](int /*argc*/, char* /*argv*/[]) -> int {
    s_bus->ResetStatistics();
    ShellPrintf("[osp_bus] Statistics reset.\r\n");
    return 0;
  };

  static const ShellSubCmd kSubs[] = {
      {"status", nullptr, "Show bus statistics", +sub_status},
      {"reset", nullptr, "Reset all counters", +sub_reset},
  };

  static auto cmd = [](int argc, char* argv[]) -> int { return ShellDispatch(argc, argv, kSubs, 2U, +show_status); };

  (void)osp::detail::GlobalCmdRegistry::Instance().Register("osp_bus", +cmd, "AsyncBus statistics and control");
}

/// Register osp_pool command.
template <typename PoolType>
inline void RegisterWorkerPool(PoolType& pool) {
  static PoolType* s_pool = &pool;
  static auto cmd = [](int /*argc*/, char* /*argv*/[]) -> int {
    auto stats = s_pool->GetStats();
    ShellPrintf("[osp_pool] WorkerPool Statistics\r\n");
    ShellPrintf("  dispatched:      %" PRIu64 "\r\n", stats.dispatched);
    ShellPrintf("  processed:       %" PRIu64 "\r\n", stats.processed);
    ShellPrintf("  queue_full:      %" PRIu64 "\r\n", stats.worker_queue_full);
    ShellPrintf("  bus_published:   %" PRIu64 "\r\n", stats.bus_stats.messages_published);
    ShellPrintf("  bus_dropped:     %" PRIu64 "\r\n", stats.bus_stats.messages_dropped);
    return 0;
  };
  (void)osp::detail::GlobalCmdRegistry::Instance().Register("osp_pool", +cmd, "Show WorkerPool statistics");
}

// ============================================================================
// Network Transport Layer
// ============================================================================

/// Register osp_transport command.
template <typename TrackerType>
inline void RegisterTransport(TrackerType& tracker) {
  static TrackerType* s_tracker = &tracker;
  static auto cmd = [](int /*argc*/, char* /*argv*/[]) -> int {
    uint64_t total = s_tracker->TotalReceived();
    uint64_t lost = s_tracker->LostCount();
    uint64_t reorder = s_tracker->ReorderedCount();
    uint64_t dup = s_tracker->DuplicateCount();
    ShellPrintf("[osp_transport] SequenceTracker\r\n");
    ShellPrintf("  total_received:  %" PRIu64 "\r\n", total);
    ShellPrintf("  lost:            %" PRIu64 "\r\n", lost);
    ShellPrintf("  reordered:       %" PRIu64 "\r\n", reorder);
    ShellPrintf("  duplicates:      %" PRIu64 "\r\n", dup);
    if (total > 0U) {
      uint64_t loss_pct = (lost * 10000U) / total;
      ShellPrintf("  loss_rate:       %" PRIu64 ".%02" PRIu64 "%%\r\n", loss_pct / 100U, loss_pct % 100U);
    }
    return 0;
  };
  (void)osp::detail::GlobalCmdRegistry::Instance().Register("osp_transport", +cmd, "Show transport sequence tracker");
}

/// Register osp_serial command.
template <typename SerialType>
inline void RegisterSerial(SerialType& serial) {
  static SerialType* s_serial = &serial;
  static auto cmd = [](int /*argc*/, char* /*argv*/[]) -> int {
    auto s = s_serial->GetStatistics();
    ShellPrintf("[osp_serial] SerialTransport Statistics\r\n");
    ShellPrintf("  frames_sent:      %" PRIu64 "\r\n", s.frames_sent);
    ShellPrintf("  frames_received:  %" PRIu64 "\r\n", s.frames_received);
    ShellPrintf("  bytes_sent:       %" PRIu64 "\r\n", s.bytes_sent);
    ShellPrintf("  bytes_received:   %" PRIu64 "\r\n", s.bytes_received);
    ShellPrintf("  crc_errors:       %" PRIu64 "\r\n", s.crc_errors);
    ShellPrintf("  sync_errors:      %" PRIu64 "\r\n", s.sync_errors);
    ShellPrintf("  timeout_errors:   %" PRIu64 "\r\n", s.timeout_errors);
    ShellPrintf("  seq_gaps:         %" PRIu64 "\r\n", s.seq_gaps);
    ShellPrintf("  retransmits:      %" PRIu64 "\r\n", s.retransmits);
    ShellPrintf("  ack_timeouts:     %" PRIu64 "\r\n", s.ack_timeouts);
    ShellPrintf("  rate_limit_drops: %" PRIu64 "\r\n", s.rate_limit_drops);
    return 0;
  };
  (void)osp::detail::GlobalCmdRegistry::Instance().Register("osp_serial", +cmd, "Show serial transport statistics");
}

// ============================================================================
// Service Layer
// ============================================================================

/// Register osp_nodes command (HsmNodeManager).
template <typename HsmNodeMgrType>
inline void RegisterHsmNodes(HsmNodeMgrType& mgr) {
  static HsmNodeMgrType* s_mgr = &mgr;
  static auto cmd = [](int /*argc*/, char* /*argv*/[]) -> int {
    uint32_t count = s_mgr->NodeCount();
    ShellPrintf("[osp_nodes] HsmNodeManager (%" PRIu32 " active)\r\n", count);
    s_mgr->ForEachNode([](const HsmNodeInfo& info) {
      uint64_t now = SteadyNowUs();
      uint64_t ago = (now > info.last_heartbeat_us) ? (now - info.last_heartbeat_us) : 0U;
      ShellPrintf("  node_id=%" PRIu16
                  "  state=%-14s"
                  "  last_hb=%" PRIu64
                  "ms_ago"
                  "  missed=%" PRIu32 "\r\n",
                  info.node_id, info.state_name, ago / 1000U, info.missed_count);
    });
    return 0;
  };
  (void)osp::detail::GlobalCmdRegistry::Instance().Register("osp_nodes", +cmd, "Show HSM node manager status");
}

/// Register osp_nodes_basic command (NodeManager).
template <typename NodeMgrType>
inline void RegisterNodeManager(NodeMgrType& mgr) {
  static NodeMgrType* s_mgr = &mgr;
  static auto cmd = [](int /*argc*/, char* /*argv*/[]) -> int {
    uint32_t count = s_mgr->NodeCount();
    ShellPrintf("[osp_nodes_basic] NodeManager (%" PRIu32 " active)\r\n", count);
    s_mgr->ForEach([](const auto& entry) {
      if (entry.is_listener) {
        ShellPrintf("  node_id=%" PRIu16 "  [listener]\r\n", entry.node_id);
      } else {
        ShellPrintf("  node_id=%" PRIu16 "  remote=%s:%" PRIu16 "\r\n", entry.node_id, entry.remote_host.c_str(),
                    entry.remote_port);
      }
    });
    return 0;
  };
  (void)osp::detail::GlobalCmdRegistry::Instance().Register("osp_nodes_basic", +cmd, "Show basic node manager status");
}

/// Register osp_service command (HsmService).
template <typename ServiceHsmType>
inline void RegisterServiceHsm(ServiceHsmType& svc) {
  static ServiceHsmType* s_svc = &svc;
  static auto cmd = [](int /*argc*/, char* /*argv*/[]) -> int {
    ShellPrintf("[osp_service] HsmService\r\n");
    ShellPrintf("  state: %s\r\n", s_svc->GetState());
    return 0;
  };
  (void)osp::detail::GlobalCmdRegistry::Instance().Register("osp_service", +cmd, "Show service HSM state");
}

/// Register osp_discovery command (HsmDiscovery).
template <typename DiscoveryHsmType>
inline void RegisterDiscoveryHsm(DiscoveryHsmType& disc) {
  static DiscoveryHsmType* s_disc = &disc;
  static auto cmd = [](int /*argc*/, char* /*argv*/[]) -> int {
    ShellPrintf("[osp_discovery] HsmDiscovery\r\n");
    ShellPrintf("  state:      %s\r\n", s_disc->GetState());
    ShellPrintf("  lost_count: %" PRIu32 "\r\n", s_disc->GetLostCount());
    return 0;
  };
  (void)osp::detail::GlobalCmdRegistry::Instance().Register("osp_discovery", +cmd, "Show discovery HSM state");
}

// ============================================================================
// App Layer
// ============================================================================

/// Register osp_lifecycle command (LifecycleNode).
/// Requires: osp/lifecycle_node.hpp included by caller.
template <typename LifecycleNodeType>
inline void RegisterLifecycle(LifecycleNodeType& node) {
  static LifecycleNodeType* s_node = &node;

  static auto show_status = [](int /*argc*/, char* /*argv*/[]) -> int {
    ShellPrintf("[osp_lifecycle] LifecycleNode\r\n");
    auto state = s_node->GetState();
    const char* coarse = "Unknown";
    switch (static_cast<uint8_t>(state)) {
      case 0:
        coarse = "Unconfigured";
        break;
      case 1:
        coarse = "Inactive";
        break;
      case 2:
        coarse = "Active";
        break;
      case 3:
        coarse = "Finalized";
        break;
      default:
        break;
    }
    ShellPrintf("  state: %s (%s)\r\n", coarse, s_node->DetailedStateName());
    return 0;
  };

  static auto lifecycle_error_name = [](LifecycleError err) -> const char* {
    switch (err) {
      case LifecycleError::kInvalidTransition:
        return "InvalidTransition";
      case LifecycleError::kCallbackFailed:
        return "CallbackFailed";
      case LifecycleError::kAlreadyFinalized:
        return "AlreadyFinalized";
      default:
        return "Unknown";
    }
  };

  static auto try_transition = [](const char* name, expected<void, LifecycleError> result) -> int {
    if (result.has_value()) {
      ShellPrintf("[osp_lifecycle] %s OK.\r\n", name);
      return 0;
    }
    ShellPrintf("[osp_lifecycle] %s failed: %s\r\n", name, lifecycle_error_name(result.get_error()));
    return -1;
  };

  static auto sub_status = [](int argc, char* argv[]) -> int { return (+show_status)(argc, argv); };
  static auto sub_configure = [](int /*argc*/, char* /*argv*/[]) -> int {
    return try_transition("Configure", s_node->Configure());
  };
  static auto sub_activate = [](int /*argc*/, char* /*argv*/[]) -> int {
    return try_transition("Activate", s_node->Activate());
  };
  static auto sub_deactivate = [](int /*argc*/, char* /*argv*/[]) -> int {
    return try_transition("Deactivate", s_node->Deactivate());
  };
  static auto sub_cleanup = [](int /*argc*/, char* /*argv*/[]) -> int {
    return try_transition("Cleanup", s_node->Cleanup());
  };
  static auto sub_shutdown = [](int /*argc*/, char* /*argv*/[]) -> int {
    return try_transition("Shutdown", s_node->Shutdown());
  };

  static const ShellSubCmd kSubs[] = {
      {"status", nullptr, "Show current state", +sub_status},
      {"configure", nullptr, "Transition: configure", +sub_configure},
      {"activate", nullptr, "Transition: activate", +sub_activate},
      {"deactivate", nullptr, "Transition: deactivate", +sub_deactivate},
      {"cleanup", nullptr, "Transition: cleanup", +sub_cleanup},
      {"shutdown", nullptr, "Transition: shutdown", +sub_shutdown},
  };

  static auto cmd = [](int argc, char* argv[]) -> int { return ShellDispatch(argc, argv, kSubs, 6U, +show_status); };

  (void)osp::detail::GlobalCmdRegistry::Instance().Register("osp_lifecycle", +cmd,
                                                            "Lifecycle node state and transitions");
}

/// Register osp_qos command (prints a QoS profile).
/// Requires: osp/qos.hpp included by caller.
/// @tparam QosType  Must have reliability, history, durability, history_depth,
///                  deadline_ms, lifespan_ms fields.
template <typename QosType>
inline void RegisterQos(const QosType& profile, const char* label = "default") {
  static const QosType* s_profile = &profile;
  static const char* s_label = label;
  static auto cmd = [](int /*argc*/, char* /*argv*/[]) -> int {
    ShellPrintf("[osp_qos] QosProfile '%s'\r\n", s_label);
    ShellPrintf("  reliability:   %s\r\n",
                static_cast<uint8_t>(s_profile->reliability) == 1U ? "Reliable" : "BestEffort");
    ShellPrintf("  history:       %s\r\n", static_cast<uint8_t>(s_profile->history) == 1U ? "KeepAll" : "KeepLast");
    ShellPrintf("  durability:    %s\r\n",
                static_cast<uint8_t>(s_profile->durability) == 1U ? "TransientLocal" : "Volatile");
    ShellPrintf("  history_depth: %" PRIu32 "\r\n", s_profile->history_depth);
    ShellPrintf("  deadline_ms:   %" PRIu32 "\r\n", s_profile->deadline_ms);
    ShellPrintf("  lifespan_ms:   %" PRIu32 "\r\n", s_profile->lifespan_ms);
    return 0;
  };
  (void)osp::detail::GlobalCmdRegistry::Instance().Register("osp_qos", +cmd, "Show QoS profile");
}

/// Register osp_app command (Application instance pool).
/// Requires: osp/app.hpp included by caller.
template <typename AppType>
inline void RegisterApp(AppType& app) {
  static AppType* s_app = &app;
  static auto cmd = [](int /*argc*/, char* /*argv*/[]) -> int {
    ShellPrintf("[osp_app] Application '%s' (id=%" PRIu16 ")\r\n", s_app->Name(), s_app->AppId());
    ShellPrintf("  instances:    %" PRIu32 "\r\n", s_app->InstanceCount());
    ShellPrintf("  pending_msgs: %" PRIu32 "\r\n", s_app->PendingMessages());
    return 0;
  };
  osp::detail::GlobalCmdRegistry::Instance().Register("osp_app", +cmd, "Show application instance pool status");
}

// ============================================================================
// Foundation Layer
// ============================================================================

/// Register osp_sysmon command (SystemMonitor).
template <typename MonitorType>
inline void RegisterSystemMonitor(MonitorType& mon) {
  static MonitorType* s_mon = &mon;
  static auto cmd = [](int /*argc*/, char* /*argv*/[]) -> int {
    auto snap = s_mon->LastSnapshot();
    ShellPrintf("[osp_sysmon] SystemMonitor\r\n");
    ShellPrintf("  CPU:  total=%" PRIu32 "%%  user=%" PRIu32 "%%  sys=%" PRIu32 "%%  iowait=%" PRIu32 "%%\r\n",
                snap.cpu.total_percent, snap.cpu.user_percent, snap.cpu.system_percent, snap.cpu.iowait_percent);
    if (snap.cpu.temperature_mc >= 0) {
      ShellPrintf("  Temp: %d.%d C\r\n", snap.cpu.temperature_mc / 1000, (snap.cpu.temperature_mc % 1000) / 100);
    } else {
      ShellPrintf("  Temp: N/A\r\n");
    }
    ShellPrintf("  Mem:  total=%" PRIu64 "kB  avail=%" PRIu64 "kB  used=%" PRIu32 "%%\r\n", snap.memory.total_kb,
                snap.memory.available_kb, snap.memory.used_percent);
    uint32_t disk_count = s_mon->DiskPathCount();
    for (uint32_t i = 0U; i < disk_count; ++i) {
      const auto& ds = s_mon->GetDiskSnapshot(i);
      ShellPrintf("  Disk[%" PRIu32 "]: total=%" PRIu64 "B  avail=%" PRIu64 "B  used=%" PRIu32 "%%\r\n", i,
                  ds.total_bytes, ds.available_bytes, ds.used_percent);
    }
    return 0;
  };
  (void)osp::detail::GlobalCmdRegistry::Instance().Register("osp_sysmon", +cmd, "Show system health monitor status");
}

/// Register osp_mempool command.
template <typename PoolType>
inline void RegisterMemPool(PoolType& pool, const char* label = "pool") {
  static PoolType* s_pool = &pool;
  static const char* s_label = label;
  static auto cmd = [](int /*argc*/, char* /*argv*/[]) -> int {
    ShellPrintf("[osp_mempool] %s\r\n", s_label);
    ShellPrintf("  capacity: %" PRIu32 "\r\n", s_pool->Capacity());
    ShellPrintf("  used:     %" PRIu32 "\r\n", s_pool->UsedCount());
    ShellPrintf("  free:     %" PRIu32 "\r\n", s_pool->FreeCount());
    return 0;
  };
  (void)osp::detail::GlobalCmdRegistry::Instance().Register("osp_mempool", +cmd, "Show memory pool usage");
}

// ============================================================================
// Runtime Control Commands
// ============================================================================

/// Register osp_log command.
/// Subcommands: status | level <0-5|debug|info|warn|error|fatal|off>
inline void RegisterLog() {
  static auto show_status = [](int /*argc*/, char* /*argv*/[]) -> int {
    auto lvl = log::GetLevel();
    ShellPrintf("[osp_log] level: %s (%" PRIu8 ")\r\n", log::detail::LevelTag(lvl), static_cast<uint8_t>(lvl));
    return 0;
  };

  static auto sub_status = [](int argc, char* argv[]) -> int { return (+show_status)(argc, argv); };

  static auto sub_level = [](int argc, char* argv[]) -> int {
    if (!ShellArgCheck(argc, 2, "osp_log level <0-5|debug|info|...>")) {
      return -1;
    }
    // Try numeric first.
    auto num = ShellParseUint(argv[1]);
    if (num.has_value()) {
      if (num.value() > 5U) {
        ShellPrintf("Invalid level: %" PRIu32 " (0-5)\r\n", num.value());
        return -1;
      }
      log::SetLevel(static_cast<log::Level>(static_cast<uint8_t>(num.value())));
      ShellPrintf("[osp_log] level set to %s\r\n", log::detail::LevelTag(log::GetLevel()));
      return 0;
    }
    // Try name match (case-insensitive).
    static const struct {
      const char* name;
      log::Level level;
    } kNames[] = {
        {"debug", log::Level::kDebug}, {"info", log::Level::kInfo},   {"warn", log::Level::kWarn},
        {"error", log::Level::kError}, {"fatal", log::Level::kFatal}, {"off", log::Level::kOff},
    };
    for (const auto& n : kNames) {
      if (osp::detail::ShellStrCaseEq(argv[1], n.name)) {
        log::SetLevel(n.level);
        ShellPrintf("[osp_log] level set to %s\r\n", log::detail::LevelTag(log::GetLevel()));
        return 0;
      }
    }
    ShellPrintf("Unknown level: %s\r\n", argv[1]);
    return -1;
  };

  static const ShellSubCmd kSubs[] = {
      {"status", nullptr, "Show current log level", +sub_status},
      {"level", "<0-5|debug|info|warn|error|fatal|off>", "Set log level", +sub_level},
  };

  static auto cmd = [](int argc, char* argv[]) -> int { return ShellDispatch(argc, argv, kSubs, 2U, +show_status); };

  (void)osp::detail::GlobalCmdRegistry::Instance().Register("osp_log", +cmd, "Log level display and control");
}

/// Register osp_config command.
/// Subcommands: list [section] | get <section> <key> | set <section> <key> <value>
template <typename ConfigType>
inline void RegisterConfig(ConfigType& cfg) {
  static ConfigType* s_cfg = &cfg;

  static auto show_list = [](int argc, char* argv[]) -> int {
    const char* filter = (argc >= 2) ? argv[1] : nullptr;
    if (filter != nullptr) {
      ShellPrintf("[osp_config] entries in [%s]:\r\n", filter);
    } else {
      ShellPrintf("[osp_config] all entries (%" PRIu32 "):\r\n", s_cfg->EntryCount());
    }
    s_cfg->ForEach([&](const char* section, const char* key, const char* value) {
      if (filter != nullptr && !osp::detail::ShellStrCaseEq(section, filter)) {
        return;
      }
      ShellPrintf("  [%s] %s = %s\r\n", section, key, value);
    });
    return 0;
  };

  static auto sub_list = [](int argc, char* argv[]) -> int { return (+show_list)(argc, argv); };

  static auto sub_get = [](int argc, char* argv[]) -> int {
    if (!ShellArgCheck(argc, 3, "osp_config get <section> <key>")) {
      return -1;
    }
    const char* val = s_cfg->GetString(argv[1], argv[2], nullptr);
    if (val == nullptr) {
      ShellPrintf("[%s] %s: not found\r\n", argv[1], argv[2]);
      return -1;
    }
    ShellPrintf("[%s] %s = %s\r\n", argv[1], argv[2], val);
    return 0;
  };

  static auto sub_set = [](int argc, char* argv[]) -> int {
    if (!ShellArgCheck(argc, 4, "osp_config set <section> <key> <value>")) {
      return -1;
    }
    bool ok = s_cfg->SetString(argv[1], argv[2], argv[3]);
    if (ok) {
      ShellPrintf("[%s] %s = %s (set)\r\n", argv[1], argv[2], argv[3]);
    } else {
      ShellPrintf("Failed: config store full.\r\n");
    }
    return ok ? 0 : -1;
  };

  static const ShellSubCmd kSubs[] = {
      {"list", "[section]", "List config entries", +sub_list},
      {"get", "<section> <key>", "Get a config value", +sub_get},
      {"set", "<section> <key> <value>", "Set a config value (in-memory)", +sub_set},
  };

  static auto cmd = [](int argc, char* argv[]) -> int { return ShellDispatch(argc, argv, kSubs, 3U, +show_list); };

  (void)osp::detail::GlobalCmdRegistry::Instance().Register("osp_config", +cmd, "Runtime config view and modification");
}

}  // namespace shell_cmd
}  // namespace osp

#endif  // OSP_SHELL_COMMANDS_HPP_
