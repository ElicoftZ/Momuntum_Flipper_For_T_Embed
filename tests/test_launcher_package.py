import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import json

spec = importlib.util.spec_from_file_location('package_launcher', Path(__file__).resolve().parents[1] / 'tools/package_launcher_release.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class LauncherPackageTests(unittest.TestCase):
    def image(self, version='2.4.0-rc1', chip=9):
        data = bytearray(512)
        data[0] = 0xE9
        struct.pack_into('<H', data, 12, chip)
        struct.pack_into('<I', data, 32, 0xABCD5432)
        data[48:48 + len(version)] = version.encode()
        return data

    def test_stages_matching_app_only_manifest(self):
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / 'app.bin'
            binary.write_bytes(self.image())
            target = Path(folder) / 'channel'
            module.package(binary, target)
            manifest = json.loads((target / 'launcher.json').read_text())
            self.assertEqual(manifest['version'], '2.4.0-rc1')
            self.assertEqual(manifest['size'], len(binary.read_bytes()))
            self.assertEqual(manifest['launcher_protocol'], 1)
            self.assertEqual(binary.read_bytes(), (target / manifest['file']).read_bytes())
            self.assertFalse((target / 'bootloader.bin').exists())

    def test_rejects_old_port_wrong_chip_and_merged_image(self):
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / 'app.bin'
            for data in (self.image('2.3.14'), self.image(chip=0), bytes(1024)):
                binary.write_bytes(data)
                with self.assertRaises(ValueError):
                    module.package(binary, Path(folder) / 'channel')


if __name__ == '__main__':
    unittest.main()
