#ifndef ZMACHINE_EXEC_H
#define ZMACHINE_EXEC_H

#include <stddef.h>
#include <stdint.h>

#include "zmachine_decode.h"
#include "zmachine_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZMachine ZMachine;

/* Resolve raw decoded operands from first to last. Variable operands are
 * dereferenced using normal Z-machine semantics, including popping variable
 * 0 (the evaluation stack). */
int zmachine_resolve_operands(ZMachine *vm,
                              const ZMachineInstruction *instruction,
                              uint16_t *values,
                              size_t values_capacity);

/* Enter a routine given its packed address. Arguments are copied to locals
 * 1..n after version-specific local initialization. A packed address of zero
 * immediately returns false without creating a frame. */
int zmachine_call_routine(ZMachine *vm,
                          uint16_t packed_address,
                          const uint16_t *arguments,
                          uint8_t argument_count,
                          uint32_t return_pc,
                          uint8_t store_variable,
                          int discard_result);

/* Return from the current routine, restore the caller PC/stack frame, and
 * store the return value unless the call discarded its result. */
int zmachine_return(ZMachine *vm, uint16_t value);

/* Execute one instruction at vm->pc. This initial executor implements the
 * call/return family plus nop and quit. Unsupported opcodes report an error. */
int zmachine_step(ZMachine *vm);

#ifdef __cplusplus
}
#endif

#endif
