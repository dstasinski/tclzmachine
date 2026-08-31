/*
 * input_run.c
 *
 * Regression tests for cooperative line input.
 *
 * The first case exercises normal V5 text+parse input, dictionary tokenization,
 * and host-side character validation. The second covers Infocom-compatible V5
 * code which omits the parse operand entirely. The terminating-key cases drive
 * the V5+ header $2e table through the public host-input API: unlisted function
 * keys must be rejected without disturbing the wait, listed keys must become
 * aread's stored result without entering the text buffer, and table value 255
 * must accept any keyboard function key while preserving preloaded text.
 */

#include "tclzmachine.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *memory, size_t address, uint16_t value)
{
    memory[address] = (uint8_t)(value >> 8);
    memory[address + 1U] = (uint8_t)value;
}

static uint16_t global16(const ZMachine *vm, uint8_t variable)
{
    size_t address = (size_t)vm->globals_addr +
                     (size_t)(variable - 0x10U) * 2U;
    return (uint16_t)(((uint16_t)vm->memory[address] << 8) |
                      vm->memory[address + 1U]);
}

static void init_vm(ZMachine *vm)
{
    memset(vm, 0, sizeof(*vm));
    vm->memory = (uint8_t *)calloc(512U, 1U);
    assert(vm->memory != NULL);
    vm->memory_size = 512U;
    vm->version = 5U;
    vm->pc = 0x20U;
    vm->initial_pc = 0x20U;
    vm->static_memory_addr = 0x180U;
    vm->dictionary_addr = 0x120U;
    vm->globals_addr = 0x40U;
    vm->state = ZM_STATE_READY;
    vm->pending_input_terminator = 13U;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
}

static void free_vm(ZMachine *vm)
{
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
    free(vm->memory);
}

/* Install V5 VAR:4 read with only a text-buffer operand, then quit. */
static void install_one_operand_read(ZMachine *vm)
{
    vm->memory[0x20U] = 0xE4U;
    vm->memory[0x21U] = 0x3FU; /* large constant, then omitted operands */
    put16(vm->memory, 0x22U, 0x0080U);
    vm->memory[0x24U] = 0x10U; /* terminating-character store variable */
    vm->memory[0x25U] = 0xBAU; /* quit */
    vm->memory[0x80U] = 20U;
}

int main(void)
{
    {
        ZMachine vm;
        init_vm(&vm);

        /* read 0x80 0xa0 -> global 0x10; then quit. */
        vm.memory[0x20U] = 0xE4U;
        vm.memory[0x21U] = 0x0FU;
        put16(vm.memory, 0x22U, 0x0080U);
        put16(vm.memory, 0x24U, 0x00A0U);
        vm.memory[0x26U] = 0x10U;
        vm.memory[0x27U] = 0xBAU;

        vm.memory[0x80U] = 20U;
        vm.memory[0xA0U] = 4U;

        /* V5 dictionary containing the single word "look". */
        vm.memory[0x120U] = 0U;
        vm.memory[0x121U] = 6U;
        put16(vm.memory, 0x122U, 1U);
        vm.memory[0x124U] = 0x46U;
        vm.memory[0x125U] = 0x94U;
        vm.memory[0x126U] = 0x40U;
        vm.memory[0x127U] = 0xA5U;
        vm.memory[0x128U] = 0x94U;
        vm.memory[0x129U] = 0xA5U;

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_WAITING_INPUT);
        assert(vm.pc == 0x20U);

        /*
         * Tcl strings are UTF-8 but the current line-input capability is
         * printable ASCII ZSCII only. Reject both multibyte Unicode and control
         * characters without consuming the pending read or poisoning the VM.
         */
        assert(zmachine_supply_input(&vm, "\xC3\xA9") == TCL_ERROR);
        assert(vm.state == ZM_STATE_WAITING_INPUT);
        assert(vm.input_available == 0);
        assert(Tcl_DStringLength(&vm.pending_input) == 0);
        assert(strstr(vm.error, "printable ASCII") != NULL);

        assert(zmachine_supply_input(&vm, "bad\tinput") == TCL_ERROR);
        assert(vm.state == ZM_STATE_WAITING_INPUT);
        assert(vm.input_available == 0);

        assert(zmachine_supply_input(&vm, "LOOK") == TCL_OK);
        assert(vm.error[0] == '\0');
        assert(vm.pending_input_terminator == 13U);
        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_HALTED);
        assert(vm.pc == 0x28U);

        assert(vm.memory[0x81U] == 4U);
        assert(memcmp(vm.memory + 0x82U, "look", 4U) == 0);
        assert(vm.memory[0xA1U] == 1U);
        assert(vm.memory[0xA2U] == 0x01U && vm.memory[0xA3U] == 0x24U);
        assert(vm.memory[0xA4U] == 4U);
        assert(vm.memory[0xA5U] == 2U);
        assert(global16(&vm, 0x10U) == 13U);
        assert(vm.pending_input_terminator == 13U);

        free_vm(&vm);
    }

    {
        ZMachine vm;
        init_vm(&vm);

        /*
         * V5 read with only the text-buffer operand. The remaining operand
         * slots are omitted, so compatibility behavior is parse-buffer zero.
         */
        install_one_operand_read(&vm);

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_WAITING_INPUT);
        assert(zmachine_supply_input(&vm, "LOOK") == TCL_OK);
        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_HALTED);
        assert(vm.pc == 0x26U);
        assert(vm.memory[0x81U] == 4U);
        assert(memcmp(vm.memory + 0x82U, "look", 4U) == 0);
        assert(global16(&vm, 0x10U) == 13U);

        free_vm(&vm);
    }

    {
        ZMachine vm;
        init_vm(&vm);
        install_one_operand_read(&vm);

        /* Header word $2e selects a table containing cursor-up only. */
        put16(vm.memory, 0x2EU, 0x0150U);
        vm.memory[0x150U] = 129U;
        vm.memory[0x151U] = 0U;

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_WAITING_INPUT);
        assert(vm.pc == 0x20U);

        /* Cursor-down is valid ZSCII input but is not terminating here. */
        assert(zmachine_supply_key(&vm, 130U) == TCL_ERROR);
        assert(vm.state == ZM_STATE_WAITING_INPUT);
        assert(vm.pc == 0x20U);
        assert(vm.input_available == 0);
        assert(strstr(vm.error, "not listed") != NULL);

        assert(zmachine_supply_key(&vm, 129U) == TCL_OK);
        assert(vm.state == ZM_STATE_READY);
        assert(vm.input_available == 1);
        assert(vm.pending_input_terminator == 129U);
        assert(Tcl_DStringLength(&vm.pending_input) == 0);

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_HALTED);
        assert(vm.pc == 0x26U);
        assert(vm.memory[0x81U] == 0U);
        assert(global16(&vm, 0x10U) == 129U);
        assert(vm.pending_input_terminator == 13U);

        free_vm(&vm);
    }

    {
        ZMachine vm;
        init_vm(&vm);
        install_one_operand_read(&vm);

        /* 255 means any function key; preload text must survive termination. */
        put16(vm.memory, 0x2EU, 0x0150U);
        vm.memory[0x150U] = 255U;
        vm.memory[0x151U] = 0U;
        vm.memory[0x81U] = 4U;
        memcpy(vm.memory + 0x82U, "LOOK", 4U);

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_WAITING_INPUT);
        assert(zmachine_supply_key(&vm, 154U) == TCL_OK);
        assert(zmachine_run(&vm) == TCL_OK);

        assert(vm.state == ZM_STATE_HALTED);
        assert(vm.memory[0x81U] == 4U);
        assert(memcmp(vm.memory + 0x82U, "look", 4U) == 0);
        assert(global16(&vm, 0x10U) == 154U);
        assert(vm.pending_input_terminator == 13U);

        free_vm(&vm);
    }

    puts("input run-loop tests passed");
    return 0;
}
