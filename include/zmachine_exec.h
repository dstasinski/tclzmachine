/*
 * zmachine_exec.h
 *
 * Opcode execution primitives for the Z-machine core.
 *
 * The decoder produces raw operand fields without dereferencing variables.
 * This layer resolves those operands using VM state, performs routine entry and
 * return, applies store/branch semantics, and executes one opcode at a time.
 * Cooperative operations that may suspend the VM, such as line input, remain
 * in the run-loop layer rather than blocking here.
 */

#ifndef ZMACHINE_EXEC_H
#define ZMACHINE_EXEC_H

#include <stddef.h>
#include <stdint.h>

#include "zmachine_decode.h"
#include "zmachine_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Resolve decoded operands from left to right into concrete 16-bit values.
 *
 * Constants are copied directly. Variable-type operands are always evaluated
 * by value using normal Z-machine semantics, so variable 0 pops the evaluation
 * stack. For inc, dec, inc_chk, dec_chk, load, store, and V1-V5 pull, the
 * resulting value is then interpreted by the opcode as a variable number and
 * that target variable is accessed indirectly. Thus a target variable number
 * of 0 is read or written in place, while a Variable-type operand 0 used to
 * obtain that target number is still evaluated normally. Left-to-right order
 * remains observable and must not be changed.
 */
int zmachine_resolve_operands(ZMachine *vm,
                              const ZMachineInstruction *instruction,
                              uint16_t *values,
                              size_t values_capacity);

/*
 * Enter a routine addressed by a packed routine address.
 *
 * V1-V4 routines initialize locals from default words stored in the routine
 * header; V5+ initialize all locals to zero.  Supplied arguments then replace
 * locals 1..n.  return_pc/store_variable describe the caller continuation.
 * When discard_result is nonzero the eventual return value is ignored.
 * Calling packed address zero immediately returns false without creating a
 * routine frame, as required by the Z-machine standard.
 */
int zmachine_call_routine(ZMachine *vm,
                          uint16_t packed_address,
                          const uint16_t *arguments,
                          uint8_t argument_count,
                          uint32_t return_pc,
                          uint8_t store_variable,
                          int discard_result);

/*
 * Return value from the active routine.
 *
 * The current frame is removed, private evaluation-stack values are discarded,
 * the saved caller PC is restored, and value is written to the saved store
 * variable unless the original call discarded its result.
 */
int zmachine_return(ZMachine *vm, uint16_t value);

/*
 * Decode and execute exactly one instruction at vm->pc.
 *
 * On success, vm->pc/state reflect the completed instruction.  Unsupported or
 * malformed instructions put the VM into ZM_STATE_ERROR and return TCL_ERROR.
 */
int zmachine_step(ZMachine *vm);

#ifdef __cplusplus
}
#endif

#endif
