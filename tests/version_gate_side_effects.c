/*
 * version_gate_side_effects.c
 *
 * Regression coverage for the rule that opcode version legality is established
 * before operand evaluation. A VARIABLE operand naming variable 0 pops the
 * evaluation stack, so an opcode which is illegal in the current story version
 * must be rejected without evaluating that operand.
 *
 * Versions 7 and 8 use the Version-5 instruction/screen model except for their
 * documented memory/address differences. V6-only presentation/graphics opcodes
 * therefore remain illegal in V7/V8 even though their numeric version is larger.
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

static void expect_version_error(uint8_t version,
                                 const uint8_t *instruction,
                                 size_t instruction_size,
                                 const char *message_fragment,
                                 uint16_t sentinel)
{
    ZMachine vm;

    init_vm(&vm, version);
    assert(instruction_size <= vm.memory_size - 0x20U);
    memcpy(vm.memory + 0x20U, instruction, instruction_size);
    vm.pc = 0x20U;
    assert(zmachine_stack_push(&vm, sentinel) == TCL_OK);

    assert(zmachine_step(&vm) == TCL_ERROR);
    assert(vm.state == ZM_STATE_ERROR);
    assert(vm.sp == 1U);
    assert(vm.stack[0] == sentinel);
    assert(strstr(vm.error, message_fragment) != NULL);

    free_vm(&vm);
}

int main(void)
{
    /* V3 may decode 2OP:25, but call_2s does not exist until V4. */
    {
        static const uint8_t code[] = {
            0xD9U,
            0x9FU,
            0x00U,
            0x01U
        };
        expect_version_error(3U, code, sizeof(code),
                             "call_2s is illegal before V4", 0xA101U);
    }

    /* call_2n is V5+, so a V4 story must fail before resolving variable 0. */
    {
        static const uint8_t code[] = {
            0xDAU,
            0x9FU,
            0x00U,
            0x01U
        };
        expect_version_error(4U, code, sizeof(code),
                             "2OP opcode requires V5 or later", 0xA202U);
    }

    /* set_colour is also V5+, including variable-form 2OP encoding. */
    {
        static const uint8_t code[] = {
            0xDBU,
            0x9FU,
            0x00U,
            0x01U
        };
        expect_version_error(4U, code, sizeof(code),
                             "2OP opcode requires V5 or later", 0xA303U);
    }

    /* Short-form 1OP:8 call_1s begins in V4. */
    {
        static const uint8_t code[] = {
            0xA8U,
            0x00U
        };
        expect_version_error(3U, code, sizeof(code),
                             "call_1s is illegal before V4", 0xA404U);
    }

    /* call_vs2 has two operand-type bytes and begins in V4. */
    {
        static const uint8_t code[] = {
            0xECU,
            0xBFU,
            0xFFU,
            0x00U
        };
        expect_version_error(3U, code, sizeof(code),
                             "VAR opcode requires V4 or later", 0xA505U);
    }

    /* call_vn and call_vn2 are V5 additions. */
    {
        static const uint8_t call_vn[] = {
            0xF9U, 0xBFU, 0x00U
        };
        static const uint8_t call_vn2[] = {
            0xFAU, 0xBFU, 0xFFU, 0x00U
        };
        expect_version_error(4U, call_vn, sizeof(call_vn),
                             "VAR opcode requires V5 or later", 0xA606U);
        expect_version_error(4U, call_vn2, sizeof(call_vn2),
                             "VAR opcode requires V5 or later", 0xA707U);
    }

    /* VAR:24 is the V5+ form of not; V1-V4 use 1OP:15 instead. */
    {
        static const uint8_t code[] = {
            0xF8U, 0xBFU, 0x00U
        };
        expect_version_error(4U, code, sizeof(code),
                             "VAR opcode requires V5 or later", 0xA808U);
    }

    /* split_window does not exist before V3. */
    {
        static const uint8_t code[] = {
            0xEAU, 0xBFU, 0x00U
        };
        expect_version_error(2U, code, sizeof(code),
                             "VAR opcode requires V3 or later", 0xA909U);
    }

    /* EXT:16 belongs to the Version-6-only window model. */
    {
        static const uint8_t code[] = {
            0xBEU,
            0x10U,
            0xBFU,
            0x00U
        };
        expect_version_error(5U, code, sizeof(code),
                             "available only in Version 6", 0xAA0AU);
        expect_version_error(7U, code, sizeof(code),
                             "available only in Version 6", 0xAA1AU);
        expect_version_error(8U, code, sizeof(code),
                             "available only in Version 6", 0xAA2AU);
    }

    /* EXT:5 draw_picture likewise must not become legal merely because V7 > V6. */
    {
        static const uint8_t code[] = {
            0xBEU,
            0x05U,
            0xBFU,
            0x00U
        };
        expect_version_error(7U, code, sizeof(code),
                             "available only in Version 6", 0xAB0BU);
        expect_version_error(8U, code, sizeof(code),
                             "available only in Version 6", 0xAB1BU);
    }

    puts("opcode version preflight side-effect tests passed");
    return 0;
}
