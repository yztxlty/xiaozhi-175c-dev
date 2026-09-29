from pathlib import Path


def test_device_auth_v2_precedes_management_connection_and_pairing_receipt():
    source = (Path(__file__).resolve().parents[1] / 'main/application.cc').read_text()
    activation = source.split('void Application::ActivationTask() {', 1)[1].split('\nvoid Application::CheckAssetsVersion()', 1)[0]
    assert activation.index('ydp.Connect(') < activation.index('InitializeManagementClient()')
    assert activation.index('ydp.Connect(') < activation.index('ReportPairingReceipt()')
