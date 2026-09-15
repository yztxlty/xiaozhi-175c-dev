from pathlib import Path


ROOT = Path(__file__).parents[1]


def test_existing_ai_audio_output_path_is_not_wrapped_by_music():
    codec = (ROOT / 'main/audio/audio_codec.cc').read_text()
    player = (ROOT / 'main/music/music_player.cc').read_text()
    output = codec[codec.index('void AudioCodec::OutputData'):codec.index('bool AudioCodec::InputData')]
    assert 'output_gate_' not in output
    assert 'Write(data.data(), data.size());' in output
    assert 'codec->OutputData(pcm);' in player


def test_ai_state_preempts_music_without_blocking_voice_output():
    player = (ROOT / 'main/music/music_player.cc').read_text()
    client = (ROOT / 'main/music/music_client.cc').read_text()
    board = (ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc').read_text()

    assert 'GetDeviceState() != kDeviceStateIdle' in player
    assert 'void MusicClient::SetBusy(bool busy)' in client
    assert 'music_client_->SetBusy(state != kDeviceStateIdle)' in board


def test_music_stream_chunks_use_psram_to_preserve_ui_heap():
    player = (ROOT / 'main/music/music_player.cc').read_text()
    assert 'heap_caps_malloc(sizeof(MusicChunk), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)' in player
    assert 'heap_caps_free(chunk)' in player
    assert 'new (std::nothrow) MusicChunk' not in player


def test_initial_playlist_snapshot_is_acknowledged():
    client = (ROOT / 'main/music/music_client.cc').read_text()
    assert 'if (cJSON_IsNumber(revision)) {' in client
    assert 'if (replacement && cJSON_IsNumber(revision)) {' not in client


def test_music_prefills_decoded_audio_time_in_native_psram_buffer():
    player = (ROOT / 'main/music/music_player.cc').read_text()
    assert 'xRingbufferCreateWithCaps' in player
    assert 'kPrefillMs = 1500' in player
    assert 'self->buffered_samples_.load()' in player
    assert 'self->queued_end_epoch_.load() != epoch' in player


def test_music_output_is_revoked_before_ai_takes_the_codec():
    header = (ROOT / 'main/music/music_player.h').read_text()
    player = (ROOT / 'main/music/music_player.cc').read_text()
    assert 'std::mutex output_mutex_' in header
    assert 'const auto epoch = active_epoch_.load();' in player
    assert 'std::lock_guard<std::mutex> output_lock(output_mutex_);' in player
    assert 'epoch != active_epoch_.load()' in player
