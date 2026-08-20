/*
 * tcl_extension.c
 *
 * Tcl 8.6 loadable-extension boundary for tclzmachine.
 *
 * This file owns Tcl-visible session names and presentation options.  Each
 * session contains one independent ZMachine instance, allowing a bot to keep
 * many games alive concurrently.  VM execution remains unaware of IRC or Tcl
 * formatting policy; optional word wrapping is applied only while returning a
 * completed output string to Tcl.
 */

#include "tclzmachine.h"
#include "zmachine_wrap.h"

#include <stdlib.h>
#include <string.h>

/* One Tcl-visible game session and its presentation configuration. */
typedef struct Session {
    char *name;                  /* Unique name supplied by the Tcl caller. */
    ZMachine *vm;               /* Independent interpreter instance. */
    size_t wordwrap_bytes;       /* 0 disables automatic output wrapping. */
    struct Session *next;        /* Singly-linked extension session list. */
} Session;

/* Interpreter-associated state shared by the Tcl command implementations. */
typedef struct ExtensionState {
    Session *sessions;
} ExtensionState;

/* Allocate a C-owned duplicate of a Tcl string. */
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

/* Find a named session without exposing the linked-list representation. */
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

/* Free all sessions when the containing Tcl interpreter is destroyed. */
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

/* Tcl command: zmachine::create session storyfile */
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
    s->wordwrap_bytes = 0U; /* Preserve canonical story output by default. */

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

/*
 * Tcl command: zmachine::command session command
 *
 * Player input is queued and the VM runs cooperatively until it asks for the
 * next line, halts, or errors.  Word wrapping is applied to a temporary Tcl
 * string only after execution finishes, keeping canonical VM output intact.
 */
static int cmd_command(ClientData clientData, Tcl_Interp *interp,
                       int objc, Tcl_Obj *const objv[])
{
    ExtensionState *state = (ExtensionState *)clientData;
    Session *s;
    const char *name;
    const char *line;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "session command");
        return TCL_ERROR;
    }

    name = Tcl_GetString(objv[1]);
    line = Tcl_GetString(objv[2]);
    s = find_session(state, name);
    if (!s) {
        Tcl_SetObjResult(interp,
            Tcl_ObjPrintf("unknown session \"%s\"", name));
        return TCL_ERROR;
    }

    if (zmachine_supply_input(s->vm, line) != TCL_OK ||
        zmachine_run(s->vm) != TCL_OK) {
        Tcl_SetObjResult(interp,
            Tcl_NewStringObj(zmachine_last_error(s->vm), -1));
        return TCL_ERROR;
    }

    if (s->wordwrap_bytes == 0U) {
        Tcl_SetObjResult(interp,
            Tcl_NewStringObj(zmachine_output_data(s->vm),
                             zmachine_output_length(s->vm)));
    } else {
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

/*
 * Tcl command: zmachine::configure session ?-wordwrap ?bytes??
 *
 * With no option, return all presentation settings as a dictionary.  With an
 * option only, return its current value.  Supplying a value changes that
 * setting.  -wordwrap 0 disables wrapping; positive values are maximum UTF-8
 * byte counts per returned physical line.
 */
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

/* Tcl command: zmachine::info session */
static int cmd_info(ClientData clientData, Tcl_Interp *interp,
                    int objc, Tcl_Obj *const objv[])
{
    ExtensionState *state = (ExtensionState *)clientData;
    Session *s;
    Tcl_Obj *dict;
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
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("wordWrapBytes", -1),
                   Tcl_NewWideIntObj((Tcl_WideInt)s->wordwrap_bytes));
    Tcl_SetObjResult(interp, dict);
    return TCL_OK;
}

/* Tcl command: zmachine::destroy session */
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

/*
 * Package initialization entry point called by Tcl's load command.
 *
 * The extension requests the Tcl 8.6 stub API, creates its namespace and
 * commands, and associates the allocated ExtensionState with interpreter
 * deletion so no game sessions leak when Tcl exits.
 */
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
