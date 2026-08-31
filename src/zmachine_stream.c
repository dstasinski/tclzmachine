/*
 * zmachine_stream.c
 *
 * External command/replay and transcript stream support for the embedded
 * Z-machine runtime.
 *
 * Z-machine input stream 1 reads commands from a host file whose format is the
 * same as output stream 4. Output stream 2 receives story text plus completed
 * line input, while output stream 4 records completed commands and read_char
 * keypresses. The Z-machine deliberately does not choose host filenames, so a
 * first selection of one of these streams yields with
 * ZM_STATE_WAITING_STREAM_FILE; Tcl then supplies a path through
 * zmachine_stream_file().
 *
 * Command files use the standards-suggested textual convention. Ordinary line
 * commands occupy one physical line. A non-Enter terminating key is appended as
 * [N], and read_char events are written as a line containing [N]. This makes the
 * writer and replay reader exactly round-trip compatible while remaining easy
 * to inspect and edit by hand.
 */

#include "tclzmachine.h"
#include "zmachine_decode.h"
#include "zmachine_exec.h"
#include "zmachine_stream.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Public implementations immediately below this host-stream wrapper. */
extern int zmachine_step_stream_base(ZMachine *vm);
extern int zmachine_run_stream_base(ZMachine *vm);
extern void zmachine_output_append_stream_base(ZMachine *vm,
                                                const char *text,
                                                size_t len);
extern int zmachine_input_read_line_stream_base(ZMachine *vm,
                                                 uint16_t text_buffer,
                                                 uint16_t parse_buffer,
                                                 uint16_t *terminator);
extern void zmachine_destroy_stream_base(ZMachine *vm);
extern int zmachine_load_story_stream_base(ZMachine *vm, const char *path);
extern int zmachine_reset_stream_base(ZMachine *vm);

typedef enum StreamRequestKind {
    STREAM_REQUEST_NONE = 0,
    STREAM_REQUEST_REPLAY,
    STREAM_REQUEST_TRANSCRIPT,
    STREAM_REQUEST_RECORD
} StreamRequestKind;

/*
 * Internal result from prepare_replay_input(). Keep successful queuing distinct
 * from Tcl's public status values: TCL_ERROR is numerically 1, so using 1 as a
 * private "queued" sentinel would make every successfully read replay record
 * look like an interpreter error to the run wrapper.
 */
typedef enum ReplayPrepareResult {
    REPLAY_PREPARE_EOF = 0,
    REPLAY_PREPARE_ERROR = TCL_ERROR,
    REPLAY_PREPARE_QUEUED = 2
} ReplayPrepareResult;

struct ZMachineStreamIO {
    FILE *replay;
    FILE *transcript;
    FILE *record;
    uint8_t input_stream;
    int record_selected;
    StreamRequestKind pending_request;
    uint32_t pending_next_pc;
};

/* Record an API/file diagnostic without poisoning a retryable VM session. */
static int stream_api_error(ZMachine *vm, const char *message)
{
    if (vm)
        snprintf(vm->error, sizeof(vm->error), "%s",
                 message ? message : "stream file operation failed");
    return TCL_ERROR;
}

/* Malformed executing Z-code is terminal, unlike a host filename error. */
static int stream_vm_error(ZMachine *vm, const char *message)
{
    if (vm) {
        vm->state = ZM_STATE_ERROR;
        snprintf(vm->error, sizeof(vm->error), "%s",
                 message ? message : "invalid Z-machine stream operation");
    }
    return TCL_ERROR;
}

static ZMachineStreamIO *ensure_stream_io(ZMachine *vm)
{
    if (!vm)
        return NULL;
    if (!vm->stream_io)
        vm->stream_io = (ZMachineStreamIO *)calloc(1U, sizeof(*vm->stream_io));
    return vm->stream_io;
}

static void close_file(FILE **fp)
{
    if (fp && *fp) {
        fclose(*fp);
        *fp = NULL;
    }
}

static void close_stream_io(ZMachine *vm)
{
    ZMachineStreamIO *io;

    if (!vm || !vm->stream_io)
        return;
    io = vm->stream_io;
    close_file(&io->replay);
    close_file(&io->transcript);
    close_file(&io->record);
    free(io);
    vm->stream_io = NULL;
}

static int stream2_selected(const ZMachine *vm)
{
    if (!vm || !vm->memory || vm->memory_size <= 0x11U)
        return 0;
    return (vm->memory[0x11U] & 0x01U) != 0U;
}

static void select_stream2(ZMachine *vm, int selected)
{
    uint16_t flags;

    if (!vm || !vm->memory || vm->memory_size <= 0x11U)
        return;
    flags = (uint16_t)(((uint16_t)vm->memory[0x10U] << 8) |
                       vm->memory[0x11U]);
    if (selected)
        flags |= 0x0001U;
    else
        flags &= (uint16_t)~0x0001U;
    vm->memory[0x10U] = (uint8_t)(flags >> 8);
    vm->memory[0x11U] = (uint8_t)flags;
    vm->flags2 = flags;
}

static const char *request_name(StreamRequestKind kind)
{
    switch (kind) {
    case STREAM_REQUEST_REPLAY:
        return "replay";
    case STREAM_REQUEST_TRANSCRIPT:
        return "transcript";
    case STREAM_REQUEST_RECORD:
        return "record";
    default:
        return "";
    }
}

static StreamRequestKind request_from_name(const char *name)
{
    if (!name)
        return STREAM_REQUEST_NONE;
    if (strcmp(name, "replay") == 0)
        return STREAM_REQUEST_REPLAY;
    if (strcmp(name, "transcript") == 0)
        return STREAM_REQUEST_TRANSCRIPT;
    if (strcmp(name, "record") == 0)
        return STREAM_REQUEST_RECORD;
    return STREAM_REQUEST_NONE;
}

static int begin_request(ZMachine *vm, StreamRequestKind kind,
                         uint32_t next_pc)
{
    ZMachineStreamIO *io = ensure_stream_io(vm);

    if (!io)
        return stream_vm_error(vm, "out of memory while selecting external stream");
    io->pending_request = kind;
    io->pending_next_pc = next_pc;
    vm->state = ZM_STATE_WAITING_STREAM_FILE;
    return TCL_OK;
}

/*
 * Request a transcript path after an executing story selects Flags 2 bit 0.
 *
 * V1/V2 can select stream 2 only through Flags 2, and later versions may use
 * either Flags 2 or output_stream 2. The public step wrapper calls this only
 * after observing a real 0-to-1 transition caused by a delegated instruction.
 * That distinction is important: restored/restarted interpreter state may
 * already have bit 0 set and, by invariant, already has its host transcript
 * resource. Treating every pre-existing bit as a fresh selection would also
 * make synthetic VM-state tests spuriously request host filesystem policy.
 */
static int request_transcript_if_needed(ZMachine *vm)
{
    ZMachineStreamIO *io;

    if (!vm || !stream2_selected(vm) ||
        vm->state == ZM_STATE_HALTED || vm->state == ZM_STATE_ERROR)
        return TCL_OK;

    io = ensure_stream_io(vm);
    if (!io)
        return stream_vm_error(vm,
                               "out of memory while selecting transcript stream");
    if (io->transcript || io->pending_request != STREAM_REQUEST_NONE)
        return TCL_OK;

    return begin_request(vm, STREAM_REQUEST_TRANSCRIPT, vm->pc);
}

const char *zmachine_pending_stream_request(const ZMachine *vm)
{
    return (vm && vm->stream_io) ?
           request_name(vm->stream_io->pending_request) : "";
}

int zmachine_current_input_stream(const ZMachine *vm)
{
    return (vm && vm->stream_io) ? vm->stream_io->input_stream : 0;
}

int zmachine_command_recording_selected(const ZMachine *vm)
{
    return vm && vm->stream_io && vm->stream_io->record_selected;
}

/*
 * Reset interpreter-only stream selections after the restart opcode.
 *
 * Restart restores VM state rather than host resources. The Standard preserves
 * only Flags 2 bits 0 and 1, so stream-1 command replay and output stream 4 must
 * return to their default unselected state. Open host files stay associated with
 * the session: a later story selection can resume them without asking Tcl for a
 * second path, matching the Standard's one-transcript-file-per-session guidance.
 * Transcript selection itself is represented by the preserved Flags 2 bit and
 * is therefore deliberately left untouched here.
 */
void zmachine_stream_after_restart(ZMachine *vm)
{
    ZMachineStreamIO *io;

    if (!vm || !vm->stream_io)
        return;

    io = vm->stream_io;
    io->input_stream = 0U;
    io->record_selected = 0;
    io->pending_request = STREAM_REQUEST_NONE;
    io->pending_next_pc = 0U;
    if (io->record)
        fflush(io->record);
}

/* Open/configure one host stream and, when applicable, resume its opcode. */
int zmachine_stream_file(ZMachine *vm, const char *kind_name, const char *path)
{
    StreamRequestKind kind;
    ZMachineStreamIO *io;
    FILE *fp;
    const char *mode;

    if (!vm || !path || !path[0])
        return stream_api_error(vm, "invalid stream file path");
    kind = request_from_name(kind_name);
    if (kind == STREAM_REQUEST_NONE)
        return stream_api_error(vm, "stream file kind must be replay, transcript, or record");

    io = ensure_stream_io(vm);
    if (!io)
        return stream_api_error(vm, "out of memory while configuring stream file");
    if (io->pending_request != STREAM_REQUEST_NONE &&
        io->pending_request != kind)
        return stream_api_error(vm, "stream file kind does not match the pending request");

    mode = kind == STREAM_REQUEST_REPLAY ? "rb" : "wb";
    fp = fopen(path, mode);
    if (!fp)
        return stream_api_error(vm, "unable to open requested stream file");

    if (kind == STREAM_REQUEST_REPLAY) {
        close_file(&io->replay);
        io->replay = fp;
    } else if (kind == STREAM_REQUEST_TRANSCRIPT) {
        close_file(&io->transcript);
        io->transcript = fp;
    } else {
        close_file(&io->record);
        io->record = fp;
    }

    if (io->pending_request == kind) {
        if (kind == STREAM_REQUEST_REPLAY)
            io->input_stream = 1U;
        else if (kind == STREAM_REQUEST_TRANSCRIPT)
            select_stream2(vm, 1);
        else
            io->record_selected = 1;

        vm->pc = io->pending_next_pc;
        io->pending_request = STREAM_REQUEST_NONE;
        io->pending_next_pc = 0U;
        vm->state = ZM_STATE_READY;
    }

    vm->error[0] = '\0';
    return TCL_OK;
}

int zmachine_cancel_stream_file(ZMachine *vm)
{
    ZMachineStreamIO *io;

    if (!vm || !vm->stream_io ||
        vm->stream_io->pending_request == STREAM_REQUEST_NONE ||
        vm->state != ZM_STATE_WAITING_STREAM_FILE)
        return stream_api_error(vm, "no external stream file request is pending");

    io = vm->stream_io;
    if (io->pending_request == STREAM_REQUEST_REPLAY)
        io->input_stream = 0U;
    else if (io->pending_request == STREAM_REQUEST_TRANSCRIPT)
        select_stream2(vm, 0);
    else if (io->pending_request == STREAM_REQUEST_RECORD)
        io->record_selected = 0;

    vm->pc = io->pending_next_pc;
    io->pending_request = STREAM_REQUEST_NONE;
    io->pending_next_pc = 0U;
    vm->state = ZM_STATE_READY;
    vm->error[0] = '\0';
    return TCL_OK;
}

/* Write bytes to an external stream and turn host I/O failures into VM errors. */
static int write_external(ZMachine *vm, FILE *fp,
                          const void *data, size_t len,
                          const char *what)
{
    if (!fp || len == 0U)
        return TCL_OK;
    if (fwrite(data, 1U, len, fp) != len || fflush(fp) != 0)
        return stream_vm_error(vm, what);
    return TCL_OK;
}

static int write_decimal_marker(ZMachine *vm, FILE *fp, uint16_t value)
{
    char marker[16];
    int length = snprintf(marker, sizeof(marker), "[%u]", (unsigned)value);

    if (length < 0 || (size_t)length >= sizeof(marker))
        return stream_vm_error(vm, "unable to format command-file key record");
    return write_external(vm, fp, marker, (size_t)length,
                          "unable to write command recording file");
}

/*
 * Record one completed command and perform the read-input echo side effects.
 *
 * `echo` contains only newly accepted host input, preserving its display case.
 * V5+ preloaded text is intentionally absent because the Standard says the game,
 * not the interpreter, redisplays that prefix. Stream 4 is different: it records
 * the completed logical command, so it uses the final lower-cased story buffer.
 * A non-Enter terminating key finishes a read without synthesizing a carriage
 * return on streams 1/2. Stream 3 suppresses those screen/transcript echoes but
 * does not suppress command recording, which records input rather than story
 * output.
 */
static int note_line_input(ZMachine *vm, uint16_t text_buffer,
                           uint16_t terminator,
                           const char *echo, size_t echo_length)
{
    ZMachineStreamIO *io = vm ? vm->stream_io : NULL;
    const uint8_t *text;
    size_t length = 0U;
    size_t start;

    if (!vm || !vm->memory)
        return TCL_ERROR;

    if (vm->version <= 4U) {
        start = (size_t)text_buffer + 1U;
        while (start + length < vm->memory_size &&
               vm->memory[start + length] != 0U)
            ++length;
    } else {
        if ((size_t)text_buffer + 1U >= vm->memory_size)
            return TCL_ERROR;
        start = (size_t)text_buffer + 2U;
        length = vm->memory[(size_t)text_buffer + 1U];
    }
    if (start + length > vm->memory_size)
        return TCL_ERROR;
    text = vm->memory + start;

    if (vm->stream3_depth == 0U) {
        static const char newline = '\n';

        if (echo && echo_length > 0U) {
            zmachine_output_append(vm, echo, echo_length);
            if (vm->state == ZM_STATE_ERROR)
                return TCL_ERROR;
        }
        if (terminator == 13U) {
            zmachine_output_append(vm, &newline, 1U);
            if (vm->state == ZM_STATE_ERROR)
                return TCL_ERROR;
        }
    }

    if (io && io->record && io->record_selected) {
        static const char newline = '\n';
        if (write_external(vm, io->record, text, length,
                           "unable to write command recording file") != TCL_OK)
            return TCL_ERROR;
        if (terminator != 13U &&
            write_decimal_marker(vm, io->record, terminator) != TCL_OK)
            return TCL_ERROR;
        if (write_external(vm, io->record, &newline, 1U,
                           "unable to write command recording file") != TCL_OK)
            return TCL_ERROR;
    }

    return TCL_OK;
}

static int note_key_input(ZMachine *vm, uint16_t zscii)
{
    ZMachineStreamIO *io = vm ? vm->stream_io : NULL;
    static const char newline = '\n';

    if (!io || !io->record || !io->record_selected)
        return TCL_OK;
    if (write_decimal_marker(vm, io->record, zscii) != TCL_OK)
        return TCL_ERROR;
    return write_external(vm, io->record, &newline, 1U,
                          "unable to write command recording file");
}

/* Find a standards-style trailing [N] key marker and remove it from the line. */
static int parse_trailing_marker(char *line, uint16_t *value, int *present)
{
    size_t len;
    char *open;
    unsigned long parsed = 0UL;
    char *p;

    *present = 0;
    if (!line || !value)
        return 0;
    len = strlen(line);
    if (len < 3U || line[len - 1U] != ']')
        return 1;

    open = strrchr(line, '[');
    if (!open || open == line + len - 2U)
        return 1;
    for (p = open + 1; p < line + len - 1U; ++p) {
        if (!isdigit((unsigned char)*p))
            return 1;
        parsed = parsed * 10UL + (unsigned long)(*p - '0');
        if (parsed > 255UL)
            return 0;
    }

    *open = '\0';
    *value = (uint16_t)parsed;
    *present = 1;
    return 1;
}

static int current_input_opcode(const ZMachine *vm,
                                ZMachineInstruction *instruction)
{
    char decode_error[128];

    if (!vm || !vm->memory || vm->state != ZM_STATE_WAITING_INPUT)
        return 0;
    if (!zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                     vm->pc, instruction,
                                     decode_error, sizeof(decode_error)))
        return 0;
    if (instruction->form != ZM_FORM_VARIABLE ||
        instruction->operand_count != ZM_OPERANDS_VAR)
        return 0;
    return instruction->opcode_number == 4U ||
           instruction->opcode_number == 22U;
}

/* Queue the next replay record for the currently suspended read/read_char. */
static int prepare_replay_input(ZMachine *vm)
{
    ZMachineStreamIO *io;
    ZMachineInstruction instruction;
    char line[4096];
    size_t len;
    uint16_t marker = 13U;
    int has_marker = 0;
    size_t i;

    if (!vm || !vm->stream_io || vm->stream_io->input_stream != 1U)
        return REPLAY_PREPARE_EOF;
    io = vm->stream_io;
    if (!io->replay) {
        io->input_stream = 0U;
        return REPLAY_PREPARE_EOF;
    }
    if (!current_input_opcode(vm, &instruction)) {
        stream_vm_error(vm, "command replay reached an unknown input wait");
        return REPLAY_PREPARE_ERROR;
    }

    if (!fgets(line, (int)sizeof(line), io->replay)) {
        if (ferror(io->replay)) {
            stream_vm_error(vm, "unable to read command replay file");
            return REPLAY_PREPARE_ERROR;
        }
        close_file(&io->replay);
        io->input_stream = 0U;
        return REPLAY_PREPARE_EOF; /* EOF returns input to the keyboard host. */
    }

    len = strlen(line);
    if (len == sizeof(line) - 1U && line[len - 1U] != '\n') {
        stream_vm_error(vm, "command replay line is too long");
        return REPLAY_PREPARE_ERROR;
    }
    while (len > 0U && (line[len - 1U] == '\n' || line[len - 1U] == '\r'))
        line[--len] = '\0';

    if (!parse_trailing_marker(line, &marker, &has_marker)) {
        stream_vm_error(vm, "invalid numeric marker in command replay file");
        return REPLAY_PREPARE_ERROR;
    }

    if (instruction.opcode_number == 22U) {
        uint16_t key;
        if (has_marker && line[0] == '\0') {
            key = marker;
        } else if (!has_marker && strlen(line) == 1U &&
                   (unsigned char)line[0] >= 32U &&
                   (unsigned char)line[0] <= 126U) {
            key = (uint8_t)line[0];
        } else {
            stream_vm_error(vm,
                            "read_char replay record must contain one character or [N]");
            return REPLAY_PREPARE_ERROR;
        }
        if (zmachine_supply_key(vm, key) != TCL_OK)
            return REPLAY_PREPARE_ERROR;
        return REPLAY_PREPARE_QUEUED;
    }

    if (has_marker && marker != 13U &&
        (marker < 129U || marker > 154U)) {
        stream_vm_error(vm, "invalid line terminator in command replay file");
        return REPLAY_PREPARE_ERROR;
    }
    for (i = 0U; line[i] != '\0'; ++i) {
        unsigned char ch = (unsigned char)line[i];
        if (ch < 32U || ch > 126U) {
            stream_vm_error(vm,
                            "command replay line contains unsupported non-ASCII input");
            return REPLAY_PREPARE_ERROR;
        }
    }

    /*
     * A replayed non-Enter line terminator must obey the same V5+ terminating
     * character table as a key supplied directly by Tcl. Reuse the existing
     * numeric-key API for that validation, then replace its intentionally empty
     * line with the command-file text while retaining the validated terminator.
     * This keeps replay on the ordinary read/preload/tokenization path without
     * duplicating header-table semantics in the command-file reader.
     */
    if (has_marker && marker != 13U) {
        if (zmachine_supply_key(vm, marker) != TCL_OK) {
            stream_vm_error(vm,
                            "command replay line terminator is not accepted by the story");
            return REPLAY_PREPARE_ERROR;
        }
        Tcl_DStringSetLength(&vm->pending_input, 0);
        Tcl_DStringAppend(&vm->pending_input, line, -1);
        vm->input_available = 1;
    } else {
        if (zmachine_supply_input(vm, line) != TCL_OK)
            return REPLAY_PREPARE_ERROR;
        vm->pending_input_terminator = 13U;
    }
    return REPLAY_PREPARE_QUEUED;
}

/*
 * Public run wrapper: automatically feed stream-1 records until the story
 * yields for keyboard input, another host request, or termination. Output from
 * multiple replay turns is accumulated so a single Tcl call does not lose text
 * merely because the lower cooperative loop clears its per-run output buffer.
 * Direct Flags-2 transcript selection is handled by the public step wrapper at
 * the instruction which changes the bit.
 */
int zmachine_run(ZMachine *vm)
{
    Tcl_DString accumulated;
    int rc = TCL_OK;
    int replayed = 0;

    Tcl_DStringInit(&accumulated);
    for (;;) {
        rc = zmachine_run_stream_base(vm);
        if (rc != TCL_OK)
            break;

        if (vm->state == ZM_STATE_WAITING_INPUT &&
            zmachine_current_input_stream(vm) == 1) {
            Tcl_DStringAppend(&accumulated,
                              Tcl_DStringValue(&vm->output),
                              Tcl_DStringLength(&vm->output));
            Tcl_DStringSetLength(&vm->output, 0);
            replayed = 1;
            rc = prepare_replay_input(vm);
            if (rc == REPLAY_PREPARE_ERROR)
                break;
            if (rc == REPLAY_PREPARE_EOF) {
                rc = TCL_OK;
                break;
            }
            continue;
        }
        break;
    }

    if (replayed) {
        Tcl_DStringAppend(&accumulated,
                          Tcl_DStringValue(&vm->output),
                          Tcl_DStringLength(&vm->output));
        Tcl_DStringSetLength(&vm->output, 0);
        Tcl_DStringAppend(&vm->output,
                          Tcl_DStringValue(&accumulated),
                          Tcl_DStringLength(&accumulated));
    }
    Tcl_DStringFree(&accumulated);
    return rc;
}

/* Transcript stream 2 receives story output unless stream 3 has precedence. */
void zmachine_output_append(ZMachine *vm, const char *text, size_t len)
{
    ZMachineStreamIO *io = vm ? vm->stream_io : NULL;

    if (io && io->transcript && stream2_selected(vm) &&
        vm->stream3_depth == 0U && text && len > 0U) {
        if (write_external(vm, io->transcript, text, len,
                           "unable to write transcript file") != TCL_OK)
            return;
    }
    zmachine_output_append_stream_base(vm, text, len);
}

/*
 * Add stream echo/recording side effects after the mature input writer succeeds.
 *
 * Keep a private copy of the newly supplied host bytes because the lower input
 * engine consumes them and lowercases the story buffer. For V5+ preloaded input,
 * only the newly accepted suffix is echoed; the existing prefix is left for the
 * story to redisplay. Stream 4 still records the completed final buffer.
 */
int zmachine_input_read_line(ZMachine *vm,
                             uint16_t text_buffer,
                             uint16_t parse_buffer,
                             uint16_t *terminator)
{
    Tcl_DString supplied;
    size_t prefix_length = 0U;
    size_t final_length = 0U;
    size_t echo_length = 0U;
    uint16_t local_terminator = 13U;
    int rc;

    Tcl_DStringInit(&supplied);
    if (vm && vm->input_available) {
        Tcl_DStringAppend(&supplied,
                          Tcl_DStringValue(&vm->pending_input),
                          Tcl_DStringLength(&vm->pending_input));
        if (vm->version >= 5U && vm->memory &&
            (size_t)text_buffer + 1U < vm->memory_size)
            prefix_length = vm->memory[(size_t)text_buffer + 1U];
    }

    rc = zmachine_input_read_line_stream_base(vm, text_buffer, parse_buffer,
                                              &local_terminator);
    if (rc != TCL_OK) {
        Tcl_DStringFree(&supplied);
        return rc;
    }

    if (vm->version <= 4U) {
        size_t start = (size_t)text_buffer + 1U;
        while (start + final_length < vm->memory_size &&
               vm->memory[start + final_length] != 0U)
            ++final_length;
    } else if ((size_t)text_buffer + 1U < vm->memory_size) {
        final_length = vm->memory[(size_t)text_buffer + 1U];
    }

    if (final_length > prefix_length)
        echo_length = final_length - prefix_length;
    if (echo_length > (size_t)Tcl_DStringLength(&supplied))
        echo_length = (size_t)Tcl_DStringLength(&supplied);

    if (note_line_input(vm, text_buffer, local_terminator,
                        Tcl_DStringValue(&supplied), echo_length) != TCL_OK) {
        Tcl_DStringFree(&supplied);
        return TCL_ERROR;
    }
    Tcl_DStringFree(&supplied);

    if (terminator)
        *terminator = local_terminator;
    return TCL_OK;
}

/* Derive the read_char value which is about to be consumed by the lower layer. */
static int pending_read_char_value(const ZMachine *vm, uint16_t *zscii)
{
    const unsigned char *data;
    int length;

    if (!vm || !zscii || !vm->input_available)
        return 0;
    data = (const unsigned char *)Tcl_DStringValue((Tcl_DString *)&vm->pending_input);
    length = Tcl_DStringLength((Tcl_DString *)&vm->pending_input);
    if (length == 2 && data[0] == 0U) {
        *zscii = data[1];
        return 1;
    }
    if (length == 0) {
        *zscii = 13U;
        return 1;
    }
    *zscii = (data[0] >= 32U && data[0] <= 126U) ? data[0] : 13U;
    return 1;
}

/*
 * Intercept stream-selection opcodes and observe completed read_char.
 *
 * This layer owns all VAR:19 output_stream values, not only the host-file
 * streams 2 and 4. The first operand may itself be variable 0, whose evaluation
 * pops the stack; resolving it merely to decide whether to delegate and then
 * asking the old presentation handler to resolve it again would pop twice.
 * Keeping all output-stream selection here guarantees each operand is evaluated
 * exactly once while preserving the existing stream-1 and nested stream-3 rules.
 *
 * Delegated instructions are checked for a Flags 2 bit-0 transition. This
 * catches storeb/storew/copy_table transcript selection while avoiding a false
 * host-file request merely because a restored/restarted VM already has the bit
 * set. A legitimate pre-existing selection already has its transcript resource.
 */
int zmachine_step(ZMachine *vm)
{
    ZMachineInstruction instruction;
    uint16_t values[ZM_MAX_OPERANDS];
    char decode_error[128];
    int was_read_char = 0;
    uint16_t read_char_value = 0U;
    int transcript_was_selected;
    uint32_t old_pc;
    int rc;

    if (!vm || !vm->memory)
        return stream_vm_error(vm, "cannot execute without a loaded story");

    transcript_was_selected = stream2_selected(vm);

    if (!zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                     vm->pc, &instruction,
                                     decode_error, sizeof(decode_error)))
        return stream_vm_error(vm, decode_error[0] ? decode_error :
                               "unable to decode Z-machine instruction");

    if (instruction.form == ZM_FORM_VARIABLE &&
        instruction.operand_count == ZM_OPERANDS_VAR &&
        (instruction.opcode_number == 19U ||
         instruction.opcode_number == 20U)) {
        if (instruction.operand_count_actual < 1U)
            return stream_vm_error(vm, "stream selection opcode is missing its operand");
        if (zmachine_resolve_operands(vm, &instruction, values,
                                      ZM_MAX_OPERANDS) != TCL_OK)
            return TCL_ERROR;

        if (instruction.opcode_number == 20U) { /* input_stream */
            ZMachineStreamIO *io = ensure_stream_io(vm);
            if (!io)
                return stream_vm_error(vm, "out of memory while selecting input stream");
            if (values[0] == 0U) {
                io->input_stream = 0U;
                vm->pc = instruction.next_pc;
                return TCL_OK;
            }
            if (values[0] != 1U)
                return stream_vm_error(vm, "invalid Z-machine input stream");
            if (io->replay) {
                io->input_stream = 1U;
                vm->pc = instruction.next_pc;
                return TCL_OK;
            }
            return begin_request(vm, STREAM_REQUEST_REPLAY,
                                 instruction.next_pc);
        }

        /* output_stream stream [table] */
        {
            int16_t stream = (int16_t)values[0];

            if (stream == 0) {
                vm->pc = instruction.next_pc;
                return TCL_OK;
            }

            if (stream == 1 || stream == -1) {
                vm->output_stream1_enabled = stream > 0;
                vm->pc = instruction.next_pc;
                return TCL_OK;
            }

            if (stream == 2 || stream == -2) {
                ZMachineStreamIO *io = ensure_stream_io(vm);
                if (!io)
                    return stream_vm_error(vm, "out of memory while selecting transcript stream");
                if (stream < 0) {
                    select_stream2(vm, 0);
                    if (io->transcript)
                        fflush(io->transcript);
                    vm->pc = instruction.next_pc;
                    return TCL_OK;
                }
                if (io->transcript) {
                    select_stream2(vm, 1);
                    vm->pc = instruction.next_pc;
                    return TCL_OK;
                }
                return begin_request(vm, STREAM_REQUEST_TRANSCRIPT,
                                     instruction.next_pc);
            }

            if (stream == 3) {
                uint16_t table;

                if (instruction.operand_count_actual < 2U)
                    return stream_vm_error(vm, "output_stream 3 is missing its table operand");
                if (vm->stream3_depth >= ZM_MAX_STREAM3_DEPTH)
                    return stream_vm_error(vm, "output_stream 3 nesting limit exceeded");
                table = values[1];
                if ((size_t)table + 1U >= vm->memory_size ||
                    (size_t)table + 1U >= (size_t)vm->static_memory_addr)
                    return stream_vm_error(vm, "output_stream 3 table is outside dynamic memory");
                vm->memory[table] = 0U;
                vm->memory[table + 1U] = 0U;
                vm->stream3_tables[vm->stream3_depth++] = table;
                vm->pc = instruction.next_pc;
                return TCL_OK;
            }

            if (stream == -3) {
                if (vm->stream3_depth == 0U)
                    return stream_vm_error(vm, "output_stream -3 without active stream 3");
                --vm->stream3_depth;
                vm->pc = instruction.next_pc;
                return TCL_OK;
            }

            if (stream == 4 || stream == -4) {
                ZMachineStreamIO *io = ensure_stream_io(vm);
                if (!io)
                    return stream_vm_error(vm, "out of memory while selecting command stream");
                if (stream < 0) {
                    io->record_selected = 0;
                    if (io->record)
                        fflush(io->record);
                    vm->pc = instruction.next_pc;
                    return TCL_OK;
                }
                if (io->record) {
                    io->record_selected = 1;
                    vm->pc = instruction.next_pc;
                    return TCL_OK;
                }
                return begin_request(vm, STREAM_REQUEST_RECORD,
                                     instruction.next_pc);
            }

            return stream_vm_error(vm, "unsupported Z-machine output stream number");
        }
    }

    if (instruction.form == ZM_FORM_VARIABLE &&
        instruction.operand_count == ZM_OPERANDS_VAR &&
        instruction.opcode_number == 22U)
        was_read_char = pending_read_char_value(vm, &read_char_value);

    old_pc = vm->pc;
    rc = zmachine_step_stream_base(vm);
    if (rc == TCL_OK && was_read_char && vm->pc != old_pc &&
        note_key_input(vm, read_char_value) != TCL_OK)
        return TCL_ERROR;
    if (rc == TCL_OK && vm->state == ZM_STATE_READY &&
        !transcript_was_selected && stream2_selected(vm) &&
        request_transcript_if_needed(vm) != TCL_OK)
        return TCL_ERROR;
    return rc;
}

/* Wrapper lifetime functions keep FILE handles out of the base VM module. */
void zmachine_destroy(ZMachine *vm)
{
    close_stream_io(vm);
    zmachine_destroy_stream_base(vm);
}

int zmachine_load_story(ZMachine *vm, const char *path)
{
    int rc;
    close_stream_io(vm);
    rc = zmachine_load_story_stream_base(vm, path);
    return rc;
}

int zmachine_reset(ZMachine *vm)
{
    int rc = zmachine_reset_stream_base(vm);
    if (rc == TCL_OK)
        close_stream_io(vm);
    return rc;
}
