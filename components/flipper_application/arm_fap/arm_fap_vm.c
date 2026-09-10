#include "arm_fap_vm.h"
#include "arm_fap_profile.h"
#include <stdio.h>
#include <string.h>

static uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t u32(const uint8_t* p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put32(uint8_t* p, uint32_t v) {
    for(unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static bool range(size_t size, uint32_t start, uint32_t count) {
    return start <= size && count <= size - start;
}
static const uint8_t* section(const uint8_t* f, unsigned i) {
    return f + u32(f + 32) + i * 40;
}
static bool header_valid(const uint8_t* f, size_t size) {
    if(size < 52 || size > ARM_FAP_MAX_FILE || memcmp(f, "\177ELF\1\1\1", 7) ||
       u16(f + 16) != 1 || u16(f + 18) != 40 || u32(f + 20) != 1 ||
       u16(f + 40) != 52 || u16(f + 46) != 40 || !u16(f + 48) ||
       u16(f + 48) > ARM_FAP_MAX_SECTIONS || u16(f + 50) >= u16(f + 48) ||
       !range(size, u32(f + 32), u16(f + 48) * 40u)) return false;
    for(unsigned i = 0; i < u16(f + 48); i++) {
        const uint8_t* s = section(f, i);
        if(u32(s + 4) != 8 && !range(size, u32(s + 16), u32(s + 20))) return false;
    }
    return true;
}
static const char* name_at(const uint8_t* f, const uint8_t* s, uint32_t off) {
    if(off >= u32(s + 20)) return NULL;
    const char* name = (const char*)f + u32(s + 16) + off;
    return memchr(name, 0, u32(s + 20) - off) ? name : NULL;
}
bool arm_fap_inspect(const uint8_t* f, size_t size, uint8_t manifest[85]) {
    if(!header_valid(f, size)) return false;
    const uint8_t* names = section(f, u16(f + 50));
    if(u32(names + 4) != 3) return false;
    for(unsigned i = 0; i < u16(f + 48); i++) {
        const uint8_t* s = section(f, i);
        const char* name = name_at(f, names, u32(s));
        if(!name) return false;
        if(!strcmp(name, ".fapmeta") && u32(s + 4) == 1 &&
           (u32(s + 20) == 85 || (u32(s + 20) == 86 && f[u32(s + 16) + 85] == 0))) {
            memcpy(manifest, f + u32(s + 16), 85);
            return u32(manifest) == 0x52474448 && u32(manifest + 4) == 1 &&
                   u16(manifest + 12) == 7 && u16(manifest + 14) != 0;
        }
    }
    return false;
}
void arm_fap_vm_fault(ArmFapVm* vm, const char* error) {
    if(!vm->error[0]) snprintf(vm->error, sizeof(vm->error), "%s", error);
}
void* arm_fap_vm_pointer(ArmFapVm* vm, uint32_t address, size_t size, bool write) {
    uint32_t off = address - ARM_FAP_BASE;
    if(size > ARM_FAP_RAM_SIZE || !range(ARM_FAP_RAM_SIZE, off, (uint32_t)size)) {
        arm_fap_vm_fault(vm, "Guest address out of bounds");
        return NULL;
    }
    bool valid = false;
    for(size_t i = 0; i < vm->region_count; i++) {
        ArmFapRegion* r = &vm->regions[i];
        if(address >= r->address && range(r->size, address - r->address, (uint32_t)size)) {
            valid = !write || (r->flags & 1);
            break;
        }
    }
    if(address >= vm->heap_start && address <= vm->heap_end && size <= vm->heap_end - address)
        valid = true;
    if(address >= vm->heap_limit && range(ARM_FAP_RAM_SIZE, off, (uint32_t)size)) valid = true;
    if(!valid) {
        arm_fap_vm_fault(vm, "Guest access to unmapped/read-only memory");
        return NULL;
    }
    return vm->ram + off;
}
uint32_t arm_fap_vm_read(ArmFapVm* vm, uint32_t a, unsigned bytes) {
    const uint8_t* p = arm_fap_vm_pointer(vm, a, bytes, false);
    if(!p) return 0;
    return bytes == 1 ? p[0] : bytes == 2 ? u16(p) : u32(p);
}
bool arm_fap_vm_write(ArmFapVm* vm, uint32_t a, uint32_t v, unsigned bytes) {
    uint8_t* p = arm_fap_vm_pointer(vm, a, bytes, true);
    if(!p) return false;
    for(unsigned i = 0; i < bytes; i++) p[i] = (uint8_t)(v >> (i * 8));
    return true;
}
const char* arm_fap_vm_string(ArmFapVm* vm, uint32_t a) {
    for(unsigned i = 0; i < 1024; i++) {
        const char* p = arm_fap_vm_pointer(vm, a + i, 1, false);
        if(!p) return NULL;
        if(!*p) return (const char*)vm->ram + (a - ARM_FAP_BASE);
    }
    arm_fap_vm_fault(vm, "Guest string exceeds limit");
    return NULL;
}

/* These imports operate only on validated guest bytes. They share the same
 * implementation on the ESP32 and host; no guest pointer reaches host libc
 * until its complete access range has been checked. */
static bool memory_import(ArmFapVm* vm, ArmFapImport id) {
    uint32_t dst = vm->r[0], src = vm->r[1], capacity = vm->r[2];
    if(id == ArmImport_memcpy || id == ArmImport_memmove || id == ArmImport_memset) {
        if(!capacity) return true; /* Return dst without dereferencing either pointer. */
        uint8_t* output = arm_fap_vm_pointer(vm, dst, capacity, true);
        if(!output) return false;
        if(id == ArmImport_memset) {
            memset(output, (uint8_t)src, capacity);
        } else {
            const uint8_t* input = arm_fap_vm_pointer(vm, src, capacity, false);
            if(!input) return false;
            if(id == ArmImport_memcpy && dst < src + capacity && src < dst + capacity) {
                arm_fap_vm_fault(vm, "Overlapping guest memcpy"); return false;
            }
            memmove(output, input, capacity);
        }
        return true;
    }
    if(id == ArmImport_strlen || id == ArmImport_strcmp) {
        const char* left = arm_fap_vm_string(vm, dst);
        if(!left) return false;
        if(id == ArmImport_strlen) vm->r[0] = (uint32_t)strlen(left);
        else {
            const char* right = arm_fap_vm_string(vm, src);
            if(!right) return false;
            vm->r[0] = (uint32_t)strcmp(left, right);
        }
        return true;
    }
    const char* input = arm_fap_vm_string(vm, src);
    if(!input) return false;
    uint32_t length = (uint32_t)strlen(input), used = 0;
    if(!capacity) { vm->r[0] = length; return true; }
    char* output = arm_fap_vm_pointer(vm, dst, capacity, true);
    if(!output) return false;
    if(id == ArmImport_strlcat) {
        const char* end = memchr(output, 0, capacity);
        used = end ? (uint32_t)(end - output) : capacity;
    }
    vm->r[0] = used + length;
    if(used == capacity) return true; /* No NUL in the destination: do not change it. */
    uint32_t copied = capacity - used - 1;
    if(copied > length) copied = length;
    if(dst < src + length + 1 && src < dst + used + copied + 1) {
        arm_fap_vm_fault(vm, "Overlapping guest string copy"); return false;
    }
    memcpy(output + used, input, copied);
    output[used + copied] = 0;
    return true;
}

uint32_t arm_fap_vm_alloc(ArmFapVm* vm, size_t size) {
    if(!size || size > ARM_FAP_RAM_SIZE) return 0;
    uint32_t n = ((uint32_t)size + 7) & ~7u;
    unsigned slot = 32;
    for(unsigned i = 0; i < 32; i++) {
        if(!vm->allocations[i].used) {
            if(!vm->allocations[i].address) slot = i;
            else if(vm->allocations[i].size >= n) {
                vm->allocations[i].used = true;
                memset(vm->ram + vm->allocations[i].address - ARM_FAP_BASE, 0, n);
                return vm->allocations[i].address;
            }
        }
    }
    if(slot == 32) return 0;
    if(n > vm->heap_limit - vm->heap_end) return 0;
    uint32_t result = vm->heap_end;
    vm->heap_end += n;
    vm->allocations[slot].address = result;
    vm->allocations[slot].size = n;
    vm->allocations[slot].used = true;
    memset(vm->ram + result - ARM_FAP_BASE, 0, n);
    return result;
}
bool arm_fap_vm_free(ArmFapVm* vm, uint32_t address) {
    if(!address) return true;
    for(unsigned i = 0; i < 32; i++) {
        if(vm->allocations[i].address == address && vm->allocations[i].used) {
            vm->allocations[i].used = false;
            return true;
        }
    }
    arm_fap_vm_fault(vm, "Invalid guest free"); return false;
}
static const char* const import_names[] = {
#define ARM_FAP_NAME(name) #name,
    ARM_FAP_IMPORTS(ARM_FAP_NAME)
#undef ARM_FAP_NAME
};
bool arm_fap_vm_load(ArmFapVm* vm, const uint8_t* f, size_t size) {
    uint8_t manifest[85];
    if(!vm->ram || !arm_fap_inspect(f, size, manifest)) {
        arm_fap_vm_fault(vm, "Invalid ARM FAP"); return false;
    }
    if(!arm_fap_profile_accepts(u16(manifest + 10), u16(manifest + 8))) {
        arm_fap_vm_fault(vm, "ARM API profile unsupported (87.0-1 / 88.0-2)"); return false;
    }
    uint32_t addresses[ARM_FAP_MAX_SECTIONS] = {0};
    uint32_t cursor = ARM_FAP_BASE;
    vm->heap_limit = ARM_FAP_BASE + ARM_FAP_RAM_SIZE - ARM_FAP_STACK_SIZE;
    vm->r[13] = ARM_FAP_BASE + ARM_FAP_RAM_SIZE;
    const uint8_t* names = section(f, u16(f + 50));
    for(unsigned i = 0; i < u16(f + 48); i++) {
        const uint8_t* s = section(f, i);
        uint32_t flags = u32(s + 8), n = u32(s + 20), align = u32(s + 32);
        if(!(flags & 2) || !n) continue;
        if((u32(s + 4) != 1 && u32(s + 4) != 8) || align > 4096 || (align & (align - 1))) {
            arm_fap_vm_fault(vm, "Unsupported ARM section"); return false;
        }
        if(align < 4) align = 4;
        cursor = (cursor + align - 1) & ~(align - 1);
        if(cursor > vm->heap_limit || n > vm->heap_limit - cursor) {
            arm_fap_vm_fault(vm, "ARM image exceeds arena"); return false;
        }
        addresses[i] = cursor;
        vm->regions[vm->region_count++] = (ArmFapRegion){cursor, n, flags};
        if(u32(s + 4) == 8) memset(vm->ram + cursor - ARM_FAP_BASE, 0, n);
        else memcpy(vm->ram + cursor - ARM_FAP_BASE, f + u32(s + 16), n);
        const char* name = name_at(f, names, u32(s));
        if(name && !strcmp(name, ".text") && (flags & 4) && (u32(f + 24) & 1) &&
           (u32(f + 24) & ~1u) < n) vm->entry = cursor + u32(f + 24);
        cursor += n;
    }
    vm->heap_start = vm->heap_end = (cursor + 7) & ~7u;
    if(!vm->entry) { arm_fap_vm_fault(vm, "Invalid Thumb entry point"); return false; }
    for(unsigned i = 0; i < u16(f + 48); i++) {
        const uint8_t* s = section(f, i);
        if(u32(s + 4) == 4) goto invalid_relocation; /* SHT_RELA is not implemented. */
        if(u32(s + 4) != 9) continue; /* Standard SHT_REL; fast relocation copies are optional. */
        uint32_t dest = u32(s + 28), symidx = u32(s + 24);
        if(dest >= u16(f + 48) || symidx >= u16(f + 48) || u32(s + 20) % 8 ||
           u32(s + 36) != 8) goto invalid_relocation;
        if(!addresses[dest]) continue;
        const uint8_t* syms = section(f, symidx);
        if(u32(syms + 4) != 2 || u32(syms + 36) != 16 || u32(syms + 20) % 16 ||
           u32(syms + 24) >= u16(f + 48)) goto invalid_relocation;
        const uint8_t* strings = section(f, u32(syms + 24));
        if(u32(strings + 4) != 3) goto invalid_relocation;
        for(uint32_t off = 0; off < u32(s + 20); off += 8) {
            const uint8_t* rel = f + u32(s + 16) + off;
            uint32_t index = u32(rel + 4) >> 8, target = u32(rel);
            if((u32(rel + 4) & 255) != 2 || index >= u32(syms + 20) / 16 ||
               !range(u32(section(f, dest) + 20), target, 4)) goto invalid_relocation;
            const uint8_t* sym = f + u32(syms + 16) + index * 16;
            uint32_t value = u32(sym + 4), sec = u16(sym + 14);
            if(sec == 0) {
                const char* name = name_at(f, strings, u32(sym));
                if(!name) goto invalid_relocation;
                unsigned id;
                for(id = 0; id < ArmImportCount; id++) if(!strcmp(name, import_names[id])) break;
                if(id == ArmImportCount) {
                    snprintf(vm->error, sizeof(vm->error), "Unsupported ARM import: %.75s", name);
                    return false;
                }
                value = ARM_FAP_IMPORT_BASE + id * 4 + 1;
            } else if(sec < u16(f + 48) && addresses[sec] &&
                      (value & ~1u) <= u32(section(f, sec) + 20)) value += addresses[sec];
            else goto invalid_relocation;
            uint8_t* p = vm->ram + addresses[dest] - ARM_FAP_BASE + target;
            put32(p, u32(p) + value);
        }
    }
    return true;
invalid_relocation:
    arm_fap_vm_fault(vm, "Unsupported or invalid ARM relocation");
    return false;
}

static void nz(ArmFapVm* vm, uint32_t v) { vm->n = (v >> 31) != 0; vm->z = v == 0; }
static uint32_t add(ArmFapVm* vm, uint32_t a, uint32_t b, bool sub, bool flags) {
    uint32_t v = sub ? a - b : a + b;
    if(flags) {
        nz(vm, v);
        vm->c = sub ? a >= b : ((uint64_t)a + b) > UINT32_MAX;
        vm->v = sub ? ((a ^ b) & (a ^ v) & 0x80000000u) != 0 :
                      ((~(a ^ b)) & (a ^ v) & 0x80000000u) != 0;
    }
    return v;
}
static bool condition(ArmFapVm* vm, unsigned c) {
    bool yes;
    switch(c >> 1) {
    case 0: yes = vm->z; break;
    case 1: yes = vm->c; break;
    case 2: yes = vm->n; break;
    case 3: yes = vm->v; break;
    case 4: yes = vm->c && !vm->z; break;
    case 5: yes = vm->n == vm->v; break;
    case 6: yes = !vm->z && vm->n == vm->v; break;
    default: return c == 14;
    }
    return (c & 1) ? !yes : yes;
}
static uint32_t reg(ArmFapVm* vm, unsigned r, uint32_t pc) { return r == 15 ? pc + 4 : vm->r[r]; }
static void branch(ArmFapVm* vm, uint32_t target) {
    if(!(target & 1)) arm_fap_vm_fault(vm, "Non-Thumb branch target");
    vm->r[15] = target & ~1u;
}
static void stack(ArmFapVm* vm, uint16_t list, bool pop) {
    uint32_t n = 0;
    for(unsigned i = 0; i < 16; i++) if(list & (1u << i)) n += 4;
    uint32_t a = vm->r[13];
    if(!pop) a -= n;
    for(unsigned i = 0; i < 16; i++) if(list & (1u << i)) {
        if(pop) {
            uint32_t value = arm_fap_vm_read(vm, a, 4);
            if(i == 15) branch(vm, value); else vm->r[i] = value;
        } else arm_fap_vm_write(vm, a, vm->r[i], 4);
        a += 4;
    }
    vm->r[13] += pop ? n : -n;
}
static bool executable(ArmFapVm* vm, uint32_t pc, unsigned size) {
    for(size_t i = 0; i < vm->region_count; i++) {
        ArmFapRegion* r = &vm->regions[i];
        if((r->flags & 4) && pc >= r->address && range(r->size, pc - r->address, size)) return true;
    }
    return false;
}
static void step(ArmFapVm* vm) {
    uint32_t pc = vm->r[15];
    vm->fault_pc = pc;
    if(!executable(vm, pc, 2)) { arm_fap_vm_fault(vm, "Execution outside guest code"); return; }
    uint16_t h = (uint16_t)arm_fap_vm_read(vm, pc, 2);
    vm->fault_instruction = h;
    vm->r[15] = pc + 2;
    bool in_it = vm->itstate != 0;
    bool old_n = vm->n, old_z = vm->z, old_c = vm->c, old_v = vm->v;
    if(in_it) {
        bool execute = condition(vm, vm->itstate >> 4);
        vm->itstate = (vm->itstate & 7) ? (vm->itstate & 0xe0) | ((vm->itstate << 1) & 0x1f) : 0;
        if(!execute) {
            if((h & 0xf800) >= 0xe800) {
                if(!executable(vm, pc, 4)) { arm_fap_vm_fault(vm, "Execution outside guest code"); return; }
                vm->r[15] = pc + 4;
            }
            return;
        }
    }
    unsigned rd = h & 7, rn = (h >> 3) & 7, rm = (h >> 6) & 7;
    if((h & 0xf800) < 0x1800) { /* LSL/LSR/ASR immediate */
        unsigned op = h >> 11, shift = (h >> 6) & 31;
        uint32_t v = vm->r[rn];
        if(op && !shift) shift = 32;
        if(shift) vm->c = op ? ((v >> (shift - 1)) & 1) : ((v >> (32 - shift)) & 1);
        if(op == 0) v <<= shift;
        else if(op == 1) v = shift == 32 ? 0 : v >> shift;
        else v = shift == 32 ? (v >> 31 ? UINT32_MAX : 0) : (uint32_t)((int32_t)v >> shift);
        vm->r[rd] = v; nz(vm, v);
    } else if((h & 0xf800) == 0x1800) {
        vm->r[rd] = add(vm, vm->r[rn], h & 0x400 ? rm : vm->r[rm], (h & 0x200) != 0, true);
    } else if((h & 0xe000) == 0x2000) {
        rd = (h >> 8) & 7; unsigned op = (h >> 11) & 3;
        uint32_t v = op == 0 ? h & 255 : add(vm, vm->r[rd], h & 255, op != 2, true);
        if(op != 1) vm->r[rd] = v;
        if(op == 0) nz(vm, v);
    } else if((h & 0xff00) == 0x4600 || (h & 0xff00) == 0x4400 || (h & 0xff00) == 0x4500) {
        rd = (h & 7) | ((h >> 4) & 8); rm = (h >> 3) & 15;
        uint32_t v = reg(vm, rm, pc);
        if((h & 0xff00) != 0x4600) v = add(vm, reg(vm, rd, pc), v, (h & 0x100) != 0, (h & 0x100) != 0);
        if((h & 0xff00) != 0x4500) { if(rd == 15) vm->r[15] = v & ~1u; else vm->r[rd] = v; }
    } else if((h & 0xff07) == 0x4700) {
        uint32_t target = reg(vm, (h >> 3) & 15, pc);
        if(h & 0x80) vm->r[14] = (pc + 2) | 1;
        branch(vm, target);
    } else if((h & 0xf800) == 0x4800) {
        vm->r[(h >> 8) & 7] = arm_fap_vm_read(vm, ((pc + 4) & ~3u) + (h & 255) * 4, 4);
    } else if((h & 0xe000) == 0x6000 || (h & 0xf000) == 0x8000) {
        unsigned bytes = (h & 0xf000) == 0x8000 ? 2 : (h & 0x1000) ? 1 : 4;
        uint32_t a = vm->r[rn] + ((h >> 6) & 31) * bytes;
        if(h & 0x800) vm->r[rd] = arm_fap_vm_read(vm, a, bytes);
        else arm_fap_vm_write(vm, a, vm->r[rd], bytes);
    } else if((h & 0xf000) == 0x9000) {
        uint32_t a = vm->r[13] + (h & 255) * 4;
        rd = (h >> 8) & 7;
        if(h & 0x800) vm->r[rd] = arm_fap_vm_read(vm, a, 4);
        else arm_fap_vm_write(vm, a, vm->r[rd], 4);
    } else if((h & 0xf000) == 0xa000) {
        vm->r[(h >> 8) & 7] = (h & 0x800 ? vm->r[13] : (pc + 4) & ~3u) + (h & 255) * 4;
    } else if((h & 0xff00) == 0xb000) {
        vm->r[13] += (h & 0x80) ? -(uint32_t)((h & 127) * 4) : (h & 127) * 4;
    } else if((h & 0xf500) == 0xb100) {
        if((vm->r[rd] != 0) == ((h & 0x800) != 0)) vm->r[15] = pc + 4 + ((h >> 3) & 31) * 2 + ((h >> 9) & 1) * 64;
    } else if((h & 0xffc0) == 0xb2c0) vm->r[rd] = vm->r[rn] & 255;
    else if((h & 0xf600) == 0xb400) {
        bool pop = (h & 0x800) != 0;
        stack(vm, (h & 255) | ((h & 0x100) ? (1u << (pop ? 15 : 14)) : 0), pop);
    } else if((h & 0xf000) == 0xd000 && ((h >> 8) & 15) < 14) {
        if(condition(vm, (h >> 8) & 15)) vm->r[15] = pc + 4 + (int8_t)(h & 255) * 2;
    } else if((h & 0xf800) == 0xe000) {
        int32_t delta = h & 0x7ff; if(delta & 0x400) delta -= 0x800;
        vm->r[15] = pc + 4 + delta * 2;
    } else if(h == 0xbf00) { /* NOP */
    } else if((h & 0xff00) == 0xbf00 && (h & 15)) {
        if(in_it || ((h >> 4) & 15) >= 14) goto unsupported;
        vm->itstate = h & 255;
    } else if((h & 0xf800) >= 0xe800) {
        if(!executable(vm, pc, 4)) goto unsupported;
        uint16_t l = (uint16_t)arm_fap_vm_read(vm, pc + 2, 2);
        vm->fault_instruction = ((uint32_t)h << 16) | l;
        vm->r[15] = pc + 4;
        rn = h & 15; rd = (l >> 8) & 15; rm = l & 15;
        if(h == 0xe92d || h == 0xe8bd) stack(vm, l, h == 0xe8bd);
        else if((h & 0xfff0) == 0xfbb0 && (l & 0xf0f0) == 0xf0f0) {
            vm->r[rd] = vm->r[rm] ? vm->r[rn] / vm->r[rm] : 0;
        } else if((h & 0xfff0) == 0xfb90 && (l & 0xf0f0) == 0xf0f0) {
            int32_t numerator = (int32_t)vm->r[rn], denominator = (int32_t)vm->r[rm];
            vm->r[rd] = !denominator ? 0 : numerator == INT32_MIN && denominator == -1 ?
                        (uint32_t)INT32_MIN : (uint32_t)(numerator / denominator);
        } else if((h & 0xfff0) == 0xfb00 && (l & 0x00f0) == 0x10) {
            unsigned ra = l >> 12;
            if(rd == 15 || rn == 15 || rm == 15 || ra == 15) goto unsupported;
            vm->r[rd] = vm->r[ra] - vm->r[rn] * vm->r[rm];
        } else if((h & 0xff7f) == 0xf85f) {
            rd = l >> 12;
            uint32_t base = (pc + 4) & ~3u;
            uint32_t address = h & 0x80 ? base + (l & 4095) : base - (l & 4095);
            if(rd == 15) goto unsupported;
            vm->r[rd] = arm_fap_vm_read(vm, address, 4);
        } else if((h & 0xfff0) == 0xf890 && rn != 15) {
            rd = l >> 12;
            if(rd == 15) goto unsupported;
            vm->r[rd] = arm_fap_vm_read(vm, vm->r[rn] + (l & 4095), 1);
        } else if((h & 0xfff0) == 0xeb00 && !(l & 0x8030)) {
            unsigned shift = ((l >> 12) & 7) * 4 + ((l >> 6) & 3);
            if(rd == 15 || rn == 15 || rm == 15) goto unsupported;
            vm->r[rd] = vm->r[rn] + (vm->r[rm] << shift);
        } else if((h & 0xfbef) == 0xf04f || (h & 0xfbf0) == 0xf1b0 || (h & 0xfbf0) == 0xf010) {
            if(l & 0x8000) goto unsupported;
            uint32_t imm = ((h >> 10) & 1) * 2048 + ((l >> 12) & 7) * 256 + (l & 255);
            uint32_t v;
            if(imm < 1024) {
                v = imm & 255;
                switch(imm >> 8) { case 1: v |= v << 16; break; case 2: v = (v << 8) | (v << 24); break; case 3: v *= 0x01010101; break; default: break; }
            } else {
                unsigned rotate = imm >> 7;
                v = (imm & 127) | 128;
                v = (v >> rotate) | (v << (32 - rotate));
            }
            if((h & 0xfbef) == 0xf04f && !(h & 0x10) && rd != 15) vm->r[rd] = v;
            else if((h & 0xfbf0) == 0xf1b0 && rd == 15) add(vm, vm->r[rn], v, true, true);
            else if((h & 0xfbf0) == 0xf010 && rd == 15 && rn != 15) {
                nz(vm, vm->r[rn] & v);
                if(imm >= 1024) vm->c = (v >> 31) != 0;
            }
            else goto unsupported;
        } else if((h & 0xfff0) == 0xf880) {
            rd = l >> 12;
            arm_fap_vm_write(vm, vm->r[rn] + (l & 4095), vm->r[rd], 1);
        } else if((h & 0xfff0) == 0xe9c0) {
            rd = l >> 12; rm = (l >> 8) & 15;
            uint32_t a = vm->r[rn] + (l & 255) * 4;
            arm_fap_vm_write(vm, a, vm->r[rd], 4);
            arm_fap_vm_write(vm, a + 4, vm->r[rm], 4);
        } else goto unsupported;
    } else goto unsupported;
    /* Thumb's short MOV/shift/add/sub encodings suppress implicit flag writes
     * inside IT blocks. Explicit CMP/TST and wide S encodings still set flags. */
    if(in_it && ((h & 0xe000) == 0 ||
       ((h & 0xe000) == 0x2000 && ((h >> 11) & 3) != 1))) {
        vm->n = old_n; vm->z = old_z; vm->c = old_c; vm->v = old_v;
    }
    return;
unsupported:
    arm_fap_vm_fault(vm, "Unsupported Thumb instruction");
}

bool arm_fap_vm_call(ArmFapVm* vm, uint32_t entry, const uint32_t* args, size_t argc,
                     uint32_t budget, uint32_t* result) {
    uint32_t saved[16]; memcpy(saved, vm->r, sizeof(saved));
    bool n = vm->n, z = vm->z, c = vm->c, v = vm->v;
    uint8_t itstate = vm->itstate;
    if(argc > 4 || !budget || !(entry & 1)) {
        arm_fap_vm_fault(vm, "Invalid guest call"); return false;
    }
    for(size_t i = 0; i < argc; i++) vm->r[i] = args[i];
    vm->itstate = 0;
    vm->r[14] = ARM_FAP_RETURN | 1;
    branch(vm, entry);
    uint32_t remaining = budget, yields = vm->yields;
    while(!vm->error[0] && vm->r[15] != ARM_FAP_RETURN) {
        /* A real blocking wait gives GUI/input time to run. Bound execution
         * between waits, allowing ordinary queue-driven app loops to persist. */
        if(vm->yields != yields) { remaining = budget; yields = vm->yields; }
        if(!remaining--) { arm_fap_vm_fault(vm, "ARM instruction budget exceeded"); break; }
        vm->instructions++;
        uint32_t pc = vm->r[15];
        if(pc >= ARM_FAP_IMPORT_BASE && pc < ARM_FAP_IMPORT_BASE + ArmImportCount * 4 && !(pc & 3)) {
            ArmFapImport id = (ArmFapImport)((pc - ARM_FAP_IMPORT_BASE) / 4);
            bool ok = id >= ArmImport_memcpy && id <= ArmImport_strlcat ? memory_import(vm, id) :
                      vm->import && vm->import(vm, id, vm->context);
            if(!ok) {
                arm_fap_vm_fault(vm, "ARM API bridge failed"); break;
            }
            branch(vm, vm->r[14]);
        } else step(vm);
    }
    if(result) *result = vm->r[0];
    memcpy(vm->r, saved, sizeof(saved));
    vm->n = n; vm->z = z; vm->c = c; vm->v = v;
    vm->itstate = itstate;
    return !vm->error[0];
}
