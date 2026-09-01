# Text/presentation semantic audit

This document audits presentation-oriented opcodes for the supported text-only Z-machine versions: V1-V5, V7, and V8. Version 6 remains intentionally unsupported.

Primary references:

- [Z-machine Standard 1.1 section 8: screen model](https://inform-fiction.org/zmachine/standards/z1point1/sect08.html)
- [Z-machine Standard 1.1 section 15: opcode dictionary](https://inform-fiction.org/zmachine/standards/z1point1/sect15.html)
- [`CONFORMANCE.md`](CONFORMANCE.md)
- [`OPCODE_STATUS.md`](OPCODE_STATUS.md)

## Audit rule

A presentation opcode is implemented directly when its semantics are observable without requiring a terminal. Purely visual effects may be reduced or neutralized, but queryable/store/branch results and non-visual state must remain truthful.

A partial virtual-screen implementation is deliberately avoided. For example, remembering only the last explicit `set_cursor` position would make `get_cursor` incorrect as soon as text output moved the cursor. Cursor tracking should therefore be added only together with coherent window geometry and output-driven cursor movement.

## Current supported-version behavior

| Opcode / feature | Current behavior | Audit classification |
| --- | --- | --- |
| V3 `show_status` | Presentation-neutral no-op; later-version Wishbringer compatibility no-op retained | COMPAT / intentional |
| `split_window` | Consumed without creating a visible split | TEXT / intentional reduction |
| `set_window` | Tracks window 0/1 selection; nonzero-window output is suppressed from canonical narrative output | TEXT / meaningful state retained |
| `erase_window` | Visual clearing is neutral; `-1` selects window 0 as required | TEXT / partial state retained |
| `erase_line` | Presentation-only no-op in supported non-V6 versions | TEXT / intentional reduction |
| `set_cursor` | Presentation-only no-op | TEXT / formal conformance boundary |
| `get_cursor` | Writes deterministic virtual row 1, column 1 after validating the complete destination range | TEXT / formal conformance boundary |
| `set_text_style` | Canonical plain output remains neutral; optional mIRC output tracks/renderable style state | TEXT / host-mapped |
| `buffer_mode` | Consumed; canonical line-oriented output does not expose a terminal buffering model | TEXT / formal conformance boundary |
| `set_font` | Font 1 is the only available font; query/select result semantics are implemented | TEXT / truthful capability reduction |
| `set_colour` | Plain output neutral; optional mIRC renderer maps supported standard colours | TEXT / host-mapped |
| `set_true_colour` | Plain output neutral; optional mIRC renderer approximates true colour to its palette | TEXT / host-mapped |
| `print_table` | Rectangular screen output becomes canonical line-oriented rows | TEXT / intentional reduction |
| `sound_effect` | Sound capability remains unadvertised; valid requests are consumed without audio/callback presentation | COMPAT / intentional |

## Specific findings

### Window selection is worth retaining

The current `set_window` state is meaningful even without a screen. Upper-window/status text is intentionally excluded from Tcl/IRC narrative output, so losing the selected-window state would leak presentation text into gameplay replies.

`erase_window -1` likewise must select window 0. Without that state transition, a story that clears/unsplits the screen after writing status text could leave all later narrative output suppressed.

### Cursor state should not be half-implemented

The Standard makes cursor position queryable through `get_cursor`. Correct V4/V5 behavior depends on several coupled operations:

- `split_window` changes upper-window geometry;
- `set_cursor` explicitly positions the upper-window cursor and has no effect in the lower window;
- printing characters/newlines moves the active cursor;
- `erase_window` changes cursor positions in version-dependent ways;
- `get_cursor` must flush buffered output and report the resulting active-window cursor.

Tracking only explicit `set_cursor` calls would therefore be misleading. A future virtual-window implementation should introduce geometry and cursor movement as one coherent unit rather than incrementally advertising state that becomes stale after output.

### V4/V5 lax cursor compatibility must be considered together

The Standard notes that many games, including standard Inform menu code and Infocom's `Sherlock`, have positioned the cursor below the current split. It recommends an implicit `split_window` in V4/V5 when possible. Any future cursor model should include that historical tolerance from its first implementation.

### `buffer_mode` is distinct from Tcl output wrapping

Z-machine buffering controls terminal word breaking for streams 1/2 and, in V3-V5, applies only to the lower window. Tcl's host-facing `-wordwrap` option is an output-delivery policy and should not be silently treated as the same VM feature. A future implementation should keep those two concepts separate.

### `set_font` remains intentionally conservative

The runtime can truthfully report normal font 1. It cannot guarantee a fixed-pitch or alternate font inside the user's Tcl/IRC client, so unavailable font requests correctly return 0 rather than pretending the presentation request succeeded.

## Header/capability consistency

The interpreter remains conservative:

- V3 upper-window support is not advertised through Flags 1 bit 5;
- fixed-pitch capability is not advertised;
- timed input remains unadvertised even though legacy timed forms can fall back to untimed input;
- colour/style capability is advertised only by host presentation modes that can actually render it;
- pictures, mouse, menus, sampled sound, and V6 graphics/window facilities are not advertised.

This is preferable to claiming a capability merely because the corresponding opcode can be consumed safely.

## No runtime change from this audit checkpoint

The audit did **not** introduce a partial cursor/window model. The existing behavior is internally safer than adding state that would become incorrect after ordinary text output. This checkpoint therefore records the boundary explicitly rather than increasing apparent conformance without complete semantics.

The next presentation implementation milestone, if pursued, should be a coherent two-window virtual model containing at least:

1. upper-window height and lower/upper cursor coordinates;
2. version-correct reset, restart, erase, and split behavior;
3. cursor movement caused by characters and newlines;
4. V4/V5 implicit split tolerance for historically lax `set_cursor` calls;
5. `get_cursor` reporting from that live state;
6. lower-window buffering state separate from host-side IRC/Tcl word wrapping.

Until that unit is implemented and regression-tested, the formal Standards revision bytes should remain `0.0`.