# tclzmachine

tclzmachine is a lightweight, embeddable, multi-session, **text-only Z-machine runtime** written in C for Tcl applications, with IRC bots as the primary use case.

The project is an independent implementation based on the public Z-machine specification. It does not embed or depend on Frotz, Bocfel, or another Z-machine interpreter.

## Compatibility target

The intended VM compatibility target is:

| Z-machine version | Goal |
| --- | --- |
| V1 | Supported |
| V2 | Supported |
| V3 | Supported |
| V4 | Supported |
| V5 | Supported |
| V6 | **Intentionally unsupported** |
| V7 | Supported |
| V8 | Supported |

V6 is excluded because its presentation model is unusually screen-oriented and does not fit the project's IRC/text-only purpose. V7 and V8 are retained because their VM model follows the V5 text-oriented model while extending address/file-size rules useful for larger Z-code games.

The interpreter does not expose graphics, cursor positioning, mouse input, sound, fonts, colors, menus, or rich terminal layout. Where a supported story uses presentation opcodes that can safely degrade to plain text, the runtime should preserve meaningful textual output and discard presentation details.

## Current status

The runtime now contains a working execution core rather than only starter scaffolding. Implemented areas include:

- Tcl 8.6 loadable C extension
- independent named game sessions
- Z-machine story-file loading
- header parsing for V1-V5, V7, and V8
- explicit rejection of V6
- version-specific story-length scaling and packed-address decoding
- V7 routine and string offset support
- per-session story memory, program counter, evaluation stack, call frames, random state, input, and output
- instruction decoding for LONG, SHORT, VARIABLE, and V5+ EXTENDED forms
- variable, local, global, stack, store, branch, and indirect-variable semantics
- routine calls and returns, including V5+ `catch` / `throw`
- arithmetic, logical, shift, memory, control-flow, object, attribute, and property opcodes used by current compatibility tests
- Z-text decoding, abbreviations, default/custom alphabets, inline strings, object short names, and canonical UTF-8 output
- standard default ZSCII 155-223 Unicode translations and V5+ story-defined Unicode translation tables through header-extension word 3
- cooperative `read` and `read_char` suspension/resumption, including V5+ preloaded line-input buffers
- printable-ASCII host line validation plus exact numeric ZSCII key input for `read_char` and V5+ terminating-character-table line completion
- Z-machine input stream 1 command replay, output stream 2 transcript files, and output stream 4 command/key recording through host-selected paths
- dictionary lookup, parse-buffer tokenization, V5+ `tokenise`, and `encode_text`, including custom alphabet tables and standard lowercase dictionary encryption
- restart, verify, random, scan-table, argument-count, and related compatibility behavior
- form-aware cooperative dispatch which keeps EXTENDED opcodes distinct from VAR-table opcodes
- text-only presentation handling including nested output stream 3 memory capture with original ZSCII-byte preservation
- dynamically allocated one-level `save_undo` / `restore_undo` state restoration
- cooperative full-game save/restore with Quetzal FORM IFZS persistence
- V5+ operand-bearing auxiliary save/restore byte-region files
- Quetzal UMem writing plus UMem and compressed CMem restore support
- optional per-session UTF-8-safe byte-oriented word wrapping for IRC payloads
- focused CTest coverage for decoder, state, object, text, input, execution, property, undo, Quetzal, external/auxiliary files, presentation, tokenization, wrapping, and operand-side-effect behavior
- repository-owned V3 and V5 end-to-end Tcl integration stories, including full-game save/restore
- manual real-story compatibility probes

### Real-game compatibility reached so far

An official Version 3 Zork I story (Revision 88 / serial 840726) has been tested locally through the Tcl API. The runtime successfully boots the story, reaches the input prompt, and preserves state across a multi-turn sequence including:

```text
look
open mailbox
take leaflet
read leaflet
inventory
```

The local compatibility catalog has completed its startup/input smoke probe for all 33 tested story files, including V5 cases which exposed presentation-table, indirect-variable, null-object, lexical-opcode, and extended-opcode dispatch issues during development. This remains a smoke-test milestone rather than a claim of complete Z-machine conformance.

Official story files themselves are **not** committed to this repository.

## Tcl API

Load the package and create one independent game session:

```tcl
package require tclzmachine

zmachine::create game1 /path/to/zork1.z3
```

Send one player command. `zmachine::command` is the line-oriented API used for `read`/`sread`/`aread` input. The current host keyboard contract accepts printable ASCII ZSCII (space through `~`) and rejects control bytes or non-ASCII Tcl UTF-8 input rather than misinterpreting multibyte UTF-8 as multiple ZSCII characters:

```tcl
set response [zmachine::command game1 "look"]
puts $response
```

The call resumes the VM and returns when the story asks for another line/character of input, requests a save/restore or external stream filename, halts, or encounters an error.

Use `zmachine::key` to supply one exact numeric ZSCII keyboard event when the story is waiting for character input:

```tcl
# ZSCII 129 = cursor up
set response [zmachine::key game1 129]
puts $response
```

For `read_char`, `zmachine::key` accepts ordinary keyboard-input ZSCII such as Enter (`13`), Escape (`27`), printable ASCII (`32`-`126`), cursor/function/keypad codes (`129`-`154`), and extra-character codes that are defined by the story's active Unicode translation table. Undefined/reserved codes are rejected without consuming the pending input request, so the host may retry. Mouse/menu event codes `252`-`254` are intentionally unavailable because this text-only runtime exposes no mouse or click-position state.

For a suspended line read, `zmachine::key` may supply Enter (`13`) or, in V5 and later, a keyboard function key (`129`-`154`) named by the story's terminating-character table at header word `$2e`. Table value `255` means any function key. The terminating key becomes `aread`'s stored result and is not inserted into the text buffer; any V5+ preloaded text already in that buffer is preserved and tokenized normally. An unlisted function key is rejected without consuming the pending line read.

The older convenience behavior remains: if a normal `zmachine::command` reaches `read_char` while its command string is already queued, the first printable ASCII character can satisfy that request. `zmachine::key` is the unambiguous API for non-ASCII ZSCII keyboard events.

Inspect session metadata:

```tcl
set info [zmachine::info game1]
puts $info
```

`inputRequest` is empty when no cooperative input is pending, `line` while the session is suspended on `read`/`sread`/`aread`, and `char` while it is suspended on `read_char`. This lets an embedding bot choose between `zmachine::command` and `zmachine::key` without decoding VM instructions itself.

External Z-machine stream state is reported through:

- `streamRequest` - empty normally, or `replay`, `transcript`, or `record` while the story is waiting for a host path
- `inputStream` - `0` for interactive Tcl input or `1` while command-file replay is selected
- `commandRecording` - boolean indicating whether output stream 4 is currently recording commands/key presses

`fileRequest` is independent of those stream fields. It is empty during ordinary play and becomes `save` or `restore` when the story has yielded for save/restore file selection. `fileRequestKind` is `full` for a complete Quetzal game-state request and `auxiliary` for a V5+ byte-region file request.

For auxiliary requests, the same dictionary also exposes:

- `suggestedFileName` - the story's filename normalized to uppercase 8.3-style form, with `.AUX` added when no extension was supplied
- `filePrompt` - `-1` when the optional prompt operand was omitted, `0` when the story requests silent filename use, or `1` when it requests confirmation
- `fileTable` - the story-memory address of the byte region
- `fileBytes` - the requested maximum byte count

The embedding application remains responsible for translating all filename requests into safe host paths.

### Command replay, transcript, and recording streams

Z-machine command/replay and transcript filenames are host policy just like saved-game filenames. When a story first selects input stream 1, output stream 2, or output stream 4 and no path has been configured yet, execution yields and `streamRequest` identifies the needed file. Supply it with:

```tcl
set info [zmachine::info game1]
set request [dict get $info streamRequest]

switch -- $request {
    replay {
        set more [zmachine::streamfile game1 replay /safe/path/commands.txt]
    }
    transcript {
        set more [zmachine::streamfile game1 transcript /safe/path/transcript.txt]
    }
    record {
        set more [zmachine::streamfile game1 record /safe/path/commands.out]
    }
}
```

`zmachine::streamfile` may also be called before a story selects a stream to preconfigure its path. Once a transcript or command-recording file has been chosen, deselecting and reselecting that stream reuses the same open file rather than asking the host for a filename repeatedly. `zmachine::cancel game1` declines a pending stream request and lets execution continue with that external stream unselected.

Input stream 1 automatically feeds command records while the story is waiting on `read` or `read_char`. At end of file, replay closes and input returns to stream 0 so Tcl can resume interactive input.

Output stream 4 and input stream 1 share a simple human-readable command format compatible with the Standard's suggested `[N]` convention:

```text
look
turn it on.[154]
[129]
```

The first line is an ordinary Enter-terminated command. The second is a line terminated by ZSCII function key `154`. A line containing only `[129]` represents an exact `read_char` keypress. Output stream 4 writes a completed command in one operation after input finishes; exact `read_char` keys are written as their numeric marker.

When transcript stream 2 is active, story output is copied to its UTF-8 host file and V1-V5 completed line input is echoed there. While output stream 3 is active, its standard exclusive-output behavior suppresses story text from the other selected output streams. Completed V1-V5 line input is also preserved in the canonical VM output returned through Tcl as required by the Z-machine input-echo rules. If an IRC or other embedding application wants to avoid displaying a command twice because the transport already showed the user's message, that suppression belongs to host presentation policy rather than the VM core.

### Save/restore handshake

Filename policy deliberately belongs to Tcl/the embedding application rather than the VM. When a story executes a full-game or auxiliary save/restore opcode, execution yields. The host can choose a path, retry after an I/O error, or decline the request.

Example save handling:

```tcl
set text [zmachine::command game1 "save"]
puts $text

set info [zmachine::info game1]
if {[dict get $info fileRequest] eq "save"} {
    if {[dict get $info fileRequestKind] eq "auxiliary"} {
        puts "Story suggested: [dict get $info suggestedFileName]"
    }
    set more [zmachine::save game1 /path/chosen/by/the/host]
    puts $more
}
```

Restore works the same way:

```tcl
set text [zmachine::command game1 "restore"]
puts $text

if {[dict get [zmachine::info game1] fileRequest] eq "restore"} {
    set more [zmachine::restore game1 /path/chosen/by/the/host]
    puts $more
}
```

To tell the story that the player declined the filename request or that the host does not want to retry a failed file operation:

```tcl
zmachine::cancel game1
```

For a full-game request, a successful save makes the story's save opcode return success, while a successful restore transfers execution back to the original save point with the version-appropriate restored result or branch behavior. Full-game files use Quetzal `FORM IFZS`; tclzmachine writes the required `IFhd`, `UMem`, and `Stks` chunks and accepts either `UMem` or standard compressed `CMem` when restoring.

For a V5+ auxiliary request, `zmachine::save` writes exactly the requested story-memory bytes and stores `1` on success. `zmachine::restore` reads at most the requested byte count into dynamic memory and stores the number of bytes actually loaded; a missing auxiliary file stores `0`. Auxiliary files are not part of the saved state of play. Cancel stores the normal failure result `0` for an auxiliary request.

Configure optional output wrapping. The value is a maximum UTF-8 byte count per returned physical line; `0` disables automatic wrapping and is the default:

```tcl
zmachine::configure game1 -wordwrap 400
```

Destroy the session when finished:

```tcl
zmachine::destroy game1
```

## IRC-oriented output wrapping

Wrapping is intentionally a **presentation-layer feature**. The Z-machine core always generates canonical, unwrapped text. `zmachine::command`, `zmachine::key`, stream/file-request completion calls apply the configured session limit only while returning that text to Tcl.

The wrapper:

- measures limits in UTF-8 bytes rather than characters
- prefers whitespace boundaries
- preserves story-supplied newlines
- does not split inside a UTF-8 code point
- can hard-wrap a single long word when necessary
- is disabled by default

This lets an IRC bot choose a conservative payload size while leaving room for the IRC command, target, tags, prefix, and CRLF framing.

Example:

```tcl
zmachine::configure game1 -wordwrap 380

foreach line [split [zmachine::command game1 "look"] "\n"] {
    # Send $line through the bot's IRC library.
}
```

## Build

Requirements:

- C compiler
- CMake 3.16+
- Tcl 8.6 development headers and library

On Debian/Ubuntu/Linux Mint:

```sh
sudo apt install build-essential cmake tcl8.6-dev
```

Build and test:

```sh
cmake -S . -B build -DTCLZMACHINE_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Install:

```sh
sudo cmake --install build
```

## Real-story compatibility probes

Official Infocom story files are copyrighted and must not be added to this repository. Local copies can be tested with the supplied Tcl probes.

Single command:

```sh
tclsh tests/probe_story.tcl \
    ./build/tclzmachine.so \
    /path/to/story.z3 \
    look
```

Multiple commands in one persistent session:

```sh
tclsh tests/probe_session.tcl \
    ./build/tclzmachine.so \
    /path/to/story.z3 \
    "look" \
    "open mailbox" \
    "take leaflet" \
    "inventory"
```

When a story reaches an unimplemented opcode, the probe prints the VM diagnostic and session state so compatibility work can proceed from the exact failing instruction rather than by guessing.

## Project-owned integration games

The repository contains two purpose-built interactive-fiction fixtures under `tests/games/`. Their human-readable sources and compiled story files are both committed, so normal integration testing does not require an Inform compiler.

```text
tests/games/
├── source/
│   ├── tclzmachine-test.inf
│   └── tclzmachine-v3.inf
├── compiled/
│   ├── tclzmachine-test.z5
│   └── tclzmachine-v3.z3
└── README.md
```

The V5 fixture uses the current Inform 6 standard library and exercises a real parser, object/inventory state, room movement, V5 lexical opcodes, and Quetzal save/restore through the public Tcl API.

The V3 fixture deliberately uses no standard library so it can be a genuine Version 3 story with the current Inform compiler. Its scripted regression exercises the V1-V3 `sread` text-buffer format, V3 routine/call behavior, byte-sized object-tree links, `get_child`, `get_parent`, `insert_obj`, globals, branching, and V3 branch-form full-game save/restore. The restore test saves from inside a called routine, mutates both location and object ownership, then verifies that the saved call frame and world state are reconstructed.

GitHub Actions rebuilds the deterministic fixtures from source and commits changed binaries back to `Frobnost`; ordinary CTest runs the committed binaries directly.

## Documentation requirement

All project `.c` and `.h` files are expected to be fully commented for the 1.0 release. Comments should explain file purpose, public/internal API contracts, structures and fields, version-specific behavior, non-obvious algorithms, and important Z-machine specification decisions rather than merely restating C syntax.

## Implementation roadmap to 1.0

1. Continue systematic opcode compatibility work using real story files and focused project-owned fixtures as probes.
2. Complete text-oriented handling for remaining safe presentation/status opcodes.
3. Broaden owned integration/compatibility coverage for V1, V2, V4, V7, and V8 where targeted fixtures add useful signal.
4. Complete the source/header documentation audit, including implementation and test sources required by the 1.0 documentation standard.
5. Harden malformed-story bounds checking, error diagnostics, and Tcl/API documentation.
6. Run a final standards-oriented release audit beyond the current 33-story startup smoke catalog.

## Version-dependent rules

`src/zmachine_version.c` centralizes compatibility rules that should not be scattered through the opcode engine. In particular:

- V1-V3 packed addresses use `2P`.
- V4-V5 packed addresses use `4P`.
- V7 routine addresses use `4P + 8R_O` and string addresses use `4P + 8S_O`.
- V8 packed addresses use `8P`.
- Header file-length words scale by 2 for V1-V3, 4 for V4-V5, and 8 for V7-V8.

## IRC-oriented architecture

A game session remains resident as one native VM instance:

```text
IRC/Tcl -> one input line/key -> VM executes -> next input/file request -> Tcl
```

IRC framing, flood control, user ownership, authentication, save/stream-path policy, channel routing, and bot-specific behavior belong in Tcl/the bot rather than the VM core.

## Licensing

No third-party Z-machine interpreter source is included. The repository's license applies to this independent implementation; official Infocom story data is not part of the project.
