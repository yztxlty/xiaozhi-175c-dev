#!/usr/bin/env python3
"""构建并归档幽光 soul ESP32 OTA 应用镜像。"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = ROOT / "build-175c"
RELEASE_DIR = ROOT / "release"
VERSION_FILE = ROOT / "CMakeLists.txt"
VERSION_PATTERN = re.compile(r'set\(PROJECT_VER "(\d+)\.(\d+)\.(\d+)"\)')


def next_patch(version: str) -> str:
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", version)
    if not match:
        raise ValueError("版本号必须为 主.次.修订，例如 1.0.6")
    major, minor, patch = match.groups()
    return f"{major}.{minor}.{int(patch) + 1}"


def read_version() -> str:
    match = VERSION_PATTERN.search(VERSION_FILE.read_text(encoding="utf-8"))
    if not match:
        raise RuntimeError("CMakeLists.txt 中未找到 PROJECT_VER")
    return ".".join(match.groups())


def write_version(version: str) -> None:
    content = VERSION_FILE.read_text(encoding="utf-8")
    updated, count = VERSION_PATTERN.subn(f'set(PROJECT_VER "{version}")', content, count=1)
    if count != 1:
        raise RuntimeError("无法更新 PROJECT_VER")
    VERSION_FILE.write_text(updated, encoding="utf-8")


def package_current_build(version: str) -> Path:
    image = BUILD_DIR / "xiaozhi.bin"
    if not image.exists():
        raise RuntimeError(f"缺少构建产物：{image}")
    data = image.read_bytes()
    if not data or data[0] != 0xE9:
        raise RuntimeError("xiaozhi.bin 不是有效的 ESP 应用镜像")
    if version.encode("utf-8") not in data:
        raise RuntimeError(f"镜像内未发现版本号 {version}")

    RELEASE_DIR.mkdir(exist_ok=True)
    target = RELEASE_DIR / f"xiaozhi-{version}.bin"
    temporary = target.with_suffix(".bin.tmp")
    shutil.copy2(image, temporary)
    os.replace(temporary, target)
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    print(f"OTA 包：{target}")
    print(f"SHA-256：{digest}")
    return target


def main() -> None:
    parser = argparse.ArgumentParser(description="构建幽光 soul ESP32 OTA 应用镜像")
    parser.add_argument("--reuse-current", action="store_true", help="仅重打当前版本，用于中断恢复")
    args = parser.parse_args()

    if not os.environ.get("IDF_PATH"):
        raise SystemExit("请先执行：source /Users/yuezhenting/esp/esp-idf/export.sh")

    version = read_version()
    if not args.reuse_current:
        version = next_patch(version)
        write_version(version)

    subprocess.run(["idf.py", "-B", str(BUILD_DIR), "build"], cwd=ROOT, check=True)
    package_current_build(version)


if __name__ == "__main__":
    main()
