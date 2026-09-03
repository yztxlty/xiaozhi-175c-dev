import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]


def _read(*parts):
    return (ROOT.joinpath(*parts)).read_text()


def _slice(source, start_marker, end_marker):
    start = source.index(start_marker)
    end = source.index(end_marker, start + 1)
    return source[start:end]


def test_open_audio_fail_does_not_restart_activation():
    listen = _slice(
        _read("main/application.cc"),
        "void Application::EnterConversationListening(",
        "void Application::EnterVoiceDismissed()",
    )
    fail = listen[listen.index("Open audio channel failed"):]
    assert "StartActivationIfNeeded" not in fail
    assert "kDeviceStateIdle" in fail


def test_initialize_protocol_reuses_existing_clients():
    init = _slice(
        _read("main/application.cc"),
        "void Application::InitializeProtocol()",
        "void Application::HandleCustomMessage(",
    )
    assert "if (protocol_ == nullptr)" in init or "if (!protocol_)" in init
    assert "if (management_client_ == nullptr)" in init or "if (!management_client_)" in init


def test_pairing_wifi_change_restarts_protocol_when_chat_session_was_reset():
    connected = _slice(
        _read("main/application.cc"),
        "void Application::HandleNetworkConnectedEvent()",
        "void Application::HandleNetworkDisconnectedEvent()",
    )
    listen = _slice(
        _read("main/application.cc"),
        "void Application::EnterConversationListening(",
        "void Application::EnterVoiceDismissed()",
    )

    assert "management_client_ != nullptr && protocol_ != nullptr" in connected
    assert "StartActivationIfNeeded" in connected
    assert "protocol_ == nullptr" in listen
    assert "StartActivationIfNeeded" in listen
    assert "stay connecting" not in listen


def test_network_ready_enters_listening_not_standby():
    source = _read("main/application.cc")
    connected = _slice(source, "void Application::HandleNetworkConnectedEvent()", "void Application::HandleNetworkDisconnectedEvent()")
    done = _slice(source, "void Application::HandleActivationDoneEvent()", "void Application::ActivationTask()")
    machine = _read("main/device_state_machine.cc")
    starting = _slice(machine, "case kDeviceStateStarting:", "case kDeviceStateWifiConfiguring:")

    assert "kDeviceStateIdle" not in connected
    assert "kDeviceStateConnecting" in connected
    assert "kDeviceStateIdle" not in done
    assert "EnterConversationListening(" in done
    assert "我已联网，现在可以对话啦" in done
    assert "kDeviceStateConnecting" in starting
    assert "LOW_POWER" not in done


def test_dismiss_commands_enter_low_power_and_button_resumes_listening():
    source = _read("main/application.cc")
    assert "IsVoiceDismissCommand(" in source
    assert '"退下"' in source
    assert '"关机"' in source
    assert "EnterVoiceDismissed(" in source
    assert "在呢，可以正常实时拾音对话" in source
    toggle = _slice(source, "void Application::HandleToggleChatEvent()", "void Application::ContinueOpenAudioChannel")
    assert "voice_dismissed_" in toggle
    assert "EnterConversationListening(" in toggle


def test_wake_words_are_youguang_not_xiaozhi():
    custom = _read("main/audio/wake_words/custom_wake_word.cc")
    lang = _read("main/assets/locales/zh-CN/language.json")
    defaults = _read("sdkconfig.defaults.esp32s3")
    sdkconfig = _read("sdkconfig")

    assert "你好幽光" in custom
    assert "你好小幽" in custom
    assert "小幽小幽" in custom
    assert "ni hao you guang" in custom
    assert "ni hao xiao you" in custom
    assert "xiao you xiao you" in custom
    assert "NIHAOXIAOZHI" not in defaults
    assert "CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y" not in sdkconfig
    assert "你好小智" not in custom
    assert "我已联网，现在可以对话啦" in _read("main/application.cc")


def test_protocol_can_request_exact_tts_prompt():
    protocol = _read("main/protocols/protocol.cc")
    header = _read("main/protocols/protocol.h")
    assert "SendSpeakRequest" in header
    assert '"tts"' in protocol
    assert "request" in protocol


if __name__ == "__main__":
    test_pairing_wifi_change_restarts_protocol_when_chat_session_was_reset()
    test_network_ready_enters_listening_not_standby()
    test_dismiss_commands_enter_low_power_and_button_resumes_listening()
    test_wake_words_are_youguang_not_xiaozhi()
    test_protocol_can_request_exact_tts_prompt()
    print("PASS: conversation listen flow")
