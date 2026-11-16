/**
 * @file test_process_win.cpp
 * @brief Catch2 tests for osp::process.hpp on the Windows branch (real Win32
 *        implementation: Toolhelp32, CreateProcessW, SuspendThread/ResumeThread,
 *        TerminateProcess).
 *
 * Guarded by OSP_PLATFORM_WINDOWS.
 */

#include "osp/process.hpp"

#include <string>

#include <catch2/catch_test_macros.hpp>

#if defined(OSP_PLATFORM_WINDOWS)

#include <windows.h>

namespace {

// Get the current process's executable file name (e.g. "osp_tests_win.exe").
std::string CurrentExeName() {
  wchar_t path[MAX_PATH];
  const DWORD n = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring wpath(path, n);
  std::string ascii;
  for (wchar_t c : wpath) {
    if (c > 127) break;  // not pure ASCII; bail out
    ascii.push_back(static_cast<char>(c));
  }
  const size_t slash = ascii.find_last_of('\\');
  if (slash != std::string::npos) ascii = ascii.substr(slash + 1);
  return ascii;
}

}  // namespace

TEST_CASE("process_win - FindPidByName finds the current process", "[process][windows]") {
  osp::pid_t pid = -1;
  REQUIRE(osp::FindPidByName(CurrentExeName().c_str(), pid) == osp::ProcessResult::kSuccess);
  REQUIRE(pid == static_cast<osp::pid_t>(::GetCurrentProcessId()));
}

TEST_CASE("process_win - FindPidByName returns kNotFound for nonexistent", "[process][windows]") {
  osp::pid_t pid = -1;
  REQUIRE(osp::FindPidByName("__osp_nonexistent_proc_42__.exe", pid) == osp::ProcessResult::kNotFound);
}

TEST_CASE("process_win - IsProcessAlive", "[process][windows]") {
  REQUIRE(osp::IsProcessAlive(static_cast<osp::pid_t>(::GetCurrentProcessId())));
  REQUIRE_FALSE(osp::IsProcessAlive(0));
  REQUIRE_FALSE(osp::IsProcessAlive(static_cast<osp::pid_t>(0x7FFFFFFF)));  // very unlikely to exist
}

TEST_CASE("process_win - process control on invalid PID returns kFailed", "[process][windows]") {
  // PID 0 is not a valid target on Windows; all control ops must fail cleanly.
  REQUIRE(osp::FreezeProcess(0) == osp::ProcessResult::kFailed);
  REQUIRE(osp::ResumeProcess(0) == osp::ProcessResult::kFailed);
  REQUIRE(osp::TerminateProcess(0) == osp::ProcessResult::kFailed);
  REQUIRE(osp::KillProcess(0) == osp::ProcessResult::kFailed);
}

TEST_CASE("process_win - Subprocess cmd /c exit 7 returns code 7", "[process][windows]") {
  const char* argv[] = {"cmd", "/c", "exit", "7", nullptr};
  osp::SubprocessConfig cfg;
  cfg.argv = argv;

  osp::Subprocess proc;
  REQUIRE(proc.Start(cfg) == osp::ProcessResult::kSuccess);
  REQUIRE(proc.GetPid() > 0);

  auto wr = proc.Wait(5000);
  REQUIRE_FALSE(wr.timed_out);
  REQUIRE(wr.exited);
  REQUIRE(wr.exit_code == 7);
  REQUIRE_FALSE(proc.IsRunning());
}

TEST_CASE("process_win - Subprocess Wait times out and then reaps", "[process][windows]") {
  const char* argv[] = {"cmd", "/c", "ping", "-n", "10", "127.0.0.1", nullptr};
  osp::SubprocessConfig cfg;
  cfg.argv = argv;

  osp::Subprocess proc;
  REQUIRE(proc.Start(cfg) == osp::ProcessResult::kSuccess);
  REQUIRE(proc.GetPid() > 0);
  REQUIRE(proc.IsRunning());

  // Short wait -- should time out (ping -n 10 takes ~9s).
  auto wr = proc.Wait(50);
  REQUIRE(wr.timed_out);
  REQUIRE(proc.IsRunning());

  // Terminate the child and reap it.
  REQUIRE(proc.Signal(9) == osp::ProcessResult::kSuccess);
  auto wr2 = proc.Wait(3000);
  REQUIRE_FALSE(wr2.timed_out);
  REQUIRE_FALSE(proc.IsRunning());
}

TEST_CASE("process_win - Freeze/Resume a live child process", "[process][windows]") {
  const char* argv[] = {"cmd", "/c", "ping", "-n", "8", "127.0.0.1", nullptr};
  osp::SubprocessConfig cfg;
  cfg.argv = argv;

  osp::Subprocess proc;
  REQUIRE(proc.Start(cfg) == osp::ProcessResult::kSuccess);
  const osp::pid_t cpid = proc.GetPid();
  REQUIRE(cpid > 0);
  REQUIRE(proc.IsRunning());

  // Smoke test: a live process can be frozen and resumed.
  REQUIRE(osp::FreezeProcess(cpid) == osp::ProcessResult::kSuccess);
  REQUIRE(osp::IsProcessAlive(cpid));
  // Frozen process cannot exit, so a short wait must time out.
  auto wr = proc.Wait(200);
  REQUIRE(wr.timed_out);

  REQUIRE(osp::ResumeProcess(cpid) == osp::ProcessResult::kSuccess);
  REQUIRE(proc.IsRunning());

  // Terminate to keep the test fast.
  REQUIRE(proc.Signal(9) == osp::ProcessResult::kSuccess);
  auto wr2 = proc.Wait(3000);
  REQUIRE_FALSE(wr2.timed_out);
  REQUIRE_FALSE(proc.IsRunning());
}

TEST_CASE("process_win - RunCommand captures output and exit code", "[process][windows]") {
  const char* argv[] = {"cmd", "/c", "echo", "hello", nullptr};
  std::string output;
  int exit_code = -1;
  REQUIRE(osp::RunCommand(argv, output, exit_code) == osp::ProcessResult::kSuccess);
  REQUIRE(exit_code == 0);
  REQUIRE(output.find("hello") != std::string::npos);
}

TEST_CASE("process_win - Subprocess Start with null argv fails", "[process][windows]") {
  osp::SubprocessConfig cfg;
  cfg.argv = nullptr;

  osp::Subprocess proc;
  REQUIRE(proc.Start(cfg) == osp::ProcessResult::kFailed);
}

TEST_CASE("process_win - Subprocess nonexistent executable fails Start", "[process][windows]") {
  const char* argv[] = {"__osp_nonexistent_cmd_42__", nullptr};
  osp::SubprocessConfig cfg;
  cfg.argv = argv;

  osp::Subprocess proc;
  // CreateProcessW cannot find the module, so Start fails.
  REQUIRE(proc.Start(cfg) == osp::ProcessResult::kFailed);
}

#else  // OSP_PLATFORM_WINDOWS

TEST_CASE("process_win - Windows process tests are Windows-only", "[process][windows]") {
  SKIP("process_win tests only run on Windows");
}

#endif  // OSP_PLATFORM_WINDOWS
