# atx-impl — Project Status

_Snapshot: 2026-06-19. Branch `main` @ `a7a1a73`._

`atx-impl` is the CLI driver for the ATX alpha research stack. It wires the
engine (`atx-engine`), core types (`atx-core`), and time-series store
(`atx-tsdb`) into an end-to-end options-alpha pipeline over ORATS data.

## What it does

Single binary (`atx-impl.exe`) with 8 subcommands. Typical chain:

```
load  ->  panel  ->  discover  ->  combine  ->  optimize  ->  report
```

| Subcommand | Source | Purpose |
|------------|--------|---------|
| `load`     | `stage_load.cpp`     | Raw ORATS zip -> `.seg` files |
| `panel`    | `stage_panel.cpp`    | Build filtered universe panel (`panel_enriched.bin`) |
| `discover` | `stage_discover.cpp` | Genetic search for alpha DSL expressions |
| `combine`  | `stage_combine.cpp`  | Blend admitted alphas into one signal |
| `optimize` | `stage_optimize.cpp` | Solve portfolio book weights |
| `report`   | `stage_report.cpp`   | Performance report (equity curve, pnl, capacity) |
| `run`      | `stage_run.cpp`      | Full pipeline from a config file |
| `regime`   | `stage_regime.cpp`   | Build a regime/macro `.seg` from staged CSVs |

Every stage prints a digest line: `[atx-impl] stage=<s> digest=<hex16> ...`.
Flag parsing lives in `config.cpp` (`apply_flag_value`); shapes in `config.hpp`.

## Build & test

- Toolchain: clang-cl + Ninja, build dir `build-rel` (Release, `/W4 /WX`).
- Dep cache: `C:/atx-cache/deps`. Third-party submodules required (e.g.
  `databento-cpp`) — run submodule init before configure on fresh/merged trees.
- CLI target: `atx-impl`.
- Tests: `atx-impl-tests` (this crate), plus engine suites
  `atx-engine-factory-tests`, `atx-engine-<module>-tests`.
- `atx-impl/tests/`: load, panel, discover, combine, optimize, report-capacity,
  book-shape, sector-groups, cli-smoke, e2e-pipeline, store-discover.

## Recently landed (on `main`)

- **Resumable discover (schema v2):** `--run-db` + `--resume`. Store-backed
  progress sink (`store_progress_sink.cpp`) persists run/checkpoint/iteration/
  event/log tables; checkpoint `state_hash` verified on resume (rejects corrupt
  blobs); full accumulated search state restored byte-identically.
- **Search quality (alpha-search-quality merge):** ramped + deduped grammar
  init; random immigrants + stagnation early-stop; adaptive operator selection
  with credit-based weight updates + annealed jitter sigma. Resume-identity
  invariant pins `adaptive_operators=false` for byte-identical guarantee.
- **S11 return-structure clustering** + **persistence-v2 + GICS enrich (mega)**.

## In flight — NOT yet on `main`

Branch `feat/store-resumable-discover` carries 2 commits (2 ahead, parked):

- `7652527` **fix(optimize): default rebalance daily, not weekly.** Diagnosed:
  weekly held 5-day-stale positions and destroyed fast-decaying signals
  (daily Sharpe 1.64 -> weekly 0.45) while raising realized turnover. Damp
  turnover with `--trade-rate` (Gârleanu-Pedersen partial step), not by
  coarsening the rebalance cadence.
- `b3ee4af` **feat(discover): robustness IS-Sharpe gate + RAM-aware worker cap.**
  - `--min-is-sharpe`: train-window Sharpe floor applied before holdout admit
    (kills holdout-lucky alphas). Disabled sentinel `-inf` keeps legacy
    byte-identity.
  - Worker auto-cap: each worker Engine owns a `SlotPool` growing to
    peak-slots x (dates x instruments x 8 bytes ~= 208 MB). `auto = cores-1`
    OOM'd on deep searches; cap now bounds workers to ~60% physical RAM.

Merge these into `main` when alpha-DB work resumes.

## Alpha-DB deliverable — current state

Goal: curated DB of alphas that are robust, high-capacity, low-turnover,
low-correlation, high-fitness, mined from ORATS via the DSL search pipeline.

Last deep run (gen-15, pop-60, OOS gate is>=0.3 AND oos>=0.5):

```
admitted=2 evaluated=356 reject_hist=[Accept=2,_,331,5,3,15]
```

- `ts_argmax(atmCenI_126d, 20)` — is=0.72 oos=0.82 turnover=0.38. **Real**
  options-vol signal.
- `zscore(zscore(sector))` — is=0.88 oos=1.44 turnover=0.029. **Degenerate**:
  a static sector-id tilt, not a tradeable alpha. Should be filtered.

**Validated this session:** OOM fix (gen-15 completes), robustness gate
(0 holdout-lucky admits), daily-rebalance default. **Open problem:** search
converges early (best fitness plateaus ~gen 4) and under-produces robust
alphas — binding reject is holdout *fitness* (331), not the IS floor. 2 alphas
(one junk) is too thin for a low-correlation portfolio.

### Next steps

1. **Filter sector-as-signal** — exclude categorical `sector` from numeric
   grammar/seeds (`zscore(sector)`-type degeneracies).
2. **Populate the DB** — broaden search diversity, accumulate multi-seed passes
   into the persistent library, or parallelize the sequential OOS admission
   loop (356 candidates x 2 full-panel evals dominated the ~1600 s run).
3. **Liquidity floor (capacity)** — `--min-adv-usd` panel-build flag exists but
   needs a panel rebuild (deferred); kills the max_participation_pct outlier.

## Layout

```
atx-impl/src/
  main.cpp / dispatch.cpp        entry + subcommand dispatch
  config.{hpp,cpp}               flag parsing + config shapes
  stage_*.cpp                    one file per subcommand
  store_progress_sink.{hpp,cpp}  resumable-discover progress sink
  serialize_{panel,genome}.cpp   binary IO
  artifacts.cpp                  run artifacts
atx-impl/tests/                  per-stage + e2e + smoke tests
```
