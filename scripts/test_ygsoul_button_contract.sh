#!/usr/bin/env bash
set -euo pipefail

board_file="main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc"

# BOOT: short press controls voice, long press enters Wi-Fi provisioning.
grep -q 'boot_button_(BOOT_BUTTON_GPIO, false, 3000)' "$board_file"
grep -q 'boot_button_\.OnLongPress' "$board_file"
grep -q 'EnterWifiConfigMode();' "$board_file"

# BLE provisioning must not be interrupted by the normal chat/audio toggle.
perl -0ne 'exit !/#ifdef CONFIG_USE_YGSOUL_BLE_WIFI_PROVISIONING\s+if \(state == kDeviceStateWifiConfiguring\) \{\s+return;\s+\}/s' main/application.cc

# PWR: read the existing EXIO4 hardware signal and never shut down before 10s.
grep -q 'IO_EXPANDER_PIN_NUM_4' "$board_file"
grep -q 'esp_io_expander_get_level(io_expander' "$board_file"
grep -q 'HandlePwrButton' "$board_file"
grep -q 'PWR_BUTTON_LONG_PRESS_US' "$board_file"
grep -q 'WriteReg(0x27, 0x1C)' "$board_file"
perl -0ne 'exit !/InitializeCodecI2c\(\);\s+InitializeTca9554\(\);\s+InitializeAxp2101\(\);/s' "$board_file"
# 1.75C may not have TCA9554; never abort boot if expander init fails.
perl -0ne 'exit !/void InitializeTca9554\(void\) \{.*?io_expander = NULL;.*?return;/s' "$board_file"
! awk '/void InitializeTca9554/,/^    void InitializeAxp2101/' "$board_file" | grep -q 'ESP_ERROR_CHECK'

echo "YGSoul button contract passed"
