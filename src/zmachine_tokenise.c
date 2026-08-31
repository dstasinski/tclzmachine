/*
 * zmachine_tokenise.c
 *
 * Lexical-opcode layer for Version 5+ `tokenise` and `encode_text`.
 *
 * Line-oriented `read` is handled by the cooperative run loop because it may
 * suspend waiting for Tcl input. These lexical opcodes never block: they
 * analyze or encode text already present in story memory. Keeping this small
 * layer adjacent to the input subsystem lets all three operations share one
 * dictionary implementation without moving host input policy into the
 * ordinary executor.
 *
 * CMake places this layer between cooperative file dispatch and presentation
 * dispatch. It therefore decodes only far enough to recognize its two VAR-table
 * opcodes, resolves their operands exactly once, then either executes locally or
 * delegates the untouched instruction path downward.
 */

#include "tclzmachine.h"
#include "zmachine_decode.h"
#include "zmachine_exec.h"
#include "zmachine_input.h"

#include <stdio.h>

/* Presentation wrapper supplied by zmachine_dispatch.c after symbol rename. */
extern int zmachine_step_present(ZMachine *vm);

/* Put the VM in its terminal error state with a lexical-layer diagnostic. */
static int lexical_error(ZMachine *vm, const char *message)
{
    if (vm) {
        vm->state = ZM_STATE_ERROR;
        snprintf(vm->error, sizeof(vm->error), "%s", message);
    }
    return TCL_ERROR;
}

/*
 * Execute one instruction through the non-blocking lexical layer.
 *
 * Only V5+ VAR:27 (`tokenise`) and VAR:28 (`encode_text`) are consumed here.
 * Everything else delegates to the presentation/core chain without resolving
 * operands in this layer, avoiding observable double reads/pops of variable 0.
 *
 * tokenise operands are:
 *   text buffer, parse buffer, optional dictionary address, optional flag.
 * A zero/omitted dictionary selects the story's main dictionary. A nonzero flag
 * asks the tokenizer to leave parse entries for unrecognized words unchanged.
 *
 * encode_text operands are:
 *   source text address, source length, source offset, destination address.
 * The shared input/dictionary subsystem performs the version-correct dictionary
 * encoding and all story-memory bounds checks.
 */
int zmachine_step_tokenise(ZMachine *vm)
{
    ZMachineInstruction instruction;
    uint16_t values[ZM_MAX_OPERANDS];
    char decode_error[128];

    if (!vm || !vm->memory)
        return lexical_error(vm, "cannot execute without a loaded story");

    if (!zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                     vm->pc, &instruction,
                                     decode_error, sizeof(decode_error))) {
        return lexical_error(vm, decode_error[0] ? decode_error :
                             "unable to decode Z-machine instruction");
    }

    if (vm->version < 5U ||
        instruction.form != ZM_FORM_VARIABLE ||
        instruction.operand_count != ZM_OPERANDS_VAR ||
        (instruction.opcode_number != 27U &&
         instruction.opcode_number != 28U))
        return zmachine_step_present(vm);

    if (zmachine_resolve_operands(vm, &instruction, values,
                                  ZM_MAX_OPERANDS) != TCL_OK)
        return TCL_ERROR;

    if (instruction.opcode_number == 27U) {
        uint16_t dictionary = 0U;
        int preserve_unrecognized = 0;

        if (instruction.operand_count_actual < 2U)
            return lexical_error(vm,
                                 "tokenise requires text and parse buffer operands");
        if (instruction.operand_count_actual >= 3U)
            dictionary = values[2];
        if (instruction.operand_count_actual >= 4U)
            preserve_unrecognized = values[3] != 0U;

        if (zmachine_input_tokenize_buffer(vm, values[0], values[1],
                                           dictionary,
                                           preserve_unrecognized) != TCL_OK)
            return lexical_error(vm,
                                 "unable to tokenize Z-machine text buffer");
    } else {
        if (instruction.operand_count_actual < 4U)
            return lexical_error(vm,
                                 "encode_text requires four operands");
        if (zmachine_input_encode_text(vm,
                                       values[0], values[1],
                                       values[2], values[3]) != TCL_OK)
            return lexical_error(vm,
                                 "unable to encode Z-machine dictionary text");
    }

    vm->pc = instruction.next_pc;
    return TCL_OK;
}
