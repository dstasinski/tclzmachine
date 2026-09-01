/*
 * zmachine.c
 *
 * Core session lifetime, story-image loading/reset, packed-address helpers, and
 * canonical output buffering for tclzmachine.
 *
 * This module deliberately contains no opcode-dispatch loop. Cooperative
 * execution lives in zmachine_run.c, while instruction semantics are layered
 * through preflight, stream/file, lexical/input, presentation, and core
 * executors. Keeping lifetime and story-image ownership here prevents a second
 * copy of opcode behavior from drifting away from the authoritative run path.
 *
 * The canonical output buffer is presentation-neutral UTF-8. Output stream 3
 * is handled here because it changes the destination of text bytes into story
 * memory; optional mIRC decoration and host word wrapping remain above this
 * module and never alter canonical VM state.
 */

#include "tclzmachine.h"
#include "zmachine_undo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZM_HEADER_SIZE 64U

/* Read a big-endian 16-bit value from story memory. */
static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* Put the VM in its terminal error state with a short diagnostic. */
static void set_error(ZMachine *vm, const char *msg)
{
    vm->state = ZM_STATE_ERROR;
    snprintf(vm->error, sizeof(vm->error), "%s", msg ? msg : "unknown error");
}

/* Validate header fields which are required before execution can begin. */
static int validate_header_layout(ZMachine *vm)
{
    if (vm->static_memory_addr < ZM_HEADER_SIZE ||
        vm->static_memory_addr > vm->memory_size) {
        set_error(vm, "invalid static-memory base in Z-machine header");
        return TCL_ERROR;
    }
    if ((size_t)vm->initial_pc >= vm->memory_size) {
        set_error(vm, "initial program counter is outside the story file");
        return TCL_ERROR;
    }
    if (vm->declared_file_length != 0 &&
        vm->declared_file_length > vm->memory_size) {
        set_error(vm, "story file is shorter than the length declared in its header");
        return TCL_ERROR;
    }
    return TCL_OK;
}

/* Allocate a zeroed interpreter object and initialize Tcl-owned strings. */
ZMachine *zmachine_create(void)
{
    ZMachine *vm = (ZMachine *)calloc(1, sizeof(*vm));
    if (!vm)
        return NULL;

    Tcl_DStringInit(&vm->output);
    Tcl_DStringInit(&vm->pending_input);
    vm->state = ZM_STATE_READY;
    vm->random_state = 1U;
    vm->output_stream1_enabled = 1;
    return vm;
}

/* Release all memory owned by one independent game session. */
void zmachine_destroy(ZMachine *vm)
{
    if (!vm)
        return;

    zmachine_undo_discard(vm);
    free(vm->initial_dynamic_memory);
    free(vm->memory);
    Tcl_DStringFree(&vm->output);
    Tcl_DStringFree(&vm->pending_input);
    free(vm);
}

/*
 * Load a story file, cache the header fields used frequently by the VM, and
 * retain a pristine copy of dynamic memory for restart/save-state work.
 *
 * The loader owns the complete mutable story image. The restart snapshot covers
 * exactly dynamic memory so later restart/verify operations can recover the
 * original story bytes even after interpreter-owned header fields have changed.
 * On any validation/allocation failure no partially loaded story is executable.
 */
int zmachine_load_story(ZMachine *vm, const char *path)
{
    FILE *fp;
    long size;
    uint8_t *buf;
    uint8_t *dynamic_copy;
    uint8_t version;

    if (!vm || !path)
        return TCL_ERROR;

    fp = fopen(path, "rb");
    if (!fp) {
        set_error(vm, "unable to open story file");
        return TCL_ERROR;
    }

    if (fseek(fp, 0, SEEK_END) != 0 ||
        (size = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) != 0) {
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

    /* An undo snapshot belongs only to the story image that created it. */
    zmachine_undo_discard(vm);
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
    vm->declared_file_length =
        zmachine_header_file_length(version, vm->header_file_length_word);
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

    dynamic_copy = (uint8_t *)malloc(vm->static_memory_addr);
    if (!dynamic_copy) {
        free(vm->memory);
        vm->memory = NULL;
        vm->memory_size = 0;
        set_error(vm, "out of memory while snapshotting dynamic memory");
        return TCL_ERROR;
    }
    memcpy(dynamic_copy, vm->memory, vm->static_memory_addr);
    free(vm->initial_dynamic_memory);
    vm->initial_dynamic_memory = dynamic_copy;
    vm->initial_dynamic_memory_size = vm->static_memory_addr;

    return zmachine_reset(vm);
}

/*
 * Reset volatile execution state without changing the currently loaded story
 * image. A Z-machine restart is different: it first restores original dynamic
 * memory and is implemented by the authoritative run layer.
 */
int zmachine_reset(ZMachine *vm)
{
    if (!vm || !vm->memory) {
        if (vm)
            set_error(vm, "no story is loaded");
        return TCL_ERROR;
    }

    zmachine_undo_discard(vm);
    vm->pc = vm->initial_pc;
    vm->sp = 0;
    vm->frame_count = 0;
    vm->state = ZM_STATE_READY;
    vm->input_available = 0;
    vm->error[0] = '\0';
    vm->random_state = 1U;
    vm->current_window = 0U;
    vm->output_stream1_enabled = 1;
    vm->stream3_depth = 0U;
    memset(vm->stream3_tables, 0, sizeof(vm->stream3_tables));
    Tcl_DStringSetLength(&vm->output, 0);
    Tcl_DStringSetLength(&vm->pending_input, 0);
    zmachine_refresh_interpreter_header(vm);
    return TCL_OK;
}

/*
 * Queue one host line for the next cooperative input request.
 *
 * Host-character validation is layered above this base implementation. The
 * queued Tcl_DString is interpreter-owned and is consumed only after the input
 * subsystem has successfully committed the story's text/parse buffers.
 */
int zmachine_supply_input(ZMachine *vm, const char *line)
{
    if (!vm || !line)
        return TCL_ERROR;
    if (vm->state == ZM_STATE_HALTED || vm->state == ZM_STATE_ERROR)
        return TCL_ERROR;

    Tcl_DStringSetLength(&vm->pending_input, 0);
    Tcl_DStringAppend(&vm->pending_input, line, -1);
    vm->input_available = 1;
    if (vm->state == ZM_STATE_WAITING_INPUT)
        vm->state = ZM_STATE_READY;
    return TCL_OK;
}

/* Convert a packed routine address according to the loaded story version. */
uint32_t zmachine_unpack_routine_address(const ZMachine *vm, uint16_t packed)
{
    if (!vm)
        return 0U;
    return zmachine_unpack_address(vm->version, ZM_ADDR_ROUTINE, packed,
                                   vm->routine_offset, vm->string_offset);
}

/* Convert a packed string address according to the loaded story version. */
uint32_t zmachine_unpack_string_address(const ZMachine *vm, uint16_t packed)
{
    if (!vm)
        return 0U;
    return zmachine_unpack_address(vm->version, ZM_ADDR_STRING, packed,
                                   vm->routine_offset, vm->string_offset);
}

/* Clear text accumulated during the current Tcl command. */
void zmachine_output_clear(ZMachine *vm)
{
    if (vm)
        Tcl_DStringSetLength(&vm->output, 0);
}

/*
 * Append one ZSCII byte to the innermost active stream-3 memory table.
 *
 * The story owns the table capacity, as required by the Z-machine standard,
 * but the host runtime must still prevent an invalid story from writing beyond
 * the mapped story image or into static memory. The count word is maintained
 * continuously; although its contents are unspecified while the stream is
 * active, this makes nested stream resumption straightforward and leaves the
 * required final count in place as soon as the stream is deselected.
 */
static int output_stream3_append_byte(ZMachine *vm, uint8_t zscii)
{
    size_t table;
    size_t address;
    uint16_t count;

    if (!vm || !vm->memory || vm->stream3_depth == 0U)
        return TCL_ERROR;

    table = vm->stream3_tables[vm->stream3_depth - 1U];
    if (table + 1U >= vm->memory_size ||
        table + 1U >= (size_t)vm->static_memory_addr) {
        set_error(vm, "output stream 3 table is outside dynamic memory");
        return TCL_ERROR;
    }

    count = read_be16(vm->memory + table);
    if (count == 0xffffU) {
        set_error(vm, "output stream 3 character count overflow");
        return TCL_ERROR;
    }

    address = table + 2U + (size_t)count;
    if (address >= vm->memory_size ||
        address >= (size_t)vm->static_memory_addr) {
        set_error(vm, "output stream 3 write exceeds dynamic memory");
        return TCL_ERROR;
    }

    vm->memory[address] = zscii;
    ++count;
    vm->memory[table] = (uint8_t)(count >> 8);
    vm->memory[table + 1U] = (uint8_t)(count & 0xffU);
    return TCL_OK;
}

/*
 * Append canonical story text at the interpreter output boundary.
 *
 * Stream 3 has precedence over every other selected output stream. Canonical
 * newlines are converted back to ZSCII 13 for the memory table; printable ASCII
 * is stored directly. Generic non-ASCII UTF-8 appended outside the ZSCII-aware
 * text module becomes one '?' in stream 3; print_char and print_unicode use the
 * text module and therefore preserve/reverse-map their proper ZSCII values.
 *
 * When stream 3 is inactive, only stream 1 output for lower window 0 reaches the
 * Tcl-facing canonical buffer. Upper-window/status text is presentation-only in
 * this IRC runtime and is discarded while the story keeps its selected window.
 */
void zmachine_output_append(ZMachine *vm, const char *text, size_t len)
{
    if (!vm || !text || len == 0U)
        return;

    if (vm->stream3_depth > 0U) {
        size_t i = 0U;

        while (i < len && vm->state != ZM_STATE_ERROR) {
            unsigned char ch = (unsigned char)text[i];
            uint8_t zscii;

            if (ch == (unsigned char)'\n') {
                zscii = 13U;
                ++i;
            } else if (ch >= 32U && ch <= 126U) {
                zscii = ch;
                ++i;
            } else if (ch < 0x80U) {
                zscii = (uint8_t)'?';
                ++i;
            } else {
                zscii = (uint8_t)'?';
                ++i;
                while (i < len &&
                       (((unsigned char)text[i] & 0xc0U) == 0x80U))
                    ++i;
            }

            if (output_stream3_append_byte(vm, zscii) != TCL_OK)
                return;
        }
        return;
    }

    if (!vm->output_stream1_enabled || vm->current_window != 0U)
        return;

    Tcl_DStringAppend(&vm->output, text, (int)len);
}

/* Return the canonical output buffer as UTF-8 text. */
const char *zmachine_output_data(const ZMachine *vm)
{
    return vm ? Tcl_DStringValue((Tcl_DString *)&vm->output) : "";
}

/* Return the number of bytes currently stored in the output buffer. */
int zmachine_output_length(const ZMachine *vm)
{
    return vm ? Tcl_DStringLength((Tcl_DString *)&vm->output) : 0;
}

/* Return the last interpreter error, or a fixed message for a null VM. */
const char *zmachine_last_error(const ZMachine *vm)
{
    return vm ? vm->error : "invalid VM";
}
