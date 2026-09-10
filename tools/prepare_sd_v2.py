"""Merge a v2 SD ZIP with this port's assets without modifying either input.

Original files, including replaced binaries, are retained in nested ZIPs.
The output has SD-root paths so it can be extracted onto an existing card.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import struct
import time
import zipfile

from elftools.elf.elffile import ELFFile
from check_fap_symbols import elf_gnu_hash, load_api_hashes


def safe_name(name):
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or "\\" in name or ":" in name:
        raise ValueError(f"Unsafe ZIP path: {name}")
    return str(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_zip", type=Path)
    parser.add_argument("output_zip", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    port = root / "sdcard"
    files, dirs, preserved = {}, set(), io.BytesIO()
    with zipfile.ZipFile(preserved, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(port.rglob("*")):
            name = path.relative_to(port).as_posix()
            if path.is_symlink():
                raise ValueError(f"Unexpected symlink: {path}")
            if path.is_dir():
                dirs.add(name)
                archive.writestr(name + "/", b"")
            else:
                files[name] = path.read_bytes()
                archive.writestr(name, files[name])
    original_port = dict(files)
    with zipfile.ZipFile(args.input_zip) as archive:
        if archive.testzip() is not None:
            raise ValueError("Input ZIP checksum failure")
        seen = set()
        for entry in archive.infolist():
            name = safe_name(entry.filename.rstrip("/"))
            if name.casefold() in seen:
                raise ValueError(f"Duplicate ZIP path: {name}")
            seen.add(name.casefold())
            if entry.is_dir():
                dirs.add(name)
            else:
                files[name] = archive.read(entry)

    # Preserve every key from both dictionaries, including port additions.
    dictionaries = []
    for name, old in original_port.items():
        if name.startswith("nfc/assets/") and "dict" in name and name in files:
            new = files[name]
            if old != new:
                lines = list(dict.fromkeys((new + b"\n" + old).decode("utf-8-sig").splitlines()))
                files[name] = ("\n".join(lines) + "\n").encode()
                dictionaries.append(name)

    hashes = load_api_hashes(str(root / "components/flipper_application/flipper_application/firmware_api.c"))
    archived_arm, native_apps, native_plugins = [], [], []
    for name, data in list(files.items()):
        if not name.endswith((".fap", ".fal")):
            continue
        elf = ELFFile(io.BytesIO(data))
        if elf["e_machine"] == "EM_ARM":
            archived_arm.append(name)
            replacement = original_port.get(name, b"")
            if replacement[:4] == b"\x7fELF" and struct.unpack_from("<H", replacement, 18)[0] == 94:
                files[name] = replacement
                elf = ELFFile(io.BytesIO(replacement))
            else:
                del files[name]
                continue
        if elf["e_machine"] != "EM_XTENSA":
            raise ValueError(f"Wrong native architecture: {name}")
        meta = elf.get_section_by_name(".fapmeta")
        if meta is None or struct.unpack_from("<HHH", meta.data(), 8) != (0, 1, 32):
            raise ValueError(f"Wrong port API or target: {name}")
        # JS uses statically linked modules on ESP32; its legacy SD plugins
        # use a private composite API, not the firmware table alone.
        if not name.startswith("apps_data/js_app/plugins/"):
            missing = [s.name for s in elf.get_section_by_name(".symtab").iter_symbols()
                       if s.name and s["st_shndx"] == "SHN_UNDEF"
                       and s["st_info"]["bind"] != "STB_WEAK" and elf_gnu_hash(s.name) not in hashes]
            if missing:
                raise ValueError(f"Missing firmware imports in {name}: {missing}")
        (native_apps if name.endswith(".fap") else native_plugins).append(name)

    # Check that selected animation manifests refer to complete animation sets.
    for name, data in files.items():
        if name.endswith("/manifest.txt") and b"Flipper Animation Manifest" in data:
            parent = str(PurePosixPath(name).parent)
            for animation in re.findall(rb"^Name:\s*(.+?)\r?$", data, re.M):
                meta_name = parent + "/" + animation.decode() + "/meta.txt"
                if meta_name not in files:
                    raise ValueError(f"Missing animation metadata: {meta_name}")
                frames = re.search(rb"^Frames order:\s*(.+)", files[meta_name], re.M)
                if frames:
                    for frame in set(frames.group(1).split()):
                        frame_name = str(PurePosixPath(meta_name).parent) + "/frame_" + str(int(frame)) + ".bm"
                        # The port accepts source PNGs when a .bm is absent.
                        if frame_name not in files and frame_name[:-3] + ".png" not in files:
                            raise ValueError(f"Missing animation frame: {frame_name}")

    files["version.txt"] = b"2.0.0\n"
    files["_preserved/sdcard-input.zip"] = args.input_zip.read_bytes()
    files["_preserved/port-sd-before-v2.zip"] = preserved.getvalue()
    files["README-v2.txt"] = (
        "T-Embed CC1101 / ESP32-S3 SD package for firmware 2.0.0\n"
        "Extract the contents to the SD root. Do not format your existing card.\n"
        "Keep your saved captures, credentials and settings if asked to replace them.\n"
        "Use firmware with the deferred Sub-GHz app launch fix.\n"
        "_preserved contains exact originals; do not extract those archives onto the card.\n"
        "ARM native plugins are preserved there; NFC protocol support and JS modules\n"
        "are built into this ESP32 firmware. Legacy native JS SD modules are retained.\n"
        "NFC dictionaries combine both inputs. All source folders are retained.\n"
        "This is a package repair, not a filesystem repair or a backup of your physical card.\n"
        "Binary checks passed; app behavior still requires testing on the device.\n"
    ).encode()
    report = {"input_sha256": hashlib.sha256(args.input_zip.read_bytes()).hexdigest(),
              "archived_arm_plugins": archived_arm, "native_apps": native_apps,
              "native_plugins": native_plugins, "merged_dictionaries": dictionaries,
              "hardware_tested": False}
    files["sd-package-report.json"] = json.dumps(report, indent=2).encode()
    files.pop("Manifest", None)
    files.pop("files.txt", None)
    for name in files:
        dirs.update(str(p) for p in PurePosixPath(name).parents if str(p) != ".")
    # Regenerate the stale supplied resource index for the merged payload.
    manifest = ["V:0", f"T:{int(time.time())}"] + ["D:" + d for d in sorted(dirs)]
    manifest += [f"F:{hashlib.md5(data).hexdigest()}:{len(data)}:{name}" for name, data in sorted(files.items())]
    files["Manifest"] = ("\n".join(manifest) + "\n").encode()
    files["files.txt"] = ("\n".join(f"{hashlib.sha256(data).hexdigest()} {len(data)} {name}"
                                    for name, data in sorted(files.items())) + "\n").encode()
    if len({n.casefold() for n in files}) != len(files):
        raise ValueError("Case-insensitive filename collision")
    args.output_zip.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output_zip, "x", zipfile.ZIP_DEFLATED) as output:
        for directory in sorted(dirs):
            output.writestr(directory + "/", b"")
        for name, data in sorted(files.items()):
            output.writestr(name, data)
    with zipfile.ZipFile(args.output_zip) as output:
        assert output.testzip() is None
        assert all(output.read(name) == data for name, data in files.items())
    print(f"Verified {len(files)} files and {len(dirs)} folders; {len(native_apps)} native apps, "
          f"{len(native_plugins)} native plugins; {len(archived_arm)} ARM files preserved in ZIP.")
    print(args.output_zip.resolve())


if __name__ == "__main__":
    main()
