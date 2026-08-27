#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-build}"
app_image="$build_dir/xiaozhi.bin"
assets_image="$build_dir/generated_assets.bin"
partition_image="$build_dir/partition_table/partition-table.bin"
content_image="$build_dir/content.bin"
max_image_bytes=$((6 * 1024 * 1024))

for image in "$app_image" "$assets_image" "$partition_image" "$content_image"; do
    if [[ ! -s "$image" ]]; then
        echo "missing build artifact: $image" >&2
        exit 1
    fi
done

app_bytes="$(wc -c < "$app_image" | tr -d ' ')"
assets_bytes="$(wc -c < "$assets_image" | tr -d ' ')"
if (( app_bytes >= max_image_bytes )); then
    echo "application image exceeds 6MiB: $app_bytes bytes" >&2
    exit 1
fi
if (( assets_bytes >= max_image_bytes )); then
    echo "assets image exceeds 6MiB: $assets_bytes bytes" >&2
    exit 1
fi

idf_root="${IDF_PATH:-/Users/yuezhenting/esp/esp-idf}"
partition_tool="$idf_root/components/partition_table/gen_esp32part.py"
if [[ ! -f "$partition_tool" ]]; then
    echo "partition parser not found: $partition_tool" >&2
    exit 1
fi
partition_dump="$(python3 "$partition_tool" "$partition_image")"
grep -q '^assets,data,spiffs,0x200000,6M,' <<<"$partition_dump"
grep -q '^ota_0,app,ota_0,0x800000,6M,' <<<"$partition_dump"
grep -q '^ota_1,app,ota_1,0xe00000,6M,' <<<"$partition_dump"
grep -q '^content,data,fat,0x1400000,12M,' <<<"$partition_dump"

echo "application: $app_bytes bytes, free: $((max_image_bytes - app_bytes)) bytes"
echo "assets: $assets_bytes bytes, free: $((max_image_bytes - assets_bytes)) bytes"
shasum -a 256 "$app_image" "$assets_image" "$partition_image"
content_bytes="$(wc -c < "$content_image" | tr -d ' ')"
if (( content_bytes != 12 * 1024 * 1024 )); then
    echo "content image must exactly fill 12MiB: $content_bytes bytes" >&2
    exit 1
fi
shasum -a 256 "$content_image"
echo "storage build verification passed"
