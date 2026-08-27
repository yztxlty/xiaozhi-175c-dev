#!/usr/bin/env python3
"""Host-side contracts for the 32MB storage foundation."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FLASH_SIZE = 32 * 1024 * 1024


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
        "ota_0": ("app", "ota_0", 0x200000, 0x600000),
        "ota_1": ("app", "ota_1", 0x800000, 0x600000),
        "assets": ("data", "spiffs", 0xE00000, 0x600000),
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


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--partition-only", action="store_true")
    parser.parse_args()
    test_partition_contract()
    print("storage partition contract passed")


if __name__ == "__main__":
    main()
