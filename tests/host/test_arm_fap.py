"""Run the unchanged published FAP in the C core and Unicorn; compare API traces.

Run build_arm_fap_test.bat first. Python requires pyelftools, capstone and unicorn.
Workspace-only wheels may be placed in build_host/python_arm.
"""
import ctypes as C
import hashlib
import io
from pathlib import Path
import random
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'build_host/python_arm'))
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import *

BASE, SIZE, IMPORT, RET = 0x10000000, 65536, 0x08000000, 0x08001000
FAP = (ROOT / 'tests/fixtures/arm_fap_rps/pixel_rps.fap').read_bytes()
assert hashlib.sha256(FAP).hexdigest() == '550e2bc478cb6bf7da2ec18fbb8737fcf24031f2a01afbb1d2b2b1a47263175c'
LAB_FAP = (ROOT / 'tests/fixtures/arm_fap_rps_lab/pixel_rps.fap').read_bytes()
assert hashlib.sha256(LAB_FAP).hexdigest() == '3caee16c0ed537bead531b44ddd98158d424d7d4732ae5aa2fc5e83ac0a651e4'
header = (ROOT / 'components/flipper_application/arm_fap/arm_fap_vm.h').read_text()
IMPORTS = re.findall(r'X\((\w+)\)', header.split('#define ARM_FAP_IMPORTS(X)', 1)[1].split('typedef enum', 1)[0])
REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
        UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
        UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
        UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC]
CALLBACK = C.CFUNCTYPE(C.c_bool, C.c_void_p, C.c_int, C.c_void_p)
dll = C.CDLL(str(ROOT / 'build_host/arm_fap_vm.dll'))
def export(name, result, *args):
    fn = getattr(dll, 'armtest_' + name); fn.restype = result; fn.argtypes = list(args); return fn
create = export('create', C.c_void_p, C.c_void_p, C.c_size_t, CALLBACK)
destroy = export('destroy', None, C.c_void_p)
error = export('error', C.c_char_p, C.c_void_p)
entry = export('entry', C.c_uint32, C.c_void_p)
get = export('get', C.c_uint32, C.c_void_p, C.c_uint)
setreg = export('set', None, C.c_void_p, C.c_uint, C.c_uint32)
ram = export('ram', C.c_void_p, C.c_void_p)
alloc = export('alloc', C.c_uint32, C.c_void_p, C.c_size_t)
free = export('free', C.c_bool, C.c_void_p, C.c_uint32)
call = export('call', C.c_bool, C.c_void_p, C.c_uint32, C.c_void_p, C.c_size_t, C.c_uint32, C.c_void_p)
step = export('step', None, C.c_void_p)
flags = export('flags', C.c_uint32, C.c_void_p)
setflags = export('set_flags', None, C.c_void_p, C.c_uint32)
faultpc = export('fault_pc', C.c_uint32, C.c_void_p)
faultop = export('fault_instruction', C.c_uint32, C.c_void_p)
pointer = export('pointer', C.c_void_p, C.c_void_p, C.c_uint32, C.c_size_t, C.c_bool)
rpc_value = export('rpc_value', C.c_char_p, C.c_char_p, C.c_char_p)
mark_yield = export('yield', None, C.c_void_p)

class Native:
    def __init__(self, data=FAP):
        self.exception = None
        def trap(vm, idx, _):
            try:
                result = self.api.invoke(IMPORTS[idx]); setreg(vm, 0, result or 0); return True
            except Exception as exc:
                self.exception = exc; return False
        self.cb = CALLBACK(trap)
        self.data = C.create_string_buffer(data)
        self.vm = create(self.data, len(data), self.cb)
        self.entry = entry(self.vm)
    def reg(self, n): return get(self.vm, n)
    def arg(self, n): return self.reg(n) if n < 4 else int.from_bytes(self.read(self.reg(13) + (n-4)*4, 4), 'little')
    def read(self, a, n):
        p = pointer(self.vm, a, n, False); assert p, error(self.vm); return C.string_at(p, n)
    def write(self, a, data):
        p = pointer(self.vm, a, len(data), True); assert p, error(self.vm); C.memmove(p, data, len(data))
    def alloc(self, n): return alloc(self.vm, n)
    def free(self, a): assert free(self.vm, a)
    def call(self, pc, *args, budget=100000):
        result = C.c_uint32()
        ok = call(self.vm, pc, (C.c_uint32 * len(args))(*args), len(args), budget, C.byref(result))
        if self.exception: raise self.exception
        assert ok, (error(self.vm), hex(faultpc(self.vm)), hex(faultop(self.vm)))
        return result.value
    def close(self): destroy(self.vm)

class Oracle:
    def __init__(self, data=FAP):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.uc.mem_map(BASE, SIZE); self.uc.mem_map(IMPORT, 4096); self.uc.mem_map(RET & ~4095, 4096)
        e = ELFFile(io.BytesIO(data)); self.sections = {}; cursor = BASE
        for i, s in enumerate(e.iter_sections()):
            if s['sh_flags'] & 2 and s['sh_size']:
                align = max(4, s['sh_addralign']); cursor = (cursor + align - 1) & -align
                self.sections[i] = cursor; self.uc.mem_write(cursor, s.data()); cursor += s['sh_size']
                if s.name == '.text': self.entry = self.sections[i] + e['e_entry']
        self.heap = (cursor + 7) & -8
        for s in e.iter_sections():
            if s['sh_type'] != 'SHT_REL': continue
            syms = e.get_section(s['sh_link']); dest = self.sections[s['sh_info']]
            for r in s.iter_relocations():
                assert r['r_info_type'] == 2
                sym = syms.get_symbol(r['r_info_sym'])
                value = IMPORT + IMPORTS.index(sym.name)*4 + 1 if sym['st_shndx'] == 'SHN_UNDEF' else self.sections[sym['st_shndx']] + sym['st_value']
                a = dest + r['r_offset']; addend = int.from_bytes(self.read(a, 4), 'little')
                self.write(a, struct.pack('<I', (addend + value) & 0xffffffff))
        self.uc.reg_write(UC_ARM_REG_SP, BASE + SIZE)
        self.uc.hook_add(UC_HOOK_CODE, lambda uc, a, n, ctx: uc.emu_stop() if IMPORT <= a < IMPORT + len(IMPORTS)*4 else None)
    def reg(self, n): return self.uc.reg_read(REGS[n])
    def arg(self, n): return self.reg(n) if n < 4 else int.from_bytes(self.read(self.reg(13)+(n-4)*4, 4), 'little')
    def read(self, a, n): return bytes(self.uc.mem_read(a, n))
    def write(self, a, data): self.uc.mem_write(a, bytes(data))
    def alloc(self, n): a = self.heap; self.heap += (n+7)&-8; self.write(a, bytes(n)); return a
    def free(self, a): pass
    def call(self, pc, *args):
        saved = self.uc.context_save()
        for i, value in enumerate(args): self.uc.reg_write(REGS[i], value)
        self.uc.reg_write(UC_ARM_REG_LR, RET | 1)
        for _ in range(20000):
            self.uc.emu_start(pc | 1, RET, count=100000)
            pc = self.reg(15)
            if pc == RET: break
            assert IMPORT <= pc < IMPORT + len(IMPORTS)*4, hex(pc)
            self.uc.reg_write(UC_ARM_REG_R0, self.api.invoke(IMPORTS[(pc-IMPORT)//4]) or 0)
            pc = self.reg(14)
        else: raise AssertionError('Oracle call budget')
        result = self.reg(0); self.uc.context_restore(saved); return result
    def close(self): pass

class Api:
    def __init__(self, machine):
        self.m = machine; machine.api = self; self.trace = []; self.cb = {}; self.random = 0
        self.event = machine.alloc(8); self.model = 0; self.stopped = False; self.images = set()
    def invoke(self, name):
        m = self.m; a = [m.arg(i) for i in range(4)]
        self.trace.append((name, a.copy()))
        if name == 'malloc': return m.alloc(a[0])
        if name == 'free': m.free(a[0]); return 0
        if name == '__furi_crash_implementation': raise AssertionError('Guest crash')
        if name == 'furi_hal_random_get': self.random += 1; return 6-self.random
        if name == 'view_alloc': return 0xe0000000
        if name == 'view_dispatcher_alloc': return 0xe1000000
        if name == 'furi_record_open': assert m.read(a[0],4) == b'gui\0'; return 0xe2000000
        if name == 'view_allocate_model': assert a[1:3] == [2,4]; self.model=m.alloc(a[2]); return 0
        if name == 'view_get_model': return self.model
        if name in ('view_set_context','view_dispatcher_set_event_callback_context'):
            self.context = a[1]; return 0
        if name.endswith('_callback'):
            self.cb[name] = a[1]; return 0
        if name == 'view_dispatcher_run':
            for round_number, expected in enumerate([2,1]):
                for frame in range(18):
                    m.call(self.cb['view_set_draw_callback'], 0xe3000000, self.model)
                    m.call(self.cb['view_dispatcher_set_tick_event_callback'], self.context)
                    if frame == 8:
                        before=m.read(self.model,4)
                        for key,kind,consumed in [(0,2,0),(1,2,0),(4,1,0),(4,2,1),(5,2,0)]:
                            m.write(self.event,struct.pack('<IBBxx',1,key,kind))
                            assert m.call(self.cb['view_set_input_callback'],self.event,self.context)==consumed
                            assert m.read(self.model,4)==before
                assert m.read(self.model,4) == bytes([1,expected,expected,18]), m.read(self.model,4)
                if round_number == 0:
                    m.write(self.event, struct.pack('<IBBxx', 1,4,2))
                    assert m.call(self.cb['view_set_input_callback'],self.event,self.context) == 1
            assert m.call(self.cb['view_dispatcher_set_navigation_event_callback'],self.context) == 1
            assert self.stopped
        if name == 'view_dispatcher_stop': self.stopped = True
        if name == 'view_free': m.free(self.model)
        if name == 'canvas_draw_icon':
            width,height,count,rate,frames=struct.unpack('<HHBB2xI',m.read(a[3],12))
            frame=int.from_bytes(m.read(frames,4),'little')
            assert 0 < width <= 128 and 0 < height <=64 and count == 1
            assert m.read(frame,1)[0] in (0,1)
            self.images.add((width,height))
        if name == 'canvas_draw_str_aligned':
            assert m.arg(4)==3 and m.read(m.arg(5),5)==b'PLAY\0'
        return 0

def integration(data=FAP):
    traces=[]
    for cls in (Native, Oracle):
        m=cls(data); api=Api(m)
        assert m.call(m.entry,0)==0
        assert len(api.images)==3
        traces.append(api.trace); m.close()
    assert traces[0]==traces[1], next((i,a,b) for i,(a,b) in enumerate(zip(*traces)) if a!=b)
    print(f'Unchanged ARM FAP ({len(data)} bytes): identical {len(traces[0])}-call API trace in C interpreter and Unicorn; all 3 sprites, 2 rounds, OK and Back passed.')

def profiles():
    # Exercise the actual C RPC transformation for both device-info protocols.
    for separator in ('.', '_'):
        for key, expected in [('firmware.target', b'7'), ('firmware.api.major', b'88'), ('firmware.api.minor', b'2')]:
            assert rpc_value(key.replace('.', separator).encode(), b'old') == expected
        for key in ('hardware.target', 'firmware.version', 'firmware.branch.name', 'protobuf.version.major', 'battery.voltage'):
            assert rpc_value(key.replace('.', separator).encode(), b'actual') == b'actual'
    # Version selection must not disable the import whitelist or accept future ABIs.
    meta = ELFFile(io.BytesIO(FAP)).get_section_by_name('.fapmeta')['sh_offset']
    for major, minor, accepted in [(87,0,True),(87,1,True),(88,0,True),(88,1,True),(88,2,True),
                                    (86,1,False),(87,2,False),(88,3,False),(89,0,False)]:
        data = bytearray(FAP); struct.pack_into('<HH',data,meta+8,minor,major)
        m=Native(bytes(data)); assert bool(error(m.vm)) != accepted, (major,minor,error(m.vm)); m.close()
    data=bytearray(LAB_FAP)
    offset=data.index(b'furi_hal_random_get\0'); data[offset]=ord('x')
    m=Native(bytes(data)); assert b'Unsupported ARM import: xuri_hal_random_get' in error(m.vm); m.close()
    print('RPC catalog identity, unchanged hardware/version properties, bounded API profiles and missing-import rejection passed.')

def differential():
    md=Cs(CS_ARCH_ARM,CS_MODE_THUMB|CS_MODE_MCLASS)
    e=ELFFile(io.BytesIO(FAP)); text=e.get_section_by_name('.text').data()
    instructions=[i for lo,hi in [(0,64),(72,84),(88,208),(252,344),(356,564)] for i in md.disasm(text[lo:hi],BASE+lo)]
    rng=random.Random(457); comparisons=0
    # Compare register/flag semantics of all non-memory/non-branch instructions
    # in the actual binary over random operands and all incoming NZCV flags.
    selected={'mov','movs','mov.w','adds','add.w','subs','cmp','cmp.w','lsrs','uxtb','udiv','nop'}
    for ins in instructions:
        if ins.mnemonic not in selected: continue
        for _ in range(32):
            m=Native(); o=Oracle()
            for r in range(13):
                value=rng.getrandbits(32); setreg(m.vm,r,value); o.uc.reg_write(REGS[r],value)
            f=rng.getrandbits(4)<<28
            setflags(m.vm,f); o.uc.reg_write(UC_ARM_REG_APSR,f)
            setreg(m.vm,15,ins.address); step(m.vm)
            assert not error(m.vm), (ins.mnemonic,error(m.vm))
            o.uc.emu_start(ins.address|1,0,count=1)
            assert [get(m.vm,r) for r in range(16)]==[o.reg(r) for r in range(16)], (ins.mnemonic,ins.op_str)
            assert flags(m.vm)==o.uc.reg_read(UC_ARM_REG_APSR)&0xf0000000,(ins.mnemonic,ins.op_str)
            comparisons+=1; m.close()
    print(f'{comparisons} instruction/flag comparisons against Unicorn passed.')

def failures():
    for n in (0,51,100,len(FAP)-1):
        m=Native(FAP[:n]); assert error(m.vm); m.close()
    corrupt=bytearray(FAP); struct.pack_into('<I',corrupt,32,0xfffffff0)
    m=Native(bytes(corrupt)); assert error(m.vm);m.close()
    m=Native(); assert not pointer(m.vm,BASE,4,True); m.close()
    m=Native(); assert not pointer(m.vm,0xdeadbeef,4,False);m.close()
    m=Native(); a=m.alloc(32);m.free(a);assert m.alloc(16)==a;m.close()
    m=Native();C.memmove(ram(m.vm),b'\xfe\xe7',2)
    result=C.c_uint32();assert not call(m.vm,BASE|1,None,0,10,C.byref(result));assert b'budget' in error(m.vm);m.close()
    m=Native();C.memmove(ram(m.vm),b'\x00\xde',2)
    assert not call(m.vm,BASE|1,None,0,10,C.byref(result));assert b'Unsupported' in error(m.vm);m.close()
    print('Truncation, invalid ELF bounds, guest memory protection, allocation reuse, instruction budget and unsupported opcode checks passed.')

def memory_imports():
    def invoke(m, name, *args):
        return m.call(IMPORT + IMPORTS.index(name) * 4 + 1, *args)

    # Enter through the guest import trap, exercising the same C bridge as firmware.
    m = Native(); src = m.alloc(128); dst = m.alloc(128)
    m.write(src, b'abc\x80\xff\0'); m.write(dst, b'?' * 128)
    assert invoke(m, 'strlen', src) == 5
    assert invoke(m, 'memcpy', dst, src, 6) == dst
    assert m.read(dst, 8) == b'abc\x80\xff\0??'
    assert invoke(m, 'strcmp', src, dst) == 0
    m.write(dst, b'abc\x7f\xff\0')
    assert C.c_int32(invoke(m, 'strcmp', src, dst)).value > 0
    assert C.c_int32(invoke(m, 'strcmp', dst, src)).value < 0
    assert invoke(m, 'memset', dst, 0x141, 4) == dst
    assert m.read(dst, 6) == b'AAAA\xff\0'
    for name in ('memcpy', 'memmove', 'memset'):
        assert invoke(m, name, 0xdeadbeef, 0xffffffff, 0) == 0xdeadbeef
    for name in ('strlcpy', 'strlcat'):
        assert invoke(m, name, 0, src, 0) == 5
    for destination, source in ((1, 0), (0, 1)):
        data = b'0123456789'
        expected = bytearray(data); expected[destination:destination+8] = data[source:source+8]
        m.write(dst, data)
        assert invoke(m, 'memmove', dst+destination, dst+source, 8) == dst+destination
        assert m.read(dst, len(data)) == bytes(expected)
    m.close()

    rng = random.Random(906)
    for _ in range(128):
        capacity = rng.randrange(65)
        source = bytes(rng.randrange(1, 256) for _ in range(rng.randrange(80)))
        prefix = b'x' * rng.randrange(70)
        original = prefix + b'\0' + b'?' * (127-len(prefix))
        for name in ('strlcpy', 'strlcat'):
            m = Native(); src = m.alloc(128); block = m.alloc(144); dst = block+8
            m.write(src, source+b'\0'); m.write(block, b'G'*8 + original + b'G'*8)
            expected = bytearray(original)
            used = min(len(prefix), capacity) if name == 'strlcat' else 0
            if capacity and used < capacity:
                copied = source[:capacity-used-1]
                expected[used:used+len(copied)+1] = copied+b'\0'
            assert invoke(m, name, dst, src, capacity) == used+len(source)
            assert m.read(block, 144) == b'G'*8 + bytes(expected) + b'G'*8
            m.close()

    # No partial write when the source, size or destination is invalid.
    for name, kind in [('memcpy','read_only'), ('memmove','source'), ('memset','size'),
                       ('memcpy','overlap'), ('strlcpy','unterminated'),
                       ('strlcat','size'), ('strlcat','overlap'), ('strlen','limit')]:
        m = Native(); src = m.alloc(32); dst = m.alloc(32)
        m.write(src, b'abc\0'); m.write(dst, b'XY\0' + b'?'*29)
        args = [dst, src, 16]
        if kind == 'read_only': args[0] = BASE
        if kind == 'source': args[1] = 0xffffffff
        if kind == 'size': args[2] = 0xffffffff
        if kind == 'overlap': args[1] = dst+1
        if kind == 'unterminated':
            args[1] = BASE+SIZE-8; m.write(args[1], b'x'*8)
        if kind == 'limit':
            long_string = m.alloc(1025); m.write(long_string, b'x'*1024+b'\0'); args = [long_string]
        before = C.string_at(ram(m.vm), SIZE)
        result = C.c_uint32()
        assert not call(m.vm, IMPORT+IMPORTS.index(name)*4+1,
                        (C.c_uint32*len(args))(*args), len(args), 100, C.byref(result)), (name, kind)
        assert error(m.vm), (name, kind)
        assert C.string_at(ram(m.vm), SIZE) == before, (name, kind)
        m.close()
    print('Seven guest memory/string imports: ABI returns, unsigned comparison, overlapping moves, '
          '256 truncation/guard cases and invalid-access rejection passed.')

def viewport_integration():
    data = (ROOT/'tests/fixtures/arm_fap_dvd/dvd_screensaver.fap').read_bytes()
    assert hashlib.sha256(data).hexdigest() == 'ee75dc13a432f576365d160489fd027b489de183a26a76e350c2e282c11ec7eb'

    class ViewportApi:
        def __init__(self, m):
            self.m = m; m.api = self; self.trace = []; self.cb = {}; self.queue = []
            self.waits = 0; self.random = 0; self.draws = []; self.freed = set()
            self.event = m.alloc(8)

        def invoke(self, name):
            m = self.m; a = [m.arg(i) for i in range(4)]
            self.trace.append((name, a.copy()))
            if name == 'malloc': return m.alloc(a[0])
            if name == 'rand': self.random += 1; return self.random*37
            if name == 'furi_message_queue_alloc':
                assert a[:2] == [8, 8]; return 0xe5000000
            if name == 'view_port_alloc': return 0xe4000000
            if name in ('view_port_draw_callback_set', 'view_port_input_callback_set'):
                assert a[0] == 0xe4000000; self.cb[name] = a[1:3]; return 0
            if name == 'furi_record_open':
                if m.read(a[0],4) == b'gui\0': return 0xe2000000
                assert m.read(a[0],13) == b'notification\0'; return 0xe6000000
            if name == 'gui_add_view_port': assert a[:3] == [0xe2000000,0xe4000000,4]
            if name.startswith('notification_message'):
                symbol = 'sequence_display_backlight_enforce_on' if name.endswith('_block') else 'sequence_display_backlight_enforce_auto'
                assert a[:2] == [0xe6000000,IMPORT+IMPORTS.index(symbol)*4+1]
            if name == 'furi_message_queue_put':
                assert a[0] == 0xe5000000
                self.queue.append(m.read(a[1],8)); return 0
            if name == 'furi_message_queue_get':
                assert a[0] == 0xe5000000 and a[2] == 100
                self.waits += 1
                if isinstance(m, Native): mark_yield(m.vm)
                callback, context = self.cb['view_port_draw_callback_set']
                m.call(callback, 0xe3000000, context)
                # Drive more instructions than one budget, with real app wait boundaries.
                if self.waits % 100 == 0:
                    m.write(self.event, struct.pack('<IBBxx', self.waits, 5 if self.waits == 2200 else 4, 0))
                    callback, context = self.cb['view_port_input_callback_set']
                    m.call(callback, self.event, context)
                if self.queue:
                    m.write(a[1], self.queue.pop(0)); return 0
                return 0xfffffffe
            if name == 'canvas_draw_icon':
                assert a[0] == 0xe3000000 and 0 <= a[1] <= 96 and 0 <= a[2] <= 48
                self.draws.append(a[1:3])
            if name in ('furi_message_queue_free', 'view_port_free', 'gui_remove_view_port'):
                self.freed.add(name)
            return 0

    n = Native(data); o = Oracle(data); a = ViewportApi(n); b = ViewportApi(o)
    assert not error(n.vm), error(n.vm)
    assert n.call(n.entry,0) == o.call(o.entry,0) == 0
    assert a.trace == b.trace
    assert len(a.draws) == 2200 and a.draws == b.draws
    assert len(set(map(tuple,a.draws))) > 100
    assert a.freed == {'furi_message_queue_free','view_port_free','gui_remove_view_port'}
    n.close(); o.close()
    print(f'Unchanged catalog DVD FAP: {len(a.trace)} identical API calls, 2,200 frames, '
          'nested input/draw callbacks, queue waits and Back exit passed in C and Unicorn.')

def thumb_extensions():
    data = (ROOT/'tests/fixtures/arm_fap_dvd/dvd_screensaver.fap').read_bytes()
    elf = ELFFile(io.BytesIO(data)); text = elf.get_section_by_name('.text').data()
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB|CS_MODE_MCLASS)
    selected = [i for i in md.disasm(text[0x3c:0x14c], BASE+0x3c)
                if i.mnemonic in {'sdiv','mls','tst.w','ldr.w','ldrb.w'}]
    rng = random.Random(907); count = 0
    for ins in selected:
        for run in range(32):
            m = Native(data); o = Oracle(data)
            for r in range(13):
                value = rng.getrandbits(32); setreg(m.vm,r,value); o.uc.reg_write(REGS[r],value)
            # The DVD LDRB reads its stack; initialize that byte on both machines.
            sp = BASE+SIZE-128; setreg(m.vm,13,sp); o.uc.reg_write(UC_ARM_REG_SP,sp)
            m.write(sp,b'\xa5'*32); o.write(sp,b'\xa5'*32)
            if ins.mnemonic == 'sdiv' and run < 2:
                for r,value in [(0,0x80000000),(3,0 if run==0 else 0xffffffff)]:
                    setreg(m.vm,r,value); o.uc.reg_write(REGS[r],value)
            f=rng.getrandbits(4)<<28; setflags(m.vm,f); o.uc.reg_write(UC_ARM_REG_APSR,f)
            setreg(m.vm,15,ins.address); step(m.vm)
            assert not error(m.vm), (ins.mnemonic,error(m.vm))
            o.uc.emu_start(ins.address|1,0,count=1)
            assert [get(m.vm,r) for r in range(16)] == [o.reg(r) for r in range(16)], (ins.mnemonic,ins.op_str)
            assert flags(m.vm) == o.uc.reg_read(UC_ARM_REG_APSR)&0xf0000000, ins.mnemonic
            count+=1; m.close()
    # IT/ITE must skip complete wide instructions and preserve implicit flags.
    for value in (0,1,2,3,0x80000000,0xffffffff):
        for incoming in range(16):
            m=Native(data); o=Oracle(data)
            setreg(m.vm,0,value); o.uc.reg_write(UC_ARM_REG_R0,value)
            setflags(m.vm,incoming<<28); o.uc.reg_write(UC_ARM_REG_APSR,incoming<<28)
            setreg(m.vm,15,BASE+0x7c)
            for _ in range(8):
                if get(m.vm,15)==BASE+0x88: break
                step(m.vm); assert not error(m.vm), error(m.vm)
            assert get(m.vm,15)==BASE+0x88
            o.uc.emu_start((BASE+0x7c)|1,BASE+0x88,count=8)
            assert [get(m.vm,r) for r in range(16)]==[o.reg(r) for r in range(16)]
            assert flags(m.vm)==o.uc.reg_read(UC_ARM_REG_APSR)&0xf0000000
            count+=1; m.close()
    print(f'{count} additional DVD instruction/IT-block comparisons against Unicorn passed.')

if __name__=='__main__':
    integration(); integration(LAB_FAP); profiles(); differential(); failures(); memory_imports(); viewport_integration(); thumb_extensions()
