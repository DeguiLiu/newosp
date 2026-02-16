/**
 * @file test_hsm.cpp
 * @brief Tests for osp/hsm.hpp
 */

#include "osp/hsm.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

// ============================================================================
// Event IDs
// ============================================================================

enum : uint32_t {
  kEvNone = 0,
  kEvGo = 1,
  kEvBack = 2,
  kEvTick = 3,
  kEvReset = 4,
  kEvGuarded = 5,
  kEvSelf = 6,
  kEvDeep = 7,
  kEvUnknown = 99,
};

// ============================================================================
// Forward declaration and test context
// ============================================================================

struct TestContext;
using SM = osp::StateMachine<TestContext, 16>;

struct TestContext {
  std::vector<std::string> log;
  int counter = 0;
  SM* sm = nullptr;  // back-pointer so handlers can call RequestTransition
};

// ============================================================================
// State indices (set during test setup)
// ============================================================================

// Declared at file scope so handler functions can reference them.
// Each TEST_CASE populates these as needed.
static int32_t s_root = -1;
static int32_t s_a = -1;
static int32_t s_b = -1;
static int32_t s_a1 = -1;
static int32_t s_a2 = -1;
static int32_t s_b1 = -1;

// ============================================================================
// Helper: build a simple two-sibling state machine
//
//   root
//    +-- A
//    +-- B
// ============================================================================

static void BuildSimpleSM(SM& sm) {
  s_root = sm.AddState({
      "root", -1,
      [](TestContext& ctx, const osp::Event&) -> osp::TransitionResult {
        ctx.log.push_back("root:handler");
        return osp::TransitionResult::kHandled;
      },
      [](TestContext& ctx) { ctx.log.push_back("root:entry"); },
      [](TestContext& ctx) { ctx.log.push_back("root:exit"); },
      nullptr});

  s_a = sm.AddState({
      "A", s_root,
      [](TestContext& ctx, const osp::Event& ev) -> osp::TransitionResult {
        ctx.log.push_back("A:handler");
        if (ev.id == kEvGo) {
          return ctx.sm->RequestTransition(s_b);
        }
        if (ev.id == kEvSelf) {
          return ctx.sm->RequestTransition(s_a);
        }
        return osp::TransitionResult::kUnhandled;
      },
      [](TestContext& ctx) { ctx.log.push_back("A:entry"); },
      [](TestContext& ctx) { ctx.log.push_back("A:exit"); },
      nullptr});

  s_b = sm.AddState({
      "B", s_root,
      [](TestContext& ctx, const osp::Event& ev) -> osp::TransitionResult {
        ctx.log.push_back("B:handler");
        if (ev.id == kEvBack) {
          return ctx.sm->RequestTransition(s_a);
        }
        return osp::TransitionResult::kUnhandled;
      },
      [](TestContext& ctx) { ctx.log.push_back("B:entry"); },
      [](TestContext& ctx) { ctx.log.push_back("B:exit"); },
      nullptr});
}

// ============================================================================
// Helper: build a deeper hierarchy
//
//   root
//    +-- A
//    |   +-- A1
//    |   +-- A2
//    +-- B
//        +-- B1
// ============================================================================

static void BuildDeepSM(SM& sm) {
  s_root = sm.AddState({
      "root", -1,
      [](TestContext& ctx, const osp::Event&) -> osp::TransitionResult {
        ctx.log.push_back("root:handler");
        return osp::TransitionResult::kHandled;
      },
      [](TestContext& ctx) { ctx.log.push_back("root:entry"); },
      [](TestContext& ctx) { ctx.log.push_back("root:exit"); },
      nullptr});

  s_a = sm.AddState({
      "A", s_root,
      [](TestContext& ctx, const osp::Event& ev) -> osp::TransitionResult {
        ctx.log.push_back("A:handler");
        if (ev.id == kEvTick) {
          ++ctx.counter;
          return osp::TransitionResult::kHandled;
        }
        return osp::TransitionResult::kUnhandled;
      },
      [](TestContext& ctx) { ctx.log.push_back("A:entry"); },
      [](TestContext& ctx) { ctx.log.push_back("A:exit"); },
      nullptr});

  s_a1 = sm.AddState({
      "A1", s_a,
      [](TestContext& ctx, const osp::Event& ev) -> osp::TransitionResult {
        ctx.log.push_back("A1:handler");
        if (ev.id == kEvGo) {
          return ctx.sm->RequestTransition(s_a2);
        }
        if (ev.id == kEvDeep) {
          return ctx.sm->RequestTransition(s_b1);
        }
        return osp::TransitionResult::kUnhandled;
      },
      [](TestContext& ctx) { ctx.log.push_back("A1:entry"); },
      [](TestContext& ctx) { ctx.log.push_back("A1:exit"); },
      nullptr});

  s_a2 = sm.AddState({
      "A2", s_a,
      [](TestContext& ctx, const osp::Event& ev) -> osp::TransitionResult {
        ctx.log.push_back("A2:handler");
        if (ev.id == kEvBack) {
          return ctx.sm->RequestTransition(s_a1);
        }
        return osp::TransitionResult::kUnhandled;
      },
      [](TestContext& ctx) { ctx.log.push_back("A2:entry"); },
      [](TestContext& ctx) { ctx.log.push_back("A2:exit"); },
      nullptr});

  s_b = sm.AddState({
      "B", s_root,
      nullptr,
      [](TestContext& ctx) { ctx.log.push_back("B:entry"); },
      [](TestContext& ctx) { ctx.log.push_back("B:exit"); },
      nullptr});

  s_b1 = sm.AddState({
      "B1", s_b,
      [](TestContext& ctx, const osp::Event& ev) -> osp::TransitionResult {
        ctx.log.push_back("B1:handler");
        if (ev.id == kEvBack) {
          return ctx.sm->RequestTransition(s_a1);
        }
        return osp::TransitionResult::kUnhandled;
      },
      [](TestContext& ctx) { ctx.log.push_back("B1:entry"); },
      [](TestContext& ctx) { ctx.log.push_back("B1:exit"); },
      nullptr});
}

// ============================================================================
// Test 1: Create state machine, add states, start
// ============================================================================

TEST_CASE("hsm - create add and start", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  BuildSimpleSM(sm);

  REQUIRE(sm.StateCount() == 3);
  REQUIRE_FALSE(sm.IsStarted());

  sm.SetInitialState(s_a);
  sm.Start();

  REQUIRE(sm.IsStarted());
  REQUIRE(sm.CurrentState() == s_a);
  REQUIRE(std::string(sm.CurrentStateName()) == "A");

  // Entry order: root first, then A (top-down)
  REQUIRE(ctx.log.size() == 2);
  REQUIRE(ctx.log[0] == "root:entry");
  REQUIRE(ctx.log[1] == "A:entry");
}

// ============================================================================
// Test 2: Simple transition between sibling states
// ============================================================================

TEST_CASE("hsm - simple sibling transition", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  BuildSimpleSM(sm);
  sm.SetInitialState(s_a);
  sm.Start();
  ctx.log.clear();

  sm.Dispatch({kEvGo, nullptr});

  REQUIRE(sm.CurrentState() == s_b);
  REQUIRE(std::string(sm.CurrentStateName()) == "B");

  // A:handler -> A:exit -> B:entry
  REQUIRE(ctx.log.size() == 3);
  REQUIRE(ctx.log[0] == "A:handler");
  REQUIRE(ctx.log[1] == "A:exit");
  REQUIRE(ctx.log[2] == "B:entry");
}

// ============================================================================
// Test 3: Entry/exit actions called in correct order
// ============================================================================

TEST_CASE("hsm - entry exit action order", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  BuildSimpleSM(sm);
  sm.SetInitialState(s_a);
  sm.Start();
  ctx.log.clear();

  // A -> B: should exit A, enter B (siblings, LCA is root, no root exit/entry)
  sm.Dispatch({kEvGo, nullptr});

  // Verify exit happens before entry
  size_t exit_pos = 0;
  size_t entry_pos = 0;
  for (size_t i = 0; i < ctx.log.size(); ++i) {
    if (ctx.log[i] == "A:exit") exit_pos = i;
    if (ctx.log[i] == "B:entry") entry_pos = i;
  }
  REQUIRE(exit_pos < entry_pos);
}

// ============================================================================
// Test 4: Event bubbling to parent state
// ============================================================================

TEST_CASE("hsm - event bubbling to parent", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  BuildDeepSM(sm);
  sm.SetInitialState(s_a1);
  sm.Start();
  ctx.log.clear();

  // kEvTick is not handled by A1, but handled by A (parent)
  sm.Dispatch({kEvTick, nullptr});

  REQUIRE(ctx.counter == 1);
  // A1:handler (unhandled) then A:handler (handled with kTick)
  REQUIRE(ctx.log.size() == 2);
  REQUIRE(ctx.log[0] == "A1:handler");
  REQUIRE(ctx.log[1] == "A:handler");
}

// ============================================================================
// Test 5: LCA-based transition (cross-branch)
// ============================================================================

TEST_CASE("hsm - LCA based cross branch transition", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  BuildDeepSM(sm);
  sm.SetInitialState(s_a1);
  sm.Start();
  ctx.log.clear();

  // Transition from A1 to B1: LCA is root
  // Exit: A1, A  (not root)
  // Entry: B, B1 (not root)
  sm.Dispatch({kEvDeep, nullptr});

  REQUIRE(sm.CurrentState() == s_b1);

  REQUIRE(ctx.log.size() == 5);
  REQUIRE(ctx.log[0] == "A1:handler");
  REQUIRE(ctx.log[1] == "A1:exit");
  REQUIRE(ctx.log[2] == "A:exit");
  REQUIRE(ctx.log[3] == "B:entry");
  REQUIRE(ctx.log[4] == "B1:entry");
}

// ============================================================================
// Test 6: Guard condition blocks transition
// ============================================================================

TEST_CASE("hsm - guard blocks transition", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  s_root = sm.AddState({
      "root", -1, nullptr, nullptr, nullptr, nullptr});

  // State with a guard that blocks when counter < 5
  s_a = sm.AddState({
      "A", s_root,
      [](TestContext& ctx, const osp::Event& ev) -> osp::TransitionResult {
        ctx.log.push_back("A:handler");
        if (ev.id == kEvGuarded) {
          return ctx.sm->RequestTransition(s_b);
        }
        return osp::TransitionResult::kUnhandled;
      },
      nullptr, nullptr,
      // Guard: only handle events when counter >= 5
      [](const TestContext& ctx, const osp::Event&) -> bool {
        return ctx.counter >= 5;
      }});

  s_b = sm.AddState({
      "B", s_root,
      nullptr,
      [](TestContext& ctx) { ctx.log.push_back("B:entry"); },
      nullptr,
      nullptr});

  sm.SetInitialState(s_a);
  sm.Start();
  ctx.log.clear();

  // Guard blocks: counter is 0
  sm.Dispatch({kEvGuarded, nullptr});
  REQUIRE(sm.CurrentState() == s_a);
  REQUIRE(ctx.log.empty());  // handler not even called

  // Now satisfy guard
  ctx.counter = 5;
  sm.Dispatch({kEvGuarded, nullptr});
  REQUIRE(sm.CurrentState() == s_b);
  REQUIRE(ctx.log[0] == "A:handler");
  REQUIRE(ctx.log[1] == "B:entry");
}

// ============================================================================
// Test 7: Self-transition (exit + re-enter same state)
// ============================================================================

TEST_CASE("hsm - self transition", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  BuildSimpleSM(sm);
  sm.SetInitialState(s_a);
  sm.Start();
  ctx.log.clear();

  sm.Dispatch({kEvSelf, nullptr});

  REQUIRE(sm.CurrentState() == s_a);

  // A:handler -> A:exit -> A:entry
  REQUIRE(ctx.log.size() == 3);
  REQUIRE(ctx.log[0] == "A:handler");
  REQUIRE(ctx.log[1] == "A:exit");
  REQUIRE(ctx.log[2] == "A:entry");
}

// ============================================================================
// Test 8: Initial state entered on Start()
// ============================================================================

TEST_CASE("hsm - initial state entered on start", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  BuildDeepSM(sm);
  sm.SetInitialState(s_a1);
  sm.Start();

  REQUIRE(sm.CurrentState() == s_a1);

  // Entry path from root -> A -> A1
  REQUIRE(ctx.log.size() == 3);
  REQUIRE(ctx.log[0] == "root:entry");
  REQUIRE(ctx.log[1] == "A:entry");
  REQUIRE(ctx.log[2] == "A1:entry");
}

// ============================================================================
// Test 9: Multiple events processed in sequence
// ============================================================================

TEST_CASE("hsm - multiple events in sequence", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  BuildDeepSM(sm);
  sm.SetInitialState(s_a1);
  sm.Start();
  ctx.log.clear();

  // A1 -> A2 (siblings under A, LCA = A)
  sm.Dispatch({kEvGo, nullptr});
  REQUIRE(sm.CurrentState() == s_a2);

  // A2 -> A1 (siblings under A, LCA = A)
  sm.Dispatch({kEvBack, nullptr});
  REQUIRE(sm.CurrentState() == s_a1);

  // A1 -> B1 (cross-branch, LCA = root)
  sm.Dispatch({kEvDeep, nullptr});
  REQUIRE(sm.CurrentState() == s_b1);

  // B1 -> A1 (cross-branch, LCA = root)
  sm.Dispatch({kEvBack, nullptr});
  REQUIRE(sm.CurrentState() == s_a1);
}

// ============================================================================
// Test 10: IsInState query
// ============================================================================

TEST_CASE("hsm - IsInState query", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  BuildDeepSM(sm);
  sm.SetInitialState(s_a1);
  sm.Start();

  // Current state is A1, which is a child of A, which is a child of root
  REQUIRE(sm.IsInState(s_a1));
  REQUIRE(sm.IsInState(s_a));
  REQUIRE(sm.IsInState(s_root));

  // Not in B or B1
  REQUIRE_FALSE(sm.IsInState(s_b));
  REQUIRE_FALSE(sm.IsInState(s_b1));
  REQUIRE_FALSE(sm.IsInState(s_a2));

  // Transition to B1
  ctx.log.clear();
  sm.Dispatch({kEvDeep, nullptr});

  REQUIRE(sm.IsInState(s_b1));
  REQUIRE(sm.IsInState(s_b));
  REQUIRE(sm.IsInState(s_root));
  REQUIRE_FALSE(sm.IsInState(s_a));
  REQUIRE_FALSE(sm.IsInState(s_a1));
}

TEST_CASE("hsm - guard condition blocks transition", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  s_root = sm.AddState({"root", -1, nullptr, nullptr, nullptr, nullptr});

  s_a = sm.AddState({
      "A", s_root,
      [](TestContext& ctx, const osp::Event& ev) -> osp::TransitionResult {
        if (ev.id == kEvGuarded) {
          return ctx.sm->RequestTransition(s_b);
        }
        return osp::TransitionResult::kUnhandled;
      },
      nullptr, nullptr,
      // Guard: only allow transition when counter >= 10
      [](const TestContext& ctx, const osp::Event& ev) -> bool {
        if (ev.id == kEvGuarded) {
          return ctx.counter >= 10;
        }
        return true;
      }});

  s_b = sm.AddState({
      "B", s_root,
      nullptr,
      [](TestContext& ctx) { ctx.log.push_back("B:entry"); },
      nullptr, nullptr});

  sm.SetInitialState(s_a);
  sm.Start();
  ctx.log.clear();

  // Guard blocks: counter is 0
  sm.Dispatch({kEvGuarded, nullptr});
  REQUIRE(sm.CurrentState() == s_a);
  REQUIRE(ctx.log.empty());

  // Satisfy guard condition
  ctx.counter = 10;
  sm.Dispatch({kEvGuarded, nullptr});
  REQUIRE(sm.CurrentState() == s_b);
  REQUIRE(ctx.log.size() == 1);
  REQUIRE(ctx.log[0] == "B:entry");
}

TEST_CASE("hsm - deep hierarchy 3+ levels LCA transition", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  // Build 4-level hierarchy: root -> A -> A1 -> A1a
  //                          root -> B -> B1 -> B1a
  s_root = sm.AddState({
      "root", -1, nullptr,
      [](TestContext& ctx) { ctx.log.push_back("root:entry"); },
      [](TestContext& ctx) { ctx.log.push_back("root:exit"); },
      nullptr});

  s_a = sm.AddState({
      "A", s_root, nullptr,
      [](TestContext& ctx) { ctx.log.push_back("A:entry"); },
      [](TestContext& ctx) { ctx.log.push_back("A:exit"); },
      nullptr});

  s_a1 = sm.AddState({
      "A1", s_a, nullptr,
      [](TestContext& ctx) { ctx.log.push_back("A1:entry"); },
      [](TestContext& ctx) { ctx.log.push_back("A1:exit"); },
      nullptr});

  static int32_t s_a1a = sm.AddState({
      "A1a", s_a1,
      [](TestContext& ctx, const osp::Event& ev) -> osp::TransitionResult {
        if (ev.id == kEvDeep) {
          return ctx.sm->RequestTransition(s_b1);
        }
        return osp::TransitionResult::kUnhandled;
      },
      [](TestContext& ctx) { ctx.log.push_back("A1a:entry"); },
      [](TestContext& ctx) { ctx.log.push_back("A1a:exit"); },
      nullptr});

  s_b = sm.AddState({
      "B", s_root, nullptr,
      [](TestContext& ctx) { ctx.log.push_back("B:entry"); },
      [](TestContext& ctx) { ctx.log.push_back("B:exit"); },
      nullptr});

  s_b1 = sm.AddState({
      "B1", s_b, nullptr,
      [](TestContext& ctx) { ctx.log.push_back("B1:entry"); },
      [](TestContext& ctx) { ctx.log.push_back("B1:exit"); },
      nullptr});

  sm.SetInitialState(s_a1a);
  sm.Start();
  ctx.log.clear();

  // Transition from A1a to B1: LCA is root
  // Exit: A1a, A1, A (not root)
  // Entry: B, B1 (not root)
  sm.Dispatch({kEvDeep, nullptr});

  REQUIRE(sm.CurrentState() == s_b1);
  REQUIRE(ctx.log.size() == 5);
  REQUIRE(ctx.log[0] == "A1a:exit");
  REQUIRE(ctx.log[1] == "A1:exit");
  REQUIRE(ctx.log[2] == "A:exit");
  REQUIRE(ctx.log[3] == "B:entry");
  REQUIRE(ctx.log[4] == "B1:entry");
}

TEST_CASE("hsm - self-transition same state", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  s_root = sm.AddState({"root", -1, nullptr, nullptr, nullptr, nullptr});

  s_a = sm.AddState({
      "A", s_root,
      [](TestContext& ctx, const osp::Event& ev) -> osp::TransitionResult {
        ctx.log.push_back("A:handler");
        if (ev.id == kEvSelf) {
          return ctx.sm->RequestTransition(s_a);
        }
        return osp::TransitionResult::kUnhandled;
      },
      [](TestContext& ctx) { ctx.log.push_back("A:entry"); },
      [](TestContext& ctx) { ctx.log.push_back("A:exit"); },
      nullptr});

  sm.SetInitialState(s_a);
  sm.Start();
  ctx.log.clear();

  // Self-transition should exit and re-enter
  sm.Dispatch({kEvSelf, nullptr});

  REQUIRE(sm.CurrentState() == s_a);
  REQUIRE(ctx.log.size() == 3);
  REQUIRE(ctx.log[0] == "A:handler");
  REQUIRE(ctx.log[1] == "A:exit");
  REQUIRE(ctx.log[2] == "A:entry");
}

TEST_CASE("hsm - dispatch event with no handler", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  BuildSimpleSM(sm);
  sm.SetInitialState(s_a);
  sm.Start();
  ctx.log.clear();

  // Dispatch an event that no state handles
  sm.Dispatch({kEvUnknown, nullptr});

  // Should remain in state A
  REQUIRE(sm.CurrentState() == s_a);

  // Handler was called but returned kUnhandled, bubbled to root
  REQUIRE(ctx.log.size() == 2);
  REQUIRE(ctx.log[0] == "A:handler");
  REQUIRE(ctx.log[1] == "root:handler");
}

TEST_CASE("hsm - entry exit action ordering in hierarchy", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  BuildDeepSM(sm);
  sm.SetInitialState(s_a1);
  sm.Start();

  // Verify initial entry order: root -> A -> A1 (top-down)
  REQUIRE(ctx.log.size() == 3);
  REQUIRE(ctx.log[0] == "root:entry");
  REQUIRE(ctx.log[1] == "A:entry");
  REQUIRE(ctx.log[2] == "A1:entry");

  ctx.log.clear();

  // Transition A1 -> A2 (siblings under A)
  // Exit: A1 (bottom-up)
  // Entry: A2 (top-down)
  sm.Dispatch({kEvGo, nullptr});

  REQUIRE(sm.CurrentState() == s_a2);

  // Find positions of exit and entry
  bool found_a1_exit = false;
  bool found_a2_entry = false;
  size_t a1_exit_pos = 0;
  size_t a2_entry_pos = 0;

  for (size_t i = 0; i < ctx.log.size(); ++i) {
    if (ctx.log[i] == "A1:exit") {
      found_a1_exit = true;
      a1_exit_pos = i;
    }
    if (ctx.log[i] == "A2:entry") {
      found_a2_entry = true;
      a2_entry_pos = i;
    }
  }

  REQUIRE(found_a1_exit);
  REQUIRE(found_a2_entry);
  REQUIRE(a1_exit_pos < a2_entry_pos);
}

// ============================================================================
// ForceTransition tests
// ============================================================================

TEST_CASE("hsm - ForceTransition basic transition", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  BuildSimpleSM(sm);
  sm.SetInitialState(s_a);
  sm.Start();

  REQUIRE(sm.CurrentState() == s_a);
  ctx.log.clear();

  // Force transition from A to B without dispatching an event
  bool ok = sm.ForceTransition(s_b);
  REQUIRE(ok);
  REQUIRE(sm.CurrentState() == s_b);

  // Should have exit A, entry B (within same parent root)
  bool found_a_exit = false;
  bool found_b_entry = false;
  for (const auto& entry : ctx.log) {
    if (entry == "A:exit") found_a_exit = true;
    if (entry == "B:entry") found_b_entry = true;
  }
  REQUIRE(found_a_exit);
  REQUIRE(found_b_entry);
}

TEST_CASE("hsm - ForceTransition hierarchical exit/entry", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  BuildDeepSM(sm);
  sm.SetInitialState(s_a1);
  sm.Start();

  REQUIRE(sm.CurrentState() == s_a1);
  ctx.log.clear();

  // Force transition from A1 (child of A) to B1 (child of B)
  // Exit path: A1 -> A (up to root, not including root)
  // Entry path: B -> B1 (from root down, not including root)
  bool ok = sm.ForceTransition(s_b1);
  REQUIRE(ok);
  REQUIRE(sm.CurrentState() == s_b1);

  bool found_a1_exit = false;
  bool found_a_exit = false;
  bool found_b_entry = false;
  bool found_b1_entry = false;
  size_t a1_exit_pos = 0, a_exit_pos = 0;
  size_t b_entry_pos = 0, b1_entry_pos = 0;

  for (size_t i = 0; i < ctx.log.size(); ++i) {
    if (ctx.log[i] == "A1:exit") { found_a1_exit = true; a1_exit_pos = i; }
    if (ctx.log[i] == "A:exit") { found_a_exit = true; a_exit_pos = i; }
    if (ctx.log[i] == "B:entry") { found_b_entry = true; b_entry_pos = i; }
    if (ctx.log[i] == "B1:entry") { found_b1_entry = true; b1_entry_pos = i; }
  }

  REQUIRE(found_a1_exit);
  REQUIRE(found_a_exit);
  REQUIRE(found_b_entry);
  REQUIRE(found_b1_entry);

  // Exit order: A1 before A (bottom-up)
  REQUIRE(a1_exit_pos < a_exit_pos);
  // Entry order: B before B1 (top-down)
  REQUIRE(b_entry_pos < b1_entry_pos);
  // Exit before entry
  REQUIRE(a_exit_pos < b_entry_pos);
}

TEST_CASE("hsm - ForceTransition returns false when not started", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  BuildSimpleSM(sm);
  sm.SetInitialState(s_a);
  // NOT calling sm.Start()

  bool ok = sm.ForceTransition(s_b);
  REQUIRE_FALSE(ok);
}

TEST_CASE("hsm - ForceTransition returns false for invalid target", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  BuildSimpleSM(sm);
  sm.SetInitialState(s_a);
  sm.Start();

  // Negative index
  REQUIRE_FALSE(sm.ForceTransition(-1));
  // Out of range index
  REQUIRE_FALSE(sm.ForceTransition(100));
  // Current state should be unchanged
  REQUIRE(sm.CurrentState() == s_a);
}

TEST_CASE("hsm - ForceTransition self-transition", "[hsm]") {
  TestContext ctx;
  SM sm(ctx);
  ctx.sm = &sm;

  BuildSimpleSM(sm);
  sm.SetInitialState(s_a);
  sm.Start();
  ctx.log.clear();

  // Force self-transition: exit A then re-enter A
  bool ok = sm.ForceTransition(s_a);
  REQUIRE(ok);
  REQUIRE(sm.CurrentState() == s_a);

  bool found_a_exit = false;
  bool found_a_entry = false;
  for (const auto& entry : ctx.log) {
    if (entry == "A:exit") found_a_exit = true;
    if (entry == "A:entry") found_a_entry = true;
  }
  REQUIRE(found_a_exit);
  REQUIRE(found_a_entry);
}
