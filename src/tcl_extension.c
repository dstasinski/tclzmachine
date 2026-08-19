#include "tclzmachine.h"

#include <stdlib.h>
#include <string.h>

typedef struct Session {
    char *name;
    ZMachine *vm;
    struct Session *next;
} Session;

typedef struct ExtensionState {
    Session *sessions;
} ExtensionState;

static char *dup_string(const char *s)
{
    size_t len;
    char *copy;

    if (!s) {
        return NULL;
    }

    len = strlen(s) + 1;
    copy = (char *)malloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

static Session *find_session(ExtensionState *state, const char *name)
{
    Session *s;
    for (s = state->sessions; s; s = s->next) {
        if (strcmp(s->name, name) == 0) {
            return s;
        }
    }
    return NULL;
}

static void free_state(ClientData clientData, Tcl_Interp *interp)
{
    ExtensionState *state = (ExtensionState *)clientData;
    Session *s = state->sessions;
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

static int cmd_create(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
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
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("session \"%s\" already exists", name));
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
        Tcl_SetObjResult(interp, Tcl_NewStringObj(zmachine_last_error(s->vm), -1));
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

static int cmd_command(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
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
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown session \"%s\"", name));
        return TCL_ERROR;
    }

    if (zmachine_supply_input(s->vm, line) != TCL_OK || zmachine_run(s->vm) != TCL_OK) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(zmachine_last_error(s->vm), -1));
        return TCL_ERROR;
    }

    Tcl_SetObjResult(interp,
        Tcl_NewStringObj(zmachine_output_data(s->vm), zmachine_output_length(s->vm)));
    return TCL_OK;
}

static int cmd_info(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
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
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown session \"%s\"", name));
        return TCL_ERROR;
    }

    dict = Tcl_NewDictObj();
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("version", -1), Tcl_NewIntObj(s->vm->version));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("supportedVersions", -1), Tcl_NewStringObj(zmachine_supported_versions(), -1));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("textOnly", -1), Tcl_NewBooleanObj(TCLZMACHINE_TEXT_ONLY));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("pc", -1), Tcl_NewWideIntObj((Tcl_WideInt)s->vm->pc));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("memorySize", -1), Tcl_NewWideIntObj((Tcl_WideInt)s->vm->memory_size));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("declaredFileLength", -1), Tcl_NewWideIntObj((Tcl_WideInt)s->vm->declared_file_length));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("routineOffset", -1), Tcl_NewIntObj(s->vm->routine_offset));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("stringOffset", -1), Tcl_NewIntObj(s->vm->string_offset));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("state", -1), Tcl_NewIntObj((int)s->vm->state));
    Tcl_SetObjResult(interp, dict);
    return TCL_OK;
}

static int cmd_destroy(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
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

    Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown session \"%s\"", name));
    return TCL_ERROR;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int Tclzmachine_Init(Tcl_Interp *interp)
{
    ExtensionState *state;

    if (Tcl_InitStubs(interp, "8.6", 0) == NULL) {
        return TCL_ERROR;
    }

    state = (ExtensionState *)calloc(1, sizeof(*state));
    if (!state) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("unable to allocate extension state", -1));
        return TCL_ERROR;
    }

    Tcl_CreateNamespace(interp, "::zmachine", NULL, NULL);
    Tcl_CreateObjCommand(interp, "::zmachine::create", cmd_create, state, NULL);
    Tcl_CreateObjCommand(interp, "::zmachine::command", cmd_command, state, NULL);
    Tcl_CreateObjCommand(interp, "::zmachine::info", cmd_info, state, NULL);
    Tcl_CreateObjCommand(interp, "::zmachine::destroy", cmd_destroy, state, NULL);

    Tcl_CallWhenDeleted(interp, free_state, state);

    if (Tcl_PkgProvide(interp, "tclzmachine", TCLZMACHINE_VERSION) != TCL_OK) {
        return TCL_ERROR;
    }

    return TCL_OK;
}
