from pathlib import Path


def test_pairing_receipt_precedes_device_auth_v2():
    source = (Path(__file__).resolve().parents[1] / 'main/application.cc').read_text()
    activation = source.split('void Application::ActivationTask() {', 1)[1].split('\nvoid Application::CheckAssetsVersion()', 1)[0]
    assert activation.index('InitializeManagementClient()') < activation.index('ReportPairingReceipt()')
    assert activation.index('ReportPairingReceipt()') < activation.index('ydp.Connect(')


def test_factory_proof_precedes_management_connection():
    source = (Path(__file__).resolve().parents[1] / 'main/application.cc').read_text()
    activation = source.split('void Application::ActivationTask() {', 1)[1].split('\nvoid Application::CheckAssetsVersion()', 1)[0]
    assert activation.index('ProveFactory(') < activation.index('InitializeManagementClient()')
