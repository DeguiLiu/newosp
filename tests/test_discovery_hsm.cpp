/**
 * @file test_discovery_hsm.cpp
 * @brief Unit tests for HsmDiscovery.
 */

#include "osp/discovery_hsm.hpp"
#include "osp/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("discovery_hsm - Initial state is Idle", "[HsmDiscovery]") {
  osp::HsmDiscovery<64> discovery;

  CHECK(std::strcmp(discovery.GetState(), "Idle") == 0);
  CHECK(discovery.IsIdle());
  CHECK_FALSE(discovery.IsAnnouncing());
  CHECK_FALSE(discovery.IsDiscovering());
  CHECK_FALSE(discovery.IsStable());
  CHECK_FALSE(discovery.IsDegraded());
  CHECK(discovery.GetDiscoveredCount() == 0U);
  CHECK(discovery.GetLostCount() == 0U);
}

TEST_CASE("discovery_hsm - Start transitions to Announcing", "[HsmDiscovery]") {
  osp::HsmDiscovery<64> discovery;

  discovery.Start();

  CHECK(std::strcmp(discovery.GetState(), "Announcing") == 0);
  CHECK(discovery.IsAnnouncing());
  CHECK_FALSE(discovery.IsIdle());
}

TEST_CASE("discovery_hsm - NodeFound transitions to Discovering", "[HsmDiscovery]") {
  osp::HsmDiscovery<64> discovery;

  discovery.Start();
  CHECK(discovery.IsAnnouncing());

  discovery.OnNodeFound();

  CHECK(std::strcmp(discovery.GetState(), "Discovering") == 0);
  CHECK(discovery.IsDiscovering());
  CHECK_FALSE(discovery.IsAnnouncing());
  CHECK(discovery.GetDiscoveredCount() == 1U);
}

TEST_CASE("discovery_hsm - NetworkStable transitions to Stable", "[HsmDiscovery]") {
  osp::HsmDiscovery<64> discovery;
  discovery.SetStableThreshold(2);

  discovery.Start();
  discovery.OnNodeFound();
  CHECK(discovery.IsDiscovering());

  discovery.OnNodeFound();
  CHECK(discovery.GetDiscoveredCount() == 2U);

  discovery.CheckStability();

  CHECK(std::strcmp(discovery.GetState(), "Stable") == 0);
  CHECK(discovery.IsStable());
  CHECK_FALSE(discovery.IsDiscovering());
}

TEST_CASE("discovery_hsm - NodeLost in Stable transitions to Degraded", "[HsmDiscovery]") {
  osp::HsmDiscovery<64> discovery;
  discovery.SetStableThreshold(2);

  discovery.Start();
  discovery.OnNodeFound();
  discovery.OnNodeFound();
  discovery.CheckStability();
  CHECK(discovery.IsStable());

  discovery.OnNodeLost();

  CHECK(std::strcmp(discovery.GetState(), "Degraded") == 0);
  CHECK(discovery.IsDegraded());
  CHECK_FALSE(discovery.IsStable());
  CHECK(discovery.GetLostCount() == 1U);
}

TEST_CASE("discovery_hsm - NodeFound in Degraded transitions to Discovering", "[HsmDiscovery]") {
  osp::HsmDiscovery<64> discovery;
  discovery.SetStableThreshold(2);

  discovery.Start();
  discovery.OnNodeFound();
  discovery.OnNodeFound();
  discovery.CheckStability();
  CHECK(discovery.IsStable());

  discovery.OnNodeLost();
  CHECK(discovery.IsDegraded());

  discovery.OnNodeFound();

  CHECK(std::strcmp(discovery.GetState(), "Discovering") == 0);
  CHECK(discovery.IsDiscovering());
  CHECK_FALSE(discovery.IsDegraded());
}

TEST_CASE("discovery_hsm - Stop transitions to Stopped", "[HsmDiscovery]") {
  osp::HsmDiscovery<64> discovery;

  discovery.Start();
  discovery.OnNodeFound();
  CHECK(discovery.IsDiscovering());

  discovery.Stop();

  CHECK(std::strcmp(discovery.GetState(), "Stopped") == 0);
  CHECK(discovery.IsStopped());
  CHECK_FALSE(discovery.IsDiscovering());
}

TEST_CASE("discovery_hsm - Stable callback fires", "[HsmDiscovery]") {
  osp::HsmDiscovery<64> discovery;
  discovery.SetStableThreshold(2);

  std::atomic<bool> callback_fired{false};

  discovery.OnStable(
      [](void* ctx) {
        auto* fired = static_cast<std::atomic<bool>*>(ctx);
        fired->store(true);
      },
      &callback_fired);

  discovery.Start();
  discovery.OnNodeFound();
  discovery.OnNodeFound();

  CHECK_FALSE(callback_fired.load());

  discovery.CheckStability();

  CHECK(callback_fired.load());
  CHECK(discovery.IsStable());
}

TEST_CASE("discovery_hsm - Degraded callback fires", "[HsmDiscovery]") {
  osp::HsmDiscovery<64> discovery;
  discovery.SetStableThreshold(2);

  std::atomic<bool> callback_fired{false};

  discovery.OnDegraded(
      [](void* ctx) {
        auto* fired = static_cast<std::atomic<bool>*>(ctx);
        fired->store(true);
      },
      &callback_fired);

  discovery.Start();
  discovery.OnNodeFound();
  discovery.OnNodeFound();
  discovery.CheckStability();
  CHECK(discovery.IsStable());

  CHECK_FALSE(callback_fired.load());

  discovery.OnNodeLost();

  CHECK(callback_fired.load());
  CHECK(discovery.IsDegraded());
}

TEST_CASE("discovery_hsm - Multiple nodes discovered", "[HsmDiscovery]") {
  osp::HsmDiscovery<64> discovery;
  discovery.SetStableThreshold(3);

  discovery.Start();
  CHECK(discovery.GetDiscoveredCount() == 0U);

  discovery.OnNodeFound();
  CHECK(discovery.GetDiscoveredCount() == 1U);
  CHECK(discovery.IsDiscovering());

  discovery.OnNodeFound();
  CHECK(discovery.GetDiscoveredCount() == 2U);
  CHECK(discovery.IsDiscovering());

  discovery.OnNodeFound();
  CHECK(discovery.GetDiscoveredCount() == 3U);
  CHECK(discovery.IsDiscovering());

  discovery.CheckStability();
  CHECK(discovery.IsStable());

  // Continue discovering in Stable state
  discovery.OnNodeFound();
  CHECK(discovery.GetDiscoveredCount() == 4U);
  CHECK(discovery.IsStable());

  discovery.OnNodeFound();
  CHECK(discovery.GetDiscoveredCount() == 5U);
  CHECK(discovery.IsStable());
}
