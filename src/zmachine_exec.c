/*
 * zmachine_exec.c
 *
 * Core non-blocking Z-machine instruction execution.
 *
 * This module receives already-decoded instructions, evaluates operands in
 * specification order, performs routine calls/returns, applies store and branch
 * records, and implements ordinary VM opcodes whose behavior belongs in the C
 * execution core. Operations which can suspend for host interaction are layered
 * above this file: line input is coordinated by the run loop, file selection by
 * zmachine_file.c, lexical VAR opcodes by zmachine_tokenise.c, and text-only
 * presentation adaptations by zmachine_dispatch.c.
 *
 * A key semantic boundary is worth making explicit: decoding identifies an
 * operand as a constant or variable reference, while resolving it obtains the
 * operand's value. A VARIABLE operand naming variable 0 therefore pops the
 * evaluation stack during resolution. Opcodes whose resolved operand denotes a
 * variable number (`inc`, `dec`, `load`, `store`, `inc_chk`, `dec_chk`, `pull`)
 * then perform their target access with indirect=1; if that resulting variable
 * number is 0, the target operation peeks/replaces the stack rather than causing
 * another ordinary variable-0 push/pop.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"
#include "zmachine_object.h"
#include "zmachine_property.h"
#include "zmachine_text.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Put the VM into its terminal error state with an executor diagnostic. */
static void exec_error(ZMachine *vm, const char *message)
{
    if (!vm) return;
    vm->state = ZM_STATE_ERROR;
    snprintf(vm->error, sizeof(vm->error), "%s", message);
}

/* Bounds-checked story-memory byte read. Reads may include static memory. */
static int read_byte(const ZMachine *vm, uint32_t address, uint8_t *value)
{
    if (!vm || !vm->memory || !value || (size_t)address >= vm->memory_size)
        return TCL_ERROR;
    *value = vm->memory[address];
    return TCL_OK;
}

/* Bounds-checked big-endian story-memory word read. */
static int read_word(const ZMachine *vm, uint32_t address, uint16_t *value)
{
    if (!vm || !vm->memory || !value || (size_t)address + 1U >= vm->memory_size)
        return TCL_ERROR;
    *value = (uint16_t)(((uint16_t)vm->memory[address] << 8) |
                       vm->memory[address + 1U]);
    return TCL_OK;
}

/* Write one byte, enforcing the Z-machine rule that only dynamic memory mutates. */
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

/* Write one big-endian word without permitting either byte into static memory. */
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

/*
 * Evaluate every decoded operand from left to right into caller-owned storage.
 *
 * Large/small constants already contain their values. VARIABLE operands are
 * ordinary value operands and therefore call zmachine_variable_read() with
 * indirect=0. This ordering is observable when variable 0 is used because each
 * such read pops the evaluation stack; callers must not pre-resolve and then
 * resolve the same instruction again.
 *
 * `values_capacity` prevents helpers with smaller temporary arrays from silently
 * accepting an instruction they cannot represent. Omitted operand markers are
 * decoder structure, never executable operands, and are rejected here.
 */
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
            if (zmachine_variable_read(vm, (uint8_t)operand->value, 0,
                                       &values[i]) != TCL_OK)
                return TCL_ERROR;
        } else {
            exec_error(vm, "attempt to resolve an omitted Z-machine operand");
            return TCL_ERROR;
        }
    }
    return TCL_OK;
}

/*
 * Enter a routine addressed by the version-dependent packed routine format.
 *
 * packed address zero is the standard null-routine call: no frame is created,
 * execution continues at return_pc, and storing calls receive false (0).
 *
 * A real routine begins with its local count. V1-V4 store one 16-bit default
 * value for each local in the routine header; V5+ omit those words and locals
 * begin at zero. Supplied call arguments overwrite locals 1..N. argument_mask
 * records which argument positions were actually supplied for check_arg_count.
 * The frame captures the caller's return PC, evaluation-stack base, result
 * destination, locals, and discard-result flag before execution jumps to the
 * first instruction after the routine header.
 */
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
        if (!discard_result &&
            zmachine_variable_write(vm, store_variable, 0, 0U) != TCL_OK)
            return TCL_ERROR;
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

/*
 * Return a value from the current routine.
 *
 * Popping the frame also restores the caller's evaluation-stack boundary. The
 * saved return PC becomes current, and storing call forms write the returned
 * value using ordinary variable semantics. call_n/call_vn-style frames simply
 * discard it.
 */
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

/*
 * Apply a one-byte store record at store_pc.
 * next_pc, when requested, receives the address immediately after the store
 * variable so combined store+branch opcodes can parse their branch there.
 */
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

/*
 * Decode and apply a branch record for the supplied boolean condition.
 *
 * Bit 7 selects whether the encoded condition branches on true or false. Bit 6
 * selects a one-byte 6-bit offset; otherwise a signed 14-bit offset follows.
 * Offsets are relative to the address after the branch data with the standard
 * -2 adjustment. Taken offsets 0 and 1 are special: they return false/true from
 * the current routine rather than changing PC. A non-taken branch simply
 * continues after its branch record.
 */
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

/* Store a result, then interpret the immediately following branch record. */
static int store_then_branch(ZMachine *vm, uint32_t store_pc,
                             uint16_t value, int condition)
{
    uint32_t branch_pc;
    if (store_result(vm, store_pc, value, &branch_pc) != TCL_OK)
        return TCL_ERROR;
    return branch_result(vm, branch_pc, condition);
}

/*
 * Execute any call-family instruction after its operands have been resolved.
 * Operand 0 is the packed routine address; later operands are call arguments.
 * Storing forms have a store-variable byte immediately after the instruction,
 * which must be included in the caller continuation. Non-storing forms leave
 * the continuation at instruction->next_pc and mark the frame discard_result.
 */
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
        ++return_pc;
    }
    argument_count = (uint8_t)(instruction->operand_count_actual - 1U);
    return zmachine_call_routine(vm, values[0], values + 1, argument_count,
                                 return_pc, store_variable, discard_result);
}

/* Convenience wrapper for ordinary store opcodes that have no branch record. */
static int execute_store(ZMachine *vm,
                         const ZMachineInstruction *instruction,
                         uint16_t value)
{
    uint32_t next_pc;
    if (store_result(vm, instruction->next_pc, value, &next_pc) != TCL_OK)
        return TCL_ERROR;
    vm->pc = next_pc;
    return TCL_OK;
}

/*
 * Perform signed 16-bit division or remainder with explicit VM edge handling.
 * Division by zero is a story error. INT16_MIN / -1 cannot be represented by a
 * C int16_t result, so arithmetic is promoted and the low 16 bits are stored as
 * the Z-machine value; its remainder is exactly zero.
 */
static int signed_divmod(ZMachine *vm, int16_t a, int16_t b,
                         int want_mod, uint16_t *result)
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

/*
 * Execute a 2OP opcode regardless of whether it was encoded in long form or
 * variable form. The Z-machine permits variable-form encodings for 2OP
 * instructions when large constants or more flexible operand types are needed;
 * the decoder's operand_count identifies the opcode table independently of the
 * physical encoding form.
 *
 * Return TCL_CONTINUE only when the opcode number is not implemented/valid for
 * the current version, allowing the top-level dispatcher to report one uniform
 * unsupported-opcode diagnostic.
 */
static int execute_2op(ZMachine *vm,
                       const ZMachineInstruction *instruction,
                       const uint16_t *values)
{
    switch (instruction->opcode_number) {
    /* je may legally compare the first operand against several later operands. */
    case 1U: {
        uint8_t i;
        int equal = 0;
        for (i = 1U; i < instruction->operand_count_actual; ++i)
            if (values[0] == values[i]) equal = 1;
        return branch_result(vm, instruction->next_pc, equal);
    }
    /* jl/jg are signed comparisons even though VM storage is uint16_t. */
    case 2U: return branch_result(vm, instruction->next_pc,
                                  (int16_t)values[0] < (int16_t)values[1]);
    case 3U: return branch_result(vm, instruction->next_pc,
                                  (int16_t)values[0] > (int16_t)values[1]);
    /*
     * dec_chk/inc_chk operand 0's resolved value is a variable number. The
     * indirect target access prevents variable-number 0 from causing another
     * normal stack pop/push after operand resolution.
     */
    case 4U: {
        uint8_t var = (uint8_t)values[0];
        uint16_t v;
        if (zmachine_variable_read(vm, var, 1, &v) != TCL_OK) return TCL_ERROR;
        v = (uint16_t)(v - 1U);
        if (zmachine_variable_write(vm, var, 1, v) != TCL_OK) return TCL_ERROR;
        return branch_result(vm, instruction->next_pc,
                             (int16_t)v < (int16_t)values[1]);
    }
    case 5U: {
        uint8_t var = (uint8_t)values[0];
        uint16_t v;
        if (zmachine_variable_read(vm, var, 1, &v) != TCL_OK) return TCL_ERROR;
        v = (uint16_t)(v + 1U);
        if (zmachine_variable_write(vm, var, 1, v) != TCL_OK) return TCL_ERROR;
        return branch_result(vm, instruction->next_pc,
                             (int16_t)v > (int16_t)values[1]);
    }
    case 6U: {
        uint16_t parent;
        if (zmachine_object_get_parent(vm, values[0], &parent) != TCL_OK)
            return TCL_ERROR;
        return branch_result(vm, instruction->next_pc, parent == values[1]);
    }
    case 7U: return branch_result(vm, instruction->next_pc,
                                  (values[0] & values[1]) == values[1]);
    case 8U: return execute_store(vm, instruction,
                                  (uint16_t)(values[0] | values[1]));
    case 9U: return execute_store(vm, instruction,
                                  (uint16_t)(values[0] & values[1]));
    case 10U: {
        int is_set;
        if (zmachine_object_test_attr(vm, values[0], (uint8_t)values[1],
                                      &is_set) != TCL_OK)
            return TCL_ERROR;
        return branch_result(vm, instruction->next_pc, is_set);
    }
    case 11U:
        if (zmachine_object_set_attr(vm, values[0], (uint8_t)values[1], 1) != TCL_OK)
            return TCL_ERROR;
        vm->pc = instruction->next_pc;
        return TCL_OK;
    case 12U:
        if (zmachine_object_set_attr(vm, values[0], (uint8_t)values[1], 0) != TCL_OK)
            return TCL_ERROR;
        vm->pc = instruction->next_pc;
        return TCL_OK;
    /* store's first resolved operand denotes the target variable indirectly. */
    case 13U:
        if (zmachine_variable_write(vm, (uint8_t)values[0], 1, values[1]) != TCL_OK)
            return TCL_ERROR;
        vm->pc = instruction->next_pc;
        return TCL_OK;
    case 14U:
        if (zmachine_object_insert(vm, values[0], values[1]) != TCL_OK)
            return TCL_ERROR;
        vm->pc = instruction->next_pc;
        return TCL_OK;
    case 15U: {
        uint32_t addr = (uint32_t)values[0] + 2U * (uint32_t)values[1];
        uint16_t v;
        if (read_word(vm, addr, &v) != TCL_OK) {
            exec_error(vm, "Z-machine loadw address is outside story memory");
            return TCL_ERROR;
        }
        return execute_store(vm, instruction, v);
    }
    case 16U: {
        uint32_t addr = (uint32_t)values[0] + (uint32_t)values[1];
        uint8_t v;
        if (read_byte(vm, addr, &v) != TCL_OK) {
            exec_error(vm, "Z-machine loadb address is outside story memory");
            return TCL_ERROR;
        }
        return execute_store(vm, instruction, v);
    }
    case 17U: {
        uint16_t value;
        if (zmachine_object_get_prop(vm, values[0], values[1], &value) != TCL_OK)
            return TCL_ERROR;
        return execute_store(vm, instruction, value);
    }
    case 18U: {
        uint16_t address;
        if (zmachine_object_get_prop_addr(vm, values[0], values[1], &address) != TCL_OK)
            return TCL_ERROR;
        return execute_store(vm, instruction, address);
    }
    case 19U: {
        uint16_t next;
        if (zmachine_object_get_next_prop(vm, values[0], values[1], &next) != TCL_OK)
            return TCL_ERROR;
        return execute_store(vm, instruction, next);
    }
    /* add/sub wrap naturally in the 16-bit Z-machine value domain. */
    case 20U: return execute_store(vm, instruction,
                                   (uint16_t)(values[0] + values[1]));
    case 21U: return execute_store(vm, instruction,
                                   (uint16_t)(values[0] - values[1]));
    case 22U: return execute_store(vm, instruction,
                                   (uint16_t)((int32_t)(int16_t)values[0] *
                                              (int32_t)(int16_t)values[1]));
    case 23U: {
        uint16_t r;
        if (signed_divmod(vm, (int16_t)values[0], (int16_t)values[1], 0, &r) != TCL_OK)
            return TCL_ERROR;
        return execute_store(vm, instruction, r);
    }
    case 24U: {
        uint16_t r;
        if (signed_divmod(vm, (int16_t)values[0], (int16_t)values[1], 1, &r) != TCL_OK)
            return TCL_ERROR;
        return execute_store(vm, instruction, r);
    }
    /* Later-version call variants reuse the common call machinery. */
    case 25U:
        if (vm->version >= 4U) return execute_call(vm, instruction, values, 0);
        break;
    case 26U:
        if (vm->version >= 5U) return execute_call(vm, instruction, values, 1);
        break;
    default:
        break;
    }
    return TCL_CONTINUE;
}

/*
 * Implement VAR:29 copy_table.
 *
 * Positive sizes use memmove semantics so overlapping ranges preserve the
 * source. Negative sizes always copy forward, which deliberately allows overlap
 * to affect later source bytes. A zero destination clears the source range.
 * Reads may originate anywhere in story memory; writes/clears must remain in
 * dynamic memory.
 */
static int execute_copy_table(ZMachine *vm, const uint16_t *values,
                              uint8_t operand_count, uint32_t next_pc)
{
    uint32_t first, second;
    int16_t signed_size;
    uint32_t length, i;

    if (operand_count < 3U) {
        exec_error(vm, "copy_table requires three operands");
        return TCL_ERROR;
    }
    first = values[0];
    second = values[1];
    signed_size = (int16_t)values[2];
    length = (uint32_t)(signed_size < 0 ? -(int32_t)signed_size : signed_size);

    if ((size_t)first + length > vm->memory_size) {
        exec_error(vm, "copy_table source is outside story memory");
        return TCL_ERROR;
    }
    if (second == 0U) {
        if ((size_t)first + length > (size_t)vm->static_memory_addr) {
            exec_error(vm, "copy_table clear exceeds dynamic memory");
            return TCL_ERROR;
        }
        memset(vm->memory + first, 0, length);
        vm->pc = next_pc;
        return TCL_OK;
    }
    if ((size_t)second + length > vm->memory_size ||
        (size_t)second + length > (size_t)vm->static_memory_addr) {
        exec_error(vm, "copy_table destination is outside dynamic memory");
        return TCL_ERROR;
    }

    if (signed_size >= 0) {
        memmove(vm->memory + second, vm->memory + first, length);
    } else {
        for (i = 0U; i < length; ++i)
            vm->memory[second + i] = vm->memory[first + i];
    }
    vm->pc = next_pc;
    return TCL_OK;
}

/*
 * Execute one ordinary, non-blocking instruction at vm->pc.
 *
 * This function is compiled under the symbol name `zmachine_step_core` by
 * CMake. Upper dispatch layers own cooperative file requests, tokenise/
 * encode_text, and text-only presentation behavior before delegating here.
 *
 * Operands are resolved once, before opcode-table dispatch, preserving the
 * Z-machine's left-to-right side effects. The decoder's operand_count selects
 * 0OP/1OP/2OP/VAR semantics; `form` is additionally checked before VAR dispatch
 * because EXT instructions intentionally share the decoder's VAR-sized operand
 * storage bucket but belong to a completely different opcode table.
 */
int zmachine_step(ZMachine *vm)
{
    ZMachineInstruction instruction;
    uint16_t values[ZM_MAX_OPERANDS];
    char decode_error_text[128];
    int rc;

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
    if (zmachine_resolve_operands(vm, &instruction, values,
                                  ZM_MAX_OPERANDS) != TCL_OK)
        return TCL_ERROR;

    /* 2OP is a logical opcode table, not necessarily the LONG encoding form. */
    if (instruction.operand_count == ZM_OPERANDS_2OP) {
        rc = execute_2op(vm, &instruction, values);
        if (rc != TCL_CONTINUE)
            return rc;
    }

    if (instruction.operand_count == ZM_OPERANDS_0OP) {
        switch (instruction.opcode_number) {
        case 0U: return zmachine_return(vm, 1U);  /* rtrue */
        case 1U: return zmachine_return(vm, 0U);  /* rfalse */
        case 2U: {                               /* print */
            uint32_t next;
            if (zmachine_text_print(vm, instruction.next_pc, &next) != TCL_OK)
                return TCL_ERROR;
            vm->pc = next;
            return TCL_OK;
        }
        case 3U: {                               /* print_ret */
            uint32_t next;
            if (zmachine_text_print(vm, instruction.next_pc, &next) != TCL_OK)
                return TCL_ERROR;
            if (zmachine_text_output_zscii(vm, 13U) != TCL_OK)
                return TCL_ERROR;
            vm->pc = next;
            return zmachine_return(vm, 1U);
        }
        case 4U: vm->pc = instruction.next_pc; return TCL_OK; /* nop */
        case 8U: {                               /* ret_popped */
            uint16_t value;
            if (zmachine_stack_pop(vm, &value) != TCL_OK) return TCL_ERROR;
            return zmachine_return(vm, value);
        }
        case 10U:                                /* quit */
            vm->pc = instruction.next_pc;
            vm->state = ZM_STATE_HALTED;
            return TCL_OK;
        case 11U:                                /* new_line */
            if (zmachine_text_output_zscii(vm, 13U) != TCL_OK) return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        default: break;
        }
    }

    if (instruction.operand_count == ZM_OPERANDS_1OP) {
        switch (instruction.opcode_number) {
        case 0U: return branch_result(vm, instruction.next_pc, values[0] == 0U); /* jz */
        case 1U: {                               /* get_sibling -> result ?branch */
            uint16_t sibling;
            if (zmachine_object_get_sibling(vm, values[0], &sibling) != TCL_OK)
                return TCL_ERROR;
            return store_then_branch(vm, instruction.next_pc, sibling, sibling != 0U);
        }
        case 2U: {                               /* get_child -> result ?branch */
            uint16_t child;
            if (zmachine_object_get_child(vm, values[0], &child) != TCL_OK)
                return TCL_ERROR;
            return store_then_branch(vm, instruction.next_pc, child, child != 0U);
        }
        case 3U: {                               /* get_parent -> result */
            uint16_t parent;
            if (zmachine_object_get_parent(vm, values[0], &parent) != TCL_OK)
                return TCL_ERROR;
            return execute_store(vm, &instruction, parent);
        }
        case 4U: {                               /* get_prop_len -> result */
            uint16_t length;
            if (zmachine_property_length_from_address(vm, values[0], &length) != TCL_OK) {
                exec_error(vm, "invalid property address in get_prop_len");
                return TCL_ERROR;
            }
            return execute_store(vm, &instruction, length);
        }
        /* inc/dec use an indirect variable number after normal operand resolution. */
        case 5U: {
            uint8_t var = (uint8_t)values[0];
            uint16_t v;
            if (zmachine_variable_read(vm, var, 1, &v) != TCL_OK) return TCL_ERROR;
            v = (uint16_t)(v + 1U);
            if (zmachine_variable_write(vm, var, 1, v) != TCL_OK) return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        }
        case 6U: {
            uint8_t var = (uint8_t)values[0];
            uint16_t v;
            if (zmachine_variable_read(vm, var, 1, &v) != TCL_OK) return TCL_ERROR;
            v = (uint16_t)(v - 1U);
            if (zmachine_variable_write(vm, var, 1, v) != TCL_OK) return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        }
        case 7U:                                 /* print_addr */
            if (zmachine_text_print(vm, values[0], NULL) != TCL_OK) return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        case 8U:                                 /* call_1s V4+ */
            if (vm->version >= 4U) return execute_call(vm, &instruction, values, 0);
            break;
        case 9U:                                 /* remove_obj */
            if (zmachine_object_remove(vm, values[0]) != TCL_OK) return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        case 10U:                                /* print_obj */
            if (zmachine_text_print_object_name(vm, values[0]) != TCL_OK) return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        case 11U: return zmachine_return(vm, values[0]); /* ret */
        case 12U: {                              /* jump: signed offset, same -2 convention */
            int16_t offset = (int16_t)values[0];
            vm->pc = (uint32_t)((int32_t)instruction.next_pc + offset - 2);
            if ((size_t)vm->pc >= vm->memory_size) {
                exec_error(vm, "Z-machine jump target is outside story memory");
                return TCL_ERROR;
            }
            return TCL_OK;
        }
        case 13U:                                /* print_paddr */
            if (zmachine_text_print_packed(vm, values[0]) != TCL_OK) return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        case 14U: {                              /* load (indirect variable) -> result */
            uint16_t v;
            if (zmachine_variable_read(vm, (uint8_t)values[0], 1, &v) != TCL_OK)
                return TCL_ERROR;
            return execute_store(vm, &instruction, v);
        }
        case 15U:
            /* Opcode 15 is `not` through V4 and becomes call_1n in V5+. */
            if (vm->version <= 4U)
                return execute_store(vm, &instruction, (uint16_t)~values[0]);
            return execute_call(vm, &instruction, values, 1);
        default: break;
        }
    }

    /*
     * EXTENDED instructions share the decoder's VAR operand-count bucket but
     * belong to a distinct opcode table. Keep them out of ordinary VAR dispatch;
     * implemented EXT behavior is consumed by upper layers before reaching core.
     */
    if (instruction.form != ZM_FORM_EXTENDED &&
        instruction.operand_count == ZM_OPERANDS_VAR) {
        switch (instruction.opcode_number) {
        case 0U: return execute_call(vm, &instruction, values, 0); /* call_vs */
        case 1U: {                               /* storew */
            uint32_t addr = (uint32_t)values[0] + 2U * (uint32_t)values[1];
            if (write_word(vm, addr, values[2]) != TCL_OK) return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        }
        case 2U: {                               /* storeb */
            uint32_t addr = (uint32_t)values[0] + (uint32_t)values[1];
            if (write_byte(vm, addr, (uint8_t)values[2]) != TCL_OK) return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        }
        case 3U:                                 /* put_prop */
            if (zmachine_object_put_prop(vm, values[0], values[1], values[2]) != TCL_OK)
                return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        case 5U:                                 /* print_char */
            if (zmachine_text_output_zscii(vm, values[0]) != TCL_OK) return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        case 6U: {                               /* print_num: signed decimal */
            char number[32];
            int n = snprintf(number, sizeof(number), "%d", (int)(int16_t)values[0]);
            if (n < 0 || (size_t)n >= sizeof(number)) {
                exec_error(vm, "unable to format Z-machine number output");
                return TCL_ERROR;
            }
            zmachine_output_append(vm, number, (size_t)n);
            vm->pc = instruction.next_pc;
            return TCL_OK;
        }
        case 8U:                                 /* push */
            if (zmachine_stack_push(vm, values[0]) != TCL_OK) return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        case 9U: {                               /* pull: pop once, indirect-write target */
            uint16_t v;
            if (zmachine_stack_pop(vm, &v) != TCL_OK) return TCL_ERROR;
            if (zmachine_variable_write(vm, (uint8_t)values[0], 1, v) != TCL_OK)
                return TCL_ERROR;
            vm->pc = instruction.next_pc;
            return TCL_OK;
        }
        case 12U:                                /* call_vs2 V4+ */
            if (vm->version >= 4U) return execute_call(vm, &instruction, values, 0);
            break;
        case 25U:                                /* call_vn V5+ */
            if (vm->version >= 5U) return execute_call(vm, &instruction, values, 1);
            break;
        case 26U:                                /* call_vn2 V5+ */
            if (vm->version >= 5U) return execute_call(vm, &instruction, values, 1);
            break;
        case 29U:                                /* copy_table V5+ */
            if (vm->version >= 5U)
                return execute_copy_table(vm, values,
                                          instruction.operand_count_actual,
                                          instruction.next_pc);
            break;
        default: break;
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
