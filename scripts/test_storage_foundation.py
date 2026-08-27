#!/usr/bin/env python3
"""Host-side contracts for the 32MB storage foundation."""

from __future__ import annotations

import argparse
import csv
import hashlib
import re
import struct
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FLASH_SIZE = 32 * 1024 * 1024
BOARD_DIR = ROOT / "main/boards/waveshare/esp32-s3-touch-amoled-1.75"
SOURCE_ARRAY_DIR = BOARD_DIR / "assets/source_arrays"

YGSOUL_ASSETS = {
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


def parse_size(value: str) -> int:
    value = value.strip().upper()
    if value.startswith("0X"):
        return int(value, 16)
    if value.endswith("K"):
        return int(value[:-1]) * 1024
    if value.endswith("M"):
        return int(value[:-1]) * 1024 * 1024
    return int(value)


@dataclass(frozen=True)
class Partition:
    name: str
    partition_type: str
    subtype: str
    offset: int
    size: int

    @property
    def end(self) -> int:
        return self.offset + self.size


def load_partitions() -> dict[str, Partition]:
    csv_path = ROOT / "partitions/v2/32m.csv"
    current_offset = 0x9000
    partitions: dict[str, Partition] = {}
    with csv_path.open(newline="", encoding="utf-8") as stream:
        rows = csv.reader(line for line in stream if not line.lstrip().startswith("#"))
        for row in rows:
            if not row or not row[0].strip():
                continue
            name, partition_type, subtype, raw_offset, raw_size = (
                field.strip() for field in row[:5]
            )
            size = parse_size(raw_size)
            offset = parse_size(raw_offset) if raw_offset else current_offset
            partitions[name] = Partition(name, partition_type, subtype, offset, size)
            current_offset = offset + size
    return partitions


def test_partition_contract() -> None:
    partitions = load_partitions()
    expected = {
        "assets": ("data", "spiffs", 0x200000, 0x600000),
        "ota_0": ("app", "ota_0", 0x800000, 0x600000),
        "ota_1": ("app", "ota_1", 0xE00000, 0x600000),
        "content": ("data", "fat", 0x1400000, 0xC00000),
    }
    for name, values in expected.items():
        assert name in partitions, f"missing partition: {name}"
        actual = partitions[name]
        assert (
            actual.partition_type,
            actual.subtype,
            actual.offset,
            actual.size,
        ) == values, f"{name} mismatch: {actual}"

    ordered = sorted(partitions.values(), key=lambda item: item.offset)
    for previous, current in zip(ordered, ordered[1:]):
        assert previous.end <= current.offset, (
            f"partition overlap: {previous.name} ends at {previous.end:#x}, "
            f"{current.name} starts at {current.offset:#x}"
        )
    assert partitions["content"].end == FLASH_SIZE


def extract_c_array(path: Path, symbol: str) -> bytes:
    source = path.read_text(encoding="utf-8")
    match = re.search(
        rf"\b{re.escape(symbol)}\s*\[\]\s*=\s*\{{(?P<body>.*?)\}};",
        source,
        re.DOTALL,
    )
    assert match, f"array not found: {symbol} in {path}"
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", match.group("body")))


def test_assets_contract() -> None:
    asset_dir = BOARD_DIR / "assets/cbin"
    for name, (source_name, symbol, color_format, width, height, stride) in YGSOUL_ASSETS.items():
        cbin_path = asset_dir / f"{name}.cbin"
        assert cbin_path.is_file(), f"missing CBin: {cbin_path}"
        data = cbin_path.read_bytes()
        assert len(data) >= 16, f"CBin header truncated: {cbin_path}"
        header = struct.unpack("<4sBBHHHI", data[:16])
        source_pixels = extract_c_array(SOURCE_ARRAY_DIR / source_name, symbol)
        assert header == (b"YGI1", color_format, 0, width, height, stride, len(source_pixels)), (
            f"CBin header mismatch: {name}: {header}"
        )
        assert len(data) == 16 + len(source_pixels), f"CBin size mismatch: {name}"
        assert hashlib.sha256(data[16:]).digest() == hashlib.sha256(source_pixels).digest(), (
            f"CBin pixel payload changed: {name}"
        )


def test_source_contract() -> None:
    board_source = (BOARD_DIR / "esp32-s3-touch-amoled-1.75.cc").read_text(encoding="utf-8")
    cmake_source = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
    for name in YGSOUL_ASSETS:
        assert f'"{name}.cbin"' in board_source, f"board does not load {name}.cbin"
    assert "LoadYGSoulAssets" in board_source
    assert "LvglAllocatedImage" in board_source
    assert "heap_caps_malloc" in board_source
    assert "LV_COLOR_FORMAT_RGB565A8" in board_source
    assert "payload_size != expected_payload_size" in board_source
    assert "LvglSourceImage" not in board_source
    assert "assets/cbin" in cmake_source
    for source_name, *_ in YGSOUL_ASSETS.values():
        assert not (BOARD_DIR / source_name).exists(), f"compiled image source remains: {source_name}"

    storage_header = (ROOT / "main/storage/content_storage.h").read_text(encoding="utf-8")
    storage_source = (ROOT / "main/storage/content_storage.cc").read_text(encoding="utf-8")
    main_source = (ROOT / "main/main.cc").read_text(encoding="utf-8")
    assert "kMusicQuotaBytes = 8 * 1024 * 1024" in storage_header
    assert "kGameQuotaBytes = 2 * 1024 * 1024" in storage_header
    assert "kSafetyReserveBytes = 1536 * 1024" in storage_header
    assert "mount_config.format_if_mount_failed = false" in storage_source
    assert "esp_partition_read" not in storage_source
    assert "Health::Corrupt" in storage_source
    assert "ContentStorage::GetInstance().Initialize()" in main_source


def test_flash_script_contract() -> None:
    script_path = ROOT / "scripts/flash_32m_storage_migration.sh"
    assert script_path.is_file(), f"missing migration script: {script_path}"
    script = script_path.read_text(encoding="utf-8")
    lowered = script.lower()
    for forbidden in ("erase_flash", "write_efuse", "burn_efuse", "disable_download"):
        assert forbidden not in lowered, f"unsafe command in migration script: {forbidden}"
    for required in (
        "read_flash 0x0 0x2000000",
        "read_flash 0x9000 0x32000",
        "read_flash 0x3b000 0xd2000",
        "read_flash 0x10d000 0x2000",
        "read_flash 0x10f000 0x1000",
        '0x1400000 "$content_image"',
        "verify_flash",
        "0x200000",
        "0x800000",
        "0xe00000",
        "shasum -a 256",
    ):
        assert required in script, f"migration safety step missing: {required}"
    assert script.index("read_flash 0x0 0x2000000") < script.index("write_flash")
    assert script.index("verify_backup_size") < script.index("write_flash")
    assert "--before no_reset --after hard_reset run" in script
    first_slot = script.index('0x800000 "$app_image"')
    second_slot = script.index('0xe00000 "$app_image"')
    partition_table = script.index('0x8000 "$partition_image"')
    assets = script.index('0x200000 "$assets_image"')
    assert first_slot < second_slot < partition_table < assets, (
        "migration does not preserve bootable slots before replacing the old app"
    )


def test_content_image_contract() -> None:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "fatfs_create_spiflash_image(content" in cmake
    storage_source = (ROOT / "main/storage/content_storage.cc").read_text(encoding="utf-8")
    assert "format_if_mount_failed = false" in storage_source


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--partition-only", action="store_true")
    parser.add_argument("--assets-only", action="store_true")
    parser.add_argument("--source-only", action="store_true")
    parser.add_argument("--flash-script-only", action="store_true")
    args = parser.parse_args()
    if args.assets_only:
        test_assets_contract()
        print("YGSoul lossless CBin contract passed")
        return
    if args.source_only:
        test_source_contract()
        print("storage source contract passed")
        return
    if args.flash_script_only:
        test_flash_script_contract()
        print("safe flash migration contract passed")
        return
    test_partition_contract()
    test_assets_contract()
    test_source_contract()
    test_flash_script_contract()
    test_content_image_contract()
    print("storage partition contract passed")


if __name__ == "__main__":
    main()
