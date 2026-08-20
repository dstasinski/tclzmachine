/*
 * zmachine_object.h
 *
 * Version-aware access to the Z-machine object tree, attributes, and property
 * tables.
 *
 * V1-V3 object entries are 9 bytes with 32 attributes and one-byte relation
 * fields.  V4+ entries are 14 bytes with 48 attributes and two-byte relation
 * fields.  Property header formats also differ between those families.  This
 * module hides those layout differences from opcode execution.
 */

#ifndef ZMACHINE_OBJECT_H
#define ZMACHINE_OBJECT_H

#include <stdint.h>
#include "zmachine_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decoded metadata for one property table entry.
 *
 * number is the property number. length is the property data size in bytes.
 * data_address points at the first property data byte. next_header_address is
 * the address where parsing should continue to reach the next property.
 * number == 0 represents the terminating property marker / not-found result.
 */
typedef struct ZMachinePropertyInfo {
    uint16_t number;
    uint16_t length;
    uint32_t data_address;
    uint32_t next_header_address;
} ZMachinePropertyInfo;

/* Read the three object-tree relation fields. */
int zmachine_object_get_parent(const ZMachine *vm,
                               uint16_t object,
                               uint16_t *parent);
int zmachine_object_get_sibling(const ZMachine *vm,
                                uint16_t object,
                                uint16_t *sibling);
int zmachine_object_get_child(const ZMachine *vm,
                              uint16_t object,
                              uint16_t *child);

/* Write relation fields using the correct V1-V3 or V4+ field width. */
int zmachine_object_set_parent(ZMachine *vm,
                               uint16_t object,
                               uint16_t parent);
int zmachine_object_set_sibling(ZMachine *vm,
                                uint16_t object,
                                uint16_t sibling);
int zmachine_object_set_child(ZMachine *vm,
                              uint16_t object,
                              uint16_t child);

/* Test or modify one object attribute bit (0..31 in V1-V3, 0..47 in V4+). */
int zmachine_object_test_attr(const ZMachine *vm,
                              uint16_t object,
                              uint8_t attribute,
                              int *is_set);
int zmachine_object_set_attr(ZMachine *vm,
                             uint16_t object,
                             uint8_t attribute,
                             int is_set);

/*
 * Remove an object from its current parent/sibling chain, or insert it as the
 * new first child of destination.  insert automatically unlinks the object
 * from any previous parent before attaching it to destination.
 */
int zmachine_object_remove(ZMachine *vm, uint16_t object);
int zmachine_object_insert(ZMachine *vm,
                           uint16_t object,
                           uint16_t destination);

/* Return the absolute address of an object's property table. */
int zmachine_object_property_table(const ZMachine *vm,
                                   uint16_t object,
                                   uint32_t *address);

/*
 * Find one property in an object's descending-number property list.
 * A successful search for a missing property returns TCL_OK with info->number
 * set to zero; malformed tables return TCL_ERROR.
 */
int zmachine_object_find_property(const ZMachine *vm,
                                  uint16_t object,
                                  uint16_t property,
                                  ZMachinePropertyInfo *info);

/*
 * Read a one- or two-byte object property.  If the object does not define the
 * property, the standard default-property word from the object table is used.
 */
int zmachine_object_get_prop(const ZMachine *vm,
                             uint16_t object,
                             uint16_t property,
                             uint16_t *value);

/* Write an existing one- or two-byte property in dynamic story memory. */
int zmachine_object_put_prop(ZMachine *vm,
                             uint16_t object,
                             uint16_t property,
                             uint16_t value);

/* Return the property's data address, or zero if the property is absent. */
int zmachine_object_get_prop_addr(const ZMachine *vm,
                                  uint16_t object,
                                  uint16_t property,
                                  uint16_t *address);

/*
 * Return the next property number in the object's descending property list.
 * Passing property == 0 requests the first property.
 */
int zmachine_object_get_next_prop(const ZMachine *vm,
                                  uint16_t object,
                                  uint16_t property,
                                  uint16_t *next_property);

#ifdef __cplusplus
}
#endif

#endif
