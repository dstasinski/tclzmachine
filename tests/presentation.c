/*
 * presentation.c
 *
 * Regression tests for presentation-oriented Z-machine behavior adapted to
 * the stream-oriented Tcl/IRC frontend. These tests cover layout no-ops,
 * cooperative character input, output-stream bookkeeping, and print_table
 * conversion to plain text.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    vm->output_stream1_enabled = 1;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
}

static void free_vm(ZMachine *vm)
{
    free(vm->memory);
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
}

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

    /* VAR:11 set_window is likewise only a presentation selection. */
    vm.memory[0x23] = 0xEBU;
    vm.memory[0x24] = 0x7FU;
    vm.memory[0x25] = 0U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x26U && vm.state == ZM_STATE_READY);

    /* VAR:13 erase_window small-constant operand. */
    vm.memory[0x26] = 0xEDU;
    vm.memory[0x27] = 0x7FU;
    vm.memory[0x28] = 1U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x29U && vm.state == ZM_STATE_READY);

    /* VAR:15 set_cursor has two coordinates but no textual effect. */
    vm.memory[0x29] = 0xEFU;
    vm.memory[0x2A] = 0x5FU;
    vm.memory[0x2B] = 1U;
    vm.memory[0x2C] = 1U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x2DU && vm.state == ZM_STATE_READY);

    /* VAR:17 set_text_style small-constant operand. */
    vm.memory[0x2D] = 0xF1U;
    vm.memory[0x2E] = 0x7FU;
    vm.memory[0x2F] = 2U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x30U && vm.state == ZM_STATE_READY);

    /* VAR:18 buffer_mode small-constant operand. */
    vm.memory[0x30] = 0xF2U;
    vm.memory[0x31] = 0x7FU;
    vm.memory[0x32] = 1U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x33U && vm.state == ZM_STATE_READY);

    /* A discarded opcode must still evaluate stack-variable operands. */
    assert(zmachine_stack_push(&vm, 0xFFFFU) == TCL_OK);
    vm.memory[0x33] = 0xEDU;
    vm.memory[0x34] = 0xBFU;
    vm.memory[0x35] = 0U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x36U && vm.sp == 0U);

    /* read_char suspends, then consumes one explicit Tcl input item. */
    vm.memory[0x36] = 0xF6U;
    vm.memory[0x37] = 0x7FU;
    vm.memory[0x38] = 1U;
    vm.memory[0x39] = 0x10U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x36U && vm.state == ZM_STATE_WAITING_INPUT);
    assert(zmachine_supply_input(&vm, "x") == TCL_OK);
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x3AU && read_global(&vm, 0x10U) == (uint16_t)'x');
    assert(vm.input_available == 0);

    /* output_stream 3 records its destination table and initializes length. */
    vm.memory[0x3A] = 0xF3U;
    vm.memory[0x3B] = 0x4FU; /* small stream number, large table address */
    vm.memory[0x3C] = 3U;
    vm.memory[0x3D] = 0x00U;
    vm.memory[0x3E] = 0x90U;
    vm.memory[0x90] = 0xAAU;
    vm.memory[0x91] = 0xBBU;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x3FU && vm.stream3_depth == 1U);
    assert(vm.stream3_tables[0] == 0x90U);
    assert(vm.memory[0x90] == 0U && vm.memory[0x91] == 0U);

    /* output_stream -3 closes the most recent memory stream. */
    vm.memory[0x3F] = 0xF3U;
    vm.memory[0x40] = 0x3FU; /* one large constant */
    vm.memory[0x41] = 0xFFU;
    vm.memory[0x42] = 0xFDU;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x43U && vm.stream3_depth == 0U);

    /* print_table emits rows as plain text separated by newlines. */
    memcpy(vm.memory + 0x80U, "ABCD", 4U);
    vm.memory[0x43] = 0xFEU;
    vm.memory[0x44] = 0x15U; /* large address, then three small operands */
    vm.memory[0x45] = 0x00U;
    vm.memory[0x46] = 0x80U;
    vm.memory[0x47] = 2U; /* width */
    vm.memory[0x48] = 2U; /* height */
    vm.memory[0x49] = 0U; /* skip */
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x4AU);
    assert(strcmp(Tcl_DStringValue(&vm.output), "AB\nCD") == 0);

    free_vm(&vm);
    puts("presentation opcode tests passed");
    return 0;
}
