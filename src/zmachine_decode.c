/*
 * zmachine_decode.c
 *
 * Byte-level decoder for Z-machine instructions.
 *
 * The decoder recognizes LONG, SHORT, VARIABLE, and V5+ EXTENDED encodings,
 * extracts operand type information, and reads raw operand fields.  It stops
 * exactly at the end of the operand list so the executor can subsequently
 * interpret opcode-specific store variables, branch data, or inline Z-text.
 */

#include "zmachine_decode.h"

#include <stdio.h>
#include <string.h>

/* Copy one decoder diagnostic into the caller-provided buffer when available. */
static void decode_error(char *error, size_t error_size, const char *message)
{
    if (error && error_size > 0U)
        snprintf(error, error_size, "%s", message);
}

/*
 * Read one byte from story memory and advance the local decoder cursor.
 * Return 0 rather than reading beyond the supplied story image.
 */
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

/* Read one big-endian 16-bit operand and advance the decoder cursor twice. */
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
        !read_u8(memory, memory_size, pc, &lo, error, error_size))
        return 0;

    *value = (uint16_t)(((uint16_t)hi << 8) | lo);
    return 1;
}

/*
 * Append one decoded operand type to the temporary type list.
 *
 * An OMITTED code ends the logical operand list.  The standard does not permit
 * a later non-omitted operand in the same type-byte sequence, so detecting one
 * here catches malformed instructions before any operand bytes are consumed.
 */
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

/*
 * Decode the four two-bit operand type fields in one type byte, most
 * significant field first, matching their order in the instruction stream.
 */
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
                         error, error_size))
            return 0;
    }

    return 1;
}

/* Decode one complete opcode and its raw operands from story memory. */
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

    if (error && error_size > 0U)
        error[0] = '\0';

    if (!memory || !instruction) {
        decode_error(error, error_size, "invalid decoder argument");
        return 0;
    }

    memset(instruction, 0, sizeof(*instruction));
    instruction->address = pc;

    if (!read_u8(memory, memory_size, &pc, &opcode, error, error_size))
        return 0;

    instruction->opcode_byte = opcode;

    /*
     * In V5+, 0xBE is not a SHORT-form 0OP instruction; it is the prefix for
     * the extended opcode table.  The following byte is the EXT opcode number
     * and the next byte is its operand-type descriptor.
     */
    if (opcode == 0xBEU && version >= 5U) {
        instruction->form = ZM_FORM_EXTENDED;
        instruction->operand_count = ZM_OPERANDS_VAR;

        if (!read_u8(memory, memory_size, &pc,
                     &instruction->opcode_number, error, error_size) ||
            !read_u8(memory, memory_size, &pc,
                     &type_byte, error, error_size))
            return 0;

        instruction->type_byte_count = 1U;
        if (!decode_type_byte(type_byte, types, &type_count, &omitted_seen,
                              error, error_size))
            return 0;

    /*
     * VARIABLE form begins with binary 11xxxxxx.  Bit 5 selects VAR versus
     * variable-form 2OP; the low five bits carry the opcode number.
     */
    } else if ((opcode & 0xC0U) == 0xC0U) {
        instruction->form = ZM_FORM_VARIABLE;
        instruction->opcode_number = opcode & 0x1FU;
        instruction->operand_count =
            (opcode & 0x20U) ? ZM_OPERANDS_VAR : ZM_OPERANDS_2OP;

        if (!read_u8(memory, memory_size, &pc, &type_byte,
                     error, error_size))
            return 0;

        instruction->type_byte_count = 1U;
        if (!decode_type_byte(type_byte, types, &type_count, &omitted_seen,
                              error, error_size))
            return 0;

        /*
         * Only VAR:12 call_vs2 and VAR:26 call_vn2 use a second type byte,
         * allowing up to eight operands.  Variable-form 2OP opcodes never do.
         */
        if (instruction->operand_count == ZM_OPERANDS_VAR &&
            (instruction->opcode_number == 12U ||
             instruction->opcode_number == 26U)) {
            if (!read_u8(memory, memory_size, &pc, &type_byte,
                         error, error_size))
                return 0;

            instruction->type_byte_count = 2U;
            if (!decode_type_byte(type_byte, types, &type_count,
                                  &omitted_seen, error, error_size))
                return 0;
        }

    /*
     * SHORT form begins with binary 10xxxxxx.  Bits 5..4 contain one operand
     * type; OMITTED means the instruction belongs to the 0OP table.
     */
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

    /*
     * LONG form is everything else.  It always has two operands; bits 6 and 5
     * choose VARIABLE versus SMALL CONSTANT independently for operands 1 and 2.
     */
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

    /*
     * Read raw operand fields in source order.  LARGE CONSTANT consumes two
     * bytes; SMALL CONSTANT and VARIABLE both consume one.  Variable operands
     * remain unresolved variable numbers until the execution layer evaluates
     * them left-to-right.
     */
    for (i = 0U; i < type_count; ++i) {
        instruction->operands[i].type = types[i];

        if (types[i] == ZM_OPERAND_LARGE_CONSTANT) {
            if (!read_u16(memory, memory_size, &pc,
                          &instruction->operands[i].value,
                          error, error_size))
                return 0;
        } else {
            uint8_t value;

            if (!read_u8(memory, memory_size, &pc, &value,
                         error, error_size))
                return 0;
            instruction->operands[i].value = value;
        }
    }

    /*
     * Compatibility normalization for the released Galatea Z8.
     *
     * Standard read_char is VAR:22 with keyboard device 1 as its first operand.
     * Galatea contains a historically assembled form whose type byte omits all
     * operands. Treat that one malformed encoding as if it had encoded small
     * constant 1. No story byte is consumed, so next_pc still points at the
     * original store-variable byte and every downstream execution layer sees
     * the same Standard-equivalent instruction.
     */
    if (version >= 4U &&
        instruction->form == ZM_FORM_VARIABLE &&
        instruction->operand_count == ZM_OPERANDS_VAR &&
        instruction->opcode_number == 22U &&
        instruction->operand_count_actual == 0U) {
        instruction->operands[0].type = ZM_OPERAND_SMALL_CONSTANT;
        instruction->operands[0].value = 1U;
        instruction->operand_count_actual = 1U;
    }

    /*
     * Compatibility fallback for stories which request timed input even though
     * this interpreter correctly advertises that timed keyboard input is not
     * available (Flags 1 bit 7 is clear).
     *
     * Older Inform-era stories sometimes issue timed read/read_char forms
     * without honoring that capability bit.  Aborting makes those otherwise
     * playable stories unusable.  Preserve the encoded operand count and byte
     * length, but normalize the optional time/routine operands to literal zero.
     * Downstream preflight and every input execution path therefore see an
     * ordinary untimed request, while next_pc remains at the exact encoded end
     * of the instruction.  Because the feature is explicitly unavailable, the
     * ignored timer/callback operands have no supported semantic side effects.
     */
    if (version >= 4U &&
        instruction->form == ZM_FORM_VARIABLE &&
        instruction->operand_count == ZM_OPERANDS_VAR) {
        if (instruction->opcode_number == 4U) { /* read text parse [time routine] */
            if (instruction->operand_count_actual >= 3U) {
                instruction->operands[2].type = ZM_OPERAND_SMALL_CONSTANT;
                instruction->operands[2].value = 0U;
            }
            if (instruction->operand_count_actual >= 4U) {
                instruction->operands[3].type = ZM_OPERAND_SMALL_CONSTANT;
                instruction->operands[3].value = 0U;
            }
        } else if (instruction->opcode_number == 22U) { /* read_char 1 [time routine] */
            if (instruction->operand_count_actual >= 2U) {
                instruction->operands[1].type = ZM_OPERAND_SMALL_CONSTANT;
                instruction->operands[1].value = 0U;
            }
            if (instruction->operand_count_actual >= 3U) {
                instruction->operands[2].type = ZM_OPERAND_SMALL_CONSTANT;
                instruction->operands[2].value = 0U;
            }
        }
    }

    /* Opcode-specific trailing data begins at next_pc. */
    instruction->next_pc = pc;
    return 1;
}
