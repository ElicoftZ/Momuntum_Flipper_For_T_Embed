"""Run the hardware-independent release checks from an MSVC developer shell.

Use the ESP-IDF Python interpreter. Any failed check stops the release gate.
"""
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
os.chdir(root)


def run(args):
    print('CHECK:', ' '.join(map(str, args)), flush=True)
    subprocess.run(args, check=True)


for batch in ('run_host_tests.bat', 'run_furi_string_psram_test.bat'):
    run([os.environ.get('COMSPEC', 'cmd.exe'), '/d', '/c', 'tests\\host\\' + batch])
run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_*.py', '-v'])
for script in (
    'run_nfc_source_tests.py', 'run_chameleon_tests.py', 'run_launcher_bridge_tests.py', 'run_multiboot_tests.py',
    'run_wifi_start_tests.py', 'run_file_sharing_tests.py', 'run_probe_worker_tests.py',
    'run_ble_serial_gap_tests.py', 'test_loader_deferred.py', 'wardriving_regression.py',
    'test_arm_fap_other.py',
):
    run([sys.executable, 'tests/host/' + script])

out = root / 'build_host/release_checks'
out.mkdir(parents=True, exist_ok=True)
for name, sources in (
    ('ble_detector', ['tests/host/test_ble_detector_parse.c',
                      'applications/main/ble_detector/ble_detector_parse.c']),
    ('probe_parse', ['tests/host/probe_parse_test.c']),
    ('whisper_pair', ['tests/host/whisper_pair_test.c']),
):
    exe = out / (name + '.exe')
    run(['cl', '/nologo', '/std:c11', *sources, '/Fo' + str(out) + '/', '/Fe' + str(exe)])
    run([str(exe)])
print('PASS: configured hardware-independent release checks completed', flush=True)
