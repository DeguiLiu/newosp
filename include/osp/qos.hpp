/**
 * @file qos.hpp
 * @brief Quality of Service (QoS) policies for pub-sub communication.
 *
 * Defines QoS profiles for reliability, history, durability, and timing constraints.
 */

#ifndef OSP_QOS_HPP_
#define OSP_QOS_HPP_

#include <cstdint>

#include "osp/platform.hpp"

namespace osp {

// ============================================================================
// QoS Policy Enumerations
// ============================================================================

/**
 * @brief QoS reliability policy.
 */
enum class ReliabilityPolicy : uint8_t {
  kBestEffort,  // Best-effort, allows loss (UDP semantics)
  kReliable     // Reliable delivery (TCP semantics + retransmit)
};

/**
 * @brief QoS history depth policy.
 */
enum class HistoryPolicy : uint8_t {
  kKeepLast,    // Keep last N messages
  kKeepAll      // Keep all messages (until queue full)
};

/**
 * @brief QoS durability policy.
 */
enum class DurabilityPolicy : uint8_t {
  kVolatile,        // No late-joiner support
  kTransientLocal   // Late joiners get cached messages
};

// ============================================================================
// QoS Profile
// ============================================================================

/**
 * @brief QoS configuration profile.
 *
 * Defines quality of service parameters for publishers and subscribers.
 */
struct QosProfile {
  ReliabilityPolicy reliability = ReliabilityPolicy::kBestEffort;
  HistoryPolicy history = HistoryPolicy::kKeepLast;
  DurabilityPolicy durability = DurabilityPolicy::kVolatile;
  uint32_t history_depth = 10;      // KeepLast mode message count
  uint32_t deadline_ms = 0;         // 0=no deadline, >0=message timeout discard
  uint32_t lifespan_ms = 0;         // 0=no lifespan, >0=message expiry discard

  /**
   * @brief Equality comparison.
   */
  constexpr bool operator==(const QosProfile& other) const {
    return reliability == other.reliability &&
           history == other.history &&
           durability == other.durability &&
           history_depth == other.history_depth &&
           deadline_ms == other.deadline_ms &&
           lifespan_ms == other.lifespan_ms;
  }

  /**
   * @brief Inequality comparison.
   */
  constexpr bool operator!=(const QosProfile& other) const {
    return !(*this == other);
  }
};

// ============================================================================
// Predefined QoS Profiles
// ============================================================================

/**
 * @brief QoS for sensor data: best-effort, keep last 1, volatile.
 *
 * Suitable for high-frequency sensor readings where only the latest value matters.
 */
constexpr QosProfile QosSensorData{
  ReliabilityPolicy::kBestEffort,
  HistoryPolicy::kKeepLast,
  DurabilityPolicy::kVolatile,
  1,   // only keep latest value
  0, 0
};

/**
 * @brief QoS for control commands: reliable, keep all, 100ms deadline.
 *
 * Suitable for critical control commands that must be delivered reliably.
 */
constexpr QosProfile QosControlCommand{
  ReliabilityPolicy::kReliable,
  HistoryPolicy::kKeepAll,
  DurabilityPolicy::kVolatile,
  0,
  100,  // 100ms deadline
  0
};

/**
 * @brief Default QoS profile: best-effort, keep last 10, volatile.
 */
constexpr QosProfile QosSystemDefault{};

/**
 * @brief QoS for reliable sensor data: reliable, keep last 5, 1s lifespan.
 *
 * Suitable for sensor data that must be delivered reliably with bounded history.
 */
constexpr QosProfile QosReliableSensor{
  ReliabilityPolicy::kReliable,
  HistoryPolicy::kKeepLast,
  DurabilityPolicy::kVolatile,
  5,
  0,
  1000  // 1s lifespan
};

// ============================================================================
// QoS Compatibility Checking
// ============================================================================

/**
 * @brief Checks if publisher and subscriber QoS profiles are compatible.
 *
 * Compatibility rules:
 * - Reliability: Reliable publisher is compatible with both BestEffort and Reliable subscribers.
 *                BestEffort publisher is only compatible with BestEffort subscribers.
 * - Durability: TransientLocal publisher is compatible with both Volatile and TransientLocal subscribers.
 *               Volatile publisher is only compatible with Volatile subscribers.
 *
 * @param pub_qos Publisher QoS profile.
 * @param sub_qos Subscriber QoS profile.
 * @return true if compatible, false otherwise.
 */
constexpr bool IsCompatible(const QosProfile& pub_qos, const QosProfile& sub_qos) {
  // Reliability compatibility check
  bool reliability_compatible = false;
  if (pub_qos.reliability == ReliabilityPolicy::kReliable) {
    // Reliable publisher is compatible with both BestEffort and Reliable subscribers
    reliability_compatible = true;
  } else {
    // BestEffort publisher is only compatible with BestEffort subscribers
    reliability_compatible = (sub_qos.reliability == ReliabilityPolicy::kBestEffort);
  }

  // Durability compatibility check
  bool durability_compatible = false;
  if (pub_qos.durability == DurabilityPolicy::kTransientLocal) {
    // TransientLocal publisher is compatible with both Volatile and TransientLocal subscribers
    durability_compatible = true;
  } else {
    // Volatile publisher is only compatible with Volatile subscribers
    durability_compatible = (sub_qos.durability == DurabilityPolicy::kVolatile);
  }

  return reliability_compatible && durability_compatible;
}

// ============================================================================
// QoS Timing Helpers
// ============================================================================

/**
 * @brief Checks if a message has exceeded its deadline.
 *
 * @param profile QoS profile containing deadline configuration.
 * @param elapsed_ms Time elapsed since message was sent/received.
 * @return true if deadline is exceeded, false if no deadline or not exceeded.
 */
constexpr bool IsDeadlineExpired(const QosProfile& profile, uint32_t elapsed_ms) {
  return profile.deadline_ms > 0 && elapsed_ms > profile.deadline_ms;
}

/**
 * @brief Checks if a message has exceeded its lifespan.
 *
 * @param profile QoS profile containing lifespan configuration.
 * @param elapsed_ms Time elapsed since message was created.
 * @return true if lifespan is exceeded, false if no lifespan or not exceeded.
 */
constexpr bool IsLifespanExpired(const QosProfile& profile, uint32_t elapsed_ms) {
  return profile.lifespan_ms > 0 && elapsed_ms > profile.lifespan_ms;
}

}  // namespace osp

#endif  // OSP_QOS_HPP_
