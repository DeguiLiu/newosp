# newosp AI Tooling

This directory contains all AI-related configuration, tools, and documentation for the newosp project. These are optional and not required for normal library usage.

## Directory Structure

```
.ai/
├── .clang-format      # Code formatting rules (Google C++ Style, C++17)
├── CPPLINT.cfg        # Lint configuration (disabled checks documented inline)
├── format.sh          # Auto-format all C++ files
├── lint.sh            # Run cpplint on all C++ files
├── check.sh           # Full quality gate (format + lint + build + test)
├── server.py          # MCP documentation server for Claude Code / Cursor
├── llms.txt           # Concise API index (17KB, llmstxt.org format)
├── llms-full.txt      # Full API reference (77KB, all 38 modules)
└── README.md          # This file
```

## Code Quality Scripts

### Format (`format.sh`)

Format all C++ source files using clang-format with the project's style rules.

```bash
# Format all files in-place
.ai/format.sh

# Check only (dry-run, exit 1 if changes needed)
.ai/format.sh --check

# Format specific directory
.ai/format.sh include/osp/bus.hpp
```

Style: Google C++ Style Guide, C++17, 120 column limit. Config: `.ai/.clang-format`.

### Lint (`lint.sh`)

Run cpplint static analysis on all C++ files.

```bash
# Lint all files
.ai/lint.sh

# Lint + auto-fix whitespace issues
.ai/lint.sh --fix

# Lint specific directory
.ai/lint.sh include/
```

Config: `.ai/CPPLINT.cfg`. Uses `--config` flag, no symlink needed.

### Full Check (`check.sh`)

Run the complete quality gate: format check + lint + cmake build + ctest.

```bash
# Full check (format + lint + build + test)
.ai/check.sh

# Quick check (skip tests)
.ai/check.sh --quick

# With AddressSanitizer + UndefinedBehaviorSanitizer
.ai/check.sh --sanitizer
```

## MCP Documentation Server

Provides on-demand API documentation to AI coding agents via Model Context Protocol.

### Setup

```bash
# Install dependency (one-time)
pip install 'mcp[cli]'

# Register with Claude Code
claude mcp add newosp-docs -- python3 /path/to/newosp/.ai/server.py

# Or set newosp path via environment variable
NEWOSP_ROOT=/path/to/newosp python3 .ai/server.py
```

### Available Tools

| Tool | Usage | Description |
|------|-------|-------------|
| `list_modules()` | Get module overview | Lists all 38+ modules by category with brief descriptions |
| `fetch_module_doc("bus")` | Get specific module API | Returns classes, functions, enums, and full source |
| `fetch_design_doc("transport")` | Get design doc section | Fetches design document filtered by keyword |
| `search_api("AsyncBus")` | Search across headers | Grep-like search with file and line context |
| `list_examples()` | Browse examples | Lists all example programs with descriptions |

### When to Use

- `list_modules()` first to understand what's available
- `fetch_module_doc()` when implementing features using specific modules
- `search_api()` when looking for a specific class, function, or pattern
- `fetch_design_doc()` for architecture decisions and design rationale

## LLM Documentation Index

For AI tools that read documentation directly (Cursor, Windsurf, ChatGPT):

- `llms.txt` (17KB) - Concise overview: module descriptions, key APIs, quick start
- `llms-full.txt` (77KB) - Complete reference: all classes, function signatures, parameters, thread safety

These follow the [llmstxt.org](https://llmstxt.org) standard.

## Configuration Files

### .clang-format

Google C++ Style Guide base with:
- C++17 standard
- 120 column limit
- Include sorting: main header > project > C wrappers > C++ stdlib
- Pointer alignment: left (`int* ptr`)

### CPPLINT.cfg

Disabled checks (with rationale documented in file):
- `legal/copyright` - No copyright header requirement
- `build/c++11` - Allow C++17 features freely
- `runtime/references` - Allow non-const references (modern C++)
- `whitespace/*` - Formatting handled by clang-format
- Full list in `.ai/CPPLINT.cfg`
