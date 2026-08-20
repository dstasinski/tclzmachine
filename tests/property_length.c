/*
 * property_length.c
 *
 * Regression coverage for backward property-length decoding and the 1OP
 * get_prop_len instruction.  The zero-address case is included because
 * original Infocom story files rely on the standard-mandated result of zero.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"
#include "zmachine_property.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Initialize a small synthetic VM image for direct helper/opcode tests. */
static void init_vm(ZMachine *vm, uint8_t version)
{
    memset(vm, 0, sizeof(*vm));
    vm->memory_size = 256U;
    vm->memory = (uint8_t *)calloc(vm->memory_size, 1U);
    assert(vm->memory != NULL);
    vm->version = version;
    vm->static_memory_addr = 256U;
    vm->globals_addr = 0x80U;
    vm->state = ZM_STATE_READY;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
}

/* Release storage owned by a synthetic VM. */
static void free_vm(ZMachine *vm)
{
    free(vm->memory);
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
}

/* Read global variable 16 directly from synthetic story memory. */
static uint16_t global16(const ZMachine *vm)
{
    return (uint16_t)(((uint16_t)vm->memory[vm->globals_addr] << 8) |
                      vm->memory[vm->globals_addr + 1U]);
}

int main(void)
{
    uint16_t length;

    /* V3 one-byte property header: bits 5-7 encode length minus one. */
    {
        ZMachine vm;
        init_vm(&vm, 3U);
        vm.memory[0x3fU] = (uint8_t)(3U << 5); /* four-byte property */
        assert(zmachine_property_length_from_address(&vm, 0x40U, &length) == TCL_OK);
        assert(length == 4U);
        free_vm(&vm);
    }

    /* V5 one-byte and two-byte headers must both decode backward. */
    {
        ZMachine vm;
        init_vm(&vm, 5U);

        vm.memory[0x3fU] = 0x40U; /* one-byte header, length two */
        assert(zmachine_property_length_from_address(&vm, 0x40U, &length) == TCL_OK);
        assert(length == 2U);

        vm.memory[0x4fU] = 0x85U; /* second size byte: bit 7 set, length five */
        assert(zmachine_property_length_from_address(&vm, 0x50U, &length) == TCL_OK);
        assert(length == 5U);

        vm.memory[0x5fU] = 0x80U; /* zero length field means 64 bytes */
        assert(zmachine_property_length_from_address(&vm, 0x60U, &length) == TCL_OK);
        assert(length == 64U);
        free_vm(&vm);
    }

    /* The standard explicitly requires get_prop_len 0 to return zero. */
    {
        ZMachine vm;
        init_vm(&vm, 3U);
        assert(zmachine_property_length_from_address(&vm, 0U, &length) == TCL_OK);
        assert(length == 0U);
        free_vm(&vm);
    }

    /* Execute a real 1OP get_prop_len instruction and verify its store result. */
    {
        ZMachine vm;
        init_vm(&vm, 3U);
        vm.pc = 0x20U;
        vm.memory[0x1fU] = (uint8_t)(2U << 5); /* three-byte property */

        /* 0x84 = short-form 1OP opcode 4 with a large-constant operand. */
        vm.memory[0x20U] = 0x84U;
        vm.memory[0x21U] = 0x00U;
        vm.memory[0x22U] = 0x20U;
        vm.memory[0x23U] = 0x10U; /* store in global variable 16 */

        assert(zmachine_step(&vm) == TCL_OK);
        assert(global16(&vm) == 3U);
        assert(vm.pc == 0x24U);
        free_vm(&vm);
    }

    puts("property length tests passed");
    return 0;
}
