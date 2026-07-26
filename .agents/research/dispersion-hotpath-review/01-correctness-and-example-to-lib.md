# atx-vol SPY dispersion backtest — deep-dive review

Axis (1) CORRECTNESS of the dispersion backtest + Axis (5) example → library extraction.
Reviewer: senior quant, READ-ONLY, main HEAD. Repo `C:/atx`, module `atx-vol/`.
Scope read in full: `examples/spy_dispersion_backtest.cpp` (761 LOC, all 6 subcommands);
`src/dispersion.cpp`, `dispersion_workflow.cpp`, `dispersion_strategy.cpp`,
`dispersion_strangle.cpp`, `dispersion_backtest.cpp`, `listed_dispersion.cpp`,
`listed_dispersion_schedule.cpp`, `listed_dispersion_reconciliation.cpp`,
`listed_dispersion_strategy.cpp`, `backtest.cpp` (2076 LOC), `strategy.cpp` (lifecycle);
headers `dispersion.hpp`, `dispersion_workflow.hpp`, `dispersion_backtest.hpp`,
`backtest.hpp`, `strategy.hpp`, `listed_dispersion*.hpp`. CMake wiring confirmed
(`atx-vol/CMakeLists.txt:99-106` library sources; `:317` the example target).

Counts: Critical 0 · High 3 · Medium 5 · Low 4 · Positives (verified PASS) 4.

---

## POSITIVES (verified correct — do not "fix")

**P1 — Vega-flat neutralization is correct on BOTH paths.**
- Straddle book `dispersion.cpp:488-496`: `index.qty = ±V/(straddle_vega·mult)`;
  `name.qty = ∓(w_i/Σw_surv)·V/(straddle_vega·mult)`. Index gross vega `= V`; Σ name gross
  vega `= V·Σŵ = V`; opposite sign (`name_sign=-index_sign`). Renormalization is over the
  SURVIVORS (`sum_w`, `:479-486`), so a DropRenormalize drop preserves neutrality each
  rebalance. Put forced onto the call's exact K/expiry (`:193-199`) — one concrete straddle,
  not two independent ATM re-resolves. Implied-corr closed form + degenerate-denominator guard
  (`:381-388`) correct.
- Listed schedule `listed_dispersion_schedule.cpp:291-311`: same sign algebra with continuous
  (fractional) contract counts; `validate_roll` (`:248-261`) independently re-checks
  `|net_vega|/gross_index_target ≤ max_relative_residual` and `|Σŵ−1| ≤ tol` — a genuine
  post-build neutrality assertion, not just a comment.

**P2 — Look-ahead: CLEAN across all four backtest paths (see the LOOK-AHEAD VERDICT section).**

**P3 — PnL/greek reconciliation is a real independent cross-check.**
`validate_listed_reconciliation_backtest` (`listed_dispersion_reconciliation.cpp:341-364`)
recovers the pure option greek-explain MTM from the engine columns
(`pnl_total − settlement − shares − financing + cost`) and compares it, per date, to an
INDEPENDENTLY recomputed held-cohort mark-to-mark (`Σ qty·mult·(mark_t − mark_{t−1})`) at
`1e-8`. Roll-date cohort switchover is timed correctly (old cohort marked Held before the new
Entry cohort is seeded, `:285-330`), matching the engine (compute_step MTMs the pre-roll book
because on_step runs AFTER compute_step).

**P4 — Determinism.** `std::map`/`std::sort` keyed selection everywhere in the schedule/universe
path; pricer reduction is documented thread-invariant; `resolve_universe_uids` preserves input
order. One edge exception under M2.

---

## CORRECTNESS FINDINGS

### HIGH

**H1 — `run-backtest` hard-fails whenever `build-schedule` defers the first roll.**
`listed_dispersion_reconciliation.cpp:240-243` rejects any timeline whose first snapshot is not
the first roll date:
```
if (snapshots.front().date != schedule.rolls.front().roll_date)
    return Err(InvalidArgument, "first snapshot must be first entry date");
```
But `build_schedule_command` (`spy_dispersion_backtest.cpp:389-413`) legitimately DEFERS the
first roll when the earliest clock date fails selection or the `min_weight_coverage` gate
(`if (active_expiry == 0) continue;`). `run_backtest_command` (`:504-512`) then feeds reconcile
EVERY clock date starting at `clock.refs()[0]`. So a run whose first tradeable date is later than
`refs[0]` — exactly what the coverage gate is designed to allow — makes `run-backtest` abort at
the reconcile step even though `run_backtest()` itself handled the leading flat date fine
(`ListedDispersionStrategy::on_step` returns Ok/empty for `base.ts < roll.valuation_ts`,
`listed_dispersion_strategy.cpp:95-97`). Confirmed still present at HEAD (was pf2 follow-up #2,
deferred/unfixed).
Fix: teach `reconcile_listed_dispersion` to skip leading flat dates while keeping row-count
alignment (emit zero-PnL rows for pre-entry dates), OR gate `run_backtest_command` to start the
reconciliation timeline at `schedule.rolls.front().roll_date`. The backtest column
`validate_listed_reconciliation_backtest` compares row-for-row, so whichever path is chosen must
keep `reconciliation.rows.size() == backtest.size()`.

**H2 — Surface backtest and projected-VaR freeze the universe at the FIRST clock date (not
point-in-time).**
`run_surface_backtest_command` (`spy_dispersion_backtest.cpp:531`) and
`run_projected_var_command` (`:604`) both build ONE universe via
`universe_at(universe_rows, clock.refs().front().date)` and hold it for the entire run.
`DispersionStrategy` re-binds symbols→uids per snapshot and drops names absent on a date, but
membership and WEIGHTS are those of the earliest date for every later date. Any add/reweight in
the schedule after the first date is ignored, so a multi-month run trades a stale basket. This is
NOT look-ahead (first date is the oldest, so it is staleness, not future info), but it is a
point-in-time fidelity break and is INCONSISTENT with the listed path, which correctly calls
`universe_at(ref.date)` per roll (`:362`). For the throughput benchmark it is defensible; for a
research result it silently misstates exposures.
Fix: give `DispersionStrategy`/`run_dispersion_backtest` the full `UniverseSchedule` (rows) and
re-resolve `universe_at(base.date)` inside `on_step`, mirroring the listed path.

**H3 — A constituent can never LEAVE the basket (sticky membership).**
`universe_at` (`dispersion_workflow.cpp:232-245`) keeps, per symbol, the latest row with
`effective_date ≤ date`; `read_universe` (`:205`) rejects `raw_weight ≤ 0`. There is therefore
NO representation for "name removed at reconstitution" — a zero/negative weight (the natural
delisting encoding) is a parse error, and an absent name in a later effective set still lingers
from its older row. The basket only ever grows or reweights. For an index with real
reconstitution turnover the composition drifts from the true index over the run.
Fix: support an explicit removal/zero-weight sentinel row (`effective_date, symbol, 0, source,
as_of` meaning "out as of date"), or define universe rows as a FULL point-in-time snapshot per
`effective_date` and have `universe_at` take only rows of the single latest `effective_date ≤
date` (drop the per-symbol carry-forward).

### MEDIUM

**M2 — `read_universe` does not reject duplicate `(effective_date, symbol)` rows →
nondeterministic weight.** `dispersion_workflow.cpp:205-217` validates and sorts by
`(effective_date, symbol)` but never dedups. `universe_at` (`:232-241`) does
`active[symbol] = &row` while iterating the sorted rows, so for two rows with the same
`(effective_date, symbol)` but different `raw_weight` the "winner" is the last among equal
elements — and `std::sort` is NOT stable, so which weight is picked is unspecified. Determinism
hole on malformed input.
Fix: reject duplicate `(effective_date, symbol)` keys with `AlreadyExists` in `read_universe`.

**M1 — `verify` depends on a Python-only artifact and never checks its contents.**
`verify_command` (`spy_dispersion_backtest.cpp:444-452`) requires `reference_reconciliation.tsv`,
which NO C++ command writes — it is produced solely by `tools/reference_spy_dispersion.py`
(confirmed by grep; only the example + that tool + a sprint doc mention it). Worse, `verify` only
checks the file EXISTS and is non-empty; it never parses or cross-checks its numbers against the
C++ backtest. So (a) `verify` can never pass on C++-only artifacts, and (b) the headline
"independent reference reconciliation" gate is not actually enforced — a stale or garbage file
passes. This is the module's determinism/parity anchor and it is unenforced in-process.
Fix: either write a native reference reconciliation (the machinery already exists —
`reconcile_listed_dispersion` IS an independent reprice) and have `verify` compare row values, or
document `verify` as a cross-language step and add an in-process numeric assertion.

**M3 — Reconciliation entry-mark equality is EXACT (tolerance 0.0) across two pricing routes.**
`ListedReconciliationConfig::entry_mark_tolerance` defaults to `0.0` with `strict_model=true`
(`listed_dispersion_reconciliation.hpp:86-88`); `mark_leg` (`:165-169`) rejects the run when
`|fair_value(K,T,side) − leg.model_mark| > 0`. `leg.model_mark` was produced at build-schedule
time by `surface->evaluate(K,T,side, Price|Delta|Vega)` (`listed_dispersion_schedule.cpp:110`),
while reconcile reprices via `surface->fair_value(...)`. Both derive `T` from the SAME expiry and
the SAME archive `now_ts`, so today they are bit-identical (both AL Price boundary) and the check
is a strong integrity assertion. But it is a float-exact coupling between two distinct entry
points; any future route/compiler change that perturbs one by 1 ULP silently converts a clean run
into a hard abort. `ListedDispersionStrategy::on_step` has the same landmine
(`listed_dispersion_strategy.cpp:116`: `seed.greeks().price != leg.model_mark`).
Fix: adopt a tiny relative tolerance (e.g. a few ULP / `1e-12·|mark|`) on both exact checks, or
pin a single shared pricing entry point for build + reconcile + run.

**M4 — Index symbol `"SPY"` is hardcoded in the LIBRARY, not the driver.**
`all_symbols` (`dispersion_workflow.cpp:224`, seeds `{"SPY"}`) and `universe_at` (`:238-240`,
`index = DispersionMember{"SPY",...}` and `if (symbol != "SPY")`) bake the index symbol into the
reusable workflow. A different index (SPX proxy, sector ETF) requires editing library code. The
`RunSpec` has no `index_symbol` field.
Fix: add `RunSpec::index_symbol` (default "SPY") and thread it through `all_symbols`/`universe_at`.

**M5 — Clock-gap fragility on the surface book roll.** The surface `DispersionStrategy`
(RollAtHorizon) closes-at-marks only on a step whose `base_ts` lands in `(expiry−roll_at_T,
expiry)`. Its lot expiries are SYNTHETIC (`base_ts + round(target_T·year)` or a projected
calendar timestamp), so they never coincide with a real clock snapshot. If the corpus has a gap
that skips the whole roll window and the next observed date is at/after a synthetic expiry,
`compute_step` (`backtest.cpp:811-818`) throws `NotFound "no exact expiry observation for lot"`
(fail-closed, correct — a later spot is not a settlement price). So the flagship benchmark hard-
fails if any corpus gap exceeds `roll_dte_days`. This is robustness, not silent error, but there
is no guardrail relating `roll_dte_days` to the max clock gap.
Fix: document/assert `roll_dte_days > max_clock_gap_days`, or roll on `residual_T ≤ roll_at_T`
using a `≤` at the first in-window step so a single in-window date always triggers the mark-close.

### LOW

**L1 — Coverage ratio lacks a zero-denominator guard.**
`spy_dispersion_backtest.cpp:405`: `coverage = traded_weight / requested_weight`. `requested_weight`
is `> 0` by construction (`universe_at` non-empty + `raw_weight > 0`), so NaN cannot occur today,
but an explicit guard would harden it against a future universe change (pf2 low-sev, still open).

**L2 — Look-ahead boundary in `build-schedule` is the surface ts, not the quote ts.**
`build_schedule_command` passes `snapshot.ts_ns()` (surface-archive `now_ts`) as the selection
valuation (`:387`), while the joined quotes carry the OPRA panel `frame.snapshot_ts_ns`
(`:331`). `select_listed_dispersion` judges `quote_ts ≤ valuation` and `expiry > valuation`
against the surface ts. Both derive from `snapshot_suffix` ("T19:55:00Z") and are equal today, so
benign; if the surface and OPRA pipelines ever diverged, the look-ahead boundary would follow the
surface ts silently. Add an equality assertion.

**L3 — The surface dispersion book cannot be held-to-expiry-settled.** Synthetic expiries never
match a clock observation, so a `LifecycleSpec::HoldToExpiry` dispersion config would hard-fail
every settlement step (M5 mechanism). The canonical `make_dispersion_backtest_strategy`
(`dispersion_backtest.cpp:26-28`) uses RollAtHorizon and is safe, but nothing rejects a misused
HoldToExpiry dispersion strategy. This is the "synthetic-tenor lots may not settle" concern from
review 06 — real, but out of the shipped path.

**L4 — `entry_every_n` (default 21) is dead config under RollAtHorizon.** `lifecycle_decide`
(`strategy.cpp:816-824`) ignores the entry cadence in RollAtHorizon (opens only on empty book or
`residual_T < roll_at_T`). `DispersionBacktestConfig::entry_every_n=21` therefore has no effect on
the canonical dispersion backtest — confusing surface area.

---

## LOOK-AHEAD VERDICT: CLEAN (no violations found)

Traced each command's snapshot timestamp semantics:
- **run-surface-backtest** (`run_dispersion_backtest` → `DispersionStrategy` →
  `run_backtest(strategy)`): every entry resolves + sizes + marks on `base.set()` at
  `base.ts_ns()`; `compute_step` MTMs base→shifted so forward PnL is realized only to the NEXT
  observation; settlement fires only at an EXACT expiry timestamp (`backtest.cpp:811-818`), never
  a later spot. Decisions/marks/fills all at-or-before the decision ts. No look-ahead.
- **build-schedule / run-backtest (listed):** `select_listed_dispersion`
  (`listed_dispersion.cpp:52-57`) rejects `quote_ts > valuation` and `expiry ≤ valuation`;
  `reconcile_listed_dispersion` re-checks `quote_ts ≤ valuation` (`:115-118`). Guarded.
- **run-projected-var:** each scenario uses its OWN date's surfaces at its own ts; the strict
  `valuation_ts == scenario.ts` guard in `historical_projection` prevents cross-date leakage.
- Only nuance: L2 (surface-ts vs quote-ts boundary), benign today.

Settlement semantics (`backtest.cpp:1976-1992` strategy overload) settle expiries at INTRINSIC
using the shifted-date spot and the base mark, only after `base = shifted`, only at an exact
expiry observation — cash-settled intrinsic, correct and look-ahead-free.

---

## AXIS 5 — EXAMPLE → LIBRARY EXTRACTION INVENTORY

The driver is ~761 LOC; roughly 620 of them are library workflow, not CLI glue. Only `main`
(arg parse, `:716-761`), the tiny local `split`/`parse_number`/`read_text`/`hash_*` helpers
(`:55-95`), and the profiling/counter TSV dumps (`:540-579`, build-flag-gated diagnostics) are
genuinely thin glue. Everything below should move into the library (a new
`dispersion_run.{hpp,cpp}` orchestration TU, plus writers on the existing modules).

| Driver block (file:line) | What it really is | Proposed library API | Blockers |
|---|---|---|---|
| `build_corpus_command` :232-318 | Full corpus-build orchestration: read spec/universe, load OPRA daterange, build `QualifiedCorpusConfig` with **hardcoded admission policy** (min_quotes=20, min_slices=2, calendar_abs_k=0.7, HFT preset, LinearVariance, pinned `policy_fingerprint` "…v4-pinned-linear-calendar-floor-k0.7"), append dates, write manifest/quality/inventory, copy universe+definitions, write resolved spec | `Result<CorpusBuildSummary> build_dispersion_corpus(const RunSpec&, const DispersionCorpusPolicy&, path run_dir)` | The pinned admission constants + `policy_fingerprint`/`input_fingerprint` strings are load-bearing for `verify` determinism — must move as NAMED library defaults (`DispersionCorpusPolicy`), not inline literals. Depends on corpus + opra_batch. |
| `build_schedule_command` :339-432 | The immutable-schedule build loop: Clock from manifest, OCC verify, per-ref universe resolve, quote load, `select_listed_dispersion`, **coverage gate**, roll-deferral state machine (`active_expiry`, `roll_dte_days`), `build_listed_dispersion_roll`, write schedule | `Result<ListedDispersionSchedule> build_listed_dispersion_schedule(const RunSpec&, const ScheduleInputs&)` (inputs = universe rows, definitions, clock, snapshot loader, forward lookup) | The forward-lookup closure (`snapshot.find`→`forward_at`), `MissingNamePolicy::DropRenormalize`, min-coverage gate, and roll-deferral logic are all orchestration currently in the driver. `hash_file(archive)` for `surface_fingerprint`. |
| `run_backtest_command` :471-521 | Listed backtest + reconciliation driver: read artifacts, `ListedDispersionStrategy::create`, `run_backtest`, `all_rolls_consumed` gate, reload snapshots+quotes, assemble `ListedReconciliationSnapshot[]`, `reconcile_listed_dispersion`, `validate_listed_reconciliation_backtest`, write marks + reconciliation | `Result<ListedBacktestArtifacts> run_listed_dispersion_backtest(const RunSpec&, const RunArtifacts&, path run_dir)` | The snapshot/quote reload loop (`:492-503`) and reconciliation-snapshot assembly are pure orchestration. Fix H1 as part of the move. |
| `run_projected_var_command` :586-702 | ~116 LOC: load all snapshots, resolve universe, `build_dispersion_book`, build `RelativeOptionPosition[]`, `PreparedHistoricalProjection`, `evaluate_into`, compute VaR/ES at {0.95,0.99}, write 3 TSVs | `Result<ProjectedVarResult> run_dispersion_projected_var(const Clock&, DispersionUniverse, const ProjectedVarConfig&)` + `write_projected_var_*` serializers | Relative-position construction from `DispersionBook`, hardcoded confidence levels, and all three TSV layouts live in the driver. TSV writers should become library serializers (like `serialize_listed_reconciliation`). |
| `run_surface_backtest_command` config assembly :533-539 | Maps `RunSpec` → `DispersionBacktestConfig` | `DispersionBacktestConfig dispersion_backtest_config_from_run_spec(const RunSpec&)` | Trivial; the core `run_dispersion_backtest` is ALREADY library (good). Profiling/counter dumps `:540-579` can stay in the driver (build-flag CLI diagnostics). |
| `verify_command` :434-469 | Artifact-envelope + core-mode acceptance verification: required-file list, `validate_listed_dispersion_schedule`, quality/manifest cross-check, core gates (≥60 dates, ≥3 rolls, ≥40 names/roll) | `Status verify_dispersion_run(const RunSpec&, path run_dir)` | Hardcoded artifact filename list + core thresholds; the `reference_reconciliation.tsv` dependency (M1) should be resolved to a native check during the move. |
| `persist_occ_ess_evidence` :119-165 | Atomic OCC ESS evidence copy + inventory TSV | `Status persist_occ_ess_evidence(path run_dir, const RunSpec&, const OpraBatchResult&)` | Depends on `read_occ_ess_report_file`; belongs on the occ_ess module. |
| `verify_occ_ess_evidence` :167-207 | OCC ESS inventory verify + qualified-date authority check | `Status verify_occ_ess_evidence(path run_dir, const Clock&)` | Belongs on occ_ess module. |
| `load_listed_quotes` :320-337 | OPRA daterange → `ListedOptionQuote[]` join wrapper | `Result<std::vector<ListedOptionQuote>> load_listed_quotes(const RunSpec&, const ListedDefinitionTable&, symbols, date)` | Thin wrapper over `batch_spec`/`load_opra_daterange`/`listed_quotes_from_opra`; move to listed_opra. |
| `write_input_inventory` :97-117 | OpraBatchResult inventory TSV writer | `Status write_input_inventory(path, const OpraBatchResult&)` | Pure; move to opra_batch. |
| `write_methodology_map` :209-230 | Static methodology-provenance TSV (constant content) | `Status write_methodology_map(path)` | Entirely constant; belongs in library as a documentation artifact writer. |

Top 8 by payoff: `build_dispersion_corpus` (+ `DispersionCorpusPolicy` for the pinned admission
constants) · `build_listed_dispersion_schedule` (roll-deferral loop + coverage gate) ·
`run_listed_dispersion_backtest` (backtest + reconciliation, fixes H1) ·
`run_dispersion_projected_var` (+ VaR TSV serializers) · `verify_dispersion_run` (fixes M1) ·
`dispersion_backtest_config_from_run_spec` · `persist_occ_ess_evidence`/`verify_occ_ess_evidence`
· `load_listed_quotes` + the two inventory/methodology writers.

Structural blocker common to all: the pinned fingerprint STRINGS and admission/threshold CONSTANTS
currently live as inline literals in the driver and are load-bearing for `verify`/reproduction.
They must be promoted to named library defaults in the same move, or the extraction silently
changes the reproduction fingerprint.
