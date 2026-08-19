#include "tclzmachine.h"
#include "zmachine_object.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void init_vm(ZMachine *vm, uint8_t version, size_t size, uint16_t object_table)
{
    memset(vm, 0, sizeof(*vm));
    vm->memory = (uint8_t *)calloc(size, 1U);
    assert(vm->memory != NULL);
    vm->memory_size = size;
    vm->version = version;
    vm->static_memory_addr = (uint16_t)size;
    vm->object_table_addr = object_table;
    vm->state = ZM_STATE_READY;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
}

static void free_vm(ZMachine *vm)
{
    free(vm->memory);
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
}

static void put16(uint8_t *memory, size_t address, uint16_t value)
{
    memory[address] = (uint8_t)(value >> 8);
    memory[address + 1U] = (uint8_t)value;
}

static size_t v3_entry(size_t table, unsigned object)
{
    return table + 62U + (object - 1U) * 9U;
}

static size_t v5_entry(size_t table, unsigned object)
{
    return table + 126U + (object - 1U) * 14U;
}

int main(void)
{
    {
        ZMachine vm;
        uint16_t value;
        int set;
        size_t o1, o2, o3;
        init_vm(&vm, 3U, 1024U, 0x40U);
        o1 = v3_entry(0x40U, 1U);
        o2 = v3_entry(0x40U, 2U);
        o3 = v3_entry(0x40U, 3U);

        /* object 1 has children 2 -> 3 */
        vm.memory[o1 + 6U] = 2U;
        vm.memory[o2 + 4U] = 1U;
        vm.memory[o2 + 5U] = 3U;
        vm.memory[o3 + 4U] = 1U;

        assert(zmachine_object_get_child(&vm, 1U, &value) == TCL_OK && value == 2U);
        assert(zmachine_object_get_parent(&vm, 2U, &value) == TCL_OK && value == 1U);
        assert(zmachine_object_get_sibling(&vm, 2U, &value) == TCL_OK && value == 3U);

        assert(zmachine_object_set_attr(&vm, 2U, 0U, 1) == TCL_OK);
        assert(zmachine_object_test_attr(&vm, 2U, 0U, &set) == TCL_OK && set);
        assert(zmachine_object_set_attr(&vm, 2U, 31U, 1) == TCL_OK);
        assert(zmachine_object_test_attr(&vm, 2U, 31U, &set) == TCL_OK && set);

        assert(zmachine_object_remove(&vm, 2U) == TCL_OK);
        assert(zmachine_object_get_child(&vm, 1U, &value) == TCL_OK && value == 3U);
        assert(zmachine_object_get_parent(&vm, 2U, &value) == TCL_OK && value == 0U);

        assert(zmachine_object_insert(&vm, 2U, 3U) == TCL_OK);
        assert(zmachine_object_get_child(&vm, 3U, &value) == TCL_OK && value == 2U);
        assert(zmachine_object_get_parent(&vm, 2U, &value) == TCL_OK && value == 3U);
        free_vm(&vm);
    }

    {
        ZMachine vm;
        uint16_t value, addr, next;
        ZMachinePropertyInfo info;
        size_t o1;
        init_vm(&vm, 3U, 1024U, 0x40U);
        o1 = v3_entry(0x40U, 1U);

        /* default property 5 */
        put16(vm.memory, 0x40U + 8U, 0xA55AU);

        /* property table at 0x200, no short name.
         * prop 10 len 2 = header 0x2a, prop 5 len 1 = 0x05, terminator. */
        put16(vm.memory, o1 + 7U, 0x0200U);
        vm.memory[0x200U] = 0U;
        vm.memory[0x201U] = 0x2AU;
        vm.memory[0x202U] = 0x12U;
        vm.memory[0x203U] = 0x34U;
        vm.memory[0x204U] = 0x05U;
        vm.memory[0x205U] = 0x7EU;
        vm.memory[0x206U] = 0U;

        assert(zmachine_object_get_prop(&vm, 1U, 10U, &value) == TCL_OK && value == 0x1234U);
        assert(zmachine_object_get_prop(&vm, 1U, 5U, &value) == TCL_OK && value == 0x7EU);
        assert(zmachine_object_get_prop(&vm, 1U, 4U, &value) == TCL_OK && value == 0U);
        assert(zmachine_object_get_prop(&vm, 1U, 5U, &value) == TCL_OK);
        assert(zmachine_object_put_prop(&vm, 1U, 10U, 0xBEEFU) == TCL_OK);
        assert(zmachine_object_get_prop(&vm, 1U, 10U, &value) == TCL_OK && value == 0xBEEFU);
        assert(zmachine_object_get_prop_addr(&vm, 1U, 10U, &addr) == TCL_OK && addr == 0x202U);
        assert(zmachine_object_get_next_prop(&vm, 1U, 0U, &next) == TCL_OK && next == 10U);
        assert(zmachine_object_get_next_prop(&vm, 1U, 10U, &next) == TCL_OK && next == 5U);
        assert(zmachine_object_find_property(&vm, 1U, 10U, &info) == TCL_OK && info.length == 2U);
        free_vm(&vm);
    }

    {
        ZMachine vm;
        uint16_t value, next;
        int set;
        size_t o1, o2;
        init_vm(&vm, 5U, 2048U, 0x40U);
        o1 = v5_entry(0x40U, 1U);
        o2 = v5_entry(0x40U, 2U);

        put16(vm.memory, o1 + 10U, 2U);
        put16(vm.memory, o2 + 6U, 1U);
        assert(zmachine_object_get_child(&vm, 1U, &value) == TCL_OK && value == 2U);
        assert(zmachine_object_get_parent(&vm, 2U, &value) == TCL_OK && value == 1U);
        assert(zmachine_object_set_attr(&vm, 2U, 47U, 1) == TCL_OK);
        assert(zmachine_object_test_attr(&vm, 2U, 47U, &set) == TCL_OK && set);

        /* V5 property table at 0x300: property 40 length 2, property 3 length 4. */
        put16(vm.memory, o1 + 12U, 0x0300U);
        vm.memory[0x300U] = 0U;
        vm.memory[0x301U] = 0x68U; /* prop 40, one-byte header, len 2 */
        vm.memory[0x302U] = 0xCAU;
        vm.memory[0x303U] = 0xFEU;
        vm.memory[0x304U] = 0x83U; /* prop 3, two-byte size header */
        vm.memory[0x305U] = 0x04U;
        vm.memory[0x306U] = 1U; vm.memory[0x307U] = 2U;
        vm.memory[0x308U] = 3U; vm.memory[0x309U] = 4U;
        vm.memory[0x30AU] = 0U;

        assert(zmachine_object_get_prop(&vm, 1U, 40U, &value) == TCL_OK && value == 0xCAFEU);
        assert(zmachine_object_get_next_prop(&vm, 1U, 40U, &next) == TCL_OK && next == 3U);
        assert(zmachine_object_get_next_prop(&vm, 1U, 3U, &next) == TCL_OK && next == 0U);
        free_vm(&vm);
    }

    puts("object subsystem tests passed");
    return 0;
}
