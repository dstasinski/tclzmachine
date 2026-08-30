/*
 * zmachine.c
 *
 * Story loading, per-session lifetime management, the cooperative execution
 * loop, and a small set of interpreter-level opcodes which are best handled
 * outside the ordinary instruction executor. The run loop stops whenever a
 * story asks for line input so Tcl/IRC code never blocks on a terminal.
 */

#include "tclzmachine.h"
#include "zmachine_decode.h"
#include "zmachine_exec.h"
#include "zmachine_input.h"
#include "zmachine_undo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ZM_HEADER_SIZE 64U
#define ZM_RUN_STEP_LIMIT 1000000U

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

/*
 * Decode and apply a Z-machine branch record.
 *
 * Branch data follows the operands (and any store variable) rather than being
 * part of the instruction decoder's operand list. Offsets 0 and 1 are special
 * and mean "return false" and "return true" from the current routine.
 */
static int apply_branch(ZMachine *vm, uint32_t branch_pc, int condition)
{
    uint8_t first;
    int branch_on_true;
    int32_t offset;
    uint32_t after;

    if ((size_t)branch_pc >= vm->memory_size) {
        set_error(vm, "truncated Z-machine branch");
        return TCL_ERROR;
    }

    first = vm->memory[branch_pc];
    branch_on_true = (first & 0x80U) != 0U;

    if (first & 0x40U) {
        offset = (int32_t)(first & 0x3fU);
        after = branch_pc + 1U;
    } else {
        uint16_t raw;
        if ((size_t)branch_pc + 1U >= vm->memory_size) {
            set_error(vm, "truncated Z-machine branch");
            return TCL_ERROR;
        }
        raw = (uint16_t)(((uint16_t)(first & 0x3fU) << 8) |
                         vm->memory[branch_pc + 1U]);
        if (raw & 0x2000U)
            raw |= 0xc000U;
        offset = (int16_t)raw;
        after = branch_pc + 2U;
    }

    if (!!condition != !!branch_on_true) {
        vm->pc = after;
        return TCL_OK;
    }

    if (offset == 0)
        return zmachine_return(vm, 0U);
    if (offset == 1)
        return zmachine_return(vm, 1U);

    vm->pc = (uint32_t)((int32_t)after + offset - 2);
    if ((size_t)vm->pc >= vm->memory_size) {
        set_error(vm, "Z-machine branch target is outside story memory");
        return TCL_ERROR;
    }
    return TCL_OK;
}

/* Store an opcode result and return the address immediately following it. */
static int store_result(ZMachine *vm, uint32_t store_pc,
                        uint16_t value, uint32_t *next_pc)
{
    uint8_t variable;

    if ((size_t)store_pc >= vm->memory_size) {
        set_error(vm, "truncated Z-machine store variable");
        return TCL_ERROR;
    }
    variable = vm->memory[store_pc];
    if (zmachine_variable_write(vm, variable, 0, value) != TCL_OK)
        return TCL_ERROR;
    if (next_pc)
        *next_pc = store_pc + 1U;
    return TCL_OK;
}

/* Compute the story-file checksum used by the verify opcode. */
static int story_checksum_matches(const ZMachine *vm)
{
    size_t end;
    size_t i;
    uint32_t sum = 0U;

    if (!vm || !vm->memory || vm->memory_size <= ZM_HEADER_SIZE)
        return 0;

    end = vm->declared_file_length != 0 ?
          vm->declared_file_length : vm->memory_size;
    if (end > vm->memory_size)
        end = vm->memory_size;

    for (i = ZM_HEADER_SIZE; i < end; ++i)
        sum += vm->memory[i];

    return (uint16_t)sum == vm->checksum;
}

/*
 * Simple per-session pseudo-random generator.
 *
 * The Z-machine standard specifies observable seeding and result ranges but
 * deliberately does not mandate a host PRNG algorithm. An LCG is sufficient
 * here, deterministic after a negative seed and independent between sessions.
 */
static uint16_t random_result(ZMachine *vm, int16_t range)
{
    if (range < 0) {
        uint32_t seed = (uint32_t)(-(int32_t)range);
        vm->random_state = seed ? seed : 1U;
        return 0U;
    }

    if (range == 0) {
        uint32_t seed = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)vm;
        vm->random_state = seed ? seed : 1U;
        return 0U;
    }

    vm->random_state = vm->random_state * 1103515245U + 12345U;
    return (uint16_t)(1U + (vm->random_state % (uint16_t)range));
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

/* Reset volatile execution state without changing story memory. */
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
    return TCL_OK;
}

/* Queue a line of player input for the next read instruction. */
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

/*
 * Handle the line-oriented read opcode at the cooperative run-loop boundary.
 * This is deliberately outside the ordinary executor because an IRC runtime
 * must suspend rather than blocking while waiting for a terminal keyboard.
 *
 * V5+ permits the parse buffer to be zero, which suppresses lexical analysis.
 * Some Infocom code encodes that case by omitting the trailing parse operand
 * entirely. For compatibility, a one-operand V5+ read is treated exactly as
 * though an explicit zero parse-buffer operand had been supplied.
 */
static int handle_read_opcode(ZMachine *vm, int *handled)
{
    ZMachineInstruction instruction;
    uint16_t values[ZM_MAX_OPERANDS];
    uint16_t text_buffer;
    uint16_t parse_buffer;
    uint16_t terminator = 13U;
    char decode_error[128];
    uint32_t next_pc;

    *handled = 0;
    if (!zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                     vm->pc, &instruction,
                                     decode_error, sizeof(decode_error))) {
        set_error(vm, decode_error[0] ? decode_error :
                  "unable to decode Z-machine instruction");
        return TCL_ERROR;
    }

    if (instruction.operand_count != ZM_OPERANDS_VAR ||
        instruction.opcode_number != 4U)
        return TCL_OK;

    *handled = 1;
    if (!vm->input_available) {
        vm->state = ZM_STATE_WAITING_INPUT;
        return TCL_OK;
    }

    if (instruction.operand_count_actual < 1U ||
        (vm->version <= 4U && instruction.operand_count_actual < 2U)) {
        set_error(vm, "read opcode is missing required buffer operands");
        return TCL_ERROR;
    }

    if (zmachine_resolve_operands(vm, &instruction, values,
                                  ZM_MAX_OPERANDS) != TCL_OK)
        return TCL_ERROR;

    text_buffer = values[0];
    parse_buffer = instruction.operand_count_actual >= 2U ? values[1] : 0U;

    if (zmachine_input_read_line(vm, text_buffer, parse_buffer, &terminator) != TCL_OK) {
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

/*
 * Handle small interpreter-level compatibility opcodes before normal dispatch.
 * These operations either affect the whole VM (restart/verify/random) or are
 * intentionally presentation-neutral in this text-only implementation.
 */
static int handle_core_opcode(ZMachine *vm, int *handled)
{
    ZMachineInstruction instruction;
    uint16_t values[ZM_MAX_OPERANDS];
    char decode_error[128];

    *handled = 0;
    if (!zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                     vm->pc, &instruction,
                                     decode_error, sizeof(decode_error))) {
        set_error(vm, decode_error[0] ? decode_error :
                  "unable to decode Z-machine instruction");
        return TCL_ERROR;
    }

    if (instruction.operand_count == ZM_OPERANDS_0OP) {
        switch (instruction.opcode_number) {
        case 7U: { /* restart */
            uint16_t preserved_flags2;

            *handled = 1;
            if (!vm->initial_dynamic_memory ||
                vm->initial_dynamic_memory_size != vm->static_memory_addr) {
                set_error(vm, "restart image is unavailable");
                return TCL_ERROR;
            }

            preserved_flags2 = read_be16(vm->memory + 0x10U);
            memcpy(vm->memory, vm->initial_dynamic_memory,
                   vm->initial_dynamic_memory_size);
            vm->memory[0x10U] = (uint8_t)(preserved_flags2 >> 8);
            vm->memory[0x11U] = (uint8_t)preserved_flags2;
            vm->flags2 = preserved_flags2;

            zmachine_undo_discard(vm);
            vm->pc = vm->initial_pc;
            vm->sp = 0U;
            vm->frame_count = 0U;
            vm->input_available = 0;
            vm->random_state = 1U;
            vm->current_window = 0U;
            vm->output_stream1_enabled = 1;
            vm->stream3_depth = 0U;
            memset(vm->stream3_tables, 0, sizeof(vm->stream3_tables));
            Tcl_DStringSetLength(&vm->pending_input, 0);
            return TCL_OK;
        }

        case 9U: /* pop in V1-V4; V5+ uses catch instead. */
            if (vm->version <= 4U) {
                uint16_t ignored;
                *handled = 1;
                if (zmachine_stack_pop(vm, &ignored) != TCL_OK)
                    return TCL_ERROR;
                vm->pc = instruction.next_pc;
            }
            return TCL_OK;

        case 12U: /* show_status -- presentation-only in V3. */
            if (vm->version == 3U) {
                *handled = 1;
                vm->pc = instruction.next_pc;
            }
            return TCL_OK;

        case 13U: /* verify */
            *handled = 1;
            return apply_branch(vm, instruction.next_pc,
                                story_checksum_matches(vm));

        case 15U: /* piracy -- standard asks interpreters to always branch. */
            if (vm->version >= 5U) {
                *handled = 1;
                return apply_branch(vm, instruction.next_pc, 1);
            }
            return TCL_OK;

        default:
            return TCL_OK;
        }
    }

    if (instruction.operand_count != ZM_OPERANDS_VAR)
        return TCL_OK;

    /*
     * Only resolve operands for opcodes owned by this handler. Variable
     * operands can pop stack variable 0, so resolving an unhandled opcode here
     * and then again in zmachine_step() would corrupt the evaluation stack.
     */
    if (instruction.opcode_number != 7U &&
        instruction.opcode_number != 23U &&
        instruction.opcode_number != 31U)
        return TCL_OK;

    if (zmachine_resolve_operands(vm, &instruction, values,
                                  ZM_MAX_OPERANDS) != TCL_OK)
        return TCL_ERROR;

    switch (instruction.opcode_number) {
    case 7U: { /* random range -> result */
        uint32_t next_pc;
        if (instruction.operand_count_actual < 1U)
            return TCL_OK;
        *handled = 1;
        if (store_result(vm, instruction.next_pc,
                         random_result(vm, (int16_t)values[0]),
                         &next_pc) != TCL_OK)
            return TCL_ERROR;
        vm->pc = next_pc;
        return TCL_OK;
    }

    case 23U: { /* scan_table x table len [form] -> result ?branch */
        uint16_t form = instruction.operand_count_actual >= 4U ? values[3] : 0x82U;
        uint16_t result = 0U;
        uint16_t i;
        uint32_t store_pc;
        uint32_t field_size;
        int words;

        if (vm->version < 4U || instruction.operand_count_actual < 3U)
            return TCL_OK;
        *handled = 1;
        words = (form & 0x80U) != 0U;
        field_size = form & 0x7fU;
        if (field_size == 0U) {
            set_error(vm, "scan_table field size is zero");
            return TCL_ERROR;
        }

        for (i = 0U; i < values[2]; ++i) {
            uint32_t address = (uint32_t)values[1] + (uint32_t)i * field_size;
            uint16_t candidate;

            if (words) {
                if ((size_t)address + 1U >= vm->memory_size) {
                    set_error(vm, "scan_table reads outside story memory");
                    return TCL_ERROR;
                }
                candidate = read_be16(vm->memory + address);
            } else {
                if ((size_t)address >= vm->memory_size) {
                    set_error(vm, "scan_table reads outside story memory");
                    return TCL_ERROR;
                }
                candidate = vm->memory[address];
            }

            if (candidate == values[0]) {
                result = (uint16_t)address;
                break;
            }
        }

        if (store_result(vm, instruction.next_pc, result, &store_pc) != TCL_OK)
            return TCL_ERROR;
        return apply_branch(vm, store_pc, result != 0U);
    }

    case 31U: { /* check_arg_count argument-number ?branch */
        const ZMachineFrame *frame;
        uint16_t argument_number;
        int supplied = 0;

        if (vm->version < 5U || instruction.operand_count_actual < 1U)
            return TCL_OK;
        *handled = 1;
        argument_number = values[0];
        frame = zmachine_current_frame_const(vm);
        if (frame && argument_number >= 1U && argument_number <= 7U)
            supplied = (frame->argument_mask &
                        (uint8_t)(1U << (argument_number - 1U))) != 0U;
        return apply_branch(vm, instruction.next_pc, supplied);
    }

    default:
        return TCL_OK;
    }
}

/*
 * Execute synchronously until the story halts, errors, or requests another
 * line of player input. A generous instruction cap protects host applications
 * from a malformed or deliberately non-yielding story file.
 */
int zmachine_run(ZMachine *vm)
{
    unsigned long steps = 0UL;

    if (!vm || !vm->memory) {
        if (vm)
            set_error(vm, "no story is loaded");
        return TCL_ERROR;
    }
    if (vm->state == ZM_STATE_ERROR)
        return TCL_ERROR;
    if (vm->state == ZM_STATE_HALTED)
        return TCL_OK;

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

        if (handle_core_opcode(vm, &handled) != TCL_OK)
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
 * is stored directly. A non-ASCII UTF-8 codepoint currently becomes one '?',
 * matching this runtime's existing fallback until the full ZSCII/Unicode table
 * is implemented. When stream 3 is inactive, output reaches the Tcl-facing
 * canonical buffer only while stream 1 is selected.
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

    if (!vm->output_stream1_enabled)
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
