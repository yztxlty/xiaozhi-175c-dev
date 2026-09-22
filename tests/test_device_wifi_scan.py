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
    assert(ValidScanId("0123abcd"));
    assert(!ValidScanId("")); assert(!ValidScanId("0123456789abcdefg"));
    assert(!ValidScanId("1234567\"")); assert(ValidScanId("1234567z"));
    assert(Base64(std::string("\x00\xff", 2)) == "AP8=");
    assert(Frequency(1) == 2412 && Frequency(13) == 2472);
    assert(Frequency(14) == 2484 && Frequency(36) == 0);
    std::vector<AccessPoint> rows;
    bool truncated = false;
    AccessPoint ap{" Home5G ", -80, true, 6, 3};
    AddAccessPoint(rows, ap, truncated);
    assert(rows.size() == 1 && rows[0].ssid == " Home5G ");
    ap.rssi = -40; AddAccessPoint(rows, ap, truncated);
    ap.rssi = -90; AddAccessPoint(rows, ap, truncated);
    assert(rows.size() == 1 && rows[0].rssi == -40);
    ap.channel = 36;
    AddAccessPoint(rows, ap, truncated); assert(rows.size() == 1);
    ap.channel = 6; ap.ssid.clear();
    AddAccessPoint(rows, ap, truncated); assert(rows.size() == 1);
    for (unsigned i = 0; i != 40; ++i) {
        char ssid[8]; snprintf(ssid, sizeof(ssid), "AP%02u", i);
        AddAccessPoint(rows, {ssid, -20-static_cast<int>(i), true, 1, 3}, truncated);
    }
    assert(rows.size() == kMaxNetworks && truncated);
    for (size_t i = 1; i < rows.size(); ++i) assert(rows[i-1].rssi >= rows[i].rssi);
    AccessPoint longest{std::string(32, '\x01'), -127, true, 14, 7};
    std::vector<AccessPoint> one{longest};
    std::cout << Reply("12345678", State::Ready, one, 0) << '\n';
    std::cout << Reply("12345678", State::Ready, one, -1) << '\n';
    std::cout << Reply("12345678", State::Ready, one, 1) << '\n';
    std::cout << Reply("12345678", State::Ready, {}, 0) << '\n';
    std::cout << Reply("12345678", State::Scanning, {}, 0) << '\n';
    std::cout << FailedReply("bad\"id", Error::InvalidRequest) << '\n';
    AccessPoint chinese{u8" 客厅5G ", -35, true, 6, 3};
    std::cout << Reply("12345678", State::Ready, {chinese}, 0) << '\n';
}
'''
    cpp = tmp_path / 'scan.cc'
    cpp.write_text(source)
    executable = tmp_path / 'scan'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(COMMON), str(cpp), '-o', str(executable)], check=True)
    lines = subprocess.check_output([str(executable)], text=True).splitlines()
    assert all(len(line.encode()) <= 238 for line in lines)
    page, bad_status, bad_index, empty, scanning, bad_id, chinese = map(json.loads, lines)
    import base64
    assert base64.b64decode(page['ap']['ssidBase64']) == b'\x01' * 32
    assert page['index'] == 0 and page['total'] == 1 and page['ap']['channel'] == 14
    assert bad_status['code'] == 'INDEX_OUT_OF_RANGE'
    assert bad_index['code'] == 'INDEX_OUT_OF_RANGE'
    assert empty['status'] == 'READY' and empty['total'] == 0 and empty['ap'] is None
    assert scanning['status'] == 'SCANNING'
    assert 'scanId' not in bad_id and bad_id['code'] == 'INVALID_REQUEST'
    assert base64.b64decode(chinese['ap']['ssidBase64']).decode() == ' 客厅5G '


def test_provisioning_does_not_change_receipt_or_credentials_for_scan():
    source = (COMMON / 'ygsoul_ble_provisioning.cc').read_text()
    worker = (COMMON / 'ygsoul_wifi_scan.h').read_text()
    assert 'cJSON_AddNumberToObject(json, "wifiScanVersion", 1)' in source
    assert source.index('command == ygsoul::wifi_scan::kRequest') < source.index('command != ygsoul::ble::kWifiConfig')
    assert source.index('if (wifi_scan_.Busy())') < source.index('pairing_receipt_.Begin(token, ssid)')
    wifi_config = source.split('if (wifi_scan_.Busy())', 1)[1]
    assert wifi_config.index('wifi_scan_.Reset()') < wifi_config.index('pairing_receipt_.Begin(token, ssid)')
    assert 'wifi_scan_.Reset()' in source and 'wifi_scan_.Close()' in source
    assert 'esp_ble_gatts_close' in source and 'notify_mutex_' in source
    event = source.split('void YgSoulBleProvisioning::OnNetworkEvent', 1)[1].split('void YgSoulBleProvisioning::OnWifiConnectTimeout', 1)[0]
    assert event.index('PairingReceipt::Result::Ignored') < event.index('SendStatus("SUCCEEDED")')
    assert event.index('PairingReceipt::Result::Mismatch') < event.index('SendStatus("SUCCEEDED")')
    assert 'ReplaceSsidListAndPairingSession(' in event
    assert 'merged_ssids_, pairing_session_id)' in event
    assert 'AddSsid' not in worker and 'SetString' not in worker
    assert 'else if (!manager.IsConnected())' in worker and 'if (own_radio)' in worker
    assert 'generation_ == generation' in worker
    assert 'esp_wifi_scan_start(&config, true)' in worker
    assert 'std::vector<ygsoul::wifi_scan::AccessPoint>().swap(rows_)' in worker


def test_wifi_station_stop_clears_all_attempt_state():
    source = (ROOT / 'components/78__esp-wifi-connect/wifi_station.cc').read_text()
    stop = source.split('void WifiStation::Stop()', 1)[1].split('void WifiStation::OnScanBegin', 1)[0]
    for reset in (
        'connect_queue_.clear()',
        'ssid_.clear()',
        'password_.clear()',
        'ip_address_.clear()',
        'reconnect_count_ = 0',
        'scan_current_interval_microseconds_ = scan_min_interval_microseconds_',
    ):
        assert reset in stop


def test_single_saved_ssid_uses_driver_target_scan_without_enumerating_all_aps():
    source = (ROOT / 'components/78__esp-wifi-connect/wifi_station.cc').read_text()
    handler = source.split('void WifiStation::WifiEventHandler', 1)[1].split(
        'void WifiStation::IpEventHandler', 1
    )[0]
    station_start = handler.split('WIFI_EVENT_STA_START', 1)[1].split(
        'WIFI_EVENT_SCAN_DONE', 1
    )[0]
    assert 'GetSsidList()' in station_start
    assert '.size() == 1' in station_start
    assert 'StartConnect()' in station_start
    assert station_start.index('StartConnect()') < station_start.index('esp_wifi_scan_start(nullptr, false)')


def test_got_ip_uses_the_current_netif_and_real_associated_ssid():
    source = (ROOT / 'components/78__esp-wifi-connect/wifi_station.cc').read_text()
    handler = source.split('void WifiStation::IpEventHandler', 1)[1]

    assert 'event->esp_netif != this_->station_netif_' in handler
    assert 'esp_netif_get_ip_info' in handler
    assert 'esp_wifi_sta_get_ap_info' in handler
    assert 'on_connected_(connected_ssid)' in handler
    assert 'on_connected_(this_->ssid_)' not in handler


def test_delayed_scan_timer_cannot_cross_wifi_station_sessions():
    source = (ROOT / 'components/78__esp-wifi-connect/wifi_station.cc').read_text()
    header = (ROOT / 'components/78__esp-wifi-connect/include/wifi_station.h').read_text()
    start = source.split('void WifiStation::Start()', 1)[1].split(
        'bool WifiStation::WaitForConnected', 1
    )[0]
    stop = source.split('void WifiStation::Stop()', 1)[1].split(
        'void WifiStation::OnScanBegin', 1
    )[0]

    assert 'std::atomic<int64_t> scan_deadline_us_' in header
    assert 'scan_deadline_us_.store(0)' in start
    assert 'scan_deadline_us_.store(0)' in stop
    assert 'ScheduleScan(' in source
    assert 'compare_exchange_strong' in start
    assert 'esp_wifi_scan_start(nullptr, false)' in start


def test_wifi_manager_never_holds_state_mutex_while_stopping_station():
    source = (ROOT / 'components/78__esp-wifi-connect/wifi_manager.cc').read_text()
    header = (ROOT / 'components/78__esp-wifi-connect/include/wifi_manager.h').read_text()
    stop = source.split('void WifiManager::StopStation()', 1)[1].split(
        'bool WifiManager::IsConnected', 1
    )[0]
    start_ap = source.split('void WifiManager::StartConfigAp()', 1)[1].split(
        'void WifiManager::StopConfigAp', 1
    )[0]

    assert 'operation_mutex_' in header
    assert 'std::lock_guard<std::mutex> operation_lock(operation_mutex_)' in stop
    assert 'station_active_ = false;' in stop
    assert stop.index('station_active_ = false;') < stop.index('station->Stop();')
    assert 'station_->Stop();' not in stop
    assert 'station_->Stop();' not in start_ap
