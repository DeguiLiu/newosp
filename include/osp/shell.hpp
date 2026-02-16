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
 * @file shell.hpp
 * @brief Header-only multi-backend debug shell with telnet/console/UART
 *        access, command registration, IAC protocol, ESC sequences,
 *        history navigation, and optional authentication.
 *
 * Provides:
 * - DebugShell:  TCP telnet backend (IAC negotiation, optional auth)
 * - ConsoleShell: stdin/stdout backend (termios raw mode)
 * - UartShell:    Serial port backend (configurable baud rate)
 *
 * All backends share the global command registry, line editor with
 * arrow-key history and Tab completion.
 *
 * Designed for embedded ARM-Linux diagnostics.
 * Requires C++17. Compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_SHELL_HPP_
#define OSP_SHELL_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#if OSP_HAS_NETWORK
#include <arpa/inet.h>
#endif
#include <fcntl.h>
#if OSP_HAS_NETWORK
#include <netinet/in.h>
#endif
#include <poll.h>
#if OSP_HAS_NETWORK
#include <sys/socket.h>
#endif
#include <termios.h>
#include <unistd.h>

// ============================================================================
// Compile-time configuration
// ============================================================================

#ifndef OSP_SHELL_LINE_BUF_SIZE
#define OSP_SHELL_LINE_BUF_SIZE 256
#endif

#ifndef OSP_SHELL_HISTORY_SIZE
#define OSP_SHELL_HISTORY_SIZE 16
#endif

#ifndef OSP_SHELL_MAX_ARGS
#define OSP_SHELL_MAX_ARGS 16
#endif

namespace osp {

// ============================================================================
// Shell Types
// ============================================================================

/// @brief Function pointer signature for shell commands.
using ShellCmdFn = int (*)(int argc, char* argv[]);

/// @brief Descriptor for a registered shell command.
struct ShellCmd {
  const char* name;  ///< Command name (must be a string literal or static).
  const char* desc;  ///< Human-readable description.
  ShellCmdFn func;   ///< Callback to invoke.
};

// ============================================================================
// Forward declarations
// ============================================================================

#if OSP_HAS_NETWORK
class DebugShell;
#endif
class ConsoleShell;
class UartShell;

namespace detail {

// ============================================================================
// I/O function pointer types and backend implementations
// ============================================================================

/// @brief Write function pointer type for backend-agnostic I/O.
using ShellWriteFn = ssize_t (*)(int fd, const void* buf, size_t len);

/// @brief Read function pointer type for backend-agnostic I/O.
using ShellReadFn = ssize_t (*)(int fd, void* buf, size_t len);

#if OSP_HAS_NETWORK
/// @brief TCP write wrapper (send with MSG_NOSIGNAL).
inline ssize_t ShellTcpWrite(int fd, const void* buf, size_t len) {
  return ::send(fd, buf, len, MSG_NOSIGNAL);
}

/// @brief TCP read wrapper (recv).
inline ssize_t ShellTcpRead(int fd, void* buf, size_t len) {
  return ::recv(fd, buf, len, 0);
}
#endif  // OSP_HAS_NETWORK

/// @brief POSIX write wrapper (for Console/UART).
inline ssize_t ShellPosixWrite(int fd, const void* buf, size_t len) {
  return ::write(fd, buf, len);
}

/// @brief POSIX read wrapper (for Console/UART).
inline ssize_t ShellPosixRead(int fd, void* buf, size_t len) {
  return ::read(fd, buf, len);
}

// ============================================================================
// ShellSession - per-connection state (shared by all backends)
// ============================================================================

/// @brief Session state used internally by all shell backends.
struct ShellSession {
  // --- I/O abstraction ---
  int read_fd = -1;             ///< Read file descriptor.
  int write_fd = -1;            ///< Write file descriptor (may differ for Console).
  ShellWriteFn write_fn = nullptr;
  ShellReadFn  read_fn = nullptr;

  // --- Line editing ---
  char line_buf[OSP_SHELL_LINE_BUF_SIZE] = {};
  uint32_t line_pos = 0;

  // --- History ring buffer ---
  char history[OSP_SHELL_HISTORY_SIZE][OSP_SHELL_LINE_BUF_SIZE] = {};
  uint32_t hist_count = 0;      ///< Total entries stored.
  uint32_t hist_head = 0;       ///< Next write position.
  uint32_t hist_browse = 0;     ///< Current browse position.
  bool hist_browsing = false;   ///< Currently navigating history.

  // --- ESC sequence state ---
  enum class EscState : uint8_t { kNone = 0, kEsc, kBracket };
  EscState esc_state = EscState::kNone;

  // --- IAC protocol state (telnet only) ---
  enum class IacState : uint8_t { kNormal = 0, kIac, kNego, kSub };
  IacState iac_state = IacState::kNormal;

  // --- Authentication state ---
  bool authenticated = false;
  uint8_t auth_attempts = 0;

  // --- Control ---
  std::thread thread;
  std::atomic<bool> active{false};
  bool telnet_mode = false;     ///< true = TCP (enable IAC filtering).
  bool skip_next_lf = false;    ///< CRLF dedup (replaces MSG_PEEK).
};

/// @brief Returns a reference to the thread-local current session pointer.
inline ShellSession*& CurrentSession() noexcept {
  static thread_local ShellSession* s = nullptr;
  return s;
}

/// @brief Initialize a ShellSession for a given backend.
///
/// Consolidates the repeated session setup code used by DebugShell,
/// ConsoleShell, and UartShell into a single helper.
inline void ShellSessionInit(ShellSession& s, int read_fd, int write_fd,
                         ShellWriteFn write_fn, ShellReadFn read_fn,
                         bool telnet_mode) noexcept {
  s.read_fd = read_fd;
  s.write_fd = write_fd;
  s.write_fn = write_fn;
  s.read_fn = read_fn;
  s.telnet_mode = telnet_mode;
  s.iac_state = ShellSession::IacState::kNormal;
  s.esc_state = ShellSession::EscState::kNone;
  s.line_pos = 0;
  s.hist_browsing = false;
  s.skip_next_lf = false;
  s.active.store(true, std::memory_order_release);
}

// ============================================================================
// Session I/O helpers
// ============================================================================

/// @brief Write a NUL-terminated string to the session.
inline void ShellSessionWrite(ShellSession& s, const char* str) {
  if (s.write_fn != nullptr && str != nullptr) {
    const size_t len = std::strlen(str);
    if (len > 0) {
      (void)s.write_fn(s.write_fd, str, len);
    }
  }
}

/// @brief Write a buffer of given length to the session.
inline void ShellSessionWrite(ShellSession& s, const void* buf, size_t len) {
  if (s.write_fn != nullptr && len > 0) {
    (void)s.write_fn(s.write_fd, buf, len);
  }
}

// ============================================================================
// History functions
// ============================================================================

/// @brief Replace the current terminal line with new content.
inline void ReplaceLine(ShellSession& s,
                         const char* new_line,
                         uint32_t new_len) {
  // Erase current line on terminal.
  for (uint32_t i = 0; i < s.line_pos; ++i) {
    ShellSessionWrite(s, "\b \b", 3);
  }
  // Display new content.
  if (new_len > 0) {
    ShellSessionWrite(s, new_line, new_len);
  }
  // Update session state.
  if (new_len > 0) {
    std::memcpy(s.line_buf, new_line, new_len);
  }
  s.line_buf[new_len] = '\0';
  s.line_pos = new_len;
}

/// @brief Push the current line into the history ring buffer.
inline void PushHistory(ShellSession& s) {
  if (s.line_pos == 0) return;
  s.line_buf[s.line_pos] = '\0';
  // Skip consecutive duplicates.
  if (s.hist_count > 0) {
    const uint32_t last =
        (s.hist_head + OSP_SHELL_HISTORY_SIZE - 1) % OSP_SHELL_HISTORY_SIZE;
    if (std::strcmp(s.history[last], s.line_buf) == 0) return;
  }
  std::memcpy(s.history[s.hist_head], s.line_buf, s.line_pos + 1);
  s.hist_head = (s.hist_head + 1) % OSP_SHELL_HISTORY_SIZE;
  if (s.hist_count < OSP_SHELL_HISTORY_SIZE) ++s.hist_count;
}

/// @brief Navigate to the previous (older) history entry.
inline void HistoryUp(ShellSession& s) {
  if (s.hist_count == 0) return;
  if (!s.hist_browsing) {
    s.hist_browse = s.hist_head;
    s.hist_browsing = true;
  }
  const uint32_t oldest = (s.hist_count < OSP_SHELL_HISTORY_SIZE)
                               ? 0
                               : s.hist_head;
  if (s.hist_browse == oldest) return;
  s.hist_browse =
      (s.hist_browse + OSP_SHELL_HISTORY_SIZE - 1) % OSP_SHELL_HISTORY_SIZE;
  const uint32_t len =
      static_cast<uint32_t>(std::strlen(s.history[s.hist_browse]));
  ReplaceLine(s, s.history[s.hist_browse], len);
}

/// @brief Navigate to the next (newer) history entry, or back to empty line.
inline void HistoryDown(ShellSession& s) {
  if (!s.hist_browsing) return;
  const uint32_t newest =
      (s.hist_head + OSP_SHELL_HISTORY_SIZE - 1) % OSP_SHELL_HISTORY_SIZE;
  if (s.hist_browse == newest || s.hist_browse == s.hist_head) {
    // Back to current (empty) line.
    s.hist_browsing = false;
    ReplaceLine(s, "", 0);
    return;
  }
  s.hist_browse = (s.hist_browse + 1) % OSP_SHELL_HISTORY_SIZE;
  const uint32_t len =
      static_cast<uint32_t>(std::strlen(s.history[s.hist_browse]));
  ReplaceLine(s, s.history[s.hist_browse], len);
}

// ============================================================================
// IAC protocol byte filter (telnet only)
// ============================================================================

/// @brief Filter IAC protocol bytes from telnet stream.
/// @return The effective character, or '\0' if the byte was consumed.
[[nodiscard]] inline char FilterIac(ShellSession& s, uint8_t byte) noexcept {
  switch (s.iac_state) {
    case ShellSession::IacState::kNormal:
      if (byte == 0xFF) {
        s.iac_state = ShellSession::IacState::kIac;
        return '\0';
      }
      return static_cast<char>(byte);

    case ShellSession::IacState::kIac:
      if (byte >= 0xFB && byte <= 0xFE) {
        // WILL (0xFB), WONT (0xFC), DO (0xFD), DONT (0xFE)
        s.iac_state = ShellSession::IacState::kNego;
        return '\0';
      }
      if (byte == 0xFA) {
        // SB (subnegotiation begin)
        s.iac_state = ShellSession::IacState::kSub;
        return '\0';
      }
      s.iac_state = ShellSession::IacState::kNormal;
      // IAC IAC = literal 0xFF
      return (byte == 0xFF) ? static_cast<char>(0xFF) : '\0';

    case ShellSession::IacState::kNego:
      // Consume the option byte after WILL/WONT/DO/DONT.
      s.iac_state = ShellSession::IacState::kNormal;
      return '\0';

    case ShellSession::IacState::kSub:
      // Consume bytes until IAC SE (0xFF 0xF0).
      if (byte == 0xFF) {
        s.iac_state = ShellSession::IacState::kIac;
      }
      return '\0';
  }
  return '\0';
}

// ============================================================================
// GlobalCmdRegistry - Meyer's singleton command table
// ============================================================================

/**
 * @brief Global command registry for the debug shell.
 *
 * Thread-safe for registration (mutex-protected). Lookup and enumeration are
 * read-only after registration phase completes. Capacity is fixed at compile
 * time (kMaxCommands).
 */
class GlobalCmdRegistry final {
 public:
  /// @brief Return the singleton instance.
  static GlobalCmdRegistry& Instance() noexcept {
    static GlobalCmdRegistry reg;
    return reg;
  }

  /**
   * @brief Register a new shell command.
   * @param name  Command name (must point to static storage).
   * @param fn    Function to invoke.
   * @param desc  Human-readable description (must point to static storage).
   * @return success on OK; ShellError::kRegistryFull or kDuplicateName on
   *         failure.
   */
  [[nodiscard]] inline expected<void, ShellError> Register(const char* name,
                                             ShellCmdFn fn,
                                             const char* desc) noexcept;

  /**
   * @brief Find a command by exact name.
   * @return Pointer to the ShellCmd, or nullptr if not found.
   */
  [[nodiscard]] inline const ShellCmd* Find(const char* name) const noexcept;

  /**
   * @brief Auto-complete a command name prefix.
   *
   * If exactly one match, copies the full name into @p out_buf.
   * If multiple matches, copies the longest common prefix.
   *
   * @param prefix   The prefix to match.
   * @param out_buf  Destination buffer for the completion string.
   * @param buf_size Size of @p out_buf in bytes.
   * @return Number of matching commands.
   */
  [[nodiscard]] inline uint32_t AutoComplete(const char* prefix,
                               char* out_buf,
                               uint32_t buf_size) const noexcept;

  /**
   * @brief Iterate over all registered commands.
   * @param visitor Callback invoked once per command.
   */
  inline void ForEach(
      function_ref<void(const ShellCmd&)> visitor) const noexcept;

  /// @brief Return the number of registered commands.
  [[nodiscard]] uint32_t Count() const noexcept { return count_; }

 private:
  GlobalCmdRegistry() = default;
  GlobalCmdRegistry(const GlobalCmdRegistry&) = delete;
  GlobalCmdRegistry& operator=(const GlobalCmdRegistry&) = delete;

  static constexpr uint32_t kMaxCommands = 64;
  ShellCmd cmds_[kMaxCommands] = {};
  uint32_t count_ = 0;
  mutable std::mutex mtx_;
};

// ----------------------------------------------------------------------------
// GlobalCmdRegistry inline implementation
// ----------------------------------------------------------------------------

inline expected<void, ShellError> GlobalCmdRegistry::Register(
    const char* name,
    ShellCmdFn fn,
    const char* desc) noexcept {
  std::lock_guard<std::mutex> lock(mtx_);
  // Duplicate check
  for (uint32_t i = 0; i < count_; ++i) {
    if (std::strcmp(cmds_[i].name, name) == 0) {
      return expected<void, ShellError>::error(ShellError::kDuplicateName);
    }
  }
  if (count_ >= kMaxCommands) {
    return expected<void, ShellError>::error(ShellError::kRegistryFull);
  }
  cmds_[count_].name = name;
  cmds_[count_].desc = desc;
  cmds_[count_].func = fn;
  ++count_;
  return expected<void, ShellError>::success();
}

inline const ShellCmd* GlobalCmdRegistry::Find(
    const char* name) const noexcept {
  // Read-only scan; safe after registration phase.
  for (uint32_t i = 0; i < count_; ++i) {
    if (std::strcmp(cmds_[i].name, name) == 0) {
      return &cmds_[i];
    }
  }
  return nullptr;
}

inline uint32_t GlobalCmdRegistry::AutoComplete(const char* prefix,
                                                char* out_buf,
                                                uint32_t buf_size) const
    noexcept {
  if (buf_size == 0 || prefix == nullptr || out_buf == nullptr) {
    return 0;
  }

  const uint32_t prefix_len =
      static_cast<uint32_t>(std::strlen(prefix));

  // Collect indices of matching commands.
  uint32_t match_indices[kMaxCommands];
  uint32_t match_count = 0;

  for (uint32_t i = 0; i < count_; ++i) {
    if (std::strncmp(cmds_[i].name, prefix, prefix_len) == 0) {
      match_indices[match_count++] = i;
    }
  }

  if (match_count == 0) {
    out_buf[0] = '\0';
    return 0;
  }

  if (match_count == 1) {
    // Single match -- copy the full name.
    const char* full = cmds_[match_indices[0]].name;
    const uint32_t len = static_cast<uint32_t>(std::strlen(full));
    const uint32_t copy_len = (len < buf_size - 1) ? len : (buf_size - 1);
    std::memcpy(out_buf, full, copy_len);
    out_buf[copy_len] = '\0';
    return 1;
  }

  // Multiple matches -- find the longest common prefix among them.
  const char* first = cmds_[match_indices[0]].name;
  uint32_t common_len = static_cast<uint32_t>(std::strlen(first));

  for (uint32_t m = 1; m < match_count; ++m) {
    const char* other = cmds_[match_indices[m]].name;
    uint32_t j = 0;
    while (j < common_len && first[j] == other[j]) {
      ++j;
    }
    common_len = j;
  }

  const uint32_t copy_len =
      (common_len < buf_size - 1) ? common_len : (buf_size - 1);
  std::memcpy(out_buf, first, copy_len);
  out_buf[copy_len] = '\0';
  return match_count;
}

inline void GlobalCmdRegistry::ForEach(
    function_ref<void(const ShellCmd&)> visitor) const noexcept {
  for (uint32_t i = 0; i < count_; ++i) {
    visitor(cmds_[i]);
  }
}

// ============================================================================
// ShellSplit - In-place command line tokeniser (RT-Thread msh_split port)
// ============================================================================

/// @brief Maximum number of arguments supported per command line.
static constexpr uint32_t kMaxArgs = OSP_SHELL_MAX_ARGS;

/**
 * @brief Split a command line in-place into argc / argv.
 *
 * Replaces whitespace with NUL bytes and fills @p argv with pointers into
 * @p cmd.  Supports double-quoted and single-quoted strings.
 *
 * @param cmd    Mutable command line buffer (modified in-place).
 * @param length Length of @p cmd in bytes (excluding NUL terminator).
 * @param argv   Output array of argument pointers; must hold kMaxArgs entries.
 * @return Number of arguments parsed (argc).
 */
inline int ShellSplit(char* cmd,
                      uint32_t length,
                      char* argv[kMaxArgs]) {
  int argc = 0;
  uint32_t i = 0;

  while (i < length && argc < static_cast<int>(kMaxArgs)) {
    // Skip leading whitespace.
    while (i < length && (cmd[i] == ' ' || cmd[i] == '\t')) {
      cmd[i] = '\0';
      ++i;
    }
    if (i >= length) {
      break;
    }

    if (cmd[i] == '"' || cmd[i] == '\'') {
      // Quoted argument -- consume up to matching quote.
      const char quote = cmd[i];
      cmd[i] = '\0';  // strip opening quote
      ++i;
      if (i >= length) {
        break;
      }
      argv[argc++] = &cmd[i];
      while (i < length && cmd[i] != quote) {
        ++i;
      }
      if (i < length) {
        cmd[i] = '\0';  // strip closing quote
        ++i;
      }
    } else {
      // Unquoted argument.
      argv[argc++] = &cmd[i];
      while (i < length && cmd[i] != ' ' && cmd[i] != '\t') {
        ++i;
      }
    }
  }

  return argc;
}

// ============================================================================
// Tab completion helper
// ============================================================================

/// @brief Handle Tab key press: auto-complete command name.
inline void TabComplete(ShellSession& s, const char* prompt) noexcept {
  s.line_buf[s.line_pos] = '\0';
  char completion[64] = {};
  const uint32_t matches = GlobalCmdRegistry::Instance().AutoComplete(
      s.line_buf, completion, sizeof(completion));

  if (matches == 1) {
    uint32_t comp_len = static_cast<uint32_t>(std::strlen(completion));
    if (comp_len >= OSP_SHELL_LINE_BUF_SIZE - 1) {
      comp_len = OSP_SHELL_LINE_BUF_SIZE - 2;
    }
    // Clear current line and replace with completion + space.
    for (uint32_t i = 0; i < s.line_pos; ++i) {
      ShellSessionWrite(s, "\b \b", 3);
    }
    std::memcpy(s.line_buf, completion, comp_len);
    s.line_buf[comp_len] = ' ';
    s.line_pos = comp_len + 1;
    s.line_buf[s.line_pos] = '\0';
    ShellSessionWrite(s, s.line_buf, s.line_pos);
  } else if (matches > 1) {
    // Show all matching commands.
    ShellSessionWrite(s, "\r\n");
    GlobalCmdRegistry::Instance().ForEach([&](const ShellCmd& cmd) {
      if (std::strncmp(cmd.name, s.line_buf, s.line_pos) == 0) {
        ShellSessionWrite(s, cmd.name);
        ShellSessionWrite(s, "  ");
      }
    });
    ShellSessionWrite(s, "\r\n");
    // Re-display prompt and fill with longest common prefix.
    ShellSessionWrite(s, prompt);
    uint32_t comp_len = static_cast<uint32_t>(std::strlen(completion));
    if (comp_len >= OSP_SHELL_LINE_BUF_SIZE - 1) {
      comp_len = OSP_SHELL_LINE_BUF_SIZE - 2;
    }
    std::memcpy(s.line_buf, completion, comp_len);
    s.line_pos = comp_len;
    s.line_buf[s.line_pos] = '\0';
    ShellSessionWrite(s, s.line_buf, s.line_pos);
  }
}

// ============================================================================
// ProcessByte - Unified byte handler (IAC + ESC + line editing)
// ============================================================================

/**
 * @brief Process a single input byte through IAC filter, ESC FSM, and
 *        line editing logic.
 * @return true if the line is ready for execution (Enter pressed).
 */
[[nodiscard]] inline bool ProcessByte(ShellSession& s,
                         uint8_t raw_byte,
                         const char* prompt) noexcept {
  // IAC filtering (telnet mode only).
  char ch;
  if (s.telnet_mode) {
    ch = FilterIac(s, raw_byte);
    if (ch == '\0') return false;
  } else {
    ch = static_cast<char>(raw_byte);
  }

  // CRLF dedup: skip \n after \r.
  if (s.skip_next_lf) {
    s.skip_next_lf = false;
    if (ch == '\n') return false;
  }

  // ESC sequence state machine.
  switch (s.esc_state) {
    case ShellSession::EscState::kEsc:
      if (ch == '[') {
        s.esc_state = ShellSession::EscState::kBracket;
        return false;
      }
      s.esc_state = ShellSession::EscState::kNone;
      return false;

    case ShellSession::EscState::kBracket:
      s.esc_state = ShellSession::EscState::kNone;
      switch (ch) {
        case 'A': HistoryUp(s); return false;
        case 'B': HistoryDown(s); return false;
        case 'C': return false;  // Right arrow (reserved)
        case 'D': return false;  // Left arrow (reserved)
        default: return false;
      }

    case ShellSession::EscState::kNone:
      break;
  }

  // Control characters.
  if (ch == '\x1b') {
    s.esc_state = ShellSession::EscState::kEsc;
    return false;
  }

  if (ch == 0x03) {
    // Ctrl+C: cancel current line.
    ShellSessionWrite(s, "^C\r\n");
    s.line_pos = 0;
    s.line_buf[0] = '\0';
    s.hist_browsing = false;
    ShellSessionWrite(s, prompt);
    return false;
  }

  if (ch == 0x04) {
    // Ctrl+D: EOF (close session if line is empty).
    if (s.line_pos == 0) {
      s.active.store(false, std::memory_order_release);
      return false;
    }
  }

  if (ch == 0x7F || ch == 0x08) {
    // Backspace.
    if (s.line_pos > 0) {
      --s.line_pos;
      ShellSessionWrite(s, "\b \b", 3);
    }
    return false;
  }

  if (ch == '\t') {
    TabComplete(s, prompt);
    return false;
  }

  if (ch == '\r' || ch == '\n') {
    ShellSessionWrite(s, "\r\n");
    if (ch == '\r') s.skip_next_lf = true;
    s.hist_browsing = false;
    return true;
  }

  // Printable characters.
  if (ch >= 0x20 &&
      s.line_pos < static_cast<uint32_t>(OSP_SHELL_LINE_BUF_SIZE - 1)) {
    s.line_buf[s.line_pos++] = ch;
    ShellSessionWrite(s, &ch, 1);
  }
  return false;
}

// ============================================================================
// ShellExecuteLine - Parse and dispatch the current line buffer
// ============================================================================

/// @brief Parse the line buffer, push to history, and dispatch the command.
inline void ShellExecuteLine(ShellSession& s) {
  PushHistory(s);
  s.line_buf[s.line_pos] = '\0';
  char* argv[kMaxArgs] = {};
  int argc = ShellSplit(s.line_buf, s.line_pos, argv);
  if (argc == 0) return;

  const ShellCmd* cmd = GlobalCmdRegistry::Instance().Find(argv[0]);
  if (cmd != nullptr) {
    CurrentSession() = &s;
    cmd->func(argc, argv);
    CurrentSession() = nullptr;
  } else {
    char msg[160];
    int n = std::snprintf(msg, sizeof(msg),
                          "unknown command: %s\r\n", argv[0]);
    if (n > 0) {
      ShellSessionWrite(s, msg, static_cast<size_t>(n));
    }
  }
}

}  // namespace detail

// ============================================================================
// ShellAutoReg - Static auto-registration helper
// ============================================================================

/**
 * @brief Statically registers a shell command at program startup.
 *
 * Typically used via the OSP_SHELL_CMD macro rather than directly.
 */
class ShellAutoReg {
 public:
  ShellAutoReg(const char* name, ShellCmdFn func, const char* desc) {
    (void)detail::GlobalCmdRegistry::Instance().Register(name, func, desc);
  }
};

/**
 * @brief Register a function as a shell command.
 *
 * The function must have the signature `int cmd(int argc, char* argv[])`.
 *
 * Example:
 * @code
 *   static int reboot(int argc, char* argv[]) { ... }
 *   OSP_SHELL_CMD(reboot, "Reboot the system");
 * @endcode
 */
#define OSP_SHELL_CMD(cmd, desc) \
  static ::osp::ShellAutoReg OSP_CONCAT(_shell_reg_, cmd)(#cmd, cmd, desc)

// ============================================================================
// Built-in "help" command
// ============================================================================

namespace detail {

/**
 * @brief Built-in "help" command that lists all registered commands.
 */
inline int ShellBuiltinHelp(int /*argc*/, char* /*argv*/[]) {
  ShellSession* sess = CurrentSession();
  if (sess == nullptr) return -1;
  char buf[256];
  GlobalCmdRegistry::Instance().ForEach([&](const ShellCmd& cmd) {
    int n = std::snprintf(buf, sizeof(buf), "  %-16s - %s\r\n",
                          cmd.name, cmd.desc ? cmd.desc : "");
    if (n > 0) {
      ShellSessionWrite(*sess, buf, static_cast<size_t>(n));
    }
  });
  return 0;
}

/// @brief Auto-register the built-in help command.
inline bool RegisterHelpOnce() noexcept {
  static const bool done = []() {
    (void)GlobalCmdRegistry::Instance().Register("help", ShellBuiltinHelp,
                                           "List all commands");
    return true;
  }();
  return done;
}

/// @brief Trigger help registration at static-init time.
static const bool kHelpRegistered OSP_UNUSED = RegisterHelpOnce();

}  // namespace detail

// ============================================================================
// ShellPrintf - Backend-agnostic printf into the current session
// ============================================================================

/**
 * @brief Printf into the current session's output.
 *
 * May only be called from within a command callback.  Uses a thread-local
 * session pointer set by the shell before command dispatch.  Works with
 * any backend (TCP, Console, UART) via the session's write_fn.
 *
 * @param fmt printf-style format string.
 * @return Number of bytes written, or -1 on error.
 */
inline int ShellPrintf(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

inline int ShellPrintf(const char* fmt, ...) {
  detail::ShellSession* sess = detail::CurrentSession();
  if (sess == nullptr || sess->write_fn == nullptr) {
    return -1;
  }

  char buf[256];
  va_list args;
  va_start(args, fmt);
  int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  if (n > 0) {
    int to_send = (n < static_cast<int>(sizeof(buf)))
                      ? n
                      : static_cast<int>(sizeof(buf) - 1);
    ssize_t sent = sess->write_fn(sess->write_fd, buf,
                                  static_cast<size_t>(to_send));
    return static_cast<int>(sent);
  }
  return -1;
}

#if OSP_HAS_NETWORK
// ============================================================================
// DebugShell - TCP telnet debug server
// ============================================================================

/**
 * @brief Lightweight telnet debug shell with IAC protocol, authentication,
 *        arrow-key history, and Tab completion.
 *
 * Listens on a configurable TCP port and accepts up to @p max_connections
 * concurrent telnet sessions.  Each session runs in its own thread, reading
 * commands interactively and dispatching them through the global command
 * registry.
 *
 * Typical usage:
 * @code
 *   osp::DebugShell::Config cfg;
 *   cfg.port = 5090;
 *   osp::DebugShell shell(cfg);
 *   shell.Start();
 *   // ... application runs ...
 *   shell.Stop();
 * @endcode
 */
class DebugShell final {
 public:
  /// @brief Shell configuration.
  struct Config {
    uint16_t port;            ///< TCP listen port.
    uint32_t max_connections; ///< Maximum concurrent sessions.
    const char* prompt;       ///< Prompt string sent to clients.
    const char* banner;       ///< Banner shown on connect (nullptr = none).
    const char* username;     ///< Auth username (nullptr = no auth).
    const char* password;     ///< Auth password.

    Config() noexcept
        : port(5090),
          max_connections(2),
          prompt("osp> "),
          banner(nullptr),
          username(nullptr),
          password(nullptr) {}
  };

  /**
   * @brief Construct a debug shell with the given configuration.
   * @param cfg Shell configuration (default: port 5090, 2 sessions).
   */
  explicit DebugShell(const Config& cfg = Config{}) : cfg_(cfg) {
    // Ensure built-in help is registered.
    detail::RegisterHelpOnce();
  }

  ~DebugShell() { Stop(); }

  DebugShell(const DebugShell&) = delete;
  DebugShell& operator=(const DebugShell&) = delete;

  /**
   * @brief Start the telnet server.
   *
   * Creates a TCP listening socket and spawns the accept thread.
   *
   * @return success on OK; ShellError::kPortInUse if bind fails;
   *         ShellError::kAlreadyRunning if already started.
   */
  [[nodiscard]] inline expected<void, ShellError> Start();

  /**
   * @brief Stop the telnet server and close all sessions.
   *
   * Joins the accept thread and all session threads. Safe to call even if
   * the shell is not running.
   */
  inline void Stop();

  /**
   * @brief Printf into the current session.
   *
   * May only be called from within a command callback. Uses a thread-local
   * session pointer set by the shell before command dispatch.
   * Works with all backends (TCP, Console, UART).
   *
   * @param fmt printf-style format string.
   * @return Number of bytes written, or -1 on error.
   */
  static inline int Printf(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
      __attribute__((format(printf, 1, 2)))
#endif
      ;

  /// @brief Check whether the shell is currently running.
  [[nodiscard]] bool IsRunning() const noexcept {
    return running_.load(std::memory_order_relaxed);
  }

  /** @brief Set heartbeat for external watchdog monitoring (accept thread). */
  void SetHeartbeat(ThreadHeartbeat* hb) noexcept { heartbeat_ = hb; }

 private:
  using Session = detail::ShellSession;

  Config cfg_;
  int listen_fd_ = -1;
  std::thread accept_thread_;
  Session* sessions_ = nullptr;
  std::atomic<bool> running_{false};
  ThreadHeartbeat* heartbeat_{nullptr};

  /// @brief Accept loop (runs in accept_thread_).
  inline void AcceptLoop();

  /// @brief Per-session interactive loop.
  inline void SessionLoop(Session& s);

  /// @brief Run authentication flow for a session.
  /// @return true if authenticated, false if failed.
  inline bool RunAuth(Session& s);

  /// @brief Send a telnet IAC command.
  static inline void SendIac(int fd, uint8_t cmd, uint8_t option) {
    uint8_t buf[3] = {0xFF, cmd, option};
    (void)::send(fd, buf, 3, MSG_NOSIGNAL);
  }
};

// ============================================================================
// DebugShell inline implementation
// ============================================================================

inline expected<void, ShellError> DebugShell::Start() {
  if (running_.load(std::memory_order_acquire)) {
    return expected<void, ShellError>::error(ShellError::kAlreadyRunning);
  }

  // Create TCP socket.
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return expected<void, ShellError>::error(ShellError::kPortInUse);
  }

  // RAII guard: close listen_fd_ on any error path below.
  ScopeGuard fd_guard(FixedFunction<void()>([this]() {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }));

  // Allow address reuse.
  int opt = 1;
  (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt,
                      sizeof(opt));

  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(cfg_.port);

  if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) < 0) {
    return expected<void, ShellError>::error(ShellError::kPortInUse);
  }

  if (::listen(listen_fd_, static_cast<int>(cfg_.max_connections)) < 0) {
    return expected<void, ShellError>::error(ShellError::kPortInUse);
  }

  // MISRA C++ Rule 18-4-1 deviation: dynamic allocation required because
  // max_connections is a runtime config value. ShellSession contains
  // std::thread (non-trivially-copyable), precluding FixedVector.
  // Allocated once at Start(), freed at Stop() -- cold path only.
  sessions_ = new Session[cfg_.max_connections];

  running_.store(true, std::memory_order_release);
  accept_thread_ = std::thread([this]() { AcceptLoop(); });

  fd_guard.release();  // Success -- keep the fd open.
  return expected<void, ShellError>::success();
}

inline void DebugShell::Stop() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  running_.store(false, std::memory_order_release);

  // Shutdown the listening socket to unblock poll()/accept() in AcceptLoop.
  // Do NOT close() or modify listen_fd_ here -- AcceptLoop reads it concurrently.
  // The actual close happens after accept_thread_.join() below.
  if (listen_fd_ >= 0) {
    (void)::shutdown(listen_fd_, SHUT_RDWR);
  }

  // Shutdown all active session sockets to unblock their recv().
  // Use shutdown() instead of close() -- POSIX requires that close() on an fd
  // used concurrently by another thread is undefined behavior.  shutdown()
  // signals the socket without closing the fd, letting the session thread's
  // ScopeGuard perform the actual close().
  if (sessions_ != nullptr) {
    for (uint32_t i = 0; i < cfg_.max_connections; ++i) {
      // Snapshot the fd before signaling shutdown -- session thread's
      // ScopeGuard may clear read_fd after we set active=false.
      int fd = sessions_[i].read_fd;
      sessions_[i].active.store(false, std::memory_order_release);
      if (fd >= 0) {
        (void)::shutdown(fd, SHUT_RDWR);
      }
    }
  }

  // Join the accept thread.
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }

  // Now safe to close listen_fd_ (accept thread has exited).
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }

  // Join all session threads (each thread closes its own fd via ScopeGuard).
  if (sessions_ != nullptr) {
    for (uint32_t i = 0; i < cfg_.max_connections; ++i) {
      if (sessions_[i].thread.joinable()) {
        sessions_[i].thread.join();
      }
    }
    delete[] sessions_;
    sessions_ = nullptr;
  }
}

inline void DebugShell::AcceptLoop() {
  while (running_.load(std::memory_order_relaxed)) {
    if (heartbeat_ != nullptr) { heartbeat_->Beat(); }

    // Use poll() to avoid blocking forever in accept().
    // close(listen_fd_) in Stop() does NOT reliably unblock accept() on Linux.
    struct pollfd pfd;
    pfd.fd = listen_fd_;
    pfd.events = POLLIN;
    int pr = ::poll(&pfd, 1, 200);
    if (pr == 0) continue;            // Timeout -- re-check running_ flag.
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;                          // Fatal poll error.
    }

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_fd = ::accept(listen_fd_,
                             reinterpret_cast<struct sockaddr*>(&client_addr),
                             &addr_len);
    if (client_fd < 0) {
      // accept() returns -1 when listen_fd_ is closed during Stop().
      break;
    }

    if (!running_.load(std::memory_order_relaxed)) {
      ::close(client_fd);
      break;
    }

    // Find a free session slot.
    bool placed = false;
    for (uint32_t i = 0; i < cfg_.max_connections; ++i) {
      if (!sessions_[i].active.load(std::memory_order_acquire)) {
        // Ensure any previous thread is joined before reuse.
        if (sessions_[i].thread.joinable()) {
          sessions_[i].thread.join();
        }
        // Initialize session for TCP telnet.
        detail::ShellSessionInit(sessions_[i], client_fd, client_fd,
                            detail::ShellTcpWrite, detail::ShellTcpRead, true);
        sessions_[i].authenticated = (cfg_.username == nullptr);
        sessions_[i].auth_attempts = 0;
        sessions_[i].active.store(true, std::memory_order_release);
        sessions_[i].thread =
            std::thread([this, i]() { SessionLoop(sessions_[i]); });
        placed = true;
        break;
      }
    }

    if (!placed) {
      // No session slots available -- reject the connection.
      const char* msg = "Too many connections.\r\n";
      (void)::send(client_fd, msg, std::strlen(msg), MSG_NOSIGNAL);
      ::close(client_fd);
    }
  }
}

inline bool DebugShell::RunAuth(Session& s) {
  static constexpr uint8_t kMaxAuthAttempts = 3;
  char user_buf[64] = {};
  char pass_buf[64] = {};

  while (s.auth_attempts < kMaxAuthAttempts &&
         s.active.load(std::memory_order_acquire) &&
         running_.load(std::memory_order_relaxed)) {
    // Username prompt.
    detail::ShellSessionWrite(s, "Username: ");
    uint32_t upos = 0;
    while (upos < sizeof(user_buf) - 1) {
      uint8_t byte;
      ssize_t n = s.read_fn(s.read_fd, &byte, 1);
      if (n <= 0) return false;
      // Filter IAC bytes.
      char ch = detail::FilterIac(s, byte);
      if (ch == '\0') continue;
      if (ch == '\r' || ch == '\n') {
        if (ch == '\r') s.skip_next_lf = true;
        break;
      }
      if (s.skip_next_lf) {
        s.skip_next_lf = false;
        if (ch == '\n') continue;
      }
      if (ch == 0x7F || ch == 0x08) {
        if (upos > 0) {
          --upos;
          detail::ShellSessionWrite(s, "\b \b", 3);
        }
        continue;
      }
      if (ch >= 0x20) {
        user_buf[upos++] = ch;
        detail::ShellSessionWrite(s, &ch, 1);
      }
    }
    user_buf[upos] = '\0';
    detail::ShellSessionWrite(s, "\r\n");

    // Password prompt (masked with '*').
    detail::ShellSessionWrite(s, "Password: ");
    uint32_t ppos = 0;
    while (ppos < sizeof(pass_buf) - 1) {
      uint8_t byte;
      ssize_t n = s.read_fn(s.read_fd, &byte, 1);
      if (n <= 0) return false;
      char ch = detail::FilterIac(s, byte);
      if (ch == '\0') continue;
      if (ch == '\r' || ch == '\n') {
        if (ch == '\r') s.skip_next_lf = true;
        break;
      }
      if (s.skip_next_lf) {
        s.skip_next_lf = false;
        if (ch == '\n') continue;
      }
      if (ch == 0x7F || ch == 0x08) {
        if (ppos > 0) {
          --ppos;
          detail::ShellSessionWrite(s, "\b \b", 3);
        }
        continue;
      }
      if (ch >= 0x20) {
        pass_buf[ppos++] = ch;
        detail::ShellSessionWrite(s, "*");  // Mask password.
      }
    }
    pass_buf[ppos] = '\0';
    detail::ShellSessionWrite(s, "\r\n");

    // Verify credentials.
    if (std::strcmp(user_buf, cfg_.username) == 0 &&
        std::strcmp(pass_buf, cfg_.password) == 0) {
      s.authenticated = true;
      detail::ShellSessionWrite(s, "\r\nAuthenticated.\r\n\r\n");
      return true;
    }
    ++s.auth_attempts;
    detail::ShellSessionWrite(s, "\r\nLogin incorrect.\r\n\r\n");
  }
  detail::ShellSessionWrite(s, "Too many attempts. Disconnecting.\r\n");
  return false;
}

inline void DebugShell::SessionLoop(Session& s) {
  // RAII cleanup: close socket and mark session inactive on any exit path.
  // Note: read_fd is NOT set to -1 here to avoid a data race with Stop().
  // Stop() calls shutdown(read_fd) to unblock recv(), then join() ensures
  // the session thread has exited before cleaning up.
  OSP_SCOPE_EXIT(
    if (s.read_fd >= 0) {
      ::close(s.read_fd);
    }
    s.active.store(false, std::memory_order_release);
  );

  // Telnet IAC negotiation: WILL SGA + WILL ECHO.
  SendIac(s.read_fd, 0xFB, 0x03);  // WILL Suppress Go Ahead
  SendIac(s.read_fd, 0xFB, 0x01);  // WILL Echo

  // Send banner if configured.
  if (cfg_.banner != nullptr) {
    detail::ShellSessionWrite(s, cfg_.banner);
  }

  // Run authentication if configured.
  if (!s.authenticated) {
    if (!RunAuth(s)) {
      return;  // ScopeGuard handles cleanup.
    }
  }

  // Send initial prompt.
  detail::ShellSessionWrite(s, cfg_.prompt);

  while (running_.load(std::memory_order_relaxed) &&
         s.active.load(std::memory_order_acquire)) {
    // Use poll() with timeout so the thread can check running_ periodically
    // even if no data arrives.  Pure blocking recv() can hang during shutdown
    // if the client disconnects while the kernel hasn't signaled the server fd.
    struct pollfd pfd;
    pfd.fd = s.read_fd;
    pfd.events = POLLIN;
    int pr = ::poll(&pfd, 1, 200);
    if (pr == 0) continue;  // Timeout -- re-check running_ flag.
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }

    uint8_t byte;
    ssize_t n = s.read_fn(s.read_fd, &byte, 1);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;

    if (detail::ProcessByte(s, byte, cfg_.prompt)) {
      detail::ShellExecuteLine(s);
      s.line_pos = 0;
      s.line_buf[0] = '\0';
      if (s.active.load(std::memory_order_acquire)) {
        detail::ShellSessionWrite(s, cfg_.prompt);
      }
    }
  }
  // ScopeGuard handles cleanup on all exit paths.
}

inline int DebugShell::Printf(const char* fmt, ...) {
  detail::ShellSession* sess = detail::CurrentSession();
  if (sess == nullptr || sess->write_fn == nullptr) {
    return -1;
  }

  char buf[256];
  va_list args;
  va_start(args, fmt);
  int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  if (n > 0) {
    int to_send = (n < static_cast<int>(sizeof(buf)))
                      ? n
                      : static_cast<int>(sizeof(buf) - 1);
    ssize_t sent = sess->write_fn(sess->write_fd, buf,
                                   static_cast<size_t>(to_send));
    return static_cast<int>(sent);
  }
  return -1;
}
#endif  // OSP_HAS_NETWORK

// ============================================================================
// ConsoleShell - stdin/stdout backend
// ============================================================================

/**
 * @brief Interactive console shell using stdin/stdout.
 *
 * Configures the terminal to raw mode for character-by-character input.
 * Restores the original terminal settings on Stop().
 */
class ConsoleShell final {
 public:
  struct Config {
    const char* prompt;
    int read_fd;
    int write_fd;
    bool raw_mode;

    Config() noexcept
        : prompt("osp> "),
          read_fd(STDIN_FILENO),
          write_fd(STDOUT_FILENO),
          raw_mode(true) {}
  };

  explicit ConsoleShell(const Config& cfg = Config{}) : cfg_(cfg) {
    detail::RegisterHelpOnce();
  }

  ~ConsoleShell() { Stop(); }

  ConsoleShell(const ConsoleShell&) = delete;
  ConsoleShell& operator=(const ConsoleShell&) = delete;

  /// @brief Start the console shell asynchronously.
  [[nodiscard]] inline expected<void, ShellError> Start() noexcept;

  /// @brief Stop the console shell and restore terminal.
  inline void Stop() noexcept;

  /// @brief Run the shell synchronously (blocking).
  inline void Run() noexcept;

  [[nodiscard]] bool IsRunning() const noexcept {
    return running_.load(std::memory_order_relaxed);
  }

 private:
  Config cfg_;
  detail::ShellSession session_ = {};
  std::thread thread_;
  std::atomic<bool> running_{false};
  struct termios orig_termios_ = {};
  bool termios_saved_ = false;

  inline void SetRawMode() noexcept;
  inline void RestoreTermios() noexcept;
  inline void RunLoop() noexcept;
};

// ----------------------------------------------------------------------------
// ConsoleShell inline implementation
// ----------------------------------------------------------------------------

inline void ConsoleShell::SetRawMode() noexcept {
  if (!cfg_.raw_mode) return;
  if (::tcgetattr(cfg_.read_fd, &orig_termios_) != 0) return;
  termios_saved_ = true;

  struct termios raw = orig_termios_;
  raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | ICRNL | INLCR |
                                          IGNCR);
  raw.c_oflag &= ~static_cast<tcflag_t>(OPOST);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  (void)::tcsetattr(cfg_.read_fd, TCSANOW, &raw);
}

inline void ConsoleShell::RestoreTermios() noexcept {
  if (termios_saved_) {
    (void)::tcsetattr(cfg_.read_fd, TCSANOW, &orig_termios_);
    termios_saved_ = false;
  }
}

inline expected<void, ShellError> ConsoleShell::Start() noexcept {
  if (running_.load(std::memory_order_relaxed)) {
    return expected<void, ShellError>::error(ShellError::kAlreadyRunning);
  }

  SetRawMode();

  detail::ShellSessionInit(session_, cfg_.read_fd, cfg_.write_fd,
                       detail::ShellPosixWrite, detail::ShellPosixRead, false);

  running_.store(true, std::memory_order_release);
  thread_ = std::thread([this]() { RunLoop(); });

  return expected<void, ShellError>::success();
}

inline void ConsoleShell::Stop() noexcept {
  if (!running_.load(std::memory_order_relaxed)) return;
  running_.store(false, std::memory_order_release);
  session_.active.store(false, std::memory_order_release);

  if (thread_.joinable()) {
    thread_.join();
  }
  RestoreTermios();
}

inline void ConsoleShell::Run() noexcept {
  SetRawMode();

  detail::ShellSessionInit(session_, cfg_.read_fd, cfg_.write_fd,
                       detail::ShellPosixWrite, detail::ShellPosixRead, false);
  running_.store(true, std::memory_order_release);

  RunLoop();

  RestoreTermios();
  running_.store(false, std::memory_order_release);
}

inline void ConsoleShell::RunLoop() noexcept {
  auto& s = session_;
  detail::ShellSessionWrite(s, cfg_.prompt);

  while (running_.load(std::memory_order_relaxed) &&
         s.active.load(std::memory_order_acquire)) {
    struct pollfd pfd;
    pfd.fd = s.read_fd;
    pfd.events = POLLIN;
    int pr = ::poll(&pfd, 1, 100);
    if (pr == 0) continue;
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }

    uint8_t byte;
    ssize_t n = s.read_fn(s.read_fd, &byte, 1);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      break;
    }

    if (detail::ProcessByte(s, byte, cfg_.prompt)) {
      detail::ShellExecuteLine(s);
      s.line_pos = 0;
      s.line_buf[0] = '\0';
      if (s.active.load(std::memory_order_acquire)) {
        detail::ShellSessionWrite(s, cfg_.prompt);
      }
    }
  }
}

// ============================================================================
// UartShell - Serial port backend
// ============================================================================

/**
 * @brief UART serial port shell backend.
 *
 * Opens a serial device (e.g., /dev/ttyS0, /dev/ttyUSB0) and provides
 * interactive command-line access over a serial connection.
 */
class UartShell final {
 public:
  struct Config {
    const char* device;
    uint32_t baudrate;
    const char* prompt;
    int override_fd;  ///< For testing: use this fd instead of opening device.

    Config() noexcept
        : device("/dev/ttyS0"),
          baudrate(115200),
          prompt("osp> "),
          override_fd(-1) {}
  };

  explicit UartShell(const Config& cfg = Config{}) : cfg_(cfg) {
    detail::RegisterHelpOnce();
  }

  ~UartShell() { Stop(); }

  UartShell(const UartShell&) = delete;
  UartShell& operator=(const UartShell&) = delete;

  /// @brief Start the UART shell.
  [[nodiscard]] inline expected<void, ShellError> Start() noexcept;

  /// @brief Stop the UART shell.
  inline void Stop() noexcept;

  [[nodiscard]] bool IsRunning() const noexcept {
    return running_.load(std::memory_order_relaxed);
  }

 private:
  Config cfg_;
  detail::ShellSession session_ = {};
  std::thread thread_;
  std::atomic<bool> running_{false};
  int uart_fd_ = -1;
  bool owns_fd_ = false;

  inline void RunLoop() noexcept;

  static constexpr speed_t BaudToSpeed(uint32_t baud) noexcept {
    switch (baud) {
      case 9600:   return B9600;
      case 19200:  return B19200;
      case 38400:  return B38400;
      case 57600:  return B57600;
      case 115200: return B115200;
      default:     return B115200;
    }
  }
};

// ----------------------------------------------------------------------------
// UartShell inline implementation
// ----------------------------------------------------------------------------

inline expected<void, ShellError> UartShell::Start() noexcept {
  if (running_.load(std::memory_order_relaxed)) {
    return expected<void, ShellError>::error(ShellError::kAlreadyRunning);
  }

  if (cfg_.override_fd >= 0) {
    uart_fd_ = cfg_.override_fd;
    owns_fd_ = false;
  } else {
    uart_fd_ = ::open(cfg_.device, O_RDWR | O_NOCTTY);
    if (uart_fd_ < 0) {
      return expected<void, ShellError>::error(ShellError::kDeviceOpenFailed);
    }
    owns_fd_ = true;

    // RAII guard: close uart_fd_ on any error path below.
    ScopeGuard fd_guard(FixedFunction<void()>([this]() {
      ::close(uart_fd_);
      uart_fd_ = -1;
      owns_fd_ = false;
    }));

    // Configure termios: 8N1, no flow control, raw mode.
    struct termios tty = {};
    if (::tcgetattr(uart_fd_, &tty) != 0) {
      return expected<void, ShellError>::error(ShellError::kDeviceOpenFailed);
    }

    const speed_t spd = BaudToSpeed(cfg_.baudrate);
    (void)::cfsetispeed(&tty, spd);
    (void)::cfsetospeed(&tty, spd);

    tty.c_cflag = (tty.c_cflag & ~static_cast<tcflag_t>(CSIZE)) | CS8;
    tty.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD);
    tty.c_cflag &= ~static_cast<tcflag_t>(PARENB | CSTOPB | CRTSCTS);

    tty.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY |
                                            ICRNL | INLCR | IGNCR);
    tty.c_oflag &= ~static_cast<tcflag_t>(OPOST);

    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;

    if (::tcsetattr(uart_fd_, TCSANOW, &tty) != 0) {
      return expected<void, ShellError>::error(ShellError::kDeviceOpenFailed);
    }

    fd_guard.release();  // Success -- keep the fd open.
  }

  detail::ShellSessionInit(session_, uart_fd_, uart_fd_,
                       detail::ShellPosixWrite, detail::ShellPosixRead, false);

  running_.store(true, std::memory_order_release);
  thread_ = std::thread([this]() { RunLoop(); });

  return expected<void, ShellError>::success();
}

inline void UartShell::Stop() noexcept {
  if (!running_.load(std::memory_order_relaxed)) return;
  running_.store(false, std::memory_order_release);
  session_.active.store(false, std::memory_order_release);

  if (thread_.joinable()) {
    thread_.join();
  }

  if (owns_fd_ && uart_fd_ >= 0) {
    ::close(uart_fd_);
    uart_fd_ = -1;
  }
}

inline void UartShell::RunLoop() noexcept {
  auto& s = session_;
  detail::ShellSessionWrite(s, cfg_.prompt);

  while (running_.load(std::memory_order_relaxed) &&
         s.active.load(std::memory_order_acquire)) {
    struct pollfd pfd;
    pfd.fd = s.read_fd;
    pfd.events = POLLIN;
    int pr = ::poll(&pfd, 1, 200);
    if (pr == 0) continue;
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }

    uint8_t byte;
    ssize_t n = s.read_fn(s.read_fd, &byte, 1);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      break;
    }

    if (detail::ProcessByte(s, byte, cfg_.prompt)) {
      detail::ShellExecuteLine(s);
      s.line_pos = 0;
      s.line_buf[0] = '\0';
      if (s.active.load(std::memory_order_acquire)) {
        detail::ShellSessionWrite(s, cfg_.prompt);
      }
    }
  }
}

}  // namespace osp

#endif  // OSP_SHELL_HPP_
