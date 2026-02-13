#!/usr/bin/env python3
"""
ospgen.py -- newosp code generator.

Reads YAML definition files and generates C++ headers using Jinja2 templates.
Supports message/event definitions and node topology generation.

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


def snake_to_camel(name: str) -> str:
    """UPPER_SNAKE -> CamelCase: SYN_ACK -> SynAck"""
    return "".join(w.capitalize() for w in name.lower().split("_"))


def snake_to_k_prefix(namespace: str, name: str) -> str:
    """UPPER_SNAKE -> kPrefixName: CONNECT -> kProtocolConnect"""
    prefix = namespace.capitalize()
    camel = snake_to_camel(name)
    return f"k{prefix}{camel}"


def parse_array_type(type_str: str):
    """Parse 'char[32]' -> ('char', '32'), 'uint32_t' -> None"""
    m = re.match(r"^(\w+)\[(\d+)\]$", type_str)
    if m:
        return m.group(1), m.group(2)
    return None


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


def build_env(template_dir: str) -> Environment:
    """Create Jinja2 environment with custom filters."""
    env = Environment(
        loader=FileSystemLoader(template_dir),
        keep_trailing_newline=True,
        trim_blocks=True,
        lstrip_blocks=True,
    )
    env.filters["snake_to_camel"] = snake_to_camel
    env.filters["parse_array"] = parse_array_type
    env.globals["snake_to_k_prefix"] = snake_to_k_prefix
    env.globals["parse_array_type"] = parse_array_type
    return env


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
