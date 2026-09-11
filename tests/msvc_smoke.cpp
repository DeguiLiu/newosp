/**
 * @file msvc_smoke.cpp
 * @brief MSVC-cleanliness smoke TU for the Windows tray-app header closure.
 *
 * The consumer set is hsm/hsm_table/vocabulary/mem_pool/spsc_ringbuffer/breaker/
 * log/async_log/config/toml/semaphore/timer/watchdog plus thread.hpp. This TU
 * has no runtime behavior: its only job is to fail compilation if any header in
 * that closure pulls in a POSIX/GCC-only construct under MSVC. It is compiled as
 * an OBJECT library on every platform so GCC/Clang catch regressions too.
 */

#include "osp/async_log.hpp"
#include "osp/breaker.hpp"
#include "osp/config.hpp"
#include "osp/hsm.hpp"
#include "osp/hsm_table.hpp"
#include "osp/log.hpp"
#include "osp/mem_pool.hpp"
#include "osp/semaphore.hpp"
#include "osp/spsc_ringbuffer.hpp"
#include "osp/thread.hpp"
#include "osp/timer.hpp"
#include "osp/toml.hpp"
#include "osp/vocabulary.hpp"
#include "osp/watchdog.hpp"

// Every header must at least define a complete osp::Thread.
static_assert(sizeof(osp::Thread) > 0, "osp::Thread must be complete");
