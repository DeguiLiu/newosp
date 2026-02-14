# Benchmarks

Performance benchmark programs for newosp transport and messaging subsystems.

Test results are documented in [docs/benchmark_report_zh.md](../../docs/benchmark_report_zh.md).

## Files

| File | Description | Key Metrics |
|------|-------------|-------------|
| `serial_benchmark.cpp` | Serial transport throughput over PTY pairs | FPS, KB/s for 8B-512B payloads, with/without ACK |
| `transport_benchmark.cpp` | TCP loopback and ShmRingBuffer SPSC throughput | FPS, MB/s for 64B-4096B payloads |
| `bus_payload_benchmark.cpp` | AsyncBus intra-process throughput with large payloads | M msgs/s, MB/s for 64B-8192B payloads |

## Build & Run

```bash
# Serial benchmark (requires Linux PTY)
g++ -O3 -std=c++17 -I include -o serial_benchmark \
    examples/benchmarks/serial_benchmark.cpp -lutil -lpthread

# Transport benchmark (TCP max frame 2048B)
g++ -O3 -std=c++17 -I include -DOSP_TRANSPORT_MAX_FRAME_SIZE=2048U \
    -o transport_benchmark examples/benchmarks/transport_benchmark.cpp -lpthread -lrt

# Bus payload benchmark (BatchSize=4096 for large envelope)
g++ -O3 -std=c++17 -I include -DOSP_BUS_BATCH_SIZE=4096U \
    -o bus_payload_benchmark examples/benchmarks/bus_payload_benchmark.cpp -lpthread
```
