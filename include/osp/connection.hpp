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
 * @file connection.hpp
 * @brief Fixed-capacity connection pool for managing network connections.
 *
 * Header-only, stack-allocated, zero heap allocation.
 * Compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_CONNECTION_HPP_
#define OSP_CONNECTION_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <cstdint>
#include <cstring>

#include <array>
#include <chrono>

namespace osp {

// ============================================================================
// Error Enum
// ============================================================================

enum class ConnectionError : uint8_t { kPoolFull, kNotFound, kInvalidId, kAlreadyExists, kTimeout };

// ============================================================================
// ConnectionId (strong type)
// ============================================================================

struct ConnectionId {
  uint32_t value;

  bool operator==(const ConnectionId& other) const { return value == other.value; }
  bool operator!=(const ConnectionId& other) const { return value != other.value; }

  static ConnectionId Invalid() { return {UINT32_MAX}; }
  bool IsValid() const { return value != UINT32_MAX; }
};

// ============================================================================
// ConnectionState
// ============================================================================

enum class ConnectionState : uint8_t { kIdle = 0, kConnecting, kConnected, kDisconnecting, kClosed };

// ============================================================================
// ConnectionInfo
// ============================================================================

struct ConnectionInfo {
  ConnectionId id;
  ConnectionState state;
  uint64_t created_time_us;  // timestamp when created
  uint64_t last_active_us;   // timestamp of last activity
  uint32_t remote_ip;        // IPv4 in host byte order
  uint16_t remote_port;
  char label[32];  // optional human-readable label
};

// ============================================================================
// ConnectionPool<MaxConnections>
// ============================================================================

#ifndef OSP_CONNECTION_POOL_MAX
#define OSP_CONNECTION_POOL_MAX 32U
#endif

template <uint32_t MaxConnections = OSP_CONNECTION_POOL_MAX>
class ConnectionPool {
 public:
  ConnectionPool() noexcept : count_(0U), next_id_(0U) {
    for (uint32_t i = 0U; i < MaxConnections; ++i) {
      slots_[i].active = false;
    }
  }

  /**
   * @brief Add a new connection to the pool.
   * @param remote_ip   IPv4 address in host byte order
   * @param remote_port Remote port number
   * @param label       Optional human-readable label (truncated to 31 chars)
   * @return ConnectionId on success, ConnectionError on failure
   */
  expected<ConnectionId, ConnectionError> Add(uint32_t remote_ip, uint16_t remote_port, const char* label = nullptr) {
    if (count_ >= MaxConnections) {
      return expected<ConnectionId, ConnectionError>::error(ConnectionError::kPoolFull);
    }

    // Find first inactive slot
    for (uint32_t i = 0U; i < MaxConnections; ++i) {
      if (!slots_[i].active) {
        const uint64_t now = SteadyNowUs();
        ConnectionInfo& info = slots_[i].info;
        info.id = ConnectionId{next_id_++};
        info.state = ConnectionState::kIdle;
        info.created_time_us = now;
        info.last_active_us = now;
        info.remote_ip = remote_ip;
        info.remote_port = remote_port;

        if (label != nullptr) {
          // Copy up to 31 chars + null terminator
          uint32_t j = 0U;
          while (j < 31U && label[j] != '\0') {
            info.label[j] = label[j];
            ++j;
          }
          info.label[j] = '\0';
        } else {
          info.label[0] = '\0';
        }

        slots_[i].active = true;
        ++count_;
        return expected<ConnectionId, ConnectionError>::success(info.id);
      }
    }

    // Should not reach here if count_ < MaxConnections, but be safe
    return expected<ConnectionId, ConnectionError>::error(ConnectionError::kPoolFull);
  }

  /**
   * @brief Remove a connection by ID.
   * @param id Connection ID to remove
   * @return void on success, ConnectionError on failure
   */
  expected<void, ConnectionError> Remove(ConnectionId id) {
    if (!id.IsValid()) {
      return expected<void, ConnectionError>::error(ConnectionError::kInvalidId);
    }

    const int32_t idx = FindSlot(id);
    if (idx < 0) {
      return expected<void, ConnectionError>::error(ConnectionError::kNotFound);
    }

    slots_[static_cast<uint32_t>(idx)].active = false;
    --count_;
    return expected<void, ConnectionError>::success();
  }

  /**
   * @brief Update connection state.
   * @param id    Connection ID
   * @param state New connection state
   * @return void on success, ConnectionError on failure
   */
  expected<void, ConnectionError> SetState(ConnectionId id, ConnectionState state) {
    if (!id.IsValid()) {
      return expected<void, ConnectionError>::error(ConnectionError::kInvalidId);
    }

    const int32_t idx = FindSlot(id);
    if (idx < 0) {
      return expected<void, ConnectionError>::error(ConnectionError::kNotFound);
    }

    slots_[static_cast<uint32_t>(idx)].info.state = state;
    return expected<void, ConnectionError>::success();
  }

  /**
   * @brief Touch: update last_active_us timestamp to now.
   * @param id Connection ID
   * @return void on success, ConnectionError on failure
   */
  expected<void, ConnectionError> Touch(ConnectionId id) {
    if (!id.IsValid()) {
      return expected<void, ConnectionError>::error(ConnectionError::kInvalidId);
    }

    const int32_t idx = FindSlot(id);
    if (idx < 0) {
      return expected<void, ConnectionError>::error(ConnectionError::kNotFound);
    }

    slots_[static_cast<uint32_t>(idx)].info.last_active_us = SteadyNowUs();
    return expected<void, ConnectionError>::success();
  }

  /**
   * @brief Find a connection by ID (const).
   * @param id Connection ID
   * @return Pointer to ConnectionInfo if found, nullptr otherwise
   */
  const ConnectionInfo* Find(ConnectionId id) const {
    const int32_t idx = FindSlot(id);
    if (idx < 0) {
      return nullptr;
    }
    return &slots_[static_cast<uint32_t>(idx)].info;
  }

  /**
   * @brief Find a connection by ID (mutable).
   * @param id Connection ID
   * @return Pointer to ConnectionInfo if found, nullptr otherwise
   */
  ConnectionInfo* Find(ConnectionId id) {
    const int32_t idx = FindSlot(id);
    if (idx < 0) {
      return nullptr;
    }
    return &slots_[static_cast<uint32_t>(idx)].info;
  }

  /**
   * @brief Iterate over all active connections.
   * @param func Callable with signature void(const ConnectionInfo&)
   */
  template <typename Func>
  void ForEach(Func&& func) const {
    for (uint32_t i = 0U; i < MaxConnections; ++i) {
      if (slots_[i].active) {
        func(slots_[i].info);
      }
    }
  }

  /**
   * @brief Remove connections inactive for longer than timeout_us.
   * @param timeout_us Timeout in microseconds
   * @return Number of connections removed
   */
  uint32_t RemoveTimedOut(uint64_t timeout_us) {
    const uint64_t now = SteadyNowUs();
    uint32_t removed = 0U;

    for (uint32_t i = 0U; i < MaxConnections; ++i) {
      if (slots_[i].active) {
        if (now - slots_[i].info.last_active_us > timeout_us) {
          slots_[i].active = false;
          --count_;
          ++removed;
        }
      }
    }

    return removed;
  }

  /** @brief Number of active connections. */
  uint32_t Count() const noexcept { return count_; }

  /** @brief Maximum number of connections. */
  uint32_t Capacity() const noexcept { return MaxConnections; }

  /** @brief True if pool is full. */
  bool IsFull() const noexcept { return count_ >= MaxConnections; }

  /** @brief True if pool is empty. */
  bool IsEmpty() const noexcept { return count_ == 0U; }

  /** @brief Clear all connections. */
  void Clear() noexcept {
    for (uint32_t i = 0U; i < MaxConnections; ++i) {
      slots_[i].active = false;
    }
    count_ = 0U;
  }

 private:
  struct Slot {
    ConnectionInfo info;
    bool active;
  };

  std::array<Slot, MaxConnections> slots_;
  uint32_t count_;
  uint32_t next_id_;

  /**
   * @brief Linear scan to find slot index for a given ConnectionId.
   * @param id Connection ID to search for
   * @return Slot index if found, -1 otherwise
   */
  int32_t FindSlot(ConnectionId id) const {
    for (uint32_t i = 0U; i < MaxConnections; ++i) {
      if (slots_[i].active && slots_[i].info.id == id) {
        return static_cast<int32_t>(i);
      }
    }
    return -1;
  }
};

}  // namespace osp

#endif  // OSP_CONNECTION_HPP_
