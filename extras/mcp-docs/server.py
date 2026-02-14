#!/usr/bin/env python3
"""newosp MCP Documentation Server

A lightweight MCP server that exposes newosp C++ library documentation
to Claude Code and other MCP-compatible AI coding agents.

Based on the LangChain MCPDoc pattern:
  https://blog.langchain.com/how-to-turn-claude-code-into-a-domain-specific-coding-agent/

Usage:
  # stdio mode (for Claude Code)
  python3 server.py

  # SSE mode (for debugging with MCP Inspector)
  python3 server.py --transport sse --port 8082
"""

import argparse
import os
import re
import sys
from pathlib import Path

try:
    from mcp.server.fastmcp import FastMCP
except ImportError:
    print("Error: fastmcp not installed. Run: pip install 'mcp[cli]'", file=sys.stderr)
    sys.exit(1)

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SCRIPT_DIR = Path(__file__).resolve().parent
# Default: look for docs next to this script
DEFAULT_DOCS_DIR = SCRIPT_DIR
# Fallback: newosp project root
NEWOSP_ROOT = SCRIPT_DIR.parent.parent
INCLUDE_DIR = NEWOSP_ROOT / "include" / "osp"
DESIGN_DOC = NEWOSP_ROOT / "docs" / "design_zh.md"

# Module categories for organized listing
MODULE_CATEGORIES = {
    "Foundation": [
        "platform.hpp", "vocabulary.hpp", "spsc_ringbuffer.hpp",
    ],
    "Core": [
        "config.hpp", "log.hpp", "timer.hpp", "shell.hpp", "mem_pool.hpp",
        "shutdown.hpp", "bus.hpp", "node.hpp", "worker_pool.hpp",
        "executor.hpp", "hsm.hpp", "bt.hpp", "semaphore.hpp",
    ],
    "Network": [
        "socket.hpp", "connection.hpp", "io_poller.hpp", "transport.hpp",
        "shm_transport.hpp", "data_fusion.hpp", "discovery.hpp",
        "service.hpp", "node_manager.hpp", "transport_factory.hpp", "net.hpp",
    ],
    "Application": [
        "app.hpp", "post.hpp", "qos.hpp", "lifecycle_node.hpp",
    ],
    "Serial": [
        "serial_transport.hpp",
    ],
    "HSM Integration": [
        "node_manager_hsm.hpp", "service_hsm.hpp", "discovery_hsm.hpp",
    ],
    "Diagnostics": [
        "watchdog.hpp", "fault_collector.hpp", "shell_commands.hpp",
    ],
}

# ---------------------------------------------------------------------------
# Document loading helpers
# ---------------------------------------------------------------------------

def _load_text(path: Path) -> str:
    """Load a text file, return empty string if not found."""
    try:
        return path.read_text(encoding="utf-8")
    except (FileNotFoundError, PermissionError):
        return ""


def _extract_header_summary(hpp_path: Path) -> dict:
    """Extract key info from a C++ header file.

    Returns dict with keys: brief, classes, functions, enums.
    """
    content = _load_text(hpp_path)
    if not content:
        return {"brief": "", "classes": [], "functions": [], "enums": []}

    # Extract file-level brief from @brief tag or @file doc comment
    brief = ""
    m = re.search(r"@brief\s+(.+?)(?:\n|$)", content)
    if not m:
        # Try /// style comment (skip license block)
        for line in content.split("\n"):
            stripped = line.strip()
            if stripped.startswith("///") and "license" not in stripped.lower():
                brief = stripped.lstrip("/").strip()
                if brief:
                    break
    if m:
        brief = m.group(1).strip()

    # Extract class/struct names (skip forward declarations)
    classes = []
    for m in re.finditer(
        r"(?:template\s*<[^>]*>\s*)?(?:class|struct)\s+(\w+)\s*(?:final\s*)?[:{]",
        content,
    ):
        name = m.group(1)
        if name not in classes and not name.startswith("_"):
            classes.append(name)

    # Extract public function signatures (non-member or important methods)
    functions = []
    for m in re.finditer(
        r"^\s*(?:inline\s+|static\s+|constexpr\s+)*"
        r"(?:auto|void|bool|int|uint\w+|size_t|std::\w+|osp::\w+|Expected<[^>]+>)\s+"
        r"(\w+)\s*\([^)]*\)",
        content,
        re.MULTILINE,
    ):
        name = m.group(1)
        if name not in functions and not name.startswith("_"):
            functions.append(name)

    # Extract enum names
    enums = []
    for m in re.finditer(r"enum\s+(?:class\s+)?(\w+)", content):
        name = m.group(1)
        if name not in enums:
            enums.append(name)

    return {
        "brief": brief,
        "classes": classes[:10],  # cap to avoid noise
        "functions": functions[:15],
        "enums": enums[:8],
    }


# ---------------------------------------------------------------------------
# MCP Server
# ---------------------------------------------------------------------------

mcp = FastMCP(
    "newosp-docs",
    instructions="Documentation server for newosp C++17 embedded infrastructure library",
)


@mcp.tool()
def list_modules() -> str:
    """List all newosp modules organized by category.

    Returns a structured overview of all 38+ header files grouped into:
    Foundation, Core, Network, Application, Serial, HSM Integration, Diagnostics.
    """
    lines = [
        "# newosp Module Index",
        "",
        "> C++17 header-only infrastructure library for industrial embedded systems",
        "> Target: ARM-Linux (GCC/Clang), -fno-exceptions -fno-rtti optional",
        "",
    ]

    for category, modules in MODULE_CATEGORIES.items():
        lines.append(f"## {category}")
        lines.append("")
        for mod in modules:
            hpp_path = INCLUDE_DIR / mod
            info = _extract_header_summary(hpp_path)
            brief = info["brief"] or "(no description)"
            classes_str = ", ".join(info["classes"][:5]) if info["classes"] else ""
            line = f"- **{mod}**: {brief}"
            if classes_str:
                line += f" [{classes_str}]"
            lines.append(line)
        lines.append("")

    return "\n".join(lines)


@mcp.tool()
def fetch_module_doc(module_name: str) -> str:
    """Fetch detailed API documentation for a specific newosp module.

    Args:
        module_name: Header filename (e.g. 'bus.hpp', 'node.hpp', 'hsm.hpp').
                     Also accepts module name without extension (e.g. 'bus').

    Returns the full header content with extracted API summary.
    """
    # Normalize name
    if not module_name.endswith(".hpp"):
        module_name = module_name + ".hpp"

    hpp_path = INCLUDE_DIR / module_name
    if not hpp_path.exists():
        available = [f.name for f in INCLUDE_DIR.glob("*.hpp")]
        return (
            f"Module '{module_name}' not found.\n\n"
            f"Available modules:\n" + "\n".join(f"  - {m}" for m in sorted(available))
        )

    info = _extract_header_summary(hpp_path)
    content = _load_text(hpp_path)

    lines = [
        f"# {module_name}",
        "",
    ]

    if info["brief"]:
        lines.append(f"> {info['brief']}")
        lines.append("")

    if info["classes"]:
        lines.append("## Classes / Structs")
        for c in info["classes"]:
            lines.append(f"- `{c}`")
        lines.append("")

    if info["enums"]:
        lines.append("## Enums")
        for e in info["enums"]:
            lines.append(f"- `{e}`")
        lines.append("")

    if info["functions"]:
        lines.append("## Key Functions")
        for f in info["functions"]:
            lines.append(f"- `{f}()`")
        lines.append("")

    lines.append("## Full Source")
    lines.append("")
    lines.append("```cpp")
    lines.append(content)
    lines.append("```")

    return "\n".join(lines)


@mcp.tool()
def fetch_design_doc(section: str = "") -> str:
    """Fetch newosp design document (Chinese).

    Args:
        section: Optional section keyword to filter (e.g. 'bus', 'transport',
                 'shm', 'hsm', 'executor'). Empty returns the full document.

    Returns the design document content, optionally filtered to a section.
    """
    content = _load_text(DESIGN_DOC)
    if not content:
        return "Design document not found at: " + str(DESIGN_DOC)

    if not section:
        # Return table of contents + first 3000 chars
        toc_lines = []
        for line in content.split("\n"):
            if line.startswith("#"):
                toc_lines.append(line)
        toc = "\n".join(toc_lines)

        return (
            "# newosp Design Document - Table of Contents\n\n"
            + toc
            + "\n\n---\n\n"
            + "## Document Preview (first 3000 chars)\n\n"
            + content[:3000]
            + "\n\n...(truncated, use section parameter to fetch specific parts)"
        )

    # Extract section matching keyword
    section_lower = section.lower()
    lines = content.split("\n")
    result = []
    capturing = False
    capture_level = 0

    for line in lines:
        if line.startswith("#"):
            level = len(line) - len(line.lstrip("#"))
            if section_lower in line.lower():
                capturing = True
                capture_level = level
                result.append(line)
            elif capturing and level <= capture_level:
                break  # hit next section at same or higher level
            elif capturing:
                result.append(line)
        elif capturing:
            result.append(line)

    if not result:
        return f"No section matching '{section}' found in design document."

    return "\n".join(result)


@mcp.tool()
def search_api(query: str) -> str:
    """Search across all newosp headers for a class, function, enum, or pattern.

    Args:
        query: Search term (e.g. 'AsyncBus', 'publish', 'FrameHeader',
               'ShmRingBuffer', 'SpinLock').

    Returns matching lines with file context.
    """
    if not INCLUDE_DIR.exists():
        return f"Include directory not found: {INCLUDE_DIR}"

    query_lower = query.lower()
    results = []

    for hpp_path in sorted(INCLUDE_DIR.glob("*.hpp")):
        content = _load_text(hpp_path)
        matches = []
        for i, line in enumerate(content.split("\n"), 1):
            if query_lower in line.lower():
                matches.append(f"  L{i}: {line.rstrip()}")

        if matches:
            results.append(f"### {hpp_path.name} ({len(matches)} matches)")
            # Show up to 20 matches per file
            results.extend(matches[:20])
            if len(matches) > 20:
                results.append(f"  ... and {len(matches) - 20} more matches")
            results.append("")

    if not results:
        return f"No matches found for '{query}' across newosp headers."

    return (
        f"# Search results for '{query}'\n\n" + "\n".join(results)
    )


@mcp.tool()
def list_examples() -> str:
    """List all newosp example programs with descriptions."""
    examples_dir = NEWOSP_ROOT / "examples"
    if not examples_dir.exists():
        return "Examples directory not found."

    lines = ["# newosp Examples", ""]

    # Single-file examples
    for cpp in sorted(examples_dir.glob("*.cpp")):
        content = _load_text(cpp)
        desc = ""
        # Try @brief first
        m = re.search(r"@brief\s+(.+?)(?:\n|$)", content)
        if not m:
            # Try @file line
            m = re.search(r"@file\s+\S+\s*\n\s*\*?\s*@brief\s+(.+?)(?:\n|$)", content)
        if not m:
            # Try "filename -- description" pattern
            m = re.search(r"\b" + re.escape(cpp.stem) + r"[.\w]*\s+--\s+(.+?)(?:\n|$)", content)
        if not m:
            # Try first meaningful // comment (skip separators and copyright)
            for cline in content.split("\n")[:30]:
                stripped = cline.strip().lstrip("/*").strip()
                if (stripped.startswith("//") or stripped) and \
                   len(stripped) > 10 and \
                   "===" not in stripped and "---" not in stripped and \
                   "copyright" not in stripped.lower() and \
                   "license" not in stripped.lower() and \
                   "permission" not in stripped.lower():
                    desc = stripped.lstrip("/ *").strip()
                    break
        if m and not desc:
            desc = m.group(1).strip()
        lines.append(f"- **{cpp.name}**: {desc}")

    lines.append("")

    # Multi-file examples (subdirectories)
    for subdir in sorted(examples_dir.iterdir()):
        if subdir.is_dir() and not subdir.name.startswith("."):
            readme = subdir / "README.md"
            desc = ""
            if readme.exists():
                first_line = _load_text(readme).split("\n")[0]
                desc = first_line.lstrip("# ").strip()
            cpp_count = len(list(subdir.glob("*.cpp")))
            lines.append(
                f"- **{subdir.name}/**: {desc} ({cpp_count} source files)"
            )

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="newosp MCP Documentation Server")
    parser.add_argument(
        "--transport",
        choices=["stdio", "sse"],
        default="stdio",
        help="Transport mode (default: stdio for Claude Code)",
    )
    parser.add_argument("--port", type=int, default=8082, help="Port for SSE mode")
    parser.add_argument("--host", default="localhost", help="Host for SSE mode")
    args = parser.parse_args()

    if args.transport == "sse":
        mcp.run(transport="sse", host=args.host, port=args.port)
    else:
        mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
