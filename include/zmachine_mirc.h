/*
 * zmachine_mirc.h
 *
 * Optional mIRC rendering for the Tcl-facing presentation boundary.
 *
 * Canonical VM output remains plain UTF-8.  When a Tcl session selects the
 * `mirc` output format, this layer mirrors only text which actually reaches
 * stream 1/window 0 and decorates it with IRC control codes corresponding to
 * the Z-machine's current colour and text-style state.
 */
#ifndef ZMACHINE_MIRC_H
#define ZMACHINE_MIRC_H

#include "tclzmachine.h"

#include <stddef.h>
#include <tcl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Enable/disable mIRC presentation for one VM. Plain output is the default. */
int zmachine_mirc_set_enabled(ZMachine *vm, int enabled);
int zmachine_mirc_enabled(const ZMachine *vm);

/* Formatted output accumulated during the most recent public zmachine_run(). */
const char *zmachine_mirc_output_data(const ZMachine *vm);
int zmachine_mirc_output_length(const ZMachine *vm);

/*
 * Wrap already-rendered mIRC text without splitting formatting sequences.
 * Formatting bytes count toward max_bytes because IRC message limits are byte
 * limits. Inserted lines re-establish the active formatting state so a bot may
 * send each newline-delimited result as an independent PRIVMSG.
 */
int zmachine_mirc_wrap_output(const char *text,
                              size_t length,
                              size_t max_bytes,
                              Tcl_DString *result);

#ifdef __cplusplus
}
#endif

#endif
