/**
 * @file protocol.hpp
 * @brief Shared protocol definitions for net_stress demo.
 *
 * All message types are trivially copyable POD structs for zero-copy
 * transport over osp::Service RPC.
 *
 * Original OSP demo mapping:
 *   TEST_REQ_EVENT / TEST_EVENT_ACK  -> HandshakeReq / HandshakeResp (RPC)
 *   COMM_TEST_EVENT (send+echo)      -> EchoReq / EchoResp (RPC)
 *   POWER_UP_EVENT                   -> Bus: StartTestCmd
 *   OSP_DISCONNECT                   -> Bus: PeerEvent (disconnected)
 *   p() / s() debug commands         -> DebugShell commands
 *   parsecfg INI                     -> command-line args (simplified)
 *
 * newosp vocabulary types used:
 *   FixedString, FixedVector, expected, ScopeGuard, Config
 */

#ifndef NET_STRESS_PROTOCOL_HPP_
#define NET_STRESS_PROTOCOL_HPP_

#include <cstdint>
#include <cstring>
#include <ctime>
#include <variant>

#include "osp/config.hpp"
#include "osp/log.hpp"

namespace net_stress {

// ============================================================================
// Network Configuration
// ============================================================================

static constexpr uint16_t kHandshakePort     = 20000U;
static constexpr uint16_t kEchoPort          = 20001U;
static constexpr uint16_t kFilePort          = 20002U;
static constexpr uint16_t kServerShellPort   = 9600U;
static constexpr uint16_t kClientShellPort   = 9601U;
static constexpr uint16_t kMonitorShellPort  = 9602U;
static constexpr uint32_t kMaxPayloadBytes   = 4096U;
static constexpr uint32_t kMaxClients        = 64U;
static constexpr uint32_t kDefaultIntervalMs = 1000U;
static constexpr uint32_t kDefaultPayloadLen = 1024U;
static constexpr uint32_t kConnectTimeoutMs  = 3000U;
static constexpr uint32_t kProtocolVersion   = 1U;
static constexpr uint32_t kServerId          = 0x50000001U;

// ============================================================================
// Handshake RPC: osp::Service<HandshakeReq, HandshakeResp>
// ============================================================================

struct HandshakeReq {
  uint32_t client_id;
  uint32_t version;
  char     name[32];
};

struct HandshakeResp {
  uint32_t server_id;
  uint32_t slot;
  uint16_t echo_port;
  uint8_t  accepted;       // 1=ok, 0=rejected
  uint8_t  reserved;
  char     server_name[32];
};

// ============================================================================
// Echo RPC: osp::Service<EchoReq, EchoResp>
// ============================================================================

struct EchoReq {
  uint32_t client_id;
  uint32_t seq;
  uint32_t payload_len;
  uint32_t reserved;
  uint64_t send_ts_ns;
  uint8_t  payload[kMaxPayloadBytes];
};

struct EchoResp {
  uint32_t server_id;
  uint32_t seq;
  uint32_t payload_len;
  uint32_t reserved;
  uint64_t client_ts_ns;   // echoed back from request
  uint64_t server_ts_ns;   // server processing timestamp
  uint8_t  payload[kMaxPayloadBytes];
};

// ============================================================================
// Bus Message Types (local pub/sub within client process)
// ============================================================================

/// Peer connection/disconnection event.
struct PeerEvent {
  uint32_t client_id;
  uint32_t slot;
  uint8_t  connected;      // 1=connected, 0=disconnected
};

/// Command to start/stop the stress test.
struct StartTestCmd {
  uint32_t interval_ms;
  uint32_t payload_len;
};

struct StopTestCmd {
  uint32_t reason;         // 0=user, 1=timeout, 2=error
};

/// Aggregated statistics snapshot.
struct StatsSnapshot {
  uint32_t active_clients;
  uint32_t total_sent;
  uint32_t total_recv;
  uint32_t total_errors;
  uint64_t total_rtt_us;
  uint32_t rtt_samples;
  uint64_t elapsed_ms;
};

/// Per-client echo result (published after each echo round).
struct EchoResult {
  uint32_t client_id;
  uint32_t seq;
  uint64_t rtt_us;
  uint8_t  success;
};

/// File transfer request (client -> server via dedicated RPC).
struct FileTransferReq {
  uint32_t client_id;
  uint32_t chunk_seq;       // chunk sequence number (0 = header)
  uint32_t total_chunks;
  uint32_t chunk_len;
  uint32_t file_size;       // total file size (only in chunk_seq==0)
  uint8_t  data[kMaxPayloadBytes];
};

struct FileTransferResp {
  uint32_t server_id;
  uint32_t chunk_seq;
  uint32_t received_bytes;
  uint8_t  accepted;        // 1=ok, 0=error
  uint8_t  complete;        // 1=all chunks received
  uint8_t  reserved[2];
};

/// File transfer progress (published via Bus).
struct FileProgress {
  uint32_t client_id;
  uint32_t chunks_sent;
  uint32_t total_chunks;
  uint32_t bytes_sent;
  uint32_t file_size;
  uint8_t  complete;
  uint8_t  success;
};

/// The variant type for the bus.
using BusPayload = std::variant<PeerEvent, StartTestCmd, StopTestCmd,
                                StatsSnapshot, EchoResult, FileProgress>;

// ============================================================================
// Utility
// ============================================================================

inline uint64_t NowNs() noexcept {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

inline uint64_t NowMs() noexcept { return NowNs() / 1000000ULL; }

/// Fill buffer with repeating pattern based on seq number.
inline void FillPattern(uint8_t* buf, uint32_t len, uint32_t seq) noexcept {
  if (buf == nullptr) return;
  for (uint32_t i = 0; i < len; ++i) {
    buf[i] = static_cast<uint8_t>((seq + i) & 0xFFU);
  }
}

/// Verify pattern in buffer.
inline bool VerifyPattern(const uint8_t* buf, uint32_t len,
                          uint32_t seq) noexcept {
  if (buf == nullptr) return false;
  for (uint32_t i = 0; i < len; ++i) {
    if (buf[i] != static_cast<uint8_t>((seq + i) & 0xFFU)) return false;
  }
  return true;
}

// ============================================================================
// INI Configuration Loading
// ============================================================================

/// Parsed configuration from net_stress.ini.
struct NetStressConfig {
  // [server]
  uint16_t server_hs_port     = kHandshakePort;
  uint16_t server_echo_port   = kEchoPort;
  uint16_t server_file_port   = kFilePort;
  uint16_t server_shell_port  = kServerShellPort;
  // [client]
  uint32_t client_num         = 4U;
  uint32_t client_interval_ms = kDefaultIntervalMs;
  uint32_t client_payload_len = kDefaultPayloadLen;
  uint16_t client_shell_port  = kClientShellPort;
  // [monitor]
  uint32_t monitor_probe_ms   = 2000U;
  uint16_t monitor_shell_port = kMonitorShellPort;
  // [network]
  uint32_t connect_timeout_ms = kConnectTimeoutMs;
  uint32_t max_clients        = kMaxClients;
};

/// Try to load config from INI file. Returns true if file was loaded.
/// Missing keys keep their default values from NetStressConfig.
inline bool LoadConfig(const char* path, NetStressConfig& cfg) {
  osp::Config<osp::IniBackend> ini;
  auto r = ini.LoadFile(path);
  if (!r.has_value()) {
    OSP_LOG_WARN("CONFIG", "Cannot load '%s', using defaults", path);
    return false;
  }
  OSP_LOG_INFO("CONFIG", "Loaded configuration from '%s'", path);

  // [server]
  cfg.server_hs_port    = ini.GetPort("server", "handshake_port", cfg.server_hs_port);
  cfg.server_echo_port  = ini.GetPort("server", "echo_port", cfg.server_echo_port);
  cfg.server_file_port  = ini.GetPort("server", "file_port", cfg.server_file_port);
  cfg.server_shell_port = ini.GetPort("server", "shell_port", cfg.server_shell_port);

  // [client]
  cfg.client_num         = static_cast<uint32_t>(
      ini.GetInt("client", "num_clients", static_cast<int32_t>(cfg.client_num)));
  cfg.client_interval_ms = static_cast<uint32_t>(
      ini.GetInt("client", "interval_ms", static_cast<int32_t>(cfg.client_interval_ms)));
  cfg.client_payload_len = static_cast<uint32_t>(
      ini.GetInt("client", "payload_len", static_cast<int32_t>(cfg.client_payload_len)));
  cfg.client_shell_port  = ini.GetPort("client", "shell_port", cfg.client_shell_port);

  // [monitor]
  cfg.monitor_probe_ms   = static_cast<uint32_t>(
      ini.GetInt("monitor", "probe_interval_ms", static_cast<int32_t>(cfg.monitor_probe_ms)));
  cfg.monitor_shell_port = ini.GetPort("monitor", "shell_port", cfg.monitor_shell_port);

  // [network]
  cfg.connect_timeout_ms = static_cast<uint32_t>(
      ini.GetInt("network", "connect_timeout_ms", static_cast<int32_t>(cfg.connect_timeout_ms)));
  cfg.max_clients        = static_cast<uint32_t>(
      ini.GetInt("network", "max_clients", static_cast<int32_t>(cfg.max_clients)));

  return true;
}

/// Scan argv for "--config <path>" and return the path, or nullptr.
inline const char* FindConfigArg(int argc, char* argv[]) {
  for (int i = 1; i < argc - 1; ++i) {
    if (std::strcmp(argv[i], "--config") == 0) {
      return argv[i + 1];
    }
  }
  return nullptr;
}

}  // namespace net_stress

#endif  // NET_STRESS_PROTOCOL_HPP_
