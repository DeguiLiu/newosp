/**
 * @file test_bt.cpp
 * @brief Tests for bt.hpp - lightweight behavior tree library.
 */

#include "osp/bt.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

// ============================================================================
// Test context
// ============================================================================

struct BtTestContext {
  int counter = 0;
  bool flag = false;
  std::vector<std::string> log;
};

// ============================================================================
// Reusable tick functions
// ============================================================================

static osp::NodeStatus AlwaysSucceed(BtTestContext& /*ctx*/) {
  return osp::NodeStatus::kSuccess;
}

static osp::NodeStatus AlwaysFail(BtTestContext& /*ctx*/) {
  return osp::NodeStatus::kFailure;
}

static osp::NodeStatus IncrementCounter(BtTestContext& ctx) {
  ++ctx.counter;
  return osp::NodeStatus::kSuccess;
}

static osp::NodeStatus FailOnSecond(BtTestContext& ctx) {
  ++ctx.counter;
  if (ctx.counter == 2) {
    return osp::NodeStatus::kFailure;
  }
  return osp::NodeStatus::kSuccess;
}

static osp::NodeStatus ReturnRunning(BtTestContext& /*ctx*/) {
  return osp::NodeStatus::kRunning;
}

static osp::NodeStatus RunningThenSucceed(BtTestContext& ctx) {
  ++ctx.counter;
  if (ctx.counter < 3) {
    return osp::NodeStatus::kRunning;
  }
  return osp::NodeStatus::kSuccess;
}

static osp::NodeStatus LogAndSucceed(BtTestContext& ctx) {
  ctx.log.push_back("action");
  return osp::NodeStatus::kSuccess;
}

// ============================================================================
// Test cases
// ============================================================================

TEST_CASE("bt - single action node succeeds", "[bt]") {
  BtTestContext ctx;
  osp::BehaviorTree<BtTestContext> tree(ctx, "test");

  auto root = tree.AddAction("succeed", AlwaysSucceed);
  REQUIRE(root == 0);
  tree.SetRoot(root);

  auto status = tree.Tick();
  REQUIRE(status == osp::NodeStatus::kSuccess);
  REQUIRE(tree.LastStatus() == osp::NodeStatus::kSuccess);
}

TEST_CASE("bt - single action node fails", "[bt]") {
  BtTestContext ctx;
  osp::BehaviorTree<BtTestContext> tree(ctx, "test");

  auto root = tree.AddAction("fail", AlwaysFail);
  tree.SetRoot(root);

  auto status = tree.Tick();
  REQUIRE(status == osp::NodeStatus::kFailure);
  REQUIRE(tree.LastStatus() == osp::NodeStatus::kFailure);
}

TEST_CASE("bt - sequence all succeed", "[bt]") {
  BtTestContext ctx;
  osp::BehaviorTree<BtTestContext> tree(ctx, "test");

  auto seq = tree.AddSequence("seq");
  tree.AddAction("a1", IncrementCounter, seq);
  tree.AddAction("a2", IncrementCounter, seq);
  tree.AddAction("a3", IncrementCounter, seq);
  tree.SetRoot(seq);

  auto status = tree.Tick();
  REQUIRE(status == osp::NodeStatus::kSuccess);
  REQUIRE(ctx.counter == 3);
}

TEST_CASE("bt - sequence second child fails", "[bt]") {
  BtTestContext ctx;
  osp::BehaviorTree<BtTestContext> tree(ctx, "test");

  // FailOnSecond: counter==1 -> success, counter==2 -> failure
  auto seq = tree.AddSequence("seq");
  tree.AddAction("a1", FailOnSecond, seq);
  tree.AddAction("a2", FailOnSecond, seq);
  tree.AddAction("a3", FailOnSecond, seq);
  tree.SetRoot(seq);

  auto status = tree.Tick();
  REQUIRE(status == osp::NodeStatus::kFailure);
  // a1 incremented to 1 (success), a2 incremented to 2 (failure), a3 not run
  REQUIRE(ctx.counter == 2);
}

TEST_CASE("bt - selector first child succeeds", "[bt]") {
  BtTestContext ctx;
  osp::BehaviorTree<BtTestContext> tree(ctx, "test");

  auto sel = tree.AddSelector("sel");
  tree.AddAction("a1", AlwaysSucceed, sel);
  tree.AddAction("a2", IncrementCounter, sel);
  tree.SetRoot(sel);

  auto status = tree.Tick();
  REQUIRE(status == osp::NodeStatus::kSuccess);
  // Second child should not have been ticked
  REQUIRE(ctx.counter == 0);
}

TEST_CASE("bt - selector fallback to second child", "[bt]") {
  BtTestContext ctx;
  osp::BehaviorTree<BtTestContext> tree(ctx, "test");

  auto sel = tree.AddSelector("sel");
  tree.AddAction("a1", AlwaysFail, sel);
  tree.AddAction("a2", IncrementCounter, sel);
  tree.SetRoot(sel);

  auto status = tree.Tick();
  REQUIRE(status == osp::NodeStatus::kSuccess);
  // First child failed, second child ran and succeeded
  REQUIRE(ctx.counter == 1);
}

TEST_CASE("bt - inverter inverts success to failure", "[bt]") {
  BtTestContext ctx;
  osp::BehaviorTree<BtTestContext> tree(ctx, "test");

  auto inv = tree.AddInverter("inv");
  tree.AddAction("succeed", AlwaysSucceed, inv);
  tree.SetRoot(inv);

  auto status = tree.Tick();
  REQUIRE(status == osp::NodeStatus::kFailure);
}

TEST_CASE("bt - inverter inverts failure to success", "[bt]") {
  BtTestContext ctx;
  osp::BehaviorTree<BtTestContext> tree(ctx, "test");

  auto inv = tree.AddInverter("inv");
  tree.AddAction("fail", AlwaysFail, inv);
  tree.SetRoot(inv);

  auto status = tree.Tick();
  REQUIRE(status == osp::NodeStatus::kSuccess);
}

TEST_CASE("bt - parallel threshold met", "[bt]") {
  BtTestContext ctx;
  osp::BehaviorTree<BtTestContext> tree(ctx, "test");

  // 3 children, threshold 2: need 2 successes
  auto par = tree.AddParallel("par", 2);
  tree.AddAction("a1", AlwaysSucceed, par);
  tree.AddAction("a2", AlwaysFail, par);
  tree.AddAction("a3", AlwaysSucceed, par);
  tree.SetRoot(par);

  auto status = tree.Tick();
  // 2 successes >= threshold(2), so kSuccess
  REQUIRE(status == osp::NodeStatus::kSuccess);
}

TEST_CASE("bt - parallel threshold not met", "[bt]") {
  BtTestContext ctx;
  osp::BehaviorTree<BtTestContext> tree(ctx, "test");

  // 3 children, threshold 3: need all 3 to succeed
  auto par = tree.AddParallel("par", 3);
  tree.AddAction("a1", AlwaysSucceed, par);
  tree.AddAction("a2", AlwaysFail, par);
  tree.AddAction("a3", AlwaysSucceed, par);
  tree.SetRoot(par);

  auto status = tree.Tick();
  // 2 successes < threshold(3), 1 failure > (3-3)=0 allowed, so kFailure
  REQUIRE(status == osp::NodeStatus::kFailure);
}

TEST_CASE("bt - repeat finite count", "[bt]") {
  BtTestContext ctx;
  osp::BehaviorTree<BtTestContext> tree(ctx, "test");

  // Repeat child 3 times
  auto rep = tree.AddRepeat("rep", 3);
  tree.AddAction("inc", IncrementCounter, rep);
  tree.SetRoot(rep);

  // First tick: child succeeds, counter=1, repeat_counter=1 -> kRunning
  auto s1 = tree.Tick();
  REQUIRE(s1 == osp::NodeStatus::kRunning);
  REQUIRE(ctx.counter == 1);

  // Second tick: child succeeds, counter=2, repeat_counter=2 -> kRunning
  auto s2 = tree.Tick();
  REQUIRE(s2 == osp::NodeStatus::kRunning);
  REQUIRE(ctx.counter == 2);

  // Third tick: child succeeds, counter=3, repeat_counter=3 -> kSuccess
  auto s3 = tree.Tick();
  REQUIRE(s3 == osp::NodeStatus::kSuccess);
  REQUIRE(ctx.counter == 3);
}

TEST_CASE("bt - running action across multiple ticks", "[bt]") {
  BtTestContext ctx;
  osp::BehaviorTree<BtTestContext> tree(ctx, "test");

  // RunningThenSucceed: returns kRunning until counter reaches 3
  auto root = tree.AddAction("async", RunningThenSucceed);
  tree.SetRoot(root);

  // Tick 1: counter=1, still running
  auto s1 = tree.Tick();
  REQUIRE(s1 == osp::NodeStatus::kRunning);
  REQUIRE(ctx.counter == 1);

  // Tick 2: counter=2, still running
  auto s2 = tree.Tick();
  REQUIRE(s2 == osp::NodeStatus::kRunning);
  REQUIRE(ctx.counter == 2);

  // Tick 3: counter=3, now succeeds
  auto s3 = tree.Tick();
  REQUIRE(s3 == osp::NodeStatus::kSuccess);
  REQUIRE(ctx.counter == 3);
}

// ============================================================================
// Additional coverage
// ============================================================================

TEST_CASE("bt - node count and tree name", "[bt]") {
  BtTestContext ctx;
  osp::BehaviorTree<BtTestContext> tree(ctx, "my_tree");

  REQUIRE(tree.NodeCount() == 0);
  REQUIRE(std::string(tree.Name()) == "my_tree");

  tree.AddAction("a1", AlwaysSucceed);
  tree.AddSequence("seq");
  REQUIRE(tree.NodeCount() == 2);
}

TEST_CASE("bt - inverter preserves running", "[bt]") {
  BtTestContext ctx;
  osp::BehaviorTree<BtTestContext> tree(ctx, "test");

  auto inv = tree.AddInverter("inv");
  tree.AddAction("running", ReturnRunning, inv);
  tree.SetRoot(inv);

  auto status = tree.Tick();
  REQUIRE(status == osp::NodeStatus::kRunning);
}

TEST_CASE("bt - selector all fail", "[bt]") {
  BtTestContext ctx;
  osp::BehaviorTree<BtTestContext> tree(ctx, "test");

  auto sel = tree.AddSelector("sel");
  tree.AddAction("f1", AlwaysFail, sel);
  tree.AddAction("f2", AlwaysFail, sel);
  tree.SetRoot(sel);

  auto status = tree.Tick();
  REQUIRE(status == osp::NodeStatus::kFailure);
}
