/*
 * tcl_extension.c
 *
 * Tcl 8.6 loadable-extension boundary for tclzmachine.
 *
 * This file owns Tcl-visible session names, line/key input, presentation
 * options, and host file policy. Each session contains one independent ZMachine
 * instance. Ordinary VM execution remains unaware of IRC formatting and
 * filesystem naming; optional mIRC rendering is selected here at the host-facing
 * presentation boundary. When a story requests a save/restore or external
 * command stream file, the VM yields and Tcl supplies the actual host path.
 */

#include "tclzmachine.h"
#include "zmachine_decode.h"
#include "zmachine_mirc.h"
#include "zmachine_stream.h"
#include "zmachine_wrap.h"

#include <stdlib.h>
#include <string.h>

typedef struct Session {
    char *name;
    ZMachine *vm;
    size_t wordwrap_bytes;
    struct Session *next;
} Session;

typedef struct ExtensionState {
    Session *sessions;
} ExtensionState;

static char *dup_string(const char *s)
{
    size_t len;
    char *copy;

    if (!s)
        return NULL;
    len = strlen(s) + 1U;
    copy = (char *)malloc(len);
    if (copy)
        memcpy(copy, s, len);
    return copy;
}

static Session *find_session(ExtensionState *state, const char *name)
{
    Session *s;

    if (!state || !name)
        return NULL;
    for (s = state->sessions; s; s = s->next) {
        if (strcmp(s->name, name) == 0)
            return s;
    }
    return NULL;
}

static void free_state(ClientData clientData, Tcl_Interp *interp)
{
    ExtensionState *state = (ExtensionState *)clientData;
    Session *s = state ? state->sessions : NULL;
    (void)interp;

    while (s) {
        Session *next = s->next;
        zmachine_destroy(s->vm);
        free(s->name);
        free(s);
        s = next;
    }
    free(state);
}

static const char *session_output_format(const Session *s)
{
    return s && s->vm && zmachine_mirc_enabled(s->vm) ? "mirc" : "plain";
}

/* Convert VM output to the representation selected by the Tcl host. */
static int set_session_output(Tcl_Interp *interp, Session *s)
{
    const char *text;
    int length;
    int mirc;

    if (!interp || !s || !s->vm)
        return TCL_ERROR;

    mirc = zmachine_mirc_enabled(s->vm);
    if (mirc) {
        text = zmachine_mirc_output_data(s->vm);
        length = zmachine_mirc_output_length(s->vm);
    } else {
        text = zmachine_output_data(s->vm);
        length = zmachine_output_length(s->vm);
    }

    if (s->wordwrap_bytes == 0U) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(text, length));
        return TCL_OK;
    }

    {
        Tcl_DString wrapped;
        int rc;

        Tcl_DStringInit(&wrapped);
        if (mirc) {
            rc = zmachine_mirc_wrap_output(text, (size_t)length,
                                           s->wordwrap_bytes, &wrapped);
        } else {
            rc = zmachine_wrap_output(text, (size_t)length,
                                      s->wordwrap_bytes, &wrapped);
        }

        if (rc != TCL_OK) {
            Tcl_DStringFree(&wrapped);
            Tcl_SetObjResult(interp,
                Tcl_NewStringObj("unable to word-wrap game output", -1));
            return TCL_ERROR;
        }

        Tcl_SetObjResult(interp,
            Tcl_NewStringObj(Tcl_DStringValue(&wrapped),
                             Tcl_DStringLength(&wrapped)));
        Tcl_DStringFree(&wrapped);
    }
    return TCL_OK;
}

/* Produce a useful Tcl diagnostic even if a lower layer omitted vm->error. */
static int set_vm_failure(Tcl_Interp *interp, ZMachine *vm)
{
    const char *message;

    if (!interp || !vm)
        return TCL_ERROR;
    message = zmachine_last_error(vm);
    if (message && message[0] != '\0') {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(message, -1));
        return TCL_ERROR;
    }

    if (vm->state == ZM_STATE_HALTED) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("Z-machine session is halted", -1));
        return TCL_ERROR;
    }

    {
        ZMachineInstruction instruction;
        char decode_error[128];
        decode_error[0] = '\0';

        if (zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                        vm->pc, &instruction,
                                        decode_error, sizeof(decode_error))) {
            Tcl_SetObjResult(interp,
                Tcl_ObjPrintf("Z-machine operation failed at pc 0x%lx (state %d): form=%u count=%u opcode=%u operands=%u",
                              (unsigned long)vm->pc,
                              (int)vm->state,
                              (unsigned)instruction.form,
                              (unsigned)instruction.operand_count,
                              (unsigned)instruction.opcode_number,
                              (unsigned)instruction.operand_count_actual));
        } else {
            Tcl_SetObjResult(interp,
                Tcl_ObjPrintf("Z-machine operation failed at pc 0x%lx (state %d): decode failed: %s",
                              (unsigned long)vm->pc,
                              (int)vm->state,
                              decode_error[0] ? decode_error : "unknown decode error"));
        }
    }
    return TCL_ERROR;
}

/* Run after input/file completion until the next cooperative yield. */
static int run_session(Tcl_Interp *interp, Session *s)
{
    if (zmachine_run(s->vm) != TCL_OK)
        return set_vm_failure(interp, s->vm);
    return set_session_output(interp, s);
}

/*
 * Classify the instruction on which a cooperative input wait is suspended.
 *
 * No additional VM state is stored for this purpose: `read` and `read_char`
 * leave the program counter at the waiting instruction, so decoding that one
 * instruction provides stable host metadata without another synchronization
 * field which could become stale. Unknown/malformed waits are reported as an
 * empty string rather than turning a read-only `zmachine::info` call into an
 * execution error.
 */
static const char *input_request_kind(const ZMachine *vm)
{
    ZMachineInstruction instruction;
    char decode_error[128];

    if (!vm || !vm->memory || vm->state != ZM_STATE_WAITING_INPUT)
        return "";

    if (!zmachine_decode_instruction(vm->memory, vm->memory_size, vm->version,
                                     vm->pc, &instruction,
                                     decode_error, sizeof(decode_error)))
        return "";

    if (instruction.form != ZM_FORM_VARIABLE ||
        instruction.operand_count != ZM_OPERANDS_VAR)
        return "";

    if (instruction.opcode_number == 4U)
        return "line";
    if (instruction.opcode_number == 22U)
        return "char";
    return "";
}

/* Refuse player input while the VM is waiting for some host file decision. */
static int reject_file_wait(Tcl_Interp *interp, const char *name, ZMachine *vm)
{
    if (!vm)
        return TCL_OK;
    if (vm->state == ZM_STATE_WAITING_SAVE ||
        vm->state == ZM_STATE_WAITING_RESTORE) {
        Tcl_SetObjResult(interp,
            Tcl_ObjPrintf("session \"%s\" is waiting for zmachine::%s or zmachine::cancel",
                          name,
                          vm->state == ZM_STATE_WAITING_SAVE ? "save" : "restore"));
        return TCL_ERROR;
    }
    if (vm->state == ZM_STATE_WAITING_STREAM_FILE) {
        Tcl_SetObjResult(interp,
            Tcl_ObjPrintf("session \"%s\" is waiting for zmachine::streamfile or zmachine::cancel",
                          name));
        return TCL_ERROR;
    }
    return TCL_OK;
}

static int cmd_create(ClientData clientData, Tcl_Interp *interp,
                      int objc, Tcl_Obj *const objv[])
{
    ExtensionState *state = (ExtensionState *)clientData;
    const char *name;
    const char *path;
    Session *s;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "session storyfile");
        return TCL_ERROR;
    }

    name = Tcl_GetString(objv[1]);
    path = Tcl_GetString(objv[2]);
    if (find_session(state, name)) {
        Tcl_SetObjResult(interp,
            Tcl_ObjPrintf("session \"%s\" already exists", name));
        return TCL_ERROR;
    }

    s = (Session *)calloc(1, sizeof(*s));
    if (!s) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("out of memory", -1));
        return TCL_ERROR;
    }

    s->name = dup_string(name);
    s->vm = zmachine_create();
    if (!s->name || !s->vm) {
        free(s->name);
        zmachine_destroy(s->vm);
        free(s);
        Tcl_SetObjResult(interp, Tcl_NewStringObj("out of memory", -1));
        return TCL_ERROR;
    }

    if (zmachine_load_story(s->vm, path) != TCL_OK) {
        Tcl_SetObjResult(interp,
            Tcl_NewStringObj(zmachine_last_error(s->vm), -1));
        free(s->name);
        zmachine_destroy(s->vm);
        free(s);
        return TCL_ERROR;
    }

    s->next = state->sessions;
    state->sessions = s;
    Tcl_SetObjResult(interp, Tcl_NewStringObj(name, -1));
    return TCL_OK;
}

static int cmd_command(ClientData clientData, Tcl_Interp *interp,
                       int objc, Tcl_Obj *const objv[])
{
    ExtensionState *state = (ExtensionState *)clientData;
    Session *s;
    const char *name;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "session command");
        return TCL_ERROR;
    }

    name = Tcl_GetString(objv[1]);
    s = find_session(state, name);
    if (!s) {
        Tcl_SetObjResult(interp,
            Tcl_ObjPrintf("unknown session \"%s\"", name));
        return TCL_ERROR;
    }
    if (reject_file_wait(interp, name, s->vm) != TCL_OK)
        return TCL_ERROR;

    if (zmachine_supply_input(s->vm, Tcl_GetString(objv[2])) != TCL_OK)
        return set_vm_failure(interp, s->vm);
    return run_session(interp, s);
}

/* Tcl command: zmachine::key session zscii -- satisfy a suspended input key. */
static int cmd_key(ClientData clientData, Tcl_Interp *interp,
                   int objc, Tcl_Obj *const objv[])
{
    ExtensionState *state = (ExtensionState *)clientData;
    Session *s;
    const char *name;
    int value;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "session zsciiCode");
        return TCL_ERROR;
    }

    name = Tcl_GetString(objv[1]);
    s = find_session(state, name);
    if (!s) {
        Tcl_SetObjResult(interp,
            Tcl_ObjPrintf("unknown session \"%s\"", name));
        return TCL_ERROR;
    }
    if (reject_file_wait(interp, name, s->vm) != TCL_OK)
        return TCL_ERROR;

    if (Tcl_GetIntFromObj(interp, objv[2], &value) != TCL_OK)
        return TCL_ERROR;
    if (value < 0 || value > 255) {
        Tcl_SetObjResult(interp,
            Tcl_NewStringObj("ZSCII key code must be between 0 and 255", -1));
        return TCL_ERROR;
    }

    if (zmachine_supply_key(s->vm, (uint16_t)value) != TCL_OK)
        return set_vm_failure(interp, s->vm);
    return run_session(interp, s);
}

/*
 * Tcl command: zmachine::streamfile session kind path
 *
 * `kind` is replay, transcript, or record. A matching pending request is
 * completed and execution resumes. The same command may also preconfigure a
 * file before the story selects that stream, which is useful for V1/V2
 * transcript support where stream 2 is selected directly through Flags 2.
 */
static int cmd_streamfile(ClientData clientData, Tcl_Interp *interp,
                          int objc, Tcl_Obj *const objv[])
{
    ExtensionState *state = (ExtensionState *)clientData;
    Session *s;
    const char *name;
    int resume;

    if (objc != 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "session kind path");
        return TCL_ERROR;
    }
    name = Tcl_GetString(objv[1]);
    s = find_session(state, name);
    if (!s) {
        Tcl_SetObjResult(interp,
            Tcl_ObjPrintf("unknown session \"%s\"", name));
        return TCL_ERROR;
    }

    resume = s->vm->state == ZM_STATE_WAITING_STREAM_FILE;
    if (zmachine_stream_file(s->vm,
                             Tcl_GetString(objv[2]),
                             Tcl_GetString(objv[3])) != TCL_OK)
        return set_vm_failure(interp, s->vm);
    if (resume)
        return run_session(interp, s);

    Tcl_ResetResult(interp);
    return TCL_OK;
}

static int cmd_save(ClientData clientData, Tcl_Interp *interp,
                    int objc, Tcl_Obj *const objv[])
{
    ExtensionState *state = (ExtensionState *)clientData;
    Session *s;
    const char *name;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "session path");
        return TCL_ERROR;
    }
    name = Tcl_GetString(objv[1]);
    s = find_session(state, name);
    if (!s) {
        Tcl_SetObjResult(interp,
            Tcl_ObjPrintf("unknown session \"%s\"", name));
        return TCL_ERROR;
    }

    if (zmachine_save_file(s->vm, Tcl_GetString(objv[2])) != TCL_OK)
        return set_vm_failure(interp, s->vm);
    return run_session(interp, s);
}

static int cmd_restore(ClientData clientData, Tcl_Interp *interp,
                       int objc, Tcl_Obj *const objv[])
{
    ExtensionState *state = (ExtensionState *)clientData;
    Session *s;
    const char *name;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "session path");
        return TCL_ERROR;
    }
    name = Tcl_GetString(objv[1]);
    s = find_session(state, name);
    if (!s) {
        Tcl_SetObjResult(interp,
            Tcl_ObjPrintf("unknown session \"%s\"", name));
        return TCL_ERROR;
    }

    if (zmachine_restore_file(s->vm, Tcl_GetString(objv[2])) != TCL_OK)
        return set_vm_failure(interp, s->vm);
    return run_session(interp, s);
}

/* Tcl command: zmachine::cancel session -- decline the pending host file. */
static int cmd_cancel(ClientData clientData, Tcl_Interp *interp,
                      int objc, Tcl_Obj *const objv[])
{
    ExtensionState *state = (ExtensionState *)clientData;
    Session *s;
    const char *name;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "session");
        return TCL_ERROR;
    }
    name = Tcl_GetString(objv[1]);
    s = find_session(state, name);
    if (!s) {
        Tcl_SetObjResult(interp,
            Tcl_ObjPrintf("unknown session \"%s\"", name));
        return TCL_ERROR;
    }

    if (s->vm->state == ZM_STATE_WAITING_STREAM_FILE) {
        if (zmachine_cancel_stream_file(s->vm) != TCL_OK)
            return set_vm_failure(interp, s->vm);
    } else {
        if (zmachine_cancel_file(s->vm) != TCL_OK)
            return set_vm_failure(interp, s->vm);
    }
    return run_session(interp, s);
}

static int cmd_configure(ClientData clientData, Tcl_Interp *interp,
                         int objc, Tcl_Obj *const objv[])
{
    ExtensionState *state = (ExtensionState *)clientData;
    Session *s;
    const char *name;
    const char *option;

    if (objc < 2 || objc > 4) {
        Tcl_WrongNumArgs(interp, 1, objv,
                         "session ?-wordwrap|-format ?value??");
        return TCL_ERROR;
    }

    name = Tcl_GetString(objv[1]);
    s = find_session(state, name);
    if (!s) {
        Tcl_SetObjResult(interp,
            Tcl_ObjPrintf("unknown session \"%s\"", name));
        return TCL_ERROR;
    }

    if (objc == 2) {
        Tcl_Obj *dict = Tcl_NewDictObj();
        Tcl_DictObjPut(interp, dict,
                       Tcl_NewStringObj("-wordwrap", -1),
                       Tcl_NewWideIntObj((Tcl_WideInt)s->wordwrap_bytes));
        Tcl_DictObjPut(interp, dict,
                       Tcl_NewStringObj("-format", -1),
                       Tcl_NewStringObj(session_output_format(s), -1));
        Tcl_SetObjResult(interp, dict);
        return TCL_OK;
    }

    option = Tcl_GetString(objv[2]);
    if (strcmp(option, "-wordwrap") == 0) {
        if (objc == 3) {
            Tcl_SetObjResult(interp,
                Tcl_NewWideIntObj((Tcl_WideInt)s->wordwrap_bytes));
            return TCL_OK;
        }

        {
            Tcl_WideInt value;
            if (Tcl_GetWideIntFromObj(interp, objv[3], &value) != TCL_OK)
                return TCL_ERROR;
            if (value < 0) {
                Tcl_SetObjResult(interp,
                    Tcl_NewStringObj("-wordwrap must be zero or a positive byte count", -1));
                return TCL_ERROR;
            }
            s->wordwrap_bytes = (size_t)value;
        }

        Tcl_SetObjResult(interp,
            Tcl_NewWideIntObj((Tcl_WideInt)s->wordwrap_bytes));
        return TCL_OK;
    }

    if (strcmp(option, "-format") == 0) {
        const char *format;
        int enabled;

        if (objc == 3) {
            Tcl_SetObjResult(interp,
                Tcl_NewStringObj(session_output_format(s), -1));
            return TCL_OK;
        }

        format = Tcl_GetString(objv[3]);
        if (strcmp(format, "plain") == 0)
            enabled = 0;
        else if (strcmp(format, "mirc") == 0)
            enabled = 1;
        else {
            Tcl_SetObjResult(interp,
                Tcl_ObjPrintf("unknown output format \"%s\": must be plain or mirc",
                              format));
            return TCL_ERROR;
        }

        if (zmachine_mirc_set_enabled(s->vm, enabled) != TCL_OK)
            return set_vm_failure(interp, s->vm);
        Tcl_SetObjResult(interp,
            Tcl_NewStringObj(session_output_format(s), -1));
        return TCL_OK;
    }

    Tcl_SetObjResult(interp,
        Tcl_ObjPrintf("unknown option \"%s\": must be -wordwrap or -format",
                      option));
    return TCL_ERROR;
}

/*
 * Return session state and cooperative host-request metadata as a Tcl dict.
 *
 * inputRequest is empty unless the VM is suspended on input, in which case it
 * is `line` for VAR:4 read/sread/aread or `char` for VAR:22 read_char.
 * streamRequest is replay, transcript, or record while a Z-machine stream
 * selection is waiting for a host path. inputStream reports the currently
 * selected Z-machine input source (0 keyboard/Tcl, 1 replay file).
 * outputFormat is `plain` by default or `mirc` when the host has enabled the
 * optional IRC presentation renderer.
 *
 * fileRequest remains the stable save/restore indicator. fileRequestKind
 * distinguishes full Quetzal state from V5+ auxiliary byte-region transfers.
 */
static int cmd_info(ClientData clientData, Tcl_Interp *interp,
                    int objc, Tcl_Obj *const objv[])
{
    ExtensionState *state = (ExtensionState *)clientData;
    Session *s;
    Tcl_Obj *dict;
    const char *name;
    const char *file_request = "";
    const char *file_request_kind = "";
    const char *input_request;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "session");
        return TCL_ERROR;
    }

    name = Tcl_GetString(objv[1]);
    s = find_session(state, name);
    if (!s) {
        Tcl_SetObjResult(interp,
            Tcl_ObjPrintf("unknown session \"%s\"", name));
        return TCL_ERROR;
    }

    input_request = input_request_kind(s->vm);

    if (s->vm->state == ZM_STATE_WAITING_SAVE)
        file_request = "save";
    else if (s->vm->state == ZM_STATE_WAITING_RESTORE)
        file_request = "restore";

    if (s->vm->pending_file_kind == ZM_FILE_REQUEST_FULL)
        file_request_kind = "full";
    else if (s->vm->pending_file_kind == ZM_FILE_REQUEST_AUXILIARY)
        file_request_kind = "auxiliary";

    dict = Tcl_NewDictObj();
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("version", -1),
                   Tcl_NewIntObj(s->vm->version));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("supportedVersions", -1),
                   Tcl_NewStringObj(zmachine_supported_versions(), -1));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("textOnly", -1),
                   Tcl_NewBooleanObj(TCLZMACHINE_TEXT_ONLY));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("pc", -1),
                   Tcl_NewWideIntObj((Tcl_WideInt)s->vm->pc));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("memorySize", -1),
                   Tcl_NewWideIntObj((Tcl_WideInt)s->vm->memory_size));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("declaredFileLength", -1),
                   Tcl_NewWideIntObj((Tcl_WideInt)s->vm->declared_file_length));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("routineOffset", -1),
                   Tcl_NewIntObj(s->vm->routine_offset));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("stringOffset", -1),
                   Tcl_NewIntObj(s->vm->string_offset));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("state", -1),
                   Tcl_NewIntObj((int)s->vm->state));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("inputRequest", -1),
                   Tcl_NewStringObj(input_request, -1));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("inputStream", -1),
                   Tcl_NewIntObj(zmachine_current_input_stream(s->vm)));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("streamRequest", -1),
                   Tcl_NewStringObj(zmachine_pending_stream_request(s->vm), -1));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("commandRecording", -1),
                   Tcl_NewBooleanObj(zmachine_command_recording_selected(s->vm)));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("outputFormat", -1),
                   Tcl_NewStringObj(session_output_format(s), -1));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("fileRequest", -1),
                   Tcl_NewStringObj(file_request, -1));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("fileRequestKind", -1),
                   Tcl_NewStringObj(file_request_kind, -1));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("suggestedFileName", -1),
                   Tcl_NewStringObj(s->vm->pending_file_name, -1));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("filePrompt", -1),
                   Tcl_NewIntObj(s->vm->pending_file_prompt));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("fileTable", -1),
                   Tcl_NewIntObj(s->vm->pending_file_table));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("fileBytes", -1),
                   Tcl_NewIntObj(s->vm->pending_file_bytes));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("wordWrapBytes", -1),
                   Tcl_NewWideIntObj((Tcl_WideInt)s->wordwrap_bytes));
    Tcl_SetObjResult(interp, dict);
    return TCL_OK;
}

static int cmd_destroy(ClientData clientData, Tcl_Interp *interp,
                       int objc, Tcl_Obj *const objv[])
{
    ExtensionState *state = (ExtensionState *)clientData;
    Session **link;
    const char *name;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "session");
        return TCL_ERROR;
    }

    name = Tcl_GetString(objv[1]);
    for (link = &state->sessions; *link; link = &(*link)->next) {
        if (strcmp((*link)->name, name) == 0) {
            Session *victim = *link;
            *link = victim->next;
            zmachine_destroy(victim->vm);
            free(victim->name);
            free(victim);
            return TCL_OK;
        }
    }

    Tcl_SetObjResult(interp,
        Tcl_ObjPrintf("unknown session \"%s\"", name));
    return TCL_ERROR;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int Tclzmachine_Init(Tcl_Interp *interp)
{
    ExtensionState *state;

    if (Tcl_InitStubs(interp, "8.6", 0) == NULL)
        return TCL_ERROR;

    state = (ExtensionState *)calloc(1, sizeof(*state));
    if (!state) {
        Tcl_SetObjResult(interp,
            Tcl_NewStringObj("unable to allocate extension state", -1));
        return TCL_ERROR;
    }

    Tcl_CreateNamespace(interp, "::zmachine", NULL, NULL);
    Tcl_CreateObjCommand(interp, "::zmachine::create",
                         cmd_create, state, NULL);
    Tcl_CreateObjCommand(interp, "::zmachine::command",
                         cmd_command, state, NULL);
    Tcl_CreateObjCommand(interp, "::zmachine::key",
                         cmd_key, state, NULL);
    Tcl_CreateObjCommand(interp, "::zmachine::streamfile",
                         cmd_streamfile, state, NULL);
    Tcl_CreateObjCommand(interp, "::zmachine::save",
                         cmd_save, state, NULL);
    Tcl_CreateObjCommand(interp, "::zmachine::restore",
                         cmd_restore, state, NULL);
    Tcl_CreateObjCommand(interp, "::zmachine::cancel",
                         cmd_cancel, state, NULL);
    Tcl_CreateObjCommand(interp, "::zmachine::configure",
                         cmd_configure, state, NULL);
    Tcl_CreateObjCommand(interp, "::zmachine::info",
                         cmd_info, state, NULL);
    Tcl_CreateObjCommand(interp, "::zmachine::destroy",
                         cmd_destroy, state, NULL);

    Tcl_CallWhenDeleted(interp, free_state, state);

    if (Tcl_PkgProvide(interp, "tclzmachine", TCLZMACHINE_VERSION) != TCL_OK)
        return TCL_ERROR;

    return TCL_OK;
}
