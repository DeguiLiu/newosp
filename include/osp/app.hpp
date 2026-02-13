/**
 * @file app.hpp
 * @brief Application/Instance two-layer model compatible with original OSP.
 *
 * Provides CApp/CInstance equivalent: message-driven application framework
 * with instance pool, state machine, and event dispatch.
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_APP_HPP_
#define OSP_APP_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <mutex>

namespace osp {

// ============================================================================
// Application Error
// ============================================================================

enum class AppError : uint8_t {
  kInstancePoolFull,
  kInstanceNotFound,
  kFactoryNotSet,
  kAlreadyRunning,
  kNotRunning,
  kInvalidId,
  kQueueFull
};

// ============================================================================
// Global Instance ID (IID) encoding/decoding
// ============================================================================

constexpr uint32_t MakeIID(uint16_t app_id, uint16_t ins_id) noexcept {
  return (static_cast<uint32_t>(app_id) << 16) | ins_id;
}
constexpr uint16_t GetAppId(uint32_t iid) noexcept {
  return static_cast<uint16_t>(iid >> 16);
}
constexpr uint16_t GetInsId(uint32_t iid) noexcept {
  return static_cast<uint16_t>(iid & 0xFFFF);
}

// Special instance IDs
constexpr uint16_t kInsPending = 0;
constexpr uint16_t kInsDaemon  = 0xFFFC;
constexpr uint16_t kInsEach    = 0xFFFF;

// ============================================================================
// ResponseChannel - for synchronous request-response (OspSendAndWait)
// ============================================================================

#ifndef OSP_RESPONSE_DATA_SIZE
#define OSP_RESPONSE_DATA_SIZE 256U
#endif

struct ResponseChannel {
  std::mutex mtx;
  std::condition_variable cv;
  uint8_t data[OSP_RESPONSE_DATA_SIZE];
  uint32_t data_len;
  bool replied;

  ResponseChannel() noexcept : data{}, data_len(0), replied(false) {}

  void Reply(const void* buf, uint32_t len) noexcept {
    std::lock_guard<std::mutex> lock(mtx);
    data_len = (len <= OSP_RESPONSE_DATA_SIZE) ? len : OSP_RESPONSE_DATA_SIZE;
    if (buf != nullptr && data_len > 0) {
      std::memcpy(data, buf, data_len);
    }
    replied = true;
    cv.notify_one();
  }
};

// ============================================================================
// Instance base class
// ============================================================================

class Instance {
 public:
  Instance() noexcept : state_(0), ins_id_(0), response_channel_(nullptr) {}
  virtual ~Instance() = default;

  virtual void OnMessage(uint16_t event, const void* data,
                         uint32_t len) = 0;

  uint16_t CurState() const noexcept { return state_; }
  void SetState(uint16_t state) noexcept { state_ = state; }
  uint16_t InsId() const noexcept { return ins_id_; }

  void SetInsId(uint16_t id) noexcept { ins_id_ = id; }

  // Reply to a synchronous OspSendAndWait call
  bool Reply(const void* data, uint32_t len) noexcept {
    if (response_channel_ == nullptr) return false;
    response_channel_->Reply(data, len);
    return true;
  }

  bool HasPendingReply() const noexcept {
    return response_channel_ != nullptr;
  }

  // Internal: set by Application when dispatching sync messages
  void SetResponseChannel(ResponseChannel* ch) noexcept {
    response_channel_ = ch;
  }

 private:
  uint16_t state_;
  uint16_t ins_id_;
  ResponseChannel* response_channel_;
};

// ============================================================================
// Instance factory
// ============================================================================

using InstanceFactory = Instance* (*)();

// ============================================================================
// Application message
// ============================================================================

#ifndef OSP_APP_MSG_DATA_SIZE
#define OSP_APP_MSG_DATA_SIZE 256U
#endif

struct AppMessage {
  uint16_t ins_id;
  uint16_t event;
  uint32_t data_len;
  uint8_t data[OSP_APP_MSG_DATA_SIZE];
  ResponseChannel* response_channel;  // non-null for sync messages
};

// ============================================================================
// Application Configuration
// ============================================================================

#ifndef OSP_APP_MAX_INSTANCES
#define OSP_APP_MAX_INSTANCES 64U
#endif

#ifndef OSP_APP_QUEUE_DEPTH
#define OSP_APP_QUEUE_DEPTH 256U
#endif

static constexpr uint32_t kAppNameMaxLen = 31;

// ============================================================================
// Application<MaxInstances>
// ============================================================================

template <uint16_t MaxInstances = OSP_APP_MAX_INSTANCES>
class Application {
 public:
  Application(uint16_t app_id, const char* name) noexcept
      : app_id_(app_id),
        factory_(nullptr),
        instance_count_(0),
        queue_head_(0),
        queue_tail_(0) {
    for (uint16_t i = 0; i < MaxInstances; ++i) {
      instances_[i] = nullptr;
    }
    if (name != nullptr) {
      uint32_t i = 0;
      for (; i < kAppNameMaxLen && name[i] != '\0'; ++i) {
        name_[i] = name[i];
      }
      name_[i] = '\0';
    } else {
      name_[0] = '\0';
    }
  }

  ~Application() noexcept {
    for (uint16_t i = 0; i < MaxInstances; ++i) {
      if (instances_[i] != nullptr) {
        delete instances_[i];
        instances_[i] = nullptr;
      }
    }
  }

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  // ======================== Factory ========================

  void SetFactory(InstanceFactory factory) noexcept { factory_ = factory; }

  // ======================== Instance Management ========================

  expected<uint16_t, AppError> CreateInstance() noexcept {
    if (factory_ == nullptr) {
      return expected<uint16_t, AppError>::error(AppError::kFactoryNotSet);
    }
    // Find free slot (ins_id starts from 1, slot index = ins_id - 1)
    for (uint16_t i = 0; i < MaxInstances; ++i) {
      if (instances_[i] == nullptr) {
        Instance* inst = factory_();
        if (inst == nullptr) {
          return expected<uint16_t, AppError>::error(
              AppError::kInstancePoolFull);
        }
        uint16_t ins_id = static_cast<uint16_t>(i + 1);
        inst->SetInsId(ins_id);
        instances_[i] = inst;
        ++instance_count_;
        return expected<uint16_t, AppError>::success(ins_id);
      }
    }
    return expected<uint16_t, AppError>::error(AppError::kInstancePoolFull);
  }

  expected<void, AppError> DestroyInstance(uint16_t ins_id) noexcept {
    if (ins_id == 0 || ins_id > MaxInstances) {
      return expected<void, AppError>::error(AppError::kInvalidId);
    }
    uint16_t idx = ins_id - 1;
    if (instances_[idx] == nullptr) {
      return expected<void, AppError>::error(AppError::kInstanceNotFound);
    }
    delete instances_[idx];
    instances_[idx] = nullptr;
    --instance_count_;
    return expected<void, AppError>::success();
  }

  // ======================== Message Queue ========================

  bool Post(uint16_t ins_id, uint16_t event, const void* data,
            uint32_t len,
            ResponseChannel* response_ch = nullptr) noexcept {
    uint32_t next_tail = (queue_tail_ + 1) % OSP_APP_QUEUE_DEPTH;
    if (next_tail == queue_head_) {
      return false;  // Queue full
    }
    AppMessage& msg = queue_[queue_tail_];
    msg.ins_id = ins_id;
    msg.event = event;
    msg.data_len = (len <= OSP_APP_MSG_DATA_SIZE) ? len : OSP_APP_MSG_DATA_SIZE;
    if (data != nullptr && msg.data_len > 0) {
      std::memcpy(msg.data, data, msg.data_len);
    }
    msg.response_channel = response_ch;
    queue_tail_ = next_tail;
    return true;
  }

  bool ProcessOne() noexcept {
    if (queue_head_ == queue_tail_) {
      return false;  // Empty
    }
    const AppMessage& msg = queue_[queue_head_];
    queue_head_ = (queue_head_ + 1) % OSP_APP_QUEUE_DEPTH;

    if (msg.ins_id == kInsEach) {
      // Broadcast to all instances
      for (uint16_t i = 0; i < MaxInstances; ++i) {
        if (instances_[i] != nullptr) {
          instances_[i]->SetResponseChannel(msg.response_channel);
          instances_[i]->OnMessage(msg.event, msg.data, msg.data_len);
          instances_[i]->SetResponseChannel(nullptr);
        }
      }
    } else {
      Instance* inst = GetInstance(msg.ins_id);
      if (inst != nullptr) {
        inst->SetResponseChannel(msg.response_channel);
        inst->OnMessage(msg.event, msg.data, msg.data_len);
        inst->SetResponseChannel(nullptr);
      }
    }
    return true;
  }

  uint32_t ProcessAll() noexcept {
    uint32_t count = 0;
    while (ProcessOne()) {
      ++count;
    }
    return count;
  }

  // ======================== Accessors ========================

  uint16_t AppId() const noexcept { return app_id_; }
  const char* Name() const noexcept { return name_; }
  uint32_t InstanceCount() const noexcept { return instance_count_; }

  Instance* GetInstance(uint16_t ins_id) noexcept {
    if (ins_id == 0 || ins_id > MaxInstances) return nullptr;
    return instances_[ins_id - 1];
  }

  uint32_t PendingMessages() const noexcept {
    if (queue_tail_ >= queue_head_) {
      return queue_tail_ - queue_head_;
    }
    return OSP_APP_QUEUE_DEPTH - queue_head_ + queue_tail_;
  }

 private:
  uint16_t app_id_;
  char name_[kAppNameMaxLen + 1];
  InstanceFactory factory_;
  Instance* instances_[MaxInstances];
  uint32_t instance_count_;
  AppMessage queue_[OSP_APP_QUEUE_DEPTH];
  uint32_t queue_head_;
  uint32_t queue_tail_;
};

}  // namespace osp

#endif  // OSP_APP_HPP_
