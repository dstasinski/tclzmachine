/*
 * io_compat.c
 *
 * Regression coverage for Z-machine input_stream selection and unavailable
 * sound compatibility. Stream 0 selects interactive host input immediately;
 * selecting stream 1 without a configured replay file yields cooperatively so
 * Tcl can choose the command-file path. Sound remains intentionally absent from
 * this text-only interpreter, but its operands still have normal VM effects.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"
#include "zmachine_state.h"
#include "zmachine_stream.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void init_vm(ZMachine *vm)
{
    memset(vm, 0, sizeof(*vm));
    vm->memory = (uint8_t *)calloc(512U, 1U);
    assert(vm->memory != NULL);
    vm->memory_size = 512U;
    vm->version = 5U;
    vm->static_memory_addr = 0x180U;
    vm->state = ZM_STATE_READY;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
}

static void free_vm(ZMachine *vm)
{
    /* Public reset also releases any lazily allocated host stream state. */
    assert(zmachine_reset(vm) == TCL_OK);
    free(vm->memory);
    vm->memory = NULL;
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
}

int main(void)
{
    /* Stream 0 is immediate; first selection of stream 1 requests a host file. */
    {
        ZMachine vm;

        init_vm(&vm);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xF4U; /* VAR:20 input_stream */
        vm.memory[0x21U] = 0x7FU;
        vm.memory[0x22U] = 0U;
        vm.memory[0x23U] = 0xF4U;
        vm.memory[0x24U] = 0x7FU;
        vm.memory[0x25U] = 1U;

        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x23U);
        assert(zmachine_current_input_stream(&vm) == 0);

        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x23U);
        assert(vm.state == ZM_STATE_WAITING_STREAM_FILE);
        assert(strcmp(zmachine_pending_stream_request(&vm), "replay") == 0);

        /* Declining replay leaves keyboard input selected and advances opcode. */
        assert(zmachine_cancel_stream_file(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_READY);
        assert(vm.pc == 0x26U);
        assert(zmachine_current_input_stream(&vm) == 0);
        free_vm(&vm);
    }

    /* Non-standard stream numbers remain story errors. */
    {
        ZMachine vm;

        init_vm(&vm);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xF4U;
        vm.memory[0x21U] = 0x7FU;
        vm.memory[0x22U] = 2U;

        assert(zmachine_step(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "input stream") != NULL);
        /* Do not call reset on a deliberately errored VM; no stream state exists. */
        free(vm.memory);
        Tcl_DStringFree(&vm.output);
        Tcl_DStringFree(&vm.pending_input);
    }

    /* Historical zero-operand sound_effect is harmless even without sound. */
    {
        ZMachine vm;

        init_vm(&vm);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xF5U; /* VAR:21 sound_effect */
        vm.memory[0x21U] = 0xFFU; /* no operands */

        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x22U);
        free_vm(&vm);
    }

    /* A discarded sound operand still performs normal variable-0 stack pop. */
    {
        ZMachine vm;

        init_vm(&vm);
        assert(zmachine_stack_push(&vm, 1U) == TCL_OK);
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xF5U;
        vm.memory[0x21U] = 0xBFU; /* variable operand, then omitted */
        vm.memory[0x22U] = 0U;    /* variable 0 / stack */

        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x23U);
        assert(vm.sp == 0U);
        free_vm(&vm);
    }

    puts("embedded input/sound compatibility tests passed");
    return 0;
}
