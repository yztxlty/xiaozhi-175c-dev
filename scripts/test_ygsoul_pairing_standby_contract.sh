#!/usr/bin/env bash
set -euo pipefail

app="main/application.cc"
wifi_board="main/boards/common/wifi_board.cc"
ble="main/boards/common/ygsoul_ble_provisioning.cc"
board="main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc"

# Stay in real pairing until 结束配网 / 退出配网.
grep -q '结束配网' "$app"
grep -q '退出配网' "$app"
! grep -q 'Wi-Fi rejoined, report online and enter listening' "$app"
! perl -0ne 'exit !/kDeviceStateWifiConfiguring && management_client_.*?EnterConversationListening/s' "$app"

# Keep station Wi-Fi when already connected; long-press uses the same enter path.
perl -0ne 'exit !/void WifiBoard::EnterWifiConfigMode\(\) \{.*?if \(WifiManager::GetInstance\(\)\.IsConnected\(\)\) \{.*?StartWifiConfigMode\(\);.*?return;/s' "$wifi_board"
grep -q 'EnterWifiConfigMode();' "$board"
grep -q 'boot_button_\.OnLongPress' "$board"

# BLE advertising survives Wi-Fi Connected; stop only on explicit exit.
! perl -0ne 'exit !/OnNetworkEvent\(NetworkEvent event.*?Connected.*?Stop\(\);/s' "$ble"
perl -0ne 'exit !/void WifiBoard::ExitWifiConfigMode\(\) \{.*?YgSoulBleProvisioning::GetInstance\(\)\.Stop\(\);/s' "$wifi_board"
grep -q 'EnsureAdvertising' "$ble"
grep -q 'EnsureAdvertising' "$app"

# Activation / pairing exit goes to standby, not auto chat.
grep -q 'EnterStandby' "$app"
! perl -0ne 'exit !/void Application::HandleActivationDoneEvent\(\) \{.*?EnterConversationListening/s' "$app"

# Standby: wake word only; dismiss closes audio so cloud VAD cannot barge in.
perl -0ne 'exit !/void Application::EnterStandby\(\) \{.*?CloseAudioChannel\(\);.*?EnableWakeWordDetection\(true\);/s' "$app"
grep -q 'Standby ignores button chat' "$app"

# Super power save after 1 minute standby; function key returns to standby.
grep -q 'seconds_to_sleep = 60\|PowerSaveTimer(-1, 60' "$board"
grep -q 'EnterSuperPowerSave' "$board"
grep -q 'EnableWakeWordDetection(false)' "$board"
grep -q 'ExitSuperPowerSave' "$board"

echo "YGSoul pairing/standby contract passed"
