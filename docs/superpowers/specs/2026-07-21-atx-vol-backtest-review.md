# Productionization Sprint Grounding Report — atx-vol Backtest Framework

**Scope.** Consolidates five architecture maps and 32 adversarially-verified findings (CONFIRMED / PLAUSIBLE only) into one sprint-grounding document. Sprint goal: a *general* backtest framework consumed by dispersion / strangle / mag7 (dispersion migrated first, seams strategy-agnostic), plus a custom binary mmap `RunArchive` result container replacing the 1,253-line `examples/spy_dispersion_backtest.cpp` orchestrator and its ~20 loose output files.

All file refs are absolute under `C:\atx\atx-vol\`. Line numbers are as cited in the verified findings and are approximate anchors, not guarantees post-edit.

---

## 1. Executive Summary

The listed-dispersion pipeline works and is economically correct on its validated corpus, but it is **structurally mis-layered**: the genuine multi-date orchestration and roughly 300–400 lines of route-defining quant economics live inside a single example binary (`examples/spy_dispersion_backtest.cpp`), not in the library. The header comment of the *surface-only* driver (`dispersion_backtest.hpp:3-5` — "the example CLI is intentionally limited to parsing files and writing artifacts") already states the intended contract; the **listed route violates it** and should be brought up to it.

Three problem clusters dominate:

1. **Stranded economics (library boundary).** The schedule *builder* (roll cadence, weight-coverage acceptance gate, deferral, cohort numbering), the project-schedule cold-idealization onto ATM-forward strikes, the reconciliation-snapshot assembly, the projected-VaR book→template synthesis, and the methodology policy (admission rule + acceptance thresholds + query-route) are all example-local. They are unreachable by any unit test and non-reusable by strangle/mag7. `dispersion_workflow` is *not* the home for this — it is a clean, stateless spec/universe front-end that additionally **hardcodes `"SPY"`** and must be de-SPY'd, not overloaded.

2. **Result-storage fragility (the RunArchive motivation).** A `--run DIR` is ~20 independent flat files with **no run-level manifest, no checksum anywhere, and inconsistent/missing versioning on the highest-churn files**. The canonical P&L track `backtest.tsv` is the least-protected artifact of all: non-atomic truncating write, no magic/version, no CRC — while *lower*-traffic schedule/reconciliation writers already use the atomic pending→rename pattern. The 25-column `BacktestResult` schema is hand-duplicated in four places (two C++ writers that disagree on delimiter *and* `nav` position, plus two Python readers), and one Python copy already lists a phantom column (`step_pnl_total`) no writer emits — drift has already happened.

3. **Recompute & no cross-run cache (performance).** Every subcommand is its own OS process with a fresh in-memory `SnapshotCache` and no disk backing, so a parameter sweep re-pays the full definitions parse (~12 s) + reconciliation OPRA join (~16 s) on every point even though none of the swept knobs change the corpus, definitions, or joins. The reconciliation OPRA join additionally **over-produces the entire panel (~100× the ~102 scheduled legs it actually marks)** and issues ~60 serial single-date batches instead of one parallel range batch; the definitions parser does ~8.7M redundant heap allocations and per-row ISO-date reparses.

**One CONFIRMED correctness/robustness defect** must be fixed as a gate for the refactor: `reconcile_listed_dispersion` hard-requires `clock[0] == schedule.rolls.front().roll_date`, which is strictly stronger than the engine's precondition; any leading warm-up / low-coverage session aborts `run-backtest` on an otherwise-valid corpus (it works today only because `date_lo` happened to coincide with the first roll). Everything else is boundary/robustness/perf/latent-fragility — no silent-corruption bug survived verification.

Recommended sprint sequencing (dependency order, each step keeps the CLI compiling against a shrinking shim):
**(A)** result-store `run_archive` + `run_diagnostics` module (mechanical lift, no economics) → **(B)** `listed_dispersion_pipeline` (moves the (c) economics under test, fixes the clock-coupling defect at a seam) → **(C)** `backtest_driver` spine (dedupes the six drivers) → **(D)** engine `StepObserver` hook + `dispersion_workflow` de-SPY → **(E)** perf passes (targeted join, cross-run cache, parser).

---

## 2. Findings Ranked by Severity

Severities are the adversarially-*adjusted* ratings from verification (several original "high"s were reduced because the impact is offline/regenerable-artifact/latent rather than an active wrong-number bug). `[C]` = CONFIRMED, `[P]` = PLAUSIBLE.

### MEDIUM

| # | Finding | Files | One-line fix direction |
|---|---------|-------|------------------------|
| M1 `[C]` | **Reconciliation hard-requires `clock[0] == first roll date`; a warm-up/low-coverage opening session aborts the whole stage** even though the engine tolerates leading pre-roll dates. | `src/listed_dispersion_reconciliation.cpp:240-243`; `examples/spy_dispersion_backtest.cpp:451-540,630-646` | Trim the reconciliation timeline (and the backtest comparison) to start at `schedule.rolls.front().roll_date`, **or** make `reconcile_listed_dispersion` emit zero rows until the first roll date (mirror `ListedDispersionStrategy::on_step`), and enforce/validate the coupling at spec-resolution. |
| M2 `[C]` | **Reconciliation OPRA join over-produces the entire panel** (~thousands of rows/date OSI-parsed + `definitions.find()`'d) but `mark_leg` consumes only the ~2·(1+n_names) scheduled legs. | `examples/spy_dispersion_backtest.cpp:630-636`; `src/listed_opra.cpp:334-388`; `src/listed_dispersion_reconciliation.cpp:111-127,173` | Add a leg-key-filtered join variant fed the union of frozen scheduled contract keys (raw_symbol/OSI, strike, side, expiry already on the legs); cheap strike/side/underlier prefilter before any `definitions.find`. |
| M3 `[C]` | **No persistent cross-subcommand/cross-sweep cache**: each subcommand is one process with a fresh in-memory `SnapshotCache`; sweep knobs don't change corpus/definitions/joins, yet every point re-pays them. | `examples/spy_dispersion_backtest.cpp:1224-1247,606,933`; `src/snapshot_cache.cpp` | Persistent on-disk cache keyed by (corpus fingerprint, definitions fingerprint, universe, date range): pre-parsed binary definitions blob + per-date joined quotes for scheduled contracts; deeper win — cache the *selection* so only vega sizing recomputes when `gross_index_vega` changes. |
| M4 `[C]` | **`ListedDefinitionTable::create` runs per-row `iso_to_ns(trade_date + "T23:59:59...Z")`** (heap concat + ISO parse) over ~8.7M rows though only ~60 distinct trade_dates exist. | `src/listed_opra.cpp:148` | Memoize `trade_end` by distinct `trade_date` (`unordered_map<string,int64_t>`), compute once per date. |
| M5 `[C]` | **`parse_listed_definitions` allocates a `vector<string_view>` per row** (~8.7M small heap allocs) + a ~140MB line-index vector. | `src/listed_opra.cpp:201,211,67-80` | Single forward pass locating the 9 fixed tab boundaries inline, no per-row vector; destination `reserve` already present. |
| M6 `[C]` | **project-schedule cold-idealization (~120 LOC of "route-P" economics) is entirely example-local**; only the final `build_listed_dispersion_roll` sizing is a library call. No `project_listed_schedule()` seam. | `examples/spy_dispersion_backtest.cpp:700-816`; `include/atx/vol/listed_dispersion_schedule.hpp` | Extract `project_listed_schedule(listed, ArchiveLookup, ProjectionConfig{analytic, QueryExecution}) -> ListedDispersionSchedule` into a new `listed_dispersion_pipeline` module. |
| M7 `[C]` | **Multi-date schedule *builder* (roll cadence + coverage gate + deferral + cohort numbering + acceptance gate) exists only in the CLI**; library ships only the two per-date primitives. | `examples/spy_dispersion_backtest.cpp:451-540`; `include/atx/vol/listed_dispersion.hpp`, `.../listed_dispersion_schedule.hpp` | Add `build_listed_dispersion_schedule(clock, ListedScheduleSpec, universe_rows, definitions, quote_source) -> Result<...>` + `make_listed_forward_lookup(SurfaceSet&)`. |
| M8 `[C]` | **Projected-VaR path is 100% CLI-local**: book→`OptionProjectionSpec` synthesis + three bespoke TSV schemas with no library writer. | `examples/spy_dispersion_backtest.cpp:1068-1164`; `include/atx/vol/historical_projection.hpp`, `.../dispersion.hpp` | Add `dispersion_book_var(book, scenarios, confidences) -> {frames, legs, ProjectedHistoricalVar}` to the pipeline module; register the three schemas in the result-store. |
| M9 `[C]` | **Vega-unit ×100 (per-vol-point → per-unit-vol) is a hand-applied fudge duplicated at two boundaries**; the same spec field means per-vol-point on the listed side, per-unit-vol on synthetic/VaR side, reconciled only by an in-example multiply. | `examples/spy_dispersion_backtest.cpp:1000,1078`; `include/atx/vol/dispersion_backtest.hpp`, `.../dispersion.hpp` | Named library constant `kVegaVolPointToUnitVol = 100.0` or a typed adapter so the conversion happens once inside the library. |
| M10 `[C]` | **Two divergent `BacktestResult` serializers + 4-place column-schema duplication already drifting** (TSV vs CSV, `nav` at index 15 vs index 2, one Python copy lists a phantom `step_pnl_total`). | `src/tearsheet.cpp:189-216`; `src/run_report.cpp:74-104`; `python/.../io.py:19-25,62`; `python/.../parity.py` | One `constexpr kBacktestSeriesColumns` descriptor the writer iterates and that folds into `schema_hash`; codegen the Python descriptor; retire the divergent CSV order. |
| M11 `[C]` | **~20 loose result files, no run-level manifest, no checksum anywhere, inconsistent/missing versioning** (highest-churn files are exactly the unversioned ones; `\t1` vs `\tv1` token drift). `verify` even requires `reference_reconciliation.tsv` that no subcommand writes. | `examples/spy_dispersion_backtest.cpp`; `src/tearsheet.cpp`; `src/listed_dispersion_reconciliation.cpp` | Fold machine-read results into one binary `RunArchive` (magic+version+schema_hash, section directory = manifest, per-section + metadata CRC-32C via existing `detail::crc32c`). |
| M12 `[C]` | **`backtest.tsv` — the hottest, most-parsed artifact — is the least protected**: plain truncating `ofstream` (no temp+rename), no magic/version, no CRC, while lower-traffic writers are atomic. A mid-write crash yields a silently-truncated track `parity.py` folds row-by-row. | `src/tearsheet.cpp:283-298` | Any replacement writer must use pending→rename + stamp version + payload CRC; in `RunArchive` the P&L tracks become CRC-covered columnar sections written atomically. |

### LOW

| # | Finding | Files | One-line fix direction |
|---|---------|-------|------------------------|
| L1 `[P]` | Reconciliation entry-mark gate defaults to **zero tolerance across two distinct price paths** (`fair_value` vs fused `evaluate`); no headroom for a future 1-ULP divergence or tier change. | `include/atx/vol/listed_dispersion_reconciliation.hpp:87`; `src/listed_dispersion_reconciliation.cpp:154-169`; `src/listed_dispersion_schedule.cpp:110-115` | Small nonzero default (~1e-9) *or* author `leg.model_mark` through the same `fair_value` entry point; document the LegacyCompatible-tier assumption. |
| L2 `[P]` | **Mark-divergence bps collapses to 0 when frozen `schedule_mark == 0`** (permitted for deep-OTM legs), understating divergence in the channel the parity report keys on. | `examples/spy_dispersion_backtest.cpp:876-878` | Emit raw abs diff (or NA) when denom==0; gate parity on abs diff for zero-mark legs. |
| L3 `[P]` | **Definitions parse re-serializes the full ~696MB table just to fingerprint it**; fingerprint is unused on the backtest read path (but *is* read by the generator diagnostic + a test — so not a no-op removal). | `src/listed_opra.cpp:160-161,248` | Add a create-from-trusted-sorted-bytes constructor; on the parse path fingerprint `contents` directly only where byte-canonical. |
| L4 `[C]` | **run-projected-backtest deserializes every archive twice**: the divergence replay bypasses the shared cache. | `examples/spy_dispersion_backtest.cpp:852,967`; `src/backtest.cpp:1839,1911` | Route the replay's loads through `config.snapshot_cache->load(...)` so the priced run hits cache. |
| L5 `[C]` | **Reconciliation issues ~60 serial single-date OPRA batches** instead of one parallel range batch; worker pool drained/refilled per date. | `examples/spy_dispersion_backtest.cpp:410,630-636`; `src/opra_batch.cpp:433` | One `load_opra_daterange` over `[date_lo,date_hi]` for all symbols, then bucket by date; compounds with M2. |
| L6 `[C]` | Within run-backtest, snapshots are **already reused via the shared cache (reconciliation loads are hits)**, but each archive is whole-board deserialized (subset path unused). Informational. | `src/backtest.cpp:1181-1243`; `examples/spy_dispersion_backtest.cpp:606,631-632` | Thread a referenced-uid subset into the cache only if archives are broader than the universe; otherwise cross-process persistence (M3) is the lever. |
| L7 `[P]` | **build-schedule reads each roll archive twice** — `MarketSnapshot::load` then a full-file `hash_file` for `surface_fingerprint` (only on ~3–6 roll dates). | `examples/spy_dispersion_backtest.cpp:456,529,92-95`; `src/snapshot_cache.cpp:94-110` | Derive `surface_fingerprint` from the v2 header content-identity computed at load, or hash the already-mapped bytes. |
| L8 `[P]` | **Missing per-date adapter seams** (OPRA-join / forward-lookup / risk-lookup / reconciliation-snapshot assembly) each re-wired per call site; overstated on the recon-depends-on-closures point, but the missing-seam kernel is real. | `examples/spy_dispersion_backtest.cpp:406-430,485-496,624-648,727-736`; `include/atx/vol/listed_opra.hpp` | `listed_quotes_for_date(...)`, `make_listed_forward_lookup(...)`, `make_listed_risk_lookup(...)`, `assemble_reconciliation_snapshots(...)` in the pipeline module. |
| L9 `[C]` | **Methodology encoded as loose inline literals across four subcommands** (admission rule, query-route, `≥51/≥60/≥3/≥40` acceptance gates) with the `≥60/≥3` gate duplicated in build-schedule and verify; no versioned policy struct. | `examples/spy_dispersion_backtest.cpp:322-357,444-446,537-540,571-583,930-948,287-291`; `include/atx/vol/corpus.hpp` | One `ListedDispersionMethodology` struct (admission + fit template + thresholds + query-route + occ-ess authority + `policy_fingerprint`) consumed by every stage. |
| L10 `[C]` | **Mark-divergence capture shadows the real engine run** (`write_mark_divergence_replay` re-implements load+on_step) because `run_backtest` exposes no per-step hook; the shadow already differs from the engine (no settlement/erase before on_step). | `examples/spy_dispersion_backtest.cpp:834-897,967`; `include/atx/vol/backtest.hpp`; `.../listed_dispersion_strategy.hpp` | Add an optional `StepObserver` to `RunConfig` firing after each `on_step` with strategy access; reuse the single real run; move the bps metric into the pipeline module. |
| L11 `[P]` | **The strategy-agnostic 9-stage spine is copy-pasted across 5 of 6 drivers** (synthetic-corpus trio, meta-header hand-roll, `Args/parse_args/nv`, `split/join/fmt_num`); `run_report.hpp` is the already-existing back half, under-consumed. (Headline overstated — spine not identical; only ~1 driver uses `EngineRunStats`.) | `examples/{mag7_dispersion_backtest,spy_dispersion_pnl,spy_strangle_backtest,dispersion_backtest,strategy_examples}.cpp` | Introduce a `BacktestJob`/`run_backtest_job` driver owning the fixed spine, parameterizing only stages 1–5, converging engine slots on `→ BacktestResult`. |
| L12 `[C]` | **`dispersion_workflow` is the config front-end, not orchestration — and hardcodes `"SPY"`** as the always-first index in `all_symbols`/`universe_at`. | `include/atx/vol/dispersion_workflow.hpp`; `src/dispersion_workflow.cpp:224,238-240` | Keep it as the front-end; add `RunSpec.index_symbol` to drop the SPY hardcode; new methodology knobs go to `ListedDispersionMethodology`, not `RunSpec`. |
| L13 `[C]` | **`parity.py` is a third independent TSV parser** hard-coding writer-owned column names as string literals; a C++ rename breaks the HTML report at render time, not build time. | `python/.../parity.py:67-91,206-479`; `python/.../io.py` | Ship a pure-Python `RunArchive` reader whose column access goes through the generated schema descriptor and whose `open()` checks `schema_hash`; keep binding-free by reading the archive. |
| L14 `[C]` | **Enum/dtype encodings inconsistent and undocumented in-file** — names (`Entry/Held`) in contract_marks, raw uint32 ordinals in surface_manifest (silently breaks on enum reorder), `C/P` + `1/0` in schedule; no legend travels with data. | `src/listed_dispersion_reconciliation.cpp:366-409`; `src/corpus.cpp:1702-1743`; `src/listed_dispersion_schedule.cpp` | In `RunArchive`, encode categoricals as u8 codes + a per-section label table (mapping travels with data); dict-encode string columns; `schema_hash` covers the label set. |
| L15 `[P]` | **Verified-correct core mechanisms** (settlement fail-closed, one-sided execution gate, holiday classifier, vega-neutral sizing, `all_rolls_consumed` guards) with one residual unenforced coupling: run-backtest implicitly needs the clock to end before the final cohort's expiry. | `src/backtest.cpp:811-818,1584-1591`; `include/atx/vol/listed_dispersion_strategy.hpp:75-78`; `src/listed_opra.cpp:294-300`; `src/listed_dispersion_schedule.cpp:248-261` | No correctness change; document the "corpus ends before final cohort expiry" and "LegacyCompatible tier" preconditions alongside the M1 fix. |

**Proposal findings** (`module decomposition`, `RunArchive requirements`, `artifact partition`, `migration`) verified CONFIRMED/PLAUSIBLE and are folded into Sections 4–5 rather than re-listed as defects.

---

## 3. Current-Architecture Assessment

### 3.1 The example-vs-library boundary today

There are **two backtest routes and one real orchestrator**:

- **Surface-only (synthetic) route** — `DispersionStrategy → run_backtest`, packaged as the *one genuine composed library driver* `run_dispersion_backtest` (`dispersion_backtest.hpp/.cpp`). Its header comment is the design contract the sprint should generalize: the CLI parses files and writes artifacts; strategy/lifecycle/hedge/engine defaults live in the library.
- **Listed (real-tape) route** — `select_listed_dispersion → build_listed_dispersion_roll → persist ListedDispersionSchedule → ListedDispersionStrategy → run_backtest → reconcile_listed_dispersion`. This route has strong **per-date primitives** and a genuine **whole-timeline reconciler**, but **no multi-date schedule-assembly driver, no projection seam, no VaR seam, and no pipeline sequencer** in the library.

The library already provides reusable primitives — `run_backtest` (B0 fixed-book / B1 strategy), `Clock::from_manifest`/`from_surface_db`, `MarketSnapshot::load`, `SnapshotCache`, `build_dispersion_book`, the listed per-date primitives, schedule/manifest/quality persistence, `tearsheet`/emitters, `CorpusBuildSession` — and exactly one composed driver (`run_dispersion_backtest`, surface-only). **No library function chains any two listed-route stages.** Ordering, run-directory layout, and every inter-stage file contract live only in `examples/spy_dispersion_backtest.cpp`'s eight `*_command` functions.

`dispersion_workflow` — the obvious candidate "home" — is confirmed a **stateless config/input front-end**: six free functions (`read_run_spec`/`write_resolved_spec`, `read_universe`, `all_symbols`, `universe_at`, `batch_spec`) over POD `RunSpec`/`UniverseRow`. It includes only `dispersion.hpp`, `opra_batch.hpp`, `types.hpp` — never `backtest.hpp`, `corpus.hpp`, or any strategy/engine header. It performs no clocking, pricing, engine call, or stage sequencing, and it **hardcodes `"SPY"`**. It stays the front-end; it is *not* where orchestration goes.

### 3.2 What is stranded in the example (category-(c) economics + file contracts)

Ranked by economic density (from Map 1's migration list):

1. **project-schedule per-roll idealization (`700-816`)** — residual-T + structural guards; cold `ListedRiskLookup` (`full_greek_seed(..., analytic=true, ColdReference)`); `make_straddle`/`make_quote` re-striking each leg to `surface->forward_at(residual_T)`, cold-repricing at zero synthetic spread, stamping `standard_monthly/deliverable=true` and carrying provenance; selection reconstruction from frozen legs. A complete "project a listed roll onto surface-ideal strikes with cold greeks" routine. **Owns an unguarded cross-command parity contract** with the `ColdReference` replay in `run_projected_backtest_command` (must stay bit-identical; nothing checks it at compile time).
2. **build-schedule cadence + acceptance economics (`451-540`)** — DTE roll-trigger, per-date universe rebind, hand-built forward-lookup closure, weight-coverage acceptance gate, deferral policy, cohort numbering, `surface_fingerprint` via `hash_file`, entry/three-roll gate.
3. **Reconciliation-snapshot assembly (`624-648`)** — loads every snapshot, joins quotes per date, keeps owner vectors alive, hand-packs the `ListedReconciliationSnapshot` vector before the library seam.
4. **Projected-VaR (`1050-1169`)** — dispersion config, `build_dispersion_book`, book→`OptionProjectionSpec` synthesis, `PreparedHistoricalProjection::evaluate_into`, 95/99% VaR, and three bespoke TSV schemas with no library writer.
5. **Vega-unit ×100 convention** — duplicated at `1000` and `1078`.
6. **Methodology-as-loose-config** — admission rule + fit template + `policy_fingerprint` (`337-357`), acceptance thresholds (`322-324/444-446/537-540/571-583`), query-route (`930-948`), OCC-ESS data-authority gate (`287-291`).
7. **Mark-divergence bps metric + shadow replay (`834-897`)** — needed only because `run_backtest` hides per-step strategy state.
8. **Example-local output schemas with no library writer** — `projected_risk_{scenarios,legs}.tsv`, `projected_var.tsv`, `backtest_profile/counters.tsv`, `mark_divergence.tsv`, `input_inventory.tsv`, `occ_ess_inventory.tsv`, `methodology_map.tsv`, `diagnostics_*.tsv`.
9. **Compliance/evidence machinery** — `persist_occ_ess_evidence`/`verify_occ_ess_evidence` (atomic publish + data-authority gate), `write_methodology_map`, `PhaseTimer`/`write_diagnostics`, and `verify_command`'s envelope/count/core-mode gates.

Cross-driver, the *other* five examples re-implement the strategy-agnostic 9-stage spine (clock → strategy → RunConfig → timed run → tearsheet → EngineRunStats → emit → summary → exit-codes). The back half already exists as `run_report.hpp` (`MetaKv`, emitters, metric extractors) but only `mag7` consumes it; the missing front half is a data-source/Clock provider, a strategy factory, a CLI/option registry, and a RunConfig overlay hook — plus reconciling `run_backtest` vs `run_dispersion_backtest` behind a single `→ BacktestResult` seam and treating `spy_strangle_tradeable`'s manual evaluator as an alternate engine mode.

---

## 4. Proposed Library Module Decomposition

Four new library concerns split by axis, plus two small in-place changes. Strategy-agnostic pieces are named so strangle/mag7 reuse them without knowing about dispersion; dispersion-specific economics land in one clearly-labeled module.

### (1) `atx/vol/run_diagnostics.hpp` + `.cpp` — diagnostics module *(strategy-agnostic; mechanical lift)*
Lift `PhaseTimer` (ex `105-146`) and `write_diagnostics` (ex `157-181`) out of the example verbatim. A named-phase wall-time/count accumulator + the versioned `ATX_DISPERSION_DIAGNOSTICS` `diagnostics_<stage>.tsv` writer (keep the existing magic string; do **not** silently rename it) + stderr summary. Deletes the ~4 in-example `PhaseTimer` constructions and makes the diagnostics format one owned contract.

### (2) `atx/vol/run_archive.hpp` + `.cpp` — result-store module *(strategy-agnostic)*
A `RunDir` handle owning the run-directory envelope and the single source of truth for:
- **(a)** the artifact filename set (`run_spec`, `surface_manifest`, `quality`, `universe_schedule`, `definitions`, `trade_schedule`, `backtest`, `contract_marks`, `reconciliation`, `occ_ess_inventory`, `diagnostics_*`, `mark_divergence`, `projected_*`);
- **(b)** the 25-col `kBacktestSeriesColumns` schema (replacing the 4-place duplication; retiring/unifying `run_report.cpp` CSV vs `tearsheet.cpp` TSV);
- **(c)** the uniform magic/version header + per-member checksum;
- **(d)** the `verify()` envelope/existence/count/core-mode gates (ex `552-587`).
Absorbs `write_input_inventory`, `write_methodology_map`, `persist/verify_occ_ess_evidence` as `RunDir` members. Typed accessors: `dir.spec()`, `dir.clock()`, `dir.schedule()`, `dir.write_backtest(r)`, etc. Section 5 defines the binary container this module writes.

### (3) `atx/vol/backtest_driver.hpp` + `.cpp` — strategy-agnostic spine
```
struct BacktestJob {
  ClockSource      clock;          // synthetic-build | manifest-file | SurfaceDb whole/windowed
  StrategyFactory  strategy;       // returns IStrategy&  OR  {DispersionUniverse, DispersionBacktestConfig}
  RunConfigOverlay overlay;        // frictions / tier / adaptive-confirm / preload / cache sizing
  OutputProfile    outputs;        // five-CSV | single-self-describing-TSV | plain | RunArchive
  MetaKv           meta;
};
run_backtest_job(job) -> { BacktestResult, TearSheet, EngineRunStats };
```
Owns the fixed spine (clock-build → strategy-construct → timed `run_backtest` → `tearsheet()` fold → `EngineRunStats` capture → `OutputProfile` emit → standard console summary → exit-code convention). The **engine slot** abstracts `run_backtest` vs `run_dispersion_backtest` vs the `tradeable` manual evaluator behind `→ BacktestResult`. Deletes Map-3's duplication (synthetic-corpus trio, meta-header hand-roll, arg-parser idiom, `split/join/fmt_num`, timed-run block, error ladder).

### (4) `atx/vol/listed_dispersion_pipeline.hpp` + `.cpp` — dispersion-specific orchestration *(the listed-route home)*
- `listed_quotes_for_date(spec, definitions, symbols, date)` [ex `406-430`]
- `make_listed_forward_lookup(SurfaceSet&)` [ex `485-496`], `make_listed_risk_lookup(...)` [ex `727-736`]
- `build_listed_dispersion_schedule(clock, ListedScheduleSpec, universe_rows, definitions, quote_source) -> Result<ListedDispersionSchedule>` [ex `451-540`: cadence, coverage gate, deferral, cohort, `surface_fingerprint`, acceptance] — **this is the seam where M1's clock/first-roll coupling is fixed** (produce a schedule whose first roll date is known and enforced against the clock)
- `project_listed_schedule(listed, ArchiveLookup, ProjectionConfig{analytic, QueryExecution}) -> ListedDispersionSchedule` [ex `700-816`]
- `assemble_reconciliation_snapshots(clock, spec, definitions, cache)` + `reconcile_listed_schedule(...)` [ex `624-648`] — the assembler owns the timeline-trim fix for M1
- `dispersion_book_var(book, scenarios, confidences)` [ex `1068-1162`]
- `constexpr double kVegaVolPointToUnitVol = 100.0` replacing the by-hand ×100 (M9)
- `struct ListedDispersionMethodology` (admission rule + fit template + acceptance thresholds `51/60/3/40` + query-route + occ-ess-authority flag + `policy_fingerprint`) replacing the loose config (L9)

### (5) `atx/vol/backtest.hpp` — CHANGED
Add an optional `StepObserver` to `RunConfig` firing after each `on_step` with strategy access, so mark-divergence capture reuses the **one** real engine run instead of the `write_mark_divergence_replay` shadow loop — removes the double engine pass in run-projected-backtest (L10, L4).

### (6) `atx/vol/dispersion_workflow.hpp` — CHANGED
Keep as pure config/input front-end; add `RunSpec.index_symbol` to drop the SPY hardcode in `all_symbols`/`universe_at` (L12). New methodology knobs go to `ListedDispersionMethodology`, not `RunSpec`.

### Thin-CLI shape
`examples/spy_dispersion_backtest.cpp` collapses **1,253 → ~200 lines**: keep `main()` arg-dispatch (`1187-1253`) + one library call per subcommand.
- **build-schedule** (~120 → ~4): `RunDir d(run); d.write_schedule(build_listed_dispersion_schedule(d.clock(), method, d.universe_rows(), d.definitions(), quote_source));`
- **project-schedule** (~150 → ~2): `d.write_projected(project_listed_schedule(d.schedule(), d.archive_lookup(), method.projection));`
- **run-backtest** (~75 → ~4): `auto r = run_listed_dispersion_backtest(d, method); d.write_backtest(r); reconcile_listed_schedule(d);`
- **run-surface-backtest / run-projected-var / run-projected-backtest**: single `run_backtest_job(...)` / `dispersion_book_var(...)` / projection call each.
- **verify** (`552-587` → ~1): `d.verify(method);`

`PhaseTimer` + the seven writer/verifier helpers (`105-316`) leave the example entirely. A single `run_listed_dispersion_pipeline(d, method, stages)` can reduce `main()` to stage-flag parsing while **preserving the deliberate process-boundary design** — each stage still independently loadable from disk, "no fitter/session object crosses a boundary".

**Landing order** (each keeps the CLI compiling against a shrinking shim): (2)+(1) first (mechanical lift, no economics) → (4) listed pipeline (moves (c) economics under test, closes M1/M6/M7/M8/M9/L9) → (3) driver spine (dedupes the other examples, L11) → (5) engine step-hook (L10) → (6) de-SPY (L12).

---

## 5. Proposed `RunArchive` Format (`run.atxrun`)

Ports the proven ATXVSA2 skeleton verbatim (`docs/atxvsa2-format.md` §2/§5; structs at `include/atx/vol/surface_archive.hpp:450-579`): contiguous mmap region, byte-offset directory = manifest, columnar SoA sections, header + metadata + per-section CRC-32C, `sizeof`+column `schema_hash`, atomic `.tmp`+rename write, `ArchiveContentIdentity`. Reuses `detail::crc32c` / `crc32c_update` / `align_up` (`include/atx/vol/detail/archive_util.hpp`) and the read-only mmap seam `tsdb::Mapping` via `open_borrowed(span, owner)` (`surface_archive.hpp:674`).

### Container layout
```
offset 0     RunArchiveHeader (256 B, fields by descending alignment, naturally aligned)
             magic "ATXRUN01"; major/minor; header_size=256; endian=1; pointer_bits=64;
             file_size; created_ts_ns;
             schema_hash        (u64: sizeof-fold of every on-disk struct + fold of each
                                 section's column {name,dtype,unit} table + a RunArchive salt)
             writer_version_hash;
             run_identity_hash  (u64 over run_spec bytes + input fingerprints -> content-derived id)
             section_dir_offset; data_offset; section_count;
             header_crc32c (own field zeroed); metadata_crc32c (over the section directory);
             flags; reserved-pad to 256.

section_dir  SectionDescriptor[section_count]  (sorted by name -> O(1) find; IS the manifest)
             section_kind {ScalarKV|TimeSeries|SubTable}; char name[32];
             u64 section_offset; u64 section_size; u64 n_rows; u32 n_cols;
             u64 col_desc_offset; u32 payload_crc32c  (a COPY of the section's own CRC, so
             metadata_crc32c covers it -> any in-place section rewrite changes content-identity,
             the F6 trick at surface_archive.hpp:519-527).  metadata_crc32c covers this whole array.

data         each Section 64-B aligned, self-contained, record-relative offsets (no pointers ->
             no relocation on mmap): SectionHeader (magic, record_size, n_rows, n_cols, col_desc
             block of record-relative offsets, payload_crc32c over section with own field zeroed)
             then ColumnDescriptor[n_cols] {char name[32]; u8 dtype (f64|i64|u32|u8-enum|dict-str);
             u64 col_offset; u64 aux_offset} then contiguous typed column arrays 8-B aligned.
             String columns (symbol/raw_symbol/instrument_id) = Arrow-style dictionary: u32 code
             column + per-section string table at aux_offset. Enum columns (side C/P, role, status)
             = u8 code column + in-section label table (fixes L14: mapping travels with data).
```

### Section list
1. **`meta`** (ScalarKV) — resolved run_spec echo, window (`date_lo/hi`), the roll-level scalars lifted out of the schedule leg rows (`gross_index_vega_target`/`net_vega`/`gross_vega`/`n_names`/`valuation_ts_ns`/`expiry_ts_ns`), manifest counts.
2. **`backtest`** (TimeSeries SoA) — `ts_ns` i64 + the 25 `BacktestResult` f64 columns in one declared order + one appended column per signal.
3. **`projected_cold`**, 4. **`projected_nodiv`** (TimeSeries) — identical schema to `backtest`.
5. **`reconciliation`** (TimeSeries) — the 11 columns (`date`, `valuation_ts_ns`, `held_cohort`, `model_option_pnl`, `quote_mid_pnl`, `model_minus_quote_pnl`, `model_nav`, `quote_mid_nav`, `quote_mid_coverage`, `n_held_lots`, `n_quote_mid_lots`).
6. **`trade_schedule`**, 7. **`projected_schedule`** (SubTable, rolls×legs) — the 29 schedule columns minus the roll-level scalars (now a per-roll header block); `source_fingerprint`/`surface_fingerprint` as dict-str.
8. **`contract_marks`** (SubTable, date×leg×role) — the 19 columns with role/status/side as u8+label tables and NA-able raw fields as f64 + validity bit.
9. **`mark_divergence`** (SubTable) — `date`/`symbol`/`raw_symbol`/`strike`/`expiry_ts_ns`/`side`/`schedule_mark`/`live_mark`/`diff`/`abs_diff_bps_of_mark`.
10. **`diagnostics`** (SubTable) — `subcommand`/`phase`/`wall_ms`/`count`.

### Integrity / open / atomic write
`open()` validates magic + version + endian + pointer_bits + `schema_hash` + `header_crc32c` + `metadata_crc32c` + section bounds **only** (lazy per-section CRC via `validate_section(name)`, never on read). mmap via `tsdb::Mapping` through `open_borrowed(span, owner)` so open faults only header+directory pages; reading one section touches only its extent. **Atomic write** = build an in-memory buffer then pending→rename (the pattern already in `listed_dispersion_reconciliation.cpp:104`), fixing the non-atomic `tearsheet` writer (M12). **Content identity** reuses `ArchiveContentIdentity {file_size, created_ts_ns, header_crc32c, metadata_crc32c}` verbatim (`surface_archive.hpp:147`), enabling cache/staleness reuse.

### Schema single-source
One `constexpr` column-descriptor registry in `run_archive_schema.hpp` that (a) the writer iterates to emit columns, (b) folds into `schema_hash`, (c) a codegen step exports to a Python module — killing the four-place duplication (`tearsheet.cpp:189`, `run_report.cpp:74`, `io.py::_SERIES:19`, `parity.py` hardcoded names) and the divergent `nav` order (M10, L13, L14).

### Python read story
A pure-Python `python/src/atxvol/report/runarchive.py` (**no binding import**, honoring `parity.py`'s binding-free constraint at `parity.py:21-26`): mmap the file; `struct.unpack` the 256-B header (fixed little-endian); assert magic/endian/pointer_bits; **recompute the Python-side `schema_hash` from the same generated descriptor and compare — drift fails at open, not at row access** (fixes M10/L13). Parse the section directory into `{name: descriptor}`. For a TimeSeries/SubTable, return each f64/i64 column as `numpy.frombuffer(mmap, dtype, n_rows, offset)` — zero-copy views over the mapped bytes — decoding dict-str via the section string table and u8-enum via the label table; **no per-cell `float()`** (`io.py:62` today does `float(r[i])` for every cell). Payload-CRC verification is optional/lazy so a render need not pay it. A `read_backtest_section()` shim returns the existing `(BacktestResult|dict, meta, extra)` tuple so `parity.py`'s four `_read_tsv` calls (`parity.py:191-194`) swap to `archive.section(name)` with downstream code unchanged.

### Migration (dual-write, no flag day)
- **Phase 1** — writers dual-emit `run.atxrun` **and** the legacy TSVs; content-identity + `schema_hash` checks come online immediately.
- **Phase 2** — `runarchive.py` + `read_backtest_section()` shim land; `parity.py`/`io.py` switch to `archive.section(name)` with signatures unchanged. A `runarchive dump <section> --tsv` command regenerates byte-identical legacy TSVs for tools not yet cut over.
- **Phase 3** — retire `run_report.cpp`'s divergent CSV (fixing the `nav` order) and drop legacy TSVs once external harnesses move; keep `dump --tsv` as the escape hatch.

**Partition rule.** Results the run *produces* and that are machine-read go in the archive. Text stays for authored/human-edited inputs and compliance evidence — `run_spec.tsv`/`universe_schedule.tsv` (embed only their hash for identity), the 696 MB `definitions.tsv` (never in a result container), and `occ_ess/*.txt`+`occ_ess_inventory.tsv`+`methodology_map.tsv`+`input_inventory.tsv`. **Caveat (from verification):** the produces-vs-consumes cut is leaky — `trade_schedule`, `surface_manifest`, `quality`, `definitions` are all *produced by and consumed within* the run. The frozen schedule especially is **both** an inter-stage wire and a result: keep it as its standalone versioned atomic TSV during migration **and** embed a copy as an archive section for the durable result-of-record and content-identity.

---

## 6. Correctness Invariants To Preserve Through The Refactor

These are the load-bearing behaviors the extraction MUST NOT alter. Each is a regression-test target.

**I1 — Two-route cold parity (bit-exact).** The project-schedule cold-reprice (ex `700-816`) is *deliberately constructed* so the persisted `projected_schedule.tsv` marks equal the live cold seed marks that `run-projected-backtest --execution cold` recomputes at replay (ex `690-696`, `940`). Both paths must keep `analytic=true` + `QueryExecution::ColdReference`. When `project_listed_schedule` and the projected backtest move into `listed_dispersion_pipeline`, they must **share one code path or one asserted constant**, not two copies — today the parity is unguarded at compile time (M6). Add a parity test that runs both and asserts leg-mark equality.

**I2 — Settlement fail-closed.** `compute_step` returns `Err "no exact expiry observation"` when `lot.expiry_ts_ns < shifted.ts_ns()` — expiry must land exactly on a snapshot ts (`backtest.cpp:811-818`), consistent with the B1 cash-settle loop (`1978-1992`). Listed expiries are stamped midnight-UTC (`listed_opra.cpp:360-366`) and never coincide with an intraday ts, so in normal operation build-schedule keeps the last roll within `roll_dte` of corpus end and settlement never fires. Preserve this fail-closed behavior and, per L15, **document** the "corpus ends before final cohort expiry" precondition rather than leaving it emergent.

**I3 — One-sided execution gate.** The gate fires only when `required_execution==ColdReference` AND a fast tier is prepared AND `cfg.price.query_execution != ColdReference` (`backtest.cpp:1584-1591`) — the only regime where `Configured` diverges from cold. On the default `LegacyCompatible` tier, `Configured==cold`, so `ExactArchive` replay reproduces frozen marks and `Record` (required==`Configured`) stays intentionally ungated. `ExactArchive`'s own bit-exact `!=` gate (`listed_dispersion_strategy.cpp:118-127`) is the backstop. Do not make the gate two-sided.

**I4 — Vega-neutral sizing identity.** Index short target `= -gross`; name targets `= +normalized_weight·gross`, summing to `+gross` (Σ`normalized_weight==1`); net `== 0`, validated by `validate_roll` (`listed_dispersion_schedule.cpp:248-261`). Call and put share one quantity (true straddle); the delta band is a per-uid deadband hedging to zero when `|net|>band`. The M9 ×100 constant extraction must not perturb this — the per-vol-point vs per-unit-vol factor is exactly 100 and the current hand-multiply is correct; a named constant must produce byte-identical sizing.

**I5 — Reconciliation entry-mark equality basis.** `entry_mark_tolerance` defaults to 0.0 and demands bit-exact equality between reconciliation `fair_value()` and schedule `evaluate(...).price` (L1). If the refactor changes either pricer entry point *or* loads the reconciliation snapshot at a non-`LegacyCompatible` tier, this silently aborts. Preserve the tier assumption **or** unify the two entry points so equality holds by construction; if a tolerance is introduced, keep it ≤ the andersen_lake economic-parity tolerance.

**I6 — `all_rolls_consumed` gates.** Present and fail-closed at all three sites (ex `611`, `892`, `968`). The thin-CLI rewrite must retain them at each subcommand boundary.

**I7 — Output byte-stability where consumers depend on it.**
- **`ListedDispersionSchedule` TSV** is parsed back with *full re-validation* (`parse_listed_dispersion_schedule` re-derives vega arithmetic rather than trusting persisted totals). Any archive-section round-trip must reproduce values that re-validate; keep the standalone versioned TSV byte-stable during migration (it is the process-boundary wire).
- **`parity.py` keys on exact column names** (`pnl_total`, `nav`, `pnl_gamma/vega/theta/unexplained`; schedule `is_index`/`symbol`/`n_names`/`gross_index_vega_target`/`expiry_ts_ns`; divergence `abs_diff_bps_of_mark`/`schedule_mark`/`live_mark`/`strike`/`side`/`diff`). Until the schema-descriptor read lands, these names are a **binding interface** — a rename breaks the report at render time (M10, L13).
- **mag7's five pinned CSV filenames** (`series.csv`, `strategy_metrics.csv`, `engine_metrics.csv`, `db_stats.csv`, `populate_stats.csv`) are a binding contract for its Python renderer; the driver-spine `OutputProfile` must reproduce them exactly.
- **Mag7 vs spy_dispersion_pnl metadata conventions differ** (separate metrics file vs metrics inlined into the meta header). The `OutputProfile` must keep both, not force one.

**I8 — Process-boundary independence.** "Each command is a process boundary; no fitter/session object crosses it." A `run_listed_dispersion_pipeline` convenience is allowed only if each stage remains independently runnable from disk state.

---

## 7. Performance Opportunities Ranked By Payoff

Timing estimates are the reviewers' PhaseTimer-derived measurements (unverified against a live run per scope constraints); the *structural* claims are all CONFIRMED. Payoff ordering weights (magnitude × frequency-across-sweeps).

**P1 — Persistent cross-subcommand / cross-sweep result cache (M3).** *Highest payoff.* Today each subcommand is one process with a fresh in-memory `SnapshotCache` (no disk backing), so a parameter sweep re-pays ~28 s of invariant I/O per point (~12 s definitions parse + ~16 s reconciliation join) even though **no swept knob** (`gross_index_vega`, `delta_band`, `target/min/max_dte`, `roll_dte`, `min_weight_coverage`) changes the corpus, definitions, or OPRA joins — `batch_spec` pulls only `opra_root`/`path_template`/`snapshot_suffix`/`flat_rate`. Introduce an on-disk cache keyed by (corpus fingerprint, definitions fingerprint, universe, date range) storing a pre-parsed binary definitions blob + per-date joined quotes for scheduled contracts (+ optionally deserialized snapshots). **Deeper win:** `gross_index_vega` affects only sizing, not *which* contracts are selected, so caching the selection means a vega sweep recomputes only vega arithmetic. The `RunArchive`'s `run_identity_hash` + `ArchiveContentIdentity` give the cache-key and staleness primitives for free.

**P2 — Targeted reconciliation OPRA join (M2).** *High payoff, single-run.* Reconciliation OSI-parses + `definitions.find()`'s every panel row (`listed_opra.cpp:334-388`) — ~thousands/date — but consumes only the ~102 scheduled legs per active cohort (`mark_leg` does `quotes.find(key_of(leg))`). ~100× over-produced. Add a leg-key-filtered join variant fed the union of frozen contract keys (all already on the legs: raw_symbol/OSI, strike, side, expiry), with a cheap strike/side/underlier prefilter before any `definitions.find`. Dominates the ~16 s reconciliation phase.

**P3 — Definitions-parse hot loop (M4 + M5).** *High payoff, paid twice/pipeline.* (a) Memoize `trade_end` by distinct `trade_date` — eliminates ~8.7M `iso_to_ns(trade_date + "...Z")` heap-concat+parse calls (only ~60 distinct dates). (b) Replace the two-level `split` with a single forward pass over the 9 fixed tab boundaries — eliminates ~8.7M per-row `vector<string_view>` allocations + the ~140MB line-index vector. Together, a large fraction of the ~12 s parse. Pairs with a create-from-trusted-sorted-bytes constructor that also skips the unused-fingerprint re-serialization on the backtest read path (L3 — but keep the fingerprint for the generator diagnostic + the `listed_opra_test.cpp` assertion).

**P4 — One parallel range OPRA batch instead of ~60 serial single-date batches (L5).** `load_opra_daterange` already fans per-file reads over `n_threads`, but `load_listed_quotes` pins `date_lo=date_hi=date`, so reconciliation launches ~60 sequential batches, draining/refilling the jthread pool each time with a tail-latency join barrier. Issue one range call over `[date_lo,date_hi]` for all symbols, then bucket returned entries by `date`. Recoverable time is bounded (51 files already saturate typical core counts per batch), so this is a tail-imbalance + spawn/join win; **combine with P2** — a targeted range join compounds both. (Naive fix iterates calendar days including ~1275 cheap NotFound cells; bucket by trading day.)

**P5 — Route the projected-backtest divergence replay through the shared cache (L4).** `write_mark_divergence_replay` loads each archive with static `MarketSnapshot::load`, bypassing `config.snapshot_cache`, so the subsequent priced `run_backtest` re-deserializes the same ~60 archives (cache empty). Route the replay's loads through `config.snapshot_cache->load(...)`. **Structural fix (L10):** the `StepObserver` engine hook eliminates the shadow pass entirely — one real run captures divergence — which is both a correctness-fragility win (no drift between shadow and engine) and this perf win.

**P6 — Avoid the second full-file read on roll dates (L7).** build-schedule loads each roll archive via `MarketSnapshot::load` then re-reads the whole file via `hash_file` for `surface_fingerprint`. Only ~3–6 roll dates, so minor, but derive `surface_fingerprint` from the v2 header content-identity computed at load (`snapshot_cache.cpp:94-110`) or hash the already-mapped bytes.

**P7 — Subset archive deserialize (L6, conditional).** Snapshot reuse inside run-backtest is already optimal (reconciliation loads are cache hits). The only remaining per-archive cost is whole-board deserialize + per-surface `with_query_pricing` prep, taken because the shared cache threads empty `referenced_uids`. If corpus archives ever hold *more* than the universe, thread a referenced-uid subset; otherwise this is not a lever — cross-process persistence (P1) is.

**Compounding note.** P1 subsumes the *repeated* cost of P2/P3/P4 across a sweep; P2/P3/P4 attack the *single-run* floor. Sprint sequencing: land P1's cache substrate on top of the `RunArchive` content-identity (Section 5), then P2+P4 as one targeted-join change, then P3 as an isolated parser pass, then the P5 hook alongside the L10 observability change.

---

*End of report. All economics-migration work is gated on preserving the Section 6 invariants (especially I1 two-route cold parity and I7 schedule re-validation / parity.py column names); the M1 clock-coupling defect should be fixed at the `build_listed_dispersion_schedule` / `reconcile_listed_schedule` seam as the first economics extraction, since it is the one CONFIRMED correctness-robustness defect and it lands naturally where the timeline-trim belongs.*