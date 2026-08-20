/*
 * zmachine_decode.h
 *
 * Low-level Z-machine instruction decoding.
 *
 * This layer knows only how instruction bytes are shaped: opcode form,
 * operand-count class, operand type bytes, and raw operand fields.  It does not
 * resolve variable operands, consume store variables, decode branch records,
 * interpret inline text, or execute opcodes.  Keeping those responsibilities
 * separate makes instruction length and version-dependent encoding easier to
 * test independently from VM semantics.
 */

#ifndef ZMACHINE_DECODE_H
#define ZMACHINE_DECODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* call_vs2/call_vn2 are the only standard instructions with up to 8 operands. */
#define ZM_MAX_OPERANDS 8U

/* Physical instruction encodings identified by the high bits of the opcode. */
typedef enum ZMachineInstructionForm {
    ZM_FORM_LONG = 0,   /* Two small-constant/variable operands encoded inline. */
    ZM_FORM_SHORT,      /* 0OP or 1OP, with operand type in opcode bits 5..4. */
    ZM_FORM_VARIABLE,   /* Operand types follow in one or two type bytes. */
    ZM_FORM_EXTENDED    /* V5+ 0xBE prefix followed by extended opcode number. */
} ZMachineInstructionForm;

/* Logical opcode families used by the Z-machine opcode tables. */
typedef enum ZMachineOperandCount {
    ZM_OPERANDS_0OP = 0,
    ZM_OPERANDS_1OP,
    ZM_OPERANDS_2OP,
    ZM_OPERANDS_VAR
} ZMachineOperandCount;

/* Two-bit operand type codes stored in variable-form type bytes. */
typedef enum ZMachineOperandType {
    ZM_OPERAND_LARGE_CONSTANT = 0, /* 16-bit immediate value. */
    ZM_OPERAND_SMALL_CONSTANT = 1, /* 8-bit immediate value. */
    ZM_OPERAND_VARIABLE = 2,       /* 8-bit variable number, not its contents. */
    ZM_OPERAND_OMITTED = 3         /* End of the operand list. */
} ZMachineOperandType;

/* One raw operand as it appears in the instruction stream. */
typedef struct ZMachineDecodedOperand {
    ZMachineOperandType type;
    uint16_t value; /* Constant value or unresolved variable number. */
} ZMachineDecodedOperand;

/*
 * Complete decoder result for one instruction.
 *
 * address is the opcode byte address. operands_address is the first raw operand
 * byte after any opcode/type bytes. next_pc points immediately after the raw
 * operands; opcode-specific store/branch/text data begins there when present.
 */
typedef struct ZMachineInstruction {
    uint32_t address;
    uint32_t operands_address;
    uint32_t next_pc;

    uint8_t opcode_byte;        /* Original first opcode byte. */
    uint8_t opcode_number;      /* Logical opcode number within its family. */
    ZMachineInstructionForm form;
    ZMachineOperandCount operand_count;

    uint8_t type_byte_count;    /* 0, 1, or 2 physical operand-type bytes. */
    uint8_t operand_count_actual;
    ZMachineDecodedOperand operands[ZM_MAX_OPERANDS];
} ZMachineInstruction;

/*
 * Decode one instruction beginning at pc.
 *
 * memory/memory_size describe the complete story image. version is required
 * because opcode byte 0xBE becomes the EXT prefix only in V5 and later.
 * instruction receives the decoded form and raw operands. error, when non-NULL
 * and error_size > 0, receives a short diagnostic for malformed or truncated
 * input.
 *
 * Variable operands intentionally remain variable numbers. Store variables,
 * branch data, and inline Z-text are opcode semantics and are not consumed by
 * this low-level decoder.
 *
 * Return 1 on success and 0 on invalid arguments, malformed type bytes, too
 * many operands, or truncated story memory.
 */
int zmachine_decode_instruction(const uint8_t *memory,
                                size_t memory_size,
                                uint8_t version,
                                uint32_t pc,
                                ZMachineInstruction *instruction,
                                char *error,
                                size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
