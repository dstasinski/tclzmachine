/*
 * input_preload.c
 *
 * Regression coverage for Version 5+ read/aread text buffers which contain
 * preloaded characters from an interrupted earlier input operation. New Tcl
 * input must append after that prefix, respect the original maximum length, and
 * leave the older V1-V4 buffer convention unchanged.
 */

#include "tclzmachine.h"
#include "zmachine_input.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void init_vm(ZMachine *vm, uint8_t version)
{
    memset(vm, 0, sizeof(*vm));
    vm->memory = (uint8_t *)calloc(512U, 1U);
    assert(vm->memory != NULL);
    vm->memory_size = 512U;
    vm->version = version;
    vm->static_memory_addr = 0x180U;
    vm->state = ZM_STATE_READY;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
}

static void queue_input(ZMachine *vm, const char *text)
{
    Tcl_DStringSetLength(&vm->pending_input, 0);
    Tcl_DStringAppend(&vm->pending_input, text, -1);
    vm->input_available = 1;
}

static void free_vm(ZMachine *vm)
{
    free(vm->memory);
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
}

int main(void)
{
    /* V5+ appends newly entered text after the byte-1 preloaded prefix. */
    {
        ZMachine vm;
        uint16_t terminator = 0U;

        init_vm(&vm, 5U);
        vm.memory[0x80U] = 10U;
        vm.memory[0x81U] = 4U;
        memcpy(vm.memory + 0x82U, "take", 4U);
        queue_input(&vm, " LAMP");

        assert(zmachine_input_read_line(&vm, 0x80U, 0U,
                                        &terminator) == TCL_OK);
        assert(vm.memory[0x81U] == 9U);
        assert(memcmp(vm.memory + 0x82U, "take lamp", 9U) == 0);
        assert(terminator == 13U);
        assert(vm.input_available == 0);
        assert(Tcl_DStringLength(&vm.pending_input) == 0);
        free_vm(&vm);
    }

    /* The combined prefix+new text is truncated to the story's original max. */
    {
        ZMachine vm;

        init_vm(&vm, 5U);
        vm.memory[0x80U] = 6U;
        vm.memory[0x81U] = 4U;
        memcpy(vm.memory + 0x82U, "look", 4U);
        queue_input(&vm, " around");

        assert(zmachine_input_read_line(&vm, 0x80U, 0U, NULL) == TCL_OK);
        assert(vm.memory[0x81U] == 6U);
        assert(memcmp(vm.memory + 0x82U, "look a", 6U) == 0);
        free_vm(&vm);
    }

    /* An impossible preloaded count is rejected without consuming Tcl input. */
    {
        ZMachine vm;

        init_vm(&vm, 5U);
        vm.memory[0x80U] = 3U;
        vm.memory[0x81U] = 4U;
        memcpy(vm.memory + 0x82U, "oops", 4U);
        queue_input(&vm, "x");

        assert(zmachine_input_read_line(&vm, 0x80U, 0U, NULL) == TCL_ERROR);
        assert(vm.input_available == 1);
        assert(strcmp(Tcl_DStringValue(&vm.pending_input), "x") == 0);
        free_vm(&vm);
    }

    /* V1-V4 retain their byte-1 text plus zero-terminator convention. */
    {
        ZMachine vm;

        init_vm(&vm, 4U);
        vm.memory[0x80U] = 6U;
        queue_input(&vm, "LOOK");

        assert(zmachine_input_read_line(&vm, 0x80U, 0U, NULL) == TCL_OK);
        assert(memcmp(vm.memory + 0x81U, "look", 4U) == 0);
        assert(vm.memory[0x85U] == 0U);
        free_vm(&vm);
    }

    puts("preloaded input tests passed");
    return 0;
}
