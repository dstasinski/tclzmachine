/*
 * zmachine_quetzal.h
 *
 * Quetzal IFZS saved-game persistence for tclzmachine.
 *
 * The serializer/deserializer owns only Z-machine state-of-play data. Host
 * filename selection and Tcl/IRC policy remain outside this module. Files are
 * written with an uncompressed UMem chunk for simplicity and portability;
 * restores accept both UMem and the standard XOR/run-length CMem encoding.
 */

#ifndef ZMACHINE_QUETZAL_H
#define ZMACHINE_QUETZAL_H

#include <stdint.h>

#include "zmachine_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Write one Quetzal FORM IFZS file.
 *
 * saved_pc is the address required by the Quetzal IFhd chunk: for V1-V3 it
 * points at the save instruction's branch record, while for V4+ it points at
 * the save instruction's store-variable byte. Return TCL_OK on success.
 */
int zmachine_quetzal_save(ZMachine *vm,
                          const char *path,
                          uint32_t saved_pc);

/*
 * Restore one Quetzal FORM IFZS file belonging to the currently loaded story.
 *
 * The story identity in IFhd is checked before committing restored state.
 * Dynamic memory, evaluation stack, routine frames and the saved PC are
 * restored. Flags 2 and interpreter/presentation state remain live. On success
 * *saved_pc receives the IFhd PC so the caller can complete restore semantics.
 */
int zmachine_quetzal_restore(ZMachine *vm,
                             const char *path,
                             uint32_t *saved_pc);

#ifdef __cplusplus
}
#endif

#endif
