from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Esp32YdpChallengeContractTests(unittest.TestCase):
    def test_activation_uses_server_challenge_and_efuse_hmac(self):
        source = (ROOT / 'main/protocols/ydp_bootstrap.cc').read_text()
        client = (ROOT / 'main/protocols/ydp_client.cc').read_text()
        helper = (ROOT / 'main/protocols/ydp_device_auth.h').read_text()

        self.assertIn('/ydp/v1/activation/challenge', source)
        self.assertIn('/ydp/v1/credentials/refresh', source)
        self.assertIn('esp_hmac_calculate(HMAC_KEY0', source)
        self.assertIn('YGSoul-Device-Auth-v2', helper)
        self.assertIn('"authVersion"', source)
        self.assertIn('"keyVersion"', source)
        self.assertIn('"firmwareVersion"', source)
        self.assertIn('"deviceNonce"', source)
        self.assertIn('"purpose"', source)
        self.assertIn('"transport"', source)
        self.assertIn('"timestamp"', source)
        self.assertIn('"challenge"', source)
        self.assertNotIn('"occurredAt"', source)
        self.assertNotIn('"sessionToken"', source)
        self.assertIn('YdpBootstrap::GetInstance().Activate', client)
        self.assertIn('YdpBootstrap::GetInstance().Refresh', client)


if __name__ == '__main__':
    unittest.main()
