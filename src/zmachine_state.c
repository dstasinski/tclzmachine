#include "tclzmachine.h"

#include <stdio.h>
#include <string.h>

static void state_error(ZMachine *vm, const char *message)
{
    if (!vm) {
        return;
    }
    vm->state = ZM_STATE_ERROR;
    snprintf(vm->error, sizeof(vm->error), "%s", message);
}

static int stack_floor(const ZMachine *vm, size_t *floor)
{
    if (!vm || !floor) {
        return TCL_ERROR;
    }

    if (vm->frame_count == 0U) {
        *floor = 0U;
    } else {
        *floor = vm->frames[vm->frame_count - 1U].stack_base;
    }
    return TCL_OK;
}

static int global_address(const ZMachine *vm, uint8_t variable, size_t *address)
{
    size_t offset;

    if (!vm || !address || variable < 0x10U) {
        return TCL_ERROR;
    }

    offset = (size_t)(variable - 0x10U) * 2U;
    *address = (size_t)vm->globals_addr + offset;

    if (*address + 1U >= vm->memory_size) {
        return TCL_ERROR;
    }
    return TCL_OK;
}

int zmachine_stack_push(ZMachine *vm, uint16_t value)
{
    if (!vm) {
        return TCL_ERROR;
    }
    if (vm->sp >= (sizeof(vm->stack) / sizeof(vm->stack[0]))) {
        state_error(vm, "Z-machine evaluation stack overflow");
        return TCL_ERROR;
    }
    vm->stack[vm->sp++] = value;
    return TCL_OK;
}

int zmachine_stack_pop(ZMachine *vm, uint16_t *value)
{
    size_t floor;

    if (!vm || !value) {
        return TCL_ERROR;
    }
    if (stack_floor(vm, &floor) != TCL_OK || vm->sp <= floor) {
        state_error(vm, "Z-machine evaluation stack underflow");
        return TCL_ERROR;
    }
    *value = vm->stack[--vm->sp];
    return TCL_OK;
}

int zmachine_stack_peek(ZMachine *vm, uint16_t *value)
{
    size_t floor;

    if (!vm || !value) {
        return TCL_ERROR;
    }
    if (stack_floor(vm, &floor) != TCL_OK || vm->sp <= floor) {
        state_error(vm, "Z-machine evaluation stack underflow");
        return TCL_ERROR;
    }
    *value = vm->stack[vm->sp - 1U];
    return TCL_OK;
}

int zmachine_stack_replace_top(ZMachine *vm, uint16_t value)
{
    size_t floor;

    if (!vm) {
        return TCL_ERROR;
    }
    if (stack_floor(vm, &floor) != TCL_OK || vm->sp <= floor) {
        state_error(vm, "Z-machine evaluation stack underflow");
        return TCL_ERROR;
    }
    vm->stack[vm->sp - 1U] = value;
    return TCL_OK;
}

ZMachineFrame *zmachine_current_frame(ZMachine *vm)
{
    if (!vm || vm->frame_count == 0U) {
        return NULL;
    }
    return &vm->frames[vm->frame_count - 1U];
}

const ZMachineFrame *zmachine_current_frame_const(const ZMachine *vm)
{
    if (!vm || vm->frame_count == 0U) {
        return NULL;
    }
    return &vm->frames[vm->frame_count - 1U];
}

int zmachine_variable_read(ZMachine *vm,
                           uint8_t variable,
                           int indirect,
                           uint16_t *value)
{
    ZMachineFrame *frame;
    size_t address;

    if (!vm || !value) {
        return TCL_ERROR;
    }

    if (variable == 0U) {
        return indirect ? zmachine_stack_peek(vm, value)
                        : zmachine_stack_pop(vm, value);
    }

    if (variable < 0x10U) {
        frame = zmachine_current_frame(vm);
        if (!frame || variable > frame->local_count) {
            state_error(vm, "reference to nonexistent Z-machine local variable");
            return TCL_ERROR;
        }
        *value = frame->locals[variable - 1U];
        return TCL_OK;
    }

    if (!vm->memory || global_address(vm, variable, &address) != TCL_OK) {
        state_error(vm, "Z-machine global variable address is outside story memory");
        return TCL_ERROR;
    }

    *value = (uint16_t)(((uint16_t)vm->memory[address] << 8) |
                        vm->memory[address + 1U]);
    return TCL_OK;
}

int zmachine_variable_write(ZMachine *vm,
                            uint8_t variable,
                            int indirect,
                            uint16_t value)
{
    ZMachineFrame *frame;
    size_t address;

    if (!vm) {
        return TCL_ERROR;
    }

    if (variable == 0U) {
        return indirect ? zmachine_stack_replace_top(vm, value)
                        : zmachine_stack_push(vm, value);
    }

    if (variable < 0x10U) {
        frame = zmachine_current_frame(vm);
        if (!frame || variable > frame->local_count) {
            state_error(vm, "reference to nonexistent Z-machine local variable");
            return TCL_ERROR;
        }
        frame->locals[variable - 1U] = value;
        return TCL_OK;
    }

    if (!vm->memory || global_address(vm, variable, &address) != TCL_OK ||
        address + 1U >= (size_t)vm->static_memory_addr) {
        state_error(vm, "Z-machine global variable write is outside dynamic memory");
        return TCL_ERROR;
    }

    vm->memory[address] = (uint8_t)(value >> 8);
    vm->memory[address + 1U] = (uint8_t)(value & 0xffU);
    return TCL_OK;
}

int zmachine_frame_push(ZMachine *vm,
                        uint32_t return_pc,
                        uint8_t store_variable,
                        int discard_result,
                        const uint16_t *locals,
                        uint8_t local_count,
                        uint8_t argument_mask)
{
    ZMachineFrame *frame;

    if (!vm) {
        return TCL_ERROR;
    }
    if (local_count > ZM_MAX_LOCALS) {
        state_error(vm, "Z-machine routine has more than 15 locals");
        return TCL_ERROR;
    }
    if (vm->frame_count >= ZM_MAX_FRAMES) {
        state_error(vm, "Z-machine call stack overflow");
        return TCL_ERROR;
    }

    frame = &vm->frames[vm->frame_count++];
    memset(frame, 0, sizeof(*frame));
    frame->return_pc = return_pc;
    frame->stack_base = vm->sp;
    frame->local_count = local_count;
    frame->store_variable = store_variable;
    frame->argument_mask = argument_mask;
    frame->discard_result = discard_result ? 1U : 0U;

    if (locals && local_count > 0U) {
        memcpy(frame->locals, locals, (size_t)local_count * sizeof(uint16_t));
    }
    return TCL_OK;
}

int zmachine_frame_pop(ZMachine *vm, ZMachineFrame *frame)
{
    ZMachineFrame popped;

    if (!vm) {
        return TCL_ERROR;
    }
    if (vm->frame_count == 0U) {
        state_error(vm, "attempt to return with no active Z-machine routine");
        return TCL_ERROR;
    }

    popped = vm->frames[vm->frame_count - 1U];
    vm->sp = popped.stack_base;
    --vm->frame_count;

    if (frame) {
        *frame = popped;
    }
    return TCL_OK;
}
