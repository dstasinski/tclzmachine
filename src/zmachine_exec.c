#include "tclzmachine.h"
#include "zmachine_exec.h"

#include <stdio.h>
#include <string.h>

static void exec_error(ZMachine *vm, const char *message)
{
    if (!vm) {
        return;
    }
    vm->state = ZM_STATE_ERROR;
    snprintf(vm->error, sizeof(vm->error), "%s", message);
}

static int read_byte(const ZMachine *vm, uint32_t address, uint8_t *value)
{
    if (!vm || !vm->memory || !value || (size_t)address >= vm->memory_size) {
        return TCL_ERROR;
    }
    *value = vm->memory[address];
    return TCL_OK;
}

static int read_word(const ZMachine *vm, uint32_t address, uint16_t *value)
{
    if (!vm || !vm->memory || !value ||
        (size_t)address + 1U >= vm->memory_size) {
        return TCL_ERROR;
    }
    *value = (uint16_t)(((uint16_t)vm->memory[address] << 8) |
                        vm->memory[address + 1U]);
    return TCL_OK;
}

int zmachine_resolve_operands(ZMachine *vm,
                              const ZMachineInstruction *instruction,
                              uint16_t *values,
                              size_t values_capacity)
{
    uint8_t i;

    if (!vm || !instruction ||
        (instruction->operand_count_actual > 0U && !values) ||
        values_capacity < instruction->operand_count_actual) {
        if (vm) {
            exec_error(vm, "invalid operand-resolution request");
        }
        return TCL_ERROR;
    }

    for (i = 0U; i < instruction->operand_count_actual; ++i) {
        const ZMachineDecodedOperand *operand = &instruction->operands[i];

        switch (operand->type) {
        case ZM_OPERAND_LARGE_CONSTANT:
        case ZM_OPERAND_SMALL_CONSTANT:
            values[i] = operand->value;
            break;

        case ZM_OPERAND_VARIABLE:
            if (zmachine_variable_read(vm, (uint8_t)operand->value, 0,
                                       &values[i]) != TCL_OK) {
                return TCL_ERROR;
            }
            break;

        default:
            exec_error(vm, "attempt to resolve an omitted Z-machine operand");
            return TCL_ERROR;
        }
    }

    return TCL_OK;
}

int zmachine_call_routine(ZMachine *vm,
                          uint16_t packed_address,
                          const uint16_t *arguments,
                          uint8_t argument_count,
                          uint32_t return_pc,
                          uint8_t store_variable,
                          int discard_result)
{
    uint32_t address;
    uint32_t cursor;
    uint16_t locals[ZM_MAX_LOCALS];
    uint8_t local_count;
    uint8_t i;
    uint8_t argument_mask = 0U;

    if (!vm || !vm->memory) {
        if (vm) {
            exec_error(vm, "cannot call routine without a loaded story");
        }
        return TCL_ERROR;
    }

    if (argument_count > 7U) {
        exec_error(vm, "Z-machine routine call has more than 7 arguments");
        return TCL_ERROR;
    }

    if (packed_address == 0U) {
        vm->pc = return_pc;
        if (!discard_result &&
            zmachine_variable_write(vm, store_variable, 0, 0U) != TCL_OK) {
            return TCL_ERROR;
        }
        return TCL_OK;
    }

    address = zmachine_unpack_routine_address(vm, packed_address);
    if ((size_t)address >= vm->memory_size) {
        exec_error(vm, "Z-machine routine address is outside story memory");
        return TCL_ERROR;
    }

    cursor = address;
    if (read_byte(vm, cursor++, &local_count) != TCL_OK ||
        local_count > ZM_MAX_LOCALS) {
        exec_error(vm, "invalid Z-machine routine header");
        return TCL_ERROR;
    }

    memset(locals, 0, sizeof(locals));

    if (vm->version <= 4U) {
        for (i = 0U; i < local_count; ++i) {
            if (read_word(vm, cursor, &locals[i]) != TCL_OK) {
                exec_error(vm, "truncated Z-machine routine local defaults");
                return TCL_ERROR;
            }
            cursor += 2U;
        }
    }

    for (i = 0U; i < argument_count && i < local_count; ++i) {
        locals[i] = arguments ? arguments[i] : 0U;
    }

    for (i = 0U; i < argument_count; ++i) {
        argument_mask = (uint8_t)(argument_mask | (uint8_t)(1U << i));
    }

    if (zmachine_frame_push(vm, return_pc, store_variable, discard_result,
                            locals, local_count, argument_mask) != TCL_OK) {
        return TCL_ERROR;
    }

    vm->pc = cursor;
    return TCL_OK;
}

int zmachine_return(ZMachine *vm, uint16_t value)
{
    ZMachineFrame frame;

    if (!vm) {
        return TCL_ERROR;
    }

    if (zmachine_frame_pop(vm, &frame) != TCL_OK) {
        return TCL_ERROR;
    }

    vm->pc = frame.return_pc;
    if (!frame.discard_result &&
        zmachine_variable_write(vm, frame.store_variable, 0, value) != TCL_OK) {
        return TCL_ERROR;
    }

    return TCL_OK;
}

static int read_store_variable(ZMachine *vm,
                               const ZMachineInstruction *instruction,
                               uint8_t *store_variable,
                               uint32_t *return_pc)
{
    if (read_byte(vm, instruction->next_pc, store_variable) != TCL_OK) {
        exec_error(vm, "truncated Z-machine store instruction");
        return TCL_ERROR;
    }
    *return_pc = instruction->next_pc + 1U;
    return TCL_OK;
}

static int execute_call(ZMachine *vm,
                        const ZMachineInstruction *instruction,
                        const uint16_t *values,
                        int discard_result)
{
    uint8_t store_variable = 0U;
    uint32_t return_pc = instruction->next_pc;
    uint8_t argument_count;

    if (instruction->operand_count_actual < 1U) {
        exec_error(vm, "Z-machine call instruction has no routine operand");
        return TCL_ERROR;
    }

    if (!discard_result &&
        read_store_variable(vm, instruction, &store_variable, &return_pc) != TCL_OK) {
        return TCL_ERROR;
    }

    argument_count = (uint8_t)(instruction->operand_count_actual - 1U);
    return zmachine_call_routine(vm, values[0], values + 1,
                                 argument_count, return_pc,
                                 store_variable, discard_result);
}

int zmachine_step(ZMachine *vm)
{
    ZMachineInstruction instruction;
    uint16_t values[ZM_MAX_OPERANDS];
    char decode_error_text[128];

    if (!vm || !vm->memory) {
        if (vm) {
            exec_error(vm, "cannot execute without a loaded story");
        }
        return TCL_ERROR;
    }

    if (vm->state == ZM_STATE_HALTED) {
        return TCL_OK;
    }

    if (!zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                     vm->pc, &instruction,
                                     decode_error_text,
                                     sizeof(decode_error_text))) {
        exec_error(vm, decode_error_text[0] ? decode_error_text
                                            : "unable to decode Z-machine instruction");
        return TCL_ERROR;
    }

    if (zmachine_resolve_operands(vm, &instruction, values,
                                  ZM_MAX_OPERANDS) != TCL_OK) {
        return TCL_ERROR;
    }

    if (instruction.operand_count == ZM_OPERANDS_0OP) {
        switch (instruction.opcode_number) {
        case 0U: /* rtrue */
            return zmachine_return(vm, 1U);
        case 1U: /* rfalse */
            return zmachine_return(vm, 0U);
        case 4U: /* nop */
            vm->pc = instruction.next_pc;
            return TCL_OK;
        case 8U: { /* ret_popped */
            uint16_t value;
            if (zmachine_stack_pop(vm, &value) != TCL_OK) {
                return TCL_ERROR;
            }
            return zmachine_return(vm, value);
        }
        case 10U: /* quit */
            vm->pc = instruction.next_pc;
            vm->state = ZM_STATE_HALTED;
            return TCL_OK;
        default:
            break;
        }
    }

    if (instruction.operand_count == ZM_OPERANDS_1OP) {
        if (instruction.opcode_number == 11U) { /* ret */
            return zmachine_return(vm, values[0]);
        }
        if (instruction.opcode_number == 8U && vm->version >= 4U) { /* call_1s */
            return execute_call(vm, &instruction, values, 0);
        }
        if (instruction.opcode_number == 15U && vm->version >= 5U) { /* call_1n */
            return execute_call(vm, &instruction, values, 1);
        }
    }

    if (instruction.operand_count == ZM_OPERANDS_2OP) {
        if (instruction.opcode_number == 25U && vm->version >= 4U) { /* call_2s */
            return execute_call(vm, &instruction, values, 0);
        }
        if (instruction.opcode_number == 26U && vm->version >= 5U) { /* call_2n */
            return execute_call(vm, &instruction, values, 1);
        }
    }

    if (instruction.operand_count == ZM_OPERANDS_VAR) {
        if (instruction.opcode_number == 0U) { /* call / call_vs */
            return execute_call(vm, &instruction, values, 0);
        }
        if (instruction.opcode_number == 12U && vm->version >= 4U) { /* call_vs2 */
            return execute_call(vm, &instruction, values, 0);
        }
        if (instruction.opcode_number == 25U && vm->version >= 5U) { /* call_vn */
            return execute_call(vm, &instruction, values, 1);
        }
        if (instruction.opcode_number == 26U && vm->version >= 5U) { /* call_vn2 */
            return execute_call(vm, &instruction, values, 1);
        }
    }

    snprintf(vm->error, sizeof(vm->error),
             "unsupported Z-machine opcode form=%u count=%u opcode=%u at 0x%lx",
             (unsigned)instruction.form,
             (unsigned)instruction.operand_count,
             (unsigned)instruction.opcode_number,
             (unsigned long)instruction.address);
    vm->state = ZM_STATE_ERROR;
    return TCL_ERROR;
}
