from pathlib import Path


ROOT = Path(__file__).parents[1]


def test_role_animation_install_is_atomic_and_rejects_unsafe_or_non_eaf_items():
    source = (ROOT / "main/device_content/role_animation_store.cc").read_text()
    assert 'format->valuestring, "eaf"' in source
    assert "ContentStorage::IsSafeFileName" in source
    assert "incoming_actions" in source
    assert "if (!was_active)" in source
    assert "std::remove(path.c_str())" in source
    assert "ValidateRoleImage(path.c_str(), \"eaf\")" in source
    assert 'SaveManifest(inactive_slot' in source
    assert 'SetString("role_animation_slot", inactive_slot)' in source
    assert source.index('SaveManifest(inactive_slot') < source.index('SetString("role_animation_slot", inactive_slot)')
    assert "if (!retained)" in source
    assert "std::remove(previous.path.c_str())" in source


def test_boot_rejects_incomplete_or_missing_role_animation_package():
    source = (ROOT / "main/device_content/role_animation_store.cc").read_text()
    assert "loaded.size() != static_cast<size_t>(cJSON_GetArraySize(actions))" in source
    assert 'FILE* asset_file = fopen(path.c_str(), "rb")' in source


def test_role_animation_commands_report_exact_manifest_and_query_same_snapshot():
    application = (ROOT / "main/application.cc").read_text()
    assert 'strcmp(command->valuestring, "applyRoleAnimations")' in application
    assert 'strcmp(command->valuestring, "getRoleAnimations")' in application
    assert 'RoleAnimationStore::GetInstance().AddReported(reported, request_id->valuestring)' in application


def test_only_real_listening_and_speaking_events_are_advertised_and_routed():
    board = (ROOT / "main/boards/common/wifi_board.cc").read_text()
    application = (ROOT / "main/application.cc").read_text()
    assert 'cJSON_AddStringToArray(actions, "listening")' in board
    assert 'cJSON_AddStringToArray(actions, "speaking")' in board
    assert 'cJSON_AddNumberToObject(capability, "maxActions", 2)' in board
    assert 'RoleAnimationStore::GetInstance().Show("listening")' in application
    assert 'RoleAnimationStore::GetInstance().Show("speaking")' in application


def test_role_switch_invalidates_old_action_package_without_touching_audio_state_machine():
    application = (ROOT / "main/application.cc").read_text()
    commit = application[application.index('strcmp(command->valuestring, "commitActiveRole")'):]
    assert 'RoleAnimationStore::GetInstance().ClearForRoleChange' in commit
    store = (ROOT / "main/device_content/role_animation_store.cc").read_text()
    assert 'RoleVisualStore::GetInstance().LoadActive()' in store
