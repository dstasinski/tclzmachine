/*
 * zmachine_key.c
 *
 * Numeric ZSCII keypress support for cooperative `read_char` and V5+ line
 * termination.
 *
 * `zmachine::command` remains the ordinary line-oriented Tcl/IRC API. A
 * separate numeric key API lets a host supply exact ZSCII keyboard events which
 * cannot be represented safely as UTF-8 command text: cursor/function/keypad
 * keys for `read_char`, and function keys named by a V5+ terminating-character
 * table for `read`/`aread`.
 *
 * A key destined for `read_char` is represented internally as a private
 * two-byte pending-input sentinel (NUL, ZSCII). A key terminating line input
 * instead queues an empty line and records the exact code in
 * pending_input_terminator. The normal line-input path can then preserve V5+
 * preloaded text, tokenize it, and store the terminator without ever inserting
 * the input-only function key into the story's text buffer.
 *
 * This module is an instruction-dispatch layer between lexical opcodes and the
 * existing presentation wrapper. It consumes only `read_char` when the private
 * numeric sentinel is pending and delegates every other instruction unchanged.
 */

#include "tclzmachine.h"
#include "zmachine_decode.h"
#include "zmachine_exec.h"

#include <stdio.h>

/* Existing presentation wrapper, immediately below this input layer. */
extern int zmachine_step_present(ZMachine *vm);

#define ZM_DEFAULT_EXTRA_FIRST 155U
#define ZM_DEFAULT_EXTRA_LAST 223U
#define ZM_EXTRA_LAST 251U
#define ZM_FUNCTION_FIRST 129U
#define ZM_FUNCTION_LAST 154U

/* Record a host/API diagnostic without destroying an otherwise resumable VM. */
static int key_api_error(ZMachine *vm, const char *message)
{
    if (vm)
        snprintf(vm->error, sizeof(vm->error), "%s", message);
    return TCL_ERROR;
}

/* Put the VM into its terminal error state for malformed executing Z-code. */
static int key_vm_error(ZMachine *vm, const char *message)
{
    if (vm) {
        vm->state = ZM_STATE_ERROR;
        snprintf(vm->error, sizeof(vm->error), "%s", message);
    }
    return TCL_ERROR;
}

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/*
 * Return whether one extra-character ZSCII code is defined by this story.
 *
 * V1-V4 and V5+ stories without a usable custom Unicode table use the Standard
 * default mapping, which defines 155..223. A V5+ header-extension word 3 may
 * instead select a counted table defining an arbitrary prefix of 155..251.
 */
static int extra_input_defined(const ZMachine *vm, uint16_t zscii)
{
    uint16_t extension_words;
    uint16_t table;
    uint8_t count;
    size_t index;

    if (!vm || !vm->memory ||
        zscii < ZM_DEFAULT_EXTRA_FIRST || zscii > ZM_EXTRA_LAST)
        return 0;

    if (vm->version <= 4U || vm->header_extension_addr == 0U)
        return zscii <= ZM_DEFAULT_EXTRA_LAST;

    if ((size_t)vm->header_extension_addr + 1U >= vm->memory_size)
        return 0;
    extension_words = read_be16(vm->memory + vm->header_extension_addr);
    if (extension_words < 3U)
        return zscii <= ZM_DEFAULT_EXTRA_LAST;

    if ((size_t)vm->header_extension_addr + 7U >= vm->memory_size)
        return 0;
    table = read_be16(vm->memory + vm->header_extension_addr + 6U);
    if (table == 0U)
        return zscii <= ZM_DEFAULT_EXTRA_LAST;
    if ((size_t)table >= vm->memory_size)
        return 0;

    count = vm->memory[table];
    if (count > 97U ||
        (size_t)table + 1U + (size_t)count * 2U > vm->memory_size)
        return 0;

    index = (size_t)(zscii - ZM_DEFAULT_EXTRA_FIRST);
    return index < count;
}

/*
 * Return nonzero exactly for keyboard-style codes this text-only host can
 * deliver. Mouse/menu codes 252..254 are deliberately excluded: the runtime
 * advertises no mouse and has no coordinates/click state with which to make
 * those events meaningful, even though the ZSCII table defines them as input.
 */
static int zscii_input_defined(const ZMachine *vm, uint16_t zscii)
{
    if (zscii == 8U || zscii == 13U || zscii == 27U)
        return 1;
    if (zscii >= 32U && zscii <= 126U)
        return 1;
    if (zscii >= ZM_FUNCTION_FIRST && zscii <= ZM_FUNCTION_LAST)
        return 1;
    if (zscii >= ZM_DEFAULT_EXTRA_FIRST && zscii <= ZM_EXTRA_LAST)
        return extra_input_defined(vm, zscii);
    return 0;
}

/*
 * Identify the cooperative input opcode currently suspended at vm->pc.
 *
 * Return 1 for VAR:4 `read`, 2 for VAR:22 `read_char`, and zero otherwise.
 * The run loop leaves the PC on the unexecuted instruction while waiting, so
 * no additional input-kind state is necessary in ZMachine.
 */
static int current_input_kind(ZMachine *vm, ZMachineInstruction *instruction)
{
    char decode_error[128];

    if (!vm || !vm->memory)
        return 0;
    if (!zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                     vm->pc, instruction,
                                     decode_error, sizeof(decode_error)))
        return 0;

    if (instruction->form != ZM_FORM_VARIABLE ||
        instruction->operand_count != ZM_OPERANDS_VAR)
        return 0;
    if (instruction->opcode_number == 4U)
        return 1;
    if (vm->version >= 4U && instruction->opcode_number == 22U)
        return 2;
    return 0;
}

/*
 * Check whether a V5+ function key is listed in header word $2e's table.
 *
 * The table is a zero-terminated byte list. Only function-key codes are legal
 * there, and 255 means any function key. This text-only runtime deliberately
 * exposes only keyboard function codes 129..154; mouse/menu codes 252..254 are
 * not host-deliverable even if a story lists them.
 *
 * Return 1 when allowed, 0 when not listed/no table, and -1 for a malformed
 * nonzero pointer or unterminated table. Malformation is reported as an API
 * error rather than poisoning a resumable session merely because the host tried
 * to supply a key.
 */
static int line_terminating_key_allowed(const ZMachine *vm, uint16_t zscii)
{
    uint16_t table;
    size_t address;

    if (!vm || !vm->memory || vm->version < 5U ||
        zscii < ZM_FUNCTION_FIRST || zscii > ZM_FUNCTION_LAST)
        return 0;
    if (vm->memory_size <= 0x2fU)
        return -1;

    table = read_be16(vm->memory + 0x2eU);
    if (table == 0U)
        return 0;
    if ((size_t)table >= vm->memory_size)
        return -1;

    for (address = table; address < vm->memory_size; ++address) {
        uint8_t entry = vm->memory[address];

        if (entry == 0U)
            return 0;
        if (entry == 255U || entry == (uint8_t)zscii)
            return 1;
    }

    return -1;
}

/* Queue an exact numeric key for the cooperative input request at vm->pc. */
int zmachine_supply_key(ZMachine *vm, uint16_t zscii)
{
    ZMachineInstruction instruction;
    int input_kind;
    char encoded[2];

    if (!vm || !vm->memory)
        return TCL_ERROR;
    if (vm->state != ZM_STATE_WAITING_INPUT)
        return key_api_error(vm, "Z-machine session is not waiting for input");

    input_kind = current_input_kind(vm, &instruction);
    if (input_kind == 0)
        return key_api_error(vm, "Z-machine session is not suspended on a supported input opcode");

    if (input_kind == 1) {
        int allowed;

        if (zscii != 13U) {
            if (vm->version < 5U ||
                zscii < ZM_FUNCTION_FIRST || zscii > ZM_FUNCTION_LAST)
                return key_api_error(vm,
                    "line input accepts only Enter or an available terminating function key");

            allowed = line_terminating_key_allowed(vm, zscii);
            if (allowed < 0)
                return key_api_error(vm,
                    "malformed Z-machine terminating-character table");
            if (!allowed)
                return key_api_error(vm,
                    "function key is not listed in the Z-machine terminating-character table");
        }

        Tcl_DStringSetLength(&vm->pending_input, 0);
        vm->pending_input_terminator = zscii;
        vm->input_available = 1;
        vm->state = ZM_STATE_READY;
        vm->error[0] = '\0';
        return TCL_OK;
    }

    if (!zscii_input_defined(vm, zscii))
        return key_api_error(vm,
                             "invalid, undefined, or unavailable ZSCII input key");

    encoded[0] = '\0';
    encoded[1] = (char)(uint8_t)zscii;
    Tcl_DStringSetLength(&vm->pending_input, 0);
    Tcl_DStringAppend(&vm->pending_input, encoded, 2);
    vm->input_available = 1;
    vm->state = ZM_STATE_READY;
    vm->error[0] = '\0';
    return TCL_OK;
}

/* Return nonzero and decode the sentinel when a numeric read_char key is pending. */
static int pending_numeric_key(const ZMachine *vm, uint16_t *zscii)
{
    const unsigned char *data;

    if (!vm || !zscii || !vm->input_available ||
        Tcl_DStringLength(&vm->pending_input) != 2)
        return 0;

    data = (const unsigned char *)Tcl_DStringValue(&vm->pending_input);
    if (data[0] != 0U)
        return 0;

    *zscii = data[1];
    return 1;
}

/* Execute one instruction through the numeric-key input layer. */
int zmachine_step_input(ZMachine *vm)
{
    ZMachineInstruction instruction;
    uint16_t values[ZM_MAX_OPERANDS];
    uint16_t zscii;
    uint8_t store_variable;

    if (!vm || !vm->memory)
        return key_vm_error(vm, "cannot execute without a loaded story");

    if (!pending_numeric_key(vm, &zscii))
        return zmachine_step_present(vm);

    if (current_input_kind(vm, &instruction) != 2)
        return key_vm_error(vm, "numeric read_char key was queued outside read_char");

    if (zmachine_resolve_operands(vm, &instruction, values,
                                  ZM_MAX_OPERANDS) != TCL_OK)
        return TCL_ERROR;

    /* Galatea's historical zero-operand form implies keyboard device 1. */
    if (instruction.operand_count_actual >= 1U && values[0] != 1U)
        return key_vm_error(vm, "read_char requested an unsupported input device");
    if (instruction.operand_count_actual >= 2U && values[1] != 0U)
        return key_vm_error(vm, "timed read_char is not yet supported");
    if (instruction.operand_count_actual >= 3U && values[2] != 0U)
        return key_vm_error(vm, "timed read_char callback is not yet supported");
    if ((size_t)instruction.next_pc >= vm->memory_size)
        return key_vm_error(vm, "truncated read_char store variable");

    store_variable = vm->memory[instruction.next_pc];
    if (zmachine_variable_write(vm, store_variable, 0, zscii) != TCL_OK)
        return TCL_ERROR;

    Tcl_DStringSetLength(&vm->pending_input, 0);
    vm->input_available = 0;
    vm->pc = instruction.next_pc + 1U;
    return TCL_OK;
}
