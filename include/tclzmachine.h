#ifndef TCLZMACHINE_H
#define TCLZMACHINE_H

#include <stddef.h>
#include <stdint.h>
#include <tcl.h>

#include "zmachine_state.h"
#include "zmachine_version.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCLZMACHINE_VERSION "0.2.0"
#define TCLZMACHINE_TEXT_ONLY 1

typedef enum ZMachineRunState {
    ZM_STATE_READY = 0,
    ZM_STATE_WAITING_INPUT,
    ZM_STATE_HALTED,
    ZM_STATE_ERROR
} ZMachineRunState;

struct ZMachine {
    uint8_t *memory;
    size_t memory_size;

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

    uint32_t pc;
    uint16_t stack[4096];
    size_t sp;
    ZMachineFrame frames[ZM_MAX_FRAMES];
    size_t frame_count;

    Tcl_DString output;
    Tcl_DString pending_input;

    ZMachineRunState state;
    char error[256];
};

ZMachine *zmachine_create(void);
void zmachine_destroy(ZMachine *vm);

int zmachine_load_story(ZMachine *vm, const char *path);
int zmachine_reset(ZMachine *vm);
int zmachine_supply_input(ZMachine *vm, const char *line);
int zmachine_run(ZMachine *vm);

uint32_t zmachine_unpack_routine_address(const ZMachine *vm, uint16_t packed);
uint32_t zmachine_unpack_string_address(const ZMachine *vm, uint16_t packed);

void zmachine_output_clear(ZMachine *vm);
void zmachine_output_append(ZMachine *vm, const char *text, size_t len);
const char *zmachine_output_data(const ZMachine *vm);
int zmachine_output_length(const ZMachine *vm);

const char *zmachine_last_error(const ZMachine *vm);

#ifdef __cplusplus
}
#endif

#endif
