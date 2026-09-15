from pathlib import Path


ROOT = Path(__file__).parents[1]


def test_watch_face_store_is_single_jpeg_and_atomic():
    header = (ROOT / 'main/watch/watch_face_store.h').read_text()
    source = (ROOT / 'main/watch/watch_face_store.cc').read_text()
    assert 'bool Apply(const cJSON* params)' in header
    assert 'CurrentPath()' in header
    assert 'cJSON_GetArraySize(files) != 1' in source
    assert 'strcmp(format->valuestring, "jpg")' in source
    assert 'width->valueint != 466' in source
    assert 'height->valueint != 466' in source
    assert 'std::rename' in source
    assert 'watch_face_a.jpg' in source and 'watch_face_b.jpg' in source


def test_watch_face_command_and_commercial_renderer_are_connected():
    app = (ROOT / 'main/application.cc').read_text()
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()
    cmake = (ROOT / 'main/CMakeLists.txt').read_text()
    assert 'applyWatchFace' in app
    assert 'WatchFaceStore::GetInstance().Apply' in app
    assert 'watchFaceResourceId' in app
    assert 'watch/watch_face_store.cc' in cmake
    assert 'LoadWatchFaceAsset' in board
    assert 'lv_line_create' in board
    assert 'WatchFaceStore::GetInstance().CurrentPath()' in board
    assert 'default_watch_face_start' in board
    assert 'DecodeWatchFaceAsset(default_watch_face_start' in board
    assert 'default_watch_face.jpg' in cmake
