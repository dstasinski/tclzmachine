/*
 * zmachine_preflight.c
 *
 * Global instruction-legality preflight for the layered VM dispatcher.
 *
 * Several higher layers own opcodes before execution reaches the original core:
 * external stream selection, lexical operations, numeric input, presentation,
 * and optional mIRC styling.  Those layers historically performed their own
 * minimum-operand checks, but a malformed instruction could still contain extra
 * VARIABLE operands.  Resolving variable 0 pops the evaluation stack, so an
 * instruction which is going to be rejected for version or arity must be
 * rejected before any owning layer evaluates any operand.
 *
 * This wrapper is therefore the public zmachine_step() entry point.  It decodes
 * once, checks version legality first, then checks structural operand counts for
 * the supported non-V6 instruction model, and only then delegates the untouched
 * instruction to the former public stream wrapper.  Lower-layer checks remain as
 * defense in depth and for value-dependent legality such as output_stream 3
 * requiring a table operand.
 *
 * The checks intentionally preserve two project/Standard compatibility rules:
 * sound_effect is accepted from V3 because historical Infocom V3 code used it,
 * and out-of-range EXT:29+ opcodes retain the Standard's ignore behavior.  In
 * particular, V5 EXT:29 is ignored rather than arity-validated; EXT:29 becomes
 * buffer_screen only in V6+, which means supported V7/V8 execute its one-operand
 * form normally.
 */

#include "tclzmachine.h"
#include "zmachine_decode.h"

#include <stdio.h>

/* Former public stream wrapper, renamed for this layer by CMake. */
extern int zmachine_step_preflight_base(ZMachine *vm);

static int preflight_error(ZMachine *vm, const char *message)
{
    if (vm) {
        vm->state = ZM_STATE_ERROR;
        snprintf(vm->error, sizeof(vm->error), "%s", message);
    }
    return TCL_ERROR;
}

/*
 * Establish opcode-version legality before considering operand shape.
 *
 * This mirrors the lower core guard deliberately: higher ownership layers may
 * consume an instruction before it reaches that core guard, so the invariant
 * must also exist at the outermost execution boundary.
 */
static int validate_version(ZMachine *vm,
                            const ZMachineInstruction *instruction)
{
    uint8_t opcode;

    if (!vm || !instruction)
        return preflight_error(vm, "invalid opcode version preflight request");

    opcode = instruction->opcode_number;

    if (instruction->form == ZM_FORM_EXTENDED) {
        if (vm->version < 5U)
            return preflight_error(vm, "extended opcode is illegal before V5");

        if (opcode == 14U || opcode == 15U)
            return preflight_error(vm, "reserved extended Z-machine opcode");

        if (vm->version < 6U &&
            ((opcode >= 5U && opcode <= 8U) ||
             (opcode >= 16U && opcode <= 28U)))
            return preflight_error(vm, "extended opcode requires V6 or later");

        return TCL_OK;
    }

    if (instruction->operand_count == ZM_OPERANDS_2OP) {
        if (opcode == 25U && vm->version < 4U)
            return preflight_error(vm, "call_2s is illegal before V4");
        if (opcode >= 26U && opcode <= 28U && vm->version < 5U)
            return preflight_error(vm, "2OP opcode requires V5 or later");
        return TCL_OK;
    }

    if (instruction->operand_count == ZM_OPERANDS_1OP) {
        if (opcode == 8U && vm->version < 4U)
            return preflight_error(vm, "call_1s is illegal before V4");
        return TCL_OK;
    }

    if (instruction->form != ZM_FORM_VARIABLE ||
        instruction->operand_count != ZM_OPERANDS_VAR)
        return TCL_OK;

    switch (opcode) {
    case 10U: /* split_window */
    case 11U: /* set_window */
    case 19U: /* output_stream */
    case 20U: /* input_stream */
    case 21U: /* sound_effect: historical V3 compatibility */
        if (vm->version < 3U)
            return preflight_error(vm, "VAR opcode requires V3 or later");
        break;

    case 12U: /* call_vs2 */
    case 13U: /* erase_window */
    case 14U: /* erase_line */
    case 15U: /* set_cursor */
    case 16U: /* get_cursor */
    case 17U: /* set_text_style */
    case 18U: /* buffer_mode */
    case 22U: /* read_char */
    case 23U: /* scan_table */
        if (vm->version < 4U)
            return preflight_error(vm, "VAR opcode requires V4 or later");
        break;

    case 24U: /* not */
    case 25U: /* call_vn */
    case 26U: /* call_vn2 */
    case 27U: /* tokenise */
    case 28U: /* encode_text */
    case 29U: /* copy_table */
    case 30U: /* print_table */
    case 31U: /* check_arg_count */
        if (vm->version < 5U)
            return preflight_error(vm, "VAR opcode requires V5 or later");
        break;

    default:
        break;
    }

    return TCL_OK;
}

static int require_exact(ZMachine *vm,
                         const ZMachineInstruction *instruction,
                         uint8_t count,
                         const char *message)
{
    if (instruction->operand_count_actual != count)
        return preflight_error(vm, message);
    return TCL_OK;
}

static int require_range(ZMachine *vm,
                         const ZMachineInstruction *instruction,
                         uint8_t minimum,
                         uint8_t maximum,
                         const char *message)
{
    if (instruction->operand_count_actual < minimum ||
        instruction->operand_count_actual > maximum)
        return preflight_error(vm, message);
    return TCL_OK;
}

/*
 * Validate operand counts which can be decided from decoded structure alone.
 *
 * Value-dependent rules stay in their owning layer.  For example output_stream
 * accepts one or two operands structurally; after the stream number is evaluated,
 * selecting stream 3 still requires the second table operand.  This preflight's
 * job is to reject impossible counts before resolving *any* operand.
 */
static int validate_arity(ZMachine *vm,
                          const ZMachineInstruction *instruction)
{
    uint8_t opcode;

    if (!vm || !instruction)
        return preflight_error(vm, "invalid opcode arity preflight request");

    opcode = instruction->opcode_number;

    if (instruction->operand_count == ZM_OPERANDS_2OP &&
        opcode >= 1U && opcode <= 28U) {
        if (opcode == 1U)
            return require_range(vm, instruction, 2U, 4U,
                                 "je requires between two and four operands");
        return require_exact(vm, instruction, 2U,
                             "2OP instruction requires exactly two operands");
    }

    if (instruction->form == ZM_FORM_EXTENDED) {
        switch (opcode) {
        case 0U: /* save: zero-operand full save or 3/4-operand auxiliary form */
        case 1U: /* restore */
            if (instruction->operand_count_actual != 0U &&
                instruction->operand_count_actual != 3U &&
                instruction->operand_count_actual != 4U)
                return preflight_error(vm,
                    "extended save/restore requires zero, three, or four operands");
            break;

        case 2U: /* log_shift */
        case 3U: /* art_shift */
            return require_exact(vm, instruction, 2U,
                                 "shift opcode requires exactly two operands");

        case 4U: /* set_font */
            return require_exact(vm, instruction, 1U,
                                 "set_font requires exactly one operand");

        case 9U: /* save_undo */
        case 10U: /* restore_undo */
            return require_exact(vm, instruction, 0U,
                                 "undo opcode does not accept operands");

        case 11U: /* print_unicode */
            return require_exact(vm, instruction, 1U,
                                 "print_unicode requires exactly one operand");

        case 12U: /* check_unicode */
            return require_exact(vm, instruction, 1U,
                                 "check_unicode requires exactly one operand");

        case 13U: /* set_true_colour; optional window is V6-only */
            return require_exact(vm, instruction, 2U,
                                 "set_true_colour requires exactly two operands");

        case 29U: /* buffer_screen only once it is defined in V6+ */
            if (vm->version >= 6U)
                return require_exact(vm, instruction, 1U,
                                     "buffer_screen requires exactly one operand");
            break;

        default:
            break;
        }
        return TCL_OK;
    }

    if (instruction->form != ZM_FORM_VARIABLE ||
        instruction->operand_count != ZM_OPERANDS_VAR)
        return TCL_OK;

    switch (opcode) {
    case 0U: /* call/call_vs */
        return require_range(vm, instruction, 1U, 4U,
                             "call_vs requires one to four operands");

    case 1U: /* storew */
    case 2U: /* storeb */
    case 3U: /* put_prop */
        return require_exact(vm, instruction, 3U,
                             "three-operand VAR instruction has invalid arity");

    /* VAR:4 read has version-dependent optional operands; its owner validates it. */

    case 5U: /* print_char */
    case 6U: /* print_num */
    case 8U: /* push */
    case 9U: /* pull in supported non-V6 versions */
        return require_exact(vm, instruction, 1U,
                             "single-operand VAR instruction has invalid arity");

    case 7U: /* random */
        return require_exact(vm, instruction, 1U,
                             "random requires exactly one operand");

    case 10U: /* split_window */
        return require_exact(vm, instruction, 1U,
                             "split_window requires exactly one operand");

    case 11U: /* set_window */
        return require_exact(vm, instruction, 1U,
                             "set_window requires exactly one operand");

    case 12U: /* call_vs2 */
        return require_range(vm, instruction, 1U, 8U,
                             "call_vs2 requires one to eight operands");

    case 13U: /* erase_window */
        return require_exact(vm, instruction, 1U,
                             "erase_window requires exactly one operand");

    case 14U: /* erase_line */
        return require_exact(vm, instruction, 1U,
                             "erase_line requires exactly one operand");

    case 15U: /* set_cursor */
        return require_exact(vm, instruction, 2U,
                             "set_cursor requires exactly two operands");

    case 16U: /* get_cursor */
        return require_exact(vm, instruction, 1U,
                             "get_cursor requires exactly one operand");

    case 17U: /* set_text_style */
        return require_exact(vm, instruction, 1U,
                             "set_text_style requires exactly one operand");

    case 18U: /* buffer_mode */
        return require_exact(vm, instruction, 1U,
                             "buffer_mode requires exactly one operand");

    case 19U: /* output_stream number [table]; width is V6-only */
        return require_range(vm, instruction, 1U, 2U,
                             "output_stream requires one or two operands");

    case 20U: /* input_stream */
        return require_exact(vm, instruction, 1U,
                             "input_stream requires exactly one operand");

    case 21U: /* sound_effect: historical zero-operand form is tolerated */
        return require_range(vm, instruction, 0U, 4U,
                             "sound_effect accepts at most four operands");

    case 22U: /* read_char 1 [time routine] */
        return require_range(vm, instruction, 1U, 3U,
                             "read_char requires one to three operands");

    case 23U: /* scan_table x table len [form] */
        return require_range(vm, instruction, 3U, 4U,
                             "scan_table requires three or four operands");

    case 24U: /* not */
        return require_exact(vm, instruction, 1U,
                             "not requires exactly one operand");

    case 25U: /* call_vn */
        return require_range(vm, instruction, 1U, 4U,
                             "call_vn requires one to four operands");

    case 26U: /* call_vn2 */
        return require_range(vm, instruction, 1U, 8U,
                             "call_vn2 requires one to eight operands");

    case 27U: /* tokenise text parse [dictionary flag] */
        return require_range(vm, instruction, 2U, 4U,
                             "tokenise requires two to four operands");

    case 28U: /* encode_text zscii length from coded */
        return require_exact(vm, instruction, 4U,
                             "encode_text requires exactly four operands");

    case 29U: /* copy_table */
        return require_exact(vm, instruction, 3U,
                             "copy_table requires exactly three operands");

    case 30U: /* print_table zscii-text width [height skip] */
        return require_range(vm, instruction, 2U, 4U,
                             "print_table requires two to four operands");

    case 31U: /* check_arg_count */
        return require_exact(vm, instruction, 1U,
                             "check_arg_count requires exactly one operand");

    default:
        return TCL_OK;
    }
}

int zmachine_step(ZMachine *vm)
{
    ZMachineInstruction instruction;
    char decode_error[128];

    if (!vm || !vm->memory)
        return preflight_error(vm, "cannot execute without a loaded story");

    if (!zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                     vm->pc, &instruction,
                                     decode_error, sizeof(decode_error))) {
        return preflight_error(vm, decode_error[0] ? decode_error :
                               "unable to decode Z-machine instruction");
    }

    if (validate_version(vm, &instruction) != TCL_OK)
        return TCL_ERROR;
    if (validate_arity(vm, &instruction) != TCL_OK)
        return TCL_ERROR;

    return zmachine_step_preflight_base(vm);
}
