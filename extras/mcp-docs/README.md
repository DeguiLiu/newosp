# newosp MCP Documentation Server

A lightweight MCP server that exposes newosp C++ library documentation to Claude Code and other MCP-compatible AI coding agents.

Based on the [LangChain MCPDoc pattern](https://blog.langchain.com/how-to-turn-claude-code-into-a-domain-specific-coding-agent/).

## Why

Claude Code works best with domain-specific libraries when it has:
1. A concise `CLAUDE.md` with core concepts and pitfalls (high-level orientation)
2. MCP tools to fetch detailed API docs on demand (deep-dive when needed)

This server provides #2 for newosp.

## Tools

| Tool | Description |
|------|-------------|
| `list_modules` | List all 38+ modules organized by category |
| `fetch_module_doc` | Fetch full API doc for a specific module (classes, functions, source) |
| `fetch_design_doc` | Fetch design document (TOC or specific section) |
| `search_api` | Search across all headers for class/function/pattern |
| `list_examples` | List all example programs with descriptions |

## Install

```bash
# Install MCP SDK
pip install 'mcp[cli]'

# Test locally (SSE mode for MCP Inspector)
cd /path/to/newosp/extras/mcp-docs
python3 server.py --transport sse --port 8082
```

## Configure Claude Code

Add to `~/.claude/settings.json` or project `.claude/settings.json`:

```json
{
  "mcpServers": {
    "newosp-docs": {
      "command": "python3",
      "args": ["/path/to/newosp/extras/mcp-docs/server.py"],
      "env": {}
    }
  }
}
```

Or with `claude mcp add`:

```bash
claude mcp add newosp-docs -- python3 /path/to/newosp/extras/mcp-docs/server.py
```

## Configure Cursor / Windsurf

Add to `~/.cursor/mcp.json`:

```json
{
  "mcpServers": {
    "newosp-docs": {
      "command": "python3",
      "args": ["/path/to/newosp/extras/mcp-docs/server.py"]
    }
  }
}
```

## Usage in Claude Code

Once configured, Claude Code can use these tools automatically:

```
> Help me implement a publisher node using newosp bus

Claude will call:
  1. list_modules() -> see bus.hpp, node.hpp
  2. fetch_module_doc("bus") -> get AsyncBus API
  3. fetch_module_doc("node") -> get Node API
  4. search_api("publish") -> find publish patterns
```

## Architecture

```
server.py
  ├── list_modules()        -> scans include/osp/*.hpp, extracts brief + classes
  ├── fetch_module_doc()    -> returns full header with API summary
  ├── fetch_design_doc()    -> reads docs/design_zh.md (TOC or section filter)
  ├── search_api()          -> grep across all headers
  └── list_examples()       -> lists examples/*.cpp and subdirectories
```

No external dependencies beyond `mcp[cli]`. No database, no indexing step.
Reads directly from newosp source tree at runtime.
