/**
 * @file test_io_poller.cpp
 * @brief Tests for io_poller.hpp: IoPoller, PollResult, IoEvent.
 */

#include <catch2/catch_test_macros.hpp>
#include "osp/io_poller.hpp"

#include <cstring>
#include <unistd.h>

// ============================================================================
// Default construction
// ============================================================================

TEST_CASE("io_poller - default construction is valid", "[io_poller]") {
  osp::IoPoller poller;
  REQUIRE(poller.IsValid());
  REQUIRE(poller.Fd() >= 0);
}

// ============================================================================
// Add fd
// ============================================================================

TEST_CASE("io_poller - Add fd succeeds", "[io_poller]") {
  osp::IoPoller poller;
  REQUIRE(poller.IsValid());

  int pipefd[2];
  REQUIRE(::pipe(pipefd) == 0);

  auto r = poller.Add(pipefd[0], static_cast<uint8_t>(osp::IoEvent::kReadable));
  REQUIRE(r.has_value());

  ::close(pipefd[0]);
  ::close(pipefd[1]);
}

// ============================================================================
// Remove fd
// ============================================================================

TEST_CASE("io_poller - Remove fd succeeds", "[io_poller]") {
  osp::IoPoller poller;
  REQUIRE(poller.IsValid());

  int pipefd[2];
  REQUIRE(::pipe(pipefd) == 0);

  auto add_r = poller.Add(pipefd[0],
                           static_cast<uint8_t>(osp::IoEvent::kReadable));
  REQUIRE(add_r.has_value());

  auto rm_r = poller.Remove(pipefd[0]);
  REQUIRE(rm_r.has_value());

  ::close(pipefd[0]);
  ::close(pipefd[1]);
}

// ============================================================================
// Wait with non-blocking timeout returns 0
// ============================================================================

TEST_CASE("io_poller - Wait non-blocking returns 0 when no events",
          "[io_poller]") {
  osp::IoPoller poller;
  REQUIRE(poller.IsValid());

  int pipefd[2];
  REQUIRE(::pipe(pipefd) == 0);

  auto add_r = poller.Add(pipefd[0],
                           static_cast<uint8_t>(osp::IoEvent::kReadable));
  REQUIRE(add_r.has_value());

  // Non-blocking poll: no data written, should return 0 events
  osp::PollResult results[4];
  auto wait_r = poller.Wait(results, 4, 0);
  REQUIRE(wait_r.has_value());
  REQUIRE(wait_r.value() == 0);

  ::close(pipefd[0]);
  ::close(pipefd[1]);
}

// ============================================================================
// Readable event on pipe
// ============================================================================

TEST_CASE("io_poller - readable event on pipe", "[io_poller]") {
  osp::IoPoller poller;
  REQUIRE(poller.IsValid());

  int pipefd[2];
  REQUIRE(::pipe(pipefd) == 0);

  auto add_r = poller.Add(pipefd[0],
                           static_cast<uint8_t>(osp::IoEvent::kReadable));
  REQUIRE(add_r.has_value());

  // Write data to make the read end readable
  const char msg[] = "test";
  ssize_t written = ::write(pipefd[1], msg, sizeof(msg));
  REQUIRE(written == static_cast<ssize_t>(sizeof(msg)));

  osp::PollResult results[4];
  auto wait_r = poller.Wait(results, 4, 100);
  REQUIRE(wait_r.has_value());
  REQUIRE(wait_r.value() >= 1);
  REQUIRE(results[0].fd == pipefd[0]);
  REQUIRE((results[0].events & static_cast<uint8_t>(osp::IoEvent::kReadable)) != 0);

  // Drain the pipe
  char buf[64];
  (void)::read(pipefd[0], buf, sizeof(buf));

  ::close(pipefd[0]);
  ::close(pipefd[1]);
}

// ============================================================================
// Modify events
// ============================================================================

TEST_CASE("io_poller - modify events", "[io_poller]") {
  osp::IoPoller poller;
  REQUIRE(poller.IsValid());

  int pipefd[2];
  REQUIRE(::pipe(pipefd) == 0);

  // Add with readable
  auto add_r = poller.Add(pipefd[0],
                           static_cast<uint8_t>(osp::IoEvent::kReadable));
  REQUIRE(add_r.has_value());

  // Modify to writable only (pipe read end is not writable, so this tests
  // that Modify itself does not fail)
  auto mod_r = poller.Modify(pipefd[0],
                              static_cast<uint8_t>(osp::IoEvent::kWritable));
  REQUIRE(mod_r.has_value());

  ::close(pipefd[0]);
  ::close(pipefd[1]);
}

// ============================================================================
// Move construction
// ============================================================================

TEST_CASE("io_poller - move construction", "[io_poller]") {
  osp::IoPoller a;
  REQUIRE(a.IsValid());
  int original_fd = a.Fd();

  osp::IoPoller b(static_cast<osp::IoPoller&&>(a));
  REQUIRE(!a.IsValid());
  REQUIRE(a.Fd() < 0);
  REQUIRE(b.IsValid());
  REQUIRE(b.Fd() == original_fd);
}

// ============================================================================
// Results() accessor with internal buffer
// ============================================================================

TEST_CASE("io_poller - Results accessor with internal buffer", "[io_poller]") {
  osp::IoPoller poller;
  REQUIRE(poller.IsValid());

  int pipefd[2];
  REQUIRE(::pipe(pipefd) == 0);

  auto add_r = poller.Add(pipefd[0],
                           static_cast<uint8_t>(osp::IoEvent::kReadable));
  REQUIRE(add_r.has_value());

  // Write to make readable
  const char msg[] = "data";
  (void)::write(pipefd[1], msg, sizeof(msg));

  // Use the convenience Wait overload (internal buffer)
  auto wait_r = poller.Wait(100);
  REQUIRE(wait_r.has_value());
  REQUIRE(wait_r.value() >= 1);

  const osp::PollResult* res = poller.Results();
  REQUIRE(res != nullptr);
  REQUIRE(res[0].fd == pipefd[0]);
  REQUIRE((res[0].events & static_cast<uint8_t>(osp::IoEvent::kReadable)) != 0);

  // Drain the pipe
  char buf[64];
  (void)::read(pipefd[0], buf, sizeof(buf));

  ::close(pipefd[0]);
  ::close(pipefd[1]);
}
