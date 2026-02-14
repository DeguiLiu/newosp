# Project Instructions

## Target Platform

- Target: ARM-Linux embedded (GCC / Clang)
- MSVC is NOT a target platform for this project or GitHub CI
- Compile flags: `-fno-exceptions -fno-rtti` (optional, controlled by CMake option)

## Coding Standards

### Memory & Allocation
- Stack-first: use `FixedVector<T,N>`, `FixedString<N>`, `FixedFunction<Sig,Size>` over std containers
- Hot path forbids heap allocation (`new`, `malloc`, `std::vector`, `std::string`)
- RAII for all resources, no naked `new`/`delete`
- Placement new + `ScopeGuard` for non-default-constructible types in fixed storage

### Type Safety
- Fixed-width integers: `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t` from `<cstdint>`
- `expected<V,E>` for error handling (not exceptions), `optional<T>` for nullable
- `NewType<T, Tag>` for strong typing (e.g. `WatchdogSlotId`, `NodeId`)
- POD structs for cross-boundary data (diagnostics snapshots, IPC messages)

### Concurrency
- Lock-free MPSC (CAS-based) for bus, mutex only as fallback
- Wait-free SPSC for ring buffers, `acquire`/`release` semantics on ARM
- Batch pattern: accumulate in loop, single `store`/`fetch_add` after loop
- Collect-release-execute: gather data under lock, release lock, then invoke callbacks
- `ThreadHeartbeat::Beat()` in every thread's hot loop for watchdog integration

### Compile-Time Dispatch
- Template parameters > name hiding > CRTP > function pointer+ctx > virtual
- `if constexpr` for dual-signature callbacks (bool early-stop vs void traverse-all)
- Key parameters (queue depth, max count, buffer size) as template args, not runtime config

### Module Design
- Header-only: all implementation in `.hpp`, CMake `INTERFACE` library
- Bridge file pattern: separate header to connect modules without direct dependency
- No circular dependencies between layers (Foundation < Core < Network < Application)

### Error Patterns to Avoid
- `error()` is static factory, `get_error()` is getter -- don't confuse them
- `IsRunning()` only checks atomic flag, thread may be dead -- use Watchdog
- `friend` + `static_cast<Base*>` for protected access is illegal (CWG 1873) -- use public wrapper

## Testing

- Framework: Catch2 v3, one `test_<module>.cpp` per module
- Sanitizers: ASan + UBSan + TSan, all must pass
- TSan + `fork()` incompatible: skip fork tests with `#if defined(__SANITIZE_THREAD__)`
- Dual build: normal mode (all tests) + `-fno-exceptions` mode (core-only tests)

## AI Tooling

All AI-related configuration and tools are in `.ai/` directory. See `.ai/README.md` for details.

- Code formatting: `.ai/format.sh [--check]`
- Linting: `.ai/lint.sh [--fix]`
- Full quality check: `.ai/check.sh [--quick] [--sanitizer]`
- MCP documentation server: `.ai/server.py`
- API index for LLM context: `.ai/llms.txt` (concise) / `.ai/llms-full.txt` (full)
- clang-format config: `.ai/.clang-format`
- cpplint config: `.ai/CPPLINT.cfg`
