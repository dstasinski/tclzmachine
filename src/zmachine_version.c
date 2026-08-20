/*
 * zmachine_version.c
 *
 * Centralized Z-machine version policy and byte-address conversion rules.
 * Keeping these calculations here prevents version-specific scaling logic from
 * being duplicated throughout the loader, text decoder, and routine caller.
 */

#include "zmachine_version.h"

/* Return nonzero only for the text-oriented versions tclzmachine supports. */
int zmachine_version_supported(uint8_t version)
{
    switch (version) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 7:
    case 8:
        return 1;
    case 6:
    default:
        return 0;
    }
}

/* Expose one canonical supported-version string to Tcl and diagnostics. */
const char *zmachine_supported_versions(void)
{
    return ZM_SUPPORTED_VERSIONS;
}

/* Convert the scaled header file-length word into a physical byte count. */
size_t zmachine_header_file_length(uint8_t version,
                                   uint16_t header_length_word)
{
    size_t scale;

    /* A zero word means the story did not provide a declared length. */
    if (header_length_word == 0)
        return 0;

    switch (version) {
    case 1:
    case 2:
    case 3:
        scale = 2U;
        break;
    case 4:
    case 5:
        scale = 4U;
        break;
    case 7:
    case 8:
        scale = 8U;
        break;
    default:
        return 0;
    }

    return (size_t)header_length_word * scale;
}

/* Convert a version-dependent packed routine/string address to a byte address. */
uint32_t zmachine_unpack_address(uint8_t version,
                                 ZMachineAddressKind kind,
                                 uint16_t packed,
                                 uint16_t routine_offset,
                                 uint16_t string_offset)
{
    uint32_t base = (uint32_t)packed;

    switch (version) {
    case 1:
    case 2:
    case 3:
        return base * 2U;

    case 4:
    case 5:
        return base * 4U;

    case 7:
        /*
         * V7 is unique: packed addresses are four-byte scaled, then receive an
         * additional eight-byte-scaled base selected by address kind.
         */
        if (kind == ZM_ADDR_ROUTINE)
            return base * 4U + (uint32_t)routine_offset * 8U;
        return base * 4U + (uint32_t)string_offset * 8U;

    case 8:
        return base * 8U;

    default:
        return 0;
    }
}
