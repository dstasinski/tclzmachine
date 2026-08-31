/*
 * presentation_extended.c
 *
 * Standards-oriented regression coverage for text-only presentation behavior
 * which still has observable VM effects. The runtime does not emulate a real
 * screen, fonts, or colours, but stories can depend on selected-window state,
 * get_cursor writes, set_font/check_unicode return values, Unicode stream-3
 * conversion, and output-stream bookkeeping.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void init_vm(ZMachine *vm)
{
    memset(vm, 0, sizeof(*vm));
    vm->memory = (uint8_t *)calloc(1024U, 1U);
    assert(vm->memory != NULL);
    vm->memory_size = 1024U;
    vm->version = 5U;
    vm->static_memory_addr = 0x300U;
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

static uint16_t read_word(const ZMachine *vm, size_t address)
{
    return (uint16_t)(((uint16_t)vm->memory[address] << 8) |
                      vm->memory[address + 1U]);
}

static uint16_t read_global(const ZMachine *vm, uint8_t variable)
{
    size_t address = (size_t)vm->globals_addr +
                     (size_t)(variable - 0x10U) * 2U;
    return read_word(vm, address);
}

int main(void)
{
    ZMachine vm;

    init_vm(&vm);
    vm.pc = 0x20U;

    /* VAR:11 set_window 1 selects the presentation-only upper window. */
    vm.memory[0x20U] = 0xEBU;
    vm.memory[0x21U] = 0x7FU;
    vm.memory[0x22U] = 1U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x23U && vm.current_window == 1U);
    zmachine_output_append(&vm, "upper", 5U);
    assert(Tcl_DStringLength(&vm.output) == 0);

    /* Returning to lower window 0 resumes canonical Tcl-facing output. */
    vm.memory[0x23U] = 0xEBU;
    vm.memory[0x24U] = 0x7FU;
    vm.memory[0x25U] = 0U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x26U && vm.current_window == 0U);
    zmachine_output_append(&vm, "lower", 5U);
    assert(strcmp(Tcl_DStringValue(&vm.output), "lower") == 0);

    /* VAR:14 erase_line has no textual effect but remains a valid V5 opcode. */
    vm.memory[0x26U] = 0xEEU;
    vm.memory[0x27U] = 0x7FU;
    vm.memory[0x28U] = 1U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x29U);

    /* VAR:16 get_cursor writes the deterministic virtual cursor position. */
    vm.memory[0x29U] = 0xF0U;
    vm.memory[0x2AU] = 0x3FU;
    vm.memory[0x2BU] = 0x01U;
    vm.memory[0x2CU] = 0x80U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x2DU);
    assert(read_word(&vm, 0x180U) == 1U);
    assert(read_word(&vm, 0x182U) == 1U);

    /* 2OP:27 set_colour is presentation-only but must decode/evaluate normally. */
    vm.memory[0x2DU] = 0x1BU;
    vm.memory[0x2EU] = 2U;
    vm.memory[0x2FU] = 9U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x30U);

    /* EXT:4 set_font advertises only normal font 1 in the text-only frontend. */
    vm.memory[0x30U] = 0xBEU;
    vm.memory[0x31U] = 4U;
    vm.memory[0x32U] = 0x7FU;
    vm.memory[0x33U] = 1U;
    vm.memory[0x34U] = 0x10U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x35U && read_global(&vm, 0x10U) == 1U);

    vm.memory[0x35U] = 0xBEU;
    vm.memory[0x36U] = 4U;
    vm.memory[0x37U] = 0x7FU;
    vm.memory[0x38U] = 4U;
    vm.memory[0x39U] = 0x11U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x3AU && read_global(&vm, 0x11U) == 0U);

    /* EXT:11 print_unicode emits arbitrary printable BMP Unicode as UTF-8. */
    Tcl_DStringSetLength(&vm.output, 0);
    vm.memory[0x3AU] = 0xBEU;
    vm.memory[0x3BU] = 11U;
    vm.memory[0x3CU] = 0x3FU;
    vm.memory[0x3DU] = 0x20U;
    vm.memory[0x3EU] = 0xACU; /* U+20AC EURO SIGN */
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x3FU);
    assert(strcmp(Tcl_DStringValue(&vm.output), "\xE2\x82\xAC") == 0);

    /* EXT:12 reports printable Unicode output and ASCII keyboard input. */
    vm.memory[0x3FU] = 0xBEU;
    vm.memory[0x40U] = 12U;
    vm.memory[0x41U] = 0x3FU;
    vm.memory[0x42U] = 0x20U;
    vm.memory[0x43U] = 0xACU;
    vm.memory[0x44U] = 0x12U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x45U && read_global(&vm, 0x12U) == 1U);

    vm.memory[0x45U] = 0xBEU;
    vm.memory[0x46U] = 12U;
    vm.memory[0x47U] = 0x3FU;
    vm.memory[0x48U] = 0x00U;
    vm.memory[0x49U] = 0x41U; /* U+0041 'A' */
    vm.memory[0x4AU] = 0x13U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x4BU && read_global(&vm, 0x13U) == 3U);

    /* EXT:13 set_true_colour is a text-only no-op after operand evaluation. */
    vm.memory[0x4BU] = 0xBEU;
    vm.memory[0x4CU] = 13U;
    vm.memory[0x4DU] = 0x0FU;
    vm.memory[0x4EU] = 0xFFU;
    vm.memory[0x4FU] = 0xFFU;
    vm.memory[0x50U] = 0x00U;
    vm.memory[0x51U] = 0x00U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x52U);

    /* output_stream 2 must mirror its selected state into Flags 2 bit 0. */
    vm.memory[0x52U] = 0xF3U;
    vm.memory[0x53U] = 0x7FU;
    vm.memory[0x54U] = 2U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x55U);
    assert((vm.flags2 & 1U) != 0U && (vm.memory[0x11U] & 1U) != 0U);

    vm.memory[0x55U] = 0xF3U;
    vm.memory[0x56U] = 0x3FU;
    vm.memory[0x57U] = 0xFFU;
    vm.memory[0x58U] = 0xFEU; /* -2 */
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x59U);
    assert((vm.flags2 & 1U) == 0U && (vm.memory[0x11U] & 1U) == 0U);

    /*
     * print_unicode to stream 3 reverse-maps through the active ZSCII table.
     * U+00E4 exists in the default table as ZSCII 155; U+20AC does not and
     * therefore becomes '?' as required for non-ZSCII stream-3 output.
     */
    vm.memory[0x59U] = 0xF3U;
    vm.memory[0x5AU] = 0x4FU;
    vm.memory[0x5BU] = 3U;
    vm.memory[0x5CU] = 0x01U;
    vm.memory[0x5DU] = 0xA0U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x5EU && vm.stream3_depth == 1U);

    vm.memory[0x5EU] = 0xBEU;
    vm.memory[0x5FU] = 11U;
    vm.memory[0x60U] = 0x3FU;
    vm.memory[0x61U] = 0x00U;
    vm.memory[0x62U] = 0xE4U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x63U);

    vm.memory[0x63U] = 0xBEU;
    vm.memory[0x64U] = 11U;
    vm.memory[0x65U] = 0x3FU;
    vm.memory[0x66U] = 0x20U;
    vm.memory[0x67U] = 0xACU;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x68U);

    vm.memory[0x68U] = 0xF3U;
    vm.memory[0x69U] = 0x3FU;
    vm.memory[0x6AU] = 0xFFU;
    vm.memory[0x6BU] = 0xFDU; /* -3 */
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x6CU && vm.stream3_depth == 0U);
    assert(read_word(&vm, 0x1A0U) == 2U);
    assert(vm.memory[0x1A2U] == 155U);
    assert(vm.memory[0x1A3U] == (uint8_t)'?');
    assert(strcmp(Tcl_DStringValue(&vm.output), "\xE2\x82\xAC") == 0);

    free_vm(&vm);

    /*
     * A malformed get_cursor destination must be rejected before either word
     * is written. This catches partial mutation at the dynamic/static boundary.
     */
    init_vm(&vm);
    vm.pc = 0x20U;
    vm.memory[0x20U] = 0xF0U;
    vm.memory[0x21U] = 0x3FU;
    vm.memory[0x22U] = 0x02U;
    vm.memory[0x23U] = 0xFEU;
    vm.memory[0x2FEU] = 0xAAU;
    vm.memory[0x2FFU] = 0xBBU;
    assert(zmachine_step(&vm) == TCL_ERROR);
    assert(vm.state == ZM_STATE_ERROR);
    assert(strstr(vm.error, "dynamic memory") != NULL);
    assert(vm.memory[0x2FEU] == 0xAAU && vm.memory[0x2FFU] == 0xBBU);
    free_vm(&vm);

    puts("extended presentation opcode tests passed");
    return 0;
}
