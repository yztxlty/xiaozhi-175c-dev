#!/usr/bin/env bash
set -euo pipefail

board_file="main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc"
cmake_file="main/CMakeLists.txt"

grep -q "YGSOUL_UI_BACKGROUND" "$board_file"
grep -q "ShowYGSoulCompanion" "$board_file"
grep -q "ShowYGSoulBootLogo" "$board_file"
grep -q "ygsoul_boot_lvgl.h" "$board_file"
grep -q "ygsoul_boot_" "$board_file"
grep -q "lv_anim_start" "$board_file"
! grep -q "SetCharacterOpacity" "$board_file"
grep -q "virtual void SetStatus" "$board_file"
grep -q "showing_boot_logo_" "$board_file"
grep -q "ApplyYGSoulOverlayStyle" "$board_file"
grep -q "ApplyYGSoulChatMessageColor" "$board_file"
grep -q "lv_color_hex(YGSOUL_UI_TEXT)" "$board_file"
grep -q 'strcmp(role, "system")' "$board_file"
grep -q 'lv_color_white()' "$board_file"
grep -q "ygsoul_companion_" "$board_file"
grep -q "ygsoul_companion_lvgl.h" "$board_file"
grep -q "ygsoul_image_" "$board_file"
grep -q "ygsoul_boot_image_" "$board_file"
grep -q "lv_obj_set_style_translate_y" "$board_file"
grep -q "lv_image_set_scale(ygsoul_image_, 220)" "$board_file"
grep -q "lv_anim_set_values(&scale_animation, 220, 226)" "$board_file"
! grep -q "lv_image_set_src(emoji_image_" "$board_file"
grep -q "LvglSourceImage" "$board_file"
! grep -q "GetAssetData(.*ygsoul_companion" "$board_file"
! grep -q "DEFAULT_ASSETS_EXTRA_FILES" "$cmake_file"
test -s "main/boards/waveshare/esp32-s3-touch-amoled-1.75/assets/ygsoul_companion.png"

if grep -q "SendStartListening\|SendStopListening\|SendAudio\|protocol_->" "$board_file"; then
    ! grep -q "SendStartListening\|SendStopListening\|SendAudio\|protocol_->" "$board_file"
fi

echo "YGSoul display contract passed"
