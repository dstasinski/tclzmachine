/*
 * zmachine_dispatch.c
 *
 * Text-only presentation-policy wrapper around the core opcode executor.
 *
 * The Z-machine includes several instructions whose only purpose is to alter
 * the visual presentation of text on a terminal.  tclzmachine deliberately
 * has no screen, windows, cursor, colours, or font state because its primary
 * frontend is a Tcl/IRC request-response interface.  Supported presentation
 * opcodes which have no meaningful textual effect are therefore consumed here
 * as text-only no-ops, while all ordinary VM instructions are delegated to
 * the core executor in zmachine_exec.c.
 *
 * Keeping this policy in a separate translation unit prevents screen-specific
 * compatibility decisions from becoming mixed into object, arithmetic,
 * routine, and memory semantics.  CMake compiles zmachine_exec.c with its
 * public zmachine_step symbol renamed to zmachine_step_core; this file exports
 * the public zmachine_step wrapper used by the rest of the interpreter.
 */

#include "tclzmachine.h"
#include "zmachine_decode.h"
#include "zmachine_exec.h"

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

/*
 * Return nonzero for V4+ presentation opcodes which can safely disappear in a
 * stream-oriented text frontend.
 *
 * VAR:13 erase_window changes only screen contents/cursor placement.
 * VAR:17 set_text_style changes visual style only.
 * VAR:18 buffer_mode controls terminal-side line buffering/word wrapping.
 *
 * tclzmachine performs optional wrapping at the Tcl output boundary instead,
 * so none of these instructions should modify canonical VM text output.
 */
static int is_text_only_noop(const ZMachine *vm,
                             const ZMachineInstruction *instruction)
{
    if (!vm || !instruction || vm->version < 4U ||
        instruction->operand_count != ZM_OPERANDS_VAR)
        return 0;

    return instruction->opcode_number == 13U ||
           instruction->opcode_number == 17U ||
           instruction->opcode_number == 18U;
}

/*
 * Execute one instruction, intercepting presentation-only operations first.
 *
 * Even a discarded presentation instruction must have its operands evaluated:
 * a variable operand may name stack variable 0, whose read has the observable
 * side effect of popping the evaluation stack.  We therefore use the normal
 * left-to-right operand resolver before advancing past a text-only no-op.
 */
int zmachine_step(ZMachine *vm)
{
    ZMachineInstruction instruction;
    uint16_t values[ZM_MAX_OPERANDS];
    char decode_error[128];

    if (!vm || !vm->memory)
        return dispatch_error(vm, "cannot execute without a loaded story");

    if (!zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                     vm->pc, &instruction,
                                     decode_error, sizeof(decode_error))) {
        return dispatch_error(vm, decode_error[0] ? decode_error :
                              "unable to decode Z-machine instruction");
    }

    if (!is_text_only_noop(vm, &instruction))
        return zmachine_step_core(vm);

    if (instruction.operand_count_actual < 1U)
        return dispatch_error(vm, "presentation opcode is missing its operand");

    if (zmachine_resolve_operands(vm, &instruction, values,
                                  ZM_MAX_OPERANDS) != TCL_OK)
        return TCL_ERROR;

    /* Values are intentionally ignored after their required evaluation. */
    (void)values;
    vm->pc = instruction.next_pc;
    return TCL_OK;
}
