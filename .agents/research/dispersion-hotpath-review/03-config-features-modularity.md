# Dispersion backtest — CONFIGURABILITY / MISSING-or-UNWIRED / API-MODULARITY (axes 3-4-5)

Reviewer: senior API/architecture reviewer (read-only). Repo `C:/atx`, module `atx-vol/`, main HEAD.
Scope verified by reading in full: `dispersion.hpp/.cpp`, `dispersion_backtest.hpp/.cpp`,
`dispersion_strategy.cpp`, `strategy.hpp` (+ `lifecycle_decide`), `dispersion_workflow.hpp/.cpp`,
`backtest.hpp`, `tearsheet.hpp`, `listed_dispersion.hpp`, `examples/spy_dispersion_backtest.cpp`,
`spy_dispersion_run_spec.tsv`, `spy_dispersion_universe.tsv`, CMake target. Cross-checked 06/04 review
docs and re-verified every claim by grep. NO build, NO edits.

Orientation: two dispersion families share `dispersion.hpp` sizing but diverge downstream.
(1) **Straddle-book / surface backtest** — `run-surface-backtest` → `run_dispersion_backtest` →
`DispersionBacktestConfig` → `DispersionStrategy` → `build_dispersion_book`. THE SOTA speed path.
(2) **Listed proxy** — `run-backtest` → schedule/reconcile → `ListedDispersionStrategy`.
This review centers on (1); (2) and the strangle-DSL family (`make_dispersion_strangle_spec`, used by
`mag7_dispersion_backtest`/`spy_dispersion_pnl`) are noted where they explain a gap.

Config flows through **FOUR separate structs** with lossy hand-wiring between them:
`RunSpec` (TSV, ~20 keys, `dispersion_workflow.hpp:16`) → `DispersionBacktestConfig` (9 fields,
`dispersion_backtest.hpp:15`) → `DispersionConfig` (sizing, `dispersion.hpp:155`) + `RunConfig`
(engine, `backtest.hpp:300`). The CLI (`run_surface_backtest_command`,
`spy_dispersion_backtest.cpp:523-584`) copies only **6** RunSpec fields into
`DispersionBacktestConfig`; the rest are silently dropped for this path.

---

## (i) CONFIG KNOB TABLE

Honored? = end-to-end effect on the **run-surface-backtest** (straddle-book) path unless noted.

### A. RunSpec TSV keys (`dispersion_workflow.cpp:65-149`, `spy_dispersion_run_spec.tsv`)

| Knob | Source | Honored (surface bt)? | Default | Notes |
|---|---|---|---|---|
| `label` | TSV | cosmetic only | "SPY…proxy" | persisted, methodology map |
| `date_lo`/`date_hi` | TSV | fit-time only | — | build-corpus window; surface bt reads manifest |
| `snapshot_suffix` | TSV | fit-time only | `T19:55:00Z` | OPRA snapshot minute; irrelevant to reload path |
| `opra_root`/`path_template` | TSV | fit-time only | — | build-corpus |
| `universe_schedule` | TSV | **YES** | — | `universe_at` PIT resolve |
| `definitions` | TSV | **NO (listed only)** | empty | build-schedule/run-backtest |
| `occ_ess_root` | TSV | **NO (listed only)** | empty | evidence gate for listed path |
| `flat_rate` | TSV | **fit-time only** | 0.043 | feeds `batch_spec.r` at fit; NOT the backtest financing rate (financing off; when on it reads `surfaces().front().pricing().r`, 04-M4) |
| `min_names` | TSV | **YES** | 10 | → `MissingNameSpec.min_names` |
| `min_weight_coverage` | TSV | **NO (listed only)** | 0.8 | build-schedule roll gate |
| `target_dte_days` | TSV | **YES** | 30 | → `target_T`/projected days |
| `min_dte_days` | TSV | **NO (listed only)** | 21 | `ListedDispersionSelectionConfig` only |
| `max_dte_days` | TSV | **NO (listed only)** | 60 | `ListedDispersionSelectionConfig` only |
| `roll_dte_days` | TSV | **YES** | 7 | → `roll_at_T` |
| `gross_index_vega` | TSV | **YES** | 10000 | → `target_vega` |
| `delta_band` | TSV | **YES** | 0 | → hedge band |
| `fit_workers` | TSV | **NO (fit/proj-var only)** | 0 | surface bt pricer threads = `RunConfig.price.n_threads`=0 (all cores), unrelated |
| `core_mode` | TSV | gate only | 0 | breadth/date acceptance gates |

### B. DispersionBacktestConfig (library surface, `dispersion_backtest.hpp:15`)

| Knob | Honored? | Default | Should be configurable? |
|---|---|---|---|
| `target_dte_days` | YES | 30 | ok |
| `roll_dte_days` | YES | 7 | ok |
| `gross_index_vega` | YES | 10000 | ok |
| `delta_band` | YES | 0 | ok |
| `min_names` | YES | 2 | ok |
| `entry_every_n` | **PLUMBED-BUT-IGNORED** | 21 | it sets `lifecycle.entry_every_n` (`dispersion_backtest.cpp:28`) but `lifecycle_decide` under `RollAtHorizon` never reads it (`strategy.cpp:816-824`) — dead in this path; misleading |
| `project_to_calendar_expiry` | YES | true | ok |
| `record_diagnostics` | honored in `signals()` but **CLI never sets it true** → implied-corr signal inert in the benchmark (A3) | false | expose |
| `run` (RunConfig) | YES, but CLI sets only `run.unpriced=Error` | {} | everything else stays default |

### C. DispersionConfig — sizing core (`dispersion.hpp:155`)

| Knob | Honored by sizing? | Exposed to CLI/TSV? | Should be? |
|---|---|---|---|
| `target_T` | YES (unless `projected_maturity` set) | via target_dte_days | ok |
| `target_vega` | YES | via gross_index_vega | ok |
| `side` (`DispersionSide`) | YES in `build_dispersion_book` | **NO — hardcoded `ShortIndexLongNames`** (`dispersion_backtest.cpp:16`, `spy_dispersion_backtest.cpp:612`) | **YES** — cannot run reverse/long-index dispersion |
| `multiplier` | YES | **NO — hardcoded 100** everywhere | maybe (per-name multiplier) |
| `missing` (policy+min) | YES | via min_names (policy fixed DropRenormalize) | policy should be selectable |
| `projected_maturity` | YES | via project_to_calendar_expiry | ok |
| `record_diagnostics` | YES | see B | expose |

### D. RunConfig — engine, as applied to dispersion (`backtest.hpp:300`)

| Knob | Honored? | CLI/TSV? | Should be? |
|---|---|---|---|
| `price{n_threads=0, analytic_greeks=true}` | YES (default) | NO | expose threads |
| `query_pricing_tier` | YES (LegacyCompatible) | NO | expose (perf knob) |
| `frictions` (`FrictionModel`) | YES but **always OFF** | **NO knob** | **YES** — dispersion is always frictionless (mid fills, 0 cost) |
| `financing` (`FinancingConfig`) | YES but **always OFF** | **NO knob** | **YES** — no borrow/carry; `flat_rate` not wired here |
| `record_every_n` | YES (1) | NO | leave 1 (H1 corruption if >1) |
| `unpriced` | set to `Error` by both CLI paths | forced | ok (fail-closed) |
| `snapshot_cache` | surface path null (private) / listed sets one | NO | minor |
| `query_cache_build_policy` | YES (Eager) | NO | perf knob |
| `surface_provenance_policy` | YES but **always Compatibility** | **NO knob** | **YES** — a production run cannot demand `RequireAdmittedRisk` via config |
| `settlement_mark_memo` | YES (true) | NO | ok |

### E. Fit config — build-corpus, ALL HARDCODED in the driver (`spy_dispersion_backtest.cpp:251-271`)

| Knob | Value | Configurable? |
|---|---|---|
| `FitPreset` | `Hft` | **NO — hardcoded** |
| curve kind | `LinearVariance` | **NO — hardcoded** |
| `enforce_calendar_floor` | true | **NO** |
| admission `min_quotes` | 20 | **NO** |
| admission `min_slices` | 2 | **NO** |
| admission `require_calendar_arb_free` / `calendar_abs_k` | true / 0.7 | **NO** |
| admission `require_source_provenance` | true | **NO** |
| `policy_fingerprint` string | pinned literal | **NO** |

**Verdict (config surface):** typed at the LEAF (`DispersionConfig`/`RunConfig` are clean typed
structs), TSV-SOUP at the SEAM. `RunSpec` is a flat `std::map<string,string>` (`dispersion_workflow.cpp:67`)
hand-mapped field-by-field, and only ~6 of its keys reach the surface backtest; ~7 keys are honored
only on OTHER subcommands and are silently inert here. The most damaging **hardcoded-should-be-configurable**
items: (1) **side** (locked long-dispersion), (2) **frictions** (always frictionless), (3) **financing/borrow**
(always off; flat_rate misrouted), (4) **hedge cadence/kind** (locked Daily/DeltaToZero),
(5) **fit preset/curve/admission** (locked in the driver), (6) **strike rule** (locked ATM-forward
straddle — no delta/strangle in this path), (7) **surface-provenance policy** (locked Compatibility).

---

## (ii) FEATURES MISSING / UNWIRED — by severity

### HIGH
1. **No transaction-cost / borrow / financing wiring for dispersion.** `FrictionModel` + `FinancingConfig`
   exist on `RunConfig` (`backtest.hpp:257,269`) and are honored by the engine, but neither
   `DispersionBacktestConfig` nor the run spec exposes them; the surface + listed dispersion runs are
   ALWAYS frictionless mid-fills with no borrow/carry. *(realism)*
2. **No risk limits / capital / drawdown stop** anywhere in the dispersion or engine loop (no
   max-vega/gamma/notional, no margin, no circuit-breaker). Backtests run unconstrained. *(04-review confirmed)*
3. **Only one weighting scheme + one strike rule.** Sizing is index-weight-normalized ATM-forward
   straddles only (`dispersion.cpp:488-496`, K=`forward_at(T)`). No equal-vega / inverse-vol /
   equal-notional weighting; no delta-strike or strangle structure in `build_dispersion_book` (the
   strangle variant is a *separate* family, `make_dispersion_strangle_spec`). *(feature breadth)*

### MEDIUM
4. **`entry_every_n` plumbed-but-ignored.** `dispersion_backtest.hpp:21` → `dispersion_backtest.cpp:28`
   sets `lifecycle.entry_every_n`, but `RollAtHorizon` in `lifecycle_decide` (`strategy.cpp:816-824`)
   never reads it. A user tuning "entry cadence" gets no effect.
5. **`min_dte_days` / `max_dte_days` / `min_weight_coverage` ignored by the surface backtest.** Parsed +
   range-validated (`dispersion_workflow.cpp:131-133,142-144`) but consumed only by the listed
   `select_listed_dispersion` path. On `run-surface-backtest` the DTE window is a single point
   (target only). *(silent no-op knobs)*
6. **`DispersionSide` hardcoded** to `ShortIndexLongNames` in `make_dispersion_backtest_strategy`
   (`dispersion_backtest.cpp:16`) and `run_projected_var` (`spy_dispersion_backtest.cpp:612`) — reverse
   dispersion impossible without editing code.
7. **Hedge kind + cadence hardcoded** `DeltaToZero`/`Daily` (`dispersion_backtest.cpp:31-33`); only the
   band is configurable. No hedge-frequency sweep, no cheaper delta-only mask (04-A6).
8. **`verify` needs an artifact no C++ writes.** `verify_command` (`spy_dispersion_backtest.cpp:447`)
   requires `reference_reconciliation.tsv`, produced only by external `tools/reference_spy_dispersion.py`
   — a cross-language gate (06-A8).
9. **`run-projected-var` is half-wired.** Full subcommand (`spy_dispersion_backtest.cpp:586-702`,
   emits `projected_var.tsv`/`projected_risk_*.tsv`) but NOT part of the `verify` gate, has **no test**
   (grep: referenced only in the example + sprint docs), and re-hardcodes side/multiplier.
10. **Fit preset / curve / admission thresholds hardcoded** in `build_corpus_command`
    (`spy_dispersion_backtest.cpp:251-271`) — no way to sweep fit config from the run spec.
11. **No benchmark-relative stats + surface path emits no tearsheet.** `TearSheet` (`tearsheet.hpp:40`)
    is absolute-only (no beta/alpha/IR/tracking-error), AND `run-surface-backtest` writes only the raw
    SoA `surface_backtest.tsv` (`spy_dispersion_backtest.cpp:580`) — it never calls `tearsheet()`, so the
    CLI produces no headline stats at all for this path.
12. **`record_diagnostics` never enabled by the CLI** → implied-correlation signal (`dispersion_signal`)
    is dead weight in the benchmark (06-A3).

### LOW
13. **`flat_rate` mis-routed / `fit_workers` not plumbed** to the surface backtest (rate→fit only,
    threads→default pool). *(rate-source ambiguity, 04-M4)*
14. **`surface_provenance_policy` un-exposed** — production strictness (`RequireAdmittedRisk`) unreachable via config.
15. **`build_book` / `dropped_on` accessors** (`dispersion_strategy.cpp:214,223`) are exercised only by
    tests, never by the drivers — parity/diagnostic seams, not production-wired (not dead: tests call them).
16. **No scenario/parameter sweep harness** over (side, tenor, vega, delta_band, universe);
    `run-projected-var` parallelizes historical scenarios of ONE book, not a config grid.
17. **No intraday / multi-snapshot, no corporate-actions** in the dispersion path (one snapshot/day;
    GOOG/GOOGL dedup hand-coded elsewhere). *(06-review B(b))*

**Dead-code proof:** no true dead code in the dispersion sizing/strategy stack — every public symbol
(`build_dispersion_book`, `dispersion_signal`, `with_uid`, `resolve_universe_uids`, `build_book`,
`dropped_on`) has a caller in `examples/` or `tests/` (grep-verified). The issues above are
*plumbed-but-ignored config* (#4,5,12) and *un-exposed capability* (#1,2,6,7,10,14), not orphaned code.

---

## (iii) API / ROBUSTNESS / MODULARITY assessment

**Usable as a library? PARTIALLY — output side yes, input side no.**

POSITIVES
- **Typed entry point exists:** `run_dispersion_backtest(const Clock&, DispersionUniverse,
  const DispersionBacktestConfig&) -> Result<BacktestResult>` (`dispersion_backtest.hpp:37`). The header
  explicitly states the example CLI is "intentionally limited to parsing files and writing artifacts;
  strategy, lifecycle, hedge, and engine defaults live here for library callers."
- **Typed result:** `BacktestResult` (`backtest.hpp:390`) is a real SoA result — full PnL track,
  8-axis Taylor attribution, settlement/shares/financing/cost lanes, book greeks, `n_unpriced_*`,
  full-resolution `step_pnl_total`, and named `signals`. `tearsheet(BacktestResult) -> TearSheet`
  (`tearsheet.hpp:80`) is a pure typed fold. Results are NOT merely TSV side effects.
- **Clean separation:** sizing (`dispersion.cpp`, pure/stateless — reads a `SurfaceSet`, returns a
  book, no clock/IO), strategy adapter (`dispersion_strategy.cpp`), engine (`backtest.cpp`), analytics
  (`tearsheet.cpp`), and IO/parse (`dispersion_workflow.cpp`) are properly layered. The listed
  selection/schedule/reconcile modules are independently testable.
- **Robust degradation:** missing-name handling is first-class and typed (`MissingNamePolicy`,
  `DroppedName`, `MissingNameSpec`). Under `DropRenormalize` a missing name is dropped, weights
  renormalize over survivors preserving vega-neutrality, and a below-`min_names` date becomes a
  documented no-trade (`dispersion_strategy.cpp:120-128`) — the run DEGRADES, it does not abort. The
  index leg is never droppable (always fatal if missing). `unpriced=Error` fail-closes held-valuation.
  Extensively unit-tested (`dispersion_test`, `multiname_pipeline_test`, `strategy_test`, `corpus_test`).

NEGATIVES
- **Lossy config seam:** four config structs, hand-wired, no single typed spec. `read_run_spec` returns
  a `RunSpec` that the CLI destructures field-by-field into `DispersionBacktestConfig`; unknown keys
  are neither rejected nor forwarded, and ~half the keys are silently inert on a given subcommand
  (§ii #5,13). No typed round-trip from TSV → the config the library actually consumes.
- **Orchestration trapped in the 761-LOC example `main`:** corpus build, `Clock` construction,
  reconciliation glue, all artifact persistence, and the profile/counters emit live in
  `spy_dispersion_backtest.cpp`, not in a library. A second consumer must re-glue manifest→Clock,
  snapshot loading, and every TSV writer. Only the strategy+engine call is library-ized.
- **No typed outcome bundle:** callers get a `BacktestResult` but must separately call `tearsheet`,
  collect drops (`dropped_on`), and re-derive per-roll attribution; there is no
  `DispersionBacktestOutcome` aggregating track + sheet + drops + provenance.

### Proposed typed public library API

Collapse the four structs into ONE typed spec that the TSV deserializes into (unknown key = error),
and return a typed outcome instead of TSV side effects:

```cpp
enum class WeightingScheme { IndexWeight, EqualVega, InverseVol, EqualNotional };   // NEW
struct StrikeRule { enum K { AtmForwardStraddle, DeltaStrangle } kind; double delta; }; // NEW

struct DispersionRunConfig {
  DispersionSide          side;          // EXPOSE (was hardcoded)
  double                  gross_index_vega, multiplier;
  WeightingScheme         weighting;     // NEW
  StrikeRule              strike;        // NEW
  struct { double target, min, max, roll; } dte;   // min/max actually honored
  struct { LifecycleSpec::Entry entry; unsigned every_n; } lifecycle;
  HedgeSpec               hedge;         // kind+cadence+band all exposed
  FrictionModel           frictions;     // EXPOSE
  FinancingConfig         financing;     // EXPOSE + wire flat_rate here
  MissingNameSpec         missing;       // policy selectable
  RiskLimits              limits;        // NEW: max vega/gamma/notional, dd stop
  struct { unsigned threads; QueryPricingTier tier; SurfaceProvenancePolicy provenance;
           bool record_diagnostics; } engine;
  FitConfig               fit;           // NEW: preset/curve/admission for build-corpus
};

struct DispersionBacktestOutcome {       // NEW typed bundle
  BacktestResult                 track;
  TearSheet                      sheet;
  std::vector<DatedDrops>        drops;      // per-date DroppedName
  std::vector<RollAttribution>   per_roll;   // per-cohort P&L/vega
};

Result<DispersionRunConfig>     read_dispersion_run_config(const fs::path&);   // typed, strict
Result<DispersionBacktestOutcome> run_dispersion_backtest(const Clock&, DispersionUniverse,
                                                          const DispersionRunConfig&);
Status write_dispersion_artifacts(const fs::path& dir, const DispersionBacktestOutcome&);
Result<Clock> open_dispersion_corpus(const fs::path& run_dir);   // manifest→Clock, lifted from main
```

### Five highest-value modularization moves
1. **One typed `DispersionRunConfig`** the TSV deserializes into directly (strict unknown-key
   rejection); delete the lossy `RunSpec`→`DispersionBacktestConfig`→`DispersionConfig` hand-mapping so
   no knob is silently dropped. Fixes §ii #4,5,12,13 at a stroke.
2. **Expose the already-plumbed engine capability** — `side`, `frictions`, `financing`,
   `surface_provenance_policy`, `query_pricing_tier`, hedge cadence/kind — through that config
   (the `RunConfig`/`DispersionConfig` fields already exist; only the seam is missing). Fixes #1,6,7,14.
3. **Return a typed `DispersionBacktestOutcome`** (track + tearsheet + drops + per-roll attribution)
   and move ALL persistence into `write_dispersion_artifacts`; the surface path should compute a
   `TearSheet` (fixes #11) and per-name/per-leg attribution instead of dumping raw SoA only.
4. **Lift corpus/Clock/reconciliation orchestration out of the 761-LOC example** into
   `dispersion_backtest.hpp` (e.g. `open_dispersion_corpus`, a reconciliation façade) so a second
   consumer isn't forced to re-glue `main`.
5. **Add the missing capability config blocks** — `WeightingScheme`, `StrikeRule`, `RiskLimits`, and a
   typed `FitConfig` (fixes #2,3,10) — plus a thin **scenario-sweep** driver over the typed config
   (fixes #16), reusing the existing `run-projected-var` scenario fan-out for the config grid.
