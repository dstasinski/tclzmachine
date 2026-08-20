/*
 * zmachine_version.h
 *
 * Version-policy and address-layout helpers for tclzmachine.
 *
 * The runtime intentionally supports Z-machine Versions 1-5, 7, and 8 while
 * rejecting Version 6 because its presentation model is outside this
 * text-only IRC-oriented implementation.  This module centralizes the
 * version-dependent file-length and packed-address rules so those differences
 * do not become scattered throughout the interpreter.
 */

#ifndef ZMACHINE_VERSION_H
#define ZMACHINE_VERSION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZM_VERSION_MIN 1
#define ZM_VERSION_MAX 8
#define ZM_SUPPORTED_VERSIONS "1,2,3,4,5,7,8"

/* Packed routine and string addresses differ only in the V7 offset applied. */
typedef enum ZMachineAddressKind {
    ZM_ADDR_ROUTINE = 0,
    ZM_ADDR_STRING = 1
} ZMachineAddressKind;

/* Return nonzero only for versions intentionally supported by this runtime. */
int zmachine_version_supported(uint8_t version);

/* Return the stable human-readable supported-version list used by Tcl info. */
const char *zmachine_supported_versions(void);

/*
 * Convert header word 0x1A into a story-file byte length.
 *
 * The stored word is scaled by 2 in V1-V3, 4 in V4-V5, and 8 in V6-V8.  Since
 * V6 is unsupported here, the helper accepts only supported versions.  A zero
 * header word means that the story did not declare a file length and returns 0.
 */
size_t zmachine_header_file_length(uint8_t version,
                                   uint16_t header_length_word);

/*
 * Convert a packed routine or string address into an absolute byte address.
 *
 * V1-V3 multiply by 2, V4-V5 multiply by 4, V8 multiplies by 8, and V7 uses
 * multiply-by-4 plus the corresponding header offset (routine at 0x28, string
 * at 0x2A) multiplied by 8.  Returns 0 for unsupported versions.
 */
uint32_t zmachine_unpack_address(uint8_t version,
                                 ZMachineAddressKind kind,
                                 uint16_t packed,
                                 uint16_t routine_offset,
                                 uint16_t string_offset);

#ifdef __cplusplus
}
#endif

#endif
