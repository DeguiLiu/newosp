/**
 * @file test_post.cpp
 * @brief Tests for AppRegistry, OspPost, OspSendAndWait.
 */

#include "osp/post.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <thread>

// ============================================================================
// Test Instance for post tests (fire-and-forget)
// ============================================================================

struct PostTestInstance : public osp::Instance {
  int last_event = -1;
  uint32_t last_len = 0;
  int msg_count = 0;

  void OnMessage(uint16_t event, const void* data, uint32_t len) {
    last_event = event;
    last_len = len;
    ++msg_count;
    (void)data;
  }
};

// ============================================================================
// Test Instance that replies (for OspSendAndWait)
// ============================================================================

struct ReplyTestInstance : public osp::Instance {
  int last_event = -1;

  void OnMessage(uint16_t event, const void* data, uint32_t len) {
    last_event = event;

    if (HasPendingReply()) {
      if (data != nullptr && len >= sizeof(uint32_t)) {
        uint32_t req_val;
        std::memcpy(&req_val, data, sizeof(req_val));
        uint32_t response = req_val + 1;
        Reply(&response, sizeof(response));
      } else {
        uint32_t response = static_cast<uint32_t>(event) * 10;
        Reply(&response, sizeof(response));
      }
    }
  }
};

// ============================================================================
// Test Instance that does NOT reply (for timeout testing)
// ============================================================================

struct NoReplyInstance : public osp::Instance {
  void OnMessage(uint16_t event, const void* data, uint32_t len) {
    (void)event;
    (void)data;
    (void)len;
  }
};

// ============================================================================
// AppRegistry basics
// ============================================================================

TEST_CASE("post - AppRegistry register and unregister", "[post]") {
  auto& reg = osp::AppRegistry::Instance();
  reg.Reset();
  REQUIRE(reg.Count() == 0);

  osp::Application<PostTestInstance, 8> app(1, "reg_app");
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

  REQUIRE(!reg.Register(1, nullptr,
      [](void*, uint16_t, uint16_t, const void*, uint32_t,
         osp::ResponseChannel*) -> bool { return true; }));
  REQUIRE(!reg.Register(1, reinterpret_cast<void*>(1), nullptr));
  REQUIRE(reg.Count() == 0);
}

// ============================================================================
// OspPost local delivery
// ============================================================================

TEST_CASE("post - OspPost local delivery", "[post]") {
  auto& reg = osp::AppRegistry::Instance();
  reg.Reset();

  osp::Application<PostTestInstance, 8> app(10, "post_app");
  osp::RegisterApp(app);

  auto r = app.CreateInstance();
  REQUIRE(r.has_value());

  uint32_t dst = osp::MakeIID(10, r.value());
  uint32_t payload = 0xDEAD;
  REQUIRE(osp::OspPost(dst, 42, &payload, sizeof(payload)));

  REQUIRE(app.ProcessOne());
  auto* inst = app.GetInstance(r.value());
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

  osp::Application<PostTestInstance, 8> app(20, "bcast_app");
  osp::RegisterApp(app);

  auto r1 = app.CreateInstance();
  auto r2 = app.CreateInstance();
  REQUIRE(r1.has_value());
  REQUIRE(r2.has_value());

  uint32_t dst = osp::MakeIID(20, osp::kInsEach);
  REQUIRE(osp::OspPost(dst, 300, nullptr, 0));
  app.ProcessAll();

  for (uint16_t id : {r1.value(), r2.value()}) {
    auto* inst = app.GetInstance(id);
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
// OspSendAndWait - synchronous request-response
// ============================================================================

TEST_CASE("post - OspSendAndWait with reply data", "[post]") {
  auto& reg = osp::AppRegistry::Instance();
  reg.Reset();

  osp::Application<ReplyTestInstance, 8> app(30, "saw_app");
  osp::RegisterApp(app);

  auto r = app.CreateInstance();
  REQUIRE(r.has_value());

  uint32_t dst = osp::MakeIID(30, r.value());
  uint32_t req_val = 42;
  uint32_t ack_val = 0;

  std::thread processor([&app]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    app.ProcessOne();
  });

  auto result = osp::OspSendAndWait(
      dst, 50, &req_val, sizeof(req_val),
      &ack_val, sizeof(ack_val), 0, 2000);

  processor.join();

  REQUIRE(result.has_value());
  REQUIRE(result.value() == sizeof(uint32_t));
  REQUIRE(ack_val == 43);

  osp::UnregisterApp(app);
}

TEST_CASE("post - OspSendAndWait no request data", "[post]") {
  auto& reg = osp::AppRegistry::Instance();
  reg.Reset();

  osp::Application<ReplyTestInstance, 8> app(31, "saw_nodata");
  osp::RegisterApp(app);

  auto r = app.CreateInstance();
  REQUIRE(r.has_value());

  uint32_t dst = osp::MakeIID(31, r.value());
  uint32_t ack_val = 0;

  std::thread processor([&app]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    app.ProcessOne();
  });

  auto result = osp::OspSendAndWait(
      dst, 7, nullptr, 0, &ack_val, sizeof(ack_val), 0, 2000);

  processor.join();

  REQUIRE(result.has_value());
  REQUIRE(result.value() == sizeof(uint32_t));
  REQUIRE(ack_val == 70);

  osp::UnregisterApp(app);
}

TEST_CASE("post - OspSendAndWait timeout when no reply", "[post]") {
  auto& reg = osp::AppRegistry::Instance();
  reg.Reset();

  osp::Application<NoReplyInstance, 8> app(32, "saw_timeout");
  osp::RegisterApp(app);

  auto r = app.CreateInstance();
  REQUIRE(r.has_value());

  uint32_t dst = osp::MakeIID(32, r.value());
  uint32_t ack_val = 0;

  std::thread processor([&app]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    app.ProcessOne();
  });

  auto result = osp::OspSendAndWait(
      dst, 1, nullptr, 0, &ack_val, sizeof(ack_val), 0,
      /*timeout_ms=*/200);

  processor.join();

  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::PostError::kTimeout);

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

TEST_CASE("post - OspSendAndWait small ack buffer truncates", "[post]") {
  auto& reg = osp::AppRegistry::Instance();
  reg.Reset();

  osp::Application<ReplyTestInstance, 8> app(33, "saw_trunc");
  osp::RegisterApp(app);

  auto r = app.CreateInstance();
  REQUIRE(r.has_value());

  uint32_t dst = osp::MakeIID(33, r.value());
  uint32_t req_val = 100;
  uint16_t small_buf = 0;

  std::thread processor([&app]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    app.ProcessOne();
  });

  auto result = osp::OspSendAndWait(
      dst, 50, &req_val, sizeof(req_val),
      &small_buf, sizeof(small_buf), 0, 2000);

  processor.join();

  REQUIRE(result.has_value());
  REQUIRE(result.value() == sizeof(uint16_t));

  osp::UnregisterApp(app);
}

TEST_CASE("post - Instance Reply without pending returns false", "[post]") {
  PostTestInstance inst;
  uint32_t data = 42;
  REQUIRE(!inst.Reply(&data, sizeof(data)));
  REQUIRE(!inst.HasPendingReply());
}

// ============================================================================
// Multiple apps in registry
// ============================================================================

TEST_CASE("post - Multiple apps routing", "[post]") {
  auto& reg = osp::AppRegistry::Instance();
  reg.Reset();

  osp::Application<PostTestInstance, 4> app1(40, "multi1");
  osp::Application<PostTestInstance, 4> app2(41, "multi2");

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

  auto* inst1 = app1.GetInstance(r1.value());
  auto* inst2 = app2.GetInstance(r2.value());
  REQUIRE(inst1->msg_count == 0);
  REQUIRE(inst2->last_event == 500);

  osp::UnregisterApp(app1);
  osp::UnregisterApp(app2);
}
