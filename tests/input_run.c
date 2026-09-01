/*
 * input_run.c
 *
 * Regression tests for cooperative line input.
 *
 * Coverage includes normal V5 text+parse input, the established one-operand V5
 * compatibility form, version-specific structural validation before suspension,
 * untimed fallback for stories which request unavailable timed input, and V5+
 * terminating-character handling through the public key API.
 */

#include "tclzmachine.h"
#include "zmachine_state.h"

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
    vm->output_stream1_enabled = 1;
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
    vm->memory[0x21U] = 0x3FU;
    put16(vm->memory, 0x22U, 0x0080U);
    vm->memory[0x24U] = 0x10U;
    vm->memory[0x25U] = 0xBAU;
    vm->memory[0x80U] = 20U;
}

int main(void)
{
    /* Normal V5 read, tokenization, and ASCII host-input validation. */
    {
        ZMachine vm;
        init_vm(&vm);

        vm.memory[0x20U] = 0xE4U;
        vm.memory[0x21U] = 0x0FU;
        put16(vm.memory, 0x22U, 0x0080U);
        put16(vm.memory, 0x24U, 0x00A0U);
        vm.memory[0x26U] = 0x10U;
        vm.memory[0x27U] = 0xBAU;

        vm.memory[0x80U] = 20U;
        vm.memory[0xA0U] = 4U;

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

    /* Preserve the established V5 omitted-parse compatibility path. */
    {
        ZMachine vm;
        init_vm(&vm);
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

    /* V3 read cannot acquire a third timed operand and must fail before wait/pop. */
    {
        ZMachine vm;
        init_vm(&vm);
        vm.version = 3U;
        vm.memory[0x20U] = 0xE4U;
        vm.memory[0x21U] = 0x97U; /* variable, small, small, omitted */
        vm.memory[0x22U] = 0x00U; /* variable 0: must not pop */
        vm.memory[0x23U] = 0xA0U;
        vm.memory[0x24U] = 0x00U;
        assert(zmachine_stack_push(&vm, 0xD303U) == TCL_OK);

        assert(zmachine_run(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "V1-V3 read requires exactly two operands") != NULL);
        assert(vm.sp == 1U && vm.stack[0] == 0xD303U);

        free_vm(&vm);
    }

    /* V4 requires both text and parse before it may suspend. */
    {
        ZMachine vm;
        init_vm(&vm);
        vm.version = 4U;
        vm.memory[0x20U] = 0xE4U;
        vm.memory[0x21U] = 0xBFU;
        vm.memory[0x22U] = 0x00U;
        assert(zmachine_stack_push(&vm, 0xD404U) == TCL_OK);

        assert(zmachine_run(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "V4 read requires two to four operands") != NULL);
        assert(vm.sp == 1U && vm.stack[0] == 0xD404U);

        free_vm(&vm);
    }

    /* A valid V4 two-operand read may suspend without evaluating variable 0. */
    {
        ZMachine vm;
        init_vm(&vm);
        vm.version = 4U;
        vm.memory[0x20U] = 0xE4U;
        vm.memory[0x21U] = 0x9FU; /* variable, small, omitted, omitted */
        vm.memory[0x22U] = 0x00U;
        vm.memory[0x23U] = 0xA0U;
        assert(zmachine_stack_push(&vm, 0xD414U) == TCL_OK);

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_WAITING_INPUT);
        assert(vm.sp == 1U && vm.stack[0] == 0xD414U);

        free_vm(&vm);
    }

    /*
     * The host advertises timed input as unavailable. If an older V4+ story
     * nevertheless supplies a nonzero timer, degrade to an ordinary untimed
     * request rather than aborting; the VM should suspend at the read PC.
     */
    {
        ZMachine vm;
        init_vm(&vm);
        vm.memory[0x20U] = 0xE4U;
        vm.memory[0x21U] = 0x07U; /* large, large, small, omitted */
        put16(vm.memory, 0x22U, 0x0080U);
        put16(vm.memory, 0x24U, 0x00A0U);
        vm.memory[0x26U] = 1U;
        vm.memory[0x27U] = 0x10U;
        vm.memory[0x28U] = 0xBAU;
        vm.memory[0x80U] = 20U;
        vm.memory[0xA0U] = 4U;

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_WAITING_INPUT);
        assert(vm.pc == 0x20U);

        assert(zmachine_supply_input(&vm, "LOOK") == TCL_OK);
        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_HALTED);
        assert(vm.pc == 0x29U);
        assert(vm.memory[0x81U] == 4U);
        assert(memcmp(vm.memory + 0x82U, "look", 4U) == 0);
        assert(global16(&vm, 0x10U) == 13U);

        free_vm(&vm);
    }

    /* Header word $2e selecting one terminating function key. */
    {
        ZMachine vm;
        init_vm(&vm);
        install_one_operand_read(&vm);

        put16(vm.memory, 0x2EU, 0x0150U);
        vm.memory[0x150U] = 129U;
        vm.memory[0x151U] = 0U;

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_WAITING_INPUT);
        assert(vm.pc == 0x20U);

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

    /* 255 means any function key; preloaded text must survive termination. */
    {
        ZMachine vm;
        init_vm(&vm);
        install_one_operand_read(&vm);

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