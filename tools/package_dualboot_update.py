"""Package the multiboot update only when the real recovery hook is linked.

Run with ESP-IDF's Python after building build_multiboot. This deliberately
omits the partition table so an update can retain installed secondary apps.
"""
from pathlib import Path
import hashlib
import shutil
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build_multiboot"
OUTPUT = ROOT / "releases" / "dualboot-probe-sniff"


def require_function(path, name):
    with path.open("rb") as stream:
        symbols = ELFFile(stream).get_section_by_name(".symtab")
        matches = symbols.get_symbol_by_name(name) if symbols else None
        if not matches or not any(
            symbol["st_info"]["bind"] == "STB_GLOBAL"
            and symbol["st_info"]["type"] == "STT_FUNC"
            and symbol["st_size"] > 0
            and symbol["st_shndx"] != "SHN_UNDEF"
            for symbol in matches
        ):
            raise SystemExit(f"Missing real {name} in {path}; rebuild before packaging")


def main():
    config = (BUILD / "sdkconfig").read_text()
    if 'CONFIG_MOMENTUM_MULTIBOOT=y' not in config or (
        'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_multiboot_16mb.csv"' not in config
    ):
        raise SystemExit("Expected the T-Embed multiboot configuration")
    require_function(BUILD / "bootloader/bootloader.elf", "bootloader_after_init")
    for symbol in ("dualboot_app", "wlan_app_scene_probe_sniff_on_enter",
                   "wlan_app_scene_probe_sniff_on_event", "wlan_app_scene_probe_sniff_on_exit"):
        require_function(BUILD / "furi_esp32.elf", symbol)
    binaries = [("bootloader/bootloader.bin", "bootloader.bin", 0x8000),
                ("furi_esp32.bin", "momentum.bin", 0x400000)]
    for source, _, limit in binaries:
        if not 0 < (BUILD / source).stat().st_size <= limit:
            raise SystemExit(f"{source} does not fit its partition")
    OUTPUT.mkdir(parents=True, exist_ok=True)
    hashes = []
    for source, name, _ in binaries:
        shutil.copyfile(BUILD / source, OUTPUT / name)
        hashes.append(f"{hashlib.sha256((OUTPUT / name).read_bytes()).hexdigest()}  {name}")
    (OUTPUT / "SHA256SUMS.txt").write_text("\n".join(hashes) + "\n")
    shutil.copyfile(ROOT / "docs/dualboot-probe-sniff.md", OUTPUT / "UPDATE.md")
    print(f"Verified recovery hook and Probe Sniff; update packaged in {OUTPUT}")


if __name__ == "__main__":
    main()
