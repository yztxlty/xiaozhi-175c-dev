"""编译固件真实回执逻辑，保护会话归属及先 ACK 后离线的顺序。"""
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_pairing_receipt_matches_only_current_waiting_wifi(tmp_path):
    source = r'''
#include "boards/common/ygsoul_ble_protocol.h"
#include <cassert>
#include <thread>
int main() {
    using namespace ygsoul::ble;
    PairingReceipt receipt;
    const std::string id = "12345678-1234-4234-8234-123456789abc";
    assert(receipt.SessionId().empty());
    assert(!receipt.Begin("legacy-token", "new-wifi"));
    assert(!receipt.Begin(id, "new-wifi"));
    assert(!receipt.Begin(id + ".", "new-wifi"));
    assert(!receipt.Begin("xxxxxxxx-1234-4234-8234-123456789abc.secret", "new-wifi"));
    assert(receipt.Begin(id + ".secret", "new-wifi"));
    assert(receipt.Complete("new-wifi") == PairingReceipt::Result::Ignored);
    receipt.StartWaiting();
    // A late event from the previous station must not consume the current attempt.
    assert(receipt.Complete("old-wifi") == PairingReceipt::Result::Mismatch);
    assert(receipt.Complete("new-wifi") == PairingReceipt::Result::Matched);
    assert(receipt.SessionId() == id);
    PairingReceipt restored;
    restored.RestoreCompleted(id);
    assert(restored.SessionId() == id);
    assert(!receipt.CancelPending()); // 停止 BLE 不得清除成功回执。
    assert(receipt.SessionId() == id);
    assert(receipt.Complete("new-wifi") == PairingReceipt::Result::Ignored);
    assert(receipt.Begin(id + ".next", "next-wifi"));
    assert(receipt.SessionId().empty());
    receipt.StartWaiting();
    assert(receipt.Complete("old-wifi") == PairingReceipt::Result::Mismatch);
    assert(receipt.SessionId().empty());
    assert(receipt.Complete("next-wifi") == PairingReceipt::Result::Matched);
    receipt.Reset();
    assert(receipt.SessionId().empty());
    assert(receipt.Complete("next-wifi") == PairingReceipt::Result::Ignored);
    assert(receipt.Begin(id + ".secret", "wifi"));
    receipt.StartWaiting();
    assert(receipt.CancelPending());
    assert(receipt.Complete("wifi") == PairingReceipt::Result::Ignored);
    std::thread writer([&] {
        for (int i = 0; i < 1000; ++i) {
            assert(receipt.Begin(id + ".secret", "wifi"));
            receipt.StartWaiting();
            receipt.Complete("wifi");
        }
    });
    for (int i = 0; i < 1000; ++i) {
        const auto value = receipt.SessionId();
        assert(value.empty() || value == id);
    }
    writer.join();
}
'''
    binary = tmp_path / "pairing-receipt"
    compiled = subprocess.run(
        ["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "main"),
         "-x", "c++", "-", str(ROOT / "main/boards/common/ygsoul_ble_protocol.cc"), "-o", str(binary)],
        input=source, text=True, capture_output=True,
    )
    assert compiled.returncode == 0, compiled.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def test_command_ack_precedes_offline_and_wifi_cleanup():
    source = (ROOT / "main/application.cc").read_text(encoding="utf-8")
    handler = source.index("void Application::HandleCustomMessage")
    ack_finished = source.index("cJSON_Delete(response);", handler)
    for command in ("unbind", "factoryReset"):
        start = source.index(f'if (succeeded && strcmp(command->valuestring, "{command}") == 0) {{', ack_finished)
        block = source[start:source.index("\n    }", start)]
        assert 'ReportDeviceUplink("event", "device.offline");' in block
        assert block.index('ReportDeviceUplink("event", "device.offline");') < block.index("Schedule(")
        assert source.index("management_client_->Send(text);") < start
        assert source.index("protocol_->SendDeviceMessage(text);") < start
    receipt = source.split("bool Application::ReportPairingReceipt", 1)[1].split(
        "void Application::ReportDeviceUplink", 1
    )[0]
    assert '"pairingSessionId", pairing_session_id.c_str()' in receipt
    assert "GetPairingSessionId()" in receipt
    assert "management_client_->Send(text)" in receipt
    assert "protocol_->SendDeviceMessage" not in receipt
    uplink = source.split("void Application::ReportDeviceUplink", 1)[1].split(
        "void Application::ReportDeviceTelemetry", 1
    )[0]
    assert "pairingSessionId" not in uplink


def test_websocket_installs_receive_callbacks_before_tls_connect():
    source = (ROOT / "components/78__esp-ml307/src/web_socket.cc").read_text(encoding="utf-8")
    connect = source.split("bool WebSocket::Connect(const char* uri)", 1)[1].split("bool WebSocket::Send", 1)[0]
    assert connect.index("xEventGroupClearBits") < connect.index("tcp_->Connect")
    assert connect.index("tcp_->OnStream") < connect.index("tcp_->Connect")
    assert connect.index("tcp_->OnDisconnected") < connect.index("tcp_->Connect")


def test_provisioning_uses_receipt_and_preserves_it_on_stop():
    source = (ROOT / "main/boards/common/ygsoul_ble_provisioning.cc").read_text(encoding="utf-8")
    assert "pairing_receipt_.Begin(token, ssid)" in source
    event = source.split("void YgSoulBleProvisioning::OnNetworkEvent", 1)[1].split("void YgSoulBleProvisioning::OnWifiConnectTimeout", 1)[0]
    assert "pairing_receipt_.Complete(data)" in event
    mismatch = event.split("PairingReceipt::Result::Mismatch", 1)[1].split("}", 1)[0]
    assert "return" in mismatch
    assert "FailProvisioning" not in mismatch
    assert event.index("PairingReceipt::Result::Ignored") < event.index('SendStatus("SUCCEEDED")')
    assert event.index("PairingReceipt::Result::Mismatch") < event.index('SendStatus("SUCCEEDED")')
    stop = source.split("void YgSoulBleProvisioning::Stop()", 1)[1].split("void YgSoulBleProvisioning::GattsEvent", 1)[0]
    assert "pairing_receipt_.CancelPending()" in stop
    assert "ReplaceSsidList(previous_ssids, false)" in stop
    assert "pairing_receipt_.Begin" not in stop


def test_new_ble_start_clears_memory_and_persisted_pairing_receipt():
    source = (ROOT / "main/boards/common/ygsoul_ble_provisioning.cc").read_text(encoding="utf-8")
    start = source.split("esp_err_t YgSoulBleProvisioning::Start()", 1)[1].split(
        "void YgSoulBleProvisioning::EnsureAdvertising", 1
    )[0]
    assert "pairing_receipt_.Reset()" in start
    assert "EraseKey(kPairingReceiptKey)" in start
    assert start.index("pairing_receipt_.Reset()") < start.index("wifi_scan_.Open()")


def test_pairing_receipt_remains_persisted_until_the_next_pairing_start():
    source = (ROOT / "main/boards/common/ygsoul_ble_provisioning.cc").read_text(encoding="utf-8")
    getter = source.split("std::string YgSoulBleProvisioning::GetPairingSessionId()", 1)[1].split(
        "esp_err_t YgSoulBleProvisioning::Start()", 1
    )[0]
    start = source.split("esp_err_t YgSoulBleProvisioning::Start()", 1)[1].split(
        "void YgSoulBleProvisioning::EnsureAdvertising", 1
    )[0]

    assert "Settings(kWifiSettingsNamespace, true).GetString(kWifiPairingReceiptKey)" in getter
    assert "Settings(kLegacyPairingReceiptSettingsNamespace, true)" in getter
    assert ".GetString(kPairingReceiptKey)" in getter
    assert "EraseKey(kPairingReceiptKey)" not in getter
    assert "EraseKey(kPairingReceiptKey)" in start


def test_pairing_target_isolated_in_memory_without_overwriting_saved_history():
    provisioning = (ROOT / "main/boards/common/ygsoul_ble_provisioning.cc").read_text(encoding="utf-8")
    manager = (ROOT / "components/78__esp-wifi-connect/ssid_manager.cc").read_text(encoding="utf-8")
    header = (ROOT / "components/78__esp-wifi-connect/include/ssid_manager.h").read_text(encoding="utf-8")
    command = provisioning.split("void YgSoulBleProvisioning::HandleFrame", 1)[1].split(
        "void YgSoulBleProvisioning::Notify", 1
    )[0]

    assert "AddSsid(ssid, password, false)" in command
    assert "ReplaceSsidList({{ssid, password}}, false)" in command
    assert "ReplaceSsidList(previous_ssids, false)" in provisioning
    assert "bool save_to_nvs = true" in header
    assert "if (save_to_nvs)" in manager


def test_management_reconnect_replays_receipt_and_worker_exit_is_recoverable():
    source = (ROOT / "main/application.cc").read_text(encoding="utf-8")
    init = source.split("void Application::InitializeProtocol()", 1)[1].split(
        "void Application::HandleCustomMessage", 1
    )[0]
    connected = init.split("management_client_->OnConnected", 1)[1].split(
        "management_client_->OnMessage", 1
    )[0]
    task = source.split("void Application::ActivationTask()", 1)[1].split(
        "void Application::CheckAssetsVersion", 1
    )[0]

    assert connected.index("ReportPairingReceipt()") < connected.index('ReportDeviceUplink("attributes")')
    recovery = task.split("while (management_client_ == nullptr || !management_client_->IsConnected())", 1)[1].split(
        "while (!ReportPairingReceipt())", 1
    )[0]
    assert "management_client_->Start()" in recovery


def test_pairing_receipt_precedes_voice_protocol_and_ydp_cannot_hold_performance_power():
    source = (ROOT / "main/application.cc").read_text(encoding="utf-8")
    header = (ROOT / "main/application.h").read_text(encoding="utf-8")
    task = source.split("void Application::ActivationTask()", 1)[1].split(
        "void Application::CheckAssetsVersion", 1
    )[0]

    assert "void InitializeManagementClient();" in header
    assert task.index("InitializeManagementClient();") < task.index("while (!ReportPairingReceipt())")
    assert task.index("while (!ReportPairingReceipt())") < task.index("InitializeProtocol();")
    assert task.index("PowerSaveLevel::BALANCED") < task.index("ydp.Connect")
    assert task.index("ydp.Connect") < task.index("InitializeProtocol();")


def test_management_worker_is_supervised_after_an_unexpected_exit():
    source = (ROOT / "main/application.cc").read_text(encoding="utf-8")
    tick = source.split("if (bits & MAIN_EVENT_CLOCK_TICK)", 1)[1].split(
        "if (bits & MAIN_EVENT_SCHEDULE)", 1
    )[0] if source.count("if (bits & MAIN_EVENT_SCHEDULE)") > 1 else source.split(
        "if (bits & MAIN_EVENT_CLOCK_TICK)", 1
    )[1].split("void Application::StartNetworkTimeSync", 1)[0]
    connected = source.split("void Application::HandleNetworkConnectedEvent()", 1)[1].split(
        "void Application::HandleNetworkDisconnectedEvent", 1
    )[0]

    assert "management_client_->Start()" in tick
    assert "management_client_->Start()" in connected
    assert "ReportPairingReceipt()" in connected


def test_provisioning_never_stops_station_while_holding_state_mutex():
    source = (ROOT / "main/boards/common/ygsoul_ble_provisioning.cc").read_text(encoding="utf-8")
    header = (ROOT / "main/boards/common/ygsoul_ble_provisioning.h").read_text(encoding="utf-8")
    stop = source.split("void YgSoulBleProvisioning::Stop()", 1)[1].split(
        "void YgSoulBleProvisioning::GattsEvent", 1
    )[0]
    fail = source.split("void YgSoulBleProvisioning::FailProvisioning", 1)[1]

    assert "wifi_operation_mutex_" in header
    assert "std::lock_guard<std::mutex> operation_lock(wifi_operation_mutex_)" in stop
    assert "std::lock_guard<std::mutex> operation_lock(wifi_operation_mutex_)" in fail
    assert "FailProvisioningLocked" not in source


def test_successful_pairing_receipt_survives_one_unexpected_reboot():
    source = (ROOT / "main/boards/common/ygsoul_ble_provisioning.cc").read_text(encoding="utf-8")
    header = (ROOT / "main/boards/common/ygsoul_ble_provisioning.h").read_text(encoding="utf-8")
    manager = (ROOT / "components/78__esp-wifi-connect/ssid_manager.cc").read_text(encoding="utf-8")
    manager_header = (ROOT / "components/78__esp-wifi-connect/include/ssid_manager.h").read_text(encoding="utf-8")
    assert 'kPairingReceiptKey' in source
    assert 'EraseKey(kPairingReceiptKey)' in source
    assert 'ReplaceSsidListAndPairingSession(' in source
    assert 'merged_ssids_, pairing_session_id)' in source
    assert 'SetString(kPairingReceiptKey, pairing_session_id)' not in source
    assert 'GetString(kPairingReceiptKey)' in source
    assert 'RestoreCompleted' in source
    assert 'std::string GetPairingSessionId()' in header
    assert 'ReplaceSsidListAndPairingSession' in manager_header
    transaction = manager.split('void SsidManager::SaveToNvs', 1)[1].split(
        'void SsidManager::AddSsid', 1
    )[0]
    assert 'nvs_set_str(nvs_handle, "pair_session", pairing_session_id->c_str())' in transaction
    assert transaction.index('nvs_set_str(nvs_handle, "pair_session"') < transaction.index('nvs_commit(nvs_handle)')
    assert transaction.count('nvs_commit(nvs_handle)') == 1


def test_terminal_provisioning_status_survives_ble_reconnect_for_app_query():
    source = (ROOT / "main/boards/common/ygsoul_ble_provisioning.cc").read_text(encoding="utf-8")
    header = (ROOT / "main/boards/common/ygsoul_ble_provisioning.h").read_text(encoding="utf-8")
    assert 'provisioningStatus' in source
    assert 'provisioningErrorCode' in source
    assert 'provisioning_status_' in header
    assert 'provisioning_error_code_' in header


def test_pairing_disables_wifi_power_save_before_dhcp():
    source = (ROOT / "main/boards/common/ygsoul_ble_provisioning.cc").read_text(encoding="utf-8")
    request = source.split("void YgSoulBleProvisioning::HandleFrame", 1)[1].split(
        "void YgSoulBleProvisioning::Notify", 1
    )[0].split("ygsoul::ble::WifiConfig config;", 1)[1]
    connect = request.split('ESP_LOGI(kTag, "WiFi credentials isolated; starting target station")', 1)[1]

    assert request.index('SendStatus("CONNECTING")') < request.index("Application::GetInstance().Schedule")
    assert request.index("Application::GetInstance().Schedule") < request.index("WifiManager::GetInstance().StopStation();")
    assert request.index("esp_ble_gatts_close") < request.index("WifiManager::GetInstance().StopStation();")
    assert connect.index("WifiManager::GetInstance().StartStation();") < connect.index(
        "PowerSaveLevel::PERFORMANCE"
    )
