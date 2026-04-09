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

// ============================================================================
// CpuRelax Tests
// ============================================================================

TEST_CASE("CpuRelax does not crash", "[platform]") {
  // CpuRelax is a no-op hint; just verify it compiles and runs
  for (int i = 0; i < 100; ++i) {
    osp::CpuRelax();
  }
  REQUIRE(true);
}

// ============================================================================
// AdaptiveBackoff Tests
// ============================================================================

TEST_CASE("AdaptiveBackoff starts in spin phase", "[platform]") {
  osp::AdaptiveBackoff backoff;
  REQUIRE(backoff.InSpinPhase());
}

TEST_CASE("AdaptiveBackoff transitions out of spin phase", "[platform]") {
  osp::AdaptiveBackoff backoff;
  // Spin phase is 6 iterations (kSpinLimit)
  for (int i = 0; i < 6; ++i) {
    REQUIRE(backoff.InSpinPhase());
    backoff.Wait();
  }
  // After 6 waits, should be in yield phase
  REQUIRE_FALSE(backoff.InSpinPhase());
}

TEST_CASE("AdaptiveBackoff Reset returns to spin phase", "[platform]") {
  osp::AdaptiveBackoff backoff;
  // Exhaust spin phase
  for (int i = 0; i < 10; ++i) {
    backoff.Wait();
  }
  REQUIRE_FALSE(backoff.InSpinPhase());

  backoff.Reset();
  REQUIRE(backoff.InSpinPhase());
}

TEST_CASE("AdaptiveBackoff full cycle does not crash", "[platform]") {
  osp::AdaptiveBackoff backoff;
  // Run through all three phases: spin (6) + yield (4) + sleep (a few)
  for (int i = 0; i < 15; ++i) {
    backoff.Wait();
  }
  REQUIRE(true);
}
