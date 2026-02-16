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
 * @file process.hpp
 * @brief Linux process management: find, freeze, resume, terminate, spawn.
 *
 * Header-only, C++14 compatible, no std::regex dependency.
 * Uses /proc filesystem for process discovery and POSIX signals
 * for process control. Linux-only (requires /proc and kill(2)).
 *
 * Features:
 *   - Process discovery: FindPidByName (exact match via /proc/[pid]/comm)
 *   - Process control: Freeze/Resume/Terminate/Kill by PID or name
 *   - Process state: IsProcessStopped, IsProcessAlive, ReadProcessState
 *   - Subprocess spawn: SpawnProcess with stdout/stderr pipe capture
 *   - Subprocess wait: WaitProcess with timeout support
 *   - Output capture: ReadPipe non-blocking read into FixedVector/buffer
 *
 * Inspired by reproc, subprocess.h, and TinyProcessLibrary.
 * Compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_PROCESS_HPP_
#define OSP_PROCESS_HPP_

#include "osp/platform.hpp"

#if defined(OSP_PLATFORM_LINUX)

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace osp {

// ============================================================================
// ProcessResult
// ============================================================================

enum class ProcessResult : int8_t {
  kSuccess = 0,
  kNotFound = -1,   ///< Process not found by name
  kFailed = -2,     ///< Signal delivery or state change failed
  kWaitError = -3,  ///< waitpid(2) error after SIGKILL
};

namespace detail {

// ============================================================================
// DirGuard - RAII wrapper for DIR*
// ============================================================================

class DirGuard {
 public:
  explicit DirGuard(DIR* dir) : dir_(dir) {}
  ~DirGuard() {
    if (dir_) {
      closedir(dir_);
    }
  }
  DIR* get() const { return dir_; }

  DirGuard(const DirGuard&) = delete;
  DirGuard& operator=(const DirGuard&) = delete;

 private:
  DIR* dir_;
};

// ============================================================================
// Helper functions
// ============================================================================

/// @brief Sleep for @p ms milliseconds (nanosleep, not deprecated usleep).
inline void SleepMs(uint32_t ms) {
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(ms / 1000U);
  ts.tv_nsec = static_cast<long>(ms % 1000U) * 1000000L;  // NOLINT
  nanosleep(&ts, nullptr);
}

/// @brief Check if string consists entirely of ASCII digits.
inline bool IsDigitString(const char* s) {
  if (*s == '\0')
    return false;
  for (; *s != '\0'; ++s) {
    if (*s < '0' || *s > '9')
      return false;
  }
  return true;
}

/// @brief Extract basename from a path (e.g., "/usr/bin/foo" -> "foo").
inline const char* Basename(const char* path) {
  const char* last_slash = nullptr;
  for (const char* p = path; *p != '\0'; ++p) {
    if (*p == '/')
      last_slash = p;
  }
  return last_slash ? (last_slash + 1) : path;
}

/// @brief Read content of a /proc file into @p buf (up to buf_size-1 bytes).
/// @return Number of bytes read, or -1 on error.
inline int ReadProcFile(const char* path, char* buf, size_t buf_size) {
  // Use POSIX read() instead of std::ifstream to avoid heap allocation
  int fd = open(path, O_RDONLY);  // NOLINT
  if (fd < 0)
    return -1;
  ssize_t n = read(fd, buf, buf_size - 1);
  close(fd);  // NOLINT
  if (n < 0)
    return -1;
  buf[static_cast<size_t>(n)] = '\0';
  return static_cast<int>(n);
}

}  // namespace detail

// ============================================================================
// Process query functions
// ============================================================================

/**
 * @brief Check if a process is in stopped (T) state.
 * @param pid Process ID to check.
 * @return true if stopped, false if running or process not found.
 */
inline bool IsProcessStopped(pid_t pid) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/status", static_cast<int>(pid));

  char buf[1024];
  if (detail::ReadProcFile(path, buf, sizeof(buf)) < 0) {
    return false;
  }

  // Find "State:" line and extract state character
  // Format: "State:\tS (sleeping)" or "State:\tT (stopped)"
  const char* state_line = strstr(buf, "State:");
  if (!state_line)
    return false;

  const char* p = state_line + 6;  // skip "State:"
  while (*p == ' ' || *p == '\t')
    ++p;
  return *p == 'T';
}

/**
 * @brief Find PID by exact process name.
 *
 * Matches against /proc/[pid]/comm (exact match, max 15 chars),
 * then falls back to /proc/[pid]/cmdline basename match.
 *
 * @param name Process name to search for (exact match, not substring).
 * @param[out] out_pid Found process ID.
 * @return kSuccess if found, kNotFound otherwise, kFailed on /proc error.
 *
 * @note TOCTOU: The returned PID may become stale if the process exits
 *       between find and use. Use pidfd_open(2) on Linux 5.1+ to avoid this.
 */
inline ProcessResult FindPidByName(const char* name, pid_t& out_pid) {
  detail::DirGuard dir(opendir("/proc"));
  if (!dir.get()) {
    return ProcessResult::kFailed;
  }

  struct dirent* entry;
  while ((entry = readdir(dir.get())) != nullptr) {
    if (entry->d_type != DT_DIR)
      continue;
    if (!detail::IsDigitString(entry->d_name))
      continue;

    char path[280];
    char buf[256];

    // Try /proc/[pid]/comm first (kernel-provided, max 15 chars, no path)
    snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
    int n = detail::ReadProcFile(path, buf, sizeof(buf));
    if (n > 0) {
      // comm has trailing newline, strip it
      if (n > 0 && buf[n - 1] == '\n')
        buf[n - 1] = '\0';
      if (strcmp(buf, name) == 0) {
        out_pid = static_cast<pid_t>(atoi(entry->d_name));
        return ProcessResult::kSuccess;
      }
    }

    // Fallback: /proc/[pid]/cmdline argv[0] basename match
    snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
    n = detail::ReadProcFile(path, buf, sizeof(buf));
    if (n > 0) {
      // cmdline is NUL-separated; first segment is argv[0]
      const char* base = detail::Basename(buf);
      if (strcmp(base, name) == 0) {
        out_pid = static_cast<pid_t>(atoi(entry->d_name));
        return ProcessResult::kSuccess;
      }
    }
  }

  return ProcessResult::kNotFound;
}

// ============================================================================
// Process control functions
// ============================================================================

/**
 * @brief Freeze (SIGSTOP) a process by PID.
 * @param pid Process ID.
 * @return kSuccess if frozen, kFailed if signal delivery or state check fails.
 */
inline ProcessResult FreezeProcess(pid_t pid) {
  if (kill(pid, SIGSTOP) != 0) {
    return ProcessResult::kFailed;
  }
  detail::SleepMs(1);
  return IsProcessStopped(pid) ? ProcessResult::kSuccess : ProcessResult::kFailed;
}

/**
 * @brief Freeze a process by name.
 * @param name Exact process name.
 * @return kSuccess, kNotFound, or kFailed.
 */
inline ProcessResult FreezeProcessByName(const char* name) {
  pid_t pid;
  ProcessResult r = FindPidByName(name, pid);
  if (r != ProcessResult::kSuccess)
    return r;
  return FreezeProcess(pid);
}

/**
 * @brief Resume (SIGCONT) a stopped process by PID with retry.
 * @param pid Process ID.
 * @param attempts Maximum retry count (default 3).
 * @return kSuccess if resumed, kFailed after all attempts exhausted.
 */
inline ProcessResult ResumeProcess(pid_t pid, uint32_t attempts = 3) {
  for (uint32_t i = 0; i < attempts; ++i) {
    if (kill(pid, SIGCONT) == 0) {
      detail::SleepMs(1);
      if (!IsProcessStopped(pid)) {
        return ProcessResult::kSuccess;
      }
      detail::SleepMs(100);
    }
  }
  return ProcessResult::kFailed;
}

/**
 * @brief Resume a stopped process by name.
 * @param name Exact process name.
 * @param attempts Maximum retry count (default 3).
 * @return kSuccess, kNotFound, or kFailed.
 */
inline ProcessResult ResumeProcessByName(const char* name, uint32_t attempts = 3) {
  pid_t pid;
  ProcessResult r = FindPidByName(name, pid);
  if (r != ProcessResult::kSuccess)
    return r;
  return ResumeProcess(pid, attempts);
}

/**
 * @brief Terminate (SIGKILL) a process by PID with retry.
 * @param pid Process ID.
 * @param attempts Maximum retry count (default 3).
 * @return kSuccess, kFailed, or kWaitError.
 *
 * @note Calls waitpid(2) after successful kill. Only works for child
 *       processes or when the caller has appropriate privileges.
 *       For non-child processes, waitpid will fail -- use
 *       TerminateProcessNoWait() instead.
 */
inline ProcessResult TerminateProcess(pid_t pid, uint32_t attempts = 3) {
  for (uint32_t i = 0; i < attempts; ++i) {
    if (kill(pid, SIGKILL) == 0) {
      int status;
      pid_t w = waitpid(pid, &status, 0);
      if (w == -1) {
        // ECHILD: not a child process -- kill succeeded but can't wait
        if (errno == ECHILD)
          return ProcessResult::kSuccess;
        return ProcessResult::kWaitError;
      }
      return ProcessResult::kSuccess;
    }
  }
  return ProcessResult::kFailed;
}

/**
 * @brief Terminate a process by name with retry.
 * @param name Exact process name.
 * @param attempts Maximum retry count (default 3).
 * @return kSuccess, kNotFound, kFailed, or kWaitError.
 */
inline ProcessResult TerminateProcessByName(const char* name, uint32_t attempts = 3) {
  pid_t pid;
  ProcessResult r = FindPidByName(name, pid);
  if (r != ProcessResult::kSuccess)
    return r;
  return TerminateProcess(pid, attempts);
}

/**
 * @brief Send SIGKILL without waitpid. For non-child processes.
 * @param pid Process ID.
 * @return kSuccess if signal sent, kFailed otherwise.
 */
inline ProcessResult KillProcess(pid_t pid) {
  return (kill(pid, SIGKILL) == 0) ? ProcessResult::kSuccess : ProcessResult::kFailed;
}

/**
 * @brief Check if a process is alive (exists and can receive signals).
 * @param pid Process ID.
 * @return true if alive, false if not.
 */
inline bool IsProcessAlive(pid_t pid) {
  return kill(pid, 0) == 0;
}

/**
 * @brief Read single-char process state from /proc/[pid]/status.
 * @param pid Process ID.
 * @return State character ('R','S','D','T','Z','X','I') or '\0' on error.
 *
 * Common states: R=running, S=sleeping, D=disk sleep, T=stopped,
 *                Z=zombie, X=dead, I=idle.
 */
inline char ReadProcessState(pid_t pid) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/status", static_cast<int>(pid));

  char buf[1024];
  if (detail::ReadProcFile(path, buf, sizeof(buf)) < 0) {
    return '\0';
  }

  const char* state_line = strstr(buf, "State:");
  if (!state_line)
    return '\0';

  const char* p = state_line + 6;
  while (*p == ' ' || *p == '\t')
    ++p;
  return *p;
}

// ============================================================================
// PipeGuard - RAII wrapper for pipe file descriptors
// ============================================================================

namespace detail {

class PipeGuard {
 public:
  PipeGuard() : fd_{-1, -1} {}
  ~PipeGuard() { CloseAll(); }

  /// @brief Create a pipe. Returns false on failure.
  bool Create() {
    if (pipe(fd_) != 0)
      return false;  // NOLINT
    return true;
  }

  int ReadEnd() const { return fd_[0]; }
  int WriteEnd() const { return fd_[1]; }

  void CloseRead() {
    if (fd_[0] >= 0) {
      close(fd_[0]);
      fd_[0] = -1;
    }  // NOLINT
  }
  void CloseWrite() {
    if (fd_[1] >= 0) {
      close(fd_[1]);
      fd_[1] = -1;
    }  // NOLINT
  }
  void CloseAll() {
    CloseRead();
    CloseWrite();
  }

  /// @brief Release read-end ownership (caller takes responsibility).
  int ReleaseRead() {
    int r = fd_[0];
    fd_[0] = -1;
    return r;
  }
  /// @brief Release write-end ownership.
  int ReleaseWrite() {
    int r = fd_[1];
    fd_[1] = -1;
    return r;
  }

  PipeGuard(const PipeGuard&) = delete;
  PipeGuard& operator=(const PipeGuard&) = delete;

 private:
  int fd_[2];
};

/// @brief Set a file descriptor to non-blocking mode.
inline bool SetNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return false;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace detail

// ============================================================================
// Subprocess - spawn child process with optional pipe capture
// ============================================================================

/// @brief Subprocess configuration.
struct SubprocessConfig {
  static constexpr uint32_t kDefaultBufSize = 4096;

  const char* const* argv;  ///< NULL-terminated argument array
  const char* working_dir;  ///< chdir before exec (nullptr = inherit)
  bool capture_stdout;      ///< Redirect child stdout to pipe
  bool capture_stderr;      ///< Redirect child stderr to pipe
  bool merge_stderr;        ///< Merge stderr into stdout pipe

  SubprocessConfig()
      : argv(nullptr), working_dir(nullptr), capture_stdout(false), capture_stderr(false), merge_stderr(false) {}
};

/// @brief Wait result from WaitProcess.
struct WaitResult {
  bool exited;      ///< true if child exited normally
  int exit_code;    ///< Exit code (valid if exited==true)
  bool signaled;    ///< true if child was killed by signal
  int term_signal;  ///< Signal number (valid if signaled==true)
  bool timed_out;   ///< true if wait timed out

  WaitResult() : exited(false), exit_code(-1), signaled(false), term_signal(0), timed_out(false) {}
};

/**
 * @brief Lightweight subprocess handle.
 *
 * Manages a child process with optional stdout/stderr pipe capture.
 * RAII: destructor kills the child if still alive and closes pipes.
 *
 * Usage:
 * @code
 *   const char* argv[] = {"ls", "-la", nullptr};
 *   osp::SubprocessConfig cfg;
 *   cfg.argv = argv;
 *   cfg.capture_stdout = true;
 *
 *   osp::Subprocess proc;
 *   if (proc.Start(cfg) == osp::ProcessResult::kSuccess) {
 *     char buf[1024];
 *     int n = proc.ReadStdout(buf, sizeof(buf));
 *     auto result = proc.Wait();
 *   }
 * @endcode
 */
class Subprocess {
 public:
  Subprocess() : pid_(-1), stdout_fd_(-1), stderr_fd_(-1) {}

  ~Subprocess() {
    if (stdout_fd_ >= 0)
      close(stdout_fd_);  // NOLINT
    if (stderr_fd_ >= 0)
      close(stderr_fd_);  // NOLINT
    if (pid_ > 0 && IsProcessAlive(pid_)) {
      kill(pid_, SIGKILL);
      int status;
      waitpid(pid_, &status, 0);
    }
  }

  // Non-copyable, movable
  Subprocess(const Subprocess&) = delete;
  Subprocess& operator=(const Subprocess&) = delete;

  Subprocess(Subprocess&& other) : pid_(other.pid_), stdout_fd_(other.stdout_fd_), stderr_fd_(other.stderr_fd_) {
    other.pid_ = -1;
    other.stdout_fd_ = -1;
    other.stderr_fd_ = -1;
  }

  Subprocess& operator=(Subprocess&& other) {
    if (this != &other) {
      // Clean up current state
      if (stdout_fd_ >= 0)
        close(stdout_fd_);  // NOLINT
      if (stderr_fd_ >= 0)
        close(stderr_fd_);  // NOLINT
      if (pid_ > 0 && IsProcessAlive(pid_)) {
        kill(pid_, SIGKILL);
        int status;
        waitpid(pid_, &status, 0);
      }
      pid_ = other.pid_;
      stdout_fd_ = other.stdout_fd_;
      stderr_fd_ = other.stderr_fd_;
      other.pid_ = -1;
      other.stdout_fd_ = -1;
      other.stderr_fd_ = -1;
    }
    return *this;
  }

  /**
   * @brief Spawn a child process.
   * @param cfg Subprocess configuration (argv must be set).
   * @return kSuccess on success, kFailed on fork/pipe/exec error.
   */
  ProcessResult Start(const SubprocessConfig& cfg) {
    if (!cfg.argv || !cfg.argv[0])
      return ProcessResult::kFailed;

    detail::PipeGuard stdout_pipe, stderr_pipe;

    if (cfg.capture_stdout || cfg.merge_stderr) {
      if (!stdout_pipe.Create())
        return ProcessResult::kFailed;
    }
    if (cfg.capture_stderr && !cfg.merge_stderr) {
      if (!stderr_pipe.Create())
        return ProcessResult::kFailed;
    }

    pid_t child = fork();
    if (child < 0)
      return ProcessResult::kFailed;

    if (child == 0) {
      // -- Child process --
      // Create new session to isolate signals from parent
      setsid();

      // Reset all signal dispositions to default (SIG_IGN survives exec)
      struct sigaction sa_dfl;
      std::memset(&sa_dfl, 0, sizeof(sa_dfl));
      sa_dfl.sa_handler = SIG_DFL;
      for (int sig = 1; sig < 32; ++sig) {
        sigaction(sig, &sa_dfl, nullptr);  // ignore errors for uncatchable
      }

      if (cfg.working_dir) {
        if (chdir(cfg.working_dir) != 0)
          _exit(127);
      }

      if (cfg.capture_stdout || cfg.merge_stderr) {
        stdout_pipe.CloseRead();
        dup2(stdout_pipe.WriteEnd(), STDOUT_FILENO);
        if (cfg.merge_stderr) {
          dup2(stdout_pipe.WriteEnd(), STDERR_FILENO);
        }
        stdout_pipe.CloseWrite();
      }

      if (cfg.capture_stderr && !cfg.merge_stderr) {
        stderr_pipe.CloseRead();
        dup2(stderr_pipe.WriteEnd(), STDERR_FILENO);
        stderr_pipe.CloseWrite();
      }

      // exec (argv[0] is the program)
      execvp(cfg.argv[0], const_cast<char* const*>(cfg.argv));
      _exit(127);  // exec failed
    }

    // -- Parent process --
    pid_ = child;

    if (cfg.capture_stdout || cfg.merge_stderr) {
      stdout_pipe.CloseWrite();
      stdout_fd_ = stdout_pipe.ReleaseRead();
      detail::SetNonBlocking(stdout_fd_);
    }

    if (cfg.capture_stderr && !cfg.merge_stderr) {
      stderr_pipe.CloseWrite();
      stderr_fd_ = stderr_pipe.ReleaseRead();
      detail::SetNonBlocking(stderr_fd_);
    }

    return ProcessResult::kSuccess;
  }

  /**
   * @brief Read from child's stdout pipe.
   * @param buf Output buffer.
   * @param buf_size Buffer size.
   * @return Bytes read, 0 if no data available or pipe closed, -1 on error.
   */
  int ReadStdout(char* buf, size_t buf_size) { return ReadFd(stdout_fd_, buf, buf_size); }

  /**
   * @brief Read from child's stderr pipe.
   * @param buf Output buffer.
   * @param buf_size Buffer size.
   * @return Bytes read, 0 if no data available or pipe closed, -1 on error.
   */
  int ReadStderr(char* buf, size_t buf_size) { return ReadFd(stderr_fd_, buf, buf_size); }

  /**
   * @brief Read all available stdout into a std::string (blocking until EOF).
   * @return Captured output string.
   */
  std::string ReadAllStdout() { return ReadAllFd(stdout_fd_); }

  /**
   * @brief Read all available stderr into a std::string (blocking until EOF).
   * @return Captured output string.
   */
  std::string ReadAllStderr() { return ReadAllFd(stderr_fd_); }

  /**
   * @brief Wait for child process to exit.
   * @param timeout_ms Timeout in milliseconds (0 = wait forever).
   * @return WaitResult with exit status information.
   */
  WaitResult Wait(uint32_t timeout_ms = 0) {
    WaitResult wr;
    if (pid_ <= 0) {
      wr.exited = true;
      wr.exit_code = -1;
      return wr;
    }

    if (timeout_ms == 0) {
      // Blocking wait
      int status;
      pid_t w = waitpid(pid_, &status, 0);
      if (w > 0) {
        FillWaitResult(status, wr);
        pid_ = -1;
      }
      return wr;
    }

    // Polling wait with timeout
    uint32_t elapsed = 0;
    constexpr uint32_t kPollIntervalMs = 5;
    while (elapsed < timeout_ms) {
      int status;
      pid_t w = waitpid(pid_, &status, WNOHANG);
      if (w > 0) {
        FillWaitResult(status, wr);
        pid_ = -1;
        return wr;
      }
      if (w < 0)
        break;  // error
      detail::SleepMs(kPollIntervalMs);
      elapsed += kPollIntervalMs;
    }

    wr.timed_out = true;
    return wr;
  }

  /**
   * @brief Send a signal to the child process.
   * @param signo Signal number.
   * @return kSuccess if sent, kFailed otherwise.
   */
  ProcessResult Signal(int signo) {
    if (pid_ <= 0)
      return ProcessResult::kFailed;
    return (kill(pid_, signo) == 0) ? ProcessResult::kSuccess : ProcessResult::kFailed;
  }

  /// @brief Get child PID (-1 if not started or already waited).
  pid_t GetPid() const { return pid_; }

  /// @brief Check if child is still running.
  bool IsRunning() const { return pid_ > 0 && IsProcessAlive(pid_); }

 private:
  pid_t pid_;
  int stdout_fd_;
  int stderr_fd_;

  static int ReadFd(int fd, char* buf, size_t buf_size) {
    if (fd < 0 || buf_size == 0)
      return -1;
    ssize_t n = read(fd, buf, buf_size);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return 0;
      return -1;
    }
    return static_cast<int>(n);
  }

  static std::string ReadAllFd(int fd) {
    std::string result;
    if (fd < 0)
      return result;

    // Switch to blocking for drain
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
      fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }

    char buf[4096];
    for (;;) {
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n <= 0)
        break;
      result.append(buf, static_cast<size_t>(n));
    }
    return result;
  }

  static void FillWaitResult(int status, WaitResult& wr) {
    if (WIFEXITED(status)) {
      wr.exited = true;
      wr.exit_code = WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
      wr.signaled = true;
      wr.term_signal = WTERMSIG(status);
    }
  }
};

// ============================================================================
// Convenience: run a command and capture output (like popen but safer)
// ============================================================================

/**
 * @brief Run a command with arguments and capture stdout.
 *
 * Convenience wrapper around Subprocess. Blocks until child exits.
 *
 * @param argv NULL-terminated argument array.
 * @param[out] output Captured stdout content.
 * @param[out] exit_code Child exit code.
 * @return kSuccess if child ran and exited, kFailed on spawn error.
 *
 * @code
 *   const char* argv[] = {"uname", "-r", nullptr};
 *   std::string output;
 *   int code;
 *   osp::RunCommand(argv, output, code);
 * @endcode
 */
inline ProcessResult RunCommand(const char* const* argv, std::string& output, int& exit_code) {
  SubprocessConfig cfg;
  cfg.argv = argv;
  cfg.capture_stdout = true;
  cfg.merge_stderr = true;

  Subprocess proc;
  ProcessResult r = proc.Start(cfg);
  if (r != ProcessResult::kSuccess)
    return r;

  output = proc.ReadAllStdout();
  WaitResult wr = proc.Wait();
  exit_code = wr.exited ? wr.exit_code : -1;
  return ProcessResult::kSuccess;
}

}  // namespace osp

#endif  // defined(OSP_PLATFORM_LINUX)

#endif  // OSP_PROCESS_HPP_
