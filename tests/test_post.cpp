/**
 * @file test_post.cpp
 * @brief Tests for AppRegistry, OspPost, OspSendAndWait.
 */

#include "osp/post.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

// ============================================================================
// Test Instance for post tests
// ============================================================================

struct PostTestInstance : public osp::Instance {
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

static osp::Instance* PostTestFactory() { return new PostTestInstance(); }

// ============================================================================
// AppRegistry basics
// ============================================================================

TEST_CASE("post - AppRegistry register and unregister", "[post]") {
  auto& reg = osp::AppRegistry::Instance();
  reg.Reset();
  REQUIRE(reg.Count() == 0);

  osp::Application<8> app(1, "reg_app");
  REQUIRE(osp::RegisterApp(app));
  REQUIRE(reg.Count() == 1);

  // Duplicate registration fails
  REQUIRE(!osp::RegisterApp(app));
  REQUIRE(reg.Count() == 1);

  REQUIRE(osp::UnregisterApp(app));
  REQUIRE(reg.Count() == 0);

  // Double unregister fails
  REQUIRE(!osp::UnregisterApp(app));
}

TEST_CASE("post - AppRegistry null registration", "[post]") {
  auto& reg = osp::AppRegistry::Instance();
  reg.Reset();

  REQUIRE(!reg.Register(1, nullptr, [](void*, uint16_t, uint16_t,
                                       const void*, uint32_t) -> bool {
    return true;
  }));
  REQUIRE(!reg.Register(1, reinterpret_cast<void*>(1), nullptr));
  REQUIRE(reg.Count() == 0);
}

// ============================================================================
// OspPost local delivery
// ============================================================================

TEST_CASE("post - OspPost local delivery", "[post]") {
  auto& reg = osp::AppRegistry::Instance();
  reg.Reset();

  osp::Application<8> app(10, "post_app");
  app.SetFactory(PostTestFactory);
  osp::RegisterApp(app);

  auto r = app.CreateInstance();
  REQUIRE(r.has_value());

  uint32_t dst = osp::MakeIID(10, r.value());
  uint32_t payload = 0xDEAD;
  REQUIRE(osp::OspPost(dst, 42, &payload, sizeof(payload)));

  REQUIRE(app.ProcessOne());
  auto* inst = static_cast<PostTestInstance*>(app.GetInstance(r.value()));
  REQUIRE(inst != nullptr);
  REQUIRE(inst->last_event == 42);
  REQUIRE(inst->last_len == sizeof(payload));

  osp::UnregisterApp(app);
}

TEST_CASE("post - OspPost to unknown app fails", "[post]") {
  auto& reg = osp::AppRegistry::Instance();
  reg.Reset();

  uint32_t dst = osp::MakeIID(99, 1);
  REQUIRE(!osp::OspPost(dst, 1, nullptr, 0));
}

TEST_CASE("post - OspPost broadcast via kInsEach", "[post]") {
  auto& reg = osp::AppRegistry::Instance();
  reg.Reset();

  osp::Application<8> app(20, "bcast_app");
  app.SetFactory(PostTestFactory);
  osp::RegisterApp(app);

  auto r1 = app.CreateInstance();
  auto r2 = app.CreateInstance();
  REQUIRE(r1.has_value());
  REQUIRE(r2.has_value());

  uint32_t dst = osp::MakeIID(20, osp::kInsEach);
  REQUIRE(osp::OspPost(dst, 300, nullptr, 0));
  app.ProcessAll();

  for (uint16_t id : {r1.value(), r2.value()}) {
    auto* inst = static_cast<PostTestInstance*>(app.GetInstance(id));
    REQUIRE(inst != nullptr);
    REQUIRE(inst->last_event == 300);
  }

  osp::UnregisterApp(app);
}

// ============================================================================
// Remote delivery stub
// ============================================================================

TEST_CASE("post - OspPost remote returns false", "[post]") {
  REQUIRE(!osp::OspPost(osp::MakeIID(1, 1), 1, nullptr, 0, /*dst_node=*/5));
}

// ============================================================================
// OspSendAndWait
// ============================================================================

TEST_CASE("post - OspSendAndWait local success", "[post]") {
  auto& reg = osp::AppRegistry::Instance();
  reg.Reset();

  osp::Application<8> app(30, "saw_app");
  app.SetFactory(PostTestFactory);
  osp::RegisterApp(app);

  auto r = app.CreateInstance();
  REQUIRE(r.has_value());

  uint32_t dst = osp::MakeIID(30, r.value());
  auto result = osp::OspSendAndWait(dst, 50, nullptr, 0, nullptr, 0);
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  osp::UnregisterApp(app);
}

TEST_CASE("post - OspSendAndWait remote fails", "[post]") {
  auto result = osp::OspSendAndWait(
      osp::MakeIID(1, 1), 1, nullptr, 0, nullptr, 0, /*dst_node=*/3);
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::PostError::kSendFailed);
}

TEST_CASE("post - OspSendAndWait unknown app fails", "[post]") {
  auto& reg = osp::AppRegistry::Instance();
  reg.Reset();

  auto result = osp::OspSendAndWait(
      osp::MakeIID(99, 1), 1, nullptr, 0, nullptr, 0);
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::PostError::kAppNotFound);
}

// ============================================================================
// Multiple apps in registry
// ============================================================================

TEST_CASE("post - Multiple apps routing", "[post]") {
  auto& reg = osp::AppRegistry::Instance();
  reg.Reset();

  osp::Application<4> app1(40, "multi1");
  osp::Application<4> app2(41, "multi2");
  app1.SetFactory(PostTestFactory);
  app2.SetFactory(PostTestFactory);

  osp::RegisterApp(app1);
  osp::RegisterApp(app2);
  REQUIRE(reg.Count() == 2);

  auto r1 = app1.CreateInstance();
  auto r2 = app2.CreateInstance();
  REQUIRE(r1.has_value());
  REQUIRE(r2.has_value());

  // Post to app2 only
  uint32_t dst = osp::MakeIID(41, r2.value());
  REQUIRE(osp::OspPost(dst, 500, nullptr, 0));
  app2.ProcessAll();

  auto* inst1 = static_cast<PostTestInstance*>(app1.GetInstance(r1.value()));
  auto* inst2 = static_cast<PostTestInstance*>(app2.GetInstance(r2.value()));
  REQUIRE(inst1->msg_count == 0);
  REQUIRE(inst2->last_event == 500);

  osp::UnregisterApp(app1);
  osp::UnregisterApp(app2);
}
