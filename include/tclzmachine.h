#ifndef TCLZMACHINE_H
#define TCLZMACHINE_H

/*
 * tclzmachine.h
 *
 * Central runtime definition for the Tcl Z-machine extension. The project is
 * intentionally text-only and supports Z-machine versions 1-5, 7, and 8.
 * Version 6 is excluded because its presentation model depends heavily on
 * pixel-positioned windows and graphics, which do not fit the IRC-oriented
 * runtime this project is designed to provide.
 */

#include <stddef.h>
#include <stdint.h>
#include <tcl.h>

#include "zmachine_state.h"
#include "zmachine_version.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCLZMACHINE_VERSION "1.0.0"
#define TCLZMACHINE_TEXT_ONLY 1
#define ZM_MAX_STREAM3_DEPTH 16U
#define ZM_AUX_FILENAME_MAX 12U

/* Opaque heap-owned cache used only after a story requests save_undo. */
typedef struct ZMachineUndoState ZMachineUndoState;

/*
 * Opaque host-file stream state. This lazily owns replay/transcript/record FILE
 * handles and their interpreter-only selection state without exposing stdio
 * implementation details through the public VM structure.
 */
typedef struct ZMachineStreamIO ZMachineStreamIO;

/*
 * Opaque presentation state for optional mIRC rendering. Canonical VM output
 * remains plain UTF-8; this object exists only when a story uses presentation
 * opcodes or a Tcl host enables the `mirc` output format.
 */
typedef struct ZMachineMircState ZMachineMircState;

/* Coarse execution state visible to the Tcl-facing session layer. */
typedef enum ZMachineRunState {
    ZM_STATE_READY = 0,
    ZM_STATE_WAITING_INPUT,
    ZM_STATE_HALTED,
    ZM_STATE_ERROR,
    /* Cooperative host-file requests; appended to preserve existing values. */
    ZM_STATE_WAITING_SAVE,
    ZM_STATE_WAITING_RESTORE,
    ZM_STATE_WAITING_STREAM_FILE
} ZMachineRunState;

/*
 * Kind of host-file request currently suspended at the Tcl boundary.
 *
 * FULL requests serialize/restore complete game state through Quetzal.
 * AUXILIARY requests transfer only a story-selected byte region and are not
 * part of the state of play. NONE is used whenever no file opcode is pending.
 */
typedef enum ZMachineFileRequestKind {
    ZM_FILE_REQUEST_NONE = 0,
    ZM_FILE_REQUEST_FULL,
    ZM_FILE_REQUEST_AUXILIARY
} ZMachineFileRequestKind;

/*
 * Complete per-session interpreter state. No ordinary VM state is global;
 * every Tcl session owns one ZMachine instance so many games can coexist in
 * one process without sharing stacks, memory, random state, or I/O buffers.
 */
struct ZMachine {
    /* Mutable story image. Dynamic memory is modified directly by Z-code. */
    uint8_t *memory;
    size_t memory_size;

    /* Original dynamic-memory bytes retained for restart and Quetzal CMem. */
    uint8_t *initial_dynamic_memory;
    size_t initial_dynamic_memory_size;

    /* Optional one-level in-memory save used by EXT:9/EXT:10 undo opcodes. */
    ZMachineUndoState *undo_state;

    /* Optional replay/transcript/command-record host stream state. */
    ZMachineStreamIO *stream_io;

    /* Optional host-facing mIRC renderer state and capability selection. */
    ZMachineMircState *mirc_state;
    int mirc_output_enabled;

    /* Cached fields from the story-file header. */
    uint8_t version;
    uint16_t flags1;
    uint16_t release_number;
    uint16_t high_memory_addr;
    uint16_t initial_pc;
    uint16_t dictionary_addr;
    uint16_t object_table_addr;
    uint16_t globals_addr;
    uint16_t static_memory_addr;
    uint16_t flags2;
    uint16_t abbreviations_addr;
    uint16_t header_file_length_word;
    size_t declared_file_length;
    uint16_t checksum;
    uint16_t routine_offset;
    uint16_t string_offset;
    uint16_t header_extension_addr;

    /* Program counter, evaluation stack, and routine call frames. */
    uint32_t pc;
    uint16_t stack[4096];
    size_t sp;
    ZMachineFrame frames[ZM_MAX_FRAMES];
    size_t frame_count;

    /*
     * Cooperative save/restore state retained while Tcl chooses a path.
     *
     * pending_file_pc points at the save/restore store or branch continuation.
     * For auxiliary V5+ EXT:0/EXT:1 requests, table/bytes identify the memory
     * transfer, suggested_name is the normalized story-supplied 8.3-style name,
     * and prompt is -1 when omitted, 0 for silent use, or 1 when the story asks
     * the host to confirm the filename. Full Quetzal requests leave the
     * auxiliary fields zero/empty.
     */
    uint32_t pending_file_pc;
    ZMachineFileRequestKind pending_file_kind;
    uint16_t pending_file_table;
    uint16_t pending_file_bytes;
    int pending_file_prompt;
    char pending_file_name[ZM_AUX_FILENAME_MAX + 1U];

    /* Per-session pseudo-random generator state used by the random opcode. */
    uint32_t random_state;

    /*
     * Minimal presentation state retained by the text-only frontend.
     *
     * Z-machine windows 1 and above are presentation/status regions. The IRC
     * runtime does not expose their cursor or layout data, so text written to a
     * nonzero window is discarded rather than mixed into narrative output.
     */
    uint8_t current_window;

    /*
     * Output-stream state required by VAR:19 output_stream. Stream 1 is the
     * ordinary screen/text stream and is enabled by default after reset. Stream
     * 3 may be nested to the standard maximum depth of 16; each entry stores
     * the destination table address for one memory-capture level. While any
     * stream-3 level is active it receives output exclusively, and closing an
     * inner level resumes the preceding table without changing stream-1 state.
     */
    int output_stream1_enabled;
    uint16_t stream3_tables[ZM_MAX_STREAM3_DEPTH];
    size_t stream3_depth;

    /*
     * Canonical output and one queued unit of player input.
     *
     * pending_input holds line text or the private two-byte read_char sentinel.
     * pending_input_terminator is normally carriage return (13); when a V5+
     * terminating-character-table function key finishes `read`, it carries that
     * exact ZSCII code so aread can store the correct result without placing the
     * input-only key in the text buffer.
     */
    Tcl_DString output;
    Tcl_DString pending_input;
    uint16_t pending_input_terminator;
    int input_available;

    ZMachineRunState state;
    char error[256];
};

/* Lifetime and story-image management. */
ZMachine *zmachine_create(void);
void zmachine_destroy(ZMachine *vm);
int zmachine_load_story(ZMachine *vm, const char *path);
int zmachine_reset(ZMachine *vm);

/*
 * Rewrite interpreter-owned/Rst header fields to the capabilities actually
 * exposed by this text-only runtime. This is called after initial load, restart,
 * and restore. It intentionally leaves the formal Standards revision at 0.0
 * until the project has completed a full conformance audit.
 */
void zmachine_refresh_interpreter_header(ZMachine *vm);

/* Queue host input for a cooperative read or read_char request. */
int zmachine_supply_input(ZMachine *vm, const char *line);
int zmachine_supply_key(ZMachine *vm, uint16_t zscii);
int zmachine_run(ZMachine *vm);

/* Complete, retry, or decline a cooperative story save/restore request. */
int zmachine_save_file(ZMachine *vm, const char *path);
int zmachine_restore_file(ZMachine *vm, const char *path);
int zmachine_cancel_file(ZMachine *vm);

/* Version-aware packed-address conversion helpers. */
uint32_t zmachine_unpack_routine_address(const ZMachine *vm, uint16_t packed);
uint32_t zmachine_unpack_string_address(const ZMachine *vm, uint16_t packed);

/* Canonical, unwrapped textual output generated by the VM. */
void zmachine_output_clear(ZMachine *vm);
void zmachine_output_append(ZMachine *vm, const char *text, size_t len);
const char *zmachine_output_data(const ZMachine *vm);
int zmachine_output_length(const ZMachine *vm);

/* Last interpreter error for this session. */
const char *zmachine_last_error(const ZMachine *vm);

#ifdef __cplusplus
}
#endif

#endif
