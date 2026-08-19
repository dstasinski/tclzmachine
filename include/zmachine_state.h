#ifndef ZMACHINE_STATE_H
#define ZMACHINE_STATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZM_MAX_LOCALS 15U
#define ZM_MAX_FRAMES 256U

typedef struct ZMachine ZMachine;

typedef struct ZMachineFrame {
    uint32_t return_pc;
    size_t stack_base;
    uint16_t locals[ZM_MAX_LOCALS];
    uint8_t local_count;
    uint8_t store_variable;
    uint8_t argument_mask;
    uint8_t discard_result;
} ZMachineFrame;

/* Evaluation stack helpers. */
int zmachine_stack_push(ZMachine *vm, uint16_t value);
int zmachine_stack_pop(ZMachine *vm, uint16_t *value);
int zmachine_stack_peek(ZMachine *vm, uint16_t *value);
int zmachine_stack_replace_top(ZMachine *vm, uint16_t value);

/*
 * Read or write a Z-machine variable.
 *
 * Variable 0 is the evaluation-stack pointer: normal reads pop and normal
 * writes push. For the opcodes which take an indirect variable reference,
 * pass indirect != 0 so variable 0 is instead peeked/replaced in place.
 * Variables 1..15 are locals and 16..255 are globals.
 */
int zmachine_variable_read(ZMachine *vm,
                           uint8_t variable,
                           int indirect,
                           uint16_t *value);
int zmachine_variable_write(ZMachine *vm,
                            uint8_t variable,
                            int indirect,
                            uint16_t value);

/* Routine-call frame helpers. The caller supplies already-initialized locals. */
int zmachine_frame_push(ZMachine *vm,
                        uint32_t return_pc,
                        uint8_t store_variable,
                        int discard_result,
                        const uint16_t *locals,
                        uint8_t local_count,
                        uint8_t argument_mask);
int zmachine_frame_pop(ZMachine *vm, ZMachineFrame *frame);
ZMachineFrame *zmachine_current_frame(ZMachine *vm);
const ZMachineFrame *zmachine_current_frame_const(const ZMachine *vm);

#ifdef __cplusplus
}
#endif

#endif
