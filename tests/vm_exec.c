#include "tclzmachine.h"
#include "zmachine_exec.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void write_global(ZMachine *vm, uint8_t variable, uint16_t value)
{
    size_t address = (size_t)vm->globals_addr +
                     (size_t)(variable - 0x10U) * 2U;
    vm->memory[address] = (uint8_t)(value >> 8);
    vm->memory[address + 1U] = (uint8_t)value;
}

int main(void)
{
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
        uint16_t values[2];

        init_vm(&vm, 5U, 512U);
        assert(zmachine_stack_push(&vm, 0x1111U) == TCL_OK);
        assert(zmachine_stack_push(&vm, 0x2222U) == TCL_OK);

        /* 1OP load: operand zero is the variable number, not its value. */
        memset(&insn, 0, sizeof(insn));
        insn.operand_count = ZM_OPERANDS_1OP;
        insn.opcode_number = 14U;
        insn.operand_count_actual = 1U;
        insn.operands[0].type = ZM_OPERAND_VARIABLE;
        insn.operands[0].value = 0U;
        assert(zmachine_resolve_operands(&vm, &insn, values, 2U) == TCL_OK);
        assert(values[0] == 0U && vm.sp == 2U);

        /* 2OP dec_chk: later operands still resolve normally left-to-right. */
        memset(&insn, 0, sizeof(insn));
        insn.operand_count = ZM_OPERANDS_2OP;
        insn.opcode_number = 4U;
        insn.operand_count_actual = 2U;
        insn.operands[0].type = ZM_OPERAND_VARIABLE;
        insn.operands[0].value = 0U;
        insn.operands[1].type = ZM_OPERAND_SMALL_CONSTANT;
        insn.operands[1].value = 7U;
        assert(zmachine_resolve_operands(&vm, &insn, values, 2U) == TCL_OK);
        assert(values[0] == 0U && values[1] == 7U && vm.sp == 2U);

        /* VAR:9 pull in V1-V5 has the same indirect operand-zero rule. */
        memset(&insn, 0, sizeof(insn));
        insn.form = ZM_FORM_VARIABLE;
        insn.operand_count = ZM_OPERANDS_VAR;
        insn.opcode_number = 9U;
        insn.operand_count_actual = 1U;
        insn.operands[0].type = ZM_OPERAND_VARIABLE;
        insn.operands[0].value = 0U;
        assert(zmachine_resolve_operands(&vm, &insn, values, 2U) == TCL_OK);
        assert(values[0] == 0U && vm.sp == 2U);

        free_vm(&vm);
    }

    {
        ZMachine vm;
        init_vm(&vm, 5U, 512U);

        /* inc variable 0 must replace the top stack value without popping it. */
        assert(zmachine_stack_push(&vm, 0x1111U) == TCL_OK);
        vm.pc = 0x20U;
        vm.memory[0x20] = 0xA5U; /* 1OP:5 inc, variable operand */
        vm.memory[0x21] = 0U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x22U && vm.sp == 1U && vm.stack[0] == 0x1112U);

        /* load variable 0 peeks the stack and stores the value elsewhere. */
        vm.memory[0x22] = 0xAEU; /* 1OP:14 load, variable operand */
        vm.memory[0x23] = 0U;
        vm.memory[0x24] = 0x10U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x25U && vm.sp == 1U && vm.stack[0] == 0x1112U);
        assert(read_global(&vm, 0x10U) == 0x1112U);

        /* 2OP:13 store indirectly replaces stack variable 0. */
        vm.memory[0x25] = 0x4DU; /* first operand variable, second small */
        vm.memory[0x26] = 0U;
        vm.memory[0x27] = 0x2AU;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x28U && vm.sp == 1U && vm.stack[0] == 0x002AU);

        free_vm(&vm);
    }

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

    {
        ZMachine vm;
        init_vm(&vm, 5U, 512U);
        vm.pc = 0x20U;
        vm.memory[0x20] = 0x88U; vm.memory[0x21] = 0x00U; vm.memory[0x22] = 0x00U; vm.memory[0x23] = 0x10U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x24U && vm.frame_count == 0U && read_global(&vm, 0x10U) == 0U);
        free_vm(&vm);
    }

    {
        ZMachine vm;
        init_vm(&vm, 5U, 512U);
        vm.pc = 0x20U;
        vm.memory[0x20] = 0x14U; vm.memory[0x21] = 5U; vm.memory[0x22] = 7U; vm.memory[0x23] = 0x10U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x24U && read_global(&vm, 0x10U) == 12U);
        free_vm(&vm);
    }

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

    {
        ZMachine vm;
        init_vm(&vm, 5U, 512U);
        vm.pc = 0x20U;
        vm.memory[0x20] = 0x90U; vm.memory[0x21] = 1U; vm.memory[0x22] = 0xC3U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x23U);
        free_vm(&vm);
    }

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

    {
        ZMachine vm;
        init_vm(&vm, 5U, 512U);
        vm.pc = 0x20U;
        vm.memory[0x20] = 0x0DU; vm.memory[0x21] = 0x10U; vm.memory[0x22] = 0x2AU;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(read_global(&vm, 0x10U) == 0x2AU && vm.pc == 0x23U);
        free_vm(&vm);
    }

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
