/*
 * core_compat.c
 *
 * Focused regression tests for interpreter-level opcodes handled by the
 * cooperative run loop rather than the ordinary opcode executor.
 */

#include "tclzmachine.h"
#include "zmachine_state.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Construct a minimal in-memory VM suitable for synthetic opcode tests. */
static void init_vm(ZMachine *vm, uint8_t version, size_t size)
{
    memset(vm, 0, sizeof(*vm));
    vm->memory = (uint8_t *)calloc(size, 1U);
    assert(vm->memory != NULL);
    vm->memory_size = size;
    vm->version = version;
    vm->static_memory_addr = (uint16_t)size;
    vm->initial_pc = 0x20U;
    vm->pc = vm->initial_pc;
    vm->globals_addr = 0x60U;
    vm->state = ZM_STATE_READY;
    vm->random_state = 1U;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
}

/* Release storage owned by a synthetic VM. */
static void free_vm(ZMachine *vm)
{
    free(vm->initial_dynamic_memory);
    free(vm->memory);
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
}

/* Read one global variable directly from the synthetic story image. */
static uint16_t read_global(const ZMachine *vm, uint8_t variable)
{
    size_t address = (size_t)vm->globals_addr +
                     (size_t)(variable - 0x10U) * 2U;
    return (uint16_t)(((uint16_t)vm->memory[address] << 8) |
                      vm->memory[address + 1U]);
}

int main(void)
{
    /* restart restores dynamic memory and jumps to the initial PC. */
    {
        ZMachine vm;
        init_vm(&vm, 3U, 256U);
        vm.initial_pc = 0x30U;
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xB7U; /* restart */
        vm.memory[0x30U] = 0xBAU; /* quit */
        vm.memory[0x50U] = 0x11U;
        vm.initial_dynamic_memory_size = vm.static_memory_addr;
        vm.initial_dynamic_memory = (uint8_t *)malloc(vm.initial_dynamic_memory_size);
        assert(vm.initial_dynamic_memory != NULL);
        memcpy(vm.initial_dynamic_memory, vm.memory,
               vm.initial_dynamic_memory_size);
        vm.memory[0x50U] = 0x99U;

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_HALTED);
        assert(vm.memory[0x50U] == 0x11U);
        assert(vm.pc == 0x31U);
        free_vm(&vm);
    }

    /* verify branches when the checksum of bytes 0x40..EOF is correct. */
    {
        ZMachine vm;
        init_vm(&vm, 3U, 128U);
        vm.declared_file_length = vm.memory_size;
        vm.checksum = 0U; /* all bytes from 0x40 onward are zero */
        vm.memory[0x20U] = 0xBDU; /* verify */
        vm.memory[0x21U] = 0xC4U; /* branch true by +4 -> 0x24 */
        vm.memory[0x22U] = 0xBAU; /* false-path quit */
        vm.memory[0x24U] = 0xBAU; /* true-path quit */

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.pc == 0x25U);
        free_vm(&vm);
    }

    /* A negative random operand deterministically seeds the session PRNG. */
    {
        ZMachine vm;
        uint16_t first_random;
        init_vm(&vm, 5U, 256U);

        /* random -5 -> g16; random 10 -> g17; quit */
        vm.memory[0x20U] = 0xE7U;
        vm.memory[0x21U] = 0x3FU;
        vm.memory[0x22U] = 0xFFU;
        vm.memory[0x23U] = 0xFBU;
        vm.memory[0x24U] = 0x10U;
        vm.memory[0x25U] = 0xE7U;
        vm.memory[0x26U] = 0x7FU;
        vm.memory[0x27U] = 10U;
        vm.memory[0x28U] = 0x11U;
        vm.memory[0x29U] = 0xBAU;

        assert(zmachine_run(&vm) == TCL_OK);
        assert(read_global(&vm, 0x10U) == 0U);
        first_random = read_global(&vm, 0x11U);
        assert(first_random >= 1U && first_random <= 10U);

        vm.pc = 0x20U;
        vm.state = ZM_STATE_READY;
        assert(zmachine_run(&vm) == TCL_OK);
        assert(read_global(&vm, 0x11U) == first_random);
        free_vm(&vm);
    }

    /* scan_table stores the matching address and branches when found. */
    {
        ZMachine vm;
        init_vm(&vm, 5U, 256U);
        vm.memory[0x80U] = 0x11U;
        vm.memory[0x81U] = 0x11U;
        vm.memory[0x82U] = 0x12U;
        vm.memory[0x83U] = 0x34U;

        /* scan_table 0x1234 0x80 2 -> g16 ?true */
        vm.memory[0x20U] = 0xF7U;
        vm.memory[0x21U] = 0x17U;
        vm.memory[0x22U] = 0x12U;
        vm.memory[0x23U] = 0x34U;
        vm.memory[0x24U] = 0x80U;
        vm.memory[0x25U] = 0x02U;
        vm.memory[0x26U] = 0x10U;
        vm.memory[0x27U] = 0xC4U; /* true target 0x2a */
        vm.memory[0x28U] = 0xBAU;
        vm.memory[0x2AU] = 0xBAU;

        assert(zmachine_run(&vm) == TCL_OK);
        assert(read_global(&vm, 0x10U) == 0x82U);
        assert(vm.pc == 0x2BU);
        free_vm(&vm);
    }

    /* check_arg_count branches only for arguments actually supplied. */
    {
        ZMachine vm;
        uint16_t locals[2] = {0U, 0U};
        init_vm(&vm, 5U, 256U);
        assert(zmachine_frame_push(&vm, 0U, 0U, 1,
                                   locals, 2U, 0x03U) == TCL_OK);

        vm.memory[0x20U] = 0xFFU;
        vm.memory[0x21U] = 0x7FU;
        vm.memory[0x22U] = 0x02U; /* second argument */
        vm.memory[0x23U] = 0xC4U; /* true target 0x26 */
        vm.memory[0x24U] = 0xBAU;
        vm.memory[0x26U] = 0xBAU;

        assert(zmachine_run(&vm) == TCL_OK);
        assert(vm.pc == 0x27U);
        free_vm(&vm);
    }

    puts("core compatibility opcode tests passed");
    return 0;
}
