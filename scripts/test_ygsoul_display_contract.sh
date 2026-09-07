#!/usr/bin/env bash
set -euo pipefail

board_file="main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc"
cmake_file="main/CMakeLists.txt"
board_dir="main/boards/waveshare/esp32-s3-touch-amoled-1.75"
cbin_dir="$board_dir/assets/cbin"

grep -q "YGSOUL_UI_BACKGROUND" "$board_file"
grep -q "ShowYGSoulCompanion" "$board_file"
grep -q "ShowYGSoulBootLogo" "$board_file"
! grep -q "SetCharacterOpacity" "$board_file"
grep -q "virtual void SetStatus" "$board_file"
grep -q "case kDeviceStateWifiConfiguring:[[:space:]]*$" main/application.cc
grep -A2 -q "case kDeviceStateWifiConfiguring:" main/application.cc
grep -A2 -q "display->SetStatus(Lang::Strings::WIFI_CONFIG_MODE);" main/application.cc
grep -q "showing_boot_logo_" "$board_file"
grep -q "conversation_ui" "$board_file"
grep -q "kDeviceStateListening" "$board_file"
grep -q "ApplyYGSoulOverlayStyle" "$board_file"
grep -q "ApplyYGSoulChatMessageColor" "$board_file"
grep -q "lv_color_hex(YGSOUL_UI_TEXT)" "$board_file"
grep -q 'strcmp(role, "system")' "$board_file"
grep -q 'lv_color_white()' "$board_file"
grep -q "ygsoul_companion_" "$board_file"
grep -q "ygsoul_image_" "$board_file"
grep -q "ygsoul_mouth_image_" "$board_file"
grep -q "SetYGSoulMouthFrame" "$board_file"
grep -q "StartYGSoulSpeakingAnimation" "$board_file"
grep -q "StopYGSoulSpeakingAnimation" "$board_file"
grep -q "lv_timer_create" "$board_file"
grep -q "YGSOUL_MOUTH_CLOSED" "$board_file"
grep -q "YGSOUL_MOUTH_FRAME_COUNT" "$board_file"
grep -q "static_assert" "$board_file"
grep -q 'Failed to create YGSoul mouth timer' "$board_file"
! grep -q "StartYGSoulMotion" "$board_file"
! grep -q "scale_animation" "$board_file"
! grep -q "lv_image_set_src(emoji_image_" "$board_file"
grep -q "LoadYGSoulAssets" "$board_file"
grep -q "LvglAllocatedImage" "$board_file"
grep -q "heap_caps_malloc" "$board_file"
grep -q 'memcmp(bytes, "YGI1", 4)' "$board_file"
grep -q "Assets::GetInstance" "$board_file"
grep -q 'ygsoul_companion_nomouth.cbin' "$board_file"
grep -q 'ygsoul_mouth_1.cbin' "$board_file"
grep -q 'ygsoul_mouth_2.cbin' "$board_file"
grep -q 'ygsoul_mouth_3.cbin' "$board_file"
! grep -q "LvglSourceImage" "$board_file"
grep -q "DEFAULT_ASSETS_EXTRA_FILES" "$cmake_file"
grep -q "assets/cbin" "$cmake_file"
test -s "main/boards/waveshare/esp32-s3-touch-amoled-1.75/assets/ygsoul_companion.png"
test -s "main/boards/waveshare/esp32-s3-touch-amoled-1.75/assets/ygsoul_companion_nomouth.png"
for resource in ygsoul_companion_nomouth ygsoul_mouth_1 ygsoul_mouth_2 ygsoul_mouth_3; do
    test -s "$cbin_dir/$resource.cbin"
done

test ! -e "$board_dir/ygsoul_boot_lvgl.h"
test ! -e "$board_dir/ygsoul_companion_nomouth.c"
test ! -e "$board_dir/ygsoul_mouth_1.c"
test ! -e "$board_dir/ygsoul_mouth_2.c"
test ! -e "$board_dir/ygsoul_mouth_3.c"

create_line="$(grep -n 'ygsoul_mouth_timer_ = lv_timer_create' "$board_file" | cut -d: -f1)"
failure_line="$(grep -n 'Failed to create YGSoul mouth timer' "$board_file" | cut -d: -f1)"
speaking_line="$(grep -n 'ygsoul_mouth_speaking_ = true' "$board_file" | cut -d: -f1)"
test "$create_line" -lt "$failure_line"
test "$failure_line" -lt "$speaking_line"

if grep -q "SendStartListening\|SendStopListening\|SendAudio\|protocol_->" "$board_file"; then
    ! grep -q "SendStartListening\|SendStopListening\|SendAudio\|protocol_->" "$board_file"
fi

echo "YGSoul display contract passed"
