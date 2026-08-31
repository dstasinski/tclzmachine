/*
 * tcl_extension.c
 *
 * Tcl 8.6 loadable-extension boundary for tclzmachine.
 *
 * This file owns Tcl-visible session names, presentation options, and host file
 * policy. Each session contains one independent ZMachine instance. Ordinary VM
 * execution remains unaware of IRC formatting and filesystem naming; when a
 * story requests either a full-game or auxiliary save/restore, the VM yields
 * and Tcl supplies the actual host path.
 */

#include "tclzmachine.h"
#include "zmachine_decode.h"
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

/* Convert the session's canonical VM output to its Tcl-facing result. */
static int set_session_output(Tcl_Interp *interp, Session *s)
{
    if (!interp || !s || !s->vm)
        return TCL_ERROR;

    if (s->wordwrap_bytes == 0U) {
        Tcl_SetObjResult(interp,
            Tcl_NewStringObj(zmachine_output_data(s->vm),
                             zmachine_output_length(s->vm)));
        return TCL_OK;
    }

    {
        Tcl_DString wrapped;
        Tcl_DStringInit(&wrapped);

        if (zmachine_wrap_output(zmachine_output_data(s->vm),
                                 (size_t)zmachine_output_length(s->vm),
                                 s->wordwrap_bytes,
                                 &wrapped) != TCL_OK) {
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

    if (s->vm->state == ZM_STATE_WAITING_SAVE ||
        s->vm->state == ZM_STATE_WAITING_RESTORE) {
        Tcl_SetObjResult(interp,
            Tcl_ObjPrintf("session \"%s\" is waiting for zmachine::%s or zmachine::cancel",
                          name,
                          s->vm->state == ZM_STATE_WAITING_SAVE ? "save" : "restore"));
        return TCL_ERROR;
    }

    if (zmachine_supply_input(s->vm, Tcl_GetString(objv[2])) != TCL_OK)
        return set_vm_failure(interp, s->vm);
    return run_session(interp, s);
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

/* Tcl command: zmachine::cancel session -- decline pending save/restore. */
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

    if (zmachine_cancel_file(s->vm) != TCL_OK)
        return set_vm_failure(interp, s->vm);
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
                         "session ?-wordwrap ?bytes??");
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
        Tcl_SetObjResult(interp, dict);
        return TCL_OK;
    }

    option = Tcl_GetString(objv[2]);
    if (strcmp(option, "-wordwrap") != 0) {
        Tcl_SetObjResult(interp,
            Tcl_ObjPrintf("unknown option \"%s\": must be -wordwrap", option));
        return TCL_ERROR;
    }

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

/*
 * Return session state and cooperative file-request metadata as a Tcl dict.
 *
 * fileRequest remains the stable high-level save/restore indicator. When a
 * request is pending, fileRequestKind distinguishes full Quetzal state from a
 * V5+ auxiliary byte-region transfer. Auxiliary fields are always present so
 * callers can use straightforward dict access without testing for key existence.
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
