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
