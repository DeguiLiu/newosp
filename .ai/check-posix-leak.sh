#!/bin/bash
# .ai/check-posix-leak.sh -- Assert POSIX/GCC-only constructs in the Windows
# consumer header closure are reachable only on POSIX platforms.
#
# The Windows tray app consumes a fixed header closure (see tests/msvc_smoke.cpp).
# Every occurrence of unistd.h / pthread.h / sched.h / termios.h / sys/epoll.h /
# sys/socket.h / fork( in those headers must sit inside a platform #if
# (e.g. OSP_PLATFORM_LINUX, __GNUC__, __has_include, or the !OSP_PLATFORM_WINDOWS
# branch of a platform switch). Otherwise MSVC would pull in a POSIX header.
#
# Usage: .ai/check-posix-leak.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
INCLUDE_DIR="$PROJECT_ROOT/include/osp"

# Consumer closure (17 headers): the modules the Windows consumer includes plus
# their intra-osp dependencies. Keep in sync with tests/msvc_smoke.cpp.
CLOSURE=(
  async_log breaker config hsm hsm_table inicpp log mem_pool opt platform
  semaphore spsc_ringbuffer thread timer toml vocabulary watchdog
)

# grep -E pattern for the forbidden POSIX/GCC-only constructs.
TOKENS='unistd\.h|pthread\.h|sched\.h|termios\.h|sys/epoll\.h|sys/socket\.h|(^|[^[:alnum:]_])fork[[:space:]]*\('

FAILURES=0
echo "== POSIX-leak check: ${#CLOSURE[@]} consumer headers =="

for hdr in "${CLOSURE[@]}"; do
  file="$INCLUDE_DIR/$hdr.hpp"
  if [ ! -f "$file" ]; then
    echo "  WARN: missing header: $file" >&2
    continue
  fi

  # Walk each file tracking #if depth and branch. A construct is "guarded" when
  # some enclosing level guarantees it is unreachable on Windows:
  #   - a positive POSIX/compiler guard on the *then* branch
  #     (OSP_PLATFORM_LINUX/MACOS, __linux__, __APPLE__, __GNUC__, __clang__,
  #      __has_include, or a negated OSP_PLATFORM_WINDOWS), or
  #   - the *else* branch of a Windows guard (i.e. the !Windows path).
  # Critically, the #else of an RT-Thread or compiler guard is NOT safe: it is
  # exactly the branch MSVC takes (this is the original thread.hpp leak).
  violations="$(awk -v tok="$TOKENS" '
    function classify(l) {
      if (l ~ /ifndef[[:space:]]+OSP_PLATFORM_WINDOWS/) return "posix"
      if (l ~ /![[:space:]]*(defined[[:space:]]*\([[:space:]]*)?OSP_PLATFORM_WINDOWS/) return "posix"
      if (l ~ /OSP_PLATFORM_WINDOWS|_WIN32|_MSC_VER/) return "windows"
      if (l ~ /OSP_PLATFORM_RTTHREAD/) return "rt"
      if (l ~ /OSP_PLATFORM_LINUX|OSP_PLATFORM_MACOS|__linux__|__APPLE__|__GNUC__|__clang__|__has_include|__has_feature/) return "posix"
      return "other"
    }
    /^[[:space:]]*#[[:space:]]*if/ {
      depth++
      kind[depth] = classify($0)
      branch[depth] = "then"
    }
    /^[[:space:]]*#[[:space:]]*elif/ {
      kind[depth] = classify($0)
      branch[depth] = "then"
    }
    /^[[:space:]]*#[[:space:]]*else/ {
      branch[depth] = "else"
    }
    /^[[:space:]]*#[[:space:]]*endif/ {
      kind[depth] = ""
      branch[depth] = ""
      if (depth > 0) { depth-- }
    }
    $0 ~ tok {
      ok = 0
      for (i = 1; i <= depth; i++) {
        if (kind[i] == "posix" && branch[i] == "then") { ok = 1 }
        if (kind[i] == "windows" && branch[i] == "else") { ok = 1 }
      }
      if (!ok) { printf "%d:%s\n", NR, $0 }
    }
  ' "$file")"

  if [ -n "$violations" ]; then
    echo "  FAIL: $hdr.hpp has unguarded POSIX construct(s):" >&2
    echo "$violations" | sed 's/^/    /' >&2
    FAILURES=$((FAILURES + 1))
  else
    echo "  PASS: $hdr.hpp"
  fi
done

echo ""
if [ "$FAILURES" -eq 0 ]; then
  echo "check-posix-leak: ALL PASS"
else
  echo "check-posix-leak: $FAILURES header(s) leak POSIX constructs" >&2
fi
exit "$FAILURES"
