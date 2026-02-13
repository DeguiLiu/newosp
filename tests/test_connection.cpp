/**
 * @file test_connection.cpp
 * @brief Tests for connection.hpp
 */

#include "osp/connection.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <thread>

// Use a small pool for easy capacity testing
using TestPool = osp::ConnectionPool<8>;

TEST_CASE("connection - default pool is empty", "[connection]") {
  TestPool pool;
  REQUIRE(pool.Count() == 0U);
  REQUIRE(pool.IsEmpty());
  REQUIRE(!pool.IsFull());
  REQUIRE(pool.Capacity() == 8U);
}

TEST_CASE("connection - add returns valid id", "[connection]") {
  TestPool pool;
  auto result = pool.Add(0x7F000001U, 8080, "server-1");
  REQUIRE(result.has_value());
  REQUIRE(result.value().IsValid());
  REQUIRE(pool.Count() == 1U);
  REQUIRE(!pool.IsEmpty());
}

TEST_CASE("connection - find returns correct info", "[connection]") {
  TestPool pool;
  auto result = pool.Add(0xC0A80001U, 443, "https-peer");
  REQUIRE(result.has_value());

  const auto id = result.value();
  const osp::ConnectionInfo* info = pool.Find(id);
  REQUIRE(info != nullptr);
  REQUIRE(info->id == id);
  REQUIRE(info->state == osp::ConnectionState::kIdle);
  REQUIRE(info->remote_ip == 0xC0A80001U);
  REQUIRE(info->remote_port == 443);
  REQUIRE(std::strcmp(info->label, "https-peer") == 0);
  REQUIRE(info->created_time_us > 0U);
  REQUIRE(info->last_active_us > 0U);
}

TEST_CASE("connection - remove connection", "[connection]") {
  TestPool pool;
  auto result = pool.Add(0x0A000001U, 9090);
  REQUIRE(result.has_value());

  const auto id = result.value();
  REQUIRE(pool.Count() == 1U);

  auto rm = pool.Remove(id);
  REQUIRE(rm.has_value());
  REQUIRE(pool.Count() == 0U);
  REQUIRE(pool.IsEmpty());

  // Find should return nullptr after removal
  REQUIRE(pool.Find(id) == nullptr);
}

TEST_CASE("connection - remove non-existent returns error", "[connection]") {
  TestPool pool;
  auto rm = pool.Remove(osp::ConnectionId{42U});
  REQUIRE(!rm.has_value());
  REQUIRE(rm.get_error() == osp::ConnectionError::kNotFound);
}

TEST_CASE("connection - pool full returns kPoolFull", "[connection]") {
  osp::ConnectionPool<4> pool;

  for (uint16_t i = 0; i < 4; ++i) {
    auto r = pool.Add(0x7F000001U, static_cast<uint16_t>(8080U + i));
    REQUIRE(r.has_value());
  }

  REQUIRE(pool.IsFull());
  REQUIRE(pool.Count() == 4U);

  auto overflow = pool.Add(0x7F000001U, 9999);
  REQUIRE(!overflow.has_value());
  REQUIRE(overflow.get_error() == osp::ConnectionError::kPoolFull);
}

TEST_CASE("connection - set state updates state", "[connection]") {
  TestPool pool;
  auto result = pool.Add(0x0A000001U, 5000);
  REQUIRE(result.has_value());

  const auto id = result.value();

  auto s = pool.SetState(id, osp::ConnectionState::kConnecting);
  REQUIRE(s.has_value());

  const auto* info = pool.Find(id);
  REQUIRE(info != nullptr);
  REQUIRE(info->state == osp::ConnectionState::kConnecting);

  s = pool.SetState(id, osp::ConnectionState::kConnected);
  REQUIRE(s.has_value());
  REQUIRE(pool.Find(id)->state == osp::ConnectionState::kConnected);
}

TEST_CASE("connection - touch updates last_active_us", "[connection]") {
  TestPool pool;
  auto result = pool.Add(0x0A000001U, 6000);
  REQUIRE(result.has_value());

  const auto id = result.value();
  const auto* info = pool.Find(id);
  REQUIRE(info != nullptr);

  const uint64_t old_active = info->last_active_us;

  // Small sleep to ensure timestamp advances
  std::this_thread::sleep_for(std::chrono::milliseconds(2));

  auto t = pool.Touch(id);
  REQUIRE(t.has_value());

  REQUIRE(pool.Find(id)->last_active_us > old_active);
}

TEST_CASE("connection - foreach iterates all active", "[connection]") {
  TestPool pool;
  pool.Add(0x01U, 1000, "a");
  pool.Add(0x02U, 2000, "b");
  pool.Add(0x03U, 3000, "c");

  uint32_t visited = 0U;
  pool.ForEach([&visited](const osp::ConnectionInfo& info) {
    (void)info;
    ++visited;
  });
  REQUIRE(visited == 3U);

  // Remove one, iterate again
  auto result = pool.Add(0x04U, 4000, "d");
  REQUIRE(result.has_value());
  pool.Remove(result.value());

  visited = 0U;
  pool.ForEach([&visited](const osp::ConnectionInfo& /*info*/) {
    ++visited;
  });
  REQUIRE(visited == 3U);
}

TEST_CASE("connection - remove timed out removes stale", "[connection]") {
  TestPool pool;
  pool.Add(0x01U, 1000, "stale-1");
  pool.Add(0x02U, 2000, "stale-2");

  // Sleep to let these connections age
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // Add a fresh connection
  pool.Add(0x03U, 3000, "fresh");

  REQUIRE(pool.Count() == 3U);

  // Remove connections older than 10ms (10000us)
  uint32_t removed = pool.RemoveTimedOut(10000U);
  REQUIRE(removed == 2U);
  REQUIRE(pool.Count() == 1U);

  // Verify the fresh one survived
  uint32_t remaining = 0U;
  pool.ForEach([&remaining](const osp::ConnectionInfo& info) {
    REQUIRE(std::strcmp(info.label, "fresh") == 0);
    ++remaining;
  });
  REQUIRE(remaining == 1U);
}

TEST_CASE("connection - clear empties pool", "[connection]") {
  TestPool pool;
  pool.Add(0x01U, 1000);
  pool.Add(0x02U, 2000);
  pool.Add(0x03U, 3000);
  REQUIRE(pool.Count() == 3U);

  pool.Clear();
  REQUIRE(pool.Count() == 0U);
  REQUIRE(pool.IsEmpty());
  REQUIRE(!pool.IsFull());
}

TEST_CASE("connection - connection id validity", "[connection]") {
  osp::ConnectionId valid_id{0U};
  REQUIRE(valid_id.IsValid());

  osp::ConnectionId also_valid{100U};
  REQUIRE(also_valid.IsValid());

  osp::ConnectionId invalid = osp::ConnectionId::Invalid();
  REQUIRE(!invalid.IsValid());
  REQUIRE(invalid.value == UINT32_MAX);

  // Equality / inequality
  osp::ConnectionId a{5U};
  osp::ConnectionId b{5U};
  osp::ConnectionId c{6U};
  REQUIRE(a == b);
  REQUIRE(a != c);
}

TEST_CASE("connection - remove with invalid id returns error", "[connection]") {
  TestPool pool;
  auto rm = pool.Remove(osp::ConnectionId::Invalid());
  REQUIRE(!rm.has_value());
  REQUIRE(rm.get_error() == osp::ConnectionError::kInvalidId);
}

TEST_CASE("connection - set state on non-existent returns error",
          "[connection]") {
  TestPool pool;
  auto s = pool.SetState(osp::ConnectionId{999U},
                         osp::ConnectionState::kConnected);
  REQUIRE(!s.has_value());
  REQUIRE(s.get_error() == osp::ConnectionError::kNotFound);
}

TEST_CASE("connection - label truncation at 31 chars", "[connection]") {
  TestPool pool;
  // 40-char label -- should be truncated to 31
  const char* long_label = "abcdefghijklmnopqrstuvwxyz012345678";
  auto result = pool.Add(0x01U, 1000, long_label);
  REQUIRE(result.has_value());

  const auto* info = pool.Find(result.value());
  REQUIRE(info != nullptr);
  REQUIRE(std::strlen(info->label) == 31U);
  REQUIRE(std::strncmp(info->label, long_label, 31U) == 0);
}

TEST_CASE("connection - add after remove reuses slot", "[connection]") {
  osp::ConnectionPool<2> pool;

  auto r1 = pool.Add(0x01U, 1000);
  auto r2 = pool.Add(0x02U, 2000);
  REQUIRE(r1.has_value());
  REQUIRE(r2.has_value());
  REQUIRE(pool.IsFull());

  // Remove first, add again should succeed
  pool.Remove(r1.value());
  REQUIRE(pool.Count() == 1U);

  auto r3 = pool.Add(0x03U, 3000);
  REQUIRE(r3.has_value());
  REQUIRE(pool.IsFull());
}
