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
 * @file async_log.hpp
 * @brief Async logging backend with per-thread SPSC ring buffers.
 *
 * Extends log.hpp with a non-blocking async path for DEBUG/INFO/WARN logs.
 * ERROR/FATAL always use the sync fprintf path (crash-safe).
 *
 * Architecture:
 *   Thread 0 --> SpscRingbuffer<LogEntry, N> --+
 *   Thread 1 --> SpscRingbuffer<LogEntry, N> --+--> WriterThread --> Sink
 *   Thread N --> SpscRingbuffer<LogEntry, N> --+     (round-robin poll)
 *
 * Each logging thread gets its own SPSC buffer (wait-free push, zero
 * contention). A single background writer thread polls all active buffers
 * with AdaptiveBackoff (spin -> yield -> sleep).
 *
 * When the async path is not started, all macros fall back to the sync
 * LogWrite() from log.hpp transparently.
 *
 * Compile-time configuration:
 *   OSP_ASYNC_LOG_QUEUE_DEPTH  -- per-thread SPSC depth (default 256)
 *   OSP_ASYNC_LOG_MAX_THREADS  -- max concurrent logging threads (default 8)
 *
 * Compatible with -fno-exceptions -fno-rtti, C++14/17.
 */

#ifndef OSP_ASYNC_LOG_HPP_
#define OSP_ASYNC_LOG_HPP_

#include "osp/log.hpp"
#include "osp/platform.hpp"
#include "osp/spsc_ringbuffer.hpp"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#endif

// ============================================================================
// Compile-Time Configuration
// ============================================================================

#ifndef OSP_ASYNC_LOG_QUEUE_DEPTH
#define OSP_ASYNC_LOG_QUEUE_DEPTH 256U
#endif

#ifndef OSP_ASYNC_LOG_MAX_THREADS
#define OSP_ASYNC_LOG_MAX_THREADS 8U
#endif

/// @brief Interval (seconds) between drop-stats reports to stderr (0=disable).
#ifndef OSP_ASYNC_LOG_DROP_REPORT_INTERVAL_S
#define OSP_ASYNC_LOG_DROP_REPORT_INTERVAL_S 10U
#endif

namespace osp {
namespace log {

// ============================================================================
// LogEntry -- Pre-formatted log entry for async queue
// ============================================================================

/**
 * @brief Fixed-size log entry stored in per-thread SPSC ring buffer.
 *
 * All fields are trivially copyable (POD) so SpscRingbuffer can use the
 * memcpy batch path. Size: 320 bytes = 5 cache lines.
 */
struct LogEntry {
  uint64_t timestamp_ns;      ///<  8B  Monotonic timestamp (CLOCK_MONOTONIC).
  uint32_t wallclock_sec;     ///<  4B  Wall-clock seconds since epoch.
  uint16_t wallclock_ms;      ///<  2B  Wall-clock milliseconds.
  Level level;                ///<  1B  Severity level.
  uint8_t padding0;           ///<  1B  Alignment padding.
  char category[16];          ///< 16B  Null-terminated category string.
  char message[256];          ///<256B  Pre-formatted message (vsnprintf).
  char file[24];              ///< 24B  Source file basename (truncated).
  uint32_t line;              ///<  4B  Source line number.
  uint32_t thread_id;         ///<  4B  Cached thread ID.
};

static_assert(sizeof(LogEntry) == 320, "LogEntry size must be 320 bytes");
static_assert(std::is_trivially_copyable<LogEntry>::value,
              "LogEntry must be trivially copyable for SPSC memcpy path");

// ============================================================================
// LogSinkFn -- Output sink function pointer
// ============================================================================

/**
 * @brief Sink function signature for batch log output.
 *
 * @param entries  Array of log entries.
 * @param count    Number of entries in the array.
 * @param context  User-provided context pointer.
 */
using LogSinkFn = void (*)(const LogEntry* entries, uint32_t count,
                           void* context);

// ============================================================================
// AsyncLogConfig
// ============================================================================

/**
 * @brief Configuration for the async logging backend.
 */
struct AsyncLogConfig {
  LogSinkFn sink = nullptr;       ///< Output sink (nullptr = stderr).
  void* sink_context = nullptr;   ///< Context passed to sink.
};

// ============================================================================
// AsyncLogStats
// ============================================================================

/**
 * @brief Runtime statistics for the async logging backend.
 */
struct AsyncLogStats {
  uint64_t entries_written;   ///< Total entries written to sink.
  uint64_t entries_dropped;   ///< Total entries dropped (queue full).
  uint64_t sync_fallbacks;    ///< Entries that fell back to sync write.
};

// ============================================================================
// Internal Detail
// ============================================================================

namespace detail {

// --- Helpers ----------------------------------------------------------------

/// @brief Get current thread ID (cached via thread_local).
inline uint32_t GetCachedThreadId() noexcept {
  static thread_local uint32_t tl_tid = 0;
  if (tl_tid == 0) {
#if defined(__linux__)
    tl_tid = static_cast<uint32_t>(::syscall(SYS_gettid));
#else
    tl_tid = static_cast<uint32_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
  }
  return tl_tid;
}

/// @brief Copy src to dst with truncation. Always null-terminates.
inline void SafeStrCopy(char* dst, size_t dst_size,
                        const char* src) noexcept {
  if (src == nullptr || dst_size == 0) {
    if (dst_size > 0) dst[0] = '\0';
    return;
  }
  size_t len = std::strlen(src);
  if (len >= dst_size) {
    len = dst_size - 1;
  }
  std::memcpy(dst, src, len);
  dst[len] = '\0';
}

/// @brief Extract basename from a full file path.
inline const char* Basename(const char* path) noexcept {
  if (path == nullptr) return "?";
  const char* slash = std::strrchr(path, '/');
  return (slash != nullptr) ? slash + 1 : path;
}

/// @brief Capture wall-clock seconds and milliseconds.
inline void CaptureWallclock(uint32_t& sec, uint16_t& ms) noexcept {
#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  sec = static_cast<uint32_t>(ts.tv_sec);
  ms = static_cast<uint16_t>(ts.tv_nsec / 1000000L);
#else
  sec = static_cast<uint32_t>(std::time(nullptr));
  ms = 0;
#endif
}

/// @brief Format a LogEntry's wallclock into "YYYY-MM-DD HH:MM:SS.mmm".
inline void FormatEntryTimestamp(const LogEntry& e, char* buf,
                                size_t bufsz) noexcept {
  time_t t = static_cast<time_t>(e.wallclock_sec);
#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)
  struct tm tm_local;
  localtime_r(&t, &tm_local);
  (void)snprintf(buf, bufsz,
                 "%04d-%02d-%02d %02d:%02d:%02d.%03u",
                 tm_local.tm_year + 1900, tm_local.tm_mon + 1,
                 tm_local.tm_mday, tm_local.tm_hour,
                 tm_local.tm_min, tm_local.tm_sec,
                 static_cast<unsigned>(e.wallclock_ms));
#else
  struct std::tm* tm_local = std::localtime(&t);
  if (tm_local != nullptr) {
    (void)snprintf(buf, bufsz,
                   "%04d-%02d-%02d %02d:%02d:%02d.%03u",
                   tm_local->tm_year + 1900, tm_local->tm_mon + 1,
                   tm_local->tm_mday, tm_local->tm_hour,
                   tm_local->tm_min, tm_local->tm_sec,
                   static_cast<unsigned>(e.wallclock_ms));
  } else {
    (void)snprintf(buf, bufsz, "0000-00-00 00:00:00.000");
  }
#endif
}

// --- AdaptiveBackoff (inline copy to avoid worker_pool.hpp dependency) ------

/// @brief Three-phase backoff: spin -> yield -> sleep.
/// Copied from worker_pool.hpp to avoid pulling in AsyncBus dependency chain.
class LogBackoff {
 public:
  void Reset() noexcept { spin_count_ = 0U; }

  void Wait() noexcept {
    if (spin_count_ < kSpinLimit) {
      const uint32_t iters = 1U << spin_count_;
      for (uint32_t i = 0U; i < iters; ++i) {
        CpuRelax();
      }
      ++spin_count_;
    } else if (spin_count_ < kSpinLimit + kYieldLimit) {
      std::this_thread::yield();
      ++spin_count_;
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }

 private:
  static constexpr uint32_t kSpinLimit = 6U;
  static constexpr uint32_t kYieldLimit = 4U;

  static void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
  }

  uint32_t spin_count_{0U};
};

// --- Per-Thread SPSC Buffer -------------------------------------------------

/// @brief Per-thread log buffer wrapping a SPSC ring buffer.
struct LogBuffer {
  SpscRingbuffer<LogEntry, OSP_ASYNC_LOG_QUEUE_DEPTH> queue;
  std::atomic<bool> active{false};
  uint32_t thread_id{0};
};

// --- Thread-Local Cleanup ---------------------------------------------------

/// @brief RAII guard that releases the LogBuffer slot on thread exit.
struct TlsCleanup {
  LogBuffer* buf{nullptr};
  ~TlsCleanup() {
    if (buf != nullptr) {
      buf->active.store(false, std::memory_order_release);
    }
  }
};

// --- Default Stderr Sink ----------------------------------------------------

/// @brief Default sink: write log entries to stderr.
inline void StderrSink(const LogEntry* entries, uint32_t count,
                       void* /*ctx*/) noexcept {
  for (uint32_t i = 0; i < count; ++i) {
    const auto& e = entries[i];
    char ts_buf[64];
    FormatEntryTimestamp(e, ts_buf, sizeof(ts_buf));

#ifdef NDEBUG
    (void)std::fprintf(stderr, "[%s] [%s] [%s] %s\n",
                       ts_buf,
                       log::detail::LevelTag(e.level),
                       e.category,
                       e.message);
#else
    (void)std::fprintf(stderr, "[%s] [%s] [%s] %s (%s:%u)\n",
                       ts_buf,
                       log::detail::LevelTag(e.level),
                       e.category,
                       e.message,
                       e.file,
                       e.line);
#endif
  }
}

// --- AsyncLogContext Singleton -----------------------------------------------

/// @brief Global async logging state (Meyer's singleton).
struct AsyncLogContext {
  LogBuffer buffers[OSP_ASYNC_LOG_MAX_THREADS];

  std::atomic<bool> running{false};
  std::atomic<bool> shutdown{false};

  LogSinkFn sink{nullptr};
  void* sink_context{nullptr};

  std::thread writer_thread;

  // Statistics (separate cache lines to avoid false sharing).
  alignas(kCacheLineSize) std::atomic<uint64_t> entries_written{0};
  alignas(kCacheLineSize) std::atomic<uint64_t> entries_dropped{0};
  alignas(kCacheLineSize) std::atomic<uint64_t> sync_fallbacks{0};

  static AsyncLogContext& Instance() noexcept {
    static AsyncLogContext ctx;
    return ctx;
  }

 private:
  AsyncLogContext() noexcept = default;
};

// --- Thread Registration ----------------------------------------------------

/// @brief Acquire a LogBuffer slot for the current thread.
/// @return Pointer to the thread's LogBuffer, or nullptr if all slots taken.
inline LogBuffer* AcquireLogBuffer() noexcept {
  static thread_local TlsCleanup tls_cleanup;
  if (tls_cleanup.buf != nullptr) {
    return tls_cleanup.buf;  // Fast path: already registered.
  }

  auto& ctx = AsyncLogContext::Instance();
  for (uint32_t i = 0; i < OSP_ASYNC_LOG_MAX_THREADS; ++i) {
    bool expected = false;
    if (ctx.buffers[i].active.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      ctx.buffers[i].thread_id = GetCachedThreadId();
      tls_cleanup.buf = &ctx.buffers[i];
      return tls_cleanup.buf;
    }
  }
  return nullptr;  // All slots taken.
}

// --- Writer Thread ----------------------------------------------------------

/// @brief Background writer loop. Polls all active SPSC buffers.
inline void WriterLoop() noexcept {
  auto& ctx = AsyncLogContext::Instance();
  LogBackoff backoff;

  // Resolve sink: default to StderrSink.
  LogSinkFn sink = ctx.sink ? ctx.sink : StderrSink;
  void* sink_ctx = ctx.sink_context;

  static constexpr uint32_t kBatchSize = 32U;
  LogEntry batch[kBatchSize];

  // Drop-stats reporting state.
  uint64_t last_reported_drops = 0;
  auto next_report_time = std::chrono::steady_clock::now() +
      std::chrono::seconds(OSP_ASYNC_LOG_DROP_REPORT_INTERVAL_S);

  while (!ctx.shutdown.load(std::memory_order_acquire)) {
    uint32_t total_popped = 0;

    for (uint32_t i = 0; i < OSP_ASYNC_LOG_MAX_THREADS; ++i) {
      if (!ctx.buffers[i].active.load(std::memory_order_acquire) &&
          ctx.buffers[i].queue.IsEmpty()) {
        continue;
      }

      size_t n = ctx.buffers[i].queue.PopBatch(batch, kBatchSize);
      if (n > 0) {
        sink(batch, static_cast<uint32_t>(n), sink_ctx);
        total_popped += static_cast<uint32_t>(n);
        ctx.entries_written.fetch_add(static_cast<uint64_t>(n),
                                      std::memory_order_relaxed);
      }
    }

    if (total_popped > 0) {
      backoff.Reset();
    } else {
      backoff.Wait();
    }

    // --- Periodic drop-stats report (sync ERROR to stderr) ---
    if (OSP_ASYNC_LOG_DROP_REPORT_INTERVAL_S > 0) {
      auto now = std::chrono::steady_clock::now();
      if (now >= next_report_time) {
        uint64_t cur_drops = ctx.entries_dropped.load(
            std::memory_order_relaxed);
        uint64_t cur_written = ctx.entries_written.load(
            std::memory_order_relaxed);
        uint64_t new_drops = cur_drops - last_reported_drops;
        if (new_drops > 0) {
          // Report via sync stderr (always visible, crash-safe).
          (void)std::fprintf(stderr,
              "[AsyncLog] WARN: %" PRIu64 " entries dropped in last %us"
              " (total: written=%" PRIu64 " dropped=%" PRIu64
              " fallbacks=%" PRIu64 ")\n",
              new_drops, OSP_ASYNC_LOG_DROP_REPORT_INTERVAL_S,
              cur_written, cur_drops,
              ctx.sync_fallbacks.load(std::memory_order_relaxed));
        }
        last_reported_drops = cur_drops;
        next_report_time = now +
            std::chrono::seconds(OSP_ASYNC_LOG_DROP_REPORT_INTERVAL_S);
      }
    }
  }

  // Final drain: multiple rounds until all buffers empty.
  for (uint32_t round = 0; round < 10; ++round) {
    uint32_t drained = 0;
    for (uint32_t i = 0; i < OSP_ASYNC_LOG_MAX_THREADS; ++i) {
      size_t n = ctx.buffers[i].queue.PopBatch(batch, kBatchSize);
      if (n > 0) {
        sink(batch, static_cast<uint32_t>(n), sink_ctx);
        drained += static_cast<uint32_t>(n);
        ctx.entries_written.fetch_add(static_cast<uint64_t>(n),
                                      std::memory_order_relaxed);
      }
    }
    if (drained == 0) break;
  }

  // Final drop-stats report on shutdown.
  {
    uint64_t final_drops = ctx.entries_dropped.load(
        std::memory_order_relaxed);
    uint64_t unreported = final_drops - last_reported_drops;
    if (unreported > 0) {
      (void)std::fprintf(stderr,
          "[AsyncLog] WARN: %" PRIu64 " entries dropped since last report"
          " (total: written=%" PRIu64 " dropped=%" PRIu64
          " fallbacks=%" PRIu64 ")\n",
          unreported,
          ctx.entries_written.load(std::memory_order_relaxed),
          final_drops,
          ctx.sync_fallbacks.load(std::memory_order_relaxed));
    }
  }
}

}  // namespace detail

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Start the async logging backend.
 *
 * Spawns the writer thread. Idempotent (safe to call multiple times).
 */
inline void StartAsync(const AsyncLogConfig& config = {}) noexcept {
  auto& ctx = detail::AsyncLogContext::Instance();
  if (ctx.running.load(std::memory_order_acquire)) {
    return;  // Already running.
  }

  ctx.sink = config.sink;
  ctx.sink_context = config.sink_context;
  ctx.shutdown.store(false, std::memory_order_release);
  ctx.running.store(true, std::memory_order_release);

  ctx.writer_thread = std::thread(detail::WriterLoop);
}

/**
 * @brief Stop the async logging backend. Drains remaining entries.
 */
inline void StopAsync() noexcept {
  auto& ctx = detail::AsyncLogContext::Instance();
  if (!ctx.running.load(std::memory_order_acquire)) {
    return;
  }

  ctx.shutdown.store(true, std::memory_order_release);
  if (ctx.writer_thread.joinable()) {
    ctx.writer_thread.join();
  }
  ctx.running.store(false, std::memory_order_release);
}

/**
 * @brief Check if async logging is currently active.
 */
inline bool IsAsyncEnabled() noexcept {
  return detail::AsyncLogContext::Instance().running.load(
      std::memory_order_acquire);
}

/**
 * @brief Set the output sink. Can be called before StartAsync().
 */
inline void SetSink(LogSinkFn fn, void* context = nullptr) noexcept {
  auto& ctx = detail::AsyncLogContext::Instance();
  ctx.sink = fn;
  ctx.sink_context = context;
}

/**
 * @brief Get async logging statistics.
 */
inline AsyncLogStats GetAsyncStats() noexcept {
  auto& ctx = detail::AsyncLogContext::Instance();
  AsyncLogStats stats;
  stats.entries_written = ctx.entries_written.load(std::memory_order_relaxed);
  stats.entries_dropped = ctx.entries_dropped.load(std::memory_order_relaxed);
  stats.sync_fallbacks = ctx.sync_fallbacks.load(std::memory_order_relaxed);
  return stats;
}

/**
 * @brief Reset async logging statistics counters.
 */
inline void ResetAsyncStats() noexcept {
  auto& ctx = detail::AsyncLogContext::Instance();
  ctx.entries_written.store(0, std::memory_order_relaxed);
  ctx.entries_dropped.store(0, std::memory_order_relaxed);
  ctx.sync_fallbacks.store(0, std::memory_order_relaxed);
}

// ============================================================================
// AsyncLogWrite -- Core async log write function
// ============================================================================

/**
 * @brief Route log entry to async or sync path based on severity.
 *
 * - ERROR/FATAL: always sync (crash-safe, fprintf + fflush).
 * - DEBUG/INFO/WARN: async via per-thread SPSC ring buffer.
 * - Falls back to sync if async is not started or no buffer available.
 * - Drops silently (increments counter) if the SPSC queue is full.
 */
inline void AsyncLogWrite(Level level, const char* category, const char* file,
                          int line, const char* fmt, ...) noexcept {
  // --- Runtime level gate ---
  if (static_cast<uint8_t>(level) <
      static_cast<uint8_t>(detail::LogLevelRef())) {
    return;
  }

  // --- ERROR/FATAL: always sync (crash-safe) ---
  if (static_cast<uint8_t>(level) >= static_cast<uint8_t>(Level::kError)) {
    va_list args;
    va_start(args, fmt);
    LogWriteVa(level, category, file, line, fmt, args);
    va_end(args);
    return;
  }

  auto& ctx = detail::AsyncLogContext::Instance();

  // --- Async not started: fallback to sync ---
  if (!ctx.running.load(std::memory_order_acquire)) {
    va_list args;
    va_start(args, fmt);
    LogWriteVa(level, category, file, line, fmt, args);
    va_end(args);
    return;
  }

  // --- Acquire per-thread SPSC buffer ---
  detail::LogBuffer* buf = detail::AcquireLogBuffer();
  if (buf == nullptr) {
    // All thread slots taken: fallback to sync.
    ctx.sync_fallbacks.fetch_add(1, std::memory_order_relaxed);
    va_list args;
    va_start(args, fmt);
    LogWriteVa(level, category, file, line, fmt, args);
    va_end(args);
    return;
  }

  // --- Build LogEntry on caller stack ---
  LogEntry entry;
  entry.timestamp_ns = SteadyNowNs();
  detail::CaptureWallclock(entry.wallclock_sec, entry.wallclock_ms);
  entry.level = level;
  entry.padding0 = 0;
  entry.thread_id = buf->thread_id;
  entry.line = static_cast<uint32_t>(line);

  detail::SafeStrCopy(entry.category, sizeof(entry.category), category);
  detail::SafeStrCopy(entry.file, sizeof(entry.file),
                      detail::Basename(file));

  va_list args;
  va_start(args, fmt);
  (void)vsnprintf(entry.message, sizeof(entry.message), fmt, args);
  va_end(args);

  // --- Push to per-thread SPSC (wait-free) ---
  if (!buf->queue.Push(entry)) {
    ctx.entries_dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace log
}  // namespace osp

// ============================================================================
// Macro Redefinition (optional: override log.hpp macros for async routing)
// ============================================================================

// Undef the sync macros from log.hpp for DEBUG/INFO/WARN only.
// ERROR and FATAL remain sync (crash-safe).
#undef OSP_LOG_DEBUG
#undef OSP_LOG_INFO
#undef OSP_LOG_WARN

#define OSP_LOG_DEBUG(cat, fmt, ...)                                         \
  do {                                                                      \
    if (OSP_LOG_MIN_LEVEL <= 0) {                                           \
      ::osp::log::AsyncLogWrite(::osp::log::Level::kDebug, cat,            \
                                __FILE__, __LINE__, fmt, ##__VA_ARGS__);    \
    }                                                                       \
  } while (0)

#define OSP_LOG_INFO(cat, fmt, ...)                                         \
  do {                                                                      \
    if (OSP_LOG_MIN_LEVEL <= 1) {                                           \
      ::osp::log::AsyncLogWrite(::osp::log::Level::kInfo, cat,             \
                                __FILE__, __LINE__, fmt, ##__VA_ARGS__);    \
    }                                                                       \
  } while (0)

#define OSP_LOG_WARN(cat, fmt, ...)                                         \
  do {                                                                      \
    if (OSP_LOG_MIN_LEVEL <= 2) {                                           \
      ::osp::log::AsyncLogWrite(::osp::log::Level::kWarn, cat,             \
                                __FILE__, __LINE__, fmt, ##__VA_ARGS__);    \
    }                                                                       \
  } while (0)

#endif  // OSP_ASYNC_LOG_HPP_
