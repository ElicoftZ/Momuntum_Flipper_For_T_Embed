#!/usr/bin/env python3
"""Static regression checks for persistent BadUSB settings on the ESP32 port."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "applications/main/bad_usb/bad_usb_app.c"


class BadUsbSettingsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text(encoding="utf-8")

    def test_settings_file_contract_matches_official_firmware(self):
        self.assertIn('BAD_USB_APP_BASE_FOLDER "/.badusb.settings"', self.source)
        self.assertIn('"Flipper BadUSB Settings File"', self.source)
        self.assertIn("BAD_USB_SETTINGS_VERSION        1", self.source)

    def test_layout_and_interface_are_loaded_and_saved(self):
        for fragment in (
            'flipper_format_read_string(settings, "layout", value)',
            'flipper_format_read_uint32(settings, "interface", &interface, 1)',
            'flipper_format_write_string(settings, "layout", app->keyboard_layout)',
            'flipper_format_write_uint32(settings, "interface", &interface, 1)',
        ):
            self.assertIn(fragment, self.source)

    def test_invalid_layout_preserves_a_valid_interface(self):
        restore_layout = self.source.index("furi_string_set(app->keyboard_layout, value)")
        restore_interface = self.source.index("app->interface = interface", restore_layout)
        mark_loaded = self.source.index("loaded = true", restore_interface)
        validate_layout = self.source.index("storage_common_stat(", mark_loaded)
        fallback_layout = self.source.index(
            "furi_string_set(app->keyboard_layout, BAD_USB_SETTINGS_DEFAULT_LAYOUT)",
            validate_layout,
        )
        self.assertLess(restore_layout, restore_interface)
        self.assertLess(restore_interface, mark_loaded)
        self.assertLess(mark_loaded, validate_layout)
        self.assertLess(validate_layout, fallback_layout)

    def test_invalid_or_missing_settings_fall_back_safely(self):
        self.assertIn("layout_info.size != 256", self.source)
        self.assertIn("BAD_USB_SETTINGS_DEFAULT_LAYOUT", self.source)
        self.assertIn("if(!loaded)", self.source)


if __name__ == "__main__":
    unittest.main()
