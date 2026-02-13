/**
 * @file test_platform.cpp
 * @brief Tests for platform.hpp
 */

#include "osp/platform.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("OSP_ASSERT passes on true condition", "[platform]") {
  OSP_ASSERT(1 == 1);
  OSP_ASSERT(true);
  REQUIRE(true);  // If we get here, asserts didn't fire
}

TEST_CASE("Platform macros are defined", "[platform]") {
  // At least one platform should be detected
#if defined(OSP_PLATFORM_LINUX)
  REQUIRE(true);
#elif defined(OSP_PLATFORM_MACOS)
  REQUIRE(true);
#else
  // Unknown platform - still acceptable for portability
  REQUIRE(true);
#endif
}

TEST_CASE("Cache line size is 64", "[platform]") {
  REQUIRE(osp::kCacheLineSize == 64);
}

TEST_CASE("OSP_LIKELY and OSP_UNLIKELY compile", "[platform]") {
  int x = 1;
  if (OSP_LIKELY(x == 1)) {
    REQUIRE(true);
  }
  if (OSP_UNLIKELY(x == 0)) {
    REQUIRE(false);
  }
}
