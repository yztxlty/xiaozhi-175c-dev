import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).parents[1]


def test_launcher_gestures_and_gallery_loop():
    source = r'''
#include <cassert>
#include "ui/ygsoul_launcher_state.h"
int main() {
  YGSoulLauncherState state(3);
  assert(state.page() == YGSoulPage::kDesktop);
  state.Gesture(YGSoulGesture::kUp);
  assert(state.page() == YGSoulPage::kFeatures);
  state.OpenFeature(1);
  assert(state.page() == YGSoulPage::kGallery);
  state.Gesture(YGSoulGesture::kLeft);
  assert(state.gallery_index() == 1);
  state.Gesture(YGSoulGesture::kRight);
  assert(state.gallery_index() == 0);
  state.Gesture(YGSoulGesture::kRight);
  assert(state.gallery_index() == 2);
  state.Home();
  state.Gesture(YGSoulGesture::kDown);
  assert(state.page() == YGSoulPage::kSettings);
  state.Gesture(YGSoulGesture::kUp);
  assert(state.page() == YGSoulPage::kDesktop);
}
'''
    with tempfile.TemporaryDirectory() as directory:
        main = Path(directory) / 'main.cc'
        binary = Path(directory) / 'check'
        main.write_text(source)
        subprocess.run([
            'c++', '-std=c++17', '-I', str(ROOT / 'main'), str(main),
            str(ROOT / 'main/ui/ygsoul_launcher_state.cc'), '-o', str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


def test_every_launcher_module_has_a_gesture_exit():
    source = r'''
#include <cassert>
#include "ui/ygsoul_launcher_state.h"
int main() {
  YGSoulLauncherState state(3);
  state.Gesture(YGSoulGesture::kUp);
  assert(state.page() == YGSoulPage::kFeatures);
  state.Gesture(YGSoulGesture::kDown);
  assert(state.page() == YGSoulPage::kDesktop);

  state.Gesture(YGSoulGesture::kUp);
  state.OpenFeature(0);
  assert(state.page() == YGSoulPage::kMusic);
  state.Back();
  assert(state.page() == YGSoulPage::kFeatures);

  state.Gesture(YGSoulGesture::kUp);
  state.OpenFeature(1);
  assert(state.page() == YGSoulPage::kGallery);
  state.Gesture(YGSoulGesture::kLeft);
  assert(state.gallery_index() == 1);
  state.Gesture(YGSoulGesture::kDown);
  assert(state.page() == YGSoulPage::kDesktop);

  state.Gesture(YGSoulGesture::kUp);
  state.OpenFeature(2);
  assert(state.page() == YGSoulPage::kChat);
  state.Gesture(YGSoulGesture::kDown);
  assert(state.page() == YGSoulPage::kDesktop);

  state.Gesture(YGSoulGesture::kDown);
  assert(state.page() == YGSoulPage::kSettings);
  state.Gesture(YGSoulGesture::kUp);
  assert(state.page() == YGSoulPage::kDesktop);
}
'''
    with tempfile.TemporaryDirectory() as directory:
        main = Path(directory) / 'main.cc'
        binary = Path(directory) / 'check'
        main.write_text(source)
        subprocess.run([
            'c++', '-std=c++17', '-I', str(ROOT / 'main'), str(main),
            str(ROOT / 'main/ui/ygsoul_launcher_state.cc'), '-o', str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


def test_watch_entry_uses_a_second_feature_page_without_moving_existing_entries():
    source = r'''
#include <cassert>
#include "ui/ygsoul_launcher_state.h"
int main() {
  YGSoulLauncherState state;
  state.Gesture(YGSoulGesture::kUp);
  assert(state.page() == YGSoulPage::kFeatures);
  assert(state.feature_page() == 0);
  state.Gesture(YGSoulGesture::kLeft);
  assert(state.page() == YGSoulPage::kFeatures);
  assert(state.feature_page() == 1);
  state.OpenFeature(4);
  assert(state.page() == YGSoulPage::kWatch);
  state.Back();
  assert(state.page() == YGSoulPage::kFeatures);
  assert(state.feature_page() == 1);
  state.Gesture(YGSoulGesture::kRight);
  assert(state.feature_page() == 0);
}
'''
    with tempfile.TemporaryDirectory() as directory:
        main = Path(directory) / 'main.cc'
        binary = Path(directory) / 'check'
        main.write_text(source)
        subprocess.run([
            'c++', '-std=c++17', '-I', str(ROOT / 'main'), str(main),
            str(ROOT / 'main/ui/ygsoul_launcher_state.cc'), '-o', str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


def test_round_ui_keeps_the_original_grid_and_adds_an_independent_watch_page():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    features = board[board.index('void RenderFeatures()'):board.index('lv_obj_t* CreateCover')]
    assert '78 + (index % 2) * 162' in features
    assert '118 + (index / 2) * 134' in features
    assert 'launcher_state_.feature_page() != 0' in features
    assert 'FONT_AWESOME_WATCH' in features
    assert '"智能手表"' in features
    assert 'void RenderWatch()' in board
    assert 'case YGSoulPage::kWatch: RenderWatch(); break;' in board
    assert 'GetBatteryLevel' in board[board.index('void UpdateWatchClock()'):board.index('lv_obj_t* CreateCover')]


def test_network_starts_native_time_sync_without_changing_the_existing_ota_flow():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    app = (ROOT / 'main/application.cc').read_text()
    watch = board[board.index('void UpdateWatchClock()'):board.index('lv_obj_t* CreateCover')]
    assert 'esp_netif_sntp_init' in app
    assert 'ESP_NETIF_SNTP_DEFAULT_CONFIG' in app
    assert 'esp_netif_sntp_init' not in watch
    assert 'Ota' not in watch


def test_visible_back_button_and_launcher_gestures_do_not_interrupt_ai_runtime():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    gesture_callback = board[board.index('static void GestureCallback'):board.index('void CreateLauncher()')]
    feature_callback = board[board.index('static void FeatureClickCallback'):board.index('static void BackClickCallback')]
    assert 'lv_obj_add_event_cb(back, BackClickCallback, LV_EVENT_CLICKED, this);' in board
    assert 'lv_label_set_text(label, FONT_AWESOME_ANGLE_LEFT);' in board
    assert 'ToggleChatState' not in gesture_callback
    assert 'if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {' in gesture_callback
    assert 'self->HideLauncher();' in gesture_callback
    assert 'ToggleChatState' not in feature_callback


def test_music_frame_parser_rejects_stale_and_truncated_frames():
    source = r'''
#include <cassert>
#include <cstdint>
#include <vector>
#include "music/music_frame.h"
int main() {
  std::vector<uint8_t> frame = {
    'Y','G','M','1', 1,1, 0,0,
    0,0,0,2, 0,0,0,9,
    0,0,0,0,0,0,0,7,
    0,0,0,3, 1,2,3
  };
  MusicFrameView parsed{};
  assert(ParseMusicFrame(frame.data(), frame.size(), 2, parsed));
  assert(parsed.epoch == 2 && parsed.sequence == 9 && parsed.pts_ms == 7);
  assert(parsed.payload_size == 3 && parsed.payload[2] == 3 && !parsed.end);
  assert(!ParseMusicFrame(frame.data(), frame.size(), 3, parsed));
  frame.pop_back();
  assert(!ParseMusicFrame(frame.data(), frame.size(), 0, parsed));
}
'''
    with tempfile.TemporaryDirectory() as directory:
        main = Path(directory) / 'main.cc'
        binary = Path(directory) / 'check'
        main.write_text(source)
        subprocess.run([
            'c++', '-std=c++17', '-I', str(ROOT / 'main'), str(main),
            str(ROOT / 'main/music/music_frame.cc'), '-o', str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


def test_voice_state_preempts_independent_music_without_ai_runtime_dependency():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    client = (ROOT / 'main/music/music_client.cc').read_text()
    music = '\n'.join(path.read_text() for path in (ROOT / 'main/music').glob('*.[ch]*'))
    assert 'music_client_->SetBusy(state != kDeviceStateIdle)' in board
    assert 'music.playback.stop' in client
    assert 'music.availability' in client
    for forbidden in ('protocols/xiaozhi_protocol', 'audio_service.h', 'voice_runtime', 'tts', 'asr'):
        assert forbidden not in music.lower()


def test_music_network_worker_starts_after_network_stack_and_survives_launcher_navigation():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    setup_ui = board[board.index('virtual void SetupUI() override'):board.index('virtual void SetEmotion')]
    feature_callback = board[board.index('static void FeatureClickCallback'):board.index('static void BackClickCallback')]
    hide_launcher = board[board.index('void HideLauncher()'):board.index('static void GestureCallback')]
    set_emotion = board[board.index('virtual void SetEmotion'):board.index('virtual void SetChatMessage')]
    assert 'StartMusicClient();' not in setup_ui
    assert 'self->StartMusicClient();' in feature_callback
    assert 'music_client_->Stop()' not in hide_launcher
    assert 'launcher_state_.Home();\n            HideLauncher();' in set_emotion


def test_music_worker_cannot_duplicate_and_decode_stack_uses_psram():
    client = (ROOT / 'main/music/music_client.cc').read_text()
    player = (ROOT / 'main/music/music_player.cc').read_text()
    start = client[client.index('void MusicClient::Start()'):client.index('void MusicClient::Stop()')]
    assert 'task_ != nullptr' in start
    run = client[client.index('void MusicClient::Run'):]
    assert 'xTaskCreateWithCaps' in client
    assert 'xTaskCreateWithCaps' in player
    assert 'MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT' in client
    assert 'MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT' in player
    assert 'self->ReloadCredentials();' in run
    assert 'xTaskNotifyGive(task_)' in client
    assert 'vTaskDeleteWithCaps(nullptr)' in client
    assert 'vTaskDeleteWithCaps(nullptr)' in player


def test_every_forced_launcher_close_releases_music_and_gallery():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    assert 'launcher_state_.Home();\n            StopGalleryAnimation();' not in board


def test_music_playlist_uses_vertical_selection_and_tap_opens_player():
    source = r'''
#include <cassert>
#include "ui/ygsoul_launcher_state.h"
int main() {
  YGSoulLauncherState state(3, 8);
  state.Gesture(YGSoulGesture::kUp);
  state.OpenFeature(0);
  assert(state.page() == YGSoulPage::kMusicPlaylist);
  state.Gesture(YGSoulGesture::kUp);
  assert(state.music_index() == 1);
  state.Gesture(YGSoulGesture::kDown);
  assert(state.music_index() == 0);
  state.OpenMusicPlayer();
  assert(state.page() == YGSoulPage::kMusicPlayer);
  state.Back();
  assert(state.page() == YGSoulPage::kMusicPlaylist);
  state.Back();
  assert(state.page() == YGSoulPage::kFeatures);
}
'''
    with tempfile.TemporaryDirectory() as directory:
        main = Path(directory) / 'main.cc'
        binary = Path(directory) / 'check'
        main.write_text(source)
        subprocess.run([
            'c++', '-std=c++17', '-I', str(ROOT / 'main'), str(main),
            str(ROOT / 'main/ui/ygsoul_launcher_state.cc'), '-o', str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


def test_playlist_cards_have_single_line_ellipsis_and_relaxed_spacing():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    playlist = board[board.index('void RenderMusicPlaylist()'):board.index('void RenderMusicDisconnected')]
    assert 'CreateLauncherButton("", 73, 110 + row * 70, 320, 66)' in playlist
    assert 'lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);' in playlist
    assert 'lv_label_set_long_mode(artist, LV_LABEL_LONG_MODE_DOTS);' in playlist
    assert 'lv_obj_set_style_text_font(artist, &font_puhui_16_4, 0);' in playlist


def test_failed_ota_restores_idle_state_before_accepting_another_check():
    source = (ROOT / 'main/application.cc').read_text()
    failed = source[source.index('if (!upgrade_success) {'):source.index('} else {', source.index('if (!upgrade_success) {'))]
    assert 'SetDeviceState(kDeviceStateIdle);' in failed


def test_round_ui_contains_design_components_and_safe_back_control():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    for token in (
        'FONT_AWESOME_MUSIC', 'FONT_AWESOME_IMAGE', 'FONT_AWESOME_MICROCHIP_AI',
        'FONT_AWESOME_GEAR', 'RenderMusicPlaylist', 'RenderMusicPlayer',
        'RenderMusicDisconnected', 'ygsoul_music_cover.cbin', '播放中', '连接中断',
        '上下滑动选择  ·  轻触播放',
    ):
        assert token in board
    assert 'lv_obj_set_pos(back, 36, 32);' not in board
    assert 'lv_obj_set_pos(back, 96, 56);' in board


def test_player_controls_and_settings_labels_stay_inside_round_safe_area():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    assert 'CreateLauncherButton("", 126, 382, 50, 50)' in board
    assert 'CreateLauncherButton("", 201, 370, 64, 64)' in board
    assert 'CreateLauncherButton("", 290, 382, 50, 50)' in board
    assert 'lv_obj_set_style_bg_opa(previous, LV_OPA_TRANSP, 0);' in board
    assert 'lv_obj_set_style_bg_opa(next, LV_OPA_TRANSP, 0);' in board
    assert 'lv_obj_set_style_transform_scale(previous_icon, 384, 0);' in board
    assert 'lv_obj_set_style_transform_scale(next_icon, 384, 0);' in board
    assert 'lv_obj_set_style_pad_all(button, 0, 0);' in board
    assert 'CreateSmallLabel(wifi, "Wi-Fi"' in board
    assert 'CreateSmallLabel(power, "省电模式"' in board
    assert 'lv_obj_remove_style_all(back);' in board


def test_playlist_swipe_is_local_and_avoids_expensive_glow_layers():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    gesture = board[board.index('static void GestureCallback'):board.index('void CreateLauncher()')]
    assert 'music_client_->Select' not in gesture
    assert 'lv_obj_set_style_shadow_width(button, 0, 0);' in board
    assert 'lv_obj_set_style_shadow_width(star, 0, 0);' in board


def test_playlist_swipe_consumes_release_before_card_click():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    gesture = board[board.index('static void GestureCallback'):board.index('void CreateLauncher()')]
    assert 'before == YGSoulPage::kMusicPlaylist' in gesture
    assert 'lv_indev_wait_release(indev);' in gesture
    assert gesture.index('lv_indev_wait_release(indev);') < gesture.index('self->launcher_state_.Gesture(gesture);')


def test_launcher_transition_moves_one_temporary_snapshot_instead_of_full_widget_tree():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    animate = board[board.index('void AnimateLauncher'):board.index('void StopGalleryAnimation')]
    gesture = board[board.index('static void GestureCallback'):board.index('void CreateLauncher()')]
    assert 'MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT' in animate
    assert 'lv_snapshot_take_to_draw_buf(launcher_panel_, LV_COLOR_FORMAT_RGB565' in animate
    assert 'lv_image_set_src(launcher_transition_image_, &launcher_transition_buffer_);' in animate
    assert 'lv_anim_set_var(&animation, launcher_transition_image_);' in animate
    assert 'lv_anim_set_var(&animation, launcher_panel_);' not in animate
    assert 'if (self->launcher_transitioning_) return;' in gesture


def test_feature_icons_are_large_and_share_the_same_center_axis():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    features = board[board.index('void RenderFeatures()'):board.index('lv_obj_t* CreateCover')]
    assert 'auto icon = CreateIcon(launcher_buttons_[index], icons[index], 0, 0, 80' in features
    assert 'lv_obj_set_style_transform_scale(icon, 320, 0);' in features
    assert 'lv_obj_align(icon, LV_ALIGN_TOP_MID, -8, 12);' in features
    assert 'auto label = CreateSmallLabel(launcher_buttons_[index], labels[index], 0, 0, 126' in features
    assert 'lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 72);' in features


def test_unset_clock_warning_is_rate_limited_instead_of_spamming_every_frame():
    display = (ROOT / 'main/display/lvgl_display/lvgl_display.cc').read_text()
    invalid = display[display.index('if (tm->tm_year >='):display.index('esp_pm_lock_acquire')]
    assert 'last_status_update_time_ = std::chrono::system_clock::now();' in invalid


def test_music_pause_gates_local_output_instead_of_only_stopping_network_input():
    client = (ROOT / 'main/music/music_client.cc').read_text()
    player = (ROOT / 'main/music/music_player.cc').read_text()
    assert 'player_.Pause();' in client
    assert 'player_.Resume();' in client
    output = player[player.index('void MusicPlayer::Output('):player.index('void MusicPlayer::QueueState(')]
    assert 'self->paused_' in output
    assert output.index('self->paused_') < output.index('codec->OutputData(pcm)')


def test_player_has_real_seek_dynamic_time_and_play_only_wave_animation():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    client_h = (ROOT / 'main/music/music_client.h').read_text()
    client_cc = (ROOT / 'main/music/music_client.cc').read_text()
    player = board[board.index('void RenderMusicPlayer()'):board.index('void RenderGallery()')]
    assert 'lv_slider_create(launcher_content_)' in player
    assert 'lv_obj_set_ext_click_area(progress, 20);' in player
    assert 'MusicSeekCallback' in player
    assert 'LV_EVENT_RELEASED' in player
    assert 'music_progress_timer_ = lv_timer_create(MusicProgressTimerCallback, 180, this);' in player
    assert 'state == MusicUiState::kPlaying' in board[board.index('static void MusicProgressTimerCallback'):board.index('void RenderFeatures()')]
    assert 'bool Seek(uint32_t position_ms);' in client_h
    assert 'music.playback.seek' in client_cc


def test_player_titles_are_single_line_and_sections_have_breathing_room():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    player = board[board.index('void RenderMusicPlayer()'):board.index('void RenderGallery()')]
    assert 'lv_obj_set_height(title, 34);' in player
    assert 'lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);' in player
    assert 'lv_obj_set_height(artist, 22);' in player
    assert 'lv_label_set_long_mode(artist, LV_LABEL_LONG_MODE_DOTS);' in player
    assert 'lv_obj_set_pos(progress, 96, 342);' in player
