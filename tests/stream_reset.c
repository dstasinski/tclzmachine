/*
 * stream_reset.c
 *
 * Regression coverage for the public host-stream reset boundary.
 *
 * zmachine_reset() is an API/session reset, not the Z-machine restart opcode.
 * The stream wrapper therefore closes all replay/transcript/record FILE handles.
 * Flags 2 bit 0 must be cleared at the same boundary so the story never observes
 * "transcripting on" when no transcript resource exists. Bit 1 is unrelated
 * fixed-pitch game state and must not be cleared merely because host streams are
 * reset.
 */

#include "tclzmachine.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t read_word(const uint8_t *memory, size_t address)
{
    return (uint16_t)(((uint16_t)memory[address] << 8) |
                      memory[address + 1U]);
}

static void write_word(uint8_t *memory, size_t address, uint16_t value)
{
    memory[address] = (uint8_t)(value >> 8);
    memory[address + 1U] = (uint8_t)value;
}

int main(void)
{
    ZMachine vm;
    uint16_t flags2;

    memset(&vm, 0, sizeof(vm));
    vm.memory_size = 1024U;
    vm.memory = (uint8_t *)calloc(vm.memory_size, 1U);
    assert(vm.memory != NULL);

    vm.version = 5U;
    vm.initial_pc = 0x80U;
    vm.pc = vm.initial_pc;
    vm.static_memory_addr = 0x300U;
    vm.globals_addr = 0x100U;
    vm.state = ZM_STATE_READY;
    vm.output_stream1_enabled = 1;
    Tcl_DStringInit(&vm.output);
    Tcl_DStringInit(&vm.pending_input);

    /* Both transcription and fixed-pitch request begin selected. */
    write_word(vm.memory, 0x10U, 0x0003U);
    vm.flags2 = 0x0003U;

    assert(zmachine_reset(&vm) == TCL_OK);

    flags2 = read_word(vm.memory, 0x10U);
    assert((flags2 & 0x0001U) == 0U); /* no transcript FILE after API reset */
    assert((flags2 & 0x0002U) != 0U); /* unrelated fixed-pitch state preserved */
    assert(vm.flags2 == flags2);

    free(vm.initial_dynamic_memory);
    free(vm.memory);
    Tcl_DStringFree(&vm.output);
    Tcl_DStringFree(&vm.pending_input);

    puts("stream reset synchronization tests passed");
    return 0;
}
