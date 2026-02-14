# Project Instructions

## Target Platform

- Target: ARM-Linux embedded (GCC / Clang)
- MSVC is NOT a target platform for this project or GitHub CI
- Compile flags: `-fno-exceptions -fno-rtti` (optional, controlled by CMake option)

## AI Tooling

All AI-related configuration and tools are in `.ai/` directory. See `.ai/README.md` for details.

- Code formatting: `.ai/format.sh [--check]`
- Linting: `.ai/lint.sh [--fix]`
- Full quality check: `.ai/check.sh [--quick] [--sanitizer]`
- MCP documentation server: `.ai/server.py`
- API index for LLM context: `.ai/llms.txt` (concise) / `.ai/llms-full.txt` (full)
- clang-format config: `.ai/.clang-format`
- cpplint config: `.ai/CPPLINT.cfg`
