/*
 * zmachine_text.h
 *
 * Z-character/ZSCII decoding and canonical UTF-8 text output.
 *
 * This module translates packed Z-machine text into the session's canonical
 * output buffer.  It understands version-specific alphabets, abbreviations,
 * 10-bit ZSCII escapes, and V5+ custom alphabet tables.  Presentation policy
 * such as IRC word wrapping is intentionally handled after this layer so the
 * VM's own output remains faithful to the story.
 */

#ifndef ZMACHINE_TEXT_H
#define ZMACHINE_TEXT_H

#include <stdint.h>
#include "zmachine_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decode a Z-encoded string beginning at an absolute byte address and append
 * its Unicode/UTF-8 representation to vm->output.  If next_address is non-NULL,
 * it receives the first byte after the packed Z-string, which is required for
 * inline print/print_ret instruction execution.
 */
int zmachine_text_print(ZMachine *vm,
                        uint32_t address,
                        uint32_t *next_address);

/*
 * Convert a version-dependent packed string address to a byte address, decode
 * the Z-string there, and append it to the canonical output buffer.
 */
int zmachine_text_print_packed(ZMachine *vm, uint16_t packed_address);

/*
 * Decode and append the short name from an object's property table.  Object
 * short names use ordinary Z-encoded text preceded by a word-count byte.
 */
int zmachine_text_print_object_name(ZMachine *vm, uint16_t object);

/*
 * Append one output ZSCII character/code to vm->output as UTF-8.
 * Carriage return (ZSCII 13) becomes '\n'; printable ASCII is copied directly;
 * supported extended ZSCII values are converted without exposing terminal
 * control sequences to Tcl or IRC callers.
 */
int zmachine_text_output_zscii(ZMachine *vm, uint16_t zscii);

#ifdef __cplusplus
}
#endif

#endif
