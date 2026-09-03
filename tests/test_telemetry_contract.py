import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_wifi_status_exposes_real_rssi_and_asset_storage_free_bytes():
    source = (ROOT / "main/boards/common/wifi_board.cc").read_text()
    assets = (ROOT / "main/assets.h").read_text()

    assert 'cJSON_AddNumberToObject(network, "rssi", rssi);' in source
    assert 'Assets::GetInstance().GetFreeSpace()' in source
    assert 'cJSON_AddNumberToObject(root, "storageFree", storage_free);' in source
    assert 'size_t GetFreeSpace() const' in assets
    assert 'partition_->size - used_size_' in assets


def test_management_commands_cover_reported_volume_and_brightness():
    source = (ROOT / "main/application.cc").read_text()

    assert 'strcmp(command->valuestring, "setVolume") == 0' in source
    assert 'strcmp(command->valuestring, "setBrightness") == 0' in source
    assert 'backlight->SetBrightness(value->valueint, true);' in source
    assert 'command_changes_telemetry' in source


def test_management_commands_ack_unbind_without_wiping_wifi():
    source = (ROOT / "main/application.cc").read_text()
    assert 'strcmp(command->valuestring, "unbind") == 0' in source
    assert 'cJSON_AddBoolToObject(reported, "unbound", true)' in source
    unbind_idx = source.index('strcmp(command->valuestring, "unbind") == 0')
    factory_idx = source.index('strcmp(command->valuestring, "factoryReset") == 0')
    unbind_block = source[unbind_idx:factory_idx]
    assert 'SsidManager::GetInstance().Clear()' not in unbind_block
    assert 'EnterWifiConfigMode' not in unbind_block


def test_management_commands_factory_reset_acks_before_wiping_wifi():
    source = (ROOT / "main/application.cc").read_text()
    assert 'strcmp(command->valuestring, "factoryReset") == 0' in source
    assert 'cJSON_AddBoolToObject(reported, "factoryReset", true)' in source
    assert 'factoryReset ACK sent, wiping wifi after flush' in source
    ack_idx = source.index('cJSON_AddBoolToObject(reported, "factoryReset", true)')
    wipe_idx = source.index('SsidManager::GetInstance().Clear()')
    assert ack_idx < wipe_idx
    board = (ROOT / "main/boards/common/wifi_board.cc").read_text()
    assert 'kDeviceStateActivating' in board
    assert 'EnterWifiConfigMode' in source


def test_management_commands_cover_supported_auto_sleep_and_periodic_telemetry():
    application = (ROOT / "main/application.cc").read_text()
    client = (ROOT / "main/device_management_client.cc").read_text()
    status = (ROOT / "main/boards/common/wifi_board.cc").read_text()

    assert 'strcmp(command->valuestring, "setAutoSleep") == 0' in application
    assert 'board.SetAutoSleepMinutes(value->valueint)' in application
    assert 'management_client_->OnHeartbeat' in application
    assert 'on_heartbeat_' in client
    assert 'cJSON_AddNumberToObject(root, "autoSleep", auto_sleep_minutes);' in status


def test_device_attributes_expose_real_identity_and_firmware_metadata():
    status = (ROOT / "main/boards/common/wifi_board.cc").read_text()

    for key in ('vendorSn', 'productKey', 'hardwareVersion', 'firmwareVersion'):
        assert f'"{key}"' in status


if __name__ == "__main__":
    test_wifi_status_exposes_real_rssi_and_asset_storage_free_bytes()
    test_management_commands_cover_reported_volume_and_brightness()
    test_management_commands_ack_unbind_without_wiping_wifi()
    test_management_commands_factory_reset_acks_before_wiping_wifi()
    test_management_commands_cover_supported_auto_sleep_and_periodic_telemetry()
    test_device_attributes_expose_real_identity_and_firmware_metadata()
    print("PASS: telemetry contract")
