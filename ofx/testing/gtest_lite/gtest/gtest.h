// gtest_lite: a tiny header-only stand-in for <gtest/gtest.h>.
//
// It exists only so the PaperCraft C++ suites (ofx/papercraft_ofx_test.cc and
// ofx/papercraft_kernel_test.cc) can run on machines without GoogleTest, via
// `ofx/run_cpp_tests.sh --lite`. CI and Bazel keep using real GoogleTest;
// do not depend on anything here that real GoogleTest lacks.
//
// Supported subset:
//   - TEST(Suite, Name) and TEST_F(Fixture, Name) with ::testing::Test
//     fixtures (virtual SetUp / TearDown; the body is skipped when SetUp
//     hits a fatal failure, TearDown always runs).
//   - EXPECT_* / ASSERT_* for TRUE, FALSE, EQ, NE, LT, LE, GT, GE, NEAR,
//     FLOAT_EQ, DOUBLE_EQ (4 ULPs, like GoogleTest), STREQ, each accepting
//     `<< message` streaming.
//   - ASSERT_NO_FATAL_FAILURE(statement) and SCOPED_TRACE(message).
// ASSERT_* returns from the enclosing void function, like GoogleTest.
// main() lives in gtest_lite_main.cc; the binary exits non-zero if any test
// fails.
#ifndef PAPERCRAFT_OFX_TESTING_GTEST_LITE_GTEST_GTEST_H_
#define PAPERCRAFT_OFX_TESTING_GTEST_LITE_GTEST_GTEST_H_

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace gtest_lite {

// One registered test: "Suite.Name" plus a function that runs it.
struct TestCase {
  std::string name;
  void (*run)();
};

// Every test registered by TEST / TEST_F, in definition order.
inline std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}

// Failure counters of the test currently running.
struct State {
  int failures = 0;        // All failed checks.
  int fatal_failures = 0;  // ASSERT_* failures only.
};

// Mutable state of the running test (tests run serially on one thread).
inline State& Current() {
  static State state;
  return state;
}

// Active SCOPED_TRACE messages, outermost first.
inline std::vector<std::string>& TraceStack() {
  static std::vector<std::string> stack;
  return stack;
}

// Static-initialization hook used by TEST / TEST_F to register a test.
struct Registrar {
  // Appends `name` / `run` to the registry.
  Registrar(const char* name, void (*run)()) {
    Registry().push_back({name, run});
  }
};

// True when `std::ostream << T` compiles.
template <typename T, typename = void>
struct IsStreamable : std::false_type {};
template <typename T>
struct IsStreamable<T, std::void_t<decltype(std::declval<std::ostream&>()
                                            << std::declval<const T&>())>>
    : std::true_type {};

// Renders a value for failure messages; unprintable types become a marker.
template <typename T>
std::string Str(const T& value) {
  if constexpr (std::is_same_v<T, std::nullptr_t>) {
    return "nullptr";
  } else if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (IsStreamable<T>::value) {
    std::ostringstream os;
    os.precision(std::numeric_limits<double>::max_digits10);
    os << value;
    return os.str();
  } else {
    return "<unprintable value>";
  }
}

// Outcome of one check: `ok`, else a description of the mismatch.
struct Result {
  bool ok = true;
  std::string msg;
  explicit operator bool() const { return ok; }
};

// EXPECT_TRUE / EXPECT_FALSE: `value` must equal `expected`.
inline Result Bool(bool value, bool expected, const char* expr) {
  if (value == expected) {
    return {};
  }
  return {false, std::string("Value of: ") + expr + "\n  Actual: " +
                     Str(value) + "\nExpected: " + Str(expected)};
}

// Binary comparison `f(a, b)` (==, !=, <, ...), described with `op`.
template <typename F, typename A, typename B>
Result Compare(F f, const A& a, const B& b, const char* expr_a,
               const char* expr_b, const char* op) {
  if (f(a, b)) {
    return {};
  }
  return {false, std::string("Expected: (") + expr_a + ") " + op + " (" +
                     expr_b + "), actual: " + Str(a) + " vs " + Str(b)};
}

// C-string equality; two nullptrs are equal, one nullptr is not.
inline Result StrEq(const char* a, const char* b, const char* expr_a,
                    const char* expr_b) {
  const bool ok = (a == nullptr || b == nullptr) ? a == b
                                                 : std::strcmp(a, b) == 0;
  if (ok) {
    return {};
  }
  return {false, std::string("Expected equality of: ") + expr_a + " and " +
                     expr_b + "\n  \"" + (a ? a : "(null)") + "\" vs \"" +
                     (b ? b : "(null)") + "\""};
}

// |a - b| <= tolerance.
inline Result Near(double a, double b, double tolerance, const char* expr_a,
                   const char* expr_b) {
  if (std::fabs(a - b) <= tolerance) {
    return {};
  }
  return {false, std::string("The difference between ") + expr_a + " and " +
                     expr_b + " is " + Str(std::fabs(a - b)) +
                     ", which exceeds " + Str(tolerance) + " (" + Str(a) +
                     " vs " + Str(b) + ")"};
}

// Maps an IEEE float/double bit pattern onto a monotonic unsigned scale so
// the difference of two mapped values is their distance in ULPs.
template <typename Bits>
Bits BiasedBits(Bits bits) {
  constexpr Bits kSign = Bits{1} << (sizeof(Bits) * 8 - 1);
  return (bits & kSign) != 0 ? ~bits + 1 : kSign | bits;
}

// GoogleTest's EXPECT_FLOAT_EQ / EXPECT_DOUBLE_EQ: within 4 ULPs, NaN never
// equal.
template <typename Float>
Result UlpEq(Float a, Float b, const char* expr_a, const char* expr_b) {
  using Bits = std::conditional_t<sizeof(Float) == 4, std::uint32_t,
                                  std::uint64_t>;
  bool ok = false;
  if (!std::isnan(a) && !std::isnan(b)) {
    Bits ba = 0;
    Bits bb = 0;
    std::memcpy(&ba, &a, sizeof(a));
    std::memcpy(&bb, &b, sizeof(b));
    const Bits ua = BiasedBits(ba);
    const Bits ub = BiasedBits(bb);
    ok = (ua >= ub ? ua - ub : ub - ua) <= 4;
  }
  if (ok) {
    return {};
  }
  return {false, std::string("Expected equality (4 ULPs) of: ") + expr_a +
                     " and " + expr_b + ", actual: " + Str(a) + " vs " +
                     Str(b)};
}

// Collects the optional `<< ...` user message of a failed check.
struct Message {
  std::ostringstream os;
  // Appends any streamable value.
  template <typename T>
  Message& operator<<(const T& value) {
    os << Str(value);
    return *this;
  }
};

// Reports one failed check; assigned the user Message by the check macros.
struct FailureReporter {
  const char* file;
  int line;
  std::string msg;
  bool fatal;
  // Prints the failure with active traces and bumps the counters.
  void operator=(const Message& message) const {
    std::fprintf(stderr, "%s:%d: Failure\n%s\n", file, line, msg.c_str());
    const std::string user = message.os.str();
    if (!user.empty()) {
      std::fprintf(stderr, "%s\n", user.c_str());
    }
    const auto& traces = TraceStack();
    if (!traces.empty()) {
      std::fprintf(stderr, "Google Test trace:\n");
      for (auto it = traces.rbegin(); it != traces.rend(); ++it) {
        std::fprintf(stderr, "%s\n", it->c_str());
      }
    }
    ++Current().failures;
    if (fatal) {
      ++Current().fatal_failures;
    }
  }
};

// RAII guard behind SCOPED_TRACE.
class ScopedTrace {
 public:
  // Pushes "file:line: message" for failures reported in this scope.
  template <typename T>
  ScopedTrace(const char* file, int line, const T& message) {
    TraceStack().push_back(std::string(file) + ":" + std::to_string(line) +
                           ": " + Str(message));
  }
  ~ScopedTrace() { TraceStack().pop_back(); }
  ScopedTrace(const ScopedTrace&) = delete;
  ScopedTrace& operator=(const ScopedTrace&) = delete;
};

// Runs `fn` and reports whether it added no fatal failure.
inline bool RunWithoutNewFatalFailure(const std::function<void()>& fn) {
  const int before = Current().fatal_failures;
  fn();
  return Current().fatal_failures == before;
}

// Base class of every test (::testing::Test).
class Test {
 public:
  virtual ~Test() = default;
  // Runs SetUp, the body unless SetUp failed fatally, then TearDown.
  void Run() {
    if (RunWithoutNewFatalFailure([this] { SetUp(); })) {
      TestBody();
    }
    TearDown();
  }

 protected:
  // Per-test fixture hooks.
  virtual void SetUp() {}
  virtual void TearDown() {}

 private:
  virtual void TestBody() = 0;
};

// Instantiates test class T and runs it.
template <typename T>
void RunTest() {
  T test;
  test.Run();
}

// Runs every registered test whose name contains `filter` (all when empty)
// and prints a GoogleTest-like log. Returns 0 if all pass, else 1.
inline int RunAllTests(const std::string& filter) {
  int ran = 0;
  std::vector<std::string> failed;
  for (const TestCase& test : Registry()) {
    if (!filter.empty() && test.name.find(filter) == std::string::npos) {
      continue;
    }
    Current() = State{};
    std::printf("[ RUN      ] %s\n", test.name.c_str());
    std::fflush(stdout);
    test.run();
    ++ran;
    if (Current().failures > 0) {
      failed.push_back(test.name);
      std::printf("[  FAILED  ] %s\n", test.name.c_str());
    } else {
      std::printf("[       OK ] %s\n", test.name.c_str());
    }
    std::fflush(stdout);
  }
  std::printf("[==========] %d tests ran.\n", ran);
  std::printf("[  PASSED  ] %d tests.\n",
              ran - static_cast<int>(failed.size()));
  for (const std::string& name : failed) {
    std::printf("[  FAILED  ] %s\n", name.c_str());
  }
  if (ran == 0) {
    std::printf("No tests matched filter \"%s\".\n", filter.c_str());
    return 1;
  }
  return failed.empty() ? 0 : 1;
}

}  // namespace gtest_lite

namespace testing {
using Test = ::gtest_lite::Test;
}  // namespace testing

// The switch/if-else shape makes each check a single statement that is safe
// in unbraced if/else and still accepts a trailing `<< message`.
#define GTEST_LITE_AMBIGUOUS_ELSE_BLOCKER_ \
  switch (0)                               \
  case 0:                                  \
  default:

#define GTEST_LITE_CHECK_(result_expr, on_failure, fatal)                  \
  GTEST_LITE_AMBIGUOUS_ELSE_BLOCKER_                                       \
  if (const ::gtest_lite::Result gtest_lite_r_ = (result_expr)) {          \
  } else                                                                   \
    on_failure ::gtest_lite::FailureReporter{__FILE__, __LINE__,           \
                                             gtest_lite_r_.msg, fatal} =   \
        ::gtest_lite::Message()

#define GTEST_LITE_EXPECT_(result_expr) \
  GTEST_LITE_CHECK_(result_expr, , false)
#define GTEST_LITE_ASSERT_(result_expr) \
  GTEST_LITE_CHECK_(result_expr, return, true)

#define GTEST_LITE_CMP_(a, b, op)                                       \
  ::gtest_lite::Compare(                                                \
      [](const auto& x, const auto& y) { return x op y; }, (a), (b), #a, \
      #b, #op)

#define EXPECT_TRUE(c) \
  GTEST_LITE_EXPECT_(::gtest_lite::Bool(static_cast<bool>(c), true, #c))
#define EXPECT_FALSE(c) \
  GTEST_LITE_EXPECT_(::gtest_lite::Bool(static_cast<bool>(c), false, #c))
#define ASSERT_TRUE(c) \
  GTEST_LITE_ASSERT_(::gtest_lite::Bool(static_cast<bool>(c), true, #c))
#define ASSERT_FALSE(c) \
  GTEST_LITE_ASSERT_(::gtest_lite::Bool(static_cast<bool>(c), false, #c))

#define EXPECT_EQ(a, b) GTEST_LITE_EXPECT_(GTEST_LITE_CMP_(a, b, ==))
#define EXPECT_NE(a, b) GTEST_LITE_EXPECT_(GTEST_LITE_CMP_(a, b, !=))
#define EXPECT_LT(a, b) GTEST_LITE_EXPECT_(GTEST_LITE_CMP_(a, b, <))
#define EXPECT_LE(a, b) GTEST_LITE_EXPECT_(GTEST_LITE_CMP_(a, b, <=))
#define EXPECT_GT(a, b) GTEST_LITE_EXPECT_(GTEST_LITE_CMP_(a, b, >))
#define EXPECT_GE(a, b) GTEST_LITE_EXPECT_(GTEST_LITE_CMP_(a, b, >=))
#define ASSERT_EQ(a, b) GTEST_LITE_ASSERT_(GTEST_LITE_CMP_(a, b, ==))
#define ASSERT_NE(a, b) GTEST_LITE_ASSERT_(GTEST_LITE_CMP_(a, b, !=))
#define ASSERT_LT(a, b) GTEST_LITE_ASSERT_(GTEST_LITE_CMP_(a, b, <))
#define ASSERT_LE(a, b) GTEST_LITE_ASSERT_(GTEST_LITE_CMP_(a, b, <=))
#define ASSERT_GT(a, b) GTEST_LITE_ASSERT_(GTEST_LITE_CMP_(a, b, >))
#define ASSERT_GE(a, b) GTEST_LITE_ASSERT_(GTEST_LITE_CMP_(a, b, >=))

#define EXPECT_NEAR(a, b, tol) \
  GTEST_LITE_EXPECT_(::gtest_lite::Near((a), (b), (tol), #a, #b))
#define ASSERT_NEAR(a, b, tol) \
  GTEST_LITE_ASSERT_(::gtest_lite::Near((a), (b), (tol), #a, #b))
#define EXPECT_FLOAT_EQ(a, b) \
  GTEST_LITE_EXPECT_(::gtest_lite::UlpEq<float>((a), (b), #a, #b))
#define ASSERT_FLOAT_EQ(a, b) \
  GTEST_LITE_ASSERT_(::gtest_lite::UlpEq<float>((a), (b), #a, #b))
#define EXPECT_DOUBLE_EQ(a, b) \
  GTEST_LITE_EXPECT_(::gtest_lite::UlpEq<double>((a), (b), #a, #b))
#define ASSERT_DOUBLE_EQ(a, b) \
  GTEST_LITE_ASSERT_(::gtest_lite::UlpEq<double>((a), (b), #a, #b))
#define EXPECT_STREQ(a, b) \
  GTEST_LITE_EXPECT_(::gtest_lite::StrEq((a), (b), #a, #b))
#define ASSERT_STREQ(a, b) \
  GTEST_LITE_ASSERT_(::gtest_lite::StrEq((a), (b), #a, #b))

#define ASSERT_NO_FATAL_FAILURE(statement)                              \
  GTEST_LITE_ASSERT_((::gtest_lite::Result{                             \
      ::gtest_lite::RunWithoutNewFatalFailure([&]() { statement; }),    \
      "Expected: " #statement " doesn't generate new fatal failures"}))

#define GTEST_LITE_CONCAT_INNER_(a, b) a##b
#define GTEST_LITE_CONCAT_(a, b) GTEST_LITE_CONCAT_INNER_(a, b)
#define SCOPED_TRACE(message)                                     \
  const ::gtest_lite::ScopedTrace GTEST_LITE_CONCAT_(             \
      gtest_lite_trace_, __LINE__)(__FILE__, __LINE__, (message))

#define GTEST_LITE_TEST_(suite, name, parent)                             \
  class suite##_##name##_Test : public parent {                           \
   private:                                                               \
    void TestBody() override;                                             \
  };                                                                      \
  static const ::gtest_lite::Registrar suite##_##name##_registrar_(       \
      #suite "." #name, &::gtest_lite::RunTest<suite##_##name##_Test>);   \
  void suite##_##name##_Test::TestBody()

#define TEST(suite, name) GTEST_LITE_TEST_(suite, name, ::testing::Test)
#define TEST_F(fixture, name) GTEST_LITE_TEST_(fixture, name, fixture)

#endif  // PAPERCRAFT_OFX_TESTING_GTEST_LITE_GTEST_GTEST_H_
