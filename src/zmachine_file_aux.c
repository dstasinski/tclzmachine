/*
 * zmachine_file_aux.c
 *
 * Cooperative Version 5+ auxiliary-file save/restore layer.
 *
 * EXT:0/EXT:1 with no operands remain full-game Quetzal requests and delegate
 * to zmachine_file.c. With the standard table/bytes/name operands, however,
 * these opcodes transfer only a story-selected byte region to an external file.
 * Auxiliary files are explicitly not part of the state of play, so this layer
 * never snapshots stacks, globals, random state, or any other VM state.
 *
 * Filesystem/path policy remains at the Tcl boundary. The story-supplied name is
 * normalized to an 8.3-style uppercase suggestion and retained as request
 * metadata, but the actual path is still supplied by zmachine::save or
 * zmachine::restore. The optional prompt operand is likewise metadata for the
 * host: -1 means omitted, 0 means silent use requested, and 1 means the story
 * asks for filename confirmation.
 */

#include "tclzmachine.h"
#include "zmachine_decode.h"
#include "zmachine_exec.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Full-game file layer, renamed at compile time by CMake. */
extern int zmachine_step_file_base(ZMachine *vm);
extern int zmachine_save_file_base(ZMachine *vm, const char *path);
extern int zmachine_restore_file_base(ZMachine *vm, const char *path);
extern int zmachine_cancel_file_base(ZMachine *vm);

/* Mark a malformed story/file opcode as a terminal VM error. */
static int aux_story_error(ZMachine *vm, const char *message)
{
    if (vm) {
        vm->state = ZM_STATE_ERROR;
        snprintf(vm->error, sizeof(vm->error), "%s", message);
    }
    return TCL_ERROR;
}

/* Report host I/O trouble while leaving the cooperative request retryable. */
static int aux_host_error(ZMachine *vm, const char *message)
{
    if (vm)
        snprintf(vm->error, sizeof(vm->error), "%s", message);
    return TCL_ERROR;
}

/* Clear all metadata belonging to a completed/cancelled file request. */
static void clear_file_request(ZMachine *vm)
{
    if (!vm)
        return;

    vm->pending_file_pc = 0U;
    vm->pending_file_kind = ZM_FILE_REQUEST_NONE;
    vm->pending_file_table = 0U;
    vm->pending_file_bytes = 0U;
    vm->pending_file_prompt = -1;
    vm->pending_file_name[0] = '\0';
}

/* Store an auxiliary opcode result and resume immediately after its store byte. */
static int complete_auxiliary(ZMachine *vm, uint16_t result)
{
    uint32_t store_pc;
    uint8_t variable;

    if (!vm || !vm->memory)
        return TCL_ERROR;

    store_pc = vm->pending_file_pc;
    if ((size_t)store_pc >= vm->memory_size) {
        clear_file_request(vm);
        return aux_story_error(vm, "truncated auxiliary save/restore store variable");
    }

    variable = vm->memory[store_pc];
    clear_file_request(vm);
    vm->state = ZM_STATE_READY;
    vm->error[0] = '\0';

    if (zmachine_variable_write(vm, variable, 0, result) != TCL_OK) {
        vm->state = ZM_STATE_ERROR;
        return TCL_ERROR;
    }

    vm->pc = store_pc + 1U;
    return TCL_OK;
}

/*
 * Convert the story's length-prefixed ASCII filename into safe 8.3 metadata.
 *
 * The Standard requires upper case, removal of illegal filename characters,
 * and an .AUX suffix when no full stop is supplied. The historical format is
 * one-to-eight alphanumeric base characters plus zero-to-three extension
 * characters, so excess characters are discarded rather than exposed to the
 * host filesystem. An empty base becomes NULL.
 */
static int read_suggested_name(ZMachine *vm, uint16_t address,
                               char out[ZM_AUX_FILENAME_MAX + 1U])
{
    size_t source;
    size_t length;
    size_t i;
    char base[9];
    char extension[4];
    size_t base_len = 0U;
    size_t ext_len = 0U;
    int saw_dot = 0;

    if (!vm || !vm->memory || !out)
        return TCL_ERROR;

    source = address;
    if (source >= vm->memory_size)
        return aux_story_error(vm, "auxiliary filename array is outside story memory");

    length = vm->memory[source++];
    if (source + length > vm->memory_size)
        return aux_story_error(vm, "truncated auxiliary filename array");

    for (i = 0U; i < length; ++i) {
        unsigned char c = vm->memory[source + i];

        if (c == (unsigned char)'.' && !saw_dot) {
            saw_dot = 1;
            continue;
        }
        if (!isalnum(c))
            continue;

        c = (unsigned char)toupper(c);
        if (!saw_dot) {
            if (base_len < 8U)
                base[base_len++] = (char)c;
        } else if (ext_len < 3U) {
            extension[ext_len++] = (char)c;
        }
    }

    if (base_len == 0U) {
        memcpy(base, "NULL", 4U);
        base_len = 4U;
    }

    memcpy(out, base, base_len);
    if (!saw_dot) {
        memcpy(out + base_len, ".AUX", 4U);
        out[base_len + 4U] = '\0';
    } else {
        out[base_len] = '.';
        if (ext_len > 0U)
            memcpy(out + base_len + 1U, extension, ext_len);
        out[base_len + 1U + ext_len] = '\0';
    }

    return TCL_OK;
}

/* Validate the memory region used by an auxiliary transfer. */
static int validate_aux_region(ZMachine *vm, uint16_t table,
                               uint16_t bytes, int restoring)
{
    size_t start = table;
    size_t length = bytes;
    size_t end;

    if (!vm || !vm->memory)
        return TCL_ERROR;

    if (length > vm->memory_size || start > vm->memory_size - length)
        return aux_story_error(vm, "auxiliary file region is outside story memory");

    end = start + length;
    if (restoring && end > (size_t)vm->static_memory_addr)
        return aux_story_error(vm, "auxiliary restore writes outside dynamic memory");

    return TCL_OK;
}

/*
 * Recognize EXT:0/EXT:1 carrying auxiliary operands and suspend for a host path.
 *
 * Operand resolution occurs before suspension and only for forms owned here;
 * this preserves variable-0 stack-pop semantics exactly once. The result store
 * byte is intentionally left untouched until the Tcl host completes/cancels
 * the request.
 */
static int handle_auxiliary_request(ZMachine *vm,
                                    const ZMachineInstruction *instruction,
                                    int *handled)
{
    uint16_t values[ZM_MAX_OPERANDS];
    int restoring;

    *handled = 0;
    if (!vm || !instruction || vm->version < 5U ||
        instruction->form != ZM_FORM_EXTENDED ||
        (instruction->opcode_number != 0U &&
         instruction->opcode_number != 1U) ||
        instruction->operand_count_actual == 0U)
        return TCL_OK;

    *handled = 1;
    if (instruction->operand_count_actual != 3U &&
        instruction->operand_count_actual != 4U)
        return aux_story_error(vm,
            "auxiliary save/restore requires table, bytes, name, and optional prompt");

    if ((size_t)instruction->next_pc >= vm->memory_size)
        return aux_story_error(vm, "truncated auxiliary save/restore store variable");

    if (zmachine_resolve_operands(vm, instruction, values,
                                  ZM_MAX_OPERANDS) != TCL_OK)
        return TCL_ERROR;

    restoring = instruction->opcode_number == 1U;
    if (validate_aux_region(vm, values[0], values[1], restoring) != TCL_OK)
        return TCL_ERROR;

    if (instruction->operand_count_actual == 4U && values[3] > 1U)
        return aux_story_error(vm, "auxiliary save/restore prompt must be 0 or 1");

    if (read_suggested_name(vm, values[2], vm->pending_file_name) != TCL_OK)
        return TCL_ERROR;

    vm->pending_file_pc = instruction->next_pc;
    vm->pending_file_kind = ZM_FILE_REQUEST_AUXILIARY;
    vm->pending_file_table = values[0];
    vm->pending_file_bytes = values[1];
    vm->pending_file_prompt = instruction->operand_count_actual == 4U ?
                              (int)values[3] : -1;
    vm->state = restoring ? ZM_STATE_WAITING_RESTORE : ZM_STATE_WAITING_SAVE;
    vm->error[0] = '\0';
    return TCL_OK;
}

/*
 * Public instruction entry point above the existing full-game file layer.
 *
 * Auxiliary forms are intercepted here. Every other instruction delegates to
 * the existing file layer; if that layer suspends for a zero-operand full-game
 * save/restore, mark the request as FULL so later Tcl completion can route to
 * Quetzal rather than raw auxiliary I/O.
 */
int zmachine_step(ZMachine *vm)
{
    ZMachineInstruction instruction;
    char decode_error[128];
    int handled;
    int rc;

    if (!vm || !vm->memory)
        return aux_story_error(vm, "cannot execute without a loaded story");

    if (!zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                     vm->pc, &instruction,
                                     decode_error, sizeof(decode_error))) {
        return aux_story_error(vm, decode_error[0] ? decode_error :
                               "unable to decode Z-machine instruction");
    }

    if (handle_auxiliary_request(vm, &instruction, &handled) != TCL_OK)
        return TCL_ERROR;
    if (handled)
        return TCL_OK;

    rc = zmachine_step_file_base(vm);
    if (rc == TCL_OK &&
        (vm->state == ZM_STATE_WAITING_SAVE ||
         vm->state == ZM_STATE_WAITING_RESTORE) &&
        vm->pending_file_kind == ZM_FILE_REQUEST_NONE) {
        vm->pending_file_kind = ZM_FILE_REQUEST_FULL;
        vm->pending_file_prompt = -1;
        vm->pending_file_name[0] = '\0';
    }
    return rc;
}

/* Write exactly the requested story-memory bytes to an auxiliary file. */
static int save_auxiliary(ZMachine *vm, const char *path)
{
    FILE *fp;
    size_t written;
    int close_rc;

    if (validate_aux_region(vm, vm->pending_file_table,
                            vm->pending_file_bytes, 0) != TCL_OK)
        return TCL_ERROR;

    fp = fopen(path, "wb");
    if (!fp)
        return aux_host_error(vm, "unable to open auxiliary save file");

    written = fwrite(vm->memory + vm->pending_file_table, 1U,
                     vm->pending_file_bytes, fp);
    close_rc = fclose(fp);
    if (written != vm->pending_file_bytes || close_rc != 0)
        return aux_host_error(vm, "unable to write auxiliary save file");

    return complete_auxiliary(vm, 1U);
}

/*
 * Read up to the requested byte count transactionally into dynamic memory.
 *
 * A missing file is the ordinary Z-machine restore-failure result 0. Other host
 * I/O errors are surfaced to Tcl and leave the request pending so the host may
 * retry another path or cancel. Short files are valid and return the actual
 * number of bytes loaded.
 */
static int restore_auxiliary(ZMachine *vm, const char *path)
{
    FILE *fp;
    uint8_t *buffer = NULL;
    size_t requested = vm->pending_file_bytes;
    size_t loaded = 0U;

    if (validate_aux_region(vm, vm->pending_file_table,
                            vm->pending_file_bytes, 1) != TCL_OK)
        return TCL_ERROR;

    fp = fopen(path, "rb");
    if (!fp) {
        if (errno == ENOENT)
            return complete_auxiliary(vm, 0U);
        return aux_host_error(vm, "unable to open auxiliary restore file");
    }

    if (requested > 0U) {
        buffer = (uint8_t *)malloc(requested);
        if (!buffer) {
            fclose(fp);
            return aux_host_error(vm,
                "out of memory while reading auxiliary restore file");
        }
        loaded = fread(buffer, 1U, requested, fp);
        if (ferror(fp)) {
            free(buffer);
            fclose(fp);
            return aux_host_error(vm, "unable to read auxiliary restore file");
        }
    }

    fclose(fp);
    if (loaded > 0U)
        memcpy(vm->memory + vm->pending_file_table, buffer, loaded);
    free(buffer);

    return complete_auxiliary(vm, (uint16_t)loaded);
}

/* Complete either a full Quetzal save or an auxiliary byte-region save. */
int zmachine_save_file(ZMachine *vm, const char *path)
{
    int rc;

    if (!vm || !path || vm->state != ZM_STATE_WAITING_SAVE)
        return aux_host_error(vm, "Z-machine is not waiting for a save filename");

    if (vm->pending_file_kind == ZM_FILE_REQUEST_AUXILIARY)
        return save_auxiliary(vm, path);

    rc = zmachine_save_file_base(vm, path);
    if (rc == TCL_OK)
        clear_file_request(vm);
    return rc;
}

/* Complete either a full Quetzal restore or an auxiliary byte-region restore. */
int zmachine_restore_file(ZMachine *vm, const char *path)
{
    int rc;

    if (!vm || !path || vm->state != ZM_STATE_WAITING_RESTORE)
        return aux_host_error(vm, "Z-machine is not waiting for a restore filename");

    if (vm->pending_file_kind == ZM_FILE_REQUEST_AUXILIARY)
        return restore_auxiliary(vm, path);

    rc = zmachine_restore_file_base(vm, path);
    if (rc == TCL_OK)
        clear_file_request(vm);
    return rc;
}

/* Cancel either request kind and return the suspended opcode's failure result. */
int zmachine_cancel_file(ZMachine *vm)
{
    int rc;

    if (!vm || (vm->state != ZM_STATE_WAITING_SAVE &&
                vm->state != ZM_STATE_WAITING_RESTORE))
        return aux_host_error(vm,
            "Z-machine is not waiting for a save/restore filename");

    if (vm->pending_file_kind == ZM_FILE_REQUEST_AUXILIARY)
        return complete_auxiliary(vm, 0U);

    rc = zmachine_cancel_file_base(vm);
    if (rc == TCL_OK)
        clear_file_request(vm);
    return rc;
}
