# Z-machine opcode implementation status

This file is the release-audit inventory for the Z-machine instruction set implemented by `tclzmachine`.

Primary specification: [Z-machine Standard 1.1, section 14](https://inform-fiction.org/zmachine/standards/z1point1/sect14.html) and [section 15](https://inform-fiction.org/zmachine/standards/z1point1/sect15.html).

`tclzmachine` intentionally supports story versions **1, 2, 3, 4, 5, 7, and 8**. Version 6 story files are rejected because the V6 graphics/window model is outside this project's text-only scope. Per the Standard, V7 and V8 use the V5 instruction/screen model except for their documented memory/address changes.

## Status legend

| Status | Meaning |
| --- | --- |
| **FULL** | VM semantics are implemented directly. |
| **HOST** | Semantics are implemented, with required filename/input/stream policy delegated to Tcl. |
| **TEXT** | Implemented for the text-only runtime; visual-only behavior is reduced, neutralized, or mapped to optional mIRC presentation. |
| **COMPAT** | Deliberate compatibility behavior for historical/Standard-documented exceptions. |
| **V6-ONLY** | Intentionally unavailable because V6 stories are unsupported. |
| **IGNORED** | The Standard requires the opcode range to be ignored for the supported version. |

The audit currently finds **no missing named opcode in the supported V1-V5/V7/V8 instruction set**. Unsupported behavior is limited to capabilities the interpreter deliberately does not advertise, plus Version-6-only facilities.

## 2OP opcodes

| Opcode | Name | Version | Status | Implementation / note |
| --- | --- | --- | --- | --- |
| 2OP:1 | `je` | V1+ | FULL | Core branch executor; 2-4 operands. |
| 2OP:2 | `jl` | V1+ | FULL | Signed comparison. |
| 2OP:3 | `jg` | V1+ | FULL | Signed comparison. |
| 2OP:4 | `dec_chk` | V1+ | FULL | Indirect variable semantics preserved. |
| 2OP:5 | `inc_chk` | V1+ | FULL | Indirect variable semantics preserved. |
| 2OP:6 | `jin` | V1+ | FULL | Object subsystem. |
| 2OP:7 | `test` | V1+ | FULL | Core branch executor. |
| 2OP:8 | `or` | V1+ | FULL | Core arithmetic/bitwise executor. |
| 2OP:9 | `and` | V1+ | FULL | Core arithmetic/bitwise executor. |
| 2OP:10 | `test_attr` | V1+ | FULL | Object subsystem. |
| 2OP:11 | `set_attr` | V1+ | FULL | Object subsystem. |
| 2OP:12 | `clear_attr` | V1+ | FULL | Object subsystem. |
| 2OP:13 | `store` | V1+ | FULL | Indirect target-variable semantics. |
| 2OP:14 | `insert_obj` | V1+ | FULL | Object subsystem. |
| 2OP:15 | `loadw` | V1+ | FULL | Bounds-checked story-memory read. |
| 2OP:16 | `loadb` | V1+ | FULL | Bounds-checked story-memory read. |
| 2OP:17 | `get_prop` | V1+ | FULL | Property subsystem. |
| 2OP:18 | `get_prop_addr` | V1+ | FULL | Property subsystem. |
| 2OP:19 | `get_next_prop` | V1+ | FULL | Property subsystem. |
| 2OP:20 | `add` | V1+ | FULL | 16-bit wrap semantics. |
| 2OP:21 | `sub` | V1+ | FULL | 16-bit wrap semantics. |
| 2OP:22 | `mul` | V1+ | FULL | Signed 16-bit semantics. |
| 2OP:23 | `div` | V1+ | FULL | Signed division with deterministic divide-by-zero error. |
| 2OP:24 | `mod` | V1+ | FULL | Signed remainder. |
| 2OP:25 | `call_2s` | V4+ | FULL | Common routine-call machinery. |
| 2OP:26 | `call_2n` | V5/V7/V8 | FULL | Discard-result routine call. |
| 2OP:27 | `set_colour` | V5/V7/V8 | TEXT | Plain output is neutral; optional mIRC output maps standard colours. |
| 2OP:28 | `throw` | V5/V7/V8 | FULL | Frame-cookie unwind/return semantics. |

**Undefined 2OP slots:** opcode 0 and opcodes 29-31 are illegal in every supported version and are rejected by the authoritative preflight before operand evaluation.

## 1OP opcodes

| Opcode | Name | Version | Status | Implementation / note |
| --- | --- | --- | --- | --- |
| 1OP:0 | `jz` | V1+ | FULL | Core branch executor. |
| 1OP:1 | `get_sibling` | V1+ | FULL | Object subsystem. |
| 1OP:2 | `get_child` | V1+ | FULL | Object subsystem; literal object 0 has a legacy-compatible zero/false path. |
| 1OP:3 | `get_parent` | V1+ | FULL | Object subsystem. |
| 1OP:4 | `get_prop_len` | V1+ | FULL | Version-correct property-length decoding. |
| 1OP:5 | `inc` | V1+ | FULL | Indirect variable semantics. |
| 1OP:6 | `dec` | V1+ | FULL | Indirect variable semantics. |
| 1OP:7 | `print_addr` | V1+ | FULL | Z-text subsystem. |
| 1OP:8 | `call_1s` | V4+ | FULL | Common routine-call machinery. |
| 1OP:9 | `remove_obj` | V1+ | FULL | Object subsystem. |
| 1OP:10 | `print_obj` | V1+ | FULL | Object short-name output. |
| 1OP:11 | `ret` | V1+ | FULL | Routine-frame return. |
| 1OP:12 | `jump` | V1+ | FULL | Signed branch displacement. |
| 1OP:13 | `print_paddr` | V1+ | FULL | Version-correct packed string address. |
| 1OP:14 | `load` | V1+ | FULL | Indirect variable read. |
| 1OP:15 | `not` / `call_1n` | V1-V4 / V5,V7,V8 | FULL | Version split implemented in core executor. |

## 0OP opcodes

`0OP:14` is not a semantic instruction in V5+: byte `0xBE` is the extended-opcode prefix. Before V5 it is illegal. It is therefore not counted as a named 0OP below.

| Opcode | Name | Version | Status | Implementation / note |
| --- | --- | --- | --- | --- |
| 0OP:0 | `rtrue` | V1+ | FULL | Routine return true. |
| 0OP:1 | `rfalse` | V1+ | FULL | Routine return false. |
| 0OP:2 | `print` | V1+ | FULL | Inline Z-text. |
| 0OP:3 | `print_ret` | V1+ | FULL | Inline Z-text, newline, return true. |
| 0OP:4 | `nop` | V1+ | FULL | Advances with no effect. |
| 0OP:5 | `save` | V1-V4 | HOST | Cooperative filename request; V1-V3 branch and V4 store forms implemented. |
| 0OP:6 | `restore` | V1-V4 | HOST | Cooperative Quetzal restore; version-correct continuation semantics. |
| 0OP:7 | `restart` | V1+ | FULL | Restores original dynamic memory and preserves only required Flags 2 bits. |
| 0OP:8 | `ret_popped` | V1+ | FULL | Stack pop and routine return. |
| 0OP:9 | `pop` / `catch` | V1-V4 / V5,V7,V8 | FULL | Version split implemented; catch uses routine-frame-count cookie. |
| 0OP:10 | `quit` | V1+ | FULL | Halts VM cleanly. |
| 0OP:11 | `new_line` | V1+ | FULL | Canonical text newline. |
| 0OP:12 | `show_status` | V3 | COMPAT | Text-only no-op; V4+ compatibility no-op retained for the known V5 Wishbringer anomaly. |
| 0OP:13 | `verify` | V3+ | FULL | Checksum uses immutable originally-loaded story bytes. |
| 0OP:15 | `piracy` | V5/V7/V8 | FULL | Always branches as genuine. |

## VAR opcodes

| Opcode | Name | Version | Status | Implementation / note |
| --- | --- | --- | --- | --- |
| VAR:0 | `call` / `call_vs` | V1+ / V4+ name | FULL | Common routine-call machinery. |
| VAR:1 | `storew` | V1+ | FULL | Dynamic-memory checked word write. |
| VAR:2 | `storeb` | V1+ | FULL | Dynamic-memory checked byte write. |
| VAR:3 | `put_prop` | V1+ | FULL | Property subsystem. |
| VAR:4 | `sread` / `aread` | V1+ | HOST | Cooperative Tcl input, tokenization, preloaded V5 text, terminating keys; timed input intentionally unavailable. |
| VAR:5 | `print_char` | V1+ | FULL | ZSCII output. |
| VAR:6 | `print_num` | V1+ | FULL | Signed decimal output. |
| VAR:7 | `random` | V1+ | FULL | Session PRNG and standard seed contract. |
| VAR:8 | `push` | V1+ | FULL | Evaluation stack. |
| VAR:9 | `pull` | V1-V5/V7/V8 | FULL | V5-model indirect variable target. V6 user-stack form is outside scope. |
| VAR:10 | `split_window` | V3+ | TEXT | Consumed; no visual split exists in the Tcl/IRC surface. |
| VAR:11 | `set_window` | V3+ | TEXT | Tracks lower/upper window selection so upper-window text is suppressed. |
| VAR:12 | `call_vs2` | V4+ | FULL | Up to seven call arguments. |
| VAR:13 | `erase_window` | V4+ | TEXT | Reduced screen state; `-1` correctly selects window 0. |
| VAR:14 | `erase_line` | V4+ | TEXT | Presentation-only no-op in supported non-V6 model. |
| VAR:15 | `set_cursor` | V4+ | TEXT | Presentation-only no-op in supported non-V6 model. |
| VAR:16 | `get_cursor` | V4+ | TEXT | Writes deterministic virtual row 1, column 1. |
| VAR:17 | `set_text_style` | V4+ | TEXT | Plain output neutral; optional mIRC bold/italic/reverse presentation. |
| VAR:18 | `buffer_mode` | V4+ | TEXT | No-op on the line-oriented host surface. |
| VAR:19 | `output_stream` | V3+ | HOST | Streams 1-4, nested stream 3, transcript and command-record files implemented. |
| VAR:20 | `input_stream` | V3+ | HOST | Keyboard/command-file replay implemented. |
| VAR:21 | `sound_effect` | V5, historical V3 compatibility | COMPAT | Sound capability is not advertised; valid forms are consumed without starting sound/callbacks. |
| VAR:22 | `read_char` | V4+ | HOST | Cooperative character/ZSCII-key input; timed input intentionally unavailable. |
| VAR:23 | `scan_table` | V4+ | FULL | Word/byte forms and optional field-size form. |
| VAR:24 | `not` | V5/V7/V8 | FULL | 16-bit complement. |
| VAR:25 | `call_vn` | V5/V7/V8 | FULL | Discard-result routine call. |
| VAR:26 | `call_vn2` | V5/V7/V8 | FULL | Up to seven arguments. |
| VAR:27 | `tokenise` | V5/V7/V8 | FULL | Dictionary selection and preserve-unrecognized flag implemented. |
| VAR:28 | `encode_text` | V5/V7/V8 | FULL | Version-correct dictionary encoding. |
| VAR:29 | `copy_table` | V5/V7/V8 | FULL | Positive overlap-safe, negative forward-copy, zero-destination clear. |
| VAR:30 | `print_table` | V5/V7/V8 | TEXT | Renders table rows as canonical line-oriented text. |
| VAR:31 | `check_arg_count` | V5/V7/V8 | FULL | Uses frame argument mask. |

## EXT opcodes

| Opcode | Name | Version | Status | Implementation / note |
| --- | --- | --- | --- | --- |
| EXT:0 | `save` | V5/V7/V8 | HOST | Zero operands = full Quetzal save; 3/4 operands = auxiliary byte-region save with suggested name/prompt metadata. |
| EXT:1 | `restore` | V5/V7/V8 | HOST | Full Quetzal or auxiliary byte-region restore. |
| EXT:2 | `log_shift` | V5/V7/V8 | FULL | Deterministic 16-bit logical shifts. |
| EXT:3 | `art_shift` | V5/V7/V8 | FULL | Deterministic 16-bit arithmetic shifts. |
| EXT:4 | `set_font` | V5/V7/V8 | TEXT | Only normal font 1 is advertised/available; result semantics implemented. |
| EXT:5 | `draw_picture` | V6 | V6-ONLY | V6 intentionally unsupported. |
| EXT:6 | `picture_data` | V6 | V6-ONLY | V6 intentionally unsupported. |
| EXT:7 | `erase_picture` | V6 | V6-ONLY | V6 intentionally unsupported. |
| EXT:8 | `set_margins` | V6 | V6-ONLY | V6 intentionally unsupported. |
| EXT:9 | `save_undo` | V5/V7/V8 | FULL | One-level in-memory undo cache. |
| EXT:10 | `restore_undo` | V5/V7/V8 | FULL | Restores cached VM state and resumes save point. |
| EXT:11 | `print_unicode` | V5/V7/V8, Standard 1.0 | TEXT | Genuine UTF-8 output. Formal Standards revision remains intentionally unadvertised during hardening. |
| EXT:12 | `check_unicode` | V5/V7/V8, Standard 1.0 | TEXT | Reports actual text-path Unicode print/input capability. |
| EXT:13 | `set_true_colour` | V5/V7/V8, Standard 1.1 | TEXT | Plain output neutral; optional mIRC mode approximates to classic IRC palette. |
| EXT:16 | `move_window` | V6 | V6-ONLY | V6 intentionally unsupported. |
| EXT:17 | `window_size` | V6 | V6-ONLY | V6 intentionally unsupported. |
| EXT:18 | `window_style` | V6 | V6-ONLY | V6 intentionally unsupported. |
| EXT:19 | `get_wind_prop` | V6 | V6-ONLY | V6 intentionally unsupported. |
| EXT:20 | `scroll_window` | V6 | V6-ONLY | V6 intentionally unsupported. |
| EXT:21 | `pop_stack` | V6 | V6-ONLY | V6 intentionally unsupported. |
| EXT:22 | `read_mouse` | V6 | V6-ONLY | V6 intentionally unsupported. |
| EXT:23 | `mouse_window` | V6 | V6-ONLY | V6 intentionally unsupported. |
| EXT:24 | `push_stack` | V6 | V6-ONLY | V6 intentionally unsupported. |
| EXT:25 | `put_wind_prop` | V6 | V6-ONLY | V6 intentionally unsupported. |
| EXT:26 | `print_form` | V6 | V6-ONLY | V6 intentionally unsupported. |
| EXT:27 | `make_menu` | V6 | V6-ONLY | V6 intentionally unsupported. |
| EXT:28 | `picture_table` | V6 | V6-ONLY | V6 intentionally unsupported. |
| EXT:29 | `buffer_screen` | V6 | IGNORED | In supported V5/V7/V8 it is outside the defined EXT range and is ignored without evaluating operands, as required for EXT:29-255. |

EXT:14 and EXT:15 are reserved in Standard 1.1. EXT:30-255 are outside the defined Standard-1.1 set and are ignored for supported V5/V7/V8 stories as required by section 14.2.1. V6-only EXT:5-8 and EXT:16-28 are rejected rather than silently consumed when encountered in V5/V7/V8.

## Audit summary

Across the 119 named/semantic entries represented by the Standard-1.1 table (excluding the non-semantic `0xBE` EXT prefix):

- **77 FULL**
- **8 HOST**
- **14 TEXT**
- **2 COMPAT**
- **17 V6-ONLY**
- **1 IGNORED** (`EXT:29` in the supported non-V6 versions)
- **0 missing named supported opcodes**

The next release-hardening work is therefore not broad opcode implementation. It is conformance depth: edge cases, header capability truthfulness, unsupported-feature behavior, Standards revision advertising, and story-catalog coverage.

## Known capability limits relevant to opcode conformance

- Timed `read`/`read_char` is not implemented and is not advertised.
- Sampled sound is not implemented and is not advertised; `sound_effect` is consumed safely.
- Pictures, mouse, menus, and the V6 window model are not implemented; V6 stories are rejected.
- Fixed-pitch/font-3 presentation is not advertised.
- Plain Tcl output does not advertise colour/bold/italic. Optional `mirc` format advertises the presentation capabilities it can actually render.
- The header Standards revision bytes remain **0.0 intentionally** until the final conformance audit. Individual Standard 1.0/1.1 opcodes are implemented, but the project does not yet claim complete formal revision compliance.
