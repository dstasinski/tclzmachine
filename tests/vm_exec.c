/*
 * vm_exec.c
 *
 * Focused regression coverage for the ordinary core instruction executor and
 * its public helper contracts. The synthetic cases exercise V3/V5 routine
 * entry/local initialization, return/store behavior, left-to-right operand
 * resolution, direct and computed indirect-variable references (especially
 * stack variable 0), call-null semantics, arithmetic/branch instructions,
 * variable-form signed division/remainder, and dynamic-memory stores.
 *
 * Higher cooperative layers such as input, file requests, presentation, and
 * lexical opcodes have dedicated tests; this file is intended to keep failures
 * attributable to core execution semantics.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Allocate a zeroed synthetic story image with globals and writable memory. */
static void init_vm(ZMachine *vm, uint8_t version, size_t size)
{
    memset(vm, 0, sizeof(*vm));
    vm->memory = (uint8_t *)calloc(size, 1U);
    assert(vm->memory != NULL);
    vm->memory_size = size;
    vm->version = version;
    vm->static_memory_addr = (uint16_t)size;
    vm->globals_addr = 0x40U;
    vm->state = ZM_STATE_READY;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
}

/* Release storage initialized by init_vm(). */
static void free_vm(ZMachine *vm)
{
    free(vm->memory);
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
}

/* Read/write global words directly so opcode effects can be asserted. */
static uint16_t read_global(const ZMachine *vm, uint8_t variable)
{
    size_t address = (size_t)vm->globals_addr +
                     (size_t)(variable - 0x10U) * 2U;
    return (uint16_t)(((uint16_t)vm->memory[address] << 8) |
                      vm->memory[address + 1U]);
}

static void write_global(ZMachine *vm, uint8_t variable, uint16_t value)
{
    size_t address = (size_t)vm->globals_addr +
                     (size_t)(variable - 0x10U) * 2U;
    vm->memory[address] = (uint8_t)(value >> 8);
    vm->memory[address + 1U] = (uint8_t)value;
}

int main(void)
{
    /* V5 routine locals begin at zero, then supplied arguments overwrite them. */
    {
        ZMachine vm;
        uint16_t args[2] = {0x1111U, 0x2222U};
        ZMachineFrame *frame;
        init_vm(&vm, 5U, 1024U);
        vm.memory[0x100] = 3U;
        assert(zmachine_call_routine(&vm, 0x40U, args, 2U, 0x33U, 0x10U, 0) == TCL_OK);
        assert(vm.pc == 0x101U && vm.frame_count == 1U);
        frame = zmachine_current_frame(&vm);
        assert(frame && frame->locals[0] == 0x1111U && frame->locals[1] == 0x2222U && frame->locals[2] == 0U);
        assert(frame->argument_mask == 0x03U);
        assert(zmachine_return(&vm, 0xBEEFU) == TCL_OK);
        assert(vm.frame_count == 0U && vm.pc == 0x33U && read_global(&vm, 0x10U) == 0xBEEFU);
        free_vm(&vm);
    }

    /* V1-V4 routine headers contain default local words before the first opcode. */
    {
        ZMachine vm;
        uint16_t args[1] = {0x9999U};
        ZMachineFrame *frame;
        init_vm(&vm, 3U, 1024U);
        vm.memory[0x80] = 2U;
        vm.memory[0x81] = 0x12U; vm.memory[0x82] = 0x34U;
        vm.memory[0x83] = 0x56U; vm.memory[0x84] = 0x78U;
        assert(zmachine_call_routine(&vm, 0x40U, args, 1U, 0x22U, 0U, 1) == TCL_OK);
        assert(vm.pc == 0x85U);
        frame = zmachine_current_frame(&vm);
        assert(frame && frame->locals[0] == 0x9999U && frame->locals[1] == 0x5678U);
        free_vm(&vm);
    }

    /* Variable operands are resolved left-to-right, so two variable-0 reads pop twice. */
    {
        ZMachine vm;
        ZMachineInstruction insn;
        uint16_t values[2];
        init_vm(&vm, 5U, 512U);
        assert(zmachine_stack_push(&vm, 0x1111U) == TCL_OK);
        assert(zmachine_stack_push(&vm, 0x2222U) == TCL_OK);
        memset(&insn, 0, sizeof(insn));
        insn.operand_count_actual = 2U;
        insn.operands[0].type = ZM_OPERAND_VARIABLE; insn.operands[0].value = 0U;
        insn.operands[1].type = ZM_OPERAND_VARIABLE; insn.operands[1].value = 0U;
        assert(zmachine_resolve_operands(&vm, &insn, values, 2U) == TCL_OK);
        assert(values[0] == 0x2222U && values[1] == 0x1111U && vm.sp == 0U);
        free_vm(&vm);
    }

    {
        ZMachine vm;
        ZMachineInstruction insn;
        uint16_t values[1];

        init_vm(&vm, 5U, 512U);
        assert(zmachine_stack_push(&vm, 0x1111U) == TCL_OK);
        assert(zmachine_stack_push(&vm, 0U) == TCL_OK);

        /*
         * Even for a variable-by-reference opcode, a Variable-type operand is
         * first evaluated by value. Here stack variable 0 supplies target
         * variable number 0, so operand resolution pops only that target number.
         */
        memset(&insn, 0, sizeof(insn));
        insn.operand_count = ZM_OPERANDS_1OP;
        insn.opcode_number = 14U;
        insn.operand_count_actual = 1U;
        insn.operands[0].type = ZM_OPERAND_VARIABLE;
        insn.operands[0].value = 0U;
        assert(zmachine_resolve_operands(&vm, &insn, values, 1U) == TCL_OK);
        assert(values[0] == 0U);
        assert(vm.sp == 1U && vm.stack[0] == 0x1111U);

        free_vm(&vm);
    }

    /* Exercise indirect variable-number opcodes with both constant and computed target 0. */
    {
        ZMachine vm;
        init_vm(&vm, 5U, 512U);

        /* A small-constant 0 directly names stack variable 0. */
        assert(zmachine_stack_push(&vm, 0x1111U) == TCL_OK);
        vm.pc = 0x20U;
        vm.memory[0x20] = 0x95U; /* 1OP:5 inc, small-constant operand */
        vm.memory[0x21] = 0U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x22U && vm.sp == 1U && vm.stack[0] == 0x1112U);

        /*
         * A Variable-type operand 0 is evaluated first. Its popped value is the
         * target variable number, after which target variable 0 is still
         * modified in place rather than popped or pushed.
         */
        assert(zmachine_stack_push(&vm, 0U) == TCL_OK);
        vm.memory[0x22] = 0xA5U; /* 1OP:5 inc, Variable-type operand */
        vm.memory[0x23] = 0U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x24U && vm.sp == 1U && vm.stack[0] == 0x1113U);

        /* load with a direct target 0 peeks stack variable 0. */
        vm.memory[0x24] = 0x9EU; /* 1OP:14 load, small constant */
        vm.memory[0x25] = 0U;
        vm.memory[0x26] = 0x10U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x27U && vm.sp == 1U && vm.stack[0] == 0x1113U);
        assert(read_global(&vm, 0x10U) == 0x1113U);

        /* store with a direct target 0 replaces stack variable 0 in place. */
        vm.memory[0x27] = 0x0DU; /* 2OP:13 store, small/small */
        vm.memory[0x28] = 0U;
        vm.memory[0x29] = 0x2AU;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x2AU && vm.sp == 1U && vm.stack[0] == 0x002AU);

        /*
         * VAR:9 pull with a Variable-type stack operand first pops target
         * variable number 0, then pulls the value, then replaces the new top.
         */
        vm.stack[0] = 0x1111U;
        assert(zmachine_stack_push(&vm, 0x2222U) == TCL_OK);
        assert(zmachine_stack_push(&vm, 0U) == TCL_OK);
        vm.memory[0x2A] = 0xE9U;
        vm.memory[0x2B] = 0xBFU;
        vm.memory[0x2C] = 0U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x2DU && vm.sp == 1U && vm.stack[0] == 0x2222U);

        free_vm(&vm);
    }

    /* call_1s enters a routine, rtrue returns to the store continuation. */
    {
        ZMachine vm;
        init_vm(&vm, 5U, 1024U);
        vm.pc = 0x20U;
        vm.memory[0x20] = 0x88U; vm.memory[0x21] = 0x00U; vm.memory[0x22] = 0x40U; vm.memory[0x23] = 0x10U;
        vm.memory[0x100] = 0U; vm.memory[0x101] = 0xB0U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x101U && vm.frame_count == 1U);
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x24U && vm.frame_count == 0U && read_global(&vm, 0x10U) == 1U);
        free_vm(&vm);
    }

    /* Calling packed address zero stores false immediately without creating a frame. */
    {
        ZMachine vm;
        init_vm(&vm, 5U, 512U);
        vm.pc = 0x20U;
        vm.memory[0x20] = 0x88U; vm.memory[0x21] = 0x00U; vm.memory[0x22] = 0x00U; vm.memory[0x23] = 0x10U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x24U && vm.frame_count == 0U && read_global(&vm, 0x10U) == 0U);
        free_vm(&vm);
    }

    /* Basic signed/value-domain arithmetic store. */
    {
        ZMachine vm;
        init_vm(&vm, 5U, 512U);
        vm.pc = 0x20U;
        vm.memory[0x20] = 0x14U; vm.memory[0x21] = 5U; vm.memory[0x22] = 7U; vm.memory[0x23] = 0x10U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x24U && read_global(&vm, 0x10U) == 12U);
        free_vm(&vm);
    }

    /* jz true takes the encoded short branch. */
    {
        ZMachine vm;
        init_vm(&vm, 5U, 512U);
        vm.pc = 0x20U;
        vm.memory[0x20] = 0x90U; vm.memory[0x21] = 0U; vm.memory[0x22] = 0xC3U;
        vm.memory[0x23] = 0xB4U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x24U);
        free_vm(&vm);
    }

    /* jz false falls through immediately after the branch record. */
    {
        ZMachine vm;
        init_vm(&vm, 5U, 512U);
        vm.pc = 0x20U;
        vm.memory[0x20] = 0x90U; vm.memory[0x21] = 1U; vm.memory[0x22] = 0xC3U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x23U);
        free_vm(&vm);
    }

    /* Indirect global inc followed by load stores the incremented value. */
    {
        ZMachine vm;
        init_vm(&vm, 5U, 512U);
        write_global(&vm, 0x10U, 9U);
        vm.pc = 0x20U;
        vm.memory[0x20] = 0x95U; vm.memory[0x21] = 0x10U;
        vm.memory[0x22] = 0x9EU; vm.memory[0x23] = 0x10U; vm.memory[0x24] = 0x11U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(read_global(&vm, 0x10U) == 10U && vm.pc == 0x22U);
        assert(zmachine_step(&vm) == TCL_OK);
        assert(read_global(&vm, 0x11U) == 10U && vm.pc == 0x25U);
        free_vm(&vm);
    }

    /* 2OP store treats operand zero as the resolved target variable number. */
    {
        ZMachine vm;
        init_vm(&vm, 5U, 512U);
        vm.pc = 0x20U;
        vm.memory[0x20] = 0x0DU; vm.memory[0x21] = 0x10U; vm.memory[0x22] = 0x2AU;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(read_global(&vm, 0x10U) == 0x2AU && vm.pc == 0x23U);
        free_vm(&vm);
    }

    /* VARIABLE encoding of logical 2OP div/mod preserves signed Z-machine semantics. */
    {
        ZMachine vm;
        init_vm(&vm, 5U, 512U);
        vm.pc = 0x20U;
        /* variable-form div, both operands large constants: -7 / 3 */
        vm.memory[0x20] = 0xD7U; vm.memory[0x21] = 0x0FU;
        vm.memory[0x22] = 0xFFU; vm.memory[0x23] = 0xF9U;
        vm.memory[0x24] = 0x00U; vm.memory[0x25] = 0x03U;
        vm.memory[0x26] = 0x10U;
        /* variable-form mod, both operands large constants: -7 % 3 */
        vm.memory[0x27] = 0xD8U; vm.memory[0x28] = 0x0FU;
        vm.memory[0x29] = 0xFFU; vm.memory[0x2A] = 0xF9U;
        vm.memory[0x2B] = 0x00U; vm.memory[0x2C] = 0x03U;
        vm.memory[0x2D] = 0x11U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x27U && (int16_t)read_global(&vm, 0x10U) == -2);
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x2EU && (int16_t)read_global(&vm, 0x11U) == -1);
        free_vm(&vm);
    }

    /* VAR:1 storew writes a big-endian word at base + 2*index. */
    {
        ZMachine vm;
        init_vm(&vm, 5U, 512U);
        vm.pc = 0x20U;
        vm.memory[0x20] = 0xE1U; vm.memory[0x21] = 0x53U;
        vm.memory[0x22] = 0x80U; vm.memory[0x23] = 2U; vm.memory[0x24] = 0x12U; vm.memory[0x25] = 0x34U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.memory[0x84] == 0x12U && vm.memory[0x85] == 0x34U);
        free_vm(&vm);
    }

    puts("vm execution tests passed");
    return 0;
}
