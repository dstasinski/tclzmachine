# Source Documentation and Release-Cleanup Audit

This document records the pre-1.0 source documentation sweep for the `tclzmachine` C runtime on branch `Frobnost`.

The purpose of the sweep was not to add comments mechanically. It was to verify that each implementation/header file explains its architectural responsibility, that exposed interfaces describe semantics which are easy to misuse, and that transitional implementation scaffolding is not being preserved merely because it once made refactoring safer.

## Scope

The audit covered all project C implementation and header files built or exposed by the repository at the checkpoint:

- 26 `.c` files: `src/tcl_extension.c` plus the 25 files in `TCLZMACHINE_VM_SOURCES`;
- 15 `.h` files under `include/`.

Tests, generated build products, Inform story sources/binaries, and third-party headers were outside the source-comment sweep itself.

## Result

The source tree already had a strong documentation baseline. Execution, persistence, input, object, text, stream, presentation, version, and state modules have architectural file headers rather than placeholder comments. Public/internal headers document the important contracts around:

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

No broad comment-only churn was needed. Files whose existing comments already accurately described their boundaries were left unchanged.

## Release cleanup performed

The sweep found one substantive piece of transitional code: `src/zmachine.c` still contained the old cooperative execution loop and private copies of the opcode helpers it used. CMake renamed that obsolete entry point to `zmachine_run_legacy`, while `src/zmachine_run.c` had long since become the actual public run engine.

That duplicate path was removed in commit:

`af5a60c6528c93be8c3df880730d0ac9b7f3779e` — **Remove legacy duplicate run loop**

After removal, `src/zmachine.c` owns session/story-image lifetime, packed-address helpers, host input queuing, and canonical output buffering. The single authoritative cooperative execution loop lives in `src/zmachine_run.c`.

The cleanup was completed by removing the obsolete CMake `zmachine_run=zmachine_run_legacy` compile definition and updating the `zmachine_run.c` architecture header so it no longer describes the deleted fallback loop.

This matters for correctness as well as maintainability: there is no second copy of `read`, `restart`, `verify`, `random`, `scan_table`, or `check_arg_count` run-loop behavior which can drift away from shared preflight and operand-side-effect rules.

## Validation

The final source-cleanup checkpoint was:

```text
03b5277ea5979530496225e1f1ceeff7b08c46df
```

GitHub Actions run `33564366041` passed all 33 CTest entries on that exact commit, including run-loop, preflight-authority, input, stream/file, Quetzal, presentation, and repository-story integration tests. The deterministic V3 and V5 fixtures were already current.

After pulling the cleanup checkpoint locally, the complete deterministic suite also passed and the filtered supported-story catalog under `/home/daniel/z/` completed successfully. That requalification is recorded in `CATALOG_COMPATIBILITY.md`.

## Compatibility evidence

The supported-story catalog result is:

```text
PASS=323
FAIL=0
SKIP=0
TOTAL=323
```

The catalog probe is a startup/`look` smoke qualification. It is meaningful real-story evidence but is not a full playthrough or a formal Standards conformance suite.

## Package-version handoff

At the exact source-audit commit above, the package still identified itself as `0.2.0`. That was intentional: package identity, CMake metadata, Tcl package version, install paths, README language, and release notes were reserved for the dedicated release-metadata checkpoint which follows this audit.

The 1.0 release-metadata checkpoint changes those values together to `1.0.0`; this audit remains the historical record explaining why the version bump was not mixed into the source-cleanup commit.

## Formal Standards revision

Header bytes `$32/$33` intentionally remain `0.0`. The text-only architecture has known presentation/conformance boundaries documented in `CONFORMANCE.md` and `PRESENTATION_AUDIT.md`; source cleanup and package versioning are not reasons to advertise formal Z-machine Standard 1.0 or 1.1 conformance.

## Virtual window model

The presentation audit intentionally did not add a partial cursor/window model. A correct stateful `set_cursor`/`get_cursor` implementation must also account for text-driven cursor movement, split geometry, erase behavior, and historical version rules. A last-explicit-position cache would make queryable state less correct, not more.

## 1.0 source status

The C/H source tree now has an explicit architecture and ownership story, the public/internal headers document the semantics most likely to cause embedding or VM errors, and the major transitional duplicate run loop has been removed rather than documented as permanent debt.

After the dedicated 1.0.0 metadata/documentation checkpoint, the remaining release work is qualification rather than implementation: require green full CI, rerun the supported-story catalog on the exact release-candidate commit, verify fixtures did not move the branch, and only then tag that exact commit for 1.0.0.
