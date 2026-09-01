/*
 * header.c
 *
 * Regression coverage for interpreter-owned/Rst header fields. The tests use
 * synthetic V3, V5, and V7 headers so capability masking can be checked without
 * involving a compiler-generated story. They deliberately begin with capability
 * bits set, then verify that the text-only runtime clears only unsupported
 * facilities while preserving game/live bits such as transcription and undo.
 */

#include "tclzmachine.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void init_vm(ZMachine *vm, uint8_t version)
{
    memset(vm, 0, sizeof(*vm));
    vm->memory = (uint8_t *)calloc(1024U, 1U);
    assert(vm->memory != NULL);
    vm->memory_size = 1024U;
    vm->version = version;
    vm->static_memory_addr = 0x300U;
}

static void free_vm(ZMachine *vm)
{
    free(vm->memory);
}

static uint16_t read_word(const ZMachine *vm, size_t address)
{
    return (uint16_t)(((uint16_t)vm->memory[address] << 8) |
                      vm->memory[address + 1U]);
}

static void write_word(ZMachine *vm, size_t address, uint16_t value)
{
    vm->memory[address] = (uint8_t)(value >> 8);
    vm->memory[address + 1U] = (uint8_t)value;
}

int main(void)
{
    {
        ZMachine vm;

        init_vm(&vm, 3U);
        vm.memory[0x01U] = 0x7eU;
        vm.memory[0x32U] = 1U;
        vm.memory[0x33U] = 1U;

        zmachine_refresh_interpreter_header(&vm);

        /* Status unavailable; no advertised upper window or font-pitch mode. */
        assert((vm.memory[0x01U] & 0x10U) != 0U);
        assert((vm.memory[0x01U] & 0x20U) == 0U);
        assert((vm.memory[0x01U] & 0x40U) == 0U);
        /* Story-owned status-line type remains untouched. */
        assert((vm.memory[0x01U] & 0x02U) != 0U);
        assert(vm.flags1 == vm.memory[0x01U]);
        assert(vm.memory[0x32U] == 0U && vm.memory[0x33U] == 0U);
        free_vm(&vm);
    }

    {
        ZMachine vm;
        uint16_t flags2;

        init_vm(&vm, 5U);
        vm.memory[0x01U] = 0xffU;
        write_word(&vm, 0x10U, 0xffffU);
        vm.header_extension_addr = 0x200U;
        write_word(&vm, 0x200U, 6U);
        write_word(&vm, 0x208U, 0xffffU); /* Flags 3 */
        write_word(&vm, 0x20aU, 0x1234U); /* stale true foreground */
        write_word(&vm, 0x20cU, 0x5678U); /* stale true background */

        zmachine_refresh_interpreter_header(&vm);

        /* No colours, styles, or timed input are advertised. */
        assert((vm.memory[0x01U] & 0x01U) == 0U);
        assert((vm.memory[0x01U] & 0x04U) == 0U);
        assert((vm.memory[0x01U] & 0x08U) == 0U);
        assert((vm.memory[0x01U] & 0x10U) == 0U);
        assert((vm.memory[0x01U] & 0x80U) == 0U);

        flags2 = read_word(&vm, 0x10U);
        assert((flags2 & 0x0008U) == 0U); /* pictures/font 3 unavailable */
        assert((flags2 & 0x0020U) == 0U); /* mouse unavailable */
        assert((flags2 & 0x0080U) == 0U); /* sound unavailable */
        assert((flags2 & 0x0010U) != 0U); /* undo request preserved */
        assert((flags2 & 0x0001U) != 0U); /* transcript state preserved */
        assert(vm.flags2 == flags2);

        assert(vm.memory[0x1eU] == 2U);
        assert(vm.memory[0x1fU] == (uint8_t)'T');
        assert(vm.memory[0x20U] == 255U);
        assert(vm.memory[0x21U] == 80U);
        assert(read_word(&vm, 0x22U) == 80U);
        assert(read_word(&vm, 0x24U) == 255U);
        assert(vm.memory[0x26U] == 1U && vm.memory[0x27U] == 1U);
        assert(vm.memory[0x2cU] == 2U && vm.memory[0x2dU] == 9U);
        assert(vm.memory[0x32U] == 0U && vm.memory[0x33U] == 0U);

        /* Standard 1.1 header-extension reset fields are normalized too. */
        assert(read_word(&vm, 0x208U) == 0U);
        assert(read_word(&vm, 0x20aU) == 0x7fffU); /* true white foreground */
        assert(read_word(&vm, 0x20cU) == 0x0000U); /* true black background */
        free_vm(&vm);
    }

    {
        ZMachine vm;

        init_vm(&vm, 5U);
        vm.header_extension_addr = 0x200U;
        write_word(&vm, 0x200U, 4U);      /* Flags 3 is the last declared word. */
        write_word(&vm, 0x208U, 0xffffU); /* Flags 3 */
        write_word(&vm, 0x20aU, 0x1234U); /* physically present, not declared */
        write_word(&vm, 0x20cU, 0x5678U);

        zmachine_refresh_interpreter_header(&vm);

        assert(read_word(&vm, 0x208U) == 0U);
        /* Interpreter writes beyond the declared extension length do nothing. */
        assert(read_word(&vm, 0x20aU) == 0x1234U);
        assert(read_word(&vm, 0x20cU) == 0x5678U);
        free_vm(&vm);
    }

    {
        ZMachine vm;
        uint16_t flags2;

        init_vm(&vm, 7U);
        vm.memory[0x01U] = 0xffU;
        write_word(&vm, 0x10U, 0xffffU);

        zmachine_refresh_interpreter_header(&vm);

        /* V6+ graphics/sound availability bits are also false for V7/V8. */
        assert((vm.memory[0x01U] & 0x02U) == 0U);
        assert((vm.memory[0x01U] & 0x20U) == 0U);
        flags2 = read_word(&vm, 0x10U);
        assert((flags2 & 0x0100U) == 0U); /* menus unavailable */
        assert((flags2 & 0x0010U) != 0U); /* undo remains available */
        free_vm(&vm);
    }

    puts("interpreter header tests passed");
    return 0;
}
