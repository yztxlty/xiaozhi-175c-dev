import importlib.util
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "package_ygsoul_ota.py"


def load_module():
    spec = importlib.util.spec_from_file_location("package_ygsoul_ota", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FirmwarePackagingTests(unittest.TestCase):
    def test_patch_version_only_increments_the_third_segment(self):
        package = load_module()
        self.assertEqual(package.next_patch("1.0.6"), "1.0.7")
        self.assertEqual(package.next_patch("12.34.999"), "12.34.1000")
        with self.assertRaises(ValueError):
            package.next_patch("1.0")

    def test_uses_the_single_175c_build_and_release_directories(self):
        package = load_module()
        self.assertEqual(package.BUILD_DIR.name, "build-175c")
        self.assertEqual(package.RELEASE_DIR.name, "release")


if __name__ == "__main__":
    unittest.main()
