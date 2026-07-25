# Backtest Framework — Wave B: `listed_dispersion_pipeline` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lift the ~350–400 LOC of stranded listed-dispersion orchestration and route economics out of `atx-vol/examples/spy_dispersion_backtest.cpp` into a new library module `atx/vol/listed_dispersion_pipeline.{hpp,cpp}`, so every subcommand becomes one library call and the extracted economics are finally reachable by unit tests. Fix the one CONFIRMED correctness defect (**M1**, the reconciliation clock-coupling) at the new seam behind a RED-first regression test, and guard the **I1** two-route cold parity with a value-level test — plus fold the deferred Wave-A minors that fit. Value-preserving: every golden hash and the 135-session economics must be byte-identical afterward.

**Architecture:** The new module is the *listed-route home* named so strangle/mag7 never depend on it. It owns: the per-date adapter seams (`listed_quotes_for_date`, `make_listed_forward_lookup`, `make_listed_risk_lookup`), the multi-date schedule builder (`build_listed_dispersion_schedule` — cadence / coverage gate / deferral / cohort numbering / acceptance gate; **M1 enforced at build**), the cold projection (`project_listed_schedule` — one asserted `analytic=true`+`ColdReference` code path shared with the projected-backtest replay, **I1**), the reconciliation-snapshot assembler + reconcile wrapper (`assemble_reconciliation_snapshots` + `reconcile_listed_schedule` — **M1 timeline-trim lives here**), the projected-VaR synthesis (`dispersion_book_var`), the `ListedDispersionMethodology` policy struct (replacing loose inline literals), and `constexpr kVegaVolPointToUnitVol = 100.0` (replacing the hand-applied ×100 at two boundaries). The example CLI collapses to arg-dispatch + one call per subcommand while preserving process-boundary independence (**I8**). Reference the design spec for the full module map — do **not** restate it: [`docs/superpowers/specs/2026-07-21-atx-vol-backtest-framework-design.md`](../specs/2026-07-21-atx-vol-backtest-framework-design.md) §3 (M1), §4.4 (module), §6 (invariants I1–I8); grounding review [`docs/superpowers/specs/2026-07-21-atx-vol-backtest-review.md`](../specs/2026-07-21-atx-vol-backtest-review.md) M1/M6/M7/M8/M9/L9/L8.

**Tech Stack:** C++20 (MSVC, Release preset `build-rel`, AVX2), `atx::vol` library; reuses the existing listed primitives (`select_listed_dispersion`, `build_listed_dispersion_roll`, `reconcile_listed_dispersion`, `validate_listed_dispersion_schedule`, `build_dispersion_book`, `PreparedHistoricalProjection`); RunArchive/RunDir from Wave A (`run_archive.hpp`); Python 3 + numpy (binding-free reader `runarchive.py`); CMake; gtest target `atx-vol-tests`; pytest under `atx-vol/python`.

## Global Constraints

- Work directly on local `main`, in place. Explicit-path `git add` only — **never** `git add -A/-u/.` (the tree carries unrelated uncommitted work: surface-db, sha256, kb/db dirs).
- ONE build at a time. Release preset only (`cmake --build C:\atx\build-rel --target atx-vol-tests`). Shared deps at `C:\atx-cache\deps`. `parquet.dll` needs `C:\atx\build-rel\bin` on PATH to run examples/tests.
- Do NOT modify golden fixtures (`atx-vol/python/tests/data/runarchive/wave_a_fixture.atxrun`, sha256 `71ea9632…29f7424`) or the pinned schema hash `0xdcce47781ac8390d`. Do NOT touch `C:\atx-data` run dirs (controller owns them; subagents use the 3-session fixture copy recipe from `dispersion-parity/task-9-report.md`, NEVER modify `scratchpad\paired`). Never read `C:\atx\.env`.
- **Schema-format freeze.** The RunArchive registry (`run_archive_schema.hpp`), the on-disk structs (`run_archive.hpp`), and `_schema.py` are FROZEN. Any change to a section/column set = a new golden fixture + a `kRaMinor` bump + a regenerated `_schema.py`. **Wave B must avoid a schema bump** — every task here is either non-format (economics/CLI) or a *dedup that leaves the emitted bytes and `schema_hash` bit-identical* (T6). If a task appears to need a registry change, STOP and escalate to the controller.
- On-disk structs are an ABI: `static_assert` on `sizeof` AND `offsetof`. Host little-endian LP64 only. All binary output little-endian; all text `\n`, `%.17g` round-trip doubles.
- Windows/PowerShell: `$ErrorActionPreference='Continue'` (native stderr wraps in ErrorRecords); use `git commit -F <file>` or a bash heredoc for multi-line messages (here-strings mangle).
- Commit trailer on every commit: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- **Known pre-existing RED tests — do NOT chase or "fix"** (documented in `.superpowers/sdd/backtest-wave-a/t10-failure-triage.md`; zero file/include intersection with Wave B): `BoundaryHoist.PriceBitIdenticalToPrechange` (1-ULP SSE2 golden-pin variance), `SurfaceV2Qualification.RiskBuild…Budgets/Latency` and `/Balanced` (on-main `e7d5ebb` perf commit vs unmerged test re-pin on `feat/pipeline-m`). A green Wave B = all-green **modulo these three**.

---

## File Structure

**New library module** (register `src/listed_dispersion_pipeline.cpp` in `atx-vol/CMakeLists.txt` immediately after `src/listed_dispersion_strategy.cpp`, line ~108):
- `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp` — the listed-route economics + methodology + constants.
- `atx-vol/src/listed_dispersion_pipeline.cpp` — implementations lifted from the example.

**New test** (register in `atx-vol/tests/CMakeLists.txt` `add_executable(atx-vol-tests ...)`, near line 101 beside the other `listed_dispersion_*_test.cpp`):
- `atx-vol/tests/listed_dispersion_pipeline_test.cpp` — reuses the synthetic-surface scaffolding pattern from `listed_dispersion_reconciliation_test.cpp` (`make_surface`/`surfaces`/`pointers`/`SurfaceSet::create`).

**New optional header (T7 split, if the controller approves #8):**
- `atx-vol/include/atx/vol/backtest_series_columns.hpp` — the single-source `{name, BacktestResult member-ptr}` binding table shared by `tearsheet.cpp` and `run_archive.cpp` (T6).

**Modified:**
- `atx-vol/src/listed_dispersion_reconciliation.cpp` — (T2 only, if the controller picks the "keep hard-require, trim at assembler" remedy this file is UNCHANGED; the trim lives in the new module).
- `atx-vol/src/tearsheet.cpp`, `atx-vol/src/run_archive.cpp` — (T6) dedup the 25-double list to one source; (T7) `RunDir::write_run_archive` deterministic `created_ts_ns` + verify negative-path.
- `atx-vol/examples/spy_dispersion_backtest.cpp` — (T9) thin cutover: each `*_command` becomes a library call.
- `atx-vol/python/src/atxvol/report/runarchive.py`, `atx-vol/python/tests/test_runarchive.py` — (T8) reader hardening + negative tests.
- `atx-vol/CMakeLists.txt`, `atx-vol/tests/CMakeLists.txt` — module + test registration.

**Deliberately NOT touched:** the RunArchive on-disk format, `run_archive_schema.hpp`, `_schema.py`, the committed golden fixture, `dispersion_workflow`'s SPY hardcode (Wave D / L12), the `StepObserver` engine hook (Wave D / L10), the `backtest_driver` spine (Wave C), perf passes (Wave E).

---

## Task 1: `listed_dispersion_pipeline` module foundation — methodology policy, vega/parity constants, per-date adapter seams (M9, L9, L8)

**Files:**
- Create: `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp`, `atx-vol/src/listed_dispersion_pipeline.cpp`
- Modify: `atx-vol/CMakeLists.txt` (register the `.cpp`)
- Create + register: `atx-vol/tests/listed_dispersion_pipeline_test.cpp` (`atx-vol/tests/CMakeLists.txt`)

**Interfaces (Produces):**
- `inline constexpr double kVegaVolPointToUnitVol = 100.0;` — the per-vol-point → per-unit-vol factor (M9/I4). Replaces the literal `* 100.0` at `spy_dispersion_backtest.cpp:1032` and `:1110`.
- `struct ListedDispersionMethodology { CorpusAdmissionRule admission; /* fit template knobs */ std::size_t min_names_entry{51}; std::size_t core_min_dates{60}; std::size_t core_min_rolls{3}; std::uint32_t core_min_names_per_roll{40}; QueryExecution query_route{QueryExecution::ColdReference}; bool occ_ess_authority{true}; std::uint64_t policy_fingerprint() const; };` — one versioned policy replacing the loose inline literals scattered across `build-corpus`/`build-schedule`/`verify`/`run-projected-backtest` (L9). Thresholds pinned to the current values (`51/60/3/40`).
- `Result<std::vector<ListedOptionQuote>> listed_quotes_for_date(const RunSpec& spec, const ListedDefinitionTable& definitions, std::span<const std::string> symbols, std::string_view date);` — verbatim lift of the example `load_listed_quotes` (`:401-425`), including the `MissingDefinitionPolicy::SkipUnlisted` comment/behavior.
- `ListedForwardLookup make_listed_forward_lookup(const MarketSnapshot& snapshot);` — lift of the forward closure (`:480-491`).
- `ListedRiskLookup make_listed_risk_lookup(const MarketSnapshot& snapshot, double residual_T, bool analytic, QueryExecution execution);` — lift of the cold-risk closure (`:733-742`).

**Notes:** Header-light — include only what the signatures need (`dispersion_workflow.hpp`, `listed_opra.hpp`, `listed_dispersion_schedule.hpp`, `backtest.hpp` for `MarketSnapshot`/`QueryExecution`). `listed_quotes_for_date` needs live OPRA parquet, so it is NOT unit-tested here — its correctness is pinned by the T10 fixture gate; the T1 tests cover the pure/synthetic-surface seams.

- [x] **Step 1: Write the failing test** — in `listed_dispersion_pipeline_test.cpp`: (a) `static_assert(kVegaVolPointToUnitVol == 100.0)` + a runtime `EXPECT_EQ`; (b) `ListedDispersionMethodology{}.policy_fingerprint()` is nonzero and stable across two calls, and two methodologies differing in one threshold produce different fingerprints; (c) `make_listed_forward_lookup(snapshot)` over a synthetic `MarketSnapshot` (build a `SurfaceSet` from `make_surface`, mirror `listed_dispersion_reconciliation_test.cpp:35-76`) returns a finite positive forward; `make_listed_risk_lookup(...)` returns a finite `ListedOptionRisk`.
- [x] **Step 2: Run test to verify it fails** — `cmake --build C:\atx\build-rel --target atx-vol-tests` → expected FAIL: `listed_dispersion_pipeline.hpp` not found.
- [x] **Step 3: Implement** the header + `.cpp`; register the `.cpp` in `atx-vol/CMakeLists.txt` and the test in `atx-vol/tests/CMakeLists.txt`. Lift the three helpers verbatim (preserve comments); define the constant + methodology.
- [x] **Step 4: Run test to verify it passes** — build + `C:\atx\build-rel\bin\atx-vol-tests.exe --gtest_filter=ListedDispersionPipeline.*` → PASS.
- [x] **Step 5: Commit** (`git add atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp atx-vol/src/listed_dispersion_pipeline.cpp atx-vol/CMakeLists.txt atx-vol/tests/listed_dispersion_pipeline_test.cpp atx-vol/tests/CMakeLists.txt`).

---

## Task 2: M1 fix — reconciliation timeline-trim seam (RED regression first)

**Files:**
- Modify: `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp`, `atx-vol/src/listed_dispersion_pipeline.cpp`
- Test: `atx-vol/tests/listed_dispersion_pipeline_test.cpp`

**Interfaces (Produces):**
- `Result<std::vector<ListedReconciliationSnapshot>> assemble_reconciliation_snapshots(std::span<const ListedReconciliationSnapshot> full_timeline, const ListedDispersionSchedule& schedule);` — trims the leading pre-roll sessions so the returned span starts at `schedule.rolls.front().roll_date` (mirrors `ListedDispersionStrategy::on_step` emitting zero rows pre-roll). Errors if the first roll date is absent from the timeline (the coupling made explicit, not emergent).
- `Result<ListedDispersionReconciliation> reconcile_listed_schedule(const ListedDispersionSchedule& schedule, std::span<const ListedReconciliationSnapshot> full_timeline, const ListedReconciliationConfig& config = {});` — assembles (trims) then calls `reconcile_listed_dispersion`. The single reconciliation entry point the CLI uses.

**The defect (M1):** `reconcile_listed_dispersion` (`src/listed_dispersion_reconciliation.cpp:240-243`) hard-requires `snapshots.front().date == schedule.rolls.front().roll_date`. The example (`spy_dispersion_backtest.cpp:627-641`) feeds it the FULL `clock.refs()` timeline, so any leading warm-up/low-coverage session (front date < first roll date) aborts an otherwise-valid corpus. Today it only works because `date_lo` coincides with the first roll. **Fix at the new seam** (leave the low-level precondition intact as a defensive invariant — see open question 3): trim in `assemble_reconciliation_snapshots`.

- [x] **Step 1: Write the failing (RED) test** — `ReconcileClockCoupling_AbortsOnWarmupLeadIn`: build a valid one-roll `ListedDispersionSchedule` (via `build_listed_dispersion_roll` over synthetic surfaces, per the schedule-test pattern) whose `rolls.front().roll_date` is day-1; construct a strictly-ordered `ListedReconciliationSnapshot` vector `[{date=day0, surfaces=&set0, ...}, {date=day1,...}, {date=day2,...}]` (day0 < roll_date). Assert `reconcile_listed_dispersion(schedule, full)` returns `Err` with code `InvalidArgument` (documents the defect — the front-date check at `:240` fires before any pricing, so no surfaces are dereferenced on this path). This RED test is the anchor; it stays green forever as the low-level precondition.
- [x] **Step 2: Run to verify it fails** — the *new seam* does not exist yet, so also add `ReconcileListedSchedule_TrimsWarmupLeadIn` (the GREEN target): `reconcile_listed_schedule(schedule, full)` should succeed and equal `reconcile_listed_dispersion(schedule, full.subspan(first_roll_index))`. Build → FAIL: `reconcile_listed_schedule` undefined. (The RED anchor from Step 1 passes immediately — that is expected; it PROVES the defect exists.)
- [x] **Step 3: Implement** `assemble_reconciliation_snapshots` (find the first index with `date == rolls.front().roll_date`; error if none; return the trimmed copy) + `reconcile_listed_schedule` (assemble → reconcile). No change to `listed_dispersion_reconciliation.cpp`.
- [x] **Step 4: Run to verify it passes** — build + `--gtest_filter=ListedDispersionPipeline.*:ReconcileListedSchedule*:ReconcileClockCoupling*` → PASS (trim seam succeeds on warm-up; equality with the manually-trimmed reconcile holds).
- [x] **Step 5: Commit** (`git add atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp atx-vol/src/listed_dispersion_pipeline.cpp atx-vol/tests/listed_dispersion_pipeline_test.cpp`).

---

## Task 3: `build_listed_dispersion_schedule` extraction + M1 schedule-build enforcement (M7)

**Files:**
- Modify: `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp`, `atx-vol/src/listed_dispersion_pipeline.cpp`
- Test: `atx-vol/tests/listed_dispersion_pipeline_test.cpp`

**Interfaces (Produces):**
- `struct ListedScheduleSpec { double target_dte_days; double min_dte_days; double max_dte_days; double roll_dte_days; std::size_t min_names; double min_weight_coverage; double gross_index_vega; bool core_mode; };` — the swept knobs pulled from `RunSpec` (POD, so the builder does not depend on `RunSpec` layout).
- `Result<ListedDispersionSchedule> build_listed_dispersion_schedule(const Clock& clock, const ListedScheduleSpec& spec, const ListedDispersionMethodology& method, std::span<const UniverseRow> universe_rows, const ListedDefinitionTable& definitions, const RunSpec& quote_source);` — verbatim lift of `build_schedule_command`'s selection loop (`spy_dispersion_backtest.cpp:446-535`): DTE roll-trigger, per-date universe rebind, forward-lookup (via `make_listed_forward_lookup`), coverage-acceptance gate, deferral, cohort numbering, `surface_fingerprint` via `hash_file`, entry/three-roll acceptance gate. **M1 enforcement:** validate that `clock` contains `rolls.front().roll_date` and expose it; the coupling is enforced here, not left emergent.

**Notes:** the full build needs live OPRA + surfaces, so its economic output is pinned at T10 (byte-identical `trade_schedule` golden `b640b3ab…`). The T3 unit tests cover the *pure* acceptance logic that does not need parquet.

- [x] **Step 1: Write the failing test** — `BuildSchedule_RejectsEmptyAndSubThreshold`: drive the acceptance gate directly — an empty roll set → `Err(Unavailable, "…entry/three-roll acceptance gate")`; a core-mode run with < `core_min_rolls` rolls → `Err`. (Construct via a minimal synthetic `quote_source`/clock stub or, if parquet is unavoidable, assert the gate branch through a seam that takes a pre-built `std::vector<ListedScheduleRoll>` — factor the acceptance check into a testable `Status accept_listed_schedule(const ListedDispersionSchedule&, const ListedScheduleSpec&, const ListedDispersionMethodology&)` helper.)
- [x] **Step 2: Run to verify it fails** — build → FAIL: `build_listed_dispersion_schedule` / `accept_listed_schedule` undefined.
- [x] **Step 3: Implement** the builder + the extracted acceptance helper; enforce the M1 clock/first-roll coupling at build.
- [x] **Step 4: Run to verify it passes** — build + filter → PASS.
- [x] **Step 5: Commit** (explicit paths: module hpp/cpp + test).

---

## Task 4: `project_listed_schedule` extraction + I1 two-route cold parity (M6, I1)

**Files:**
- Modify: `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp`, `atx-vol/src/listed_dispersion_pipeline.cpp`
- Test: `atx-vol/tests/listed_dispersion_pipeline_test.cpp`

**Interfaces (Produces):**
- `using ListedArchiveLookup = std::function<Result<const MarketSnapshot*>(std::string_view roll_date)>;` — the per-roll snapshot provider (replaces the example's `archive_of` map + inline `MarketSnapshot::load`).
- `struct ProjectionConfig { bool analytic{true}; QueryExecution execution{QueryExecution::ColdReference}; };` — the cold idealization knobs. **The single asserted parity constant** (I1): a `static_assert`/named constant that the projected-backtest replay ALSO reads.
- `Result<ListedDispersionSchedule> project_listed_schedule(const ListedDispersionSchedule& listed, const ListedArchiveLookup& archives, const ProjectionConfig& cfg);` — verbatim lift of `project_schedule_command`'s cold reprice (`spy_dispersion_backtest.cpp:706-822`): residual-T, structural guards, `make_listed_risk_lookup` cold seed, `make_straddle`/`make_quote` re-striking to `surface->forward_at(residual_T)`, `build_listed_dispersion_roll` sizing, `validate_listed_dispersion_schedule`.

**I1 (the headline gate):** `project_listed_schedule` and `run-projected-backtest --execution cold` must share ONE code path / ONE asserted constant (`analytic=true` + `QueryExecution::ColdReference`), so the persisted `projected_schedule` marks equal the live cold seed marks the replay recomputes — guarded at compile time, not by luck.

- [x] **Step 1: Write the failing test** — `TwoRouteColdParity_LegMarksEqual`: over synthetic surfaces, build a one-roll listed schedule; run `project_listed_schedule(listed, archives, {analytic:true, execution:ColdReference})`; independently compute each leg's cold seed mark through the SAME `make_listed_risk_lookup`/`full_greek_seed(..., analytic=true, ColdReference)` the projected-backtest replay uses; assert per-leg `model_mark` bit-equality (`EXPECT_EQ` on the raw doubles). Add `ProjectionConfigColdIsCanonical` asserting `ProjectionConfig{}.analytic == true && ProjectionConfig{}.execution == QueryExecution::ColdReference`.
- [x] **Step 2: Run to verify it fails** — build → FAIL: `project_listed_schedule` undefined.
- [x] **Step 3: Implement** `project_listed_schedule` + `ListedArchiveLookup` + `ProjectionConfig`, lifting the cold reprice; the projected-backtest replay (wired in T9) reads the same `ProjectionConfig` constant.
- [x] **Step 4: Run to verify it passes** — build + filter → PASS (bit-exact leg-mark parity).
- [x] **Step 5: Commit** (module hpp/cpp + test).

---

## Task 5: `dispersion_book_var` extraction (M8)

**Files:**
- Modify: `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp`, `atx-vol/src/listed_dispersion_pipeline.cpp`
- Test: `atx-vol/tests/listed_dispersion_pipeline_test.cpp`

**Interfaces (Produces):**
- `struct DispersionBookVar { std::vector<HistoricalProjectionFrame> frames; std::vector<ProjectedOption> legs; std::vector<ProjectedHistoricalVar> risks; std::size_t n_positions; };`
- `Result<DispersionBookVar> dispersion_book_var(const DispersionBook& book, std::span<const HistoricalProjectionScenario> scenarios, std::span<const double> confidences, const HistoricalProjectionConfig& cfg);` — verbatim lift of `run_projected_var_command`'s book→`OptionProjectionSpec` synthesis + `PreparedHistoricalProjection::evaluate_into` + `projected_historical_var` for each confidence (`spy_dispersion_backtest.cpp:1119-1194`). Applies `kVegaVolPointToUnitVol` where the example applies `* 100.0` (`:1110`) — the constant lives in the builder of the `DispersionConfig`, so the CLI hands per-vol-point vega and the library scales once (M9).

**Notes:** the TSV emission stays in the CLI (three bespoke schemas are out-of-archive per the design partition rule — NOT folded into `run.atxrun` this wave, no schema bump). The library returns the frames/legs/risks; the CLI serializes.

- [x] **Step 1: Write the failing test** — `DispersionBookVar_SplitsConfidences`: build a small `DispersionBook` + synthetic scenarios; assert `dispersion_book_var(book, scenarios, {0.95, 0.99}, cfg)` returns two `risks` with `confidence` 0.95/0.99, `n_positions == book.positions.size()`, and every frame `n_failed == 0`. (Synthetic surfaces; small position count.)
- [x] **Step 2: Run to verify it fails** — build → FAIL: `dispersion_book_var` undefined.
- [x] **Step 3: Implement** the lift; route the ×100 through `kVegaVolPointToUnitVol`.
- [x] **Step 4: Run to verify it passes** — build + filter → PASS.
- [x] **Step 5: Commit** (module hpp/cpp + test).

---

## Task 6: 25-double backtest column single-source-of-truth (deferred Minor #6) — bytes MUST stay identical

**Files:**
- Create: `atx-vol/include/atx/vol/backtest_series_columns.hpp`
- Modify: `atx-vol/src/tearsheet.cpp` (`append_backtest_series_tsv`, `:190-216`), `atx-vol/src/run_archive.cpp` (`encode_backtest_section`, `dbl_cols` at `:590-616`)
- Test: `atx-vol/tests/run_archive_test.cpp` (or `tearsheet_test.cpp`)

**Interfaces (Produces):**
- `struct BacktestSeriesColumn { std::string_view name; const std::vector<double> BacktestResult::* member; };`
- `std::span<const BacktestSeriesColumn> backtest_series_columns();` — the ONE ordered `{name, member-ptr}` table (25 entries, `pnl_total … n_unpriced_greeks`) that BOTH `append_backtest_series_tsv` and `encode_backtest_section` iterate, replacing their two hand-kept `dbl_cols[]` arrays.

**Freeze guard (critical):** this is a pure dedup. The emitted TSV bytes, the encoder's column order, and `ra_schema_hash()` (`0xdcce…`) must be **bit-identical** afterward. The registry (`run_archive_schema.hpp`) and `_schema.py` are NOT touched. A `static_assert`/test pins `backtest_series_columns()`'s names to the registry `backtest` columns[2..26] in order, so the shared table can never drift from the frozen registry.

- [x] **Step 1: Write the failing test** — `BacktestSeriesColumns_MatchRegistryOrder`: assert `backtest_series_columns()` has 25 entries whose `name`s equal `ra_sections()`'s `backtest` section columns index 2..26 in order (skipping `date`,`ts_ns`); and a round-trip test that `encode_backtest_section` output over a 2-row `BacktestResult` is column-for-column value-equal to `append_backtest_series_tsv` (already covered by existing tests — extend to assert every one of the 25 columns, closing Minor #17's "one value per dtype-class" gap for this section).
- [x] **Step 2: Run to verify it fails** — build → FAIL: `backtest_series_columns` undefined.
- [x] **Step 3: Implement** the shared header; rewrite both `dbl_cols[]` sites to iterate it. Verify NO change to `schema_hash` and NO edit to `run_archive_schema.hpp`/`_schema.py`.
- [x] **Step 4: Run to verify it passes** — build + run full `RunArchive*`/`Tearsheet*` filters → PASS; the committed fixture `MatchesCommittedPythonFixture` still byte-identical (proves bytes unchanged).
- [x] **Step 5: Commit** (`git add atx-vol/include/atx/vol/backtest_series_columns.hpp atx-vol/src/tearsheet.cpp atx-vol/src/run_archive.cpp atx-vol/tests/run_archive_test.cpp`).

---

## Task 7: `created_ts_ns` determinism policy (Minor #7) + verify negative count-gate test (Minor #9)

**Files:**
- Modify: `atx-vol/include/atx/vol/run_archive.hpp`, `atx-vol/src/run_archive.cpp` (`RunDir::write_run_archive`, `RunDir::verify`)
- Test: `atx-vol/tests/run_archive_test.cpp`

**Interfaces (Produces / changes):**
- `RunDir::write_run_archive` stamps a **deterministic** `created_ts_ns` derived from the run's identity (e.g. `static_cast<std::int64_t>(run_identity_hash())`) instead of `0` (which the writer fills from the system clock, making `run.atxrun` bytes + `header_crc32c` + `ArchiveContentIdentity` vary run-to-run). Two writes with identical inputs → **byte-identical** `run.atxrun`. This also makes the Wave-E P1 cache key stable via `ArchiveContentIdentity` (see open question 1 — confirm the derivation).
- No new public API for the count gate — just a regression test for the existing gate.

**Freeze guard:** the committed golden fixture is written by the T5-era test helper with its own fixed `created_ts_ns` — this task does NOT touch the writer's `created_ts_ns==0 ⇒ system-clock` semantics, only what `RunDir` PASSES. Fixture bytes unaffected. The economic golden dumps (`backtest`/`projected_cold` section TSVs) do not include `created_ts_ns`, so the 135-session hashes are unaffected.

- [x] **Step 1: Write the failing tests** — `RunDir_WriteIsByteDeterministic`: write the same sections to two temp run dirs with identical inputs; assert the two `run.atxrun` files are byte-identical. `RunDir_VerifyRejectsCountGateMismatch`: build a `run.atxrun` whose `backtest` and `reconciliation` sections have unequal `n_rows`; assert `RunDir::verify()` returns `Err(InvalidArgument)` (the cardinality cross-check).
- [x] **Step 2: Run to verify they fail** — build → FAIL (nondeterministic bytes today; no count-gate negative coverage).
- [x] **Step 3: Implement** the deterministic `created_ts_ns` in `RunDir::write_run_archive`; add the count-gate negative test (the gate already exists in `verify` — this is coverage, so if it already passes, keep it as a locked regression).
- [x] **Step 4: Run to verify they pass** — build + `RunDir*` filter → PASS; `MatchesCommittedPythonFixture` still byte-identical.
- [x] **Step 5: Commit** (`git add atx-vol/include/atx/vol/run_archive.hpp atx-vol/src/run_archive.cpp atx-vol/tests/run_archive_test.cpp`).

---

## Task 8: Python reader hardening (Minors #10, #11, #12, #13) — PYTHON-ONLY, no C++ build

**Files:**
- Modify: `atx-vol/python/src/atxvol/report/runarchive.py`
- Test: `atx-vol/python/tests/test_runarchive.py`

**Interfaces (changes):**
- **#10** Negative `section()`-framing tests: byte-patch a valid archive to produce (a) a bad section-record magic, (b) a dict/enum code out of table range, (c) a non-monotone aux offset table; assert each raises `ValueError` at `section()` (parity with the C++ hostile-archive branches).
- **#11** Version-mismatch test: patch header `major` to 2 (and `minor` to 1) with CRCs recomputed; assert `open()` raises `ValueError`.
- **#12** `RunArchive.close()` (`runarchive.py:534-543`) must not leak `_fh` on `BufferError` (outstanding numpy views) — close `_fh` in a `finally`/best-effort path; test that `close()` with a live view does not leave the handle dangling (or documents the retry).
- **#13** Forged non-utf8 string table currently raises `UnicodeDecodeError` — wrap `_string_table`'s `.decode` (`:287`) so corruption raises the documented `ValueError` instead.

- [x] **Step 1: Write the failing tests** — add the four cases above to `test_runarchive.py`, each starting from a valid fixture-derived buffer and patching bytes (CRCs recomputed where `open()` would otherwise reject earlier). RED because the reader raises the wrong exception type / leaks the handle.
- [x] **Step 2: Run to verify they fail** — `pytest atx-vol/python/tests/test_runarchive.py -v` → FAIL (wrong exception / leak).
- [x] **Step 3: Implement** the `ValueError` wrapping for section-framing + version + non-utf8; fix `close()`.
- [x] **Step 4: Run to verify they pass** — `pytest atx-vol/python` → PASS (all prior tests still green).
- [x] **Step 5: Commit** (`git add atx-vol/python/src/atxvol/report/runarchive.py atx-vol/python/tests/test_runarchive.py`).

---

## Task 9: Thin-CLI cutover — every subcommand becomes one library call (M1 wired; per-track archive constraint documented)

**Files:**
- Modify: `atx-vol/examples/spy_dispersion_backtest.cpp`
- Test: `atx-vol/python/tests/test_dispersion_runarchive_e2e.py` (extend) — the 3-session fixture end-to-end

**Interfaces (Consumes):** all of Tasks 1–5 (+ T6/T7 already landed).

Replace, per subcommand, the inline economics with the library seams — preserving process-boundary independence (I8) and every existing `run.atxrun` section write / merge-write behavior:
- `build_schedule_command` → `build_listed_dispersion_schedule(clock, spec_knobs, method, universe_rows, definitions, spec)`; keep the `trade_schedule.tsv` + `encode_schedule_section` + diagnostics writes.
- `run_backtest_command` → keep the strategy/engine run; replace the inline reconciliation-snapshot assembly (`:621-643`) with `reconcile_listed_schedule(schedule, full_timeline)` — **this wires the M1 trim into production**, so a warm-up lead-in no longer aborts.
- `project_schedule_command` → `project_listed_schedule(listed, archive_lookup, ProjectionConfig{})`; keep the schedule-section + diagnostics writes.
- `run_projected_backtest_command` → the cold replay reads the **same** `ProjectionConfig` constant as `project_listed_schedule` (I1 shared path).
- `run_projected_var_command` → `dispersion_book_var(book, scenarios, {0.95,0.99}, cfg)`; the CLI keeps the three bespoke TSV emissions (out-of-archive per the partition rule).
- `run_surface_backtest_command` (`:1032`) + `run_projected_var_command` (`:1110`) → replace `* 100.0` with `kVegaVolPointToUnitVol`.
- Replace loose methodology literals (`min_names 51`, core `60/3/40`) with `ListedDispersionMethodology` reads (L9).
- **Document the per-track archive constraint** (F1 remainder): a header comment + one line in the plan/ledger stating that route-scoped `meta`/`diagnostics` section names would require a `kRaMinor` schema bump + new golden, so Wave B keeps the Wave-A **merge-write** (identity-hash-guarded union) as the shared-dir mechanism and does NOT introduce per-track section names. No format change this wave.

- [x] **Step 1: Write the failing test** — extend the 3-session fixture e2e (`test_dispersion_runarchive_e2e.py`, fixture recipe from `dispersion-parity/task-9-report.md`; NEVER modify `scratchpad\paired`): run `build-schedule → run-backtest → project-schedule → run-projected-backtest --execution cold` and assert `run.atxrun` reproduces the known 3-session economics `final_nav=-456.5769067` (dates=3, rolls=1) and that the listed + projected sections coexist (merge-write union). RED because the example still has inline economics (or the test asserts a seam the CLI does not yet call).
- [x] **Step 2: Run to verify it fails** — build the example (`scratchpad\build_example.bat` or the `atxvol_spy_dispersion_backtest` target); run the fixture pipeline → FAIL until cutover.
- [x] **Step 3: Implement** the cutover; delete the now-duplicated inline economics from the example (they live in the module).
- [x] **Step 4: Run to verify it passes** — rebuild example; run the 3-session fixture pipeline; `pytest atx-vol/python` → PASS; economics byte-identical (`final_nav=-456.5769067`).
- [x] **Step 5: Commit** (`git add atx-vol/examples/spy_dispersion_backtest.cpp atx-vol/python/tests/test_dispersion_runarchive_e2e.py`).

---

## Task 10: Wave-B integration gate (controller) + docs

**Files:** Modify this plan (check boxes); update `.superpowers/sdd/backtest-wave-b/progress.md` (controller-owned ledger).

- [x] **Step 1:** Full Release build of `atx-vol` targets + `atx-vol-tests`; run `atx-vol-tests.exe` from `C:\atx\build-rel` CWD (all green **modulo** the three documented pre-existing reds: `BoundaryHoist.PriceBitIdenticalToPrechange`, `SurfaceV2Qualification…/Latency`, `…/Balanced`); run `pytest atx-vol/python` (all green incl. the new reader-hardening + e2e tests).
- [x] **Step 2:** Controller runs the **parity-full (135-session)** pipeline end-to-end on the idle box (`C:\atx-data`, controller-only): confirm economics UNCHANGED — listed `final_nav=125026.0592`, projected-cold `final_nav=123243.1172`, `dates=135`, `rolls=7`, daily-pnl `corr=0.99718`, `mark_divergence` rows=0. Confirm the golden dump hashes still match: `dump backtest --tsv == a05470c7…`, `projected_cold == cbabca44…`, `trade_schedule == b640b3ab…`, `projected_schedule == d6793d46…`. Confirm `run.atxrun` opens + `validate_all()` passes + the Python reader renders the report.
- [x] **Step 3:** Confirm the **M1 fix is exercised in production** — the parity-full run's clock does not abort on any leading warm-up/low-coverage session (previously masked because `date_lo` == first roll). Confirm the merge-write union still holds (listed + projected sections coexist in the shared parity-full dir). No new loose result TSVs; no schema bump (`kRaMinor` still 0; `schema_hash` still `0xdcce…`).
- [x] **Step 4: Commit** the ledger + checked plan.

---

## Batching (for parallel Opus subagents)

**Genuine parallel lanes** are limited by the ONE-build-slot rule: all C++ tasks serialize on `build-rel`. Only Python (T8) and read-only reviewers run truly concurrently.

- **File-disjoint groups** (authorable concurrently; builds still serialize):
  1. **Pipeline module** `listed_dispersion_pipeline.{hpp,cpp}` + its test — **T1 → T2 → T3 → T4 → T5** are same-file, so they run **strictly sequential** on the build slot, in this order (T2/M1 early per design).
  2. **Result-store** `run_archive.cpp`/`tearsheet.cpp` — **T6, T7** (file-disjoint from group 1; sequential w.r.t. each other since both touch `run_archive.cpp`).
  3. **Python** — **T8** (no C++ build) → runs **parallel** to any group-1/group-2 C++ task.
  4. **Example** — **T9** depends on groups 1–2 landing; build slot; near-last.
- **Proposed schedule:**
  - Batch 1: **T1** (build slot) ∥ **T8** (python-only).
  - Batch 2: **T2** (build slot) ∥ T8 review (read-only).
  - Batch 3: **T3** → Batch 4: **T4** → Batch 5: **T5** (each build slot; reviewers read-only in parallel).
  - Batch 6: **T6** → **T7** (build slot; file-disjoint from the pipeline module, so may be authored during 3–5 but built after).
  - Batch 7: **T9** (build slot, depends on T1–T5).
  - **T10** = controller gate (owns `C:\atx-data`; never a subagent).
- **Reviewers** (Opus/Fable, read-only) shadow each task via the per-commit diff, as in Wave A.

---

## Self-Review

**Spec coverage (Wave B slice of design §3/§4.4/§6):**
- `listed_dispersion_pipeline` module (§4.4) → T1–T5. ✓
- M1 reconciliation clock-coupling fix + RED regression (§3) → T2 (RED anchor + trim seam), wired in production T9, exercised T10. ✓
- I1 two-route cold parity (§6) → T4 (shared `ProjectionConfig` constant + bit-exact leg-mark test). ✓
- M9 `kVegaVolPointToUnitVol` (§4.4, I4) → T1 constant, applied T5/T9. ✓
- L9 `ListedDispersionMethodology` → T1, consumed T3/T9. ✓
- M7 `build_listed_dispersion_schedule` → T3. M6 `project_listed_schedule` → T4. M8 `dispersion_book_var` → T5. L8 adapter seams → T1. ✓
- Thin CLI / I8 process-boundary independence → T9. ✓
- **Deferred Wave-A minors folded:** #6 (25-double single-source) → T6; #7 (created_ts_ns determinism) + #9 (verify count-gate negative) → T7; #10/#11/#12/#13 (python reader hardening) → T8; #17 (encoder per-column value assert) → T6. ✓
- **Per-track archive (F1 remainder)** → documented constraint in T9 (no schema bump this wave). ✓
- **Economics preservation** (value-preserving refactor) → T9 3-session gate + T10 135-session golden-hash gate. ✓
- **Correctly OUT of Wave B:** StepObserver (Wave D/L10), de-SPY `dispersion_workflow` (Wave D/L12), `backtest_driver` spine (Wave C/L11), perf P1–P7 (Wave E), the three known pre-existing reds, DROPped minors #19–26. Noted, not gaps.

**Freeze integrity:** T6/T7 are the only tasks near the format; both are pure dedup / caller-side determinism that leave emitted bytes + `schema_hash` bit-identical (asserted against the committed golden fixture). No registry/`_schema.py`/struct edit anywhere in Wave B → no `kRaMinor` bump.

**Placeholder scan:** every seam names a real lift site (`spy_dispersion_backtest.cpp:401-425/446-535/706-822/1119-1194/1032/1110`) and a real existing primitive (`reconcile_listed_dispersion:240-243`, `build_listed_dispersion_roll`, `select_listed_dispersion`, `PreparedHistoricalProjection`). No "TBD/similar-to". Test names are concrete. ✓

## Controller decisions (pre-dispatch, 2026-07-23)

1. **T7 `created_ts_ns`**: derive deterministically from `run_identity_hash`
   (content-derived pseudo-timestamp; document the field as NOT wall clock; no
   schema bump; wall-clock provenance = run-dir file mtimes only). Stabilizes
   future Wave-E cache keys and enables byte-identity assertions.
2. **T2 M1 location**: trim at `assemble_reconciliation_snapshots`; keep the
   low-level `reconcile_listed_dispersion` hard-require as defensive invariant.
3. **`reference_reconciliation.tsv`**: stays dropped from `verify`
   (externally-produced optional comparison artifact, not a gate).
4. **No schema bump in Wave B** — confirmed; merge-write remains the shared-dir
   mechanism, per-track section naming deferred.
5. **Scope**: T5 (`dispersion_book_var`) stays in Wave B.
6. **T7 addition (Minor #16)**: run_archive writer still flush-not-fsync —
   commit 86f2210 fixed only the surface archive. Mirror its
   fsync-before-rename pattern in `write_run_archive_file` as part of T7.

---

## Gate outcome (controller, 2026-07-24)

Commit chain on `main`, base `6e3af60`:
`12a6e4c` T8 -> `4d12d96` T1 -> `bb0e744` T2 -> `f9fb4e2` T3 -> `17b1477` T4 ->
`c2e5463` T5 -> `b1cfd16` T6 -> `9a24e78` T7 -> `382fee2` T9.

All nine implementation tasks closed with both verdicts green (Spec compliance
✅ / Code quality Approved) and zero unresolved Critical or Important findings.
Per-task detail and the full Minor roll-up live in
`.superpowers/sdd/backtest-wave-b/progress.md`.

**Step 1** — full Release build; `atx-vol-tests.exe` from `C:\atx\build-rel` CWD:
2013 ran, **1967 passed / 43 skipped / 3 failed**, the three failures being
exactly the documented pre-existing reds. `pytest atx-vol/python`: 87 passed,
plus the 5 archive e2e tests passing on an isolated rerun (a concurrent-load
scheduling error by the controller — full gtest, the 135-session parity run and
the Python suite at once — had made the spawned example exe fail-fast with
`0xC0000409`; the exe is healthy standalone).

**Step 2** — parity-full (135-session) end-to-end rerun: economics **exact**.
Listed `final_nav=125026.0592`, projected-cold `final_nav=123243.1172`,
`dates=135`, `rolls=7`, daily-pnl `corr=0.99718`, `mark_divergence rows=0`.
4/4 golden hashes matched (`a05470c7…` backtest dump, `cbabca44…`
projected_cold, `b640b3ab…` trade_schedule, `d6793d46…` projected_schedule).
`validate_all()` passes; the Python reader renders the parity report (153986 B).

**Step 3** — **NOT satisfied; see the final review.** The parity-full clock ran
all 135 sessions without aborting, but that does **not** demonstrate the M1 fix:
in this corpus `date_lo` equals the first roll date, so the lead-in is zero and
the trimming path never executes. The claim that M1 is "exercised in production"
was an overstatement by the controller — a run with no lead-in cannot exercise a
lead-in fix. The final review then showed the fix does not work end-to-end at
all (Important #1 below), so this step is genuinely open, not merely unproven.
The rest of Step 3 does hold: the merge-write union holds — the shared parity-full dir carries **9** sections
(listed + projected coexisting, `projected_schedule` included because this run
also took step 3 under merge-write, vs Wave A's 8). No new loose result TSVs;
no schema bump (`kRaMinor` still 0, `schema_hash` still `0xdcce47781ac8390d`).

Determinism note: whole-file `run.atxrun` bytes differ across an identical rerun
because the `diagnostics` section carries wall-clock `wall_ms`. By design — the
economics sections are byte-stable (dump hashes exact across the rerun), so the
T7 guarantee (identical payloads -> identical bytes) is unaffected.

### Carried out of Wave B

1. **`run-projected-var` economics are unpinned.** ~~No golden was ever captured
   for that route.~~ **CLOSED 2026-07-24** — golden captured on parity-full
   (135 scenarios, 22 positions) and proven to reproduce across an immediate
   rerun: `projected_risk_scenarios.tsv` `0cf8ac4b50f34ea6`,
   `projected_risk_legs.tsv` `0a8b38984c7b6064`, and `projected_var.tsv` with
   field 7 excluded `d370c78dbb01b513`. That exclusion is required:
   `projections_per_second` is a wall-clock rate and moves every run (31800.9 ->
   32822.3 on back-to-back runs), so the whole-file hash is meaningless while the
   economics beside it are exact. Full detail and the pinned VaR/ES values are in
   the ledger.
2. **The Python test suite was restructured after the gate and is uncommitted.**
   The archive e2e module used to spawn the C++ dispersion pipeline six times to
   produce a `run.atxrun` it then merely read — the wrong layer, and the reason
   the suite took ~15 minutes. It now reads a committed 9-section archive fixture
   (`tests/data/runarchive/dispersion_paired.atxrun`); the two contracts only the
   CLI can demonstrate are kept behind a `slow` marker, deselected by default.
   Redundant work was also removed from `test_backtest.py` (6 backtests -> 4, and
   a smaller surface fixture), `test_parity.py` (one report build, not six) and
   `test_runarchive.py` (one shared mapping). **This work is unverified** — it has
   never been run green — and is deliberately left out of the Wave B commits.

### Final whole-branch review (fresh Opus reviewer, 6e3af60..382fee2)

Full report: `.superpowers/sdd/backtest-wave-b/final-review.md`.

**Spec compliance: ✅** — every task landed and the extractions are faithful
lifts with no economic delta (each moved block traced against the code it
replaced). **Code quality: Request changes** — 0 Critical, 3 Important, 7 Minor.
**Wave B is therefore NOT closed.**

**Important 1 — the M1 warm-up-lead-in fix does not work end-to-end.**
`assemble_reconciliation_snapshots` trims leading pre-roll sessions, so
`reconcile_listed_schedule` returns `clock.size() - lead_in` rows; the next call
in `run-backtest`, `validate_listed_reconciliation_backtest`
(`listed_dispersion_reconciliation.cpp:344-348`), hard-requires
`rows.size() == backtest.size()` while the backtest still spans every clock date.
The abort simply moves downstream, with a more misleading message
("invalid tolerance or row count"). `RunDir::verify` (`run_archive.cpp:1630`)
carries the same gate over the archive sections. The T2 test is green because it
exercises the seam in isolation and never reaches the validator. Controller
verified this independently against the code before accepting it.
*Fix:* make both gates date-aligned — require the reconciliation rows to be a
contiguous suffix of the backtest dates and compare pairwise from that offset,
which is behaviour-identical when the lead-in is zero.

**Important 2 — `ListedArchiveLookup`'s borrow contract changes the memory
profile.** The old loop freed each `MarketSnapshot` per roll; the borrowed-pointer
seam forces the caller to retain every roll-date board (a full heap deserialize,
not an mmap) for the whole call. Harmless at 7 rolls, ~120 boards resident on a
multi-year corpus.

**Important 3 — `ListedDispersionMethodology` is a third copy of the thresholds,
not the single authority its header claims.** `verify` still uses
`RunVerifyOptions`' independent 60/3/40 (`run_archive.hpp:566-568`) and the cold
route reads `ProjectionConfig`. Four of its seven fields are dead; the worst is
`query_route`, a `ColdReference` that nothing reads sitting beside the real
authority.

**Minor (7)** — the T6 `static_assert` cannot pin member bindings and the only
independent oracle uses `{0.0, 0.0}` for 23 of 25 fixture columns;
`BuildScheduleSymbolIsDeclared` cannot fail; `TwoRouteColdParity_LegMarksEqual`
is `f(x) == f(x)` (the real I1 gate is the Python e2e); a permanently-zero
`archive_load` diagnostics row; the projected-VaR failure gate moved ahead of the
diagnostic TSV writes (stale artifacts, lost evidence); an undocumented borrow in
`assemble_reconciliation_snapshots`' return; and the merge-write identity hash
covering only `run_spec.tsv` + `universe_schedule.tsv` (pre-existing at 191e409,
outside the diff).

Explicitly cleared, so they are not re-checked: `hash_archive_file` is
byte-identical (`read_text` was already binary-mode); the build-schedule loop and
the cold projection are line-for-line verbatim; every literal->policy
substitution is value-identical; `analytic_greeks = true` matches `RunConfig`'s
existing default (no economics change); the rename is unreachable unless
write+sync+close all succeeded; and `created_ts_ns` bits round-trip while
`ArchiveContentIdentity` still discriminates content.
