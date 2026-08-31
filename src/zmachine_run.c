/*
 * zmachine_run.c
 *
 * Cooperative execution loop and interpreter-level opcode dispatch.
 *
 * This module owns operations which affect the whole interpreter session or may
 * suspend waiting for Tcl input. Ordinary VM execution remains in the layered
 * zmachine_step() chain. Keeping this boundary explicit fixes an important
 * decoder distinction: EXTENDED instructions use the decoder's VAR-sized
 * operand bucket, but they are not VAR-table opcodes. Every VAR opcode handled
 * here therefore requires ZM_FORM_VARIABLE as well as the opcode number.
 *
 * The original run-loop implementation remains in zmachine.c under the renamed
 * symbol zmachine_run_legacy for now; story loading, lifetime management, and
 * output buffering still live there. This module is the public run path and is
 * intentionally written so the legacy loop can be removed in a later cleanup
 * without changing VM behavior.
 */

#include "tclzmachine.h"
#include "zmachine_decode.h"
#include "zmachine_exec.h"
#include "zmachine_input.h"
#include "zmachine_stream.h"
#include "zmachine_undo.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ZM_HEADER_SIZE 64U
#define ZM_RUN_STEP_LIMIT 1000000U

/* Put the VM into its terminal error state with a run-layer diagnostic. */
static int run_error(ZMachine *vm, const char *message)
{
    if (vm) {
        vm->state = ZM_STATE_ERROR;
        snprintf(vm->error, sizeof(vm->error), "%s", message);
    }
    return TCL_ERROR;
}

/* Read one big-endian word from an already bounds-checked story address. */
static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* Store an opcode result and return the address immediately following it. */
static int store_result(ZMachine *vm, uint32_t store_pc,
                        uint16_t value, uint32_t *next_pc)
{
    uint8_t variable;

    if (!vm || !vm->memory || (size_t)store_pc >= vm->memory_size)
        return run_error(vm, "truncated Z-machine store variable");

    variable = vm->memory[store_pc];
    if (zmachine_variable_write(vm, variable, 0, value) != TCL_OK)
        return TCL_ERROR;
    if (next_pc)
        *next_pc = store_pc + 1U;
    return TCL_OK;
}

/*
 * Apply a Z-machine branch record for a known boolean condition.
 *
 * Branch offsets 0 and 1 are the special rfalse/rtrue returns. Other offsets
 * are signed 14-bit or unsigned 6-bit displacements relative to the address
 * after the branch data with the standard -2 adjustment.
 */
static int apply_branch(ZMachine *vm, uint32_t branch_pc, int condition)
{
    uint8_t first;
    int branch_on_true;
    int32_t offset;
    uint32_t after;

    if (!vm || !vm->memory || (size_t)branch_pc >= vm->memory_size)
        return run_error(vm, "truncated Z-machine branch");

    first = vm->memory[branch_pc];
    branch_on_true = (first & 0x80U) != 0U;

    if (first & 0x40U) {
        offset = (int32_t)(first & 0x3fU);
        after = branch_pc + 1U;
    } else {
        uint16_t raw;

        if ((size_t)branch_pc + 1U >= vm->memory_size)
            return run_error(vm, "truncated Z-machine branch");
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
        return run_error(vm, "Z-machine branch target is outside story memory");
    return TCL_OK;
}

/*
 * Check the immutable bytes of the originally loaded story for `verify`.
 *
 * Dynamic memory and interpreter-owned header bytes change during play, so the
 * checksum must use the pristine restart snapshot for that range. Static/high
 * memory is immutable and may be read from the live image. Synthetic test VMs
 * without a restart image fall back to their live dynamic bytes.
 */
static int story_checksum_matches(const ZMachine *vm)
{
    size_t end;
    size_t i;
    uint32_t sum = 0U;

    if (!vm || !vm->memory || vm->memory_size <= ZM_HEADER_SIZE)
        return 0;

    end = vm->declared_file_length != 0U ?
          vm->declared_file_length : vm->memory_size;
    if (end > vm->memory_size)
        end = vm->memory_size;

    for (i = ZM_HEADER_SIZE; i < end; ++i) {
        if (vm->initial_dynamic_memory &&
            i < vm->initial_dynamic_memory_size)
            sum += vm->initial_dynamic_memory[i];
        else
            sum += vm->memory[i];
    }

    return (uint16_t)sum == vm->checksum;
}

/* Per-session pseudo-random generator implementing the standard seed contract. */
static uint16_t random_result(ZMachine *vm, int16_t range)
{
    if (range < 0) {
        uint32_t seed = (uint32_t)(-(int32_t)range);
        vm->random_state = seed ? seed : 1U;
        return 0U;
    }

    if (range == 0) {
        uint32_t seed = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)vm;
        vm->random_state = seed ? seed : 1U;
        return 0U;
    }

    vm->random_state = vm->random_state * 1103515245U + 12345U;
    return (uint16_t)(1U + (vm->random_state % (uint16_t)range));
}

/*
 * Handle VAR:4 read/sread/aread at the cooperative host boundary.
 *
 * Structural validation happens before suspension, but operands are not
 * evaluated until input is actually available. This distinction is observable
 * when an operand is variable 0: merely yielding for input must not pop the
 * evaluation stack. V5+ permits a missing/zero parse buffer and stores the
 * terminating character. Timed input is not advertised by this runtime, so
 * nonzero timeout/routine operands are rejected rather than silently pretending
 * the timeout facility exists.
 */
static int handle_read(ZMachine *vm,
                       const ZMachineInstruction *instruction,
                       int *handled)
{
    uint16_t values[ZM_MAX_OPERANDS];
    uint16_t text_buffer;
    uint16_t parse_buffer;
    uint16_t terminator = 13U;
    uint32_t next_pc;

    *handled = 0;
    if (!vm || !instruction ||
        instruction->form != ZM_FORM_VARIABLE ||
        instruction->operand_count != ZM_OPERANDS_VAR ||
        instruction->opcode_number != 4U)
        return TCL_OK;

    *handled = 1;
    if (instruction->operand_count_actual < 1U ||
        (vm->version <= 4U && instruction->operand_count_actual < 2U))
        return run_error(vm, "read opcode is missing required buffer operands");

    if (!vm->input_available) {
        vm->state = ZM_STATE_WAITING_INPUT;
        return TCL_OK;
    }

    if (zmachine_resolve_operands(vm, instruction, values,
                                  ZM_MAX_OPERANDS) != TCL_OK)
        return TCL_ERROR;

    if (instruction->operand_count_actual >= 3U && values[2] != 0U)
        return run_error(vm, "timed line input is unsupported");
    if (instruction->operand_count_actual >= 4U && values[3] != 0U)
        return run_error(vm, "timed line input routine is unsupported");

    text_buffer = values[0];
    parse_buffer = instruction->operand_count_actual >= 2U ? values[1] : 0U;

    if (zmachine_input_read_line(vm, text_buffer, parse_buffer,
                                 &terminator) != TCL_OK)
        return run_error(vm, "unable to store or tokenize line input");

    next_pc = instruction->next_pc;
    if (vm->version >= 5U) {
        uint8_t store_variable;

        if ((size_t)next_pc >= vm->memory_size)
            return run_error(vm, "truncated read store variable");
        store_variable = vm->memory[next_pc++];
        if (zmachine_variable_write(vm, store_variable, 0,
                                    terminator) != TCL_OK)
            return TCL_ERROR;
    }

    vm->pc = next_pc;
    return TCL_OK;
}

/*
 * Restore the story's initial dynamic memory and volatile session state.
 *
 * restart is intentionally narrower than restore/undo for Flags 2. Its opcode
 * contract says only bit 0 (transcription) and bit 1 (fixed-pitch request)
 * survive from the live session. Every other Flags 2 bit comes back from the
 * pristine story image, after which zmachine_refresh_interpreter_header()
 * reapplies the interpreter-owned capability/Rst fields. Preserving the whole
 * live word here would incorrectly carry transient redraw and game-request bits
 * across a restart. Host stream files are not VM state: the stream layer then
 * resets replay input and command recording selections while retaining any
 * already-chosen files for later reuse in the same interpreter session.
 */
static int execute_restart(ZMachine *vm)
{
    uint16_t live_flags2;
    uint16_t initial_flags2;
    uint16_t restarted_flags2;

    if (!vm->initial_dynamic_memory ||
        vm->initial_dynamic_memory_size != vm->static_memory_addr ||
        vm->initial_dynamic_memory_size <= 0x11U)
        return run_error(vm, "restart image is unavailable");

    live_flags2 = read_be16(vm->memory + 0x10U);
    initial_flags2 = read_be16(vm->initial_dynamic_memory + 0x10U);
    restarted_flags2 = (uint16_t)((initial_flags2 & (uint16_t)~0x0003U) |
                                  (live_flags2 & 0x0003U));

    memcpy(vm->memory, vm->initial_dynamic_memory,
           vm->initial_dynamic_memory_size);
    vm->memory[0x10U] = (uint8_t)(restarted_flags2 >> 8);
    vm->memory[0x11U] = (uint8_t)restarted_flags2;
    vm->flags2 = restarted_flags2;

    zmachine_undo_discard(vm);
    vm->pc = vm->initial_pc;
    vm->sp = 0U;
    vm->frame_count = 0U;
    vm->input_available = 0;
    vm->random_state = 1U;
    vm->current_window = 0U;
    vm->output_stream1_enabled = 1;
    vm->stream3_depth = 0U;
    memset(vm->stream3_tables, 0, sizeof(vm->stream3_tables));
    Tcl_DStringSetLength(&vm->pending_input, 0);
    zmachine_refresh_interpreter_header(vm);
    zmachine_stream_after_restart(vm);
    return TCL_OK;
}

/*
 * Execute interpreter-level 0OP and VAR-table operations.
 *
 * Only the VAR form is accepted for random/scan_table/check_arg_count. That
 * guard is the central reason this module exists: an EXT opcode with the same
 * low opcode number must continue down to the extended/presentation/core layers
 * rather than being consumed here accidentally.
 *
 * Version and arity checks for owned VAR opcodes deliberately precede operand
 * resolution. An instruction which is illegal for the story version must not
 * acquire operand side effects before it is rejected; in particular, resolving
 * variable 0 pops the evaluation stack. Once an opcode is known to be legal and
 * structurally complete, its operands are resolved exactly once.
 */
static int handle_interpreter_opcode(ZMachine *vm,
                                     const ZMachineInstruction *instruction,
                                     int *handled)
{
    uint16_t values[ZM_MAX_OPERANDS];

    *handled = 0;
    if (!vm || !instruction)
        return TCL_OK;

    if (instruction->operand_count == ZM_OPERANDS_0OP) {
        switch (instruction->opcode_number) {
        case 7U: /* restart */
            *handled = 1;
            return execute_restart(vm);

        case 9U: /* pop in V1-V4; V5+ catch is handled in the core layer. */
            if (vm->version <= 4U) {
                uint16_t ignored;
                *handled = 1;
                if (zmachine_stack_pop(vm, &ignored) != TCL_OK)
                    return TCL_ERROR;
                vm->pc = instruction->next_pc;
            }
            return TCL_OK;

        case 12U: /* show_status: real V3 status update, compatibility nop later. */
            if (vm->version >= 3U) {
                *handled = 1;
                vm->pc = instruction->next_pc;
            }
            return TCL_OK;

        case 13U: /* verify is introduced in Version 3. */
            *handled = 1;
            if (vm->version < 3U)
                return run_error(vm, "verify is unavailable before Version 3");
            return apply_branch(vm, instruction->next_pc,
                                story_checksum_matches(vm));

        case 15U: /* piracy is introduced in Version 5. */
            *handled = 1;
            if (vm->version < 5U)
                return run_error(vm, "piracy is unavailable before Version 5");
            /* Standards-conforming interpreters always take the piracy branch. */
            return apply_branch(vm, instruction->next_pc, 1);

        default:
            return TCL_OK;
        }
    }

    if (instruction->form != ZM_FORM_VARIABLE ||
        instruction->operand_count != ZM_OPERANDS_VAR)
        return TCL_OK;

    if (instruction->opcode_number != 7U &&
        instruction->opcode_number != 23U &&
        instruction->opcode_number != 31U)
        return TCL_OK;

    /*
     * Establish ownership, version legality, and arity before resolving any
     * operand. Returning handled=1 on an illegal owned opcode prevents a lower
     * layer from resolving the same variable operands again while reporting a
     * generic unsupported-opcode failure.
     */
    switch (instruction->opcode_number) {
    case 7U: /* random range -> result */
        *handled = 1;
        if (instruction->operand_count_actual != 1U)
            return run_error(vm, "random requires exactly one range operand");
        break;

    case 23U: /* scan_table x table len [form] -> result ?branch */
        *handled = 1;
        if (vm->version < 4U)
            return run_error(vm, "scan_table is unavailable before Version 4");
        if (instruction->operand_count_actual < 3U ||
            instruction->operand_count_actual > 4U)
            return run_error(vm, "scan_table requires three or four operands");
        break;

    case 31U: /* check_arg_count argument-number ?branch */
        *handled = 1;
        if (vm->version < 5U)
            return run_error(vm, "check_arg_count is unavailable before Version 5");
        if (instruction->operand_count_actual != 1U)
            return run_error(vm, "check_arg_count requires exactly one argument number");
        break;

    default:
        return TCL_OK;
    }

    if (zmachine_resolve_operands(vm, instruction, values,
                                  ZM_MAX_OPERANDS) != TCL_OK)
        return TCL_ERROR;

    switch (instruction->opcode_number) {
    case 7U: { /* random range -> result */
        uint32_t next_pc;

        if (store_result(vm, instruction->next_pc,
                         random_result(vm, (int16_t)values[0]),
                         &next_pc) != TCL_OK)
            return TCL_ERROR;
        vm->pc = next_pc;
        return TCL_OK;
    }

    case 23U: { /* scan_table x table len [form] -> result ?branch */
        uint16_t form;
        uint16_t result = 0U;
        uint16_t i;
        uint32_t branch_pc;
        uint32_t field_size;
        int words;

        form = instruction->operand_count_actual >= 4U ? values[3] : 0x82U;
        words = (form & 0x80U) != 0U;
        field_size = form & 0x7fU;
        if (field_size == 0U)
            return run_error(vm, "scan_table field size is zero");

        for (i = 0U; i < values[2]; ++i) {
            uint32_t address = (uint32_t)values[1] +
                               (uint32_t)i * field_size;
            uint16_t candidate;

            if (words) {
                if ((size_t)address + 1U >= vm->memory_size)
                    return run_error(vm, "scan_table reads outside story memory");
                candidate = read_be16(vm->memory + address);
            } else {
                if ((size_t)address >= vm->memory_size)
                    return run_error(vm, "scan_table reads outside story memory");
                candidate = vm->memory[address];
            }

            if (candidate == values[0]) {
                result = (uint16_t)address;
                break;
            }
        }

        if (store_result(vm, instruction->next_pc, result,
                         &branch_pc) != TCL_OK)
            return TCL_ERROR;
        return apply_branch(vm, branch_pc, result != 0U);
    }

    case 31U: { /* check_arg_count argument-number ?branch */
        const ZMachineFrame *frame;
        uint16_t argument_number;
        int supplied = 0;

        argument_number = values[0];
        frame = zmachine_current_frame_const(vm);
        if (frame && argument_number >= 1U && argument_number <= 7U)
            supplied = (frame->argument_mask &
                        (uint8_t)(1U << (argument_number - 1U))) != 0U;
        return apply_branch(vm, instruction->next_pc, supplied);
    }

    default:
        return TCL_OK;
    }
}

/*
 * Execute synchronously until input/file interaction, halt, or error.
 *
 * Each loop iteration decodes once for the two interpreter-owned interception
 * layers. An ordinary instruction then executes through public zmachine_step(),
 * which may itself yield for save/restore file policy. The generous instruction
 * cap bounds malformed stories which never yield to their Tcl host.
 */
int zmachine_run(ZMachine *vm)
{
    unsigned long steps = 0UL;

    if (!vm || !vm->memory)
        return run_error(vm, "no story is loaded");
    if (vm->state == ZM_STATE_ERROR)
        return TCL_ERROR;
    if (vm->state == ZM_STATE_HALTED)
        return TCL_OK;

    zmachine_output_clear(vm);
    vm->state = ZM_STATE_READY;

    while (vm->state == ZM_STATE_READY) {
        ZMachineInstruction instruction;
        char decode_error[128];
        int handled;

        if (++steps > ZM_RUN_STEP_LIMIT)
            return run_error(vm, "Z-machine execution step limit exceeded");

        if (!zmachine_decode_instruction(vm->memory, vm->memory_size,
                                         vm->version, vm->pc,
                                         &instruction,
                                         decode_error,
                                         sizeof(decode_error))) {
            return run_error(vm, decode_error[0] ? decode_error :
                             "unable to decode Z-machine instruction");
        }

        if (handle_read(vm, &instruction, &handled) != TCL_OK)
            return TCL_ERROR;
        if (vm->state != ZM_STATE_READY)
            break;
        if (handled)
            continue;

        if (handle_interpreter_opcode(vm, &instruction, &handled) != TCL_OK)
            return TCL_ERROR;
        if (vm->state != ZM_STATE_READY)
            break;
        if (handled)
            continue;

        if (zmachine_step(vm) != TCL_OK)
            return TCL_ERROR;
    }

    return vm->state == ZM_STATE_ERROR ? TCL_ERROR : TCL_OK;
}
