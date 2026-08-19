#include "tclzmachine.h"
#include "zmachine_object.h"

#include <stdio.h>
#include <string.h>

static int obj_error(ZMachine *vm, const char *message)
{
    if (vm) {
        vm->state = ZM_STATE_ERROR;
        snprintf(vm->error, sizeof(vm->error), "%s", message);
    }
    return TCL_ERROR;
}

static int read_u8(const ZMachine *vm, uint32_t address, uint8_t *value)
{
    if (!vm || !vm->memory || !value || (size_t)address >= vm->memory_size)
        return TCL_ERROR;
    *value = vm->memory[address];
    return TCL_OK;
}

static int read_u16(const ZMachine *vm, uint32_t address, uint16_t *value)
{
    if (!vm || !vm->memory || !value || (size_t)address + 1U >= vm->memory_size)
        return TCL_ERROR;
    *value = (uint16_t)(((uint16_t)vm->memory[address] << 8) | vm->memory[address + 1U]);
    return TCL_OK;
}

static int write_u8(ZMachine *vm, uint32_t address, uint8_t value)
{
    if (!vm || !vm->memory || (size_t)address >= vm->memory_size ||
        (size_t)address >= (size_t)vm->static_memory_addr)
        return obj_error(vm, "object write is outside dynamic memory");
    vm->memory[address] = value;
    return TCL_OK;
}

static int write_u16(ZMachine *vm, uint32_t address, uint16_t value)
{
    if (!vm || !vm->memory || (size_t)address + 1U >= vm->memory_size ||
        (size_t)address + 1U >= (size_t)vm->static_memory_addr)
        return obj_error(vm, "object write is outside dynamic memory");
    vm->memory[address] = (uint8_t)(value >> 8);
    vm->memory[address + 1U] = (uint8_t)value;
    return TCL_OK;
}

static uint32_t defaults_size(const ZMachine *vm)
{
    return (vm->version <= 3U) ? 62U : 126U;
}

static uint32_t object_entry_size(const ZMachine *vm)
{
    return (vm->version <= 3U) ? 9U : 14U;
}

static int object_entry(const ZMachine *vm, uint16_t object, uint32_t *address)
{
    uint32_t base;
    uint32_t size;
    uint32_t a;
    if (!vm || !address || object == 0U)
        return TCL_ERROR;
    base = (uint32_t)vm->object_table_addr + defaults_size(vm);
    size = object_entry_size(vm);
    a = base + (uint32_t)(object - 1U) * size;
    if ((size_t)a + size > vm->memory_size)
        return TCL_ERROR;
    *address = a;
    return TCL_OK;
}

static int relation_offset(const ZMachine *vm, int which, uint32_t *offset)
{
    if (!vm || !offset) return TCL_ERROR;
    if (vm->version <= 3U) {
        *offset = (which == 0) ? 4U : (which == 1 ? 5U : 6U);
    } else {
        *offset = (which == 0) ? 6U : (which == 1 ? 8U : 10U);
    }
    return TCL_OK;
}

static int get_relation(const ZMachine *vm, uint16_t object, int which, uint16_t *value)
{
    uint32_t entry, offset;
    if (!value || object_entry(vm, object, &entry) != TCL_OK ||
        relation_offset(vm, which, &offset) != TCL_OK)
        return TCL_ERROR;
    if (vm->version <= 3U) {
        uint8_t b;
        if (read_u8(vm, entry + offset, &b) != TCL_OK) return TCL_ERROR;
        *value = b;
        return TCL_OK;
    }
    return read_u16(vm, entry + offset, value);
}

static int set_relation(ZMachine *vm, uint16_t object, int which, uint16_t value)
{
    uint32_t entry, offset;
    if (object_entry(vm, object, &entry) != TCL_OK ||
        relation_offset(vm, which, &offset) != TCL_OK)
        return obj_error(vm, "invalid Z-machine object reference");
    if (vm->version <= 3U) {
        if (value > 255U) return obj_error(vm, "V1-V3 object relation exceeds 255");
        return write_u8(vm, entry + offset, (uint8_t)value);
    }
    return write_u16(vm, entry + offset, value);
}

int zmachine_object_get_parent(const ZMachine *vm, uint16_t object, uint16_t *parent)
{ return get_relation(vm, object, 0, parent); }
int zmachine_object_get_sibling(const ZMachine *vm, uint16_t object, uint16_t *sibling)
{ return get_relation(vm, object, 1, sibling); }
int zmachine_object_get_child(const ZMachine *vm, uint16_t object, uint16_t *child)
{ return get_relation(vm, object, 2, child); }
int zmachine_object_set_parent(ZMachine *vm, uint16_t object, uint16_t parent)
{ return set_relation(vm, object, 0, parent); }
int zmachine_object_set_sibling(ZMachine *vm, uint16_t object, uint16_t sibling)
{ return set_relation(vm, object, 1, sibling); }
int zmachine_object_set_child(ZMachine *vm, uint16_t object, uint16_t child)
{ return set_relation(vm, object, 2, child); }

int zmachine_object_test_attr(const ZMachine *vm, uint16_t object, uint8_t attribute, int *is_set)
{
    uint32_t entry;
    uint8_t byte;
    uint8_t max_attr = (vm && vm->version <= 3U) ? 31U : 47U;
    if (!vm || !is_set || attribute > max_attr || object_entry(vm, object, &entry) != TCL_OK)
        return TCL_ERROR;
    if (read_u8(vm, entry + attribute / 8U, &byte) != TCL_OK) return TCL_ERROR;
    *is_set = (byte & (uint8_t)(0x80U >> (attribute % 8U))) != 0U;
    return TCL_OK;
}

int zmachine_object_set_attr(ZMachine *vm, uint16_t object, uint8_t attribute, int is_set)
{
    uint32_t entry, address;
    uint8_t byte, mask;
    uint8_t max_attr = (vm && vm->version <= 3U) ? 31U : 47U;
    if (!vm || attribute > max_attr || object_entry(vm, object, &entry) != TCL_OK)
        return obj_error(vm, "invalid Z-machine object attribute");
    address = entry + attribute / 8U;
    if (read_u8(vm, address, &byte) != TCL_OK) return obj_error(vm, "invalid object attribute address");
    mask = (uint8_t)(0x80U >> (attribute % 8U));
    byte = is_set ? (uint8_t)(byte | mask) : (uint8_t)(byte & (uint8_t)~mask);
    return write_u8(vm, address, byte);
}

int zmachine_object_remove(ZMachine *vm, uint16_t object)
{
    uint16_t parent, sibling, first, current, next;
    if (!vm || object == 0U) return obj_error(vm, "cannot remove object 0");
    if (zmachine_object_get_parent(vm, object, &parent) != TCL_OK ||
        zmachine_object_get_sibling(vm, object, &sibling) != TCL_OK)
        return obj_error(vm, "invalid Z-machine object");
    if (parent != 0U) {
        if (zmachine_object_get_child(vm, parent, &first) != TCL_OK)
            return obj_error(vm, "invalid parent object");
        if (first == object) {
            if (zmachine_object_set_child(vm, parent, sibling) != TCL_OK) return TCL_ERROR;
        } else {
            current = first;
            while (current != 0U) {
                if (zmachine_object_get_sibling(vm, current, &next) != TCL_OK)
                    return obj_error(vm, "invalid sibling chain");
                if (next == object) {
                    if (zmachine_object_set_sibling(vm, current, sibling) != TCL_OK) return TCL_ERROR;
                    break;
                }
                current = next;
            }
        }
    }
    if (zmachine_object_set_parent(vm, object, 0U) != TCL_OK) return TCL_ERROR;
    return zmachine_object_set_sibling(vm, object, 0U);
}

int zmachine_object_insert(ZMachine *vm, uint16_t object, uint16_t destination)
{
    uint16_t first;
    if (!vm || object == 0U || destination == 0U || object == destination)
        return obj_error(vm, "invalid object insertion");
    if (zmachine_object_remove(vm, object) != TCL_OK) return TCL_ERROR;
    if (zmachine_object_get_child(vm, destination, &first) != TCL_OK)
        return obj_error(vm, "invalid destination object");
    if (zmachine_object_set_parent(vm, object, destination) != TCL_OK ||
        zmachine_object_set_sibling(vm, object, first) != TCL_OK ||
        zmachine_object_set_child(vm, destination, object) != TCL_OK)
        return TCL_ERROR;
    return TCL_OK;
}

int zmachine_object_property_table(const ZMachine *vm, uint16_t object, uint32_t *address)
{
    uint32_t entry;
    uint32_t offset;
    uint16_t ptr;
    if (!address || object_entry(vm, object, &entry) != TCL_OK) return TCL_ERROR;
    offset = (vm->version <= 3U) ? 7U : 12U;
    if (read_u16(vm, entry + offset, &ptr) != TCL_OK) return TCL_ERROR;
    if ((size_t)ptr >= vm->memory_size) return TCL_ERROR;
    *address = ptr;
    return TCL_OK;
}

static int first_property_header(const ZMachine *vm, uint16_t object, uint32_t *address)
{
    uint32_t table;
    uint8_t words;
    if (zmachine_object_property_table(vm, object, &table) != TCL_OK ||
        read_u8(vm, table, &words) != TCL_OK)
        return TCL_ERROR;
    *address = table + 1U + (uint32_t)words * 2U;
    if ((size_t)*address >= vm->memory_size) return TCL_ERROR;
    return TCL_OK;
}

static int parse_property(const ZMachine *vm, uint32_t header, ZMachinePropertyInfo *info)
{
    uint8_t first;
    if (!info || read_u8(vm, header, &first) != TCL_OK) return TCL_ERROR;
    memset(info, 0, sizeof(*info));
    if (first == 0U) {
        info->next_header_address = header;
        return TCL_OK;
    }
    if (vm->version <= 3U) {
        info->number = (uint16_t)(first & 0x1fU);
        info->length = (uint16_t)((first >> 5) + 1U);
        info->data_address = header + 1U;
        info->next_header_address = info->data_address + info->length;
    } else {
        info->number = (uint16_t)(first & 0x3fU);
        if (first & 0x80U) {
            uint8_t second;
            if (read_u8(vm, header + 1U, &second) != TCL_OK) return TCL_ERROR;
            info->length = (uint16_t)(second & 0x3fU);
            if (info->length == 0U) info->length = 64U;
            info->data_address = header + 2U;
        } else {
            info->length = (first & 0x40U) ? 2U : 1U;
            info->data_address = header + 1U;
        }
        info->next_header_address = info->data_address + info->length;
    }
    if ((size_t)info->data_address + info->length > vm->memory_size) return TCL_ERROR;
    return TCL_OK;
}

int zmachine_object_find_property(const ZMachine *vm, uint16_t object, uint16_t property,
                                  ZMachinePropertyInfo *info)
{
    uint32_t header;
    ZMachinePropertyInfo p;
    if (!vm || !info || property == 0U || first_property_header(vm, object, &header) != TCL_OK)
        return TCL_ERROR;
    while ((size_t)header < vm->memory_size) {
        if (parse_property(vm, header, &p) != TCL_OK) return TCL_ERROR;
        if (p.number == 0U) break;
        if (p.number == property) { *info = p; return TCL_OK; }
        if (p.number < property) break;
        header = p.next_header_address;
    }
    memset(info, 0, sizeof(*info));
    return TCL_OK;
}

int zmachine_object_get_prop(const ZMachine *vm, uint16_t object, uint16_t property, uint16_t *value)
{
    ZMachinePropertyInfo info;
    uint32_t def;
    if (!vm || !value || property == 0U) return TCL_ERROR;
    if (zmachine_object_find_property(vm, object, property, &info) != TCL_OK) return TCL_ERROR;
    if (info.number == 0U) {
        if (property > ((vm->version <= 3U) ? 31U : 63U)) return TCL_ERROR;
        def = (uint32_t)vm->object_table_addr + (uint32_t)(property - 1U) * 2U;
        return read_u16(vm, def, value);
    }
    if (info.length == 1U) {
        uint8_t b;
        if (read_u8(vm, info.data_address, &b) != TCL_OK) return TCL_ERROR;
        *value = b;
        return TCL_OK;
    }
    if (info.length == 2U) return read_u16(vm, info.data_address, value);
    return TCL_ERROR;
}

int zmachine_object_put_prop(ZMachine *vm, uint16_t object, uint16_t property, uint16_t value)
{
    ZMachinePropertyInfo info;
    if (!vm || property == 0U || zmachine_object_find_property(vm, object, property, &info) != TCL_OK ||
        info.number == 0U)
        return obj_error(vm, "attempt to write nonexistent Z-machine property");
    if (info.length == 1U) return write_u8(vm, info.data_address, (uint8_t)value);
    if (info.length == 2U) return write_u16(vm, info.data_address, value);
    return obj_error(vm, "put_prop requires a one- or two-byte property");
}

int zmachine_object_get_prop_addr(const ZMachine *vm, uint16_t object, uint16_t property, uint16_t *address)
{
    ZMachinePropertyInfo info;
    if (!address || zmachine_object_find_property(vm, object, property, &info) != TCL_OK) return TCL_ERROR;
    *address = (uint16_t)(info.number ? info.data_address : 0U);
    return TCL_OK;
}

int zmachine_object_get_next_prop(const ZMachine *vm, uint16_t object, uint16_t property, uint16_t *next_property)
{
    uint32_t header;
    ZMachinePropertyInfo info;
    if (!vm || !next_property || first_property_header(vm, object, &header) != TCL_OK) return TCL_ERROR;
    if (property == 0U) {
        if (parse_property(vm, header, &info) != TCL_OK) return TCL_ERROR;
        *next_property = info.number;
        return TCL_OK;
    }
    while ((size_t)header < vm->memory_size) {
        if (parse_property(vm, header, &info) != TCL_OK) return TCL_ERROR;
        if (info.number == 0U) return TCL_ERROR;
        if (info.number == property) {
            if (parse_property(vm, info.next_header_address, &info) != TCL_OK) return TCL_ERROR;
            *next_property = info.number;
            return TCL_OK;
        }
        header = info.next_header_address;
    }
    return TCL_ERROR;
}
