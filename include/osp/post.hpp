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

class AppRegistry {
 public:
  using PostFn = bool (*)(void* app, uint16_t ins_id, uint16_t event,
                          const void* data, uint32_t len);

  static AppRegistry& Instance() noexcept {
    static AppRegistry instance;
    return instance;
  }

  bool Register(uint16_t app_id, void* app_ptr, PostFn post_fn) noexcept {
    if (app_ptr == nullptr || post_fn == nullptr) return false;
    // Check duplicate
    for (uint32_t i = 0; i < count_; ++i) {
      if (entries_[i].active && entries_[i].app_id == app_id) {
        return false;
      }
    }
    // Find free slot
    for (uint32_t i = 0; i < OSP_POST_MAX_APPS; ++i) {
      if (!entries_[i].active) {
        entries_[i].app_id = app_id;
        entries_[i].app_ptr = app_ptr;
        entries_[i].post_fn = post_fn;
        entries_[i].active = true;
        ++count_;
        return true;
      }
    }
    return false;
  }

  bool Unregister(uint16_t app_id) noexcept {
    for (uint32_t i = 0; i < OSP_POST_MAX_APPS; ++i) {
      if (entries_[i].active && entries_[i].app_id == app_id) {
        entries_[i].active = false;
        entries_[i].app_ptr = nullptr;
        entries_[i].post_fn = nullptr;
        --count_;
        return true;
      }
    }
    return false;
  }

  bool PostLocal(uint32_t dst_iid, uint16_t event,
                 const void* data, uint32_t len) noexcept {
    uint16_t app_id = GetAppId(dst_iid);
    uint16_t ins_id = GetInsId(dst_iid);
    for (uint32_t i = 0; i < OSP_POST_MAX_APPS; ++i) {
      if (entries_[i].active && entries_[i].app_id == app_id) {
        return entries_[i].post_fn(entries_[i].app_ptr, ins_id, event,
                                   data, len);
      }
    }
    return false;
  }

  uint32_t Count() const noexcept { return count_; }

  void Reset() noexcept {
    for (uint32_t i = 0; i < OSP_POST_MAX_APPS; ++i) {
      entries_[i].active = false;
      entries_[i].app_ptr = nullptr;
      entries_[i].post_fn = nullptr;
    }
    count_ = 0;
  }

 private:
  AppRegistry() noexcept : count_(0) {
    for (uint32_t i = 0; i < OSP_POST_MAX_APPS; ++i) {
      entries_[i].active = false;
      entries_[i].app_ptr = nullptr;
      entries_[i].post_fn = nullptr;
    }
  }

  struct AppEntry {
    void* app_ptr;
    PostFn post_fn;
    uint16_t app_id;
    bool active;
  };

  AppEntry entries_[OSP_POST_MAX_APPS];
  uint32_t count_;
};

// ============================================================================
// RegisterApp helper
// ============================================================================

template <uint16_t MaxInstances>
bool RegisterApp(Application<MaxInstances>& app) noexcept {
  return AppRegistry::Instance().Register(
      app.AppId(), &app,
      [](void* ptr, uint16_t ins_id, uint16_t event,
         const void* data, uint32_t len) -> bool {
        auto* a = static_cast<Application<MaxInstances>*>(ptr);
        return a->Post(ins_id, event, data, len);
      });
}

template <uint16_t MaxInstances>
bool UnregisterApp(Application<MaxInstances>& app) noexcept {
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
// OspSendAndWait - Synchronous request-response (simplified)
// ============================================================================

inline expected<uint32_t, PostError> OspSendAndWait(
    uint32_t dst_iid, uint16_t event,
    const void* data, uint32_t len,
    void* ack_buf, uint32_t ack_buf_size,
    uint16_t dst_node = 0,
    int timeout_ms = 2000) noexcept {
  (void)ack_buf;
  (void)ack_buf_size;

  if (dst_node != 0) {
    return expected<uint32_t, PostError>::error(PostError::kSendFailed);
  }

  // Post the message
  if (!OspPost(dst_iid, event, data, len, dst_node)) {
    return expected<uint32_t, PostError>::error(PostError::kAppNotFound);
  }

  // For local delivery, message is already queued - return success with 0 ack
  // Full sync response would require a response channel (future enhancement)
  (void)timeout_ms;
  return expected<uint32_t, PostError>::success(0);
}

}  // namespace osp

#endif  // OSP_POST_HPP_
