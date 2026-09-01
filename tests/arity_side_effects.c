/*
 * arity_side_effects.c
 *
 * Regression coverage for global pre-resolution opcode validation.
 *
 * A VARIABLE operand naming variable 0 pops the evaluation stack. Malformed
 * but decodable Z-code must therefore be rejected for impossible operand count
 * or an already-invalid literal selector before any ownership layer evaluates
 * another operand. These cases deliberately exercise multiple dispatcher layers
 * through the public zmachine_step() boundary with a stack sentinel.
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

static void expect_preflight_error(uint8_t version,
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

static void expect_ignored_ext29_keeps_stack(uint8_t version, uint16_t sentinel)
{
    ZMachine vm;
    static const uint8_t code[] = {
        0xBEU, 0x1DU, /* EXT:29 -- buffer_screen exists only in V6 */
        0x9FU,       /* variable, small constant, then omitted */
        0x00U,       /* variable 0: must not pop */
        0x01U
    };

    init_vm(&vm, version);
    memcpy(vm.memory + 0x20U, code, sizeof(code));
    vm.pc = 0x20U;
    assert(zmachine_stack_push(&vm, sentinel) == TCL_OK);

    assert(zmachine_step(&vm) == TCL_OK);
    assert(vm.state == ZM_STATE_READY);
    assert(vm.sp == 1U);
    assert(vm.stack[0] == sentinel);
    assert(vm.pc == 0x20U + sizeof(code));

    free_vm(&vm);
}

int main(void)
{
    /* Stream layer: input_stream is exactly one operand. */
    {
        static const uint8_t code[] = {
            0xF4U, 0x9FU, 0x00U, 0x01U
        };
        expect_preflight_error(5U, code, sizeof(code),
                               "input_stream requires exactly one operand",
                               0xB101U);
    }

    /* Stream layer: supported non-V6 output_stream accepts at most table. */
    {
        static const uint8_t code[] = {
            0xF3U, 0x97U, 0x00U, 0x01U, 0x02U
        };
        expect_preflight_error(5U, code, sizeof(code),
                               "output_stream requires one or two operands",
                               0xB202U);
    }

    /* Invalid literal stream numbers fail before a later variable-0 table pops. */
    {
        static const uint8_t code[] = {
            0xF3U, 0x6FU, /* small constant, variable, omitted, omitted */
            0x05U,       /* invalid stream 5 */
            0x00U        /* variable 0: must not pop */
        };
        expect_preflight_error(5U, code, sizeof(code),
                               "unsupported Z-machine output stream number",
                               0xB212U);
    }

    /* Selecting stream 3 without its table is a value-dependent shape error. */
    {
        static const uint8_t code[] = {
            0xF3U, 0x7FU, 0x03U
        };
        expect_preflight_error(5U, code, sizeof(code),
                               "output_stream 3 requires a table operand",
                               0xB222U);
    }

    /* The outer guard catches version-illegal stream opcodes before pop. */
    {
        static const uint8_t code[] = {
            0xF3U, 0xBFU, 0x00U
        };
        expect_preflight_error(2U, code, sizeof(code),
                               "VAR opcode requires V3 or later",
                               0xB303U);
    }

    /* V1-V3 read has exactly text and parse operands. */
    {
        static const uint8_t code[] = {
            0xE4U, 0xBFU, 0x00U
        };
        expect_preflight_error(3U, code, sizeof(code),
                               "V1-V3 read requires exactly two operands",
                               0xB313U);
    }

    /* V4 read requires text+parse, with only the timed pair optional. */
    {
        static const uint8_t code[] = {
            0xE4U, 0xBFU, 0x00U
        };
        expect_preflight_error(4U, code, sizeof(code),
                               "V4 read requires two to four operands",
                               0xB323U);
    }

    /* Lexical layer: tokenise requires text and parse destinations. */
    {
        static const uint8_t code[] = {
            0xFBU, 0xBFU, 0x00U
        };
        expect_preflight_error(5U, code, sizeof(code),
                               "tokenise requires two to four operands",
                               0xB404U);
    }

    /* A literal zero tokenise parse buffer fails before text variable 0 pops. */
    {
        static const uint8_t code[] = {
            0xFBU, 0x8FU, /* variable, large constant, then omitted */
            0x00U,       /* text from variable 0 */
            0x00U, 0x00U /* invalid parse address 0 */
        };
        expect_preflight_error(5U, code, sizeof(code),
                               "nonzero parse buffer",
                               0xB414U);
    }

    /* Lexical layer: encode_text has a fixed four-operand signature. */
    {
        static const uint8_t code[] = {
            0xFCU, 0x97U, 0x00U, 0x01U, 0x02U
        };
        expect_preflight_error(5U, code, sizeof(code),
                               "encode_text requires exactly four operands",
                               0xB505U);
    }

    /* Input/presentation layer: read_char allows device plus timed pair only. */
    {
        static const uint8_t code[] = {
            0xF6U, 0x95U, 0x00U, 0x01U, 0x00U, 0x00U
        };
        expect_preflight_error(5U, code, sizeof(code),
                               "read_char requires one to three operands",
                               0xB606U);
    }

    /* Invalid literal input device fails before a later variable operand pops. */
    {
        static const uint8_t code[] = {
            0xF6U, 0x6FU, /* small constant, variable */
            0x02U,       /* only device 1 is legal */
            0x00U
        };
        expect_preflight_error(5U, code, sizeof(code),
                               "read_char input device must be 1",
                               0xB616U);
    }

    /* mIRC/presentation layer: set_text_style is exactly one operand. */
    {
        static const uint8_t code[] = {
            0xF1U, 0x9FU, 0x00U, 0x01U
        };
        expect_preflight_error(5U, code, sizeof(code),
                               "set_text_style requires exactly one operand",
                               0xB707U);
    }

    /* V4 text-only presentation layer: set_cursor requires line and column. */
    {
        static const uint8_t code[] = {
            0xEFU, 0xBFU, 0x00U
        };
        expect_preflight_error(4U, code, sizeof(code),
                               "set_cursor requires exactly two operands",
                               0xB808U);
    }

    /* V7/V8 follow the V5 screen model, so V6's third window operand is illegal. */
    {
        static const uint8_t code[] = {
            0xDBU, 0x97U, 0x00U, 0x01U, 0x02U
        };
        expect_preflight_error(7U, code, sizeof(code),
                               "2OP instruction requires exactly two operands",
                               0xB818U);
    }

    /* print_table requires an address and width before optional height/skip. */
    {
        static const uint8_t code[] = {
            0xFEU, 0xBFU, 0x00U
        };
        expect_preflight_error(5U, code, sizeof(code),
                               "print_table requires two to four operands",
                               0xB909U);
    }

    /* Variable-form 2OP presentation: set_colour is exactly two operands. */
    {
        static const uint8_t code[] = {
            0xDBU, 0xBFU, 0x00U
        };
        expect_preflight_error(5U, code, sizeof(code),
                               "2OP instruction requires exactly two operands",
                               0xBA0AU);
    }

    /* An invalid literal colour fails before the other variable operand pops. */
    {
        static const uint8_t code[] = {
            0xDBU, 0x6FU, /* small constant, variable */
            0x0AU,       /* colour 10 is V6-only */
            0x00U
        };
        expect_preflight_error(7U, code, sizeof(code),
                               "unsupported set_colour foreground value",
                               0xBA1AU);
    }

    /* Pure core: throw must not pop its value before noticing missing frame. */
    {
        static const uint8_t code[] = {
            0xDCU, 0xBFU, 0x00U
        };
        expect_preflight_error(5U, code, sizeof(code),
                               "2OP instruction requires exactly two operands",
                               0xBB0BU);
    }

    /* Pure core: V5 VAR-form not accepts one operand only. */
    {
        static const uint8_t code[] = {
            0xF8U, 0x9FU, 0x00U, 0x01U
        };
        expect_preflight_error(5U, code, sizeof(code),
                               "not requires exactly one operand",
                               0xBC0CU);
    }

    /* Pure core: both shift opcodes require number and places. */
    {
        static const uint8_t log_shift[] = {
            0xBEU, 0x02U, 0xBFU, 0x00U
        };
        static const uint8_t art_shift[] = {
            0xBEU, 0x03U, 0xBFU, 0x00U
        };
        expect_preflight_error(5U, log_shift, sizeof(log_shift),
                               "shift opcode requires exactly two operands",
                               0xBD0DU);
        expect_preflight_error(5U, art_shift, sizeof(art_shift),
                               "shift opcode requires exactly two operands",
                               0xBE0EU);
    }

    /* Invalid literal shift count rejects before a number in variable 0 pops. */
    {
        static const uint8_t code[] = {
            0xBEU, 0x02U, 0x8FU, /* variable, large constant */
            0x00U,              /* number from variable 0 */
            0x00U, 0x10U        /* invalid +16 places */
        };
        expect_preflight_error(5U, code, sizeof(code),
                               "outside -15..15",
                               0xBE1EU);
    }

    /* mIRC/presentation layer: supported set_true_colour has two operands. */
    {
        static const uint8_t code[] = {
            0xBEU, 0x0DU, 0xBFU, 0x00U
        };
        expect_preflight_error(5U, code, sizeof(code),
                               "set_true_colour requires exactly two operands",
                               0xBF0FU);
    }

    /* V6-only true-colour -3 cannot pop the later variable background in V7. */
    {
        static const uint8_t code[] = {
            0xBEU, 0x0DU, 0x8FU, /* large constant, variable */
            0xFFU, 0xFDU,       /* -3: V6-only colour-under-cursor */
            0x00U
        };
        expect_preflight_error(7U, code, sizeof(code),
                               "unsupported set_true_colour foreground value",
                               0xBF1FU);
    }

    /* Auxiliary EXT save/restore cannot have a partial two-operand signature. */
    {
        static const uint8_t code[] = {
            0xBEU, 0x00U, 0x9FU, 0x00U, 0x01U
        };
        expect_preflight_error(5U, code, sizeof(code),
                               "extended save/restore requires zero, three, or four operands",
                               0xC111U);
    }

    /* EXT:29 is undefined in every supported non-V6 screen model and is ignored. */
    expect_ignored_ext29_keeps_stack(5U, 0xC515U);
    expect_ignored_ext29_keeps_stack(7U, 0xC717U);
    expect_ignored_ext29_keeps_stack(8U, 0xC818U);

    puts("global opcode arity/value preflight side-effect tests passed");
    return 0;
}
