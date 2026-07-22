# Design — atx-vol Backtest Framework Productionization

**Date:** 2026-07-21
**Status:** approved-in-substance (design forks resolved); pending written-spec review
**Grounding review:** [`2026-07-21-atx-vol-backtest-review.md`](2026-07-21-atx-vol-backtest-review.md) (39-agent orchestrated code review, 32 adversarially-verified findings)
**Execution:** subagent-driven development (SDD), Opus 4.8 implementers, directly on local `main`.

---

## 1. Problem

The listed-dispersion backtest pipeline is economically correct but structurally mis-layered.
The real orchestration and ~300–400 LOC of route-defining quant economics live inside a single
1,253-line example binary (`atx-vol/examples/spy_dispersion_backtest.cpp`), not in the library.
Results are ~20 loose flat files with no manifest, no checksums, and a 25-column schema
hand-duplicated in four places that has already drifted. Every subcommand is its own process
with a fresh in-memory cache, so parameter sweeps re-pay ~28 s of invariant I/O per point.

The surface-side of atx-vol already has a SOTA custom storage format (ATXVSA2: mmap columnar,
CRC-layered, schema-hashed, atomic write). Backtest **results** have no such format. This sprint
closes that gap and pulls the pipeline into the library.

## 2. Goals & scope

**In scope (all waves A–E, per user decision):**

- **A.** Result-store: new `run_diagnostics` + `run_archive` modules; the binary **RunArchive**
  (`run.atxrun`) container becomes the single result store.
- **B.** `listed_dispersion_pipeline` module: migrate the stranded economics under test; fix the
  one confirmed correctness defect (M1) at the new seam.
- **C.** `backtest_driver` spine (`BacktestJob`/`run_backtest_job`): the strategy-agnostic
  general framework; migrate `strangle` and `mag7` (and the other example drivers) onto it.
- **D.** Engine `StepObserver` hook on `RunConfig` (kills the shadow-replay double pass);
  de-SPY `dispersion_workflow` (`RunSpec.index_symbol`).
- **E.** Performance passes P1–P7 (persistent cross-run cache, targeted reconciliation join,
  definitions-parse hot loop, parallel range OPRA batch, cache-routed replay, etc.).

**Migration model — HARD CUTOVER (per user decision):** RunArchive is the *only* machine-read
result store. No dual-write of legacy TSVs. Every consumer (the Python report `parity.py`/`io.py`,
mag7's five-CSV renderer, the `verify` subcommand, any downstream harness) is ported to read the
archive in this sprint. A `runarchive dump <section> --tsv` command exists as an on-demand
escape hatch for ad-hoc inspection, but nothing in the pipeline depends on loose TSVs.

**Out of scope:** new strategies; changing the economics/results themselves (the refactor must be
value-preserving); the 696 MB `definitions.tsv` input format; OCC-ESS compliance-evidence text
artifacts (stay as authored/compliance text, hash-referenced from the archive `meta`).

## 3. The one correctness defect to fix (gate)

**M1 — reconciliation clock coupling.** `reconcile_listed_dispersion`
(`src/listed_dispersion_reconciliation.cpp:240-243`) hard-requires
`clock[0] == schedule.rolls.front().roll_date`, strictly stronger than the engine's precondition.
Any leading warm-up / low-coverage session aborts `run-backtest` on an otherwise-valid corpus; it
works today only because `date_lo` coincides with the first roll. **Fix at the new seam:** the
reconciliation-snapshot assembler trims the timeline to start at the first roll date (mirroring
`ListedDispersionStrategy::on_step` emitting zero rows pre-roll), and the coupling is
enforced/validated at schedule build, not left emergent. This is the first economics extraction
in wave B because the fix lands naturally where the timeline-trim belongs.

## 4. Target library architecture

Four new modules + two in-place changes. Strategy-agnostic pieces are named so strangle/mag7 reuse
them without knowing about dispersion; dispersion economics land in one labeled module. The example
CLI collapses **1,253 → ~200 lines** (arg-dispatch + one library call per subcommand).

### 4.1 `atx/vol/run_diagnostics.{hpp,cpp}` — diagnostics (strategy-agnostic)
Verbatim lift of `PhaseTimer` and `write_diagnostics` out of the example. Named-phase
wall-time/count accumulator + the versioned diagnostics writer (keep the existing
`ATX_DISPERSION_DIAGNOSTICS` magic — do not silently rename) + stderr summary. Under hard cutover
the diagnostics also become a RunArchive `diagnostics` section; the standalone TSV emit is retired
except via `dump`.

### 4.2 `atx/vol/run_archive.{hpp,cpp}` — result store (strategy-agnostic)
A `RunDir` handle owning the run-directory envelope and the single source of truth for: the artifact
set, the 25-col `kBacktestSeriesColumns` schema (replacing the 4-place duplication), the uniform
magic/version/CRC header, and the `verify()` gates. Absorbs `write_input_inventory`,
`write_methodology_map`, `persist/verify_occ_ess_evidence`. Typed accessors: `dir.spec()`,
`dir.clock()`, `dir.schedule()`, `dir.write_backtest(r)`, `dir.section(name)`, etc. Writes the
binary container defined in §5.

### 4.3 `atx/vol/backtest_driver.{hpp,cpp}` — strategy-agnostic spine
```cpp
struct BacktestJob {
  ClockSource      clock;     // synthetic-build | manifest-file | SurfaceDb whole/windowed
  StrategyFactory  strategy;  // IStrategy&  OR  {DispersionUniverse, DispersionBacktestConfig}
  RunConfigOverlay overlay;   // frictions / tier / adaptive-confirm / preload / cache sizing
  OutputProfile    outputs;   // RunArchive (+ dump escape hatch)
  MetaKv           meta;
};
run_backtest_job(job) -> { BacktestResult, TearSheet, EngineRunStats };
```
Owns the fixed 9-stage spine (clock-build → strategy-construct → timed `run_backtest` → `tearsheet()`
fold → `EngineRunStats` capture → `OutputProfile` emit → console summary → exit-code convention).
The **engine slot** abstracts `run_backtest` vs `run_dispersion_backtest` vs the tradeable manual
evaluator behind `→ BacktestResult`. Deletes the spine duplicated across the six example drivers.

### 4.4 `atx/vol/listed_dispersion_pipeline.{hpp,cpp}` — dispersion economics (the listed-route home)
- `listed_quotes_for_date(spec, definitions, symbols, date)`
- `make_listed_forward_lookup(SurfaceSet&)`, `make_listed_risk_lookup(...)`
- `build_listed_dispersion_schedule(clock, ListedScheduleSpec, universe_rows, definitions, quote_source) -> Result<ListedDispersionSchedule>`
  — cadence, coverage gate, deferral, cohort numbering, `surface_fingerprint`, acceptance gate;
  **M1 fix lands here** (first roll date known and enforced against the clock).
- `project_listed_schedule(listed, ArchiveLookup, ProjectionConfig{analytic, QueryExecution}) -> ListedDispersionSchedule`
  — the cold ATM-forward idealization; **shares one code path / one asserted constant with the
  projected-backtest replay** so the two-route parity (I1) is guarded at compile time, not by luck.
- `assemble_reconciliation_snapshots(...)` + `reconcile_listed_schedule(...)` — owns the M1 timeline-trim.
- `dispersion_book_var(book, scenarios, confidences) -> {frames, legs, ProjectedHistoricalVar}`.
- `constexpr double kVegaVolPointToUnitVol = 100.0` — replaces the hand-applied ×100 at two boundaries.
- `struct ListedDispersionMethodology` — admission rule + fit template + acceptance thresholds
  (`51/60/3/40`) + query-route + occ-ess-authority flag + `policy_fingerprint`; replaces loose inline literals.

### 4.5 `atx/vol/backtest.hpp` — CHANGED
Add an optional `StepObserver` to `RunConfig`, fired after each `on_step` with strategy access, so
mark-divergence capture reuses the single real engine run instead of the `write_mark_divergence_replay`
shadow loop (removes the double engine pass; removes shadow-vs-engine drift risk).

### 4.6 `atx/vol/dispersion_workflow.hpp` — CHANGED
Keep as the pure config/input front-end; add `RunSpec.index_symbol` to drop the hardcoded `"SPY"` in
`all_symbols`/`universe_at`. New methodology knobs go to `ListedDispersionMethodology`, not `RunSpec`.

### 4.7 Thin-CLI shape
`main()` arg-dispatch kept; each subcommand becomes one library call, e.g.
`build-schedule` → `d.write_schedule(build_listed_dispersion_schedule(d.clock(), method, d.universe_rows(), d.definitions(), quote_source));`
`verify` → `d.verify(method);`. Process-boundary independence (I8) preserved: each stage still runs
standalone from disk state (now the archive section is the inter-stage wire).

## 5. RunArchive format (`run.atxrun`, magic `ATXRUN01`)

Ports the ATXVSA2 skeleton verbatim (`include/atx/vol/surface_archive.hpp:450-579`): one contiguous
mmap region; byte-offset **section directory = manifest** (no pointers → no relocation on map);
columnar SoA sections; header + metadata + per-section CRC-32C; `sizeof`-fold + per-column
`{name,dtype,unit}` table `schema_hash`; atomic `.tmp`+rename write; `ArchiveContentIdentity`
`{file_size, created_ts_ns, header_crc32c, metadata_crc32c}` for cache keys. Reuses
`detail::crc32c`/`crc32c_update`/`align_up` (`detail/archive_util.hpp`) and the read-only mmap seam
`tsdb::Mapping` via `open_borrowed(span, owner)`.

**Container:** `RunArchiveHeader` (256 B, naturally aligned; carries `schema_hash`,
`writer_version_hash`, and a content-derived `run_identity_hash` over run_spec bytes + input
fingerprints — the cross-run cache key of §7/P1) → `SectionDescriptor[]` (sorted by
name → O(1) find; carries a copy of each section's `payload_crc32c` so `metadata_crc32c` covers it —
the F6 content-identity trick) → 64-B-aligned self-contained sections. Each section:
`SectionHeader` (magic, record_size, n_rows, n_cols, payload_crc32c-own-zeroed) → `ColumnDescriptor[]`
(name, dtype ∈ {f64,i64,u32,u8-enum,dict-str}, record-relative offsets) → contiguous typed column
arrays. String columns = Arrow-style u32 code + per-section string table; enum columns = u8 code +
in-section label table (mapping travels with the data — fixes the undocumented-ordinal fragility).

**Sections:** `meta` (ScalarKV: resolved spec echo, window, roll-level scalars, input hashes,
counts), `backtest` / `projected_cold` / `projected_nodiv` (TimeSeries SoA: `ts_ns` + 25
`BacktestResult` f64 columns + per-signal columns), `reconciliation` (TimeSeries, 11 cols),
`trade_schedule` / `projected_schedule` (SubTable rolls×legs), `contract_marks` (SubTable), 
`mark_divergence` (SubTable), `diagnostics` (SubTable).

**Integrity/open:** `open()` validates magic+version+endian+pointer_bits+`schema_hash`+header CRC+
metadata CRC+section bounds only; per-section CRC is lazy (`validate_section(name)`), never on the
read path. mmap open faults only header+directory pages; reading one section touches only its extent.

**Schema single-source:** one `constexpr` column-descriptor registry in `run_archive_schema.hpp`
that (a) the C++ writer iterates to emit columns, (b) folds into `schema_hash`, (c) a codegen step
exports to a Python module — killing the four-place duplication and the divergent `nav` order.

**Python read story (hard cutover):** a pure-Python `python/src/atxvol/report/runarchive.py`
(no binding import): mmap the file; `struct.unpack` the fixed 256-B header; assert
magic/endian/pointer_bits; **recompute `schema_hash` from the generated descriptor and compare —
drift fails at `open()`, not at row access.** Columns returned as zero-copy `numpy.frombuffer` views;
dict-str decoded via the section string table; u8-enum via the label table; no per-cell `float()`.
`parity.py`/`io.py` are rewritten to call `archive.section(name)` — column access goes through the
generated descriptor, so a future C++ column rename is caught at open, not at render.

## 6. Correctness invariants (regression targets — must not change)

- **I1 — Two-route cold parity (bit-exact).** `project_listed_schedule` and the
  `run-projected-backtest --execution cold` replay must share one code path / one asserted constant
  (both `analytic=true` + `QueryExecution::ColdReference`). Add a parity test asserting leg-mark equality.
- **I2 — Settlement fail-closed.** Expiry must land exactly on a snapshot ts or `compute_step`
  returns Err; preserve, and *document* the "corpus ends before final cohort expiry" precondition.
- **I3 — One-sided execution gate.** Fires only when `required==ColdReference` AND a fast tier is
  prepared AND `query_execution != ColdReference`. Do not make it two-sided.
- **I4 — Vega-neutral sizing identity.** Index short `= -gross`; names sum to `+gross`; net `== 0`.
  The `kVegaVolPointToUnitVol = 100.0` extraction must produce byte-identical sizing.
- **I5 — Reconciliation entry-mark equality basis.** `entry_mark_tolerance` default 0.0 demands
  bit-exact `fair_value()` vs schedule `evaluate().price`; preserve the LegacyCompatible-tier
  assumption or unify the two pricer entry points so equality holds by construction.
- **I6 — `all_rolls_consumed` gates.** Retained at every subcommand boundary in the thin CLI.
- **I7 — Output value-stability where consumers key on it.** Schedule re-validation on read must
  reproduce re-validating values; parity.py column names + mag7's five pinned CSV filenames are
  binding until their consumers are ported (which, under hard cutover, is this sprint).
- **I8 — Process-boundary independence.** Each stage independently runnable from disk state.

## 7. Performance opportunities (wave E, ranked by payoff)

- **P1 — Persistent cross-run result cache (highest).** On-disk cache keyed by (corpus fingerprint,
  definitions fingerprint, universe, date range): pre-parsed binary definitions blob + per-date
  joined quotes for scheduled contracts. Deeper win: cache the *selection* so a `gross_index_vega`
  sweep recomputes only vega arithmetic. Uses RunArchive `run_identity_hash` + `ArchiveContentIdentity`.
- **P2 — Targeted reconciliation OPRA join.** Leg-key-filtered join fed the union of frozen scheduled
  contract keys (~102 legs) instead of OSI-parsing the whole panel (~100× over-production).
- **P3 — Definitions-parse hot loop.** Memoize `trade_end` by distinct `trade_date` (~60 distinct
  vs 8.7M rows); single forward pass over the 9 fixed tab boundaries (no per-row `vector<string_view>`).
- **P4 — One parallel range OPRA batch** over `[date_lo,date_hi]` bucketed by date, instead of ~60
  serial single-date batches. Compounds with P2.
- **P5 — Route the divergence replay through the shared `SnapshotCache`** (or eliminate it via the
  P/L StepObserver hook — one real run captures divergence).
- **P6 — Avoid the second full-file read on roll dates** (derive `surface_fingerprint` from the v2
  header content-identity at load).
- **P7 — Subset archive deserialize** (conditional; only if corpus archives are broader than the universe).

## 8. Sequencing (landing order — each keeps the CLI compiling against a shrinking shim)

1. **Wave A** — `run_diagnostics` + `run_archive` + RunArchive format + schema single-source +
   pure-Python reader. Mechanical lift, no economics. Fixes M10/M11/M12/L13/L14.
2. **Wave B** — `listed_dispersion_pipeline`: extract the (c) economics under test; **M1 fix**;
   I1 parity test. Closes M6/M7/M8/M9/L9. Thin the dispersion CLI.
3. **Wave C** — `backtest_driver` spine; migrate strangle/mag7/other drivers; port mag7's renderer
   to RunArchive. Closes L11.
4. **Wave D** — engine `StepObserver` hook (L10); de-SPY `dispersion_workflow` (L12).
5. **Wave E** — perf passes P1–P7.

## 9. Testing strategy

- Unit tests for every migrated economic function (previously unreachable in the example): schedule
  build, projection, VaR, reconciliation assembly, methodology fingerprint.
- RunArchive: round-trip (write → open → section == source), schema-hash drift rejection, CRC
  tamper rejection, mmap subset read, Python reader parity with C++ writer, atomic-write crash safety.
- Invariant regression tests I1–I8 (I1 two-route parity is the headline gate).
- End-to-end: the 135-session parity-full run reproduces the validated economics (final NAV,
  daily-pnl correlation, zero mark divergence) reading from RunArchive.
- Byte/valued-stability gates on any artifact whose consumer is not yet ported within the same wave.

## 10. SDD execution

- Controller authors per-task briefs; Opus 4.8 subagent implementers; Opus/Fable reviewers.
- Directly on local `main`, in place. Explicit-path commits only — never `git add -A/-u/.` (the tree
  carries unrelated uncommitted work). One build at a time, Release preset, shared `C:\atx-cache\deps`.
- Do not modify golden fixtures. Controller owns `C:\atx-data` run dirs (subagents never touch them).
- Commit trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- Detailed task decomposition + acceptance gates: produced by the writing-plans step from this spec.
