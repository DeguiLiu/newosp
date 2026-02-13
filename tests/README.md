# newosp Test Suite

Comprehensive test suite for newosp C++17 header-only embedded infrastructure library using Catch2 v3.5.2.

## Test Statistics

- Normal mode: 500 tests
- `-fno-exceptions` mode: 366 tests (Core Tests only)

## Test Organization

### Core Tests (No External Dependencies)

Core tests run in both normal and `-fno-exceptions` modes. These tests do not depend on sockpp.

| Test File | Module | Description |
|-----------|--------|-------------|
| `test_platform.cpp` | `platform.hpp` | Platform detection, OSP_ASSERT, compiler hints |
| `test_vocabulary.cpp` | `vocabulary.hpp` | expected, optional, FixedFunction, FixedVector, FixedString, ScopeGuard |
| `test_config.cpp` | `config.hpp` | INI configuration parser (inih integration) |
| `test_log.cpp` | `log.hpp` | Stderr logging macros and log levels |
| `test_timer.cpp` | `timer.hpp` | atime-cpp timer scheduling |
| `test_shell.cpp` | `shell.hpp` | Command registry and parsing (no telnet) |
| `test_mem_pool.cpp` | `mem_pool.hpp` | Stack-first memory pool allocator |
| `test_shutdown.cpp` | `shutdown.hpp` | pipe(2) wakeup and LIFO callback registration |
| `test_bus.cpp` | `bus.hpp` | Lock-free MPSC message bus with exponential backoff |
| `test_node.cpp` | `node.hpp` | Lightweight Pub/Sub communication node |
| `test_lifecycle_node.cpp` | `lifecycle_node.hpp` | LifecycleNode state machine (Unconfigured → Inactive → Active → Finalized) |
| `test_worker_pool.cpp` | `worker_pool.hpp` | Worker thread pool with adaptive backoff |
| `test_executor.cpp` | `executor.hpp` | SingleThreadExecutor, StaticExecutor, PinnedExecutor |
| `test_hsm.cpp` | `hsm.hpp` | Hierarchical state machine |
| `test_bt.cpp` | `bt.hpp` | Lightweight behavior tree library |
| `test_semaphore.cpp` | `semaphore.hpp` | LightSemaphore, BinarySemaphore, PosixSemaphore |
| `test_shm_transport.cpp` | `shm_transport.hpp` | SharedMemorySegment, ShmRingBuffer, ShmChannel with ARM weak memory ordering |
| `test_data_fusion.cpp` | `data_fusion.hpp` | Multi-message alignment and time synchronization |
| `test_qos.cpp` | `qos.hpp` | QoS profiles (Reliability, History, Deadline, Lifespan) |
| `test_app.cpp` | `app.hpp` | Application, Instance, MakeIID |
| `test_post.cpp` | `post.hpp` | AppRegistry, OspPost, OspSendAndWait |
| `test_io_poller.cpp` | `io_poller.hpp` | epoll event loop (IoPoller, PollResult, IoEvent) |
| `test_serial_transport.cpp` | `serial_transport.hpp` | Industrial-grade serial transport with CRC-CCITT, PTY testing |

### Network Tests (Requires sockpp)

Network tests require sockpp dependency and are excluded in `-fno-exceptions` mode.

| Test File | Module | Description |
|-----------|--------|-------------|
| `test_socket.cpp` | `socket.hpp` | SocketAddress, TcpSocket, UdpSocket, TcpListener |
| `test_connection.cpp` | `connection.hpp` | Connection pool management |
| `test_transport.cpp` | `transport.hpp` | Endpoint, FrameCodec, Serializer, TcpTransport, NetworkNode |
| `test_transport_factory.cpp` | `transport_factory.hpp` | Transport factory pattern |
| `test_discovery.cpp` | `discovery.hpp` | StaticDiscovery and MulticastDiscovery |
| `test_service.cpp` | `service.hpp` | RPC Service and Client |
| `test_node_manager.cpp` | `node_manager.hpp` | Node management and heartbeat |
| `test_node_manager_hsm.cpp` | `node_manager_hsm.hpp` | HSM-based node manager |
| `test_net.cpp` | `net.hpp` | sockpp integration layer |
| `test_integration.cpp` | Multiple | Cross-module integration tests (Node, Bus, WorkerPool, Timer, HSM, BT, ConnectionPool, DataFusion, Executor) |

## Building Tests

Enable test building with CMake option:

```bash
cd /tmp/newosp
mkdir build && cd build
cmake -DOSP_BUILD_TESTS=ON ..
cmake --build .
```

### Build with -fno-exceptions

To build in exception-free mode (Core Tests only):

```bash
cmake -DOSP_BUILD_TESTS=ON -DOSP_NO_EXCEPTIONS=ON ..
cmake --build .
```

## Running Tests

### Run All Tests

```bash
ctest --output-on-failure
```

### Run Specific Test Pattern

```bash
./tests/osp_tests "[bus]"
./tests/osp_tests "[node]"
./tests/osp_tests "AsyncBus*"
```

### Run with Verbose Output

```bash
./tests/osp_tests --success
```

### List All Test Cases

```bash
./tests/osp_tests --list-tests
```

## Sanitizer Validation

Tests are validated with multiple sanitizers to ensure memory safety and thread safety.

### AddressSanitizer (ASan)

Detects memory errors (buffer overflows, use-after-free, memory leaks):

```bash
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
      -DOSP_BUILD_TESTS=ON ..
cmake --build .
ctest --output-on-failure
```

### ThreadSanitizer (TSan)

Detects data races and thread synchronization issues:

```bash
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread" \
      -DOSP_BUILD_TESTS=ON ..
cmake --build .
ctest --output-on-failure
```

### UndefinedBehaviorSanitizer (UBSan)

Detects undefined behavior (integer overflow, null pointer dereference, etc.):

```bash
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-omit-frame-pointer" \
      -DOSP_BUILD_TESTS=ON ..
cmake --build .
ctest --output-on-failure
```

## CI Integration

Tests run automatically on GitHub Actions for:

- Ubuntu (GCC/Clang) - Debug and Release
- macOS (Clang) - Debug and Release
- ASan, TSan, UBSan validation
- `-fno-exceptions` build verification

## Test Coverage

Tests cover:

- Basic functionality and API contracts
- Edge cases and error handling
- Multi-threaded scenarios and race conditions
- Memory safety (stack/heap allocation patterns)
- Lock-free algorithm correctness
- Cross-module integration scenarios
- Platform-specific behavior (ARM weak memory ordering, epoll, PTY)

## Contributing

When adding new modules:

1. Create `test_<module>.cpp` with file header documenting purpose
2. Add test cases covering normal operation, edge cases, and error paths
3. Verify with sanitizers (ASan, TSan, UBSan)
4. Ensure tests pass in both normal and `-fno-exceptions` modes (if applicable)
5. Update this README with test file description
