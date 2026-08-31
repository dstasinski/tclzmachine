/*
 * tokenise.c
 *
 * Focused Version 5 regression for lexical VAR opcodes `tokenise` and
 * `encode_text`.
 *
 * The test exercises the public instruction path, the main story dictionary,
 * the optional user-dictionary operand, dictionary separators, parse-buffer
 * positions, the flag which preserves slots for unrecognized words, exact
 * dictionary encryption (including A2, truncation, and custom alphabets), and
 * the distinction between read's special parse=0 convention and tokenise's
 * required parse-table destination.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"
#include "zmachine_undo.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t word_at(const uint8_t *memory, size_t address)
{
    return (uint16_t)(((uint16_t)memory[address] << 8) |
                      memory[address + 1U]);
}

static void write_dictionary(ZMachine *vm,
                             uint16_t address,
                             const uint8_t encoded_word[6])
{
    vm->memory[address] = 1U;                  /* one separator */
    vm->memory[address + 1U] = (uint8_t)',';
    vm->memory[address + 2U] = 6U;             /* entry width */
    vm->memory[address + 3U] = 0U;
    vm->memory[address + 4U] = 1U;             /* one entry */
    memcpy(vm->memory + address + 5U, encoded_word, 6U);
}

int main(void)
{
    static const uint8_t encoded_look[6] = {
        0x46U, 0x94U, 0x40U, 0xA5U, 0x94U, 0xA5U
    };
    static const uint8_t encoded_mystery[6] = {
        0x4BU, 0xD8U, 0x65U, 0x57U, 0xF8U, 0xA5U
    };
    static const uint8_t encoded_zero[6] = {
        0x15U, 0x05U, 0x14U, 0xA5U, 0x94U, 0xA5U
    };
    static const uint8_t encoded_partial_escape[6] = {
        0x18U, 0xC6U, 0x18U, 0xC6U, 0x94U, 0xC3U
    };
    static const uint8_t encoded_custom_at[6] = {
        0x10U, 0xC5U, 0x14U, 0xA5U, 0x94U, 0xA5U
    };
    static const char text[] = "look, mystery";
    ZMachine vm;
    uint16_t main_entry = 0x105U;
    uint16_t user_entry = 0x125U;

    memset(&vm, 0, sizeof(vm));
    vm.memory_size = 1024U;
    vm.memory = (uint8_t *)calloc(vm.memory_size, 1U);
    assert(vm.memory != NULL);
    vm.version = 5U;
    vm.static_memory_addr = 0x300U;
    vm.dictionary_addr = 0x100U;
    vm.globals_addr = 0x200U;
    vm.state = ZM_STATE_READY;
    vm.output_stream1_enabled = 1;
    Tcl_DStringInit(&vm.output);
    Tcl_DStringInit(&vm.pending_input);

    write_dictionary(&vm, 0x100U, encoded_look);
    write_dictionary(&vm, 0x120U, encoded_mystery);

    /* V5 text buffer: maximum byte, current length byte, then characters. */
    vm.memory[0x40U] = 40U;
    vm.memory[0x41U] = (uint8_t)(sizeof(text) - 1U);
    memcpy(vm.memory + 0x42U, text, sizeof(text) - 1U);
    vm.memory[0x80U] = 4U;                    /* parse-buffer capacity */

    /* VAR:27 tokenise text parse, using the main dictionary. */
    vm.pc = 0x20U;
    vm.memory[0x20U] = 0xFBU;
    vm.memory[0x21U] = 0x5FU;                 /* small, small, omitted, omitted */
    vm.memory[0x22U] = 0x40U;
    vm.memory[0x23U] = 0x80U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x24U);

    assert(vm.memory[0x81U] == 3U);
    assert(word_at(vm.memory, 0x82U) == main_entry);
    assert(vm.memory[0x84U] == 4U && vm.memory[0x85U] == 2U);
    assert(word_at(vm.memory, 0x86U) == 0U);
    assert(vm.memory[0x88U] == 1U && vm.memory[0x89U] == 6U);
    assert(word_at(vm.memory, 0x8AU) == 0U);
    assert(vm.memory[0x8CU] == 7U && vm.memory[0x8DU] == 8U);

    /*
     * Re-tokenize through a user dictionary containing only "mystery". With
     * flag=1, unknown "look" and comma slots must remain byte-for-byte intact,
     * while the recognized third slot is replaced with the user entry.
     */
    memset(vm.memory + 0x82U, 0xA5, 12U);
    vm.pc = 0x30U;
    vm.memory[0x30U] = 0xFBU;
    vm.memory[0x31U] = 0x51U;                 /* small, small, large, small */
    vm.memory[0x32U] = 0x40U;
    vm.memory[0x33U] = 0x80U;
    vm.memory[0x34U] = 0x01U;
    vm.memory[0x35U] = 0x20U;
    vm.memory[0x36U] = 1U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x37U);

    assert(vm.memory[0x81U] == 3U);
    assert(vm.memory[0x82U] == 0xA5U && vm.memory[0x83U] == 0xA5U &&
           vm.memory[0x84U] == 0xA5U && vm.memory[0x85U] == 0xA5U);
    assert(vm.memory[0x86U] == 0xA5U && vm.memory[0x87U] == 0xA5U &&
           vm.memory[0x88U] == 0xA5U && vm.memory[0x89U] == 0xA5U);
    assert(word_at(vm.memory, 0x8AU) == user_entry);
    assert(vm.memory[0x8CU] == 7U && vm.memory[0x8DU] == 8U);

    /* VAR:28 encode_text 0x42 4 0 0xA0 must produce the "look" key. */
    memset(vm.memory + 0xA0U, 0, 6U);
    vm.pc = 0x50U;
    vm.memory[0x50U] = 0xFCU;
    vm.memory[0x51U] = 0x55U;                 /* four small constants */
    vm.memory[0x52U] = 0x42U;
    vm.memory[0x53U] = 4U;
    vm.memory[0x54U] = 0U;
    vm.memory[0x55U] = 0xA0U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x56U);
    assert(memcmp(vm.memory + 0xA0U, encoded_look, 6U) == 0);

    /* Default A2/8 is digit zero; A2/6 is the ZSCII escape, not '0'. */
    vm.memory[0x70U] = (uint8_t)'0';
    memset(vm.memory + 0xB0U, 0, 6U);
    vm.pc = 0x58U;
    vm.memory[0x58U] = 0xFCU;
    vm.memory[0x59U] = 0x55U;
    vm.memory[0x5AU] = 0x70U;
    vm.memory[0x5BU] = 1U;
    vm.memory[0x5CU] = 0U;
    vm.memory[0x5DU] = 0xB0U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x5EU);
    assert(memcmp(vm.memory + 0xB0U, encoded_zero, 6U) == 0);

    /*
     * Six direct 'a' Z-characters leave only three slots for '~'. Since '~'
     * needs shift-A2, escape, high, low, the low five bits are intentionally
     * truncated and the preceding three construction Z-characters remain.
     */
    memcpy(vm.memory + 0x72U, "aaaaaa~", 7U);
    memset(vm.memory + 0xB8U, 0, 6U);
    vm.pc = 0x68U;
    vm.memory[0x68U] = 0xFCU;
    vm.memory[0x69U] = 0x55U;
    vm.memory[0x6AU] = 0x72U;
    vm.memory[0x6BU] = 7U;
    vm.memory[0x6CU] = 0U;
    vm.memory[0x6DU] = 0xB8U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x6EU);
    assert(memcmp(vm.memory + 0xB8U, encoded_partial_escape, 6U) == 0);

    /*
     * Install a V5 custom alphabet table whose A1 Z-character 6 is '@'. Input
     * is lowercased first, then all custom alphabets are searched; the encoded
     * key therefore begins with shift-A1 (4), then Z-character 6.
     */
    vm.memory[0x34U] = 0x01U;
    vm.memory[0x35U] = 0x80U;
    memset(vm.memory + 0x180U, 0xFF, 78U);
    vm.memory[0x180U + 26U] = (uint8_t)'@';
    vm.memory[0x71U] = (uint8_t)'@';
    memset(vm.memory + 0xC0U, 0, 6U);
    vm.pc = 0x78U;
    vm.memory[0x78U] = 0xFCU;
    vm.memory[0x79U] = 0x55U;
    vm.memory[0x7AU] = 0x71U;
    vm.memory[0x7BU] = 1U;
    vm.memory[0x7CU] = 0U;
    vm.memory[0x7DU] = 0xC0U;
    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.pc == 0x7EU);
    assert(memcmp(vm.memory + 0xC0U, encoded_custom_at, 6U) == 0);

    /* Restore default alphabets before testing the tokenise error path. */
    vm.memory[0x34U] = 0U;
    vm.memory[0x35U] = 0U;

    /*
     * VAR:27 has no read-style “parse zero means skip parsing” convention.
     * Rejecting it here prevents a silent successful no-op and makes the
     * malformed destination visible to the embedding application.
     */
    vm.pc = 0xD0U;
    vm.state = ZM_STATE_READY;
    vm.error[0] = '\0';
    vm.memory[0xD0U] = 0xFBU;
    vm.memory[0xD1U] = 0x5FU;
    vm.memory[0xD2U] = 0x40U;
    vm.memory[0xD3U] = 0U;
    assert(zmachine_step(&vm) == TCL_ERROR);
    assert(vm.state == ZM_STATE_ERROR);
    assert(strstr(vm.error, "nonzero parse buffer") != NULL);
    assert(vm.memory[0x41U] == (uint8_t)(sizeof(text) - 1U));

    zmachine_undo_discard(&vm);
    free(vm.memory);
    Tcl_DStringFree(&vm.output);
    Tcl_DStringFree(&vm.pending_input);

    puts("V5 lexical opcode regression passed");
    return 0;
}