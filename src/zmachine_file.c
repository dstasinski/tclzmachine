/*
 * zmachine_file.c
 *
 * Cooperative full-game save/restore dispatch and completion.
 *
 * Z-machine save/restore opcodes require host filename interaction. Rather than
 * embedding filesystem or IRC policy in the executor, this wrapper recognizes
 * full-state save/restore requests and yields to Tcl. The host later supplies a
 * path through zmachine_save_file()/zmachine_restore_file(), which use Quetzal
 * persistence and then complete the original opcode's branch/store semantics.
 */

#include "tclzmachine.h"
#include "zmachine_decode.h"
#include "zmachine_exec.h"
#include "zmachine_quetzal.h"

#include <stdio.h>

/* Presentation wrapper supplied by zmachine_dispatch.c after symbol rename. */
extern int zmachine_step_present(ZMachine *vm);

static int file_error(ZMachine *vm, const char *message)
{
    if (vm)
        snprintf(vm->error, sizeof(vm->error), "%s", message);
    return TCL_ERROR;
}

/* Apply a branch record without forcing the VM into an error on host I/O. */
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
 * Recognize only full-game save/restore forms.
 *
 * V1-V3 0OP forms branch; V4 0OP forms store. V5+ moved full save/restore to
 * EXT:0/EXT:1. Extended forms with operands describe auxiliary-file operations
 * and are deliberately left to later implementation rather than being mistaken
 * for Quetzal full-state saves.
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

/* Public one-instruction entry point, layered above presentation dispatch. */
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

    return zmachine_step_present(vm);
}

int zmachine_save_file(ZMachine *vm, const char *path)
{
    uint32_t continuation;
    int rc;

    if (!vm || vm->state != ZM_STATE_WAITING_SAVE)
        return file_error(vm, "Z-machine is not waiting for a save filename");

    continuation = vm->pending_file_pc;
    if (zmachine_quetzal_save(vm, path, continuation) != TCL_OK)
        return TCL_ERROR; /* Leave WAITING_SAVE so Tcl may retry another path. */

    vm->state = ZM_STATE_READY;
    vm->pending_file_pc = 0U;
    vm->error[0] = '\0';

    if (vm->version <= 3U)
        rc = apply_branch(vm, continuation, 1);
    else
        rc = store_result(vm, continuation, 1U);

    if (rc != TCL_OK)
        vm->state = ZM_STATE_ERROR;
    return rc;
}

int zmachine_restore_file(ZMachine *vm, const char *path)
{
    uint32_t saved_pc;
    int rc;

    if (!vm || vm->state != ZM_STATE_WAITING_RESTORE)
        return file_error(vm, "Z-machine is not waiting for a restore filename");

    if (zmachine_quetzal_restore(vm, path, &saved_pc) != TCL_OK)
        return TCL_ERROR; /* Current restore request remains retryable. */

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
