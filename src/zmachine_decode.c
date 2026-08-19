#include "zmachine_decode.h"

#include <stdio.h>
#include <string.h>

static void decode_error(char *error, size_t error_size, const char *message)
{
    if (error && error_size > 0U) {
        snprintf(error, error_size, "%s", message);
    }
}

static int read_u8(const uint8_t *memory,
                   size_t memory_size,
                   uint32_t *pc,
                   uint8_t *value,
                   char *error,
                   size_t error_size)
{
    if ((size_t)*pc >= memory_size) {
        decode_error(error, error_size, "truncated Z-machine instruction");
        return 0;
    }

    *value = memory[*pc];
    ++*pc;
    return 1;
}

static int read_u16(const uint8_t *memory,
                    size_t memory_size,
                    uint32_t *pc,
                    uint16_t *value,
                    char *error,
                    size_t error_size)
{
    uint8_t hi;
    uint8_t lo;

    if (!read_u8(memory, memory_size, pc, &hi, error, error_size) ||
        !read_u8(memory, memory_size, pc, &lo, error, error_size)) {
        return 0;
    }

    *value = (uint16_t)(((uint16_t)hi << 8) | lo);
    return 1;
}

static int append_type(ZMachineOperandType type,
                       ZMachineOperandType *types,
                       uint8_t *count,
                       int *omitted_seen,
                       char *error,
                       size_t error_size)
{
    if (type == ZM_OPERAND_OMITTED) {
        *omitted_seen = 1;
        return 1;
    }

    if (*omitted_seen) {
        decode_error(error, error_size,
                     "non-omitted operand follows an omitted operand");
        return 0;
    }

    if (*count >= ZM_MAX_OPERANDS) {
        decode_error(error, error_size, "too many Z-machine operands");
        return 0;
    }

    types[*count] = type;
    ++*count;
    return 1;
}

static int decode_type_byte(uint8_t byte,
                            ZMachineOperandType *types,
                            uint8_t *count,
                            int *omitted_seen,
                            char *error,
                            size_t error_size)
{
    int shift;

    for (shift = 6; shift >= 0; shift -= 2) {
        ZMachineOperandType type =
            (ZMachineOperandType)((byte >> shift) & 0x03U);

        if (!append_type(type, types, count, omitted_seen,
                         error, error_size)) {
            return 0;
        }
    }

    return 1;
}

int zmachine_decode_instruction(const uint8_t *memory,
                                size_t memory_size,
                                uint8_t version,
                                uint32_t pc,
                                ZMachineInstruction *instruction,
                                char *error,
                                size_t error_size)
{
    ZMachineOperandType types[ZM_MAX_OPERANDS];
    uint8_t opcode;
    uint8_t type_byte;
    uint8_t type_count = 0U;
    uint8_t i;
    int omitted_seen = 0;

    if (error && error_size > 0U) {
        error[0] = '\0';
    }

    if (!memory || !instruction) {
        decode_error(error, error_size, "invalid decoder argument");
        return 0;
    }

    memset(instruction, 0, sizeof(*instruction));
    instruction->address = pc;

    if (!read_u8(memory, memory_size, &pc, &opcode, error, error_size)) {
        return 0;
    }

    instruction->opcode_byte = opcode;

    if (opcode == 0xBEU && version >= 5U) {
        instruction->form = ZM_FORM_EXTENDED;
        instruction->operand_count = ZM_OPERANDS_VAR;

        if (!read_u8(memory, memory_size, &pc,
                     &instruction->opcode_number, error, error_size) ||
            !read_u8(memory, memory_size, &pc,
                     &type_byte, error, error_size)) {
            return 0;
        }

        instruction->type_byte_count = 1U;
        if (!decode_type_byte(type_byte, types, &type_count, &omitted_seen,
                              error, error_size)) {
            return 0;
        }
    } else if ((opcode & 0xC0U) == 0xC0U) {
        instruction->form = ZM_FORM_VARIABLE;
        instruction->opcode_number = opcode & 0x1FU;
        instruction->operand_count =
            (opcode & 0x20U) ? ZM_OPERANDS_VAR : ZM_OPERANDS_2OP;

        if (!read_u8(memory, memory_size, &pc, &type_byte,
                     error, error_size)) {
            return 0;
        }

        instruction->type_byte_count = 1U;
        if (!decode_type_byte(type_byte, types, &type_count, &omitted_seen,
                              error, error_size)) {
            return 0;
        }

        /*
         * Only VAR:12 call_vs2 and VAR:26 call_vn2 use a second type byte.
         * Variable-form encodings of 2OP opcodes never do.
         */
        if (instruction->operand_count == ZM_OPERANDS_VAR &&
            (instruction->opcode_number == 12U ||
             instruction->opcode_number == 26U)) {
            if (!read_u8(memory, memory_size, &pc, &type_byte,
                         error, error_size)) {
                return 0;
            }

            instruction->type_byte_count = 2U;
            if (!decode_type_byte(type_byte, types, &type_count,
                                  &omitted_seen, error, error_size)) {
                return 0;
            }
        }
    } else if ((opcode & 0xC0U) == 0x80U) {
        ZMachineOperandType type =
            (ZMachineOperandType)((opcode >> 4) & 0x03U);

        instruction->form = ZM_FORM_SHORT;
        instruction->opcode_number = opcode & 0x0FU;

        if (type == ZM_OPERAND_OMITTED) {
            instruction->operand_count = ZM_OPERANDS_0OP;
        } else {
            instruction->operand_count = ZM_OPERANDS_1OP;
            types[type_count++] = type;
        }
    } else {
        instruction->form = ZM_FORM_LONG;
        instruction->operand_count = ZM_OPERANDS_2OP;
        instruction->opcode_number = opcode & 0x1FU;

        types[type_count++] =
            (opcode & 0x40U) ? ZM_OPERAND_VARIABLE
                             : ZM_OPERAND_SMALL_CONSTANT;
        types[type_count++] =
            (opcode & 0x20U) ? ZM_OPERAND_VARIABLE
                             : ZM_OPERAND_SMALL_CONSTANT;
    }

    instruction->operands_address = pc;
    instruction->operand_count_actual = type_count;

    for (i = 0U; i < type_count; ++i) {
        instruction->operands[i].type = types[i];

        if (types[i] == ZM_OPERAND_LARGE_CONSTANT) {
            if (!read_u16(memory, memory_size, &pc,
                          &instruction->operands[i].value,
                          error, error_size)) {
                return 0;
            }
        } else {
            uint8_t value;

            if (!read_u8(memory, memory_size, &pc, &value,
                         error, error_size)) {
                return 0;
            }
            instruction->operands[i].value = value;
        }
    }

    instruction->next_pc = pc;
    return 1;
}
