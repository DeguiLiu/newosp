/**
 * @file test_integration.cpp
 * @brief Cross-module integration tests for newosp library.
 *
 * Exercises interactions between Node, Bus, WorkerPool, Timer,
 * HSM, BehaviorTree, ConnectionPool, DataFusion, Executor, and Semaphore.
 */

#include "osp/bus.hpp"
#include "osp/bt.hpp"
#include "osp/connection.hpp"
#include "osp/data_fusion.hpp"
#include "osp/executor.hpp"
#include "osp/hsm.hpp"
#include "osp/node.hpp"
#include "osp/semaphore.hpp"
#include "osp/timer.hpp"
#include "osp/worker_pool.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <variant>

// ============================================================================
// Integration message types
// ============================================================================

struct IntegCmd {
  uint32_t id;
  uint32_t action;
};

struct IntegData {
  uint32_t id;
  float value;
};

struct IntegEvent {
  uint32_t code;
};

using IntegPayload = std::variant<IntegCmd, IntegData, IntegEvent>;
using IntegBus = osp::AsyncBus<IntegPayload>;

// ============================================================================
// Test 1: Node + WorkerPool + Bus pipeline
// ============================================================================

namespace {

std::atomic<uint32_t> g_wp_processed{0};
osp::LightSemaphore g_wp_sem{0};

void HandleIntegCmd(const IntegCmd& cmd, const osp::MessageHeader&) {
  (void)cmd;
  g_wp_processed.fetch_add(1, std::memory_order_relaxed);
  g_wp_sem.Signal();
}

}  // namespace

TEST_CASE("integration - Node + WorkerPool + Bus pipeline", "[integration]") {
  IntegBus::Instance().Reset();
  g_wp_processed.store(0, std::memory_order_relaxed);

  osp::WorkerPoolConfig cfg;
  cfg.name = osp::FixedString<32>("integ_pool");
  cfg.worker_num = 2;
  osp::WorkerPool<IntegPayload> pool(cfg);
  pool.RegisterHandler<IntegCmd>(&HandleIntegCmd);
  pool.Start();

  osp::Node<IntegPayload> publisher("pub_node", 1);
  constexpr uint32_t kMsgCount = 10;
  for (uint32_t i = 0; i < kMsgCount; ++i) {
    REQUIRE(publisher.Publish(IntegCmd{i, 100}));
  }

  // Wait for all messages with timeout
  for (uint32_t i = 0; i < kMsgCount; ++i) {
    bool ok = g_wp_sem.WaitFor(2000000);  // 2s per message
    if (!ok) break;
  }

  pool.Shutdown();
  REQUIRE(g_wp_processed.load(std::memory_order_relaxed) == kMsgCount);
}

// ============================================================================
// Test 2: Node + Timer + Bus periodic publish
// ============================================================================

namespace {

struct TimerPubCtx {
  std::atomic<uint32_t> pub_count{0};
};

TimerPubCtx g_timer_pub_ctx;

void TimerPublishCallback(void* ctx) {
  auto* tctx = static_cast<TimerPubCtx*>(ctx);
  uint32_t seq = tctx->pub_count.fetch_add(1, std::memory_order_relaxed);
  IntegBus::Instance().Publish(
      IntegPayload(IntegData{seq, 3.14f}), 99);
}

}  // namespace

TEST_CASE("integration - Node + Timer + Bus periodic publish", "[integration]") {
  IntegBus::Instance().Reset();
  g_timer_pub_ctx.pub_count.store(0, std::memory_order_relaxed);

  std::atomic<uint32_t> recv_count{0};
  osp::Node<IntegPayload> subscriber("sub_node", 2);
  subscriber.Subscribe<IntegData>(
      [&recv_count](const IntegData&, const osp::MessageHeader&) {
        recv_count.fetch_add(1, std::memory_order_relaxed);
      });

  osp::TimerScheduler<4> sched;
  auto result = sched.Add(20, &TimerPublishCallback, &g_timer_pub_ctx);
  REQUIRE(result.has_value());
  sched.Start();

  // Let timer fire for ~200ms, processing bus in a loop
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  while (std::chrono::steady_clock::now() < deadline) {
    subscriber.SpinOnce();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  sched.Stop();
  // Drain remaining
  for (int i = 0; i < 10; ++i) {
    subscriber.SpinOnce();
  }

  // With 20ms period over 250ms, expect at least 5 messages
  REQUIRE(recv_count.load(std::memory_order_relaxed) >= 5);
}

// ============================================================================
// Test 3: HSM + Bus event-driven state transitions
// ============================================================================

namespace {

// HSM event IDs
constexpr uint32_t kEvtGoB = 1;
constexpr uint32_t kEvtGoC = 2;
constexpr uint32_t kEvtGoA = 3;

struct HsmIntegCtx {
  osp::StateMachine<HsmIntegCtx, 8>* sm;
  int32_t state_a;
  int32_t state_b;
  int32_t state_c;
  std::atomic<uint32_t> transitions{0};
};

osp::TransitionResult StateAHandler(HsmIntegCtx& ctx, const osp::Event& evt) {
  if (evt.id == kEvtGoB) {
    ctx.transitions.fetch_add(1, std::memory_order_relaxed);
    return ctx.sm->RequestTransition(ctx.state_b);
  }
  return osp::TransitionResult::kHandled;
}

osp::TransitionResult StateBHandler(HsmIntegCtx& ctx, const osp::Event& evt) {
  if (evt.id == kEvtGoC) {
    ctx.transitions.fetch_add(1, std::memory_order_relaxed);
    return ctx.sm->RequestTransition(ctx.state_c);
  }
  return osp::TransitionResult::kHandled;
}

osp::TransitionResult StateCHandler(HsmIntegCtx& ctx, const osp::Event& evt) {
  if (evt.id == kEvtGoA) {
    ctx.transitions.fetch_add(1, std::memory_order_relaxed);
    return ctx.sm->RequestTransition(ctx.state_a);
  }
  return osp::TransitionResult::kHandled;
}

}  // namespace

TEST_CASE("integration - HSM + Bus event-driven state transitions", "[integration]") {
  IntegBus::Instance().Reset();

  HsmIntegCtx ctx{};
  osp::StateMachine<HsmIntegCtx, 8> sm(ctx);
  ctx.sm = &sm;

  ctx.state_a = sm.AddState({"StateA", -1, &StateAHandler, nullptr, nullptr, nullptr});
  ctx.state_b = sm.AddState({"StateB", -1, &StateBHandler, nullptr, nullptr, nullptr});
  ctx.state_c = sm.AddState({"StateC", -1, &StateCHandler, nullptr, nullptr, nullptr});
  sm.SetInitialState(ctx.state_a);
  sm.Start();

  REQUIRE(sm.CurrentState() == ctx.state_a);

  // Subscribe to IntegEvent on bus, dispatch to HSM
  osp::Node<IntegPayload> driver("hsm_driver", 10);
  driver.Subscribe<IntegEvent>(
      [&sm](const IntegEvent& evt, const osp::MessageHeader&) {
        osp::Event hsm_evt{evt.code, nullptr};
        sm.Dispatch(hsm_evt);
      });

  // Publish events via bus
  driver.Publish(IntegEvent{kEvtGoB});
  driver.SpinOnce();
  REQUIRE(sm.CurrentState() == ctx.state_b);

  driver.Publish(IntegEvent{kEvtGoC});
  driver.SpinOnce();
  REQUIRE(sm.CurrentState() == ctx.state_c);

  driver.Publish(IntegEvent{kEvtGoA});
  driver.SpinOnce();
  REQUIRE(sm.CurrentState() == ctx.state_a);

  REQUIRE(ctx.transitions.load(std::memory_order_relaxed) == 3);
}

// ============================================================================
// Test 4: BehaviorTree + Node action integration
// ============================================================================

namespace {

struct BtIntegCtx {
  osp::Node<IntegPayload>* node;
  uint32_t action_seq;
};

osp::NodeStatus BtPublishAction1(BtIntegCtx& ctx) {
  ctx.node->Publish(IntegCmd{ctx.action_seq++, 1});
  return osp::NodeStatus::kSuccess;
}

osp::NodeStatus BtPublishAction2(BtIntegCtx& ctx) {
  ctx.node->Publish(IntegCmd{ctx.action_seq++, 2});
  return osp::NodeStatus::kSuccess;
}

}  // namespace

TEST_CASE("integration - BehaviorTree + Node action", "[integration]") {
  IntegBus::Instance().Reset();

  osp::Node<IntegPayload> bt_node("bt_node", 20);
  BtIntegCtx bt_ctx{&bt_node, 0};

  osp::BehaviorTree<BtIntegCtx> tree(bt_ctx, "integ_bt");
  auto root = tree.AddSequence("root");
  tree.AddAction("publish_cmd1", &BtPublishAction1, root);
  tree.AddAction("publish_cmd2", &BtPublishAction2, root);
  tree.SetRoot(root);

  std::atomic<uint32_t> cmd_count{0};
  uint32_t last_action = 0;
  osp::Node<IntegPayload> observer("observer", 21);
  observer.Subscribe<IntegCmd>(
      [&cmd_count, &last_action](const IntegCmd& cmd, const osp::MessageHeader&) {
        cmd_count.fetch_add(1, std::memory_order_relaxed);
        last_action = cmd.action;
      });

  // Tick the tree
  auto status = tree.Tick();
  REQUIRE(status == osp::NodeStatus::kSuccess);
  REQUIRE(bt_ctx.action_seq == 2);

  // Process bus messages
  observer.SpinOnce();

  REQUIRE(cmd_count.load(std::memory_order_relaxed) == 2);
  REQUIRE(last_action == 2);
}

// ============================================================================
// Test 5: ConnectionPool + Timer timeout cleanup
// ============================================================================

TEST_CASE("integration - ConnectionPool + Timer timeout cleanup", "[integration]") {
  osp::ConnectionPool<16> pool;

  // Add 3 connections
  auto c1 = pool.Add(0x0A000001, 8080, "conn1");
  auto c2 = pool.Add(0x0A000002, 8081, "conn2");
  auto c3 = pool.Add(0x0A000003, 8082, "conn3");
  REQUIRE(c1.has_value());
  REQUIRE(c2.has_value());
  REQUIRE(c3.has_value());
  REQUIRE(pool.Count() == 3);

  struct CleanupCtx {
    osp::ConnectionPool<16>* pool;
    osp::ConnectionId keep_alive;
    std::atomic<uint32_t> cleaned{0};
    std::atomic<uint32_t> timer_fires{0};
  };
  CleanupCtx cleanup_ctx;
  cleanup_ctx.pool = &pool;
  cleanup_ctx.keep_alive = c1.value();

  osp::TimerScheduler<4> sched;
  // Timer fires every 30ms, touches c1 to keep it alive, then cleans up
  // connections older than 80ms
  auto task = sched.Add(30, [](void* ctx) {
    auto* cc = static_cast<CleanupCtx*>(ctx);
    // Keep c1 alive by touching it each tick
    cc->pool->Touch(cc->keep_alive);
    uint32_t removed = cc->pool->RemoveTimedOut(80000);  // 80ms in us
    cc->cleaned.fetch_add(removed, std::memory_order_relaxed);
    cc->timer_fires.fetch_add(1, std::memory_order_relaxed);
  }, &cleanup_ctx);
  REQUIRE(task.has_value());

  sched.Start();
  // Wait long enough for c2/c3 to age past 80ms and timer to clean them
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  sched.Stop();

  // c2 and c3 should have been cleaned up
  REQUIRE(cleanup_ctx.cleaned.load(std::memory_order_relaxed) >= 2);
  REQUIRE(pool.Count() <= 1);
  // c1 should still be alive (touched by timer)
  REQUIRE(pool.Find(c1.value()) != nullptr);
}

// ============================================================================
// Test 6: FusedSubscription + Node multi-source fusion
// ============================================================================

namespace {

std::atomic<uint32_t> g_fusion_fires{0};
IntegCmd g_fused_cmd{};
IntegData g_fused_data{};

void FusionCallback(const std::tuple<IntegCmd, IntegData>& fused) {
  g_fused_cmd = std::get<0>(fused);
  g_fused_data = std::get<1>(fused);
  g_fusion_fires.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

TEST_CASE("integration - FusedSubscription + Node multi-source fusion", "[integration]") {
  IntegBus::Instance().Reset();
  g_fusion_fires.store(0, std::memory_order_relaxed);

  osp::FusedSubscription<IntegPayload, IntegCmd, IntegData> fusion;
  fusion.SetCallback(&FusionCallback);
  bool activated = fusion.Activate(IntegBus::Instance());
  REQUIRE(activated);

  osp::Node<IntegPayload> node_a("source_a", 30);
  osp::Node<IntegPayload> node_b("source_b", 31);

  // Publish IntegCmd from node_a
  node_a.Publish(IntegCmd{42, 7});
  IntegBus::Instance().ProcessBatch();

  // Fusion should not fire yet (only one type received)
  REQUIRE(g_fusion_fires.load(std::memory_order_relaxed) == 0);

  // Publish IntegData from node_b
  node_b.Publish(IntegData{42, 2.718f});
  IntegBus::Instance().ProcessBatch();

  // Now both types received, fusion should fire
  REQUIRE(g_fusion_fires.load(std::memory_order_relaxed) == 1);
  REQUIRE(g_fused_cmd.id == 42);
  REQUIRE(g_fused_cmd.action == 7);
  REQUIRE(g_fused_data.value > 2.7f);

  // Second round: should fire again after both arrive
  node_a.Publish(IntegCmd{99, 1});
  IntegBus::Instance().ProcessBatch();
  REQUIRE(g_fusion_fires.load(std::memory_order_relaxed) == 1);

  node_b.Publish(IntegData{99, 1.0f});
  IntegBus::Instance().ProcessBatch();
  REQUIRE(g_fusion_fires.load(std::memory_order_relaxed) == 2);

  fusion.Deactivate(IntegBus::Instance());
}

// ============================================================================
// Test 7: Executor + Node scheduling
// ============================================================================

TEST_CASE("integration - StaticExecutor + Node scheduling", "[integration]") {
  IntegBus::Instance().Reset();

  std::atomic<uint32_t> node1_recv{0};
  std::atomic<uint32_t> node2_recv{0};

  osp::Node<IntegPayload> node1("exec_node1", 40);
  osp::Node<IntegPayload> node2("exec_node2", 41);

  node1.Subscribe<IntegCmd>(
      [&node1_recv](const IntegCmd&, const osp::MessageHeader&) {
        node1_recv.fetch_add(1, std::memory_order_relaxed);
      });
  node2.Subscribe<IntegCmd>(
      [&node2_recv](const IntegCmd&, const osp::MessageHeader&) {
        node2_recv.fetch_add(1, std::memory_order_relaxed);
      });

  osp::StaticExecutor<IntegPayload> exec;
  exec.AddNode(node1);
  exec.AddNode(node2);
  exec.Start();

  // Publish messages from an external node
  osp::Node<IntegPayload> sender("sender", 42);
  constexpr uint32_t kCount = 5;
  for (uint32_t i = 0; i < kCount; ++i) {
    sender.Publish(IntegCmd{i, 0});
  }

  // Wait for processing
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline) {
    uint32_t total = node1_recv.load(std::memory_order_relaxed) +
                     node2_recv.load(std::memory_order_relaxed);
    if (total >= kCount * 2) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  exec.Stop();

  // Both nodes subscribe to IntegCmd, so each message dispatches to both
  REQUIRE(node1_recv.load(std::memory_order_relaxed) == kCount);
  REQUIRE(node2_recv.load(std::memory_order_relaxed) == kCount);
}
