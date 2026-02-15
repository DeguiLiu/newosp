/**
 * @file test_process.cpp
 * @brief Tests for process.hpp
 */

#include "osp/process.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>

// ============================================================================
// Helper: fork a dummy child process that sleeps
// ============================================================================

static pid_t SpawnSleeper() {
  pid_t pid = fork();
  if (pid == 0) {
    // Child: sleep indefinitely
    while (true) {
      pause();
    }
    _exit(0);
  }
  return pid;
}

static void CleanupChild(pid_t pid) {
  kill(pid, SIGKILL);
  int status;
  waitpid(pid, &status, 0);
}

// ============================================================================
// ProcessResult enum
// ============================================================================

TEST_CASE("ProcessResult values", "[process]") {
  REQUIRE(static_cast<int8_t>(osp::ProcessResult::kSuccess) == 0);
  REQUIRE(static_cast<int8_t>(osp::ProcessResult::kNotFound) == -1);
  REQUIRE(static_cast<int8_t>(osp::ProcessResult::kFailed) == -2);
  REQUIRE(static_cast<int8_t>(osp::ProcessResult::kWaitError) == -3);
}

// ============================================================================
// IsProcessStopped
// ============================================================================

TEST_CASE("IsProcessStopped returns false for running process", "[process]") {
  // Current process (self) is running, not stopped
  REQUIRE_FALSE(osp::IsProcessStopped(getpid()));
}

TEST_CASE("IsProcessStopped returns false for nonexistent PID", "[process]") {
  // PID 0 is kernel, very unlikely to be stopped; large PID unlikely to exist
  REQUIRE_FALSE(osp::IsProcessStopped(999999));
}

TEST_CASE("IsProcessStopped detects stopped child", "[process]") {
  pid_t child = SpawnSleeper();
  REQUIRE(child > 0);

  // Stop the child
  REQUIRE(kill(child, SIGSTOP) == 0);
  usleep(5000);  // 5ms for state change

  REQUIRE(osp::IsProcessStopped(child));

  // Resume and verify
  REQUIRE(kill(child, SIGCONT) == 0);
  usleep(5000);
  REQUIRE_FALSE(osp::IsProcessStopped(child));

  CleanupChild(child);
}

// ============================================================================
// FindPidByName
// ============================================================================

TEST_CASE("FindPidByName finds init or systemd", "[process]") {
  // PID 1 is typically "init" or "systemd"
  pid_t pid;
  // Try systemd first, then init
  auto r = osp::FindPidByName("systemd", pid);
  if (r != osp::ProcessResult::kSuccess) {
    r = osp::FindPidByName("init", pid);
  }
  // In containers, PID 1 might be something else, so we allow not found
  if (r == osp::ProcessResult::kSuccess) {
    REQUIRE(pid > 0);
  }
}

TEST_CASE("FindPidByName returns kNotFound for nonexistent", "[process]") {
  pid_t pid;
  auto r = osp::FindPidByName("__osp_nonexistent_process_42__", pid);
  REQUIRE(r == osp::ProcessResult::kNotFound);
}

TEST_CASE("FindPidByName does exact match, not substring", "[process]") {
  // "sh" should not match "bash" or "sshd"
  pid_t pid_sh, pid_bash;
  auto r_bash = osp::FindPidByName("bash", pid_bash);
  auto r_sh = osp::FindPidByName("sh", pid_sh);

  // If both found, they should be different PIDs (unless "sh" is actually sh)
  if (r_bash == osp::ProcessResult::kSuccess &&
      r_sh == osp::ProcessResult::kSuccess) {
    // "sh" PID's comm should actually be "sh", not "bash"
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", static_cast<int>(pid_sh));
    char buf[64];
    int n = open(path, O_RDONLY);  // NOLINT
    if (n >= 0) {
      ssize_t sz = read(n, buf, sizeof(buf) - 1);
      close(n);  // NOLINT
      if (sz > 0) {
        buf[sz] = '\0';
        if (sz > 0 && buf[sz - 1] == '\n') buf[sz - 1] = '\0';
        // The comm should be "sh", not "bash"
        REQUIRE(std::string(buf) == "sh");
      }
    }
  }
}

// ============================================================================
// FreezeProcess / ResumeProcess
// ============================================================================

TEST_CASE("Freeze and resume child process by PID", "[process]") {
  pid_t child = SpawnSleeper();
  REQUIRE(child > 0);

  SECTION("FreezeProcess stops the child") {
    auto r = osp::FreezeProcess(child);
    REQUIRE(r == osp::ProcessResult::kSuccess);
    REQUIRE(osp::IsProcessStopped(child));
  }

  SECTION("ResumeProcess resumes a frozen child") {
    REQUIRE(osp::FreezeProcess(child) == osp::ProcessResult::kSuccess);
    auto r = osp::ResumeProcess(child);
    REQUIRE(r == osp::ProcessResult::kSuccess);
    REQUIRE_FALSE(osp::IsProcessStopped(child));
  }

  SECTION("ResumeProcess on already-running process succeeds") {
    auto r = osp::ResumeProcess(child);
    REQUIRE(r == osp::ProcessResult::kSuccess);
  }

  CleanupChild(child);
}

TEST_CASE("FreezeProcess fails for nonexistent PID", "[process]") {
  auto r = osp::FreezeProcess(999999);
  REQUIRE(r == osp::ProcessResult::kFailed);
}

// ============================================================================
// TerminateProcess
// ============================================================================

TEST_CASE("TerminateProcess kills child process", "[process]") {
  pid_t child = SpawnSleeper();
  REQUIRE(child > 0);

  auto r = osp::TerminateProcess(child);
  REQUIRE(r == osp::ProcessResult::kSuccess);

  // Verify child is gone
  REQUIRE(kill(child, 0) != 0);
}

TEST_CASE("TerminateProcess fails for nonexistent PID", "[process]") {
  auto r = osp::TerminateProcess(999999);
  REQUIRE(r == osp::ProcessResult::kFailed);
}

// ============================================================================
// KillProcess
// ============================================================================

TEST_CASE("KillProcess sends SIGKILL without wait", "[process]") {
  pid_t child = SpawnSleeper();
  REQUIRE(child > 0);

  auto r = osp::KillProcess(child);
  REQUIRE(r == osp::ProcessResult::kSuccess);

  // Reap the zombie
  int status;
  waitpid(child, &status, 0);
}

// ============================================================================
// detail helpers
// ============================================================================

TEST_CASE("detail::IsDigitString", "[process]") {
  REQUIRE(osp::detail::IsDigitString("12345"));
  REQUIRE(osp::detail::IsDigitString("0"));
  REQUIRE_FALSE(osp::detail::IsDigitString(""));
  REQUIRE_FALSE(osp::detail::IsDigitString("abc"));
  REQUIRE_FALSE(osp::detail::IsDigitString("123a"));
  REQUIRE_FALSE(osp::detail::IsDigitString("."));
}

TEST_CASE("detail::Basename", "[process]") {
  REQUIRE(std::string(osp::detail::Basename("/usr/bin/foo")) == "foo");
  REQUIRE(std::string(osp::detail::Basename("foo")) == "foo");
  REQUIRE(std::string(osp::detail::Basename("/foo")) == "foo");
  REQUIRE(std::string(osp::detail::Basename("a/b/c")) == "c");
}

TEST_CASE("detail::SleepMs does not crash", "[process]") {
  osp::detail::SleepMs(1);  // 1ms, just verify no crash
}

// ============================================================================
// IsProcessAlive / ReadProcessState
// ============================================================================

TEST_CASE("IsProcessAlive returns true for self", "[process]") {
  REQUIRE(osp::IsProcessAlive(getpid()));
}

TEST_CASE("IsProcessAlive returns false for nonexistent PID", "[process]") {
  REQUIRE_FALSE(osp::IsProcessAlive(999999));
}

TEST_CASE("ReadProcessState returns valid state for self", "[process]") {
  char state = osp::ReadProcessState(getpid());
  // Test process should be running (R) or sleeping (S)
  REQUIRE((state == 'R' || state == 'S'));
}

TEST_CASE("ReadProcessState returns NUL for nonexistent PID", "[process]") {
  REQUIRE(osp::ReadProcessState(999999) == '\0');
}

// ============================================================================
// Subprocess - spawn and capture
// ============================================================================

TEST_CASE("Subprocess echo captures stdout", "[process]") {
  const char* argv[] = {"echo", "hello osp", nullptr};
  osp::SubprocessConfig cfg;
  cfg.argv = argv;
  cfg.capture_stdout = true;

  osp::Subprocess proc;
  REQUIRE(proc.Start(cfg) == osp::ProcessResult::kSuccess);
  REQUIRE(proc.GetPid() > 0);

  std::string output = proc.ReadAllStdout();
  auto wr = proc.Wait();

  REQUIRE(wr.exited);
  REQUIRE(wr.exit_code == 0);
  REQUIRE(output == "hello osp\n");
}

TEST_CASE("Subprocess captures stderr separately", "[process]") {
  // "ls --invalid-option" writes to stderr
  const char* argv[] = {"ls", "--invalid-option-xyz", nullptr};
  osp::SubprocessConfig cfg;
  cfg.argv = argv;
  cfg.capture_stdout = true;
  cfg.capture_stderr = true;

  osp::Subprocess proc;
  REQUIRE(proc.Start(cfg) == osp::ProcessResult::kSuccess);

  std::string err = proc.ReadAllStderr();
  auto wr = proc.Wait();

  REQUIRE(wr.exited);
  REQUIRE(wr.exit_code != 0);
  REQUIRE_FALSE(err.empty());
}

TEST_CASE("Subprocess merge_stderr combines into stdout", "[process]") {
  const char* argv[] = {"ls", "--invalid-option-xyz", nullptr};
  osp::SubprocessConfig cfg;
  cfg.argv = argv;
  cfg.capture_stdout = true;
  cfg.merge_stderr = true;

  osp::Subprocess proc;
  REQUIRE(proc.Start(cfg) == osp::ProcessResult::kSuccess);

  std::string output = proc.ReadAllStdout();
  auto wr = proc.Wait();

  REQUIRE(wr.exited);
  REQUIRE_FALSE(output.empty());  // stderr merged into stdout
}

TEST_CASE("Subprocess nonexistent command exits 127", "[process]") {
  const char* argv[] = {"__osp_nonexistent_cmd__", nullptr};
  osp::SubprocessConfig cfg;
  cfg.argv = argv;

  osp::Subprocess proc;
  REQUIRE(proc.Start(cfg) == osp::ProcessResult::kSuccess);

  auto wr = proc.Wait();
  REQUIRE(wr.exited);
  REQUIRE(wr.exit_code == 127);
}

TEST_CASE("Subprocess Wait with timeout", "[process]") {
  const char* argv[] = {"sleep", "10", nullptr};
  osp::SubprocessConfig cfg;
  cfg.argv = argv;

  osp::Subprocess proc;
  REQUIRE(proc.Start(cfg) == osp::ProcessResult::kSuccess);
  REQUIRE(proc.IsRunning());

  // Wait with short timeout -- should time out
  auto wr = proc.Wait(50);
  REQUIRE(wr.timed_out);
  REQUIRE(proc.IsRunning());

  // Kill it
  REQUIRE(proc.Signal(SIGKILL) == osp::ProcessResult::kSuccess);
  auto wr2 = proc.Wait(1000);
  REQUIRE_FALSE(wr2.timed_out);
  REQUIRE(wr2.signaled);
  REQUIRE(wr2.term_signal == SIGKILL);
}

TEST_CASE("Subprocess Signal sends SIGUSR1", "[process]") {
  // Use SIGUSR1 instead of SIGTERM to avoid Catch2's signal handler
  // interference. SIGUSR1 default action is terminate, same as SIGTERM.
  const char* argv[] = {"sleep", "10", nullptr};
  osp::SubprocessConfig cfg;
  cfg.argv = argv;

  osp::Subprocess proc;
  REQUIRE(proc.Start(cfg) == osp::ProcessResult::kSuccess);

  osp::detail::SleepMs(50);  // let child start

  REQUIRE(proc.Signal(SIGUSR1) == osp::ProcessResult::kSuccess);
  auto wr = proc.Wait(2000);

  REQUIRE_FALSE(wr.timed_out);
  REQUIRE(wr.signaled);
  REQUIRE(wr.term_signal == SIGUSR1);
}

TEST_CASE("Subprocess move semantics", "[process]") {
  const char* argv[] = {"echo", "move", nullptr};
  osp::SubprocessConfig cfg;
  cfg.argv = argv;
  cfg.capture_stdout = true;

  osp::Subprocess proc;
  REQUIRE(proc.Start(cfg) == osp::ProcessResult::kSuccess);
  pid_t original_pid = proc.GetPid();

  // Move construct
  osp::Subprocess proc2(std::move(proc));
  REQUIRE(proc.GetPid() == -1);  // NOLINT(bugprone-use-after-move)
  REQUIRE(proc2.GetPid() == original_pid);

  std::string output = proc2.ReadAllStdout();
  auto wr = proc2.Wait();
  REQUIRE(wr.exited);
  REQUIRE(output == "move\n");
}

TEST_CASE("Subprocess destructor kills running child", "[process]") {
  pid_t child_pid;
  {
    const char* argv[] = {"sleep", "60", nullptr};
    osp::SubprocessConfig cfg;
    cfg.argv = argv;

    osp::Subprocess proc;
    REQUIRE(proc.Start(cfg) == osp::ProcessResult::kSuccess);
    child_pid = proc.GetPid();
    REQUIRE(osp::IsProcessAlive(child_pid));
    // proc goes out of scope here -- destructor should kill child
  }
  // Give a moment for cleanup
  osp::detail::SleepMs(10);
  REQUIRE_FALSE(osp::IsProcessAlive(child_pid));
}

TEST_CASE("Subprocess Start with null argv fails", "[process]") {
  osp::SubprocessConfig cfg;
  cfg.argv = nullptr;

  osp::Subprocess proc;
  REQUIRE(proc.Start(cfg) == osp::ProcessResult::kFailed);
}

TEST_CASE("Subprocess working_dir", "[process]") {
  const char* argv[] = {"pwd", nullptr};
  osp::SubprocessConfig cfg;
  cfg.argv = argv;
  cfg.capture_stdout = true;
  cfg.working_dir = "/tmp";

  osp::Subprocess proc;
  REQUIRE(proc.Start(cfg) == osp::ProcessResult::kSuccess);

  std::string output = proc.ReadAllStdout();
  auto wr = proc.Wait();
  REQUIRE(wr.exited);
  REQUIRE(wr.exit_code == 0);
  // Output should contain /tmp
  REQUIRE(output.find("/tmp") != std::string::npos);
}

// ============================================================================
// RunCommand convenience
// ============================================================================

TEST_CASE("RunCommand captures uname output", "[process]") {
  const char* argv[] = {"uname", "-s", nullptr};
  std::string output;
  int code;
  auto r = osp::RunCommand(argv, output, code);
  REQUIRE(r == osp::ProcessResult::kSuccess);
  REQUIRE(code == 0);
  REQUIRE(output.find("Linux") != std::string::npos);
}

TEST_CASE("RunCommand reports nonzero exit code", "[process]") {
  const char* argv[] = {"false", nullptr};
  std::string output;
  int code;
  auto r = osp::RunCommand(argv, output, code);
  REQUIRE(r == osp::ProcessResult::kSuccess);
  REQUIRE(code != 0);
}
