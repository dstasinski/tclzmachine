#include "tclzmachine.h"
#include "zmachine_text.h"

#include <assert.h>
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
    vm->state = ZM_STATE_READY;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
}

static void free_vm(ZMachine *vm)
{
    free(vm->memory);
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
}

static void put_word(uint8_t *memory, size_t address, uint16_t word)
{
    memory[address] = (uint8_t)(word >> 8);
    memory[address + 1U] = (uint8_t)word;
}

static uint16_t zword(unsigned a, unsigned b, unsigned c, int last)
{
    return (uint16_t)((last ? 0x8000U : 0U) |
                      ((a & 31U) << 10) |
                      ((b & 31U) << 5) |
                      (c & 31U));
}

int main(void)
{
    {
        ZMachine vm;
        uint32_t next;
        init_vm(&vm, 3U, 512U);

        /* "hello": h e l / l o + padding */
        put_word(vm.memory, 0x100U, zword(13U, 10U, 17U, 0));
        put_word(vm.memory, 0x102U, zword(17U, 20U, 5U, 1));
        assert(zmachine_text_print(&vm, 0x100U, &next) == TCL_OK);
        assert(next == 0x104U);
        assert(strcmp(zmachine_output_data(&vm), "hello") == 0);
        free_vm(&vm);
    }

    {
        ZMachine vm;
        init_vm(&vm, 3U, 512U);

        /* A1 shift + H, then A2 shift + !, then newline. */
        put_word(vm.memory, 0x100U, zword(4U, 13U, 5U, 0));
        put_word(vm.memory, 0x102U, zword(20U, 5U, 7U, 1));
        assert(zmachine_text_print(&vm, 0x100U, NULL) == TCL_OK);
        assert(strcmp(zmachine_output_data(&vm), "H!\n") == 0);
        free_vm(&vm);
    }

    {
        ZMachine vm;
        init_vm(&vm, 3U, 512U);
        vm.abbreviations_addr = 0x40U;

        /* abbreviation entry 0 points to byte address 0x120. */
        put_word(vm.memory, 0x40U, 0x0090U);
        put_word(vm.memory, 0x120U, zword(31U, 20U, 23U, 1)); /* "zor" */
        put_word(vm.memory, 0x100U, zword(1U, 0U, 16U, 0));  /* abbrev + k */
        put_word(vm.memory, 0x102U, zword(5U, 5U, 5U, 1));
        assert(zmachine_text_print(&vm, 0x100U, NULL) == TCL_OK);
        assert(strcmp(zmachine_output_data(&vm), "zork") == 0);
        free_vm(&vm);
    }

    {
        ZMachine vm;
        init_vm(&vm, 5U, 512U);

        /* Custom alphabet table: change A0 zchar 6 from 'a' to '@'. */
        vm.memory[0x34U] = 0x00U;
        vm.memory[0x35U] = 0x80U;
        memcpy(vm.memory + 0x80U, "abcdefghijklmnopqrstuvwxyz", 26U);
        memcpy(vm.memory + 0x80U + 26U, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 26U);
        memset(vm.memory + 0x80U + 52U, ' ', 26U);
        vm.memory[0x80U] = '@';
        put_word(vm.memory, 0x100U, zword(6U, 5U, 5U, 1));
        assert(zmachine_text_print(&vm, 0x100U, NULL) == TCL_OK);
        assert(strcmp(zmachine_output_data(&vm), "@") == 0);
        free_vm(&vm);
    }

    puts("Z-text decoder tests passed");
    return 0;
}
