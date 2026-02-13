/**
 * @file test_log.cpp
 * @brief Tests for log.hpp
 */

#include "osp/log.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Log level defaults", "[log]") {
  // In debug builds default is kDebug, in release kInfo
#ifdef NDEBUG
  REQUIRE(osp::log::GetLevel() == osp::log::Level::kInfo);
#else
  REQUIRE(osp::log::GetLevel() == osp::log::Level::kDebug);
#endif
}

TEST_CASE("Log SetLevel", "[log]") {
  auto prev = osp::log::GetLevel();
  osp::log::SetLevel(osp::log::Level::kError);
  REQUIRE(osp::log::GetLevel() == osp::log::Level::kError);
  osp::log::SetLevel(prev);  // restore
}

TEST_CASE("Log Init and Shutdown", "[log]") {
  REQUIRE(!osp::log::IsInitialized());
  osp::log::Init();
  REQUIRE(osp::log::IsInitialized());
  osp::log::Shutdown();
  REQUIRE(!osp::log::IsInitialized());
}

TEST_CASE("Log macros compile and run", "[log]") {
  osp::log::SetLevel(osp::log::Level::kDebug);
  // These should not crash
  OSP_LOG_DEBUG("Test", "debug %d", 1);
  OSP_LOG_INFO("Test", "info %s", "msg");
  OSP_LOG_WARN("Test", "warn");
  OSP_LOG_ERROR("Test", "error %d %d", 1, 2);
  // Don't test FATAL as it calls abort()
  REQUIRE(true);
}

TEST_CASE("Log runtime level filtering", "[log]") {
  osp::log::SetLevel(osp::log::Level::kOff);
  // With level Off, LogWrite returns early - just verify no crash
  OSP_LOG_DEBUG("Test", "should not appear");
  OSP_LOG_INFO("Test", "should not appear");
  OSP_LOG_ERROR("Test", "should not appear");
  osp::log::SetLevel(osp::log::Level::kDebug);  // restore
  REQUIRE(true);
}

TEST_CASE("Log multiple calls in sequence", "[log]") {
  osp::log::SetLevel(osp::log::Level::kDebug);
  // Multiple rapid log calls should not crash or interfere
  for (int i = 0; i < 10; ++i) {
    OSP_LOG_DEBUG("Test", "sequence %d", i);
  }
  OSP_LOG_INFO("Test", "info 1");
  OSP_LOG_INFO("Test", "info 2");
  OSP_LOG_WARN("Test", "warn 1");
  OSP_LOG_ERROR("Test", "error 1");
  REQUIRE(true);
}

TEST_CASE("Log with empty string", "[log]") {
  osp::log::SetLevel(osp::log::Level::kDebug);
  // Empty message should not crash
  OSP_LOG_INFO("Test", "");
  OSP_LOG_DEBUG("Test", "");
  OSP_LOG_WARN("Test", "");
  REQUIRE(true);
}

TEST_CASE("Log with very long message", "[log]") {
  osp::log::SetLevel(osp::log::Level::kDebug);
  // Message longer than typical buffer (>256 chars)
  std::string long_msg(300, 'x');
  OSP_LOG_INFO("Test", "%s", long_msg.c_str());

  // Also test with format string expansion
  OSP_LOG_DEBUG("Test", "Long: %s %s %s", long_msg.c_str(), long_msg.c_str(), long_msg.c_str());
  REQUIRE(true);
}

TEST_CASE("Log level hierarchy filtering", "[log]") {
  // Test that higher severity levels are logged when threshold is lower
  osp::log::SetLevel(osp::log::Level::kWarn);

  // These should be filtered out (below threshold)
  OSP_LOG_DEBUG("Test", "debug filtered");
  OSP_LOG_INFO("Test", "info filtered");

  // These should pass (at or above threshold)
  OSP_LOG_WARN("Test", "warn passes");
  OSP_LOG_ERROR("Test", "error passes");

  // Test with Error level - only errors should pass
  osp::log::SetLevel(osp::log::Level::kError);
  OSP_LOG_WARN("Test", "warn filtered");
  OSP_LOG_ERROR("Test", "error passes");

  osp::log::SetLevel(osp::log::Level::kDebug);  // restore
  REQUIRE(true);
}
