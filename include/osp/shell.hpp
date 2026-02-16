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
 * @brief Header-only debug shell with pluggable I/O backends.
 *
 * Provides a global command registry, auto-registration macros, in-place
 * command line parsing, and a default TCP telnet backend (DebugShell).
 * Additional backends (ConsoleShell, UartShell) are in separate headers.
 *
 * The session I/O is abstracted via function pointers (read_fn / write_fn)
 * so that all backends share the same command dispatch, tab completion,
 * and Printf routing.
 *
 * Compatible with -fno-exceptions -fno-rtti, C++14/17.
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

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

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

class DebugShell;

namespace detail {

// ============================================================================
// I/O function pointer types
// ============================================================================

/// @brief Write function: ssize_t write(int fd, const void* buf, size_t len).
using ShellWriteFn = ssize_t (*)(int fd, const void* buf, size_t len);

/// @brief Read function: ssize_t read(int fd, void* buf, size_t len).
using ShellReadFn = ssize_t (*)(int fd, void* buf, size_t len);

// ============================================================================
// Built-in I/O backends
// ============================================================================

/// @brief TCP backend: send() with MSG_NOSIGNAL.
inline ssize_t ShellTcpWrite(int fd, const void* buf, size_t len) {
  return ::send(fd, buf, len, MSG_NOSIGNAL);
}

/// @brief TCP backend: recv().
inline ssize_t ShellTcpRead(int fd, void* buf, size_t len) {
  return ::recv(fd, buf, len, 0);
}

/// @brief POSIX backend: write() for stdin/stdout/UART.
inline ssize_t ShellPosixWrite(int fd, const void* buf, size_t len) {
  return ::write(fd, buf, len);
}

/// @brief POSIX backend: read() for stdin/UART.
inline ssize_t ShellPosixRead(int fd, void* buf, size_t len) {
  return ::read(fd, buf, len);
}

// ============================================================================
// ShellSession - Backend-agnostic session state
// ============================================================================

/// @brief Session state used by all shell backends.
struct ShellSession {
  int read_fd = -1;             ///< File descriptor for reading input.
  int write_fd = -1;            ///< File descriptor for writing output.
  std::thread thread;
  char line_buf[128] = {};
  uint32_t line_pos = 0;
  std::atomic<bool> active{false};

  ShellWriteFn write_fn = nullptr;  ///< Backend-specific write function.
  ShellReadFn read_fn = nullptr;    ///< Backend-specific read function.
  bool telnet_mode = false;         ///< True for TCP (handle \\r\\n peek).
};

/// @brief Returns a reference to the thread-local current session pointer.
inline ShellSession*& CurrentSession() noexcept {
  static thread_local ShellSession* s = nullptr;
  return s;
}

// ============================================================================
// Session I/O helpers
// ============================================================================

/// @brief Write a NUL-terminated string to a session.
inline void ShellSessionWrite(ShellSession& s, const char* str) {
  if (s.write_fn != nullptr && str != nullptr) {
    const size_t len = std::strlen(str);
    if (len > 0) {
      (void)s.write_fn(s.write_fd, str, len);
    }
  }
}

/// @brief Write exactly @p len bytes to a session.
inline void ShellSessionWriteN(ShellSession& s, const char* buf, size_t len) {
  if (s.write_fn != nullptr && len > 0) {
    (void)s.write_fn(s.write_fd, buf, len);
  }
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
  inline expected<void, ShellError> Register(const char* name,
                                             ShellCmdFn fn,
                                             const char* desc) noexcept;

  /**
   * @brief Find a command by exact name.
   * @return Pointer to the ShellCmd, or nullptr if not found.
   */
  inline const ShellCmd* Find(const char* name) const noexcept;

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
  inline uint32_t AutoComplete(const char* prefix,
                               char* out_buf,
                               uint32_t buf_size) const noexcept;

  /**
   * @brief Iterate over all registered commands.
   * @param visitor Callback invoked once per command.
   */
  inline void ForEach(
      function_ref<void(const ShellCmd&)> visitor) const noexcept;

  /// @brief Return the number of registered commands.
  uint32_t Count() const noexcept { return count_; }

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
static constexpr uint32_t kMaxArgs = 8;

/**
 * @brief Split a command line in-place into argc / argv.
 *
 * Replaces whitespace with NUL bytes and fills @p argv with pointers into
 * @p cmd.  Supports double-quoted strings: cmd "hello world" arg2
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

    if (cmd[i] == '"') {
      // Quoted argument -- consume up to matching quote.
      cmd[i] = '\0';  // strip opening quote
      ++i;
      if (i >= length) {
        break;
      }
      argv[argc++] = &cmd[i];
      while (i < length && cmd[i] != '"') {
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
// Shared session logic - command execution and interactive loop
// ============================================================================

/**
 * @brief Parse and dispatch the current line buffer of a session.
 *
 * Called by all shell backends. Uses GlobalCmdRegistry for command lookup.
 */
inline void ShellExecuteLine(ShellSession& s) {
  char* argv[kMaxArgs] = {};
  int argc = ShellSplit(s.line_buf, s.line_pos, argv);

  if (argc == 0) {
    return;
  }

  const ShellCmd* cmd = GlobalCmdRegistry::Instance().Find(argv[0]);

  if (cmd != nullptr) {
    // Set the thread-local session pointer so Printf() works.
    CurrentSession() = &s;
    cmd->func(argc, argv);
    CurrentSession() = nullptr;
  } else {
    char msg[160];
    int n = std::snprintf(msg, sizeof(msg),
                          "unknown command: %s\r\n", argv[0]);
    if (n > 0) {
      ShellSessionWriteN(s, msg, static_cast<size_t>(n));
    }
  }
}

/**
 * @brief Interactive session loop shared by all backends.
 *
 * Reads characters one at a time from the session's read_fn, handles
 * editing (backspace, Ctrl+C), tab completion, and command dispatch.
 *
 * @param s       Session with configured read_fn / write_fn.
 * @param running Atomic flag; loop exits when set to false.
 * @param prompt  Prompt string to display.
 */
inline void ShellRunSession(ShellSession& s,
                            std::atomic<bool>& running,
                            const char* prompt) {
  ShellSessionWrite(s, prompt);

  while (running.load(std::memory_order_relaxed) &&
         s.active.load(std::memory_order_acquire)) {
    // Poll with timeout so we periodically re-check the running flag.
    // This avoids the need to close(fd) from another thread to unblock
    // a blocking read(), which is a POSIX-undefined data race (TSan).
    struct pollfd pfd;
    pfd.fd = s.read_fd;
    pfd.events = POLLIN;
    int pr = ::poll(&pfd, 1, 200);
    if (pr == 0) {
      continue;  // Timeout -- re-check running flag.
    }
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }

    char ch;
    ssize_t n = s.read_fn(s.read_fd, &ch, 1);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) {
      break;
    }

    if (ch == 0x03) {
      // Ctrl+C -- close session.
      ShellSessionWrite(s, "\r\nBye.\r\n");
      break;
    }

    if (ch == 0x7F || ch == 0x08) {
      // Backspace.
      if (s.line_pos > 0) {
        --s.line_pos;
        ShellSessionWrite(s, "\b \b");
      }
      continue;
    }

    if (ch == '\t') {
      // Tab -- auto-complete.
      s.line_buf[s.line_pos] = '\0';
      char completion[64] = {};
      uint32_t matches = GlobalCmdRegistry::Instance().AutoComplete(
          s.line_buf, completion, sizeof(completion));

      if (matches == 1) {
        // Clear the current line on terminal and replace.
        for (uint32_t i = 0; i < s.line_pos; ++i) {
          ShellSessionWrite(s, "\b \b");
        }
        uint32_t comp_len = static_cast<uint32_t>(std::strlen(completion));
        if (comp_len >= sizeof(s.line_buf) - 1) {
          comp_len = sizeof(s.line_buf) - 2;
        }
        std::memcpy(s.line_buf, completion, comp_len);
        s.line_buf[comp_len] = ' ';
        s.line_pos = comp_len + 1;
        s.line_buf[s.line_pos] = '\0';
        ShellSessionWriteN(s, s.line_buf, s.line_pos);
      } else if (matches > 1) {
        // Show all matches.
        ShellSessionWrite(s, "\r\n");
        GlobalCmdRegistry::Instance().ForEach(
            [&s](const ShellCmd& cmd) {
              if (std::strncmp(cmd.name, s.line_buf,
                               s.line_pos) == 0) {
                ShellSessionWrite(s, cmd.name);
                ShellSessionWrite(s, "  ");
              }
            });
        ShellSessionWrite(s, "\r\n");
        // Re-display prompt and current input.
        ShellSessionWrite(s, prompt);
        // Fill with the longest common prefix.
        uint32_t comp_len = static_cast<uint32_t>(std::strlen(completion));
        if (comp_len >= sizeof(s.line_buf) - 1) {
          comp_len = sizeof(s.line_buf) - 2;
        }
        std::memcpy(s.line_buf, completion, comp_len);
        s.line_pos = comp_len;
        s.line_buf[s.line_pos] = '\0';
        ShellSessionWriteN(s, s.line_buf, s.line_pos);
      }
      continue;
    }

    if (ch == '\r' || ch == '\n') {
      // Enter pressed -- execute the line.
      ShellSessionWrite(s, "\r\n");

      // Telnet sends \r\n; peek-and-consume the trailing byte.
      if (s.telnet_mode && ch == '\r') {
        char next;
        ssize_t peek = ::recv(s.read_fd, &next, 1, MSG_PEEK);
        if (peek == 1 && (next == '\n' || next == '\0')) {
          (void)::recv(s.read_fd, &next, 1, 0);  // consume it
        }
      }

      s.line_buf[s.line_pos] = '\0';
      if (s.line_pos > 0) {
        ShellExecuteLine(s);
      }
      s.line_pos = 0;
      ShellSessionWrite(s, prompt);
      continue;
    }

    // Regular printable character.
    if (s.line_pos < sizeof(s.line_buf) - 1) {
      s.line_buf[s.line_pos++] = ch;
      // Echo the character back.
      ShellSessionWriteN(s, &ch, 1);
    }
  }

  s.active.store(false, std::memory_order_release);
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
    detail::GlobalCmdRegistry::Instance().Register(name, func, desc);
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
  char buf[256];
  GlobalCmdRegistry::Instance().ForEach([&buf](const ShellCmd& cmd) {
    int n = std::snprintf(buf, sizeof(buf), "  %-16s - %s\r\n",
                          cmd.name, cmd.desc ? cmd.desc : "");
    if (n > 0) {
      ShellSession*& sess = CurrentSession();
      if (sess != nullptr && sess->write_fn != nullptr) {
        (void)sess->write_fn(sess->write_fd, buf, static_cast<size_t>(n));
      }
    }
  });
  return 0;
}

/// @brief Auto-register the built-in help command.
inline bool RegisterHelpOnce() noexcept {
  static const bool done = []() {
    GlobalCmdRegistry::Instance().Register("help", ShellBuiltinHelp,
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
 * May only be called from within a command callback. Uses a thread-local
 * session pointer set by the shell before command dispatch. Works with
 * any backend (TCP, stdin, UART) via the session's write_fn.
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

// ============================================================================
// DebugShell - TCP telnet debug server (default backend)
// ============================================================================

/**
 * @brief Lightweight telnet debug shell.
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
    uint16_t port;           ///< TCP listen port.
    uint32_t max_connections;///< Maximum concurrent sessions.
    const char* prompt;      ///< Prompt string sent to clients.

    Config() noexcept : port(5090), max_connections(2), prompt("osp> ") {}
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
   *         ShellError::kAlreadyRunning (via kNotRunning reused) if already
   *         started.
   */
  inline expected<void, ShellError> Start();

  /**
   * @brief Stop the telnet server and close all sessions.
   *
   * Joins the accept thread and all session threads. Safe to call even if
   * the shell is not running.
   */
  inline void Stop();

  /**
   * @brief Printf into the current session's socket.
   *
   * May only be called from within a command callback. Uses a thread-local
   * session pointer set by the shell before command dispatch.
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
  bool IsRunning() const noexcept { return running_.load(std::memory_order_relaxed); }

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

  /// @brief Send a string to a TCP socket directly (for pre-session use).
  static inline void TcpSendStr(int fd, const char* str) {
    if (fd >= 0 && str != nullptr) {
      const size_t len = std::strlen(str);
      if (len > 0) {
        (void)::send(fd, str, len, MSG_NOSIGNAL);
      }
    }
  }
};

// ============================================================================
// DebugShell inline implementation
// ============================================================================

inline expected<void, ShellError> DebugShell::Start() {
  if (running_.load(std::memory_order_acquire)) {
    return expected<void, ShellError>::error(ShellError::kNotRunning);
  }

  // Create TCP socket.
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return expected<void, ShellError>::error(ShellError::kPortInUse);
  }

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
    ::close(listen_fd_);
    listen_fd_ = -1;
    return expected<void, ShellError>::error(ShellError::kPortInUse);
  }

  if (::listen(listen_fd_, static_cast<int>(cfg_.max_connections)) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return expected<void, ShellError>::error(ShellError::kPortInUse);
  }

  // MISRA C++ Rule 18-4-1 deviation: dynamic allocation required because
  // max_connections is a runtime config value. ShellSession contains
  // std::thread (non-trivially-copyable), precluding FixedVector.
  // Allocated once at Start(), freed at Stop() -- cold path only.
  sessions_ = new Session[cfg_.max_connections];

  running_.store(true, std::memory_order_release);
  accept_thread_ = std::thread([this]() { AcceptLoop(); });

  return expected<void, ShellError>::success();
}

inline void DebugShell::Stop() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  running_.store(false, std::memory_order_release);

  // Close the listening socket to unblock accept().
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }

  // Mark all sessions inactive so poll-based session loops will exit.
  if (sessions_ != nullptr) {
    for (uint32_t i = 0; i < cfg_.max_connections; ++i) {
      sessions_[i].active.store(false, std::memory_order_release);
    }
  }

  // Join the accept thread.
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }

  // Join all session threads, then close their sockets.
  if (sessions_ != nullptr) {
    for (uint32_t i = 0; i < cfg_.max_connections; ++i) {
      if (sessions_[i].thread.joinable()) {
        sessions_[i].thread.join();
      }
      if (sessions_[i].read_fd >= 0) {
        ::close(sessions_[i].read_fd);
        sessions_[i].read_fd = -1;
        sessions_[i].write_fd = -1;
      }
    }
    delete[] sessions_;
    sessions_ = nullptr;
  }
}

inline void DebugShell::AcceptLoop() {
  while (running_.load(std::memory_order_relaxed)) {
    if (heartbeat_ != nullptr) { heartbeat_->Beat(); }
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
        sessions_[i].read_fd = client_fd;
        sessions_[i].write_fd = client_fd;
        sessions_[i].write_fn = detail::ShellTcpWrite;
        sessions_[i].read_fn = detail::ShellTcpRead;
        sessions_[i].telnet_mode = true;
        sessions_[i].line_pos = 0;
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

inline void DebugShell::SessionLoop(Session& s) {
  // Delegate to the shared session loop.
  detail::ShellRunSession(s, running_, cfg_.prompt);

  // TCP-specific cleanup: close socket.
  if (s.read_fd >= 0) {
    ::close(s.read_fd);
    s.read_fd = -1;
    s.write_fd = -1;
  }
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

// ============================================================================
// ConsoleShell - stdin/stdout debug shell (no network required)
// ============================================================================

/**
 * @brief Interactive debug shell using stdin/stdout.
 *
 * Designed for scenarios without network access: SSH into the device,
 * early boot debugging, or CI automated testing via pipe.
 *
 * Typical usage:
 * @code
 *   osp::ConsoleShell::Config cfg;
 *   osp::ConsoleShell shell(cfg);
 *   shell.Start();   // spawns a shell thread
 *   // ... application runs ...
 *   shell.Stop();
 * @endcode
 *
 * For testing, set cfg.read_fd / cfg.write_fd to pipe file descriptors.
 */
class ConsoleShell final {
 public:
  struct Config {
    const char* prompt;   ///< Prompt string.
    int read_fd;          ///< Override read fd (-1 = STDIN_FILENO).
    int write_fd;         ///< Override write fd (-1 = STDOUT_FILENO).
    bool raw_mode;        ///< Set termios raw mode (only for real stdin).

    Config() noexcept
        : prompt("osp> "), read_fd(-1), write_fd(-1), raw_mode(true) {}
  };

  explicit ConsoleShell(const Config& cfg = Config{}) : cfg_(cfg) {
    detail::RegisterHelpOnce();
  }

  ~ConsoleShell() { Stop(); }

  ConsoleShell(const ConsoleShell&) = delete;
  ConsoleShell& operator=(const ConsoleShell&) = delete;

  inline expected<void, ShellError> Start();
  inline void Stop();

  /// @brief Printf into the current session (delegates to ShellPrintf).
  static inline int Printf(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
      __attribute__((format(printf, 1, 2)))
#endif
      ;

  bool IsRunning() const noexcept {
    return running_.load(std::memory_order_relaxed);
  }

 private:
  using Session = detail::ShellSession;

  Config cfg_;
  Session session_;
  std::atomic<bool> running_{false};
  struct termios orig_termios_{};
  bool termios_saved_ = false;
};

// ----------------------------------------------------------------------------
// ConsoleShell inline implementation
// ----------------------------------------------------------------------------

inline expected<void, ShellError> ConsoleShell::Start() {
  if (running_.load(std::memory_order_acquire)) {
    return expected<void, ShellError>::error(ShellError::kNotRunning);
  }

  const int rfd = (cfg_.read_fd >= 0) ? cfg_.read_fd : STDIN_FILENO;
  const int wfd = (cfg_.write_fd >= 0) ? cfg_.write_fd : STDOUT_FILENO;

  // Set terminal to raw mode if using real stdin.
  if (cfg_.raw_mode && rfd == STDIN_FILENO && ::isatty(STDIN_FILENO)) {
    if (::tcgetattr(STDIN_FILENO, &orig_termios_) == 0) {
      struct termios raw = orig_termios_;
      raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
      raw.c_cc[VMIN] = 1;
      raw.c_cc[VTIME] = 0;
      ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
      termios_saved_ = true;
    }
  }

  session_.read_fd = rfd;
  session_.write_fd = wfd;
  session_.write_fn = detail::ShellPosixWrite;
  session_.read_fn = detail::ShellPosixRead;
  session_.telnet_mode = false;
  session_.line_pos = 0;

  running_.store(true, std::memory_order_release);
  session_.active.store(true, std::memory_order_release);
  session_.thread = std::thread([this]() {
    detail::ShellRunSession(session_, running_, cfg_.prompt);
  });

  return expected<void, ShellError>::success();
}

inline void ConsoleShell::Stop() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  running_.store(false, std::memory_order_release);
  session_.active.store(false, std::memory_order_release);

  // Join first -- the session loop uses poll() with a 200ms timeout,
  // so it will notice running_==false and exit within one poll cycle.
  if (session_.thread.joinable()) {
    session_.thread.join();
  }

  // Restore terminal settings.
  if (termios_saved_) {
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios_);
    termios_saved_ = false;
  }
}

inline int ConsoleShell::Printf(const char* fmt, ...) {
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

// ============================================================================
// UartShell - UART/serial debug shell
// ============================================================================

/**
 * @brief Interactive debug shell over a UART/serial device.
 *
 * Opens a serial port via Linux termios, configures baud rate and raw mode,
 * and runs the standard shell session loop over the serial line.
 *
 * Typical usage:
 * @code
 *   osp::UartShell::Config cfg;
 *   cfg.device = "/dev/ttyS1";
 *   cfg.baudrate = 115200;
 *   osp::UartShell shell(cfg);
 *   shell.Start();
 *   // connect via minicom / picocom
 *   shell.Stop();
 * @endcode
 *
 * For testing, set cfg.override_fd to a PTY master fd.
 */
class UartShell final {
 public:
  struct Config {
    const char* device;    ///< Serial device path.
    uint32_t baudrate;     ///< Baud rate.
    const char* prompt;    ///< Prompt string.
    int override_fd;       ///< Override fd for testing (-1 = open device).

    Config() noexcept
        : device("/dev/ttyS0"), baudrate(115200),
          prompt("osp> "), override_fd(-1) {}
  };

  explicit UartShell(const Config& cfg = Config{}) : cfg_(cfg) {
    detail::RegisterHelpOnce();
  }

  ~UartShell() { Stop(); }

  UartShell(const UartShell&) = delete;
  UartShell& operator=(const UartShell&) = delete;

  inline expected<void, ShellError> Start();
  inline void Stop();

  /// @brief Printf into the current session (delegates to ShellPrintf).
  static inline int Printf(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
      __attribute__((format(printf, 1, 2)))
#endif
      ;

  bool IsRunning() const noexcept {
    return running_.load(std::memory_order_relaxed);
  }

 private:
  using Session = detail::ShellSession;

  Config cfg_;
  Session session_;
  int uart_fd_ = -1;
  bool owns_fd_ = false;
  std::atomic<bool> running_{false};

  static speed_t BaudToSpeed(uint32_t baud) noexcept {
    switch (baud) {
      case 9600:   return B9600;
      case 19200:  return B19200;
      case 38400:  return B38400;
      case 57600:  return B57600;
      case 115200: return B115200;
      case 230400: return B230400;
      case 460800: return B460800;
      case 921600: return B921600;
      default:     return B115200;
    }
  }
};

// ----------------------------------------------------------------------------
// UartShell inline implementation
// ----------------------------------------------------------------------------

inline expected<void, ShellError> UartShell::Start() {
  if (running_.load(std::memory_order_acquire)) {
    return expected<void, ShellError>::error(ShellError::kNotRunning);
  }

  if (cfg_.override_fd >= 0) {
    // Testing mode: use provided fd (e.g. PTY slave).
    uart_fd_ = cfg_.override_fd;
    owns_fd_ = false;
  } else {
    uart_fd_ = ::open(cfg_.device, O_RDWR | O_NOCTTY);
    if (uart_fd_ < 0) {
      return expected<void, ShellError>::error(ShellError::kBindFailed);
    }
    owns_fd_ = true;

    struct termios tio{};
    ::tcgetattr(uart_fd_, &tio);
    ::cfmakeraw(&tio);
    ::cfsetispeed(&tio, BaudToSpeed(cfg_.baudrate));
    ::cfsetospeed(&tio, BaudToSpeed(cfg_.baudrate));
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    ::tcsetattr(uart_fd_, TCSAFLUSH, &tio);
  }

  session_.read_fd = uart_fd_;
  session_.write_fd = uart_fd_;
  session_.write_fn = detail::ShellPosixWrite;
  session_.read_fn = detail::ShellPosixRead;
  session_.telnet_mode = false;
  session_.line_pos = 0;

  running_.store(true, std::memory_order_release);
  session_.active.store(true, std::memory_order_release);
  session_.thread = std::thread([this]() {
    detail::ShellRunSession(session_, running_, cfg_.prompt);
  });

  return expected<void, ShellError>::success();
}

inline void UartShell::Stop() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  running_.store(false, std::memory_order_release);
  session_.active.store(false, std::memory_order_release);

  // Join first -- poll() timeout in session loop handles exit.
  if (session_.thread.joinable()) {
    session_.thread.join();
  }

  // Close fd after thread has exited (TSan-safe).
  if (uart_fd_ >= 0 && owns_fd_) {
    ::close(uart_fd_);
    uart_fd_ = -1;
  }
}

inline int UartShell::Printf(const char* fmt, ...) {
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

}  // namespace osp

#endif  // OSP_SHELL_HPP_
