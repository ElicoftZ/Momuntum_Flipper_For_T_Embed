/* Host harness includes the actual portable core, exposing single-step only
 * here so instruction semantics can be compared with an independent emulator. */
#include "../../components/flipper_application/arm_fap/arm_fap_vm.c"
#include "../../components/rpc/rpc_fap_compat.h"
#include <stdlib.h>
#define EXPORT __declspec(dllexport)
EXPORT const char* armtest_rpc_value(const char* key, const char* value) {
    return rpc_fap_compat_value(key, value);
}
EXPORT ArmFapVm* armtest_create(const uint8_t* f, size_t n, ArmFapImportCallback cb) {
    ArmFapVm* vm = calloc(1, sizeof(*vm));
    vm->ram = calloc(1, ARM_FAP_RAM_SIZE);
    vm->import = cb;
    arm_fap_vm_load(vm, f, n);
    return vm;
}
EXPORT void armtest_destroy(ArmFapVm* vm) { free(vm->ram); free(vm); }
EXPORT const char* armtest_error(ArmFapVm* vm) { return vm->error; }
EXPORT uint32_t armtest_entry(ArmFapVm* vm) { return vm->entry; }
EXPORT void armtest_yield(ArmFapVm* vm) { vm->yields++; }
EXPORT uint32_t armtest_get(ArmFapVm* vm, unsigned r) { return vm->r[r]; }
EXPORT void armtest_set(ArmFapVm* vm, unsigned r, uint32_t v) { vm->r[r] = v; }
EXPORT uint8_t* armtest_ram(ArmFapVm* vm) { return vm->ram; }
EXPORT uint32_t armtest_alloc(ArmFapVm* vm, size_t n) { return arm_fap_vm_alloc(vm, n); }
EXPORT bool armtest_free(ArmFapVm* vm, uint32_t a) { return arm_fap_vm_free(vm, a); }
EXPORT bool armtest_call(ArmFapVm* vm, uint32_t pc, const uint32_t* a, size_t n, uint32_t budget, uint32_t* result) {
    return arm_fap_vm_call(vm, pc, a, n, budget, result);
}
EXPORT uint32_t armtest_flags(ArmFapVm* vm) { return ((uint32_t)vm->n << 31) | ((uint32_t)vm->z << 30) | ((uint32_t)vm->c << 29) | ((uint32_t)vm->v << 28); }
EXPORT void armtest_set_flags(ArmFapVm* vm, uint32_t f) { vm->n = (f >> 31) & 1; vm->z = (f >> 30) & 1; vm->c = (f >> 29) & 1; vm->v = (f >> 28) & 1; }
EXPORT void armtest_step(ArmFapVm* vm) { vm->error[0] = 0; step(vm); }
EXPORT uint32_t armtest_fault_pc(ArmFapVm* vm) { return vm->fault_pc; }
EXPORT uint32_t armtest_fault_instruction(ArmFapVm* vm) { return vm->fault_instruction; }
EXPORT void* armtest_pointer(ArmFapVm* vm, uint32_t a, size_t n, bool w) { return arm_fap_vm_pointer(vm, a, n, w); }
