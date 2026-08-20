/*
 * presentation.c
 *
 * Regression tests for presentation-only Z-machine opcodes which tclzmachine
 * intentionally reduces to no-ops in its stream-oriented text frontend.
 * These tests verify both instruction advancement and operand-evaluation side
 * effects, so ignoring visual behavior never changes ordinary VM semantics.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Build a minimal in-memory VM suitable for isolated opcode execution. */
static void init_vm(ZMachine *vm, uint8_t version, size_t size)
{
    memset(vm, 0, sizeof(*vm));
    vm->memory = (uint8_t *)calloc(size, 1U);
    assert(vm->memory != NULL);
    vm->memory_size = size;
    vm->version = version;
    vm->static_memory_addr = (uint16_t)size;
    vm->state = ZM_STATE_READY;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
}

/* Release resources initialized by init_vm. */
static void free_vm(ZMachine *vm)
{
    free(vm->memory);
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
}

int main(void)
{
    ZMachine vm;

    init_vm(&vm, 5U, 512U);

    /* VAR:13 erase_window small-constant operand. */
    vm.pc = 0x20U;
    vm.memory[0x20] = 0xEDU;
    vm.memory[0x21] = 0x7FU; /* small constant, then omitted operands */
    vm.memory[0x22] = 1U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x23U && vm.state == ZM_STATE_READY);

    /* VAR:17 set_text_style small-constant operand. */
    vm.memory[0x23] = 0xF1U;
    vm.memory[0x24] = 0x7FU;
    vm.memory[0x25] = 2U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x26U && vm.state == ZM_STATE_READY);

    /* VAR:18 buffer_mode small-constant operand. */
    vm.memory[0x26] = 0xF2U;
    vm.memory[0x27] = 0x7FU;
    vm.memory[0x28] = 1U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x29U && vm.state == ZM_STATE_READY);

    /*
     * A no-op still evaluates variable operands.  Variable 0 is the stack, so
     * this erase_window encoding must pop one value before advancing the PC.
     */
    assert(zmachine_stack_push(&vm, 0xFFFFU) == TCL_OK);
    vm.memory[0x29] = 0xEDU;
    vm.memory[0x2A] = 0xBFU; /* variable operand, then omitted operands */
    vm.memory[0x2B] = 0U;    /* variable 0: evaluation stack */
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x2CU && vm.sp == 0U);

    free_vm(&vm);
    puts("presentation opcode tests passed");
    return 0;
}
