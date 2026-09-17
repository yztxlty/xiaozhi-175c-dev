"""Host protocol tests compile production C++; hardware/IDF validation is separate."""
from pathlib import Path
import json
import subprocess

ROOT = Path(__file__).resolve().parents[1]
COMMON = ROOT / 'main/boards/common'


def test_protocol_bounds_and_snapshot(tmp_path):
    source = r'''
#include "ygsoul_wifi_scan_protocol.h"
#include <cassert>
#include <cstdio>
#include <iostream>
using namespace ygsoul::wifi_scan;
int main() {
    assert(ValidId("0123abcd"));
    assert(!ValidId("")); assert(!ValidId("012345678"));
    assert(!ValidId("1234567\"")); assert(!ValidId("1234567z"));
    assert(Hex(std::string("\x00\xff", 2)) == "00ff");
    assert(Frequency(1) == 2412 && Frequency(13) == 2472);
    assert(Frequency(14) == 2484 && Frequency(36) == 0);
    std::vector<AccessPoint> rows;
    bool truncated = false;
    AccessPoint ap{" Home5G ", "001122334455", -80, 6, 3};
    AddAccessPoint(rows, ap, truncated);
    assert(rows.size() == 1 && rows[0].ssid == " Home5G ");
    ap.rssi = -40; AddAccessPoint(rows, ap, truncated);
    ap.rssi = -90; AddAccessPoint(rows, ap, truncated);
    assert(rows.size() == 1 && rows[0].rssi == -40);
    ap.bssid = "001122334456"; ap.channel = 36;
    AddAccessPoint(rows, ap, truncated); assert(rows.size() == 1);
    ap.channel = 6; ap.ssid.clear();
    AddAccessPoint(rows, ap, truncated); assert(rows.size() == 1);
    for (unsigned i = 0; i != 40; ++i) {
        char mac[13]; snprintf(mac, sizeof(mac), "%012x", i);
        AddAccessPoint(rows, {"AP", mac, -20-static_cast<int>(i), 1, 3}, truncated);
    }
    assert(rows.size() == kMaxNetworks && truncated);
    for (size_t i = 1; i < rows.size(); ++i) assert(rows[i-1].rssi >= rows[i].rssi);
    AccessPoint longest{std::string(32, '\x01'), "aabbccddeeff", -127, 14, 7};
    std::vector<AccessPoint> one{longest};
    std::cout << Reply("12345678", "abcdef01", State::Done, one, 0, false) << '\n';
    std::cout << Reply("12345678", "abcdef01", State::Done, one, -1, false) << '\n';
    std::cout << Reply("12345678", "abcdef01", State::Done, one, 1, false) << '\n';
    std::cout << Reply("12345678", "abcdef01", State::Done, {}, -1, false) << '\n';
    std::cout << Reply("12345678", "abcdef01", State::Scanning, {}, -1, false) << '\n';
    std::cout << ErrorReply("bad\"id", "abcdef01", Error::InvalidRequest) << '\n';
    std::cout << Reply("12345678", "abcdef01", State::Cancelled, {}, -1, false) << '\n';
    AccessPoint chinese{u8" 客厅5G ", "aabbccddeeff", -35, 6, 3};
    std::cout << Reply("12345678", "abcdef01", State::Done, {chinese}, 0, false) << '\n';
}
'''
    cpp = tmp_path / 'scan.cc'
    cpp.write_text(source)
    executable = tmp_path / 'scan'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(COMMON), str(cpp), '-o', str(executable)], check=True)
    lines = subprocess.check_output([str(executable)], text=True).splitlines()
    assert all(len(line.encode()) <= 238 for line in lines)
    page, status, bad_index, empty, scanning, bad_id, cancelled, chinese = map(json.loads, lines)
    assert bytes.fromhex(page['ap']['s']) == b'\x01' * 32
    assert page['index'] == 0 and page['total'] == 1 and page['ap']['c'] == 14
    assert 'ap' not in status and status['state'] == 'DONE'
    assert bad_index['error'] == 'INDEX_OUT_OF_RANGE'
    assert empty['state'] == 'DONE' and empty['total'] == 0
    assert scanning['state'] == 'SCANNING'
    assert bad_id['id'] == '' and bad_id['error'] == 'INVALID_REQUEST'
    assert cancelled['state'] == 'CANCELLED'
    assert bytes.fromhex(chinese['ap']['s']).decode() == ' 客厅5G '


def test_provisioning_does_not_change_receipt_or_credentials_for_scan():
    source = (COMMON / 'ygsoul_ble_provisioning.cc').read_text()
    worker = (COMMON / 'ygsoul_wifi_scan.h').read_text()
    assert 'cJSON_AddNumberToObject(json, "wifiScan", 1)' in source
    assert source.index('command == ygsoul::wifi_scan::kRequest') < source.index('command != ygsoul::ble::kWifiConfig')
    assert source.index('if (wifi_scan_.Busy())') < source.index('pairing_receipt_.Begin(token, ssid)')
    assert 'wifi_scan_.Reset()' in source and 'wifi_scan_.Close()' in source
    assert 'esp_ble_gatts_close' in source and 'notify_mutex_' in source
    event = source.split('void YgSoulBleProvisioning::OnNetworkEvent', 1)[1].split('void YgSoulBleProvisioning::OnWifiConnectTimeout', 1)[0]
    assert event.index('PairingReceipt::Result::Ignored') < event.index('SendStatus("SUCCEEDED")')
    assert event.index('PairingReceipt::Result::Mismatch') < event.index('SendStatus("SUCCEEDED")')
    assert 'SetString(kPairingReceiptKey, pairing_session_id)' in event
    assert 'AddSsid' not in worker and 'SetString' not in worker
    assert 'else if (!manager.IsConnected())' in worker and 'if (own_radio)' in worker
    assert 'generation_ == generation' in worker
    assert 'esp_wifi_scan_start(&config, true)' in worker
