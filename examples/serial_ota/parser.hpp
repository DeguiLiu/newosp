/**
 * @file parser.hpp
 * @brief HSM-based protocol frame parser using osp::TableHsm.
 *
 * Modern C++17 rewrite of hsm_parser.c from the reference project.
 * Uses the static transition table (osp::TableHsm) for byte-by-byte frame
 * parsing: every (state, event) pair is one table row; conditional
 * transitions are guard rows; byte side effects are row actions.
 *
 * Protocol frame format:
 *   0xAA | LEN_LO | LEN_HI | CMD_CLASS | CMD | DATA[LEN-2] | CRC_LO | CRC_HI | 0x55
 *
 * States: Idle, LenLo, LenHi, CmdClass, Cmd, Data, CrcLo, CrcHi, Tail
 * Events: kEvtByte, kEvtReset, kEvtTimeout
 */

#ifndef SERIAL_OTA_PARSER_HPP_
#define SERIAL_OTA_PARSER_HPP_

#include "protocol.hpp"

#include "osp/hsm.hpp"
#include "osp/hsm_table.hpp"
#include "osp/log.hpp"

#include <cstdint>
#include <cstring>

namespace ota {

// ============================================================================
// Event IDs
// ============================================================================

static constexpr uint32_t kEvtByte = 1U;
static constexpr uint32_t kEvtReset = 2U;
static constexpr uint32_t kEvtTimeout = 3U;

// ============================================================================
// Parser Statistics
// ============================================================================

struct ParserStats {
  uint32_t bytes_received = 0;
  uint32_t frames_received = 0;
  uint32_t sync_errors = 0;
  uint32_t crc_errors = 0;
  uint32_t tail_errors = 0;
  uint32_t length_errors = 0;
};

// ============================================================================
// Frame Callback
// ============================================================================

using FrameCallback = void (*)(const Frame& frame, void* user_data);

// ============================================================================
// Parser Context
// ============================================================================

// State indices (table rows are constexpr; indices are fixed by table order).
enum ParserState : int32_t {
  kPsIdle = 0,
  kPsLenLo,
  kPsLenHi,
  kPsCmdClass,
  kPsCmd,
  kPsData,
  kPsCrcLo,
  kPsCrcHi,
  kPsTail,
  kPsCount
};

struct ParserContext {
  uint8_t current_byte = 0;
  Frame frame = {};
  uint16_t expected_len = 0;
  uint16_t payload_index = 0;
  uint16_t running_crc = 0;  ///< Incremental CRC over [cmd_class, cmd, data...].
  ParserStats stats = {};
  FrameCallback callback = nullptr;
  void* user_data = nullptr;
};

// ============================================================================
// Row actions and guards (free functions; decisions live in table rows)
// ============================================================================

namespace parser_detail {

// --- Idle actions ---------------------------------------------------------

/// Header byte seen: reset the frame accumulation.
inline void ActStartFrame(ParserContext& ctx, const void* /*data*/) {
  std::memset(&ctx.frame, 0, sizeof(ctx.frame));
  ctx.expected_len = 0;
  ctx.payload_index = 0;
  ctx.running_crc = 0;
}

/// Non-header byte in Idle: a resync candidate.
inline void ActSyncError(ParserContext& ctx, const void* /*data*/) {
  ++ctx.stats.sync_errors;
}

/// Guard: the current byte is the frame header.
inline bool GuardIsHeader(ParserContext& ctx, const void* /*data*/) {
  return kFrameHeader == ctx.current_byte;
}

// --- Length actions -------------------------------------------------------

inline void ActStoreLenLo(ParserContext& ctx, const void* /*data*/) {
  ctx.expected_len = ctx.current_byte;
}

inline void ActStoreLenHi(ParserContext& ctx, const void* /*data*/) {
  ctx.expected_len = static_cast<uint16_t>(ctx.expected_len | static_cast<uint16_t>(ctx.current_byte << 8U));
  // data_len derives from the completed length field so later guards can
  // read it before the Cmd action stores the command byte.
  ctx.frame.data_len = static_cast<uint16_t>(ctx.expected_len - 2U);
}

/// Length invalid: too short or payload larger than the buffer.
inline void ActLengthError(ParserContext& ctx, const void* /*data*/) {
  ++ctx.stats.length_errors;
  OSP_LOG_WARN("OTA_PARSER", "Invalid length: %u", ctx.expected_len);
}

/// Guard: the accumulated length is valid (>= 2 bytes overhead, payload
/// fits). Computed from the pending high byte (guards run before actions).
inline bool GuardLenValid(ParserContext& ctx, const void* /*data*/) {
  const uint16_t len = static_cast<uint16_t>(ctx.expected_len | static_cast<uint16_t>(ctx.current_byte << 8U));
  if (len < 2U) {
    return false;
  }
  return (len - 2U) <= kMaxPayloadLen;
}

// --- Command actions ------------------------------------------------------

inline void ActStoreCmdClass(ParserContext& ctx, const void* /*data*/) {
  ctx.frame.cmd_class = ctx.current_byte;
  ctx.running_crc = Crc16Update(ctx.running_crc, ctx.current_byte);
  ++ctx.payload_index;
}

inline void ActStoreCmd(ParserContext& ctx, const void* /*data*/) {
  ctx.frame.cmd = ctx.current_byte;
  ctx.running_crc = Crc16Update(ctx.running_crc, ctx.current_byte);
  ++ctx.payload_index;
}

/// Guard: the frame carries payload bytes (expected_len > 2 overhead bytes).
inline bool GuardHasPayload(ParserContext& ctx, const void* /*data*/) {
  return ctx.expected_len > 2U;
}

// --- Data actions ---------------------------------------------------------

inline void ActStoreData(ParserContext& ctx, const void* /*data*/) {
  const uint16_t data_offset = static_cast<uint16_t>(ctx.payload_index - 2U);
  ctx.frame.data[data_offset] = ctx.current_byte;
  ctx.running_crc = Crc16Update(ctx.running_crc, ctx.current_byte);
  ++ctx.payload_index;
}

/// Guard: this byte is the last payload byte (pre-store evaluation: guards
/// run before actions, so the +1 accounts for the byte about to be stored).
inline bool GuardPayloadDone(ParserContext& ctx, const void* /*data*/) {
  return (ctx.payload_index + 1U) >= ctx.expected_len;
}

// --- CRC actions ----------------------------------------------------------

inline void ActStoreCrcLo(ParserContext& ctx, const void* /*data*/) {
  ctx.frame.crc = ctx.current_byte;
}

inline void ActStoreCrcHi(ParserContext& ctx, const void* /*data*/) {
  ctx.frame.crc = static_cast<uint16_t>(ctx.frame.crc | static_cast<uint16_t>(ctx.current_byte << 8U));
}

// --- Tail actions ---------------------------------------------------------

inline void ActTailError(ParserContext& ctx, const void* /*data*/) {
  ++ctx.stats.tail_errors;
  OSP_LOG_WARN("OTA_PARSER", "Bad tail: 0x%02X", ctx.current_byte);
}

inline void ActCrcError(ParserContext& ctx, const void* /*data*/) {
  ++ctx.stats.crc_errors;
  OSP_LOG_WARN("OTA_PARSER", "CRC mismatch: calc=0x%04X recv=0x%04X", ctx.running_crc, ctx.frame.crc);
}

inline void ActFrameOk(ParserContext& ctx, const void* /*data*/) {
  ++ctx.stats.frames_received;
  OSP_LOG_DEBUG("OTA_PARSER", "Frame OK: class=0x%02X cmd=0x%02X len=%u", ctx.frame.cmd_class, ctx.frame.cmd,
                ctx.frame.data_len);
  if (ctx.callback != nullptr) {
    ctx.callback(ctx.frame, ctx.user_data);
  }
}

/// Guard: the current byte is the frame tail marker.
inline bool GuardIsTail(ParserContext& ctx, const void* /*data*/) {
  return kFrameTail == ctx.current_byte;
}

/// Guard: the current byte is NOT the frame tail marker.
inline bool GuardNotTail(ParserContext& ctx, const void* /*data*/) {
  return kFrameTail != ctx.current_byte;
}

/// Guard: tail marker present but the CRC does not match.
inline bool GuardTailBadCrc(ParserContext& ctx, const void* /*data*/) {
  return (kFrameTail == ctx.current_byte) && (ctx.running_crc != ctx.frame.crc);
}

/// Guard: the received CRC matches the computed CRC.
inline bool GuardCrcOk(ParserContext& ctx, const void* /*data*/) {
  return ctx.running_crc == ctx.frame.crc;
}

}  // namespace parser_detail

// ============================================================================
// Static transition table
// ============================================================================

namespace parser_detail {

using TD = osp::TransitionDef<ParserContext>;

inline constexpr osp::StateDef<ParserContext> kParserStates[kPsCount] = {
    {"Idle", -1, nullptr, nullptr},     {"LenLo", -1, nullptr, nullptr}, {"LenHi", -1, nullptr, nullptr},
    {"CmdClass", -1, nullptr, nullptr}, {"Cmd", -1, nullptr, nullptr},   {"Data", -1, nullptr, nullptr},
    {"CrcLo", -1, nullptr, nullptr},    {"CrcHi", -1, nullptr, nullptr}, {"Tail", -1, nullptr, nullptr},
};

// Byte path. Rows are scanned in order: guarded rows first, fallback next.
inline constexpr osp::TransitionDef<ParserContext> kParserTrans[] = {
    // Idle + byte: header starts a frame, anything else is a sync error.
    {kPsIdle, kEvtByte, kPsLenLo, osp::TransitionKind::kExternal, ActStartFrame, GuardIsHeader},
    {kPsIdle, kEvtByte, kPsIdle, osp::TransitionKind::kInternal, ActSyncError, nullptr},

    // Reset/timeout always return to Idle (row per state, ordered after the
    // byte rows of the same source state).
    {kPsIdle, kEvtReset, kPsIdle, osp::TransitionKind::kExternal, nullptr, nullptr},
    {kPsIdle, kEvtTimeout, kPsIdle, osp::TransitionKind::kExternal, nullptr, nullptr},

    // LenLo + byte: low length byte.
    {kPsLenLo, kEvtByte, kPsLenHi, osp::TransitionKind::kExternal, ActStoreLenLo, nullptr},
    {kPsLenLo, kEvtReset, kPsIdle, osp::TransitionKind::kExternal, nullptr, nullptr},
    {kPsLenLo, kEvtTimeout, kPsIdle, osp::TransitionKind::kExternal, nullptr, nullptr},

    // LenHi + byte: high length byte; invalid length rejects the frame.
    {kPsLenHi, kEvtByte, kPsCmdClass, osp::TransitionKind::kExternal, ActStoreLenHi, GuardLenValid},
    {kPsLenHi, kEvtByte, kPsIdle, osp::TransitionKind::kExternal, ActLengthError, nullptr},
    {kPsLenHi, kEvtReset, kPsIdle, osp::TransitionKind::kExternal, nullptr, nullptr},
    {kPsLenHi, kEvtTimeout, kPsIdle, osp::TransitionKind::kExternal, nullptr, nullptr},

    // CmdClass + byte.
    {kPsCmdClass, kEvtByte, kPsCmd, osp::TransitionKind::kExternal, ActStoreCmdClass, nullptr},
    {kPsCmdClass, kEvtReset, kPsIdle, osp::TransitionKind::kExternal, nullptr, nullptr},
    {kPsCmdClass, kEvtTimeout, kPsIdle, osp::TransitionKind::kExternal, nullptr, nullptr},

    // Cmd + byte: with payload go to Data, otherwise straight to CRC.
    {kPsCmd, kEvtByte, kPsData, osp::TransitionKind::kExternal, ActStoreCmd, GuardHasPayload},
    {kPsCmd, kEvtByte, kPsCrcLo, osp::TransitionKind::kExternal, ActStoreCmd, nullptr},
    {kPsCmd, kEvtReset, kPsIdle, osp::TransitionKind::kExternal, nullptr, nullptr},
    {kPsCmd, kEvtTimeout, kPsIdle, osp::TransitionKind::kExternal, nullptr, nullptr},

    // Data + byte: accumulate; when the payload is full move to CRC.
    {kPsData, kEvtByte, kPsCrcLo, osp::TransitionKind::kExternal, ActStoreData, GuardPayloadDone},
    {kPsData, kEvtByte, kPsData, osp::TransitionKind::kInternal, ActStoreData, nullptr},
    {kPsData, kEvtReset, kPsIdle, osp::TransitionKind::kExternal, nullptr, nullptr},
    {kPsData, kEvtTimeout, kPsIdle, osp::TransitionKind::kExternal, nullptr, nullptr},

    // CrcLo + byte: low CRC byte.
    {kPsCrcLo, kEvtByte, kPsCrcHi, osp::TransitionKind::kExternal, ActStoreCrcLo, nullptr},
    {kPsCrcLo, kEvtReset, kPsIdle, osp::TransitionKind::kExternal, nullptr, nullptr},
    {kPsCrcLo, kEvtTimeout, kPsIdle, osp::TransitionKind::kExternal, nullptr, nullptr},

    // CrcHi + byte: high CRC byte, frame complete pending tail.
    {kPsCrcHi, kEvtByte, kPsTail, osp::TransitionKind::kExternal, ActStoreCrcHi, nullptr},
    {kPsCrcHi, kEvtReset, kPsIdle, osp::TransitionKind::kExternal, nullptr, nullptr},
    {kPsCrcHi, kEvtTimeout, kPsIdle, osp::TransitionKind::kExternal, nullptr, nullptr},

    // Tail + byte: original order -- bad tail first, then tail-OK/CRC-bad,
    // then full success. One row per outcome, mutually exclusive guards.
    {kPsTail, kEvtByte, kPsIdle, osp::TransitionKind::kExternal, ActTailError, GuardNotTail},
    {kPsTail, kEvtByte, kPsIdle, osp::TransitionKind::kExternal, ActCrcError, GuardTailBadCrc},
    {kPsTail, kEvtByte, kPsIdle, osp::TransitionKind::kExternal, ActFrameOk, GuardCrcOk},
    {kPsTail, kEvtReset, kPsIdle, osp::TransitionKind::kExternal, nullptr, nullptr},
    {kPsTail, kEvtTimeout, kPsIdle, osp::TransitionKind::kExternal, nullptr, nullptr},
};

inline constexpr uint32_t kParserTransCount = sizeof(kParserTrans) / sizeof(kParserTrans[0]);

}  // namespace parser_detail

// ============================================================================
// FrameParser Class
// ============================================================================

class FrameParser final {
 public:
  FrameParser() noexcept
      : ctx_{},
        sm_(ctx_, parser_detail::kParserStates, kPsCount, parser_detail::kParserTrans,
            parser_detail::kParserTransCount) {
    sm_.SetInitialState(kPsIdle);
  }

  FrameParser(const FrameParser&) = delete;
  FrameParser& operator=(const FrameParser&) = delete;

  void SetCallback(FrameCallback callback, void* user_data = nullptr) noexcept {
    ctx_.callback = callback;
    ctx_.user_data = user_data;
  }

  void Start() noexcept { sm_.Start(); }

  void PutByte(uint8_t byte) noexcept {
    ctx_.current_byte = byte;
    ++ctx_.stats.bytes_received;
    sm_.Dispatch(osp::Event{kEvtByte, &byte});
  }

  void PutData(const uint8_t* data, uint32_t len) noexcept {
    for (uint32_t i = 0; i < len; ++i) {
      PutByte(data[i]);
    }
  }

  void Reset() noexcept { sm_.Dispatch(osp::Event{kEvtReset, nullptr}); }

  const ParserStats& GetStats() const noexcept { return ctx_.stats; }
  const char* CurrentStateName() const noexcept { return sm_.CurrentStateName(); }

 private:
  ParserContext ctx_;
  osp::TableHsm<ParserContext, kPsCount, parser_detail::kParserTransCount> sm_;
};

}  // namespace ota

#endif  // SERIAL_OTA_PARSER_HPP_