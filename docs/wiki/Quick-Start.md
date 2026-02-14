# Quick Start

This guide helps you get started with newosp in minutes.

## System Requirements

- CMake >= 3.14
- C++17 compiler (GCC >= 7, Clang >= 5)
- Linux (ARM-Linux embedded platform recommended)
- Git

## Build from Source

Clone the repository and build:

```bash
git clone https://github.com/DeguiLiu/newosp.git
cd newosp
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOSP_BUILD_EXAMPLES=ON -DOSP_BUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## Integrate into Your Project

newosp is header-only. Add it to your CMake project:

### Option 1: FetchContent (Recommended)

```cmake
include(FetchContent)
FetchContent_Declare(
  osp
  GIT_REPOSITORY https://github.com/DeguiLiu/newosp.git
  GIT_TAG        v0.1.0
)
FetchContent_MakeAvailable(osp)

target_link_libraries(your_app PRIVATE osp)
```

### Option 2: Add as Subdirectory

```cmake
add_subdirectory(newosp)
target_link_libraries(your_app PRIVATE osp)
```

## Hello World Example

Create a simple pub-sub messaging application:

```cpp
#include "osp/bus.hpp"
#include "osp/node.hpp"
#include "osp/log.hpp"

#include <variant>

// Define message types
struct SensorData { float temperature; float humidity; };
struct MotorCmd   { uint32_t mode; float target; };
using Payload = std::variant<SensorData, MotorCmd>;

int main() {
    osp::log::Init();

    // Create a sensor node and subscribe to SensorData
    osp::Node<Payload> sensor("sensor", 1);
    sensor.Subscribe<SensorData>([](const SensorData& d, const auto&) {
        OSP_LOG_INFO("sensor", "temp=%.1f humidity=%.1f", d.temperature, d.humidity);
    });

    // Publish a message and process it
    sensor.Publish(SensorData{25.0f, 60.0f});
    sensor.SpinOnce();

    osp::log::Shutdown();
    return 0;
}
```

Compile and run:

```bash
g++ -std=c++17 -I newosp/include hello.cpp -o hello -pthread
./hello
```

Output:

```
[2026-02-14 14:30:00.123] [INFO] [sensor] temp=25.0 humidity=60.0
```

## CMake Options

Configure newosp build behavior:

| Option | Default | Description |
|--------|---------|-------------|
| `OSP_BUILD_TESTS` | ON | Build test suite (Catch2 v3.5.2) |
| `OSP_BUILD_EXAMPLES` | OFF | Build example programs |
| `OSP_CONFIG_INI` | ON | Enable INI config backend (inicpp) |
| `OSP_CONFIG_JSON` | OFF | Enable JSON config backend (nlohmann/json) |
| `OSP_CONFIG_YAML` | OFF | Enable YAML config backend (fkYAML) |
| `OSP_NO_EXCEPTIONS` | OFF | Disable exceptions (`-fno-exceptions`) |
| `OSP_WITH_SOCKPP` | ON | Enable sockpp network library (socket/transport) |

Example: Build with all config backends and no exceptions:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
    -DOSP_CONFIG_INI=ON \
    -DOSP_CONFIG_JSON=ON \
    -DOSP_CONFIG_YAML=ON \
    -DOSP_NO_EXCEPTIONS=ON
cmake --build build -j$(nproc)
```

## Run Examples

After building with `-DOSP_BUILD_EXAMPLES=ON`:

```bash
# Basic pub-sub messaging
./build/examples/basic_demo

# Serial communication with HSM + BT
./build/examples/serial_demo

# Industrial OTA firmware upgrade
./build/examples/osp_serial_ota_demo

# Realtime executor with lifecycle nodes
./build/examples/realtime_executor_demo

# Behavior tree patrol robot
./build/examples/bt_patrol_demo
```

## Next Steps

- Explore [[Examples]] for more use cases
- Read [[Architecture]] to understand the design
- Check [[API-Reference]] for detailed module documentation
- See [[Home]] for project overview

## Troubleshooting

### Build fails with "C++17 required"

Ensure your compiler supports C++17:

```bash
g++ --version  # GCC >= 7
clang++ --version  # Clang >= 5
```

### Link errors with sockpp

If you don't need network features, disable sockpp:

```bash
cmake -B build -DOSP_WITH_SOCKPP=OFF
```

### Tests fail on non-Linux platforms

newosp targets ARM-Linux embedded systems. macOS/Windows support is limited.
