# Task 5 Report — apply_symbol_config pipeline binding + end-to-end test

NOTE (controller-authored): the implementer subagent was cancelled by the operator mid-verification
(while waiting on the full `-L atx_vol` gate) and never wrote this report. This report is the
controller's factual reconstruction from the implementer's interim messages plus the controller's
own verification run. Treat unattributed claims below as controller-verified.

## What was implemented (commit 8d99c49)

- `apply_symbol_config(const SymbolFitConfig&, SessionInputs&)` — free function in
  `atx-vol/include/atx/vol/surface_db.hpp` / `atx-vol/src/surface_db.cpp`. Order per brief:
  `apply_fit_preset(in, cfg.preset)` first, then `pin_curve` → `in.curve` + `in.calib`,
  then `al_override` → `in.deam.al_opts`, then the six scalars/flags unconditionally.
  Market-snapshot fields (S, r, expiry rates, cash_divs, now_ts_ns) untouched.
- `symbol_config_from_preset(FitPreset)` — captures `apply_fit_preset`'s effective policy into
  a `SymbolFitConfig` (pin_curve=false identity starting point).
- Tests appended to `atx-vol/tests/surface_db_test.cpp`:
  `SurfaceDbApply.PinnedConfig_OverridesPreset`, `SurfaceDbApply.UnpinnedConfig_PresetCurveStands`,
  `SurfaceDbEndToEnd.ConfigureStoreReloadServe` (two-session create/configure/store → reopen/serve
  → external-writer refresh flow, ConvexDense surface through the partition path).

## Test evidence

- TDD RED: implementer reported the expected failure (missing `apply_symbol_config` /
  `symbol_config_from_preset` symbols) before implementing. RED output lives in the implementer's
  transcript; not re-extracted here.
- GREEN (controller-run, post-cancellation, before commit):
  `ctest --test-dir build -R "SurfaceDb|SurfaceArchive" --output-on-failure`
  → **100% tests passed, 0 tests failed out of 33** (includes all 3 new Task 5 tests, all prior
  SurfaceDb*/SurfaceDbManifest*/SurfaceDbPartition* tests, and the SurfaceArchive regression suite).
  Total time 2.26 s.

## Deviations / process notes

- **Full-module gate (`-L atx_vol`) deliberately skipped per operator instruction** — the operator
  killed the in-flight gate run and will run the full suite themselves later. Brief Step 4's
  whole-module sanity run is therefore DEFERRED TO THE OPERATOR, not evidence-backed here.
- Implementer's baseline full-gate run (pre-task) was in flight when cancelled; no pass-count
  comparison exists.
- Files changed: exactly the three task files (+155 lines, no deletions elsewhere).

## Self-review

Not performed by the implementer (cancelled). Covered instead by the task review gate.

## Final-review fix report

Fixes for the three final whole-branch review findings on the surface_db feature.

### Finding 1 — mutation path could permanently brick the database

- `atx-vol/src/surface_db.cpp:116-198` (`write_db_manifest`, symbol-encode loop, around
  former line 139/187): after `encode_symbol_record`, added a call to
  `symbol_record_enums_valid(p.rec)` — the same wire-range check `DbManifest::open` already
  ran on read — and return `Err(ErrorCode::InvalidArgument, ...)` when it fails. Closes the
  writer-side half of the asymmetry: an out-of-range enum (e.g.
  `static_cast<FitPreset>(250)`) is now rejected before a record is ever assembled into a
  manifest buffer.
- `atx-vol/src/surface_db.cpp` (`SurfaceDb::persist_locked`, former lines 753-776): reordered
  to serialize → `DbManifest::open(*bytes)` (parse-validate a COPY of the bytes in memory,
  keeping the original for the write) → only on success, `write_manifest_file_atomic` (the
  tmp+rename) → swap `snapshot_` from the already-parsed manifest (no second parse). A
  mutation that would produce parser-rejected bytes now fails cleanly (returns the parser's
  error) with the on-disk manifest and `snapshot_` both untouched, instead of renaming bad
  bytes over `manifest.atxdb` and bricking every future `SurfaceDb::open`/`refresh`.
- New test: `atx-vol/tests/surface_db_test.cpp`, `SurfaceDb.UpsertBadEnum_FailsCleanly_DbStillOpens`
  (inserted just above `SurfaceDb.Create_RejectsExisting_Open_RejectsMissing`). Creates a db,
  upserts `SymbolFitConfig{.preset = static_cast<FitPreset>(250)}`, asserts
  `ErrorCode::InvalidArgument`, generation unchanged (still 1), `symbols()` still empty, a
  subsequent valid `upsert_symbol` succeeds (generation → 2), and `SurfaceDb::open(root)` on a
  fresh handle still opens and sees generation 2 / symbol AAPL.

### Finding 2 — thread-safety contract stronger than delivered

- `atx-vol/include/atx/vol/surface_db.hpp` (`SurfaceDb` class doc, former lines 320-324):
  reworded to state precisely what's serialized — manifest mutations (`upsert_symbol`,
  `remove_symbol`, and the manifest half of `write_partition`/`drop_partition`) are serialized
  by the internal mutex, but partition FILE operations are not fully covered
  (`write_partition` writes its archive before taking the lock; `drop_partition`'s unlink now
  happens under the lock but after the manifest rename). States explicitly that concurrent
  in-process callers must serialize same-key `write_partition`/`drop_partition` calls
  themselves, and that cross-process remains single-writer/many-reader.
- `atx-vol/src/surface_db.cpp` (`SurfaceDb::drop_partition`, former lines 931-970): moved
  `std::filesystem::remove(path, ec)` from after the lock-guard's scope-close to inside it —
  the `std::lock_guard` now spans the whole function body (manifest check → persist_locked →
  unlink), so the unlink is the last statement executed while still holding `*mu_`. Kept the
  manifest-first ordering and the existing crash-ordering comment (extended it by one sentence
  explaining the in-process-locking rationale for the move); kept the noexcept
  `std::filesystem::remove(path, ec)` overload. `write_partition`'s pre-lock archive write was
  NOT touched (plan-mandated ordering).

### Finding 3 — stale "Task N" scaffolding comments

- `atx-vol/include/atx/vol/surface_db.hpp:8-13` — removed the "(Tasks 3-5)" / "land in Task 3,
  `apply_symbol_config` in a later task" phrasing; the file-header paragraph now describes what
  the header actually contains (manifest format + `SurfaceDb` class + pipeline binding, no
  process references).
- `atx-vol/include/atx/vol/surface_db.hpp:314` (was "refresh() (Task 3); partition IO (Task 4)")
  and `:354` (was "── Partition IO (Task 4) ──") — task markers removed.
- `atx-vol/tests/surface_db_test.cpp:26-33` (file-header comment) — rewrote the false "Pure
  in-memory (no file IO — that's Task 3)" claim (the file plainly contains `SurfaceDb`
  file-IO tests) into an accurate two-part description: the in-memory `DbManifest`
  writer/parser suite, then the on-disk `SurfaceDb`/partition-store/pipeline-binding/e2e suite.
- `atx-vol/tests/surface_db_test.cpp:490` ("── SurfaceDb: partition store (Task 4) ──") and
  `:658` ("── Fitting-pipeline binding (Task 5) ──") — task markers removed.
- Swept all three files for any other "Task "/"task" occurrence (`grep -in task`). Two
  remaining hits in the test file (`surface_db_test.cpp`, "the binding bit-identity oracle for
  this task" at the partition-fixtures comment and at `expect_theo_bit_identical`) use "task"
  generically to mean "this test file's job," not a numbered sprint-task reference — left
  unchanged as out of scope for the "Task N" scaffolding sweep. `surface_db.cpp` had no
  "Task"/"task" occurrences to begin with. Did not touch any `.superpowers/` ledger files.

### Verification

Build:
```
powershell -NoProfile -ExecutionPolicy Bypass -Command "& .\scripts\atx-build.ps1 build atx-vol-tests"
```
→ clean rebuild of `surface_db.cpp` + `surface_db_test.cpp`, link succeeded, no warnings
(`/WX`-clean).

Tests:
```
ctest --test-dir build -R "SurfaceDb|SurfaceArchive" --output-on-failure
```
Tail of output:
```
33/34 Test #461: SurfaceDbPartition.BadKey_Rejected .................................................   Passed    0.10 sec
      Start 462: SurfaceDbApply.PinnedConfig_OverridesPreset
32/34 Test #462: SurfaceDbApply.PinnedConfig_OverridesPreset ........................................   Passed    0.18 sec
      Start 463: SurfaceDbApply.UnpinnedConfig_PresetCurveStands
33/34 Test #463: SurfaceDbApply.UnpinnedConfig_PresetCurveStands ....................................   Passed    0.09 sec
      Start 464: SurfaceDbEndToEnd.ConfigureStoreReloadServe
34/34 Test #464: SurfaceDbEndToEnd.ConfigureStoreReloadServe ........................................   Passed    0.16 sec

100% tests passed, 0 tests failed out of 34

Label Time Summary:
atx_vol    =   4.96 sec*proc (34 tests)

Total Test time (real) =   5.76 sec
```
34/34 passed (33 pre-existing + `SurfaceDb.UpsertBadEnum_FailsCleanly_DbStillOpens`), including
`SurfaceDb.UpsertBadEnum_FailsCleanly_DbStillOpens` (#453) and
`SurfaceDbPartition.RewriteReplaces_DropRemoves` (#459, exercises the moved-inside-lock
`drop_partition` unlink) both passing individually.

Full `-L atx_vol` gate NOT run (operator runs it separately per dispatch instructions).
