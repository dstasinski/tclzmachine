/*
 * core_extra.c
 *
 * Regression coverage for core opcodes implemented by the incremental
 * standards-completeness layer: catch/throw, VAR-form not, the two EXT shifts,
 * text-safe buffer_screen behavior, reserved extended-opcode handling, and
 * delegated-core operand validation. Tests execute through the public layered
 * zmachine_step() entry point so decoder form distinctions, validation, operand
 * side effects, and delegation behavior are exercised as well as calculations.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"
#include "zmachine_state.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void init_vm_version(ZMachine *vm, uint8_t version)
{
    memset(vm, 0, sizeof(*vm));
    vm->memory = (uint8_t *)calloc(1024U, 1U);
    assert(vm->memory != NULL);
    vm->memory_size = 1024U;
    vm->version = version;
    vm->static_memory_addr = 0x300U;
    vm->globals_addr = 0x100U;
    vm->state = ZM_STATE_READY;
    vm->output_stream1_enabled = 1;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
}

static void init_vm(ZMachine *vm)
{
    init_vm_version(vm, 5U);
}

static void free_vm(ZMachine *vm)
{
    free(vm->memory);
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
}

static uint16_t read_word(const ZMachine *vm, size_t address)
{
    return (uint16_t)(((uint16_t)vm->memory[address] << 8) |
                      vm->memory[address + 1U]);
}

static uint16_t read_global(const ZMachine *vm, uint8_t variable)
{
    size_t address = (size_t)vm->globals_addr +
                     (size_t)(variable - 0x10U) * 2U;
    return read_word(vm, address);
}

int main(void)
{
    /*
     * catch returns the number of active routine frames. throw to that cookie
     * discards newer frames, then returns from the caught routine with value.
     */
    {
        ZMachine vm;
        uint16_t frame1_locals[1] = {0x1111U};
        uint16_t frame2_locals[1] = {0x2222U};
        uint16_t frame3_locals[1] = {0x3333U};

        init_vm(&vm);
        assert(zmachine_frame_push(&vm, 0x90U, 0x10U, 1,
                                   frame1_locals, 1U, 0U) == TCL_OK);
        assert(zmachine_stack_push(&vm, 0xAAAAU) == TCL_OK);
        assert(zmachine_frame_push(&vm, 0x80U, 1U, 0,
                                   frame2_locals, 1U, 0U) == TCL_OK);

        /* 0OP:9 catch -> global 16. */
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xB9U;
        vm.memory[0x21U] = 0x10U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x22U);
        assert(vm.frame_count == 2U);
        assert(read_global(&vm, 0x10U) == 2U);

        /* Create a newer frame and private stack data which throw must discard. */
        assert(zmachine_stack_push(&vm, 0xBBBBU) == TCL_OK);
        assert(zmachine_frame_push(&vm, 0x70U, 1U, 0,
                                   frame3_locals, 1U, 0U) == TCL_OK);
        assert(zmachine_stack_push(&vm, 0xCCCCU) == TCL_OK);
        assert(vm.frame_count == 3U && vm.sp == 3U);

        /* 2OP:28 throw 0x1234 frame-cookie-2. */
        vm.pc = 0x30U;
        vm.memory[0x30U] = 0xDCU;
        vm.memory[0x31U] = 0x0FU;
        vm.memory[0x32U] = 0x12U;
        vm.memory[0x33U] = 0x34U;
        vm.memory[0x34U] = 0x00U;
        vm.memory[0x35U] = 0x02U;
        assert(zmachine_step(&vm) == TCL_OK);

        /* Frame 3 is discarded and frame 2 returns normally into frame 1. */
        assert(vm.frame_count == 1U);
        assert(vm.pc == 0x80U);
        assert(vm.sp == 1U && vm.stack[0] == 0xAAAAU);
        assert(vm.frames[0].locals[0] == 0x1234U);
        free_vm(&vm);
    }

    /* VAR:24 not stores the 16-bit complement in V5+. */
    {
        ZMachine vm;

        init_vm(&vm);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xF8U;
        vm.memory[0x21U] = 0x3FU;
        vm.memory[0x22U] = 0x0FU;
        vm.memory[0x23U] = 0x0FU;
        vm.memory[0x24U] = 0x10U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x25U);
        assert(read_global(&vm, 0x10U) == 0xF0F0U);
        free_vm(&vm);
    }

    /*
     * EXT:2 log_shift zeros incoming right-shift bits; EXT:3 art_shift copies
     * the sign bit. Positive shifts share ordinary 16-bit left-shift behavior.
     */
    {
        ZMachine vm;

        init_vm(&vm);

        /* log_shift 0x8001 -1 -> 0x4000 */
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xBEU;
        vm.memory[0x21U] = 0x02U;
        vm.memory[0x22U] = 0x0FU;
        vm.memory[0x23U] = 0x80U;
        vm.memory[0x24U] = 0x01U;
        vm.memory[0x25U] = 0xFFU;
        vm.memory[0x26U] = 0xFFU;
        vm.memory[0x27U] = 0x10U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x28U);
        assert(read_global(&vm, 0x10U) == 0x4000U);

        /* art_shift 0x8001 -1 -> 0xC000 */
        vm.pc = 0x30U;
        vm.memory[0x30U] = 0xBEU;
        vm.memory[0x31U] = 0x03U;
        vm.memory[0x32U] = 0x0FU;
        vm.memory[0x33U] = 0x80U;
        vm.memory[0x34U] = 0x01U;
        vm.memory[0x35U] = 0xFFU;
        vm.memory[0x36U] = 0xFFU;
        vm.memory[0x37U] = 0x11U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x38U);
        assert(read_global(&vm, 0x11U) == 0xC000U);

        /* log_shift 0x8001 +1 wraps in the 16-bit value domain -> 0x0002. */
        vm.pc = 0x40U;
        vm.memory[0x40U] = 0xBEU;
        vm.memory[0x41U] = 0x02U;
        vm.memory[0x42U] = 0x0FU;
        vm.memory[0x43U] = 0x80U;
        vm.memory[0x44U] = 0x01U;
        vm.memory[0x45U] = 0x00U;
        vm.memory[0x46U] = 0x01U;
        vm.memory[0x47U] = 0x12U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(read_global(&vm, 0x12U) == 0x0002U);

        /* Arithmetic boundary: -32768 >> 15 remains -1. */
        vm.pc = 0x50U;
        vm.memory[0x50U] = 0xBEU;
        vm.memory[0x51U] = 0x03U;
        vm.memory[0x52U] = 0x0FU;
        vm.memory[0x53U] = 0x80U;
        vm.memory[0x54U] = 0x00U;
        vm.memory[0x55U] = 0xFFU;
        vm.memory[0x56U] = 0xF1U; /* -15 */
        vm.memory[0x57U] = 0x13U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(read_global(&vm, 0x13U) == 0xFFFFU);

        free_vm(&vm);
    }

    /*
     * EXT:29 is not defined for V5. The Standard's out-of-range EXT rule says it
     * is ignored there, so even a variable-0 operand must remain unevaluated and
     * the byte after the decoded operands is not consumed as a store record.
     */
    {
        ZMachine vm;

        init_vm_version(&vm, 5U);
        assert(zmachine_stack_push(&vm, 1U) == TCL_OK);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xBEU;
        vm.memory[0x21U] = 0x1DU; /* EXT:29 */
        vm.memory[0x22U] = 0xBFU; /* one variable operand */
        vm.memory[0x23U] = 0x00U; /* variable 0 */
        vm.memory[0x24U] = 0x10U; /* must remain the next instruction byte */

        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x24U);
        assert(vm.sp == 1U && vm.stack[0] == 1U);
        free_vm(&vm);
    }

    /* Reserved EXT:30+ opcodes are ignored without evaluating their operands. */
    {
        ZMachine vm;

        init_vm_version(&vm, 7U);
        assert(zmachine_stack_push(&vm, 0xCAFEU) == TCL_OK);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xBEU;
        vm.memory[0x21U] = 0x1EU; /* EXT:30 */
        vm.memory[0x22U] = 0xBFU; /* one variable operand */
        vm.memory[0x23U] = 0x00U; /* variable 0 */

        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x24U);
        assert(vm.sp == 1U && vm.stack[0] == 0xCAFEU);
        free_vm(&vm);
    }

    /*
     * V7 inherits V6+ buffer_screen. This text-only runtime ignores buffering
     * advice and therefore always reports old state 0, but it still evaluates
     * the real operand and consumes the required store byte.
     */
    {
        ZMachine vm;

        init_vm_version(&vm, 7U);
        vm.memory[0x100U] = 0x12U;
        vm.memory[0x101U] = 0x34U;
        assert(zmachine_stack_push(&vm, 1U) == TCL_OK);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xBEU;
        vm.memory[0x21U] = 0x1DU; /* EXT:29 buffer_screen */
        vm.memory[0x22U] = 0xBFU; /* one variable operand */
        vm.memory[0x23U] = 0x00U; /* mode from variable 0 */
        vm.memory[0x24U] = 0x10U; /* store global 16 */

        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x25U);
        assert(vm.sp == 0U);
        assert(read_global(&vm, 0x10U) == 0U);
        free_vm(&vm);
    }

    /* A throw cookie which no longer names an active frame is a VM error. */
    {
        ZMachine vm;
        uint16_t locals[1] = {0U};

        init_vm(&vm);
        assert(zmachine_frame_push(&vm, 0x90U, 0U, 1,
                                   locals, 1U, 0U) == TCL_OK);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xDCU;
        vm.memory[0x21U] = 0x0FU;
        vm.memory[0x22U] = 0x00U;
        vm.memory[0x23U] = 0x01U;
        vm.memory[0x24U] = 0x00U;
        vm.memory[0x25U] = 0x02U;
        assert(zmachine_step(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "inactive") != NULL);
        free_vm(&vm);
    }

    /*
     * Variable-form 2OP instructions can be syntactically decoded from a type
     * byte with too few operands. `je` explicitly forbids a one-operand form;
     * rejecting it here proves malformed code cannot reach the base executor's
     * operand array with missing values.
     */
    {
        ZMachine vm;

        init_vm(&vm);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xC1U; /* variable-form 2OP:1 je */
        vm.memory[0x21U] = 0x7FU; /* one small constant, then omitted */
        vm.memory[0x22U] = 1U;
        assert(zmachine_step(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "je requires") != NULL);
        free_vm(&vm);
    }

    /* Ordinary 2OP arithmetic likewise requires both operands. */
    {
        ZMachine vm;

        init_vm(&vm);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xD4U; /* variable-form 2OP:20 add */
        vm.memory[0x21U] = 0x7FU; /* one small constant only */
        vm.memory[0x22U] = 1U;
        assert(zmachine_step(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "exactly two operands") != NULL);
        free_vm(&vm);
    }

    /* Fixed-shape VAR opcodes are checked before values[2] can be read. */
    {
        ZMachine vm;

        init_vm(&vm);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xE1U; /* VAR:1 storew */
        vm.memory[0x21U] = 0x5FU; /* two small constants, third omitted */
        vm.memory[0x22U] = 0x40U;
        vm.memory[0x23U] = 0x01U;
        assert(zmachine_step(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "three-operand") != NULL);
        assert(vm.memory[0x42U] == 0U && vm.memory[0x43U] == 0U);
        free_vm(&vm);
    }

    puts("core extra opcode tests passed");
    return 0;
}
