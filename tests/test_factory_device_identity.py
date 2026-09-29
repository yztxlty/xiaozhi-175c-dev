from pathlib import Path


ROOT = Path(__file__).resolve().parents[1] / 'main'


def test_factory_device_id_is_used_by_auth_and_device_channels():
    system_info = (ROOT / 'system_info.cc').read_text()
    assert 'SystemInfo::GetDeviceId()' in system_info
    assert 'nvs_open_from_partition(kFactoryPartition, "factory"' in system_info
    assert 'nvsfactory' in system_info

    for relative in (
        'ota.cc',
        'device_management_client.cc',
        'protocols/websocket_protocol.cc',
        'protocols/ydp_bootstrap.cc',
        'music/music_client.cc',
    ):
        assert 'SystemInfo::GetDeviceId()' in (ROOT / relative).read_text(), relative
    assert 'http->SetHeader("Device-Id", SystemInfo::GetDeviceId().c_str())' in (
        ROOT / 'device_content/role_animation_store.cc'
    ).read_text()


def test_factory_key_slot_can_select_a_separate_hardware_key():
    source = (ROOT / 'protocols/ydp_bootstrap.cc').read_text()
    assert 'SystemInfo::GetAuthKeySlot()' in source
    assert 'esp_hmac_calculate(' in source
    assert 'SystemInfo::GetFlashAuthKey(key)' in source
    assert 'mbedtls_md_hmac(' in source
    assert 'kPurposeProvision, kProofPath' in source
    assert 'FactoryProofPending()' in (ROOT / 'application.cc').read_text()
