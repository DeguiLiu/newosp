/**
 * @file platform.hpp
 * @brief Platform detection, compiler hints, and assertion macros.
 *
 * Replaces the original oscbb.h conditional compilation and custom type macros.
 */

#ifndef OSP_PLATFORM_HPP_
#define OSP_PLATFORM_HPP_

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace osp {

// ============================================================================
// Platform Detection
// ============================================================================

#if defined(__linux__)
#define OSP_PLATFORM_LINUX 1
#elif defined(__APPLE__)
#define OSP_PLATFORM_MACOS 1
#elif defined(_WIN32)
#define OSP_PLATFORM_WINDOWS 1
#endif

// ============================================================================
// Architecture Detection
// ============================================================================

#if defined(__arm__) || defined(__aarch64__)
#define OSP_ARCH_ARM 1
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#define OSP_ARCH_X86 1
#endif

// ============================================================================
// Cache Line Size
// ============================================================================

static constexpr size_t kCacheLineSize = 64;

// ============================================================================
// Compiler Hints
// ============================================================================

#if defined(__GNUC__) || defined(__clang__)
#define OSP_LIKELY(x) __builtin_expect(!!(x), 1)
#define OSP_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define OSP_UNUSED __attribute__((unused))
#else
#define OSP_LIKELY(x) (x)
#define OSP_UNLIKELY(x) (x)
#define OSP_UNUSED
#endif

// ============================================================================
// Assert Macro
// ============================================================================

namespace detail {

/**
 * @brief Called when an assertion fails in debug mode.
 *
 * Prints the failed condition, file, and line to stderr, then aborts.
 */
inline void AssertFail(const char* cond, const char* file, int line) {
  (void)std::fprintf(stderr, "OSP_ASSERT failed: %s at %s:%d\n", cond, file, line);
  std::abort();
}

}  // namespace detail

#ifdef NDEBUG
#define OSP_ASSERT(cond) ((void)0)
#else
#define OSP_ASSERT(cond)                                                    \
  ((cond) ? ((void)0) : ::osp::detail::AssertFail(#cond, __FILE__, __LINE__))
#endif

// ============================================================================
// Macro Helpers
// ============================================================================

#define OSP_CONCAT_IMPL(a, b) a##b
#define OSP_CONCAT(a, b) OSP_CONCAT_IMPL(a, b)

}  // namespace osp

#endif  // OSP_PLATFORM_HPP_
