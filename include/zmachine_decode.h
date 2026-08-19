#ifndef ZMACHINE_DECODE_H
#define ZMACHINE_DECODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZM_MAX_OPERANDS 8U

typedef enum ZMachineInstructionForm {
    ZM_FORM_LONG = 0,
    ZM_FORM_SHORT,
    ZM_FORM_VARIABLE,
    ZM_FORM_EXTENDED
} ZMachineInstructionForm;

typedef enum ZMachineOperandCount {
    ZM_OPERANDS_0OP = 0,
    ZM_OPERANDS_1OP,
    ZM_OPERANDS_2OP,
    ZM_OPERANDS_VAR
} ZMachineOperandCount;

typedef enum ZMachineOperandType {
    ZM_OPERAND_LARGE_CONSTANT = 0,
    ZM_OPERAND_SMALL_CONSTANT = 1,
    ZM_OPERAND_VARIABLE = 2,
    ZM_OPERAND_OMITTED = 3
} ZMachineOperandType;

typedef struct ZMachineDecodedOperand {
    ZMachineOperandType type;
    uint16_t value;
} ZMachineDecodedOperand;

typedef struct ZMachineInstruction {
    uint32_t address;
    uint32_t operands_address;
    uint32_t next_pc;

    uint8_t opcode_byte;
    uint8_t opcode_number;
    ZMachineInstructionForm form;
    ZMachineOperandCount operand_count;

    uint8_t type_byte_count;
    uint8_t operand_count_actual;
    ZMachineDecodedOperand operands[ZM_MAX_OPERANDS];
} ZMachineInstruction;

/*
 * Decode the opcode, operand-type bytes, and raw operands at pc.
 *
 * Variable operands remain variable numbers; they are intentionally not
 * resolved here. Store variables, branch data, and inline text are opcode
 * semantics and are likewise not consumed by this low-level decoder.
 *
 * Returns 1 on success and 0 on malformed or truncated input.
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
