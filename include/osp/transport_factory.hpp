/**
 * @file transport_factory.hpp
 * @brief Automatic transport selection (inproc/shm/tcp) for OSP-CPP.
 *
 * Provides TransportFactory for detecting the best transport type based on
 * configuration, and TransportSelector for managing transport configuration
 * and resolution.
 */

#ifndef OSP_TRANSPORT_FACTORY_HPP_
#define OSP_TRANSPORT_FACTORY_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <cstdint>
#include <cstring>

namespace osp {

// ============================================================================
// TransportType Enum
// ============================================================================

/**
 * @brief Transport type enumeration for automatic selection.
 */
enum class TransportType : uint8_t {
  kInproc = 0,  ///< In-process (same process, lock-free queue)
  kShm,         ///< Shared memory (local host, zero-copy)
  kTcp,         ///< TCP socket (remote host, network)
  kAuto         ///< Automatic detection based on config
};

// ============================================================================
// TransportConfig Struct
// ============================================================================

/**
 * @brief Configuration for transport selection.
 *
 * Used by TransportFactory to determine the best transport type.
 */
struct TransportConfig {
  TransportType type = TransportType::kAuto;
  char remote_host[64] = "127.0.0.1";
  uint16_t remote_port = 0;
  char shm_channel_name[64] = "";
  uint32_t shm_slot_size = 4096;
  uint32_t shm_slot_count = 256;
};

// ============================================================================
// TransportFactory Class
// ============================================================================

/**
 * @brief Factory for automatic transport type detection and selection.
 *
 * Provides static methods for detecting the best transport based on
 * configuration and converting transport types to string names.
 */
class TransportFactory final {
 public:
  /**
   * @brief Detects the best transport type based on configuration.
   *
   * Selection logic:
   * - If remote_host is "127.0.0.1" or "localhost" and shm_channel_name is
   *   non-empty -> kShm
   * - If remote_host is "127.0.0.1" or "localhost" and shm_channel_name is
   *   empty -> kInproc
   * - Otherwise -> kTcp
   *
   * @param cfg Transport configuration
   * @return Detected transport type (never kAuto)
   */
  static TransportType DetectBestTransport(const TransportConfig& cfg) noexcept {
    // Check if local host
    const bool is_local = IsLocalHost(cfg.remote_host);

    if (is_local) {
      // Check if shm_channel_name is non-empty
      if (cfg.shm_channel_name[0] != '\0') {
        return TransportType::kShm;
      }
      return TransportType::kInproc;
    }

    return TransportType::kTcp;
  }

  /**
   * @brief Returns the string name of a transport type.
   *
   * @param type Transport type
   * @return String name ("inproc", "shm", "tcp", or "auto")
   */
  static const char* TransportTypeName(TransportType type) noexcept {
    switch (type) {
      case TransportType::kInproc:
        return "inproc";
      case TransportType::kShm:
        return "shm";
      case TransportType::kTcp:
        return "tcp";
      case TransportType::kAuto:
        return "auto";
      default:
        return "unknown";
    }
  }

 private:
  /**
   * @brief Checks if the given host string represents localhost.
   *
   * @param host Host string to check
   * @return true if host is "127.0.0.1", "localhost", or empty
   */
  static bool IsLocalHost(const char* host) noexcept {
    if (host == nullptr || host[0] == '\0') {
      return true;  // Empty host defaults to local
    }
    return (std::strcmp(host, "127.0.0.1") == 0) ||
           (std::strcmp(host, "localhost") == 0);
  }
};

// ============================================================================
// TransportSelector Template Class
// ============================================================================

/**
 * @brief Lightweight wrapper for transport configuration and resolution.
 *
 * Stores TransportConfig and resolves kAuto to a concrete transport type.
 *
 * @tparam PayloadVariant Bus payload variant type (for type safety)
 */
template <typename PayloadVariant>
class TransportSelector final {
 public:
  TransportSelector() noexcept : config_{}, resolved_type_(TransportType::kInproc) {}

  /**
   * @brief Configures the transport selector and resolves kAuto.
   *
   * If config.type is kAuto, resolves to concrete type using
   * TransportFactory::DetectBestTransport.
   *
   * @param cfg Transport configuration
   */
  void Configure(const TransportConfig& cfg) noexcept {
    config_ = cfg;
    if (config_.type == TransportType::kAuto) {
      resolved_type_ = TransportFactory::DetectBestTransport(config_);
    } else {
      resolved_type_ = config_.type;
    }
  }

  /**
   * @brief Returns the resolved transport type.
   *
   * @return Resolved transport type (never kAuto after Configure)
   */
  TransportType ResolvedType() const noexcept { return resolved_type_; }

  /**
   * @brief Checks if the resolved transport is local (inproc or shm).
   *
   * @return true if resolved type is kInproc or kShm
   */
  bool IsLocal() const noexcept {
    return resolved_type_ == TransportType::kInproc ||
           resolved_type_ == TransportType::kShm;
  }

  /**
   * @brief Checks if the resolved transport is remote (tcp).
   *
   * @return true if resolved type is kTcp
   */
  bool IsRemote() const noexcept { return resolved_type_ == TransportType::kTcp; }

  /**
   * @brief Returns the current configuration.
   *
   * @return Reference to transport configuration
   */
  const TransportConfig& Config() const noexcept { return config_; }

 private:
  TransportConfig config_;
  TransportType resolved_type_;
};

}  // namespace osp

#endif  // OSP_TRANSPORT_FACTORY_HPP_
