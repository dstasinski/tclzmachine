/*
 * mirc_presentation.c
 *
 * Regression coverage for optional IRC/mIRC rendering. Canonical VM output must
 * stay plain while the presentation layer preserves Z-machine text styles,
 * standard colours, true-colour approximation, per-window state, capability
 * advertisement, and formatting-safe word wrapping.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"
#include "zmachine_mirc.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ZMachine *new_vm(uint8_t version)
{
    ZMachine *vm = zmachine_create();

    assert(vm != NULL);
    vm->memory = (uint8_t *)calloc(512U, 1U);
    assert(vm->memory != NULL);
    vm->memory_size = 512U;
    vm->version = version;
    vm->static_memory_addr = 0x200U;
    vm->globals_addr = 0x100U;
    vm->state = ZM_STATE_READY;
    vm->current_window = 0U;
    vm->output_stream1_enabled = 1;
    return vm;
}

static void write_style(ZMachine *vm, uint32_t pc, uint8_t style)
{
    vm->pc = pc;
    vm->memory[pc] = 0xF1U;       /* VAR:17 set_text_style */
    vm->memory[pc + 1U] = 0x7FU; /* one small constant */
    vm->memory[pc + 2U] = style;
    assert(zmachine_step(vm) == TCL_OK);
    assert(vm->pc == pc + 3U);
}

static void write_colour(ZMachine *vm, uint32_t pc,
                         uint8_t foreground, uint8_t background)
{
    vm->pc = pc;
    vm->memory[pc] = 0xDBU;       /* variable-form 2OP:27 set_colour */
    vm->memory[pc + 1U] = 0x5FU; /* two small constants */
    vm->memory[pc + 2U] = foreground;
    vm->memory[pc + 3U] = background;
    assert(zmachine_step(vm) == TCL_OK);
    assert(vm->pc == pc + 4U);
}

int main(void)
{
    ZMachine *vm;

    /* Plain is the default and advertises no colour/bold/italic capability. */
    vm = new_vm(5U);
    zmachine_refresh_interpreter_header(vm);
    assert((vm->memory[0x01U] & 0x01U) == 0U);
    assert((vm->memory[0x01U] & 0x04U) == 0U);
    assert((vm->memory[0x01U] & 0x08U) == 0U);

    assert(zmachine_mirc_set_enabled(vm, 1) == TCL_OK);
    assert(zmachine_mirc_enabled(vm));
    assert((vm->memory[0x01U] & 0x01U) != 0U); /* colour */
    assert((vm->memory[0x01U] & 0x04U) != 0U); /* bold */
    assert((vm->memory[0x01U] & 0x08U) != 0U); /* italic */
    assert((vm->memory[0x01U] & 0x10U) == 0U); /* fixed pitch still unavailable */

    /*
     * Standard 1.1 style calls activate requested nonzero bits; Roman clears
     * every style. mIRC output is separate from the canonical plain string.
     */
    write_style(vm, 0x20U, 2U); /* bold */
    zmachine_output_append(vm, "B", 1U);
    write_style(vm, 0x30U, 4U); /* add italic */
    zmachine_output_append(vm, "I", 1U);
    write_style(vm, 0x40U, 0U); /* Roman */
    write_colour(vm, 0x50U, 3U, 1U); /* red, default background */
    zmachine_output_append(vm, "R", 1U);
    write_colour(vm, 0x60U, 1U, 1U); /* both defaults */
    zmachine_output_append(vm, "P", 1U);

    assert(strcmp(zmachine_output_data(vm), "BIRP") == 0);
    assert(strcmp(zmachine_mirc_output_data(vm),
                  "\x02" "B" "\x0f"
                  "\x02\x1d" "I" "\x0f"
                  "\x03" "04" "R" "\x0f"
                  "P") == 0);
    zmachine_destroy(vm);

    /* Fixed pitch is tracked but intentionally has no IRC formatting analogue. */
    vm = new_vm(5U);
    assert(zmachine_mirc_set_enabled(vm, 1) == TCL_OK);
    write_style(vm, 0x20U, 8U);
    zmachine_output_append(vm, "fixed", 5U);
    assert(strcmp(zmachine_output_data(vm), "fixed") == 0);
    assert(strcmp(zmachine_mirc_output_data(vm), "fixed") == 0);
    zmachine_destroy(vm);

    /* Upper-window styling must not leak into lower-window narrative output. */
    vm = new_vm(5U);
    assert(zmachine_mirc_set_enabled(vm, 1) == TCL_OK);
    vm->current_window = 1U;
    write_style(vm, 0x20U, 2U);
    vm->current_window = 0U;
    zmachine_output_append(vm, "lower", 5U);
    assert(strcmp(zmachine_mirc_output_data(vm), "lower") == 0);
    zmachine_destroy(vm);

    /* Standard 1.1 true red 0x001d approximates to classic mIRC red (04). */
    vm = new_vm(5U);
    assert(zmachine_mirc_set_enabled(vm, 1) == TCL_OK);
    vm->pc = 0x20U;
    vm->memory[0x20U] = 0xBEU;
    vm->memory[0x21U] = 13U;   /* EXT:13 set_true_colour */
    vm->memory[0x22U] = 0x0FU; /* two large constants */
    vm->memory[0x23U] = 0x00U;
    vm->memory[0x24U] = 0x1DU; /* red */
    vm->memory[0x25U] = 0xFFU;
    vm->memory[0x26U] = 0xFFU; /* default background */
    assert(zmachine_step(vm) == TCL_OK);
    assert(vm->pc == 0x27U);
    zmachine_output_append(vm, "T", 1U);
    assert(strcmp(zmachine_mirc_output_data(vm),
                  "\x03" "04" "T" "\x0f") == 0);
    zmachine_destroy(vm);

    /*
     * Wrapping never splits formatting sequences, and inserted IRC lines
     * re-establish the active state so each line can be sent independently.
     */
    {
        Tcl_DString wrapped;
        const char formatted[] = "\x02" "hello world" "\x0f";

        Tcl_DStringInit(&wrapped);
        assert(zmachine_mirc_wrap_output(formatted, sizeof(formatted) - 1U,
                                         8U, &wrapped) == TCL_OK);
        assert(strcmp(Tcl_DStringValue(&wrapped),
                      "\x02" "hello\n\x02" "world" "\x0f") == 0);
        Tcl_DStringFree(&wrapped);
    }

    puts("mIRC presentation tests passed");
    return 0;
}
