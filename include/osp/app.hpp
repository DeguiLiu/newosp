/**
 * @file app.hpp
 * @brief Application/Instance two-layer model compatible with original OSP.
 *
 * Provides CApp/CInstance equivalent: message-driven application framework
 * with instance pool, state machine, and event dispatch.
 *
 * Instance embeds an HSM-driven lifecycle state machine:
 *   Root (Alive)
 *   +-- Created          (just constructed)
 *   +-- Initializing     (running init logic)
 *   +-- Ready            (parent state)
 *   |   +-- Idle         (waiting for messages)
 *   |   +-- Processing   (handling a message)
 *   |   +-- Suspended    (temporarily paused)
 *   +-- Error            (parent state)
 *   |   +-- Recoverable  (can retry)
 *   |   +-- Fatal        (must destroy)
 *   +-- Destroying       (cleanup in progress)
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_APP_HPP_
#define OSP_APP_HPP_


#include "osp/hsm.hpp"
#include "osp/mem_pool.hpp"
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
  kAlreadyRunning,
  kNotRunning,
  kInvalidId,
  kQueueFull
};
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
// Instance Coarse State (backward-compatible with uint16_t CurState())
// ============================================================================

enum class InstanceState : uint16_t {
  kCreated      = 0,
  kInitializing = 1,
  kReady        = 2,
  kError        = 3,
  kDestroying   = 4
};

// ============================================================================
// Instance Detailed State (maps 1:1 to HSM leaf states)
// ============================================================================

enum class InstanceDetailedState : uint8_t {
  kCreated      = 0,
  kInitializing = 1,
  kIdle         = 2,
  kProcessing   = 3,
  kSuspended    = 4,
  kRecoverable  = 5,
  kFatal        = 6,
  kDestroying   = 7
};

// ============================================================================
// Instance HSM Events (MISRA 7-2-1: scoped enum)
// ============================================================================

enum class InstanceHsmEvent : uint32_t {
  kInsEvtInitialize   = 0x1001,
  kInsEvtInitDone     = 0x1002,
  kInsEvtInitFail     = 0x1003,
  kInsEvtSuspend      = 0x1004,
  kInsEvtResume       = 0x1005,
  kInsEvtMarkError    = 0x1006,
  kInsEvtRecover      = 0x1007,
  kInsEvtDestroy      = 0x1008,
  kInsEvtMessage      = 0x1009,
  kInsEvtMsgDone      = 0x100A,
};

// ============================================================================
// Instance HSM Context
// ============================================================================

// Forward declaration
class Instance;

inline constexpr uint32_t kInstanceHsmMaxStates = 12;

struct InstanceHsmContext {
  StateMachine<InstanceHsmContext, kInstanceHsmMaxStates>* sm;
  Instance* instance;

  // State indices
  int32_t idx_root;
  int32_t idx_created;
  int32_t idx_initializing;
  int32_t idx_ready;
  int32_t idx_idle;
  int32_t idx_processing;
  int32_t idx_suspended;
  int32_t idx_error;
  int32_t idx_recoverable;
  int32_t idx_fatal;
  int32_t idx_destroying;

  bool error_is_fatal;

  InstanceHsmContext() noexcept
      : sm(nullptr), instance(nullptr),
        idx_root(-1), idx_created(-1), idx_initializing(-1),
        idx_ready(-1), idx_idle(-1), idx_processing(-1),
        idx_suspended(-1), idx_error(-1), idx_recoverable(-1),
        idx_fatal(-1), idx_destroying(-1),
        error_is_fatal(false) {}
};

// ============================================================================
// Instance HSM State Handlers (free functions, detail namespace)
// ============================================================================

namespace detail {
namespace instance_hsm {

using E = InstanceHsmEvent;

inline TransitionResult StateRoot(InstanceHsmContext& ctx,
                                  const Event& event) {
  if (event.id == static_cast<uint32_t>(E::kInsEvtDestroy)) {
    return ctx.sm->RequestTransition(ctx.idx_destroying);
  }
  if (event.id == static_cast<uint32_t>(E::kInsEvtMarkError)) {
    if (ctx.error_is_fatal) {
      return ctx.sm->RequestTransition(ctx.idx_fatal);
    }
    return ctx.sm->RequestTransition(ctx.idx_recoverable);
  }
  return TransitionResult::kUnhandled;
}

inline TransitionResult StateCreated(InstanceHsmContext& ctx,
                                     const Event& event) {
  if (event.id == static_cast<uint32_t>(E::kInsEvtInitialize)) {
    return ctx.sm->RequestTransition(ctx.idx_initializing);
  }
  return TransitionResult::kUnhandled;
}

inline TransitionResult StateInitializing(InstanceHsmContext& ctx,
                                          const Event& event) {
  if (event.id == static_cast<uint32_t>(E::kInsEvtInitDone)) {
    return ctx.sm->RequestTransition(ctx.idx_idle);
  }
  if (event.id == static_cast<uint32_t>(E::kInsEvtInitFail)) {
    ctx.error_is_fatal = false;
    return ctx.sm->RequestTransition(ctx.idx_recoverable);
  }
  return TransitionResult::kUnhandled;
}

inline TransitionResult StateReady(InstanceHsmContext& ctx,
                                   const Event& event) {
  if (event.id == static_cast<uint32_t>(E::kInsEvtSuspend)) {
    return ctx.sm->RequestTransition(ctx.idx_suspended);
  }
  return TransitionResult::kUnhandled;
}

inline TransitionResult StateIdle(InstanceHsmContext& ctx,
                                  const Event& event) {
  if (event.id == static_cast<uint32_t>(E::kInsEvtMessage)) {
    return ctx.sm->RequestTransition(ctx.idx_processing);
  }
  return TransitionResult::kUnhandled;
}

inline TransitionResult StateProcessing(InstanceHsmContext& ctx,
                                        const Event& event) {
  if (event.id == static_cast<uint32_t>(E::kInsEvtMsgDone)) {
    return ctx.sm->RequestTransition(ctx.idx_idle);
  }
  return TransitionResult::kUnhandled;
}

inline TransitionResult StateSuspended(InstanceHsmContext& ctx,
                                       const Event& event) {
  if (event.id == static_cast<uint32_t>(E::kInsEvtResume)) {
    return ctx.sm->RequestTransition(ctx.idx_idle);
  }
  return TransitionResult::kUnhandled;
}

inline TransitionResult StateError(InstanceHsmContext& /*ctx*/,
                                   const Event& /*event*/) {
  return TransitionResult::kUnhandled;
}

inline TransitionResult StateRecoverable(InstanceHsmContext& ctx,
                                         const Event& event) {
  if (event.id == static_cast<uint32_t>(E::kInsEvtRecover)) {
    return ctx.sm->RequestTransition(ctx.idx_idle);
  }
  return TransitionResult::kUnhandled;
}

inline TransitionResult StateFatal(InstanceHsmContext& /*ctx*/,
                                   const Event& /*event*/) {
  return TransitionResult::kUnhandled;
}

inline TransitionResult StateDestroying(InstanceHsmContext& /*ctx*/,
                                        const Event& /*event*/) {
  return TransitionResult::kHandled;
}

}  // namespace instance_hsm
}  // namespace detail


// ============================================================================
// Instance base class (HSM-driven lifecycle, no virtual dispatch)
// ============================================================================

using InstanceSmType = StateMachine<InstanceHsmContext, kInstanceHsmMaxStates>;

/// Instance base class with HSM-driven lifecycle.
///
/// Users derive from Instance and implement OnMessage(). Application<Impl>
/// knows the concrete type at compile time, so no virtual dispatch is needed.
/// OnMessage() is called via static_cast<Impl*>(this)->OnMessage().
class Instance {
 public:
  Instance() noexcept
      : ins_id_(0), response_channel_(nullptr),
        legacy_state_(0), legacy_state_set_(false),
        hsm_initialized_(false) {
    InitHsm();
  }

  /// Non-virtual destructor. Safe because Application<Impl> destroys via
  /// ObjectPool<Impl>::Destroy() which calls the derived destructor directly.
  ~Instance() {
    if (hsm_initialized_) {
      GetHsm()->~StateMachine();
      hsm_initialized_ = false;
    }
  }

  /// Default OnMessage (no-op). Derived classes hide this with their own.
  void OnMessage(uint16_t /*event*/, const void* /*data*/,
                 uint32_t /*len*/) noexcept {}

  // ======================== HSM Lifecycle API ========================

  void Initialize() noexcept {
    DispatchHsmEvent(InstanceHsmEvent::kInsEvtInitialize);
    DispatchHsmEvent(InstanceHsmEvent::kInsEvtInitDone);
  }

  void Suspend() noexcept {
    DispatchHsmEvent(InstanceHsmEvent::kInsEvtSuspend);
  }

  void Resume() noexcept {
    DispatchHsmEvent(InstanceHsmEvent::kInsEvtResume);
  }

  void MarkError(bool fatal) noexcept {
    hsm_ctx_.error_is_fatal = fatal;
    DispatchHsmEvent(InstanceHsmEvent::kInsEvtMarkError);
  }

  void Recover() noexcept {
    DispatchHsmEvent(InstanceHsmEvent::kInsEvtRecover);
  }

  void Destroy() noexcept {
    DispatchHsmEvent(InstanceHsmEvent::kInsEvtDestroy);
  }

  // ======================== State Query ========================

  uint16_t CurState() const noexcept {
    if (legacy_state_set_) {
      return legacy_state_;
    }
    return static_cast<uint16_t>(CoarseState());
  }

  void SetState(uint16_t state) noexcept {
    legacy_state_ = state;
    legacy_state_set_ = true;
  }

  InstanceState CoarseState() const noexcept {
    if (!hsm_initialized_) {
      return static_cast<InstanceState>(legacy_state_);
    }
    const auto* hsm = GetHsm();
    int32_t cur = hsm->CurrentState();
    if (cur == hsm_ctx_.idx_created) return InstanceState::kCreated;
    if (cur == hsm_ctx_.idx_initializing) return InstanceState::kInitializing;
    if (cur == hsm_ctx_.idx_idle ||
        cur == hsm_ctx_.idx_processing ||
        cur == hsm_ctx_.idx_suspended ||
        cur == hsm_ctx_.idx_ready) return InstanceState::kReady;
    if (cur == hsm_ctx_.idx_recoverable ||
        cur == hsm_ctx_.idx_fatal ||
        cur == hsm_ctx_.idx_error) return InstanceState::kError;
    if (cur == hsm_ctx_.idx_destroying) return InstanceState::kDestroying;
    return InstanceState::kCreated;
  }

  InstanceDetailedState DetailedState() const noexcept {
    if (!hsm_initialized_) {
      return InstanceDetailedState::kCreated;
    }
    const auto* hsm = GetHsm();
    int32_t cur = hsm->CurrentState();
    if (cur == hsm_ctx_.idx_created) return InstanceDetailedState::kCreated;
    if (cur == hsm_ctx_.idx_initializing) return InstanceDetailedState::kInitializing;
    if (cur == hsm_ctx_.idx_idle) return InstanceDetailedState::kIdle;
    if (cur == hsm_ctx_.idx_processing) return InstanceDetailedState::kProcessing;
    if (cur == hsm_ctx_.idx_suspended) return InstanceDetailedState::kSuspended;
    if (cur == hsm_ctx_.idx_recoverable) return InstanceDetailedState::kRecoverable;
    if (cur == hsm_ctx_.idx_fatal) return InstanceDetailedState::kFatal;
    if (cur == hsm_ctx_.idx_destroying) return InstanceDetailedState::kDestroying;
    return InstanceDetailedState::kCreated;
  }

  const char* DetailedStateName() const noexcept {
    if (!hsm_initialized_) return "Created";
    return GetHsm()->CurrentStateName();
  }

  bool IsInState(int32_t state_index) const noexcept {
    if (!hsm_initialized_) return false;
    return GetHsm()->IsInState(state_index);
  }

  // ======================== ID and Response ========================

  uint16_t InsId() const noexcept { return ins_id_; }
  void SetInsId(uint16_t id) noexcept { ins_id_ = id; }

  bool Reply(const void* data, uint32_t len) noexcept {
    if (response_channel_ == nullptr) return false;
    response_channel_->Reply(data, len);
    return true;
  }

  bool HasPendingReply() const noexcept {
    return response_channel_ != nullptr;
  }

  void SetResponseChannel(ResponseChannel* ch) noexcept {
    response_channel_ = ch;
  }

  const InstanceHsmContext& HsmContext() const noexcept { return hsm_ctx_; }

  // ======================== HSM Message Dispatch ========================

  /// Called by Application::ProcessOne() before OnMessage().
  void BeginMessage() noexcept {
    DispatchHsmEvent(InstanceHsmEvent::kInsEvtMessage);
  }

  /// Called by Application::ProcessOne() after OnMessage().
  void EndMessage() noexcept {
    DispatchHsmEvent(InstanceHsmEvent::kInsEvtMsgDone);
  }

 private:
  void DispatchHsmEvent(InstanceHsmEvent evt_id) noexcept {
    if (!hsm_initialized_) return;
    Event evt{static_cast<uint32_t>(evt_id), nullptr};
    GetHsm()->Dispatch(evt);
    legacy_state_set_ = false;
  }

 private:
  void InitHsm() noexcept {
    auto* hsm = new (hsm_storage_) InstanceSmType(hsm_ctx_);
    hsm_initialized_ = true;
    hsm_ctx_.sm = hsm;
    hsm_ctx_.instance = this;

    hsm_ctx_.idx_root = hsm->AddState({
        "Root", -1,
        detail::instance_hsm::StateRoot,
        nullptr, nullptr, nullptr});
    hsm_ctx_.idx_created = hsm->AddState({
        "Created", hsm_ctx_.idx_root,
        detail::instance_hsm::StateCreated,
        nullptr, nullptr, nullptr});
    hsm_ctx_.idx_initializing = hsm->AddState({
        "Initializing", hsm_ctx_.idx_root,
        detail::instance_hsm::StateInitializing,
        nullptr, nullptr, nullptr});
    hsm_ctx_.idx_ready = hsm->AddState({
        "Ready", hsm_ctx_.idx_root,
        detail::instance_hsm::StateReady,
        nullptr, nullptr, nullptr});
    hsm_ctx_.idx_idle = hsm->AddState({
        "Idle", hsm_ctx_.idx_ready,
        detail::instance_hsm::StateIdle,
        nullptr, nullptr, nullptr});
    hsm_ctx_.idx_processing = hsm->AddState({
        "Processing", hsm_ctx_.idx_ready,
        detail::instance_hsm::StateProcessing,
        nullptr, nullptr, nullptr});
    hsm_ctx_.idx_suspended = hsm->AddState({
        "Suspended", hsm_ctx_.idx_ready,
        detail::instance_hsm::StateSuspended,
        nullptr, nullptr, nullptr});
    hsm_ctx_.idx_error = hsm->AddState({
        "Error", hsm_ctx_.idx_root,
        detail::instance_hsm::StateError,
        nullptr, nullptr, nullptr});
    hsm_ctx_.idx_recoverable = hsm->AddState({
        "Recoverable", hsm_ctx_.idx_error,
        detail::instance_hsm::StateRecoverable,
        nullptr, nullptr, nullptr});
    hsm_ctx_.idx_fatal = hsm->AddState({
        "Fatal", hsm_ctx_.idx_error,
        detail::instance_hsm::StateFatal,
        nullptr, nullptr, nullptr});
    hsm_ctx_.idx_destroying = hsm->AddState({
        "Destroying", hsm_ctx_.idx_root,
        detail::instance_hsm::StateDestroying,
        nullptr, nullptr, nullptr});

    hsm->SetInitialState(hsm_ctx_.idx_created);
    hsm->Start();
  }

  // MISRA C++ Rule 5-2-4 deviation: reinterpret_cast for placement-new storage
  InstanceSmType* GetHsm() noexcept {
    return reinterpret_cast<InstanceSmType*>(hsm_storage_);  // NOLINT
  }
  const InstanceSmType* GetHsm() const noexcept {
    return reinterpret_cast<const InstanceSmType*>(hsm_storage_);  // NOLINT
  }

  InstanceHsmContext hsm_ctx_;
  uint16_t ins_id_;
  uint16_t legacy_state_;
  bool legacy_state_set_;
  bool hsm_initialized_;
  ResponseChannel* response_channel_;
  alignas(InstanceSmType) uint8_t hsm_storage_[sizeof(InstanceSmType)];
};

// ============================================================================
// Application message - hybrid inline/pointer storage with bit-field packing
// ============================================================================

#ifndef OSP_APP_MSG_INLINE_SIZE
#define OSP_APP_MSG_INLINE_SIZE 48U
#endif

static_assert((OSP_APP_MSG_INLINE_SIZE % 8) == 0,
              "OSP_APP_MSG_INLINE_SIZE must be 8-byte aligned");

struct alignas(8) AppMessage {
  static constexpr uint32_t kMaxDataLen = 0x00FFFFFFU;
  static constexpr uint32_t kLenShift = 8U;
  static constexpr uint32_t kExternalFlag = 0x01U;

  uint16_t ins_id;
  uint16_t event;
  uint32_t flags_and_len;
  ResponseChannel* response_channel;

  union alignas(8) {
    uint8_t  inline_data[OSP_APP_MSG_INLINE_SIZE];
    const void* ext_ptr;
  } payload;

  bool IsExternal() const noexcept {
    return (flags_and_len & kExternalFlag) != 0;
  }

  uint32_t DataLen() const noexcept {
    return flags_and_len >> kLenShift;
  }

  const void* Data() const noexcept {
    return IsExternal() ? payload.ext_ptr : payload.inline_data;
  }

  void Store(const void* data, uint32_t len) noexcept {
    if (data == nullptr || len == 0) {
      flags_and_len = 0;
      return;
    }
    if (len > kMaxDataLen) len = kMaxDataLen;

    if (len <= OSP_APP_MSG_INLINE_SIZE) {
      flags_and_len = (len << kLenShift);
      std::memcpy(payload.inline_data, data, len);
    } else {
      flags_and_len = (len << kLenShift) | kExternalFlag;
      payload.ext_ptr = data;
    }
  }
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

inline constexpr uint32_t kAppNameMaxLen = 31;

// ============================================================================
// Application<InstanceImpl, MaxInstances>
//
// InstanceImpl must derive from Instance and implement:
//   void OnMessage(uint16_t event, const void* data, uint32_t len);
//
// Thread-safety contract:
//   - Post()           : thread-safe (mutex-serialized MPSC producer)
//   - ProcessOne/All() : single-consumer, must be called from one thread only
//   - CreateInstance()  : call during init phase, before ProcessOne() starts
//   - DestroyInstance() : call during shutdown phase, after ProcessOne() stops
//   - If dynamic create/destroy is needed at runtime, caller must externally
//     serialize with ProcessOne() (e.g. same thread or stop-the-world).
//
// All instance memory is managed by ObjectPool -- zero heap allocation.
// ============================================================================

template <typename InstanceImpl, uint16_t MaxInstances = OSP_APP_MAX_INSTANCES>
class Application {
  static_assert(std::is_base_of<Instance, InstanceImpl>::value,
                "InstanceImpl must derive from osp::Instance");

 public:
  Application(uint16_t app_id, const char* name) noexcept
      : app_id_(app_id),
        name_(TruncateToCapacity, name),
        instance_count_(0),
        queue_head_(0),
        queue_tail_(0) {
    for (uint16_t i = 0; i < MaxInstances; ++i) {
      instances_[i] = nullptr;
    }
  }

  ~Application() noexcept {
    for (uint16_t i = 0; i < MaxInstances; ++i) {
      if (instances_[i] != nullptr) {
        pool_.Destroy(instances_[i]);
        instances_[i] = nullptr;
      }
    }
  }

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  expected<uint16_t, AppError> CreateInstance() noexcept {
    for (uint16_t i = 0; i < MaxInstances; ++i) {
      if (instances_[i] == nullptr) {
        InstanceImpl* inst = pool_.Create();
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
    pool_.Destroy(instances_[idx]);
    instances_[idx] = nullptr;
    --instance_count_;
    return expected<void, AppError>::success();
  }

  // Post a message to this application's queue. Thread-safe (MPSC).
  // NOTE: if len > OSP_APP_MSG_INLINE_SIZE, only the pointer is stored —
  // caller must keep the buffer alive until ProcessOne() consumes it.
  bool Post(uint16_t ins_id, uint16_t event, const void* data,
            uint32_t len,
            ResponseChannel* response_ch = nullptr) noexcept {
    std::lock_guard<std::mutex> lock(post_mutex_);
    uint32_t tail = queue_tail_.load(std::memory_order_relaxed);
    uint32_t next_tail = (tail + 1U) % OSP_APP_QUEUE_DEPTH;
    if (next_tail == queue_head_.load(std::memory_order_acquire)) {
      return false;
    }
    AppMessage& msg = queue_[tail];
    msg.ins_id = ins_id;
    msg.event = event;
    msg.flags_and_len = 0;
    msg.response_channel = response_ch;
    msg.Store(data, len);
    queue_tail_.store(next_tail, std::memory_order_release);
    return true;
  }

  bool ProcessOne() noexcept {
    uint32_t head = queue_head_.load(std::memory_order_relaxed);
    if (head == queue_tail_.load(std::memory_order_acquire)) {
      return false;
    }
    const AppMessage& msg = queue_[head];
    uint32_t next_head = (head + 1U) % OSP_APP_QUEUE_DEPTH;

    const void* data = msg.Data();
    uint32_t data_len = msg.DataLen();

    if (msg.ins_id == kInsEach) {
      for (uint16_t i = 0; i < MaxInstances; ++i) {
        if (instances_[i] != nullptr) {
          instances_[i]->SetResponseChannel(msg.response_channel);
          instances_[i]->BeginMessage();
          instances_[i]->OnMessage(msg.event, data, data_len);
          instances_[i]->EndMessage();
          instances_[i]->SetResponseChannel(nullptr);
        }
      }
    } else {
      InstanceImpl* inst = GetInstance(msg.ins_id);
      if (inst != nullptr) {
        inst->SetResponseChannel(msg.response_channel);
        inst->BeginMessage();
        inst->OnMessage(msg.event, data, data_len);
        inst->EndMessage();
        inst->SetResponseChannel(nullptr);
      }
    }
    queue_head_.store(next_head, std::memory_order_release);
    return true;
  }

  uint32_t ProcessAll() noexcept {
    uint32_t count = 0;
    while (ProcessOne()) {
      ++count;
    }
    return count;
  }

  uint16_t AppId() const noexcept { return app_id_; }
  const char* Name() const noexcept { return name_.c_str(); }
  uint32_t InstanceCount() const noexcept { return instance_count_; }

  InstanceImpl* GetInstance(uint16_t ins_id) noexcept {
    if (ins_id == 0 || ins_id > MaxInstances) return nullptr;
    return instances_[ins_id - 1];
  }

  uint32_t PendingMessages() const noexcept {
    uint32_t tail = queue_tail_.load(std::memory_order_acquire);
    uint32_t head = queue_head_.load(std::memory_order_acquire);
    if (tail >= head) {
      return tail - head;
    }
    return OSP_APP_QUEUE_DEPTH - head + tail;
  }

 private:
  uint16_t app_id_;
  FixedString<kAppNameMaxLen> name_;
  ObjectPool<InstanceImpl, MaxInstances> pool_;
  InstanceImpl* instances_[MaxInstances];
  uint32_t instance_count_;
  AppMessage queue_[OSP_APP_QUEUE_DEPTH];
  std::atomic<uint32_t> queue_head_;
  std::atomic<uint32_t> queue_tail_;
  mutable std::mutex post_mutex_;
};

}  // namespace osp

#endif  // OSP_APP_HPP_
