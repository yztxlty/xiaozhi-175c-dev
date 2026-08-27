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
content_image="$build_dir/content.bin"
for image in "$app_image" "$assets_image" "$partition_image" "$ota_data_image" "$content_image"; do
    if [[ ! -s "$image" ]]; then
        echo "missing verified build artifact: $image" >&2
        exit 1
    fi
done

scripts/verify_storage_build.sh "$build_dir"

if [[ -n "${YGSOUL_REUSE_BACKUP_DIR:-}" ]]; then
    backup_dir="$YGSOUL_REUSE_BACKUP_DIR"
    if [[ ! -d "$backup_dir" ]]; then
        echo "reusable backup directory does not exist: $backup_dir" >&2
        exit 1
    fi
else
    timestamp="$(date '+%Y%m%d-%H%M%S')"
    backup_dir="$backup_root/$timestamp"
    mkdir -p "$backup_dir"
fi
exec > >(tee -a "$backup_dir/migration.log") 2>&1

esptool_cmd() {
    "$python_bin" -m esptool --chip esp32s3 --port "$port" --baud 460800 \
        --before default_reset --after no_reset "$@"
}

reset_device() {
    "$python_bin" -m esptool --chip esp32s3 --port "$port" \
        --before no_reset --after hard_reset run
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

if [[ -z "${YGSOUL_REUSE_BACKUP_DIR:-}" ]]; then
    esptool_cmd read_flash 0x0 0x2000000 "$backup_dir/full-flash-32mb.bin"
    esptool_cmd read_flash 0x9000 0x32000 "$backup_dir/nvsfactory.bin"
    esptool_cmd read_flash 0x3b000 0xd2000 "$backup_dir/nvs.bin"
    esptool_cmd read_flash 0x10d000 0x2000 "$backup_dir/otadata.bin"
    esptool_cmd read_flash 0x10f000 0x1000 "$backup_dir/phy-init.bin"
else
    echo "Reusing previously verified device backup: $backup_dir"
    # The full image is the source of truth. Recreate the smaller convenience
    # files locally if the first backup pass was interrupted after the 32MB read.
    dd if="$backup_dir/full-flash-32mb.bin" of="$backup_dir/nvsfactory.bin" \
        bs=4096 skip=9 count=50 status=none
    dd if="$backup_dir/full-flash-32mb.bin" of="$backup_dir/nvs.bin" \
        bs=4096 skip=59 count=210 status=none
    dd if="$backup_dir/full-flash-32mb.bin" of="$backup_dir/otadata.bin" \
        bs=4096 skip=269 count=2 status=none
    dd if="$backup_dir/full-flash-32mb.bin" of="$backup_dir/phy-init.bin" \
        bs=4096 skip=271 count=1 status=none
fi

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
    "$app_image" "$assets_image" "$partition_image" "$ota_data_image" "$content_image" \
    > "$backup_dir/sha256.txt"
cat "$backup_dir/sha256.txt"

if [[ "${YGSOUL_FLASH_CONFIRM:-}" != "YES" ]]; then
    echo "Backup verified. Set YGSOUL_FLASH_CONFIRM=YES to authorize the write phase." >&2
    reset_device || true
    exit 2
fi

# Under the old layout, ota_0 begins at 0x200000. Require it to contain an ESP
# image before touching old ota_1; this guarantees a bootable old-layout slot.
dd if="$backup_dir/full-flash-32mb.bin" of="$backup_dir/old-ota0-slot.bin" \
    bs=1048576 skip=2 count=4 status=none
"$python_bin" -m esptool image_info "$backup_dir/old-ota0-slot.bin" >/dev/null

# First stage the new app at the future ota_1 address. Only then switch the
# partition table: after the switch, old ota_0 and new ota_1 are both bootable.
esptool_cmd write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB \
    0x800000 "$app_image"
esptool_cmd verify_flash 0x800000 "$app_image"
esptool_cmd write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB \
    0x8000 "$partition_image"
esptool_cmd verify_flash 0x8000 "$partition_image"
esptool_cmd write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB \
    0x200000 "$app_image"
esptool_cmd verify_flash 0x200000 "$app_image"
esptool_cmd write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB \
    0xe00000 "$assets_image"
esptool_cmd write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB \
    0x1400000 "$content_image"
esptool_cmd write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB \
    0x10d000 "$ota_data_image"

esptool_cmd verify_flash \
    0x8000 "$partition_image" \
    0x10d000 "$ota_data_image" \
    0x200000 "$app_image" \
    0x800000 "$app_image" \
    0xe00000 "$assets_image" \
    0x1400000 "$content_image"

echo "Migration write and read-back verification completed successfully."
echo "Recovery backup: $backup_dir"
reset_device
