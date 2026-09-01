# Story catalog compatibility checkpoints

This document records reproducible manual compatibility checkpoints against real Z-machine story files. It complements the deterministic repository tests and `CONFORMANCE.md`; it is not a substitute for either one.

## 2026-09-01 pre-1.0 cleanup requalification

Branch: `Frobnost`

Tested cleanup commit:

```text
03b5277ea5979530496225e1f1ceeff7b08c46df
```

After the source-documentation/release-cleanup sweep removed the obsolete duplicate cooperative run loop, the complete deterministic test suite passed locally and the filtered supported-story catalog under `/home/daniel/z/` completed successfully again.

Verified supported-story result:

```text
PASS=323
FAIL=0
SKIP=0
TOTAL=323
```

This requalification is important because it demonstrates that deleting the dead legacy run path did not change the behavior exercised by the supported real-story corpus.

## 2026-09-01 compatibility-hardening checkpoint

Branch: `Frobnost`

Original catalog-qualified runtime commit:

```text
8ead25b86d6901be2eb2160a6d55fea350bf592b
```

Probe command behavior:

- loads each candidate story through the public Tcl extension;
- runs until the initial input request;
- supplies the command `look`;
- runs again until the next cooperative stop;
- reports VM errors with state and a byte window around the failing PC.

Verified supported-story result:

```text
PASS=323
FAIL=0
SKIP=0
TOTAL=323
```

This means every story in the filtered supported-version corpus successfully completed the probe. It does **not** mean every game has been played to completion, that every code path in every story has executed, or that the interpreter claims formal Z-machine Standard 1.0/1.1 conformance.

## Mixed-directory result immediately preceding the original checkpoint

The broader local directory contained 327 files. After the compatibility fixes in that hardening cycle, its only non-passing entries were four correct skips:

- `advent.z6` — Version 6, intentionally outside the text-only runtime scope;
- `moments.z6` — Version 6, intentionally outside the text-only runtime scope;
- `chkn-ge.dat` — not a Z-machine story; first header byte was 99;
- `lunatix1.dat` — not a Z-machine story; first header byte was 51.

The corresponding mixed-directory result was therefore effectively 323 supported stories passing, with no supported-story failure.

## Compatibility cases exercised by this corpus

The catalog work directly exposed and regression-tested several historical compatibility cases:

- the released `Galatea.z8` zero-operand `read_char` encoding;
- old-Inform null-object tree and property probes used by stories such as `SoFar.z8`;
- legacy timed `read` / `read_char` forms when timed input is correctly unadvertised;
- legacy Version-5 ZSCII TAB output;
- 16-bit wrapping of direct array addresses used by `loadw`, `loadb`, `storew`, and `storeb`;
- `get_prop` on properties longer than two bytes, where the Standard declares the result unspecified and old Inform output can depend on a tolerant interpreter.

Each compatibility behavior is intentionally narrower than a general relaxation of VM validation. Mutating object/property operations and other defined error cases remain strict unless the Standard or a concrete shipped-story case justifies otherwise.

## Reproducing the probe

Build the extension with tests enabled, run the deterministic suite, then point the probe at a local story directory:

```bash
cmake -S . -B build -DTCLZMACHINE_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

tclsh tests/probe_catalog.tcl \
  ./build/tclzmachine.so \
  /path/to/story/catalog
```

For release qualification, preserve the exact Git commit, the probe summary, and the composition of the tested catalog. Do not commit third-party commercial story files to this repository.

## Interpretation for the 1.0 line

Accurate release language is:

> Supports Z-machine story versions 1-5, 7, and 8 in a text-only Tcl/IRC runtime, with complete named-opcode coverage for those versions and conservative capability advertisement. Version 6 is intentionally unsupported.

The 323/323 results are strong real-story compatibility evidence for that statement. They do not change the deliberate decision to leave the formal Standards revision bytes at `0.0` while the text/presentation model remains intentionally reduced.

The final 1.0.0 tag should be created only after the exact release-metadata commit has both a green full CI run and another successful supported-story catalog probe.
