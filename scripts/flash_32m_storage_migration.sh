#!/usr/bin/env bash
set -euo pipefail

port="${1:-}"
build_dir="${2:-build-175c}"
backup_root="${YGSOUL_DEVICE_BACKUP_ROOT:-/Users/yuezhenting/esp/device-backups/xiaozhi-175c-dev}"
python_bin="${PYTHON:-python3}"

if [[ -z "$port" || ! -e "$port" ]]; then
    echo "usage: $0 /dev/cu.usbmodemXXX [build-directory]" >&2
    exit 1
fi

app_image="$build_dir/xiaozhi.bin"
assets_image="$build_dir/generated_assets.bin"
partition_image="$build_dir/partition_table/partition-table.bin"
ota_data_image="$build_dir/ota_data_initial.bin"
for image in "$app_image" "$assets_image" "$partition_image" "$ota_data_image"; do
    if [[ ! -s "$image" ]]; then
        echo "missing verified build artifact: $image" >&2
        exit 1
    fi
done

scripts/verify_storage_build.sh "$build_dir"

timestamp="$(date '+%Y%m%d-%H%M%S')"
backup_dir="$backup_root/$timestamp"
mkdir -p "$backup_dir"
exec > >(tee -a "$backup_dir/migration.log") 2>&1

esptool_cmd() {
    "$python_bin" -m esptool --chip esp32s3 --port "$port" --baud 460800 \
        --before default_reset --after no_reset "$@"
}

verify_backup_size() {
    local file="$1"
    local expected="$2"
    local actual
    actual="$(wc -c < "$file" | tr -d ' ')"
    if [[ "$actual" != "$expected" ]]; then
        echo "backup size mismatch: $file ($actual != $expected)" >&2
        exit 1
    fi
}

echo "Backup directory: $backup_dir"
flash_info="$(esptool_cmd flash_id 2>&1)"
echo "$flash_info"
if ! grep -Eqi '32MB|256Mbit' <<<"$flash_info"; then
    echo "device is not reporting a 32MB Flash; migration stopped" >&2
    exit 1
fi

git rev-parse HEAD > "$backup_dir/source-commit.txt"
printf '%s\n' "$port" > "$backup_dir/serial-port.txt"

esptool_cmd read_flash 0x0 0x2000000 "$backup_dir/full-flash-32mb.bin"
esptool_cmd read_flash 0x9000 0x32000 "$backup_dir/nvsfactory.bin"
esptool_cmd read_flash 0x3b000 0xd2000 "$backup_dir/nvs.bin"
esptool_cmd read_flash 0x10d000 0x2000 "$backup_dir/otadata.bin"
esptool_cmd read_flash 0x10f000 0x1000 "$backup_dir/phy-init.bin"

verify_backup_size "$backup_dir/full-flash-32mb.bin" 33554432
verify_backup_size "$backup_dir/nvsfactory.bin" 204800
verify_backup_size "$backup_dir/nvs.bin" 860160
verify_backup_size "$backup_dir/otadata.bin" 8192
verify_backup_size "$backup_dir/phy-init.bin" 4096

shasum -a 256 \
    "$backup_dir/full-flash-32mb.bin" \
    "$backup_dir/nvsfactory.bin" \
    "$backup_dir/nvs.bin" \
    "$backup_dir/otadata.bin" \
    "$backup_dir/phy-init.bin" \
    "$app_image" "$assets_image" "$partition_image" "$ota_data_image" \
    > "$backup_dir/sha256.txt"
cat "$backup_dir/sha256.txt"

if [[ "${YGSOUL_FLASH_CONFIRM:-}" != "YES" ]]; then
    echo "Backup verified. Set YGSOUL_FLASH_CONFIRM=YES to authorize the write phase." >&2
    exit 2
fi

# Write a valid application to both old/new recovery paths before changing
# partition boundaries. Core conversation remains bootable even if power is
# interrupted between these explicit operations.
esptool_cmd write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB \
    0x200000 "$app_image"
esptool_cmd write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB \
    0x800000 "$app_image"
esptool_cmd write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB \
    0x8000 "$partition_image"
esptool_cmd write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB \
    0xe00000 "$assets_image"
esptool_cmd erase_region 0x1400000 0xc00000
esptool_cmd write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB \
    0x10d000 "$ota_data_image"

esptool_cmd verify_flash \
    0x8000 "$partition_image" \
    0x10d000 "$ota_data_image" \
    0x200000 "$app_image" \
    0x800000 "$app_image" \
    0xe00000 "$assets_image"

echo "Migration write and read-back verification completed successfully."
echo "Recovery backup: $backup_dir"
esptool_cmd run
