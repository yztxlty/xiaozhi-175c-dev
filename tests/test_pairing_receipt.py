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
    receipt.Begin(id + ".secret", "new-wifi");
    assert(receipt.Complete("new-wifi") == PairingReceipt::Result::Ignored);
    receipt.StartWaiting();
    assert(receipt.Complete("new-wifi") == PairingReceipt::Result::Matched);
    assert(receipt.SessionId() == id);
    PairingReceipt restored;
    restored.RestoreCompleted(id);
    assert(restored.SessionId() == id);
    assert(!receipt.CancelPending()); // 停止 BLE 不得清除成功回执。
    assert(receipt.SessionId() == id);
    assert(receipt.Complete("new-wifi") == PairingReceipt::Result::Ignored);
    receipt.Begin(id + ".next", "next-wifi");
    assert(receipt.SessionId().empty());
    receipt.StartWaiting();
    assert(receipt.Complete("old-wifi") == PairingReceipt::Result::Mismatch);
    assert(receipt.SessionId().empty());
    assert(receipt.Complete("next-wifi") == PairingReceipt::Result::Ignored);
    for (const auto& token : {std::string("legacy-token"), std::string(""),
                             id, id + ".", std::string("xxxxxxxx-1234-4234-8234-123456789abc.secret")}) {
        receipt.Begin(token, "wifi");
        receipt.StartWaiting();
        assert(receipt.Complete("wifi") == PairingReceipt::Result::Matched);
        assert(receipt.SessionId().empty());
    }
    receipt.Begin(id + ".secret", "wifi");
    receipt.StartWaiting();
    assert(receipt.CancelPending());
    assert(receipt.Complete("wifi") == PairingReceipt::Result::Ignored);
    std::thread writer([&] {
        for (int i = 0; i < 1000; ++i) {
            receipt.Begin(id + ".secret", "wifi");
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
    uplink = source.split("void Application::ReportDeviceUplink", 1)[1].split("void Application::ReportDeviceTelemetry", 1)[0]
    assert '"pairingSessionId", pairing_session_id.c_str()' in uplink
    assert "GetPairingSessionId()" in uplink


def test_provisioning_uses_receipt_and_preserves_it_on_stop():
    source = (ROOT / "main/boards/common/ygsoul_ble_provisioning.cc").read_text(encoding="utf-8")
    assert "pairing_receipt_.Begin(token, ssid)" in source
    event = source.split("void YgSoulBleProvisioning::OnNetworkEvent", 1)[1].split("void YgSoulBleProvisioning::OnWifiConnectTimeout", 1)[0]
    assert "pairing_receipt_.Complete(data)" in event
    assert 'FailProvisioning("WIFI_SSID_MISMATCH")' in event
    assert event.index("PairingReceipt::Result::Ignored") < event.index('SendStatus("SUCCEEDED")')
    assert event.index("PairingReceipt::Result::Mismatch") < event.index('SendStatus("SUCCEEDED")')
    stop = source.split("void YgSoulBleProvisioning::Stop()", 1)[1].split("\n}", 1)[0]
    assert "pairing_receipt_.CancelPending()" in stop
    assert "pairing_receipt_.Begin" not in stop


def test_successful_pairing_receipt_survives_one_unexpected_reboot():
    source = (ROOT / "main/boards/common/ygsoul_ble_provisioning.cc").read_text(encoding="utf-8")
    header = (ROOT / "main/boards/common/ygsoul_ble_provisioning.h").read_text(encoding="utf-8")
    assert 'kPairingReceiptKey' in source
    assert 'EraseKey(kPairingReceiptKey)' in source
    assert 'SetString(kPairingReceiptKey, pairing_session_id)' in source
    assert 'GetString(kPairingReceiptKey)' in source
    assert 'RestoreCompleted' in source
    assert 'std::string GetPairingSessionId()' in header
