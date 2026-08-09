#!/bin/bash
# .ai/check-rtthread-compat.sh -- Validate newosp's RT-Thread layer against a
# real RT-Thread source tree. Two tiers:
#   1. Compile check (required): instantiate the RT-Thread code paths against
#      real rt-thread headers with -Wall -Wextra -Werror. Catches API drift.
#   2. Simulator run (best-effort): build the bsp/simulator kernel (needs
#      scons) and run newosp behavioral smoke tests on the real kernel:
#      a) rtthread_sim_test.cpp  -- mutex/semaphore/thread scheduling
#      b) rtthread_sim_posix_test.cpp -- TCP loopback via the POSIX socket
#         backend (OSP_NET_BACKEND=0, direct to the host glibc socket stack)
#      c) examples/basic_demo.cpp -- bus/node examples on the real kernel
# Skips tier 2 (with a warning) if scons or the simulator BSP is missing.
#
# Note: tier 2 mutates the RT-Thread tree:
#   - rtconfig.py forces ASAN off; drivers/SConscript drops the bogus SDL2
#     link so the kernel links headless.
#   - rtconfig.h disables RT_USING_SAL / SAL_USING_* / RT_USING_LWIP / netdev
#     so the POSIX socket backend is *not* shadowed by RT-Thread's SAL strong
#     symbols (sal_socket etc.). RT-Thread's Linux-host simulator does not
#     support in-tree lwIP (readme lists lwIP only for the msvc build).
#   - cpu_port.c gets the scheduler-race hardening (lazy-switch deferrals),
#     but that only matters for multi-thread scheduling, not for tier 2 here.
#
# Usage: .ai/check-rtthread-compat.sh [rt-thread-src-dir]
#   RTTHREAD_SRC env var accepted; default /home/dgliu/rtthread_521/rt-thread-5.2.1
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
RT="${1:-${RTTHREAD_SRC:-/home/dgliu/rtthread_521/rt-thread-5.2.1}}"
CFG_DIR="$(mktemp -d)"
FAILURES=0

cleanup() {
  rm -rf "$CFG_DIR" \
     /tmp/osp_kernel.a /tmp/osp_kernel_clean.a \
     /tmp/osp_sim_test /tmp/osp_sim_test.o \
     /tmp/osp_sim_posix_test /tmp/osp_sim_posix.o \
     /tmp/basic_demo /tmp/basic_demo.o
}
trap cleanup EXIT

if [ ! -f "$RT/include/rtthread.h" ]; then
  echo "ERROR: RT-Thread source not found at: $RT" >&2
  exit 2
fi

# ----------------------------------------------------------------------------
# Tier 1: compile check against real headers (hermetic minimal rtconfig)
# ----------------------------------------------------------------------------
echo "== [1/2] compile check: headers from $RT =="
cat > "$CFG_DIR/rtconfig.h" <<'EOF'
#ifndef RT_CONFIG_H__
#define RT_CONFIG_H__

#define RT_NAME_MAX 16
#define RT_CPUS_NR 1
#define RT_ALIGN_SIZE 8
#define RT_THREAD_PRIORITY_32
#define RT_THREAD_PRIORITY_MAX 32
#define RT_TICK_PER_SECOND 1000

#define RT_USING_SEMAPHORE
#define RT_USING_MUTEX
#define RT_USING_HEAP
#define RT_USING_CONSOLE

#endif
EOF

if g++ -std=c++17 -Wall -Wextra -Werror -c -o "$CFG_DIR/check.o" \
     -I "$CFG_DIR" -I "$RT/include" -I "$PROJECT_ROOT/include" \
     -DOSP_PLATFORM_RTTHREAD=1 -DOSP_NET_BACKEND=2 \
     "$SCRIPT_DIR/rtthread_compat_check.cpp"; then
  echo "  PASS: RT-Thread layer compiles clean (-Wall -Wextra -Werror)"
else
  echo "  FAIL: compile check (API drift against $RT)" >&2
  FAILURES=$((FAILURES + 1))
fi

# ----------------------------------------------------------------------------
# Tier 2: simulator run (best-effort)
# ----------------------------------------------------------------------------
echo "== [2/2] simulator run (best-effort) =="
SIM="$RT/bsp/simulator"
if [ ! -d "$SIM" ]; then
  echo "  SKIP: no bsp/simulator in $RT"
elif ! command -v scons >/dev/null 2>&1; then
  echo "  SKIP: scons not installed (needed to build the simulator kernel)"
else
  # Patch RT-Thread simulator build quirks (idempotent).
  if ! grep -q "do NOT link SDL2" "$SIM/drivers/SConscript"; then
    python3 - "$SIM/drivers/SConscript" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace(
    "    src += ['sdl_fb.c']\nelse:\n    LIBS.append('SDL2')\n",
    "    src += ['sdl_fb.c']\n# else: do NOT link SDL2 -- sdl_fb.c is not compiled\n# (RT-Thread simulator BSP quirk; patched for headless builds).\n")
open(p, 'w').write(s)
PYEOF
  fi
  sed -i 's/^    ASAN = True/    ASAN = False/' "$SIM/rtconfig.py"

  # Disable SAL + lwIP + netdev so the POSIX socket backend is not shadowed by
  # RT-Thread's SAL strong symbols. (Linux-host simulator officially has no
  # in-tree lwIP support.) Idempotent: leave lines already commented alone.
  python3 - "$SIM/rtconfig.h" <<'PYEOF'
import re, sys
p = sys.argv[1]
s = open(p).read()
for macro in ('RT_USING_SAL', 'SAL_USING_POSIX', 'SAL_USING_LWIP',
              'RT_USING_LWIP', 'RT_USING_LWIP_LOCAL_VERSION', 'RT_USING_LWIP212',
              'RT_USING_NETDEV'):
    s = re.sub(r'(?m)^#define (' + re.escape(macro) + r')\s*$',
               r'//#undef \1', s)
open(p, 'w').write(s)
PYEOF

  (cd "$SIM" && scons -j"$(nproc)" >/dev/null)

  # Archive the kernel WITHOUT any stray SAL/lwIP objects that scons may leave
  # in build/ when a feature toggles off (scons does not prune them). Build a
  # fresh archive each time to avoid stale members masking a missing object.
  rm -f /tmp/osp_kernel_clean.a
  KERNOBJ="$(cd "$SIM" && find build -name '*.o' ! -name 'application.o' \
    ! -name '*sal*' ! -name 'net_sockets.o' ! -name 'sockets.o' ! -name 'af_inet*.o')"
  (cd "$SIM" && ar rcs /tmp/osp_kernel_clean.a $KERNOBJ)

  echo "  kernel objects: $(ar t /tmp/osp_kernel_clean.a | wc -l)"

  # --- (a) mutex/semaphore/thread scheduling smoke test --------------------
  if g++ -std=c++17 -g -o "$CFG_DIR/sim_test.o" -c \
       -I "$RT/include" -I "$SIM" -I "$RT/components/finsh" \
       -I "$PROJECT_ROOT/include" -DOSP_PLATFORM_RTTHREAD=1 -DOSP_NET_BACKEND=2 \
       -D_REENTRANT -D_LINUX -DHAVE_SYS_SIGNALS \
       "$SCRIPT_DIR/rtthread_sim_test.cpp" \
     && (cd "$SIM" && g++ -o /tmp/osp_sim_test "$CFG_DIR/sim_test.o" \
            -Wl,--whole-archive /tmp/osp_kernel_clean.a -Wl,--no-whole-archive \
            -pthread -T gcc_elf64.ld -lstdc++); then
    OUT="$(timeout 60 /tmp/osp_sim_test 2>&1 || true)"
    if echo "$OUT" | grep -q "RT-Thread test: PASS"; then
      echo "  PASS: scheduling smoke test (mutex/sem/thread) on real kernel"
    else
      echo "  FAIL: scheduling smoke test" >&2
      echo "$OUT" | grep -E "FAIL|PASS|test:" | head -5 || true
      FAILURES=$((FAILURES + 1))
    fi
  else
    echo "  FAIL: could not build/run the scheduling smoke test" >&2
    FAILURES=$((FAILURES + 1))
  fi

  # --- (b) POSIX-socket TCP loopback test (host stack via OSP_NET_BACKEND=0)
  if g++ -std=c++17 -g -o "$CFG_DIR/sim_posix.o" -c \
       -I "$RT/include" -I "$SIM" -I "$RT/components/finsh" \
       -I "$PROJECT_ROOT/include" -DOSP_PLATFORM_RTTHREAD=1 -DOSP_NET_BACKEND=0 \
       -D_REENTRANT -D_LINUX -DHAVE_SYS_SIGNALS \
       "$SCRIPT_DIR/rtthread_sim_posix_test.cpp" \
     && (cd "$SIM" && g++ -o /tmp/osp_sim_posix_test "$CFG_DIR/sim_posix.o" \
            -Wl,--whole-archive /tmp/osp_kernel_clean.a -Wl,--no-whole-archive \
            -pthread -T gcc_elf64.ld -lstdc++); then
    OUT="$(timeout 60 /tmp/osp_sim_posix_test 2>&1 || true)"
    if echo "$OUT" | grep -q "POSIX-socket test: PASS"; then
      echo "  PASS: POSIX-socket TCP loopback on real kernel"
    else
      echo "  FAIL: POSIX-socket test" >&2
      echo "$OUT" | grep -E "FAIL|PASS|test:" | head -5 || true
      FAILURES=$((FAILURES + 1))
    fi
  else
    echo "  FAIL: could not build/run the POSIX-socket test" >&2
    FAILURES=$((FAILURES + 1))
  fi

  # --- (c) examples/basic_demo.cpp on the real kernel ----------------------
  if [ -f "$PROJECT_ROOT/examples/basic_demo.cpp" ] \
     && g++ -std=c++17 -g -o "$CFG_DIR/basic_demo.o" -c \
          -I "$RT/include" -I "$SIM" -I "$RT/components/finsh" \
          -I "$PROJECT_ROOT/include" -DOSP_PLATFORM_RTTHREAD=1 -DOSP_NET_BACKEND=0 \
          -D_REENTRANT -D_LINUX -DHAVE_SYS_SIGNALS \
          "$PROJECT_ROOT/examples/basic_demo.cpp" \
     && (cd "$SIM" && g++ -o /tmp/basic_demo "$CFG_DIR/basic_demo.o" \
            -Wl,--whole-archive /tmp/osp_kernel_clean.a -Wl,--no-whole-archive \
            -pthread -T gcc_elf64.ld -lstdc++); then
    OUT="$(timeout 60 /tmp/basic_demo 2>&1 || true)"
    if echo "$OUT" | grep -qE "processed\s*:\s*[1-9]|SpinOnce"; then
      echo "  PASS: examples/basic_demo ran on real kernel"
    else
      echo "  FAIL: examples/basic_demo did not produce output" >&2
      echo "$OUT" | grep -E "processed|SpinOnce|error|Error" | head -5 || true
      FAILURES=$((FAILURES + 1))
    fi
  else
    echo "  FAIL: could not build/run examples/basic_demo" >&2
    FAILURES=$((FAILURES + 1))
  fi
fi

echo ""
if [ "$FAILURES" -eq 0 ]; then
  echo "check-rtthread-compat: ALL PASS"
else
  echo "check-rtthread-compat: $FAILURES FAILURE(S)" >&2
fi
exit "$FAILURES"
