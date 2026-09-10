#!/usr/bin/env python3
"""Static regression checks for the international-keyboard HID descriptor port."""

from __future__ import annotations

import hashlib
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JA_LAYOUT = ROOT / "applications/main/bad_usb/resources/badusb/assets/layouts/ja-JP.kl"
USB_HID = ROOT / "components/furi_hal/furi_hal_usb_hid_tinyusb.c"
BLE_HID = ROOT / "components/ble_hid/ble_hid.c"
DEPENDENCIES_LOCK = ROOT / "dependencies.lock"
EXPECTED_JA_LAYOUT_SHA256 = "63b4380c87eba737016c0b74ed55b2bfeb479bd010cdf43d53d8f2198c551985"


def _array_body(source: str, name: str) -> str:
    match = re.search(
        rf"static\s+const\s+uint8_t\s+{re.escape(name)}\[\]\s*=\s*\{{(.*?)\n\}};",
        source,
        re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"could not locate uint8_t array {name}")
    return match.group(1)


def _has_byte_sequence(source: str, values: tuple[int, ...]) -> bool:
    tokens = [int(token, 0) for token in re.findall(r"\b(?:0[xX][0-9a-fA-F]+|[0-9]+)\b", source)]
    width = len(values)
    return any(tuple(tokens[index : index + width]) == values for index in range(len(tokens) - width + 1))


class InternationalKeyboardDescriptorTest(unittest.TestCase):
    def test_ja_jp_layout_matches_official_upstream_asset(self) -> None:
        data = JA_LAYOUT.read_bytes()
        self.assertEqual(len(data), 256)
        self.assertEqual(hashlib.sha256(data).hexdigest(), EXPECTED_JA_LAYOUT_SHA256)

    def test_usb_uses_locked_tinyusb_international_keyboard_descriptor(self) -> None:
        keyboard = _array_body(USB_HID.read_text(encoding="utf-8"), "hid_report_descriptor")
        self.assertIn("TUD_HID_REPORT_DESC_KEYBOARD", keyboard)

        # TinyUSB 0.21's standard keyboard macro encodes both maxima as
        # 16-bit 255 (HID_*_MAX_N(255, 2)); pinning catches a dependency
        # downgrade that could silently narrow this descriptor.
        lock = DEPENDENCIES_LOCK.read_text(encoding="utf-8")
        tinyusb = re.search(
            r"(?ms)^  espressif/tinyusb:\n.*?^    version: ([^\n]+)$",
            lock,
        )
        if tinyusb is None:
            self.fail("locked espressif/tinyusb dependency is missing")
        self.assertEqual(tinyusb.group(1).strip(" '\""), "0.21.0~1")

    def test_ble_keyboard_descriptor_accepts_full_usage_byte_range(self) -> None:
        descriptor = _array_body(BLE_HID.read_text(encoding="utf-8"), "ble_hid_report_map")
        self.assertTrue(_has_byte_sequence(descriptor, (0x26, 0xFF, 0x00)))
        self.assertTrue(_has_byte_sequence(descriptor, (0x2A, 0xFF, 0x00)))
        self.assertFalse(_has_byte_sequence(descriptor, (0x25, 0x65)))
        self.assertFalse(_has_byte_sequence(descriptor, (0x29, 0x65)))


if __name__ == "__main__":
    unittest.main()
