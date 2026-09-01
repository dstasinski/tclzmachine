# Z-machine conformance boundary

This document records the difference between **opcode coverage** and a formal claim of Z-machine Standard conformance for `tclzmachine`.

Primary specification references:

- [Z-machine Standard 1.1 preface](https://inform-fiction.org/zmachine/standards/z1point1/preface.html)
- [Section 8: screen model](https://inform-fiction.org/zmachine/standards/z1point1/sect08.html)
- [Section 11: header](https://inform-fiction.org/zmachine/standards/z1point1/sect11.html)
- [Section 12: object table](https://inform-fiction.org/zmachine/standards/z1point1/sect12.html)
- [Section 15: opcode details](https://inform-fiction.org/zmachine/standards/z1point1/sect15.html)

The companion [`OPCODE_STATUS.md`](OPCODE_STATUS.md) inventories every named opcode applicable to the supported story versions. It currently finds no missing named opcode in V1-V5/V7/V8. That is an implementation-completeness statement, **not** a formal Standards-revision claim.

## Supported story versions

`tclzmachine` accepts:

- Version 1
- Version 2
- Version 3
- Version 4
- Version 5
- Version 7
- Version 8

Version 6 is intentionally rejected because its graphics/window model is outside the project's text-only Tcl/IRC scope.

The Standard explicitly permits an interpreter to support only some Z-machine versions. To call itself a Standard interpreter, however, it should obey the selected revision exactly for every version it claims to interpret. For that reason the header revision bytes at `$32/$33` intentionally remain `0.0` during the 1.0 release line.

## Header capability contract

The interpreter header is intentionally conservative. A story should never be told that a feature exists merely because the corresponding opcode can be safely consumed.

| Capability | Header behavior | Runtime behavior |
| --- | --- | --- |
| V3 status line | Flags 1 bit 4 set: unavailable | No separate status-line rendering |
| V3 screen splitting | Flags 1 bit 5 clear | Split/window opcodes are safely adapted, but no visible upper-window surface is advertised |
| V4+ bold | Flags 1 bit 2 set only in mIRC format | mIRC `0x02` presentation; plain format does not advertise it |
| V4+ italic | Flags 1 bit 3 set only in mIRC format | mIRC italic presentation; plain format does not advertise it |
| V4+ fixed-space style | Flags 1 bit 4 clear | Semantic request is accepted but cannot force the user's IRC/Tcl client font |
| V4+ timed input | Flags 1 bit 7 clear | Nonzero timed `read`/`read_char` requests are rejected |
| V5+ colours | Flags 1 bit 0 set only in mIRC format | Standard colours and true-colour requests map to mIRC presentation |
| Pictures / character graphics | Request bit cleared where interpreter-owned | Not provided |
| Mouse | Flags 2 request bit cleared | Not provided |
| Undo | Flags 2 request bit preserved | One-level `save_undo`/`restore_undo` implemented |
| Sound beyond bleep | Flags 2 request bit cleared; V6+ Flags 1 sound bit clear | Sampled/background sound not provided |
| Menus | V6+ Flags 2 request bit cleared | Not provided |
| Transparency | Header-extension Flags 3 cleared | V6-only feature not provided |

The header reports an 80-column virtual text surface with infinite vertical scrollback (`255` lines). V5-model font units are one character cell. Default colours are black background and white foreground. When header-extension words 5 and 6 exist, their true-colour defaults are reset to `$7FFF` foreground and `$0000` background respectively.

## Why the formal revision remains 0.0

The Standard says a revision number should be advertised only when the interpreter obeys that revision perfectly, as far as is known. `tclzmachine` deliberately chooses a host-neutral text model instead of emulating a complete terminal. Several intentional differences therefore remain even though story execution is useful and broad:

1. **V1/V2 status line.** The Standard describes an interpreter-rendered status line for Versions 1 through 3. V3 can advertise that the status line is unavailable; V1/V2 have no equivalent capability bit. `tclzmachine` does not inject a status line into canonical IRC/Tcl narrative output.
2. **V4/V5/V7/V8 screen geometry.** These versions define lower and upper windows with independent cursor behavior. The runtime tracks enough window state to prevent upper-window/status text from leaking into narrative output, but it deliberately does not emulate a pixel/cell terminal.
3. **Fixed-pitch rendering.** A story may request fixed pitch through Flags 2 or text style. The VM accepts the semantics, but a Tcl/IRC extension cannot guarantee the font chosen by the user's client.
4. **Audio presentation.** Sampled/background sound is intentionally unavailable and not advertised. Bleep requests are consumed safely rather than introducing a terminal/audio dependency into the VM core.

These are scope decisions, not missing opcode implementations. Claiming Standard 1.0 or 1.1 in the header would therefore overstate what the text-only runtime promises.

## Historical story compatibility exceptions

The compatibility layer also contains a small number of deliberate exceptions for shipped historical story files. These are kept narrow and regression-tested rather than weakening general validation.

- **Null object tree queries.** Section 12 defines object number 0 as "nothing" and says there is formally no such object. Old Inform libraries nevertheless contained known bugs which could issue object opcodes with a zero object reference; Inform patch L60701 documents these "zero errors" through Library 6/7. For compatibility, read-only `get_parent`, `get_sibling`, and `get_child` queries of object 0 produce the ordinary null result 0. The branching sibling/child forms therefore take their false branch. Mutating object operations remain strict.
- **Null property-address probes.** Old Inform code can issue `get_prop_addr` with object 0 or property 0. Since the opcode's normal "property absent" result is 0, `tclzmachine` returns 0 for those probes without relaxing `get_prop`, `put_prop`, attribute operations, or property iteration generally.
- **`get_prop_len 0`.** This is not merely a project compatibility choice: the Standard explicitly requires `get_prop_len 0` to return 0 because Infocom games and files produced by old Inform versions depend on it.
- **Galatea zero-operand `read_char`.** The released `Galatea.z8` contains a historical zero-operand `read_char`. The decoder normalizes that malformed encoding to the Standard-equivalent default keyboard-device form `read_char 1` without consuming an extra byte, preserving the following store-variable address. Normal one-to-three-operand validation, device validation, and the project's no-timed-input policy remain in force for other encodings.
- **Wishbringer `show_status`.** The Standard itself recommends treating later-version `show_status` as a no-op because a released V5 Wishbringer contains the opcode accidentally. The preflight therefore rejects it before V3 but consumes V3 and later as the documented compatibility behavior.

These exceptions improve compatibility with historical files but do not change the formal-revision decision above. Undefined behavior elsewhere remains an error unless the Standard or a specific shipped-story compatibility case justifies a narrower rule.

## Release wording

For the 1.0 release, accurate wording is:

> Supports Z-machine story versions 1-5, 7, and 8 in a text-only Tcl/IRC runtime, with complete named-opcode coverage for those versions and conservative capability advertisement. Version 6 is intentionally unsupported.

Avoid wording such as "Z-machine Standard 1.0 compliant" or "Standard 1.1 compliant" unless the intentional presentation differences above are later removed or the project adopts a separate fully conforming terminal frontend.

## Release-audit rule

Before changing bytes `$32/$33` from `0.0`, repeat the conformance audit against the official Standard and require all of the following:

- no known semantic deviation for any accepted story version;
- all interpreter-owned and restart-owned header fields correct;
- all advertised optional capabilities actually available;
- valid screen/window behavior for every accepted version;
- full regression coverage of any newly claimed Standard feature;
- successful deterministic V3/V5 fixture rebuild and full CI suite.
