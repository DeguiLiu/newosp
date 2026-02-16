#!/usr/bin/env python3
"""
ospgen.py -- newosp code generator.

Reads YAML definition files and generates C++ headers using Jinja2 templates.
Supports message/event definitions, standalone enums, and node topology generation.

Features:
  P0: sizeof assertions, event-message binding, field descriptions, standalone enums
  P1: field range validation, byte order swap, packed structs, version, deprecated
  P2: message descriptions (Doxygen), debug Dump(), topology descriptions

Usage:
    python3 ospgen.py --input defs/protocol_messages.yaml --output generated/osp/protocol_messages.hpp
    python3 ospgen.py --input defs/topology.yaml --output generated/osp/topology.hpp
"""

import argparse
import os
import re
import sys

import yaml
from jinja2 import Environment, FileSystemLoader


# ==========================================================================
# Name conversion helpers
# ==========================================================================

def snake_to_camel(name: str) -> str:
    """UPPER_SNAKE -> CamelCase: SYN_ACK -> SynAck"""
    return "".join(w.capitalize() for w in name.lower().split("_"))


def snake_to_k_prefix(namespace: str, name: str) -> str:
    """UPPER_SNAKE -> kPrefixName: CONNECT -> kProtocolConnect"""
    prefix = namespace.capitalize()
    camel = snake_to_camel(name)
    return f"k{prefix}{camel}"


def upper_snake_to_k_camel(name: str) -> str:
    """UPPER_SNAKE -> kCamelCase: STREAM_START -> kStreamStart"""
    return "k" + snake_to_camel(name)


# ==========================================================================
# Type analysis helpers
# ==========================================================================

def parse_array_type(type_str: str):
    """Parse 'char[32]' -> ('char', '32'), 'uint32_t' -> None"""
    m = re.match(r"^(\w+)\[(\d+)\]$", type_str)
    if m:
        return m.group(1), m.group(2)
    return None


_PRINTF_MAP = {
    "uint8_t":  ("%u",   "static_cast<unsigned>({})"),
    "uint16_t": ("%u",   "static_cast<unsigned>({})"),
    "uint32_t": ("%u",   "static_cast<unsigned>({})"),
    "uint64_t": ("%lu",  "static_cast<unsigned long>({})"),
    "int8_t":   ("%d",   "static_cast<int>({})"),
    "int16_t":  ("%d",   "static_cast<int>({})"),
    "int32_t":  ("%d",   "static_cast<int>({})"),
    "int64_t":  ("%ld",  "static_cast<long>({})"),
    "float":    ("%.3f", "static_cast<double>({})"),
    "double":   ("%.6f", "{}"),
    "bool":     ("%d",   "static_cast<int>({})"),
}


def printf_info(type_str: str):
    """Return (format_spec, cast_template) for snprintf, or None if not printable.

    For char[N] arrays returns ('%s', '{}') so the field name is used directly.
    For uint8_t[N] and other non-char arrays returns None (skipped in Dump).
    """
    arr = parse_array_type(type_str)
    if arr:
        base, _ = arr
        if base == "char":
            return ("%s", "{}")
        return None  # skip byte arrays etc.
    return _PRINTF_MAP.get(type_str)


_BSWAP_MAP = {
    "uint16_t": "__builtin_bswap16",
    "int16_t":  "__builtin_bswap16",
    "uint32_t": "__builtin_bswap32",
    "int32_t":  "__builtin_bswap32",
    "uint64_t": "__builtin_bswap64",
    "int64_t":  "__builtin_bswap64",
}

_BSWAP_FLOAT = {
    "float":  ("uint32_t", "__builtin_bswap32"),
    "double": ("uint64_t", "__builtin_bswap64"),
}


def needs_bswap(type_str: str) -> bool:
    """Return True if this type needs byte-swap for network order."""
    arr = parse_array_type(type_str)
    if arr:
        return False  # arrays are byte-level, no swap needed
    return type_str in _BSWAP_MAP or type_str in _BSWAP_FLOAT


def bswap_lines(field_name: str, type_str: str) -> list:
    """Return C++ lines to byte-swap a field (no trailing semicolons for flexibility).

    For integer types: single line assignment.
    For float/double: memcpy-based reinterpret + swap + memcpy back.
    """
    if type_str in _BSWAP_MAP:
        fn = _BSWAP_MAP[type_str]
        return [f"{field_name} = static_cast<{type_str}>({fn}("
                f"static_cast<{unsigned_of(type_str)}>({field_name})))"]
    if type_str in _BSWAP_FLOAT:
        uint_type, fn = _BSWAP_FLOAT[type_str]
        return [
            f"{uint_type} tmp_{field_name}",
            f"std::memcpy(&tmp_{field_name}, &{field_name}, sizeof(tmp_{field_name}))",
            f"tmp_{field_name} = {fn}(tmp_{field_name})",
            f"std::memcpy(&{field_name}, &tmp_{field_name}, sizeof(tmp_{field_name}))",
        ]
    return []


def unsigned_of(type_str: str) -> str:
    """Map signed/unsigned integer to its unsigned counterpart for bswap cast."""
    mapping = {
        "int16_t": "uint16_t",
        "int32_t": "uint32_t",
        "int64_t": "uint64_t",
        "uint16_t": "uint16_t",
        "uint32_t": "uint32_t",
        "uint64_t": "uint64_t",
    }
    return mapping.get(type_str, type_str)


def has_range_fields(msg: dict) -> bool:
    """Check if any field in the message has a range constraint."""
    for field in msg.get("fields", []):
        if "range" in field:
            return True
    return False


def has_event_binding(messages: list) -> bool:
    """Check if any message has an event binding."""
    for msg in messages:
        if "event" in msg:
            return True
    return False


def range_check_expr(field_name: str, type_str: str, range_val: list) -> str:
    """Generate a range check expression for Validate().

    Returns a C++ boolean expression that is true when the field is OUT of range.
    """
    lo, hi = range_val
    # For float types, use direct comparison
    if type_str in ("float", "double"):
        lo_str = f"{lo}f" if type_str == "float" else str(lo)
        hi_str = f"{hi}f" if type_str == "float" else str(hi)
        # Use type-appropriate suffix
        if type_str == "float":
            lo_str = f"{float(lo)}f"
            hi_str = f"{float(hi)}f"
        else:
            lo_str = str(float(lo))
            hi_str = str(float(hi))
        return f"{field_name} < {lo_str} || {field_name} > {hi_str}"
    else:
        # Integer types -- cast to avoid signed/unsigned warnings
        return (f"static_cast<{type_str}>({field_name}) < "
                f"static_cast<{type_str}>({lo}) || "
                f"static_cast<{type_str}>({field_name}) > "
                f"static_cast<{type_str}>({hi})")


# ==========================================================================
# Template detection and YAML loading
# ==========================================================================

def detect_template(data: dict) -> str:
    """Auto-detect which template to use based on YAML content."""
    if "messages" in data or "events" in data:
        return "messages.hpp.j2"
    if "nodes" in data:
        return "topology.hpp.j2"
    raise ValueError("Cannot detect template type from YAML content. "
                     "Expected 'messages', 'events', or 'nodes' key.")


def load_yaml(path: str) -> dict:
    """Load and validate YAML file."""
    with open(path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f)
    if not isinstance(data, dict):
        raise ValueError(f"YAML root must be a mapping, got {type(data).__name__}")
    return data


# ==========================================================================
# Jinja2 environment
# ==========================================================================

def build_env(template_dir: str) -> Environment:
    """Create Jinja2 environment with custom filters and globals."""
    env = Environment(
        loader=FileSystemLoader(template_dir),
        keep_trailing_newline=True,
        trim_blocks=True,
        lstrip_blocks=True,
    )
    # Filters (used as {{ value | filter_name }})
    env.filters["snake_to_camel"] = snake_to_camel
    env.filters["parse_array"] = parse_array_type
    env.filters["upper_snake_to_k"] = upper_snake_to_k_camel

    # Globals (used as {{ func(args) }})
    env.globals["snake_to_k_prefix"] = snake_to_k_prefix
    env.globals["parse_array_type"] = parse_array_type
    env.globals["printf_info"] = printf_info
    env.globals["needs_bswap"] = needs_bswap
    env.globals["bswap_lines"] = bswap_lines
    env.globals["has_range_fields"] = has_range_fields
    env.globals["has_event_binding"] = has_event_binding
    env.globals["range_check_expr"] = range_check_expr
    return env


# ==========================================================================
# Code generation
# ==========================================================================

def generate(input_path: str, output_path: str, template_dir: str = None):
    """Main generation entry point."""
    if template_dir is None:
        template_dir = os.path.join(os.path.dirname(__file__), "templates")

    data = load_yaml(input_path)
    env = build_env(template_dir)

    template_name = data.get("template", detect_template(data))
    template = env.get_template(template_name)

    # Derive guard name from output filename
    basename = os.path.basename(output_path)
    guard = "OSP_GENERATED_" + re.sub(r"[^A-Za-z0-9]", "_", basename).upper() + "_"

    rendered = template.render(
        guard=guard,
        filename=basename,
        input_file=os.path.basename(input_path),
        **data,
    )

    # Ensure output directory exists
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)

    with open(output_path, "w", encoding="utf-8") as f:
        f.write(rendered)

    print(f"ospgen: {input_path} -> {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="newosp code generator (YAML -> C++ header)")
    parser.add_argument("--input", "-i", required=True,
                        help="Input YAML definition file")
    parser.add_argument("--output", "-o", required=True,
                        help="Output C++ header file")
    parser.add_argument("--templates", "-t", default=None,
                        help="Template directory (default: tools/templates/)")
    args = parser.parse_args()

    try:
        generate(args.input, args.output, args.templates)
    except Exception as e:
        print(f"ospgen error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
