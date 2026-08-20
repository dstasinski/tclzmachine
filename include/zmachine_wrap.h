/*
 * zmachine_wrap.h
 *
 * Presentation-layer word wrapping for text returned to Tcl callers.
 *
 * The Z-machine core always produces canonical, unwrapped UTF-8 text.  This
 * helper is deliberately separate from VM execution so IRC-oriented line
 * limits never alter story memory, parser input, save-state data, or the
 * interpreter's internal notion of output.
 */
#ifndef ZMACHINE_WRAP_H
#define ZMACHINE_WRAP_H

#include <stddef.h>
#include <tcl.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Append UTF-8 text to result, optionally wrapping physical lines.
 *
 * max_bytes == 0 disables wrapping and copies the input unchanged.  Otherwise
 * each emitted line is limited to at most max_bytes UTF-8 bytes whenever
 * possible.  Existing newlines are preserved.  Wrapping prefers whitespace
 * boundaries; if a single word is longer than max_bytes it is split only at a
 * valid UTF-8 character boundary.
 *
 * The destination Tcl_DString is cleared before output is appended.
 */
int zmachine_wrap_output(const char *text,
                         size_t length,
                         size_t max_bytes,
                         Tcl_DString *result);

#ifdef __cplusplus
}
#endif

#endif
