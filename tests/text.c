/*
 * text.c
 *
 * Direct regression coverage for Z-text/ZSCII decoding independent of opcode
 * dispatch. Hand-packed strings verify V3 alphabet shifts/newlines,
 * abbreviation expansion and continuation addresses, V5 custom alphabets,
 * the standard extra-ZSCII Unicode table, story-defined Unicode mappings, safe
 * fallback for undefined entries, and the distinction between UTF-8 stream-1
 * rendering and raw ZSCII bytes captured by memory stream 3.
 */

#include "tclzmachine.h"
#include "zmachine_text.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Allocate a writable synthetic story image with normal stream 1 selected. */
static void init_vm(ZMachine *vm, uint8_t version, size_t size)
{
    memset(vm, 0, sizeof(*vm));
    vm->memory = (uint8_t *)calloc(size, 1U);
    assert(vm->memory != NULL);
    vm->memory_size = size;
    vm->version = version;
    vm->static_memory_addr = (uint16_t)size;
    vm->state = ZM_STATE_READY;
    vm->output_stream1_enabled = 1;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
}

/* Release allocations and Tcl strings initialized by init_vm(). */
static void free_vm(ZMachine *vm)
{
    free(vm->memory);
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
}

/* Write one packed Z-text/story word in big-endian order. */
static void put_word(uint8_t *memory, size_t address, uint16_t word)
{
    memory[address] = (uint8_t)(word >> 8);
    memory[address + 1U] = (uint8_t)word;
}

/* Pack three 5-bit Z-characters and optionally set the terminating high bit. */
static uint16_t zword(unsigned a, unsigned b, unsigned c, int last)
{
    return (uint16_t)((last ? 0x8000U : 0U) |
                      ((a & 31U) << 10) |
                      ((b & 31U) << 5) |
                      (c & 31U));
}

int main(void)
{
    /* Basic packed-text decoding and returned byte continuation. */
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

    /* V3 temporary alphabet shifts plus the A2 newline entry. */
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

    /* V3 abbreviation lookup expands recursively, then returns to outer text. */
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

    /* V5 custom alphabet table overrides the default A0/A1/A2 characters. */
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

    /* Pre-V5 stories always use the standard default extra-character table. */
    {
        ZMachine vm;
        init_vm(&vm, 3U, 512U);

        /* V1-V4 always use the standard default extra-character table. */
        assert(zmachine_text_output_zscii(&vm, 155U) == TCL_OK); /* U+00E4 */
        assert(zmachine_text_output_zscii(&vm, 220U) == TCL_OK); /* U+0153 */
        assert(strcmp(zmachine_output_data(&vm),
                      "\xC3\xA4\xC5\x93") == 0);
        free_vm(&vm);
    }

    /* V5 story-defined Unicode mapping and raw stream-3 ZSCII preservation. */
    {
        ZMachine vm;
        init_vm(&vm, 5U, 512U);

        /*
         * Header-extension word 3 selects a story Unicode table. ZSCII 155
         * becomes U+20AC EURO SIGN and 156 becomes U+03A9 GREEK CAPITAL OMEGA.
         */
        vm.header_extension_addr = 0x40U;
        put_word(vm.memory, 0x40U, 3U);
        put_word(vm.memory, 0x46U, 0x0080U);
        vm.memory[0x80U] = 2U;
        put_word(vm.memory, 0x81U, 0x20ACU);
        put_word(vm.memory, 0x83U, 0x03A9U);

        assert(zmachine_text_output_zscii(&vm, 155U) == TCL_OK);
        assert(zmachine_text_output_zscii(&vm, 156U) == TCL_OK);
        assert(zmachine_text_output_zscii(&vm, 157U) == TCL_OK);
        assert(strcmp(zmachine_output_data(&vm),
                      "\xE2\x82\xAC\xCE\xA9?") == 0);

        /*
         * Stream 3 stores ZSCII bytes, not the UTF-8 encoding used by stream 1.
         * The translated EURO SIGN therefore remains byte 155 in story memory.
         */
        Tcl_DStringSetLength(&vm.output, 0);
        vm.stream3_depth = 1U;
        vm.stream3_tables[0] = 0xA0U;
        vm.memory[0xA0U] = 0U;
        vm.memory[0xA1U] = 0U;
        assert(zmachine_text_output_zscii(&vm, 155U) == TCL_OK);
        assert(zmachine_text_output_zscii(&vm, 13U) == TCL_OK);
        assert(vm.memory[0xA0U] == 0U && vm.memory[0xA1U] == 2U);
        assert(vm.memory[0xA2U] == 155U);
        assert(vm.memory[0xA3U] == 13U);
        assert(Tcl_DStringLength(&vm.output) == 0);
        free_vm(&vm);
    }

    puts("Z-text decoder tests passed");
    return 0;
}
