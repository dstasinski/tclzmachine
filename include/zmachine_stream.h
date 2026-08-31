#ifndef ZMACHINE_STREAM_H
#define ZMACHINE_STREAM_H

/*
 * zmachine_stream.h
 *
 * Host-file backing for the Z-machine command/replay and transcript streams.
 * The VM owns only stream selection and the current file position; Tcl remains
 * responsible for choosing safe host paths when a story first selects an
 * external stream.
 */

#include "tclzmachine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Complete or preconfigure one external stream file. */
int zmachine_stream_file(ZMachine *vm, const char *kind, const char *path);

/* Decline the currently pending replay/transcript/record path request. */
int zmachine_cancel_stream_file(ZMachine *vm);

/* Read-only host metadata used by zmachine::info. */
const char *zmachine_pending_stream_request(const ZMachine *vm);
int zmachine_current_input_stream(const ZMachine *vm);
int zmachine_command_recording_selected(const ZMachine *vm);

#ifdef __cplusplus
}
#endif

#endif
