#!/usr/bin/env bash
set -euo pipefail

board_file="main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc"
wifi_board="main/boards/common/wifi_board.cc"
wifi_header="main/boards/common/wifi_board.h"
app="main/application.cc"
zh="main/assets/locales/zh-CN/language.json"
en="main/assets/locales/en-US/language.json"

# Voice phrases are first-class commands: enter switches immediately, exit leaves immediately.
grep -q '进入配网' "$app"
grep -q '退出配网' "$app"
grep -q 'HandleWifiConfigVoiceCommand' "$app"
grep -q 'type->valuestring, "stt"' "$app"

# Board API: voice enter keeps the current station so the user can still speak 退出.
grep -q 'EnterWifiConfigModeForVoice' "$wifi_header"
grep -q 'ExitWifiConfigMode' "$wifi_header"
grep -q 'EnterWifiConfigModeForVoice' "$wifi_board"
grep -q 'ExitWifiConfigMode' "$wifi_board"
grep -q 'WifiManager::GetInstance().IsConnected()' "$wifi_board"

# Voice enter must not drop Wi-Fi before BLE starts.
perl -0ne 'exit !/void WifiBoard::EnterWifiConfigModeForVoice\(\) \{.*?StartWifiConfigMode\(\);/s' "$wifi_board"
! perl -0ne 'exit !/void WifiBoard::EnterWifiConfigModeForVoice\(\) \{.*?StopStation\(\);/s' "$wifi_board"
perl -0ne 'exit !/void WifiBoard::ExitWifiConfigMode\(\) \{.*?YgSoulBleProvisioning::GetInstance\(\)\.Stop\(\);/s' "$wifi_board"

# MCP tools: no confirmation delay; exit is immediate.
grep -q 'self.system.reconfigure_wifi' "$wifi_board"
grep -q 'self.system.exit_wifi_config' "$wifi_board"
grep -q '进入配网模式' "$wifi_board"
grep -q '退出配网模式' "$wifi_board"
! grep -q 'You must ask the user to confirm this action' "$board_file"

# Notifications for both directions.
grep -q 'EXITING_WIFI_CONFIG_MODE' "$zh"
grep -q '退出配网模式' "$zh"
grep -q 'EXITING_WIFI_CONFIG_MODE' "$en"

echo "YGSoul voice wifi config contract passed"
