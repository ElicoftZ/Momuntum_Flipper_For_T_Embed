"""Stage an app-only OTA channel for the companion Launcher. Does not publish."""
import argparse
import json
from pathlib import Path
import shutil
import struct


def package(binary, output):
    data = binary.read_bytes()
    if len(data) < 288 or data[0] != 0xE9 or struct.unpack_from('<H', data, 12)[0] != 9:
        raise ValueError('Expected an app-only ESP32-S3 image')
    if struct.unpack_from('<I', data, 32)[0] != 0xABCD5432:
        raise ValueError('Missing ESP-IDF application descriptor')
    version = data[48:80].split(b'\0', 1)[0].decode('ascii')
    numbers = version.split('-', 1)[0].split('.')
    if len(numbers) != 3 or tuple(map(int, numbers)) < (2, 4, 0):
        raise ValueError('Launcher integration requires firmware 2.4.0 or newer')
    if len(data) > 0xE00000:
        raise ValueError('Image exceeds companion Launcher install space')
    output.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(binary, output / 'furi_esp32.bin')
    manifest = dict(schema=1, board='lilygo_t_embed_cc1101', launcher_protocol=1,
                    version=version, file='furi_esp32.bin', size=len(data))
    (output / 'launcher.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (output / 'version.txt').write_text(version + '\n', encoding='utf-8')
    print(f'Staged Launcher OTA {version}: {len(data)} bytes in {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    package(args.binary, args.output)
