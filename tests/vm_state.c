/*
 * vm_state.c
 *
 * Unit coverage for the foundational VM state primitives beneath instruction
 * execution. The test locks down ordinary versus indirect variable-0 stack
 * semantics, routine-frame local ownership and stack floors, frame metadata,
 * big-endian global-variable storage across the full global-number range, and
 * rejection of local-variable access when no routine frame exists.
 *
 * These helpers are deliberately tested without decoding opcodes so state bugs
 * can be distinguished from instruction encoding/dispatch failures.
 */

#include "tclzmachine.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Initialize a VM around caller-owned memory; no heap/Tcl-string teardown is needed. */
static void init_vm(ZMachine *vm, uint8_t *memory, size_t memory_size)
{
    memset(vm, 0, sizeof(*vm));
    memset(memory, 0, memory_size);
    vm->memory = memory;
    vm->memory_size = memory_size;
    vm->globals_addr = 0x100;
    vm->static_memory_addr = 0x300;
    vm->state = ZM_STATE_READY;
}

int main(void)
{
    ZMachine vm;
    uint8_t memory[1024];
    uint16_t value;
    uint16_t locals[3] = {0x1111, 0x2222, 0x3333};
    ZMachineFrame popped;

    init_vm(&vm, memory, sizeof(memory));

    /* Variable 0 pushes on ordinary write and pops on ordinary read. */
    assert(zmachine_variable_write(&vm, 0, 0, 0x1234) == TCL_OK);
    assert(vm.sp == 1);
    assert(zmachine_variable_read(&vm, 0, 0, &value) == TCL_OK);
    assert(value == 0x1234);
    assert(vm.sp == 0);

    /* Indirect stack references operate on the top item in place. */
    assert(zmachine_stack_push(&vm, 0xabcd) == TCL_OK);
    assert(zmachine_variable_read(&vm, 0, 1, &value) == TCL_OK);
    assert(value == 0xabcd && vm.sp == 1);
    assert(zmachine_variable_write(&vm, 0, 1, 0xbeef) == TCL_OK);
    assert(zmachine_stack_peek(&vm, &value) == TCL_OK);
    assert(value == 0xbeef && vm.sp == 1);

    /* A routine frame owns locals and establishes a fresh stack floor. */
    assert(zmachine_frame_push(&vm, 0x4242, 0x10, 0,
                               locals, 3, 0x07) == TCL_OK);
    assert(vm.frame_count == 1);
    assert(zmachine_variable_read(&vm, 2, 0, &value) == TCL_OK);
    assert(value == 0x2222);
    assert(zmachine_variable_write(&vm, 3, 0, 0x7777) == TCL_OK);
    assert(zmachine_variable_read(&vm, 3, 0, &value) == TCL_OK);
    assert(value == 0x7777);

    /* Frame pop discards only callee evaluation words and preserves caller stack. */
    assert(zmachine_stack_push(&vm, 0xaaaa) == TCL_OK);
    assert(zmachine_frame_pop(&vm, &popped) == TCL_OK);
    assert(popped.return_pc == 0x4242);
    assert(popped.store_variable == 0x10);
    assert(popped.argument_mask == 0x07);
    assert(vm.frame_count == 0);
    assert(vm.sp == 1); /* caller's pre-call stack item remains */

    /* Globals are 16-bit big-endian words in dynamic memory. */
    assert(zmachine_variable_write(&vm, 0x10, 0, 0x1357) == TCL_OK);
    assert(memory[0x100] == 0x13 && memory[0x101] == 0x57);
    assert(zmachine_variable_read(&vm, 0x10, 0, &value) == TCL_OK);
    assert(value == 0x1357);

    /* Highest legal global variable maps through the same address calculation. */
    assert(zmachine_variable_write(&vm, 0xff, 0, 0x2468) == TCL_OK);
    assert(zmachine_variable_read(&vm, 0xff, 0, &value) == TCL_OK);
    assert(value == 0x2468);

    /* Locals are illegal outside an active routine frame. */
    vm.state = ZM_STATE_READY;
    vm.error[0] = '\0';
    assert(zmachine_variable_read(&vm, 1, 0, &value) == TCL_ERROR);
    assert(vm.state == ZM_STATE_ERROR);

    puts("vm state tests passed");
    return 0;
}
