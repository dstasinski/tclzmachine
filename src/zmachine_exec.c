#include "tclzmachine.h"
#include "zmachine_exec.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static void exec_error(ZMachine *vm, const char *message)
{
    if (!vm) return;
    vm->state = ZM_STATE_ERROR;
    snprintf(vm->error, sizeof(vm->error), "%s", message);
}

static int read_byte(const ZMachine *vm, uint32_t address, uint8_t *value)
{
    if (!vm || !vm->memory || !value || (size_t)address >= vm->memory_size)
        return TCL_ERROR;
    *value = vm->memory[address];
    return TCL_OK;
}

static int read_word(const ZMachine *vm, uint32_t address, uint16_t *value)
{
    if (!vm || !vm->memory || !value || (size_t)address + 1U >= vm->memory_size)
        return TCL_ERROR;
    *value = (uint16_t)(((uint16_t)vm->memory[address] << 8) | vm->memory[address + 1U]);
    return TCL_OK;
}

static int write_byte(ZMachine *vm, uint32_t address, uint8_t value)
{
    if (!vm || !vm->memory || (size_t)address >= vm->memory_size ||
        (size_t)address >= (size_t)vm->static_memory_addr) {
        exec_error(vm, "Z-machine byte write is outside dynamic memory");
        return TCL_ERROR;
    }
    vm->memory[address] = value;
    return TCL_OK;
}

static int write_word(ZMachine *vm, uint32_t address, uint16_t value)
{
    if (!vm || !vm->memory || (size_t)address + 1U >= vm->memory_size ||
        (size_t)address + 1U >= (size_t)vm->static_memory_addr) {
        exec_error(vm, "Z-machine word write is outside dynamic memory");
        return TCL_ERROR;
    }
    vm->memory[address] = (uint8_t)(value >> 8);
    vm->memory[address + 1U] = (uint8_t)(value & 0xffU);
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
        if (vm) exec_error(vm, "invalid operand-resolution request");
        return TCL_ERROR;
    }
    for (i = 0U; i < instruction->operand_count_actual; ++i) {
        const ZMachineDecodedOperand *operand = &instruction->operands[i];
        if (operand->type == ZM_OPERAND_LARGE_CONSTANT ||
            operand->type == ZM_OPERAND_SMALL_CONSTANT) {
            values[i] = operand->value;
        } else if (operand->type == ZM_OPERAND_VARIABLE) {
            if (zmachine_variable_read(vm, (uint8_t)operand->value, 0, &values[i]) != TCL_OK)
                return TCL_ERROR;
        } else {
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
    uint32_t address, cursor;
    uint16_t locals[ZM_MAX_LOCALS];
    uint8_t local_count, i, argument_mask = 0U;

    if (!vm || !vm->memory) {
        if (vm) exec_error(vm, "cannot call routine without a loaded story");
        return TCL_ERROR;
    }
    if (argument_count > 7U) {
        exec_error(vm, "Z-machine routine call has more than 7 arguments");
        return TCL_ERROR;
    }
    if (packed_address == 0U) {
        vm->pc = return_pc;
        if (!discard_result && zmachine_variable_write(vm, store_variable, 0, 0U) != TCL_OK)
            return TCL_ERROR;
        return TCL_OK;
    }

    address = zmachine_unpack_routine_address(vm, packed_address);
    if ((size_t)address >= vm->memory_size) {
        exec_error(vm, "Z-machine routine address is outside story memory");
        return TCL_ERROR;
    }
    cursor = address;
    if (read_byte(vm, cursor++, &local_count) != TCL_OK || local_count > ZM_MAX_LOCALS) {
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
    for (i = 0U; i < argument_count && i < local_count; ++i)
        locals[i] = arguments ? arguments[i] : 0U;
    for (i = 0U; i < argument_count; ++i)
        argument_mask = (uint8_t)(argument_mask | (uint8_t)(1U << i));

    if (zmachine_frame_push(vm, return_pc, store_variable, discard_result,
                            locals, local_count, argument_mask) != TCL_OK)
        return TCL_ERROR;
    vm->pc = cursor;
    return TCL_OK;
}

int zmachine_return(ZMachine *vm, uint16_t value)
{
    ZMachineFrame frame;
    if (!vm) return TCL_ERROR;
    if (zmachine_frame_pop(vm, &frame) != TCL_OK) return TCL_ERROR;
    vm->pc = frame.return_pc;
    if (!frame.discard_result &&
        zmachine_variable_write(vm, frame.store_variable, 0, value) != TCL_OK)
        return TCL_ERROR;
    return TCL_OK;
}

static int store_result(ZMachine *vm, uint32_t store_pc, uint16_t value,
                        uint32_t *next_pc)
{
    uint8_t variable;
    if (read_byte(vm, store_pc, &variable) != TCL_OK) {
        exec_error(vm, "truncated Z-machine store instruction");
        return TCL_ERROR;
    }
    if (zmachine_variable_write(vm, variable, 0, value) != TCL_OK)
        return TCL_ERROR;
    if (next_pc) *next_pc = store_pc + 1U;
    return TCL_OK;
}

static int branch_result(ZMachine *vm, uint32_t branch_pc, int condition)
{
    uint8_t first, second = 0U;
    int branch_on_true;
    int32_t offset;
    uint32_t after;

    if (read_byte(vm, branch_pc, &first) != TCL_OK) {
        exec_error(vm, "truncated Z-machine branch");
        return TCL_ERROR;
    }
    branch_on_true = (first & 0x80U) != 0U;
    if (first & 0x40U) {
        offset = (int32_t)(first & 0x3fU);
        after = branch_pc + 1U;
    } else {
        uint16_t raw;
        if (read_byte(vm, branch_pc + 1U, &second) != TCL_OK) {
            exec_error(vm, "truncated Z-machine branch");
            return TCL_ERROR;
        }
        raw = (uint16_t)(((uint16_t)(first & 0x3fU) << 8) | second);
        if (raw & 0x2000U) raw |= 0xc000U;
        offset = (int16_t)raw;
        after = branch_pc + 2U;
    }

    if (!!condition != !!branch_on_true) {
        vm->pc = after;
        return TCL_OK;
    }
    if (offset == 0) return zmachine_return(vm, 0U);
    if (offset == 1) return zmachine_return(vm, 1U);
    vm->pc = (uint32_t)((int32_t)after + offset - 2);
    if ((size_t)vm->pc >= vm->memory_size) {
        exec_error(vm, "Z-machine branch target is outside story memory");
        return TCL_ERROR;
    }
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
    if (!discard_result) {
        if (read_byte(vm, instruction->next_pc, &store_variable) != TCL_OK) {
            exec_error(vm, "truncated Z-machine store instruction");
            return TCL_ERROR;
        }
        return_pc++;
    }
    argument_count = (uint8_t)(instruction->operand_count_actual - 1U);
    return zmachine_call_routine(vm, values[0], values + 1, argument_count,
                                 return_pc, store_variable, discard_result);
}

static int execute_store_binary(ZMachine *vm,
                                const ZMachineInstruction *instruction,
                                uint16_t value)
{
    uint32_t next_pc;
    if (store_result(vm, instruction->next_pc, value, &next_pc) != TCL_OK)
        return TCL_ERROR;
    vm->pc = next_pc;
    return TCL_OK;
}

static int signed_divmod(ZMachine *vm, int16_t a, int16_t b, int want_mod,
                         uint16_t *result)
{
    int32_t v;
    if (b == 0) {
        exec_error(vm, "division by zero in Z-machine instruction");
        return TCL_ERROR;
    }
    if (a == INT16_MIN && b == -1)
        v = want_mod ? 0 : 32768;
    else
        v = want_mod ? (a % b) : (a / b);
    *result = (uint16_t)v;
    return TCL_OK;
}

int zmachine_step(ZMachine *vm)
{
    ZMachineInstruction instruction;
    uint16_t values[ZM_MAX_OPERANDS];
    char decode_error_text[128];

    if (!vm || !vm->memory) {
        if (vm) exec_error(vm, "cannot execute without a loaded story");
        return TCL_ERROR;
    }
    if (vm->state == ZM_STATE_HALTED) return TCL_OK;

    if (!zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                     vm->pc, &instruction, decode_error_text,
                                     sizeof(decode_error_text))) {
        exec_error(vm, decode_error_text[0] ? decode_error_text :
                                            "unable to decode Z-machine instruction");
        return TCL_ERROR;
    }
    if (zmachine_resolve_operands(vm, &instruction, values, ZM_MAX_OPERANDS) != TCL_OK)
        return TCL_ERROR;

    if (instruction.operand_count == ZM_OPERANDS_0OP) {
        switch (instruction.opcode_number) {
        case 0U: return zmachine_return(vm, 1U); /* rtrue */
        case 1U: return zmachine_return(vm, 0U); /* rfalse */
        case 4U: vm->pc = instruction.next_pc; return TCL_OK; /* nop */
        case 8U: { /* ret_popped */
            uint16_t value;
            if (zmachine_stack_pop(vm, &value) != TCL_OK) return TCL_ERROR;
            return zmachine_return(vm, value);
        }
        case 10U: vm->pc = instruction.next_pc; vm->state = ZM_STATE_HALTED; return TCL_OK;
        default: break;
        }
    }

    if (instruction.operand_count == ZM_OPERANDS_1OP) {
        switch (instruction.opcode_number) {
        case 0U: /* jz */
            return branch_result(vm, instruction.next_pc, values[0] == 0U);
        case 5U: { /* inc (variable) */
            uint8_t var = (uint8_t)values[0];
            uint16_t v;
            if (zmachine_variable_read(vm, var, 1, &v) != TCL_OK) return TCL_ERROR;
            v = (uint16_t)(v + 1U);
            if (zmachine_variable_write(vm, var, 1, v) != TCL_OK) return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        }
        case 6U: { /* dec (variable) */
            uint8_t var = (uint8_t)values[0];
            uint16_t v;
            if (zmachine_variable_read(vm, var, 1, &v) != TCL_OK) return TCL_ERROR;
            v = (uint16_t)(v - 1U);
            if (zmachine_variable_write(vm, var, 1, v) != TCL_OK) return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        }
        case 8U:
            if (vm->version >= 4U) return execute_call(vm, &instruction, values, 0);
            break;
        case 11U: return zmachine_return(vm, values[0]); /* ret */
        case 12U: { /* jump */
            int16_t offset = (int16_t)values[0];
            vm->pc = (uint32_t)((int32_t)instruction.next_pc + offset - 2);
            if ((size_t)vm->pc >= vm->memory_size) {
                exec_error(vm, "Z-machine jump target is outside story memory");
                return TCL_ERROR;
            }
            return TCL_OK;
        }
        case 14U: { /* load (variable) -> result */
            uint16_t v;
            if (zmachine_variable_read(vm, (uint8_t)values[0], 1, &v) != TCL_OK)
                return TCL_ERROR;
            return execute_store_binary(vm, &instruction, v);
        }
        case 15U:
            if (vm->version <= 4U) return execute_store_binary(vm, &instruction, (uint16_t)~values[0]);
            return execute_call(vm, &instruction, values, 1);
        default: break;
        }
    }

    if (instruction.operand_count == ZM_OPERANDS_2OP) {
        switch (instruction.opcode_number) {
        case 1U: { /* je */
            uint8_t i;
            int equal = 0;
            for (i = 1U; i < instruction.operand_count_actual; ++i)
                if (values[0] == values[i]) equal = 1;
            return branch_result(vm, instruction.next_pc, equal);
        }
        case 2U: return branch_result(vm, instruction.next_pc,
                                      (int16_t)values[0] < (int16_t)values[1]);
        case 3U: return branch_result(vm, instruction.next_pc,
                                      (int16_t)values[0] > (int16_t)values[1]);
        case 4U: { /* dec_chk */
            uint8_t var = (uint8_t)values[0];
            uint16_t v;
            if (zmachine_variable_read(vm, var, 1, &v) != TCL_OK) return TCL_ERROR;
            v = (uint16_t)(v - 1U);
            if (zmachine_variable_write(vm, var, 1, v) != TCL_OK) return TCL_ERROR;
            return branch_result(vm, instruction.next_pc, (int16_t)v < (int16_t)values[1]);
        }
        case 5U: { /* inc_chk */
            uint8_t var = (uint8_t)values[0];
            uint16_t v;
            if (zmachine_variable_read(vm, var, 1, &v) != TCL_OK) return TCL_ERROR;
            v = (uint16_t)(v + 1U);
            if (zmachine_variable_write(vm, var, 1, v) != TCL_OK) return TCL_ERROR;
            return branch_result(vm, instruction.next_pc, (int16_t)v > (int16_t)values[1]);
        }
        case 7U: return branch_result(vm, instruction.next_pc,
                                      (values[0] & values[1]) == values[1]);
        case 8U: return execute_store_binary(vm, &instruction, (uint16_t)(values[0] | values[1]));
        case 9U: return execute_store_binary(vm, &instruction, (uint16_t)(values[0] & values[1]));
        case 13U: /* store (variable) value */
            if (zmachine_variable_write(vm, (uint8_t)values[0], 1, values[1]) != TCL_OK)
                return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        case 15U: { /* loadw */
            uint32_t addr = (uint32_t)values[0] + 2U * (uint32_t)values[1];
            uint16_t v;
            if (read_word(vm, addr, &v) != TCL_OK) {
                exec_error(vm, "Z-machine loadw address is outside story memory");
                return TCL_ERROR;
            }
            return execute_store_binary(vm, &instruction, v);
        }
        case 16U: { /* loadb */
            uint32_t addr = (uint32_t)values[0] + (uint32_t)values[1];
            uint8_t v;
            if (read_byte(vm, addr, &v) != TCL_OK) {
                exec_error(vm, "Z-machine loadb address is outside story memory");
                return TCL_ERROR;
            }
            return execute_store_binary(vm, &instruction, v);
        }
        case 20U: return execute_store_binary(vm, &instruction,
                                              (uint16_t)(values[0] + values[1]));
        case 21U: return execute_store_binary(vm, &instruction,
                                              (uint16_t)(values[0] - values[1]));
        case 22U: return execute_store_binary(vm, &instruction,
                                              (uint16_t)((int32_t)(int16_t)values[0] *
                                                         (int32_t)(int16_t)values[1]));
        case 23U: { uint16_t r; if (signed_divmod(vm, (int16_t)values[0], (int16_t)values[1], 0, &r) != TCL_OK) return TCL_ERROR; return execute_store_binary(vm, &instruction, r); }
        case 24U: { uint16_t r; if (signed_divmod(vm, (int16_t)values[0], (int16_t)values[1], 1, &r) != TCL_OK) return TCL_ERROR; return execute_store_binary(vm, &instruction, r); }
        case 25U: if (vm->version >= 4U) return execute_call(vm, &instruction, values, 0); break;
        case 26U: if (vm->version >= 5U) return execute_call(vm, &instruction, values, 1); break;
        default: break;
        }
    }

    if (instruction.operand_count == ZM_OPERANDS_VAR) {
        switch (instruction.opcode_number) {
        case 0U: return execute_call(vm, &instruction, values, 0);
        case 1U: { /* storew */
            uint32_t addr = (uint32_t)values[0] + 2U * (uint32_t)values[1];
            if (write_word(vm, addr, values[2]) != TCL_OK) return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        }
        case 2U: { /* storeb */
            uint32_t addr = (uint32_t)values[0] + (uint32_t)values[1];
            if (write_byte(vm, addr, (uint8_t)values[2]) != TCL_OK) return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        }
        case 8U: /* push */
            if (zmachine_stack_push(vm, values[0]) != TCL_OK) return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        case 9U: { /* pull (variable) */
            uint16_t v;
            if (zmachine_stack_pop(vm, &v) != TCL_OK) return TCL_ERROR;
            if (zmachine_variable_write(vm, (uint8_t)values[0], 1, v) != TCL_OK) return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        }
        case 12U: if (vm->version >= 4U) return execute_call(vm, &instruction, values, 0); break;
        case 25U: if (vm->version >= 5U) return execute_call(vm, &instruction, values, 1); break;
        case 26U: if (vm->version >= 5U) return execute_call(vm, &instruction, values, 1); break;
        default: break;
        }
    }

    snprintf(vm->error, sizeof(vm->error),
             "unsupported Z-machine opcode form=%u count=%u opcode=%u at 0x%lx",
             (unsigned)instruction.form, (unsigned)instruction.operand_count,
             (unsigned)instruction.opcode_number, (unsigned long)instruction.address);
    vm->state = ZM_STATE_ERROR;
    return TCL_ERROR;
}
