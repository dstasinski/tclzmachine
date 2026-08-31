/*
 * text_exec.c
 *
 * Instruction-level regression coverage for Z-machine text output opcodes.
 * Hand-encoded V3/V5 instructions verify inline `print`, `new_line`,
 * `print_addr`, object short-name `print_obj`, `print_char`, signed `print_num`,
 * and `print_ret` including its newline and routine-return side effects.
 *
 * Z-text decoding details are tested directly in text.c; this file locks down
 * executor integration, instruction continuation addresses, and call-frame
 * behavior around text-producing instructions.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Allocate a writable synthetic VM with normal stream 1 selected. */
static void init_vm(ZMachine *vm, uint8_t version, size_t size)
{
    memset(vm, 0, sizeof(*vm));
    vm->memory = (uint8_t *)calloc(size, 1U);
    assert(vm->memory != NULL);
    vm->memory_size = size;
    vm->version = version;
    vm->static_memory_addr = (uint16_t)size;
    vm->globals_addr = 0x40U;
    vm->state = ZM_STATE_READY;
    vm->output_stream1_enabled = 1;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
}

/* Release storage initialized by init_vm(). */
static void free_vm(ZMachine *vm)
{
    free(vm->memory);
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
}

/* Write one big-endian synthetic story word. */
static void put16(uint8_t *memory, size_t address, uint16_t value)
{
    memory[address] = (uint8_t)(value >> 8);
    memory[address + 1U] = (uint8_t)value;
}

/* Pack three 5-bit Z-characters into one encoded Z-text word. */
static uint16_t zword(unsigned a, unsigned b, unsigned c, int last)
{
    return (uint16_t)((last ? 0x8000U : 0U) |
                      ((a & 31U) << 10) |
                      ((b & 31U) << 5) |
                      (c & 31U));
}

int main(void)
{
    /* Inline print consumes its packed words; new_line follows at next PC. */
    {
        ZMachine vm;
        init_vm(&vm, 3U, 512U);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xB2U; /* print literal */
        put16(vm.memory, 0x21U, zword(13U, 10U, 17U, 0));
        put16(vm.memory, 0x23U, zword(17U, 20U, 5U, 1));
        vm.memory[0x25U] = 0xBBU; /* new_line */

        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x25U);
        assert(strcmp(zmachine_output_data(&vm), "hello") == 0);
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x26U);
        assert(strcmp(zmachine_output_data(&vm), "hello\n") == 0);
        free_vm(&vm);
    }

    /* print_addr decodes an unpacked byte address and then resumes normally. */
    {
        ZMachine vm;
        init_vm(&vm, 3U, 512U);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0x87U; /* print_addr large constant */
        put16(vm.memory, 0x21U, 0x0100U);
        put16(vm.memory, 0x100U, zword(31U, 20U, 23U, 1)); /* zor */
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x23U);
        assert(strcmp(zmachine_output_data(&vm), "zor") == 0);
        free_vm(&vm);
    }

    /* print_obj locates the object's short name through its property table. */
    {
        ZMachine vm;
        size_t object1;
        init_vm(&vm, 3U, 1024U);
        vm.object_table_addr = 0x80U;
        object1 = 0x80U + 62U;
        put16(vm.memory, object1 + 7U, 0x0200U);
        vm.memory[0x200U] = 2U; /* short name is two words */
        put16(vm.memory, 0x201U, zword(31U, 20U, 23U, 0));
        put16(vm.memory, 0x203U, zword(16U, 5U, 5U, 1)); /* zork */

        vm.pc = 0x20U;
        vm.memory[0x20U] = 0x9AU; /* print_obj small constant */
        vm.memory[0x21U] = 1U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x22U);
        assert(strcmp(zmachine_output_data(&vm), "zork") == 0);
        free_vm(&vm);
    }

    /* V5 print_char and signed print_num append to the same canonical stream. */
    {
        ZMachine vm;
        init_vm(&vm, 5U, 512U);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xE5U; /* print_char */
        vm.memory[0x21U] = 0x7FU; /* one small constant */
        vm.memory[0x22U] = 'X';
        vm.memory[0x23U] = 0xE6U; /* print_num */
        vm.memory[0x24U] = 0x3FU; /* one large constant */
        put16(vm.memory, 0x25U, 0xFFF9U); /* -7 */

        assert(zmachine_step(&vm) == TCL_OK);
        assert(zmachine_step(&vm) == TCL_OK);
        assert(strcmp(zmachine_output_data(&vm), "X-7") == 0);
        free_vm(&vm);
    }

    /* print_ret appends newline and returns true from the active routine. */
    {
        ZMachine vm;
        init_vm(&vm, 3U, 512U);
        vm.pc = 0x20U;
        assert(zmachine_frame_push(&vm, 0x40U, 0U, 1,
                                   NULL, 0U, 0U) == TCL_OK);
        vm.memory[0x20U] = 0xB3U; /* print_ret literal */
        put16(vm.memory, 0x21U, zword(20U, 16U, 5U, 1)); /* ok */

        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x40U);
        assert(vm.frame_count == 0U);
        assert(strcmp(zmachine_output_data(&vm), "ok\n") == 0);
        free_vm(&vm);
    }

    puts("text opcode tests passed");
    return 0;
}
