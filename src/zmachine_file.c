/*
 * zmachine_file.c
 *
 * Cooperative full-game save/restore dispatch and completion.
 *
 * Z-machine save/restore opcodes require host filename interaction. Rather than
 * embedding filesystem, account, or IRC policy in the executor, this wrapper
 * recognizes full-state save/restore requests and yields to Tcl. The host later
 * supplies a path through zmachine_save_file()/zmachine_restore_file(), which
 * use Quetzal persistence and then complete the original opcode's branch/store
 * semantics. zmachine_cancel_file() lets the host decline the request cleanly.
 *
 * Failed filesystem/Quetzal operations deliberately leave the VM in its waiting
 * state so the Tcl application can retry another path or call cancel. Only a
 * successful completion (or explicit cancel) consumes pending_file_pc.
 */

#include "tclzmachine.h"
#include "zmachine_decode.h"
#include "zmachine_exec.h"
#include "zmachine_quetzal.h"

#include <stdio.h>

/* Lexical opcode layer supplied by zmachine_tokenise.c. */
extern int zmachine_step_tokenise(ZMachine *vm);

/* Record a host/file-layer diagnostic without unconditionally changing state. */
static int file_error(ZMachine *vm, const char *message)
{
    if (vm)
        snprintf(vm->error, sizeof(vm->error), "%s", message);
    return TCL_ERROR;
}

/*
 * Apply the V1-V3 branch record belonging to a completed save/restore opcode.
 *
 * This mirrors ordinary VM branch encoding but intentionally does not mark the
 * VM erroneous for a host I/O failure; the caller decides whether an error is
 * terminal or retryable. Taken branch offsets 0/1 retain their standard
 * rfalse/rtrue special meanings.
 */
static int apply_branch(ZMachine *vm, uint32_t branch_pc, int condition)
{
    uint8_t first;
    int branch_on_true;
    int32_t offset;
    uint32_t after;

    if (!vm || (size_t)branch_pc >= vm->memory_size)
        return file_error(vm, "truncated save/restore branch record");

    first = vm->memory[branch_pc];
    branch_on_true = (first & 0x80U) != 0U;

    if (first & 0x40U) {
        offset = (int32_t)(first & 0x3fU);
        after = branch_pc + 1U;
    } else {
        uint16_t raw;
        if ((size_t)branch_pc + 1U >= vm->memory_size)
            return file_error(vm, "truncated save/restore branch record");
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
        return file_error(vm, "save/restore branch target is outside story memory");
    return TCL_OK;
}

/* Apply the V4+ one-byte store record and continue after its destination byte. */
static int store_result(ZMachine *vm, uint32_t store_pc, uint16_t value)
{
    uint8_t variable;

    if (!vm || (size_t)store_pc >= vm->memory_size)
        return file_error(vm, "truncated save/restore store variable");
    variable = vm->memory[store_pc];
    if (zmachine_variable_write(vm, variable, 0, value) != TCL_OK)
        return TCL_ERROR;
    vm->pc = store_pc + 1U;
    return TCL_OK;
}

/*
 * Finish the pending *current* save/restore request with success or failure.
 *
 * For V1-V3 `continuation` points at a branch record. V4+ full-game forms use a
 * store byte instead. Successful ordinary completion returns 1; cancellation
 * returns the version's normal false/failure result. Full restore is different:
 * it resumes the *saved* save opcode and is handled separately below.
 */
static int complete_request(ZMachine *vm, uint32_t continuation, int success)
{
    int rc;

    vm->state = ZM_STATE_READY;
    vm->pending_file_pc = 0U;
    vm->error[0] = '\0';

    if (vm->version <= 3U)
        rc = apply_branch(vm, continuation, success);
    else
        rc = store_result(vm, continuation, success ? 1U : 0U);

    if (rc != TCL_OK)
        vm->state = ZM_STATE_ERROR;
    return rc;
}

/*
 * Recognize only full-game save/restore forms and suspend before their result.
 *
 * V1-V3 0OP forms branch; V4 0OP forms store. V5+ moved full save/restore to
 * zero-operand EXT:0/EXT:1. Extended forms carrying operands describe auxiliary
 * memory-region files and are deliberately not mistaken for Quetzal full-state
 * saves. pending_file_pc records the untouched branch/store continuation so a
 * later host response can complete exactly the instruction that yielded.
 */
static int handle_file_request(ZMachine *vm,
                               const ZMachineInstruction *instruction,
                               int *handled)
{
    int request = 0;

    *handled = 0;
    if (!vm || !instruction)
        return TCL_OK;

    if (instruction->operand_count == ZM_OPERANDS_0OP && vm->version <= 4U) {
        if (instruction->opcode_number == 5U)
            request = ZM_STATE_WAITING_SAVE;
        else if (instruction->opcode_number == 6U)
            request = ZM_STATE_WAITING_RESTORE;
    } else if (vm->version >= 5U &&
               instruction->form == ZM_FORM_EXTENDED &&
               instruction->operand_count_actual == 0U) {
        if (instruction->opcode_number == 0U)
            request = ZM_STATE_WAITING_SAVE;
        else if (instruction->opcode_number == 1U)
            request = ZM_STATE_WAITING_RESTORE;
    }

    if (request == 0)
        return TCL_OK;

    if ((size_t)instruction->next_pc >= vm->memory_size)
        return file_error(vm, "truncated save/restore continuation");

    *handled = 1;
    vm->pending_file_pc = instruction->next_pc;
    vm->state = (ZMachineRunState)request;
    vm->error[0] = '\0';
    return TCL_OK;
}

/*
 * Public one-instruction entry point at the top of the layered opcode chain.
 * Save/restore requests are intercepted before any lower executor can consume
 * their branch/store bytes; all other instructions delegate to the lexical,
 * presentation, then core layers.
 */
int zmachine_step(ZMachine *vm)
{
    ZMachineInstruction instruction;
    char decode_error[128];
    int handled;

    if (!vm || !vm->memory)
        return file_error(vm, "cannot execute without a loaded story");

    if (!zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                     vm->pc, &instruction,
                                     decode_error, sizeof(decode_error))) {
        vm->state = ZM_STATE_ERROR;
        return file_error(vm, decode_error[0] ? decode_error :
                          "unable to decode Z-machine instruction");
    }

    if (handle_file_request(vm, &instruction, &handled) != TCL_OK)
        return TCL_ERROR;
    if (handled)
        return TCL_OK;

    return zmachine_step_tokenise(vm);
}

/*
 * Complete a VM that is waiting for a save filename.
 *
 * The current state is serialized to `path` with the pending opcode continuation
 * recorded in IFhd. A write failure leaves WAITING_SAVE and pending_file_pc
 * intact so the host can retry. On success the original save opcode completes
 * with its version-appropriate success branch/store result and execution resumes.
 */
int zmachine_save_file(ZMachine *vm, const char *path)
{
    uint32_t continuation;

    if (!vm || vm->state != ZM_STATE_WAITING_SAVE)
        return file_error(vm, "Z-machine is not waiting for a save filename");

    continuation = vm->pending_file_pc;
    if (zmachine_quetzal_save(vm, path, continuation) != TCL_OK)
        return TCL_ERROR; /* Leave WAITING_SAVE so the host may retry or cancel. */

    return complete_request(vm, continuation, 1);
}

/*
 * Complete a VM that is waiting for a restore filename.
 *
 * Restore is transactional inside the Quetzal layer. Failure leaves the current
 * WAITING_RESTORE request retryable. Success replaces the state of play and
 * resumes the save point stored in the file: V1-V3 take the saved save branch;
 * V4+ store 2 into the original save instruction's destination. The current
 * restore opcode never completes normally on a successful restore.
 */
int zmachine_restore_file(ZMachine *vm, const char *path)
{
    uint32_t saved_pc;
    int rc;

    if (!vm || vm->state != ZM_STATE_WAITING_RESTORE)
        return file_error(vm, "Z-machine is not waiting for a restore filename");

    if (zmachine_quetzal_restore(vm, path, &saved_pc) != TCL_OK)
        return TCL_ERROR; /* Current request remains retryable or cancellable. */

    vm->pending_file_pc = 0U;
    vm->error[0] = '\0';

    /* Resume the original save point with the standard restored result. */
    if (vm->version <= 3U)
        rc = apply_branch(vm, saved_pc, 1);
    else
        rc = store_result(vm, saved_pc, 2U);

    if (rc != TCL_OK)
        vm->state = ZM_STATE_ERROR;
    return rc;
}

/*
 * Decline whichever save/restore filename request is currently pending.
 * No file operation occurs. The suspended opcode receives its normal failure
 * result (false branch in V1-V3, stored zero in V4+) and VM execution resumes.
 */
int zmachine_cancel_file(ZMachine *vm)
{
    uint32_t continuation;

    if (!vm || (vm->state != ZM_STATE_WAITING_SAVE &&
                vm->state != ZM_STATE_WAITING_RESTORE))
        return file_error(vm, "Z-machine is not waiting for a save/restore filename");

    continuation = vm->pending_file_pc;
    return complete_request(vm, continuation, 0);
}
