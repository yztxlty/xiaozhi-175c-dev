"""编译运行实际 BOOT 回调，保护播放器熄屏后的首次按键语义。"""
from pathlib import Path
import subprocess


ROOT = Path(__file__).parents[1]
BOARD = ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc'


def test_actual_boot_callback_wakes_player_screen_before_chat(tmp_path):
    source = BOARD.read_text()
    method = source[source.index('    void InitializeButtons() {'):source.index('    void InitializeDisplay() {')]
    program = r'''
#include <cassert>
#include <functional>
#define ESP_LOGI(...)
    enum { kDeviceStateStarting, kDeviceStateIdle, kDeviceStateListening };
struct Application {
    int state = kDeviceStateIdle, toggles = 0;
    static Application& GetInstance() { static Application app; return app; }
    int GetDeviceState() { return state; }
    void ToggleChatState() { ++toggles; }
};
struct Button {
    std::function<void()> click;
    void OnClick(std::function<void()> callback) { click = callback; }
    void OnLongPress(std::function<void()>) {}
};
    struct Display {
        bool music = true;
        bool watch = false;
        int watch_exits = 0;
        bool IsMusicPlayerPage() { return music; }
        bool ExitWatchForPhysicalButton() { if (!watch) return false; watch=false; ++watch_exits; return true; }
};
struct TestBoard {
    Button boot_button_;
    Display display_storage_;
    Display* display_ = &display_storage_;
    bool super_power_save_ = true;
    int wakes = 0, pairing = 0;
    void ExitSuperPowerSave() { super_power_save_ = false; ++wakes; }
    void EnterWifiConfigMode() { ++pairing; }
''' + method + r'''
};
int main() {
    auto& app = Application::GetInstance();
    TestBoard board;
    board.InitializeButtons();
    board.boot_button_.click();
    assert(!board.super_power_save_ && board.wakes == 1);
    assert(app.toggles == 0); // 播放器熄屏：首次只亮屏。
    board.boot_button_.click();
    assert(app.toggles == 1); // 已亮屏：再次主动聆听。
    board.display_storage_.music = false;
    board.super_power_save_ = true;
        board.boot_button_.click();
        assert(board.wakes == 2 && app.toggles == 2); // 其他页面保持原规则。
        app.state = kDeviceStateListening;
        board.display_storage_.watch = true;
        board.boot_button_.click();
        assert(board.display_storage_.watch_exits == 1 && app.toggles == 2); // 对话已运行时只显露对话，不反向终止。
        app.state = kDeviceStateStarting;
    board.boot_button_.click();
    assert(board.pairing == 1 && app.toggles == 2); // 原启动配网入口保持。
}
'''
    cpp = tmp_path / 'boot.cc'
    cpp.write_text(program)
    binary = tmp_path / 'boot'
    subprocess.run(['c++', '-std=c++17', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_player_query_reads_actual_launcher_page_under_display_lock():
    source = BOARD.read_text()
    start = source.index('    bool IsMusicPlayerPage() {')
    query = source[start:source.index('\n    }', start)]
    assert 'DisplayLockGuard lock(this)' in query
    assert 'launcher_state_.page() == YGSoulPage::kMusicPlayer' in query


def test_actual_role_image_updates_preserve_music_player(tmp_path):
    source = BOARD.read_text()
    method = source[source.index('    virtual bool SetRoleImage('):source.index('    virtual void SetStatus(')]
    method = method.replace(' override', '')
    helper = ''
    if '    void ShowUpdatedRoleImage() {' in source:
        start = source.index('    void ShowUpdatedRoleImage() {')
        helper = source[start:source.index('\n    }', start) + 6]
    program = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
constexpr int MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2, ESP_OK=0;
constexpr int DISPLAY_WIDTH=466, DISPLAY_HEIGHT=466, LV_COLOR_FORMAT_RGB565=1, LV_OBJ_FLAG_HIDDEN=1;
using esp_err_t=int;
struct lv_img_dsc_t { struct { int w=1, h=1; } header; };
inline bool paused=false, foreground=false;
inline void* heap_caps_malloc(size_t size,int) { return std::malloc(size); }
inline void heap_caps_free(void* p) { std::free(p); }
inline void lv_image_set_src(void*, const void*) {}
inline void lv_obj_clear_flag(void*,int) {}
inline void lv_obj_add_flag(void*,int) {}
inline void lv_obj_center(void*) {}
inline void lv_image_set_scale(void*,unsigned) {}
inline void* lv_screen_active() { return nullptr; }
inline void* lv_eaf_create(void*) { return reinterpret_cast<void*>(4); }
inline void lv_eaf_set_src(void*,const void*) {}
inline void lv_eaf_set_loop_count(void*,int) {}
inline bool lv_eaf_is_loaded(void*) { return true; }
inline const void* lv_image_get_src(void*) { static lv_img_dsc_t image; return &image; }
inline void lv_eaf_pause(void*) { paused=true; }
inline void lv_obj_move_foreground(void*) { foreground=true; }
inline int jpeg_to_image(const uint8_t*,size_t,uint8_t** data,size_t* size,size_t* width,size_t* height,size_t* stride) {
    *data=static_cast<uint8_t*>(std::malloc(2)); *size=2; *width=*height=1; *stride=2; return ESP_OK;
}
struct LvglAllocatedImage {
    void* data=nullptr;
    LvglAllocatedImage()=default;
    LvglAllocatedImage(void* p,size_t,size_t,size_t,size_t,int):data(p) {}
    ~LvglAllocatedImage() { std::free(data); }
    const void* image_dsc() { return nullptr; }
};
struct LvglRawImage {
    LvglRawImage(void*,size_t) {}
    const void* image_dsc() { return nullptr; }
};
struct DisplayLockGuard {
    template<class T> DisplayLockGuard(T*) {}
    operator bool() const { return true; }
};
    enum class YGSoulPage { kMusicPlayer, kDesktop, kGallery, kWatch };
struct Launcher {
    YGSoulPage current=YGSoulPage::kMusicPlayer;
    YGSoulPage page() const { return current; }
    void Home() { current=YGSoulPage::kDesktop; }
};
struct TestDisplay {
    Launcher launcher_state_;
    void* ygsoul_image_=reinterpret_cast<void*>(1);
    void* ygsoul_mouth_image_=reinterpret_cast<void*>(2);
    void* launcher_panel_=reinterpret_cast<void*>(3);
    void* ygsoul_role_animation_=nullptr;
    void* ygsoul_role_animation_bytes_=nullptr;
    std::unique_ptr<LvglRawImage> ygsoul_role_animation_asset_;
    std::unique_ptr<LvglAllocatedImage> ygsoul_companion_;
    bool custom_role_image_=false;
    bool showing_boot_logo_=false;
    int hidden=0, companion_shown=0;
    ~TestDisplay() { std::free(ygsoul_role_animation_bytes_); }
    bool IsSetupUICalled() { return true; }
    std::unique_ptr<LvglAllocatedImage> LoadYGSoulAsset(const char*) { return std::make_unique<LvglAllocatedImage>(); }
    void StopRoleAnimation() {}
    void StopYGSoulSpeakingAnimation() {}
    void ShowYGSoulCompanion() { ++companion_shown; }
    void HideLauncher() { ++hidden; ShowYGSoulCompanion(); }
''' + helper + method + r'''
};
int main(int argc,char** argv) {
    for (const char* format : {"default", "jpg", "eaf"}) {
        TestDisplay display;
        paused=foreground=false;
        const char* path=std::strcmp(format,"default")==0 ? "" : argv[1];
        assert(display.SetRoleImage(path,format));
        assert(display.launcher_state_.page()==YGSoulPage::kMusicPlayer);
            assert(display.hidden==0 && display.companion_shown==0);
            assert(foreground);
            if (std::strcmp(format,"eaf")==0) assert(paused);
            TestDisplay watch;
            watch.launcher_state_.current=YGSoulPage::kWatch;
            foreground=false;
            assert(watch.SetRoleImage(path,format));
            assert(watch.launcher_state_.page()==YGSoulPage::kWatch);
            assert(watch.hidden==0 && watch.companion_shown==0 && foreground);
            TestDisplay other;
        other.launcher_state_.current=YGSoulPage::kGallery;
        assert(other.SetRoleImage(path,format));
        assert(other.companion_shown>0); // 非音乐页面保留原角色展示。
    }
}
'''
    cpp = tmp_path / 'role.cc'
    cpp.write_text(program)
    fixture = tmp_path / 'image.bin'
    fixture.write_bytes(b'jpeg-or-eaf-hardware-fixture')
    binary = tmp_path / 'role'
    subprocess.run(['c++', '-std=c++17', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(fixture)], check=True)


def test_actual_sleep_and_wake_never_restart_player_wake_model(tmp_path):
    source = BOARD.read_text()
    methods = source[source.index('    void EnterSuperPowerSave() {'):source.index('    void HandlePwrButton() {')]
    program = r'''
#include <cassert>
#define ESP_LOGI(...)
struct Audio {
    bool wake=false;
    void EnableVoiceProcessing(bool) {}
    void EnableWakeWordDetection(bool enabled) { wake=enabled; }
};
struct Application {
    Audio audio;
    static Application& GetInstance() { static Application app; return app; }
    Audio& GetAudioService() { return audio; }
};
struct Display {
    bool music=true;
    bool IsMusicPlayerPage() { return music; }
    void SetPowerSaveMode(bool) {}
};
struct Backlight { void SetBrightness(int) {} void RestoreBrightness() {} };
struct Timer { void WakeUp() {} };
struct TestBoard {
    bool super_power_save_=false;
    Display display_storage_;
    Display* display_=&display_storage_;
    Backlight light;
    Timer timer;
    Timer* power_save_timer_=&timer;
    Display* GetDisplay() { return display_; }
    Backlight* GetBacklight() { return &light; }
''' + methods + r'''
};
int main() {
    TestBoard board;
    auto& audio=Application::GetInstance().audio;
    board.EnterSuperPowerSave();
    assert(board.super_power_save_ && !audio.wake);
    board.ExitSuperPowerSave();
    assert(!board.super_power_save_ && !audio.wake);
    board.display_storage_.music=false;
    board.EnterSuperPowerSave();
    assert(board.super_power_save_ && audio.wake);
    board.ExitSuperPowerSave();
    assert(!board.super_power_save_ && audio.wake);
}
'''
    cpp = tmp_path / 'power.cc'
    cpp.write_text(program)
    binary = tmp_path / 'power'
    subprocess.run(['c++', '-std=c++17', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
