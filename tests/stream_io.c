/*
 * stream_io.c
 *
 * End-to-end coverage for the external text streams which have host files:
 * input stream 1 command replay, output stream 2 transcript, and output stream
 * 4 command recording. Tests deliberately exercise the public cooperative run
 * loop so filename requests, automatic replay feeding, line echo, and read_char
 * recording remain synchronized.
 */

#include "tclzmachine.h"
#include "zmachine_stream.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPLAY_PATH "tclzmachine-stream-replay.tmp"
#define TRANSCRIPT_PATH "tclzmachine-stream-transcript.tmp"
#define RECORD_PATH "tclzmachine-stream-record.tmp"

static void init_vm(ZMachine *vm)
{
    memset(vm, 0, sizeof(*vm));
    vm->memory = (uint8_t *)calloc(1024U, 1U);
    assert(vm->memory != NULL);
    vm->memory_size = 1024U;
    vm->version = 5U;
    vm->pc = 0x20U;
    vm->initial_pc = 0x20U;
    vm->static_memory_addr = 0x300U;
    vm->globals_addr = 0x40U;
    vm->state = ZM_STATE_READY;
    vm->output_stream1_enabled = 1;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
}

static void free_vm(ZMachine *vm)
{
    assert(zmachine_reset(vm) == TCL_OK);
    free(vm->memory);
    vm->memory = NULL;
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
}

static void put_text_read(uint8_t *memory, size_t pc, uint16_t buffer,
                          uint8_t store_variable)
{
    memory[pc] = 0xE4U;       /* VAR:4 read */
    memory[pc + 1U] = 0x3FU; /* one large constant */
    memory[pc + 2U] = (uint8_t)(buffer >> 8);
    memory[pc + 3U] = (uint8_t)buffer;
    memory[pc + 4U] = store_variable;
}

static void read_file(const char *path, char *buffer, size_t capacity)
{
    FILE *fp = fopen(path, "rb");
    size_t length;

    assert(fp != NULL);
    length = fread(buffer, 1U, capacity - 1U, fp);
    assert(!ferror(fp));
    buffer[length] = '\0';
    fclose(fp);
}

static void assert_run_ok(ZMachine *vm, const char *context)
{
    int rc = zmachine_run(vm);
    if (rc != TCL_OK) {
        fprintf(stderr,
                "%s: rc=%d state=%d pc=0x%lx error=%s\n",
                context, rc, (int)vm->state, (unsigned long)vm->pc,
                vm->error[0] ? vm->error : "(none)");
    }
    assert(rc == TCL_OK);
}

int main(void)
{
    remove(REPLAY_PATH);
    remove(TRANSCRIPT_PATH);
    remove(RECORD_PATH);

    /* input_stream 1 requests a replay path, then automatically supplies LOOK. */
    {
        ZMachine vm;
        FILE *fp;

        init_vm(&vm);
        fp = fopen(REPLAY_PATH, "wb");
        assert(fp != NULL);
        assert(fputs("LOOK\n", fp) >= 0);
        fclose(fp);

        vm.memory[0x20U] = 0xF4U; /* VAR:20 input_stream */
        vm.memory[0x21U] = 0x7FU;
        vm.memory[0x22U] = 1U;
        put_text_read(vm.memory, 0x23U, 0x0080U, 0x10U);
        vm.memory[0x28U] = 0xBAU; /* quit */
        vm.memory[0x80U] = 20U;

        assert_run_ok(&vm, "initial replay selection");
        assert(vm.state == ZM_STATE_WAITING_STREAM_FILE);
        assert(strcmp(zmachine_pending_stream_request(&vm), "replay") == 0);
        assert(vm.pc == 0x20U);

        assert(zmachine_stream_file(&vm, "replay", REPLAY_PATH) == TCL_OK);
        assert(vm.pc == 0x23U);
        assert(zmachine_current_input_stream(&vm) == 1);
        assert_run_ok(&vm, "replay execution");
        assert(vm.state == ZM_STATE_HALTED);
        assert(vm.memory[0x81U] == 4U);
        assert(memcmp(vm.memory + 0x82U, "look", 4U) == 0);
        assert(vm.memory[0x40U] == 0U && vm.memory[0x41U] == 13U);

        free_vm(&vm);
        remove(REPLAY_PATH);
    }

    /* Stream 2 receives story output plus completed V1-V5 line input echo. */
    {
        ZMachine vm;
        char contents[128];

        init_vm(&vm);

        vm.memory[0x20U] = 0xF3U; /* VAR:19 output_stream */
        vm.memory[0x21U] = 0x7FU;
        vm.memory[0x22U] = 2U;
        vm.memory[0x23U] = 0xE5U; /* VAR:5 print_char */
        vm.memory[0x24U] = 0x7FU;
        vm.memory[0x25U] = (uint8_t)'A';
        put_text_read(vm.memory, 0x26U, 0x0080U, 0x10U);
        vm.memory[0x2BU] = 0xBAU;
        vm.memory[0x80U] = 20U;

        assert_run_ok(&vm, "transcript selection");
        assert(vm.state == ZM_STATE_WAITING_STREAM_FILE);
        assert(strcmp(zmachine_pending_stream_request(&vm), "transcript") == 0);
        assert(zmachine_stream_file(&vm, "transcript", TRANSCRIPT_PATH) == TCL_OK);
        assert_run_ok(&vm, "transcript input wait");
        assert(vm.state == ZM_STATE_WAITING_INPUT);
        assert(strcmp(zmachine_output_data(&vm), "A") == 0);

        assert(zmachine_supply_input(&vm, "LOOK") == TCL_OK);
        assert_run_ok(&vm, "transcript completion");
        assert(vm.state == ZM_STATE_HALTED);
        read_file(TRANSCRIPT_PATH, contents, sizeof(contents));
        assert(strcmp(contents, "Alook\n") == 0);

        free_vm(&vm);
        remove(TRANSCRIPT_PATH);
    }

    /* Stream 4 writes completed commands and exact read_char records. */
    {
        ZMachine vm;
        char contents[128];

        init_vm(&vm);

        vm.memory[0x20U] = 0xF3U; /* output_stream 4 */
        vm.memory[0x21U] = 0x7FU;
        vm.memory[0x22U] = 4U;
        put_text_read(vm.memory, 0x23U, 0x0080U, 0x10U);
        vm.memory[0x28U] = 0xF6U; /* VAR:22 read_char */
        vm.memory[0x29U] = 0x7FU;
        vm.memory[0x2AU] = 1U;
        vm.memory[0x2BU] = 0x11U;
        vm.memory[0x2CU] = 0xF3U; /* output_stream -4 */
        vm.memory[0x2DU] = 0x3FU;
        vm.memory[0x2EU] = 0xFFU;
        vm.memory[0x2FU] = 0xFCU;
        vm.memory[0x30U] = 0xBAU;
        vm.memory[0x80U] = 20U;

        assert_run_ok(&vm, "record selection");
        assert(vm.state == ZM_STATE_WAITING_STREAM_FILE);
        assert(strcmp(zmachine_pending_stream_request(&vm), "record") == 0);
        assert(zmachine_stream_file(&vm, "record", RECORD_PATH) == TCL_OK);
        assert_run_ok(&vm, "record line wait");
        assert(vm.state == ZM_STATE_WAITING_INPUT);

        assert(zmachine_supply_input(&vm, "LOOK") == TCL_OK);
        assert_run_ok(&vm, "record character wait");
        assert(vm.state == ZM_STATE_WAITING_INPUT);
        assert(zmachine_supply_key(&vm, 129U) == TCL_OK);
        assert_run_ok(&vm, "record completion");
        assert(vm.state == ZM_STATE_HALTED);

        read_file(RECORD_PATH, contents, sizeof(contents));
        assert(strcmp(contents, "look\n[129]\n") == 0);
        free_vm(&vm);
        remove(RECORD_PATH);
    }

    puts("external stream I/O tests passed");
    return 0;
}
