import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_ygsoul_ble_protocol_handles_fragmented_frames_and_crc():
    source = r'''
#include "boards/common/ygsoul_ble_protocol.h"
#include <cassert>
#include <string>
#include <vector>

int main() {
    using namespace ygsoul::ble;
    assert(kServiceUuid == 0x1910);
    assert(kWriteCharacteristicUuid == 0x2b11);
    assert(kNotifyCharacteristicUuid == 0x2b10);
    auto query = EncodeBleFrame(kQueryDevInfo, "");
    assert(query.size() == 6);
    assert(query[0] == 0xaa && query[1] == kQueryDevInfo && query[2] == 0 && query[3] == 0);
    assert(query[4] == 0x20 && query[5] == 0x00);
    auto frame = EncodeBleFrame(kWifiConfig, std::string("\x03tok\x06YGSoul\x04test", 16));
    assert(!frame.empty());
    BleFrameParser parser;
    std::vector<Frame> frames;
    for (size_t i = 0; i < frame.size(); ++i) parser.Feed(frame.data() + i, 1, frames);
    assert(frames.size() == 1);
    assert(frames[0].command == kWifiConfig);
    frame.back() ^= 1;
    frames.clear();
    parser.Reset();
    parser.Feed(frame.data(), frame.size(), frames);
    assert(frames.empty());
    assert(EncodeBleFrame(kWifiConfig, std::string(239, 'x')).empty());
}
'''
    with tempfile.TemporaryDirectory() as directory:
        test_cpp = pathlib.Path(directory) / "protocol_test.cc"
        binary = pathlib.Path(directory) / "protocol_test"
        test_cpp.write_text(source)
        result = subprocess.run(
            ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "main"),
             str(test_cpp), str(ROOT / "main/boards/common/ygsoul_ble_protocol.cc"), "-o", str(binary)],
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0, result.stderr
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr


def test_ygsoul_ble_provisioning_characteristics_are_writable_without_pairing():
    source = (ROOT / "main/boards/common/ygsoul_ble_provisioning.cc").read_text()
    assert "kNotifyCccd" in source
    assert "ESP_GATT_UUID_CHAR_CLIENT_CONFIG" in source
    assert "ESP_GATT_PERM_WRITE, 238, 0, nullptr" in source
    assert "ESP_GATT_PERM_READ, 238, sizeof(g_notifyValue), g_notifyValue" in source
    assert "ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE" in source
    assert "ESP_GAP_BLE_SEC_REQ_EVT" in source
    assert "esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true)" in source
    assert "kWifiConnectTimeoutUs" in source
    assert "WIFI_CONNECT_TIMEOUT" in source
    assert "esp_timer_start_once(wifi_connect_timeout_, kWifiConnectTimeoutUs)" in source
    assert "event == NetworkEvent::Disconnected && waiting_for_wifi_" not in source


def test_ygsoul_ble_provisioning_rolls_back_failed_wifi_candidates():
    source = (ROOT / "main/boards/common/ygsoul_ble_provisioning.cc").read_text()
    ssid_manager = (ROOT / "components/78__esp-wifi-connect/include/ssid_manager.h").read_text()
    assert "ReplaceSsidList" in ssid_manager
    assert "previous_ssids_" in source
    assert "ReplaceSsidList(previous_ssids_)" in source
    assert "WifiManager::GetInstance().StopStation()" in source
    assert "void YgSoulBleProvisioning::FailProvisioning" in source
    assert "FailProvisioning(\"WIFI_CONNECT_TIMEOUT\")" in source
    assert "netcfg_started_ = false" in source
    assert "// Keep BLE provisioning available for an immediate retry." in source


def test_wifi_disconnect_restarts_bounded_recovery_timer():
    source = read("main/boards/common/wifi_board.cc")
    disconnected = source[source.index("case NetworkEvent::Disconnected:"):source.index("case NetworkEvent::WifiConfigModeEnter:")]
    assert "esp_timer_stop(connect_timer_)" in disconnected
    assert "esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL)" in disconnected


if __name__ == "__main__":
    test_ygsoul_ble_protocol_handles_fragmented_frames_and_crc()
    test_ygsoul_ble_provisioning_characteristics_are_writable_without_pairing()
    test_ygsoul_ble_provisioning_rolls_back_failed_wifi_candidates()
    print("PASS: YGSoul BLE protocol")
