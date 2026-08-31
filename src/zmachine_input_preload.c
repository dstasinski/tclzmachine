/*
 * zmachine_input_preload.c
 *
 * Version 5+ preloaded-line compatibility wrapper for Z-machine read/aread.
 *
 * In V5 and later, byte 1 of a text buffer may already contain a nonzero
 * character count when `read` begins. Those bytes are text left over from an
 * interrupted earlier input operation, and newly supplied input must be
 * appended after them rather than replacing them. The original input engine
 * owns lowercasing, truncation, story-memory writes, dictionary tokenization,
 * and pending-input consumption; this wrapper only constructs the complete
 * logical line before delegating to that engine.
 *
 * V1-V4 have no preloaded-count convention and delegate unchanged. Keeping the
 * merge here avoids duplicating the dictionary/tokenization implementation and
 * preserves one authoritative path for parse-buffer positions.
 */

#include "tclzmachine.h"
#include "zmachine_input.h"

#include <stddef.h>

/* Original line-input implementation, renamed at compile time by CMake. */
extern int zmachine_input_read_line_base(ZMachine *vm,
                                         uint16_t text_buffer,
                                         uint16_t parse_buffer,
                                         uint16_t *terminator);

/*
 * Satisfy one line-input request, honoring V5+ text already in the buffer.
 *
 * On a delegated failure, restore the Tcl-side pending input exactly as the
 * caller supplied it so host code retains the same retry/cancel behavior as the
 * base input path. Story-memory writes performed before a lower-level failure
 * retain the base routine's existing semantics.
 */
int zmachine_input_read_line(ZMachine *vm,
                             uint16_t text_buffer,
                             uint16_t parse_buffer,
                             uint16_t *terminator)
{
    Tcl_DString original;
    Tcl_DString merged;
    size_t prefix_length;
    size_t prefix_start;
    size_t max_chars;
    int rc;

    if (!vm || vm->version <= 4U || !vm->memory || !vm->input_available)
        return zmachine_input_read_line_base(vm, text_buffer,
                                             parse_buffer, terminator);

    if ((size_t)text_buffer + 1U >= vm->memory_size)
        return TCL_ERROR;

    max_chars = vm->memory[text_buffer];
    prefix_length = vm->memory[(size_t)text_buffer + 1U];
    if (prefix_length == 0U)
        return zmachine_input_read_line_base(vm, text_buffer,
                                             parse_buffer, terminator);

    prefix_start = (size_t)text_buffer + 2U;
    if (prefix_length > max_chars ||
        prefix_start + prefix_length > vm->memory_size ||
        prefix_start + prefix_length > (size_t)vm->static_memory_addr)
        return TCL_ERROR;

    Tcl_DStringInit(&original);
    Tcl_DStringInit(&merged);

    Tcl_DStringAppend(&original,
                      Tcl_DStringValue(&vm->pending_input),
                      Tcl_DStringLength(&vm->pending_input));
    Tcl_DStringAppend(&merged,
                      (const char *)(vm->memory + prefix_start),
                      (int)prefix_length);
    Tcl_DStringAppend(&merged,
                      Tcl_DStringValue(&original),
                      Tcl_DStringLength(&original));

    Tcl_DStringSetLength(&vm->pending_input, 0);
    Tcl_DStringAppend(&vm->pending_input,
                      Tcl_DStringValue(&merged),
                      Tcl_DStringLength(&merged));

    rc = zmachine_input_read_line_base(vm, text_buffer,
                                       parse_buffer, terminator);
    if (rc != TCL_OK) {
        Tcl_DStringSetLength(&vm->pending_input, 0);
        Tcl_DStringAppend(&vm->pending_input,
                          Tcl_DStringValue(&original),
                          Tcl_DStringLength(&original));
    }

    Tcl_DStringFree(&merged);
    Tcl_DStringFree(&original);
    return rc;
}
