# System Monitor Demo

Demonstrates `osp::SystemMonitor` with YAML configuration via `osp::Config`.

## Features

- YAML-driven threshold and sampling configuration
- CPU utilization (jiffies delta), memory, disk, temperature monitoring
- State-change alert callbacks (only fire on threshold crossing)
- FaultReporter integration pattern (commented example)
- Fallback to defaults when config file is missing

## Build

```bash
cmake -DOSP_CONFIG_YAML=ON -DOSP_BUILD_EXAMPLES=ON ..
make osp_system_monitor_demo
```

## Run

```bash
cd build/examples/system_monitor
./osp_system_monitor_demo                        # uses system_monitor.yaml
./osp_system_monitor_demo /path/to/custom.yaml   # custom config
```

## Configuration (system_monitor.yaml)

```yaml
thresholds:
  cpu_percent: 80
  cpu_temp_celsius: 80
  memory_percent: 85
  disk_percent: 90

disk_paths:
  path_0: /
  path_1: /tmp

sampling:
  interval_ms: 1000
  count: 5

logging:
  level: info
  alert_to_stderr: true
```

## Files

| File | Description |
|------|-------------|
| main.cpp | Demo entry point |
| system_monitor.yaml | Default configuration |
| CMakeLists.txt | Build config (copies YAML to build dir) |
