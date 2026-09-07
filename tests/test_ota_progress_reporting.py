import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_ota_download_progress_is_throttled_and_queued_asynchronously():
    application = (ROOT / "main/application.cc").read_text()
    callback_start = application.index("context->ota->StartUpgrade(")
    callback_end = application.index("context->done.store", callback_start)
    callback = application[callback_start:callback_end]

    assert 'ota_->ReportProgress("RUNNING"' not in callback
    assert "progress / 10 * 10" in callback
    assert 'xTaskCreate(' in application
    assert '"ota_download"' in application
    assert "xQueueCreate(1, sizeof(int))" in application
    assert "xQueueOverwrite" in callback


def test_ota_async_setup_failure_uses_the_existing_recovery_path():
    application = (ROOT / "main/application.cc").read_text()
    setup_start = application.index("struct UpgradeContext")
    recovery_start = application.index("if (!upgrade_success)", setup_start)
    setup = application[setup_start:recovery_start]

    assert "return false;" not in setup


def test_ota_upgrade_is_confirmed_only_after_booting_the_target_version():
    application = (ROOT / "main/application.cc").read_text()
    ota = (ROOT / "main/ota.cc").read_text()

    assert "ota_->PersistPendingUpgrade();" in application
    assert "ota_->ConfirmPendingUpgrade();" in application
    assert "version != current_version_" in ota
    assert 'ReportProgress("SUCCEEDED", 100' in ota


if __name__ == "__main__":
    test_ota_download_progress_is_throttled_and_queued_asynchronously()
    test_ota_async_setup_failure_uses_the_existing_recovery_path()
    test_ota_upgrade_is_confirmed_only_after_booting_the_target_version()
    print("PASS: OTA progress reporting")
