from pathlib import Path


ROOT = Path(__file__).parents[1]


def test_gallery_hash_filenames_require_fat_long_filename_support():
    for name in ('sdkconfig', 'sdkconfig.defaults.esp32s3'):
        config = (ROOT / name).read_text()
        assert 'CONFIG_FATFS_LFN_HEAP=y' in config
        assert 'CONFIG_FATFS_LFN_NONE=y' not in config


def test_gallery_storage_is_independent_and_bounded():
    header = (ROOT / 'main/storage/content_storage.h').read_text()
    source = (ROOT / 'main/storage/content_storage.cc').read_text()
    store = (ROOT / 'main/device_content/gallery_store.cc').read_text()
    assert 'Category::Gallery' in header
    assert 'kGalleryQuotaBytes = 6 * 1024 * 1024' in header
    assert '"/gallery"' in source
    partition = (ROOT / 'partitions/v2/32m.csv').read_text()
    assert 'gallery,    data,   fat,        0x1A00000,    6M,' in partition
    assert 'kGalleryPartitionLabel = "gallery"' in source
    assert 'gallery_free_bytes' in header
    assert 'CanReserve(ContentStorage::Category::Gallery, size)' in store
    assert 'std::remove(temporary.c_str())' in store
    assert 'std::remove(path.c_str());\n        if (std::rename(temporary.c_str(), path.c_str()) != 0)' in store
    assert 'std::remove(path.c_str());\n    return std::rename(next.c_str(), path.c_str()) == 0;' in store
    assert 'return rollback()' in store
    assert 'substr(0, 12)' in store
    assert 'ItemFileName' in store
    assert 'superseded' in store
    assert 'std::string(resource->valuestring) + "_"' not in store
    assert '"manifest.jsn"' in store
    assert '"manifest.tmp"' in store
    assert '"manifest.json"' not in store
    assert '2 * 1024 * 1024' in store
    assert '12' in store and '3' in store
    for forbidden in ('role_a.jpg', 'role_b.jpg', 'ygsoul_boot', 'ygsoul_companion', '/content/music'):
        assert forbidden not in store


def test_gallery_apply_appends_without_implicit_deletion():
    store = (ROOT / 'main/device_content/gallery_store.cc').read_text()
    apply = store[store.index('bool GalleryStore::Apply'):store.index('bool GalleryStore::SaveManifest')]
    assert 'next = items_' in apply
    assert 'item.item_id == id->valuestring' in apply
    assert 'static_count' in apply and 'gif_count' in apply
    assert 'for (const auto& item : old)' not in apply
    assert 'ExistingFileMatches(*existing)' in apply


def test_gallery_deletion_reports_exact_item_for_app_and_device_initiated_paths():
    app = (ROOT / 'main/application.cc').read_text(encoding='utf-8')
    header = (ROOT / 'main/application.h').read_text(encoding='utf-8')
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text(encoding='utf-8')
    assert 'deletedGalleryItemId' in app
    assert 'ReportGalleryItemDeleted' in header
    assert 'ReportGalleryItemDeleted(item.item_id)' in board


def test_gallery_commands_and_ui_are_real_and_horizontal():
    app = (ROOT / 'main/application.cc').read_text()
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    assert 'applyGallery' in app
    assert 'deleteGalleryItem' in app
    assert 'GalleryStore::GetInstance()' in app
    assert 'LV_EVENT_LONG_PRESSED' in board
    assert 'DeleteGalleryItemCallback' in board
    assert 'SwitchGalleryHorizontal' in board
    assert '220' in board
    assert 'store.IntervalSec() * 1000' in board
    assert 'store.Loop()' in board
    assert 'jpeg_to_image(' in board
    assert 'LV_COLOR_FORMAT_RGB565' in board
    assert 'lv_eaf_create(launcher_content_)' in board
    assert 'lv_eaf_set_src(gallery_image_, gallery_asset_->image_dsc())' in board
    assert 'lv_eaf_is_loaded(gallery_image_)' in board
    assert 'std::make_unique<LvglGif>' not in board
    assert 'lv_image_set_scale(gallery_image_' in board
    gallery = board[board.index('void RenderGallery()'):board.index('void RenderSettings()')]
    for forbidden in ('ygsoul_boot_lvgl', 'ygsoul_companion_', 'ygsoul_mouth_frames_'):
        assert forbidden not in gallery


def test_gallery_switch_uses_same_layer_horizontal_push_transition():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    switch = board[board.index('void SwitchGalleryHorizontal'):board.index('void StopMusicProgress')]
    assert 'lv_snapshot_take_to_draw_buf(launcher_panel_' in switch
    assert 'lv_anim_set_values(&incoming, direction * DISPLAY_WIDTH, 0)' in switch
    assert 'lv_anim_set_values(&outgoing, 0, -direction * DISPLAY_WIDTH)' in switch
    assert 'SetPanelY' not in switch


def test_gallery_uses_espressif_eaf_player_for_embedded_playback():
    manifest = (ROOT / 'main/idf_component.yml').read_text()
    assert 'espressif/esp_lv_eaf_player: ==0.1.0' in manifest
    assert 'espressif/esp_new_jpeg: ^0.6.1' in manifest
    store = (ROOT / 'main/device_content/gallery_store.cc').read_text()
    assert 'kind != "eaf"' in store
    assert 'memcmp(head + 1, "EAF", 3)' in store


def test_music_network_client_does_not_start_before_network_stack():
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    setup = board[board.index('virtual void SetupUI() override'):board.index('virtual void SetChatMessage')]
    assert 'StartMusicClient();' not in setup


def test_gallery_download_temporarily_disables_wifi_power_save():
    app = (ROOT / 'main/application.cc').read_text()
    handler = app[app.index('} else if (strcmp(command->valuestring, "applyGallery") == 0)'):
                  app.index('} else if (strcmp(command->valuestring, "deleteGalleryItem") == 0)')]
    assert 'SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE)' in handler
    assert 'SetPowerSaveLevel(PowerSaveLevel::LOW_POWER)' in handler
    assert 'PrepareGalleryDownload()' in handler
    assert 'RefreshGallery()' in handler


def test_gallery_reports_exact_device_inventory_after_every_mutation():
    app = (ROOT / 'main/application.cc').read_text()
    handler = app[app.index('if (succeeded && (strcmp(command->valuestring, "applyGallery") == 0 ||'):
                  app.index('if (!deleted_gallery_item_id.empty())')]
    assert 'gallery.Items()' in handler
    assert 'galleryItems' in handler
    assert 'itemId' in handler
    assert 'sha256' in handler
