/*
 * zmachine_input_host.c
 *
 * Host-facing validation for line input queued through `zmachine::command`.
 *
 * Tcl strings are UTF-8, whereas a Z-machine read buffer contains one-byte
 * ZSCII character codes. The current runtime advertises Unicode keyboard input
 * only for the printable ASCII subset, so copying arbitrary Tcl UTF-8 bytes into
 * a story buffer would falsely turn one Unicode character into two or three
 * unrelated ZSCII characters. Reject unsupported host text at this boundary
 * instead of silently corrupting the player's command.
 *
 * This layer intentionally does not alter VM state on an API validation error.
 * A session suspended on `read`/`read_char` therefore remains resumable and the
 * caller can retry with valid input. The underlying queue implementation still
 * owns halted/error-state checks and cooperative WAITING_INPUT -> READY state.
 */

#include "tclzmachine.h"

#include <stdio.h>

/* Original queue implementation, renamed at compile time from zmachine.c. */
extern int zmachine_supply_input_base(ZMachine *vm, const char *line);

/* Queue one printable-ASCII line for the next cooperative input request. */
int zmachine_supply_input(ZMachine *vm, const char *line)
{
    const unsigned char *p;
    int rc;

    if (!vm || !line)
        return TCL_ERROR;

    for (p = (const unsigned char *)line; *p != 0U; ++p) {
        if (*p < 32U || *p > 126U) {
            snprintf(vm->error, sizeof(vm->error),
                     "line input currently accepts printable ASCII ZSCII only");
            return TCL_ERROR;
        }
    }

    rc = zmachine_supply_input_base(vm, line);
    if (rc == TCL_OK)
        vm->error[0] = '\0';
    return rc;
}
