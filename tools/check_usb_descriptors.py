"""Parse the USB config descriptors straight out of the built ELF.

Guards the two things that are easy to get silently wrong when hand-adding an
interface: the wDescriptorLength patch offset, and the endpoint/interface
numbering. A wrong offset here means the host asks for a report descriptor of
the wrong length and enumeration fails on hardware, with no build-time signal.
"""

import re
import subprocess
import sys

OBJDUMP = r"C:/Espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump.exe"
NM = r"C:/Espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm.exe"
ELF = sys.argv[1] if len(sys.argv) > 1 else "build_t_embed/furi_esp32.elf"

syms = {}
for line in subprocess.run([NM, "-S", ELF], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 4:
        syms[p[3]] = (int(p[0], 16), int(p[1], 16))


def read(name):
    addr, size = syms[name]
    out = subprocess.run(
        [OBJDUMP, "-s", "--start-address", hex(addr), "--stop-address", hex(addr + size), ELF],
        capture_output=True,
        text=True,
    ).stdout
    data = bytearray()
    for line in out.splitlines():
        m = re.match(r"^\s*([0-9a-f]{4,16})\s((?:[0-9a-f]{2,8}\s){1,4})", line)
        if m:
            data += bytes.fromhex(m.group(2).replace(" ", ""))
    return bytes(data[:size])


def walk(blob, label, expect_u2f_wdesc_offset):
    print("\n== %s (%d bytes) ==" % (label, len(blob)))
    i = 0
    u2f_itf_start = None
    in_u2f_itf = False
    u2f_eps = []
    itf_count = 0
    eps = []
    while i < len(blob):
        blen, btype = blob[i], blob[i + 1]
        if btype == 0x02:
            print("  CONFIG   bNumInterfaces=%d wTotalLength=%d" % (blob[i + 4], blob[i + 2] | (blob[i + 3] << 8)))
            assert (blob[i + 2] | (blob[i + 3] << 8)) == len(blob), "wTotalLength != actual size"
            declared_itf = blob[i + 4]
        elif btype == 0x04:
            itf_count += 1
            cls = blob[i + 5]
            print("  ITF %d    class=0x%02x numEP=%d" % (blob[i + 2], cls, blob[i + 4]))
            if cls == 0x03 and blob[i + 4] == 2:
                u2f_itf_start = i
                in_u2f_itf = True
            else:
                in_u2f_itf = False
        elif btype == 0x21:
            wdesc = blob[i + 7] | (blob[i + 8] << 8)
            print("    HID    wDescriptorLength=%d (at offset %d)" % (wdesc, i + 7))
            if u2f_itf_start is not None and i == u2f_itf_start + 9:
                assert i + 7 == expect_u2f_wdesc_offset, (
                    "U2F wDescriptorLength is at %d, code patches %d" % (i + 7, expect_u2f_wdesc_offset)
                )
                print("    ^ U2F wDescriptorLength offset matches the patch site")
        elif btype == 0x05:
            eps.append(blob[i + 2])
            if in_u2f_itf:
                u2f_eps.append(blob[i + 2])
            print("    EP     0x%02x type=%d size=%d interval=%d" % (blob[i + 2], blob[i + 3] & 3, blob[i + 4] | (blob[i + 5] << 8), blob[i + 6]))
        i += blen
    assert declared_itf == itf_count, "bNumInterfaces %d != %d actual" % (declared_itf, itf_count)
    ins = [e for e in eps if e & 0x80]
    # dwc2_esp32.h says ep_in_count = 5, but dcd_dwc2.c allocates EP0 IN through the
    # same counter (dfifo_device_init -> dfifo_alloc(0x80)), so a config descriptor may
    # declare at most FOUR IN endpoints. Asking for five makes dcd_edpt_open() fail and
    # the device never enumerates -- silent until you plug it into a host.
    print("  -> %d interfaces, %d IN endpoints (max 4, EP0 takes the fifth), %d OUT" % (
        itf_count, len(ins), len(eps) - len(ins)))
    assert len(ins) <= 4, (
        "too many IN endpoints: %d declared + EP0 exceeds the ESP32-S3 budget of 5" % len(ins))
    # The NUMBER matters too, independently of the count. dfifo_alloc() writes the
    # TX FIFO as dieptxf[epnum - 1] and the S3 implements only four of those, so an
    # IN endpoint numbered 5+ opens without error but has no FIFO: transfers stall
    # and the endpoint is busy forever. Nothing in TinyUSB bounds-checks this.
    for e in ins:
        assert (e & 0x0F) <= 4, (
            "IN endpoint 0x%02x is number %d; the ESP32-S3 only has TX FIFOs for 1..4"
            % (e, e & 0x0F))
    for e in eps:
        assert (e & 0x0F) <= 6, "endpoint number %d is beyond the S3's ep_count of 7" % (e & 0x0F)
    assert len(set(eps)) == len(eps), "duplicate endpoint address"
    if expect_u2f_wdesc_offset is not None:
        # Check the shape, not specific numbers: the U2F interface must be a HID
        # interface carrying one IN and one OUT endpoint. Hardcoding addresses here
        # only re-breaks this file every time the layout legitimately moves.
        assert u2f_itf_start is not None, "no HID interface with an IN/OUT pair (U2F)"
        assert len(u2f_eps) == 2 and any(e & 0x80 for e in u2f_eps) and any(
            not (e & 0x80) for e in u2f_eps), (
            "U2F interface needs one IN and one OUT endpoint, got %s"
            % [hex(e) for e in u2f_eps])
        print("    U2F endpoints: %s" % " ".join(hex(e) for e in u2f_eps))
    return True


TUD_CONFIG, TUD_HID, TUD_CDC, TUD_MSC = 9, 25, 66, 23
walk(read("hid_configuration_descriptor"), "HID-only descriptor", TUD_CONFIG + TUD_HID + 9 + 7)
walk(read("s_composite_config_desc"), "Composite descriptor (storage layout)", None)
walk(read("s_composite_u2f_config_desc"), "Composite descriptor (U2F layout)",
     TUD_CONFIG + TUD_HID + TUD_CDC + 9 + 7)

rd = read("hid_u2f_report_descriptor")
print("\n== FIDO report descriptor (%d bytes) ==" % len(rd))
assert rd[:5] == bytes([0x06, 0xD0, 0xF1, 0x09, 0x01]), "not a FIDO U2FHID usage page/usage"
print("  usage page 0xF1D0, usage 0x01 (U2FHID) - correct")
print("\nAll descriptor checks passed.")
