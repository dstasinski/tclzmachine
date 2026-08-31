/*
 * quetzal.c
 *
 * Regression coverage for Quetzal persistence and cooperative save/restore
 * dispatch. The first scenario round-trips a real VM state through the public
 * EXT:0/EXT:1 file-request path. The second writes a minimal external-style
 * CMem save to verify that restores are not limited to tclzmachine's UMem
 * output format.
 */

#include "tclzmachine.h"
#include "zmachine_exec.h"
#include "zmachine_quetzal.h"
#include "zmachine_undo.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_word(uint8_t *memory, size_t address, uint16_t value)
{
    memory[address] = (uint8_t)(value >> 8);
    memory[address + 1U] = (uint8_t)value;
}

static uint16_t get_word(const uint8_t *memory, size_t address)
{
    return (uint16_t)(((uint16_t)memory[address] << 8) |
                      memory[address + 1U]);
}

static void init_vm(ZMachine *vm)
{
    memset(vm, 0, sizeof(*vm));
    vm->memory_size = 1024U;
    vm->memory = (uint8_t *)calloc(vm->memory_size, 1U);
    assert(vm->memory != NULL);
    vm->version = 5U;
    vm->static_memory_addr = 0x200U;
    vm->globals_addr = 0x100U;
    vm->release_number = 0x1234U;
    vm->header_file_length_word = 0x0100U; /* V5 scale 4 => 1024 bytes. */
    vm->declared_file_length = 1024U;
    vm->checksum = 0x5678U;
    vm->state = ZM_STATE_READY;
    vm->output_stream1_enabled = 1;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);

    vm->memory[0] = 5U;
    put_word(vm->memory, 0x02U, vm->release_number);
    put_word(vm->memory, 0x0cU, vm->globals_addr);
    put_word(vm->memory, 0x0eU, vm->static_memory_addr);
    memcpy(vm->memory + 0x12U, "260830", 6U);
    put_word(vm->memory, 0x1aU, vm->header_file_length_word);
    put_word(vm->memory, 0x1cU, vm->checksum);

    /*
     * Full-game V5 save/restore, both with no optional auxiliary operands.
     * Synthetic executable code deliberately starts above the 64-byte header:
     * interpreter-owned header bytes such as $32/$33 may legally be rewritten
     * after restore and therefore must never double as instruction bytes here.
     */
    vm->memory[0x80U] = 0xbeU;
    vm->memory[0x81U] = 0U;
    vm->memory[0x82U] = 0xffU;
    vm->memory[0x83U] = 0x10U;

    vm->memory[0xa0U] = 0xbeU;
    vm->memory[0xa1U] = 1U;
    vm->memory[0xa2U] = 0xffU;
    vm->memory[0xa3U] = 0x11U;

    /* Additional requests used to verify explicit host cancellation. */
    vm->memory[0xc0U] = 0xbeU;
    vm->memory[0xc1U] = 0U;
    vm->memory[0xc2U] = 0xffU;
    vm->memory[0xc3U] = 0x13U;

    vm->memory[0xd0U] = 0xbeU;
    vm->memory[0xd1U] = 1U;
    vm->memory[0xd2U] = 0xffU;
    vm->memory[0xd3U] = 0x14U;

    vm->initial_dynamic_memory_size = vm->static_memory_addr;
    vm->initial_dynamic_memory =
        (uint8_t *)malloc(vm->initial_dynamic_memory_size);
    assert(vm->initial_dynamic_memory != NULL);
    memcpy(vm->initial_dynamic_memory, vm->memory,
           vm->initial_dynamic_memory_size);
}

static void free_vm(ZMachine *vm)
{
    zmachine_undo_discard(vm);
    free(vm->initial_dynamic_memory);
    free(vm->memory);
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
}

static int write_bytes(FILE *fp, const void *data, size_t length)
{
    return fwrite(data, 1U, length, fp) == length;
}

static int write_be16(FILE *fp, uint16_t value)
{
    uint8_t b[2] = {(uint8_t)(value >> 8), (uint8_t)value};
    return write_bytes(fp, b, sizeof(b));
}

static int write_be24(FILE *fp, uint32_t value)
{
    uint8_t b[3] = {
        (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value
    };
    return write_bytes(fp, b, sizeof(b));
}

static int write_be32(FILE *fp, uint32_t value)
{
    uint8_t b[4] = {
        (uint8_t)(value >> 24), (uint8_t)(value >> 16),
        (uint8_t)(value >> 8), (uint8_t)value
    };
    return write_bytes(fp, b, sizeof(b));
}

static void write_cmem_fixture(const ZMachine *vm, const char *path)
{
    FILE *fp = fopen(path, "wb");
    const uint8_t cmem[] = {0U, 0x7fU, 0x12U, 0U, 0x03U, 0x34U};
    const uint8_t zero = 0U;

    assert(fp != NULL);
    assert(write_bytes(fp, "FORM", 4U));
    assert(write_be32(fp, 56U));
    assert(write_bytes(fp, "IFZS", 4U));

    assert(write_bytes(fp, "IFhd", 4U));
    assert(write_be32(fp, 13U));
    assert(write_be16(fp, vm->release_number));
    assert(write_bytes(fp, vm->initial_dynamic_memory + 0x12U, 6U));
    assert(write_be16(fp, vm->checksum));
    assert(write_be24(fp, 0x50U));
    assert(write_bytes(fp, &zero, 1U));

    assert(write_bytes(fp, "CMem", 4U));
    assert(write_be32(fp, (uint32_t)sizeof(cmem)));
    assert(write_bytes(fp, cmem, sizeof(cmem)));

    assert(write_bytes(fp, "Stks", 4U));
    assert(write_be32(fp, 8U));
    assert(write_be24(fp, 0U));
    assert(write_bytes(fp, &zero, 1U));
    assert(write_bytes(fp, &zero, 1U));
    assert(write_bytes(fp, &zero, 1U));
    assert(write_be16(fp, 0U));

    assert(fclose(fp) == 0);
}

int main(void)
{
    const char *save_path = "tclzmachine-quetzal-test.sav";
    const char *cmem_path = "tclzmachine-quetzal-cmem-test.sav";

    {
        ZMachine vm;
        uint16_t locals[2] = {0xaaaaU, 0xbbbbU};
        init_vm(&vm);

        assert(zmachine_stack_push(&vm, 0x1111U) == TCL_OK);
        assert(zmachine_frame_push(&vm, 0x90U, 0x12U, 0,
                                   locals, 2U, 0x03U) == TCL_OK);
        assert(zmachine_stack_push(&vm, 0x2222U) == TCL_OK);
        vm.memory[0x180U] = 0xaaU;
        vm.random_state = 0x11111111U;
        vm.current_window = 1U;

        vm.pc = 0x80U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_WAITING_SAVE);
        assert(vm.pending_file_pc == 0x83U);

        assert(zmachine_save_file(&vm, save_path) == TCL_OK);
        assert(vm.state == ZM_STATE_READY);
        assert(vm.pc == 0x84U);
        assert(get_word(vm.memory, 0x100U) == 1U);

        vm.memory[0x180U] = 0xbbU;
        vm.stack[0] = 0x9999U;
        vm.stack[1] = 0x8888U;
        vm.frames[0].locals[0] = 0x7777U;
        put_word(vm.memory, 0x10U, 0xa55aU);
        vm.flags2 = 0xa55aU;
        vm.random_state = 0x22222222U;
        vm.current_window = 2U;

        vm.pc = 0xa0U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_WAITING_RESTORE);
        assert(vm.pending_file_pc == 0xa3U);

        assert(zmachine_restore_file(&vm, save_path) == TCL_OK);
        assert(vm.state == ZM_STATE_READY);
        assert(vm.pc == 0x84U);
        assert(get_word(vm.memory, 0x100U) == 2U);
        assert(vm.memory[0x180U] == 0xaaU);
        assert(vm.sp == 2U);
        assert(vm.stack[0] == 0x1111U && vm.stack[1] == 0x2222U);
        assert(vm.frame_count == 1U);
        assert(vm.frames[0].stack_base == 1U);
        assert(vm.frames[0].return_pc == 0x90U);
        assert(vm.frames[0].locals[0] == 0xaaaaU);
        assert(vm.frames[0].locals[1] == 0xbbbbU);
        assert(vm.frames[0].argument_mask == 0x03U);

        /*
         * Flags 2 is live interpreter/session state. Header refresh may clear
         * unsupported request bits, but transcription/undo state must survive.
         */
        assert((get_word(vm.memory, 0x10U) & 0x0011U) ==
               (0xa55aU & 0x0011U));
        assert(vm.flags2 == get_word(vm.memory, 0x10U));
        assert(vm.random_state == 0x22222222U);
        assert(vm.current_window == 2U);

        /* Declining either host-file request completes the opcode with zero. */
        vm.pc = 0xc0U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_WAITING_SAVE);
        assert(zmachine_cancel_file(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_READY && vm.pc == 0xc4U);
        assert(get_word(vm.memory, 0x106U) == 0U);

        vm.pc = 0xd0U;
        assert(zmachine_step(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_WAITING_RESTORE);
        assert(zmachine_cancel_file(&vm) == TCL_OK);
        assert(vm.state == ZM_STATE_READY && vm.pc == 0xd4U);
        assert(get_word(vm.memory, 0x108U) == 0U);

        free_vm(&vm);
    }

    {
        ZMachine vm;
        uint32_t restored_pc = 0U;
        init_vm(&vm);
        write_cmem_fixture(&vm, cmem_path);

        vm.memory[0x80U] = 0xeeU;
        vm.memory[0x85U] = 0xeeU;
        assert(zmachine_quetzal_restore(&vm, cmem_path, &restored_pc) == TCL_OK);
        assert(restored_pc == 0x50U);
        assert(vm.memory[0x80U] ==
               (uint8_t)(vm.initial_dynamic_memory[0x80U] ^ 0x12U));
        assert(vm.memory[0x85U] ==
               (uint8_t)(vm.initial_dynamic_memory[0x85U] ^ 0x34U));
        assert(vm.sp == 0U && vm.frame_count == 0U);

        free_vm(&vm);
    }

    remove(save_path);
    remove(cmem_path);
    puts("Quetzal save/restore tests passed");
    return 0;
}
