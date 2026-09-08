/**
 * @file test_hsm_table.cpp
 * @brief Tests for osp/hsm_table.hpp (static HSM transition table).
 *
 * TDD batch 1: construct/Start + simple external transition + action.
 * Hierarchical LCA, kInternal/kSelf, reject arc, ForceTransition come in
 * later batches.
 */

#include "osp/hsm_table.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

namespace {

enum : uint32_t {
  kEvGo = 1,
  kEvBack = 2,
  kEvStay = 10,
  kEvSelfT = 11,
  kEvTick = 20,
  kEvValued = 30,
};

struct TestContext {
  std::vector<std::string> log;
  int action_count = 0;
};

inline void OnEntryRoot(TestContext& ctx) {
  ctx.log.push_back("root:entry");
}
inline void OnExitRoot(TestContext& ctx) {
  ctx.log.push_back("root:exit");
}
inline void OnEntryA(TestContext& ctx) {
  ctx.log.push_back("A:entry");
}
inline void OnExitA(TestContext& ctx) {
  ctx.log.push_back("A:exit");
}
inline void OnEntryB(TestContext& ctx) {
  ctx.log.push_back("B:entry");
}
inline void OnExitB(TestContext& ctx) {
  ctx.log.push_back("B:exit");
}

inline void ActAToB(TestContext& ctx, const void* /*data*/) {
  ctx.log.push_back("act:A->B");
  ++ctx.action_count;
}
inline void ActBToA(TestContext& ctx, const void* /*data*/) {
  ctx.log.push_back("act:B->A");
  ++ctx.action_count;
}
inline void ActStay(TestContext& ctx, const void* /*data*/) {
  ctx.log.push_back("act:stay");
  ++ctx.action_count;
}
inline void ActSelf(TestContext& ctx, const void* /*data*/) {
  ctx.log.push_back("act:self");
  ++ctx.action_count;
}

// Fixture: root + A + B (A,B under root), 2 external transitions A<->B.
static constexpr int32_t kRoot = 0;
static constexpr int32_t kA = 1;
static constexpr int32_t kB = 2;

static constexpr osp::StateDef<TestContext> kStates[] = {
    {"root", -1, &OnEntryRoot, &OnExitRoot},
    {"A", kRoot, &OnEntryA, &OnExitA},
    {"B", kRoot, &OnEntryB, &OnExitB},
};

static constexpr osp::TransitionDef<TestContext> kTrans[] = {
    {kA, kEvGo, kB, osp::TransitionKind::kExternal, &ActAToB},
    {kB, kEvBack, kA, osp::TransitionKind::kExternal, &ActBToA},
    {kA, kEvStay, kA, osp::TransitionKind::kInternal, &ActStay},
    {kA, kEvSelfT, kA, osp::TransitionKind::kSelf, &ActSelf},
};

// ============================================================================
// Deep hierarchy fixture for bubbling + LCA tests.
//   root
//    +-- A
//    |   +-- A1 (initial)
//    |   +-- A2
//    +-- B
//        +-- B1
// ============================================================================

struct DeepContext {
  std::vector<std::string> log;
  int tick = 0;
};

inline void DLog(const char* name, const char* kind, DeepContext& ctx) {
  ctx.log.push_back(std::string(name) + ":" + kind);
}
inline void DEntryRoot(DeepContext& ctx) {
  DLog("root", "entry", ctx);
}
inline void DExitRoot(DeepContext& ctx) {
  DLog("root", "exit", ctx);
}
inline void DEntryA(DeepContext& ctx) {
  DLog("A", "entry", ctx);
}
inline void DExitA(DeepContext& ctx) {
  DLog("A", "exit", ctx);
}
inline void DEntryA1(DeepContext& ctx) {
  DLog("A1", "entry", ctx);
}
inline void DExitA1(DeepContext& ctx) {
  DLog("A1", "exit", ctx);
}
inline void DEntryA2(DeepContext& ctx) {
  DLog("A2", "entry", ctx);
}
inline void DExitA2(DeepContext& ctx) {
  DLog("A2", "exit", ctx);
}
inline void DEntryB(DeepContext& ctx) {
  DLog("B", "entry", ctx);
}
inline void DExitB(DeepContext& ctx) {
  DLog("B", "exit", ctx);
}
inline void DEntryB1(DeepContext& ctx) {
  DLog("B1", "entry", ctx);
}
inline void DExitB1(DeepContext& ctx) {
  DLog("B1", "exit", ctx);
}

inline void DActTick(DeepContext& ctx, const void* /*data*/) {
  ++ctx.tick;
}
inline void DActA1ToB1(DeepContext& ctx, const void* /*data*/) {
  ctx.log.push_back("act:A1->B1");
}

// ============================================================================
// Guarded transition fixture: same (from, event) with a guard selecting
// between two targets (mirrors the parser's conditional transitions).
// ============================================================================

struct GuardContext {
  std::vector<std::string> log;
};

inline void GActAccept(GuardContext& ctx, const void* /*data*/) {
  ctx.log.push_back("accept");
}
inline void GActReject(GuardContext& ctx, const void* /*data*/) {
  ctx.log.push_back("reject");
}

inline void GEntryOk(GuardContext& ctx) {
  ctx.log.push_back("ok:entry");
}
inline void GExitOk(GuardContext& ctx) {
  ctx.log.push_back("ok:exit");
}

/// Guard: true only when the event data byte equals 0xAA (frame header).
inline bool GIsHeader(GuardContext& /*ctx*/, const void* data) {
  if (data == nullptr) {
    return false;
  }
  return 0xAAU == *static_cast<const uint8_t*>(data);
}

static constexpr int32_t kGroot = 0;
static constexpr int32_t kGidle = 1;
static constexpr int32_t kGok = 2;

static constexpr osp::StateDef<GuardContext> kGuardStates[] = {
    {"root", -1, nullptr, nullptr},
    {"idle", kGroot, nullptr, nullptr},
    {"ok", kGroot, &GEntryOk, &GExitOk},
};

// Same (idle, kEvValued): guard-first ordering picks the matching row.
static constexpr osp::TransitionDef<GuardContext> kGuardTrans[] = {
    {kGidle, kEvValued, kGok, osp::TransitionKind::kExternal, &GActAccept, &GIsHeader},
    {kGidle, kEvValued, kGidle, osp::TransitionKind::kInternal, &GActReject, nullptr},
};

static constexpr int32_t kDroot = 0;
static constexpr int32_t kDa = 1;
static constexpr int32_t kDa1 = 2;
static constexpr int32_t kDa2 = 3;
static constexpr int32_t kDb = 4;
static constexpr int32_t kDb1 = 5;

static constexpr osp::StateDef<DeepContext> kDeepStates[] = {
    {"root", -1, &DEntryRoot, &DExitRoot}, {"A", kDroot, &DEntryA, &DExitA}, {"A1", kDa, &DEntryA1, &DExitA1},
    {"A2", kDa, &DEntryA2, &DExitA2},      {"B", kDroot, &DEntryB, &DExitB}, {"B1", kDb, &DEntryB1, &DExitB1},
};

static constexpr osp::TransitionDef<DeepContext> kDeepTrans[] = {
    // Parent A handles kEvTick (leaf A1 does not); internal stay keeps A1.
    {kDa, kEvTick, kDa, osp::TransitionKind::kInternal, &DActTick},
    // Cross-branch A1 -> B1: LCA is root.
    {kDa1, kEvGo, kDb1, osp::TransitionKind::kExternal, &DActA1ToB1},
};

}  // namespace

TEST_CASE("hsm_table - construct and start enters initial state", "[hsm_table]") {
  TestContext ctx;
  osp::TableHsm<TestContext, 3, 2> hsm(ctx, kStates, 3, kTrans, 2);

  REQUIRE_FALSE(hsm.IsStarted());

  hsm.SetInitialState(kA);
  hsm.Start();

  REQUIRE(hsm.IsStarted());
  REQUIRE(hsm.CurrentState() == kA);
  REQUIRE(std::string(hsm.CurrentStateName()) == "A");

  // Entry path: root -> A (top-down)
  REQUIRE(ctx.log.size() == 2);
  REQUIRE(ctx.log[0] == "root:entry");
  REQUIRE(ctx.log[1] == "A:entry");
}

TEST_CASE("hsm_table - external transition runs action then exit/entry", "[hsm_table]") {
  TestContext ctx;
  osp::TableHsm<TestContext, 3, 2> hsm(ctx, kStates, 3, kTrans, 2);
  hsm.SetInitialState(kA);
  hsm.Start();
  ctx.log.clear();
  ctx.action_count = 0;

  hsm.Dispatch(osp::Event{kEvGo, nullptr});

  REQUIRE(hsm.CurrentState() == kB);
  REQUIRE(std::string(hsm.CurrentStateName()) == "B");

  // action -> A:exit -> B:entry (LCA = root, root stays)
  REQUIRE(ctx.log.size() == 3);
  REQUIRE(ctx.log[0] == "act:A->B");
  REQUIRE(ctx.log[1] == "A:exit");
  REQUIRE(ctx.log[2] == "B:entry");
  REQUIRE(ctx.action_count == 1);
}

TEST_CASE("hsm_table - internal transition runs action and stays", "[hsm_table]") {
  TestContext ctx;
  osp::TableHsm<TestContext, 3, 4> hsm(ctx, kStates, 3, kTrans, 4);
  hsm.SetInitialState(kA);
  hsm.Start();
  ctx.log.clear();
  ctx.action_count = 0;

  hsm.Dispatch(osp::Event{kEvStay, nullptr});

  REQUIRE(hsm.CurrentState() == kA);
  REQUIRE(ctx.action_count == 1);
  // kInternal: no exit/entry, just the action.
  REQUIRE(ctx.log.size() == 1);
  REQUIRE(ctx.log[0] == "act:stay");
}

TEST_CASE("hsm_table - self transition exits and re-enters", "[hsm_table]") {
  TestContext ctx;
  osp::TableHsm<TestContext, 3, 4> hsm(ctx, kStates, 3, kTrans, 4);
  hsm.SetInitialState(kA);
  hsm.Start();
  ctx.log.clear();
  ctx.action_count = 0;

  hsm.Dispatch(osp::Event{kEvSelfT, nullptr});

  REQUIRE(hsm.CurrentState() == kA);
  REQUIRE(ctx.action_count == 1);
  REQUIRE(ctx.log.size() == 3);
  REQUIRE(ctx.log[0] == "act:self");
  REQUIRE(ctx.log[1] == "A:exit");
  REQUIRE(ctx.log[2] == "A:entry");
}

TEST_CASE("hsm_table - event bubbles to parent handler", "[hsm_table]") {
  DeepContext ctx;
  osp::TableHsm<DeepContext, 6, 2> hsm(ctx, kDeepStates, 6, kDeepTrans, 2);
  hsm.SetInitialState(kDa1);
  hsm.Start();
  ctx.log.clear();

  hsm.Dispatch(osp::Event{kEvTick, nullptr});

  // Parent A's kInternal row runs the action; the leaf A1 is unchanged.
  REQUIRE(hsm.CurrentState() == kDa1);
  REQUIRE(ctx.tick == 1);
}

TEST_CASE("hsm_table - LCA cross-branch transition", "[hsm_table]") {
  DeepContext ctx;
  osp::TableHsm<DeepContext, 6, 2> hsm(ctx, kDeepStates, 6, kDeepTrans, 2);
  hsm.SetInitialState(kDa1);
  hsm.Start();
  ctx.log.clear();

  hsm.Dispatch(osp::Event{kEvGo, nullptr});

  REQUIRE(hsm.CurrentState() == kDb1);
  // action -> A1:exit -> A:exit -> B:entry -> B1:entry
  REQUIRE(ctx.log.size() == 5);
  REQUIRE(ctx.log[0] == "act:A1->B1");
  REQUIRE(ctx.log[1] == "A1:exit");
  REQUIRE(ctx.log[2] == "A:exit");
  REQUIRE(ctx.log[3] == "B:entry");
  REQUIRE(ctx.log[4] == "B1:entry");
}

// ============================================================================
// Batch 3: reject arc, ForceTransition, IsInState
// ============================================================================

TEST_CASE("hsm_table - unmatched event is silently rejected (reject arc)", "[hsm_table]") {
  TestContext ctx;
  osp::TableHsm<TestContext, 3, 4> hsm(ctx, kStates, 3, kTrans, 4);
  hsm.SetInitialState(kA);
  hsm.Start();
  ctx.log.clear();
  ctx.action_count = 0;

  hsm.Dispatch(osp::Event{999U, nullptr});

  // Reject: stay in A, no action, no exit/entry.
  REQUIRE(hsm.CurrentState() == kA);
  REQUIRE(ctx.action_count == 0);
  REQUIRE(ctx.log.empty());
}

TEST_CASE("hsm_table - ForceTransition executes LCA exit/entry", "[hsm_table]") {
  DeepContext ctx;
  osp::TableHsm<DeepContext, 6, 2> hsm(ctx, kDeepStates, 6, kDeepTrans, 2);
  hsm.SetInitialState(kDa1);
  hsm.Start();
  ctx.log.clear();

  REQUIRE(hsm.ForceTransition(kDb1));
  REQUIRE(hsm.CurrentState() == kDb1);
  // A1:exit -> A:exit -> B:entry -> B1:entry (no action row involved).
  REQUIRE(ctx.log.size() == 4);
  REQUIRE(ctx.log[0] == "A1:exit");
  REQUIRE(ctx.log[1] == "A:exit");
  REQUIRE(ctx.log[2] == "B:entry");
  REQUIRE(ctx.log[3] == "B1:entry");
}

TEST_CASE("hsm_table - ForceTransition rejects invalid target and not-started", "[hsm_table]") {
  TestContext ctx;
  osp::TableHsm<TestContext, 3, 4> hsm(ctx, kStates, 3, kTrans, 4);
  hsm.SetInitialState(kA);

  // Not started.
  REQUIRE_FALSE(hsm.ForceTransition(kB));

  hsm.Start();
  // Out-of-range target.
  REQUIRE_FALSE(hsm.ForceTransition(100));
  REQUIRE_FALSE(hsm.ForceTransition(-1));
  REQUIRE(hsm.CurrentState() == kA);
}

TEST_CASE("hsm_table - IsInState walks the parent chain", "[hsm_table]") {
  DeepContext ctx;
  osp::TableHsm<DeepContext, 6, 2> hsm(ctx, kDeepStates, 6, kDeepTrans, 2);
  hsm.SetInitialState(kDa1);
  hsm.Start();

  REQUIRE(hsm.IsInState(kDa1));
  REQUIRE(hsm.IsInState(kDa));
  REQUIRE(hsm.IsInState(kDroot));
  REQUIRE_FALSE(hsm.IsInState(kDa2));
  REQUIRE_FALSE(hsm.IsInState(kDb1));

  hsm.Dispatch(osp::Event{kEvGo, nullptr});
  REQUIRE(hsm.IsInState(kDb1));
  REQUIRE(hsm.IsInState(kDb));
  REQUIRE_FALSE(hsm.IsInState(kDa));
}

// ============================================================================
// Batch 4: guarded transitions
// ============================================================================

TEST_CASE("hsm_table - guard true picks the guarded row", "[hsm_table]") {
  GuardContext ctx;
  osp::TableHsm<GuardContext, 3, 2> hsm(ctx, kGuardStates, 3, kGuardTrans, 2);
  hsm.SetInitialState(kGidle);
  hsm.Start();
  ctx.log.clear();

  const uint8_t header = 0xAAU;
  hsm.Dispatch(osp::Event{kEvValued, &header});

  // Guard passes: accept action + external transition to ok.
  REQUIRE(hsm.CurrentState() == kGok);
  REQUIRE(ctx.log.size() == 2);
  REQUIRE(ctx.log[0] == "accept");
  REQUIRE(ctx.log[1] == "ok:entry");
}

TEST_CASE("hsm_table - guard false falls through to the next row", "[hsm_table]") {
  GuardContext ctx;
  osp::TableHsm<GuardContext, 3, 2> hsm(ctx, kGuardStates, 3, kGuardTrans, 2);
  hsm.SetInitialState(kGidle);
  hsm.Start();
  ctx.log.clear();

  const uint8_t noise = 0x55U;
  hsm.Dispatch(osp::Event{kEvValued, &noise});

  // Guard fails: the unguarded row runs its action and stays (kInternal).
  REQUIRE(hsm.CurrentState() == kGidle);
  REQUIRE(ctx.log.size() == 1);
  REQUIRE(ctx.log[0] == "reject");
}

TEST_CASE("hsm_table - guard false with no fallback is rejected", "[hsm_table]") {
  GuardContext ctx;
  // Only the guarded row exists: a failing guard must reject like an
  // unmatched event (no action, no state change).
  static constexpr osp::TransitionDef<GuardContext> kOnlyGuarded[] = {
      {kGidle, kEvValued, kGok, osp::TransitionKind::kExternal, &GActAccept, &GIsHeader},
  };
  osp::TableHsm<GuardContext, 3, 1> hsm(ctx, kGuardStates, 3, kOnlyGuarded, 1);
  hsm.SetInitialState(kGidle);
  hsm.Start();
  ctx.log.clear();

  const uint8_t noise = 0x00U;
  hsm.Dispatch(osp::Event{kEvValued, &noise});

  REQUIRE(hsm.CurrentState() == kGidle);
  REQUIRE(ctx.log.empty());
}
