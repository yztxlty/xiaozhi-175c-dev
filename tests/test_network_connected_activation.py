"""直接编译联网事件处理函数，复现退出配网先于联网通知的顺序。"""
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_wifi_connected_after_standby_starts_activation_without_boot(tmp_path):
    source = (ROOT / "main/application.cc").read_text(encoding="utf-8")
    handler = source.split("void Application::HandleNetworkConnectedEvent()", 1)[1]
    handler = handler.split("void Application::HandleActivationDoneEvent()", 1)[0]
    harness = r'''
#include <cassert>
#define ESP_LOGI(...)
enum State { kDeviceStateStarting, kDeviceStateWifiConfiguring,
             kDeviceStateConnecting, kDeviceStateIdle,
             kDeviceStateListening, kDeviceStateSpeaking };
struct Display { void UpdateStatusBar(bool) {} };
struct Protocol { bool closed = false; void CloseAudioChannel() { closed = true; } };
struct Board {
    static Board& GetInstance() { static Board board; return board; }
    Display* GetDisplay() { static Display display; return &display; }
};
struct Application {
    State state;
    Protocol* protocol_;
    void* management_client_;
    int activations = 0;
    State GetDeviceState() { return state; }
    void SetDeviceState(State next) { state = next; }
    void StartActivationIfNeeded() { ++activations; }
    void ReportDeviceUplink(const char*, const char* = nullptr) {}
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
};
void Application::HandleNetworkConnectedEvent()
'''+handler+r'''
int main() {
    // 配网退出已执行：待命不代表服务已经初始化。
    Application idle{kDeviceStateIdle, nullptr, nullptr};
    idle.HandleNetworkConnectedEvent();
    assert(idle.activations == 1);
    // 初始化尚未建立协议就断网，不应解引用空指针。
    idle.HandleNetworkDisconnectedEvent();
    for (auto state : {kDeviceStateStarting}) {
        Application fresh{state, nullptr, nullptr};
        fresh.HandleNetworkConnectedEvent();
        assert(fresh.activations == 1);
    }
    // 收到旧 Wi-Fi/迟到联网事件不代表本次配网已成功。
    Application pairing{kDeviceStateWifiConfiguring, nullptr, nullptr};
    pairing.HandleNetworkConnectedEvent();
    assert(pairing.state == kDeviceStateWifiConfiguring);
    assert(pairing.activations == 0);
    // 成功回执退出配网后再次派发联网事件，才允许初始化协议。
    pairing.state = kDeviceStateIdle;
    pairing.HandleNetworkConnectedEvent();
    assert(pairing.activations == 1);
    // 已验收的待命/聆听/说话在重连时不得重新初始化或改变状态。
    Protocol existing;
    for (auto state : {kDeviceStateIdle, kDeviceStateListening, kDeviceStateSpeaking}) {
        Application reconnect{state, &existing, &existing};
        reconnect.HandleNetworkConnectedEvent();
        assert(reconnect.activations == 0);
        assert(reconnect.state == state);
    }
    Application listening{kDeviceStateListening, &existing, &existing};
    listening.HandleNetworkDisconnectedEvent();
    assert(existing.closed);
}
'''
    executable = tmp_path / "network-connected-check"
    compile_result = subprocess.run(
        ["c++", "-std=c++17", "-include", "initializer_list", "-x", "c++", "-", "-o", str(executable)],
        input=harness, text=True, capture_output=True,
    )
    assert compile_result.returncode == 0, compile_result.stderr
    result = subprocess.run([str(executable)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
