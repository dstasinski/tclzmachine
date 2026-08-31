/*
 * zmachine_text.h
 *
 * Z-character/ZSCII decoding and canonical UTF-8 text output.
 *
 * This module translates packed Z-machine text into the session's canonical
 * output buffer. It understands version-specific alphabets, abbreviations,
 * 10-bit ZSCII escapes, V5+ custom alphabet tables, and the standard/default
 * or story-provided Unicode translation table for ZSCII 155..251.
 * Presentation policy such as IRC word wrapping is intentionally handled after
 * this layer so the VM's own output remains faithful to the story.
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
 * its Unicode/UTF-8 representation to vm->output. If next_address is non-NULL,
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
 * Decode and append the short name from an object's property table. Object
 * short names use ordinary Z-encoded text preceded by a word-count byte.
 */
int zmachine_text_print_object_name(ZMachine *vm, uint16_t object);

/*
 * Emit one output ZSCII character.
 *
 * Stream 1 receives UTF-8: carriage return becomes '\n', ASCII maps directly,
 * and ZSCII 155..251 is translated through the standard default Unicode table
 * or a V5+ story-defined table selected by header-extension word 3. Undefined
 * or unusable extra characters are rendered safely as '?'. While output stream
 * 3 is active, the original validated ZSCII byte is stored in story memory
 * instead of its UTF-8 representation.
 */
int zmachine_text_output_zscii(ZMachine *vm, uint16_t zscii);

/*
 * Emit one BMP Unicode character for EXT:11 print_unicode.
 *
 * Printable Unicode scalar values are encoded as UTF-8 for stream 1. When
 * stream 3 is active the character is converted back to the story's selected
 * ZSCII mapping when possible, otherwise '?' is stored as required by the
 * output-stream rules. Control values and UTF-16 surrogate code units are
 * rejected as invalid Z-machine Unicode output.
 */
int zmachine_text_output_unicode(ZMachine *vm, uint16_t codepoint);

/*
 * Return the EXT:12 check_unicode capability bits for one BMP code point.
 * Bit 0 means this UTF-8 Tcl frontend can print the character. Bit 1 is set
 * only for printable ASCII because the current cooperative input path stores
 * Tcl input as ZSCII bytes rather than decoding arbitrary Unicode keyboard
 * characters. Bits 2..15 are always zero.
 */
uint16_t zmachine_text_unicode_capabilities(uint16_t codepoint);

#ifdef __cplusplus
}
#endif

#endif
