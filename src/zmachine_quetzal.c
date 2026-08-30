/*
 * zmachine_quetzal.c
 *
 * Quetzal 1.4 FORM IFZS saved-game persistence.
 *
 * Quetzal identifies the loaded story with IFhd, stores dynamic memory in
 * either compressed CMem or literal UMem form, and serializes the call and
 * evaluation stacks in Stks. tclzmachine writes UMem because dynamic memory is
 * at most 64 KiB and simplicity matters more than disk compression here, but
 * the reader accepts both forms for interoperability with other interpreters.
 */

#include "tclzmachine.h"
#include "zmachine_quetzal.h"
#include "zmachine_undo.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUETZAL_FORM_HEADER 12U
#define QUETZAL_IFHD_LENGTH 13U

/* Record a persistence error without otherwise changing the live VM state. */
static int quetzal_error(ZMachine *vm, const char *message)
{
    if (vm)
        snprintf(vm->error, sizeof(vm->error), "%s", message);
    return TCL_ERROR;
}

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) |
           ((uint32_t)p[1] << 8) |
           (uint32_t)p[2];
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static int write_bytes(FILE *fp, const void *data, size_t length)
{
    return length == 0U || fwrite(data, 1U, length, fp) == length;
}

static int write_u8(FILE *fp, uint8_t value)
{
    return write_bytes(fp, &value, 1U);
}

static int write_be16(FILE *fp, uint16_t value)
{
    uint8_t b[2];
    b[0] = (uint8_t)(value >> 8);
    b[1] = (uint8_t)value;
    return write_bytes(fp, b, sizeof(b));
}

static int write_be24(FILE *fp, uint32_t value)
{
    uint8_t b[3];
    b[0] = (uint8_t)(value >> 16);
    b[1] = (uint8_t)(value >> 8);
    b[2] = (uint8_t)value;
    return write_bytes(fp, b, sizeof(b));
}

static int write_be32(FILE *fp, uint32_t value)
{
    uint8_t b[4];
    b[0] = (uint8_t)(value >> 24);
    b[1] = (uint8_t)(value >> 16);
    b[2] = (uint8_t)(value >> 8);
    b[3] = (uint8_t)value;
    return write_bytes(fp, b, sizeof(b));
}

static int write_id(FILE *fp, const char id[4])
{
    return write_bytes(fp, id, 4U);
}

/* Validate internal stack bases and compute the exact Quetzal Stks length. */
static int stks_length(ZMachine *vm, uint32_t *length_out)
{
    size_t length;
    size_t i;
    size_t top_level_end;

    if (!vm || !length_out || vm->sp > sizeof(vm->stack) / sizeof(vm->stack[0]) ||
        vm->frame_count > ZM_MAX_FRAMES)
        return quetzal_error(vm, "invalid VM stack state for Quetzal save");

    top_level_end = vm->frame_count > 0U ? vm->frames[0].stack_base : vm->sp;
    if (top_level_end > vm->sp || top_level_end > 0xffffU)
        return quetzal_error(vm, "invalid top-level stack base for Quetzal save");

    /* Every non-V6 Quetzal Stks chunk begins with an 8-byte dummy frame. */
    length = 8U + 2U * top_level_end;

    for (i = 0U; i < vm->frame_count; ++i) {
        const ZMachineFrame *frame = &vm->frames[i];
        size_t start = frame->stack_base;
        size_t end = i + 1U < vm->frame_count ?
                     vm->frames[i + 1U].stack_base : vm->sp;
        size_t eval_count;

        if (frame->local_count > ZM_MAX_LOCALS ||
            frame->return_pc > 0xffffffU ||
            start < top_level_end || start > end || end > vm->sp)
            return quetzal_error(vm, "invalid routine frame for Quetzal save");

        eval_count = end - start;
        if (eval_count > 0xffffU)
            return quetzal_error(vm, "routine evaluation stack is too large for Quetzal");

        length += 8U + 2U * (size_t)frame->local_count + 2U * eval_count;
        top_level_end = end;
    }

    if (length > UINT32_MAX)
        return quetzal_error(vm, "Quetzal stack chunk is too large");

    *length_out = (uint32_t)length;
    return TCL_OK;
}

static int write_stack_words(FILE *fp,
                             const uint16_t *stack,
                             size_t first,
                             size_t last)
{
    size_t i;
    for (i = first; i < last; ++i) {
        if (!write_be16(fp, stack[i]))
            return 0;
    }
    return 1;
}

static int write_stks(FILE *fp, ZMachine *vm, uint32_t chunk_length)
{
    size_t i;
    size_t top_level_end = vm->frame_count > 0U ?
                           vm->frames[0].stack_base : vm->sp;

    if (!write_id(fp, "Stks") || !write_be32(fp, chunk_length))
        return 0;

    /* Mandatory dummy frame containing evaluation stack used at top level. */
    if (!write_be24(fp, 0U) || !write_u8(fp, 0U) ||
        !write_u8(fp, 0U) || !write_u8(fp, 0U) ||
        !write_be16(fp, (uint16_t)top_level_end) ||
        !write_stack_words(fp, vm->stack, 0U, top_level_end))
        return 0;

    for (i = 0U; i < vm->frame_count; ++i) {
        const ZMachineFrame *frame = &vm->frames[i];
        size_t start = frame->stack_base;
        size_t end = i + 1U < vm->frame_count ?
                     vm->frames[i + 1U].stack_base : vm->sp;
        size_t j;
        uint8_t flags = (uint8_t)(frame->local_count & 0x0fU);
        uint8_t store = frame->store_variable;

        if (frame->discard_result) {
            flags |= 0x10U;
            store = 0U;
        }

        if (!write_be24(fp, frame->return_pc) ||
            !write_u8(fp, flags) ||
            !write_u8(fp, store) ||
            !write_u8(fp, (uint8_t)(frame->argument_mask & 0x7fU)) ||
            !write_be16(fp, (uint16_t)(end - start)))
            return 0;

        for (j = 0U; j < frame->local_count; ++j) {
            if (!write_be16(fp, frame->locals[j]))
                return 0;
        }
        if (!write_stack_words(fp, vm->stack, start, end))
            return 0;
    }

    if (chunk_length & 1U)
        return write_u8(fp, 0U);
    return 1;
}

int zmachine_quetzal_save(ZMachine *vm,
                          const char *path,
                          uint32_t saved_pc)
{
    FILE *fp;
    uint32_t stks_len;
    uint32_t umem_len;
    uint64_t form_length;
    const uint8_t *story_header;
    uint8_t pad = 0U;

    if (!vm || !vm->memory || !path || !path[0])
        return quetzal_error(vm, "invalid Quetzal save request");
    if (!vm->initial_dynamic_memory ||
        vm->initial_dynamic_memory_size != (size_t)vm->static_memory_addr ||
        vm->initial_dynamic_memory_size < 0x40U)
        return quetzal_error(vm, "original dynamic-memory image is unavailable");
    if (saved_pc > 0xffffffU || (size_t)saved_pc >= vm->memory_size)
        return quetzal_error(vm, "Quetzal saved PC is outside story memory");
    if (stks_length(vm, &stks_len) != TCL_OK)
        return TCL_ERROR;

    umem_len = vm->static_memory_addr;
    form_length = 4U +
                  (8U + QUETZAL_IFHD_LENGTH + 1U) +
                  (8U + umem_len + (umem_len & 1U)) +
                  (8U + stks_len + (stks_len & 1U));
    if (form_length > UINT32_MAX)
        return quetzal_error(vm, "Quetzal FORM is too large");

    fp = fopen(path, "wb");
    if (!fp)
        return quetzal_error(vm, "unable to open Quetzal save file for writing");

    story_header = vm->initial_dynamic_memory;

    if (!write_id(fp, "FORM") || !write_be32(fp, (uint32_t)form_length) ||
        !write_id(fp, "IFZS") ||
        !write_id(fp, "IFhd") || !write_be32(fp, QUETZAL_IFHD_LENGTH) ||
        !write_be16(fp, vm->release_number) ||
        !write_bytes(fp, story_header + 0x12U, 6U) ||
        !write_be16(fp, vm->checksum) ||
        !write_be24(fp, saved_pc) ||
        !write_u8(fp, pad) ||
        !write_id(fp, "UMem") || !write_be32(fp, umem_len) ||
        !write_bytes(fp, vm->memory, umem_len) ||
        ((umem_len & 1U) && !write_u8(fp, pad)) ||
        !write_stks(fp, vm, stks_len)) {
        fclose(fp);
        remove(path);
        return quetzal_error(vm, "unable to write complete Quetzal save file");
    }

    if (fclose(fp) != 0) {
        remove(path);
        return quetzal_error(vm, "unable to finalize Quetzal save file");
    }

    vm->error[0] = '\0';
    return TCL_OK;
}

/* Restore interpreter-owned header fields after loading saved dynamic memory. */
static void preserve_live_header_fields(const ZMachine *vm,
                                        const uint8_t *live,
                                        uint8_t *restored)
{
    if (!vm || !live || !restored || vm->static_memory_addr < 0x40U)
        return;

    /* Flags 1 contains interpreter capability bits and is not game-dynamic. */
    restored[0x01U] = live[0x01U];

    /* The complete Flags 2 word explicitly survives restore. */
    restored[0x10U] = live[0x10U];
    restored[0x11U] = live[0x11U];

    if (vm->version >= 4U) {
        restored[0x1eU] = live[0x1eU];
        restored[0x1fU] = live[0x1fU];
        restored[0x20U] = live[0x20U];
        restored[0x21U] = live[0x21U];
    }

    if (vm->version >= 5U) {
        size_t i;
        for (i = 0x22U; i <= 0x27U; ++i)
            restored[i] = live[i];
        restored[0x2cU] = live[0x2cU];
        restored[0x2dU] = live[0x2dU];
        restored[0x32U] = live[0x32U];
        restored[0x33U] = live[0x33U];
    }
}

static int decode_cmem(ZMachine *vm,
                       const uint8_t *encoded,
                       size_t encoded_length,
                       uint8_t *dynamic)
{
    size_t in = 0U;
    size_t out = 0U;
    size_t dynamic_length = vm->static_memory_addr;

    memcpy(dynamic, vm->initial_dynamic_memory, dynamic_length);

    while (in < encoded_length) {
        uint8_t value = encoded[in++];
        if (value != 0U) {
            if (out >= dynamic_length)
                return quetzal_error(vm, "CMem expands beyond dynamic memory");
            dynamic[out] ^= value;
            ++out;
        } else {
            size_t run;
            if (in >= encoded_length)
                return quetzal_error(vm, "CMem ends with an incomplete zero run");
            run = (size_t)encoded[in++] + 1U;
            if (run > dynamic_length - out)
                return quetzal_error(vm, "CMem zero run exceeds dynamic memory");
            out += run;
        }
    }

    /* An omitted CMem tail means unchanged original story bytes. */
    return TCL_OK;
}

static int parse_stks(ZMachine *vm,
                      const uint8_t *data,
                      size_t length,
                      uint16_t *stack_out,
                      size_t *sp_out,
                      ZMachineFrame *frames_out,
                      size_t *frame_count_out)
{
    size_t cursor = 0U;
    size_t sp = 0U;
    size_t frame_count = 0U;
    int first = 1;

    while (cursor < length) {
        uint32_t return_pc;
        uint8_t flags;
        uint8_t store_variable;
        uint8_t argument_mask;
        uint16_t eval_count;
        uint8_t local_count;
        size_t needed;
        size_t i;

        if (length - cursor < 8U)
            return quetzal_error(vm, "truncated Quetzal stack frame");

        return_pc = read_be24(data + cursor);
        flags = data[cursor + 3U];
        store_variable = data[cursor + 4U];
        argument_mask = data[cursor + 5U];
        eval_count = read_be16(data + cursor + 6U);
        cursor += 8U;

        if (flags & 0xe0U)
            return quetzal_error(vm, "invalid reserved bits in Quetzal stack frame");
        local_count = (uint8_t)(flags & 0x0fU);
        needed = 2U * ((size_t)local_count + (size_t)eval_count);
        if (needed > length - cursor)
            return quetzal_error(vm, "truncated Quetzal frame locals or evaluation stack");
        if ((size_t)eval_count >
            sizeof(((ZMachine *)0)->stack) / sizeof(((ZMachine *)0)->stack[0]) - sp)
            return quetzal_error(vm, "Quetzal evaluation stack exceeds interpreter capacity");

        if (first) {
            if (return_pc != 0U || flags != 0U || store_variable != 0U ||
                argument_mask != 0U || local_count != 0U)
                return quetzal_error(vm, "missing mandatory Quetzal dummy stack frame");
            first = 0;
        } else {
            ZMachineFrame *frame;
            if (frame_count >= ZM_MAX_FRAMES)
                return quetzal_error(vm, "Quetzal call stack exceeds interpreter capacity");
            if ((size_t)return_pc >= vm->memory_size)
                return quetzal_error(vm, "Quetzal return PC is outside story memory");

            frame = &frames_out[frame_count++];
            memset(frame, 0, sizeof(*frame));
            frame->return_pc = return_pc;
            frame->stack_base = sp;
            frame->local_count = local_count;
            frame->store_variable = store_variable;
            frame->argument_mask = (uint8_t)(argument_mask & 0x7fU);
            frame->discard_result = (flags & 0x10U) != 0U;

            for (i = 0U; i < local_count; ++i) {
                frame->locals[i] = read_be16(data + cursor);
                cursor += 2U;
            }
        }

        /* Dummy frames have no locals; real-frame locals were consumed above. */
        if (first == 0 && frame_count == 0U)
            ;

        for (i = 0U; i < eval_count; ++i) {
            stack_out[sp++] = read_be16(data + cursor);
            cursor += 2U;
        }
    }

    if (first)
        return quetzal_error(vm, "Quetzal Stks chunk contains no dummy frame");

    *sp_out = sp;
    *frame_count_out = frame_count;
    return TCL_OK;
}

int zmachine_quetzal_restore(ZMachine *vm,
                             const char *path,
                             uint32_t *saved_pc)
{
    FILE *fp;
    long file_length_long;
    uint8_t *file_data = NULL;
    size_t file_length;
    size_t form_end;
    size_t cursor;
    const uint8_t *ifhd = NULL;
    size_t ifhd_length = 0U;
    const uint8_t *memory_chunk = NULL;
    size_t memory_length = 0U;
    int memory_is_cmem = 0;
    const uint8_t *stks = NULL;
    size_t stks_length_value = 0U;
    uint8_t *dynamic = NULL;
    uint16_t stack_temp[4096];
    ZMachineFrame frames_temp[ZM_MAX_FRAMES];
    size_t sp_temp = 0U;
    size_t frame_count_temp = 0U;
    uint32_t pc;
    int rc = TCL_ERROR;

    if (!vm || !vm->memory || !path || !path[0] || !saved_pc)
        return quetzal_error(vm, "invalid Quetzal restore request");
    if (!vm->initial_dynamic_memory ||
        vm->initial_dynamic_memory_size != (size_t)vm->static_memory_addr ||
        vm->initial_dynamic_memory_size < 0x40U)
        return quetzal_error(vm, "original dynamic-memory image is unavailable");

    fp = fopen(path, "rb");
    if (!fp)
        return quetzal_error(vm, "unable to open Quetzal save file");

    if (fseek(fp, 0, SEEK_END) != 0 ||
        (file_length_long = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return quetzal_error(vm, "unable to determine Quetzal file size");
    }
    file_length = (size_t)file_length_long;
    if (file_length < QUETZAL_FORM_HEADER) {
        fclose(fp);
        return quetzal_error(vm, "Quetzal file is too small");
    }

    file_data = (uint8_t *)malloc(file_length);
    if (!file_data) {
        fclose(fp);
        return quetzal_error(vm, "out of memory while reading Quetzal file");
    }
    if (fread(file_data, 1U, file_length, fp) != file_length) {
        fclose(fp);
        free(file_data);
        return quetzal_error(vm, "unable to read complete Quetzal file");
    }
    fclose(fp);

    if (memcmp(file_data, "FORM", 4U) != 0 ||
        memcmp(file_data + 8U, "IFZS", 4U) != 0) {
        quetzal_error(vm, "file is not a Quetzal FORM IFZS save");
        goto done;
    }

    if ((uint64_t)read_be32(file_data + 4U) + 8U > file_length) {
        quetzal_error(vm, "Quetzal FORM length exceeds file size");
        goto done;
    }
    form_end = (size_t)read_be32(file_data + 4U) + 8U;
    cursor = QUETZAL_FORM_HEADER;

    while (cursor < form_end) {
        const uint8_t *id;
        uint32_t chunk_length;
        size_t data_start;
        size_t next;

        if (form_end - cursor < 8U) {
            quetzal_error(vm, "truncated Quetzal chunk header");
            goto done;
        }
        id = file_data + cursor;
        chunk_length = read_be32(file_data + cursor + 4U);
        data_start = cursor + 8U;
        if ((uint64_t)data_start + chunk_length > form_end) {
            quetzal_error(vm, "Quetzal chunk exceeds FORM boundary");
            goto done;
        }
        next = data_start + (size_t)chunk_length + (chunk_length & 1U);
        if (next > form_end) {
            quetzal_error(vm, "Quetzal chunk padding exceeds FORM boundary");
            goto done;
        }

        if (!ifhd && memcmp(id, "IFhd", 4U) == 0) {
            ifhd = file_data + data_start;
            ifhd_length = chunk_length;
        } else if (!memory_chunk && memcmp(id, "UMem", 4U) == 0) {
            memory_chunk = file_data + data_start;
            memory_length = chunk_length;
            memory_is_cmem = 0;
        } else if (!memory_chunk && memcmp(id, "CMem", 4U) == 0) {
            memory_chunk = file_data + data_start;
            memory_length = chunk_length;
            memory_is_cmem = 1;
        } else if (!stks && memcmp(id, "Stks", 4U) == 0) {
            stks = file_data + data_start;
            stks_length_value = chunk_length;
        }

        cursor = next;
    }

    if (!ifhd || ifhd_length < QUETZAL_IFHD_LENGTH ||
        !memory_chunk || !stks) {
        quetzal_error(vm, "Quetzal file is missing IFhd, memory, or Stks data");
        goto done;
    }

    if (read_be16(ifhd) != vm->release_number ||
        memcmp(ifhd + 2U, vm->initial_dynamic_memory + 0x12U, 6U) != 0 ||
        read_be16(ifhd + 8U) != vm->checksum) {
        quetzal_error(vm, "Quetzal save belongs to a different story file");
        goto done;
    }

    pc = read_be24(ifhd + 10U);
    if ((size_t)pc >= vm->memory_size) {
        quetzal_error(vm, "Quetzal saved PC is outside story memory");
        goto done;
    }

    dynamic = (uint8_t *)malloc(vm->static_memory_addr);
    if (!dynamic) {
        quetzal_error(vm, "out of memory while restoring Quetzal dynamic memory");
        goto done;
    }

    if (memory_is_cmem) {
        if (decode_cmem(vm, memory_chunk, memory_length, dynamic) != TCL_OK)
            goto done;
    } else {
        if (memory_length != (size_t)vm->static_memory_addr) {
            quetzal_error(vm, "UMem length does not match story dynamic memory");
            goto done;
        }
        memcpy(dynamic, memory_chunk, memory_length);
    }

    memset(stack_temp, 0, sizeof(stack_temp));
    memset(frames_temp, 0, sizeof(frames_temp));
    if (parse_stks(vm, stks, stks_length_value,
                   stack_temp, &sp_temp,
                   frames_temp, &frame_count_temp) != TCL_OK)
        goto done;

    /* Interpreter-owned header values survive or are reset from the live VM. */
    preserve_live_header_fields(vm, vm->memory, dynamic);

    memcpy(vm->memory, dynamic, vm->static_memory_addr);
    memcpy(vm->stack, stack_temp, sp_temp * sizeof(vm->stack[0]));
    memcpy(vm->frames, frames_temp,
           frame_count_temp * sizeof(vm->frames[0]));
    vm->sp = sp_temp;
    vm->frame_count = frame_count_temp;
    vm->pc = pc;
    vm->flags1 = vm->memory[0x01U];
    vm->flags2 = read_be16(vm->memory + 0x10U);
    vm->state = ZM_STATE_READY;
    vm->input_available = 0;
    Tcl_DStringSetLength(&vm->pending_input, 0);
    zmachine_undo_discard(vm);
    vm->error[0] = '\0';
    *saved_pc = pc;
    rc = TCL_OK;

done:
    free(dynamic);
    free(file_data);
    return rc;
}
