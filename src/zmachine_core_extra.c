/*
 * zmachine_core_extra.c
 *
 * Standards-completeness and validation layer for ordinary control-flow,
 * arithmetic, and host-neutral I/O-selection instructions which are not yet
 * implemented in the original core executor.
 *
 * This module is deliberately below the text/presentation wrapper. It recognizes
 * only the small set it owns, validates the operand shapes expected by the older
 * executor, resolves owned operands exactly once, and delegates every other valid
 * instruction untouched to zmachine_exec.c. Keeping operand resolution local to
 * a handled instruction is essential because reading variable 0 pops the
 * evaluation stack.
 *
 * catch/throw use the standard frame cookie: the number of routine frames
 * currently on the system stack. throw discards newer frames, then returns from
 * the routine whose frame count was caught. Logical and arithmetic shifts avoid
 * implementation-defined signed right-shift behavior by doing all bit movement
 * in the unsigned 16-bit value domain and explicitly filling sign bits.
 *
 * input_stream is host-neutral in an embedded interpreter: Tcl owns the physical
 * source of queued input, so the two standard stream numbers are accepted without
 * introducing terminal/file policy into the VM. sound_effect is likewise
 * consumed as an unavailable presentation effect after normal operand evaluation;
 * the interpreter header already tells stories that sampled sound is unavailable.
 */

#include "tclzmachine.h"
#include "zmachine_decode.h"
#include "zmachine_exec.h"
#include "zmachine_state.h"

#include <stdio.h>

/* Original ordinary executor, renamed at compile time by CMake. */
extern int zmachine_step_core_base(ZMachine *vm);

/* Put the VM into its terminal error state with a core-layer diagnostic. */
static int core_extra_error(ZMachine *vm, const char *message)
{
    if (vm) {
        vm->state = ZM_STATE_ERROR;
        snprintf(vm->error, sizeof(vm->error), "%s", message);
    }
    return TCL_ERROR;
}

/* Store one opcode result and continue after its store-variable byte. */
static int store_value(ZMachine *vm, uint32_t store_pc, uint16_t value)
{
    uint8_t variable;

    if (!vm || !vm->memory || (size_t)store_pc >= vm->memory_size)
        return core_extra_error(vm, "truncated Z-machine store variable");

    variable = vm->memory[store_pc];
    if (zmachine_variable_write(vm, variable, 0, value) != TCL_OK)
        return TCL_ERROR;

    vm->pc = store_pc + 1U;
    return TCL_OK;
}

/*
 * Validate operand counts for opcodes delegated to the original executor.
 *
 * Long and short encodings structurally guarantee their normal fixed operand
 * counts, but variable-form 2OP instructions carry an explicit operand-type byte.
 * Malformed story data can therefore omit operands while still decoding as a 2OP
 * opcode. The original executor predates this hardening layer and directly reads
 * values[1], or values[2] for several VAR opcodes. Reject impossible arities here
 * so corrupt Z-code produces a deterministic VM error instead of reading an
 * uninitialized temporary operand.
 *
 * `je` is the one base 2OP exception: it accepts two through four operands and
 * compares the first against each later value. Call-family VAR opcodes accept
 * their documented argument ranges; fixed-shape memory/output operations require
 * exactly the listed operands.
 */
static int validate_delegated_arity(ZMachine *vm,
                                    const ZMachineInstruction *instruction)
{
    uint8_t count;

    if (!vm || !instruction)
        return core_extra_error(vm, "invalid opcode arity validation request");

    count = instruction->operand_count_actual;

    if (instruction->operand_count == ZM_OPERANDS_2OP &&
        instruction->opcode_number >= 1U &&
        instruction->opcode_number <= 26U) {
        if (instruction->opcode_number == 1U) {
            if (count < 2U || count > 4U)
                return core_extra_error(vm,
                                        "je requires between two and four operands");
        } else if (count != 2U) {
            return core_extra_error(vm,
                                    "2OP instruction requires exactly two operands");
        }
        return TCL_OK;
    }

    if (instruction->form != ZM_FORM_VARIABLE ||
        instruction->operand_count != ZM_OPERANDS_VAR)
        return TCL_OK;

    switch (instruction->opcode_number) {
    case 0U: /* call_vs: routine plus up to three arguments */
        if (count < 1U || count > 4U)
            return core_extra_error(vm, "call_vs requires one to four operands");
        break;

    case 1U: /* storew */
    case 2U: /* storeb */
    case 3U: /* put_prop */
        if (count != 3U)
            return core_extra_error(vm,
                                    "three-operand VAR instruction has invalid arity");
        break;

    case 5U: /* print_char */
    case 6U: /* print_num */
    case 8U: /* push */
    case 9U: /* pull in supported non-V6 versions */
        if (count != 1U)
            return core_extra_error(vm,
                                    "single-operand VAR instruction has invalid arity");
        break;

    case 12U: /* call_vs2: routine plus up to seven arguments */
        if (vm->version >= 4U && (count < 1U || count > 8U))
            return core_extra_error(vm, "call_vs2 requires one to eight operands");
        break;

    case 25U: /* call_vn: routine plus up to three arguments */
        if (vm->version >= 5U && (count < 1U || count > 4U))
            return core_extra_error(vm, "call_vn requires one to four operands");
        break;

    case 26U: /* call_vn2: routine plus up to seven arguments */
        if (vm->version >= 5U && (count < 1U || count > 8U))
            return core_extra_error(vm, "call_vn2 requires one to eight operands");
        break;

    case 29U: /* copy_table */
        if (vm->version >= 5U && count != 3U)
            return core_extra_error(vm, "copy_table requires exactly three operands");
        break;

    default:
        break;
    }

    return TCL_OK;
}

/*
 * Return nonzero only for opcode families owned by this layer.
 *
 * VAR-table operations require ZM_FORM_VARIABLE explicitly: extended
 * instructions share the decoder's VAR-sized operand bucket, so an opcode
 * number alone is never sufficient to identify a VAR instruction.
 */
static int owns_instruction(const ZMachine *vm,
                            const ZMachineInstruction *instruction)
{
    if (!vm || !instruction)
        return 0;

    if (vm->version >= 3U &&
        instruction->form == ZM_FORM_VARIABLE &&
        instruction->operand_count == ZM_OPERANDS_VAR &&
        (instruction->opcode_number == 20U ||
         instruction->opcode_number == 21U))
        return 1; /* input_stream / sound_effect */

    if (vm->version < 5U)
        return 0;

    if (instruction->operand_count == ZM_OPERANDS_0OP &&
        instruction->opcode_number == 9U)
        return 1; /* catch */

    if (instruction->operand_count == ZM_OPERANDS_2OP &&
        instruction->opcode_number == 28U)
        return 1; /* throw */

    if (instruction->form == ZM_FORM_VARIABLE &&
        instruction->operand_count == ZM_OPERANDS_VAR &&
        instruction->opcode_number == 24U)
        return 1; /* V5+ not */

    if (instruction->form == ZM_FORM_EXTENDED &&
        (instruction->opcode_number == 2U ||
         instruction->opcode_number == 3U))
        return 1; /* log_shift / art_shift */

    return 0;
}

/*
 * Unwind to a previously caught frame and return value from that routine.
 *
 * The catch cookie is the number of active routine frames. A valid throw target
 * must still be present on the current call stack. Newer frames are discarded
 * without producing their normal return values; once the target count is
 * reached, zmachine_return() performs the return which throw represents.
 */
static int execute_throw(ZMachine *vm, uint16_t value, uint16_t frame_cookie)
{
    if (!vm || frame_cookie == 0U ||
        (size_t)frame_cookie > vm->frame_count)
        return core_extra_error(vm, "throw targets an inactive Z-machine stack frame");

    while (vm->frame_count > (size_t)frame_cookie) {
        if (zmachine_frame_pop(vm, NULL) != TCL_OK)
            return TCL_ERROR;
    }

    return zmachine_return(vm, value);
}

/* Logical 16-bit shift. Positive counts shift left; negative shift right. */
static uint16_t logical_shift(uint16_t number, int16_t places)
{
    if (places >= 0)
        return (uint16_t)((uint32_t)number << (unsigned)places);

    return (uint16_t)(number >> (unsigned)(-(int32_t)places));
}

/*
 * Arithmetic 16-bit shift without relying on implementation-defined C behavior.
 * Left shifts are identical to logical shifts. On a right shift, unsigned bit
 * movement is followed by an explicit high-bit fill when the original sign bit
 * was set.
 */
static uint16_t arithmetic_shift(uint16_t number, int16_t places)
{
    unsigned shift;
    uint16_t result;

    if (places >= 0)
        return (uint16_t)((uint32_t)number << (unsigned)places);

    shift = (unsigned)(-(int32_t)places);
    result = (uint16_t)(number >> shift);
    if ((number & 0x8000U) != 0U)
        result = (uint16_t)(result | (uint16_t)(0xffffU << (16U - shift)));

    return result;
}

/*
 * Execute one instruction through the pure-core completeness layer.
 *
 * Every decoded instruction first receives the narrow arity validation required
 * to protect the older delegated executor. Unrecognized valid instructions then
 * delegate before operand resolution. Owned operations are evaluated according
 * to their exact opcode contracts. The Standard leaves shifts outside -15..+15
 * undefined; this runtime rejects them deterministically rather than invoking
 * undefined C shifts or silently inventing semantics.
 */
int zmachine_step_core(ZMachine *vm)
{
    ZMachineInstruction instruction;
    uint16_t values[ZM_MAX_OPERANDS];
    char decode_error[128];

    if (!vm || !vm->memory)
        return core_extra_error(vm, "cannot execute without a loaded story");

    if (!zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                     vm->pc, &instruction,
                                     decode_error, sizeof(decode_error))) {
        return core_extra_error(vm, decode_error[0] ? decode_error :
                                "unable to decode Z-machine instruction");
    }

    if (validate_delegated_arity(vm, &instruction) != TCL_OK)
        return TCL_ERROR;

    if (!owns_instruction(vm, &instruction))
        return zmachine_step_core_base(vm);

    if (instruction.operand_count_actual > 0U &&
        zmachine_resolve_operands(vm, &instruction, values,
                                  ZM_MAX_OPERANDS) != TCL_OK)
        return TCL_ERROR;

    if (instruction.form == ZM_FORM_VARIABLE &&
        instruction.operand_count == ZM_OPERANDS_VAR &&
        instruction.opcode_number == 20U) {
        /*
         * input_stream 0/1 selects keyboard/command-file input in a terminal
         * interpreter. In tclzmachine the Tcl host is the source abstraction,
         * so both standard selections are accepted while all actual characters
         * continue to arrive through the same queued-input API.
         */
        if (instruction.operand_count_actual != 1U)
            return core_extra_error(vm, "input_stream requires one operand");
        if (values[0] > 1U)
            return core_extra_error(vm, "invalid Z-machine input stream");
        vm->pc = instruction.next_pc;
        return TCL_OK;
    }

    if (instruction.form == ZM_FORM_VARIABLE &&
        instruction.operand_count == ZM_OPERANDS_VAR &&
        instruction.opcode_number == 21U) {
        /*
         * Sound is intentionally unavailable in the text-only frontend. The
         * Standard specifically asks interpreters not to halt on the historical
         * zero-operand sound_effect form, so every syntactically decoded form is
         * consumed after operand evaluation. No completion callback is invoked
         * because no asynchronous sound was started.
         */
        if (instruction.operand_count_actual > 4U)
            return core_extra_error(vm, "sound_effect has too many operands");
        vm->pc = instruction.next_pc;
        return TCL_OK;
    }

    if (instruction.operand_count == ZM_OPERANDS_0OP &&
        instruction.opcode_number == 9U) {
        /* catch -> result: the Quetzal specification fixes this cookie value. */
        if (instruction.operand_count_actual != 0U)
            return core_extra_error(vm, "catch does not accept operands");
        return store_value(vm, instruction.next_pc,
                           (uint16_t)vm->frame_count);
    }

    if (instruction.operand_count == ZM_OPERANDS_2OP &&
        instruction.opcode_number == 28U) {
        if (instruction.operand_count_actual != 2U)
            return core_extra_error(vm, "throw requires value and stack-frame operands");
        return execute_throw(vm, values[0], values[1]);
    }

    if (instruction.form == ZM_FORM_VARIABLE &&
        instruction.operand_count == ZM_OPERANDS_VAR &&
        instruction.opcode_number == 24U) {
        if (instruction.operand_count_actual != 1U)
            return core_extra_error(vm, "not requires one operand");
        return store_value(vm, instruction.next_pc, (uint16_t)~values[0]);
    }

    if (instruction.form == ZM_FORM_EXTENDED &&
        (instruction.opcode_number == 2U ||
         instruction.opcode_number == 3U)) {
        int16_t places;
        uint16_t result;

        if (instruction.operand_count_actual != 2U)
            return core_extra_error(vm, "shift opcode requires number and places operands");

        places = (int16_t)values[1];
        if (places < -15 || places > 15)
            return core_extra_error(vm, "Z-machine shift count is outside -15..15");

        result = instruction.opcode_number == 2U ?
                 logical_shift(values[0], places) :
                 arithmetic_shift(values[0], places);
        return store_value(vm, instruction.next_pc, result);
    }

    return zmachine_step_core_base(vm);
}
