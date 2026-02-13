/**
 * @file test_app.cpp
 * @brief Tests for osp::Application, Instance, MakeIID.
 */

#include "osp/app.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

// ============================================================================
// Test Instance implementation
// ============================================================================

struct TestInstance : public osp::Instance {
  static int create_count;
  int last_event = -1;
  uint32_t last_len = 0;
  int msg_count = 0;

  void OnMessage(uint16_t event, const void* data, uint32_t len) override {
    last_event = event;
    last_len = len;
    ++msg_count;
    (void)data;
  }
};

int TestInstance::create_count = 0;

static osp::Instance* TestInstanceFactory() {
  ++TestInstance::create_count;
  return new TestInstance();
}

// ============================================================================
// IID encoding/decoding
// ============================================================================

TEST_CASE("app - MakeIID/GetAppId/GetInsId roundtrip", "[app]") {
  uint32_t iid = osp::MakeIID(10, 42);
  REQUIRE(osp::GetAppId(iid) == 10);
  REQUIRE(osp::GetInsId(iid) == 42);

  iid = osp::MakeIID(0xFFFF, 0xFFFF);
  REQUIRE(osp::GetAppId(iid) == 0xFFFF);
  REQUIRE(osp::GetInsId(iid) == 0xFFFF);

  iid = osp::MakeIID(0, 0);
  REQUIRE(osp::GetAppId(iid) == 0);
  REQUIRE(osp::GetInsId(iid) == 0);
}

TEST_CASE("app - Special instance IDs", "[app]") {
  REQUIRE(osp::kInsPending == 0);
  REQUIRE(osp::kInsDaemon == 0xFFFC);
  REQUIRE(osp::kInsEach == 0xFFFF);
}

// ============================================================================
// Application construction
// ============================================================================

TEST_CASE("app - Application construction", "[app]") {
  osp::Application<16> app(5, "test_app");
  REQUIRE(app.AppId() == 5);
  REQUIRE(std::strcmp(app.Name(), "test_app") == 0);
  REQUIRE(app.InstanceCount() == 0);
  REQUIRE(app.PendingMessages() == 0);
}

// ============================================================================
// Instance creation
// ============================================================================

TEST_CASE("app - CreateInstance with factory", "[app]") {
  TestInstance::create_count = 0;
  osp::Application<8> app(1, "app1");
  app.SetFactory(TestInstanceFactory);

  auto r = app.CreateInstance();
  REQUIRE(r.has_value());
  REQUIRE(r.value() >= 1);
  REQUIRE(app.InstanceCount() == 1);
  REQUIRE(TestInstance::create_count == 1);

  osp::Instance* inst = app.GetInstance(r.value());
  REQUIRE(inst != nullptr);
  REQUIRE(inst->InsId() == r.value());
}

TEST_CASE("app - CreateInstance without factory fails", "[app]") {
  osp::Application<8> app(2, "app2");
  auto r = app.CreateInstance();
  REQUIRE(!r.has_value());
  REQUIRE(r.get_error() == osp::AppError::kFactoryNotSet);
}

TEST_CASE("app - Instance pool full", "[app]") {
  osp::Application<2> app(3, "small");
  app.SetFactory(TestInstanceFactory);

  auto r1 = app.CreateInstance();
  REQUIRE(r1.has_value());
  auto r2 = app.CreateInstance();
  REQUIRE(r2.has_value());
  auto r3 = app.CreateInstance();
  REQUIRE(!r3.has_value());
  REQUIRE(r3.get_error() == osp::AppError::kInstancePoolFull);
}

// ============================================================================
// Instance destruction
// ============================================================================

TEST_CASE("app - DestroyInstance", "[app]") {
  osp::Application<8> app(4, "app4");
  app.SetFactory(TestInstanceFactory);

  auto r = app.CreateInstance();
  REQUIRE(r.has_value());
  REQUIRE(app.InstanceCount() == 1);

  auto dr = app.DestroyInstance(r.value());
  REQUIRE(dr.has_value());
  REQUIRE(app.InstanceCount() == 0);
  REQUIRE(app.GetInstance(r.value()) == nullptr);
}

TEST_CASE("app - DestroyInstance invalid id", "[app]") {
  osp::Application<8> app(5, "app5");
  auto r = app.DestroyInstance(0);
  REQUIRE(!r.has_value());
  REQUIRE(r.get_error() == osp::AppError::kInvalidId);

  auto r2 = app.DestroyInstance(99);
  REQUIRE(!r2.has_value());
}

// ============================================================================
// Message posting and processing
// ============================================================================

TEST_CASE("app - Post and ProcessOne", "[app]") {
  osp::Application<8> app(6, "app6");
  app.SetFactory(TestInstanceFactory);

  auto r = app.CreateInstance();
  REQUIRE(r.has_value());

  uint32_t payload = 42;
  REQUIRE(app.Post(r.value(), 100, &payload, sizeof(payload)));
  REQUIRE(app.PendingMessages() == 1);

  REQUIRE(app.ProcessOne());
  REQUIRE(app.PendingMessages() == 0);

  auto* inst = static_cast<TestInstance*>(app.GetInstance(r.value()));
  REQUIRE(inst != nullptr);
  REQUIRE(inst->last_event == 100);
  REQUIRE(inst->last_len == sizeof(payload));
  REQUIRE(inst->msg_count == 1);
}

TEST_CASE("app - Broadcast to all instances", "[app]") {
  osp::Application<8> app(7, "app7");
  app.SetFactory(TestInstanceFactory);

  auto r1 = app.CreateInstance();
  auto r2 = app.CreateInstance();
  auto r3 = app.CreateInstance();
  REQUIRE(r1.has_value());
  REQUIRE(r2.has_value());
  REQUIRE(r3.has_value());

  REQUIRE(app.Post(osp::kInsEach, 200, nullptr, 0));
  REQUIRE(app.ProcessOne());

  for (uint16_t id : {r1.value(), r2.value(), r3.value()}) {
    auto* inst = static_cast<TestInstance*>(app.GetInstance(id));
    REQUIRE(inst != nullptr);
    REQUIRE(inst->last_event == 200);
    REQUIRE(inst->msg_count == 1);
  }
}

TEST_CASE("app - Instance state management", "[app]") {
  osp::Application<8> app(8, "app8");
  app.SetFactory(TestInstanceFactory);

  auto r = app.CreateInstance();
  REQUIRE(r.has_value());

  auto* inst = app.GetInstance(r.value());
  REQUIRE(inst != nullptr);
  REQUIRE(inst->CurState() == 0);

  inst->SetState(5);
  REQUIRE(inst->CurState() == 5);
}

TEST_CASE("app - ProcessAll drains queue", "[app]") {
  osp::Application<8> app(9, "app9");
  app.SetFactory(TestInstanceFactory);

  auto r = app.CreateInstance();
  REQUIRE(r.has_value());

  for (int i = 0; i < 5; ++i) {
    REQUIRE(app.Post(r.value(), static_cast<uint16_t>(i), nullptr, 0));
  }
  REQUIRE(app.PendingMessages() == 5);

  uint32_t processed = app.ProcessAll();
  REQUIRE(processed == 5);
  REQUIRE(app.PendingMessages() == 0);

  auto* inst = static_cast<TestInstance*>(app.GetInstance(r.value()));
  REQUIRE(inst->msg_count == 5);
}

TEST_CASE("app - GetInstance returns correct pointer", "[app]") {
  osp::Application<8> app(10, "app10");
  app.SetFactory(TestInstanceFactory);

  auto r1 = app.CreateInstance();
  auto r2 = app.CreateInstance();
  REQUIRE(r1.has_value());
  REQUIRE(r2.has_value());

  REQUIRE(app.GetInstance(r1.value()) != app.GetInstance(r2.value()));
  REQUIRE(app.GetInstance(0) == nullptr);
  REQUIRE(app.GetInstance(100) == nullptr);
}
