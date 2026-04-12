#!/usr/bin/env python3
"""
Convert a PNG file into a C header containing a static uint8_t[] of the raw
file bytes. Used to embed the renderer's preview.png into the test firmware.

Usage:
    python3 scripts/png_to_header.py

    python3 scripts/png_to_header.py <input.png> <output.h> [var_name]

With no arguments, reads ../worker/renderer/preview.png relative to this
script's repo location and writes include/preview_png.h.
"""

import sys
from pathlib import Path


def main() -> None:
    script_dir = Path(__file__).resolve().parent
    firmware_dir = script_dir.parent
    repo_root = firmware_dir.parent

    default_in = repo_root / "worker" / "renderer" / "preview.png"
    default_out = firmware_dir / "include" / "preview_png.h"

    if len(sys.argv) >= 3:
        in_path = Path(sys.argv[1]).resolve()
        out_path = Path(sys.argv[2]).resolve()
    elif len(sys.argv) == 2:
        print("Usage: png_to_header.py [<input.png> <output.h> [var_name]]", file=sys.stderr)
        sys.exit(1)
    else:
        in_path = default_in
        out_path = default_out

    var_name = sys.argv[3] if len(sys.argv) >= 4 else "preview_png"

    if not in_path.is_file():
        print(f"error: input file not found: {in_path}", file=sys.stderr)
        sys.exit(1)

    data = in_path.read_bytes()

    lines = [
        f"// Auto-generated from {in_path.name} by scripts/png_to_header.py.",
        "// Do not edit by hand. Regenerate with:",
        "//   python3 scripts/png_to_header.py",
        "",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"const unsigned int {var_name}_len = {len(data)};",
        f"const uint8_t {var_name}[] = {{",
    ]

    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        hex_str = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"    {hex_str},")

    lines.append("};")
    lines.append("")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines))
    print(f"wrote {out_path} ({len(data)} bytes from {in_path.name})")


if __name__ == "__main__":
    main()
