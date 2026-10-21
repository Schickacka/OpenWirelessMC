#!/usr/bin/env python3
"""Regenerate sdcard_assets/default.mc from the built-in Sony/sd2psXtd template.

The 1.3 release shipped an all-zero default.mc. The firmware now detects and
repairs it at boot (CORRECOES-v1.4.md, C3), but the asset on the SD card should
also be correct so that a fresh install works even before the first boot.

Usage:
    python3 tools/regenerate_default_mc.py [output]

Default output: sdcard_assets/default.mc
"""
import re
import sys
from pathlib import Path

CARD_SIZE = 128 * 1024
BLOCK0_SIZE = 8 * 1024

ROOT = Path(__file__).resolve().parent.parent
TEMPLATE_CC = ROOT / "src" / "storage" / "ps1_empty_card.cc"


def load_template() -> bytes:
    text = TEMPLATE_CC.read_text(encoding="utf-8")
    values = [int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", text)]
    if len(values) != BLOCK0_SIZE:
        raise SystemExit(
            f"unexpected template size: {len(values)} bytes (expected {BLOCK0_SIZE})"
        )
    return bytes(values)


def main() -> int:
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "sdcard_assets" / "default.mc"

    block0 = load_template()
    checksum = 0
    for value in block0[:0x7F]:
        checksum ^= value
    if checksum != block0[0x7F]:
        raise SystemExit(
            f"template has a bad sector-0 checksum: 0x{block0[0x7F]:02X}, expected 0x{checksum:02X}"
        )

    image = block0 + b"\xFF" * (CARD_SIZE - BLOCK0_SIZE)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(image)
    print(f"wrote {output} ({len(image)} bytes, sector-0 checksum 0x{checksum:02X})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
