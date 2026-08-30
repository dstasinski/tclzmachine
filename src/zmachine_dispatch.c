/*
 * zmachine_dispatch.c
 *
 * Text-only presentation-policy wrapper around the core opcode executor.
 *
 * The Z-machine includes several instructions whose only purpose is to alter
 * visual terminal presentation. tclzmachine has no screen, cursor, colours,
 * or font state because its primary frontend is a Tcl/IRC request-response
 * interface. Presentation operations which have no meaningful textual effect
 * are therefore consumed here, while ordinary VM instructions are delegated
 * to the core executor in zmachine_exec.c.
 *
 * Character input is also adapted here. VAR:22 read_char participates in the
 * cooperative input model: the VM suspends when no character is available and
 * resumes when the next Tcl command supplies that character.
 */

#include "tclzmachine.h"
#include "zmachine_decode.h"
#include "zmachine_exec.h"
#include "zmachine_text.h"
#include "zmachine_undo.h"

#include <stdio.h>

/* Core instruction executor supplied by zmachine_exec.c after symbol rename. */
extern int zmachine_step_core(ZMachine *vm);

/* Put the VM into its terminal error state with a concise diagnostic. */
static int dispatch_error(ZMachine *vm, const char *message)
{
    if (vm) {
        vm->state = ZM_STATE_ERROR;
        snprintf(vm->error, sizeof(vm->error), "%s", message);
    }
    return TCL_ERROR;
}

/* Store a normal opcode result and advance past the store-variable byte. */
static int store_value(ZMachine *vm, uint32_t store_pc, uint16_t value)
{
    uint8_t variable;

    if (!vm || (size_t)store_pc >= vm->memory_size)
        return dispatch_error(vm, "truncated Z-machine store variable");

    variable = vm->memory[store_pc];
    if (zmachine_variable_write(vm, variable, 0, value) != TCL_OK)
        return TCL_ERROR;

    vm->pc = store_pc + 1U;
    return TCL_OK;
}

/*
 * Handle the in-memory undo opcodes from the EXT table.
 *
 * save_undo snapshots the state before storing its immediate result. That is
 * important when the destination is variable 0: a later restore must recreate
 * the pre-store stack and push 2, not retain the original 1 and push 2 again.
 * Allocation failure is reported as -1, which is the standard capability-
 * unavailable result. restore_undo with no cached state retains the project's
 * previous deterministic failure result of 0; successful restore transfers
 * control back to the saved save_undo continuation.
 */
static int handle_extended_undo(ZMachine *vm,
                                const ZMachineInstruction *instruction,
                                int *handled)
{
    int undo_result;

    *handled = 0;
    if (!vm || !instruction || vm->version < 5U ||
        instruction->form != ZM_FORM_EXTENDED)
        return TCL_OK;

    if (instruction->opcode_number == 9U) {
        uint8_t store_variable;

        *handled = 1;
        if (instruction->operand_count_actual != 0U)
            return dispatch_error(vm, "save_undo does not accept operands");
        if ((size_t)instruction->next_pc >= vm->memory_size)
            return dispatch_error(vm, "truncated save_undo store variable");

        store_variable = vm->memory[instruction->next_pc];
        undo_result = zmachine_undo_save(vm,
                                         instruction->next_pc + 1U,
                                         store_variable);
        return store_value(vm, instruction->next_pc,
                           undo_result == ZM_UNDO_SUCCESS ? 1U : 0xffffU);
    }

    if (instruction->opcode_number == 10U) {
        *handled = 1;
        if (instruction->operand_count_actual != 0U)
            return dispatch_error(vm, "restore_undo does not accept operands");

        undo_result = zmachine_undo_restore(vm);
        if (undo_result == ZM_UNDO_SUCCESS)
            return TCL_OK;
        if (undo_result == ZM_UNDO_ERROR)
            return TCL_ERROR;
        return store_value(vm, instruction->next_pc, 0U);
    }

    return TCL_OK;
}

/* Apply a Z-machine branch record for a known boolean condition. */
static int apply_branch(ZMachine *vm, uint32_t branch_pc, int condition)
{
    uint8_t first;
    int branch_on_true;
    int32_t offset;
    uint32_t after;

    if (!vm || (size_t)branch_pc >= vm->memory_size)
        return dispatch_error(vm, "truncated Z-machine branch");

    first = vm->memory[branch_pc];
    branch_on_true = (first & 0x80U) != 0U;

    if (first & 0x40U) {
        offset = (int32_t)(first & 0x3fU);
        after = branch_pc + 1U;
    } else {
        uint16_t raw;
        if ((size_t)branch_pc + 1U >= vm->memory_size)
            return dispatch_error(vm, "truncated Z-machine branch");
        raw = (uint16_t)(((uint16_t)(first & 0x3fU) << 8) |
                         vm->memory[branch_pc + 1U]);
        if (raw & 0x2000U)
            raw |= 0xc000U;
        offset = (int16_t)raw;
        after = branch_pc + 2U;
    }

    if (!!condition != !!branch_on_true) {
        vm->pc = after;
        return TCL_OK;
    }

    if (offset == 0)
        return zmachine_return(vm, 0U);
    if (offset == 1)
        return zmachine_return(vm, 1U);

    vm->pc = (uint32_t)((int32_t)after + offset - 2);
    if ((size_t)vm->pc >= vm->memory_size)
        return dispatch_error(vm, "Z-machine branch target is outside story memory");
    return TCL_OK;
}

/* Store zero and perform the branch attached to get_child/get_sibling. */
static int store_zero_and_branch_false(ZMachine *vm, uint32_t store_pc)
{
    uint8_t variable;

    if (!vm || (size_t)store_pc >= vm->memory_size)
        return dispatch_error(vm, "truncated null-object store variable");

    variable = vm->memory[store_pc++];
    if (zmachine_variable_write(vm, variable, 0, 0U) != TCL_OK)
        return TCL_ERROR;

    return apply_branch(vm, store_pc, 0);
}

/*
 * Compatibility handling for get_child object 0.
 *
 * Object zero is the Z-machine's null object and has no table entry. The
 * specification leaves queries of object zero undefined, but legacy story
 * code and compatibility suites may still issue get_child 0. Returning zero
 * and taking the opcode's false branch is the conservative behaviour.
 */
static int handle_null_get_child(ZMachine *vm,
                                 const ZMachineInstruction *instruction,
                                 int *handled)
{
    uint16_t values[ZM_MAX_OPERANDS];

    *handled = 0;
    if (!vm || !instruction ||
        instruction->operand_count != ZM_OPERANDS_1OP ||
        instruction->opcode_number != 2U)
        return TCL_OK;

    if (zmachine_resolve_operands(vm, instruction, values,
                                  ZM_MAX_OPERANDS) != TCL_OK)
        return TCL_ERROR;

    if (values[0] != 0U)
        return TCL_OK;

    *handled = 1;
    return store_zero_and_branch_false(vm, instruction->next_pc);
}

/* Return nonzero for presentation opcodes which can safely disappear. */
static int is_text_only_noop(const ZMachine *vm,
                             const ZMachineInstruction *instruction)
{
    if (!vm || !instruction || vm->version < 3U ||
        instruction->form != ZM_FORM_VARIABLE ||
        instruction->operand_count != ZM_OPERANDS_VAR)
        return 0;

    if (instruction->opcode_number == 10U ||
        instruction->opcode_number == 11U)
        return vm->version >= 3U;

    if (vm->version < 4U)
        return 0;

    return instruction->opcode_number == 13U ||
           instruction->opcode_number == 15U ||
           instruction->opcode_number == 17U ||
           instruction->opcode_number == 18U;
}

/* Handle VAR:19 output_stream. */
static int handle_output_stream(ZMachine *vm,
                                const ZMachineInstruction *instruction,
                                int *handled)
{
    uint16_t values[ZM_MAX_OPERANDS];
    int16_t stream;

    *handled = 0;
    if (!vm || !instruction || vm->version < 3U ||
        instruction->form != ZM_FORM_VARIABLE ||
        instruction->operand_count != ZM_OPERANDS_VAR ||
        instruction->opcode_number != 19U)
        return TCL_OK;

    *handled = 1;
    if (instruction->operand_count_actual < 1U)
        return dispatch_error(vm, "output_stream is missing its stream operand");

    if (zmachine_resolve_operands(vm, instruction, values,
                                  ZM_MAX_OPERANDS) != TCL_OK)
        return TCL_ERROR;

    stream = (int16_t)values[0];
    if (stream == 0) {
        vm->pc = instruction->next_pc;
        return TCL_OK;
    }

    if (stream == 1 || stream == -1) {
        vm->output_stream1_enabled = stream > 0;
        vm->pc = instruction->next_pc;
        return TCL_OK;
    }

    if (stream == 2 || stream == -2 || stream == 4 || stream == -4) {
        vm->pc = instruction->next_pc;
        return TCL_OK;
    }

    if (stream == 3) {
        uint16_t table;
        if (instruction->operand_count_actual < 2U)
            return dispatch_error(vm, "output_stream 3 is missing its table operand");
        if (vm->stream3_depth >= ZM_MAX_STREAM3_DEPTH)
            return dispatch_error(vm, "output_stream 3 nesting limit exceeded");
        table = values[1];
        if ((size_t)table + 1U >= vm->memory_size ||
            (size_t)table + 1U >= (size_t)vm->static_memory_addr)
            return dispatch_error(vm, "output_stream 3 table is outside dynamic memory");
        vm->memory[table] = 0U;
        vm->memory[table + 1U] = 0U;
        vm->stream3_tables[vm->stream3_depth++] = table;
        vm->pc = instruction->next_pc;
        return TCL_OK;
    }

    if (stream == -3) {
        if (vm->stream3_depth == 0U)
            return dispatch_error(vm, "output_stream -3 without active stream 3");
        --vm->stream3_depth;
        vm->pc = instruction->next_pc;
        return TCL_OK;
    }

    return dispatch_error(vm, "unsupported Z-machine output stream number");
}

/*
 * Handle VAR:30 print_table as plain textual rows.
 *
 * In a V5 text-only frontend the only meaningful output bytes are ZSCII null
 * (which has no effect), newline, printable ASCII, and the defined 155..251
 * extra-character range. Classic screen tables can contain input-only or
 * undefined byte values as layout data. Within print_table only, discard those
 * non-output bytes; ordinary print_char and packed Z-text remain strict.
 */
static int handle_print_table(ZMachine *vm,
                              const ZMachineInstruction *instruction,
                              int *handled)
{
    uint16_t values[ZM_MAX_OPERANDS];
    uint32_t address;
    uint16_t width, height, skip;
    uint16_t row, column;

    *handled = 0;
    if (!vm || !instruction || vm->version < 5U ||
        instruction->form != ZM_FORM_VARIABLE ||
        instruction->operand_count != ZM_OPERANDS_VAR ||
        instruction->opcode_number != 30U)
        return TCL_OK;

    *handled = 1;
    if (instruction->operand_count_actual < 2U)
        return dispatch_error(vm, "print_table requires address and width operands");
    if (zmachine_resolve_operands(vm, instruction, values,
                                  ZM_MAX_OPERANDS) != TCL_OK)
        return TCL_ERROR;

    address = values[0];
    width = values[1];
    height = instruction->operand_count_actual >= 3U ? values[2] : 1U;
    skip = instruction->operand_count_actual >= 4U ? values[3] : 0U;

    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            uint8_t ch;
            if ((size_t)address >= vm->memory_size)
                return dispatch_error(vm, "print_table reads outside story memory");
            ch = vm->memory[address++];
            if (ch == 0U)
                continue;
            if (ch != 13U &&
                !(ch >= 32U && ch <= 126U) &&
                !(ch >= 155U && ch <= 251U))
                continue;
            if (zmachine_text_output_zscii(vm, ch) != TCL_OK)
                return TCL_ERROR;
        }
        if (row + 1U < height &&
            zmachine_text_output_zscii(vm, 13U) != TCL_OK)
            return TCL_ERROR;
        address += skip;
    }

    vm->pc = instruction->next_pc;
    return TCL_OK;
}

/* Handle VAR:22 read_char using cooperative Tcl input. */
static int handle_read_char(ZMachine *vm,
                            const ZMachineInstruction *instruction,
                            int *handled)
{
    uint16_t values[ZM_MAX_OPERANDS];
    uint16_t zscii;
    uint8_t store_variable;
    const char *input;
    int input_length;

    *handled = 0;
    if (!vm || !instruction || vm->version < 4U ||
        instruction->form != ZM_FORM_VARIABLE ||
        instruction->operand_count != ZM_OPERANDS_VAR ||
        instruction->opcode_number != 22U)
        return TCL_OK;

    *handled = 1;
    if (instruction->operand_count_actual < 1U)
        return dispatch_error(vm, "read_char is missing its input-device operand");

    if (!vm->input_available) {
        vm->state = ZM_STATE_WAITING_INPUT;
        return TCL_OK;
    }

    if (zmachine_resolve_operands(vm, instruction, values,
                                  ZM_MAX_OPERANDS) != TCL_OK)
        return TCL_ERROR;
    if (values[0] != 1U)
        return dispatch_error(vm, "read_char requested an unsupported input device");
    if (instruction->operand_count_actual >= 2U && values[1] != 0U)
        return dispatch_error(vm, "timed read_char is not yet supported");
    if (instruction->operand_count_actual >= 3U && values[2] != 0U)
        return dispatch_error(vm, "timed read_char callback is not yet supported");
    if ((size_t)instruction->next_pc >= vm->memory_size)
        return dispatch_error(vm, "truncated read_char store variable");

    input = Tcl_DStringValue(&vm->pending_input);
    input_length = Tcl_DStringLength(&vm->pending_input);
    if (input_length == 0) {
        zscii = 13U;
    } else {
        unsigned char ch = (unsigned char)input[0];
        zscii = (ch >= 32U && ch <= 126U) ? (uint16_t)ch : 13U;
    }

    store_variable = vm->memory[instruction->next_pc];
    if (zmachine_variable_write(vm, store_variable, 0, zscii) != TCL_OK)
        return TCL_ERROR;

    Tcl_DStringSetLength(&vm->pending_input, 0);
    vm->input_available = 0;
    vm->pc = instruction->next_pc + 1U;
    return TCL_OK;
}

/* Execute one instruction through the text-only compatibility layer. */
int zmachine_step(ZMachine *vm)
{
    ZMachineInstruction instruction;
    uint16_t values[ZM_MAX_OPERANDS];
    char decode_error[128];
    int handled;

    if (!vm || !vm->memory)
        return dispatch_error(vm, "cannot execute without a loaded story");

    if (!zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                     vm->pc, &instruction,
                                     decode_error, sizeof(decode_error))) {
        return dispatch_error(vm, decode_error[0] ? decode_error :
                              "unable to decode Z-machine instruction");
    }

    if (handle_extended_undo(vm, &instruction, &handled) != TCL_OK)
        return TCL_ERROR;
    if (handled)
        return TCL_OK;

    if (handle_null_get_child(vm, &instruction, &handled) != TCL_OK)
        return TCL_ERROR;
    if (handled)
        return TCL_OK;

    if (handle_read_char(vm, &instruction, &handled) != TCL_OK)
        return TCL_ERROR;
    if (handled)
        return TCL_OK;

    if (handle_output_stream(vm, &instruction, &handled) != TCL_OK)
        return TCL_ERROR;
    if (handled)
        return TCL_OK;

    if (handle_print_table(vm, &instruction, &handled) != TCL_OK)
        return TCL_ERROR;
    if (handled)
        return TCL_OK;

    if (!is_text_only_noop(vm, &instruction))
        return zmachine_step_core(vm);

    if (instruction.operand_count_actual < 1U)
        return dispatch_error(vm, "presentation opcode is missing its operand");
    if (zmachine_resolve_operands(vm, &instruction, values,
                                  ZM_MAX_OPERANDS) != TCL_OK)
        return TCL_ERROR;

    (void)values;
    vm->pc = instruction.next_pc;
    return TCL_OK;
}
