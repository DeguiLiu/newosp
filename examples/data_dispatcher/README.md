# Data Dispatcher Demo -- Unified Data Distribution

## Overview

This example demonstrates the unified `DataDispatcher<StorePolicy, NotifyPolicy>` API
across two deployment modes, using identical Alloc/Submit/GetReadable/Release operations:

1. **InProc Pipeline** (`pipeline_demo.cpp`): `DataDispatcher<InProcStore>` with 4-stage DAG
   (entry -> preprocess -> logging + fusion). Reference-counted blocks, automatic recycling.
2. **Cross-Process Zero-Copy** (`producer.cpp` + `consumer_*.cpp`): `DataDispatcher<ShmStore, ShmNotify>`
   over POSIX shared memory. Zero-copy data access via `GetReadable`, refcount managed across processes.
3. **Unified API Demo** (`unified_api_demo.cpp`): Template function proving Store-agnostic code --
   the same `Alloc/GetWritable/Submit` sequence works with any StorePolicy.

All modes share the same LiDAR frame format (`common.hpp`).

## Architecture

### Mode 1: InProc Pipeline (DataDispatcher<InProcStore>)

```
InProcDispatcher<16016, 32>
  +-- InProcStore (32 blocks x 16016B, lock-free CAS alloc/release)
  +-- Pipeline (static DAG, 4 stages)
  |
  |   entry (pass-through)
  |     |
  |   preprocess (intensity filter)
  |     |
  |     +-- logging (seq check + stats)
  |     +-- fusion  (bounding box)
  |
  +-- FaultCollector (timeout/backpressure reporting)
  +-- ScanTimeout (periodic stale block reclamation)
  +-- ForceCleanup (end-of-run block recovery)
```

### Mode 2: Cross-Process (DataDispatcher<ShmStore, ShmNotify>)

```
               ShmStore Pool (/osp_dd_pool)
         32 blocks x 16016B, CAS-based refcount
                        |
    +-------------------+-------------------+
    |                   |                   |
Producer            Consumer-Logging   Consumer-Fusion
DataDispatcher      DataDispatcher     DataDispatcher
<ShmStore,ShmNotify>  <ShmStore>         <ShmStore>
    |                   |                   |
    +-- ShmNotify ---> ShmSpmcByteChannel <--+
    |   (8B NotifyMsg: block_id + payload_size)
    |
Monitor (DataDispatcher<ShmStore> + DebugShell)
    +-- ConsumerSlot[8] (per-consumer holding_mask for crash recovery)
    |
Launcher (process manager)
```

Data flow:
1. Producer: `Alloc -> GetWritable -> FillFrame -> Submit(block_id, size, consumer_count)`
2. Submit sets refcount = consumer_count, ShmNotify pushes `{block_id, payload_size}` to SPMC channel
3. Consumer: receives NotifyMsg -> `GetReadable(block_id)` (zero-copy pointer) -> process -> `Release(block_id)`
4. Block recycled when refcount reaches 0 (last consumer Release)

### Unified API Surface

Both modes use the same DataDispatcher methods:

| Method | Description |
|--------|-------------|
| `Alloc()` | Allocate a free block (returns block_id) |
| `GetWritable(id)` | Get writable pointer to block data |
| `Submit(id, size)` | Submit block to pipeline / set refcount |
| `GetReadable(id)` | Get read-only pointer (zero-copy) |
| `Release(id)` | Decrement refcount, recycle when 0 |
| `ScanTimeout()` | Reclaim timed-out blocks |
| `ForceCleanup(pred, ctx)` | Force-release blocks matching predicate |
| `RegisterConsumer(pid)` | Register consumer process, returns slot index (ShmStore only) |
| `UnregisterConsumer(id)` | Unregister consumer (ShmStore only) |
| `TrackBlockHold(cid, bid)` | Mark block as held by consumer (bitmap) (ShmStore only) |
| `TrackBlockRelease(cid, bid)` | Clear block hold (bitmap) (ShmStore only) |
| `CleanupDeadConsumers()` | Reclaim blocks from crashed consumers (ShmStore only) |
| `FreeBlocks()` / `AllocBlocks()` | Pool occupancy queries |

## Component Dependencies

| Component | Header | Usage | Used By |
|-----------|--------|-------|---------|
| `DataDispatcher` | `data_dispatcher.hpp` | Unified block pool + pipeline | All |
| `InProcStore` | `data_dispatcher.hpp` | In-process block storage | PL, U |
| `ShmStore` | `data_dispatcher.hpp` | Cross-process shm block storage | P, CL, CF, M |
| `ShmNotify` | `data_dispatcher.hpp` | Submit notification callback | P |
| `ShmSpmcByteChannel` | `shm_transport.hpp` | SPMC notification channel | P, CL, CF, M |
| `StateMachine<Ctx, N>` | `hsm.hpp` | HSM lifecycle management | P, CL, CF |
| `TimerScheduler<4>` | `timer.hpp` | Periodic stats / ScanTimeout | P, CL, CF, PL |
| `ThreadWatchdog<4>` | `watchdog.hpp` | Thread watchdog | P, PL |
| `FaultCollector<4,16>` | `fault_collector.hpp` | Fault reporting | P, PL |
| `DebugShell` | `shell.hpp` | Telnet debug shell | M |
| `ShutdownManager` | `shutdown.hpp` | Signal-safe shutdown | All |
| `Subprocess` | `process.hpp` | Child process management | L |

PL=Pipeline, U=Unified, P=Producer, CL=Consumer-Logging, CF=Consumer-Fusion, M=Monitor, L=Launcher

## Data Format

```
+---------------------------------------------+
| LidarFrame Header (16 bytes)                |
|   uint32_t magic      = 0x4C494441 ('LIDA') |
|   uint32_t seq_num    = incremental          |
|   uint32_t point_count = 1000                |
|   uint32_t timestamp_ms                      |
+---------------------------------------------+
| LidarPoint[1000] (16000 bytes)              |
|   float x, y, z                             |
|   uint8_t intensity, ring, pad[2]           |
+---------------------------------------------+
```

Frame size: 16016 bytes

## Build and Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOSP_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)

# === Mode 1: InProc Pipeline ===
./build/examples/data_dispatcher/osp_dd_pipeline 200

# === Mode 2: Cross-Process (ShmStore) ===
# Option A: launcher (starts all processes)
./build/examples/data_dispatcher/osp_dd_launcher --frames 200

# Option B: manual startup
./build/examples/data_dispatcher/osp_dd_producer --frames 500 &
./build/examples/data_dispatcher/osp_dd_consumer_logging &
./build/examples/data_dispatcher/osp_dd_consumer_fusion &
./build/examples/data_dispatcher/osp_dd_monitor --port 9600 &
telnet localhost 9600

# === Unified API Demo ===
./build/examples/data_dispatcher/osp_dd_unified 50
```

## Shell Commands (Cross-Process Mode)

Connect via `telnet localhost 9600` to the monitor process:

| Command | Description |
|---------|-------------|
| `help` | List all commands |
| `dd_status` | Pool status (capacity, free/alloc blocks) |
| `dd_scan` | Run ScanTimeout on pool |
| `dd_blocks` | List non-free blocks with state |
| `dd_notify` | Notification channel status (consumers, readable bytes) |
| `dd_peek` | Read next notification and display frame header |

## Files

| File | Description | Mode |
|------|-------------|------|
| `common.hpp` | LiDAR frame format + pool/shm config + POSIX helpers | Shared |
| `pipeline_demo.cpp` | 4-stage DAG pipeline (InProcStore) | InProc |
| `unified_api_demo.cpp` | Store-agnostic API demonstration | InProc |
| `producer.cpp` | HSM-driven producer (ShmStore + ShmNotify) | Cross-Process |
| `consumer_logging.cpp` | HSM-driven logging consumer (ShmStore) | Cross-Process |
| `consumer_fusion.cpp` | HSM-driven fusion consumer (ShmStore) | Cross-Process |
| `monitor.cpp` | Pool inspector + DebugShell (ShmStore) | Cross-Process |
| `launcher.cpp` | Process manager (spawn/health/restart) | Cross-Process |

## Mode Comparison

| Dimension | InProc Pipeline | Cross-Process ShmStore |
|-----------|-----------------|------------------------|
| Data sharing | Refcount zero-copy (same address space) | Refcount zero-copy (shared memory) |
| Release | Last consumer Release recycles block | Last consumer Release recycles block |
| Pipeline | DAG (serial + fan-out) | Flat (independent consumers) |
| Backpressure | Pool threshold callback | Producer checks FreeBlocks |
| Timeout | Per-block deadline + ScanTimeout | Per-block deadline + ScanTimeout |
| Fault isolation | None (same process) | Process-level isolation |
| Notification | Pipeline auto-dispatch | ShmSpmcByteChannel (8B NotifyMsg) |
| Use case | Complex multi-stage processing | Distributed sensor data distribution |
| Crash recovery | N/A (same process) | ConsumerSlot holding_mask + CleanupDeadConsumers |
