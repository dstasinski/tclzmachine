/*
 * zmachine_input_preload.c
 *
 * Version 5+ preloaded-line and terminating-key compatibility wrapper for
 * Z-machine read/aread.
 *
 * In V5 and later, byte 1 of a text buffer may already contain a nonzero
 * character count when `read` begins. Those bytes are text left over from an
 * interrupted earlier input operation, and newly supplied input must be
 * appended after them rather than replacing them. The original input engine
 * owns lowercasing, truncation, story-memory writes, dictionary tokenization,
 * and pending-input consumption; this wrapper only constructs the complete
 * logical line before delegating to that engine.
 *
 * The host may also terminate V5+ line input with a function key allowed by the
 * story's terminating-character table. Such a key is input-only and must never
 * be copied into the text buffer. `zmachine::key` therefore queues an empty line
 * and records its exact ZSCII code in pending_input_terminator. After the base
 * line engine has stored/tokenized the line, this wrapper replaces the base
 * routine's normal Enter result with that queued terminator.
 *
 * V1-V4 have no preloaded-count convention, but Enter still flows through the
 * same completion helper so pending_input_terminator is returned to its normal
 * baseline after every successful line read.
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
 * Finish a successful delegated read with the host-selected terminator.
 *
 * A zero field is treated as the normal carriage return for compatibility with
 * synthetic/unit-test VMs which were initialized before the field was added.
 * On failure, leave the queued terminator untouched so the host may retry the
 * same suspended input operation without losing its exact completion reason.
 */
static int finish_line_input(ZMachine *vm,
                             int rc,
                             uint16_t *terminator,
                             uint16_t queued_terminator)
{
    if (rc != TCL_OK)
        return rc;

    if (terminator)
        *terminator = queued_terminator != 0U ? queued_terminator : 13U;
    if (vm)
        vm->pending_input_terminator = 13U;
    return TCL_OK;
}

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
    uint16_t queued_terminator;
    int rc;

    queued_terminator = vm && vm->pending_input_terminator != 0U ?
                        vm->pending_input_terminator : 13U;

    if (!vm || vm->version <= 4U || !vm->memory || !vm->input_available) {
        rc = zmachine_input_read_line_base(vm, text_buffer,
                                           parse_buffer, terminator);
        return finish_line_input(vm, rc, terminator, queued_terminator);
    }

    if ((size_t)text_buffer + 1U >= vm->memory_size)
        return TCL_ERROR;

    max_chars = vm->memory[text_buffer];
    prefix_length = vm->memory[(size_t)text_buffer + 1U];
    if (prefix_length == 0U) {
        rc = zmachine_input_read_line_base(vm, text_buffer,
                                           parse_buffer, terminator);
        return finish_line_input(vm, rc, terminator, queued_terminator);
    }

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
    return finish_line_input(vm, rc, terminator, queued_terminator);
}
