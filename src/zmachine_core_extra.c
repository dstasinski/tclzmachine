/*
 * zmachine_core_extra.c
 *
 * Standards-completeness layer for a small set of ordinary control-flow,
 * arithmetic, and host-neutral I/O-selection instructions not implemented by
 * the original core executor.
 *
 * Opcode legality no longer lives here. Every public execution path first calls
 * zmachine_preflight_instruction(), which owns version gates, encoded operand
 * counts, literal-value rejection, and the Standard's ignored-EXT rule before
 * any layer may resolve an operand. This module therefore has one job: recognize
 * the instructions whose runtime semantics it implements, resolve their already
 * validated operands exactly once, execute them, and delegate everything else.
 *
 * Runtime checks which inherently depend on evaluated values remain local. For
 * example, a variable shift count must be checked after evaluation, and a throw
 * cookie must still name an active frame. Those are execution semantics rather
 * than duplicated opcode-table legality.
 *
 * catch/throw use the standard frame cookie: the number of routine frames
 * currently on the system stack. throw discards newer frames, then returns from
 * the routine whose frame count was caught. Logical and arithmetic shifts avoid
 * implementation-defined signed right-shift behavior by doing all bit movement
 * in the unsigned 16-bit value domain and explicitly filling sign bits.
 *
 * input_stream's physical command-file policy is normally owned by the higher
 * stream wrapper; this layer retains a host-neutral fallback for an already
 * validated instruction. sound_effect is consumed as an unavailable
 * presentation effect because this interpreter advertises no sampled sound.
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
 * Return nonzero only for opcode families whose runtime semantics live here.
 *
 * Physical form remains part of dispatch identity. EXTENDED instructions share
 * the decoder's VAR-sized operand bucket, so an opcode number alone must never
 * be used to identify a VAR-table instruction.
 */
static int owns_instruction(const ZMachine *vm,
                            const ZMachineInstruction *instruction)
{
    if (!vm || !instruction)
        return 0;

    if (instruction->form == ZM_FORM_VARIABLE &&
        instruction->operand_count == ZM_OPERANDS_VAR &&
        (instruction->opcode_number == 20U ||
         instruction->opcode_number == 21U))
        return 1; /* input_stream / sound_effect */

    if (vm->version >= 5U &&
        instruction->operand_count == ZM_OPERANDS_0OP &&
        instruction->opcode_number == 9U)
        return 1; /* catch; V1-V4 opcode 9 is pop and is run-loop owned */

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
 * The caller has already passed the shared preflight. Unrecognized instructions
 * delegate untouched. Owned operations resolve operands exactly once. The
 * Standard leaves shift counts outside -15..+15 undefined; this runtime rejects
 * a runtime-computed out-of-range count deterministically rather than invoking
 * undefined C shift behavior.
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
         * input_stream 0/1 selects keyboard/command-file input. The higher stream
         * wrapper normally supplies actual host-file policy; this fallback only
         * preserves the standard semantic values when reached directly.
         */
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
         * Standard asks interpreters not to halt on the historical zero-operand
         * sound_effect form, so a preflight-valid form is consumed after normal
         * operand evaluation. No completion callback is invoked because no sound
         * was started.
         */
        vm->pc = instruction.next_pc;
        return TCL_OK;
    }

    if (instruction.operand_count == ZM_OPERANDS_0OP &&
        instruction.opcode_number == 9U) {
        /* catch -> result: the Quetzal specification fixes this cookie value. */
        return store_value(vm, instruction.next_pc,
                           (uint16_t)vm->frame_count);
    }

    if (instruction.operand_count == ZM_OPERANDS_2OP &&
        instruction.opcode_number == 28U)
        return execute_throw(vm, values[0], values[1]);

    if (instruction.form == ZM_FORM_VARIABLE &&
        instruction.operand_count == ZM_OPERANDS_VAR &&
        instruction.opcode_number == 24U)
        return store_value(vm, instruction.next_pc, (uint16_t)~values[0]);

    if (instruction.form == ZM_FORM_EXTENDED &&
        (instruction.opcode_number == 2U ||
         instruction.opcode_number == 3U)) {
        int16_t places = (int16_t)values[1];
        uint16_t result;

        if (places < -15 || places > 15)
            return core_extra_error(vm,
                                    "Z-machine shift count is outside -15..15");

        result = instruction.opcode_number == 2U ?
                 logical_shift(values[0], places) :
                 arithmetic_shift(values[0], places);
        return store_value(vm, instruction.next_pc, result);
    }

    return zmachine_step_core_base(vm);
}
