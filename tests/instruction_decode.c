/*
 * instruction_decode.c
 *
 * Focused byte-level regression coverage for the Z-machine instruction decoder.
 * Synthetic instruction streams verify LONG, SHORT, VARIABLE, and V5+ EXTENDED
 * forms; logical 2OP-vs-VAR table classification; large/small/variable operand
 * decoding; the second type byte used by call_vs2/call_vn2; the pre-V5 meaning
 * of opcode byte 0xBE; the historical Galatea zero-operand read_char
 * normalization; unavailable timed-input fallback normalization; and
 * deterministic rejection of truncated instructions.
 *
 * These tests intentionally stop before operand resolution or execution. Their
 * purpose is to lock down the boundary between raw story bytes and the uniform
 * ZMachineInstruction representation consumed by later VM layers.
 */

#include "zmachine_decode.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Decode one complete synthetic instruction at address zero and require success. */
static ZMachineInstruction decode(const uint8_t *bytes,
                                  size_t size,
                                  uint8_t version)
{
    ZMachineInstruction insn;
    char error[128];

    memset(&insn, 0, sizeof(insn));
    assert(zmachine_decode_instruction(bytes, size, version, 0, &insn,
                                       error, sizeof(error)) == 1);
    return insn;
}

int main(void)
{
    /* LONG-form 2OP with two small constants. */
    {
        const uint8_t b[] = {0x05, 0x02, 0x00};
        ZMachineInstruction i = decode(b, sizeof(b), 3);
        assert(i.form == ZM_FORM_LONG);
        assert(i.operand_count == ZM_OPERANDS_2OP);
        assert(i.opcode_number == 5);
        assert(i.operand_count_actual == 2);
        assert(i.operands[0].type == ZM_OPERAND_SMALL_CONSTANT);
        assert(i.operands[0].value == 2);
        assert(i.operands[1].value == 0);
        assert(i.next_pc == 3);
    }

    /* SHORT 1OP carrying a 16-bit large constant. */
    {
        const uint8_t b[] = {0x8f, 0x01, 0x56};
        ZMachineInstruction i = decode(b, sizeof(b), 3);
        assert(i.form == ZM_FORM_SHORT);
        assert(i.operand_count == ZM_OPERANDS_1OP);
        assert(i.opcode_number == 15);
        assert(i.operands[0].type == ZM_OPERAND_LARGE_CONSTANT);
        assert(i.operands[0].value == 0x0156);
        assert(i.next_pc == 3);
    }

    /* SHORT form with omitted operand denotes a 0OP instruction. */
    {
        const uint8_t b[] = {0xb2};
        ZMachineInstruction i = decode(b, sizeof(b), 3);
        assert(i.form == ZM_FORM_SHORT);
        assert(i.operand_count == ZM_OPERANDS_0OP);
        assert(i.opcode_number == 2);
        assert(i.next_pc == 1);
    }

    /* VARIABLE encoding may still select the logical 2OP opcode table. */
    {
        const uint8_t b[] = {0xd6, 0x2f, 0x03, 0xe8, 0x02};
        ZMachineInstruction i = decode(b, sizeof(b), 3);
        assert(i.form == ZM_FORM_VARIABLE);
        assert(i.operand_count == ZM_OPERANDS_2OP);
        assert(i.opcode_number == 22);
        assert(i.type_byte_count == 1);
        assert(i.operand_count_actual == 2);
        assert(i.operands[0].value == 1000);
        assert(i.operands[1].type == ZM_OPERAND_VARIABLE);
        assert(i.operands[1].value == 2);
        assert(i.next_pc == 5);
    }

    /* call_vs2-style VAR opcodes consume a second operand-type byte. */
    {
        const uint8_t b[] = {0xec, 0x55, 0xaf, 1, 2, 3, 4, 5, 6};
        ZMachineInstruction i = decode(b, sizeof(b), 5);
        assert(i.form == ZM_FORM_VARIABLE);
        assert(i.operand_count == ZM_OPERANDS_VAR);
        assert(i.opcode_number == 12);
        assert(i.type_byte_count == 2);
        assert(i.operand_count_actual == 6);
        assert(i.operands[4].type == ZM_OPERAND_VARIABLE);
        assert(i.operands[5].type == ZM_OPERAND_VARIABLE);
        assert(i.next_pc == sizeof(b));
    }

    /* V5+ 0xBE introduces the EXTENDED opcode table and its own type byte. */
    {
        const uint8_t b[] = {0xbe, 0x03, 0x1f, 0xff, 0xfe, 0x03};
        ZMachineInstruction i = decode(b, sizeof(b), 5);
        assert(i.form == ZM_FORM_EXTENDED);
        assert(i.opcode_number == 3);
        assert(i.type_byte_count == 1);
        assert(i.operand_count_actual == 2);
        assert(i.operands[0].value == 0xfffe);
        assert(i.operands[1].value == 3);
        assert(i.next_pc == sizeof(b));
    }

    /* Before V5 the same byte remains the ordinary SHORT 0OP opcode 14. */
    {
        const uint8_t b[] = {0xbe};
        ZMachineInstruction i = decode(b, sizeof(b), 4);
        assert(i.form == ZM_FORM_SHORT);
        assert(i.operand_count == ZM_OPERANDS_0OP);
        assert(i.opcode_number == 14);
        assert(i.next_pc == 1);
    }

    /*
     * Galatea's released Z8 contains VAR:22 with an all-omitted type byte.
     * Normalize it to the Standard-equivalent keyboard device 1 without
     * consuming the following store-variable byte.
     */
    {
        const uint8_t b[] = {0xf6, 0xff, 0x10};
        ZMachineInstruction i = decode(b, sizeof(b), 8);
        assert(i.form == ZM_FORM_VARIABLE);
        assert(i.operand_count == ZM_OPERANDS_VAR);
        assert(i.opcode_number == 22);
        assert(i.operand_count_actual == 1);
        assert(i.operands[0].type == ZM_OPERAND_SMALL_CONSTANT);
        assert(i.operands[0].value == 1);
        assert(i.next_pc == 2);
    }

    /*
     * Timed read_char is not advertised by this host. Older stories which
     * nevertheless encode time/routine operands are treated as ordinary
     * untimed input without changing their encoded instruction length.
     */
    {
        const uint8_t b[] = {0xf6, 0x53, 0x01, 0x05, 0x01, 0x10};
        ZMachineInstruction i = decode(b, sizeof(b), 5);
        assert(i.opcode_number == 22);
        assert(i.operand_count_actual == 3);
        assert(i.operands[0].value == 1);
        assert(i.operands[1].type == ZM_OPERAND_SMALL_CONSTANT);
        assert(i.operands[1].value == 0);
        assert(i.operands[2].type == ZM_OPERAND_SMALL_CONSTANT);
        assert(i.operands[2].value == 0);
        assert(i.next_pc == 5);
    }

    /* The same fallback applies to V4+ read time/routine operands. */
    {
        const uint8_t b[] = {0xe4, 0x55, 0x20, 0x30, 0x04, 0x09};
        ZMachineInstruction i = decode(b, sizeof(b), 5);
        assert(i.opcode_number == 4);
        assert(i.operand_count_actual == 4);
        assert(i.operands[0].value == 0x20);
        assert(i.operands[1].value == 0x30);
        assert(i.operands[2].type == ZM_OPERAND_SMALL_CONSTANT);
        assert(i.operands[2].value == 0);
        assert(i.operands[3].type == ZM_OPERAND_SMALL_CONSTANT);
        assert(i.operands[3].value == 0);
        assert(i.next_pc == sizeof(b));
    }

    /* Missing payload bytes must produce a useful truncated-instruction error. */
    {
        const uint8_t b[] = {0x8f, 0x01};
        ZMachineInstruction i;
        char error[128];
        assert(zmachine_decode_instruction(b, sizeof(b), 3, 0, &i,
                                           error, sizeof(error)) == 0);
        assert(strstr(error, "truncated") != NULL);
    }

    puts("instruction decode tests passed");
    return 0;
}
