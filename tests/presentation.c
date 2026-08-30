/*
 * presentation.c
 *
 * Regression tests for presentation-oriented Z-machine behavior adapted to
 * the stream-oriented Tcl/IRC frontend. These tests cover layout no-ops,
 * cooperative character input, output-stream bookkeeping, print_table
 * conversion to plain text, indirect-variable semantics, and narrow legacy
 * compatibility cases handled by the presentation dispatcher.
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

    vm.pc = 0x20U;
    vm.memory[0x20] = 0xEAU;
    vm.memory[0x21] = 0x7FU;
    vm.memory[0x22] = 2U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x23U && vm.state == ZM_STATE_READY);

    vm.memory[0x23] = 0xEBU;
    vm.memory[0x24] = 0x7FU;
    vm.memory[0x25] = 0U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x26U && vm.state == ZM_STATE_READY);

    vm.memory[0x26] = 0xEDU;
    vm.memory[0x27] = 0x7FU;
    vm.memory[0x28] = 1U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x29U && vm.state == ZM_STATE_READY);

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
    vm.memory[0x86] = (uint8_t)'B';
    vm.memory[0x4A] = 0xFEU;
    vm.memory[0x4B] = 0x1FU;
    vm.memory[0x4C] = 0x00U;
    vm.memory[0x4D] = 0x84U;
    vm.memory[0x4E] = 3U;
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
     * VAR:9 pull with operand variable 0 is an indirect-variable reference.
     * It must pop exactly once and then replace the new top stack value; the
     * operand itself must not be dereferenced by the generic resolver first.
     */
    assert(zmachine_stack_push(&vm, 0x1111U) == TCL_OK);
    assert(zmachine_stack_push(&vm, 0x2222U) == TCL_OK);
    vm.memory[0x53] = 0xE9U;
    vm.memory[0x54] = 0xBFU;
    vm.memory[0x55] = 0U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x56U);
    assert(vm.sp == 1U);
    assert(vm.stack[0] == 0x2222U);

    free_vm(&vm);
    puts("presentation opcode tests passed");
    return 0;
}
