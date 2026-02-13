# newosp

Modern C++14 embedded infrastructure library for ARM-Linux, extracted and modernized from the OSP (Open Streaming Platform) codebase.

## Features

- **Zero global state**: All state encapsulated in objects (RAII)
- **Stack-first allocation**: Fixed-capacity containers, zero heap in hot paths
- **`-fno-exceptions -fno-rtti` compatible**: Designed for embedded ARM-Linux
- **Type-safe error handling**: `expected<V,E>` and `optional<T>` vocabulary types
- **Header-only**: Single CMake INTERFACE library

## Modules

| Module | Description |
|--------|-------------|
| `platform.hpp` | Platform detection, compiler hints, `OSP_ASSERT` |
| `vocabulary.hpp` | `expected`, `optional`, `FixedVector`, `FixedString`, `function_ref`, `not_null`, `NewType`, `ScopeGuard` |
| `config.hpp` | INI / JSON / YAML configuration parsing |
| `log.hpp` | Logging macros (zlog backend, stderr fallback) |
| `timer.hpp` | Timer task scheduler (based on atime-cpp) |
| `shell.hpp` | Remote debug shell (telnet + command registration) |
| `mem_pool.hpp` | Fixed-size memory pool with embedded free list |
| `shutdown.hpp` | Signal-safe graceful shutdown with LIFO callbacks |

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Options

| Option | Default | Description |
|--------|---------|-------------|
| `OSP_BUILD_TESTS` | ON | Build test suite |
| `OSP_BUILD_EXAMPLES` | OFF | Build example programs |
| `OSP_CONFIG_INI` | ON | Enable INI config backend |
| `OSP_CONFIG_JSON` | OFF | Enable JSON config backend |
| `OSP_CONFIG_YAML` | OFF | Enable YAML config backend |

## Requirements

- CMake >= 3.14
- C++14 compiler (GCC >= 5, Clang >= 3.4)
- POSIX (Linux / macOS)

## License

Apache-2.0
