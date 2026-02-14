/**
 * @file shell.hpp
 * @brief Header-only remote debug shell with telnet access and command
 *        registration.
 *
 * Provides a lightweight telnet server using raw POSIX sockets, a global
 * command registry with auto-registration macros, and in-place command line
 * parsing.  Designed for embedded ARM-Linux diagnostics.
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
#include <netinet/in.h>
#include <sys/socket.h>
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
// Forward declaration for DebugShell::Session (needed by detail)
// ============================================================================

class DebugShell;

namespace detail {

// ============================================================================
// Thread-local current session pointer (for Printf routing)
// ============================================================================

/// @brief Session type used internally by DebugShell.
struct ShellSession {
  int sock_fd = -1;
  std::thread thread;
  char line_buf[128] = {};
  uint32_t line_pos = 0;
  std::atomic<bool> active{false};
};

/// @brief Returns a reference to the thread-local current session pointer.
inline ShellSession*& CurrentSession() noexcept {
  static thread_local ShellSession* s = nullptr;
  return s;
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
      if (sess != nullptr && sess->sock_fd >= 0) {
        (void)::send(sess->sock_fd, buf, static_cast<size_t>(n), MSG_NOSIGNAL);
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
// DebugShell - Telnet debug server
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

  /// @brief Parse and dispatch the current line buffer.
  inline void ExecuteLine(Session& s);

  /// @brief Send a string to a session socket.
  static inline void SendStr(int fd, const char* str) {
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

  // Close all active session sockets to unblock their recv().
  if (sessions_ != nullptr) {
    for (uint32_t i = 0; i < cfg_.max_connections; ++i) {
      if (sessions_[i].sock_fd >= 0) {
        ::close(sessions_[i].sock_fd);
        sessions_[i].sock_fd = -1;
      }
    }
  }

  // Join the accept thread.
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }

  // Join all session threads.
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
        sessions_[i].sock_fd = client_fd;
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
  // Send initial prompt.
  SendStr(s.sock_fd, cfg_.prompt);

  while (running_.load(std::memory_order_relaxed) &&
         s.active.load(std::memory_order_acquire)) {
    char ch;
    ssize_t n = ::recv(s.sock_fd, &ch, 1, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) {
      break;
    }

    if (ch == 0x03) {
      // Ctrl+C -- close session.
      const char* msg = "\r\nBye.\r\n";
      SendStr(s.sock_fd, msg);
      break;
    }

    if (ch == 0x7F || ch == 0x08) {
      // Backspace.
      if (s.line_pos > 0) {
        --s.line_pos;
        // Send backspace-space-backspace to erase on terminal.
        const char bs[] = "\b \b";
        (void)::send(s.sock_fd, bs, 3, MSG_NOSIGNAL);
      }
      continue;
    }

    if (ch == '\t') {
      // Tab -- auto-complete.
      s.line_buf[s.line_pos] = '\0';
      char completion[64] = {};
      uint32_t matches = detail::GlobalCmdRegistry::Instance().AutoComplete(
          s.line_buf, completion, sizeof(completion));

      if (matches == 1) {
        // Clear the current line on terminal and replace.
        // Send backspaces to erase current input.
        for (uint32_t i = 0; i < s.line_pos; ++i) {
          const char bs[] = "\b \b";
          (void)::send(s.sock_fd, bs, 3, MSG_NOSIGNAL);
        }
        uint32_t comp_len = static_cast<uint32_t>(std::strlen(completion));
        if (comp_len >= sizeof(s.line_buf) - 1) {
          comp_len = sizeof(s.line_buf) - 2;
        }
        std::memcpy(s.line_buf, completion, comp_len);
        s.line_buf[comp_len] = ' ';
        s.line_pos = comp_len + 1;
        s.line_buf[s.line_pos] = '\0';
        (void)::send(s.sock_fd, s.line_buf, s.line_pos, MSG_NOSIGNAL);
      } else if (matches > 1) {
        // Show all matches.
        SendStr(s.sock_fd, "\r\n");
        detail::GlobalCmdRegistry::Instance().ForEach(
            [&](const ShellCmd& cmd) {
              if (std::strncmp(cmd.name, s.line_buf,
                               s.line_pos) == 0) {
                SendStr(s.sock_fd, cmd.name);
                SendStr(s.sock_fd, "  ");
              }
            });
        SendStr(s.sock_fd, "\r\n");
        // Re-display prompt and current input.
        SendStr(s.sock_fd, cfg_.prompt);
        // Fill with the longest common prefix.
        uint32_t comp_len = static_cast<uint32_t>(std::strlen(completion));
        if (comp_len >= sizeof(s.line_buf) - 1) {
          comp_len = sizeof(s.line_buf) - 2;
        }
        std::memcpy(s.line_buf, completion, comp_len);
        s.line_pos = comp_len;
        s.line_buf[s.line_pos] = '\0';
        (void)::send(s.sock_fd, s.line_buf, s.line_pos, MSG_NOSIGNAL);
      }
      continue;
    }

    if (ch == '\r' || ch == '\n') {
      // Enter pressed -- execute the line.
      SendStr(s.sock_fd, "\r\n");

      // Skip bare \n following \r (telnet sends \r\n).
      if (ch == '\r') {
        // Peek at the next byte; discard \n or \0.
        char next;
        ssize_t peek = ::recv(s.sock_fd, &next, 1, MSG_PEEK);
        if (peek == 1 && (next == '\n' || next == '\0')) {
          (void)::recv(s.sock_fd, &next, 1, 0);  // consume it
        }
      }

      s.line_buf[s.line_pos] = '\0';
      if (s.line_pos > 0) {
        ExecuteLine(s);
      }
      s.line_pos = 0;
      SendStr(s.sock_fd, cfg_.prompt);
      continue;
    }

    // Regular printable character.
    if (s.line_pos < sizeof(s.line_buf) - 1) {
      s.line_buf[s.line_pos++] = ch;
      // Echo the character back.
      (void)::send(s.sock_fd, &ch, 1, MSG_NOSIGNAL);
    }
  }

  // Cleanup session.
  if (s.sock_fd >= 0) {
    ::close(s.sock_fd);
    s.sock_fd = -1;
  }
  s.active.store(false, std::memory_order_release);
}

inline void DebugShell::ExecuteLine(Session& s) {
  char* argv[detail::kMaxArgs] = {};
  int argc = detail::ShellSplit(s.line_buf, s.line_pos, argv);

  if (argc == 0) {
    return;
  }

  const ShellCmd* cmd =
      detail::GlobalCmdRegistry::Instance().Find(argv[0]);

  if (cmd != nullptr) {
    // Set the thread-local session pointer so Printf() works.
    detail::CurrentSession() = &s;
    cmd->func(argc, argv);
    detail::CurrentSession() = nullptr;
  } else {
    char msg[160];
    int n = std::snprintf(msg, sizeof(msg),
                          "unknown command: %s\r\n", argv[0]);
    if (n > 0) {
      (void)::send(s.sock_fd, msg, static_cast<size_t>(n), MSG_NOSIGNAL);
    }
  }
}

inline int DebugShell::Printf(const char* fmt, ...) {
  detail::ShellSession* sess = detail::CurrentSession();
  if (sess == nullptr || sess->sock_fd < 0) {
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
    ssize_t sent = ::send(sess->sock_fd, buf, static_cast<size_t>(to_send),
                          MSG_NOSIGNAL);
    return static_cast<int>(sent);
  }
  return -1;
}

}  // namespace osp

#endif  // OSP_SHELL_HPP_
