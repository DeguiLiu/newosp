# newosp LLM Context Documentation

This directory contains LLM-optimized documentation files following the [llmstxt.org](https://llmstxt.org) standard.

## Files

### llms.txt (17 KB, 387 lines)
**Concise overview for quick LLM context loading**

- Project summary and key features
- Module overview organized by layer (Foundation → Core → Network → Application → Reliability)
- Brief API descriptions highlighting embedded-specific features
- Quick start example
- Build instructions
- Design principles and resource budget summary

**Use case:** Initial context loading, quick reference, project overview

### llms-full.txt (77 KB, 3134 lines)
**Complete API reference with detailed signatures and examples**

- Full API documentation for all 38 modules
- Class/struct definitions with complete method signatures
- Parameter descriptions and return types
- Thread safety guarantees per module
- Usage examples for each component
- Integration patterns (Watchdog + FaultCollector, full system startup)
- Compile-time configuration reference
- Thread safety summary table

**Use case:** Deep API exploration, implementation guidance, troubleshooting

## Structure

Both files follow a hierarchical organization:

1. **Foundation Layer** (8 modules)
   - platform.hpp, vocabulary.hpp, config.hpp, log.hpp, timer.hpp, shell.hpp, mem_pool.hpp, shutdown.hpp

2. **Core Communication Layer** (6 modules)
   - bus.hpp, node.hpp, worker_pool.hpp, spsc_ringbuffer.hpp, executor.hpp, semaphore.hpp, data_fusion.hpp

3. **State Machine & Behavior Tree** (2 modules)
   - hsm.hpp, bt.hpp

4. **Network & Transport Layer** (8 modules)
   - socket.hpp, io_poller.hpp, connection.hpp, transport.hpp, shm_transport.hpp, serial_transport.hpp, net.hpp, transport_factory.hpp

5. **Service & Discovery Layer** (6 modules)
   - discovery.hpp, service.hpp, node_manager.hpp, node_manager_hsm.hpp, service_hsm.hpp, discovery_hsm.hpp

6. **Application Layer** (4 modules)
   - app.hpp, post.hpp, qos.hpp, lifecycle_node.hpp

7. **Reliability Components** (3 modules)
   - watchdog.hpp, fault_collector.hpp, shell_commands.hpp

## Key Highlights

- **Zero-heap allocation:** Fixed-capacity containers, stack-first allocation
- **Lock-free/minimal-lock:** MPSC bus, SPSC ring buffer, SharedMutex
- **Embedded-friendly:** Compatible with `-fno-exceptions -fno-rtti`
- **Cache-friendly:** 64-byte cache line alignment, batch processing
- **Type-safe:** `expected<V,E>` error handling, `NewType<T,Tag>` strong typing
- **Thread-safe:** Explicit thread safety guarantees per module
- **Tested:** 788+ test cases, ASan/TSan/UBSan clean

## Usage in LLM Context

### For Code Generation
```
Load llms-full.txt → Get complete API signatures → Generate implementation
```

### For Architecture Discussion
```
Load llms.txt → Understand module relationships → Design system integration
```

### For Troubleshooting
```
Load llms-full.txt → Search thread safety section → Identify concurrency issues
```

## Maintenance

These files are manually generated from:
- `/tmp/newosp/docs/design_zh.md` (architecture and design decisions)
- `/tmp/newosp/include/osp/*.hpp` (API signatures and documentation comments)

**Regeneration:** Run the generation script when major API changes occur or new modules are added.

## Related Documentation

- **Design Document:** `/tmp/newosp/docs/design_zh.md` - Full Chinese architecture documentation
- **Benchmark Report:** `/tmp/newosp/docs/benchmark_report_zh.md` - Performance metrics
- **Examples:** `/tmp/newosp/examples/README.md` - 15+ demo applications
- **Tests:** `/tmp/newosp/tests/README.md` - Test coverage documentation

## License

MIT License - Copyright (c) 2024 liudegui

---

*Generated: 2026-02-14*
