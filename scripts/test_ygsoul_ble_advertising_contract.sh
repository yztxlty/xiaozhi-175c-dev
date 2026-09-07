#!/usr/bin/env bash
set -euo pipefail

ble_file="main/boards/common/ygsoul_ble_provisioning.cc"

# ESP-IDF requires service_uuid_len to be a 16-byte UUID-shaped buffer.
grep -q 'uint8_t g_advertisedServiceUuid\[16\]' "$ble_file"
grep -q '.service_uuid_len = sizeof(g_advertisedServiceUuid)' "$ble_file"
grep -q 'esp_ble_gap_config_adv_data(&kScanRspData)' "$ble_file"
grep -q '.set_scan_rsp = true' "$ble_file"

echo "YGSoul BLE advertising contract passed"
