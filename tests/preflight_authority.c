/*
 * preflight_authority.c
 *
 * Regression coverage proving that both cooperative execution and ordinary
 * step dispatch use one decoded-instruction legality authority.
 *
 * These cases concentrate on opcodes historically owned before the public step
 * chain (read/run-loop operations and save/restore requests) plus version-table
 * transitions which were previously duplicated in lower layers. Invalid forms
 * must fail before host suspension or operand side effects, while documented
 * compatibility behavior and ignored extended opcodes must still execute.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"
#include "zmachine_state.h"

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

static void expect_run_error(uint8_t version,
                             const uint8_t *code,
                             size_t code_size,
                             const char *message)
{
    ZMachine vm;

    init_vm(&vm, version);
    memcpy(vm.memory + 0x20U, code, code_size);
    vm.pc = 0x20U;

    assert(zmachine_run(&vm) == TCL_ERROR);
    assert(vm.state == ZM_STATE_ERROR);
    assert(strstr(vm.error, message) != NULL);

    free_vm(&vm);
}

int main(void)
{
    /* V5 moved full save/restore from the old 0OP forms to EXT:0/EXT:1. */
    {
        static const uint8_t old_save[] = {0xB5U};
        static const uint8_t old_restore[] = {0xB6U};

        expect_run_error(5U, old_save, sizeof(old_save),
                         "0OP save/restore is illegal");
        expect_run_error(5U, old_restore, sizeof(old_restore),
                         "0OP save/restore is illegal");
    }

    /* show_status does not exist before V3. */
    {
        static const uint8_t code[] = {0xBCU};
        expect_run_error(2U, code, sizeof(code),
                         "show_status is unavailable before Version 3");
    }

    /*
     * The Standard explicitly recommends accepting accidental later
     * show_status instructions as no-ops (notably Wishbringer release 23).
     */
    {
        ZMachine vm;
        static const uint8_t code[] = {
            0xBCU, /* show_status compatibility no-op */
            0xBAU  /* quit */
        };

        init_vm(&vm, 5U);
        memcpy(vm.memory + 0x20U, code, sizeof(code));
        vm.pc = 0x20U;

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_HALTED);
        assert(vm.pc == 0x22U);
        free_vm(&vm);
    }

    /* In V1-V4 byte BE is the otherwise-illegal 0OP:14, not an EXT prefix. */
    {
        static const uint8_t code[] = {0xBEU};
        expect_run_error(4U, code, sizeof(code),
                         "extended opcode prefix is illegal before Version 5");
    }

    /* piracy is introduced in V5. */
    {
        static const uint8_t code[] = {0xBFU};
        expect_run_error(4U, code, sizeof(code),
                         "piracy is unavailable before Version 5");
    }

    /* Opcode 0OP:9 changes meaning: pop through V4, catch from V5 onward. */
    {
        ZMachine vm;
        static const uint8_t code[] = {
            0xB9U, /* pop */
            0xBAU  /* quit */
        };

        init_vm(&vm, 4U);
        memcpy(vm.memory + 0x20U, code, sizeof(code));
        vm.pc = 0x20U;
        assert(zmachine_stack_push(&vm, 0xCAFEU) == TCL_OK);

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_HALTED);
        assert(vm.sp == 0U);
        free_vm(&vm);
    }

    {
        ZMachine vm;
        static const uint8_t code[] = {
            0xB9U, /* catch -> global 16 */
            0x10U,
            0xBAU
        };

        init_vm(&vm, 5U);
        memcpy(vm.memory + 0x20U, code, sizeof(code));
        vm.pc = 0x20U;

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_HALTED);
        assert(read_global(&vm, 0x10U) == 0U);
        free_vm(&vm);
    }

    /*
     * EXT:29 is outside the defined V5 table. The shared preflight consumes it
     * before the run loop or lower core can evaluate variable 0.
     */
    {
        ZMachine vm;
        static const uint8_t code[] = {
            0xBEU, 0x1DU, /* EXT:29 */
            0xBFU,       /* variable, then omitted */
            0x00U,       /* variable 0: must not pop */
            0xBAU        /* quit */
        };

        init_vm(&vm, 5U);
        memcpy(vm.memory + 0x20U, code, sizeof(code));
        vm.pc = 0x20U;
        assert(zmachine_stack_push(&vm, 0xBEEFU) == TCL_OK);

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_HALTED);
        assert(vm.sp == 1U && vm.stack[0] == 0xBEEFU);
        assert(vm.pc == 0x25U);
        free_vm(&vm);
    }

    puts("shared opcode preflight authority tests passed");
    return 0;
}
