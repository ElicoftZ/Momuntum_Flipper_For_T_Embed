"""Already-patched input/output archives must not be rewritten during configure."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'wifi_patch', Path(__file__).resolve().parents[1] / 'tools/patch_wifi_lib.py')
wifi_patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wifi_patch)


class WifiLibPatchTests(unittest.TestCase):
    def test_in_place_rerun_leaves_archive_untouched(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / 'libnet80211.a'
            archive.write_bytes(b'archive')
            before = archive.stat().st_mtime_ns

            def extract(args, cwd):
                Path(cwd, 'ieee80211_output.o').write_bytes(
                    wifi_patch.PATCH_ENTRY + wifi_patch.PATCH_BODY)

            with patch('sys.argv', ['patch', str(archive), 'objcopy', 'ar', str(archive)]), \
                 patch.object(wifi_patch.subprocess, 'check_call', side_effect=extract), \
                 patch.object(wifi_patch, 'find_section_offset', return_value=(0, 7)), \
                 patch.object(wifi_patch, 'find_func_offset_in_section', return_value=0), \
                 patch.object(wifi_patch.shutil, 'copy2', wraps=wifi_patch.shutil.copy2) as copy:
                wifi_patch.main()
                self.assertEqual(copy.call_count, 1)  # Only the temporary working copy.
            self.assertEqual(archive.read_bytes(), b'archive')
            self.assertEqual(archive.stat().st_mtime_ns, before)
