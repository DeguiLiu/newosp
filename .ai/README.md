# newosp AI Configuration

Optional AI-assisted development tools for newosp. Not required for normal use.

## Files

| File | Purpose |
|------|---------|
| `server.py` | MCP documentation server (5 tools for Claude Code / Cursor / Windsurf) |
| `llms.txt` | Concise API index (17KB, [llmstxt.org](https://llmstxt.org) format) |
| `llms-full.txt` | Full API reference (77KB, all 38 modules) |

## MCP Server Setup

```bash
# Install dependency
pip install 'mcp[cli]'

# Register with Claude Code
claude mcp add newosp-docs -- python3 /path/to/newosp/.ai/server.py

# Or specify newosp path via env var
NEWOSP_ROOT=/path/to/newosp python3 .ai/server.py

# Test locally (SSE mode + MCP Inspector)
python3 .ai/server.py --transport sse --port 8082
```

Configure in `~/.claude/settings.json`:

```json
{
  "mcpServers": {
    "newosp-docs": {
      "command": "python3",
      "args": ["/path/to/newosp/.ai/server.py"]
    }
  }
}
```

## Tools

| Tool | Description |
|------|-------------|
| `list_modules` | List all 38+ modules by category |
| `fetch_module_doc` | Fetch detailed API for a specific module |
| `fetch_design_doc` | Fetch design document (TOC or section) |
| `search_api` | Search across all headers |
| `list_examples` | List example programs |

## How It Works

Based on the [LangChain MCPDoc pattern](https://blog.langchain.com/how-to-turn-claude-code-into-a-domain-specific-coding-agent/):
CLAUDE.md provides high-level orientation, MCP tools provide on-demand API details.

`server.py` reads directly from newosp source tree at runtime. No database, no indexing step.
`llms.txt` can be read directly by Cursor/Windsurf without MCP server.
