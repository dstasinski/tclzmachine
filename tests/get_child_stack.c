/*
 * get_child_stack.c
 *
 * Regression for operand side effects around the text-only dispatcher.
 *
 * A VARIABLE operand naming variable 0 pops the evaluation stack exactly once.
 * The dispatcher has a narrow compatibility case for literal `get_child 0`;
 * it must not speculatively resolve a non-literal operand and then delegate the
 * instruction to the core, because that would pop variable 0 twice.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"
#include "zmachine_undo.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t read_global(const ZMachine *vm, uint8_t variable)
{
    size_t address = (size_t)vm->globals_addr +
                     (size_t)(variable - 0x10U) * 2U;
    return (uint16_t)(((uint16_t)vm->memory[address] << 8) |
                      vm->memory[address + 1U]);
}

int main(void)
{
    ZMachine vm;

    memset(&vm, 0, sizeof(vm));
    vm.memory_size = 512U;
    vm.memory = (uint8_t *)calloc(vm.memory_size, 1U);
    assert(vm.memory != NULL);
    vm.version = 5U;
    vm.static_memory_addr = (uint16_t)vm.memory_size;
    vm.globals_addr = 0x100U;
    vm.object_table_addr = 0x120U;
    vm.state = ZM_STATE_READY;
    vm.output_stream1_enabled = 1;
    Tcl_DStringInit(&vm.output);
    Tcl_DStringInit(&vm.pending_input);

    /*
     * V4+ object #2 begins at object_table + 126 + 14. Its child word is at
     * offset 10 and remains zero in calloc'd memory, so get_child 2 stores 0
     * and follows its false branch.
     */

    /* VAR:8 push 2. */
    vm.pc = 0x20U;
    vm.memory[0x20U] = 0xE8U;
    vm.memory[0x21U] = 0x7FU;
    vm.memory[0x22U] = 2U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x23U);
    assert(vm.sp == 1U && vm.stack[0] == 2U);

    /*
     * 1OP:2 get_child with a VARIABLE operand naming variable 0. The operand
     * must consume the pushed object number once, not once in the dispatcher
     * and again in the core. Store the child in global 0x10; branch-on-true
     * falls through because object #2 has no child.
     */
    vm.memory[0x23U] = 0xA2U;
    vm.memory[0x24U] = 0U;
    vm.memory[0x25U] = 0x10U;
    vm.memory[0x26U] = 0xC2U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x27U);
    assert(vm.sp == 0U);
    assert(read_global(&vm, 0x10U) == 0U);

    zmachine_undo_discard(&vm);
    free(vm.memory);
    Tcl_DStringFree(&vm.output);
    Tcl_DStringFree(&vm.pending_input);

    puts("get_child stack operand regression passed");
    return 0;
}
