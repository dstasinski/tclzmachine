# tclzmachine

tclzmachine 1.0.0 is a lightweight, embeddable, multi-session, **text-only Z-machine runtime** written in C for Tcl 8.6+ applications, with IRC bots as the primary use case.

The project is an independent implementation based on the public Z-machine specification. It does not embed or depend on Frotz, Bocfel, or another interpreter implementation.

## Release status

The `Frobnost` branch is the 1.0.0 release-candidate line. The runtime has complete named-opcode coverage for the supported story versions, a focused deterministic regression suite, repository-owned V3/V5 integration stories, and a manually qualified real-story catalog.

The supported-story catalog has completed the startup/`look` smoke probe for **323 of 323 supported stories**. That is strong compatibility evidence, but it is not a claim that every story was played to completion or that every path in every story executed.

See:

- [RELEASE_NOTES_1.0.md](RELEASE_NOTES_1.0.md) for the 1.0.0 release summary and known limits;
- [CATALOG_COMPATIBILITY.md](CATALOG_COMPATIBILITY.md) for the real-story qualification record;
- [CONFORMANCE.md](CONFORMANCE.md) for Standards-oriented capability boundaries;
- [OPCODE_STATUS.md](OPCODE_STATUS.md) for the opcode inventory;
- [PRESENTATION_AUDIT.md](PRESENTATION_AUDIT.md) for the text-only presentation audit;
- [SOURCE_DOCUMENTATION_AUDIT.md](SOURCE_DOCUMENTATION_AUDIT.md) for the 1.0 source cleanup record.

## Supported Z-machine versions

| Z-machine version | Status |
| --- | --- |
| V1 | Supported |
| V2 | Supported |
| V3 | Supported |
| V4 | Supported |
| V5 | Supported |
| V6 | **Intentionally unsupported** |
| V7 | Supported |
| V8 | Supported |

V6 is excluded because its presentation model depends heavily on pixel-positioned windows, graphics, and richer terminal state that do not fit this project's Tcl/IRC text-only architecture. V7 and V8 are supported using their V5-derived text-oriented semantics plus their version-specific packed-address and file-size rules.

## What 1.0 provides

The runtime includes:

- Tcl 8.6 loadable C extension with independent named game sessions;
- story loading and header handling for V1-V5, V7, and V8;
- LONG, SHORT, VARIABLE, and V5+ EXTENDED instruction decoding;
- version-aware legality/arity/literal preflight before observable operand side effects;
- evaluation stack, local/global variables, routine frames, calls/returns, branches, stores, `catch`/`throw`, and indirect-variable semantics;
- arithmetic, logical, shifts, memory, object, attribute, property, control-flow, text, lexical, stream, file, save/restore, and compatibility opcodes required by the supported-version inventory;
- Z-text decoding, abbreviations, default/custom alphabets, object short names, ZSCII, and story-defined Unicode translation tables;
- cooperative line and character input, including V5+ preloaded line buffers and terminating-character-table function keys;
- command replay, transcript output, command/key recording, and nested output stream 3 memory capture;
- dictionary lookup, tokenization, `tokenise`, and `encode_text`;
- restart, checksum verification, random, scan-table, and argument-count behavior;
- one-level `save_undo` / `restore_undo` support;
- cooperative Quetzal full-game save/restore with `IFhd`, `UMem`, `Stks`, and compressed `CMem` restore support;
- V5+ auxiliary byte-region save/restore requests;
- optional mIRC colour/style rendering that leaves canonical VM output plain;
- optional UTF-8-safe byte-count wrapping for IRC payloads;
- 33 CTest entries, including public Tcl package smoke coverage and repository-owned V3/V5 story integration tests.

## Deliberate text-only boundaries

Canonical VM output is plain UTF-8. The runtime does **not** provide a full terminal/window implementation, graphics, mouse state, menus, sampled sound, or V6 presentation semantics. Upper/status-window behavior is reduced conservatively rather than emulated as a fake terminal.

Timed `read` and `read_char` capability is not advertised. For compatibility with shipped older stories that nevertheless issue timed forms, those requests degrade to ordinary untimed host input and no timer callback is executed.

The optional mIRC renderer can advertise and render standard/true colours plus bold and italic emphasis when the host enables it. Fixed-pitch has no useful IRC equivalent. Plain output does not advertise formatting it cannot render.

The formal Z-machine Standards revision header bytes remain **0.0**. tclzmachine 1.0.0 should therefore be described as supporting story versions 1-5, 7, and 8 in a text-only runtime, **not** as formally Z-machine Standard 1.0/1.1 conformant. See `CONFORMANCE.md` for the reasons.

## Build

Requirements:

- C compiler with C99 support;
- CMake 3.16+;
- Tcl 8.6 development headers and library.

On Debian, Ubuntu, or Linux Mint:

```sh
sudo apt install build-essential cmake tcl8.6-dev
```

Build and run the complete deterministic suite:

```sh
cmake -S . -B build -DTCLZMACHINE_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Install:

```sh
sudo cmake --install build
```

The 1.0.0 package installs beneath the platform library directory in a versioned `tclzmachine1.0.0` directory containing the shared library and generated `pkgIndex.tcl`.

## Tcl package and API

Load the installed package with:

```tcl
package require tclzmachine 1.0.0
```

The public Tcl commands are:

| Command | Purpose |
| --- | --- |
| `zmachine::create session storyfile` | Create a named session and load a story. |
| `zmachine::command session text` | Supply line-oriented player input and resume execution. |
| `zmachine::key session zsciiCode` | Supply one exact ZSCII key event. |
| `zmachine::info session` | Return VM/cooperative-request metadata as a Tcl dict. |
| `zmachine::configure session ?option ?value??` | Query/set host presentation options. |
| `zmachine::streamfile session kind path` | Supply or preconfigure replay/transcript/record paths. |
| `zmachine::save session path` | Complete a pending full or auxiliary save request. |
| `zmachine::restore session path` | Complete a pending full or auxiliary restore request. |
| `zmachine::cancel session` | Decline a pending save/restore/stream-file request. |
| `zmachine::destroy session` | Destroy a session and release its resources. |

### Basic session

```tcl
package require tclzmachine 1.0.0

zmachine::create game1 /path/to/story.z5

set text [zmachine::command game1 "look"]
puts $text

zmachine::destroy game1
```

`zmachine::command` resumes the VM until the story requests more line/character input, requests a host filename, halts, or fails. Line input accepts printable ASCII host text; the runtime rejects arbitrary Tcl UTF-8 rather than misinterpreting multibyte input as multiple ZSCII bytes.

### Exact key input

Use `zmachine::key` while the story is waiting on `read_char`, or to terminate a V5+ line read with a function key named by the story's terminating-character table:

```tcl
# ZSCII 129 = cursor up
set text [zmachine::key game1 129]
```

Supported keyboard-style key values include Enter (`13`), Escape (`27`), printable ASCII (`32`-`126`), cursor/function/keypad codes (`129`-`154`), and extra-character codes defined by the story's active Unicode translation table. Mouse/menu event codes are intentionally unavailable.

### Session metadata

```tcl
set info [zmachine::info game1]
```

Important keys include:

- `version` and `supportedVersions`;
- `textOnly`;
- `pc`, `memorySize`, and `declaredFileLength`;
- `state`;
- `inputRequest` — empty, `line`, or `char`;
- `inputStream` — `0` for Tcl keyboard input or `1` for replay;
- `streamRequest` — empty, `replay`, `transcript`, or `record`;
- `commandRecording`;
- `outputFormat` — `plain` or `mirc`;
- `fileRequest` — empty, `save`, or `restore`;
- `fileRequestKind` — empty, `full`, or `auxiliary`;
- `suggestedFileName`, `filePrompt`, `fileTable`, and `fileBytes` for auxiliary file requests;
- `wordWrapBytes`.

The Tcl host owns filesystem policy. Stories can request files, but the extension never decides an arbitrary host path on their behalf.

## Save/restore handshake

A story save/restore opcode yields to Tcl so the host can choose a safe path.

```tcl
set text [zmachine::command game1 "save"]
puts $text

set info [zmachine::info game1]
if {[dict get $info fileRequest] eq "save"} {
    puts [zmachine::save game1 /safe/path/game.sav]
}
```

Restore is symmetrical:

```tcl
set text [zmachine::command game1 "restore"]
if {[dict get [zmachine::info game1] fileRequest] eq "restore"} {
    puts [zmachine::restore game1 /safe/path/game.sav]
}
```

Decline the pending request with:

```tcl
zmachine::cancel game1
```

Full-game files use Quetzal `FORM IFZS`. V5+ auxiliary requests transfer only the story-selected byte range and expose their metadata through `zmachine::info`.

## Replay, transcript, and recording streams

When a story first requests a replay, transcript, or command-recording file and no path is configured, execution yields with `streamRequest` set appropriately.

```tcl
set request [dict get [zmachine::info game1] streamRequest]

switch -- $request {
    replay     { zmachine::streamfile game1 replay /safe/path/commands.txt }
    transcript { zmachine::streamfile game1 transcript /safe/path/transcript.txt }
    record     { zmachine::streamfile game1 record /safe/path/commands.out }
}
```

`zmachine::streamfile` can also be called before the story selects the stream. Replay and recording share the Standard-style human-readable `[N]` key marker convention, for example:

```text
look
turn it on.[154]
[129]
```

Output stream 3 remains VM memory output: it stores raw ZSCII bytes and is kept separate from UTF-8 host transcript output and optional mIRC presentation.

## Optional mIRC presentation

Plain output is the default:

```tcl
zmachine::configure game1 -format plain
```

For an IRC client that understands traditional mIRC controls:

```tcl
zmachine::configure game1 -format mirc
```

The renderer maps Z-machine colours to the traditional mIRC palette and supports bold, italic, and reverse-video controls. `set_true_colour` is approximated to the nearest available traditional mIRC colour where exact reproduction is impossible.

mIRC controls are host presentation only. They never contaminate transcript files, command files, stream-3 tables, parser input, Quetzal state, or `zmachine_output_data()`.

## IRC-oriented output wrapping

Word wrapping is disabled by default. Set a maximum returned byte count per physical line with:

```tcl
zmachine::configure game1 -wordwrap 380
```

The wrapper measures bytes, prefers whitespace boundaries, preserves story newlines, does not split a UTF-8 code point, and can hard-wrap a long word when necessary. In mIRC mode, formatting controls remain atomic and active formatting is re-established after inserted line breaks without producing control-only continuation lines.

A bot can then send each returned line independently:

```tcl
foreach line [split [zmachine::command game1 "look"] "\n"] {
    # Send $line using the bot's IRC library.
}
```

## Real-story compatibility probes

Third-party story files are not committed to this repository. Local copies can be tested with the supplied probes.

Single story:

```sh
tclsh tests/probe_story.tcl \
    ./build/tclzmachine.so \
    /path/to/story.z5 \
    look
```

Persistent multi-command session:

```sh
tclsh tests/probe_session.tcl \
    ./build/tclzmachine.so \
    /path/to/story.z3 \
    "look" \
    "open mailbox" \
    "take leaflet" \
    "inventory"
```

Catalog startup/`look` smoke probe:

```sh
tclsh tests/probe_catalog.tcl \
    ./build/tclzmachine.so \
    /path/to/story/catalog
```

Failures include VM state plus a byte window around the failing PC so compatibility work can proceed from the exact encoded instruction.

The qualified supported-version corpus currently contains 323 stories and has produced:

```text
PASS=323
FAIL=0
SKIP=0
TOTAL=323
```

See `CATALOG_COMPATIBILITY.md` for the exact checkpoints and interpretation.

## Project-owned integration stories

The repository contains deterministic V3 and V5 fixtures under `tests/games/` with both source and compiled stories committed. Ordinary CTest therefore does not require an Inform compiler.

The V5 fixture uses the Inform 6 standard library and exercises a real parser, object/inventory state, room movement, lexical operations, and Quetzal save/restore through the public Tcl API.

The V3 fixture deliberately avoids the standard library and exercises V1-V3 line-buffer format, routine/call behavior, byte-sized object-tree links, object movement, globals, branching, and V3 branch-form full-game save/restore.

GitHub Actions additionally rebuilds those deterministic stories from source and verifies that the committed binaries are current.

## Architecture

A session remains resident as one native VM instance:

```text
IRC/Tcl -> input line/key -> VM executes -> input/file request -> Tcl
```

C owns Z-machine semantics, story memory, decoding, execution, persistence, and low-level validation. Tcl/the embedding application owns session names, safe filesystem paths, IRC routing, authentication, flood control, and other host policy.

Canonical output is deliberately presentation-neutral. The optional mIRC renderer is a final host-facing transformation rather than VM state.

## Licensing and story files

No third-party Z-machine interpreter source is included. The repository license applies to this independent implementation.

Commercial, freeware, or other third-party story files used for compatibility testing are not part of the project and must not be committed unless their licensing explicitly permits redistribution.
