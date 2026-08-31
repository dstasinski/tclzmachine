/*
 * zmachine_input.h
 *
 * Line-input and dictionary-tokenization support for the text-only
 * Z-machine runtime.
 *
 * The cooperative run loop owns input suspension. Once Tcl supplies a line,
 * this module writes that line into the story's text buffer using the layout
 * required by the active Z-machine version and, when requested by the story,
 * tokenizes it into the parse buffer using either the story dictionary or a
 * caller-supplied dictionary.
 */

#ifndef ZMACHINE_INPUT_H
#define ZMACHINE_INPUT_H

#include "zmachine_state.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Consume the queued line in vm->pending_input and satisfy one read opcode.
 *
 * text_buffer is the story address of the Z-machine text buffer. Versions
 * 1-4 store input beginning at byte 1 and terminate it with zero; Versions 5+
 * store the character count in byte 1 and input beginning at byte 2.
 *
 * parse_buffer may be zero. When nonzero, the function tokenizes the input
 * according to the story dictionary and fills standard four-byte parse-table
 * entries containing dictionary address, token length, and token position.
 *
 * terminator receives ZSCII 13 for the line-ending key when non-NULL. The
 * queued Tcl input is consumed only after the story buffers have been updated
 * successfully.
 */
int zmachine_input_read_line(ZMachine *vm,
                             uint16_t text_buffer,
                             uint16_t parse_buffer,
                             uint16_t *terminator);

/*
 * Perform the Version 5+ `tokenise` opcode on an existing story text buffer.
 *
 * dictionary is zero to use the main dictionary from the story header, or the
 * address of a user dictionary with the same layout. If
 * preserve_unrecognized is nonzero, an unrecognized token still contributes
 * to the parse-buffer token count but its four-byte parse slot is left
 * untouched, as required by the opcode's flag operand.
 *
 * This operation does not consume Tcl-side pending input; it analyzes text
 * which is already present in Z-machine memory.
 */
int zmachine_input_tokenize_buffer(ZMachine *vm,
                                   uint16_t text_buffer,
                                   uint16_t parse_buffer,
                                   uint16_t dictionary,
                                   int preserve_unrecognized);

#ifdef __cplusplus
}
#endif

#endif
