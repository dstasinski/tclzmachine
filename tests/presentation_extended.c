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
#include "zmachine_stream.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRANSCRIPT_TEST_PATH "tclzmachine-presentation-transcript.tmp"

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
    /* Public reset releases lazily opened transcript/replay/record streams. */
    if (vm->stream_io)
        assert(zmachine_reset(vm) == TCL_OK);
    free(vm->memory);
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
    remove(TRANSCRIPT_TEST_PATH);
}

static uint16_t read_word(const ZMachine *vm, size_t address)
{
    return (uint16_t)(((uint16_t)vm->memory[address] << 8) |
                      vm->memory[address + 1U]);
}

static uint16_t read_global(const ZMachine *vm, uint8_t variable)
{
    size_t address = vm->globals_addr + (size_t)(variable - 16U) * 2U;
    return read_word(vm, address);
}

int main(void)
{
    ZMachine vm;

    init_vm(&vm);

    /* set_window 1 suppresses upper-window text from the canonical Tcl reply. */
    vm.pc = 0x20U;
    vm.memory[0x20U] = 0xEBU;
    vm.memory[0x21U] = 0x7FU;
    vm.memory[0x22U] = 1U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x23U && vm.current_window == 1U);
    zmachine_output_append(&vm, "hidden", 6U);
    assert(Tcl_DStringLength(&vm.output) == 0);

    /* erase_window -1 must select window 0 even in the text-only model. */
    vm.memory[0x23U] = 0xEDU;
    vm.memory[0x24U] = 0x3FU;
    vm.memory[0x25U] = 0xFFU;
    vm.memory[0x26U] = 0xFFU;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x27U && vm.current_window == 0U);
    zmachine_output_append(&vm, "shown", 5U);
    assert(strcmp(Tcl_DStringValue(&vm.output), "shown") == 0);
    Tcl_DStringSetLength(&vm.output, 0);

    /* erase_line has no textual state but must consume/evaluate its operand. */
    vm.memory[0x27U] = 0xEEU;
    vm.memory[0x28U] = 0x7FU;
    vm.memory[0x29U] = 1U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x2AU);

    /* get_cursor reports the deterministic virtual position row 1, column 1. */
    vm.memory[0x2AU] = 0xF0U;
    vm.memory[0x2BU] = 0x3FU;
    vm.memory[0x2CU] = 0x00U;
    vm.memory[0x2DU] = 0xC0U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x2EU);
    assert(read_word(&vm, 0xC0U) == 1U);
    assert(read_word(&vm, 0xC2U) == 1U);

    /* A four-byte cursor destination is validated before either word is written. */
    vm.pc = 0x2AU;
    vm.memory[0x2CU] = 0x02U;
    vm.memory[0x2DU] = 0xFEU;
    vm.memory[0x2FEU] = 0xAAU;
    vm.memory[0x2FFU] = 0xBBU;
    assert(zmachine_step(&vm) == TCL_ERROR);
    assert(vm.memory[0x2FEU] == 0xAAU && vm.memory[0x2FFU] == 0xBBU);
    vm.state = ZM_STATE_READY;
    vm.error[0] = '\0';

    /* set_colour evaluates both operands then disappears in the text frontend. */
    vm.pc = 0x2EU;
    vm.memory[0x2EU] = 0x1BU;
    vm.memory[0x2FU] = 2U;
    vm.memory[0x30U] = 3U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x31U);

    /* EXT:4 set_font reports normal font 1 and rejects unavailable font 3. */
    vm.memory[0x31U] = 0xBEU;
    vm.memory[0x32U] = 4U;
    vm.memory[0x33U] = 0x7FU;
    vm.memory[0x34U] = 1U;
    vm.memory[0x35U] = 0x10U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x36U && read_global(&vm, 0x10U) == 1U);

    vm.memory[0x36U] = 0xBEU;
    vm.memory[0x37U] = 4U;
    vm.memory[0x38U] = 0x7FU;
    vm.memory[0x39U] = 3U;
    vm.memory[0x3AU] = 0x11U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x3BU && read_global(&vm, 0x11U) == 0U);

    /* EXT:11 emits real UTF-8 Unicode to the canonical stream. */
    vm.memory[0x3BU] = 0xBEU;
    vm.memory[0x3CU] = 11U;
    vm.memory[0x3DU] = 0x3FU;
    vm.memory[0x3EU] = 0x20U;
    vm.memory[0x3FU] = 0xACU; /* U+20AC */
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x40U);
    assert(strcmp(Tcl_DStringValue(&vm.output), "\xE2\x82\xAC") == 0);

    /* EXT:12 reports printable Unicode output and ASCII keyboard input. */
    vm.memory[0x40U] = 0xBEU;
    vm.memory[0x41U] = 12U;
    vm.memory[0x42U] = 0x3FU;
    vm.memory[0x43U] = 0x20U;
    vm.memory[0x44U] = 0xACU;
    vm.memory[0x45U] = 0x12U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x46U && read_global(&vm, 0x12U) == 1U);

    vm.memory[0x46U] = 0xBEU;
    vm.memory[0x47U] = 12U;
    vm.memory[0x48U] = 0x3FU;
    vm.memory[0x49U] = 0x00U;
    vm.memory[0x4AU] = 0x41U; /* U+0041 'A' */
    vm.memory[0x4BU] = 0x13U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x4CU && read_global(&vm, 0x13U) == 3U);

    /* EXT:13 set_true_colour is a text-only no-op after operand evaluation. */
    vm.memory[0x4CU] = 0xBEU;
    vm.memory[0x4DU] = 13U;
    vm.memory[0x4EU] = 0x0FU;
    vm.memory[0x4FU] = 0xFFU;
    vm.memory[0x50U] = 0xFFU;
    vm.memory[0x51U] = 0x00U;
    vm.memory[0x52U] = 0x00U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x53U);

    /*
     * output_stream 2 mirrors selected state into Flags 2 bit 0. The host path
     * is preconfigured here because file naming belongs above opcode execution.
     */
    assert(zmachine_stream_file(&vm, "transcript", TRANSCRIPT_TEST_PATH) == TCL_OK);
    vm.memory[0x53U] = 0xF3U;
    vm.memory[0x54U] = 0x7FU;
    vm.memory[0x55U] = 2U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x56U);
    assert((vm.flags2 & 1U) != 0U && (vm.memory[0x11U] & 1U) != 0U);

    vm.memory[0x56U] = 0xF3U;
    vm.memory[0x57U] = 0x3FU;
    vm.memory[0x58U] = 0xFFU;
    vm.memory[0x59U] = 0xFEU; /* -2 */
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x5AU);
    assert((vm.flags2 & 1U) == 0U && (vm.memory[0x11U] & 1U) == 0U);

    /*
     * print_unicode to stream 3 reverse-maps through the active ZSCII table.
     * U+00E4 exists in the default table as ZSCII 155; U+20AC does not and
     * therefore becomes '?' as required for non-ZSCII stream-3 output.
     */
    vm.memory[0x5AU] = 0xF3U;
    vm.memory[0x5BU] = 0x4FU;
    vm.memory[0x5CU] = 3U;
    vm.memory[0x5DU] = 0x01U;
    vm.memory[0x5EU] = 0xA0U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x5FU && vm.stream3_depth == 1U);

    vm.memory[0x5FU] = 0xBEU;
    vm.memory[0x60U] = 11U;
    vm.memory[0x61U] = 0x3FU;
    vm.memory[0x62U] = 0x00U;
    vm.memory[0x63U] = 0xE4U; /* U+00E4 */
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x64U);

    vm.memory[0x64U] = 0xBEU;
    vm.memory[0x65U] = 11U;
    vm.memory[0x66U] = 0x3FU;
    vm.memory[0x67U] = 0x20U;
    vm.memory[0x68U] = 0xACU; /* U+20AC */
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x69U);
    assert(read_word(&vm, 0x1A0U) == 2U);
    assert(vm.memory[0x1A2U] == 155U);
    assert(vm.memory[0x1A3U] == (uint8_t)'?');

    vm.memory[0x69U] = 0xF3U;
    vm.memory[0x6AU] = 0x3FU;
    vm.memory[0x6BU] = 0xFFU;
    vm.memory[0x6CU] = 0xFDU; /* -3 */
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x6DU && vm.stream3_depth == 0U);

    free_vm(&vm);
    puts("extended presentation tests passed");
    return 0;
}
