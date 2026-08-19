#ifndef ZMACHINE_TEXT_H
#define ZMACHINE_TEXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZMachine ZMachine;

/* Decode and append a Z-encoded string beginning at a byte address. */
int zmachine_text_print(ZMachine *vm, uint32_t address, uint32_t *next_address);

/* Decode and append a Z-encoded string at a packed string address. */
int zmachine_text_print_packed(ZMachine *vm, uint16_t packed_address);

/* Decode and append an object's short name. */
int zmachine_text_print_object_name(ZMachine *vm, uint16_t object);

/* Append one output ZSCII character to the VM's UTF-8 output buffer. */
int zmachine_text_output_zscii(ZMachine *vm, uint16_t zscii);

#ifdef __cplusplus
}
#endif

#endif
