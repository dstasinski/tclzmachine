# Source Documentation and Release-Cleanup Audit

This document records the pre-1.0 source documentation sweep for the
`tclzmachine` C runtime on branch `Frobnost`.

The purpose of the sweep was not to add comments mechanically. It was to verify
that each implementation/header file explains its architectural responsibility,
that exposed interfaces describe the semantics which are easy to misuse, and
that transitional implementation scaffolding is not being preserved merely
because it once made refactoring safer.

## Scope

The audit covered all project C implementation and header files currently built
or exposed by the repository:

- 26 `.c` files: `src/tcl_extension.c` plus the 25 files in
  `TCLZMACHINE_VM_SOURCES`.
- 15 `.h` files under `include/`.

Tests, generated build products, Inform story sources/binaries, and third-party
headers are outside this documentation sweep.

## Result

The source tree already had a strong documentation baseline before this sweep.
The execution, persistence, input, object, text, stream, presentation, version,
and state modules all have architectural file headers rather than placeholder
comments. Public/internal headers document the important contracts around:

- raw instruction decoding versus operand evaluation;
- variable 0 stack-pop/indirect-variable semantics;
- routine-frame and evaluation-stack ownership;
- version-dependent object/property layouts;
- line input, preloaded V5+ input, dictionary tokenization, and exact ZSCII keys;
- shared opcode preflight before observable operand side effects;
- Quetzal and undo state ownership;
- external replay/transcript/recording streams;
- canonical plain UTF-8 output versus optional mIRC presentation;
- stream 3 raw ZSCII behavior;
- supported Versions 1-5, 7, and 8 and intentional Version 6 exclusion.

No broad comment-only churn was needed. Files whose existing comments already
accurately described their boundaries were left unchanged.

## Release cleanup performed

The sweep found one substantive piece of transitional code: `src/zmachine.c`
still contained the old cooperative execution loop and private copies of the
opcode helpers it used. CMake renamed that obsolete entry point to
`zmachine_run_legacy`, while `src/zmachine_run.c` had long since become the
actual public run engine.

That duplicate path was removed in commit:

`af5a60c6528c93be8c3df880730d0ac9b7f3779e` — **Remove legacy duplicate run loop**

After removal, `src/zmachine.c` owns only session/story-image lifetime,
packed-address helpers, host input queuing, and canonical output buffering. The
single authoritative cooperative execution loop lives in `src/zmachine_run.c`.

The cleanup was then completed by removing the obsolete CMake
`zmachine_run=zmachine_run_legacy` compile definition and updating the
`zmachine_run.c` architecture header so it no longer describes the deleted
fallback loop.

This matters for correctness as well as maintainability: there is now no second
copy of `read`, `restart`, `verify`, `random`, `scan_table`, or
`check_arg_count` run-loop behavior which could drift away from shared preflight
and operand-side-effect rules.

## Validation

The first dead-loop removal was independently validated by the complete CI test
suite before the remaining build/comment cleanup was performed. All 33 tests
passed, including run-loop, preflight-authority, input, stream/file, Quetzal,
presentation, and repository-story integration tests. The deterministic V3 and
V5 fixtures remained current.

A final CI run is required after the complete documentation-cleanup checkpoint;
the final release checkpoint should record that run and exact HEAD.

## Compatibility evidence

The supported-story catalog result remains recorded separately in
`CATALOG_COMPATIBILITY.md`:

`PASS=323 FAIL=0 SKIP=0 TOTAL=323`

That catalog result belongs to its exact recorded runtime commit. This source
cleanup does not retroactively relabel the catalog result as having been run on
a later documentation/release-cleanup HEAD. The probe should be rerun whenever
a new catalog-qualified release candidate is designated.

## Items intentionally not changed here

### Package version

The project currently identifies itself as `0.2.0` in CMake,
`TCLZMACHINE_VERSION`, Tcl package metadata/install paths, and related packaging
locations. Those values should be changed together in a dedicated release
versioning checkpoint. This audit does not partially bump them to 1.0.

### Formal Standards revision

Header bytes `$32/$33` intentionally remain `0.0`. The text-only architecture
has known presentation/conformance boundaries documented in `CONFORMANCE.md`
and `PRESENTATION_AUDIT.md`; source cleanup is not a reason to advertise formal
Z-machine Standard 1.0 or 1.1 conformance.

### Virtual window model

The presentation audit intentionally did not add a partial cursor/window model.
A correct stateful `set_cursor`/`get_cursor` implementation must also account for
text-driven cursor movement, split geometry, erase behavior, and historical
version rules. A last-explicit-position cache would make queryable state less
correct, not more.

## 1.0 documentation status

At the end of this sweep, the C/H source tree has an explicit architecture and
ownership story, the public/internal headers document the semantics most likely
to cause embedding or VM errors, and the major transitional duplicate run loop
has been removed rather than documented as permanent debt.

Remaining 1.0 work should therefore be treated as release work rather than a
reason for wholesale source-comment rewriting: final version/package metadata,
release notes/README wording, a fresh release-candidate catalog probe, full CI,
and exact release tagging.