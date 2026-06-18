# atx-impl — End-to-End Equity Alpha Pipeline: Code Review & Sprint Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Each sprint below should be expanded into a bite-sized TDD plan (superpowers:writing-plans) at execution time. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Build a new top-level binary project `atx-impl` that demonstrates the complete US-equity alpha workflow — ORATS zip → tsdb segments → history panel → alpha discovery → combination → risk optimization → tradeable portfolios + PnL report — as a staged CLI (`load|panel|discover|combine|optimize|report`) plus a `run` wrapper that chains all stages in-process.

**Architecture:** `atx-impl` is an **orchestration + CLI + artifact** layer. It re-implements **no** engine math — every stage is an existing `atx-core` / `atx-engine` / `atx-tsdb` library call. The binary's job is: parse config, drive the stages, serialize intermediate artifacts to disk (so expensive stages cache), enforce byte-reproducible run digests, and emit human-readable outputs (equity curve, weights, report). The one genuine engine-library touch is closing **BookPipeline RESIDUAL #1**: wiring discovered genomes through the VM re-eval seam so the combiner consumes a *real forward-applied mega-alpha* instead of `ScriptedSignalSource` doubles.

**Tech Stack:** C++20, clang-cl + Ninja, vcpkg manifest mode, CMake presets (all Debug; Release configured ad-hoc via `build-rel`), GoogleTest. Links `atx::core`, `atx::engine`, `atx::tsdb`, `atx_warnings`.

## Global Constraints

- **C++20**, `/W4 /permissive- /WX` via the `atx_warnings` INTERFACE target (disable for throwaway builds only with `-DATX_WERROR=OFF`). New code must compile warning-clean.
- **Determinism is a hard contract.** Every stage that produces an artifact also produces a stable digest. Same config + same seeds ⇒ byte-identical artifacts and digests across runs and across worker counts (the engine already guarantees F1/F2/R1/R8; `atx-impl` must not break it — no clock/RNG/map-iteration on the artifact path).
- **No new third-party dependency** for `atx-impl`. Config is dependency-free: primary interface is `--flag value` CLI args; an optional `--config <file>` is newline-separated `flag=value` pairs parsed by hand. (Arrow/parquet are available transitively but must not be pulled into the CLI surface.)
- **Link via ALIAS targets only**: `atx::core`, `atx::engine`, `atx::tsdb`, plus `atx_warnings` (PRIVATE) and `Threads::Threads` (PRIVATE).
- **Bounded window first.** All sprint tests and the golden fixture run on a small screened universe / short window (fast in CI). The full 10-year ~20k-name run is a config-scaled later sprint, not a code-path fork.
- **Fail closed.** Every stage returns `atx::core::Result` / `Status`; the CLI maps an `Err` to a non-zero exit and a one-line diagnostic on stderr. No silent partial output.
- Commit only when the user asks; never `git add -A` (add explicit files); never push without explicit instruction. End commit messages with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

---

## Part A — Code Review: Current State of the Pipeline

**Headline finding: the engine is ~95% of the way to end-to-end already.** Every stage exists as a tested library API. `atx-impl` is integration, serialization, and CLI — not new quant math. The review below maps each stage to its existing API and flags the (few) real seams.

### A.1 Stage-by-stage inventory (all VERIFIED present + tested)

| Stage | Library API | Input → Output |
|---|---|---|
| zip → segments | `data::load_orats_history(OratsLoadConfig) → Result<OratsLoadStats>` ([orats_history.hpp:61](../../atx-engine/include/atx/engine/data/orats_history.hpp)) | zip → per-date `*.seg` + `_symbology.parquet` + `_manifest.json` |
| segments → panel | `data::build_history_panel(HistoryDataConfig) → Result<HistoryPanel>` ([history_panel.hpp:53](../../atx-engine/include/atx/engine/data/history_panel.hpp)) | seg dir + window + universe policy → 8-field date-major `alpha::Panel` (close=TRI, raw_close, volume, high, low, open, market_cap, sector) + digest |
| panel → alphas | `factory::SearchDriver::run(SearchConfig, AlphaStore) → SearchResult`; `factory::Factory::mine(FactoryConfig, AlphaStore&, AlphaGate) → FactoryReport` ([search_driver.hpp:272](../../atx-engine/include/atx/engine/factory/search_driver.hpp), [factory.hpp:170](../../atx-engine/include/atx/engine/factory/factory.hpp)) | panel + seeds → ranked `Genome`s; mine admits survivors into an `AlphaStore` with PnL+positions streams |
| genome → signal | `compile(ast, analysis) → Result<Program>`; `alpha::Engine{panel}.evaluate(program) → Result<SignalSet>` ([bytecode.hpp:219](../../atx-engine/include/atx/engine/alpha/bytecode.hpp), [signal_source.hpp:262](../../atx-engine/include/atx/engine/loop/signal_source.hpp)) | Ast → bytecode → date×instrument signal matrix |
| combine | `combine::AlphaCombiner::fit(pool, fit_begin, fit_end) → Result<Combination>`; `combine::CombinedSignalSource` applies weights PIT ([combiner.hpp:459](../../atx-engine/include/atx/engine/combine/combiner.hpp), [combined_source.hpp:166](../../atx-engine/include/atx/engine/combine/combined_source.hpp)) | alpha pool → blend weights (5 methods incl. Ledoit-Wolf shrunk MV) → mega-alpha source |
| risk model | `risk::FactorModelBuilder::build(panel, window, cap, group) → Result<FactorModel>` ([factor_model.hpp:479](../../atx-engine/include/atx/engine/risk/factor_model.hpp)) | panel → factored `V = XFXᵀ + D` (never densified) |
| optimize | `risk::PortfolioOptimizer::solve(alpha, V, w_prev) → Result<vector<f64>>`; `risk::MultiPeriodOptimizer::run(sched, alpha_at, model_at, cost) → Result<MultiPeriodResult>` ([optimizer.hpp:111](../../atx-engine/include/atx/engine/risk/optimizer.hpp), [multi_period.hpp:117](../../atx-engine/include/atx/engine/risk/multi_period.hpp)) | combined α + V + constraints → dollar-neutral, gross- & name-capped books per rebalance |
| constraints | `risk::ConstraintSet::materialize(X, w_prev, M)` — gross/net, position cap, factor/group/beta, turnover ([constraints.hpp:121](../../atx-engine/include/atx/engine/risk/constraints.hpp)) | constraint spec → linear rows + L1 budgets |
| report / PnL | `book::accumulate_report(result, sched, returns_field, lib, panel, V, as_of) → Result<BookReport>`; `book::write_report` (byte-identical) ([report.hpp:152](../../atx-engine/include/atx/engine/book/report.hpp)) | books + returns → equity curve, pnl gross/net/cost, factor exposures |
| **orchestration spine** | `book::BookPipeline` — mine→admit→promote→combine→risk→allocate→optimize→monitor→recycle→report ([pipeline.hpp:1](../../atx-engine/include/atx/engine/book/pipeline.hpp)) | the whole chain, already wired and digest-pinned |

### A.2 The real seams `atx-impl` must address (everything else is glue)

1. **Combiner re-eval seam (BookPipeline RESIDUAL #1).** Today the combine path's constituents are `loop::ScriptedSignalSource` canned-schedule doubles ([pipeline.hpp:54-60](../../atx-engine/include/atx/engine/book/pipeline.hpp)). The combine *machinery* (`AlphaStore`, `AlphaCombiner::fit`, `CombinedSignalSource`, the frozen `vector<ISignalSource*>` seam) is real; only the per-constituent source is faked. **Decision #2 closes this**: compile each discovered genome (`compile(ast, analysis)`), wrap it as a re-evaluable source over the research panel, and feed those real sources to `CombinedSignalSource`. This is the one engine-adjacent piece of work.

2. **VM re-eval field coverage.** `loop::VmSignalSource` builds only the 5 OHLCV fields ([signal_source.hpp:297-300](../../atx-engine/include/atx/engine/loop/signal_source.hpp)); a genome referencing `market_cap` or `sector` will not resolve through it. But it is also **loop-oriented** (consumes the newest-first `RollingPanel`/`PanelView`). `atx-impl`'s combine→optimize runs off the **fixed research `alpha::Panel`** (8 fields, chronological) — so the clean path is a small `atx-impl`-local re-eval source that evaluates a `Program` directly against the research `Panel` (which already exposes all 8 fields via `field_id`), returning per-date cross-sections. This **avoids** touching `VmSignalSource` at all and keeps full field coverage. (If a future live-loop demo is wanted, generalizing `VmSignalSource`'s field list becomes a separate engine task — out of scope here.)

3. **Genome (Ast) serialization.** No `unparse(Ast)→string` exists and `compile_batch` takes source strings, so the **staged** `discover → combine` handoff needs the `Genome`'s `Ast` persisted to disk. The `Ast` is a *flat, relocatable, value-owned arena* ([genome.hpp:49](../../atx-engine/include/atx/engine/factory/genome.hpp)) — designed to be dumped/reloaded. `atx-impl` adds an Ast (de)serializer. (In `run` mode, genomes stay in memory and no serialization is needed — so this is a staged-mode-only artifact.)

4. **Panel serialization.** The owned `alpha::Panel` (dates, instruments, field names, columns, universe mask) must be dumpable for the staged `panel → discover/combine` handoff. Straight binary dump of the column vectors + a small header. (Borrowed panels alias mmap; the owned multi-segment panel is the one to serialize.)

5. **Rebalance schedule + returns field.** The optimizer's `MultiPeriodOptimizer::run` needs a `RebalanceSchedule` and `accumulate_report` needs a `returns_field`. The research panel's TRI `close` provides forward returns; `atx-impl` derives a daily/weekly schedule from the panel's time axis. No new engine code — a thin schedule builder.

### A.3 Documented gaps that are explicitly OUT of scope (do not let them block)

- **No standalone historical-portfolio backtester** beyond `accumulate_report` (which already rolls books × realized returns into an equity curve). `atx-impl` uses `accumulate_report`; wiring the full `loop::BacktestLoop` (bar-by-bar, no-look-ahead order/fill simulation) is a *possible* fidelity upgrade but not required for the reference pipeline. Flag, don't build.
- Dead-alpha factor rung, statistical (APCA) factors, per-name VRA, ISC off-diagonal — all opt-in engine features (`Err(NotImplemented)` on the default path). `atx-impl` uses the default fundamental risk model. Out of scope.
- Some individual VM ops may be unimplemented; the **core ts/cs grammar works** (the factory integration test admits a real momentum signal, which requires time-series ops). Constrain seed expressions to the verified grammar; do not assume every op exists.

---

## Part B — `atx-impl` Architecture

### B.1 Directory layout (new top-level sibling)

```
atx-impl/
  CMakeLists.txt
  src/
    main.cpp                 # CLI dispatch: subcommand router + global flags
    config.hpp / config.cpp  # RunConfig + flag/file parsing (dependency-free)
    artifacts.hpp / .cpp     # on-disk artifact read/write + digest helpers
    serialize_panel.hpp/.cpp # alpha::Panel  <-> binary
    serialize_genome.hpp/.cpp# factory::Genome(Ast+Analysis) <-> binary
    research_source.hpp/.cpp # ResearchSignalSource: Program -> ISignalSource over the research Panel (closes RESIDUAL #1)
    schedule.hpp/.cpp        # RebalanceSchedule + forward-returns field from a Panel time axis
    stage_load.cpp           # `load`     subcommand
    stage_panel.cpp          # `panel`    subcommand
    stage_discover.cpp       # `discover` subcommand
    stage_combine.cpp        # `combine`  subcommand
    stage_optimize.cpp       # `optimize` subcommand
    stage_report.cpp         # `report`   subcommand
    stage_run.cpp            # `run`      wrapper (chains all stages in-process)
  tests/
    CMakeLists.txt
    cli_smoke_test.cpp
    serialize_roundtrip_test.cpp
    research_source_test.cpp
    e2e_pipeline_test.cpp    # bounded-window golden run; staged == run digest
    fixtures/                # tiny synthetic ORATS zip + expected digests
```

Top-level `CMakeLists.txt`: add `add_subdirectory(atx-impl)` after the existing `add_subdirectory(atx-engine)`.

`atx-impl/CMakeLists.txt` (matches existing executable conventions, e.g. `atx-tsdb/tools`, `atx-engine/bench`):

```cmake
add_executable(atx-impl
    src/main.cpp src/config.cpp src/artifacts.cpp
    src/serialize_panel.cpp src/serialize_genome.cpp
    src/research_source.cpp src/schedule.cpp
    src/stage_load.cpp src/stage_panel.cpp src/stage_discover.cpp
    src/stage_combine.cpp src/stage_optimize.cpp src/stage_report.cpp
    src/stage_run.cpp)
target_include_directories(atx-impl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(atx-impl PRIVATE cxx_std_20)
target_link_libraries(atx-impl PRIVATE atx::core atx::engine atx::tsdb atx_warnings)
find_package(Threads REQUIRED)
target_link_libraries(atx-impl PRIVATE Threads::Threads)

if(ATX_BUILD_TESTS)   # mirror the existing per-module test toggle
  add_subdirectory(tests)
endif()
```

### B.2 CLI surface

```
atx-impl <subcommand> [--flags] | atx-impl run --config <file>

Subcommands (staged; each reads upstream artifacts, writes its own, prints a digest line):
  load     --zip P --out DIR --min-date YYYY-MM-DD
  panel    --segs DIR --out PANEL --start YYYY-MM-DD --end YYYY-MM-DD
                       --min-adv-usd F --top-n-by-adv N
  discover --panel PANEL --out ALPHADIR --seed U64 --population N --generations N
                       --seed-expr "rank(close)" [--seed-expr ...] --min-dsr F
  combine  --panel PANEL --alphas ALPHADIR --out COMBO --method shrinkage-mv
                       --fit-begin D --fit-end D
  optimize --panel PANEL --combo COMBO --out BOOKDIR
                       --risk-aversion F --turnover-penalty F --gross L --name-cap C
                       --rebalance daily|weekly
  report   --panel PANEL --books BOOKDIR --out REPORTDIR

  run      --config FILE   # all of the above from one config, in-process (no intermediate reload)

Global: --help, --quiet, --digest-only
```

Every subcommand prints exactly one machine-parseable digest line on success:
`[atx-impl] stage=<name> digest=<hex> <key>=<val> ...` — and the same stage produces the same digest in staged and `run` modes (the e2e test asserts this).

### B.3 Artifact formats (dependency-free binary + sidecar JSON-ish manifest)

- **segments**: native `*.seg` (already defined) — `load` writes them via `load_orats_history`.
- **panel** (`PANEL`): `serialize_panel` — header (`magic`, dates D, instruments N, field count F, digest) + F×(D·N) f64 columns + (D·N) u8 universe + F NUL-padded field names.
- **alphas** (`ALPHADIR`): per admitted alpha — `serialize_genome` Ast dump + a metrics sidecar (fitness, dsr, raw, redundancy) + the alpha's PnL & positions streams (for the combiner pool). Plus `_manifest` (seed, count, search digest).
- **combo** (`COMBO`): `Combination` (weights, fit_begin, fit_end) + the alpha-id ordering + combine method + digest.
- **books** (`BOOKDIR`): `MultiPeriodResult` (books[s][i], turnover[s], cost_bps[s]) + schedule + digest.
- **report** (`REPORTDIR`): `book::write_report` byte-identical dump + `equity_curve.csv` + `weights_<date>.csv` (human-readable convenience) + `summary.txt`.

### B.4 Determinism plan

- All seeds come from config (`--seed`); no `Date.now`/RNG/`Math.random` on the artifact path.
- Digests fold artifacts in fixed (index / period / instrument) order using the engine's existing FNV/wyhash helpers (reuse `signal_set_digest`, `SearchResult::digest`, `BookReport` digest folds — do not invent a new hash).
- The `run` wrapper calls the **same stage functions** the subcommands call (each `stage_*.cpp` exposes a `Result<...> run_<stage>(const RunConfig&, ...)` consumed by both `main.cpp`'s dispatcher and `stage_run.cpp`), guaranteeing staged≡run digests.

---

## Part C — Sprint Plan

Seven sprints. Sprints S0–S6 each end in an independently testable deliverable; S7 is a scale/validation sprint. Expand each into a bite-sized TDD plan at execution time.

### Sprint S0 — Scaffold, CLI dispatch, config, digests

**Files:** Create `atx-impl/CMakeLists.txt`, `src/main.cpp`, `src/config.{hpp,cpp}`, `src/artifacts.{hpp,cpp}`, `atx-impl/tests/{CMakeLists.txt,cli_smoke_test.cpp}`; Modify root `CMakeLists.txt` (`add_subdirectory(atx-impl)`).

**Interfaces:**
- Produces: `struct RunConfig` (all stage params; populated from CLI flags or `--config` file); `int dispatch(int argc, char** argv)`; `void emit_digest_line(std::string_view stage, atx::u64 digest, ...)`; each stage's `run_<stage>` signature declared (bodies return `Err(NotImplemented)` until its sprint).
- Consumes: nothing (entry point).

**Deliverable:** `atx-impl --help` lists subcommands; unimplemented stages exit non-zero with a clean "not implemented" line; `--config` round-trips flags.

**Tests:** `cli_smoke_test` — binary builds; `--help` exits 0 and lists all 7 subcommands; `--config` file parsing equals equivalent CLI flags; unknown subcommand exits non-zero.

### Sprint S1 — `load`: zip → segments

**Files:** Create `src/stage_load.cpp`; add a tiny synthetic ORATS zip fixture under `tests/fixtures/`.

**Interfaces:** Produces `Result<data::OratsLoadStats> run_load(const RunConfig&)` — maps flags to `OratsLoadConfig`, calls `load_orats_history`, emits stats + a digest over the stats.

**Deliverable:** `atx-impl load --zip f.zip --out segs/ --min-date 2020-01-01` writes segments and prints `rows_kept`, `dates_written`, `distinct_securities`.

**Tests:** load the synthetic zip → assert `OratsLoadStats` fields and that the expected `*.seg` count exists; re-run → identical stats digest.

### Sprint S2 — `panel`: segments → serialized history panel

**Files:** Create `src/serialize_panel.{hpp,cpp}`, `src/stage_panel.cpp`, `tests/serialize_roundtrip_test.cpp`.

**Interfaces:**
- Produces: `Status write_panel(const alpha::Panel&, u64 digest, const std::string& path)`; `Result<alpha::Panel> read_panel(const std::string& path)`; `Result<u64> run_panel(const RunConfig&)`.
- Consumes: `data::build_history_panel` (segments → `HistoryPanel`).

**Deliverable:** `atx-impl panel --segs segs/ --out panel.bin --start … --end … --top-n-by-adv 200` builds and serializes the 8-field panel; prints the engine's panel digest.

**Tests:** build panel on the S1 output → serialize → `read_panel` → assert dates/instruments/field-names/cells byte-equal and digest stable (round-trip).

### Sprint S3 — `discover`: panel → alphas (+ Ast serialization)

**Files:** Create `src/serialize_genome.{hpp,cpp}`, `src/stage_discover.cpp`; extend `serialize_roundtrip_test.cpp`.

**Interfaces:**
- Produces: `Status write_genome(const factory::Genome&, const std::string&)` / `Result<factory::Genome> read_genome(const std::string&, const alpha::Library&)` (re-derives `Analysis` via `analyze` on load — the F5 oracle is the validity backstop); `Result<u64> run_discover(const RunConfig&, const alpha::Panel&, ...)` driving `Factory::mine` (or `SearchDriver::run`), persisting admitted genomes + metrics + PnL/positions streams.
- Consumes: `factory::Factory`/`SearchDriver`, `compile`, `alpha::Engine`, the read-back `alpha::Panel`.

**Deliverable:** `atx-impl discover --panel panel.bin --out alphas/ --seed 777 --population 16 --generations 5 --seed-expr "rank(close)" --min-dsr 0.5` writes N admitted alphas + a search digest.

**Tests:** discover on a fixture panel with a planted signal → ≥1 admitted alpha; **F1**: same seed → identical search digest; genome round-trip (`write_genome`→`read_genome`→`compile`→`evaluate`) reproduces the same `SignalSet`.

### Sprint S4 — `combine`: real mega-alpha seam (closes RESIDUAL #1)

**Files:** Create `src/research_source.{hpp,cpp}` (`ResearchSignalSource`), `src/stage_combine.cpp`, `tests/research_source_test.cpp`.

**Interfaces:**
- Produces: `class ResearchSignalSource final : public engine::ISignalSource` — holds a compiled `alpha::Program`; `evaluate(PanelView)`/direct-panel eval returns the current-date cross-section over the **full-field research panel** (all 8 fields resolve, unlike `VmSignalSource`'s OHLCV-only); `max_lookback()` forwards `program_.required_lookback`. `Result<u64> run_combine(const RunConfig&, const alpha::Panel&, ...)`: load genomes → `compile` each → build `AlphaStore` pool with real streams → `AlphaCombiner::fit` → `Combination` → assemble `CombinedSignalSource` over the `ResearchSignalSource` constituents.
- Consumes: `combine::AlphaCombiner`, `combine::CombinedSignalSource`, `compile`, `alpha::Engine`.

**Deliverable:** `atx-impl combine --panel panel.bin --alphas alphas/ --out combo.bin --method shrinkage-mv --fit-begin 0 --fit-end D0` writes blend weights + digest; the combined source is a genuine forward-re-evaluable mega-alpha.

**Tests:** `research_source_test` — `ResearchSignalSource` cross-section equals `Engine.evaluate(program)` at a probe date; combined cross-section equals Σ wᵢ·signalᵢ at a probe date; combine weights deterministic across runs; a genome referencing `market_cap` resolves (proves full field coverage vs OHLCV-only).

### Sprint S5 — `optimize`: combo + risk model → books

**Files:** Create `src/schedule.{hpp,cpp}`, `src/stage_optimize.cpp`.

**Interfaces:**
- Produces: `Result<risk::RebalanceSchedule> build_schedule(const alpha::Panel&, Cadence)`; `forward returns field` accessor; `Result<u64> run_optimize(const RunConfig&, ...)`: build `FactorModel` via `FactorModelBuilder::build`; define `alpha_at(s)` = combined mega-alpha cross-section at period s, `model_at(s)` = `V`; call `MultiPeriodOptimizer::run`; serialize `MultiPeriodResult`.
- Consumes: `risk::FactorModelBuilder`, `risk::MultiPeriodOptimizer`, `risk::ConstraintSet` (gross/net + name cap default), `combine::CombinedSignalSource`.

**Deliverable:** `atx-impl optimize --panel panel.bin --combo combo.bin --out books/ --gross 1.0 --name-cap 0.05 --rebalance weekly` writes per-period books + turnover + cost.

**Tests:** every book satisfies Σw≈0, Σ|w|≤gross, max|wᵢ|≤cap (to 1e-9); **R1** byte-identical books on re-run; turnover penalty `κ>0` reduces turnover vs `κ=0`.

### Sprint S6 — `report` + `run` wrapper

**Files:** Create `src/stage_report.cpp`, `src/stage_run.cpp`, `tests/e2e_pipeline_test.cpp`.

**Interfaces:**
- Produces: `Result<u64> run_report(const RunConfig&, ...)` → `book::accumulate_report` → `write_report` + `equity_curve.csv` + `summary.txt`; `int run_all(const RunConfig&)` chaining S1–S6 stage functions in-process.
- Consumes: `book::accumulate_report`, `book::write_report`.

**Deliverable:** `atx-impl report …` emits equity curve + PnL decomposition; `atx-impl run --config run.toml` executes the whole pipeline on the bounded fixture end-to-end.

**Tests:** `e2e_pipeline_test` — `run` on the bounded fixture produces a non-degenerate equity curve and a non-zero report digest; **staged pipeline digests == `run` digests** at every stage (the core integration guarantee); report bytes identical on re-run (R8).

### Sprint S7 — Scale to full 10-year universe (validation/ops)

**Files:** `docs/atx-impl/RUNBOOK.md`; no code-path fork (config-scaled only).

**Interfaces:** none new — exercise the existing stages with full-zip config.

**Deliverable:** documented full run on the real 3.3 GB / 17 M-row / 1621-date / ~20k-name ORATS zip: per-stage wall-clock + peak memory, the cache-and-resume staged workflow, and a recommended universe screen for tractable search.

**Tests:** smoke-only (full run is too large for CI) — assert the staged artifacts exist and digests are stable across two partial re-runs of the cheap stages; record timings. Note any stage that needs a Release build or `ATX_FAST_INFLATE=ON` to be practical.

---

## Risks & Decisions Captured

- **D1 (binary shape):** Both — staged subcommands + `run` wrapper sharing one set of `run_<stage>` functions.
- **D2 (combine fidelity):** Wire the real mega-alpha (genome→`compile`→`ResearchSignalSource`→`CombinedSignalSource`), closing BookPipeline RESIDUAL #1. Implemented over the **research panel** (full 8 fields), sidestepping the loop-oriented OHLCV-only `VmSignalSource`.
- **D3 (data scope):** Bounded window + screened universe for all CI/tests; full 10-year run is config-scaled in S7.
- **R1 (Ast serialization):** No `unparse` exists; the `Ast` is a flat relocatable arena — dump it raw and re-`analyze` on load. Staged-mode-only (run mode keeps genomes in memory). If raw-arena dump proves brittle, fall back to persisting the search `master_seed` + config and re-running discovery deterministically (F1 guarantees identical genomes) — slower but dependency-free.
- **R2 (field coverage):** `ResearchSignalSource` must resolve all 8 panel fields; verified by a dedicated test using a `market_cap`-referencing genome.
- **R3 (no live backtest loop):** PnL comes from `accumulate_report` (books × realized TRI returns), not the bar-by-bar `BacktestLoop`. Faithful enough for a reference pipeline; full loop fidelity is a flagged future upgrade, not in scope.

## Self-Review (spec coverage)

- zip→segments → S1 ✓ · segments→panel → S2 ✓ · alpha discovery → S3 ✓ · combining → S4 ✓ · optimizing → S5 ✓ · tradeable portfolios + report → S5/S6 ✓ · `atx-impl` binary + both CLI shapes → S0/S6 ✓ · full-universe path → S7 ✓.
- No placeholders: every stage names its existing library API with file:line; the only new code is CLI/serialization/`ResearchSignalSource`.
- Type consistency: stage handoffs use the artifact formats in B.3; `run_<stage>` functions are the single source of truth shared by staged and `run` modes.
