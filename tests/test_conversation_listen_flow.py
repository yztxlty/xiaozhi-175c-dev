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


def test_conversation_states_switch_off_boot_logo_to_companion():
    emotion = _slice(
        _read("main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc"),
        "virtual void SetEmotion(const char* emotion) override",
        "virtual void SetStatus(const char* status) override",
    )
    assert "kDeviceStateConnecting" in emotion
    assert "kDeviceStateListening" in emotion
    assert "kDeviceStateSpeaking" in emotion
    assert "ShowYGSoulCompanion" in emotion
    assert "showing_boot_logo_ && !conversation_ui" in emotion


def test_pairing_wifi_up_stays_in_config_until_exit():
    connected = _slice(
        _read("main/application.cc"),
        "void Application::HandleNetworkConnectedEvent()",
        "void Application::HandleNetworkDisconnectedEvent()",
    )
    assert "wait for 结束配网" in connected
    assert "StartActivationIfNeeded" in connected
    assert "EnterConversationListening" not in connected


def test_activation_and_pairing_exit_enter_standby():
    source = _read("main/application.cc")
    done = _slice(source, "void Application::HandleActivationDoneEvent()", "void Application::ActivationTask()")
    standby = _slice(source, "void Application::EnterStandby()", "void Application::EnterVoiceDismissed()")
    assert "EnterStandby" in done
    assert "EnterConversationListening" not in done
    assert "CRITICAL-RUNTIME-CONTRACT" in standby
    assert "SendStopListening" in standby
    assert "CloseAudioChannel" not in standby
    assert "EnableWakeWordDetection(true)" in standby


def test_super_power_save_never_tears_down_the_websocket():
    board = _read("main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc")
    super_save = _slice(board, "void EnterSuperPowerSave()", "void ExitSuperPowerSave()")
    assert "CRITICAL-RUNTIME-CONTRACT" in super_save
    assert "CloseAudioChannel" not in super_save


def test_dismiss_commands_finish_tts_before_standby_and_wake_clears_dismissal():
    source = _read("main/application.cc")
    assert "IsVoiceDismissCommand(" in source
    assert '"退下"' in source
    assert '"关机"' in source
    dismissed = _slice(source, "void Application::EnterVoiceDismissed()", "void Application::ListenForPairingCommand()")
    tts_stop = _slice(source, 'strcmp(state->valuestring, "stop")', 'strcmp(state->valuestring, "sentence_start")')
    assert "voice_dismissed_ = true" in dismissed
    assert "EnterStandby();" not in dismissed
    assert "voice_dismissed_" in tts_stop
    assert "EnterStandby" in tts_stop
    toggle = _slice(source, "void Application::HandleToggleChatEvent()", "void Application::ContinueOpenAudioChannel")
    assert "BOOT starts conversation from standby" in toggle
    assert "EnterConversationListening" in toggle
    assert "kDeviceStateListening" in toggle
    assert "EnterStandby" in toggle
    wake = _slice(source, "void Application::HandleWakeWordDetectedEvent()", "void Application::ContinueWakeWordInvoke")
    assert "CRITICAL-RUNTIME-CONTRACT" in wake
    assert "ContinueWakeWordInvoke" in wake
    assert "voice_dismissed_ = false" in wake
    assert "SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE)" in wake


def test_dismissal_acknowledgement_tts_must_start_before_it_can_finish_to_standby():
    source = _read("main/application.cc")
    incoming = _slice(source, "protocol_->OnIncomingJson(", "protocol_->Start();")
    tts_start = _slice(incoming, 'if (strcmp(state->valuestring, "start") == 0)', 'else if (strcmp(state->valuestring, "stop") == 0)')
    assert "SetDeviceState(kDeviceStateSpeaking);" in tts_start
    assert "if (voice_dismissed_)" not in tts_start


def test_explicit_cloud_standby_does_not_wait_for_a_tts_event():
    source = _read("main/application.cc")
    standby_event = _slice(source, 'strcmp(type->valuestring, "device.standby")', 'strcmp(type->valuestring, "llm")')
    assert "EnterStandby();" in standby_event


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
    assert "你好小智" not in lang


def test_protocol_can_request_exact_tts_prompt():
    protocol = _read("main/protocols/protocol.cc")
    header = _read("main/protocols/protocol.h")
    assert "SendSpeakRequest" in header
    assert '"tts"' in protocol
    assert "request" in protocol


def test_audio_backpressure_drops_stale_input_without_blocking_afe():
    source = _read("main/audio/audio_service.cc")
    push = _slice(source, "void AudioService::PushTaskToEncodeQueue(", "bool AudioService::PushPacketToDecodeQueue(")

    assert "audio_encode_queue_.size() >= MAX_ENCODE_TASKS_IN_QUEUE" in push
    assert "dropping stale audio frame" in push
    assert "audio_queue_cv_.wait(" not in push


def test_pairing_entry_restarts_before_ble_needs_active_audio_memory():
    app = _read("main/application.cc")
    wifi = _read("main/boards/common/wifi_board.cc")
    command = _slice(app, "bool Application::HandleWifiConfigVoiceCommand(", "void Application::SpeakPrompt(")
    voice_entry = _slice(wifi, "void WifiBoard::EnterWifiConfigModeForVoice()", "void WifiBoard::ExitWifiConfigMode(")
    button_entry = _slice(wifi, "void WifiBoard::EnterWifiConfigMode()", "void WifiBoard::EnterWifiConfigModeForVoice()")

    assert "EnterWifiConfigMode();" in command
    assert "EnterWifiConfigMode();" in voice_entry
    assert "WifiManager::GetInstance().IsConnected()" in button_entry
    assert "SetBool(kPairingRebootPendingKey, true)" in button_entry
    assert "Application::GetInstance().Reboot();" in button_entry


def test_pairing_can_preempt_an_active_conversation_and_switch_to_logo():
    machine = _read("main/device_state_machine.cc")
    app = _read("main/application.cc")
    wifi = _read("main/boards/common/wifi_board.cc")
    ble = _read("main/boards/common/ygsoul_ble_provisioning.cc")
    display = _read("main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc")

    listening = _slice(machine, "case kDeviceStateListening:", "case kDeviceStateSpeaking:")
    speaking = _slice(machine, "case kDeviceStateSpeaking:", "case kDeviceStateFatalError:")
    start = _slice(wifi, "void WifiBoard::StartWifiConfigMode()", "void WifiBoard::EnterWifiConfigMode()")
    emotion = _slice(display, "virtual void SetEmotion(const char* emotion) override", "virtual void SetStatus(const char* status) override")

    assert "to == kDeviceStateWifiConfiguring" in listening
    assert "to == kDeviceStateWifiConfiguring" in speaking
    assert "Application::GetInstance().Schedule" in start
    assert "YgSoulBleProvisioning::GetInstance().Start()" in start
    assert "kDeviceStateWifiConfiguring" in emotion
    assert "ShowYGSoulBootLogo();" in emotion
    assert "ygsoul_boot_image_" in display
    assert "ygsoul_boot_lvgl" in display
    assert "Voice command: enter wifi config" in app
    assert "ESP_ERROR_CHECK(esp_bt_controller_mem_release" not in ble


def test_pairing_reboots_before_ble_when_conversation_memory_is_active():
    wifi = _read("main/boards/common/wifi_board.cc")
    start_network = _slice(wifi, "void WifiBoard::StartNetwork()", "void WifiBoard::TryWifiConnect()")
    enter = _slice(wifi, "void WifiBoard::EnterWifiConfigMode()", "void WifiBoard::EnterWifiConfigModeForVoice()")
    voice = _slice(wifi, "void WifiBoard::EnterWifiConfigModeForVoice()", "void WifiBoard::ExitWifiConfigMode()")

    assert "GetBool(kPairingRebootPendingKey)" in start_network
    assert "StartWifiConfigMode();" in start_network
    assert "SetBool(kPairingRebootPendingKey, true)" in enter
    assert "Application::GetInstance().Reboot();" in enter
    assert "EnterWifiConfigMode();" in voice


def test_pairing_completion_returns_to_standby_without_rebooting():
    wifi = _read("main/boards/common/wifi_board.cc")
    ble = _read("main/boards/common/ygsoul_ble_provisioning.cc")
    exit_mode = _slice(wifi, "void WifiBoard::ExitWifiConfigMode", "bool WifiBoard::IsInWifiConfigMode")
    connected = _slice(ble, "void YgSoulBleProvisioning::OnNetworkEvent", "void YgSoulBleProvisioning::OnWifiConnectTimeout")

    assert "ExitWifiConfigMode();" in connected
    assert "Settings websocket_settings" not in exit_mode
    assert "Application::GetInstance().Reboot();" not in exit_mode
    assert "Application::GetInstance().EnterStandby();" in exit_mode


def test_pairing_starts_ble_after_conversation_audio_is_released():
    app = _read("main/application.cc")
    wifi = _read("main/boards/common/wifi_board.cc")
    start = _slice(wifi, "void WifiBoard::StartWifiConfigMode()", "void WifiBoard::EnterWifiConfigMode()")
    ble_start = _slice(start, "#ifdef CONFIG_USE_YGSOUL_BLE_WIFI_PROVISIONING", "#elif CONFIG_USE_HOTSPOT_WIFI_PROVISIONING")
    state_changed = _slice(app, "case kDeviceStateWifiConfiguring:", "default:")

    assert "YgSoulBleProvisioning::GetInstance().Start()" in ble_start
    assert "Application::GetInstance().Schedule" in ble_start
    assert "EnableVoiceProcessing(false);" in state_changed
    assert "EnableWakeWordDetection(true);" in state_changed


if __name__ == "__main__":
    test_conversation_states_switch_off_boot_logo_to_companion()
    test_pairing_wifi_up_stays_in_config_until_exit()
    test_activation_and_pairing_exit_enter_standby()
    test_super_power_save_never_tears_down_the_websocket()
    test_dismiss_commands_finish_tts_before_standby_and_wake_clears_dismissal()
    test_dismissal_acknowledgement_tts_must_start_before_it_can_finish_to_standby()
    test_wake_words_are_youguang_not_xiaozhi()
    test_protocol_can_request_exact_tts_prompt()
    test_audio_backpressure_drops_stale_input_without_blocking_afe()
    test_pairing_entry_restarts_before_ble_needs_active_audio_memory()
    test_pairing_can_preempt_an_active_conversation_and_switch_to_logo()
    test_pairing_reboots_before_ble_when_conversation_memory_is_active()
    test_pairing_completion_returns_to_standby_without_rebooting()
    test_pairing_starts_ble_after_conversation_audio_is_released()
    print("PASS: conversation listen flow")
