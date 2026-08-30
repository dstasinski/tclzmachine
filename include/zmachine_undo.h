/*
 * zmachine_undo.h
 *
 * One-level in-memory undo support for Version 5 and later Z-machine stories.
 *
 * The Z-machine defines undo as an internal save/restore of the state of play.
 * That state consists of dynamic memory, the evaluation stack, active routine
 * frames, and the saved execution continuation. Presentation state, selected
 * streams, queued host input, and the interpreter random-number generator are
 * deliberately not part of an undo snapshot.
 */

#ifndef ZMACHINE_UNDO_H
#define ZMACHINE_UNDO_H

#include <stdint.h>

#include "zmachine_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Result codes for snapshot operations which may be unavailable without error. */
#define ZM_UNDO_UNAVAILABLE 0
#define ZM_UNDO_SUCCESS 1
#define ZM_UNDO_ERROR (-1)

/*
 * Replace the current one-level undo snapshot.
 *
 * resume_pc is the address immediately after save_undo's store-variable byte,
 * and store_variable is the variable into which save_undo must later write 2
 * when restore_undo resumes the saved execution point. The snapshot is taken
 * before save_undo stores its immediate success result of 1.
 *
 * Returns ZM_UNDO_SUCCESS on success or ZM_UNDO_UNAVAILABLE if memory for the
 * snapshot cannot be obtained. Allocation failure is a supported capability
 * failure, not a terminal VM error.
 */
int zmachine_undo_save(ZMachine *vm,
                       uint32_t resume_pc,
                       uint8_t store_variable);

/*
 * Restore the most recent undo snapshot.
 *
 * On success, dynamic memory and execution stacks are restored, Flags 2 from
 * the current live header are preserved as required by the standard, and the
 * saved save_undo result is stored as 2 before execution resumes at resume_pc.
 * Host presentation/input/RNG state is intentionally left untouched.
 *
 * Returns ZM_UNDO_SUCCESS when control has moved to the saved continuation,
 * ZM_UNDO_UNAVAILABLE when no snapshot exists, or ZM_UNDO_ERROR if the cached
 * snapshot is internally inconsistent with the currently loaded story.
 */
int zmachine_undo_restore(ZMachine *vm);

/* Release any cached undo state, for story replacement, reset, or destruction. */
void zmachine_undo_discard(ZMachine *vm);

#ifdef __cplusplus
}
#endif

#endif
