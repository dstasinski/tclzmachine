/*
 * run_form_dispatch.c
 *
 * Regression coverage for the cooperative run-loop's form-aware interpreter
 * opcode dispatch. Extended instructions share the decoder's VAR operand-count
 * bucket, so they must not be mistaken for VAR:7 random, VAR:23 scan_table, or
 * VAR:31 check_arg_count merely because their low opcode number matches.
 *
 * The run loop invokes the same decoded-instruction preflight as the public step
 * boundary before any owned opcode may resolve operands or suspend for host
 * input. These tests therefore also exercise that shared authority while checking
 * that stack variable 0 retains its single-evaluation semantics and unavailable
 * timed line input degrades to ordinary input rather than becoming a VM error.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"
#include "zmachine_state.h"

#include <assert.h>
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
    /* EXT:7 must not be mistaken for VAR:7 random. */
    {
        ZMachine vm;

        init_vm(&vm, 5U);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xBEU;
        vm.memory[0x21U] = 0x07U;
        vm.memory[0x22U] = 0xFFU;

        assert(zmachine_run(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "Version 6") != NULL);
        free_vm(&vm);
    }

    /* EXT:23 must not be mistaken for VAR:23 scan_table. */
    {
        ZMachine vm;

        init_vm(&vm, 5U);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xBEU;
        vm.memory[0x21U] = 0x17U;
        vm.memory[0x22U] = 0xFFU;

        assert(zmachine_run(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "Version 6") != NULL);
        free_vm(&vm);
    }

    /* EXT:31 is in the standard ignore range, not VAR:31 check_arg_count. */
    {
        ZMachine vm;

        init_vm(&vm, 5U);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xBEU;
        vm.memory[0x21U] = 0x1FU;
        vm.memory[0x22U] = 0xFFU;
        vm.memory[0x23U] = 0xBAU;

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_HALTED);
        assert(vm.pc == 0x24U);
        free_vm(&vm);
    }

    /* V3 random remains a real VAR:7 instruction. */
    {
        ZMachine vm;

        init_vm(&vm, 3U);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xE7U;
        vm.memory[0x21U] = 0x7FU;
        vm.memory[0x22U] = 1U;
        vm.memory[0x23U] = 0x10U;
        vm.memory[0x24U] = 0xBAU;

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_HALTED);
        assert(read_global(&vm, 0x10U) == 1U);
        free_vm(&vm);
    }

    /* scan_table is unavailable before V4 and cannot pop variable 0 first. */
    {
        ZMachine vm;

        init_vm(&vm, 3U);
        assert(zmachine_stack_push(&vm, 0xCAFEU) == TCL_OK);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xF7U;
        vm.memory[0x21U] = 0x95U;
        vm.memory[0x22U] = 0x00U;
        vm.memory[0x23U] = 0x80U;
        vm.memory[0x24U] = 0x01U;
        vm.memory[0x25U] = 0x82U;

        assert(zmachine_run(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "VAR opcode requires V4 or later") != NULL);
        assert(vm.sp == 1U && vm.stack[0] == 0xCAFEU);
        free_vm(&vm);
    }

    /* check_arg_count is V5+ and likewise must not pop variable 0 in V4. */
    {
        ZMachine vm;

        init_vm(&vm, 4U);
        assert(zmachine_stack_push(&vm, 0xBEEFU) == TCL_OK);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xFFU;
        vm.memory[0x21U] = 0xBFU;
        vm.memory[0x22U] = 0x00U;

        assert(zmachine_run(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "VAR opcode requires V5 or later") != NULL);
        assert(vm.sp == 1U && vm.stack[0] == 0xBEEFU);
        free_vm(&vm);
    }

    /* verify is not available before V3. */
    {
        ZMachine vm;

        init_vm(&vm, 2U);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xBDU;

        assert(zmachine_run(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "verify") != NULL);
        assert(strstr(vm.error, "Version 3") != NULL);
        free_vm(&vm);
    }

    /* Malformed V3 read is diagnosed before host interaction. */
    {
        ZMachine vm;

        init_vm(&vm, 3U);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xE4U;
        vm.memory[0x21U] = 0xFFU;

        assert(zmachine_run(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "V1-V3 read requires exactly two operands") != NULL);
        free_vm(&vm);
    }

    /*
     * Timed input is not advertised. A V5 story which nevertheless supplies
     * time/routine operands is normalized to ordinary line input; with pending
     * host input already available, it completes normally and reaches quit.
     */
    {
        ZMachine vm;

        init_vm(&vm, 5U);
        vm.pc = 0x20U;
        vm.memory[0x80U] = 20U;
        vm.memory[0x81U] = 0U;

        vm.memory[0x20U] = 0xE4U;
        vm.memory[0x21U] = 0x55U;
        vm.memory[0x22U] = 0x80U;
        vm.memory[0x23U] = 0x00U;
        vm.memory[0x24U] = 0x01U;
        vm.memory[0x25U] = 0x00U;
        vm.memory[0x26U] = 0x10U;
        vm.memory[0x27U] = 0xBAU;
        Tcl_DStringAppend(&vm.pending_input, "look", -1);
        vm.input_available = 1;

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_HALTED);
        assert(vm.pc == 0x28U);
        assert(vm.memory[0x81U] == 4U);
        assert(memcmp(vm.memory + 0x82U, "look", 4U) == 0);
        assert(read_global(&vm, 0x10U) == 13U);
        free_vm(&vm);
    }

    puts("cooperative form-dispatch tests passed");
    return 0;
}