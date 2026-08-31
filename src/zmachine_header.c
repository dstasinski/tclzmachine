/*
 * zmachine_header.c
 *
 * Interpreter-owned Z-machine header fields and capability normalization.
 *
 * Story files contain several bytes which are not author-controlled runtime
 * data: the interpreter must rewrite them after loading, restarting, and
 * restoring so the game sees the capabilities of the interpreter which is
 * actually running it. tclzmachine intentionally exposes a conservative
 * text-only environment: no pictures, mouse, sound, timed keyboard input, or
 * fixed-pitch font are advertised. Plain Tcl output also advertises no colour
 * or selectable emphasis. When the host explicitly enables mIRC presentation,
 * colour plus bold and italic become real output capabilities and are advertised
 * to the story; reverse video remains available through set_text_style without
 * a separate header capability bit. One-level undo is implemented, so a story's
 * request for undo is left intact rather than being cleared.
 *
 * The formal Standards revision bytes remain 0.0. The Z-machine Standard says
 * an interpreter should advertise revision n.m only when it obeys that revision
 * completely; release hardening is still in progress, so claiming 1.0 or 1.1
 * here would be premature even though individual 1.1 features are implemented.
 */

#include "tclzmachine.h"

#include <stddef.h>

#define ZM_HEADER_MINIMUM_SIZE 64U
#define ZM_INTERPRETER_NUMBER 2U       /* Apple IIe: legacy text-safe identity. */
#define ZM_INTERPRETER_VERSION ((uint8_t)'T')
#define ZM_SCREEN_HEIGHT_LINES 255U    /* Infinite scrolling text surface. */
#define ZM_SCREEN_WIDTH_CHARS 80U

/* Write a big-endian word to a header location already known to be in range. */
static void write_be16(uint8_t *memory, size_t address, uint16_t value)
{
    memory[address] = (uint8_t)(value >> 8);
    memory[address + 1U] = (uint8_t)value;
}

/*
 * Clear unsupported header-extension Flags 3 when the table is writable.
 *
 * Standard 1.1 currently defines bit 0 as a request for transparency, a V6
 * presentation feature which this runtime cannot provide. Unused bits must also
 * be zeroed by the interpreter. The extension normally resides in dynamic
 * memory; if a malformed story points it into static memory, leave it untouched
 * rather than violating the VM's immutable-story boundary.
 */
static void refresh_flags3(ZMachine *vm)
{
    size_t extension;
    uint16_t words;

    if (!vm || vm->version < 5U || vm->header_extension_addr == 0U)
        return;

    extension = vm->header_extension_addr;
    if (extension + 1U >= vm->memory_size)
        return;

    words = (uint16_t)(((uint16_t)vm->memory[extension] << 8) |
                       vm->memory[extension + 1U]);
    if (words < 4U)
        return;

    /* Word 4 occupies extension+8..+9. */
    if (extension + 9U >= vm->memory_size ||
        extension + 9U >= (size_t)vm->static_memory_addr)
        return;

    vm->memory[extension + 8U] = 0U;
    vm->memory[extension + 9U] = 0U;
}

void zmachine_refresh_interpreter_header(ZMachine *vm)
{
    uint8_t flags1;
    uint16_t flags2;

    if (!vm || !vm->memory || vm->memory_size < ZM_HEADER_MINIMUM_SIZE)
        return;

    flags1 = vm->memory[0x01U];
    flags2 = (uint16_t)(((uint16_t)vm->memory[0x10U] << 8) |
                        vm->memory[0x11U]);

    if (vm->version == 3U) {
        /*
         * No separate status-line or upper-window display is exposed to Tcl.
         * The variable-pitch-default bit is also cleared because this runtime
         * has no visible font/pitch distinction.
         */
        flags1 |= 0x10U;
        flags1 &= (uint8_t)~0x20U;
        flags1 &= (uint8_t)~0x40U;
    } else if (vm->version >= 4U) {
        /* Fixed-space style and timed input are unavailable in every format. */
        flags1 &= (uint8_t)~(0x10U | 0x80U);

        if (vm->mirc_output_enabled) {
            /* mIRC has genuine bold and italic control codes. */
            flags1 |= (uint8_t)(0x04U | 0x08U);
        } else {
            flags1 &= (uint8_t)~(0x04U | 0x08U);
        }

        if (vm->version >= 5U) {
            if (vm->mirc_output_enabled)
                flags1 |= 0x01U;  /* mIRC foreground/background colours. */
            else
                flags1 &= (uint8_t)~0x01U;
        }
        if (vm->version >= 6U)
            flags1 &= (uint8_t)~(0x02U | 0x20U); /* No pictures/sound. */
    }

    if (vm->version >= 5U) {
        /*
         * The game-request bits for pictures/character graphics, mouse, sound,
         * and (V6+) menus must be cleared when those facilities are unavailable.
         * Bit 4 (undo) remains untouched because save_undo/restore_undo work.
         * Bit 0 (transcripting) is live session state and also remains untouched.
         * Bit 6 (colours requested by the story) is deliberately preserved: when
         * mIRC presentation is enabled the interpreter can now honor that request.
         */
        flags2 &= (uint16_t)~(0x0008U | 0x0020U | 0x0080U);
        if (vm->version >= 6U)
            flags2 &= (uint16_t)~0x0100U;
    }

    vm->memory[0x01U] = flags1;
    vm->memory[0x10U] = (uint8_t)(flags2 >> 8);
    vm->memory[0x11U] = (uint8_t)flags2;
    vm->flags1 = flags1;
    vm->flags2 = flags2;

    if (vm->version >= 4U) {
        /*
         * Legacy Version-5 games sometimes branch on the historical interpreter
         * number instead of using set_font. Apple IIe identity (2) is a useful
         * text-only compatibility choice: Beyond Zork specifically avoids its
         * character-graphics font on that interpreter, whereas the MS-DOS path
         * can emit IBM graphics codes when font 3 is unavailable. The Standard
         * also notes that many ITF interpreters traditionally use number 2.
         *
         * Header dimensions model a normal 80-column text device with unlimited
         * vertical scrollback, so the interpreter never needs a [MORE] prompt.
         */
        vm->memory[0x1eU] = ZM_INTERPRETER_NUMBER;
        vm->memory[0x1fU] = ZM_INTERPRETER_VERSION;
        vm->memory[0x20U] = ZM_SCREEN_HEIGHT_LINES;
        vm->memory[0x21U] = ZM_SCREEN_WIDTH_CHARS;
    }

    if (vm->version >= 5U) {
        /* V5-style text units correspond exactly to one character cell. */
        write_be16(vm->memory, 0x22U, ZM_SCREEN_WIDTH_CHARS);
        write_be16(vm->memory, 0x24U, ZM_SCREEN_HEIGHT_LINES);
        vm->memory[0x26U] = 1U; /* width of '0' in units */
        vm->memory[0x27U] = 1U; /* font height in units */

        /* Deterministic defaults used when a story explicitly selects default. */
        vm->memory[0x2cU] = 2U; /* black background */
        vm->memory[0x2dU] = 9U; /* white foreground */
    }

    /* Do not claim formal Standard conformance before the final release audit. */
    vm->memory[0x32U] = 0U;
    vm->memory[0x33U] = 0U;

    refresh_flags3(vm);
}
