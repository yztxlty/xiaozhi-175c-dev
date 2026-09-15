from pathlib import Path
import subprocess


ROOT = Path(__file__).parents[1]


def test_standby_and_recovery_do_not_restart_wake_detector_over_player():
    source = (ROOT / 'main/application.cc').read_text()
    assert 'audio_service_.EnableWakeWordDetection(true)' not in source


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def test_unsolicited_voice_cannot_leave_standby(tmp_path):
    """运行实际状态入口，迟到 TTS 不能重新激活待命设备。"""
    source = (ROOT / 'main/application.cc').read_text()
    method = function(source, 'bool Application::SetDeviceState(')
    program = '''
#include <atomic>
#include <cassert>
enum DeviceState { kDeviceStateIdle, kDeviceStateListening, kDeviceStateSpeaking,
                   kDeviceStateConnecting, kDeviceStateWifiConfiguring };
struct StateMachine {
    DeviceState state = kDeviceStateIdle;
    bool TransitionTo(DeviceState next) { state = next; return true; }
};
struct Application {
    StateMachine state_machine_;
    std::atomic_bool voice_session_active_{false};
    bool SetDeviceState(DeviceState state);
};
''' + method + '''
int main() {
    Application app;
    assert(!app.SetDeviceState(kDeviceStateSpeaking));
    assert(!app.SetDeviceState(kDeviceStateListening));
    assert(app.state_machine_.state == kDeviceStateIdle);
    app.voice_session_active_ = true; // BOOT / 内置唤醒词。
    assert(app.SetDeviceState(kDeviceStateListening));
    assert(app.SetDeviceState(kDeviceStateSpeaking));
    assert(app.SetDeviceState(kDeviceStateListening)); // 正常连续对话。
    assert(app.SetDeviceState(kDeviceStateIdle));
    assert(!app.SetDeviceState(kDeviceStateSpeaking)); // 待命后的迟到消息。
    assert(!app.SetDeviceState(kDeviceStateListening));
    assert(app.SetDeviceState(kDeviceStateWifiConfiguring));
}
'''
    cpp = tmp_path / 'voice_gate.cc'
    cpp.write_text(program)
    binary = tmp_path / 'voice_gate'
    subprocess.run(['c++', '-std=c++17', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_player_gate_follows_actual_page_and_explicit_entry():
    app = (ROOT / 'main/application.cc').read_text()
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    render = function(board, 'void RenderLauncher()')
    hide = function(board, 'void HideLauncher()')
    wake = function(app, 'void Application::HandleWakeWordDetectedEvent()')
    software_wake = function(app, 'void Application::WakeWordInvoke(')
    manual = function(app, 'void Application::EnterConversationListening(')
    assert 'SetMusicPlayerVisible(launcher_state_.page() == YGSoulPage::kMusicPlayer)' in render
    assert 'SetMusicPlayerVisible(false)' in hide
    assert wake.index('music_player_visible_') < wake.index('EncodeWakeWord()')
    assert 'voice_session_active_ = true' in wake
    assert 'voice_session_active_ = true' in manual
    assert 'voice_session_active_' in software_wake
    assert 'ToggleChatState' not in function(board, 'static void FeatureClickCallback(')
    assert 'app.ToggleChatState();' in function(board, 'void InitializeButtons()')


def test_actual_wake_and_boot_paths_obey_player_visibility(tmp_path):
    source = (ROOT / 'main/application.cc').read_text()
    methods = '\n'.join(function(source, signature) for signature in (
        'bool Application::SetDeviceState(',
        'void Application::SetMusicPlayerVisible(',
        'void Application::HandleWakeWordDetectedEvent()',
        'void Application::EnterConversationListening(',
        'void Application::SetListeningMode(',
    ))
    program = r'''
#include <atomic>
#include <cassert>
#include <string>
#include <functional>
#define ESP_LOGI(...)
#define ESP_LOGW(...)
#define MAIN_EVENT_WAKE_WORD_DETECTED 4
inline void xEventGroupClearBits(int&, int) {}
enum DeviceState { kDeviceStateIdle, kDeviceStateListening, kDeviceStateSpeaking,
    kDeviceStateConnecting, kDeviceStateWifiConfiguring, kDeviceStateActivating };
enum ListeningMode { kListeningModeAutoStop };
enum PowerSaveLevel { PERFORMANCE };
enum { kAbortReasonWakeWordDetected };
namespace Lang { namespace Sounds { constexpr int OGG_POPUP = 0; } }
struct StateMachine {
    DeviceState state = kDeviceStateIdle;
    bool TransitionTo(DeviceState next) { state = next; return true; }
};
struct Audio {
    int wake_encoded = 0;
    bool wake_running = true;
    void EnableWakeWordDetection(bool value) { wake_running = value; }
    std::string GetLastWakeWord() { return "你好小智"; }
    void EncodeWakeWord() { ++wake_encoded; }
    bool PopPacketFromSendQueue() { return false; }
    void ResetDecoder() {}
    void PlaySound(int) {}
};
struct Protocol {
    int wake_sent = 0;
    bool IsAudioChannelOpened() { return true; }
    bool OpenAudioChannel() { return true; }
    void SendWakeWordDetected(const std::string&) { ++wake_sent; }
    void SendStartListening(ListeningMode) {}
};
struct Board {
    static Board& GetInstance() { static Board board; return board; }
    void SetPowerSaveLevel(PowerSaveLevel) {}
};
struct Application {
    StateMachine state_machine_;
    std::atomic_bool voice_session_active_{false}, music_player_visible_{false};
    Protocol protocol_storage_;
    Protocol* protocol_ = &protocol_storage_;
    Audio audio_service_;
    int event_group_ = 0;
    bool voice_dismissed_ = true, play_popup_on_listening_ = false;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    DeviceState GetDeviceState() { return state_machine_.state; }
    bool SetDeviceState(DeviceState);
    void SetMusicPlayerVisible(bool);
    void HandleWakeWordDetectedEvent();
    void EnterConversationListening(const char*);
    void SetListeningMode(ListeningMode);
    ListeningMode GetDefaultListeningMode() { return kListeningModeAutoStop; }
    void ListenForPairingCommand() {}
    bool IsVoiceDismissCommand(const char*) { return false; }
    void EnterVoiceDismissed() {}
    void AbortSpeaking(int) {}
    void Schedule(std::function<void()> callback) { callback(); }
    void ContinueWakeWordInvoke(const std::string&) {}
    void StartActivationIfNeeded() {}
    void SpeakPrompt(const char*) {}
};
''' + methods + r'''
int main() {
    Application app;
    app.SetMusicPlayerVisible(true);
    assert(!app.audio_service_.wake_running);
    app.HandleWakeWordDetectedEvent();
    assert(!app.audio_service_.wake_running);
    assert(app.GetDeviceState() == kDeviceStateIdle);
    assert(app.audio_service_.wake_encoded == 0);
    assert(app.protocol_storage_.wake_sent == 0);
    assert(!app.SetDeviceState(kDeviceStateSpeaking));
    app.SetMusicPlayerVisible(false);
    assert(app.audio_service_.wake_running);
    assert(app.GetDeviceState() == kDeviceStateIdle);
    assert(!app.voice_session_active_); // 返回页面本身不授权。
    app.HandleWakeWordDetectedEvent();
    assert(app.GetDeviceState() == kDeviceStateListening);
    assert(app.protocol_storage_.wake_sent == 1);
    app.SetDeviceState(kDeviceStateIdle);
    app.SetMusicPlayerVisible(true);
    app.EnterConversationListening("BOOT");
    assert(app.GetDeviceState() == kDeviceStateListening);
    app.SetMusicPlayerVisible(false); // 既有状态 UI 收起播放器。
    assert(app.SetDeviceState(kDeviceStateSpeaking));
    assert(app.SetDeviceState(kDeviceStateListening));
    app.SetDeviceState(kDeviceStateIdle);
    assert(!app.SetDeviceState(kDeviceStateSpeaking));
}
'''
    cpp = tmp_path / 'voice_entry.cc'
    cpp.write_text(program)
    binary = tmp_path / 'voice_entry'
    subprocess.run(['c++', '-std=c++17', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_late_audio_is_rejected_before_decoder_queue():
    source = (ROOT / 'main/application.cc').read_text()
    callback = source[source.index('protocol_->OnIncomingAudio('):source.index('protocol_->OnAudioChannelOpened(')]
    assert 'voice_session_active_.load()' in callback
    assert callback.index('voice_session_active_.load()') < callback.index('PushPacketToDecodeQueue')


def test_scheduled_entries_recheck_authorization_before_network_requests():
    source = (ROOT / 'main/application.cc').read_text()
    for signature in ('void Application::ContinueOpenAudioChannel(',
                      'void Application::ContinueWakeWordInvoke('):
        method = function(source, signature)
        assert method.index('!voice_session_active_.load()') < method.index('OpenAudioChannel()')
        assert method.index('music_player_visible_.load()') < method.index('OpenAudioChannel()')
