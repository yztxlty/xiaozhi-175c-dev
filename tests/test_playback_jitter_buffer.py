import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_playback_queue_holds_xiaozhi_style_prefill():
    header = (ROOT / "main/audio/audio_service.h").read_text()
    assert "#define MAX_PLAYBACK_TASKS_IN_QUEUE 8" in header
    assert "#define OPUS_FRAME_DURATION_MS 60" in header
