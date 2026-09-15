import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).parents[1]
BOARD = ROOT / "main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc"


def test_watch_hand_math_steps_on_whole_seconds():
    source = r'''
#include <cassert>
#include <cmath>
#include <ctime>
#include "watch/watch_clock_math.h"
int main() {
    std::tm local{};
    local.tm_hour = 3;
    local.tm_min = 15;
    local.tm_sec = 30;
    const auto turns = ComputeWatchHandTurns(local);
    assert(std::abs(turns.seconds - 30.0 / 60.0) < 1e-9);
    assert(std::abs(turns.minutes - (15.0 + 30.0 / 60.0) / 60.0) < 1e-9);
    assert(std::abs(turns.hours - (3.0 + turns.minutes) / 12.0) < 1e-9);
}
'''
    with tempfile.TemporaryDirectory() as directory:
        main = Path(directory) / "main.cc"
        binary = Path(directory) / "check"
        main.write_text(source)
        subprocess.run(["c++", "-std=c++17", "-I", str(ROOT / "main"), str(main), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


def test_watch_hands_step_once_at_each_absolute_second_boundary():
    source = BOARD.read_text()
    watch = source[source.index("void UpdateWatchClock()") : source.index("lv_obj_t* CreateCover")]
    assert "gettimeofday(&now, nullptr)" in watch
    assert "watch_last_hand_second_ != now.tv_sec" in watch
    assert "watch_last_hand_second_ = now.tv_sec" in watch
    assert "ComputeWatchHandTurns(local)" in watch
    assert "now.tv_usec / 1000000.0" not in watch
    assert "lv_timer_create(WatchTimerCallback, 50, this)" in watch


def test_watch_has_centered_dynamic_data_and_no_ai_click_target():
    source = BOARD.read_text()
    watch = source[source.index("void UpdateWatchClock()") : source.index("lv_obj_t* CreateCover")]
    assert "GetStepCount" in watch
    assert '"-- 步"' in watch
    assert '"YG Soul"' in watch
    assert '"等待网络校时", 150, 48, 166, 46' in watch
    assert '"-- 步", 64, 98, 108, 100' in watch
    assert '"--%", 294, 98, 108, 100' in watch
    assert '"--:--", 128, 310, 210, 56' in watch
    assert '"YG Soul", 128, 358, 210, 36' in watch
    assert watch.index('"--:--", 128, 310, 210, 56') < watch.index('"YG Soul", 128, 358, 210, 36')
    assert "CreateWatchRegionLabel" in watch
    assert "WatchChatClickCallback" not in source
    assert "FONT_AWESOME_MICROCHIP_AI" not in watch
    assert "lv_button_create" not in source[source.index("void RenderWatch()") : source.index("lv_obj_t* CreateCover")]


def test_watch_page_is_not_preempted_by_automatic_conversation_updates():
    source = BOARD.read_text()
    transition = source[source.index("static void LauncherTransitionCompleted") : source.index("void AnimateLauncher")]
    emotion = source[source.index("virtual void SetEmotion") : source.index("void ShowUpdatedRoleImage")]
    role_refresh = source[source.index("void ShowUpdatedRoleImage") : source.index("virtual bool SetRoleImage")]
    role_image = source[source.index("virtual bool SetRoleImage") : source.index("virtual void SetStatus")]
    chat = source[source.index("virtual void SetChatMessage") : source.index("virtual void SetTheme")]
    download = source[source.index("void PrepareGalleryDownload") : source.index("void RefreshGallery")]
    assert "page() == YGSoulPage::kWatch" in transition
    assert "page() == YGSoulPage::kWatch" in emotion
    assert emotion.index("const bool conversation_ui") < emotion.index("page() == YGSoulPage::kWatch")
    assert "launcher_state_.page() == YGSoulPage::kWatch && conversation_ui" in emotion
    assert "lv_obj_move_foreground(launcher_panel_)" in emotion
    assert "page() == YGSoulPage::kWatch" in role_refresh
    assert role_image.count("page() != YGSoulPage::kWatch") >= 2
    assert "page() == YGSoulPage::kWatch" in chat
    assert "lv_obj_move_foreground(launcher_panel_)" in chat
    assert "const bool keep_watch = launcher_state_.page() == YGSoulPage::kWatch" in download
    assert "ReleaseLauncherTransition(keep_watch);" in download
    assert "StopWatchClock();" not in download


def test_physical_button_explicitly_exits_watch_before_starting_chat():
    source = BOARD.read_text()
    exit_watch = source[source.index("bool ExitWatchForPhysicalButton") : source.index("void PrepareGalleryDownload")]
    button = source[source.index("void InitializeButtons()") : source.index("void InitializeTouch")]
    assert "launcher_state_.page() != YGSoulPage::kWatch" in exit_watch
    assert "launcher_state_.Home();" in exit_watch
    assert "HideLauncher();" in exit_watch
    assert "display_->ExitWatchForPhysicalButton();" in button
    assert "const bool exited_watch = display_ != nullptr && display_->ExitWatchForPhysicalButton();" in button
    assert "if (exited_watch && app.GetDeviceState() != kDeviceStateIdle) return;" in button
    assert button.index("const bool exited_watch") < button.index("app.ToggleChatState();")
    long_press = button[button.index("boot_button_.OnLongPress") : button.index("#if CONFIG_USE_DEVICE_AEC")]
    assert long_press.index("ExitWatchForPhysicalButton") < long_press.index("EnterWifiConfigMode")


def test_board_reads_qmi8658_hardware_pedometer_without_fake_steps():
    board = BOARD.read_text()
    interface = (ROOT / "main/boards/common/board.h").read_text()
    assert "virtual bool GetStepCount(uint32_t& steps)" in interface
    assert "class Qmi8658Pedometer" in board
    assert "QMI8658_ADDRESS" in board and "0x6B" in board
    assert "QMI8658_STEP_CNT_LOW" in board and "0x5A" in board
    assert "WritePedometerPage(50, 200, 100, 0x0102)" in board
    assert "WritePedometerPage(200, 20 | (10 << 8), 0 | (4 << 8), 0x0202)" in board
    assert "UpdateBits(QMI8658_CTRL7, 0x01, 0x01)" in board
    assert "UpdateBits(QMI8658_CTRL8, 0x10, 0x10)" in board
    assert "virtual bool GetStepCount(uint32_t& steps) override" in board


def test_sensor_io_is_sampled_on_main_task_and_watch_reads_cache_only():
    app = (ROOT / "main/application.cc").read_text()
    board = BOARD.read_text()
    tick = app[app.index("if (bits & MAIN_EVENT_CLOCK_TICK)") : app.index("void Application::StartNetworkTimeSync()")]
    battery_getter = board[board.index("virtual bool GetBatteryLevel") : board.index("virtual bool GetStepCount")]
    step_getter = board[board.index("virtual bool GetStepCount") : board.index("virtual void RefreshDynamicData")]
    assert "Board::GetInstance().RefreshDynamicData();" in tick
    assert tick.index("RefreshDynamicData") < tick.index("UpdateStatusBar")
    assert "virtual void RefreshDynamicData() override" in board
    assert "pmic_->" not in battery_getter
    assert "pedometer_->Read" not in step_getter
    assert "battery_level_.load()" in battery_getter
    assert "step_count_.load()" in step_getter


def test_network_connection_starts_background_sntp_before_pairing_return():
    app = (ROOT / "main/application.cc").read_text()
    header = (ROOT / "main/application.h").read_text()
    sdkconfig = (ROOT / "sdkconfig.175c").read_text()
    handler = app[app.index("void Application::HandleNetworkConnectedEvent()") : app.index("void Application::HandleNetworkDisconnectedEvent()")]
    assert "StartNetworkTimeSync();" in handler
    assert handler.index("StartNetworkTimeSync();") < handler.index("if (state == kDeviceStateWifiConfiguring)")
    assert "void Application::StartNetworkTimeSync()" in app
    assert "ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(" in app
    assert '3, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org")' in app
    assert "config.smooth_sync = false" in app
    assert "config.sync_cb" in app
    assert "bool network_time_sync_started_ = false" in header
    assert "CONFIG_LWIP_SNTP_MAX_SERVERS=3" in sdkconfig
