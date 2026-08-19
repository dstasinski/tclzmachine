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

The interpreter will not expose graphics, cursor positioning, mouse input, sound, fonts, colors, menus, or rich terminal layout. Where a supported story uses presentation opcodes that can safely degrade to plain text, the runtime should preserve meaningful textual output and discard presentation details.

## Current status

This repository is a starter implementation. It provides:

- a Tcl 8.6 loadable C extension
- independent named game sessions
- Z-machine story-file loading
- header parsing for V1-V5, V7, and V8
- explicit rejection of V6
- version-specific story-length scaling
- version-specific packed routine/string address decoding
- V7 routine and string offset support
- per-session memory, program counter, stack, input, and output state
- Tcl commands for creating, querying, using, and destroying sessions
- a placeholder execution loop ready for opcode implementation
- focused unit tests for version/layout rules
- CMake build and install support

It does **not yet execute Z-machine instructions**.

## Planned Tcl API

```tcl
package require tclzmachine

zmachine::create game1 /path/to/zork1.z3
puts [zmachine::command game1 "look"]
puts [zmachine::info game1]
zmachine::destroy game1
```

The intended behavior of `zmachine::command` is to supply one line of player input, resume execution, collect text output, and return when the VM next requests line input, halts, or encounters an error.

## Build

Requirements:

- C compiler
- CMake 3.16+
- Tcl 8.6 development headers and library

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake tcl8.6-dev
```

Build and test:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Install:

```sh
sudo cmake --install build
```

## Implementation roadmap

1. Instruction decoder for long, short, variable and extended forms, with version-gated opcode dispatch.
2. Operand loading, variable semantics, stores and branches.
3. Evaluation stack and routine call frames.
4. Memory access helpers enforcing dynamic/static/high-memory rules.
5. Z-string, abbreviation, ZSCII and Unicode decoding needed for text output.
6. Core arithmetic, logical, memory and control-flow opcodes.
7. Object tables and property operations for both V1-V3 and V4+ layouts.
8. Dictionary lookup, lexical analysis and tokenization.
9. Line input and `read` suspension/resumption for the Tcl request/response model.
10. Text-oriented handling of V4+ window/status opcodes without exposing cursor/layout data.
11. Quetzal save/restore and restart behavior.
12. Compatibility testing against representative V1-V5, V7 and V8 story files.

## Version-dependent rules already isolated

`src/zmachine_version.c` contains the compatibility rules that should not be scattered through the opcode engine. In particular:

- V1-V3 packed addresses use `2P`.
- V4-V5 packed addresses use `4P`.
- V7 routine addresses use `4P + 8R_O` and string addresses use `4P + 8S_O`.
- V8 packed addresses use `8P`.
- Header file-length words scale by 2 for V1-V3, 4 for V4-V5, and 8 for V7-V8.

Keeping these rules centralized is intentional so the VM can share one instruction engine across its supported versions.

## IRC-oriented design

A game session is intended to remain resident as a native VM instance. Tcl supplies a command and receives only the resulting text:

```text
IRC/Tcl -> one input line -> VM executes -> next input request -> Tcl string
```

IRC framing, flood control, user ownership, persistence policy, and splitting long responses into IRC messages belong in the Tcl/bot layer rather than the VM core.

## Licensing

No third-party Z-machine interpreter source is included. Add an explicit project license before publishing the repository.
