from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Esp32YdpChallengeContractTests(unittest.TestCase):
    def test_activation_uses_server_challenge_and_efuse_hmac(self):
        source = (ROOT / 'main/protocols/ydp_bootstrap.cc').read_text()
        client = (ROOT / 'main/protocols/ydp_client.cc').read_text()

        self.assertIn('/ydp/v1/activation/challenge', source)
        self.assertIn('esp_hmac_calculate(HMAC_KEY0', source)
        self.assertIn('"firmwareVersion"', source)
        self.assertIn('"deviceNonce"', source)
        self.assertIn('"timestamp"', source)
        self.assertIn('"challenge"', source)
        self.assertNotIn('"occurredAt"', source)
        self.assertNotIn('"sessionToken"', source)
        self.assertIn('YdpBootstrap::GetInstance().Activate', client)


if __name__ == '__main__':
    unittest.main()
