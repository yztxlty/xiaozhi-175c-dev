import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_wifi_status_exposes_real_rssi_and_asset_storage_free_bytes():
    source = (ROOT / "main/boards/common/wifi_board.cc").read_text()
    assets = (ROOT / "main/assets.h").read_text()

    assert 'cJSON_AddNumberToObject(network, "rssi", rssi);' in source
    assert 'auto storage_free = assets.GetFreeSpace();' in source
    assert 'cJSON_AddNumberToObject(root, "storageFree", storage_free);' in source
    assert 'size_t GetFreeSpace() const' in assets
    assert 'used_size_ + transient_size_' in assets
    assert 'partition_->size - used' in assets


def test_management_commands_cover_reported_volume_and_brightness():
    source = (ROOT / "main/application.cc").read_text()

    assert 'strcmp(command->valuestring, "setVolume") == 0' in source
    assert 'strcmp(command->valuestring, "setBrightness") == 0' in source
    assert 'backlight->SetBrightness(value->valueint, true);' in source
    assert 'command_changes_telemetry' in source


def test_telemetry_uses_saved_brightness_not_an_in_progress_dim_transition():
    status = (ROOT / "main/boards/common/wifi_board.cc").read_text()
    screen = status[status.index("// Screen"):status.index("// Battery")]

    assert 'Settings settings("display", false);' in screen
    assert 'settings.GetInt("brightness", 75)' in screen
    assert 'backlight->brightness()' not in screen


def test_voice_mcp_controls_report_telemetry_immediately():
    source = (ROOT / "main/mcp_server.cc").read_text()
    volume_start = source.index('AddTool("self.audio_speaker.set_volume"')
    brightness_start = source.index('AddTool("self.screen.set_brightness"')
    volume = source[volume_start:brightness_start]
    brightness = source[brightness_start:source.index('#ifdef HAVE_LVGL', brightness_start)]
    assert 'Application::GetInstance().ReportDeviceTelemetry();' in volume
    assert 'Application::GetInstance().ReportDeviceTelemetry();' in brightness


def test_management_commands_ack_unbind_then_wipes_wifi_and_enters_pairing():
    source = (ROOT / "main/application.cc").read_text()
    assert 'strcmp(command->valuestring, "unbind") == 0' in source
    assert 'cJSON_AddBoolToObject(reported, "unbound", true)' in source
    unbind_idx = source.index('strcmp(command->valuestring, "unbind") == 0')
    factory_idx = source.index('strcmp(command->valuestring, "factoryReset") == 0')
    command_block = source[unbind_idx:factory_idx]
    reported_unbind_idx = source.index('if (succeeded && strcmp(command->valuestring, "unbind") == 0)', factory_idx)
    after_ack_idx = source.index('if (succeeded && strcmp(command->valuestring, "unbind") == 0)', reported_unbind_idx + 1)
    after_ack_end = source.index('if (succeeded && strcmp(command->valuestring, "factoryReset") == 0)', after_ack_idx)
    unbind_after_ack = source[after_ack_idx:after_ack_end]
    assert 'CRITICAL-RUNTIME-CONTRACT' in command_block
    assert 'SsidManager::GetInstance().Clear()' in unbind_after_ack
    assert 'wifi_settings.EraseAll()' in unbind_after_ack
    assert 'WifiManager::GetInstance().StopStation()' in unbind_after_ack
    assert 'EnterWifiConfigMode()' in unbind_after_ack
    assert 'Reboot();' not in unbind_after_ack


def test_management_commands_factory_reset_reboots_after_wiping_wifi():
    source = (ROOT / "main/application.cc").read_text()
    assert 'strcmp(command->valuestring, "factoryReset") == 0' in source
    assert 'cJSON_AddBoolToObject(reported, "factoryReset", true)' in source
    assert 'factoryReset ACK sent, wiping wifi after flush' in source
    ack_idx = source.index('cJSON_AddBoolToObject(reported, "factoryReset", true)')
    factory_schedule_idx = source.index('if (succeeded && strcmp(command->valuestring, "factoryReset") == 0)', ack_idx)
    wipe_idx = source.index('SsidManager::GetInstance().Clear()', factory_schedule_idx)
    assert ack_idx < wipe_idx
    reset_block = source[wipe_idx:source.index('\n}', wipe_idx)]
    assert 'Reboot();' in reset_block
    assert 'EnterWifiConfigMode();' not in reset_block


def test_management_commands_cover_supported_auto_sleep_and_periodic_telemetry():
    application = (ROOT / "main/application.cc").read_text()
    client = (ROOT / "main/device_management_client.cc").read_text()
    status = (ROOT / "main/boards/common/wifi_board.cc").read_text()

    assert 'strcmp(command->valuestring, "setAutoSleep") == 0' in application
    assert 'board.SetAutoSleepMinutes(value->valueint)' in application
    assert 'management_client_->OnHeartbeat' in application
    assert 'on_heartbeat_' in client
    assert 'cJSON_AddNumberToObject(root, "autoSleep", auto_sleep_minutes);' in status


def test_ygsoul_board_applies_and_reports_configured_auto_sleep_minutes():
    board = (ROOT / "main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc").read_text()

    assert 'virtual bool SetAutoSleepMinutes(int minutes) override' in board
    assert 'minutes != 1 && minutes != 5 && minutes != 15 && minutes != 30' in board
    assert 'power_save_timer_->SetSleepTimeout(minutes * 60);' in board
    assert 'Settings settings("power", true);' in board
    assert 'settings.SetInt("auto_sleep", minutes);' in board
    assert 'virtual int GetAutoSleepMinutes() override' in board
    assert 'settings.GetInt("auto_sleep", 1)' in board
    assert 'new PowerSaveTimer(-1, GetAutoSleepMinutes() * 60, -1)' in board


def test_clear_storage_erases_transient_flash_without_touching_valid_pack():
    assets = (ROOT / "main/assets.cc").read_text()
    header = (ROOT / "main/assets.h").read_text()
    application = (ROOT / "main/application.cc").read_text()
    start = assets.index('bool Assets::PurgeTransient()')
    end = assets.find('\nbool ', start + 1)
    body = assets[start:] if end < 0 else assets[start:end]
    assert 'esp_partition_erase_range' in body
    assert 'UnApplyPartition()' in body
    assert 'pack_valid_' in body
    assert 'pack_layout_known_' in body
    assert 'ScanTransientOccupancy' in body
    assert 'clearStorage: assets layout unknown, skip flash erase' in body
    assert 'transient_size_' in header
    assert 'pack_layout_known_' in header
    assert 'strcmp(command->valuestring, "clearStorage") == 0' in application
    assert 'Assets::GetInstance().PurgeTransient()' in application


def test_active_role_command_persists_and_reports_role_and_voice_ids():
    application = (ROOT / "main/application.cc").read_text()
    status = (ROOT / "main/boards/common/wifi_board.cc").read_text()
    store = (ROOT / "main/device_content/role_visual_store.cc").read_text()

    assert 'strcmp(command->valuestring, "setActiveRole") == 0' in application
    assert 'companion.SetString("active_role"' in application
    assert 'companion.SetString("voice_profile"' in application
    assert 'cJSON_AddStringToObject(root, "activeRoleId"' in status
    assert 'cJSON_AddStringToObject(root, "voiceProfileId"' in status


def test_companion_text_visibility_is_persisted_reported_and_gates_only_display():
    application = (ROOT / "main/application.cc").read_text()
    header = (ROOT / "main/application.h").read_text()
    status = (ROOT / "main/boards/common/wifi_board.cc").read_text()

    assert 'show_asr_text_' in header
    assert 'show_tts_text_' in header
    assert 'showAsrText' in application
    assert 'showTtsText' in application
    assert 'companion.SetInt("show_asr"' in application
    assert 'companion.SetInt("show_tts"' in application
    assert 'if (show_asr_text_)' in application
    assert 'if (show_tts_text_)' in application
    assert 'cJSON_AddBoolToObject(root, "showAsrText"' in status
    assert 'cJSON_AddBoolToObject(root, "showTtsText"' in status


def test_active_role_command_persists_exact_configuration_receipt_without_touching_voice_runtime():
    application = (ROOT / "main/application.cc").read_text()
    status = (ROOT / "main/boards/common/wifi_board.cc").read_text()
    store = (ROOT / "main/device_content/role_visual_store.cc").read_text()
    block = application[application.index('strcmp(command->valuestring, "setActiveRole") == 0'):]
    block = block[:block.index('strcmp(command->valuestring, "reportStatus") == 0')]

    for key in ('configurationRevision', 'roleVisualResourceId', 'roleVisualVersion'):
        assert f'"{key}"' in application
        assert f'"{key}"' in status
    assert 'RoleVisualStore::GetInstance().Commit' in block
    assert 'writable.SetInt("cfg_rev"' in store
    assert 'writable.SetString("role_res"' in store
    assert 'writable.SetInt("role_ver"' in store
    assert 'OpenAudioChannel' not in block
    assert 'SetDeviceState' not in block
    assert 'audio_' not in block


def test_role_commit_refreshes_voice_session_only_when_voice_changes_after_ack():
    application = (ROOT / "main/application.cc").read_text()
    header = (ROOT / "main/application.h").read_text()
    response = application[application.index('auto response = cJSON_CreateObject()'):application.index('if (succeeded && strcmp(command->valuestring, "checkOta") == 0)')]

    assert 'RefreshVoiceSessionAfterRoleChange' in header
    assert 'voice_session_refresh_required' in response
    assert 'RefreshVoiceSessionAfterRoleChange' in response
    assert response.index('management_client_->Send(text)') < response.index('RefreshVoiceSessionAfterRoleChange')


def test_same_role_visual_revision_does_not_decode_the_same_jpeg_again():
    store = (ROOT / "main/device_content/role_visual_store.cc").read_text()
    commit = store[store.index("bool RoleVisualStore::Commit"):store.index("void RoleVisualStore::LoadActive")]
    fast_path = commit[commit.index("active_revision"):commit.index('resource_id == "BUILTIN_FALLBACK"')]
    assert 'current.GetString("role_res") == resource_id' in fast_path
    assert 'current.GetInt("role_ver", 0) == resource_version' in fast_path
    assert 'SetRoleImage' not in fast_path


def test_display_lock_guard_never_unlocks_a_lock_it_did_not_acquire():
    display = (ROOT / "main/display/display.h").read_text()
    guard = display[display.index("class DisplayLockGuard"):display.index("class NoDisplay")]
    assert "locked_" in guard
    assert "if (locked_)" in guard


def test_tts_subtitle_accumulates_sentences_until_current_reply_finishes():
    application = (ROOT / "main/application.cc").read_text()
    header = (ROOT / "main/application.h").read_text()
    tts = application[application.index('strcmp(type->valuestring, "tts")'):application.index('strcmp(type->valuestring, "stt")')]
    assert "tts_display_text_" in header
    assert "tts_display_text_.clear()" in tts
    assert "tts_display_text_ +=" in tts
    assert 'Schedule([display, message = tts_display_text_]' in tts
    assert 'SetChatMessage("assistant", message.c_str())' in tts


def test_role_visual_uses_existing_jpeg_decoder_before_lvgl_display():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    block = board[board.index('bool SetRoleImage(const char* path)'):]
    assert 'jpeg_to_image(' in block
    assert 'std::make_unique<LvglAllocatedImage>(' in block
    assert 'decoded_data, decoded_size, decoded_width, decoded_height' in block
    assert 'decoded_stride, LV_COLOR_FORMAT_RGB565' in block


def test_custom_role_image_uses_high_contrast_conversation_overlay():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()

    color = board[board.index('void ApplyYGSoulChatMessageColor()'):board.index('void ApplyYGSoulOverlayStyle()')]
    overlay = board[board.index('void ApplyYGSoulOverlayStyle()'):board.index('void RaiseYGSoulChrome()')]
    assert 'custom_role_image_' in color
    assert 'lv_color_white()' in color
    assert 'LV_OPA_60' in overlay
    assert 'lv_color_black()' in overlay


def test_role_visual_download_is_isolated_from_conversation_runtime():
    application = (ROOT / "main/application.cc").read_text()
    cmake = (ROOT / "main/CMakeLists.txt").read_text()
    store = (ROOT / "main/device_content/role_visual_store.cc").read_text()
    display = (ROOT / "main/display/display.h").read_text()

    assert 'prepareRoleVisual' in application
    assert 'commitActiveRole' in application
    assert 'device_content/role_visual_store.cc' in cmake
    assert 'RoleVisualStore::Prepare' in store
    assert 'RoleVisualStore::Commit' in store
    assert 'SetRoleImage' in display
    assert 'std::remove(SlotPath(old_slot).c_str())' in store
    assert 'EraseKey("prep_slot")' in store
    assert 'resource_id == "BUILTIN_FALLBACK"' in store
    assert 'SetRoleImage("")' in store
    assert 'void RoleVisualStore::Clear()' in store
    assert application.count('RoleVisualStore::GetInstance().Clear()') == 2
    assert 'audio_service_' not in store
    assert 'SetDeviceState' not in store


def test_opening_code_is_persisted_before_ready_without_audio_changes():
    application = (ROOT / "main/application.cc").read_text()
    opening = application[application.index('strcmp(type->valuestring, "opening")'):application.index('strcmp(type->valuestring, "device.standby")')]
    assert 'SetString("opening_code"' in opening
    assert opening.index('SetString("opening_code"') < opening.index('"READY"')
    assert 'SendDeviceMessage' in opening
    assert 'audio_service_' not in opening
    assert 'SetDeviceState' not in opening


def test_device_attributes_expose_real_identity_and_firmware_metadata():
    status = (ROOT / "main/boards/common/wifi_board.cc").read_text()

    for key in ('vendorSn', 'productKey', 'hardwareVersion', 'firmwareVersion'):
        assert f'"{key}"' in status


if __name__ == "__main__":
    test_wifi_status_exposes_real_rssi_and_asset_storage_free_bytes()
    test_management_commands_cover_reported_volume_and_brightness()
    test_telemetry_uses_saved_brightness_not_an_in_progress_dim_transition()
    test_voice_mcp_controls_report_telemetry_immediately()
    test_management_commands_ack_unbind_then_wipes_wifi_and_enters_pairing()
    test_management_commands_factory_reset_reboots_after_wiping_wifi()
    test_management_commands_cover_supported_auto_sleep_and_periodic_telemetry()
    test_ygsoul_board_applies_and_reports_configured_auto_sleep_minutes()
    test_clear_storage_erases_transient_flash_without_touching_valid_pack()
    test_active_role_command_persists_exact_configuration_receipt_without_touching_voice_runtime()
    test_role_visual_download_is_isolated_from_conversation_runtime()
    test_role_commit_refreshes_voice_session_only_when_voice_changes_after_ack()
    test_same_role_visual_revision_does_not_decode_the_same_jpeg_again()
    test_display_lock_guard_never_unlocks_a_lock_it_did_not_acquire()
    test_tts_subtitle_accumulates_sentences_until_current_reply_finishes()
    test_opening_code_is_persisted_before_ready_without_audio_changes()
    test_device_attributes_expose_real_identity_and_firmware_metadata()
    print("PASS: telemetry contract")
