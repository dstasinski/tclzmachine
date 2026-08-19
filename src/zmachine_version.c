#include "zmachine_version.h"

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

const char *zmachine_supported_versions(void)
{
    return ZM_SUPPORTED_VERSIONS;
}

size_t zmachine_header_file_length(uint8_t version, uint16_t header_length_word)
{
    size_t scale;

    if (header_length_word == 0) {
        return 0;
    }

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
        if (kind == ZM_ADDR_ROUTINE) {
            return base * 4U + (uint32_t)routine_offset * 8U;
        }
        return base * 4U + (uint32_t)string_offset * 8U;
    case 8:
        return base * 8U;
    default:
        return 0;
    }
}
