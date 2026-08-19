#include "tclzmachine.h"
#include "zmachine_decode.h"
#include "zmachine_exec.h"
#include "zmachine_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZM_HEADER_SIZE 64U
#define ZM_RUN_STEP_LIMIT 1000000U

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void set_error(ZMachine *vm, const char *msg)
{
    vm->state = ZM_STATE_ERROR;
    snprintf(vm->error, sizeof(vm->error), "%s", msg ? msg : "unknown error");
}

static int validate_header_layout(ZMachine *vm)
{
    if (vm->static_memory_addr < ZM_HEADER_SIZE || vm->static_memory_addr > vm->memory_size) {
        set_error(vm, "invalid static-memory base in Z-machine header");
        return TCL_ERROR;
    }
    if ((size_t)vm->initial_pc >= vm->memory_size) {
        set_error(vm, "initial program counter is outside the story file");
        return TCL_ERROR;
    }
    if (vm->declared_file_length != 0 && vm->declared_file_length > vm->memory_size) {
        set_error(vm, "story file is shorter than the length declared in its header");
        return TCL_ERROR;
    }
    return TCL_OK;
}

ZMachine *zmachine_create(void)
{
    ZMachine *vm = (ZMachine *)calloc(1, sizeof(*vm));
    if (!vm) return NULL;
    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
    vm->state = ZM_STATE_READY;
    return vm;
}

void zmachine_destroy(ZMachine *vm)
{
    if (!vm) return;
    free(vm->memory);
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
    free(vm);
}

int zmachine_load_story(ZMachine *vm, const char *path)
{
    FILE *fp;
    long size;
    uint8_t *buf;
    uint8_t version;

    if (!vm || !path) return TCL_ERROR;
    fp = fopen(path, "rb");
    if (!fp) {
        set_error(vm, "unable to open story file");
        return TCL_ERROR;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        set_error(vm, "unable to determine story file size");
        return TCL_ERROR;
    }
    if (size < (long)ZM_HEADER_SIZE) {
        fclose(fp);
        set_error(vm, "story file is too small to contain a Z-machine header");
        return TCL_ERROR;
    }
    buf = (uint8_t *)malloc((size_t)size);
    if (!buf) {
        fclose(fp);
        set_error(vm, "out of memory while loading story file");
        return TCL_ERROR;
    }
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        free(buf);
        fclose(fp);
        set_error(vm, "unable to read story file");
        return TCL_ERROR;
    }
    fclose(fp);

    version = buf[0];
    if (!zmachine_version_supported(version)) {
        free(buf);
        if (version == 6)
            set_error(vm, "Z-machine Version 6 is intentionally unsupported by this text-only runtime");
        else
            set_error(vm, "unsupported Z-machine version; supported versions are 1-5, 7, and 8");
        return TCL_ERROR;
    }

    free(vm->memory);
    vm->memory = buf;
    vm->memory_size = (size_t)size;
    vm->version = version;
    vm->flags1 = buf[0x01];
    vm->release_number = read_be16(buf + 0x02);
    vm->high_memory_addr = read_be16(buf + 0x04);
    vm->initial_pc = read_be16(buf + 0x06);
    vm->dictionary_addr = read_be16(buf + 0x08);
    vm->object_table_addr = read_be16(buf + 0x0A);
    vm->globals_addr = read_be16(buf + 0x0C);
    vm->static_memory_addr = read_be16(buf + 0x0E);
    vm->flags2 = read_be16(buf + 0x10);
    vm->abbreviations_addr = read_be16(buf + 0x18);
    vm->header_file_length_word = read_be16(buf + 0x1A);
    vm->declared_file_length = zmachine_header_file_length(version, vm->header_file_length_word);
    vm->checksum = read_be16(buf + 0x1C);
    vm->routine_offset = (version == 7) ? read_be16(buf + 0x28) : 0;
    vm->string_offset = (version == 7) ? read_be16(buf + 0x2A) : 0;
    vm->header_extension_addr = (version >= 5) ? read_be16(buf + 0x36) : 0;

    if (validate_header_layout(vm) != TCL_OK) {
        free(vm->memory);
        vm->memory = NULL;
        vm->memory_size = 0;
        return TCL_ERROR;
    }
    return zmachine_reset(vm);
}

int zmachine_reset(ZMachine *vm)
{
    if (!vm || !vm->memory) {
        if (vm) set_error(vm, "no story is loaded");
        return TCL_ERROR;
    }
    vm->pc = vm->initial_pc;
    vm->sp = 0;
    vm->frame_count = 0;
    vm->state = ZM_STATE_READY;
    vm->input_available = 0;
    vm->error[0] = '\0';
    Tcl_DStringSetLength(&vm->output, 0);
    Tcl_DStringSetLength(&vm->pending_input, 0);
    return TCL_OK;
}

int zmachine_supply_input(ZMachine *vm, const char *line)
{
    if (!vm || !line) return TCL_ERROR;
    if (vm->state == ZM_STATE_HALTED || vm->state == ZM_STATE_ERROR)
        return TCL_ERROR;
    Tcl_DStringSetLength(&vm->pending_input, 0);
    Tcl_DStringAppend(&vm->pending_input, line, -1);
    vm->input_available = 1;
    if (vm->state == ZM_STATE_WAITING_INPUT)
        vm->state = ZM_STATE_READY;
    return TCL_OK;
}

static int handle_read_opcode(ZMachine *vm, int *handled)
{
    ZMachineInstruction instruction;
    uint16_t values[ZM_MAX_OPERANDS];
    uint16_t terminator = 13U;
    char decode_error[128];
    uint32_t next_pc;

    *handled = 0;
    if (!zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                     vm->pc, &instruction,
                                     decode_error, sizeof(decode_error))) {
        set_error(vm, decode_error[0] ? decode_error : "unable to decode Z-machine instruction");
        return TCL_ERROR;
    }

    if (instruction.operand_count != ZM_OPERANDS_VAR || instruction.opcode_number != 4U)
        return TCL_OK;

    *handled = 1;
    if (!vm->input_available) {
        vm->state = ZM_STATE_WAITING_INPUT;
        return TCL_OK;
    }

    if (instruction.operand_count_actual < 2U) {
        set_error(vm, "read opcode is missing text or parse buffer operands");
        return TCL_ERROR;
    }
    if (zmachine_resolve_operands(vm, &instruction, values, ZM_MAX_OPERANDS) != TCL_OK)
        return TCL_ERROR;
    if (zmachine_input_read_line(vm, values[0], values[1], &terminator) != TCL_OK) {
        set_error(vm, "unable to store or tokenize line input");
        return TCL_ERROR;
    }

    next_pc = instruction.next_pc;
    if (vm->version >= 5U) {
        uint8_t store_var;
        if ((size_t)next_pc >= vm->memory_size) {
            set_error(vm, "truncated read store variable");
            return TCL_ERROR;
        }
        store_var = vm->memory[next_pc++];
        if (zmachine_variable_write(vm, store_var, 0, terminator) != TCL_OK)
            return TCL_ERROR;
    }
    vm->pc = next_pc;
    return TCL_OK;
}

int zmachine_run(ZMachine *vm)
{
    unsigned long steps = 0UL;

    if (!vm || !vm->memory) {
        if (vm) set_error(vm, "no story is loaded");
        return TCL_ERROR;
    }
    if (vm->state == ZM_STATE_ERROR) return TCL_ERROR;
    if (vm->state == ZM_STATE_HALTED) return TCL_OK;

    zmachine_output_clear(vm);
    vm->state = ZM_STATE_READY;

    while (vm->state == ZM_STATE_READY) {
        int handled;
        if (++steps > ZM_RUN_STEP_LIMIT) {
            set_error(vm, "Z-machine execution step limit exceeded");
            return TCL_ERROR;
        }
        if (handle_read_opcode(vm, &handled) != TCL_OK)
            return TCL_ERROR;
        if (vm->state != ZM_STATE_READY)
            break;
        if (handled)
            continue;
        if (zmachine_step(vm) != TCL_OK)
            return TCL_ERROR;
    }
    return vm->state == ZM_STATE_ERROR ? TCL_ERROR : TCL_OK;
}

uint32_t zmachine_unpack_routine_address(const ZMachine *vm, uint16_t packed)
{
    if (!vm) return 0;
    return zmachine_unpack_address(vm->version, ZM_ADDR_ROUTINE, packed,
                                   vm->routine_offset, vm->string_offset);
}

uint32_t zmachine_unpack_string_address(const ZMachine *vm, uint16_t packed)
{
    if (!vm) return 0;
    return zmachine_unpack_address(vm->version, ZM_ADDR_STRING, packed,
                                   vm->routine_offset, vm->string_offset);
}

void zmachine_output_clear(ZMachine *vm)
{
    if (vm) Tcl_DStringSetLength(&vm->output, 0);
}

void zmachine_output_append(ZMachine *vm, const char *text, size_t len)
{
    if (!vm || !text || len == 0) return;
    Tcl_DStringAppend(&vm->output, text, (int)len);
}

const char *zmachine_output_data(const ZMachine *vm)
{
    return vm ? Tcl_DStringValue((Tcl_DString *)&vm->output) : "";
}

int zmachine_output_length(const ZMachine *vm)
{
    return vm ? Tcl_DStringLength((Tcl_DString *)&vm->output) : 0;
}

const char *zmachine_last_error(const ZMachine *vm)
{
    return vm ? vm->error : "invalid VM";
}
