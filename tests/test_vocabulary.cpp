/**
 * @file test_vocabulary.cpp
 * @brief Tests for vocabulary.hpp types
 */

#include "osp/vocabulary.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

// ============================================================================
// expected<V, E> tests
// ============================================================================

TEST_CASE("expected success path", "[vocabulary][expected]") {
  auto r = osp::expected<int, osp::ConfigError>::success(42);
  REQUIRE(r.has_value());
  REQUIRE(static_cast<bool>(r));
  REQUIRE(r.value() == 42);
}

TEST_CASE("expected error path", "[vocabulary][expected]") {
  auto r = osp::expected<int, osp::ConfigError>::error(
      osp::ConfigError::kFileNotFound);
  REQUIRE(!r.has_value());
  REQUIRE(!static_cast<bool>(r));
  REQUIRE(r.get_error() == osp::ConfigError::kFileNotFound);
}

TEST_CASE("expected void specialization", "[vocabulary][expected]") {
  auto ok = osp::expected<void, osp::TimerError>::success();
  REQUIRE(ok.has_value());

  auto err =
      osp::expected<void, osp::TimerError>::error(osp::TimerError::kSlotsFull);
  REQUIRE(!err.has_value());
  REQUIRE(err.get_error() == osp::TimerError::kSlotsFull);
}

TEST_CASE("expected value_or", "[vocabulary][expected]") {
  auto ok = osp::expected<int, osp::ConfigError>::success(10);
  REQUIRE(ok.value_or(99) == 10);

  auto err =
      osp::expected<int, osp::ConfigError>::error(osp::ConfigError::kParseError);
  REQUIRE(err.value_or(99) == 99);
}

TEST_CASE("expected copy/move", "[vocabulary][expected]") {
  auto r1 = osp::expected<int, osp::ConfigError>::success(7);
  auto r2 = r1;  // copy
  REQUIRE(r2.value() == 7);

  auto r3 = static_cast<osp::expected<int, osp::ConfigError>&&>(r1);  // move
  REQUIRE(r3.value() == 7);
}

// ============================================================================
// optional<T> tests
// ============================================================================

TEST_CASE("optional empty", "[vocabulary][optional]") {
  osp::optional<int> o;
  REQUIRE(!o.has_value());
  REQUIRE(!static_cast<bool>(o));
}

TEST_CASE("optional with value", "[vocabulary][optional]") {
  osp::optional<int> o(42);
  REQUIRE(o.has_value());
  REQUIRE(o.value() == 42);
}

TEST_CASE("optional value_or", "[vocabulary][optional]") {
  osp::optional<int> empty;
  REQUIRE(empty.value_or(99) == 99);

  osp::optional<int> full(10);
  REQUIRE(full.value_or(99) == 10);
}

TEST_CASE("optional reset", "[vocabulary][optional]") {
  osp::optional<int> o(42);
  REQUIRE(o.has_value());
  o.reset();
  REQUIRE(!o.has_value());
}

TEST_CASE("optional copy/move", "[vocabulary][optional]") {
  osp::optional<int> o1(5);
  osp::optional<int> o2 = o1;  // copy
  REQUIRE(o2.value() == 5);

  osp::optional<int> o3 = static_cast<osp::optional<int>&&>(o1);  // move
  REQUIRE(o3.value() == 5);
}

// ============================================================================
// FixedFunction tests
// ============================================================================

static int free_fn_result = 0;
static void free_fn() { free_fn_result = 42; }

TEST_CASE("FixedFunction from lambda", "[vocabulary][fixed_function]") {
  int captured = 0;
  osp::FixedFunction<void()> fn([&captured]() { captured = 99; });
  REQUIRE(static_cast<bool>(fn));
  fn();
  REQUIRE(captured == 99);
}

TEST_CASE("FixedFunction from function pointer", "[vocabulary][fixed_function]") {
  free_fn_result = 0;
  osp::FixedFunction<void()> fn(free_fn);
  fn();
  REQUIRE(free_fn_result == 42);
}

TEST_CASE("FixedFunction empty", "[vocabulary][fixed_function]") {
  osp::FixedFunction<void()> fn;
  REQUIRE(!static_cast<bool>(fn));
}

TEST_CASE("FixedFunction with return value", "[vocabulary][fixed_function]") {
  osp::FixedFunction<int(int, int)> fn([](int a, int b) { return a + b; });
  REQUIRE(fn(3, 4) == 7);
}

TEST_CASE("FixedFunction move", "[vocabulary][fixed_function]") {
  int val = 0;
  osp::FixedFunction<void()> fn1([&val]() { val = 100; });
  osp::FixedFunction<void()> fn2 =
      static_cast<osp::FixedFunction<void()>&&>(fn1);
  REQUIRE(!static_cast<bool>(fn1));
  REQUIRE(static_cast<bool>(fn2));
  fn2();
  REQUIRE(val == 100);
}

// ============================================================================
// function_ref tests
// ============================================================================

static int ref_fn(int x) { return x * 2; }

TEST_CASE("function_ref from lambda", "[vocabulary][function_ref]") {
  auto lam = [](int x) { return x + 1; };
  osp::function_ref<int(int)> ref(lam);
  REQUIRE(ref(5) == 6);
}

TEST_CASE("function_ref from function pointer", "[vocabulary][function_ref]") {
  osp::function_ref<int(int)> ref(ref_fn);
  REQUIRE(ref(3) == 6);
}

// ============================================================================
// FixedString tests
// ============================================================================

TEST_CASE("FixedString default empty", "[vocabulary][fixed_string]") {
  osp::FixedString<32> s;
  REQUIRE(s.empty());
  REQUIRE(s.size() == 0);
  REQUIRE(s.capacity() == 32);
}

TEST_CASE("FixedString from literal", "[vocabulary][fixed_string]") {
  osp::FixedString<32> s("hello");
  REQUIRE(s.size() == 5);
  REQUIRE(s == "hello");
}

TEST_CASE("FixedString truncation", "[vocabulary][fixed_string]") {
  osp::FixedString<5> s(osp::TruncateToCapacity, "hello world");
  REQUIRE(s.size() == 5);
  REQUIRE(std::memcmp(s.c_str(), "hello", 5) == 0);
}

TEST_CASE("FixedString assign", "[vocabulary][fixed_string]") {
  osp::FixedString<32> s;
  s.assign(osp::TruncateToCapacity, "test");
  REQUIRE(s.size() == 4);
  REQUIRE(s == "test");
}

TEST_CASE("FixedString clear", "[vocabulary][fixed_string]") {
  osp::FixedString<32> s("data");
  s.clear();
  REQUIRE(s.empty());
}

TEST_CASE("FixedString equality across capacities", "[vocabulary][fixed_string]") {
  osp::FixedString<8> s1("abc");
  osp::FixedString<32> s2("abc");
  REQUIRE(s1 == s2);
}

// ============================================================================
// FixedVector tests
// ============================================================================

TEST_CASE("FixedVector default empty", "[vocabulary][fixed_vector]") {
  osp::FixedVector<int, 8> v;
  REQUIRE(v.empty());
  REQUIRE(v.size() == 0);
  REQUIRE(v.capacity() == 8);
}

TEST_CASE("FixedVector push/pop", "[vocabulary][fixed_vector]") {
  osp::FixedVector<int, 4> v;
  REQUIRE(v.push_back(1));
  REQUIRE(v.push_back(2));
  REQUIRE(v.push_back(3));
  REQUIRE(v.size() == 3);
  REQUIRE(v[0] == 1);
  REQUIRE(v[2] == 3);

  REQUIRE(v.pop_back());
  REQUIRE(v.size() == 2);
}

TEST_CASE("FixedVector full boundary", "[vocabulary][fixed_vector]") {
  osp::FixedVector<int, 2> v;
  REQUIRE(v.push_back(10));
  REQUIRE(v.push_back(20));
  REQUIRE(v.full());
  REQUIRE(!v.push_back(30));  // should fail
}

TEST_CASE("FixedVector erase_unordered", "[vocabulary][fixed_vector]") {
  osp::FixedVector<int, 8> v;
  v.push_back(1);
  v.push_back(2);
  v.push_back(3);

  REQUIRE(v.erase_unordered(0));
  REQUIRE(v.size() == 2);
  // Element 0 replaced by last element (3)
  REQUIRE(v[0] == 3);
  REQUIRE(v[1] == 2);
}

TEST_CASE("FixedVector iterators", "[vocabulary][fixed_vector]") {
  osp::FixedVector<int, 4> v;
  v.push_back(10);
  v.push_back(20);
  v.push_back(30);

  int sum = 0;
  for (const auto& val : v) {
    sum += val;
  }
  REQUIRE(sum == 60);
}

TEST_CASE("FixedVector copy", "[vocabulary][fixed_vector]") {
  osp::FixedVector<int, 4> v1;
  v1.push_back(1);
  v1.push_back(2);

  osp::FixedVector<int, 4> v2 = v1;
  REQUIRE(v2.size() == 2);
  REQUIRE(v2[0] == 1);
  REQUIRE(v2[1] == 2);
}

TEST_CASE("FixedVector move", "[vocabulary][fixed_vector]") {
  osp::FixedVector<int, 4> v1;
  v1.push_back(5);
  v1.push_back(6);

  osp::FixedVector<int, 4> v2 =
      static_cast<osp::FixedVector<int, 4>&&>(v1);
  REQUIRE(v2.size() == 2);
  REQUIRE(v2[0] == 5);
  REQUIRE(v1.empty());
}

TEST_CASE("FixedVector with non-trivial type", "[vocabulary][fixed_vector]") {
  osp::FixedVector<std::string, 4> v;
  v.push_back(std::string("hello"));
  v.emplace_back("world");
  REQUIRE(v.size() == 2);
  REQUIRE(v[0] == "hello");
  REQUIRE(v[1] == "world");
  v.clear();
  REQUIRE(v.empty());
}

// ============================================================================
// not_null tests
// ============================================================================

TEST_CASE("not_null valid pointer", "[vocabulary][not_null]") {
  int x = 42;
  osp::not_null<int*> nn(&x);
  REQUIRE(nn.get() == &x);
  REQUIRE(*nn == 42);
}

// ============================================================================
// NewType tests
// ============================================================================

TEST_CASE("NewType prevents mixing", "[vocabulary][newtype]") {
  osp::TimerTaskId tid(1);
  osp::SessionId sid(1);

  REQUIRE(tid.value() == 1);
  REQUIRE(sid.value() == 1);

  // These are different types - can't compare:
  // tid == sid;  // Would not compile

  osp::TimerTaskId tid2(2);
  REQUIRE(tid != tid2);
  REQUIRE(tid < tid2);
}

// ============================================================================
// ScopeGuard tests
// ============================================================================

TEST_CASE("ScopeGuard executes on exit", "[vocabulary][scope_guard]") {
  int val = 0;
  {
    osp::ScopeGuard guard(
        osp::FixedFunction<void()>([&val]() { val = 42; }));
    REQUIRE(val == 0);
  }
  REQUIRE(val == 42);
}

TEST_CASE("ScopeGuard release prevents execution", "[vocabulary][scope_guard]") {
  int val = 0;
  {
    osp::ScopeGuard guard(
        osp::FixedFunction<void()>([&val]() { val = 42; }));
    guard.release();
  }
  REQUIRE(val == 0);
}

TEST_CASE("ScopeGuard move", "[vocabulary][scope_guard]") {
  int val = 0;
  {
    osp::ScopeGuard guard1(
        osp::FixedFunction<void()>([&val]() { val = 99; }));
    osp::ScopeGuard guard2 = static_cast<osp::ScopeGuard&&>(guard1);
    // guard1 is released, guard2 owns the cleanup
  }
  REQUIRE(val == 99);
}

TEST_CASE("OSP_SCOPE_EXIT macro", "[vocabulary][scope_guard]") {
  int val = 0;
  {
    OSP_SCOPE_EXIT(val = 77);
    REQUIRE(val == 0);
  }
  REQUIRE(val == 77);
}

// ============================================================================
// and_then / or_else tests
// ============================================================================

TEST_CASE("and_then on success", "[vocabulary][functional]") {
  auto r = osp::expected<int, osp::ConfigError>::success(10);
  auto r2 = osp::and_then(r, [](int v) {
    return osp::expected<int, osp::ConfigError>::success(v * 2);
  });
  REQUIRE(r2.value() == 20);
}

TEST_CASE("and_then on error propagates", "[vocabulary][functional]") {
  auto r = osp::expected<int, osp::ConfigError>::error(
      osp::ConfigError::kParseError);
  auto r2 = osp::and_then(r, [](int v) {
    return osp::expected<int, osp::ConfigError>::success(v * 2);
  });
  REQUIRE(!r2.has_value());
  REQUIRE(r2.get_error() == osp::ConfigError::kParseError);
}

TEST_CASE("or_else on error", "[vocabulary][functional]") {
  auto r = osp::expected<int, osp::ConfigError>::error(
      osp::ConfigError::kFileNotFound);
  osp::ConfigError captured{};
  osp::or_else(r, [&captured](osp::ConfigError e) { captured = e; });
  REQUIRE(captured == osp::ConfigError::kFileNotFound);
}

TEST_CASE("or_else on success does nothing", "[vocabulary][functional]") {
  auto r = osp::expected<int, osp::ConfigError>::success(5);
  bool called = false;
  osp::or_else(r, [&called](osp::ConfigError) { called = true; });
  REQUIRE(!called);
}
