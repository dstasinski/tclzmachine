/*
 * run_form_dispatch.c
 *
 * Regression coverage for the cooperative run layer's opcode-form boundary.
 * EXTENDED instructions share the decoder's VAR operand-count bucket, so the
 * run loop must not mistake EXT opcodes for VAR-table read/random/scan_table/
 * check_arg_count operations merely because their low opcode numbers match.
 *
 * Version/arity rejection is also tested with variable 0 operands. Reading
 * variable 0 pops the evaluation stack, so an opcode which is illegal for the
 * active story version must be rejected before operand resolution rather than
 * acquiring a speculative stack side effect on its way to an error.
 */

#include "tclzmachine.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void init_vm(ZMachine *vm, uint8_t version)
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
    vm->random_state = 1U;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
}

static void free_vm(ZMachine *vm)
{
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
    /* EXT:4 set_font must execute, not suspend as VAR:4 read. */
    {
        ZMachine vm;

        init_vm(&vm, 5U);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xBEU;
        vm.memory[0x21U] = 0x04U;
        vm.memory[0x22U] = 0x7FU;
        vm.memory[0x23U] = 0x01U;
        vm.memory[0x24U] = 0x10U;
        vm.memory[0x25U] = 0xBAU; /* quit */

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_HALTED);
        assert(vm.pc == 0x26U);
        assert(read_global(&vm, 0x10U) == 1U);
        free_vm(&vm);
    }

    /* Accidental later-version show_status remains a compatibility no-op. */
    {
        ZMachine vm;

        init_vm(&vm, 5U);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xBCU; /* show_status */
        vm.memory[0x21U] = 0xBAU; /* quit */

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_HALTED);
        assert(vm.pc == 0x22U);
        free_vm(&vm);
    }

    /* EXT:7 must never be consumed as VAR:7 random by the run layer. */
    {
        ZMachine vm;

        init_vm(&vm, 5U);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xBEU;
        vm.memory[0x21U] = 0x07U;
        vm.memory[0x22U] = 0xFFU; /* no operands */

        assert(zmachine_run(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "opcode") != NULL);
        free_vm(&vm);
    }

    /*
     * scan_table is V4+. A V3 encoding which names variable 0 must be rejected
     * before operand resolution, leaving the evaluation-stack sentinel intact.
     */
    {
        ZMachine vm;

        init_vm(&vm, 3U);
        assert(zmachine_stack_push(&vm, 0xCAFEU) == TCL_OK);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xF7U; /* VAR:23 scan_table */
        vm.memory[0x21U] = 0x97U; /* variable, small, small, omitted */
        vm.memory[0x22U] = 0x00U; /* variable 0: would pop if resolved */
        vm.memory[0x23U] = 0x80U;
        vm.memory[0x24U] = 0x01U;

        assert(zmachine_run(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "scan_table") != NULL);
        assert(strstr(vm.error, "Version 4") != NULL);
        assert(vm.sp == 1U && vm.stack[0] == 0xCAFEU);
        free_vm(&vm);
    }

    /* check_arg_count is V5+ and likewise must not pop variable 0 in V4. */
    {
        ZMachine vm;

        init_vm(&vm, 4U);
        assert(zmachine_stack_push(&vm, 0xBEEFU) == TCL_OK);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xFFU; /* VAR:31 check_arg_count */
        vm.memory[0x21U] = 0xBFU; /* variable, then omitted */
        vm.memory[0x22U] = 0x00U; /* variable 0: would pop if resolved */

        assert(zmachine_run(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "check_arg_count") != NULL);
        assert(strstr(vm.error, "Version 5") != NULL);
        assert(vm.sp == 1U && vm.stack[0] == 0xBEEFU);
        free_vm(&vm);
    }

    /* verify is not available before V3; reject it rather than invent behavior. */
    {
        ZMachine vm;

        init_vm(&vm, 2U);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xBDU; /* 0OP:13 verify */

        assert(zmachine_run(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "verify") != NULL);
        assert(strstr(vm.error, "Version 3") != NULL);
        free_vm(&vm);
    }

    /*
     * Malformed read has a known structural error before host interaction. Do
     * not suspend and ask Tcl for input only to diagnose the missing buffers on
     * the next cooperative turn.
     */
    {
        ZMachine vm;

        init_vm(&vm, 3U);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xE4U; /* VAR:4 sread */
        vm.memory[0x21U] = 0xFFU; /* all operands omitted */

        assert(zmachine_run(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "buffer operands") != NULL);
        free_vm(&vm);
    }

    /* Nonzero timed line input is rejected because the capability is not offered. */
    {
        ZMachine vm;

        init_vm(&vm, 5U);
        vm.pc = 0x20U;
        vm.memory[0x80U] = 20U;
        vm.memory[0x81U] = 0U;

        /* VAR:4 aread text=0x80 parse=0 time=1 routine=0 -> global16 */
        vm.memory[0x20U] = 0xE4U;
        vm.memory[0x21U] = 0x55U; /* four small constants */
        vm.memory[0x22U] = 0x80U;
        vm.memory[0x23U] = 0x00U;
        vm.memory[0x24U] = 0x01U;
        vm.memory[0x25U] = 0x00U;
        vm.memory[0x26U] = 0x10U;
        Tcl_DStringAppend(&vm.pending_input, "look", -1);
        vm.input_available = 1;

        assert(zmachine_run(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "timed line input") != NULL);
        free_vm(&vm);
    }

    puts("cooperative form-dispatch tests passed");
    return 0;
}
