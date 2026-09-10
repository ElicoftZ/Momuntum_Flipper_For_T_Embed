"""Run from an MSVC developer shell with ESP-IDF's Python environment."""
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
os.chdir(root)
out = root / 'build_host' / 'multiboot'
stubs = out / 'stubs'
stubs.mkdir(parents=True, exist_ok=True)
(stubs / 'sdkconfig.h').write_text('#define CONFIG_MOMENTUM_MULTIBOOT 1\n#define __attribute__(x)\n')
(stubs / 'esp_log.h').write_text('#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGI(...) ((void)0)\n')
(stubs / 'bootloader_flash_priv.h').write_text('''#include <stddef.h>
#include <stdbool.h>
#define ESP_OK 0
int bootloader_flash_read(size_t, void*, size_t, bool);
int bootloader_flash_write(size_t, const void*, size_t, bool);
int bootloader_flash_erase_sector(size_t);
''')
(stubs / 'esp_flash_partitions.h').write_text('#include <stdbool.h>\nint esp_partition_table_verify(const void*, bool, int*);\n')
for name, sources in [
    ('layout', ['tests/host/multiboot_layout_test.c']),
    ('recovery', ['tests/host/multiboot_recovery_test.c', 'bootloader_components/multiboot_recovery/recovery.c']),
]:
    exe = out / (name + '_test.exe')
    subprocess.run(['cl', '/nologo', '/std:c11', '/W4', '/WX', '/Icomponents/multiboot',
                    '/I' + str(stubs), *sources, 'components/multiboot/layout.c',
                    '/Fo' + str(out) + '/', '/Fe' + str(exe)], check=True)
    subprocess.run([str(exe), str(out / 'initial.bin')], check=True)
