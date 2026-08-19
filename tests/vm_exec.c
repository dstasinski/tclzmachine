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

int main(void)
{
    {
        ZMachine vm;
        uint16_t args[2] = {0x1111U, 0x2222U};
        ZMachineFrame *frame;

        init_vm(&vm, 5U, 1024U);
        vm.memory[0x100] = 3U; /* routine at packed address 0x40 */

        assert(zmachine_call_routine(&vm, 0x40U, args, 2U,
                                     0x33U, 0x10U, 0) == TCL_OK);
        assert(vm.pc == 0x101U);
        assert(vm.frame_count == 1U);
        frame = zmachine_current_frame(&vm);
        assert(frame != NULL);
        assert(frame->local_count == 3U);
        assert(frame->locals[0] == 0x1111U);
        assert(frame->locals[1] == 0x2222U);
        assert(frame->locals[2] == 0U);
        assert(frame->argument_mask == 0x03U);

        assert(zmachine_return(&vm, 0xBEEFU) == TCL_OK);
        assert(vm.frame_count == 0U);
        assert(vm.pc == 0x33U);
        assert(read_global(&vm, 0x10U) == 0xBEEFU);
        free_vm(&vm);
    }

    {
        ZMachine vm;
        uint16_t args[1] = {0x9999U};
        ZMachineFrame *frame;

        init_vm(&vm, 3U, 1024U);
        /* V3 packed 0x40 => byte 0x80. Two locals with defaults. */
        vm.memory[0x80] = 2U;
        vm.memory[0x81] = 0x12U;
        vm.memory[0x82] = 0x34U;
        vm.memory[0x83] = 0x56U;
        vm.memory[0x84] = 0x78U;

        assert(zmachine_call_routine(&vm, 0x40U, args, 1U,
                                     0x22U, 0U, 1) == TCL_OK);
        assert(vm.pc == 0x85U);
        frame = zmachine_current_frame(&vm);
        assert(frame != NULL);
        assert(frame->locals[0] == 0x9999U);
        assert(frame->locals[1] == 0x5678U);
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
        insn.operands[0].type = ZM_OPERAND_VARIABLE;
        insn.operands[0].value = 0U;
        insn.operands[1].type = ZM_OPERAND_VARIABLE;
        insn.operands[1].value = 0U;

        assert(zmachine_resolve_operands(&vm, &insn, values, 2U) == TCL_OK);
        assert(values[0] == 0x2222U);
        assert(values[1] == 0x1111U);
        assert(vm.sp == 0U);
        free_vm(&vm);
    }

    {
        ZMachine vm;

        init_vm(&vm, 5U, 1024U);
        vm.pc = 0x20U;
        /* call_1s packed 0x40 -> global 0x10 */
        vm.memory[0x20] = 0x88U;
        vm.memory[0x21] = 0x00U;
        vm.memory[0x22] = 0x40U;
        vm.memory[0x23] = 0x10U;
        vm.memory[0x100] = 0U;
        vm.memory[0x101] = 0xB0U; /* rtrue */

        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x101U);
        assert(vm.frame_count == 1U);
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x24U);
        assert(vm.frame_count == 0U);
        assert(read_global(&vm, 0x10U) == 1U);
        free_vm(&vm);
    }

    {
        ZMachine vm;

        init_vm(&vm, 5U, 512U);
        vm.pc = 0x20U;
        /* call_1s address zero must immediately store false. */
        vm.memory[0x20] = 0x88U;
        vm.memory[0x21] = 0x00U;
        vm.memory[0x22] = 0x00U;
        vm.memory[0x23] = 0x10U;
        vm.memory[0x40] = 0x12U;
        vm.memory[0x41] = 0x34U;

        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.pc == 0x24U);
        assert(vm.frame_count == 0U);
        assert(read_global(&vm, 0x10U) == 0U);
        free_vm(&vm);
    }

    puts("vm execution tests passed");
    return 0;
}
