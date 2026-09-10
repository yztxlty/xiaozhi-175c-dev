"""编译实际配网入口，检查 NVS 键长度和重启前持久化顺序。"""
import pathlib
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_pairing_flag_is_committed_before_reset(tmp_path):
    wifi = (ROOT / 'main/boards/common/wifi_board.cc').read_text()
    settings = (ROOT / 'main/settings.cc').read_text()
    constants = '\n'.join(re.findall(r'static constexpr char kPairing\w+\[\].*;', wifi))
    entry = wifi.split('void WifiBoard::EnterWifiConfigMode()', 1)[1]
    entry = entry.split('    auto& app =', 1)[0] + '\n}'
    destructor = settings.split('Settings::~Settings()', 1)[1].split('std::string Settings::GetString', 1)[0]
    setter = settings.split('void Settings::SetBool', 1)[1].split('void Settings::EraseKey', 1)[0]
    harness = r'''
#include <cassert>
#include <cstring>
#include <string>
#include <stdexcept>
#define ESP_LOGI(...)
#define ESP_LOGW(...)
#define ESP_ERROR_CHECK(expr) assert((expr) == 0)
bool pending = false, committed = false;
int nvs_set_u8(int, const char* key, unsigned char value) {
    if (std::strlen(key) > 15) return 0x1109;
    pending = value; return 0;
}
int nvs_commit(int) { committed = pending; return 0; }
void nvs_close(int) {}
struct Settings {
    int nvs_handle_ = 1;
    bool read_write_ = true, dirty_ = false;
    std::string ns_;
    Settings(const char*, bool) {}
    ~Settings();
    void SetBool(const std::string&, bool);
};
Settings::~Settings()
''' + destructor + '\nvoid Settings::SetBool' + setter + r'''
struct Application {
    static Application& GetInstance() { static Application app; return app; }
    void Reboot() {
        // 必须在发生 reset 的时刻检查，不能等栈展开后才检查提交。
        assert(committed);
        throw std::runtime_error("simulated reset");
    }
};
struct WifiManager {
    static WifiManager& GetInstance() { static WifiManager wifi; return wifi; }
    bool IsConnected() { return true; }
};
namespace Lang { namespace Strings { const char* ENTERING_WIFI_CONFIG_MODE = "pairing"; } }
struct Display { void ShowNotification(const char*) {} };
struct WifiBoard {
    bool IsInWifiConfigMode() { return false; }
    Display* GetDisplay() { static Display display; return &display; }
    void EnterWifiConfigMode();
};
''' + constants + '\nvoid WifiBoard::EnterWifiConfigMode()' + entry + r'''
int main() {
    WifiBoard board;
    try { board.EnterWifiConfigMode(); assert(false); }
    catch (const std::runtime_error&) { assert(committed); }
}
'''
    executable = tmp_path / 'pairing-reset-check'
    built = subprocess.run(['c++', '-std=c++17', '-x', 'c++', '-', '-o', str(executable)],
                           input=harness, text=True, capture_output=True)
    assert built.returncode == 0, built.stderr
    result = subprocess.run([str(executable)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
