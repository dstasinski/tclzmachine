#include "zmachine_decode.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

    {
        const uint8_t b[] = {0xb2};
        ZMachineInstruction i = decode(b, sizeof(b), 3);
        assert(i.form == ZM_FORM_SHORT);
        assert(i.operand_count == ZM_OPERANDS_0OP);
        assert(i.opcode_number == 2);
        assert(i.next_pc == 1);
    }

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

    {
        const uint8_t b[] = {0xbe};
        ZMachineInstruction i = decode(b, sizeof(b), 4);
        assert(i.form == ZM_FORM_SHORT);
        assert(i.operand_count == ZM_OPERANDS_0OP);
        assert(i.opcode_number == 14);
        assert(i.next_pc == 1);
    }

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
