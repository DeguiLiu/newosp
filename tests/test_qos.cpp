/**
 * @file test_qos.cpp
 * @brief Unit tests for QoS module.
 */

#include "osp/qos.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace osp;

TEST_CASE("QosProfile default values", "[qos]") {
  QosProfile profile;

  REQUIRE(profile.reliability == ReliabilityPolicy::kBestEffort);
  REQUIRE(profile.history == HistoryPolicy::kKeepLast);
  REQUIRE(profile.durability == DurabilityPolicy::kVolatile);
  REQUIRE(profile.history_depth == 10);
  REQUIRE(profile.deadline_ms == 0);
  REQUIRE(profile.lifespan_ms == 0);
}

TEST_CASE("QosProfile predefined profiles", "[qos]") {
  SECTION("QosSensorData") {
    REQUIRE(QosSensorData.reliability == ReliabilityPolicy::kBestEffort);
    REQUIRE(QosSensorData.history == HistoryPolicy::kKeepLast);
    REQUIRE(QosSensorData.durability == DurabilityPolicy::kVolatile);
    REQUIRE(QosSensorData.history_depth == 1);
    REQUIRE(QosSensorData.deadline_ms == 0);
    REQUIRE(QosSensorData.lifespan_ms == 0);
  }

  SECTION("QosControlCommand") {
    REQUIRE(QosControlCommand.reliability == ReliabilityPolicy::kReliable);
    REQUIRE(QosControlCommand.history == HistoryPolicy::kKeepAll);
    REQUIRE(QosControlCommand.durability == DurabilityPolicy::kVolatile);
    REQUIRE(QosControlCommand.history_depth == 0);
    REQUIRE(QosControlCommand.deadline_ms == 100);
    REQUIRE(QosControlCommand.lifespan_ms == 0);
  }

  SECTION("QosSystemDefault") {
    REQUIRE(QosSystemDefault.reliability == ReliabilityPolicy::kBestEffort);
    REQUIRE(QosSystemDefault.history == HistoryPolicy::kKeepLast);
    REQUIRE(QosSystemDefault.durability == DurabilityPolicy::kVolatile);
    REQUIRE(QosSystemDefault.history_depth == 10);
    REQUIRE(QosSystemDefault.deadline_ms == 0);
    REQUIRE(QosSystemDefault.lifespan_ms == 0);
  }

  SECTION("QosReliableSensor") {
    REQUIRE(QosReliableSensor.reliability == ReliabilityPolicy::kReliable);
    REQUIRE(QosReliableSensor.history == HistoryPolicy::kKeepLast);
    REQUIRE(QosReliableSensor.durability == DurabilityPolicy::kVolatile);
    REQUIRE(QosReliableSensor.history_depth == 5);
    REQUIRE(QosReliableSensor.deadline_ms == 0);
    REQUIRE(QosReliableSensor.lifespan_ms == 1000);
  }
}

TEST_CASE("QosProfile equality and inequality", "[qos]") {
  QosProfile profile1;
  QosProfile profile2;

  SECTION("Default profiles are equal") {
    REQUIRE(profile1 == profile2);
    REQUIRE_FALSE(profile1 != profile2);
  }

  SECTION("Different reliability") {
    profile2.reliability = ReliabilityPolicy::kReliable;
    REQUIRE(profile1 != profile2);
    REQUIRE_FALSE(profile1 == profile2);
  }

  SECTION("Different history") {
    profile2.history = HistoryPolicy::kKeepAll;
    REQUIRE(profile1 != profile2);
    REQUIRE_FALSE(profile1 == profile2);
  }

  SECTION("Different durability") {
    profile2.durability = DurabilityPolicy::kTransientLocal;
    REQUIRE(profile1 != profile2);
    REQUIRE_FALSE(profile1 == profile2);
  }

  SECTION("Different history_depth") {
    profile2.history_depth = 20;
    REQUIRE(profile1 != profile2);
    REQUIRE_FALSE(profile1 == profile2);
  }

  SECTION("Different deadline_ms") {
    profile2.deadline_ms = 100;
    REQUIRE(profile1 != profile2);
    REQUIRE_FALSE(profile1 == profile2);
  }

  SECTION("Different lifespan_ms") {
    profile2.lifespan_ms = 1000;
    REQUIRE(profile1 != profile2);
    REQUIRE_FALSE(profile1 == profile2);
  }

  SECTION("Predefined profiles comparison") {
    REQUIRE(QosSensorData != QosControlCommand);
    REQUIRE(QosSensorData != QosSystemDefault);
    REQUIRE(QosSystemDefault == QosSystemDefault);
  }
}

TEST_CASE("IsCompatible - reliability combinations", "[qos]") {
  QosProfile pub_qos;
  QosProfile sub_qos;

  SECTION("Reliable publisher with Reliable subscriber") {
    pub_qos.reliability = ReliabilityPolicy::kReliable;
    sub_qos.reliability = ReliabilityPolicy::kReliable;
    REQUIRE(IsCompatible(pub_qos, sub_qos));
  }

  SECTION("Reliable publisher with BestEffort subscriber") {
    pub_qos.reliability = ReliabilityPolicy::kReliable;
    sub_qos.reliability = ReliabilityPolicy::kBestEffort;
    REQUIRE(IsCompatible(pub_qos, sub_qos));
  }

  SECTION("BestEffort publisher with BestEffort subscriber") {
    pub_qos.reliability = ReliabilityPolicy::kBestEffort;
    sub_qos.reliability = ReliabilityPolicy::kBestEffort;
    REQUIRE(IsCompatible(pub_qos, sub_qos));
  }

  SECTION("BestEffort publisher with Reliable subscriber") {
    pub_qos.reliability = ReliabilityPolicy::kBestEffort;
    sub_qos.reliability = ReliabilityPolicy::kReliable;
    REQUIRE_FALSE(IsCompatible(pub_qos, sub_qos));
  }
}

TEST_CASE("IsCompatible - durability combinations", "[qos]") {
  QosProfile pub_qos;
  QosProfile sub_qos;

  SECTION("TransientLocal publisher with TransientLocal subscriber") {
    pub_qos.durability = DurabilityPolicy::kTransientLocal;
    sub_qos.durability = DurabilityPolicy::kTransientLocal;
    REQUIRE(IsCompatible(pub_qos, sub_qos));
  }

  SECTION("TransientLocal publisher with Volatile subscriber") {
    pub_qos.durability = DurabilityPolicy::kTransientLocal;
    sub_qos.durability = DurabilityPolicy::kVolatile;
    REQUIRE(IsCompatible(pub_qos, sub_qos));
  }

  SECTION("Volatile publisher with Volatile subscriber") {
    pub_qos.durability = DurabilityPolicy::kVolatile;
    sub_qos.durability = DurabilityPolicy::kVolatile;
    REQUIRE(IsCompatible(pub_qos, sub_qos));
  }

  SECTION("Volatile publisher with TransientLocal subscriber") {
    pub_qos.durability = DurabilityPolicy::kVolatile;
    sub_qos.durability = DurabilityPolicy::kTransientLocal;
    REQUIRE_FALSE(IsCompatible(pub_qos, sub_qos));
  }
}

TEST_CASE("IsCompatible - combined reliability and durability", "[qos]") {
  QosProfile pub_qos;
  QosProfile sub_qos;

  SECTION("Compatible reliability but incompatible durability") {
    pub_qos.reliability = ReliabilityPolicy::kReliable;
    pub_qos.durability = DurabilityPolicy::kVolatile;
    sub_qos.reliability = ReliabilityPolicy::kReliable;
    sub_qos.durability = DurabilityPolicy::kTransientLocal;
    REQUIRE_FALSE(IsCompatible(pub_qos, sub_qos));
  }

  SECTION("Incompatible reliability but compatible durability") {
    pub_qos.reliability = ReliabilityPolicy::kBestEffort;
    pub_qos.durability = DurabilityPolicy::kTransientLocal;
    sub_qos.reliability = ReliabilityPolicy::kReliable;
    sub_qos.durability = DurabilityPolicy::kVolatile;
    REQUIRE_FALSE(IsCompatible(pub_qos, sub_qos));
  }

  SECTION("Both compatible") {
    pub_qos.reliability = ReliabilityPolicy::kReliable;
    pub_qos.durability = DurabilityPolicy::kTransientLocal;
    sub_qos.reliability = ReliabilityPolicy::kBestEffort;
    sub_qos.durability = DurabilityPolicy::kVolatile;
    REQUIRE(IsCompatible(pub_qos, sub_qos));
  }
}

TEST_CASE("IsDeadlineExpired", "[qos]") {
  QosProfile profile;

  SECTION("No deadline (deadline_ms = 0) never expires") {
    profile.deadline_ms = 0;
    REQUIRE_FALSE(IsDeadlineExpired(profile, 0));
    REQUIRE_FALSE(IsDeadlineExpired(profile, 100));
    REQUIRE_FALSE(IsDeadlineExpired(profile, 1000));
    REQUIRE_FALSE(IsDeadlineExpired(profile, UINT32_MAX));
  }

  SECTION("Deadline not exceeded") {
    profile.deadline_ms = 100;
    REQUIRE_FALSE(IsDeadlineExpired(profile, 0));
    REQUIRE_FALSE(IsDeadlineExpired(profile, 50));
    REQUIRE_FALSE(IsDeadlineExpired(profile, 100));
  }

  SECTION("Deadline exceeded") {
    profile.deadline_ms = 100;
    REQUIRE(IsDeadlineExpired(profile, 101));
    REQUIRE(IsDeadlineExpired(profile, 200));
    REQUIRE(IsDeadlineExpired(profile, 1000));
  }
}

TEST_CASE("IsLifespanExpired", "[qos]") {
  QosProfile profile;

  SECTION("No lifespan (lifespan_ms = 0) never expires") {
    profile.lifespan_ms = 0;
    REQUIRE_FALSE(IsLifespanExpired(profile, 0));
    REQUIRE_FALSE(IsLifespanExpired(profile, 100));
    REQUIRE_FALSE(IsLifespanExpired(profile, 1000));
    REQUIRE_FALSE(IsLifespanExpired(profile, UINT32_MAX));
  }

  SECTION("Lifespan not exceeded") {
    profile.lifespan_ms = 1000;
    REQUIRE_FALSE(IsLifespanExpired(profile, 0));
    REQUIRE_FALSE(IsLifespanExpired(profile, 500));
    REQUIRE_FALSE(IsLifespanExpired(profile, 1000));
  }

  SECTION("Lifespan exceeded") {
    profile.lifespan_ms = 1000;
    REQUIRE(IsLifespanExpired(profile, 1001));
    REQUIRE(IsLifespanExpired(profile, 2000));
    REQUIRE(IsLifespanExpired(profile, 10000));
  }
}

TEST_CASE("Custom QosProfile construction", "[qos]") {
  SECTION("Aggregate initialization") {
    QosProfile custom{
      ReliabilityPolicy::kReliable,
      HistoryPolicy::kKeepAll,
      DurabilityPolicy::kTransientLocal,
      100,
      500,
      2000
    };

    REQUIRE(custom.reliability == ReliabilityPolicy::kReliable);
    REQUIRE(custom.history == HistoryPolicy::kKeepAll);
    REQUIRE(custom.durability == DurabilityPolicy::kTransientLocal);
    REQUIRE(custom.history_depth == 100);
    REQUIRE(custom.deadline_ms == 500);
    REQUIRE(custom.lifespan_ms == 2000);
  }

  SECTION("Partial initialization with defaults") {
    QosProfile custom{
      ReliabilityPolicy::kReliable,
      HistoryPolicy::kKeepAll
    };

    REQUIRE(custom.reliability == ReliabilityPolicy::kReliable);
    REQUIRE(custom.history == HistoryPolicy::kKeepAll);
    REQUIRE(custom.durability == DurabilityPolicy::kVolatile);
    REQUIRE(custom.history_depth == 10);
    REQUIRE(custom.deadline_ms == 0);
    REQUIRE(custom.lifespan_ms == 0);
  }

  SECTION("Copy construction") {
    QosProfile original = QosReliableSensor;
    QosProfile copy = original;

    REQUIRE(copy == original);
    REQUIRE(copy.reliability == ReliabilityPolicy::kReliable);
    REQUIRE(copy.lifespan_ms == 1000);
  }
}
