/**
 * @file post.hpp
 * @brief Unified message delivery compatible with original OSP OspPost().
 *
 * Provides automatic routing between local/IPC/network delivery via
 * a global application registry.
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_POST_HPP_
#define OSP_POST_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"
#include "osp/app.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

namespace osp {

// ============================================================================
// Post Error
// ============================================================================

enum class PostError : uint8_t {
  kAppNotFound,
  kInstanceNotFound,
  kQueueFull,
  kTimeout,
  kNotRegistered,
  kSendFailed
};

// ============================================================================
// Application Registry
// ============================================================================

#ifndef OSP_POST_MAX_APPS
#define OSP_POST_MAX_APPS 64U
#endif

// Thread-safety contract:
//   - Register/Unregister: call during init/shutdown phase only
//   - PostLocal/OspPost:   safe to call from any thread (delegates to
//     Application::Post which is mutex-protected)
//   - No internal mutex — relies on phase-based access pattern.
class AppRegistry {
 public:
  using PostFn = bool (*)(void* app, uint16_t ins_id, uint16_t event,
                          const void* data, uint32_t len,
                          ResponseChannel* response_ch);

  static AppRegistry& Instance() noexcept {
    static AppRegistry instance;
    return instance;
  }

  bool Register(uint16_t app_id, void* app_ptr, PostFn post_fn) noexcept {
    if (app_ptr == nullptr || post_fn == nullptr) return false;
    // Check duplicate
    for (uint32_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i].app_id == app_id) {
        return false;
      }
    }
    if (entries_.full()) return false;
    AppEntry entry;
    entry.app_ptr = app_ptr;
    entry.post_fn = post_fn;
    entry.app_id = app_id;
    return entries_.push_back(entry);
  }

  bool Unregister(uint16_t app_id) noexcept {
    for (uint32_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i].app_id == app_id) {
        static_cast<void>(entries_.erase_unordered(i));
        return true;
      }
    }
    return false;
  }

  bool PostLocal(uint32_t dst_iid, uint16_t event,
                 const void* data, uint32_t len,
                 ResponseChannel* response_ch = nullptr) noexcept {
    uint16_t app_id = GetAppId(dst_iid);
    uint16_t ins_id = GetInsId(dst_iid);
    for (uint32_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i].app_id == app_id) {
        return entries_[i].post_fn(entries_[i].app_ptr, ins_id, event,
                                   data, len, response_ch);
      }
    }
    return false;
  }

  uint32_t Count() const noexcept { return entries_.size(); }

  void Reset() noexcept { entries_.clear(); }

 private:
  AppRegistry() noexcept = default;

  struct AppEntry {
    void* app_ptr;
    PostFn post_fn;
    uint16_t app_id;
  };

  FixedVector<AppEntry, OSP_POST_MAX_APPS> entries_;
};

// ============================================================================
// RegisterApp helper
// ============================================================================

template <typename InstanceImpl, uint16_t MaxInstances>
bool RegisterApp(Application<InstanceImpl, MaxInstances>& app) noexcept {
  return AppRegistry::Instance().Register(
      app.AppId(), &app,
      [](void* ptr, uint16_t ins_id, uint16_t event,
         const void* data, uint32_t len,
         ResponseChannel* response_ch) -> bool {
        auto* a = static_cast<Application<InstanceImpl, MaxInstances>*>(ptr);
        return a->Post(ins_id, event, data, len, response_ch);
      });
}

template <typename InstanceImpl, uint16_t MaxInstances>
bool UnregisterApp(Application<InstanceImpl, MaxInstances>& app) noexcept {
  return AppRegistry::Instance().Unregister(app.AppId());
}

// ============================================================================
// OspPost - Unified message delivery
// ============================================================================

inline bool OspPost(uint32_t dst_iid, uint16_t event,
                    const void* data, uint32_t len,
                    uint16_t dst_node = 0) noexcept {
  if (dst_node != 0) {
    // Remote delivery - stub for now
    return false;
  }
  return AppRegistry::Instance().PostLocal(dst_iid, event, data, len);
}

// ============================================================================
// OspSendAndWait - Synchronous request-response
// ============================================================================

/**
 * @brief Send a message and wait for a synchronous reply.
 *
 * Posts the message to the target instance. The instance handler can call
 * Reply() to send response data back. This function blocks until the reply
 * is received or the timeout expires.
 *
 * @param dst_iid Destination instance ID (MakeIID(app_id, ins_id)).
 * @param event Event identifier.
 * @param data Request payload.
 * @param len Request payload length.
 * @param ack_buf Buffer to receive the response data.
 * @param ack_buf_size Size of the ack buffer.
 * @param dst_node Destination node (0 = local).
 * @param timeout_ms Timeout in milliseconds.
 * @return Number of bytes written to ack_buf on success, or PostError.
 */
inline expected<uint32_t, PostError> OspSendAndWait(
    uint32_t dst_iid, uint16_t event,
    const void* data, uint32_t len,
    void* ack_buf, uint32_t ack_buf_size,
    uint16_t dst_node = 0,
    int timeout_ms = 2000) noexcept {

  if (dst_node != 0) {
    return expected<uint32_t, PostError>::error(PostError::kSendFailed);
  }

  // Create a response channel on the stack
  ResponseChannel channel;

  // Post the message with the response channel attached
  if (!AppRegistry::Instance().PostLocal(dst_iid, event, data, len,
                                          &channel)) {
    return expected<uint32_t, PostError>::error(PostError::kAppNotFound);
  }

  // Wait for the reply with timeout
  {
    std::unique_lock<std::mutex> lock(channel.mtx);
    if (!channel.replied) {
      auto deadline = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(timeout_ms);
      channel.cv.wait_until(lock, deadline,
                            [&channel]() { return channel.replied; });
    }
  }

  if (!channel.replied) {
    return expected<uint32_t, PostError>::error(PostError::kTimeout);
  }

  // Copy response to caller's buffer
  uint32_t copy_len = channel.data_len;
  if (copy_len > ack_buf_size) {
    copy_len = ack_buf_size;
  }
  if (ack_buf != nullptr && copy_len > 0) {
    std::memcpy(ack_buf, channel.data, copy_len);
  }

  return expected<uint32_t, PostError>::success(copy_len);
}

}  // namespace osp

#endif  // OSP_POST_HPP_
