from pathlib import Path
import subprocess
import tempfile


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
    assert 'SetString("role_anim_slot", inactive_slot)' in source
    assert source.index('SaveManifest(inactive_slot') < source.index('SetString("role_anim_slot", inactive_slot)')
    assert "if (!retained)" in source
    assert "std::remove(previous.path.c_str())" in source
    assert '"role_anim_slot"' in source
    assert '"role_animation_slot"' not in source


def test_boot_rejects_incomplete_or_missing_role_animation_package():
    source = (ROOT / "main/device_content/role_animation_store.cc").read_text()
    assert "loaded.size() != static_cast<size_t>(cJSON_GetArraySize(actions))" in source
    assert 'FILE* asset_file = fopen(path.c_str(), "rb")' in source


def test_role_animation_download_identifies_the_physical_device():
    source = (ROOT / "main/device_content/role_animation_store.cc").read_text()
    assert '#include "system_info.h"' in source
    assert 'http->SetHeader("Device-Id", SystemInfo::GetDeviceId().c_str())' in source
    assert "http->SetTimeout(90000)" in source


def test_role_animation_commands_report_exact_manifest_and_query_same_snapshot():
    application = (ROOT / "main/application.cc").read_text()
    assert 'strcmp(command->valuestring, "applyRoleAnimations")' in application
    assert 'strcmp(command->valuestring, "getRoleAnimations")' in application
    assert 'RoleAnimationStore::GetInstance().AddReported(reported, request_id->valuestring)' in application


def test_animation_report_exposes_gallery_space_for_set_replacement_check():
    source = (ROOT / "main/device_content/role_animation_store.cc").read_text()
    reported = source[source.index("void RoleAnimationStore::AddReported"):].split("\n}", 1)[0]
    assert '"roleAnimationGalleryUsedBytes"' in reported
    assert '"roleAnimationGalleryFreeBytes"' in reported


def test_gallery_reservation_can_stage_second_set_then_release_old_one():
    code = r'''
#include "main/storage/content_storage.h"
#include <cassert>
int main() {
    ContentStorage::Stats stats{};
    stats.gallery_total_bytes = 6291456;
    stats.gallery_bytes = 3781462;
    stats.gallery_free_bytes = 2367488;
    for (size_t bytes : {324065U, 362608U, 353713U, 313378U, 318411U}) {
        assert(ContentStorage::CanReserve(stats, ContentStorage::Category::Gallery, bytes));
        stats.gallery_bytes += bytes;
        stats.gallery_free_bytes -= bytes;
    }
    assert(stats.gallery_free_bytes > 512 * 1024);
}
'''
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "gallery.cpp"
        binary = Path(directory) / "gallery"
        source.write_text(code)
        subprocess.run(["c++", "-std=c++17", "-I", str(ROOT), str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


def test_failed_set_switch_restarts_previous_animation_instead_of_leaving_screen_blank():
    application = (ROOT / "main/application.cc").read_text()
    apply = application[application.index('strcmp(command->valuestring, "applyRoleAnimations")'):]
    apply = apply.split('strcmp(command->valuestring, "getRoleAnimations")', 1)[0]
    assert 'if (!succeeded) RoleAnimationStore::GetInstance().Resume()' in apply
    source = (ROOT / "main/device_content/role_animation_store.cc").read_text()
    resume = source[source.index('void RoleAnimationStore::Resume()'):].split('\n}', 1)[0]
    assert 'current_action_.clear()' in resume
    assert 'Show(visible_action)' in resume


def test_animation_receipt_keeps_companion_role_and_configuration_fields():
    source = (ROOT / "main/device_content/role_animation_store.cc").read_text()
    reported = source[source.index("void RoleAnimationStore::AddReported"):].split("\n}", 1)[0]
    assert 'cJSON_AddStringToObject(reported, "activeRoleId"' not in reported
    assert 'cJSON_AddNumberToObject(reported, "configurationRevision"' not in reported


def test_role_animation_accepts_cloud_only_revision_bump_but_rejects_stale_revision():
    source = (ROOT / "main/device_content/role_animation_store.cc").read_text()
    assert 'config->valueint < companion.GetInt("cfg_rev", 0)' in source
    assert 'config->valueint != companion.GetInt("cfg_rev", 0)' not in source


def test_five_actions_are_advertised_and_dialogue_events_are_routed():
    board = (ROOT / "main/boards/common/wifi_board.cc").read_text()
    application = (ROOT / "main/application.cc").read_text()
    assert 'cJSON_AddItemToArray(actions, cJSON_CreateString("listening"))' in board
    assert 'cJSON_AddItemToArray(actions, cJSON_CreateString("speaking"))' in board
    assert 'cJSON_AddItemToArray(actions, cJSON_CreateString("thinking"))' in board
    assert 'cJSON_AddItemToArray(actions, cJSON_CreateString("idle"))' in board
    assert 'cJSON_AddItemToArray(actions, cJSON_CreateString("faint"))' in board
    assert 'cJSON_AddNumberToObject(capability, "maxActions", 5)' in board
    assert 'cJSON_AddNumberToObject(capability, "maxFileBytes", 512 * 1024)' in board
    assert 'RoleAnimationStore::GetInstance().Show("idle")' in application
    assert 'RoleAnimationStore::GetInstance().Show("listening")' in application
    assert 'RoleAnimationStore::GetInstance().Show("thinking")' in application
    assert 'RoleAnimationStore::GetInstance().Show("speaking")' in application
    assert 'RoleAnimationStore::GetInstance().Show("faint")' in application
    assert 'is_final' in application


def test_five_action_package_fits_reported_limit_and_survives_restart():
    source = (ROOT / "main/device_content/role_animation_store.cc").read_text()
    assert 'action == "thinking"' in source
    assert 'kMaxTotalBytes = 5 * kMaxFileBytes' in source
    assert 'count > 5' in source
    assert 'cJSON_GetArraySize(actions) <= 5' in source


def test_role_switch_invalidates_old_action_package_without_touching_audio_state_machine():
    application = (ROOT / "main/application.cc").read_text()
    commit = application[application.index('strcmp(command->valuestring, "commitActiveRole")'):]
    assert 'RoleAnimationStore::GetInstance().ClearForRoleChange' in commit
    store = (ROOT / "main/device_content/role_animation_store.cc").read_text()
    assert 'RoleVisualStore::GetInstance().LoadActive()' in store


def test_role_switch_releases_invalidated_animation_files_for_the_next_set():
    store = (ROOT / "main/device_content/role_animation_store.cc").read_text()
    clear = store[store.index("void RoleAnimationStore::ClearForRoleChange"):]
    clear = clear.split("\n}", 1)[0]
    assert "opendir(\"/gallery\")" in clear
    assert 'name.rfind("ra_", 0) == 0' in clear
    assert 'name.size() > 7' in clear
    assert 'name.substr(name.size() - 4) == ".eaf"' in clear
    assert "role_id_.empty() || role_id_ == role_id" not in clear


def test_asset_download_keeps_the_current_screen_visible_until_commit():
    board = (ROOT / "main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc").read_text()
    prepare = board[board.index("void PrepareGalleryDownload() override {"):]
    prepare = prepare.split("\n    }", 1)[0]
    assert "StopGalleryAnimation()" not in prepare
    assert "StopRoleAnimation()" not in prepare
    assert "if (launcher_transition_image_ != nullptr)" in prepare
    assert "lv_anim_delete(launcher_transition_image_, SetPanelY)" in prepare
    assert "ReleaseLauncherTransition(launcher_state_.page() != YGSoulPage::kDesktop)" in prepare
