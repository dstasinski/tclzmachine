/*
 * zmachine_property.h
 *
 * Helpers for Z-machine operations which begin with a property-data address
 * rather than an object/property-number pair.  These helpers are kept
 * separate from the object-table traversal API because opcodes such as
 * get_prop_len receive only the address returned by get_prop_addr.
 */

#ifndef ZMACHINE_PROPERTY_H
#define ZMACHINE_PROPERTY_H

#include <stdint.h>
#include "zmachine_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Determine the length, in bytes, of a property from the address of its first
 * data byte.
 *
 * The Z-machine deliberately makes property headers backward-decodable so an
 * interpreter can implement get_prop_len without knowing the owning object.
 * A property address of zero is a special, legal case and returns length zero;
 * several original Infocom story files depend on that behavior.
 */
int zmachine_property_length_from_address(const ZMachine *vm,
                                          uint16_t property_address,
                                          uint16_t *length);

#ifdef __cplusplus
}
#endif

#endif /* ZMACHINE_PROPERTY_H */
