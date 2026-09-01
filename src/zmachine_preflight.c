/*
 * zmachine_preflight.c
 *
 * Global instruction-legality preflight for the layered VM dispatcher.
 *
 * Several higher layers own opcodes before execution reaches the original core:
 * the cooperative run loop, external stream selection, lexical operations,
 * numeric input, presentation, and optional mIRC styling. A malformed instruction
 * can contain VARIABLE operands even when its opcode, arity, or literal selector
 * is already known to be invalid. Resolving variable 0 pops the evaluation stack,
 * so every rejection which can be decided from the encoded instruction happens
 * here before any execution layer is allowed to resolve operands.
 *
 * Versions 7 and 8 are explicitly defined by the Standard as Version-5 machines
 * except for their documented memory/address changes. They therefore use the V5
 * screen model and V5 opcode signatures rather than the special V6 window,
 * graphics, and formatted-output forms. Version 6 story files are intentionally
 * unsupported by this text-only runtime.
 *
 * Both public zmachine_step() and the cooperative run loop call
 * zmachine_preflight_instruction(). EXT:29..255 retain the Standard's special
 * ignore rule; ignored instructions advance over their encoded operands without
 * evaluating them.
 */

#include "tclzmachine.h"
#include "zmachine_decode.h"
#include "zmachine_preflight.h"

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

/* Return a decoded literal without evaluating VARIABLE operands. */
static int constant_operand(const ZMachineInstruction *instruction,
                            uint8_t index,
                            uint16_t *value)
{
    const ZMachineDecodedOperand *operand;

    if (!instruction || index >= instruction->operand_count_actual)
        return 0;
    operand = &instruction->operands[index];
    if (operand->type != ZM_OPERAND_SMALL_CONSTANT &&
        operand->type != ZM_OPERAND_LARGE_CONSTANT)
        return 0;
    if (value)
        *value = operand->value;
    return 1;
}

/*
 * Establish opcode-version legality before considering operand shape.
 *
 * V6 has a distinct screen model. Although many opcode-table entries begin at
 * V6, the Standard separately states that V7/V8 are identical to V5 except for
 * memory/address changes. Thus V6-only EXT instructions remain illegal in V7/V8.
 *
 * show_status is formally a V3-only opcode, but the Standard specifically asks
 * later interpreters to accept it as a no-op because one V5 Wishbringer release
 * contains it accidentally. That compatibility exception is authoritative here:
 * V1/V2 reject it, V3 executes the status operation, and V4+ may consume it.
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

        if (vm->version != 6U &&
            ((opcode >= 5U && opcode <= 8U) ||
             (opcode >= 16U && opcode <= 28U)))
            return preflight_error(vm,
                                   "extended opcode is available only in Version 6");

        return TCL_OK;
    }

    if (instruction->operand_count == ZM_OPERANDS_0OP) {
        switch (opcode) {
        case 5U:
        case 6U:
            if (vm->version >= 5U)
                return preflight_error(vm,
                    "0OP save/restore is illegal in Version 5 and later");
            break;

        case 12U:
            if (vm->version < 3U)
                return preflight_error(vm,
                                       "show_status is unavailable before Version 3");
            break;

        case 13U:
            if (vm->version < 3U)
                return preflight_error(vm,
                                       "verify is unavailable before Version 3");
            break;

        case 14U:
            return preflight_error(vm,
                                   "extended opcode prefix is illegal before Version 5");

        case 15U:
            if (vm->version < 5U)
                return preflight_error(vm,
                                       "piracy is unavailable before Version 5");
            break;

        default:
            break;
        }
        return TCL_OK;
    }

    if (instruction->operand_count == ZM_OPERANDS_2OP) {
        if (opcode == 0U || opcode >= 29U)
            return preflight_error(vm, "undefined Z-machine 2OP opcode");
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
    case 10U:
    case 11U:
    case 19U:
    case 20U:
    case 21U:
        if (vm->version < 3U)
            return preflight_error(vm, "VAR opcode requires V3 or later");
        break;

    case 12U:
    case 13U:
    case 14U:
    case 15U:
    case 16U:
    case 17U:
    case 18U:
    case 22U:
    case 23U:
        if (vm->version < 4U)
            return preflight_error(vm, "VAR opcode requires V4 or later");
        break;

    case 24U:
    case 25U:
    case 26U:
    case 27U:
    case 28U:
    case 29U:
    case 30U:
    case 31U:
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
        case 0U:
        case 1U:
            if (instruction->operand_count_actual != 0U &&
                instruction->operand_count_actual != 3U &&
                instruction->operand_count_actual != 4U)
                return preflight_error(vm,
                    "extended save/restore requires zero, three, or four operands");
            break;
        case 2U:
        case 3U:
            return require_exact(vm, instruction, 2U,
                                 "shift opcode requires exactly two operands");
        case 4U:
            return require_exact(vm, instruction, 1U,
                                 "set_font requires exactly one operand");
        case 9U:
        case 10U:
            return require_exact(vm, instruction, 0U,
                                 "undo opcode does not accept operands");
        case 11U:
            return require_exact(vm, instruction, 1U,
                                 "print_unicode requires exactly one operand");
        case 12U:
            return require_exact(vm, instruction, 1U,
                                 "check_unicode requires exactly one operand");
        case 13U:
            return require_exact(vm, instruction, 2U,
                                 "set_true_colour requires exactly two operands");
        case 29U:
            if (vm->version == 6U)
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
    case 0U:
        return require_range(vm, instruction, 1U, 4U,
                             "call_vs requires one to four operands");
    case 1U:
    case 2U:
    case 3U:
        return require_exact(vm, instruction, 3U,
                             "three-operand VAR instruction has invalid arity");
    case 4U:
        if (vm->version <= 3U)
            return require_exact(vm, instruction, 2U,
                                 "V1-V3 read requires exactly two operands");
        if (vm->version == 4U)
            return require_range(vm, instruction, 2U, 4U,
                                 "V4 read requires two to four operands");
        return require_range(vm, instruction, 1U, 4U,
                             "V5-compatible read requires one to four operands");
    case 5U:
    case 6U:
    case 8U:
    case 9U:
        return require_exact(vm, instruction, 1U,
                             "single-operand VAR instruction has invalid arity");
    case 7U:
        return require_exact(vm, instruction, 1U,
                             "random requires exactly one operand");
    case 10U:
        return require_exact(vm, instruction, 1U,
                             "split_window requires exactly one operand");
    case 11U:
        return require_exact(vm, instruction, 1U,
                             "set_window requires exactly one operand");
    case 12U:
        return require_range(vm, instruction, 1U, 8U,
                             "call_vs2 requires one to eight operands");
    case 13U:
        return require_exact(vm, instruction, 1U,
                             "erase_window requires exactly one operand");
    case 14U:
        return require_exact(vm, instruction, 1U,
                             "erase_line requires exactly one operand");
    case 15U:
        return require_exact(vm, instruction, 2U,
                             "set_cursor requires exactly two operands");
    case 16U:
        return require_exact(vm, instruction, 1U,
                             "get_cursor requires exactly one operand");
    case 17U:
        return require_exact(vm, instruction, 1U,
                             "set_text_style requires exactly one operand");
    case 18U:
        return require_exact(vm, instruction, 1U,
                             "buffer_mode requires exactly one operand");
    case 19U:
        return require_range(vm, instruction, 1U, 2U,
                             "output_stream requires one or two operands");
    case 20U:
        return require_exact(vm, instruction, 1U,
                             "input_stream requires exactly one operand");
    case 21U:
        return require_range(vm, instruction, 0U, 4U,
                             "sound_effect accepts at most four operands");
    case 22U:
        /*
         * Standard syntax is read_char 1 [time routine]. Galatea release 2/3
         * contains a historically assembled zero-operand read_char and relies
         * on interpreters treating the omitted device as the sole keyboard
         * device (1). Preserve that narrow compatibility form while continuing
         * to reject four operands and all invalid explicit device/timer values.
         */
        return require_range(vm, instruction, 0U, 3U,
                             "read_char accepts zero to three operands");
    case 23U:
        return require_range(vm, instruction, 3U, 4U,
                             "scan_table requires three or four operands");
    case 24U:
        return require_exact(vm, instruction, 1U,
                             "not requires exactly one operand");
    case 25U:
        return require_range(vm, instruction, 1U, 4U,
                             "call_vn requires one to four operands");
    case 26U:
        return require_range(vm, instruction, 1U, 8U,
                             "call_vn2 requires one to eight operands");
    case 27U:
        return require_range(vm, instruction, 2U, 4U,
                             "tokenise requires two to four operands");
    case 28U:
        return require_exact(vm, instruction, 4U,
                             "encode_text requires exactly four operands");
    case 29U:
        return require_exact(vm, instruction, 3U,
                             "copy_table requires exactly three operands");
    case 30U:
        return require_range(vm, instruction, 2U, 4U,
                             "print_table requires two to four operands");
    case 31U:
        return require_exact(vm, instruction, 1U,
                             "check_arg_count requires exactly one operand");
    default:
        return TCL_OK;
    }
}

static int validate_literal_values(ZMachine *vm,
                                   const ZMachineInstruction *instruction)
{
    uint16_t value;
    int16_t signed_value;

    if (!vm || !instruction)
        return preflight_error(vm, "invalid literal-value preflight request");

    if (instruction->operand_count == ZM_OPERANDS_2OP &&
        instruction->opcode_number == 27U) {
        if (constant_operand(instruction, 0U, &value) && value > 9U)
            return preflight_error(vm, "unsupported set_colour foreground value");
        if (constant_operand(instruction, 1U, &value) && value > 9U)
            return preflight_error(vm, "unsupported set_colour background value");
        return TCL_OK;
    }

    if (instruction->form == ZM_FORM_EXTENDED) {
        switch (instruction->opcode_number) {
        case 0U:
        case 1U:
            if (instruction->operand_count_actual == 4U &&
                constant_operand(instruction, 3U, &value) && value > 1U)
                return preflight_error(vm,
                                       "auxiliary save/restore prompt must be 0 or 1");
            break;
        case 2U:
        case 3U:
            if (constant_operand(instruction, 1U, &value)) {
                signed_value = (int16_t)value;
                if (signed_value < -15 || signed_value > 15)
                    return preflight_error(vm,
                                           "Z-machine shift count is outside -15..15");
            }
            break;
        case 13U:
            if (constant_operand(instruction, 0U, &value)) {
                signed_value = (int16_t)value;
                if (signed_value < -2)
                    return preflight_error(vm,
                                           "unsupported set_true_colour foreground value");
            }
            if (constant_operand(instruction, 1U, &value)) {
                signed_value = (int16_t)value;
                if (signed_value < -2)
                    return preflight_error(vm,
                                           "unsupported set_true_colour background value");
            }
            break;
        default:
            break;
        }
        return TCL_OK;
    }

    if (instruction->form != ZM_FORM_VARIABLE ||
        instruction->operand_count != ZM_OPERANDS_VAR)
        return TCL_OK;

    switch (instruction->opcode_number) {
    case 4U:
        if (instruction->operand_count_actual >= 3U &&
            constant_operand(instruction, 2U, &value) && value != 0U)
            return preflight_error(vm, "timed line input is unsupported");
        if (instruction->operand_count_actual >= 4U &&
            constant_operand(instruction, 3U, &value) && value != 0U)
            return preflight_error(vm, "timed line input routine is unsupported");
        break;
    case 11U:
        if (constant_operand(instruction, 0U, &value) && value > 1U)
            return preflight_error(vm, "unsupported Z-machine window number");
        break;
    case 19U:
        if (constant_operand(instruction, 0U, &value)) {
            signed_value = (int16_t)value;
            if (signed_value < -4 || signed_value > 4)
                return preflight_error(vm,
                                       "unsupported Z-machine output stream number");
            if (signed_value == 3 && instruction->operand_count_actual < 2U)
                return preflight_error(vm,
                                       "output_stream 3 requires a table operand");
        }
        break;
    case 20U:
        if (constant_operand(instruction, 0U, &value) && value > 1U)
            return preflight_error(vm, "invalid Z-machine input stream");
        break;
    case 21U:
        if (instruction->operand_count_actual > 1U &&
            constant_operand(instruction, 0U, &value) && value < 3U)
            return preflight_error(vm,
                "sound_effect numbers below 3 cannot have additional operands");
        if (instruction->operand_count_actual > 1U &&
            constant_operand(instruction, 1U, &value) &&
            (value < 1U || value > 4U))
            return preflight_error(vm, "invalid sound_effect effect value");
        if (vm->version == 3U && instruction->operand_count_actual > 2U &&
            constant_operand(instruction, 2U, &value) && (value & 0xff00U) != 0U)
            return preflight_error(vm,
                                   "Version 3 sound_effect does not support repeats");
        break;
    case 22U:
        if (constant_operand(instruction, 0U, &value) && value != 1U)
            return preflight_error(vm, "read_char input device must be 1");
        if (instruction->operand_count_actual > 1U &&
            constant_operand(instruction, 1U, &value) && value != 0U)
            return preflight_error(vm, "timed read_char is not yet supported");
        if (instruction->operand_count_actual > 2U &&
            constant_operand(instruction, 2U, &value) && value != 0U)
            return preflight_error(vm,
                                   "timed read_char callback is not yet supported");
        break;
    case 23U:
        if (instruction->operand_count_actual == 4U &&
            constant_operand(instruction, 3U, &value) && (value & 0x7fU) == 0U)
            return preflight_error(vm, "scan_table field size is zero");
        break;
    case 27U:
        if (constant_operand(instruction, 1U, &value) && value == 0U)
            return preflight_error(vm,
                                   "tokenise requires a nonzero parse buffer address");
        break;
    default:
        break;
    }

    return TCL_OK;
}

static int ignored_extended_opcode(const ZMachine *vm,
                                   const ZMachineInstruction *instruction)
{
    if (!vm || !instruction || instruction->form != ZM_FORM_EXTENDED ||
        instruction->opcode_number < 29U)
        return 0;
    if (instruction->opcode_number >= 30U)
        return 1;
    return vm->version != 6U;
}

int zmachine_preflight_instruction(ZMachine *vm,
                                   const ZMachineInstruction *instruction,
                                   int *ignored)
{
    if (ignored)
        *ignored = 0;
    if (!vm || !instruction)
        return preflight_error(vm, "invalid decoded opcode preflight request");
    if (validate_version(vm, instruction) != TCL_OK)
        return TCL_ERROR;
    if (validate_arity(vm, instruction) != TCL_OK)
        return TCL_ERROR;
    if (validate_literal_values(vm, instruction) != TCL_OK)
        return TCL_ERROR;
    if (ignored && ignored_extended_opcode(vm, instruction))
        *ignored = 1;
    return TCL_OK;
}

int zmachine_step(ZMachine *vm)
{
    ZMachineInstruction instruction;
    char decode_error[128];
    int ignored = 0;

    if (!vm || !vm->memory)
        return preflight_error(vm, "cannot execute without a loaded story");
    if (!zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                     vm->pc, &instruction,
                                     decode_error, sizeof(decode_error))) {
        return preflight_error(vm, decode_error[0] ? decode_error :
                               "unable to decode Z-machine instruction");
    }
    if (zmachine_preflight_instruction(vm, &instruction, &ignored) != TCL_OK)
        return TCL_ERROR;
    if (ignored) {
        vm->pc = instruction.next_pc;
        return TCL_OK;
    }
    return zmachine_step_preflight_base(vm);
}
