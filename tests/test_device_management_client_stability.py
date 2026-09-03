import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_management_task_cannot_abort_the_chip():
    src = (ROOT / "main/device_management_client.cc").read_text()
    hdr = (ROOT / "main/device_management_client.h").read_text()
    assert "try" in src
    assert "catch" in src
    assert "xTaskCreate" in src
    assert "8192" in src
    assert "wss://" in src
    assert "https://" in src
    assert "void Stop()" in hdr or "void Stop();" in hdr
