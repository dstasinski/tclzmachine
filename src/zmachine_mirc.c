/*
 * zmachine_mirc.c
 *
 * Optional mIRC presentation layer for Tcl/IRC hosts.
 *
 * The VM's canonical output remains unformatted UTF-8. This module performs two
 * deliberately separate jobs at presentation boundaries:
 *
 *  1. It sits immediately above the existing text-only presentation dispatcher
 *     and records the observable state of set_colour, set_true_colour, and
 *     set_text_style without letting those opcodes leak IRC policy into the
 *     ordinary executor.
 *
 *  2. It sits immediately below the external transcript wrapper at the output
 *     boundary. Only bytes which the canonical stream-1/window-0 implementation
 *     actually accepts are mirrored into a second, optional mIRC string. Stream
 *     3, disabled stream 1, and upper-window/status output therefore retain the
 *     exact same routing semantics as plain output.
 *
 * Plain output is the default. Enabling the mIRC format is a Tcl/host choice and
 * causes the interpreter header to advertise colour, bold, and italic support.
 * Fixed-pitch text is tracked as Z-machine state but intentionally has no IRC
 * visual equivalent and is not advertised as available.
 */

#include "tclzmachine.h"
#include "zmachine_decode.h"
#include "zmachine_exec.h"
#include "zmachine_mirc.h"
#include "zmachine_stream.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIRC_BOLD ((char)0x02)
#define MIRC_COLOUR ((char)0x03)
#define MIRC_RESET ((char)0x0f)
#define MIRC_REVERSE ((char)0x16)
#define MIRC_ITALIC ((char)0x1d)

#define ZM_STYLE_REVERSE 0x01U
#define ZM_STYLE_BOLD 0x02U
#define ZM_STYLE_ITALIC 0x04U
#define ZM_STYLE_FIXED 0x08U
#define ZM_STYLE_MASK 0x0fU

#define ZM_COLOUR_DEFAULT 0U
#define ZM_COLOUR_STANDARD 1U
#define ZM_COLOUR_TRUE 2U

/* Existing implementations immediately below/above this inserted layer. */
extern int zmachine_step_visual_base(ZMachine *vm);
extern void zmachine_output_append_plain_base(ZMachine *vm,
                                               const char *text,
                                               size_t len);
extern int zmachine_run_mirc_base(ZMachine *vm);
extern void zmachine_destroy_mirc_base(ZMachine *vm);
extern int zmachine_load_story_mirc_base(ZMachine *vm, const char *path);
extern int zmachine_reset_mirc_base(ZMachine *vm);
extern void zmachine_stream_after_restart_mirc_base(ZMachine *vm);

typedef struct MircWindowState {
    uint8_t style;
    uint8_t foreground_mode;
    uint8_t background_mode;
    int16_t foreground_value;
    int16_t background_value;
} MircWindowState;

struct ZMachineMircState {
    Tcl_DString output;
    MircWindowState windows[2];
    MircWindowState emitted;
    int emitted_valid;
    int trailing_reset;
};

typedef struct MircParsedState {
    int bold;
    int italic;
    int reverse;
    int have_foreground;
    int have_background;
    unsigned foreground;
    unsigned background;
} MircParsedState;

/* Traditional mIRC palette approximations used for 15-bit Z true colours. */
typedef struct MircRgb {
    unsigned char r;
    unsigned char g;
    unsigned char b;
} MircRgb;

static const MircRgb mirc_palette[16] = {
    {255U, 255U, 255U}, /* 00 white */
    {0U, 0U, 0U},       /* 01 black */
    {0U, 0U, 127U},     /* 02 navy/blue */
    {0U, 147U, 0U},     /* 03 green */
    {255U, 0U, 0U},     /* 04 red */
    {127U, 0U, 0U},     /* 05 brown/maroon */
    {156U, 0U, 156U},   /* 06 purple */
    {252U, 127U, 0U},   /* 07 orange */
    {255U, 255U, 0U},   /* 08 yellow */
    {0U, 252U, 0U},     /* 09 light green */
    {0U, 147U, 147U},   /* 10 teal/cyan */
    {0U, 255U, 255U},   /* 11 light cyan */
    {0U, 0U, 252U},     /* 12 light blue */
    {255U, 0U, 255U},   /* 13 pink */
    {127U, 127U, 127U}, /* 14 grey */
    {210U, 210U, 210U}  /* 15 light grey */
};

static int mirc_vm_error(ZMachine *vm, const char *message)
{
    if (vm) {
        vm->state = ZM_STATE_ERROR;
        snprintf(vm->error, sizeof(vm->error), "%s", message);
    }
    return TCL_ERROR;
}

static void clear_render_tracking(ZMachineMircState *state)
{
    if (!state)
        return;
    Tcl_DStringSetLength(&state->output, 0);
    memset(&state->emitted, 0, sizeof(state->emitted));
    state->emitted_valid = 0;
    state->trailing_reset = 0;
}

static void reset_presentation_state(ZMachineMircState *state)
{
    if (!state)
        return;
    memset(state->windows, 0, sizeof(state->windows));
    clear_render_tracking(state);
}

static ZMachineMircState *ensure_state(ZMachine *vm)
{
    ZMachineMircState *state;

    if (!vm)
        return NULL;
    state = vm->mirc_state;
    if (state)
        return state;

    state = (ZMachineMircState *)calloc(1U, sizeof(*state));
    if (!state) {
        snprintf(vm->error, sizeof(vm->error),
                 "%s", "unable to allocate mIRC presentation state");
        return NULL;
    }
    Tcl_DStringInit(&state->output);
    vm->mirc_state = state;
    return state;
}

static void free_state(ZMachine *vm)
{
    ZMachineMircState *state;

    if (!vm || !vm->mirc_state)
        return;
    state = vm->mirc_state;
    Tcl_DStringFree(&state->output);
    free(state);
    vm->mirc_state = NULL;
}

static MircWindowState *current_window_state(ZMachine *vm,
                                             ZMachineMircState *state)
{
    unsigned window;

    if (!vm || !state)
        return NULL;
    window = vm->current_window <= 1U ? vm->current_window : 0U;
    return &state->windows[window];
}

static const MircWindowState *current_window_state_const(
    const ZMachine *vm,
    const ZMachineMircState *state)
{
    unsigned window;

    if (!vm || !state)
        return NULL;
    window = vm->current_window <= 1U ? vm->current_window : 0U;
    return &state->windows[window];
}

static int window_state_equal(const MircWindowState *a,
                              const MircWindowState *b)
{
    return a && b &&
           a->style == b->style &&
           a->foreground_mode == b->foreground_mode &&
           a->background_mode == b->background_mode &&
           a->foreground_value == b->foreground_value &&
           a->background_value == b->background_value;
}

static int window_state_has_formatting(const MircWindowState *state)
{
    if (!state)
        return 0;
    return (state->style & (ZM_STYLE_REVERSE | ZM_STYLE_BOLD |
                            ZM_STYLE_ITALIC)) != 0U ||
           state->foreground_mode != ZM_COLOUR_DEFAULT ||
           state->background_mode != ZM_COLOUR_DEFAULT;
}

/* Map Z-machine standard colour numbers 2..9 to classic mIRC colours. */
static unsigned standard_to_mirc(int16_t colour)
{
    switch (colour) {
    case 2: return 1U;  /* black */
    case 3: return 4U;  /* red */
    case 4: return 3U;  /* green */
    case 5: return 8U;  /* yellow */
    case 6: return 2U;  /* blue */
    case 7: return 6U;  /* magenta */
    case 8: return 10U; /* cyan */
    case 9: return 0U;  /* white */
    default: return 0U;
    }
}

/*
 * Approximate a Standard 1.1 15-bit true colour with the nearest mIRC colour.
 * Z-machine true colour stores red in bits 0..4, green in 5..9, and blue in
 * 10..14 (the published standard red value 0x001d makes that ordering clear).
 */
static unsigned true_to_mirc(int16_t colour)
{
    unsigned value = (unsigned)(uint16_t)colour & 0x7fffU;
    unsigned r = (value & 0x1fU) * 255U / 31U;
    unsigned g = ((value >> 5) & 0x1fU) * 255U / 31U;
    unsigned b = ((value >> 10) & 0x1fU) * 255U / 31U;
    unsigned best = 0U;
    unsigned long best_distance = ~0UL;
    unsigned i;

    for (i = 0U; i < 16U; ++i) {
        long dr = (long)r - (long)mirc_palette[i].r;
        long dg = (long)g - (long)mirc_palette[i].g;
        long db = (long)b - (long)mirc_palette[i].b;
        unsigned long distance =
            (unsigned long)(dr * dr + dg * dg + db * db);
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

static unsigned component_to_mirc(uint8_t mode, int16_t value,
                                  unsigned default_colour)
{
    if (mode == ZM_COLOUR_STANDARD)
        return standard_to_mirc(value);
    if (mode == ZM_COLOUR_TRUE)
        return true_to_mirc(value);
    return default_colour;
}

static void append_colour_code(Tcl_DString *output,
                               const MircWindowState *state)
{
    char code[16];
    unsigned foreground;
    unsigned background;
    int n;

    if (!output || !state ||
        (state->foreground_mode == ZM_COLOUR_DEFAULT &&
         state->background_mode == ZM_COLOUR_DEFAULT))
        return;

    /* White/black are the deterministic text defaults advertised in the header. */
    foreground = component_to_mirc(state->foreground_mode,
                                   state->foreground_value, 0U);
    background = component_to_mirc(state->background_mode,
                                   state->background_value, 1U);

    if (state->background_mode != ZM_COLOUR_DEFAULT)
        n = snprintf(code, sizeof(code), "%c%02u,%02u",
                     MIRC_COLOUR, foreground, background);
    else
        n = snprintf(code, sizeof(code), "%c%02u",
                     MIRC_COLOUR, foreground);

    if (n > 0 && (size_t)n < sizeof(code))
        Tcl_DStringAppend(output, code, n);
}

static void append_window_codes(Tcl_DString *output,
                                const MircWindowState *state)
{
    if (!output || !state)
        return;

    append_colour_code(output, state);
    if ((state->style & ZM_STYLE_BOLD) != 0U)
        Tcl_DStringAppend(output, &((char){MIRC_BOLD}), 1);
    if ((state->style & ZM_STYLE_ITALIC) != 0U)
        Tcl_DStringAppend(output, &((char){MIRC_ITALIC}), 1);
    if ((state->style & ZM_STYLE_REVERSE) != 0U)
        Tcl_DStringAppend(output, &((char){MIRC_REVERSE}), 1);
}

/* Bring the byte stream from the previously emitted state to the current one. */
static void emit_current_state(ZMachine *vm, ZMachineMircState *state)
{
    const MircWindowState *current;

    if (!vm || !state)
        return;
    current = current_window_state_const(vm, state);
    if (!current)
        return;

    if (state->emitted_valid && window_state_equal(current, &state->emitted))
        return;

    if (state->emitted_valid && window_state_has_formatting(&state->emitted))
        Tcl_DStringAppend(&state->output, &((char){MIRC_RESET}), 1);

    if (window_state_has_formatting(current))
        append_window_codes(&state->output, current);

    state->emitted = *current;
    state->emitted_valid = 1;
}

/*
 * Append canonical bytes using the current presentation state.
 *
 * The buffer is kept safe to hand directly to an IRC client: a formatted final
 * physical line ends with Ctrl-O reset. On a later append with unchanged state,
 * that trailing reset is removed so character-at-a-time ZSCII output does not
 * acquire a formatting prefix/reset around every character. Story newlines are
 * treated as IRC message boundaries and re-establish formatting on the next
 * nonempty line.
 */
static void append_rendered_text(ZMachine *vm,
                                 ZMachineMircState *state,
                                 const char *text,
                                 size_t length)
{
    const MircWindowState *current;
    size_t pos = 0U;

    if (!vm || !state || !text || length == 0U)
        return;
    current = current_window_state_const(vm, state);
    if (!current)
        return;

    if (state->trailing_reset) {
        if (state->emitted_valid &&
            window_state_equal(current, &state->emitted)) {
            int n = Tcl_DStringLength(&state->output);
            if (n > 0)
                Tcl_DStringSetLength(&state->output, n - 1);
        } else {
            state->emitted_valid = 0;
        }
        state->trailing_reset = 0;
    }

    while (pos < length) {
        size_t end = pos;

        while (end < length && text[end] != '\n')
            ++end;

        if (end > pos) {
            emit_current_state(vm, state);
            Tcl_DStringAppend(&state->output, text + pos, (int)(end - pos));
        }

        if (end < length && text[end] == '\n') {
            if (state->emitted_valid &&
                window_state_has_formatting(&state->emitted))
                Tcl_DStringAppend(&state->output, &((char){MIRC_RESET}), 1);
            Tcl_DStringAppend(&state->output, "\n", 1);
            state->emitted_valid = 0;
            state->trailing_reset = 0;
            pos = end + 1U;
        } else {
            pos = end;
        }
    }

    if (state->emitted_valid &&
        window_state_has_formatting(&state->emitted)) {
        Tcl_DStringAppend(&state->output, &((char){MIRC_RESET}), 1);
        state->trailing_reset = 1;
    }
}

/* Update one standard-colour component; zero means leave current unchanged. */
static int set_standard_component(ZMachine *vm,
                                  uint16_t operand,
                                  uint8_t *mode,
                                  int16_t *value,
                                  const char *which)
{
    int16_t colour = (int16_t)operand;

    if (colour == 0)
        return TCL_OK;
    if (colour == 1) {
        *mode = ZM_COLOUR_DEFAULT;
        *value = 0;
        return TCL_OK;
    }
    if (colour >= 2 && colour <= 9) {
        *mode = ZM_COLOUR_STANDARD;
        *value = colour;
        return TCL_OK;
    }

    if (which && strcmp(which, "foreground") == 0)
        return mirc_vm_error(vm, "unsupported set_colour foreground value");
    return mirc_vm_error(vm, "unsupported set_colour background value");
}

/* Update one true-colour component; -2 means current/unchanged, -1 default. */
static int set_true_component(ZMachine *vm,
                              uint16_t operand,
                              uint8_t *mode,
                              int16_t *value,
                              const char *which)
{
    int16_t colour = (int16_t)operand;

    if (colour == -2)
        return TCL_OK;
    if (colour == -1) {
        *mode = ZM_COLOUR_DEFAULT;
        *value = 0;
        return TCL_OK;
    }
    if (colour >= 0) {
        *mode = ZM_COLOUR_TRUE;
        *value = colour;
        return TCL_OK;
    }

    /* -3 under-cursor and -4 transparent are Version-6-only facilities. */
    if (which && strcmp(which, "foreground") == 0)
        return mirc_vm_error(vm, "unsupported set_true_colour foreground value");
    return mirc_vm_error(vm, "unsupported set_true_colour background value");
}

/*
 * Intercept presentation state before the existing text-only dispatcher.
 * Version/form/arity checks occur before operand resolution so an illegal or
 * malformed opcode cannot pop stack variable 0 as a side effect of rejection.
 */
int zmachine_step_present(ZMachine *vm)
{
    ZMachineInstruction instruction;
    ZMachineMircState *state;
    MircWindowState *window;
    uint16_t values[ZM_MAX_OPERANDS];
    char decode_error[128];

    if (!vm || !vm->memory)
        return mirc_vm_error(vm, "cannot execute without a loaded story");

    if (!zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                     vm->pc, &instruction,
                                     decode_error, sizeof(decode_error)))
        return mirc_vm_error(vm, decode_error[0] ? decode_error :
                             "unable to decode Z-machine presentation opcode");

    /* VAR:17 set_text_style is available from Version 4 onward. */
    if (vm->version >= 4U &&
        instruction.form == ZM_FORM_VARIABLE &&
        instruction.operand_count == ZM_OPERANDS_VAR &&
        instruction.opcode_number == 17U) {
        uint16_t requested;

        if (instruction.operand_count_actual != 1U)
            return mirc_vm_error(vm, "set_text_style requires exactly one operand");
        if (zmachine_resolve_operands(vm, &instruction, values,
                                      ZM_MAX_OPERANDS) != TCL_OK)
            return TCL_ERROR;
        requested = values[0];
        if ((requested & (uint16_t)~ZM_STYLE_MASK) != 0U)
            return mirc_vm_error(vm, "invalid Z-machine text style combination");

        state = ensure_state(vm);
        if (!state)
            return mirc_vm_error(vm, "unable to allocate mIRC presentation state");
        window = current_window_state(vm, state);
        if (requested == 0U)
            window->style = 0U; /* Roman disables every active style. */
        else
            window->style = (uint8_t)(window->style | (uint8_t)requested);
        vm->pc = instruction.next_pc;
        return TCL_OK;
    }

    /* 2OP:27 set_colour is available from Version 5 onward. */
    if (vm->version >= 5U &&
        instruction.operand_count == ZM_OPERANDS_2OP &&
        instruction.opcode_number == 27U) {
        if (instruction.operand_count_actual != 2U)
            return mirc_vm_error(vm, "set_colour requires exactly two operands");
        if (zmachine_resolve_operands(vm, &instruction, values,
                                      ZM_MAX_OPERANDS) != TCL_OK)
            return TCL_ERROR;

        state = ensure_state(vm);
        if (!state)
            return mirc_vm_error(vm, "unable to allocate mIRC presentation state");
        window = current_window_state(vm, state);
        if (set_standard_component(vm, values[0],
                                   &window->foreground_mode,
                                   &window->foreground_value,
                                   "foreground") != TCL_OK ||
            set_standard_component(vm, values[1],
                                   &window->background_mode,
                                   &window->background_value,
                                   "background") != TCL_OK)
            return TCL_ERROR;
        vm->pc = instruction.next_pc;
        return TCL_OK;
    }

    /* EXT:13 set_true_colour is available in the supported V5/V7/V8 family. */
    if (vm->version >= 5U &&
        instruction.form == ZM_FORM_EXTENDED &&
        instruction.opcode_number == 13U) {
        if (instruction.operand_count_actual != 2U)
            return mirc_vm_error(vm, "set_true_colour requires exactly two operands");
        if (zmachine_resolve_operands(vm, &instruction, values,
                                      ZM_MAX_OPERANDS) != TCL_OK)
            return TCL_ERROR;

        state = ensure_state(vm);
        if (!state)
            return mirc_vm_error(vm, "unable to allocate mIRC presentation state");
        window = current_window_state(vm, state);
        if (set_true_component(vm, values[0],
                               &window->foreground_mode,
                               &window->foreground_value,
                               "foreground") != TCL_OK ||
            set_true_component(vm, values[1],
                               &window->background_mode,
                               &window->background_value,
                               "background") != TCL_OK)
            return TCL_ERROR;
        vm->pc = instruction.next_pc;
        return TCL_OK;
    }

    return zmachine_step_visual_base(vm);
}

/*
 * This symbol is the exact output entry point expected by zmachine_stream.c.
 * Call the canonical implementation first, then mirror only the suffix which
 * actually appeared in vm->output. This automatically excludes stream 3,
 * disabled stream 1, and upper-window/status text without duplicating routing
 * policy in the IRC renderer.
 */
void zmachine_output_append_stream_base(ZMachine *vm,
                                        const char *text,
                                        size_t len)
{
    int before;
    int after;
    ZMachineMircState *state;

    before = zmachine_output_length(vm);
    zmachine_output_append_plain_base(vm, text, len);
    after = zmachine_output_length(vm);

    if (!vm || !vm->mirc_output_enabled || after <= before)
        return;

    state = ensure_state(vm);
    if (!state) {
        mirc_vm_error(vm, "unable to allocate mIRC presentation state");
        return;
    }

    append_rendered_text(vm, state,
                         zmachine_output_data(vm) + before,
                         (size_t)(after - before));
}

/* Public run boundary: formatted output is per Tcl command just like plain output. */
int zmachine_run(ZMachine *vm)
{
    ZMachineMircState *state;

    if (vm && vm->mirc_output_enabled) {
        state = ensure_state(vm);
        if (!state)
            return mirc_vm_error(vm, "unable to allocate mIRC presentation state");
        clear_render_tracking(state);
    } else if (vm && vm->mirc_state) {
        clear_render_tracking(vm->mirc_state);
    }

    return zmachine_run_mirc_base(vm);
}

/* Preserve the host format choice while resetting story-controlled styling. */
int zmachine_reset(ZMachine *vm)
{
    int rc = zmachine_reset_mirc_base(vm);
    if (rc == TCL_OK && vm && vm->mirc_state)
        reset_presentation_state(vm->mirc_state);
    return rc;
}

int zmachine_load_story(ZMachine *vm, const char *path)
{
    int rc = zmachine_load_story_mirc_base(vm, path);
    if (rc == TCL_OK && vm && vm->mirc_state)
        reset_presentation_state(vm->mirc_state);
    return rc;
}

void zmachine_stream_after_restart(ZMachine *vm)
{
    zmachine_stream_after_restart_mirc_base(vm);
    if (vm && vm->mirc_state)
        reset_presentation_state(vm->mirc_state);
}

void zmachine_destroy(ZMachine *vm)
{
    free_state(vm);
    zmachine_destroy_mirc_base(vm);
}

int zmachine_mirc_set_enabled(ZMachine *vm, int enabled)
{
    ZMachineMircState *state;

    if (!vm)
        return TCL_ERROR;
    enabled = !!enabled;
    if (enabled) {
        state = ensure_state(vm);
        if (!state)
            return TCL_ERROR;
        clear_render_tracking(state);
    } else if (vm->mirc_state) {
        clear_render_tracking(vm->mirc_state);
    }

    vm->mirc_output_enabled = enabled;
    if (vm->memory)
        zmachine_refresh_interpreter_header(vm);
    return TCL_OK;
}

int zmachine_mirc_enabled(const ZMachine *vm)
{
    return vm ? !!vm->mirc_output_enabled : 0;
}

const char *zmachine_mirc_output_data(const ZMachine *vm)
{
    if (!vm || !vm->mirc_state)
        return "";
    return Tcl_DStringValue((Tcl_DString *)&vm->mirc_state->output);
}

int zmachine_mirc_output_length(const ZMachine *vm)
{
    if (!vm || !vm->mirc_state)
        return 0;
    return Tcl_DStringLength((Tcl_DString *)&vm->mirc_state->output);
}

/* Parsed-state helpers used only by the safe mIRC word wrapper. */
static void parsed_reset(MircParsedState *state)
{
    if (state)
        memset(state, 0, sizeof(*state));
}

static void parsed_append_prefix(Tcl_DString *output,
                                 const MircParsedState *state)
{
    char code[16];
    int n;

    if (!output || !state)
        return;
    if (state->have_foreground || state->have_background) {
        unsigned fg = state->have_foreground ? state->foreground : 0U;
        if (state->have_background)
            n = snprintf(code, sizeof(code), "%c%02u,%02u",
                         MIRC_COLOUR, fg, state->background);
        else
            n = snprintf(code, sizeof(code), "%c%02u", MIRC_COLOUR, fg);
        if (n > 0 && (size_t)n < sizeof(code))
            Tcl_DStringAppend(output, code, n);
    }
    if (state->bold)
        Tcl_DStringAppend(output, &((char){MIRC_BOLD}), 1);
    if (state->italic)
        Tcl_DStringAppend(output, &((char){MIRC_ITALIC}), 1);
    if (state->reverse)
        Tcl_DStringAppend(output, &((char){MIRC_REVERSE}), 1);
}

static size_t colour_token_length(const char *text, size_t length, size_t pos,
                                  MircParsedState *next)
{
    size_t cursor = pos + 1U;
    unsigned value = 0U;
    unsigned digits = 0U;

    next->have_foreground = 0;
    next->have_background = 0;

    while (cursor < length && digits < 2U &&
           isdigit((unsigned char)text[cursor])) {
        value = value * 10U + (unsigned)(text[cursor] - '0');
        ++cursor;
        ++digits;
    }
    if (digits == 0U)
        return 1U; /* bare Ctrl-C resets colours */

    next->have_foreground = 1;
    next->foreground = value;

    if (cursor < length && text[cursor] == ',') {
        size_t comma = cursor++;
        unsigned background = 0U;
        unsigned background_digits = 0U;

        while (cursor < length && background_digits < 2U &&
               isdigit((unsigned char)text[cursor])) {
            background = background * 10U + (unsigned)(text[cursor] - '0');
            ++cursor;
            ++background_digits;
        }
        if (background_digits > 0U) {
            next->have_background = 1;
            next->background = background;
        } else {
            cursor = comma; /* comma was ordinary following text */
        }
    }
    return cursor - pos;
}

static size_t parse_token(const char *text, size_t length, size_t pos,
                          const MircParsedState *current,
                          MircParsedState *next,
                          int *is_control,
                          int *is_space,
                          int *is_newline)
{
    unsigned char ch;
    size_t token = 1U;

    *next = *current;
    *is_control = 0;
    *is_space = 0;
    *is_newline = 0;

    ch = (unsigned char)text[pos];
    if (ch == (unsigned char)'\n') {
        *is_newline = 1;
        return 1U;
    }

    if (ch == (unsigned char)MIRC_RESET) {
        *is_control = 1;
        parsed_reset(next);
        return 1U;
    }
    if (ch == (unsigned char)MIRC_BOLD) {
        *is_control = 1;
        next->bold = !next->bold;
        return 1U;
    }
    if (ch == (unsigned char)MIRC_ITALIC) {
        *is_control = 1;
        next->italic = !next->italic;
        return 1U;
    }
    if (ch == (unsigned char)MIRC_REVERSE) {
        *is_control = 1;
        next->reverse = !next->reverse;
        return 1U;
    }
    if (ch == (unsigned char)MIRC_COLOUR) {
        *is_control = 1;
        return colour_token_length(text, length, pos, next);
    }

    if (ch == (unsigned char)' ' || ch == (unsigned char)'\t')
        *is_space = 1;

    if (ch >= 0xc0U) {
        if ((ch & 0xe0U) == 0xc0U)
            token = 2U;
        else if ((ch & 0xf0U) == 0xe0U)
            token = 3U;
        else if ((ch & 0xf8U) == 0xf0U)
            token = 4U;
        while (token > 1U && pos + token > length)
            --token;
        while (token > 1U) {
            size_t i;
            int valid = 1;
            for (i = 1U; i < token; ++i) {
                if (((unsigned char)text[pos + i] & 0xc0U) != 0x80U) {
                    valid = 0;
                    break;
                }
            }
            if (valid)
                break;
            --token;
        }
    }
    return token;
}

static void append_line_and_newline(Tcl_DString *result, Tcl_DString *line)
{
    Tcl_DStringAppend(result, Tcl_DStringValue(line), Tcl_DStringLength(line));
    Tcl_DStringAppend(result, "\n", 1);
    Tcl_DStringSetLength(line, 0);
}

int zmachine_mirc_wrap_output(const char *text,
                              size_t length,
                              size_t max_bytes,
                              Tcl_DString *result)
{
    Tcl_DString line;
    MircParsedState active;
    MircParsedState next;
    MircParsedState last_space_state;
    size_t pos = 0U;
    size_t last_space_line_length = 0U;
    size_t last_space_resume_pos = 0U;
    int have_last_space = 0;
    int line_has_visible = 0;
    int skip_leading_spaces = 0;

    if (!text || !result)
        return TCL_ERROR;

    Tcl_DStringSetLength(result, 0);
    if (max_bytes == 0U) {
        Tcl_DStringAppend(result, text, (int)length);
        return TCL_OK;
    }

    Tcl_DStringInit(&line);
    parsed_reset(&active);
    parsed_reset(&last_space_state);

    while (pos < length) {
        size_t token_length;
        int is_control;
        int is_space;
        int is_newline;

        token_length = parse_token(text, length, pos, &active, &next,
                                   &is_control, &is_space, &is_newline);

        if (is_newline) {
            append_line_and_newline(result, &line);
            ++pos;
            parsed_reset(&active);
            have_last_space = 0;
            line_has_visible = 0;
            skip_leading_spaces = 0;
            continue;
        }

        if (skip_leading_spaces && is_space) {
            pos += token_length;
            active = next;
            continue;
        }

        if ((size_t)Tcl_DStringLength(&line) + token_length > max_bytes &&
            line_has_visible) {
            /*
             * Ctrl-O only exists to reset formatting within an IRC message.
             * If the visible physical line already consumes the exact byte
             * budget, moving that reset to a new line would create either a
             * control-only line or a redundant formatting-prefix/reset pair
             * before later text. The IRC message boundary itself resets all
             * formatting, so consume the overflowing Ctrl-O here. If more text
             * follows, the next visible token will naturally force the full
             * line to be emitted before it is retried under the reset state.
             */
            if (is_control && token_length == 1U &&
                (unsigned char)text[pos] == (unsigned char)MIRC_RESET) {
                active = next;
                pos += token_length;
                continue;
            }

            if (have_last_space) {
                Tcl_DStringSetLength(&line, (int)last_space_line_length);
                append_line_and_newline(result, &line);
                pos = last_space_resume_pos;
                active = last_space_state;
                parsed_append_prefix(&line, &active);
                have_last_space = 0;
                line_has_visible = 0;
                skip_leading_spaces = 1;
                continue;
            }

            append_line_and_newline(result, &line);
            parsed_append_prefix(&line, &active);
            have_last_space = 0;
            line_has_visible = 0;
            skip_leading_spaces = 0;
            continue; /* retry the same token on the new physical line */
        }

        if (is_space) {
            size_t trim = (size_t)Tcl_DStringLength(&line);
            const char *line_data = Tcl_DStringValue(&line);

            while (trim > 0U &&
                   (line_data[trim - 1U] == ' ' || line_data[trim - 1U] == '\t'))
                --trim;
            last_space_line_length = trim;
            last_space_resume_pos = pos + token_length;
            last_space_state = next;
            have_last_space = 1;
        }

        Tcl_DStringAppend(&line, text + pos, (int)token_length);
        active = next;
        if (!is_control && !is_space)
            line_has_visible = 1;
        if (!is_space)
            skip_leading_spaces = 0;
        pos += token_length;
    }

    Tcl_DStringAppend(result, Tcl_DStringValue(&line), Tcl_DStringLength(&line));
    Tcl_DStringFree(&line);
    return TCL_OK;
}
