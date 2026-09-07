import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]


def _source():
    return (ROOT / "main/application.cc").read_text()


def _slice(source, start_marker, end_marker):
    start = source.index(start_marker)
    end = source.index(end_marker, start + 1)
    return source[start:end]


def test_pairing_activation_does_not_run_version_check_or_upgrade():
    task = _slice(_source(), "void Application::ActivationTask()", "void Application::CheckAssetsVersion()")

    assert "CheckNewVersion(" not in task
    assert "CheckAssetsVersion(" not in task
    assert "CHECKING_NEW_VERSION" not in task
    assert "HasNewVersion(" not in task
    assert "UpgradeFirmware(" not in task
    assert "ShowActivationCode(" not in task
    assert "FOUND_NEW_ASSETS" not in task
    assert "InitializeProtocol(" in task
    assert "assets.Apply(" not in task
    assert "CheckAssetsVersion(" not in task


def test_activation_only_confirms_a_rebooted_upgrade_without_polling_for_updates():
    task = _slice(_source(), "void Application::ActivationTask()", "void Application::CheckAssetsVersion()")

    assert "ota_->MarkCurrentVersionValid();" in task
    assert "ota_->ConfirmPendingUpgrade();" in task
    assert "have_local_endpoints" in task
    assert "HasNewVersion(" not in task
    assert "GetFirmwareUrl(" not in task


def test_cached_endpoints_do_not_skip_the_post_reboot_ota_success_report():
    task = _slice(_source(), "void Application::ActivationTask()", "void Application::CheckAssetsVersion()")
    mark_valid = task.index("ota_->MarkCurrentVersionValid();")
    confirm = task.index("ota_->ConfirmPendingUpgrade();")
    initialize = task.index("InitializeProtocol();")
    cached_branch_end = task.index("\n    }\n\n    // A successful OTA")

    assert cached_branch_end < mark_valid < confirm < initialize


def test_activation_retries_connection_config_and_never_reports_false_success():
    task = _slice(_source(), "void Application::ActivationTask()", "void Application::CheckAssetsVersion()")

    assert "while (ota_->CheckVersion() != ESP_OK)" in task
    assert "vTaskDelay(pdMS_TO_TICKS(1000));" in task
    assert "continue with local settings" not in task
    assert task.index("while (ota_->CheckVersion() != ESP_OK)") < task.index("InitializeProtocol();")
    assert task.index("InitializeProtocol();") < task.index("MAIN_EVENT_ACTIVATION_DONE")


def test_app_ota_command_checks_the_just_created_task_and_starts_upgrade():
    source = _source()
    command = _slice(source, "void Application::HandleCustomMessage", "void Application::ReportDeviceUplink")

    assert 'strcmp(command->valuestring, "checkOta")' in command
    assert "ota_->CheckVersion()" in command
    assert "UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())" in command


def test_wifi_join_enters_chat_idle_immediately_without_initializing_gate():
    source = _source()
    connected = _slice(source, "void Application::HandleNetworkConnectedEvent()", "void Application::HandleNetworkDisconnectedEvent()")
    done = _slice(source, "void Application::HandleActivationDoneEvent()", "void Application::ActivationTask()")
    task = _slice(source, "void Application::ActivationTask()", "void Application::CheckAssetsVersion()")
    init = _slice(source, "void Application::InitializeProtocol()", "void Application::HandleCustomMessage")
    machine = (ROOT / "main/device_state_machine.cc").read_text()
    starting = _slice(machine, "case kDeviceStateStarting:", "case kDeviceStateWifiConfiguring:")
    wifi_cfg = _slice(machine, "case kDeviceStateWifiConfiguring:", "case kDeviceStateAudioTesting:")

    assert 'ReportDeviceUplink("attributes")' in connected
    assert 'ReportDeviceUplink("telemetry")' in connected
    assert "kDeviceStateActivating" not in connected
    assert "StartActivationIfNeeded" in connected
    assert "Lang::Strings::INITIALIZING" not in connected
    assert "LOADING_PROTOCOL" not in init
    assert "YdpClient" not in task
    assert "EnterStandby()" in done
    assert "EnterConversationListening(" not in done
    assert "Lang::Strings::VERSION" not in done
    assert "kDeviceStateIdle" in starting


def test_missing_ota_protocol_config_defaults_to_websocket_chat():
    init = _slice(_source(), "void Application::InitializeProtocol()", "void Application::HandleCustomMessage")
    fallback = init[init.index("else {"):]

    assert "WebsocketProtocol" in fallback
    assert "MqttProtocol" not in fallback


def test_pairing_loads_packaged_models_before_network_and_activates_after_wifi_exit():
    source = _source()
    init = _slice(source, "void Application::Initialize()", "void Application::Run()")
    connected = _slice(source, "void Application::HandleNetworkConnectedEvent()", "void Application::HandleNetworkDisconnectedEvent()")

    assert "assets.Apply();" in init
    assert init.index("assets.Apply();") < init.index("board.StartNetwork();")
    assert "if (protocol_ == nullptr)" in connected
    assert connected.index("if (protocol_ == nullptr)") < connected.index("if (state == kDeviceStateStarting")


if __name__ == "__main__":
    test_pairing_activation_does_not_run_version_check_or_upgrade()
    test_activation_only_confirms_a_rebooted_upgrade_without_polling_for_updates()
    test_activation_retries_connection_config_and_never_reports_false_success()
    test_app_ota_command_checks_the_just_created_task_and_starts_upgrade()
    test_wifi_join_enters_chat_idle_immediately_without_initializing_gate()
    test_missing_ota_protocol_config_defaults_to_websocket_chat()
    print("PASS: pairing activation skips version check")
