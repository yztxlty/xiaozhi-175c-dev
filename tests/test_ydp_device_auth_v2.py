"""Host golden vectors for ESP32 Device Auth v2. Public test keys only."""
import hashlib
import hmac
import json
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from device_auth_v2_reference import signing_bytes


ROOT = pathlib.Path(__file__).resolve().parents[1]
GOLDEN = ROOT / 'tests' / 'device_auth_v2_golden.json'
BOOTSTRAP = ROOT / 'main/protocols/ydp_bootstrap.cc'
BOOTSTRAP_H = ROOT / 'main/protocols/ydp_bootstrap.h'
CLIENT = ROOT / 'main/protocols/ydp_client.cc'
HELPER = ROOT / 'main/protocols/ydp_device_auth.h'
HOST_CC = ROOT / 'tests/host/ydp_device_auth_v2_host.cc'


def load_golden():
    return json.loads(GOLDEN.read_text())


def test_golden_vectors_match_python_reference_including_zero_and_high_bytes():
    document = load_golden()
    signatures = {}
    for vector in document['vectors']:
        fields = vector['fields']
        key = bytes.fromhex(vector['key_hex'])
        assert len(key) == 32
        expected = signing_bytes(**fields)
        assert expected == bytes.fromhex(vector['signing_bytes_hex'])
        assert not expected.endswith(b'\n')
        assert expected.count(b'\n') == 10
        assert expected.split(b'\n')[0] == b'YGSoul-Device-Auth-v2'
        assert expected.split(b'\n')[-1] == b'2'
        digest = hmac.new(key, expected, hashlib.sha256).hexdigest()
        assert digest == vector['signature_hex']
        signatures[vector['name']] = digest

    assert signatures['python_reference_activate_mqtt'] != signatures['activate_refresh_must_differ']
    assert signatures['python_reference_activate_mqtt'] != signatures['mqtt_websocket_must_differ']

    mixed = next(v for v in document['vectors'] if v['name'] == 'zero_and_high_key_bytes')
    mixed_key = bytes.fromhex(mixed['key_hex'])
    assert 0x00 in mixed_key
    assert 0xff in mixed_key
    assert any(byte >= 0x80 for byte in mixed_key)
    reference_key = bytes.fromhex(document['vectors'][0]['key_hex'])
    assert 0x00 in reference_key


def test_host_helper_matches_golden_signing_bytes(tmp_path):
    document = load_golden()
    cases = []
    for index, vector in enumerate(document['vectors']):
        fields = vector['fields']
        cases.append(f'''
    {{
        const std::string actual = ygsoul::ydp::BuildDeviceAuthSigningMessage(
            "{fields['device_id']}", "{fields['product_key']}", {fields['key_version']},
            "{fields['firmware_version']}", "{fields['device_nonce']}", "{fields['timestamp']}",
            "{fields['challenge']}", "{fields['purpose']}", "{fields['transport']}");
        const std::string hex = ygsoul::ydp::ToLowerHex(
            reinterpret_cast<const uint8_t*>(actual.data()), actual.size());
        assert(!actual.empty());
        assert(actual.back() != '\\n');
        assert(hex == "{vector['signing_bytes_hex']}");
    }}
''')
    harness = '''
#include "ydp_device_auth.h"
#include <cassert>
#include <string>
int main() {
''' + '\n'.join(cases) + '''
    return 0;
}
'''
    executable = tmp_path / 'ydp-device-auth-v2'
    compile_result = subprocess.run(
        ['g++', '-std=c++17', '-I', str(ROOT / 'main/protocols'), '-x', 'c++', '-', '-o', str(executable)],
        input=harness, text=True, capture_output=True,
    )
    assert compile_result.returncode == 0, compile_result.stderr
    result = subprocess.run([str(executable)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def test_committed_host_helper_passes():
    executable = pathlib.Path('/tmp/ydp-device-auth-v2-host')
    compile_result = subprocess.run(
        ['g++', '-std=c++17', '-I', str(ROOT / 'main/protocols'), str(HOST_CC), '-o', str(executable)],
        capture_output=True, text=True,
    )
    assert compile_result.returncode == 0, compile_result.stderr
    result = subprocess.run([str(executable)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    document = load_golden()
    expected = next(v['signing_bytes_hex'] for v in document['vectors']
                    if v['name'] == 'python_reference_activate_mqtt')
    assert result.stdout.strip() == expected


def test_firmware_sends_v2_fields_and_keeps_hardware_hmac():
    source = BOOTSTRAP.read_text()
    header = BOOTSTRAP_H.read_text()
    client = CLIENT.read_text()
    helper = HELPER.read_text()

    for token in (
        'YGSoul-Device-Auth-v2',
        '"authVersion"',
        '"keyVersion"',
        '"firmwareVersion"',
        '"deviceNonce"',
        '"purpose"',
        '"transport"',
        '"timestamp"',
        '"challenge"',
        'kPurposeActivate',
        'kPurposeRefresh',
        'kTransportMqtt',
        'kTransportWebsocket',
        '/ydp/v1/activation/challenge',
        '/ydp/v1/activate',
        '/ydp/v1/credentials/refresh',
        'esp_hmac_calculate(HMAC_KEY0',
        'BuildDeviceAuthSigningMessage',
    ):
        assert token in source or token in helper, token

    assert 'void Refresh(' in header
    assert 'YdpBootstrap::GetInstance().Activate' in client
    assert 'YdpBootstrap::GetInstance().Refresh' in client
    assert 'SetKeyVersion' in client
    assert 'SetTransport' in client
    assert 'esp_hmac_calculate(HMAC_KEY0' in source
    assert 'HMAC_KEY0' in source
    assert 'get_string("deviceNumber")' in source
    assert 'get_string("deviceId")' in source
    assert 'Activation identity mismatch' in source


def test_auth_paths_never_log_keys_signatures_or_bodies():
    for path in (BOOTSTRAP, CLIENT):
        text = path.read_text()
        for line in text.splitlines():
            if 'ESP_LOG' not in line:
                continue
            lowered = line.lower()
            for forbidden in ('signature', 'auth_key', 'payload', 'hmac', 'nonce', 'challenge'):
                assert forbidden not in lowered, line
        assert 'auth_key_' not in text
        assert 'ReadAll().c_str()' not in text
