# tclzmachine 1.0.0 Release Notes

## Status

This document describes the `1.0.0` release-candidate line on branch `Frobnost`.

The package metadata is intentionally moved to the final `1.0.0` identity before tagging so the exact candidate can be built, installed, loaded through Tcl, and qualified with the same version strings that the release will ship.

The `1.0.0` tag should be created only after the exact metadata/documentation commit has:

- a successful complete GitHub Actions run;
- all 33 CTest entries passing;
- deterministic V3 and V5 compiled fixtures confirmed current;
- a successful local supported-story catalog probe for the 323-story corpus.

## Supported story versions

`tclzmachine 1.0.0` supports Z-machine story versions:

```text
1, 2, 3, 4, 5, 7, 8
```

Version 6 is intentionally unsupported because its graphics/window/pixel-oriented presentation model conflicts with the project's deliberately text-only Tcl/IRC architecture.

Recommended release wording is:

> Supports Z-machine story versions 1-5, 7, and 8 in a text-only Tcl/IRC runtime, with complete named-opcode coverage for those versions and conservative capability advertisement. Version 6 is intentionally unsupported.

## Major 1.0 capabilities

### Embedding and sessions

- Tcl 8.6+ loadable C extension.
- Multiple independent named interpreter sessions in one process.
- Cooperative execution that returns to Tcl for player input and host file decisions.
- Host-owned filesystem/path policy for saves, restores, transcripts, replay files, and command recording.

### Core VM

- LONG, SHORT, VARIABLE, and V5+ EXTENDED instruction decoding.
- Version-specific opcode legality, structural arity, and literal-value preflight before operand evaluation where the rule is knowable from the encoded instruction.
- Stack, locals, globals, indirect variables, calls/returns, branches, stores, routine frames, `catch`, and `throw`.
- Arithmetic, logical, shift, direct-memory, object, attribute, property, control-flow, lexical, text, stream, save/restore, undo, and compatibility semantics required by the supported-version opcode inventory.
- V7 routine/string packed-address offsets and V8 address scaling.

### Text and input

- Z-text decoding, abbreviations, default/custom alphabets, inline strings, and object names.
- Standard ZSCII translations plus story-defined Unicode translation tables.
- Cooperative `read`/`sread`/`aread` and `read_char`.
- V5+ preloaded line-input buffers and terminating-character tables.
- Exact numeric ZSCII key API for function/cursor/keypad events.
- Dictionary lookup, parser tokenization, `tokenise`, and `encode_text`.

### Streams and persistence

- Input stream 1 command replay.
- Output stream 2 UTF-8 transcript files.
- Output stream 4 command/key recording.
- Nested output stream 3 raw-ZSCII memory capture.
- One-level `save_undo` / `restore_undo`.
- Cooperative Quetzal full-game save/restore with `IFhd`, `UMem`, and `Stks` writing plus `UMem` and compressed `CMem` restore support.
- V5+ auxiliary byte-region save/restore files.

### IRC-oriented presentation

- Canonical VM output remains plain UTF-8.
- Optional mIRC output format maps Z-machine standard/true colours plus bold, italic, and reverse styles to IRC controls.
- Optional UTF-8-safe byte-oriented wrapping for IRC payload limits.
- Formatting-aware mIRC wrapping avoids style leakage and control-only continuation lines.

## Real-story qualification

The filtered supported-version catalog contains 323 stories. It has produced:

```text
PASS=323
FAIL=0
SKIP=0
TOTAL=323
```

The catalog probe loads each story, runs to its initial cooperative input point, sends `look`, and runs to the next cooperative stop. Failures include VM state and a byte window around the failing PC.

The original compatibility-hardening result was recorded at commit:

```text
8ead25b86d6901be2eb2160a6d55fea350bf592b
```

The supported corpus was then successfully rerun after the pre-1.0 dead-run-loop/source-cleanup checkpoint:

```text
03b5277ea5979530496225e1f1ceeff7b08c46df
```

See `CATALOG_COMPATIBILITY.md` for details.

This is a startup/input smoke qualification, not a claim that all 323 stories were played to completion.

## Historical compatibility accommodations

The 1.0 runtime contains deliberately narrow accommodations for shipped stories where strict rejection would reduce useful compatibility without providing a better defined result:

- released Galatea zero-operand `read_char` is normalized to `read_char 1` without consuming an extra story byte;
- null-object `get_parent`, `get_sibling`, and `get_child` queries yield the null result;
- null-object/property `get_prop_addr` probes yield zero;
- null-object `get_prop` reads use the property default;
- `get_prop` on a property longer than two bytes returns its first word where the Standard leaves the result unspecified;
- direct `loadw`, `loadb`, `storew`, and `storeb` address arithmetic wraps in the 16-bit Z-machine address domain before bounds checking;
- legacy Version-5 ZSCII TAB output reduces to one space on the text-only surface;
- timed `read`/`read_char` forms degrade to ordinary untimed input when a story issues them despite timed input being unadvertised.

These are not general requests to ignore malformed stories. Mutating object/property operations and defined invalid forms remain strict unless the specification or a concrete shipped-story compatibility case justifies otherwise.

## Known and intentional limitations

### No Version 6

Version 6 is rejected explicitly.

### Reduced presentation model

The runtime does not provide a complete screen/window/cursor terminal model. Graphics, mouse input/state, menus, and pixel-positioned presentation are not implemented. Upper/status-window presentation is reduced conservatively so narrative output is not corrupted by a pretend terminal model.

### Timed input

Timed keyboard input is not advertised as a capability and timer callbacks are not executed. Timed forms encountered in older stories use the untimed compatibility fallback described above.

### Sound

Sampled sound is not implemented or advertised. Safe sound forms may be consumed according to the documented text-only compatibility policy, but the host does not claim audio capability.

### Fonts/fixed pitch

Fixed-pitch requests cannot be guaranteed by an IRC/Tcl host and are not advertised as a visual capability. mIRC output can represent only the styles and colours the renderer actually supports.

### Formal Standards revision

Header bytes `$32/$33` remain `0.0` intentionally. The project has known presentation boundaries documented in `CONFORMANCE.md` and `PRESENTATION_AUDIT.md`, so packaging the runtime as version 1.0.0 does **not** mean it claims formal Z-machine Standard 1.0 or 1.1 conformance.

## Tcl 1.0 package identity

The extension provides:

```tcl
package require tclzmachine 1.0.0
```

The public C header defines:

```c
#define TCLZMACHINE_VERSION "1.0.0"
```

CMake declares project version `1.0.0`, and installation uses a version-derived `tclzmachine1.0.0` package directory containing the shared library and generated `pkgIndex.tcl`.

## Public Tcl API

The 1.0 Tcl commands are:

```text
zmachine::create
zmachine::command
zmachine::key
zmachine::info
zmachine::configure
zmachine::streamfile
zmachine::save
zmachine::restore
zmachine::cancel
zmachine::destroy
```

The API and host-request dictionaries are documented in `README.md`.

## Testing

The repository currently registers 33 CTest entries covering decoding, state, objects/properties, text, execution, cooperative input, preflight side effects, mIRC presentation, streams, auxiliary files, undo, Quetzal, wrapping, lexical operations, package loading, and deterministic V3/V5 story integration.

GitHub Actions also recompiles the project-owned V3 and V5 story fixtures from their Inform sources and checks whether the committed binaries remain current.

## Source and licensing

The implementation was written independently from the public Z-machine specification. It does not contain or depend on third-party interpreter source.

Third-party story files used for local compatibility testing are not committed to the repository unless their licensing explicitly permits redistribution.
