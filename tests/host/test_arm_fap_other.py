"""Compile selected *actual runtime cases*, then test guest/native boundaries.

The normal Unicorn harness bypasses arm_fap_runtime.c. This harness extracts
its libc, bit-field and BitBuffer cases verbatim and links the real VM and
native BitBuffer implementation. Only the OS check header is substituted.
GUI/hardware execution still requires a board. Requires MSVC Build Tools.
"""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build_host/other_bridge'
OUT.mkdir(parents=True, exist_ok=True)
runtime = (ROOT / 'components/flipper_application/arm_fap/arm_fap_runtime.c').read_text()


def section(start, end):
    return runtime.split(start, 1)[1].split(end, 1)[0]


cases = '    case ArmImport_abort:' + section('    case ArmImport_abort:', '    case ArmImport_strint_to_uint32:')
cases += '    case ArmImport_bit_lib_get_bits:' + section('    case ArmImport_bit_lib_get_bits:', '    case ArmImport_popup_alloc:')
cases += '    case ArmImport_bit_buffer_alloc:' + section('    case ArmImport_bit_buffer_alloc:', '    case ArmImport_file_stream_alloc:')
span = 'static bool guest_string_span(' + section('static bool guest_string_span(', 'static bool import_call(')
resolver = 'static ArmBitBuffer* get_bit_buffer(' + section('static ArmBitBuffer* get_bit_buffer(', 'static ArmDirWalk* get_dir_walk(')
(OUT / 'furi.h').write_text('''#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define furi_check(x) assert(x)
#define FURI_BIT(x, n) (((x) >> (n)) & 1)
''')
source = r'''
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include "arm_fap_vm.c"
#include "bit_buffer.h"
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define random rand
#define MAX_BIT_BUFFERS 4u
#define BIT_BUFFER_HANDLE 0xd5000000u
typedef struct { BitBuffer* native; uint32_t guest_buf, capacity; } ArmBitBuffer;
typedef struct {
    ArmFapVm vm;
    uint32_t errno_buf;
    ArmBitBuffer bit_buffers[MAX_BIT_BUFFERS];
} ArmFapRuntime;
static void fault(ArmFapRuntime* a, const char* s) { arm_fap_vm_fault(&a->vm, s); }
''' + resolver + span + r'''
static bool invoke(ArmFapRuntime* a, ArmFapImport id, uint32_t p, uint32_t q, uint32_t r, uint32_t s) {
    ArmFapVm* vm = &a->vm;
    vm->error[0] = 0;
    uint32_t result = 0;
    ArmBitBuffer* bit_buffer = NULL;
    if(id >= ArmImport_bit_buffer_free && id <= ArmImport_bit_buffer_get_size_bytes) {
        bit_buffer = get_bit_buffer(a, p); if(!bit_buffer) return false;
    }
    switch(id) {
''' + cases + r'''
    default: abort();
    }
    vm->r[0] = result;
    return !vm->error[0];
}
static uint32_t run(ArmFapRuntime* a, ArmFapImport id, uint32_t p, uint32_t q, uint32_t r) {
    assert(invoke(a, id, p, q, r, 0)); return a->vm.r[0];
}
int main(int argc, char** argv) {
    ArmFapRuntime a = {0}; ArmFapVm* vm = &a.vm;
    vm->ram = calloc(1, ARM_FAP_RAM_SIZE); assert(vm->ram);
    vm->heap_start = vm->heap_end = ARM_FAP_BASE + 1024;
    vm->heap_limit = ARM_FAP_BASE + ARM_FAP_RAM_SIZE - ARM_FAP_STACK_SIZE;
    vm->regions[vm->region_count++] = (ArmFapRegion){ARM_FAP_BASE, 256, 0};
    uint32_t src = arm_fap_vm_alloc(vm, 32), dst = arm_fap_vm_alloc(vm, 32);
    char* text = arm_fap_vm_pointer(vm, src, 32, true);
    char* out = arm_fap_vm_pointer(vm, dst, 32, true);
    strcpy(text, "AbcAbc"); memset(out, 0x55, 32);
    assert(run(&a, ArmImport_strchr, src, 'b', 0) == src+1);
    assert(run(&a, ArmImport_strrchr, src, 'A', 0) == src+3);
    assert(run(&a, ArmImport_strchr, src, 0, 0) == src+6);
    assert(run(&a, ArmImport_memchr, src, 'z', 6) == 0);
    assert(run(&a, ArmImport_strcpy, dst, src, 0) == dst && !strcmp(out, text));
    assert(run(&a, ArmImport_strstr, src, dst, 0) == src);
    strcpy(out, "abcabc"); assert(run(&a, ArmImport_strcasecmp, src, dst, 0) == 0);
    uint32_t copy = run(&a, ArmImport_strdup, src, 0, 0);
    assert(copy != src && !strcmp(arm_fap_vm_pointer(vm, copy, 7, false), text));
    assert(!invoke(&a, ArmImport_strcpy, ARM_FAP_BASE, src, 0, 0));
    assert(!invoke(&a, ArmImport_strcpy, src+1, src, 0, 0));
    assert(!strcmp(text, "AbcAbc"));
    assert(run(&a, ArmImport_memcmp, 0xffffffff, 0, 0) == 0);
    assert(run(&a, ArmImport_strncmp, 0xffffffff, 0, 0) == 0);
    assert(run(&a, ArmImport_strncpy, 0xffffffff, 0, 0) == 0xffffffff);
    vm->ram[ARM_FAP_RAM_SIZE-1] = 'X';
    uint32_t edge = ARM_FAP_BASE + ARM_FAP_RAM_SIZE-1;
    assert(run(&a, ArmImport_strncpy, dst, edge, 1) == dst && out[0] == 'X');
    memset(out, 0x55, 32);
    assert(!invoke(&a, ArmImport_strncpy, dst, edge, 2, 0) && out[0] == 0x55);
    assert(run(&a, ArmImport_strncpy, dst, src, 12) == dst);
    for(unsigned i=7;i<12;i++) assert(out[i] == 0);
    assert(out[12] == 0x55);
    assert(run(&a, ArmImport_calloc, 0x80000001, 2, 0) == 0);
    uint32_t block = run(&a, ArmImport_calloc, 2, 8, 0);
    uint8_t* bytes = arm_fap_vm_pointer(vm, block, 16, true);
    for(unsigned i=0;i<16;i++) { assert(bytes[i] == 0); bytes[i] = (uint8_t)i; }
    assert(run(&a, ArmImport_realloc, block, ARM_FAP_RAM_SIZE, 0) == 0);
    assert(bytes[15] == 15);
    uint32_t grown = run(&a, ArmImport_realloc, block, 64, 0);
    assert(grown && grown != block && arm_fap_vm_read(vm, grown+15, 1) == 15);
    assert(!invoke(&a, ArmImport_realloc, grown+1, 32, 0, 0));
    assert(run(&a, ArmImport_realloc, grown, 0, 0) == 0);
    strcpy(text, "18446744073709551615!");
    assert(run(&a, ArmImport_strtoull, src, dst, 10) == 0xffffffff && vm->r[1] == 0xffffffff);
    assert(arm_fap_vm_read(vm, dst, 4) == src+20);
    strcpy(text, "-2147483648x");
    assert(run(&a, ArmImport_strtol, src, dst, 10) == 0x80000000);
    assert(arm_fap_vm_read(vm, dst, 4) == src+11);
    strcpy(text, "2.5x"); assert(run(&a, ArmImport_strtof, src, dst, 0) == 0x40200000);
    assert(arm_fap_vm_read(vm, dst, 4) == src+3);
    assert(run(&a, ArmImport_roundf, 0xc0200000, 0, 0) == 0xc0400000);
    assert(!invoke(&a, ArmImport_strtol, src, dst, 1, 0));
    assert(!invoke(&a, ArmImport_abort, 0, 0, 0, 0));
    assert(!invoke(&a, ArmImport___assert_func, 0xffffffff, 0, 0, 0));
    for(unsigned i=0;i<32;i++) text[i]=(char)(i*73+19);
    for(unsigned start=0;start<190;start++) for(unsigned n=1;n<=64;n++) {
        uint64_t expected=0;
        for(unsigned j=0;j<n;j++) expected=(expected<<1)|(((uint8_t)text[(start+j)/8]>>(7-(start+j)%8))&1);
        uint32_t low=run(&a, ArmImport_bit_lib_get_bits_64, src, start, n);
        assert((((uint64_t)vm->r[1]<<32)|low)==expected);
    }
    assert(run(&a, ArmImport_bit_lib_get_bits, edge, 1, 7) == ('X' & 127));
    assert(!invoke(&a, ArmImport_bit_lib_get_bits, edge, 1, 8, 0));
    assert(!invoke(&a, ArmImport_bit_lib_get_bits_64, src, 0xffffffff, 64, 0));
    uint32_t buffer=run(&a, ArmImport_bit_buffer_alloc, 3, 0, 0);
    assert(buffer == BIT_BUFFER_HANDLE);
    run(&a, ArmImport_bit_buffer_append_byte, buffer, 0xaa, 0);
    run(&a, ArmImport_bit_buffer_append_bytes, buffer, src, 2);
    assert(run(&a, ArmImport_bit_buffer_get_size_bytes, buffer, 0, 0)==3);
    uint32_t data=run(&a, ArmImport_bit_buffer_get_data, buffer, 0, 0);
    assert(data >= ARM_FAP_BASE && data < ARM_FAP_BASE+ARM_FAP_RAM_SIZE);
    assert(arm_fap_vm_read(vm, data, 1)==0xaa);
    assert(!invoke(&a, ArmImport_bit_buffer_append_byte, buffer, 0xbb, 0, 0));
    assert(run(&a, ArmImport_bit_buffer_get_size_bytes, buffer, 0, 0)==3);
    assert(!invoke(&a, ArmImport_bit_buffer_get_data, buffer+1, 0, 0, 0));
    run(&a, ArmImport_bit_buffer_reset, buffer, 0, 0);
    assert(run(&a, ArmImport_bit_buffer_get_size_bytes, buffer, 0, 0)==0);
    run(&a, ArmImport_bit_buffer_free, buffer, 0, 0);
    assert(!invoke(&a, ArmImport_bit_buffer_free, buffer, 0, 0, 0));
    free(vm->ram);
    /* Exercise ELF data relocation using an unchanged fixture except the import name. */
    assert(argc==2); FILE* f=fopen(argv[1], "rb"); assert(f);
    fseek(f,0,SEEK_END); long size=ftell(f); rewind(f);
    uint8_t* elf=malloc(size); assert(fread(elf,1,size,f)==(size_t)size); fclose(f);
    const char old[]="furi_hal_random_get"; bool replaced=false;
    for(long i=0;i<size-(long)sizeof(old);i++) if(!memcmp(elf+i,old,sizeof(old))) {
        memset(elf+i,0,sizeof(old)); memcpy(elf+i,"_ctype_",7); replaced=true; break;
    }
    assert(replaced); memset(&a,0,sizeof(a)); vm->ram=calloc(1,ARM_FAP_RAM_SIZE);
    assert(arm_fap_vm_load(vm,elf,size));
    ArmFapRegion* table=&vm->regions[vm->region_count-1];
    assert(table->size==257 && table->flags==0);
    assert(arm_fap_vm_read(vm,table->address+'A'+1,1)==65);
    assert(arm_fap_vm_read(vm,table->address+'0'+1,1)==4);
    assert(arm_fap_vm_read(vm,table->address+' '+1,1)==136);
    assert(arm_fap_vm_read(vm,table->address+256,1)==0);
    assert(!arm_fap_vm_pointer(vm,table->address,1,true));
    free(vm->ram); free(elf);
    puts("Actual libc/BitBuffer bridges, 12,160 bit-field cases, and read-only ctype ELF relocation passed.");
    return 0;
}
'''
(OUT / 'other.c').write_text(source)
vcvars = next((p for version in ('2026', '2022', '18', '17')
               for edition in ('BuildTools', 'Community')
               if (p := Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) /
                   'Microsoft Visual Studio' / version / edition / 'VC/Auxiliary/Build/vcvars64.bat').exists()), None)
if not vcvars:
    raise SystemExit('Visual Studio Build Tools not found')
script = OUT / 'build.cmd'
script.write_text(f'''@echo off
call "{vcvars}" >nul
if errorlevel 1 exit /b %errorlevel%
cd /d "{OUT}"
cl /nologo /std:c17 /W4 /D_CRT_SECURE_NO_WARNINGS /I"{OUT}" /I"{ROOT / 'components/flipper_application/arm_fap'}" /I"{ROOT / 'components/toolbox'}" other.c "{ROOT / 'components/toolbox/bit_buffer.c'}" /Fe:other.exe
exit /b %errorlevel%
''')
subprocess.run(['cmd', '/c', str(script)], check=True)
subprocess.run([str(OUT / 'other.exe'), str(ROOT / 'tests/fixtures/arm_fap_rps/pixel_rps.fap')], check=True)
