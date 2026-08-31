/*
 * presentation.c
 *
 * Regression tests for presentation-oriented Z-machine behavior adapted to
 * the stream-oriented Tcl/IRC frontend. These tests cover layout no-ops,
 * cooperative character input, output-stream bookkeeping and routing,
 * print_table conversion to plain text, indirect-variable semantics, and
 * narrow legacy compatibility cases handled by the presentation dispatcher.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"
#include "zmachine_undo.h"

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
    zmachine_undo_discard(vm);
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

    vm.pc = 0x20U;
    vm.memory[0x20] = 0xEAU;
    vm.memory[0x21] = 0x7FU;
    vm.memory[0x22] = 2U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x23U && vm.state == ZM_STATE_READY);

    /*
     * Select the upper window, where canonical IRC output is deliberately
     * discarded. erase_window -1 must then select window 0 as required by the
     * Standard; otherwise all later narrative text would remain invisible.
     */
    vm.memory[0x23] = 0xEBU;
    vm.memory[0x24] = 0x7FU;
    vm.memory[0x25] = 1U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x26U && vm.state == ZM_STATE_READY);
    assert(vm.current_window == 1U);
    zmachine_output_append(&vm, "hidden", 6U);
    assert(Tcl_DStringLength(&vm.output) == 0);

    vm.memory[0x100] = 0xFFU;
    vm.memory[0x101] = 0xFFU; /* global 16 = -1 */
    vm.memory[0x26] = 0xEDU;
    vm.memory[0x27] = 0xBFU; /* variable operand */
    vm.memory[0x28] = 0x10U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x29U && vm.state == ZM_STATE_READY);
    assert(vm.current_window == 0U);
    zmachine_output_append(&vm, "V", 1U);
    assert(strcmp(Tcl_DStringValue(&vm.output), "V") == 0);
    Tcl_DStringSetLength(&vm.output, 0);

    vm.memory[0x29] = 0xEFU;
    vm.memory[0x2A] = 0x5FU;
    vm.memory[0x2B] = 1U;
    vm.memory[0x2C] = 1U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x2DU && vm.state == ZM_STATE_READY);

    vm.memory[0x2D] = 0xF1U;
    vm.memory[0x2E] = 0x7FU;
    vm.memory[0x2F] = 2U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x30U && vm.state == ZM_STATE_READY);

    vm.memory[0x30] = 0xF2U;
    vm.memory[0x31] = 0x7FU;
    vm.memory[0x32] = 1U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x33U && vm.state == ZM_STATE_READY);

    assert(zmachine_stack_push(&vm, 0xFFFFU) == TCL_OK);
    vm.memory[0x33] = 0xEDU;
    vm.memory[0x34] = 0xBFU;
    vm.memory[0x35] = 0U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x36U && vm.sp == 0U);

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

    vm.memory[0x3A] = 0xF3U;
    vm.memory[0x3B] = 0x4FU;
    vm.memory[0x3C] = 3U;
    vm.memory[0x3D] = 0x00U;
    vm.memory[0x3E] = 0x90U;
    vm.memory[0x90] = 0xAAU;
    vm.memory[0x91] = 0xBBU;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x3FU && vm.stream3_depth == 1U);
    assert(vm.stream3_tables[0] == 0x90U);
    assert(vm.memory[0x90] == 0U && vm.memory[0x91] == 0U);

    vm.memory[0x3F] = 0xF3U;
    vm.memory[0x40] = 0x3FU;
    vm.memory[0x41] = 0xFFU;
    vm.memory[0x42] = 0xFDU;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x43U && vm.stream3_depth == 0U);

    memcpy(vm.memory + 0x80U, "ABCD", 4U);
    vm.memory[0x43] = 0xFEU;
    vm.memory[0x44] = 0x15U;
    vm.memory[0x45] = 0x00U;
    vm.memory[0x46] = 0x80U;
    vm.memory[0x47] = 2U;
    vm.memory[0x48] = 2U;
    vm.memory[0x49] = 0U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x4AU);
    assert(strcmp(Tcl_DStringValue(&vm.output), "AB\nCD") == 0);

    Tcl_DStringSetLength(&vm.output, 0);
    vm.memory[0x84] = (uint8_t)'A';
    vm.memory[0x85] = 11U;
    vm.memory[0x86] = 137U;
    vm.memory[0x87] = 253U;
    vm.memory[0x88] = (uint8_t)'B';
    vm.memory[0x4A] = 0xFEU;
    vm.memory[0x4B] = 0x1FU;
    vm.memory[0x4C] = 0x00U;
    vm.memory[0x4D] = 0x84U;
    vm.memory[0x4E] = 5U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x4FU);
    assert(strcmp(Tcl_DStringValue(&vm.output), "AB") == 0);

    vm.memory[0x4F] = 0x92U;
    vm.memory[0x50] = 0U;
    vm.memory[0x51] = 0x11U;
    vm.memory[0x52] = 0xC2U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x53U);
    assert(read_global(&vm, 0x11U) == 0U);

    /*
     * VAR:9 pull directly targeting stack variable 0 is encoded with a small
     * constant 0. The pull removes one value, then the indirect write replaces
     * the new top rather than pushing or popping stack variable 0 again.
     */
    assert(zmachine_stack_push(&vm, 0x1111U) == TCL_OK);
    assert(zmachine_stack_push(&vm, 0x2222U) == TCL_OK);
    vm.memory[0x53] = 0xE9U;
    vm.memory[0x54] = 0x7FU;
    vm.memory[0x55] = 0U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x56U);
    assert(vm.sp == 1U);
    assert(vm.stack[0] == 0x2222U);

    /*
     * EXT:9 is save_undo, not VAR:9 pull. A successful save stores 1 and must
     * leave the evaluation stack unchanged merely because the opcode shares
     * number 9 with pull in a different opcode table.
     */
    vm.memory[0x56] = 0xBEU;
    vm.memory[0x57] = 9U;
    vm.memory[0x58] = 0xFFU; /* no operands */
    vm.memory[0x59] = 0x12U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x5AU);
    assert(vm.sp == 1U && vm.stack[0] == 0x2222U);
    assert(read_global(&vm, 0x12U) == 1U);
    assert(vm.undo_state != NULL);

    free_vm(&vm);

    /*
     * Stream 3 captures ZSCII in dynamic memory and suppresses stream 1 while
     * active. Nested captures must resume the previous table when the inner
     * stream closes, and explicit stream-1 selection remains independent.
     */
    init_vm(&vm, 5U, 512U);
    vm.pc = 0x20U;

    vm.memory[0x20] = 0xF3U;
    vm.memory[0x21] = 0x4FU;
    vm.memory[0x22] = 3U;
    vm.memory[0x23] = 0x00U;
    vm.memory[0x24] = 0x80U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x25U && vm.stream3_depth == 1U);
    zmachine_output_append(&vm, "A", 1U);

    vm.memory[0x25] = 0xF3U;
    vm.memory[0x26] = 0x4FU;
    vm.memory[0x27] = 3U;
    vm.memory[0x28] = 0x00U;
    vm.memory[0x29] = 0x90U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x2AU && vm.stream3_depth == 2U);
    zmachine_output_append(&vm, "B\n", 2U);

    vm.memory[0x2A] = 0xF3U;
    vm.memory[0x2B] = 0x3FU;
    vm.memory[0x2C] = 0xFFU;
    vm.memory[0x2D] = 0xFDU;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x2EU && vm.stream3_depth == 1U);
    zmachine_output_append(&vm, "C", 1U);

    vm.memory[0x2E] = 0xF3U;
    vm.memory[0x2F] = 0x3FU;
    vm.memory[0x30] = 0xFFU;
    vm.memory[0x31] = 0xFDU;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x32U && vm.stream3_depth == 0U);

    assert(Tcl_DStringLength(&vm.output) == 0);
    assert(vm.memory[0x80] == 0U && vm.memory[0x81] == 2U);
    assert(vm.memory[0x82] == (uint8_t)'A');
    assert(vm.memory[0x83] == (uint8_t)'C');
    assert(vm.memory[0x90] == 0U && vm.memory[0x91] == 2U);
    assert(vm.memory[0x92] == (uint8_t)'B');
    assert(vm.memory[0x93] == 13U);

    zmachine_output_append(&vm, "D\n", 2U);
    assert(strcmp(Tcl_DStringValue(&vm.output), "D\n") == 0);

    vm.memory[0x32] = 0xF3U;
    vm.memory[0x33] = 0x3FU;
    vm.memory[0x34] = 0xFFU;
    vm.memory[0x35] = 0xFFU;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x36U && vm.output_stream1_enabled == 0);
    zmachine_output_append(&vm, "E", 1U);
    assert(strcmp(Tcl_DStringValue(&vm.output), "D\n") == 0);

    vm.memory[0x36] = 0xF3U;
    vm.memory[0x37] = 0x7FU;
    vm.memory[0x38] = 1U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x39U && vm.output_stream1_enabled == 1);
    zmachine_output_append(&vm, "F", 1U);
    assert(strcmp(Tcl_DStringValue(&vm.output), "D\nF") == 0);

    vm.output_stream1_enabled = 0;
    vm.stream3_depth = 1U;
    vm.stream3_tables[0] = 0x80U;
    assert(zmachine_reset(&vm) == TCL_OK);
    assert(vm.output_stream1_enabled == 1);
    assert(vm.stream3_depth == 0U);
    assert(Tcl_DStringLength(&vm.output) == 0);

    free_vm(&vm);
    puts("presentation opcode tests passed");
    return 0;
}
