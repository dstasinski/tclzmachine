#ifndef ZMACHINE_OBJECT_H
#define ZMACHINE_OBJECT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZMachine ZMachine;

typedef struct ZMachinePropertyInfo {
    uint16_t number;
    uint16_t length;
    uint32_t data_address;
    uint32_t next_header_address;
} ZMachinePropertyInfo;

int zmachine_object_get_parent(const ZMachine *vm, uint16_t object, uint16_t *parent);
int zmachine_object_get_sibling(const ZMachine *vm, uint16_t object, uint16_t *sibling);
int zmachine_object_get_child(const ZMachine *vm, uint16_t object, uint16_t *child);
int zmachine_object_set_parent(ZMachine *vm, uint16_t object, uint16_t parent);
int zmachine_object_set_sibling(ZMachine *vm, uint16_t object, uint16_t sibling);
int zmachine_object_set_child(ZMachine *vm, uint16_t object, uint16_t child);

int zmachine_object_test_attr(const ZMachine *vm, uint16_t object, uint8_t attribute, int *is_set);
int zmachine_object_set_attr(ZMachine *vm, uint16_t object, uint8_t attribute, int is_set);

int zmachine_object_remove(ZMachine *vm, uint16_t object);
int zmachine_object_insert(ZMachine *vm, uint16_t object, uint16_t destination);

int zmachine_object_property_table(const ZMachine *vm, uint16_t object, uint32_t *address);
int zmachine_object_find_property(const ZMachine *vm, uint16_t object, uint16_t property,
                                  ZMachinePropertyInfo *info);
int zmachine_object_get_prop(const ZMachine *vm, uint16_t object, uint16_t property, uint16_t *value);
int zmachine_object_put_prop(ZMachine *vm, uint16_t object, uint16_t property, uint16_t value);
int zmachine_object_get_prop_addr(const ZMachine *vm, uint16_t object, uint16_t property, uint16_t *address);
int zmachine_object_get_next_prop(const ZMachine *vm, uint16_t object, uint16_t property, uint16_t *next_property);

#ifdef __cplusplus
}
#endif

#endif
