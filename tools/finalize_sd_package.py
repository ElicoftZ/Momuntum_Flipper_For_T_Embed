#!/usr/bin/env python3
"""Re-validate an SD package against the CURRENT firmware and emit a ready card ZIP.

prepare_sd_v2.py merges a v2 card with this port's assets; this runs afterwards (or on
its own output) to catch drift: every .fap/.fal is re-checked against today's
firmware_api.c, the folders apps expect on a fresh card are created, editor junk is
dropped, and Manifest/files.txt are regenerated for the final payload.
"""
import argparse
import hashlib
import io
import json
import struct
import time
import zipfile
from pathlib import Path, PurePosixPath

from elftools.elf.elffile import ELFFile
from check_fap_symbols import elf_gnu_hash, load_api_hashes

# Folders the firmware opens or writes on the SD card. Most apps mkdir on demand, but a
# fresh card should already show them so the user knows where files belong -- above all
# firmware/, which is DUALBOOT_FW_DIR: where Dual Boot looks for installable images.
REQUIRED_DIRS = (
    "apps/Media",
    "apps_data/findmy",
    "apps_data/hid_ble",
    "authenticator",
    "backup",
    "backup/nvs",
    "badbt",
    "firmware",
    "ibutton",
    "macro",
    "wardriving",
)

FIRMWARE_README = (
    "Drop firmware images (.bin) for the Dual Boot app in this folder.\n"
    "Dual Boot lists every valid .bin here and installs it into free flash.\n"
    "Invalid, truncated or wrong-device files are ignored, not installed.\n"
)


def validate_native(files, api_file):
    """Reject any app that would crash on this firmware, before it reaches a card."""
    hashes = load_api_hashes(str(api_file))
    apps, plugins, problems = [], [], []
    for name, data in sorted(files.items()):
        if not name.endswith((".fap", ".fal")):
            continue
        elf = ELFFile(io.BytesIO(data))
        if elf["e_machine"] != "EM_XTENSA":
            problems.append(f"{name}: not an ESP32 binary ({elf['e_machine']})")
            continue
        meta = elf.get_section_by_name(".fapmeta")
        if meta is None or struct.unpack_from("<HHH", meta.data(), 8) != (0, 1, 32):
            problems.append(f"{name}: wrong port API or target")
            continue
        # JS SD plugins resolve against a private composite API, not the table alone.
        if not name.startswith("apps_data/js_app/plugins/"):
            missing = [s.name for s in elf.get_section_by_name(".symtab").iter_symbols()
                       if s.name and s["st_shndx"] == "SHN_UNDEF"
                       and s["st_info"]["bind"] != "STB_WEAK"
                       and elf_gnu_hash(s.name) not in hashes]
            if missing:
                problems.append(f"{name}: missing firmware imports {missing}")
                continue
        (apps if name.endswith(".fap") else plugins).append(name)
    return apps, plugins, problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_zip", type=Path)
    parser.add_argument("output_zip", type=Path)
    parser.add_argument("--version", default="2.0.0")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]

    files, dirs = {}, set()
    with zipfile.ZipFile(args.input_zip) as archive:
        if archive.testzip() is not None:
            raise ValueError("Input ZIP checksum failure")
        for entry in archive.infolist():
            name = entry.filename.rstrip("/")
            if PurePosixPath(name).is_absolute() or ".." in PurePosixPath(name).parts:
                raise ValueError(f"Unsafe ZIP path: {entry.filename}")
            if entry.is_dir():
                dirs.add(name)
            else:
                files[name] = archive.read(entry)

    dropped = [n for n in files if PurePosixPath(n).name in (".DS_Store", "Thumbs.db")]
    for name in dropped:
        del files[name]
    files.pop("sd-package-report.json", None)

    apps, plugins, problems = validate_native(files, root /
        "components/flipper_application/flipper_application/firmware_api.c")
    if problems:
        raise SystemExit("Apps that would crash on this firmware:\n  " + "\n  ".join(problems))

    dirs.update(REQUIRED_DIRS)
    files.setdefault("firmware/README.txt", FIRMWARE_README.encode())
    files["version.txt"] = args.version.encode() + b"\n"

    for name in files:
        dirs.update(str(p) for p in PurePosixPath(name).parents if str(p) != ".")
    files.pop("Manifest", None)
    files.pop("files.txt", None)
    manifest = ["V:0", f"T:{int(time.time())}"] + ["D:" + d for d in sorted(dirs)]
    manifest += [f"F:{hashlib.md5(d).hexdigest()}:{len(d)}:{n}" for n, d in sorted(files.items())]
    files["Manifest"] = ("\n".join(manifest) + "\n").encode()
    files["files.txt"] = ("\n".join(f"{hashlib.sha256(d).hexdigest()} {len(d)} {n}"
                                    for n, d in sorted(files.items())) + "\n").encode()
    if len({n.casefold() for n in files}) != len(files):
        raise ValueError("Case-insensitive filename collision")

    args.output_zip.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output_zip, "w", zipfile.ZIP_DEFLATED) as output:
        for directory in sorted(dirs):
            output.writestr(directory + "/", b"")
        for name, data in sorted(files.items()):
            output.writestr(name, data)
    with zipfile.ZipFile(args.output_zip) as output:
        assert output.testzip() is None
        assert all(output.read(name) == data for name, data in files.items())

    print(json.dumps({"version": args.version, "files": len(files), "folders": len(dirs),
                      "native_apps": len(apps), "native_plugins": len(plugins),
                      "dropped_junk": dropped,
                      "sha256": hashlib.sha256(args.output_zip.read_bytes()).hexdigest()}, indent=2))
    print(args.output_zip.resolve())


if __name__ == "__main__":
    main()
