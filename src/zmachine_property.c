/*
 * zmachine_property.c
 *
 * Backward decoding helpers for Z-machine property data addresses.
 *
 * The get_prop_len opcode receives the address of the first byte of property
 * data, not the property's object number or property number.  The property
 * header format is therefore intentionally designed so the length can be
 * reconstructed by examining the byte immediately before the data.
 */

#include "tclzmachine.h"
#include "zmachine_property.h"

#include <stddef.h>

/*
 * Decode the property-data length from a data address.
 *
 * V1-V3:
 *   One header byte precedes the data.  Bits 5-7 store length minus one.
 *
 * V4+:
 *   One-byte headers describe lengths 1 or 2.  Two-byte headers are detected
 *   because bit 7 of the second size byte (the byte immediately before the
 *   data) is always set.  Its low six bits contain the length, where zero
 *   means 64 bytes.
 */
int
zmachine_property_length_from_address(const ZMachine *vm,
                                      uint16_t property_address,
                                      uint16_t *length)
{
    uint8_t size_byte;

    if (!vm || !vm->memory || !length)
        return TCL_ERROR;

    /* The standard explicitly requires get_prop_len 0 to return zero. */
    if (property_address == 0U) {
        *length = 0U;
        return TCL_OK;
    }

    if ((size_t)property_address >= vm->memory_size)
        return TCL_ERROR;

    size_byte = vm->memory[property_address - 1U];

    if (vm->version <= 3U) {
        *length = (uint16_t)((size_byte >> 5) + 1U);
        return TCL_OK;
    }

    if ((size_byte & 0x80U) != 0U) {
        *length = (uint16_t)(size_byte & 0x3fU);
        if (*length == 0U)
            *length = 64U;
        return TCL_OK;
    }

    *length = (size_byte & 0x40U) ? 2U : 1U;
    return TCL_OK;
}
