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

V6 is excluded because its presentation model is unusually screen-oriented and does not fit the project's IRC/text-only purpose. V7 and V8 are retained because their VM model is useful for later, larger text-oriented Z-code games while remaining practical for a text-only frontend.

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
- routine calls and returns
- arithmetic, logical, memory, control-flow, object, attribute, and property opcodes used by current compatibility tests
- Z-text decoding, abbreviations, default/custom alphabets, inline strings, object short names, and canonical UTF-8 output
- standard default ZSCII 155-223 Unicode translations and V5+ story-defined Unicode translation tables through header-extension word 3
- cooperative `read` and `read_char` suspension/resumption
- dictionary lookup and parse-buffer tokenization
- restart, verify, random, scan-table, argument-count, and related compatibility behavior
- text-only presentation handling including nested output stream 3 memory capture with original ZSCII-byte preservation
- dynamically allocated one-level `save_undo` / `restore_undo` state restoration
- optional per-session UTF-8-safe byte-oriented word wrapping for IRC payloads
- focused CTest coverage for decoder, state, object, text, input, execution, property, undo, presentation, and wrapping behavior
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

The local compatibility catalog currently completes its startup/input smoke probe for all 33 tested story files, including the V5 cases which previously exposed presentation-table, indirect-variable, and extended-opcode dispatch issues. This is a smoke-test milestone rather than a claim of complete Z-machine conformance.

Official story files themselves are **not** committed to this repository.

## Tcl API

Load the package and create one independent game session:

```tcl
package require tclzmachine

zmachine::create game1 /path/to/zork1.z3
```

Send one player command. The call resumes the VM and returns when the story asks for another line of input, halts, or encounters an error:

```tcl
set response [zmachine::command game1 "look"]
puts $response
```

Inspect session metadata:

```tcl
puts [zmachine::info game1]
```

Configure optional output wrapping. The value is a maximum UTF-8 byte count per returned physical line; `0` disables automatic wrapping and is the default:

```tcl
zmachine::configure game1 -wordwrap 400
```

Destroy the session when finished:

```tcl
zmachine::destroy game1
```

## IRC-oriented output wrapping

Wrapping is intentionally a **presentation-layer feature**. The Z-machine core always generates canonical, unwrapped text. `zmachine::command` applies the configured session limit only while returning that text to Tcl.

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

## Project test game

The first-release test plan includes a small purpose-built interactive-fiction fixture owned by this project. Both its human-readable source and compiled Z-machine story files will live under `tests/games/` so normal integration testing does not require an Inform compiler.

Planned layout:

```text
tests/games/
├── source/
│   └── tclzmachine-test.inf
├── compiled/
│   ├── tclzmachine-test.z3
│   └── tclzmachine-test.z5
└── README.md
```

The fixture will exercise deterministic text output, input parsing, branches, arithmetic, routine calls, object movement, inventory, properties, state changes, and quit behavior. Official games remain separate real-world compatibility tests.

## Documentation requirement

All project `.c` and `.h` files are expected to be fully commented for the 1.0 release. Comments should explain file purpose, public/internal API contracts, structures and fields, version-specific behavior, non-obvious algorithms, and important Z-machine specification decisions rather than merely restating C syntax.

## Implementation roadmap to 1.0

1. Continue opcode compatibility work using real story files as probes.
2. Complete text-oriented handling for remaining safe presentation/status opcodes.
3. Implement file-based save/restore, with Quetzal-compatible persistence as the preferred target; one-level in-memory undo is already implemented.
4. Add the project-owned compiled Z3/Z5 integration game and scripted conversations.
5. Broaden compatibility testing across representative V1-V5, V7, and V8 stories.
6. Complete the source/header documentation audit.
7. Harden error handling, malformed-story bounds checking, and API documentation.

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
IRC/Tcl -> one input line -> VM executes -> next input request -> Tcl string
```

IRC framing, flood control, user ownership, authentication, persistence policy, channel routing, and bot-specific behavior belong in Tcl/the bot rather than the VM core.

## Licensing

No third-party Z-machine interpreter source is included. The repository's license applies to this independent implementation; official Infocom story data is not part of the project.
