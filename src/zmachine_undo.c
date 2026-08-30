/*
 * zmachine_undo.c
 *
 * Dynamically allocated one-level undo snapshots for the Z-machine runtime.
 *
 * An undo save captures only the Z-machine "state of play": dynamic memory,
 * evaluation-stack contents, routine frames, and the continuation of the
 * save_undo instruction. Interpreter presentation state, host input buffers,
 * output streams, and PRNG state are intentionally not captured because the
 * Z-machine standard explicitly excludes them from saved game state.
 */

#include "tclzmachine.h"
#include "zmachine_undo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Heap-owned snapshot; allocated only after a story actually requests undo. */
struct ZMachineUndoState {
    uint8_t *dynamic_memory;
    size_t dynamic_memory_size;

    uint16_t *stack;
    size_t sp;

    ZMachineFrame *frames;
    size_t frame_count;

    uint32_t resume_pc;
    uint8_t store_variable;
};

/* Put the VM into its terminal error state for an invalid cached snapshot. */
static int undo_error(ZMachine *vm, const char *message)
{
    if (vm) {
        vm->state = ZM_STATE_ERROR;
        snprintf(vm->error, sizeof(vm->error), "%s", message);
    }
    return ZM_UNDO_ERROR;
}

/* Free a detached snapshot object and each separately-sized payload buffer. */
static void free_snapshot(ZMachineUndoState *snapshot)
{
    if (!snapshot)
        return;
    free(snapshot->dynamic_memory);
    free(snapshot->stack);
    free(snapshot->frames);
    free(snapshot);
}

void zmachine_undo_discard(ZMachine *vm)
{
    if (!vm)
        return;
    free_snapshot(vm->undo_state);
    vm->undo_state = NULL;
}

int zmachine_undo_save(ZMachine *vm,
                       uint32_t resume_pc,
                       uint8_t store_variable)
{
    ZMachineUndoState *snapshot;

    if (!vm || !vm->memory || vm->static_memory_addr == 0U)
        return ZM_UNDO_UNAVAILABLE;
    if ((size_t)resume_pc > vm->memory_size)
        return ZM_UNDO_UNAVAILABLE;

    snapshot = (ZMachineUndoState *)calloc(1U, sizeof(*snapshot));
    if (!snapshot)
        return ZM_UNDO_UNAVAILABLE;

    snapshot->dynamic_memory_size = vm->static_memory_addr;
    snapshot->dynamic_memory =
        (uint8_t *)malloc(snapshot->dynamic_memory_size);
    if (!snapshot->dynamic_memory) {
        free_snapshot(snapshot);
        return ZM_UNDO_UNAVAILABLE;
    }
    memcpy(snapshot->dynamic_memory, vm->memory,
           snapshot->dynamic_memory_size);

    snapshot->sp = vm->sp;
    if (snapshot->sp > 0U) {
        snapshot->stack =
            (uint16_t *)malloc(snapshot->sp * sizeof(snapshot->stack[0]));
        if (!snapshot->stack) {
            free_snapshot(snapshot);
            return ZM_UNDO_UNAVAILABLE;
        }
        memcpy(snapshot->stack, vm->stack,
               snapshot->sp * sizeof(snapshot->stack[0]));
    }

    snapshot->frame_count = vm->frame_count;
    if (snapshot->frame_count > 0U) {
        snapshot->frames = (ZMachineFrame *)malloc(
            snapshot->frame_count * sizeof(snapshot->frames[0]));
        if (!snapshot->frames) {
            free_snapshot(snapshot);
            return ZM_UNDO_UNAVAILABLE;
        }
        memcpy(snapshot->frames, vm->frames,
               snapshot->frame_count * sizeof(snapshot->frames[0]));
    }

    snapshot->resume_pc = resume_pc;
    snapshot->store_variable = store_variable;

    /* Do not discard the previous undo point until the replacement is complete. */
    free_snapshot(vm->undo_state);
    vm->undo_state = snapshot;
    return ZM_UNDO_SUCCESS;
}

int zmachine_undo_restore(ZMachine *vm)
{
    ZMachineUndoState *snapshot;
    uint16_t preserved_flags2;

    if (!vm || !vm->memory)
        return ZM_UNDO_UNAVAILABLE;
    snapshot = vm->undo_state;
    if (!snapshot)
        return ZM_UNDO_UNAVAILABLE;

    if (snapshot->dynamic_memory_size != (size_t)vm->static_memory_addr ||
        snapshot->dynamic_memory_size > vm->memory_size ||
        snapshot->sp > sizeof(vm->stack) / sizeof(vm->stack[0]) ||
        snapshot->frame_count > ZM_MAX_FRAMES ||
        (size_t)snapshot->resume_pc > vm->memory_size) {
        return undo_error(vm, "cached undo state is incompatible with the loaded story");
    }

    /* Flags 2 is interpreter/live-session state and survives restore/undo. */
    preserved_flags2 = (uint16_t)(((uint16_t)vm->memory[0x10U] << 8) |
                                  vm->memory[0x11U]);

    memcpy(vm->memory, snapshot->dynamic_memory,
           snapshot->dynamic_memory_size);
    vm->memory[0x10U] = (uint8_t)(preserved_flags2 >> 8);
    vm->memory[0x11U] = (uint8_t)preserved_flags2;
    vm->flags2 = preserved_flags2;

    vm->sp = snapshot->sp;
    if (snapshot->sp > 0U)
        memcpy(vm->stack, snapshot->stack,
               snapshot->sp * sizeof(vm->stack[0]));

    vm->frame_count = snapshot->frame_count;
    if (snapshot->frame_count > 0U)
        memcpy(vm->frames, snapshot->frames,
               snapshot->frame_count * sizeof(vm->frames[0]));

    /*
     * A successful restore resumes at the original save_undo and makes that
     * opcode return 2. The snapshot was taken before its result was stored, so
     * variable 0 correctly receives one pushed value rather than retaining the
     * earlier success value 1 as well.
     */
    if (zmachine_variable_write(vm, snapshot->store_variable, 0, 2U) != TCL_OK)
        return ZM_UNDO_ERROR;

    vm->pc = snapshot->resume_pc;
    vm->state = ZM_STATE_READY;
    vm->error[0] = '\0';
    return ZM_UNDO_SUCCESS;
}
