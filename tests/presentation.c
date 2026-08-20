/*
 * presentation.c
 *
 * Regression tests for presentation-only Z-machine opcodes which tclzmachine
 * intentionally reduces to no-ops in its stream-oriented text frontend, plus
 * the text-only read_char policy used for non-interactive IRC sessions.
 * These tests verify instruction advancement, operand-evaluation side effects,
 * and deterministic single-character input behavior.
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
    vm->globals_addr = 0x100U;
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

/* Read one global variable directly from the synthetic story image. */
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

    init_vm(&vm, 5U, 512U);

    /* VAR:10 split_window is layout-only and disappears in text-only mode. */
    vm.pc = 0x20U;
    vm.memory[0x20] = 0xEAU;
    vm.memory[0x21] = 0x7FU;
    vm.memory[0x22] = 2U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x23U && vm.state == ZM_STATE_READY);

    /* VAR:13 erase_window small-constant operand. */
    vm.memory[0x23] = 0xEDU;
    vm.memory[0x24] = 0x7FU;
    vm.memory[0x25] = 1U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x26U && vm.state == ZM_STATE_READY);

    /* VAR:17 set_text_style small-constant operand. */
    vm.memory[0x26] = 0xF1U;
    vm.memory[0x27] = 0x7FU;
    vm.memory[0x28] = 2U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x29U && vm.state == ZM_STATE_READY);

    /* VAR:18 buffer_mode small-constant operand. */
    vm.memory[0x29] = 0xF2U;
    vm.memory[0x2A] = 0x7FU;
    vm.memory[0x2B] = 1U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x2CU && vm.state == ZM_STATE_READY);

    /*
     * A no-op still evaluates variable operands. Variable 0 is the stack, so
     * this erase_window encoding must pop one value before advancing the PC.
     */
    assert(zmachine_stack_push(&vm, 0xFFFFU) == TCL_OK);
    vm.memory[0x2C] = 0xEDU;
    vm.memory[0x2D] = 0xBFU;
    vm.memory[0x2E] = 0U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x2FU && vm.sp == 0U);

    /*
     * VAR:22 read_char stores ZSCII carriage return in text-only mode. The
     * opcode's first operand is the required input-device selector value 1.
     */
    vm.memory[0x2F] = 0xF6U;
    vm.memory[0x30] = 0x7FU;
    vm.memory[0x31] = 1U;
    vm.memory[0x32] = 0x10U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x33U && read_global(&vm, 0x10U) == 13U);

    free_vm(&vm);
    puts("presentation opcode tests passed");
    return 0;
}
