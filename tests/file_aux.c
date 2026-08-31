/*
 * file_aux.c
 *
 * Regression coverage for Version 5+ EXT:0/EXT:1 auxiliary-file forms. These
 * tests exercise the public cooperative instruction/file APIs so operand
 * decoding, request metadata, raw byte transfer, result stores, cancellation,
 * and dynamic-memory protection are all covered together.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void init_vm(ZMachine *vm)
{
    memset(vm, 0, sizeof(*vm));
    vm->memory = (uint8_t *)calloc(1024U, 1U);
    assert(vm->memory != NULL);
    vm->memory_size = 1024U;
    vm->version = 5U;
    vm->static_memory_addr = 0x300U;
    vm->globals_addr = 0x100U;
    vm->state = ZM_STATE_READY;
    vm->output_stream1_enabled = 1;
    vm->pending_file_prompt = -1;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
}

static void free_vm(ZMachine *vm)
{
    free(vm->memory);
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
}

static uint16_t read_word(const ZMachine *vm, size_t address)
{
    return (uint16_t)(((uint16_t)vm->memory[address] << 8) |
                      vm->memory[address + 1U]);
}

static uint16_t read_global(const ZMachine *vm, uint8_t variable)
{
    size_t address = (size_t)vm->globals_addr +
                     (size_t)(variable - 0x10U) * 2U;
    return read_word(vm, address);
}

static void put_filename(ZMachine *vm, uint16_t address, const char *name)
{
    size_t length = strlen(name);
    assert(length <= 255U);
    vm->memory[address] = (uint8_t)length;
    memcpy(vm->memory + address + 1U, name, length);
}

static void write_file(const char *path, const void *data, size_t length)
{
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    assert(fwrite(data, 1U, length, fp) == length);
    assert(fclose(fp) == 0);
}

int main(void)
{
    const char *save_path = "tclzmachine-aux-save-test.bin";
    const char *restore_path = "tclzmachine-aux-restore-test.bin";
    const char *missing_path = "tclzmachine-aux-does-not-exist.bin";

    remove(save_path);
    remove(restore_path);
    remove(missing_path);

    /* Save four bytes with a story-suggested filename and prompt=0. */
    {
        ZMachine vm;
        FILE *fp;
        uint8_t disk[4];

        init_vm(&vm);
        memcpy(vm.memory + 0x180U, "DATA", 4U);
        put_filename(&vm, 0x1A0U, "slot-1");

        /* EXT:0 save 0x180 4 0x1A0 0 -> global16. */
        vm.pc = 0x20U;
        vm.memory[0x20U] = 0xBEU;
        vm.memory[0x21U] = 0x00U;
        vm.memory[0x22U] = 0x11U; /* large, small, large, small */
        vm.memory[0x23U] = 0x01U;
        vm.memory[0x24U] = 0x80U;
        vm.memory[0x25U] = 0x04U;
        vm.memory[0x26U] = 0x01U;
        vm.memory[0x27U] = 0xA0U;
        vm.memory[0x28U] = 0x00U;
        vm.memory[0x29U] = 0x10U;

        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_WAITING_SAVE);
        assert(vm.pending_file_kind == ZM_FILE_REQUEST_AUXILIARY);
        assert(vm.pending_file_pc == 0x29U);
        assert(vm.pending_file_table == 0x180U);
        assert(vm.pending_file_bytes == 4U);
        assert(vm.pending_file_prompt == 0);
        assert(strcmp(vm.pending_file_name, "SLOT1.AUX") == 0);

        assert(zmachine_save_file(&vm, save_path) == TCL_OK);
        assert(vm.state == ZM_STATE_READY);
        assert(vm.pc == 0x2AU);
        assert(read_global(&vm, 0x10U) == 1U);
        assert(vm.pending_file_kind == ZM_FILE_REQUEST_NONE);

        fp = fopen(save_path, "rb");
        assert(fp != NULL);
        assert(fread(disk, 1U, sizeof(disk), fp) == sizeof(disk));
        assert(fclose(fp) == 0);
        assert(memcmp(disk, "DATA", 4U) == 0);
        free_vm(&vm);
    }

    /* Restore a short file and return the exact number of bytes loaded. */
    {
        ZMachine vm;
        const uint8_t file_data[3] = {'X', 'Y', 'Z'};

        write_file(restore_path, file_data, sizeof(file_data));
        init_vm(&vm);
        memset(vm.memory + 0x190U, 0xEE, 6U);
        put_filename(&vm, 0x1A0U, "save.dat");

        /* EXT:1 restore 0x190 6 0x1A0 -> global17 (prompt omitted). */
        vm.pc = 0x30U;
        vm.memory[0x30U] = 0xBEU;
        vm.memory[0x31U] = 0x01U;
        vm.memory[0x32U] = 0x13U; /* large, small, large, omitted */
        vm.memory[0x33U] = 0x01U;
        vm.memory[0x34U] = 0x90U;
        vm.memory[0x35U] = 0x06U;
        vm.memory[0x36U] = 0x01U;
        vm.memory[0x37U] = 0xA0U;
        vm.memory[0x38U] = 0x11U;

        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_WAITING_RESTORE);
        assert(vm.pending_file_prompt == -1);
        assert(strcmp(vm.pending_file_name, "SAVE.DAT") == 0);

        assert(zmachine_restore_file(&vm, restore_path) == TCL_OK);
        assert(vm.state == ZM_STATE_READY);
        assert(vm.pc == 0x39U);
        assert(read_global(&vm, 0x11U) == 3U);
        assert(memcmp(vm.memory + 0x190U, "XYZ", 3U) == 0);
        assert(vm.memory[0x193U] == 0xEEU);
        assert(vm.memory[0x194U] == 0xEEU);
        assert(vm.memory[0x195U] == 0xEEU);
        free_vm(&vm);
    }

    /* Missing auxiliary restore files complete normally with result zero. */
    {
        ZMachine vm;

        init_vm(&vm);
        put_filename(&vm, 0x1A0U, "missing");
        vm.pc = 0x40U;
        vm.memory[0x40U] = 0xBEU;
        vm.memory[0x41U] = 0x01U;
        vm.memory[0x42U] = 0x13U;
        vm.memory[0x43U] = 0x01U;
        vm.memory[0x44U] = 0x90U;
        vm.memory[0x45U] = 0x06U;
        vm.memory[0x46U] = 0x01U;
        vm.memory[0x47U] = 0xA0U;
        vm.memory[0x48U] = 0x12U;

        assert(zmachine_step(&vm) == TCL_OK);
        assert(zmachine_restore_file(&vm, missing_path) == TCL_OK);
        assert(vm.state == ZM_STATE_READY);
        assert(read_global(&vm, 0x12U) == 0U);
        assert(vm.pc == 0x49U);
        free_vm(&vm);
    }

    /* Host cancellation stores the ordinary auxiliary failure result zero. */
    {
        ZMachine vm;

        init_vm(&vm);
        put_filename(&vm, 0x1A0U, "cancel");
        vm.pc = 0x50U;
        vm.memory[0x50U] = 0xBEU;
        vm.memory[0x51U] = 0x00U;
        vm.memory[0x52U] = 0x13U;
        vm.memory[0x53U] = 0x01U;
        vm.memory[0x54U] = 0x80U;
        vm.memory[0x55U] = 0x04U;
        vm.memory[0x56U] = 0x01U;
        vm.memory[0x57U] = 0xA0U;
        vm.memory[0x58U] = 0x13U;

        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_WAITING_SAVE);
        assert(zmachine_cancel_file(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_READY);
        assert(read_global(&vm, 0x13U) == 0U);
        assert(vm.pc == 0x59U);
        free_vm(&vm);
    }

    /* Auxiliary restore must never write across the static-memory boundary. */
    {
        ZMachine vm;

        init_vm(&vm);
        put_filename(&vm, 0x1A0U, "unsafe");
        vm.pc = 0x60U;
        vm.memory[0x60U] = 0xBEU;
        vm.memory[0x61U] = 0x01U;
        vm.memory[0x62U] = 0x13U;
        vm.memory[0x63U] = 0x02U;
        vm.memory[0x64U] = 0xFEU;
        vm.memory[0x65U] = 0x04U;
        vm.memory[0x66U] = 0x01U;
        vm.memory[0x67U] = 0xA0U;
        vm.memory[0x68U] = 0x14U;

        assert(zmachine_step(&vm) == TCL_ERROR);
        assert(vm.state == ZM_STATE_ERROR);
        assert(strstr(vm.error, "dynamic memory") != NULL);
        free_vm(&vm);
    }

    remove(save_path);
    remove(restore_path);
    remove(missing_path);
    puts("auxiliary save/restore tests passed");
    return 0;
}
