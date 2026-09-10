#!/usr/bin/env python3
"""Stage Hotspot Arcade's tracked web client and packs into the SD-card tree."""

from __future__ import annotations

import argparse
import filecmp
import gzip
import json
import shutil
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "applications_user" / "hotspot_arcade" / "assets"
DESTINATION = ROOT / "sdcard" / "apps_assets" / "hotspot_arcade"
WEB_LIMIT = 72 * 1024


def source_files(root: Path) -> list[Path]:
    return sorted(path for path in root.rglob("*") if path.is_file())


def validate(root: Path) -> None:
    web_dir = root / "web"
    manifest_path = web_dir / "manifest.json"
    if not manifest_path.is_file():
        raise FileNotFoundError(manifest_path)

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(manifest, list) or not manifest:
        raise ValueError(f"invalid web manifest: {manifest_path}")

    for item in manifest:
        filename = item.get("file")
        if not filename:
            raise ValueError(f"manifest entry has no file: {item!r}")
        asset = web_dir / filename
        if not asset.is_file():
            raise FileNotFoundError(asset)
        if asset.stat().st_size > WEB_LIMIT:
            raise ValueError(f"web asset exceeds {WEB_LIMIT} bytes: {asset}")
        if item.get("gzip"):
            with gzip.open(asset, "rb") as stream:
                payload = stream.read()
            if b"/ws" not in payload:
                raise ValueError(f"web client does not reference /ws: {asset}")

    required_pack_dirs = {
        "trivia",
        "wyr",
        "scramble",
        "draw",
        "spectrum",
        "kmk",
        "secrets",
        "fillblank",
        "spyfall",
    }
    actual = {path.name for path in (root / "packs").iterdir() if path.is_dir()}
    missing = sorted(required_pack_dirs - actual)
    if missing:
        raise ValueError(f"missing pack directories: {', '.join(missing)}")


def compare(source: Path, destination: Path) -> list[str]:
    mismatches: list[str] = []
    for path in source_files(source):
        relative = path.relative_to(source)
        staged = destination / relative
        if not staged.is_file() or not filecmp.cmp(path, staged, shallow=False):
            mismatches.append(relative.as_posix())
    return mismatches


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the staged copy without changing it",
    )
    args = parser.parse_args()

    validate(SOURCE)
    if not args.check:
        DESTINATION.mkdir(parents=True, exist_ok=True)
        shutil.copytree(SOURCE, DESTINATION, dirs_exist_ok=True)

    mismatches = compare(SOURCE, DESTINATION)
    if mismatches:
        print("Hotspot Arcade SD assets are missing or stale:")
        for relative in mismatches:
            print(f"  {relative}")
        return 1

    validate(DESTINATION)
    files = source_files(SOURCE)
    total = sum(path.stat().st_size for path in files)
    print(f"Hotspot Arcade assets ready: {len(files)} files, {total} bytes")
    print(DESTINATION)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
