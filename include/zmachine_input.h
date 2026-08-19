#ifndef ZMACHINE_INPUT_H
#define ZMACHINE_INPUT_H

#include "zmachine_state.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int zmachine_input_read_line(ZMachine *vm,
                             uint16_t text_buffer,
                             uint16_t parse_buffer,
                             uint16_t *terminator);

#ifdef __cplusplus
}
#endif

#endif
