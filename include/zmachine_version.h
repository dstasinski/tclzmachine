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

typedef enum ZMachineAddressKind {
    ZM_ADDR_ROUTINE = 0,
    ZM_ADDR_STRING = 1
} ZMachineAddressKind;

int zmachine_version_supported(uint8_t version);
const char *zmachine_supported_versions(void);

/* Convert the header word at 0x1a into a byte length. A zero header word
 * means "unspecified" and returns 0. */
size_t zmachine_header_file_length(uint8_t version, uint16_t header_length_word);

/* Convert a packed routine or string address into a byte address.
 * routine_offset and string_offset are the V6/V7 header words at 0x28/0x2a.
 * Returns 0 for unsupported versions. */
uint32_t zmachine_unpack_address(uint8_t version,
                                 ZMachineAddressKind kind,
                                 uint16_t packed,
                                 uint16_t routine_offset,
                                 uint16_t string_offset);

#ifdef __cplusplus
}
#endif

#endif
