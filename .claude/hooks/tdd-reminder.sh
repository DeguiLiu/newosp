#!/usr/bin/env bash
# newosp TDD reminder hook.
# Runs after Edit/Write. If the edit touched a production header (include/osp/*.hpp)
# or a test file, remind about the TDD iron law: no implementation without a
# failing test seen first (RED). See .claude/skills/newosp-tdd/SKILL.md.
set -u

FILE=""
# Claude Code passes hook input via stdin as JSON with a file_path field.
if [ -p /dev/stdin ]; then
  FILE=$(python3 -c 'import sys,json; d=json.load(sys.stdin); print(d.get("tool_input",{}).get("file_path",""))' 2>/dev/null)
fi
if [ -z "$FILE" ]; then
  exit 0
fi

case "$FILE" in
  include/osp/*.hpp)
    printf 'TDD reminder: edited production header %s.\n' "$FILE"
    printf 'Iron law: NO implementation without a failing test first (RED -> GREEN -> REFACTOR).\n'
    printf 'See .claude/skills/newosp-tdd/SKILL.md for the workflow and deadlock/UAF test patterns.\n'
    ;;
  tests/*.cpp)
    printf 'TDD: edited %s. After writing the failing test, run it to confirm RED before implementing.\n' "$FILE"
    ;;
esac
exit 0
