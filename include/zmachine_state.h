/*
 * zmachine_state.h
 *
 * Evaluation-stack, variable, and routine-frame primitives for one Z-machine
 * session.
 *
 * The Z-machine variable namespace is split three ways: variable 0 is the
 * evaluation stack, variables 1-15 are current-routine locals, and variables
 * 16-255 are globals stored in story dynamic memory.  Each routine frame also
 * records the evaluation-stack depth at entry so returning from that routine
 * discards temporary stack values without disturbing its caller.
 */

#ifndef ZMACHINE_STATE_H
#define ZMACHINE_STATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Z-machine format permits at most 15 local variables per routine. */
#define ZM_MAX_LOCALS 15U

/* Fixed interpreter call-stack capacity for one embedded session. */
#define ZM_MAX_FRAMES 256U

typedef struct ZMachine ZMachine;

/*
 * Saved state for one active Z-machine routine call.
 *
 * return_pc is the caller address to resume. stack_base is the evaluation-stack
 * depth at routine entry. locals contains the routine's private local values.
 * store_variable identifies where a returned value is written unless
 * discard_result is true. argument_mask records which of the first seven
 * routine arguments were actually supplied, supporting check_arg_count.
 */
typedef struct ZMachineFrame {
    uint32_t return_pc;
    size_t stack_base;
    uint16_t locals[ZM_MAX_LOCALS];
    uint8_t local_count;
    uint8_t store_variable;
    uint8_t argument_mask;
    uint8_t discard_result;
} ZMachineFrame;

/* Push one 16-bit value onto the current routine's evaluation stack. */
int zmachine_stack_push(ZMachine *vm, uint16_t value);

/* Pop the top evaluation-stack value without crossing the current frame base. */
int zmachine_stack_pop(ZMachine *vm, uint16_t *value);

/* Read the top evaluation-stack value without removing it. */
int zmachine_stack_peek(ZMachine *vm, uint16_t *value);

/* Replace the top evaluation-stack value without changing stack depth. */
int zmachine_stack_replace_top(ZMachine *vm, uint16_t value);

/*
 * Read a Z-machine variable according to standard storage semantics.
 *
 * variable 0 normally pops the evaluation stack.  When indirect is nonzero,
 * variable 0 is instead peeked; this is required by opcodes whose operand is a
 * variable reference rather than an ordinary evaluated operand.  Variables
 * 1..15 read current-frame locals and variables 16..255 read big-endian globals
 * from the story's global-variable table.
 */
int zmachine_variable_read(ZMachine *vm,
                           uint8_t variable,
                           int indirect,
                           uint16_t *value);

/*
 * Write a Z-machine variable.
 *
 * variable 0 normally pushes.  With indirect nonzero it replaces the current
 * top-of-stack value.  Local writes modify the active routine frame; global
 * writes are permitted only while the target word remains in dynamic memory.
 */
int zmachine_variable_write(ZMachine *vm,
                            uint8_t variable,
                            int indirect,
                            uint16_t value);

/*
 * Push a new routine frame using already-initialized local values.
 *
 * The caller is responsible for applying version-specific local defaults and
 * argument overrides before this function is called.  The current evaluation
 * stack depth becomes the new frame's stack_base.
 */
int zmachine_frame_push(ZMachine *vm,
                        uint32_t return_pc,
                        uint8_t store_variable,
                        int discard_result,
                        const uint16_t *locals,
                        uint8_t local_count,
                        uint8_t argument_mask);

/* Pop the current routine frame and restore its saved evaluation-stack base. */
int zmachine_frame_pop(ZMachine *vm, ZMachineFrame *frame);

/* Return the active routine frame or NULL when execution is at top level. */
ZMachineFrame *zmachine_current_frame(ZMachine *vm);

/* Const-qualified current-frame accessor for read-only callers. */
const ZMachineFrame *zmachine_current_frame_const(const ZMachine *vm);

#ifdef __cplusplus
}
#endif

#endif
