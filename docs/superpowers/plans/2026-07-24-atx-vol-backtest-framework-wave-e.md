# Backtest Framework — Wave E: Performance Passes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the surviving performance passes of design spec §7 (`docs/superpowers/specs/2026-07-21-atx-vol-backtest-framework-design.md` lines 191–205) on the listed-dispersion pipeline, each with a **captured before-number, a stated target, and a measured after-number**, and each behind a **byte-stability gate** proving the economics did not move. Four of the seven passes survive triage (**P5, P2, P3, P1**); three are recommended DROPPED (**P4, P6, P7**) with reasons recorded in §Triage below. No new economics, no new strategy, no schema bump.

**The two governing rules of this wave:**

1. **A perf pass with no before/after measurement is not a perf pass.** Every task states its measurement command, the baseline captured BEFORE the change, the target, and the measured after-number. A pass whose speedup cannot be read off the existing diagnostics **must add the measurement first, as its own step** — Task 1 exists because two of the four surviving passes are currently invisible to the diagnostics.
2. **Perf must not move economics.** Every task has a byte-stability gate on the 3-session fixture; the controller gate re-proves the 135-session goldens. A pass that cannot be shown byte-preserving is not landed.

**Measurement substrate (do not invent one).** Wave A lifted `PhaseTimer` into `atx-vol/include/atx/vol/run_diagnostics.hpp:28-69` and added the `diagnostics` RunArchive section (`encode_diagnostics_section`, `:79-81`). Every dispersion subcommand already declares a phase order and charges named phases; the read-out is `runarchive dump <DIR> diagnostics --tsv` → `(subcommand, phase, wall_ms, count)` rows plus a `total` row. **That is the instrument for this entire wave.** `PhaseTimer` is not RAII: a phase is opened with `auto t = PhaseTimer::now();` and closed with `timer.add("name", t, count);`; repeated `add` calls to one name accumulate; an undeclared name is appended (so the declared initializer list at each subcommand's `PhaseTimer timer({...})` is what fixes row order).

**Tech Stack:** C++20 (MSVC, Release preset `build-rel`, AVX2), `atx::vol`; `PhaseTimer`/`encode_diagnostics_section` (Wave A), `RunDir`/`RunArchive` (Wave A), `listed_dispersion_pipeline` (Wave B), `SnapshotCache` (declared in `backtest.hpp:185-226`, impl `src/snapshot_cache.cpp`), `load_opra_daterange`/`parallel_for_dynamic`, `atx::core::hash_bytes`/`hash_combine`, `detail::crc32c`/`align_up` (`detail/archive_util.hpp`). gtest target `atx-vol-tests`.

---

## Global Constraints

- Work directly on local `main`, **in place**. Explicit-path `git add` ONLY — **never** `git add -A`, `-u`, or `.` (the tree carries unrelated uncommitted work: surface-db, sha256, atx-kb/atx-db, python bindings).
- **ONE build at a time.** Release preset only: `cmake --build C:\atx\build-rel --target <tgt>`. Shared deps at `C:\atx-cache\deps`. `parquet.dll` requires `C:\atx\build-rel\bin` on `PATH` to run any example or test that touches parquet. **The full gtest suite MUST be run from `C:\atx\build-rel` as CWD** (a stale repo-root artifact cache otherwise poisons results).
- **Do NOT modify golden fixtures** (`atx-vol/python/tests/data/runarchive/wave_a_fixture.atxrun` sha256 `71ea9632…29f7424`, `atx-vol/python/tests/data/runarchive/dispersion_paired.atxrun`, `atx-vol/python/tests/data/dispersion_parity/trade_schedule.tsv`). **Do NOT touch `C:\atx-data` run dirs** — the CONTROLLER owns every measurement run there. Subagent tasks measure on **fixture-sized corpora only** (§Measurement protocol). **Never read `C:\atx\.env`.**
- **RunArchive schema FROZEN** — `schema_hash` `0xdcce47781ac8390d`, `kRaMinor` 0. Do NOT edit `run_archive_schema.hpp`, the on-disk structs in `run_archive.hpp`, or `_schema.py`. **P1's cache MUST live in its own file format OUTSIDE `run.atxrun`** (Task 7 defines `ATXDEFS1`, a standalone sidecar/cache file — it is not a RunArchive section). If any task appears to need a registry change, STOP and escalate to the controller rather than bumping.
- **The box is shared and is often heavily loaded by other work.** Wall-time measurements are comparable **only when taken back-to-back on a quiet machine**. Therefore: *the baseline and the after-measurement for a task MUST be taken in the same working session, on the same box state, by the same agent*. If another heavy job (a full gtest run, a parity run, another wave's build) starts between the two, **discard both numbers and re-measure**. Report **min-of-3**, never a mean — the minimum is the least contaminated statistic on a loaded box.
- Windows/PowerShell: `$ErrorActionPreference='Continue'` (native stderr wraps in ErrorRecords); use `git commit -F <file>` or a bash heredoc for multi-line messages (here-strings mangle).
- Commit trailer on every commit: `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>` (match the implementing model; Waves A/B used 4.8).
- **Known pre-existing RED tests — do NOT chase or "fix"** (`.superpowers/sdd/backtest-wave-a/t10-failure-triage.md`): `BoundaryHoist.PriceBitIdenticalToPrechange`, `SurfaceV2Qualification.RiskBuild…Budgets/Latency`, and `…/Balanced`. A green Wave E = all-green **modulo these three**.
- **Python is OUT OF SCOPE for this wave.** No pytest task, no `runarchive.py` change, no `_schema.py` regeneration. If a task appears to require a Python-side edit, that is a signal the change is touching the frozen schema — escalate.

---

## Triage of P1–P7 (done against the code, before planning)

Line anchors are current as of `main` @ `382fee2` + the Wave B post-review fix round.

### KEPT

| Pass | Real win (evidence) | Cost / risk | Verdict |
|---|---|---|---|
| **P5** — route the divergence replay through the shared `SnapshotCache` | `collect_mark_divergence_replay` (`examples/spy_dispersion_backtest.cpp:698-757`) loads with **static `MarketSnapshot::load`** at `:710`, bypassing the `config.snapshot_cache` created at `:795`; the priced `run_backtest` at `:839` then re-deserializes the same archives with a still-empty cache. The loop is `for (i = 0; i < clock.size(); ++i)` (`:706`) — **135 sessions on production, not the ~60 the review assumed**, so the double-deserialize is 2.2× larger than stated. The tier keys already align (`:710` passes `config.query_pricing_tier`, same value `backtest.cpp:1839` passes), so it is a like-for-like swap. | **One call site.** Economic risk LOW (the cache is the production load path already — `:548` uses it — and it is content-identity-guarded against stale serves, `snapshot_cache.cpp:94-110,145-156`). **Memory risk MEDIUM**: the cache at `:795` is *unbounded*, so routing 135 loads through it pins 135 fully-deserialized boards resident. Wave B already hit this class of bug once (see the `:621-632` single-slot comment). | **KEEP — first.** Best win/risk in the wave; smallest diff. Memory is a measured gate, not an assumption. |
| **P2** — leg-key-filtered reconciliation join | The join is `listed_quotes_from_opra` (`src/listed_opra.cpp:317-415`; **`load_listed_quotes` no longer exists** — Wave B renamed the seam). It reserves for the whole panel (`:332-333`) and per panel row does a `lower_bound` over `panel.source_identities` (`:337-341`), a `definitions.find` that is a `lower_bound` over the **entire** definitions vector with two `std::string` compares per probe (`:346-347` → `:165-173`), and an OSI parse (`:385`). Consumers need only the frozen legs: `2·(1+n_names)` = **102 legs** per date, **204 on a roll date** (`src/listed_dispersion_schedule.cpp:183`), against thousands of panel rows. | **Single caller-side site**: `examples/spy_dispersion_backtest.cpp:550-551`, where `schedule` is already in hand (read at `:520-521`) — so the leg keys are available. Needs a **public** key type: `LegKey`/`key_of` are anonymous-namespace-private today (`src/listed_dispersion_reconciliation.cpp:29-47`). **Behaviour risk MEDIUM and explicit:** the loop is not only a producer, it is a whole-panel *validation gate* — `:344` (identity missing), `:383` (look-ahead/expiry), `:390` (economics disagree), `:410` (future quote) are fatal for **any** panel row. A filter narrows the definition-dependent gates to the consumed keys. That narrowing must be a documented, tested decision, not a silent side effect. | **KEEP — second.** Biggest single-run lever in `run-backtest`. |
| **P3** — definitions-parse hot loop (+ L3) | (a) `ListedDefinitionTable::create` `src/listed_opra.cpp:148` does `iso_to_ns(definition.trade_date + "T23:59:59.999999999Z")` **unconditionally per row**; `trade_date` is an owning `std::string` (`listed_opra.hpp:20`) so the 31-char temporary exceeds MSVC SSO → a genuine malloc/free **per row** (~8.7M). (b) `split` (`:67-80`) has **no `reserve`**: the line index at `:201` is ~8.7M `push_back`s with geometric realloc (transient peak ≈2.5× the ~140 MB final, on top of the live 696 MB `contents`), and the 9-field vector at `:211` is **~6 allocations per row** on MSVC's 1.5× growth, i.e. ~50M allocs, not 8.7M. (c) **L3**: `:160-161` rebuilds the whole ~696 MB canonical TSV via `serialize_listed_definitions` (`:175-198`) purely to hash it, then discards it — so peak RSS carries `contents` + the row vector + a throwaway 696 MB string simultaneously. | (a) is ~10 lines and **needs no map**: `:136-137` sorts by `definition_key` = `tie(trade_date, instrument_id, raw_symbol)`, so a single-slot last-value memo suffices (this **corrects** the review's `unordered_map` prescription at review line 39). (c) has exactly **two** consumers of `ListedDefinitionTable::fingerprint()` — `examples/databento_spy_dispersion_definitions.cpp:541` (write-path stdout diagnostic) and `tests/listed_opra_test.cpp:92` (a *relative* `EXPECT_EQ` between two computed values) — and **no test pins a fingerprint literal**, so making it lazy is safe. (b) is the only risky sub-pass, and the parser has **zero negative-test coverage** (`:202` header gate and `:216-224` field gates are entirely untested), so negative tests are a **prerequisite**, not a follow-up. | **KEEP — third**, split so the cheap/safe sub-passes land before the risky rewrite. |
| **P1** — persistent cross-run cache | Paid twice per pipeline in **separate processes**: `read_listed_definitions_file` at `examples/spy_dispersion_backtest.cpp:420-421` (build-schedule) and `:516-517` (run-backtest), and re-paid on every sweep point because no swept knob (`gross_index_vega`, `delta_band`, `*_dte`, `min_weight_coverage`) changes the definitions bytes. | **HIGHEST risk in the wave** — a wrong key silently serves stale economics. **Its charter primitive does not work as written:** `RunDir::run_identity_hash` (`src/run_archive.cpp:1496-1514`) folds **only** `run_spec.tsv` + `universe_schedule.tsv`. `run_spec.tsv` is *exactly* the file a sweep mutates, so a cache keyed on it would have a **0% hit rate on the sweep it exists to accelerate** — it is simultaneously too wide (carries the swept knobs) and too narrow (excludes the definitions bytes and the corpus). P1 therefore needs a **per-artifact content key**, not the run identity. | **KEEP, but SCOPED and GATED** — see §P1 scope decision. |

### DROPPED — recommend deferring

| Pass | Reason (one line) |
|---|---|
| **P4** — one parallel range OPRA batch | The parallelism it claims to add **already exists**: `batch_spec` (`src/dispersion_workflow.cpp:247-259`) never sets `n_threads`, so it stays `0` = auto (`opra_batch.hpp:128`) and every single-date batch already fans its 51 symbol files over all cores via `parallel_for_dynamic` (`src/opra_batch.cpp:433-443`) — the only recoverable time is ~135 jthread pool spin-ups plus per-date tail imbalance, while one range batch pre-sizes `entries` to `n_symbols × n_dates` ≈ 51×135 ≈ 6.9k `OpraPanel`s held **live simultaneously** (`opra_batch.cpp:346-350`) against 51 today, trading a bounded wall-time win for an unbounded resident-set risk. *(Noted per the spec: P4 does compound with P2 — but a safe version needs a streaming `load_opra_daterange` consumer callback, i.e. a new API, which is a feature build and not this wave. Revisit then.)* |
| **P6** — avoid the second full-file read on roll dates | `surface_fingerprint` is a **byte-golden field**: `hash_archive_file` (`src/listed_dispersion_pipeline.cpp:39-52`) is preserved verbatim precisely because its value is baked into the committed `trade_schedule.tsv` (`surface_fingerprint = 15611810793130839`, col 29) and gates golden `b640b3ab…` — so deriving it from `ArchiveContentIdentity` is a golden re-baseline, not a perf pass, and the byte-preserving variant (hash the already-mapped bytes) would first require `MarketSnapshot` to expose its mapping (it has no `identity()` accessor at all) to save **7** whole-file hashes of 1.5–1.7 MB archives per run. |
| **P7** — subset archive deserialize | **Its stated precondition is FALSE.** The corpus is built from `all_symbols(universe_rows)` read out of the very universe file the backtest later reads (`examples/spy_dispersion_backtest.cpp:319-328` builds the archives; the same file is copied into the run dir at `:387-388` and re-read at `:515`), and `corpus.cpp:747-768` archives only *admitted* boards from those cells — so a per-date archive can never hold more surfaces than the universe. Precondition absent ⇒ dropped, per the spec's own conditional. |

### Findings recorded, deliberately NOT turned into tasks (out of the §7 charter)

These are real and code-verified. They are logged here so they are not lost, and are **recommended to the controller for a follow-up wave** rather than smuggled into a wave whose charter is P1–P7:

1. **`build_listed_dispersion_schedule` deserializes all 135 whole boards to make ~7 rolls.** `src/listed_dispersion_pipeline.cpp:213` calls static `MarketSnapshot::load(ref.archive_path)` for **every** clock ref, and only then does the DTE-skip `continue` at `:218-223`. The skip needs `snapshot.ts_ns()` (`:214-217`), and **neither `SnapshotRef` (`backtest.hpp:59-62`) nor `CorpusEntry` (`corpus.hpp:271-282`) carries a timestamp**, so hoisting the skip above the load requires a new "peek the archive header for the valuation ts" helper. That is a genuine ~19× reduction in build-schedule archive reads and it moves no golden value — but it is a new capability, not one of P1–P7. **Do not add caching here instead: each date is loaded exactly once, so a cache would buy nothing.**
2. **`run_projected_var_command` (`:950-958`) is a third uncached 135-archive sweep that retains every board in a vector.** Same defect class as P5, but each date is loaded once so there is no wall-time win — it is memory hygiene only.
3. **Supplying a shared cache disables the engine's subset path.** `backtest.cpp:1424-1434` builds `book_uids` and uses them *only* when `cfg.snapshot_cache == nullptr`; the dispersion example always supplies one (`:526`, `:795`). A book-derived subset (22 positions vs 51 archive surfaces) is the *reframed* P7 and is wireable (`MarketSnapshot::load`'s subset branch already exists at `backtest.cpp:1181-1214`), but `referenced_uids` is a **constructor-only** parameter (`backtest.hpp:197`), so one shared cache cannot serve both a subset book and the whole-board reconciliation pass — which is exactly why it is deferred rather than adopted.

### P1 scope decision

**Kept:** the **pre-parsed definitions cache** — one artifact, one provable key.
**Deferred, with reasons:**
- *Per-date joined-quotes cache* — its key must cover `opra_root`/`path_template`/`snapshot_suffix`/`flat_rate`/symbol set/date/definitions fingerprint/`MissingDefinitionPolicy`/join-code revision, and P2 is about to shrink the thing being cached by ~100×. Caching a 100×-smaller artifact is not worth the stale-serve risk.
- *"Cache the selection" (the vega-sweep win)* — the selection **is** the economics; memoizing it has the worst failure mode in the system and needs its own design pass, not a slot at the tail of a perf wave.
- *Snapshot disk-backing* — `SnapshotCache` is in-memory only by design (`src/snapshot_cache.cpp:114-206`); giving it disk backing is a new format on the hottest correctness path. Out.

**P1 is additionally GATED**: Task 7 Step 1 is a go/no-go on the number Task 5 produced. If, after P3, the `definitions_parse` phase is under **15%** of the `run-backtest` + `build-schedule` combined wall time on the fixture, P1's marginal win no longer justifies a new on-disk format and **the correct action is to stop the wave at Task 6** and record that. That is a real exit, not a formality.

---

## File Structure

**New (P2):**
- `atx-vol/include/atx/vol/listed_quote_key.hpp` — the public `ListedQuoteKey` + `quote_key_of(...)` overloads promoted out of `listed_dispersion_reconciliation.cpp`'s anonymous namespace.

**New (P1):**
- `atx-vol/include/atx/vol/listed_definitions_cache.hpp`, `atx-vol/src/listed_definitions_cache.cpp` — the `ATXDEFS1` sidecar cache (its own format; **not** a RunArchive section). Register the `.cpp` in `atx-vol/CMakeLists.txt` immediately after `src/listed_opra.cpp`.

**New tests:**
- `atx-vol/tests/listed_definitions_cache_test.cpp` (register in `atx-vol/tests/CMakeLists.txt` beside `listed_opra_test.cpp`).

**Modified:**
- `atx-vol/examples/spy_dispersion_backtest.cpp` — T1 phase splits; T2 replay cache routing; T3 filtered-join wiring; T8 cache CLI surface.
- `atx-vol/src/listed_opra.cpp` — T3 filtered join variant; T4 `trade_end` memo + lazy fingerprint; T5 single-pass field scan; T8 cache-aware read path.
- `atx-vol/include/atx/vol/listed_opra.hpp` — the `wanted` parameter; the lazy-fingerprint member.
- `atx-vol/src/listed_dispersion_reconciliation.cpp` — T3: consume the promoted public key type (delete the private duplicate).
- `atx-vol/src/run_archive.cpp`, `atx-vol/include/atx/vol/run_archive.hpp` — T6 `run_identity_hash` widening.
- `atx-vol/tests/listed_opra_test.cpp`, `atx-vol/tests/run_archive_test.cpp`, `atx-vol/tests/listed_dispersion_reconciliation_test.cpp` — tests.
- `atx-vol/CMakeLists.txt`, `atx-vol/tests/CMakeLists.txt` — registration.

**Deliberately NOT touched:** `run_archive_schema.hpp`, the RunArchive on-disk structs, `_schema.py`, any committed fixture, `src/opra_batch.cpp` (P4 dropped), `hash_archive_file` (P6 dropped), the engine subset path (P7 dropped), everything Python.

---

## Measurement protocol (binding for every task)

Each implementer follows this verbatim. Deviating invalidates the task's numbers.

1. **Private fixture.** Copy the pristine 3-session fixture run dir to a task-private directory (recipe: `.superpowers/sdd/dispersion-parity/task-9-report.md` — copy the run dir, then rewrite the `occ_ess_inventory.tsv` path column to the new location). **NEVER modify the pristine source.** Fixture scale: 3 dates, 1 roll, 33 admitted, 10 universe rows, `definitions.tsv` 13.2 MB / 123,794 rows, three 1.5 MB `.atxvsa` archives.
2. **PATH.** `$env:PATH = "C:\atx\build-rel\bin;$env:PATH"` (parquet.dll).
3. **Quiet box.** Confirm no other build / gtest / parity run is active before starting. Announce in the task report if the box was not quiet.
4. **Capture.** Run the subcommand under measurement **3× back-to-back**, and after each run read the phases:
   `C:\atx\build-rel\bin\atxvol_spy_dispersion_backtest.exe <subcommand> --run <FIXTURE>`
   `C:\atx\build-rel\bin\atxvol_spy_dispersion_backtest.exe runarchive dump <FIXTURE> diagnostics --tsv`
   Record **min** `wall_ms` per phase across the 3 runs, plus the `total` row.
5. **Peak memory** (P5 and any pass that changes residency): capture peak working set for the process — e.g. `Get-Process` sampling or `(Measure-Command { … })` alongside a `Get-Process -Name atxvol_spy_dispersion_backtest | Select-Object PeakWorkingSet64` poll. Report it in MB.
6. **Baseline then after, same session.** Baseline is captured **before** any source edit for that task. The after-measurement is captured after the build, in the same session. Both go in the task report as a table: `phase | baseline min ms | after min ms | ratio`.
7. **Reference numbers only, NOT baselines.** A prior recorded fixture diagnostics set exists at `<scratchpad>/t10B/run/diagnostics_*.tsv`: `build_schedule` setup_read 480.981 / selection 157.389 / quote_join 171.551 / total 820.138; `run_backtest` setup_read 451.624 / engine_run 23.842 / reconciliation 444.368 / total 927.890; `run_projected_backtest` setup_read 1.049 / divergence_replay 447.690 / archive_load 444.377 / priced_run 401.450 / total 851.213. **Treat these as indicative shape only.** The invocation flags behind them were not recorded (notably whether `run-projected-backtest` ran `--execution cold` or the default `configured`, which changes whether each `MarketSnapshot::load` also rebuilds the RepresentativeFast tier and therefore changes `archive_load` by an order of magnitude). **Every task captures its own baseline; none of these numbers may be cited as one.**

**Byte-stability gate (binding for every task).** On the private fixture, after the change:
- economics line exact: `backtest complete: dates=3 rolls=1 final_nav=-456.5769067`;
- `runarchive dump <FIXTURE> <section> --tsv` sha256 identical to the Task-1-captured fixture goldens for **every** section the task's code path can reach — at minimum `backtest`, `reconciliation`, `contract_marks`, `trade_schedule`; plus `projected_cold`, `projected_schedule`, `mark_divergence` for tasks touching the projected route.
- Never compare `run.atxrun` whole-file bytes: the `diagnostics` section carries wall-clock `wall_ms`, so whole-file bytes legitimately differ across runs (documented in the Wave B ledger). Compare **sections**.

---

## Task 1: Measurement + golden substrate — make the two invisible passes measurable, and pin the sections P2 will touch (RED for the whole wave)

**Files:**
- Modify: `atx-vol/examples/spy_dispersion_backtest.cpp`
- Test: `atx-vol/tests/run_archive_test.cpp` (diagnostics row-set assertion, if one exists to extend)

**Why this task exists.** Two of the four surviving passes cannot be measured today:
- **P3's target is invisible.** `read_listed_definitions_file` sits *inside* the `setup_read` phase in both subcommands (`:420-421` charged at `:432`; `:516-517` charged at `:527`), mixed with `read_run_spec` + `read_universe` + `read_manifest_file` + `Clock::from_manifest` + `verify_occ_ess_evidence` (+ the schedule read and strategy construction in run-backtest). There is **no `definitions_parse` phase**.
- **P2's target is contaminated.** The `reconciliation` phase (`:540`→`:570`) covers the per-date `snapshot_cache->load` calls (`:547-548`), the `listed_quotes_for_date` joins (`:550-551`) **and** `reconcile_listed_schedule` (`:567`) in one number.

And **P2's outputs are not byte-pinned**: the four sprint goldens are `backtest`, `projected_cold`, `trade_schedule`, `projected_schedule` — the `reconciliation` and `contract_marks` sections, which are exactly what a changed join feeds, have **no** captured hash. Landing P2 without capturing them first would be landing an unguarded economics change.

**Interfaces (Produces):**
- `build_schedule_command`'s timer declaration (`:415`) becomes `{"setup_read", "definitions_parse", "selection", "quote_join", "write_outputs"}`; the definitions read at `:420-421` is bracketed by its own `now()`/`add("definitions_parse", …, 1u)` pair and **excluded** from `setup_read` (charge `setup_read` in two segments around it so the two phases are disjoint and sum to the old `setup_read`).
- `run_backtest_command`'s timer declaration (`:511`) becomes `{"setup_read", "definitions_parse", "engine_run", "snapshot_load", "quote_join", "reconcile", "write_outputs"}`; the `:546-553` loop charges `snapshot_load` around `:547-548` and `quote_join` around `:550-551` (count = `clock.size()` each), and `reconcile` brackets `:567-569`. The aggregate `reconciliation` phase is **replaced** by these three disjoint phases.
- A `<FIXTURE>-goldens/` sha256 manifest committed to the task report (not to the repo): `backtest`, `reconciliation`, `contract_marks`, `trade_schedule`, `projected_cold`, `projected_schedule`, `mark_divergence`.

**Notes:** This is a **diagnostics row-set change** — the `diagnostics` section gains rows and one phase name disappears. That is a *data* change, not a schema change: `kDiagnosticsCols` is `(subcommand, phase, wall_ms, count)` and is untouched, so `schema_hash` cannot move. **Before editing, grep for anything that pins the diagnostics row set or phase names** (`atx-vol/tests/`, and `print_diag_summary` at `:119-125` which uses an independent whole-command total). If a test pins the row count, update that test in this task and say so. Python is out of scope — if a Python assertion pins phase names, STOP and escalate.

- [ ] **Step 1: Capture the pre-change baseline** — per §Measurement protocol on a private fixture: `build-schedule`, `run-backtest`, `project-schedule`, `run-projected-backtest --execution cold`, min-of-3 each, full phase tables. This is the wave's reference baseline; record the exact invocations including all flags.
- [ ] **Step 2: Capture the fixture section goldens (the RED that protects everything after)** — for each of the seven sections above, `runarchive dump <FIXTURE> <section> --tsv | sha256`. Record all seven hashes plus row counts in the task report. **A task later in this wave that cannot reproduce these hashes has failed.**
- [ ] **Step 3: Write the failing assertion** — extend the diagnostics test (or add one) asserting that a `run_backtest` diagnostics section contains rows named `definitions_parse`, `snapshot_load`, `quote_join`, `reconcile` and does NOT contain `reconciliation`; and that a `build_schedule` section contains `definitions_parse`. Build → FAIL (phases do not exist).
- [ ] **Step 4: Implement the phase splits** — edit both `PhaseTimer timer({...})` initializers and add the disjoint `now()`/`add()` pairs. **No economics may be touched**: this task changes only timer bookkeeping.
- [ ] **Step 5: Run to verify it passes** — `cmake --build C:\atx\build-rel --target atx-vol-tests` then the diagnostics filter → PASS.
- [ ] **Step 6: Re-measure and prove the split is a partition** — rerun `build-schedule` + `run-backtest` on the fixture; assert `definitions_parse + setup_read ≈ old setup_read` and `snapshot_load + quote_join + reconcile ≈ old reconciliation` (within run-to-run noise), and record the **new, decomposed baseline** — this is the number Tasks 3, 4, 5 and 7 are measured against.
- [ ] **Step 7: Byte-stability gate** — fixture `final_nav=-456.5769067`; all seven section hashes from Step 2 unchanged (the diagnostics section is deliberately excluded — it is what changed).
- [ ] **Step 8: Commit** (`git add atx-vol/examples/spy_dispersion_backtest.cpp atx-vol/tests/run_archive_test.cpp`).

---

## Task 2: P5 — route the mark-divergence replay through the shared `SnapshotCache`

**Files:**
- Modify: `atx-vol/examples/spy_dispersion_backtest.cpp` (`collect_mark_divergence_replay` `:698-757`; the cache construction at `:795`)

**Interfaces (changes):**
- `collect_mark_divergence_replay`'s per-session load at `:709-710` becomes `ATX_TRY(std::shared_ptr<const MarketSnapshot> snapshot, config.snapshot_cache->load(ref.archive_path, config.query_pricing_tier));` and `on_step(*snapshot, …)` (`on_step` takes `const MarketSnapshot&` — `listed_dispersion_strategy.hpp:55-58` — so the deref is the only call-site change). The `archive_load` phase brackets the cached call unchanged, so the before/after is read off the same phase name.
- The cache at `:795` is constructed with an **explicit capacity** ≥ `clock.size()` so the priced run's loads at `backtest.cpp:1839` are hits. Document at the construction site *why* the capacity is clock-sized: an LRU smaller than the clock gives zero cross-pass reuse, because the replay walks dates 0..N-1 and the priced run then walks 0..N-1 again.
- Fix the stale comment at `:822` naming `write_mark_divergence_replay` (the function is `collect_mark_divergence_replay`).

**Precondition check (Step 1).** If Wave D has landed the `RunConfig::StepObserver` hook and deleted the shadow replay, **this task is moot and must be dropped, not adapted** — one real engine run would already capture divergence. As of this plan's authoring the hook does **not** exist: `RunConfig` is `backtest.hpp:300-341` and a repo-wide grep for `StepObserver|step_observer|on_step_observed` over `atx-vol` returns zero matches. Re-verify before starting.

**Risk and its bound.** Economic risk is low — the cache is already the production load path (`:548`), passes the same tier, and evicts on content-identity change (`snapshot_cache.cpp:94-110,145-156`), so it cannot serve a stale board. The real risk is **residency**: with a clock-sized capacity, all `clock.size()` boards stay resident. **Abort condition, stated up front:** if peak working set on the fixture grows by more than 3× over baseline, do NOT land the clock-sized capacity — report the measured numbers, land nothing, and record that P5's full win requires Wave D's StepObserver (which removes the second pass entirely rather than caching it).

- [ ] **Step 1: Verify the precondition** — grep `atx-vol` for `StepObserver|step_observer`; confirm `collect_mark_divergence_replay` still exists and that `:710` is still a static `MarketSnapshot::load`. If either is false, STOP and report to the controller.
- [ ] **Step 2: Capture the baseline** — per §Measurement protocol: `run-projected-backtest --execution cold --schedule projected_schedule.tsv`, min-of-3, phases `setup_read / divergence_replay / archive_load / priced_run / write_outputs / total`, **plus peak working set**. Then repeat with `--no-divergence` (the replay-free path) — the difference between the two totals is the replay's true marginal cost and is the honest denominator for the claimed win.
- [ ] **Step 3: Write the regression test** — the behaviour that must not change is "the replay sees the same board the engine sees". Add a test that loads one fixture archive twice — once via static `MarketSnapshot::load(path, tier)` and once via `SnapshotCache(cap).load(path, tier)` — and asserts the two `SurfaceSet`s price a fixed contract to **bit-identical** `fair_value` (`EXPECT_EQ` on the raw double) and report identical `ts_ns()`. Build → this should PASS immediately; it is a **lock**, not a RED, and the task report must say so plainly. The genuine RED for this pass is the byte-stability gate in Step 6.
- [ ] **Step 4: Implement** — the load swap, the deref, the explicit capacity + its comment, the stale-comment fix.
- [ ] **Step 5: Measure after** — same invocations as Step 2, same session, min-of-3, plus peak working set. **Target:** `priced_run` falls to the engine-only cost (the replay's loads become the only cold loads), and `total` for the divergence path approaches `--no-divergence` total + one archive-load pass. Report the table and the memory delta. Apply the Step-0 abort condition if memory regressed >3×.
- [ ] **Step 6: Byte-stability gate** — fixture `final_nav=-456.5769067`; `projected_cold`, `projected_schedule`, `mark_divergence`, `backtest`, `reconciliation`, `contract_marks`, `trade_schedule` section hashes all identical to Task 1 Step 2. `mark_divergence` is the sharpest gate here: if a cache-served board priced differently from a heap-loaded one, its row count would move off zero.
- [ ] **Step 7: Commit** (`git add atx-vol/examples/spy_dispersion_backtest.cpp`).

---

## Task 3: P2 — leg-key-filtered reconciliation OPRA join

**Files:**
- Create: `atx-vol/include/atx/vol/listed_quote_key.hpp`
- Modify: `atx-vol/include/atx/vol/listed_opra.hpp`, `atx-vol/src/listed_opra.cpp`, `atx-vol/src/listed_dispersion_reconciliation.cpp`, `atx-vol/examples/spy_dispersion_backtest.cpp`
- Test: `atx-vol/tests/listed_opra_test.cpp`

**Interfaces (Produces):**
- `struct ListedQuoteKey { std::string raw_symbol; std::int64_t expiry_ts_ns; double strike; Side side; auto operator<=>(const ListedQuoteKey&) const = default; };` plus `ListedQuoteKey quote_key_of(const ListedScheduleLeg&)` and `quote_key_of(const ListedOptionQuote&)` — promoted **verbatim** out of `src/listed_dispersion_reconciliation.cpp:29-47`'s anonymous namespace (`LegKey`/`key_of`). The reconciliation file then *consumes* the public type and its private duplicate is deleted, so there is exactly one key definition — the filter and the consumer provably agree by construction.
- `listed_quotes_from_opra(..., MissingDefinitionPolicy policy, std::span<const ListedQuoteKey> wanted = {})` — when `wanted` is non-empty, a panel row whose `quote_key_of` is not in `wanted` is skipped **after** the panel-wide `source_identities` lookup and its missing-identity fatal gate (`:337-345`) and **before** `definitions.find` (`:346-347`), the OSI parse (`:385`) and quote construction. Empty `wanted` = today's behaviour, bit-for-bit.
- `listed_quotes_for_date(spec, definitions, symbols, date, std::span<const ListedQuoteKey> wanted = {})` (`listed_dispersion_pipeline.hpp:93-95`) forwards `wanted` through. The build-schedule caller (`src/listed_dispersion_pipeline.cpp:236-237`) passes **nothing** — it is *selecting* contracts, so its consumer `select_listed_dispersion` scans the full quote set (`src/listed_dispersion.cpp:46-71`) and a filter there is structurally impossible. Only the reconciliation caller filters.

**The documented behaviour narrowing (read this before implementing).** The panel loop is also a validation gate. Four fatal checks fire for *any* panel row today: missing source identity (`:344`), definition look-ahead/expiry (`:383`), economics disagreement (`:390`), future quote (`:410`). Filtering **preserves** the first (it precedes the filter) and **narrows** the last three to the consumed keys. That is an accepted, deliberate trade — panel-wide definition validation is not the reconciliation's job — and it MUST be (a) stated in the header contract for `wanted`, and (b) covered by tests proving each narrowed gate still fires for a **wanted** key. Silently losing a fail-closed gate is the failure mode of this task.

**Wiring.** In `run_backtest_command`, build the wanted-key set **once** before the `:546-553` loop from `schedule.rolls[*].legs` (every leg's `raw_symbol`/`expiry_ts_ns`/`strike`/`side` is present), sorted + deduped, and pass it to `listed_quotes_for_date` at `:550-551`. Use the union over **all** rolls, not the active cohort: a roll date marks both the held and the entering cohort (`listed_dispersion_reconciliation.cpp:270-271,288-289,322-323`), and the union is ~102·n_rolls keys — trivially small and immune to cohort-boundary reasoning errors.

- [ ] **Step 1: Capture the baseline** — per §Measurement protocol: `run-backtest`, min-of-3, with Task 1's decomposed phases. The number under test is `quote_join`; also record `reconcile` (a smaller quote set shrinks `quote_index`'s `std::map` build at `listed_dispersion_reconciliation.cpp:111-127`, so a second-order win should appear there) and peak working set.
- [ ] **Step 2: Write the failing tests** — in `listed_opra_test.cpp`, over the existing synthetic panels: (a) `FilteredJoinEqualsUnfilteredOnWantedKeys` — run `listed_quotes_from_opra` unfiltered, take the subset of results matching a chosen 3-key `wanted` set, then run filtered with that set and assert the two vectors are **element-wise identical** (every field, `EXPECT_EQ` on raw doubles) and in the same order; (b) `FilteredJoinEmptyWantedIsUnfiltered` — an empty span reproduces today's full output exactly; (c) `FilteredJoinStillFatalOnWantedLookAhead`, `…OnWantedEconomicsMismatch`, `…OnWantedFutureQuote` — each narrowed gate still returns `Err` when the offending row IS in `wanted`; (d) `FilteredJoinStillFatalOnMissingIdentity` — the panel-wide gate fires even for an unwanted row. Build → FAIL (`wanted` parameter and `ListedQuoteKey` do not exist).
- [ ] **Step 3: Implement** — create `listed_quote_key.hpp` (verbatim promotion); delete the private `LegKey`/`key_of` from `listed_dispersion_reconciliation.cpp` and consume the public ones; add `wanted` to `listed_quotes_from_opra` + `listed_quotes_for_date`; wire the reconciliation caller. Keep the whole-panel `quotes.reserve` (`:332-333`) sized to `wanted.size()` when filtering.
- [ ] **Step 4: Run to verify it passes** — build; `--gtest_filter=ListedOpra.*:ListedDispersionReconciliation*:ListedDispersionPipeline.*` → PASS (the four existing reconciliation tests are the strongest existing net: `ExactModelPnlClosesToCanonicalBacktest`, `MissingRawQuoteReducesCoverageWithoutPatchingModel`, `ReconcilesHeldMarksAcrossAtomicRoll`, `RejectsMissingSurfaceAndEntryMarkMismatch`).
- [ ] **Step 5: Measure after** — same session, min-of-3. **Target:** `quote_join` drops by roughly the panel-rows-to-legs ratio; `reconcile` drops second-order. Report the table.
- [ ] **Step 6: Byte-stability gate** — fixture `final_nav=-456.5769067`; **`reconciliation` and `contract_marks` section hashes identical to Task 1 Step 2** (these are the sections this change feeds and the reason Task 1 captured them), plus `backtest` and `trade_schedule` unchanged. A moved `quote_mid_coverage` or `n_quote_mid_lots` means a leg key was missed — investigate, do not re-baseline.
- [ ] **Step 7: Commit** (`git add atx-vol/include/atx/vol/listed_quote_key.hpp atx-vol/include/atx/vol/listed_opra.hpp atx-vol/src/listed_opra.cpp atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp atx-vol/src/listed_dispersion_pipeline.cpp atx-vol/src/listed_dispersion_reconciliation.cpp atx-vol/examples/spy_dispersion_backtest.cpp atx-vol/tests/listed_opra_test.cpp`).

---

## Task 4: P3(a) + P3(c) — per-row `trade_end` memo, lazy table fingerprint, and the parser negative tests P3(b) needs

**Files:**
- Modify: `atx-vol/include/atx/vol/listed_opra.hpp`, `atx-vol/src/listed_opra.cpp`
- Test: `atx-vol/tests/listed_opra_test.cpp`

**Interfaces (changes):**
- **P3(a)** `ListedDefinitionTable::create` (`src/listed_opra.cpp:134-163`): replace `iso_to_ns(definition.trade_date + "T23:59:59.999999999Z")` at `:148` with a `char[32]` stack-buffer format (no allocation — `iso_to_ns` already takes `std::string_view`, `atx-core/include/atx/core/hash.hpp` neighbourhood / `data.hpp:227`) **plus a single-slot memo**: the rows are sorted by `definition_key` = `tie(trade_date, instrument_id, raw_symbol)` at `:136-137` *before* the loop at `:139`, so `trade_date` is non-decreasing and a two-field `last_date`/`last_trade_end` memo is exact. **This corrects the review's prescription** (review line 39 asks for an `unordered_map`; the sort makes that unnecessary and strictly slower). Behaviour: identical `trade_end` for every row, so the `:149-152` rejection gate is unchanged.
- **P3(c)** `ListedDefinitionTable::fingerprint()` becomes **lazily computed**: drop the eager `serialize_listed_definitions` + `fingerprint_text` at `:160-161`; compute on first call and memoize behind a `std::once_flag` (mutable members). Rationale and safety: exactly two consumers exist — `examples/databento_spy_dispersion_definitions.cpp:541` (a write-path stdout diagnostic) and `tests/listed_opra_test.cpp:92` (a *relative* equality between two computed values) — **no consumer on the backtest read path, and no test pins a literal**. The win is not only CPU: today peak RSS carries `contents` (~696 MB) + the row vector + a throwaway ~696 MB re-serialization simultaneously.
- **Prerequisite for Task 5:** add the parser negative tests that do not exist today. `:202` (exact magic/header equality) and `:216-224` (field count + numeric parses) are **entirely untested**. Task 5 rewrites exactly that code, so the net must exist first.

**Notes:** Do NOT "fix" the LF-only line handling or the empty-line skip (`:208-209`) — those are load-bearing current behaviour (the empty-line skip is what absorbs the trailing element `split` emits after the file's final `\n`). A perf pass is not the place to change parser tolerance.

- [ ] **Step 1: Capture the baseline** — per §Measurement protocol, `build-schedule` and `run-backtest` min-of-3, reading the **`definitions_parse`** phase Task 1 created. Also record peak working set (P3(c) is largely an RSS win at fixture scale and a CPU+RSS win at 696 MB).
- [ ] **Step 2: Write the failing tests** — (a) `DefinitionTableTradeEndMemoMatchesPerRowCompute`: build a table whose rows span ≥3 distinct `trade_date`s with multiple rows per date and assert every row is admitted, plus that a row with `definition_ts_ns` one ns past its date's `trade_end` is rejected — pinning the memo's boundary exactly where `:148-152` puts it. (b) `DefinitionFingerprintIsLazyAndStable`: `fingerprint()` returns the same nonzero value on two consecutive calls and equals the value a freshly parsed copy reports (this is the existing `:92` invariant, restated as a lock). (c) **the negative-parse suite** — `parse_listed_definitions` returns `Err` for: wrong magic, wrong header, a row with 8 fields, a row with 10 fields, a non-numeric `instrument_id`, a non-numeric `multiplier`, a non-numeric `source_fingerprint`; and returns `Ok` for a file whose final line is empty. Build → the negative cases may pass immediately against the current parser (they are locks); (a)/(b) drive the change.
- [ ] **Step 3: Implement** P3(a) then P3(c). Keep `serialize_listed_definitions` itself unchanged — only *when* it is called moves.
- [ ] **Step 4: Run to verify it passes** — build; `--gtest_filter=ListedOpra.*:StandardMonthlyClassifier.*` → PASS (all 11 existing `ListedOpra.*` tests plus the new ones).
- [ ] **Step 5: Measure after** — same session, min-of-3. **Target:** `definitions_parse` falls measurably in both subcommands and peak working set falls. Report the table with a separate line for (a) and (c) if they can be measured independently (build (a) first, measure, then (c), measure) — attribute the win, do not report one blended number.
- [ ] **Step 6: Byte-stability gate** — fixture `final_nav=-456.5769067`; all seven Task-1 section hashes unchanged. The `trade_schedule` hash is the one that matters most here: it depends on `definitions.find` results through the whole selection path.
- [ ] **Step 7: Commit** (`git add atx-vol/include/atx/vol/listed_opra.hpp atx-vol/src/listed_opra.cpp atx-vol/tests/listed_opra_test.cpp`).

---

## Task 5: P3(b) — single forward pass over the fixed field boundaries

**Files:**
- Modify: `atx-vol/src/listed_opra.cpp`
- Test: `atx-vol/tests/listed_opra_test.cpp`

**Interfaces (changes):**
- `parse_listed_definitions` (`src/listed_opra.cpp:200-237`) stops building a per-row `std::vector<std::string_view>` at `:211`. Replace with a single forward scan locating the field boundaries inline into a `std::string_view fields[9]` (stack, no allocation).
- `split` (`:67-80`) gains a `reserve` for the line-index call at `:201` (or the line walk becomes an inline `memchr`-style forward scan that never materialises the ~140 MB index at all — preferred, since the index's realloc peak is ~2.5× its final size on top of the live 696 MB `contents`). `split`'s only two call sites are both in this function, so the helper may be changed or retired freely.

**Two invariants the rewrite MUST preserve** (both are why Task 4's negative tests come first):
1. `:216`'s `fields.size() != 9` rejects a line with **any** other tab count. A scan that merely finds the first 8 tabs and stops would silently accept a 10-field row — it must assert there is **no ninth tab** before the newline.
2. `:208-209` skips empty lines with `continue` (not an error); this is what absorbs the trailing element after the file's final `\n`. Preserve exactly. Keep the parser LF-only.

**Do not over-attribute the win.** `:227 definition.raw_symbol = fields[2];` copies a 21-char OSI symbol into a `std::string` (over MSVC SSO) — **~8.7M allocations this pass does not remove**. Say so in the report rather than claiming the whole per-row alloc count.

- [ ] **Step 1: Capture the baseline** — `definitions_parse` phase, min-of-3, both subcommands, plus peak working set. This baseline is *post-Task-4*, so the measured win here is P3(b)'s marginal contribution, not P3's total.
- [ ] **Step 2: Write the failing test** — `ParserRejectsTenFieldRow` (a row with a 9th tab) and `ParserRejectsEightFieldRow` must be RED-provable against the new scan: temporarily assert them against a first-8-tabs-only scan to prove they are real gates, then keep the correct implementation. Also add `ParseIsByteIdenticalToPriorImplementation`: parse a multi-row fixture and assert `serialize_listed_definitions(parsed)` is byte-identical to the input text (this is the existing `:91` round-trip invariant, now the primary correctness oracle for the rewrite).
- [ ] **Step 3: Run to verify it fails** — build → the RED cases fail against a naive scan, as designed.
- [ ] **Step 4: Implement** the forward scan + the line-walk change; delete or reserve `split` accordingly.
- [ ] **Step 5: Run to verify it passes** — build; `--gtest_filter=ListedOpra.*` → PASS, including all of Task 4's negative suite.
- [ ] **Step 6: Measure after** — same session, min-of-3. Report the marginal ratio and the residual (state that `raw_symbol` allocation remains).
- [ ] **Step 7: Byte-stability gate** — fixture `final_nav=-456.5769067`; all seven Task-1 section hashes unchanged.
- [ ] **Step 8: Commit** (`git add atx-vol/src/listed_opra.cpp atx-vol/tests/listed_opra_test.cpp`).

---

## Task 6: Widen `run_identity_hash` — close the merge-write staleness hole (P1's correctness prerequisite)

**Files:**
- Modify: `atx-vol/include/atx/vol/run_archive.hpp`, `atx-vol/src/run_archive.cpp`
- Test: `atx-vol/tests/run_archive_test.cpp`

**Why this is in a perf wave.** The merge-write guard in `RunDir::write_run_archive` (`src/run_archive.cpp:1516-1590`) **is already a cache**: it carries forward another route's sections across process boundaries when the recomputed identity matches. Its key, `RunDir::run_identity_hash` (`:1496-1514`), folds **only** `run_spec.tsv` and (when present) `universe_schedule.tsv`. So a changed `definitions.tsv`, a rebuilt corpus, or a rebuilt schedule does **not** invalidate the merge — the archive can silently union a `backtest` section computed from one set of inputs with a `projected_cold` section computed from another. That is precisely "a wrong cache key silently serves stale economics", and it is the existing instance of the failure mode P1 must not add a second one of. Flagged as a Minor in the Wave B final review; it is a prerequisite here.

**Interfaces (changes):**
- `RunDir::run_identity_hash()` additionally folds, in a fixed documented order, the bytes of `surface_manifest.tsv`, `input_inventory.tsv`, and `trade_schedule.tsv` **when each exists** (absent files are skipped exactly as `universe_schedule.tsv` is today, so a partially-populated run dir still yields an identity).
- **Deliberately NOT folded: `definitions.tsv` bytes.** Hashing 696 MB on every archive write would be a perf regression inside a perf wave. It is covered *transitively*: `input_inventory.tsv` carries the per-cell `source_fingerprint` / `market_input_fingerprint` for every OPRA input (written by `write_input_inventory`, `examples/spy_dispersion_backtest.cpp:182-195`), and `surface_manifest.tsv` carries the per-date corpus identity. State this reasoning in the header comment so the next reader does not "fix" it.
- The fold order and the skip-if-absent rule become part of the documented contract, and the existing "0 is reserved for unset ⇒ force nonzero" rule is preserved.

**Blast radius.** `created_ts_ns` is derived from the identity (`:1567-1576`), so `run.atxrun` header bytes change. That is already not byte-stable across runs (the `diagnostics` section carries wall-clock `wall_ms`), and **`created_ts_ns` is not a column of any economic section**, so no golden dump hash can move. The committed fixtures are written by test helpers with pinned stamps and are unaffected. Verify all three claims rather than assuming them.

- [ ] **Step 1: Write the failing tests** — `RunIdentityIsSensitiveToEachFoldedInput`: for each of `run_spec.tsv`, `universe_schedule.tsv`, `surface_manifest.tsv`, `input_inventory.tsv`, `trade_schedule.tsv`, mutate one byte in a temp run dir and assert the identity **changes**; and assert it is unchanged when an unrelated file is mutated. `MergeWriteRejectsCarryForwardAfterManifestChange`: write sections A, mutate `surface_manifest.tsv`, write sections B, then assert the archive contains **only** B's sections (no stale carry-forward). Build → FAIL (identity ignores the manifest today, so the merge carries forward).
- [ ] **Step 2: Run to verify they fail** — build + `--gtest_filter=RunDir.*:RunArchive*` → the two new tests RED, everything else green.
- [ ] **Step 3: Implement** the widened fold + the header contract comment.
- [ ] **Step 4: Run to verify they pass** — build + `--gtest_filter=RunDir.*:RunArchive*:Tearsheet*` → PASS. Confirm `ra_schema_hash()` is still `0xdcce47781ac8390d` and the committed fixture sha256 is unchanged (`MatchesCommittedPythonFixture` green).
- [ ] **Step 5: Byte-stability gate** — run the fixture pipeline end to end (`build-schedule → run-backtest → project-schedule → run-projected-backtest --execution cold`); `final_nav=-456.5769067`; all seven Task-1 section hashes unchanged; confirm the merge-write union still produces the expected multi-section archive (identity is stable *within* one input set — the union must survive).
- [ ] **Step 6: Commit** (`git add atx-vol/include/atx/vol/run_archive.hpp atx-vol/src/run_archive.cpp atx-vol/tests/run_archive_test.cpp`).

---

## Task 7: P1 — GO/NO-GO, then the `ATXDEFS1` pre-parsed definitions cache (format + writer + reader + key)

**Files:**
- Create: `atx-vol/include/atx/vol/listed_definitions_cache.hpp`, `atx-vol/src/listed_definitions_cache.cpp`
- Modify: `atx-vol/CMakeLists.txt`
- Create + register: `atx-vol/tests/listed_definitions_cache_test.cpp` (`atx-vol/tests/CMakeLists.txt`)

**Step 1 is a real exit.** Read the post-Task-5 `definitions_parse` measurement. **If `definitions_parse` is under 15% of `build-schedule` + `run-backtest` combined wall time on the fixture, STOP:** P3 has already taken the recoverable win, and a new on-disk format with a stale-serve failure mode is not justified by the remainder. Record the number and the decision, close the wave at Task 6, and report to the controller. Do not proceed on momentum.

**Interfaces (Produces):**
- `struct ListedDefinitionsCacheKey { std::uint64_t content_hash; std::uint64_t source_size; std::uint32_t format_version; std::uint32_t parser_revision; std::uint64_t abi_fold; };` with `ListedDefinitionsCacheKey definitions_cache_key(std::string_view source_bytes);`
- `Status write_definitions_cache(std::string_view cache_path, const ListedDefinitionTable&, const ListedDefinitionsCacheKey&);` — builds the whole image in memory, then `tmp` + fsync + rename (mirror `write_run_archive_file`'s fsync-before-rename, `src/run_archive.cpp`, which itself mirrors `86f2210`).
- `Result<ListedDefinitionTable> read_definitions_cache(std::string_view cache_path, const ListedDefinitionsCacheKey& expected);` — **fail-closed**: any header mismatch, key mismatch, CRC failure, short read, or truncation returns `Err` and the caller falls through to a full parse. Never a partial serve.
- `Result<ListedDefinitionTable> read_listed_definitions_cached(std::string_view tsv_path, std::string_view cache_dir);` — the seam the CLI calls: read the TSV bytes once, compute the key, try the cache, on any miss parse and (best-effort) publish. **A publish failure is never an error** — it is a logged miss.

**The cache-key design (the crux).** The key is a fold of: **(a)** `atx::core::hash_bytes` over the **full byte content** of the source `definitions.tsv`; **(b)** that content's byte length; **(c)** `kDefinitionsCacheFormat` — the `ATXDEFS1` wire version; **(d)** an `abi_fold` — a `sizeof`/`offsetof` fold over `ListedContractDefinition` (the struct the blob encodes), so a field addition or reorder can never be misread, exactly as `schema_hash` protects RunArchive; and **(e)** `kDefinitionsParserRevision`, a hand-bumped integer that **any** semantic change to `parse_listed_definitions` or `ListedDefinitionTable::create` MUST increment (Tasks 4 and 5 are precisely such changes — note in the header that they would each have required a bump had the cache predated them). On open, the reader recomputes (a)+(b) from the **current** source bytes and requires the stored key to match field-for-field, and additionally requires the reconstructed table's own `fingerprint()` to equal the value stamped in the blob header.

**What the key deliberately EXCLUDES, and why.** It contains **no** `run_spec.tsv`, no `universe_schedule.tsv`, and no swept knob. The parsed table is a pure function of the definitions bytes alone; a key that folded the run spec — which is what `RunDir::run_identity_hash` does — would miss on every sweep point, because the swept knobs live in exactly that file. **This is the reason P1 does not lean on `run_identity_hash`** (§Triage): that hash is both too wide and too narrow for this job. Widening it was still worth doing, for the *merge-write* guard, which is Task 6.

**Cost check that must be measured, not assumed.** The key requires hashing the whole source file. The read path **already** holds those bytes (`read_listed_definitions_file` reads `contents` at `src/listed_opra.cpp:243-253` before parsing), so the hash costs a memory-bandwidth pass, not a second I/O — but if the measured hash + blob-load time exceeds ~50% of the post-Task-5 parse time, the pass is not worth its risk and must be reported as such rather than landed.

**Format (`ATXDEFS1`, its own file — NOT a RunArchive section).** Header (fixed size, naturally aligned, little-endian): magic `ATXDEFS1`, major/minor, `header_size`, `endian`, `pointer_bits`, `file_size`, the five key fields, `table_fingerprint`, `n_rows`, `string_table_offset`/`size`, `column_block_offset`, `header_crc32c` (own field zeroed), `payload_crc32c`. Payload: a dict/offset string table for `trade_date` + `raw_symbol`, then contiguous typed column arrays for the numeric fields, 8-B aligned. Reuse `detail::crc32c`/`crc32c_update`/`align_up` from `detail/archive_util.hpp`. `static_assert` on `sizeof` **and** `offsetof` for the header — it is an ABI.

- [ ] **Step 1: GO/NO-GO** — compute the ratio from Task 5's report; record it; stop here if under 15%.
- [ ] **Step 2: Capture the baseline** — the post-Task-5 `definitions_parse` phase, min-of-3, both subcommands, plus the cost of a bare `hash_bytes` pass over the fixture's 13.2 MB `definitions.tsv` (a micro-measurement establishing the key's own cost).
- [ ] **Step 3: Write the failing tests** — `CacheRoundTripReconstructsTableExactly`: write a multi-row table, read it back, assert **every row is field-for-field equal** to the source (`EXPECT_EQ` on raw doubles) and `fingerprint()` matches; `CacheHeaderAbiIsPinned` (`sizeof`/`offsetof` `static_assert`s + a runtime echo); `CacheRejectsTamperedPayload` (flip one payload byte, expect `Err`); `CacheRejectsTruncatedFile`; `CacheKeyIsContentDerived` (two different byte streams ⇒ different keys; identical streams ⇒ identical keys). Build → FAIL (module does not exist).
- [ ] **Step 4: Implement** the header, writer, reader, and key; register the `.cpp` and the test.
- [ ] **Step 5: Run to verify it passes** — build; `--gtest_filter=ListedDefinitionsCache.*` → PASS.
- [ ] **Step 6: Measure the load path** — time `write_definitions_cache` then `read_definitions_cache` on the fixture's definitions table and compare against the Step-2 parse baseline. Report `parse ms | key-hash ms | cache-read ms | net ratio`. If the net is worse than 2× on the fixture, say so plainly — the 696 MB production case should be measured by the controller before the pass is called a win.
- [ ] **Step 7: Commit** (`git add atx-vol/include/atx/vol/listed_definitions_cache.hpp atx-vol/src/listed_definitions_cache.cpp atx-vol/CMakeLists.txt atx-vol/tests/listed_definitions_cache_test.cpp atx-vol/tests/CMakeLists.txt`).

---

## Task 8: P1 — prove a STALE input is detected and not served, then wire the CLI

**Files:**
- Modify: `atx-vol/src/listed_opra.cpp`, `atx-vol/include/atx/vol/listed_opra.hpp`, `atx-vol/examples/spy_dispersion_backtest.cpp`
- Test: `atx-vol/tests/listed_definitions_cache_test.cpp`

**This task is the reason P1 is allowed to exist.** A cache that cannot be shown to reject a stale input is a liability, not an optimization. The staleness proof is written **first** and must be RED against a deliberately weakened key.

**Interfaces (changes):**
- `read_listed_definitions_cached(tsv_path, cache_dir)` becomes the read used by `build_schedule_command` (`examples/spy_dispersion_backtest.cpp:420-421`) and `run_backtest_command` (`:516-517`).
- CLI surface: `--cache DIR` on both subcommands, defaulting to the `ATX_VOL_CACHE` environment variable, defaulting to **disabled** (no cache dir ⇒ today's behaviour, byte-for-byte). Add the flag to `usage()` (`:1134-1147`) and the `--flag` parser. **Cache-disabled must remain the default** so that no existing invocation, including the controller's `parity_full_run.ps1`, silently changes behaviour.
- A `definitions_cache` phase (`hit`/`miss` recorded via the `count` field: 1 = hit, 0 = miss) so the diagnostics show whether a run was served from cache. Without this a reader cannot tell a fast run from a cached run — and that ambiguity is itself a correctness hazard.

**The five staleness cases that must be proven** (each an independent test, each asserting a **MISS that falls through to a correct full parse**, never an error and never a stale serve):
1. **Content change, same size** — mutate one byte of `definitions.tsv` in place (same length) with the cache already published. Proves the key is content-derived, not size/mtime-derived. **This is the headline test.**
2. **Content change, different size** — append a row.
3. **Parser revision bump** — bump `kDefinitionsParserRevision` in the test's expected key and assert the previously-valid cache file is rejected. Proves a semantic parser change (Tasks 4/5 were exactly that) cannot serve pre-change results.
4. **ABI fold change** — construct a key with a perturbed `abi_fold` and assert rejection.
5. **Corrupt hit** — a published cache whose payload CRC is flipped is a miss, not a partial serve.

Plus the positive case: **`CacheHitEqualsFullParseExactly`** — parse the fixture TSV with the cache disabled and with a warm cache, and assert the two `ListedDefinitionTable`s are field-for-field identical over every row and report equal `fingerprint()`s. A hit that is merely *fast* and not *identical* is a defect.

- [ ] **Step 1: Write the failing staleness tests** — the five cases above plus `CacheHitEqualsFullParseExactly`. Build → FAIL (`read_listed_definitions_cached` does not exist).
- [ ] **Step 2: Prove the headline test is a real gate (RED discipline)** — temporarily weaken the key to `(source_size, format_version)` only, rebuild, and confirm case 1 **FAILS** (a same-size content change is served stale). Record the failure message in the task report, then restore the real key and rebuild clean. *This is the step whose absence let the Wave B M1 defect ship — do not skip it.*
- [ ] **Step 3: Implement** `read_listed_definitions_cached`, the `--cache`/`ATX_VOL_CACHE` surface (default disabled), the `definitions_cache` hit/miss phase, and `usage()`.
- [ ] **Step 4: Run to verify they pass** — build; `--gtest_filter=ListedDefinitionsCache.*:ListedOpra.*` → PASS.
- [ ] **Step 5: Measure the end-to-end effect** — on the private fixture: (i) cold, cache disabled (the Task 5 baseline); (ii) cold with `--cache` (pays the publish); (iii) warm with `--cache` (the hit). Three min-of-3 measurements of `definitions_parse` + `definitions_cache` + `total` for both `build-schedule` and `run-backtest`. **Report all three** — a cache that makes the cold path slower must be disclosed, not hidden behind the warm number.
- [ ] **Step 6: Byte-stability gate — run it TWICE** — once with the cache **disabled** and once with a **warm** cache, and require all seven Task-1 section hashes and `final_nav=-456.5769067` in **both**. Identical economics from a cache hit and a full parse is P1's whole contract.
- [ ] **Step 7: Commit** (`git add atx-vol/include/atx/vol/listed_opra.hpp atx-vol/src/listed_opra.cpp atx-vol/examples/spy_dispersion_backtest.cpp atx-vol/tests/listed_definitions_cache_test.cpp`).

---

## Task 9: Wave-E integration gate (controller) + measured summary

**Files:** Modify this plan (check boxes); create/update `.superpowers/sdd/backtest-wave-e/progress.md` (controller-owned ledger).

- [ ] **Step 1: Full build + full suite** — Release build of all `atx-vol` targets + `atx-vol-tests` + `atxvol_spy_dispersion_backtest`; run `atx-vol-tests.exe` **from `C:\atx\build-rel` as CWD**. All green **modulo** the three documented pre-existing reds (`BoundaryHoist.PriceBitIdenticalToPrechange`, `SurfaceV2Qualification…/Latency`, `…/Balanced`). Do **not** run the Python suite concurrently with anything else — the Wave B gate lost an hour to exactly that (three heavy suites at once made the spawned example fail-fast with `0xC0000409`).
- [ ] **Step 2: 135-session parity re-gate (controller-only, `C:\atx-data`)** — run the four-step sequence via `.superpowers/sdd/dispersion-parity/parity_full_run.ps1` (clear the step markers first) on an **idle** box. Confirm economics UNCHANGED: listed `final_nav=125026.0592`, projected-cold `final_nav=123243.1172`, `dates=135`, `rolls=7`, daily-pnl `corr=0.99718`, `mark_divergence rows=0`.
- [ ] **Step 3: Golden hashes — all seven** — `dump backtest --tsv` = `a05470c7…`; `dump projected_cold --tsv` = `cbabca44…`; `trade_schedule.tsv` = `b640b3ab…`; `projected_schedule.tsv` = `d6793d46…`. Then `run-projected-var` and confirm `projected_risk_scenarios.tsv` = `0cf8ac4b50f34ea6`, `projected_risk_legs.tsv` = `0a8b38984c7b6064`, and `projected_var.tsv` **with field 7 excluded** = `d370c78dbb01b513` (field 7 is `projections_per_second`, a wall-clock rate that moves every run — exclude it via `awk -F'\t' 'BEGIN{OFS="\t"}{ $7=""; sub(/\t\t/,"\t"); print }'`; the other two files are byte-goldens as-is). Also confirm `RunDir::verify()` / `validate_all()` pass and `schema_hash` is still `0xdcce47781ac8390d` with `kRaMinor` 0.
- [ ] **Step 4: Capture the production before/after** — this is the wave's headline number and only the controller can produce it. On the idle box, measure the **full four-step pipeline** at the wave's base commit and at HEAD, min-of-2 each (a 135-session run is expensive; state the count), reading phases via `runarchive dump <RUN> diagnostics --tsv`. Report per-subcommand phase tables plus peak working set. Explicitly confirm or refute each task's fixture-scale claim at production scale — a pass whose fixture win does not reproduce at 135 sessions / 696 MB must be reported as not reproducing.
- [ ] **Step 5: Summary table** — in the ledger, one row per landed pass: `pass | phase measured | fixture before | fixture after | production before | production after | ratio | byte-gate result`. Include the DROPPED passes with their reasons, and the out-of-charter findings from §Triage so they carry into the next wave.
- [ ] **Step 6: Commit** the ledger + this checked plan (explicit paths).

---

## Batching (for parallel Opus subagents)

**Genuine parallelism is near-zero this wave.** Every task is C++ and therefore serializes on the single `build-rel` slot; Python is out of scope, so there is no independent lane at all. Reviewers (read-only, no builds) are the only true concurrency.

- **Strict order** (each task leaves the tree compiling and each depends on the prior measurement):
  `T1 → T2 → T3 → T4 → T5 → T6 → T7 → T8 → T9(controller)`
- **File-disjointness** (relevant only for *authoring* ahead, never for building):
  - T2 touches only `examples/spy_dispersion_backtest.cpp`.
  - T4/T5 touch only `src/listed_opra.cpp` (+ its header/test) — sequential w.r.t. each other by design (T4 supplies T5's negative-test net).
  - T6 touches only `run_archive.{hpp,cpp}` + its test — file-disjoint from everything else, so it may be authored during T4/T5 and built after.
  - T7 is all-new files; T8 then wires them into `listed_opra.cpp` + the example.
- **Measurement isolation is the hard scheduling constraint, stronger than the build slot.** Two agents must never measure at the same time, and no agent may measure while a build or a parity run is in flight. The controller serialises measurement windows explicitly.
- **Reviewers** (fresh Opus, read-only) shadow each task via its per-commit diff, as in Waves A/B. A reviewer's specific charge this wave: *does the reported after-number actually come from the phase the change touched, and is the byte-gate the right one for the sections that change could reach?*
- **T9 is controller-only** — it owns `C:\atx-data` and no subagent may touch it.

---

## Self-Review

**Charter coverage (design §7, lines 191–205):**
- P1 → Tasks 7–8, **scoped** to the pre-parsed definitions cache and **gated** on Task 5's measurement; the joined-quotes cache and the cached-selection win are explicitly deferred with reasons (§P1 scope decision). ✓
- P2 → Task 3. ✓
- P3 → Tasks 4 (sub-passes a + c, plus the negative-test prerequisite) and 5 (sub-pass b). L3's fingerprint re-serialization folded into Task 4 as P3(c). ✓
- P4 → **DROPPED** (the parallelism already exists; the fix forces ~6.9k panels resident). ✓ reasoned, not skipped.
- P5 → Task 2, **first**, with the corrected 135-session scale and an explicit memory abort condition. ✓
- P6 → **DROPPED** (`surface_fingerprint` is a byte-golden value). ✓
- P7 → **DROPPED** (precondition proven false from the corpus build path). ✓
- P4's compounding-with-P2 note (spec line 200) → acknowledged in the drop rationale, with the condition under which it should be revisited. ✓

**Measurement rule satisfied:** Task 1 exists solely because P3's and P2's targets are invisible/contaminated in today's diagnostics — the measurement is added as its own task before any pass is attempted. Every subsequent task has an explicit baseline step, a target, an after-measurement step, and a min-of-3 quiet-box protocol. Tasks 4, 5, 7 and 8 report *marginal* wins against the prior task's baseline rather than one blended number. Task 9 supplies the production before/after that no subagent can. ✓

**Byte-stability rule satisfied:** Task 1 Step 2 captures the two sections (`reconciliation`, `contract_marks`) that P2 feeds and that the sprint's four goldens do **not** cover — without that step, Task 3 would be an unguarded economics change. Every task gates on the seven fixture section hashes; Task 8 gates **twice** (cache-disabled and cache-warm); Task 9 re-proves all seven production goldens including the projected-VaR trio with its documented field-7 exclusion. ✓

**RED-first where it applies:** Task 2 Step 3 and Task 4's negative suite are honestly labelled **locks, not REDs** (they pass on arrival) — the real RED for those passes is the byte gate. Genuine REDs: Task 1 Step 3 (phases absent), Task 3 Step 2 (parameter absent), Task 5 Step 2 (a naive scan fails the 10-field case), Task 6 Step 1 (the merge carries forward across a manifest change), Task 7 Step 3 (module absent), and **Task 8 Step 2**, which weakens the key on purpose to prove the staleness test is a real gate. That last step is modelled directly on the Wave B post-review discipline whose absence let M1 ship. ✓

**Freeze integrity:** No task edits `run_archive_schema.hpp`, the RunArchive on-disk structs, or `_schema.py`. Task 1 changes diagnostics *rows*, not columns (`kDiagnosticsCols` untouched ⇒ `schema_hash` cannot move) and Task 1 checks for row-set pins before editing. Task 6 changes `created_ts_ns`, which is not a column of any economic section. Task 7's `ATXDEFS1` is a standalone file, explicitly outside `run.atxrun`. `kRaMinor` stays 0. ✓

**Placeholder scan:** every seam names a real current site (`spy_dispersion_backtest.cpp:415/420-421/432/511/516-517/527/540-570/698-757/709-710/795/822/1134-1147`; `listed_opra.cpp:67-80/134-163/148/160-161/200-237/201/208-209/211/216-224/317-415/332-345/346-347/385/410`; `listed_dispersion_reconciliation.cpp:29-47/111-127/270-271/288-289/322-323`; `listed_dispersion_pipeline.cpp:213/218-223/236-237`; `run_archive.cpp:1496-1514/1516-1590/1567-1576`; `backtest.hpp:59-62/185-226/300-341`; `backtest.cpp:1424-1434/1839`; `opra_batch.cpp:346-350/433-443`; `corpus.hpp:271-282`; `corpus.cpp:747-768`; `dispersion_workflow.cpp:223-230/247-259`; `run_diagnostics.hpp:28-69/79-81`) and a real existing primitive. Test names are concrete. Two review claims were **corrected** against the code rather than copied (the `unordered_map` memo prescription; P5's "~60 archives"), and two premises were **refuted** (P6's byte-preserving assumption; P7's precondition). No "TBD" and no "similar-to". ✓

**Correctly OUT of Wave E, noted not gaps:** the `StepObserver` hook and the de-SPY (Wave D); the `backtest_driver` spine (Wave C); the three known pre-existing reds; everything Python; the three out-of-charter findings in §Triage (build-schedule's 135 whole-board loads, `run-projected-var`'s retained boards, the reframed book-subset P7), each recorded with evidence for the controller to schedule. ✓

## Open questions for the controller (pre-dispatch)

1. **Task 2's memory bound.** Is the stated abort condition (peak working set >3× baseline ⇒ do not land the clock-sized capacity) the right threshold for the production box at 135 boards, or should the capacity be a CLI knob defaulting to today's behaviour?
2. **Task 3's narrowed gates.** Confirm the accepted trade: panel-wide *definition* validation narrows to the ~102·n_rolls consumed keys. The alternative — keep a cheap whole-panel definition scan — costs most of the win back.
3. **Task 6's fold set.** Confirm `trade_schedule.tsv` belongs in the identity fold. Including it means a rebuilt schedule invalidates a prior `backtest` section (desirable), but it makes `build-schedule`'s own archive write depend on a file it wrote moments earlier.
4. **Task 7's GO/NO-GO threshold.** 15% of combined `build-schedule` + `run-backtest` wall time — is that the right bar, or should the decision wait for a controller-measured 696 MB production `definitions_parse` number instead of the fixture ratio?
5. **Task 8's default.** Cache **disabled** by default is proposed so no existing invocation changes behaviour. Confirm — the alternative (enabled with a default cache dir) would make the parity script cache-served, which changes what the gate is proving.

---

## Controller decisions (pre-dispatch, 2026-07-24)

The triage is **accepted**: P5, P2, P3 and a narrowed P1 are in; P4, P6 and P7 are
dropped. Each drop is evidence-backed and recorded in the plan body — P4's
parallelism already exists (`batch_spec` leaves `n_threads=0` = auto, so a date's
51 files already fan over all cores, and a range batch would hold ~6.9k
`OpraPanel`s resident); P6 would re-baseline a byte-golden (`surface_fingerprint`
`15611810793130839` is pinned in the committed `trade_schedule.tsv`, so
substituting `ArchiveContentIdentity` is a golden change dressed as a perf pass);
P7's precondition is provably false (the corpus is built from
`all_symbols(universe_rows)` over the same universe file the backtest re-reads, so
an archive cannot exceed the universe). Dropping three of seven passes because the
code does not support them is the right outcome, not a shortfall.

Points made binding:

1. **Task 1 (pin `reconciliation` + `contract_marks`) BLOCKS P2.** P2 is the wave's
   biggest correctness risk: it narrows three whole-panel fail-closed gates to the
   ~102 consumed leg keys, and its two output sections were the only artifacts in
   this system with no captured golden. Narrowing a fail-closed gate with nothing
   pinned underneath it is how silent economic drift ships. No P2 work is dispatched
   until those goldens exist and are recorded in the ledger.
2. **P5 goes first.** One-line fix, best win/risk, and the planner found the loop is
   all 135 sessions rather than the ~60 the review assumed — so the double
   deserialize is 2.2x larger than documented.
3. **P1 stays scoped to the pre-parsed definitions cache, and stays gated on P3's
   measurement.** If P3 removes most of the parse cost, P1's remaining win may not
   justify a persistent on-disk cache at all — in which case report that and stop.
   A cache that is not worth its staleness risk should not be built.
4. **P1's key excludes `run_spec.tsv`, deliberately.** `run_identity_hash` folds
   exactly that file, which is what a parameter sweep mutates, so keying on it would
   give a 0% hit rate on the sweep P1 exists to accelerate. The widening of
   `run_identity_hash` is still worth doing but belongs to Task 6, for the
   **merge-write staleness guard** — which today can union sections computed from
   different definitions. Keep the two concerns separate.
5. **A stale-input detection test is mandatory for P1**, not optional. The failure
   mode is silently serving stale economics, which is the worst outcome available in
   this system. The test must mutate an input and show the cache refuses to serve.
6. **The controller owns every `C:\atx-data` measurement run.** Implementers measure
   on fixture-sized corpora only. Baseline and after-measurement must be taken
   back-to-back in the same session on a quiet box, or the numbers are not
   comparable and must not be recorded as a speedup.
