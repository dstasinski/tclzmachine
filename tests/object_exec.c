/*
 * object_exec.c
 *
 * Instruction-level regression coverage for V3 object/property opcodes.
 * A hand-built V1-V3 object table is exercised through real encoded
 * instructions and the public zmachine_step() path, verifying store/branch
 * continuations as well as the underlying relationship/attribute/property
 * mutations. Direct helper tests live in object.c; this file specifically locks
 * down executor-to-object-subsystem integration.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Allocate the V3 VM layout shared by every encoded instruction below. */
static void init_vm(ZMachine *vm)
{
    memset(vm, 0, sizeof(*vm));
    vm->memory = (uint8_t *)calloc(2048U, 1U);
    assert(vm->memory != NULL);
    vm->memory_size = 2048U;
    vm->version = 3U;
    vm->static_memory_addr = 2048U;
    vm->globals_addr = 0x20U;
    vm->object_table_addr = 0x80U;
    vm->state = ZM_STATE_READY;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
}

/* Release storage owned by init_vm(). */
static void free_vm(ZMachine *vm)
{
    free(vm->memory);
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
}

/* Write one big-endian word into the synthetic story image. */
static void put16(uint8_t *memory, size_t address, uint16_t value)
{
    memory[address] = (uint8_t)(value >> 8);
    memory[address + 1U] = (uint8_t)value;
}

/* Read one global variable directly so opcode store results can be asserted. */
static uint16_t get_global(const ZMachine *vm, uint8_t variable)
{
    size_t address = (size_t)vm->globals_addr +
                     (size_t)(variable - 0x10U) * 2U;
    return (uint16_t)(((uint16_t)vm->memory[address] << 8) |
                      vm->memory[address + 1U]);
}

/* Compute a V1-V3 9-byte object entry following the 31 default words. */
static size_t entry(unsigned object)
{
    return 0x80U + 62U + (object - 1U) * 9U;
}

/* Build one small tree and descending property table used by all opcode cases. */
static void build_objects(ZMachine *vm)
{
    size_t o1 = entry(1U);
    size_t o2 = entry(2U);
    size_t o3 = entry(3U);

    /* Default property 5 is also used by the null-object get_prop regression. */
    put16(vm->memory, 0x80U + 8U, 0xA55AU);

    /* object 1 children: 2 -> 3 */
    vm->memory[o1 + 6U] = 2U;
    vm->memory[o2 + 4U] = 1U;
    vm->memory[o2 + 5U] = 3U;
    vm->memory[o3 + 4U] = 1U;

    /* object 1 property table: prop 10 len 2, prop 5 len 1. */
    put16(vm->memory, o1 + 7U, 0x0400U);
    vm->memory[0x400U] = 0U;
    vm->memory[0x401U] = 0x2AU;
    vm->memory[0x402U] = 0x12U;
    vm->memory[0x403U] = 0x34U;
    vm->memory[0x404U] = 0x05U;
    vm->memory[0x405U] = 0x7EU;
    vm->memory[0x406U] = 0U;
}

int main(void)
{
    ZMachine vm;
    size_t o1, o2;

    init_vm(&vm);
    build_objects(&vm);
    o1 = entry(1U);
    o2 = entry(2U);

    /* get_child 1 -> global 0x10, branch because child is nonzero. */
    vm.pc = 0x300U;
    vm.memory[0x300U] = 0x92U;
    vm.memory[0x301U] = 1U;
    vm.memory[0x302U] = 0x10U;
    vm.memory[0x303U] = 0xC3U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(get_global(&vm, 0x10U) == 2U);
    assert(vm.pc == 0x305U);

    /* get_sibling 2 -> global 0x11, branch because sibling is 3. */
    vm.pc = 0x310U;
    vm.memory[0x310U] = 0x91U;
    vm.memory[0x311U] = 2U;
    vm.memory[0x312U] = 0x11U;
    vm.memory[0x313U] = 0xC3U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(get_global(&vm, 0x11U) == 3U);
    assert(vm.pc == 0x315U);

    /* get_parent 2 -> global 0x12. */
    vm.pc = 0x320U;
    vm.memory[0x320U] = 0x93U;
    vm.memory[0x321U] = 2U;
    vm.memory[0x322U] = 0x12U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(get_global(&vm, 0x12U) == 1U);
    assert(vm.pc == 0x323U);

    /*
     * Old Inform libraries can leave an object variable equal to zero and then
     * issue read-only tree queries through that variable. Global 0x1f is still
     * zero here, so these exercise the resolved-variable path rather than a
     * literal-zero compatibility shortcut.
     */
    vm.pc = 0x324U;
    vm.memory[0x324U] = 0xA3U; /* get_parent variable */
    vm.memory[0x325U] = 0x1FU;
    vm.memory[0x326U] = 0x16U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(get_global(&vm, 0x16U) == 0U);
    assert(vm.pc == 0x327U);

    vm.memory[0x327U] = 0xA1U; /* get_sibling variable -> store, false branch */
    vm.memory[0x328U] = 0x1FU;
    vm.memory[0x329U] = 0x17U;
    vm.memory[0x32AU] = 0xC3U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(get_global(&vm, 0x17U) == 0U);
    assert(vm.pc == 0x32BU);

    vm.memory[0x32BU] = 0xA2U; /* get_child variable -> store, false branch */
    vm.memory[0x32CU] = 0x1FU;
    vm.memory[0x32DU] = 0x18U;
    vm.memory[0x32EU] = 0xC3U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(get_global(&vm, 0x18U) == 0U);
    assert(vm.pc == 0x32FU);

    /* jin 2 1 branches. */
    vm.pc = 0x330U;
    vm.memory[0x330U] = 0x06U;
    vm.memory[0x331U] = 2U;
    vm.memory[0x332U] = 1U;
    vm.memory[0x333U] = 0xC3U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x335U);

    /* set_attr 2 0, test_attr branches, clear_attr removes it. */
    vm.pc = 0x340U;
    vm.memory[0x340U] = 0x0BU;
    vm.memory[0x341U] = 2U;
    vm.memory[0x342U] = 0U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert((vm.memory[o2] & 0x80U) != 0U);

    vm.pc = 0x350U;
    vm.memory[0x350U] = 0x0AU;
    vm.memory[0x351U] = 2U;
    vm.memory[0x352U] = 0U;
    vm.memory[0x353U] = 0xC3U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x355U);

    vm.pc = 0x360U;
    vm.memory[0x360U] = 0x0CU;
    vm.memory[0x361U] = 2U;
    vm.memory[0x362U] = 0U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert((vm.memory[o2] & 0x80U) == 0U);

    /* remove_obj 2 leaves object 3 as object 1's first child. */
    vm.pc = 0x370U;
    vm.memory[0x370U] = 0x99U;
    vm.memory[0x371U] = 2U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.memory[o1 + 6U] == 3U);
    assert(vm.memory[o2 + 4U] == 0U);

    /* insert_obj 2 3 makes 2 the first child of 3. */
    vm.pc = 0x380U;
    vm.memory[0x380U] = 0x0EU;
    vm.memory[0x381U] = 2U;
    vm.memory[0x382U] = 3U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.memory[o2 + 4U] == 3U);

    /* get_prop 1 10 -> global 0x13. */
    vm.pc = 0x390U;
    vm.memory[0x390U] = 0x11U;
    vm.memory[0x391U] = 1U;
    vm.memory[0x392U] = 10U;
    vm.memory[0x393U] = 0x13U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(get_global(&vm, 0x13U) == 0x1234U);

    /*
     * get_prop through a variable whose resolved object value is zero. 0x51 is
     * long-form 2OP:17 with VARIABLE,SMALL operands, matching the old Inform
     * sequence seen in Gostak/Mask. Null has no local property 5, so the V3
     * default-property value is returned.
     */
    vm.pc = 0x394U;
    vm.memory[0x394U] = 0x51U;
    vm.memory[0x395U] = 0x1FU;
    vm.memory[0x396U] = 5U;
    vm.memory[0x397U] = 0x19U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(get_global(&vm, 0x19U) == 0xA55AU);
    assert(vm.pc == 0x398U);

    /* get_prop_addr 1 10 -> global 0x14. */
    vm.pc = 0x3A0U;
    vm.memory[0x3A0U] = 0x12U;
    vm.memory[0x3A1U] = 1U;
    vm.memory[0x3A2U] = 10U;
    vm.memory[0x3A3U] = 0x14U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(get_global(&vm, 0x14U) == 0x402U);

    /* get_next_prop 1 10 -> global 0x15. */
    vm.pc = 0x3B0U;
    vm.memory[0x3B0U] = 0x13U;
    vm.memory[0x3B1U] = 1U;
    vm.memory[0x3B2U] = 10U;
    vm.memory[0x3B3U] = 0x15U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(get_global(&vm, 0x15U) == 5U);

    /* put_prop 1 10 0xbeef. */
    vm.pc = 0x3C0U;
    vm.memory[0x3C0U] = 0xE3U;
    vm.memory[0x3C1U] = 0x53U; /* small, small, large, omitted */
    vm.memory[0x3C2U] = 1U;
    vm.memory[0x3C3U] = 10U;
    vm.memory[0x3C4U] = 0xBEU;
    vm.memory[0x3C5U] = 0xEFU;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.memory[0x402U] == 0xBEU && vm.memory[0x403U] == 0xEFU);

    free_vm(&vm);
    puts("object opcode execution tests passed");
    return 0;
}
