#!/usr/bin/env bash
set -euo pipefail

board_file="main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc"
cmake_file="main/CMakeLists.txt"
board_dir="main/boards/waveshare/esp32-s3-touch-amoled-1.75"
base_resource="$board_dir/ygsoul_companion_nomouth.c"
mouth_resources=(
    "$board_dir/ygsoul_mouth_1.c"
    "$board_dir/ygsoul_mouth_2.c"
    "$board_dir/ygsoul_mouth_3.c"
)

grep -q "YGSOUL_UI_BACKGROUND" "$board_file"
grep -q "ShowYGSoulCompanion" "$board_file"
grep -q "ShowYGSoulBootLogo" "$board_file"
grep -q "ygsoul_boot_lvgl.h" "$board_file"
grep -q "ygsoul_speaking_mouth_lvgl.h" "$board_file"
grep -q "ygsoul_boot_" "$board_file"
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
grep -q "ygsoul_mouth_image_" "$board_file"
grep -q "ygsoul_boot_image_" "$board_file"
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
grep -q "LvglSourceImage" "$board_file"
! grep -q "GetAssetData(.*ygsoul_companion" "$board_file"
! grep -q "DEFAULT_ASSETS_EXTRA_FILES" "$cmake_file"
test -s "main/boards/waveshare/esp32-s3-touch-amoled-1.75/assets/ygsoul_companion.png"
test -s "main/boards/waveshare/esp32-s3-touch-amoled-1.75/assets/ygsoul_companion_nomouth.png"
test -s "main/boards/waveshare/esp32-s3-touch-amoled-1.75/ygsoul_speaking_mouth_lvgl.h"
grep -q "LV_COLOR_FORMAT_RGB565A8" "main/boards/waveshare/esp32-s3-touch-amoled-1.75/ygsoul_companion_lvgl.h"
test "$(grep -c 'extern const lv_image_dsc_t ygsoul_mouth_' "main/boards/waveshare/esp32-s3-touch-amoled-1.75/ygsoul_speaking_mouth_lvgl.h")" -eq 3

grep -q '\.header.cf = LV_COLOR_FORMAT_RGB565A8' "$base_resource"
grep -q '\.header.w = 280' "$base_resource"
grep -q '\.header.h = 280' "$base_resource"
for resource in "${mouth_resources[@]}"; do
    grep -q '\.header.cf = LV_COLOR_FORMAT_RGB565A8' "$resource"
    grep -q '\.header.w = 38' "$resource"
    grep -q '\.header.h = 29' "$resource"
done

create_line="$(grep -n 'ygsoul_mouth_timer_ = lv_timer_create' "$board_file" | cut -d: -f1)"
failure_line="$(grep -n 'Failed to create YGSoul mouth timer' "$board_file" | cut -d: -f1)"
speaking_line="$(grep -n 'ygsoul_mouth_speaking_ = true' "$board_file" | cut -d: -f1)"
test "$create_line" -lt "$failure_line"
test "$failure_line" -lt "$speaking_line"

if grep -q "SendStartListening\|SendStopListening\|SendAudio\|protocol_->" "$board_file"; then
    ! grep -q "SendStartListening\|SendStopListening\|SendAudio\|protocol_->" "$board_file"
fi

echo "YGSoul display contract passed"
