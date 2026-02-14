/**
 * MIT License
 *
 * Copyright (c) 2024 liudegui
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file log.hpp
 * @brief Header-only lightweight logging system for OSP-CPP.
 *
 * Provides printf-style logging with levels, categories, timestamps, and
 * compile-time filtering. Thread-safe via POSIX fprintf. Inspired by
 * NanoLog's simplicity, but fully self-contained and header-only.
 *
 * Compatible with -fno-exceptions -fno-rtti, C++14/17.
 *
 * Compile-time log level control:
 *   Define OSP_LOG_MIN_LEVEL before including this header to filter out
 *   log calls at compile time. Values: 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR,
 *   4=FATAL, 5=OFF.
 *
 * Usage:
 *   #include "osp/log.hpp"
 *
 *   OSP_LOG_INFO("MyModule", "started with %d items", count);
 *   OSP_LOG_ERROR("Net", "connection failed: %s", strerror(errno));
 */

#ifndef OSP_LOG_HPP_
#define OSP_LOG_HPP_

#include "osp/platform.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)
#include <time.h>  // clock_gettime, localtime_r
#endif

namespace osp {
namespace log {

// ============================================================================
// Log Level
// ============================================================================

/**
 * @brief Severity levels for log messages.
 *
 * Ordered from least to most severe. kOff disables all logging.
 */
enum class Level : uint8_t {
  kDebug = 0,
  kInfo  = 1,
  kWarn  = 2,
  kError = 3,
  kFatal = 4,
  kOff   = 5
};

// ============================================================================
// Internal Detail
// ============================================================================

namespace detail {

/**
 * @brief Returns a mutable reference to the global runtime log level.
 *
 * Uses function-local static for header-only inline safety (C++14+).
 * Default: kDebug in debug builds, kInfo in release builds.
 */
inline Level& LogLevelRef() noexcept {
  static Level level =
#ifdef NDEBUG
      Level::kInfo;
#else
      Level::kDebug;
#endif
  return level;
}

/**
 * @brief Returns a mutable reference to the global initialized flag.
 */
inline bool& InitializedRef() noexcept {
  static bool init = false;
  return init;
}

/**
 * @brief Returns a short string tag for the given log level.
 */
inline const char* LevelTag(Level level) noexcept {
  switch (level) {
    case Level::kDebug: return "DEBUG";
    case Level::kInfo:  return "INFO ";
    case Level::kWarn:  return "WARN ";
    case Level::kError: return "ERROR";
    case Level::kFatal: return "FATAL";
    default:            return "?????";
  }
}

/**
 * @brief Formats the current local time into buf as "YYYY-MM-DD HH:MM:SS.mmm".
 *
 * @param buf   Output buffer, must be at least 24 bytes.
 * @param bufsz Size of buf.
 */
inline void FormatTimestamp(char* buf, size_t bufsz) noexcept {
#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);

  struct tm tm_local;
  localtime_r(&ts.tv_sec, &tm_local);

  int ms = static_cast<int>(ts.tv_nsec / 1000000L);

  (void)snprintf(buf, bufsz,
                 "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                 tm_local.tm_year + 1900,
                 tm_local.tm_mon + 1,
                 tm_local.tm_mday,
                 tm_local.tm_hour,
                 tm_local.tm_min,
                 tm_local.tm_sec,
                 ms);
#else
  // Fallback: standard C time (second-resolution only)
  std::time_t now = std::time(nullptr);
  struct std::tm* tm_local = std::localtime(&now);
  if (tm_local != nullptr) {
    (void)snprintf(buf, bufsz,
                   "%04d-%02d-%02d %02d:%02d:%02d.000",
                   tm_local->tm_year + 1900,
                   tm_local->tm_mon + 1,
                   tm_local->tm_mday,
                   tm_local->tm_hour,
                   tm_local->tm_min,
                   tm_local->tm_sec);
  } else {
    (void)snprintf(buf, bufsz, "0000-00-00 00:00:00.000");
  }
#endif
}

}  // namespace detail

// ============================================================================
// Runtime Level Control
// ============================================================================

/**
 * @brief Gets the current runtime minimum log level.
 */
inline Level GetLevel() noexcept {
  return detail::LogLevelRef();
}

/**
 * @brief Sets the runtime minimum log level.
 *
 * Messages below this level are suppressed at runtime.
 * For compile-time filtering, define OSP_LOG_MIN_LEVEL instead.
 */
inline void SetLevel(Level level) noexcept {
  detail::LogLevelRef() = level;
}

// ============================================================================
// Init / Shutdown
// ============================================================================

/**
 * @brief Initializes the logging subsystem.
 *
 * Currently sets the initialized flag. The conf_path parameter is reserved
 * for future config file support and is unused.
 *
 * @param conf_path Reserved, currently unused. Pass nullptr.
 */
inline void Init(const char* conf_path = nullptr) noexcept {
  (void)conf_path;
  detail::InitializedRef() = true;
}

/**
 * @brief Shuts down the logging subsystem and flushes stderr.
 */
inline void Shutdown() noexcept {
  std::fflush(stderr);
  detail::InitializedRef() = false;
}

/**
 * @brief Returns whether Init() has been called.
 */
inline bool IsInitialized() noexcept {
  return detail::InitializedRef();
}

// ============================================================================
// Core Log Write
// ============================================================================

/**
 * @brief Writes a log message to stderr.
 *
 * Performs runtime level check, formats a timestamped message, and writes
 * it atomically to stderr (POSIX guarantees fprintf thread-safety).
 *
 * Output format:
 *   [2024-01-01 12:00:00.123] [LEVEL] [category] message
 * In debug builds, file:line is appended:
 *   [2024-01-01 12:00:00.123] [LEVEL] [category] message (file.cpp:42)
 *
 * For Level::kFatal, abort() is called after the message is written.
 *
 * @param level    Severity level of this message.
 * @param category Module/subsystem name (e.g. "Net", "Timer").
 * @param file     Source file name (__FILE__).
 * @param line     Source line number (__LINE__).
 * @param fmt      Printf-style format string.
 * @param ...      Format arguments.
 */
inline void LogWrite(Level level, const char* category, const char* file,
                     int line, const char* fmt, ...) noexcept {
  // --- Runtime level gate ---
  if (static_cast<uint8_t>(level) < static_cast<uint8_t>(detail::LogLevelRef())) {
    return;
  }

  // --- Timestamp ---
  char ts_buf[64];
  detail::FormatTimestamp(ts_buf, sizeof(ts_buf));

  // --- Format user message ---
  char msg_buf[512];
  va_list args;
  va_start(args, fmt);
  (void)vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);

  // --- Output ---
#ifdef NDEBUG
  // Release: no file:line
  (void)std::fprintf(stderr, "[%s] [%s] [%s] %s\n",
                     ts_buf,
                     detail::LevelTag(level),
                     category ? category : "-",
                     msg_buf);
#else
  // Debug: include file:line
  // Strip path prefix - show only filename
  const char* basename = file;
  if (file != nullptr) {
    const char* slash = std::strrchr(file, '/');
    if (slash != nullptr) {
      basename = slash + 1;
    }
  }
  (void)std::fprintf(stderr, "[%s] [%s] [%s] %s (%s:%d)\n",
                     ts_buf,
                     detail::LevelTag(level),
                     category ? category : "-",
                     msg_buf,
                     basename ? basename : "?",
                     line);
#endif

  // --- Fatal: flush and abort ---
  if (level == Level::kFatal) {
    std::fflush(stderr);
    std::abort();
  }
}

}  // namespace log
}  // namespace osp

// ============================================================================
// Compile-Time Log Level Filter
// ============================================================================

/**
 * Define OSP_LOG_MIN_LEVEL before including log.hpp to set the compile-time
 * minimum level. Log calls below this level are compiled out entirely.
 *
 *   0 = kDebug (default in debug builds)
 *   1 = kInfo  (default in release builds)
 *   2 = kWarn
 *   3 = kError
 *   4 = kFatal
 *   5 = kOff   (disables all logging)
 */
#ifndef OSP_LOG_MIN_LEVEL
  #ifdef NDEBUG
    #define OSP_LOG_MIN_LEVEL 1
  #else
    #define OSP_LOG_MIN_LEVEL 0
  #endif
#endif

// ============================================================================
// Logging Macros
// ============================================================================

/**
 * @brief Log a DEBUG message with category.
 * @param cat  Category string (e.g. "Net", "Timer").
 * @param fmt  Printf-style format string.
 * @param ...  Format arguments.
 */
#define OSP_LOG_DEBUG(cat, fmt, ...)                                         \
  do {                                                                      \
    if (OSP_LOG_MIN_LEVEL <= 0) {                                           \
      ::osp::log::LogWrite(::osp::log::Level::kDebug, cat,                 \
                           __FILE__, __LINE__, fmt, ##__VA_ARGS__);         \
    }                                                                       \
  } while (0)

/**
 * @brief Log an INFO message with category.
 */
#define OSP_LOG_INFO(cat, fmt, ...)                                         \
  do {                                                                      \
    if (OSP_LOG_MIN_LEVEL <= 1) {                                           \
      ::osp::log::LogWrite(::osp::log::Level::kInfo, cat,                  \
                           __FILE__, __LINE__, fmt, ##__VA_ARGS__);         \
    }                                                                       \
  } while (0)

/**
 * @brief Log a WARN message with category.
 */
#define OSP_LOG_WARN(cat, fmt, ...)                                         \
  do {                                                                      \
    if (OSP_LOG_MIN_LEVEL <= 2) {                                           \
      ::osp::log::LogWrite(::osp::log::Level::kWarn, cat,                  \
                           __FILE__, __LINE__, fmt, ##__VA_ARGS__);         \
    }                                                                       \
  } while (0)

/**
 * @brief Log an ERROR message with category.
 */
#define OSP_LOG_ERROR(cat, fmt, ...)                                        \
  do {                                                                      \
    if (OSP_LOG_MIN_LEVEL <= 3) {                                           \
      ::osp::log::LogWrite(::osp::log::Level::kError, cat,                 \
                           __FILE__, __LINE__, fmt, ##__VA_ARGS__);         \
    }                                                                       \
  } while (0)

/**
 * @brief Log a FATAL message with category, then abort.
 */
#define OSP_LOG_FATAL(cat, fmt, ...)                                        \
  do {                                                                      \
    if (OSP_LOG_MIN_LEVEL <= 4) {                                           \
      ::osp::log::LogWrite(::osp::log::Level::kFatal, cat,                 \
                           __FILE__, __LINE__, fmt, ##__VA_ARGS__);         \
    }                                                                       \
  } while (0)

#endif  // OSP_LOG_HPP_
