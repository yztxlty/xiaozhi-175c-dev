#!/usr/bin/env python3
"""Export the already-approved YGSoul LVGL pixel arrays as lossless CBin files."""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BOARD_DIR = ROOT / "main/boards/waveshare/esp32-s3-touch-amoled-1.75"
OUTPUT_DIR = BOARD_DIR / "assets/cbin"

ASSETS = {
    "ygsoul_boot": ("ygsoul_boot_lvgl.h", "ygsoul_boot_pixels", 0x12, 466, 466, 932),
    "ygsoul_companion_nomouth": (
        "ygsoul_companion_nomouth.c",
        "ygsoul_companion_nomouth_map",
        0x14,
        280,
        280,
        560,
    ),
    "ygsoul_mouth_1": ("ygsoul_mouth_1.c", "ygsoul_mouth_1_map", 0x14, 38, 29, 76),
    "ygsoul_mouth_2": ("ygsoul_mouth_2.c", "ygsoul_mouth_2_map", 0x14, 38, 29, 76),
    "ygsoul_mouth_3": ("ygsoul_mouth_3.c", "ygsoul_mouth_3_map", 0x14, 38, 29, 76),
}


def extract_pixels(path: Path, symbol: str) -> bytes:
    source = path.read_text(encoding="utf-8")
    match = re.search(
        rf"\b{re.escape(symbol)}\s*\[\]\s*=\s*\{{(?P<body>.*?)\}};",
        source,
        re.DOTALL,
    )
    if match is None:
        raise ValueError(f"array not found: {symbol} in {path}")
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", match.group("body")))


def build_cbin(source_name: str, symbol: str, color_format: int, width: int, height: int, stride: int) -> bytes:
    pixels = extract_pixels(BOARD_DIR / source_name, symbol)
    bytes_per_pixel = 3 if color_format == 0x14 else 2
    expected_size = width * height * bytes_per_pixel
    if len(pixels) != expected_size:
        raise ValueError(f"unexpected pixel size for {symbol}: {len(pixels)} != {expected_size}")
    header = struct.pack("<BBHHHHH", 0x19, color_format, 0, width, height, stride, 0)
    return header + pixels


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="verify committed output without writing")
    args = parser.parse_args()

    if not args.check:
        OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    for name, values in ASSETS.items():
        expected = build_cbin(*values)
        output = OUTPUT_DIR / f"{name}.cbin"
        if args.check:
            if not output.is_file() or output.read_bytes() != expected:
                raise SystemExit(f"CBin out of date: {output}")
        else:
            output.write_bytes(expected)
        print(f"{name}: {len(expected)} bytes, pixel payload identical")


if __name__ == "__main__":
    main()
