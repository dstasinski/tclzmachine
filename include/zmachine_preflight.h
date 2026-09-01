/*
 * zmachine_preflight.h
 *
 * Shared instruction-legality preflight for every execution entry path.
 *
 * The Z-machine permits operand variables, including variable 0 whose read pops
 * the evaluation stack.  Version, structural arity, and literal-value failures
 * which are knowable from the encoded instruction must therefore be rejected
 * before any execution layer resolves operands.  The cooperative run loop also
 * owns a few instructions before ordinary step dispatch, so it uses the same
 * preflight routine as the public zmachine_step() boundary.
 */
#ifndef ZMACHINE_PREFLIGHT_H
#define ZMACHINE_PREFLIGHT_H

#include "tclzmachine.h"
#include "zmachine_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Validate one already-decoded instruction without resolving any operands.
 *
 * On success, *ignored is set nonzero only for an out-of-range extended opcode
 * which the Standard requires the interpreter to ignore.  This function does
 * not advance vm->pc; the caller owns that control-flow decision.  On failure
 * the VM is placed in ZM_STATE_ERROR and TCL_ERROR is returned.
 */
int zmachine_preflight_instruction(ZMachine *vm,
                                   const ZMachineInstruction *instruction,
                                   int *ignored);

#ifdef __cplusplus
}
#endif

#endif
