/*
 * undo.c
 *
 * Regression tests for EXT:9 save_undo and EXT:10 restore_undo. The tests
 * verify the Z-machine state-of-play boundary: dynamic memory, evaluation
 * stack, routine frames, and the saved continuation are restored, while Flags
 * 2 and interpreter presentation/PRNG state survive from the live session.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"
#include "zmachine_undo.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void init_vm(ZMachine *vm, size_t size)
{
    memset(vm, 0, sizeof(*vm));
    vm->memory = (uint8_t *)calloc(size, 1U);
    assert(vm->memory != NULL);
    vm->memory_size = size;
    vm->version = 5U;
    vm->static_memory_addr = 0x0200U;
    vm->globals_addr = 0x0100U;
    vm->state = ZM_STATE_READY;
    vm->random_state = 1U;
    vm->output_stream1_enabled = 1;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
}

static void free_vm(ZMachine *vm)
{
    zmachine_undo_discard(vm);
    free(vm->memory);
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
}

static uint16_t read_global(const ZMachine *vm, uint8_t variable)
{
    size_t address = (size_t)vm->globals_addr +
                     (size_t)(variable - 0x10U) * 2U;
    return (uint16_t)(((uint16_t)vm->memory[address] << 8) |
                      vm->memory[address + 1U]);
}

int main(void)
{
    {
        ZMachine vm;
        ZMachineFrame *frame;
        uint16_t locals[1] = {0x1111U};

        init_vm(&vm, 1024U);
        vm.memory[0x10U] = 0x12U;
        vm.memory[0x11U] = 0x34U;
        vm.flags2 = 0x1234U;
        vm.memory[0x180U] = 0x44U;

        /* save_undo -> local 1 */
        vm.memory[0x20U] = 0xBEU;
        vm.memory[0x21U] = 9U;
        vm.memory[0x22U] = 0xFFU;
        vm.memory[0x23U] = 1U;

        /* restore_undo -> global 16; successful restore never returns here. */
        vm.memory[0x30U] = 0xBEU;
        vm.memory[0x31U] = 10U;
        vm.memory[0x32U] = 0xFFU;
        vm.memory[0x33U] = 0x10U;

        assert(zmachine_frame_push(&vm, 0x90U, 0U, 1,
                                   locals, 1U, 0U) == TCL_OK);
        assert(zmachine_stack_push(&vm, 0x2222U) == TCL_OK);

        vm.pc = 0x20U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x24U);
        frame = zmachine_current_frame(&vm);
        assert(frame != NULL && frame->locals[0] == 1U);
        assert(vm.sp == 1U && vm.stack[0] == 0x2222U);
        assert(vm.undo_state != NULL);

        /* Mutate both saved state and interpreter-only state after the save. */
        vm.memory[0x180U] = 0x99U;
        assert(zmachine_variable_write(&vm, 1U, 0, 0x7777U) == TCL_OK);
        assert(zmachine_stack_replace_top(&vm, 0x8888U) == TCL_OK);
        assert(zmachine_stack_push(&vm, 0x9999U) == TCL_OK);

        vm.memory[0x10U] = 0xABU;
        vm.memory[0x11U] = 0xCDU;
        vm.flags2 = 0xABCDU;
        vm.random_state = 0xDEADBEEFU;
        vm.current_window = 7U;
        vm.output_stream1_enabled = 0;
        vm.stream3_depth = 1U;
        vm.stream3_tables[0] = 0x0190U;

        vm.pc = 0x30U;
        assert(zmachine_step(&vm) == TCL_OK);

        /* Control resumes after save_undo and that original store now returns 2. */
        assert(vm.pc == 0x24U);
        frame = zmachine_current_frame(&vm);
        assert(frame != NULL && frame->locals[0] == 2U);

        /* Dynamic memory and the execution stacks return to the saved state. */
        assert(vm.memory[0x180U] == 0x44U);
        assert(vm.frame_count == 1U);
        assert(vm.sp == 1U && vm.stack[0] == 0x2222U);

        /* Flags 2 and non-state-of-play interpreter state survive the undo. */
        assert(vm.memory[0x10U] == 0xABU && vm.memory[0x11U] == 0xCDU);
        assert(vm.flags2 == 0xABCDU);
        assert(vm.random_state == 0xDEADBEEFU);
        assert(vm.current_window == 7U);
        assert(vm.output_stream1_enabled == 0);
        assert(vm.stream3_depth == 1U && vm.stream3_tables[0] == 0x0190U);

        free_vm(&vm);
    }

    {
        ZMachine vm;

        init_vm(&vm, 1024U);

        /* Without a previous save_undo, retain the deterministic failure 0. */
        vm.memory[0x20U] = 0xBEU;
        vm.memory[0x21U] = 10U;
        vm.memory[0x22U] = 0xFFU;
        vm.memory[0x23U] = 0x10U;
        vm.pc = 0x20U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x24U);
        assert(read_global(&vm, 0x10U) == 0U);

        free_vm(&vm);
    }

    {
        ZMachine vm;

        init_vm(&vm, 1024U);

        /*
         * save_undo storing to variable 0 pushes 1 in live play. Undo must
         * restore the pre-store stack before pushing 2, so the old 1 vanishes.
         */
        vm.memory[0x20U] = 0xBEU;
        vm.memory[0x21U] = 9U;
        vm.memory[0x22U] = 0xFFU;
        vm.memory[0x23U] = 0U;
        vm.memory[0x30U] = 0xBEU;
        vm.memory[0x31U] = 10U;
        vm.memory[0x32U] = 0xFFU;
        vm.memory[0x33U] = 0x10U;

        assert(zmachine_stack_push(&vm, 0x1111U) == TCL_OK);
        vm.pc = 0x20U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.sp == 2U);
        assert(vm.stack[0] == 0x1111U && vm.stack[1] == 1U);

        vm.pc = 0x30U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x24U);
        assert(vm.sp == 2U);
        assert(vm.stack[0] == 0x1111U && vm.stack[1] == 2U);

        free_vm(&vm);
    }

    puts("undo state tests passed");
    return 0;
}
