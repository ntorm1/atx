# Sprint S7 — Portfolio Construction & Production Lifecycle — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan unit-by-unit. Steps use checkbox (`- [ ]`) syntax. This is the FROZEN *how*; the sprint spec [`sprint-7-portfolio-lifecycle.md`](sprint-7-portfolio-lifecycle.md) is the *what*. On conflict, **§0 (this plan's as-built amendment) overrides** the spec.

**Goal:** Turn the v2 factory (S1 truth · S2 throughput · S3 mining · S4 library · S5 learned combiner · S6 calibrated cost · P4 single-period optimizer + risk model) into an **operating book** — a **deterministic multi-period turnover-aware optimizer** over a rebalancing schedule, an **alpha-decay monitor** that demotes live alphas through the S4 lifecycle, **dead-alpha → endogenous-risk-factor** extraction (Kakushadze) fed into the P4 `FactorModel`, **breadth-driven capacity-bounded capital allocation** with reproducible **book-level reporting artifacts**, and the **full end-to-end pipeline integration test** that is the v2 done-gate.

**Architecture:** Two extension files in the existing `risk/` layer (`risk::MultiPeriodOptimizer` wrapping the as-built `PortfolioOptimizer`; `risk::dead_factor` augmenting the as-built `FactorModel`) plus a **new `book/` layer** (namespace `atx::engine::book`) for the genuinely-new operating concerns (decay monitor, capital allocation, book reporting, the E2E pipeline orchestrator). S7 is **additive**: it *drives* the S4 lifecycle state machine (built but never advanced past `Admitted`), *composes* the S5/P4 combined mega-alpha as the optimizer's expected-return proxy, *consumes* the S6-calibrated cost knobs + capacity curve, and *reuses* the S1 metrics/DSR verbatim. The multi-period solver is a sequence of single-period `PortfolioOptimizer::solve` calls threading `w_prev` across the schedule (receding-horizon MPC), so it inherits the inner solver's fixed-iteration determinism for free.

**Tech Stack:** C++20, header-only inline (`#pragma once`), namespaces `atx::engine::risk` (extensions) + `atx::engine::book` (new). Reuses `risk::{PortfolioOptimizer, OptimizerConfig, FactorModel, FactorModelBuilder, FactorModelConfig, ExposureMatrix, build_exposures, capacity_curve, CapacityPoint}`, `combine::{AlphaCombiner, Combination, CombinedSignalSource, AlphaMetrics, compute_metrics, pairwise_complete_corr}`, `eval::{deflated_sharpe, probabilistic_sharpe, expected_max_sharpe, compute_return_metrics, ReturnMetrics}`, `library::{Library, LifecycleState, AlphaId}` + `library::LibraryStore::positions`, `cost::{cost_aware_knobs, CostKnobs, CalibratedCost, capacity_point, should_trade}` (S6 branch — §0.1), `factory::{ResearchDriver, ResearchConfig}`, `loop::{ISignalSource, SignalView, VmSignalSource}`, `atx::core::{linalg (MatX/VecX/ols), simd::dot, random::Xoshiro256pp, Result, Status}`, `Eigen::SelfAdjointEigenSolver` (dead-factor eigendecomposition). GoogleTest (`atx-engine/tests/*_test.cpp`, CONFIGURE_DEPENDS — no per-unit CMake edit). clang-cl `/W4 /permissive- /WX` **+ strict FP** (`/fp:precise`, `-ffp-contract=off`). Build + ctest are the gates; clang-tidy disabled (noise).

---

## §0 — As-built reconciliation amendment (the recon fixes)

The spec was drafted from the research north-stars before this sprint's reconnaissance against the merged `feat/atx-core-stdlib` engine (and the as-yet-unmerged `feat/atx-engine-cost` branch). Nine load-bearing corrections; each changes a unit's scope.

### 0.1 The S6 `cost/` layer is on an **unmerged branch** — S7 records a hard merge prerequisite + a config-scalar fallback
The spec says S7.1 consumes "the **S6-calibrated** cost in the turnover term," S7.2 reads "the calibrated cost to tell a decaying alpha from a cost-flooded one," and S7.4 sizes "against a real capacity ceiling." Reconnaissance: the entire `cost/` layer (`cost::{calibration, temp_perm, capacity, cost_aware, robust_ls}`) lives on **`feat/atx-engine-cost`** (base `e451c5e`, cut *before* S5 landed) and **does not exist on the S7 branch** (`feat/atx-core-stdlib`); S6-5 borrow is unbuilt on either branch. The cost API S7 targets: `cost::cost_aware_knobs(const CalibratedCost&, f64 ref_participation, f64 ref_sigma, f64 horizon_days) -> CostKnobs{f64 kappa, combine::GateConfig gate, f64 fitness_cost_floor}`; `cost::capacity_point(span<const risk::CapacityPoint>) -> f64` (zero-crossing root-find); `cost::should_trade(f64 edge_bps, f64 cost_bps, f64 safety) -> bool`. **Decision:** S7-0 records **"merge + rebase `feat/atx-engine-cost` over S5"** as a *hard kickoff prerequisite* (raised to the user at kickoff). The S7 design programs against the cost API through a **thin adapter** (`book::CostInputs{f64 kappa; f64 round_trip_cost_bps; f64 capacity_gross;}`) so that if the merge slips, S7 stays buildable by passing these three scalars explicitly (the optimizer already takes `turnover_penalty` as a config scalar; `risk::capacity_curve` is on the S7 branch; the root-find is a few lines, §0.9). The cost-wiring is the adapter; nothing in S7's core math blocks on the merge.

### 0.2 The **single-period turnover-penalized solver already exists** — S7.1 drives it over a schedule, does NOT rebuild it
`risk::PortfolioOptimizer::solve(std::span<const f64> alpha, const FactorModel& V, std::span<const f64> w_prev) -> Result<std::vector<f64>>` (`optimizer.hpp:111`) already solves `max αᵀw − λwᵀVw − κ‖w−w_prev‖₁` s.t. `{Σw=0, Σ|w|≤L, |w_i|≤cap}` with a **DETERMINISTIC FIXED-ITERATION** projected/proximal loop (`max_iters=64`, no convergence early-exit, `optimizer.hpp:162`): a gradient step toward the smooth target `t`, an L1 prox soft-threshold toward `w_prev` (the κ turnover term — **this is exactly the proportional-cost no-trade band**, Constantinides 1986 / Davis-Norman 1990), then projection onto the feasible set. The OptimizerConfig (`optimizer.hpp:89`) carries `risk_aversion λ`, `turnover_penalty κ`, `gross_leverage L`, `name_cap`, `dollar_neutral`, `max_iters`. **Consequence:** S7.1's "multi-period" is **not** a new inner solver — it is a **receding-horizon driver** (Boyd et al. 2017, *Multi-Period Trading via Convex Optimization*): walk the rebalance schedule, thread `w_prev` from the prior period's realized book (so turnover is measured across the *real* schedule, not from flat each rebalance — the property single-period cannot express), set `κ` to the S6-calibrated value, optionally apply a Gârleanu-Pedersen **partial trade-rate** toward the target. The inner `solve` is reused **verbatim**, and the multi-period book chain inherits its bit-determinism by construction.

### 0.3 The **dead-alpha factor slot is reserved but unplumbed** — S7.3 builds the injection path
`risk::FactorModelConfig.n_dead_factors` exists (`exposures.hpp:112`) but is annotated *"P4-7 — ignored here"*: `build_exposures` never consumes it, and `FactorModelBuilder::build` returns **`Err(NotImplemented)` when `n_dead_factors > 0`** (`factor_model.hpp:380`). There is **no mechanism** to inject dead-alpha directions as columns of `ExposureMatrix.x`. The `FactorModel` itself stores `(X, F, D)` privately and exposes no accessor. **Decision:** S7.3 (a) extracts the dead-factor eigenvector columns (Kakushadze §0-mechanics, §4.3), and (b) builds the **augmented** model `FactorModel::create([X | X_dead], blockdiag(F, F_dead), D)` where `F_dead = diag(dead eigenvalues)` are the dead-factor variances. This requires **one additive refactor** of `FactorModelBuilder` — extract its estimation into `build_components(...) -> Result<FactorComponents{MatX X; MatX F; VecX D; usize fit_end;}>`, leave `build()` a thin wrapper — so the new `risk::dead_factor` path can compose the fundamental estimate with the dead columns and call the existing `create`. This is the single reserved-slot touch (mirrors S4b's "one additive facade accessor" precedent), not a rebuild.

### 0.4 **No wall-clock time axis / no rebalance-schedule type** — the schedule and PIT key on an as-of period index
(Inherited from S4 §0.7.) The combine/risk layer carries no timestamp; time is a positional `usize` period index. There is no rebalance-schedule type and no multi-period calendar; `ResearchDriver` loops over *runs*, not over *time*. **Consequence:** S7 defines `book::RebalanceSchedule{std::vector<usize> periods;}` (ascending as-of period indices), and every PIT decision (a rebalance, a decay transition, a dead-factor refresh) keys on an `usize as_of`. "PIT" is operationalized exactly as S4: a decision at as-of period `t` reads only data with index `≤ t`; lifecycle transitions are recorded **at** `t` and **never back-dated** (reusing the S4 append-only journal, whose legal edges already match the spine, §0.5).

### 0.5 The **lifecycle journal already encodes the spine, but nothing drives it past `Admitted`** — S7 is its first producer
`library::LifecycleState{Candidate=0, Admitted=1, Live=2, Decaying=3, Dead=4, Recycled=5}` and the legal edges `Candidate→Admitted→Live→Decaying→{Live | Dead}, Dead→Recycled` are **built and tested** in S4 (`lifecycle.hpp:71,84`); `Library::mark(AlphaId, LifecycleState, usize as_of)` (`library.hpp:192`) is the journal-append entry point and `state_as_of(id, t)` (`library.hpp:228`) is the PIT query. **`ResearchDriver` stops at `Admitted`** — the entire `Live/Decaying/Dead/Recycled` half of the spine has **zero production callers today**. **Consequence:** S7 *drives* (does not rebuild) the state machine — S7.2's monitor calls `mark(id, Live/Decaying/Dead, as_of)`, S7.3's recycler calls `mark(id, Recycled, as_of)` after harvesting a dead alpha as a risk factor. The legal-edge table and the PIT-irreversibility proof are already S4's; S7 adds the *transition logic*, not the journal.

### 0.6 **`AlphaMetrics` carries no higher moments / DSR** — the decay baseline is recomputed from the stored admitted PnL
`combine::AlphaMetrics` is exactly 7 f64 (`sharpe, turnover, returns, drawdown, margin, fitness, holding_days`; `metrics.hpp:56`) — **no skewness, no kurtosis, no DSR**. The S7.2 monitor needs the admitted Sharpe's standard error (skew/kurt-aware) and the admitted DSR to detect a statistically-significant drop. **Consequence:** the decay baseline is **derived from `lib.pnl(id)`** — the admitted backtest PnL, stored **verbatim** in the library (NaN bit-patterns preserved, S4 §0.3) — by recomputing `(sharpe, skew, exkurt, T_admit)` once at first-observation and calling `eval::deflated_sharpe` / `eval::probabilistic_sharpe` **verbatim** (no second Sharpe convention). The baseline is frozen (PIT) and reproducible from the stored stream.

### 0.7 **`Portfolio` exposes only point-in-time scalars** — the book report must accumulate its own series
`portfolio::Portfolio` (`portfolio.hpp:146`) gives `equity()/gross()/net()/leverage()/cash()` **at a point** — no equity-curve, no PnL series, no positions-over-time accessor. **Consequence:** S7.4's reporter cannot read a series off `Portfolio`; it **accumulates** the net-of-cost equity curve, per-period gross/net/turnover, factor exposures, and P&L attribution **itself** from the S7.1 multi-period book schedule × realized step-returns − the S6-calibrated cost. Reproducible artifacts (the spec's byte-identical-across-two-runs criterion) follow from the order-fixed accumulation + deterministic books.

### 0.8 The mega-alpha is applied via a **`CombinedSignalSource` holding live `ISignalSource*` constituents** — the pipeline must keep them alive
`combine::AlphaCombiner::fit(pool, fit_begin, fit_end) -> Combination{weights, fit_begin, fit_end}` (`combiner.hpp:467`) produces *weights only*; **there is no store-side `predict`**. Applying the mega-alpha is `combine::CombinedSignalSource(std::vector<ISignalSource*> sources, Combination combo, CombineMethod method)` (`combined_source.hpp:174`), whose constituents are **non-owning** (caller owns lifetime). **Consequence:** S7.1's optimizer consumes the combined α as the per-date output of a live `CombinedSignalSource`, and the S7.5 pipeline must **hold the constituent sources alive** — re-materializing each admitted alpha's `ISignalSource` by re-parsing its stored `provenance.expr_source` → compile → `VmSignalSource` (the S4b "re-eval adapter" residual, explicitly batoned to S7). For unit tests, `ScriptedSignalSource` doubles suffice; the pipeline test wires the real re-eval path.

### 0.9 **The capacity curve is on the S7 branch; the root-find is on the cost branch** — local fallback if unmerged
`risk::capacity_curve(weights, panel, sim, aum_grid) -> std::vector<CapacityPoint{f64 aum; f64 net_edge_bps;}>` is present on the S7 branch (`capacity.hpp:264`) and is **monotone non-increasing** in AUM by contract. The zero-crossing root-find `cost::capacity_point` (linear-interp where `net_edge_bps` crosses 0) is on the cost branch (§0.1). **Consequence:** S7.4 computes the capacity ceiling from `risk::capacity_curve` (present) and, if `cost::capacity_point` is unmerged, defines the identical linear-interp zero-crossing locally (`book::detail::zero_crossing`, ~6 lines) — the tsdb-crc32 / S4-canonical-hash local-fallback precedent.

> **Net scope shift vs spec:** S7 is **additive and compositional** — the inner optimizer (0.2), the lifecycle journal (0.5), the capacity curve (0.9), the DSR (0.6), and the combiner (0.8) all already exist; S7 *drives, threads, and accumulates* over them. The two genuinely-new numeric paths are the **dead-alpha eigen-extraction + FactorModel augmentation** (0.3, the reserved-slot plumbing) and the **decay detector** (Page-Hinkley + DSR drop, §4.2). The dominant *integration* risk is the **unmerged S6 cost branch** (0.1) — handled by a scalar adapter so S7's correctness never blocks on it.

---

## §1 — Research foundation: the operating-book design rules (with citations)

Derived from the research north-stars (`worldquant-systems-deep-dive.md` §6/§7/§10, `renaissance-technologies-systems-deep-dive.md` §6) and the carried-forward p1 invariants. **Non-negotiable**; every S7 unit is checked against them.

| # | Rule | Why / source |
|---|------|--------------|
| **R1** | **Deterministic multi-period solver — fixed-iteration, no early exit.** The schedule walk, the inner `solve` (`max_iters` fixed), the trade-rate blend, and every reduction are order-fixed; same inputs ⇒ byte-identical book schedule + manifest digest. The per-unit determinism crux is the *no convergence-dependent early exit*. | p1 invariant #1; the p0 optimizer-determinism rule (`optimizer.hpp` §3.2). FISTA/ISTA fixed-K determinism [Beck-Teboulle 2009]; ADMM fixed-iteration [Boyd 2011 / OSQP 2020]. |
| **R2** | **No look-ahead / receding-horizon PIT.** Every forecast is fit-on-trailing; the multi-period objective sums over a horizon but **executes only the first trade** (MPC), so the executed book is a causal function of `≤ t` data. Decay decisions read only `≤ t`; transitions are never back-dated. | p1 invariant #2/#4; receding-horizon look-ahead safety [Boyd et al. 2017 §multi-period; Rawlings-Mayne-Diehl MPC 2017]; the S4 PIT-journal contract. |
| **R3** | **Calibrated, honest cost is a decision input.** The turnover term uses the **S6-calibrated κ** (not the 2001 defaults); the monitor distinguishes genuine decay from cost-flooding via the calibrated round-trip cost; allocation respects the **calibrated capacity point**. | p1 invariant #5; RenTech §4 (cost = the strategy); Almgren et al. 2005 (impact law); the S6 baton (`cost_aware_knobs`). |
| **R4** | **No survivorship through the book.** Delisted names keep their final values across the schedule and the report; a NaN α cell gets exactly 0 weight (the optimizer's `live[]` mask, `optimizer.hpp:128`) and is excluded from reductions; dead-alpha holdings are snapshotted at death, never retroactively dropped. | p1 invariant #3; the optimizer's NaN→0-weight exclusion is the as-built guarantee. |
| **R5** | **The library lifecycle is the spine S7 drives.** Demotions (`Live→Decaying→Dead`) and recycling (`Dead→Recycled`) are **journal appends** at an as-of period; "trust slowly, retire fast" = asymmetric thresholds (fast demotion trip, long restoration window). | p1 invariant #4; WQ §10 (alpha lifecycle); the S4 state machine; CUSUM/Page-Hinkley asymmetric hysteresis [Page 1954; Moustakides 1986]. |
| **R6** | **Dead alphas become endogenous risk factors.** A flatlined alpha encodes a no-expected-return direction; its holdings overlap matrix eigen-decomposes into risk-factor loadings fed into `V = XFXᵀ + D`. Neutralizing live alphas against them **measurably reduces** the book's exposure to those directions. | WQ §10 + Kakushadze & Yu 1709.06641 (*Dead Alphas as Risk Factors*); the library self-generates its endogenous risk model — no vendor Barra for this block. |
| **R7** | **Breadth-driven, capacity-bounded Kelly sizing of one unified book.** Gross leverage `L = clip(c · SR_book/σ_book, 0, L_capacity)` (fractional Kelly = mean-variance leverage), with effective breadth cross-checked via the Fundamental Law `IR ≈ TC·IC·√BR`. The book is **one** dollar-neutral, capacity-capped mega-alpha. | RenTech §6 (Kelly/breadth, the single unified book); Grinold-Kahn *Fundamental Law*; Clarke-de Silva-Thorley (TC); Thorp (Kelly); the S6 capacity ceiling. |
| **R8** | **Reproducible headless artifacts; the E2E run is the v2 done-gate.** Reporting emits byte-identical files (anti-roadmap #5: files, not pixels). The S7.5 integration test proves all **seven** carried-forward invariants **simultaneously**, top-to-bottom, at every fitted boundary. | p1 invariant #1; the spec's v2 exit gate; the S4 reproducibility precedent (content-addressed, two-builds-equal). |

**One-sentence thesis:** *every primitive S7 needs already exists — the fixed-iteration optimizer (R1), the PIT lifecycle journal (R5), the capacity curve and calibrated cost (R3/R7), the DSR (R5/R6), the combined mega-alpha (R7) — so S7 is the compositional layer that operates them as a book, and the only genuinely-new correctness risks are (a) the multi-period schedule's determinism + look-ahead safety (R1/R2), (b) the decay detector's non-vacuous statistical power vs false-alarm rate (R5), (c) the dead-factor extraction measurably neutralizing exposure (R6), and (d) the whole pipeline composing without any invariant leaking (R8).*

---

## §2 — File structure

### 2.1 atx-core / cross-branch Pattern-B requests (decided at kickoff)

> The engine adds no general-purpose primitive (project rule). S7 records the cross-module edges and ships on existing primitives, exactly as S1–S6 did:
>
> 1. **L7 QP / projected-gradient refinement → atx-core.** Ship on the as-built `risk::PortfolioOptimizer` projected/proximal loop (the multi-period driver wraps it, §0.2); the true QP solver (OSQP-style fixed-iteration ADMM with a pre-factorized KKT system) is the recorded lift.
> 2. **Symmetric eigensolver for the dead-factor extraction → already in Eigen.** Ship on `Eigen::SelfAdjointEigenSolver` with a **fixed sign convention** (largest-|component| positive) for bit-reproducibility (§4.3); no atx-core request.
> 3. **The S6 `cost/` layer merge → branch integration, not a primitive.** Hard kickoff prerequisite (§0.1); the `book::CostInputs` scalar adapter is the fallback.
> 4. **eRank / effective-breadth helper (`(Σλ)²/Σλ²`) → atx-core L6 residual.** Ship engine-local (a 3-line order-fixed reduction, §4.3/§4.4); recorded as the lift if reused.

### 2.2 Engine files (this sprint builds these)

| File | Responsibility | Unit |
|---|---|---|
| `include/atx/engine/book/fwd.hpp` | forward decls + the doc block (namespace `atx::engine::book`; the operating-book model: schedule → multi-period optimize → monitor decay → recycle dead → allocate → report; the risk/-extensions-vs-book/-new split) | S7-0 |
| `include/atx/engine/risk/multi_period.hpp` | `RebalanceSchedule`, `MultiPeriodConfig`, `MultiPeriodResult`, `MultiPeriodOptimizer` — receding-horizon driver over `PortfolioOptimizer::solve`, threading `w_prev`, calibrated κ, Gârleanu-Pedersen trade-rate, capacity-bounded `L`; deterministic (§4.1/§0.2/R1/R2) | S7-1 |
| `include/atx/engine/book/decay_monitor.hpp` | `AdmittedBaseline`, `PageHinkleyState`, `DecayConfig`, `DecayVerdict`, `DecayMonitor` — live-vs-backtest drift (Page-Hinkley down-mode + rolling DSR drop past MinTRL), asymmetric demotion, cost-flooding discriminator; drives `Library::mark` (§4.2/§0.5/§0.6/R5) | S7-2 |
| `include/atx/engine/risk/dead_factor.hpp` | `FactorComponents` (the `build_components` refactor), `DeadAlphaFactors`, `extract_dead_factors`, `augment_factor_model` — Kakushadze holdings-overlap eigen-extraction + `FactorModel::create([X\|X_dead], blockdiag(F,F_dead), D)` (§4.3/§0.3/R6) | S7-3 |
| `include/atx/engine/book/allocation.hpp` | `AllocationConfig`, `effective_breadth`, `size_book` — fractional-Kelly leverage from book Sharpe, capacity-clipped; Fundamental-Law breadth cross-check (§4.4/§0.9/R7) | S7-4 |
| `include/atx/engine/book/report.hpp` | `BookReport` (equity/gross/net/turnover/factor-exposure/attribution/capacity-util/lifecycle-census series) + `accumulate_report` + `write_report` (headless deterministic files) (§4.5/§0.7/R8) | S7-4 |
| `include/atx/engine/book/pipeline.hpp` | `BookPipeline` — the E2E orchestrator: ResearchDriver(mine→admit) → mark Live → combine → multi-period optimize → cost → monitor decay → extract dead factors → allocate → report; ties everything (§4.6/§0.8/R8) | S7-5 |

### 2.3 Tests (one per unit, `atx-engine/tests/<name>_test.cpp`, CONFIGURE_DEPENDS)
`risk_multi_period_test.cpp` (S7-1), `book_decay_monitor_test.cpp` (S7-2), `risk_dead_factor_test.cpp` (S7-3), `book_allocation_test.cpp` (S7-4), `book_report_test.cpp` (S7-4), `book_pipeline_test.cpp` (S7-5, the full-pipeline + all-seven-invariants capstone). Bench: `bench/book_bench.cpp` (S7-5: rebalances/sec across schedule length + universe size; dead-factor extraction cost vs |dead|).

### 2.4 Ledger
`sprint-7-progress.md` (S7-0), updated per unit (copy `sprint-6-progress.md` / `sprint-4-progress.md` shape). **Likely sub-sprint split S7-a (S7-0…S7-2: schedule + monitor) / S7-b (S7-3…S7-5: dead factors, allocation+report, pipeline+close)** per the ROADMAP's ">7 units" rule (S7 is 6 units incl. marker — borderline; split only if S7.1+S7.5 over-run, exactly as S4 did).

---

## §3 — Cross-cutting gates (every coding unit) + handoff block

- **TDD:** failing GoogleTest first; `Suite_Condition_ExpectedResult`; cover happy path, **boundaries** (single-period schedule == one `solve`; empty/flat `w_prev`; all-NaN α column; a one-alpha book; zero dead alphas; a never-decaying alpha; a planted-decaying alpha; capacity ceiling below the Kelly leverage; an empty rebalance schedule; the recover-from-Decaying path), and the **invariant proofs** (R1 two-builds-byte-identical schedule, R2 truncation-invariance at every fitted boundary, R5 PIT no-retroactive demotion, R6 dead-factor exposure strictly reduced, R8 byte-identical report).
- **Determinism (R1):** the schedule walk, the inner `solve` (fixed `max_iters`, never a residual test), the trade-rate blend, the eigensolver sign convention, and every reduction are order-fixed; a **two-builds-equal** test (same inputs → byte-identical book schedule + report digest) is mandatory for S7-1 and S7-5. No RNG except the seeded SRP/eigen paths whose seed is part of the recorded artifact.
- **No look-ahead / PIT (R2):** every forecast (the combined α, the factor model, the calibrated cost, the decay baseline) is fit-on-trailing and applied OOS only; the multi-period solver executes **only the first move** of its horizon; the monitor reads only realized PnL `≤ t`; a transition at `t` never alters an earlier `state_as_of` query. **Truncation-invariance is the test** at every fitted boundary (optimizer forecast, decay baseline, dead-factor refresh).
- **Calibrated honest cost (R3):** the turnover term's κ is the S6-calibrated value (`cost::cost_aware_knobs().kappa`, or the `CostInputs` scalar); never the raw default. The monitor's decay-vs-cost-flood discriminator uses `cost::should_trade` / the calibrated round-trip cost. Allocation clips at the calibrated capacity point.
- **No survivorship (R4):** delisted/NaN names get 0 weight (the optimizer's `live[]` mask) and round-trip through the report verbatim; dead-alpha holdings snapshot at death.
- **Lifecycle discipline (R5):** transitions go **only** through `Library::mark` (the append-only journal); S7 never UPDATE/DELETEs a journal row and never re-implements the legal-edge table (it consumes S4's). Asymmetric thresholds (fast demote, slow restore) are config, recorded.
- **No hot-path alloc (R6/p1 #6):** the rebalance loop reuses pre-sized scratch (the optimizer already allocates once per `solve`; the driver pre-sizes the schedule-length book matrix); the cold paths (dead-factor eigen-extraction, report write, snapshot) may allocate (documented).
- **API discipline:** `const`/`constexpr`/`noexcept`/`[[nodiscard]]`; `Result<T>`/`Status` for expected failures (dim mismatch, empty schedule, non-SPD augmented F, unknown id); weakest sufficient types (`std::span`, `const&`, `std::string_view`); functions ≤ ~60 lines; **reuse `risk::PortfolioOptimizer`/`FactorModel`/`capacity_curve`, `eval::deflated_sharpe`, `library::mark`, `cost::*` — do NOT reinvent the solver, the DSR, the lifecycle journal, or the cost model**; the only new numeric helpers are the multi-period driver glue, the Page-Hinkley statistic, the dead-factor eigen-extraction, and the Kelly/breadth sizing (each recorded as a Pattern-B lift where general-purpose).
- **Warnings = errors:** `/W4 /permissive- /WX` (clang-cl) **+ strict FP** (`/fp:precise`, `-ffp-contract=off`). clang-tidy disabled — the strict build + ctest are the gate.
- **clangd noise:** ignore squiggles; only a real `cmake --build` + ctest are the gates.

### Handoff block (paste into every coding sub-agent brief)
```text
Implementation quality standard (atx): governed by .agents/cpp/agent.md (safety-critical-grade C++20) and
atx-engine/plans/docs/implementation-quality.md. Positive style/API references in-tree (REUSE, do NOT rebuild):
risk/optimizer.hpp (PortfolioOptimizer::solve(alpha, FactorModel V, w_prev) — the ALREADY-deterministic
fixed-iteration max αᵀw−λwᵀVw−κ‖w−w_prev‖₁ projected/proximal solver, max_iters fixed, NaN α => 0 weight; you
DRIVE it over a schedule, you do NOT rewrite the inner loop); risk/factor_model.hpp (FactorModel V=XFXᵀ+D,
create/apply_inverse/risk/neutralize; FactorModelBuilder::build — you ADD build_components to expose (X,F,D)
and augment with dead columns via the existing create); risk/exposures.hpp (ExposureMatrix, build_exposures,
FactorModelConfig.n_dead_factors — the RESERVED SLOT you plumb); risk/capacity.hpp (capacity_curve ->
{aum, net_edge_bps}, monotone non-increasing); eval/deflated_sharpe.hpp (deflated_sharpe/probabilistic_sharpe
/expected_max_sharpe — the EXACT DSR, your decay reference); eval/perf_metrics.hpp + combine/metrics.hpp
(compute_return_metrics / compute_metrics — ONE Sharpe convention, reuse it); library/library.hpp +
lifecycle.hpp (Library::mark(id, to, as_of) / state_as_of — the APPEND-ONLY PIT journal with the legal-edge
table ALREADY BUILT; you DRIVE it, never UPDATE a row, never re-implement the edges) + LibraryStore::positions
(dead-alpha holdings); combine/combined_source.hpp (CombinedSignalSource — the mega-alpha as ISignalSource,
NON-OWNING constituents you keep alive); cost/cost_aware.hpp + cost/capacity.hpp (cost_aware_knobs ->
{kappa, gate, fitness_cost_floor}, capacity_point zero-crossing — ON THE feat/atx-engine-cost BRANCH; if
unmerged, take the three scalars via book::CostInputs); atx-core linalg (ols/MatX/VecX), Eigen
SelfAdjointEigenSolver (FIXED SIGN convention for reproducibility).

THIS SPRINT'S DOMINANT RISK IS A SILENTLY-WRONG OPERATING BOOK: a multi-period solver that leaks look-ahead
or loses determinism across the schedule; a decay monitor that is VACUOUS (flags nothing, or flags everything);
a dead-factor extraction that does NOT actually reduce exposure; a report that is not reproducible. The gates:
  - DETERMINISM (R1): two builds of the SAME schedule => BYTE-IDENTICAL book matrix + report digest. The inner
    solve is fixed-iteration; the schedule walk + trade-rate + eigensolver sign + every reduction are order-fixed.
  - LOOK-AHEAD / RECEDING-HORIZON (R2): the multi-period objective may SUM over a horizon but EXECUTES ONLY THE
    FIRST trade; every forecast is fit-on-trailing; the monitor reads only realized PnL <= t. TRUNCATION-INVARIANT
    at every fitted boundary (forecast, decay baseline, dead-factor refresh). A later transition NEVER alters an
    earlier state_as_of(id, t).
  - DECAY NON-VACUOUS (R5): a PLANTED-DECAYING alpha (Sharpe drops post-admission) is demoted Live->Decaying->Dead;
    a STABLE alpha is NOT demoted. The detector must control the false-alarm rate (a test that demotes everything
    is as broken as one that demotes nothing). "Trust slowly, retire fast" = asymmetric thresholds.
  - DEAD-FACTOR NEUTRALIZES (R6): project the optimized book onto the dead-alpha eigen-subspace; the exposure norm
    AFTER feeding the dead factors into the risk model must be STRICTLY LESS than before (the recycling actually
    works). Eigensolver sign convention pinned for reproducibility.
  - CAPACITY-BOUNDED (R7): the allocated gross leverage NEVER exceeds the calibrated capacity point.
  - REPRODUCIBLE REPORT (R8): two runs => byte-identical report files.

PIT, APPEND-ONLY: lifecycle transitions are journal appends keyed to an as-of period; never back-date. NaN/delisted
names get 0 weight and round-trip verbatim (no survivorship). Reuse ONE Sharpe convention (compute_metrics) and ONE
DSR (eval::deflated_sharpe); reuse pairwise_complete_corr for any correlation. Header-only inline EXCEPT you link
atx-core's compiled db/sqlite TU transitively via library:: (already resolved by S4). Functions <= ~60 lines.
Build gate: cmake --build build --config Debug --target atx-engine-tests (/W4 /permissive- /WX + /fp:precise)
+ ctest -R <Suite>.

Shared-branch discipline: stage EXPLICIT pathspecs only (git add -- <paths>; git commit -- <paths>); NEVER
git add -A; after committing run `git show HEAD --stat` (only your files); never touch atx-core/* or atx-tsdb/*;
do not push. End commit messages with: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```

---

## §4 — Architecture & algorithms (data structures + pseudocode)

### 4.1 Multi-period optimizer — receding-horizon driver over `PortfolioOptimizer` (S7-1)

The "multi-period" property single-period cannot express: **turnover measured across the real schedule** by threading `w_prev` period-to-period, with the calibrated κ in the L1 prox. Optionally a Gârleanu-Pedersen **partial trade-rate** (trade part-way toward the target each period — the smooth-aim result for quadratic costs). The inner solver (`optimizer.hpp`, §0.2) is reused verbatim.

```cpp
// section: risk/multi_period.hpp  (namespace atx::engine::risk)
struct RebalanceSchedule {
  std::vector<atx::usize> periods;        // ascending as-of period indices where we rebalance (PIT, §0.4)
};

struct MultiPeriodConfig {
  OptimizerConfig single;                 // λ, κ, L, name_cap, dollar_neutral, max_iters — REUSED verbatim
  atx::f64 trade_rate = 1.0;              // Gârleanu-Pedersen partial step toward target ∈ (0,1]; 1 => full
  bool      capacity_bound_gross = true;  // clip single.gross_leverage at the capacity ceiling (S7-4 supplies it)
};

struct MultiPeriodResult {
  std::vector<std::vector<atx::f64>> books;     // one weight vector per schedule period (book[s][i])
  std::vector<atx::f64>              turnover;   // per-period Σ_i |book[s] − book[s-1]| (one-sided from flat at s=0)
  std::vector<atx::f64>              cost_bps;   // per-period calibrated cost charged on that turnover (R3)
};

class MultiPeriodOptimizer {
public:
  MultiPeriodConfig cfg;

  // alpha_at(s) -> the combined mega-alpha forecast at schedule period s (NaN == no opinion; fit-on-trailing,
  //               so reading it is causal, R2). model_at(s) -> the FactorModel valid at s (trailing-fit).
  // cost_kappa, round_trip_cost_bps come from CostInputs (§0.1). Returns the book schedule + turnover + cost.
  [[nodiscard]] atx::core::Result<MultiPeriodResult>
  run(const RebalanceSchedule& sched,
      const std::function<std::span<const atx::f64>(atx::usize s)>& alpha_at,
      const std::function<const FactorModel&(atx::usize s)>& model_at,
      const book::CostInputs& cost) const
  {
    MultiPeriodResult out;
    out.books.reserve(sched.periods.size());
    std::vector<atx::f64> w_prev;                                  // EMPTY at s=0 => flat (optimizer treats as 0)
    OptimizerConfig oc = cfg.single;
    oc.turnover_penalty = cost.kappa;                              // R3: calibrated κ, not the default
    if (cfg.capacity_bound_gross) oc.gross_leverage =
        std::min(oc.gross_leverage, cost.capacity_gross);         // R7: capacity-bounded gross
    PortfolioOptimizer opt{oc};

    for (atx::usize s = 0; s < sched.periods.size(); ++s) {
      const auto a = alpha_at(sched.periods[s]);                  // causal forecast at this period (R2)
      const FactorModel& V = model_at(sched.periods[s]);
      ATX_TRY(std::vector<atx::f64> target, opt.solve(a, V, w_prev));   // the EXACT single-period book (§0.2)
      // Gârleanu-Pedersen partial trade-rate: move part-way toward target from w_prev (smooth aim).
      // trade_rate == 1 => book == target (reduces to the chained single-period solve).
      std::vector<atx::f64> book = blend_toward(w_prev, target, cfg.trade_rate);   // deterministic, order-fixed
      out.turnover.push_back(l1_diff(book, w_prev));              // Σ|book − w_prev| (one-sided)
      out.cost_bps.push_back(out.turnover.back() * cost.round_trip_cost_bps);      // R3
      out.books.push_back(book);
      w_prev = std::move(book);                                   // THREAD the book forward — the multi-period crux
    }
    return atx::core::Ok(std::move(out));
  }
private:
  // book = w_prev + trade_rate·(target − w_prev). At trade_rate=1 this is `target`. Dead (NaN-target) names
  // are 0 in both target and w_prev, so they stay 0. Order-fixed (ascending i). No allocation beyond the result.
  static std::vector<atx::f64> blend_toward(std::span<const atx::f64> w_prev,
                                            std::span<const atx::f64> target, atx::f64 rate);
  static atx::f64 l1_diff(std::span<const atx::f64> a, std::span<const atx::f64> b);  // Σ|a−b|, empty b => Σ|a|
};
```
**Look-ahead safety (R2):** the objective conceptually sums over a forecast horizon, but the driver executes **only the realized first move** at each rebalance (Boyd MPC) — `alpha_at(s)` and `model_at(s)` are trailing-fit, so the executed book is a causal function of `≤ s` data. **Determinism (R1):** `solve` is fixed-iteration; `blend_toward`/`l1_diff` are order-fixed; the schedule is ascending. **Boundary:** `trade_rate=1` + a one-period schedule + empty `w_prev` ⇒ the result equals a single `PortfolioOptimizer::solve` bit-for-bit (the regression pin against P4).

### 4.2 Alpha-decay monitor — Page-Hinkley + DSR drop, PIT, asymmetric (S7-2)

Detect a statistically-significant **downward shift** in a live alpha's realized performance vs its admitted backtest, as fast as possible at a controlled false-alarm rate, then demote through the S4 lifecycle. Two complementary detectors: a fast streaming **Page-Hinkley** (down-mode, minimax-optimal for a mean shift) gated by a **MinTRL** floor (no demotion until the live Sharpe gap is statistically distinguishable), cross-checked by a **realized-DSR drop** below the admitted DSR.

```cpp
// section: book/decay_monitor.hpp  (namespace atx::engine::book)
struct AdmittedBaseline {                  // frozen at first observation from lib.pnl(id) (PIT, §0.6)
  atx::f64   sr_admit;   atx::f64 skew;    atx::f64 exkurt;   atx::usize t_admit;
  atx::f64   dsr_admit;  atx::usize n_trials;                  // the admission deflation N
  atx::f64   mean_admit; atx::f64 sd_admit;                    // for standardizing the live stream
};

struct PageHinkleyState {                  // one-sided LOWER (down) detector, O(1) update [Page 1954]
  atx::f64 mean = 0.0; atx::f64 cum = 0.0; atx::f64 max = 0.0; atx::usize n = 0;
};

struct DecayConfig {
  atx::f64   ph_delta   = 0.005;           // allowable downward drift tolerance δ
  atx::f64   ph_lambda  = 50.0;            // PH alarm threshold λ (tunes the ARL / false-alarm rate)
  atx::usize ph_min_obs = 30;              // min live obs before PH may trip
  atx::f64   psr_alpha  = 0.05;            // significance for the realized-DSR-drop gate
  atx::usize confirm_periods = 5;          // Decaying->Dead persistence (RETIRE FAST: small) — R5
  atx::usize recover_periods = 60;         // Decaying->Live restoration (TRUST SLOWLY: large) — R5
  atx::f64   cost_flood_safety = 1.0;      // edge>safety·cost => not decay, just cost-flooded (§0.1/R3)
};

struct DecayVerdict {
  bool flag; library::LifecycleState recommend;  // Live / Decaying / Dead / (unchanged)
  atx::f64 realized_psr; atx::f64 realized_dsr; atx::usize min_trl; bool cost_flooded;
};

class DecayMonitor {
public:
  DecayConfig cfg;

  // Call once per live period with the realized return r_t at as_of period t (r_t already net-of-cost).
  // `live_window` is the alpha's realized PnL since admission (the OOS continuation). Reuses eval::* verbatim.
  [[nodiscard]] DecayVerdict observe(const AdmittedBaseline& base, std::span<const atx::f64> live_window,
                                     atx::f64 gross_edge_bps, atx::f64 round_trip_cost_bps,
                                     PageHinkleyState& ph) const
  {
    // 1. Page-Hinkley DOWN update on the standardized realized return (z<0 => underperforming admitted mean).
    const atx::f64 z = (live_window.back() - base.mean_admit) / std::max(base.sd_admit, 1e-12);
    ph.n += 1; ph.mean += (z - ph.mean) / static_cast<atx::f64>(ph.n);
    ph.cum += (z - ph.mean - cfg.ph_delta);           // accumulate downward deviation
    ph.max  = std::max(ph.max, ph.cum);
    const bool ph_trip = (ph.n >= cfg.ph_min_obs) && (ph.max - ph.cum > cfg.ph_lambda);

    // 2. Realized DSR/PSR over the live window (skew/kurt-aware SE), and the MinTRL gate (§0.6).
    const auto m = realized_moments(live_window);     // (sr_live, skew, exkurt, T_live) — order-fixed
    const atx::f64 psr  = eval::probabilistic_sharpe(m.sr, base.sr_admit, m.t, m.skew, m.exkurt);  // EXACT (S1)
    const auto dsr      = eval::deflated_sharpe(m.sr, m.t, m.skew, m.exkurt, base.n_trials, std::nullopt);
    const atx::usize min_trl = min_track_record_len(m, base, cfg.psr_alpha);   // can we even tell yet?
    const bool dsr_drop = (m.t >= min_trl) && (dsr.dsr < base.dsr_admit) && (psr < cfg.psr_alpha);

    // 3. Cost-flooding discriminator (S6 baton, §0.1): a still-good alpha whose NET decayed only because cost
    //    rose is NOT decaying — it should be sized down, not retired. edge clears cost => not decay.
    const bool cost_flooded = !cost::should_trade(gross_edge_bps, round_trip_cost_bps, cfg.cost_flood_safety);

    DecayVerdict v{false, library::LifecycleState::Live, psr, dsr.dsr, min_trl, cost_flooded};
    if ((ph_trip || dsr_drop) && !cost_flooded) { v.flag = true; v.recommend = library::LifecycleState::Decaying; }
    return v;   // the caller maps the verdict to a Library::mark transition (with confirm/recover hysteresis)
  }
};
```
**Driving the lifecycle (R5, §0.5):** a thin `DecayController` holds per-alpha `(PageHinkleyState, consecutive-trip count)` and maps verdicts to `Library::mark`: `Live→Decaying` on the first non-cost-flooded flag past MinTRL; `Decaying→Dead` after `confirm_periods` consecutive flags (retire fast); `Decaying→Live` after `recover_periods` clean periods with `psr` back above threshold (trust slowly — asymmetric). Every `mark` is keyed to the live `as_of` and is a journal append (PIT). **Non-vacuity (R5, the load-bearing test):** a planted-decaying alpha (Sharpe halves post-admission) is demoted within a bounded number of periods; a stable alpha is **never** demoted across the same horizon (false-alarm control). **PIT (R2):** the monitor reads only `live_window` (realized `≤ t`) and the frozen baseline; no future leak.

### 4.3 Dead-alpha → risk-factor extraction (Kakushadze) + FactorModel augmentation (S7-3)

A dead alpha's holdings encode a no-expected-return direction. Build the **holdings overlap matrix**, eigen-decompose, keep `K = round(eRank)` principal components, and feed them as endogenous risk-factor columns into `V = XFXᵀ + D` so live alphas can be neutralized against them.

```cpp
// section: risk/dead_factor.hpp  (namespace atx::engine::risk)
struct FactorComponents { atx::core::linalg::MatX X; atx::core::linalg::MatX F; atx::core::linalg::VecX D;
                          atx::usize fit_end; };   // the build_components refactor exposes the fundamental estimate

struct DeadAlphaFactors {
  atx::core::linalg::MatX  loadings;        // M×K_dead: dead-factor exposure columns (sign-fixed eigenvectors)
  atx::core::linalg::VecX  variances;       // K_dead: dead-factor variances (the kept eigenvalues) => F_dead diag
  atx::usize               k_dead;          // = round(eRank), entropy-based (Kakushadze)
};

// Kakushadze & Yu 1709.06641: X_AB = Σ_{i∈dead} P_iA P_iB over L1-normalized dead-alpha holdings at as_of.
[[nodiscard]] atx::core::Result<DeadAlphaFactors>
extract_dead_factors(const library::Library& lib, std::span<const combine::AlphaId> dead_ids,
                     atx::usize as_of_period, atx::usize universe_size)
{
  if (dead_ids.empty()) return atx::core::Ok(DeadAlphaFactors{});   // no dead alphas => no extra factors (boundary)
  atx::core::linalg::MatX overlap = atx::core::linalg::MatX::Zero(M, M);     // M = universe_size, symmetric PSD
  for (auto id : dead_ids) {                                                 // order-fixed (ascending AlphaId)
    auto P = lib.store().positions(id, as_of_period);                        // holdings cross-section (§0.5)
    auto p = l1_normalize_ignoring_nan(P);                                   // Σ|p_A| = 1 (NaN->0); copy out (R4)
    overlap += outer(p, p);                                                  // accumulate P_iA P_iB
  }
  Eigen::SelfAdjointEigenSolver<MatX> es(overlap);                           // deterministic symmetric eig
  // descending eigenvalues; FIXED SIGN convention (largest-|component| made positive) => bit-reproducible (R1).
  auto [evals, evecs] = sort_desc_sign_fixed(es);
  const atx::usize k = effective_rank(evals);                               // K = round(exp(-Σ p_a ln p_a))
  return atx::core::Ok(DeadAlphaFactors{ evecs.leftCols(k), evals.head(k), k });
}

// eRank (Roy-Vetterli / Kakushadze): exp(Shannon entropy of the normalized eigenvalue spectrum). Order-fixed.
[[nodiscard]] inline atx::usize effective_rank(const atx::core::linalg::VecX& evals) {
  atx::f64 sum = 0.0; for (Eigen::Index i=0;i<evals.size();++i) sum += std::max(evals[i], 0.0);
  if (sum <= 0.0) return 0;
  atx::f64 H = 0.0;
  for (Eigen::Index i=0;i<evals.size();++i) { const atx::f64 p = std::max(evals[i],0.0)/sum;
    if (p > 0.0) H -= p * std::log(p); }
  return static_cast<atx::usize>(std::llround(std::exp(H)));
}

// Compose the fundamental (X,F,D) with the dead columns: V_aug = [X | X_dead] blockdiag(F, diag(var_dead)) [.]ᵀ + D.
[[nodiscard]] atx::core::Result<FactorModel>
augment_factor_model(const FactorComponents& base, const DeadAlphaFactors& dead) {
  if (dead.k_dead == 0) return FactorModel::create(base.X, base.F, base.D, 0, base.fit_end);  // passthrough
  MatX Xa(base.X.rows(), base.X.cols() + dead.k_dead);  Xa << base.X, dead.loadings;          // hstack columns
  MatX Fa = MatX::Zero(Xa.cols(), Xa.cols());
  Fa.topLeftCorner(base.F.rows(), base.F.cols()) = base.F;                                    // fundamental block
  for (atx::usize j=0;j<dead.k_dead;++j) Fa(base.F.rows()+j, base.F.rows()+j) = dead.variances[j];  // dead block
  return FactorModel::create(std::move(Xa), std::move(Fa), base.D, 0, base.fit_end);          // existing create (§0.3)
}
```
**The one reserved-slot touch (§0.3):** `FactorModelBuilder` gains `build_components(...) -> Result<FactorComponents>` (the estimation extracted from the current `build()`, which becomes a thin wrapper). **Measurable neutralization (R6, the load-bearing test):** define the book's dead-subspace exposure `‖X_deadᵀ w‖`; optimize the book once with the base model and once with the augmented model; assert the augmented book's dead-subspace exposure is **strictly smaller** (the optimizer's `V⁻¹` tilt + `neutralize` steer the book off the no-money directions). **Determinism (R1):** the overlap accumulation is order-fixed; the eigensolver sign convention is pinned; eRank is an order-fixed reduction. **Recycling (R5):** after harvesting, the controller marks each dead alpha `Dead→Recycled` at the as-of period.

### 4.4 Capital allocation — breadth-driven, capacity-bounded Kelly (S7-4)

Size the **one unified book**'s gross leverage from its realized Sharpe (fractional Kelly = mean-variance leverage, Thorp), clipped at the S6 capacity ceiling; cross-check effective breadth via the Fundamental Law.

```cpp
// section: book/allocation.hpp  (namespace atx::engine::book)
struct AllocationConfig {
  atx::f64 fractional_kelly = 0.5;        // c ∈ (0,1]: L = c·SR/σ (half-Kelly default — robustness)
  atx::f64 max_gross        = 4.0;        // hard cap regardless of Kelly/capacity
};

// Effective breadth = participation ratio of the alpha-return covariance eigenspectrum: (Σλ)²/Σλ².
// Many weakly-correlated alphas => BR_eff ≈ N; fully-correlated => BR_eff ≈ 1. Order-fixed. (Grinold-Kahn cross-check.)
[[nodiscard]] atx::f64 effective_breadth(const atx::core::linalg::VecX& cov_eigenvalues);

// L = clip( c · SR_book / σ_book , 0, min(L_capacity, max_gross) ). Capacity-bounded (R7).
[[nodiscard]] inline atx::f64 size_book(atx::f64 sr_book, atx::f64 sigma_book,
                                        atx::f64 capacity_gross, const AllocationConfig& cfg) {
  if (sigma_book <= 0.0) return 0.0;
  const atx::f64 kelly = cfg.fractional_kelly * sr_book / sigma_book;        // fractional Kelly = MV leverage
  return std::clamp(kelly, 0.0, std::min(capacity_gross, cfg.max_gross));    // NEVER exceed capacity (R7)
}
```
**Capacity ceiling (R7/§0.9):** `capacity_gross` comes from `cost::capacity_point(risk::capacity_curve(book, panel, sim, aum_grid))` (or the local zero-crossing fallback), expressed as a deployable gross given AUM. **Cross-check:** `IR ≈ TC·IC·√BR_eff` is logged to the ledger as a sanity bound (not a gate). **Boundary:** capacity below the Kelly leverage ⇒ the book is capacity-clipped (the test asserts the returned gross == `capacity_gross`).

### 4.5 Book-level reporting artifacts — headless, reproducible (S7-4)

Accumulate the book's series from the multi-period schedule (since `Portfolio` has no series, §0.7) and emit byte-identical files.

```cpp
// section: book/report.hpp  (namespace atx::engine::book)
struct BookReport {
  std::vector<atx::f64> equity_curve;            // net-of-cost cumulative (R3)
  std::vector<atx::f64> gross_leverage, net_exposure, turnover;
  std::vector<atx::f64> pnl_gross, pnl_net, pnl_cost;       // per-period P&L attribution
  atx::core::linalg::MatX factor_exposures;      // per-period book exposure Xᵀw to each risk factor (incl dead)
  std::vector<atx::f64> capacity_utilization;    // gross / capacity_point per period
  std::array<atx::usize, 6> lifecycle_census;    // count per LifecycleState at snapshot
};

// Build the report from the book schedule × realized step-returns − calibrated cost. Order-fixed; cold path.
[[nodiscard]] BookReport accumulate_report(const MultiPeriodResult& books, const alpha::Panel& panel,
                                           const RebalanceSchedule& sched, const FactorModel& V,
                                           const library::Library& lib, atx::usize as_of);

// Emit deterministic TSV/CSV files (one per series) under `dir`. Byte-identical across two runs (R8).
[[nodiscard]] atx::core::Status write_report(const BookReport&, const std::string& dir);
```
**Attribution:** the per-period book P&L is regressed on the risk-model factors (`atx::core::linalg::ols`) to split factor vs idiosyncratic contribution (Brinson / factor attribution). **Reproducibility (R8):** the accumulation is order-fixed over deterministic books; `write_report` formats with fixed precision; two runs ⇒ byte-identical files (the test diffs the bytes).

### 4.6 The E2E pipeline orchestrator (S7-5)

```cpp
// section: book/pipeline.hpp  (namespace atx::engine::book)
BookPipeline::run(cfg) -> PipelineReport:
  // 1. MINE + ADMIT (S3/S4) — reuse ResearchDriver verbatim (mine→gate→admit→snapshot)
  research := ResearchDriver(lib, dsl, panel, sim, policy, gate).run(cfg.research)
  // 2. PROMOTE to Live (S4 lifecycle) — the first production driver past Admitted (§0.5)
  for id in lib.admitted_ids(): lib.mark(id, Live, as_of = cfg.go_live_period)
  // 3. COMBINE (S5/P4) — fit the mega-alpha over the trailing window; hold constituents alive (§0.8)
  sources := re_eval_sources(lib)                       // re-parse expr_source -> VmSignalSource (S4b residual)
  combo   := AlphaCombiner{cfg.comb}.fit(pool, fit_begin, fit_end)
  mega    := CombinedSignalSource(sources, combo, cfg.comb.method)
  // 4. RISK + DEAD FACTORS (S7.3) — build the fundamental model, augment with dead-alpha directions
  base    := FactorModelBuilder{cfg.fm}.build_components(panel, window, cap, group)
  dead    := extract_dead_factors(lib, lib.ids_in_state(Dead, as_of), as_of, M)
  V       := augment_factor_model(base, dead)
  // 5. ALLOCATE (S7.4) — capacity-bounded fractional-Kelly gross
  cap_gross := zero_crossing(capacity_curve(seed_book, panel, sim, aum_grid))
  L         := size_book(sr_book, sigma_book, cap_gross, cfg.alloc)
  cost      := CostInputs{ cost_aware_knobs(calibrated, ref_part, ref_sig, horizon) , round_trip, cap_gross→L }
  // 6. OPTIMIZE over the schedule (S7.1) — multi-period, calibrated κ, capacity-bounded L
  books := MultiPeriodOptimizer{cfg.mp(L)}.run(sched, alpha_at(mega), model_at(V), cost)
  // 7. MONITOR decay (S7.2) over the forward window — demote a planted-decaying alpha through the lifecycle
  for s in forward_periods: controller.step(lib, monitor.observe(...), as_of = s)
  // 8. RECYCLE (S7.3) — any freshly-Dead alpha harvested as a risk factor is marked Recycled
  for id in newly_dead: lib.mark(id, Recycled, as_of)
  // 9. REPORT (S7.4) — accumulate + write byte-identical artifacts
  report := accumulate_report(books, panel, sched, V, lib, as_of); write_report(report, cfg.out_dir)
  return PipelineReport{ research, books.digest(), report.digest(), lifecycle_census }
```
**The capstone proof (R8):** one deterministic, point-in-time, cost-honest run asserts all seven invariants **simultaneously** — byte-identical replay (digest equal across two runs), no look-ahead at every fitted boundary (combiner weights, factor model, calibrated cost, multi-period forecast, decay baseline, dead-factor refresh all truncation-invariant), no survivorship (delisted names round-trip), honest+calibrated cost (the turnover term uses κ; net < gross), capacity-bounded (gross ≤ capacity), the lifecycle driven end-to-end (a planted-decaying alpha reaches `Dead`/`Recycled`), and the dead-factor recycling measurably neutralizing. **This test is the v2 exit gate.**

---

## §5 — Per-unit plan

> Sequential dispatch (each unit consumes the prior). Fresh implementer → spec-compliance review → code-quality review → fix loop → ledger SHA, per `superpowers:subagent-driven-development`. **Shared branch `feat/atx-core-stdlib` → explicit-pathspec commits** (handoff block). Suggested split: **S7-a** = S7-0…S7-2, **S7-b** = S7-3…S7-5.

### Task S7-0: Marker + ledger + book scaffold + S6-merge prerequisite check
**Files:** Create `atx-engine/plans/p1/sprint-7-progress.md`, `atx-engine/include/atx/engine/book/fwd.hpp`.
- [ ] **Step 1:** Write the ledger from the `sprint-6-progress.md` shape: header (`Base: feat/atx-core-stdlib @ <HEAD>`, in-place, shared-branch note), a "Plan adjustments vs source" paragraph quoting §0 (S6-cost-on-unmerged-branch + scalar-adapter-fallback; single-period-solver-reused-not-rebuilt; dead-factor-slot-reserved-but-unplumbed; lifecycle-spine-built-but-undriven-past-Admitted; AlphaMetrics-has-no-DSR-baseline-recomputed-from-stored-pnl; Portfolio-has-no-series-report-accumulates; mega-alpha-via-CombinedSource-keep-constituents-alive; capacity-curve-present-rootfind-on-cost-branch), empty per-unit table S7-0…S7-5, empty commits table. Record the **four Pattern-B edges** (§2.1) and the **S6-merge hard prerequisite** (§0.1) as kickoff risks.
- [ ] **Step 2: S6-merge prerequisite check.** Confirm with the user whether `feat/atx-engine-cost` is merged+rebased over S5. Run `git branch --list feat/atx-engine-cost` and check `include/atx/engine/cost/cost_aware.hpp` exists. **If present:** S7 programs against `cost::` directly. **If absent:** S7 uses the `book::CostInputs` scalar adapter (κ, round_trip_cost_bps, capacity_gross passed explicitly); record the decision in the ledger. Either way `book::CostInputs` is the seam, so the merge state does not block S7-1.
- [ ] **Step 3:** `book/fwd.hpp` — forward decls (the `risk/fwd.hpp` pattern): namespace `atx::engine::book`; `struct RebalanceSchedule; struct MultiPeriodConfig; struct MultiPeriodResult; class MultiPeriodOptimizer;` (note: defined in `risk/multi_period.hpp` but used by book/) `struct CostInputs; struct AdmittedBaseline; struct DecayConfig; struct DecayVerdict; class DecayMonitor; struct AllocationConfig; struct BookReport; class BookPipeline;` + a doc block: the operating-book model (schedule → optimize → monitor → recycle → allocate → report), the **risk/-extensions (multi_period, dead_factor) vs book/-new** split, and the **CostInputs scalar-adapter** rationale.
- [ ] **Step 4:** Commit (marker): `git add -- atx-engine/plans/p1/sprint-7-progress.md atx-engine/include/atx/engine/book/fwd.hpp; git commit -- <them> -m "docs(s7-0): open sprint-7 portfolio-lifecycle ledger + book scaffold" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"; git show HEAD --stat`.

### Task S7-1: Multi-period / dynamic optimizer
**Files:** Create `risk/multi_period.hpp`, `book/fwd.hpp` (add `CostInputs`); Test `tests/risk_multi_period_test.cpp`.
**Scope:** §4.1 + §0.2/§0.9. The `RebalanceSchedule`/`MultiPeriodConfig`/`MultiPeriodResult`, `MultiPeriodOptimizer::run` (schedule walk over `PortfolioOptimizer::solve`, `w_prev` threading, calibrated κ, trade-rate blend, capacity-bounded L). **First verify** `risk::PortfolioOptimizer::solve` signature + `OptimizerConfig` fields + `FactorModel` against the headers.
- [ ] **Step 1 (failing tests):** suite `MultiPeriodOptimizer` —
```cpp
TEST(MultiPeriodOptimizer, SinglePeriodScheduleEqualsSingleSolve) {           // §0.2 regression pin — load-bearing
  auto V = make_factor_model(/*M*/8, /*K*/2);
  std::vector<double> a = make_alpha(8);
  RebalanceSchedule sched{{0}};
  MultiPeriodConfig cfg; cfg.single = default_oc(); cfg.trade_rate = 1.0;
  CostInputs cost{/*kappa*/0.0, /*rt_bps*/0.0, /*cap*/1e9};
  auto mp = MultiPeriodOptimizer{cfg}.run(sched, const_alpha(a), const_model(V), cost);
  auto sp = PortfolioOptimizer{default_oc()}.solve(a, V, {});
  ASSERT_TRUE(mp.has_value() && sp.has_value());
  for (size_t i=0;i<a.size();++i)
    EXPECT_EQ(std::bit_cast<uint64_t>(mp->books[0][i]), std::bit_cast<uint64_t>((*sp)[i]));  // bit-identical
}
TEST(MultiPeriodOptimizer, ThreadsPrevBookAcrossSchedule) {                    // the multi-period crux
  // A two-period schedule where α flips sign: turnover[1] must be measured from book[0], NOT from flat.
  RebalanceSchedule sched{{0,1}};
  auto mp = MultiPeriodOptimizer{cfg_with_kappa(0.0)}.run(sched, flip_alpha(), const_model(V), cost);
  EXPECT_GT(mp->turnover[1], 0.0);                                             // it traded against the prior book
  EXPECT_NEAR(mp->turnover[1], l1_diff(mp->books[1], mp->books[0]), 1e-12);    // measured across the schedule
}
TEST(MultiPeriodOptimizer, CalibratedKappaCutsTurnover) {                      // R3
  auto lo = MultiPeriodOptimizer{cfg_with_kappa(0.0)}.run(sched2, flip_alpha(), const_model(V), cost0);
  auto hi = MultiPeriodOptimizer{cfg_with_kappa(0.5)}.run(sched2, flip_alpha(), const_model(V), cost_hi);
  EXPECT_LT(sum(hi->turnover), sum(lo->turnover));                             // κ>0 reduces turnover
}
TEST(MultiPeriodOptimizer, CapacityBoundsGross) {                             // R7
  CostInputs tight{0.0, 0.0, /*cap*/0.5};
  auto mp = MultiPeriodOptimizer{cfg_cap_on()}.run(sched, const_alpha(a), const_model(V), tight);
  for (auto& book : mp->books) EXPECT_LE(l1_norm(book), 0.5 + 1e-9);           // gross never exceeds capacity
}
TEST(MultiPeriodOptimizer, TwoBuildsByteIdentical) {                          // R1
  auto a1 = MultiPeriodOptimizer{cfg}.run(sched3, alpha_seq(), model_seq(), cost);
  auto a2 = MultiPeriodOptimizer{cfg}.run(sched3, alpha_seq(), model_seq(), cost);
  EXPECT_EQ(digest(a1->books), digest(a2->books));                            // deterministic schedule
}
TEST(MultiPeriodOptimizer, TradeRatePartialStep) {                           // Gârleanu-Pedersen smoothing
  auto full = MultiPeriodOptimizer{cfg_rate(1.0)}.run(sched, const_alpha(a), const_model(V), cost);
  auto half = MultiPeriodOptimizer{cfg_rate(0.5)}.run(sched, const_alpha(a), const_model(V), cost);
  EXPECT_LT(l1_diff(half->books[0], std::vector<double>(8,0.0)),               // half steps less from flat
            l1_diff(full->books[0], std::vector<double>(8,0.0)));
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `risk/multi_period.hpp` (§4.1): the structs + `run` (schedule loop, `oc.turnover_penalty = cost.kappa`, `oc.gross_leverage = min(L, cost.capacity_gross)` when `capacity_bound_gross`, `solve` per period, `blend_toward`, `l1_diff`, thread `w_prev`), `book::CostInputs{f64 kappa; f64 round_trip_cost_bps; f64 capacity_gross;}` in `book/fwd.hpp`. **Step 4:** `ctest -R MultiPeriodOptimizer` → pass; full suite green. **Step 5:** Commit + ledger row (`feat(s7-1): multi-period turnover-aware optimizer (receding-horizon over PortfolioOptimizer)`). Note the **L7 QP→atx-core** Pattern-B residual.

### Task S7-2: Alpha-decay monitor
**Files:** Create `book/decay_monitor.hpp`; Test `tests/book_decay_monitor_test.cpp`.
**Scope:** §4.2 + §0.5/§0.6. `AdmittedBaseline`/`PageHinkleyState`/`DecayConfig`/`DecayVerdict`, `DecayMonitor::observe` (Page-Hinkley down-mode + realized DSR/PSR drop + MinTRL gate + cost-flooding discriminator), the `DecayController` that maps verdicts to `Library::mark` with asymmetric hysteresis. **First verify** `eval::deflated_sharpe`/`probabilistic_sharpe` signatures + `library::Library::mark`/`state_as_of` + the `LifecycleState` legal edges + (if merged) `cost::should_trade`.
- [ ] **Step 1 (failing tests):** suite `DecayMonitor` — the **non-vacuous** decay proof (both directions):
```cpp
TEST(DecayMonitor, DemotesPlantedDecayingAlpha) {                            // R5 — non-vacuous (must flag)
  auto base = baseline_from(strong_admitted_pnl());                          // SR_admit ~ 2.0
  DecayMonitor mon{default_decay_cfg()}; PageHinkleyState ph;
  std::vector<double> live;                                                  // realized: Sharpe halves post-admit
  bool flagged = false;
  for (double r : decaying_stream(/*periods*/120)) { live.push_back(r);
    auto v = mon.observe(base, live, /*gross_bps*/5.0, /*rt_cost_bps*/1.0, ph);
    if (v.flag) { flagged = true; break; } }
  EXPECT_TRUE(flagged);                                                       // a real decay IS detected
}
TEST(DecayMonitor, DoesNotDemoteStableAlpha) {                              // R5 — false-alarm control (must NOT flag)
  auto base = baseline_from(strong_admitted_pnl());
  DecayMonitor mon{default_decay_cfg()}; PageHinkleyState ph;
  std::vector<double> live; bool flagged = false;
  for (double r : stable_stream(/*periods*/120, /*sharpe*/2.0)) { live.push_back(r);
    if (mon.observe(base, live, 5.0, 1.0, ph).flag) { flagged = true; break; } }
  EXPECT_FALSE(flagged);                                                      // a still-good alpha survives
}
TEST(DecayMonitor, MinTrlGatesEarlyDemotion) {                              // §0.6 — don't fire before significance
  auto base = baseline_from(strong_admitted_pnl());
  DecayMonitor mon{default_decay_cfg()}; PageHinkleyState ph;
  std::vector<double> live = first_few(decaying_stream(120), /*n*/5);         // too short to be significant
  auto v = mon.observe(base, live, 5.0, 1.0, ph);
  EXPECT_GE(v.min_trl, live.size());                                          // MinTRL not yet reached
  EXPECT_FALSE(v.flag);                                                       // no demotion on 5 obs
}
TEST(DecayMonitor, CostFloodedIsNotDecay) {                                 // R3/§0.1 — discriminator
  auto base = baseline_from(strong_admitted_pnl());
  DecayMonitor mon{default_decay_cfg()}; PageHinkleyState ph;
  auto v = mon.observe(base, weak_net_stream(80), /*gross_bps*/8.0, /*rt_cost_bps*/9.0, ph);  // edge < cost
  EXPECT_TRUE(v.cost_flooded);
  EXPECT_FALSE(v.flag);                                                       // sized down, not retired
}
TEST(DecayController, DrivesLifecyclePitAndAsymmetric) {                     // R5/§0.5 — drives Library::mark, PIT
  auto lib = open_library_with_one_live_alpha();
  DecayController ctl{default_decay_cfg()};
  for (size_t t=0;t<200;++t) ctl.step(lib, AlphaId{0}, decaying_stream(200)[t], /*as_of*/t);
  EXPECT_EQ(lib.state_as_of(AlphaId{0}, 199).value(), LifecycleState::Dead);  // Live->Decaying->Dead
  EXPECT_EQ(lib.state_as_of(AlphaId{0}, 10).value(),  LifecycleState::Live);  // earlier query unchanged (PIT)
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `book/decay_monitor.hpp` (§4.2): `baseline_from(pnl)` (recompute sr/skew/exkurt/mean/sd + admitted DSR via `eval::deflated_sharpe`), the Page-Hinkley down update, `realized_moments`, `min_track_record_len` (skew/kurt-aware SE × `(z_{1-α}/(sr_live−sr_admit))²`), `observe`, and `DecayController` (per-alpha PH state + consecutive-trip count → `Library::mark`, confirm/recover hysteresis). If `cost::should_trade` unmerged, inline the `edge > safety·cost` check. **Step 4:** `ctest -R "DecayMonitor|DecayController"` → pass; full suite green. **Step 5:** Commit + ledger row (`feat(s7-2): alpha-decay monitor (Page-Hinkley + DSR drop) driving S4 lifecycle`).

### Task S7-3: Dead-alpha → risk-factor extraction + FactorModel augmentation
**Files:** Create `risk/dead_factor.hpp`; Modify `risk/factor_model.hpp` (add `FactorModelBuilder::build_components`, refactor `build` to wrap it — the §0.3 reserved-slot touch); Test `tests/risk_dead_factor_test.cpp`.
**Scope:** §4.3 + §0.3. `FactorComponents`, `extract_dead_factors` (holdings overlap → sign-fixed eigen → eRank truncation), `effective_rank`, `augment_factor_model`. **First verify** `FactorModel::create`, `FactorModelBuilder::build`'s estimation body, `library::LibraryStore::positions(id, period)`, and `Eigen::SelfAdjointEigenSolver`.
- [ ] **Step 1 (failing tests):** suite `DeadFactor` — the **measurable neutralization** proof:
```cpp
TEST(DeadFactor, ExtractsFactorsFromDeadHoldings) {                          // Kakushadze mechanics
  auto lib = library_with_dead_alphas(/*n_dead*/12, /*M*/16);
  auto df = extract_dead_factors(lib, dead_ids(lib), /*as_of*/100, /*M*/16);
  ASSERT_TRUE(df.has_value());
  EXPECT_GE(df->k_dead, 1u); EXPECT_LE(df->k_dead, 16u);                      // eRank in [1, M]
  EXPECT_EQ(df->loadings.cols(), (Eigen::Index)df->k_dead);
}
TEST(DeadFactor, EmptyDeadSetYieldsNoFactors) {                             // boundary
  auto df = extract_dead_factors(lib, {}, 100, 16);
  EXPECT_EQ(df->k_dead, 0u);
}
TEST(DeadFactor, SignConventionIsReproducible) {                            // R1
  auto a = extract_dead_factors(lib, dead_ids(lib), 100, 16);
  auto b = extract_dead_factors(lib, dead_ids(lib), 100, 16);
  EXPECT_TRUE((a->loadings.array() == b->loadings.array()).all());           // bit-identical eigenvectors
}
TEST(DeadFactor, AugmentedModelReducesDeadSubspaceExposure) {               // R6 — load-bearing
  auto base = FactorModelBuilder{cfg}.build_components(panel, window, cap, group);
  auto df   = extract_dead_factors(lib, dead_ids(lib), 100, M);
  auto V0   = FactorModel::create(base->X, base->F, base->D, 0, base->fit_end);   // no dead factors
  auto V1   = augment_factor_model(*base, *df);                                   // with dead factors
  auto w0 = PortfolioOptimizer{oc}.solve(mega_alpha, *V0, {});
  auto w1 = PortfolioOptimizer{oc}.solve(mega_alpha, *V1, {});
  double e0 = dead_subspace_exposure(df->loadings, *w0);   // ‖X_deadᵀ w0‖
  double e1 = dead_subspace_exposure(df->loadings, *w1);   // ‖X_deadᵀ w1‖
  EXPECT_LT(e1, e0);                                                          // neutralization measurably works
}
TEST(DeadFactorBuild, BuildComponentsMatchesBuild) {                        // §0.3 — the refactor is behavior-preserving
  auto comp = FactorModelBuilder{cfg}.build_components(panel, window, cap, group);
  auto full = FactorModelBuilder{cfg}.build(panel, window, cap, group);
  ASSERT_TRUE(comp.has_value() && full.has_value());
  auto re = FactorModel::create(comp->X, comp->F, comp->D, 0, comp->fit_end);
  EXPECT_EQ(re->n_factors(), full->n_factors());
  EXPECT_NEAR(re->risk(probe_w), full->risk(probe_w), 1e-12);                // same model, bit-equal apply
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3a:** Refactor `risk/factor_model.hpp` — extract the estimation body of `FactorModelBuilder::build` into `build_components(...) -> Result<FactorComponents>` (returns `{x0.x, f, d, window}` instead of calling `create`); rewrite `build` to `ATX_TRY(comp, build_components(...)); return FactorModel::create(comp.X, comp.F, comp.D, 0, comp.fit_end);`. **Step 3b:** Implement `risk/dead_factor.hpp` (§4.3): `l1_normalize_ignoring_nan`, overlap accumulation (order-fixed), `Eigen::SelfAdjointEigenSolver` + `sort_desc_sign_fixed` (descending eigenvalue, largest-|component|-positive sign), `effective_rank`, `extract_dead_factors`, `augment_factor_model` (hstack X, block-diagonal F, reuse `create`). **Step 4:** `ctest -R "DeadFactor|DeadFactorBuild|RiskFactor"` → pass (the existing `risk_factor_model`/`risk_factor_builder` suites stay green — the refactor is behavior-preserving); full suite green. **Step 5:** Commit + ledger row (`feat(s7-3): dead-alpha→risk-factor extraction (Kakushadze) + FactorModel augmentation`). Note the **eRank→atx-core L6** Pattern-B residual.

### Task S7-4: Capital allocation + book reporting
**Files:** Create `book/allocation.hpp`, `book/report.hpp`; Test `tests/book_allocation_test.cpp`, `tests/book_report_test.cpp`.
**Scope:** §4.4/§4.5 + §0.7/§0.9. `AllocationConfig`/`effective_breadth`/`size_book` (capacity-bounded fractional Kelly); `BookReport`/`accumulate_report`/`write_report` (headless reproducible files). **First verify** `risk::capacity_curve` signature + (if merged) `cost::capacity_point`, and `atx::core::linalg::ols` for the attribution regression.
- [ ] **Step 1 (allocation tests):** suite `BookAllocation` —
```cpp
TEST(BookAllocation, KellyLeverageFromSharpe) {
  EXPECT_NEAR(size_book(/*sr*/2.0, /*sigma*/0.1, /*cap*/1e9, half_kelly()), 0.5*2.0/0.1, 1e-12);  // c·SR/σ
}
TEST(BookAllocation, CapacityClipsKelly) {                                   // R7 — load-bearing
  EXPECT_EQ(size_book(/*sr*/3.0, /*sigma*/0.1, /*cap*/0.7, half_kelly()), 0.7);  // capacity wins
}
TEST(BookAllocation, EffectiveBreadthParticipationRatio) {                   // Grinold-Kahn cross-check
  EXPECT_NEAR(effective_breadth(equal_eigs(10)), 10.0, 1e-9);                 // uncorrelated => BR≈N
  EXPECT_NEAR(effective_breadth(one_dominant_eig(10)), 1.0, 0.5);            // collinear => BR≈1
}
```
- [ ] **Step 2 (report tests):** suite `BookReport` —
```cpp
TEST(BookReport, AccumulatesNetOfCostEquityCurve) {                         // §0.7/R3
  auto rep = accumulate_report(two_period_books(), panel, sched, V, lib, /*as_of*/100);
  EXPECT_EQ(rep.equity_curve.size(), sched.periods.size());
  for (size_t s=0;s<rep.pnl_net.size();++s)
    EXPECT_NEAR(rep.pnl_net[s], rep.pnl_gross[s] - rep.pnl_cost[s], 1e-12);   // net = gross − cost
}
TEST(BookReport, CapacityUtilizationBounded) {
  auto rep = accumulate_report(books_at_cap(), panel, sched, V, lib, 100);
  for (double u : rep.capacity_utilization) EXPECT_LE(u, 1.0 + 1e-9);         // gross/capacity ≤ 1
}
TEST(BookReport, WriteIsByteIdenticalAcrossRuns) {                          // R8 — load-bearing
  auto rep = accumulate_report(fixed_books(), panel, sched, V, lib, 100);
  write_report(rep, tmpdir("a")); write_report(rep, tmpdir("b"));
  EXPECT_EQ(read_all_bytes(tmpdir("a")), read_all_bytes(tmpdir("b")));        // reproducible artifacts
}
TEST(BookReport, FactorExposureIncludesDeadColumns) {                       // R6 wiring
  auto rep = accumulate_report(books_with_dead_V(), panel, sched, V_augmented, lib, 100);
  EXPECT_EQ(rep.factor_exposures.cols(), (Eigen::Index)V_augmented.n_factors());  // incl dead factors
}
```
- [ ] **Step 3:** Build → FAIL. **Step 4:** Implement `book/allocation.hpp` (§4.4: `effective_breadth`, `size_book`, `zero_crossing` fallback if `cost::capacity_point` unmerged) and `book/report.hpp` (§4.5: `accumulate_report` — order-fixed series from books × step-returns − cost, `ols` attribution, lifecycle census via `lib.state_as_of`; `write_report` — fixed-precision TSV per series). **Step 5:** `ctest -R "BookAllocation|BookReport"` → pass; full suite green. **Step 6:** Commit + ledger row (`feat(s7-4): capacity-bounded Kelly allocation + reproducible book reporting`).

### Task S7-5: Full E2E pipeline integration + bench + close
**Files:** Create `book/pipeline.hpp`, `tests/book_pipeline_test.cpp`, `bench/book_bench.cpp`; Modify ledger + `ROADMAP.md` (S7 row + module close) + spec (closed) + create `sprint-7.md` user ref.
**Scope:** §4.6 — the orchestrator, the spec's exit criteria as one end-to-end test, the bench, the close (module-level: the whole p1 roadmap's "what v2 proves").
- [ ] **Step 1 (pipeline tests):** suite `BookPipeline` — the capstone, all seven invariants:
```cpp
TEST(BookPipeline, EndToEndRunIsByteIdentical) {                            // R1/R8 — the v2 done-gate
  auto a = BookPipeline{fixed_cfg(/*seed*/7)}.run();
  auto b = BookPipeline{fixed_cfg(/*seed*/7)}.run();
  EXPECT_EQ(a.book_digest, b.book_digest);
  EXPECT_EQ(a.report_digest, b.report_digest);                               // whole pipeline replays exactly
}
TEST(BookPipeline, NoLookAheadTruncationInvariant) {                        // R2 — at every fitted boundary
  auto full = BookPipeline{fixed_cfg(7)}.run();
  auto trunc = BookPipeline{fixed_cfg(7).truncate_future(/*at*/T_mid)}.run();
  EXPECT_EQ(prefix(full.books, T_mid), trunc.books);                         // future data never leaked back
}
TEST(BookPipeline, LifecycleDrivenEndToEnd) {                               // R5 — a planted-decaying alpha retires
  auto rep = BookPipeline{cfg_with_planted_decay()}.run();
  EXPECT_GT(rep.lifecycle_census[(int)LifecycleState::Dead], 0u);            // monitor demoted it through the spine
  EXPECT_GT(rep.lifecycle_census[(int)LifecycleState::Recycled], 0u);        // and recycled it as a risk factor
}
TEST(BookPipeline, NetBelowGrossCostHonest) {                              // R3
  auto rep = BookPipeline{fixed_cfg(7)}.run();
  EXPECT_LT(sum(rep.report.pnl_net), sum(rep.report.pnl_gross));            // calibrated cost is charged
}
TEST(BookPipeline, GrossNeverExceedsCapacity) {                            // R7
  auto rep = BookPipeline{fixed_cfg(7)}.run();
  for (double u : rep.report.capacity_utilization) EXPECT_LE(u, 1.0 + 1e-9);
}
TEST(BookPipeline, NoiseLibraryProducesFlatBook) {                         // non-vacuity guard (like S3/S4b)
  auto rep = BookPipeline{pure_noise_cfg()}.run();
  EXPECT_LE(rep.research.total_admitted, kNoiseAdmitCeiling);               // nothing real => ~nothing operates
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `book/pipeline.hpp` (§4.6): `BookPipeline::run` wiring ResearchDriver → mark Live → combine (re-eval `expr_source`→`VmSignalSource`, §0.8) → `build_components`+`extract_dead_factors`+`augment_factor_model` → `size_book` → `MultiPeriodOptimizer::run` → `DecayController` over the forward window → recycle dead → `accumulate_report`+`write_report`; return `PipelineReport{research, book_digest, report_digest, lifecycle_census, report}`. **Step 4:** `ctest -R BookPipeline` → pass; full suite green.
- [ ] **Step 5:** `bench/book_bench.cpp` — **rebalances/sec** across schedule length {64, 256, 1024} × universe {500, 2000} (the multi-period solver throughput), dead-factor extraction cost vs |dead| {10², 10³, 10⁴}, and report-accumulation throughput. Wire into `atx-engine-bench` (CONFIGURE_DEPENDS). Capture the curves into the ledger.
- [ ] **Step 6: Sprint close** (per `../docs/sprint.md`) — **and module close** (S7 is the last p1 sprint): fill the ledger (per-unit rows, commits table, throughput numbers, "What S7 proves / what v2 proves / baton to p2"); lift residuals to the ROADMAP backlog — **(a) L7 QP / fixed-iteration ADMM optimizer → atx-core** (shipped on the projected/proximal loop), **(b) eRank / effective-breadth helper → atx-core L6**, **(c) the S6 borrow-accrual completion** (the net-cost picture for a half-short book, §0.1), **(d) the re-eval adapter hardening** (re-parse→compile→`VmSignalSource` at scale, §0.8), **(e) Gârleanu-Pedersen forward-blended aim** (the full multi-step aim vs the myopic+trade-rate shipped), **(f) intraday/residual modules + live broker** (p2); flip `p1/ROADMAP.md` S7 row `⏳ → ✅ <sha>`, mark the **module status `S1–S7 CLOSED — v2 complete`**, bump `Last reviewed`; mark `sprint-7-portfolio-lifecycle.md` `Status: ✅ closed`; create `sprint-7.md` user reference (the `book::`/`risk::multi_period`/`risk::dead_factor` public API + the multi-period/decay/dead-factor/allocation/reporting guarantees + the E2E done-gate). **Step 7:** Commit close (explicit pathspecs; `git show HEAD --stat`).

---

## §6 — Exit criteria · invariants · dependencies · NOT-in-scope · baton

**Exit criteria (from the spec, made concrete):**
- Multi-period optimizer is **deterministic** (fixed-iteration; byte-identical book schedule run-to-run) and respects dollar-neutral / gross / name-cap / capacity constraints; the turnover term uses the **S6-calibrated κ** (`MultiPeriodOptimizer.{SinglePeriodScheduleEqualsSingleSolve, ThreadsPrevBook, CalibratedKappaCutsTurnover, CapacityBoundsGross, TwoBuildsByteIdentical}`).
- Decay monitor **flags a planted-decaying alpha** (non-vacuous) and **does not flag a stable one** (false-alarm control), gates on **MinTRL**, distinguishes **cost-flooding**, and **demotes through the S4 lifecycle PIT** (`DecayMonitor.*`; `DecayController.DrivesLifecyclePitAndAsymmetric`).
- Dead-alpha directions are **extracted** and **measurably reduce** the mega-alpha's exposure to those directions after feeding the P4 risk model (`DeadFactor.AugmentedModelReducesDeadSubspaceExposure`); the `build_components` refactor is behavior-preserving (`DeadFactorBuild.BuildComponentsMatchesBuild`).
- Capital allocation respects the **S6 capacity ceiling** (`BookAllocation.CapacityClipsKelly`); reporting artifacts are **reproducible** (byte-identical across two runs; `BookReport.WriteIsByteIdenticalAcrossRuns`).
- **The full E2E integration test passes** — one deterministic, point-in-time, cost-honest run through the entire pipeline, every fitted boundary truncation-invariant, the lifecycle driven end-to-end, capacity-bounded, net < gross (`BookPipeline.*`). **This is the v2 done-gate.**
- `/W4 /permissive- /WX` **+ /fp:precise** clean; a `*_test.cpp` per unit; full suite stays green.

**Invariants proven (R1–R8):** deterministic fixed-iteration multi-period solver (R1); receding-horizon no-look-ahead, truncation-invariant at every fitted boundary (R2); calibrated honest cost in the turnover term + the decay discriminator + the capacity ceiling (R3); no survivorship through the book (R4); the S4 lifecycle spine driven PIT, asymmetric "trust slowly / retire fast" (R5); dead alphas → endogenous risk factors that measurably neutralize (R6); breadth-driven capacity-bounded Kelly sizing of one unified book (R7); reproducible headless artifacts + the all-seven-invariants E2E gate (R8). **S7.5 is the simultaneity proof** — the factory, library, learned combiner, calibrated cost, risk model, and multi-period optimizer compose **without** any invariant leaking.

**Dependencies:** Upstream **everything** — **S3** (the alpha firehose, via `ResearchDriver`), **S4** (closed) — `library::{Library, LifecycleState, mark, state_as_of, pnl, get}` + `LibraryStore::positions` (the lifecycle spine + dead-alpha holdings), **S5** (closed) — `combine::{AlphaCombiner, CombinedSignalSource}` + `learn::LearnedSignalSource` (the mega-alpha forecast), **S6** (`feat/atx-engine-cost`, **merge prerequisite** §0.1) — `cost::{cost_aware_knobs, capacity_point, should_trade}` (calibrated κ + capacity + the cost-flood discriminator), **P4** (closed `f2d22f4`) — `risk::{PortfolioOptimizer, FactorModel, FactorModelBuilder, build_exposures, capacity_curve}` (the single-period solver extended + the dead-alpha slot plumbed), **S1** (closed `2158a17`) — `eval::{deflated_sharpe, probabilistic_sharpe, compute_return_metrics}` (the decay reference). **atx-core** — `linalg::{MatX, VecX, ols}`, `Eigen::SelfAdjointEigenSolver`, `Result`/`Status`. **Pattern-B requests (§2.1):** L7 QP/projected-gradient → atx-core; eRank/effective-breadth → atx-core L6. **The one engine source edit is additive** — `FactorModelBuilder::build_components` (the reserved-slot refactor, §0.3); everything else is new `risk/multi_period.hpp` + `risk/dead_factor.hpp` + `book/*` + tests.

**Explicitly NOT in this sprint** (spec + ROADMAP anti-roadmap): **no live trading / broker** (anti-roadmap #1 — S7 operates a *simulated* book over historical PIT data; live is p2); **no intraday rebalancing** (daily-bar schedule; intraday is a residual/later module); **no dashboard** (files, not pixels, #5); **no cross-machine distributed book** (single-box, #4); **no new alpha discovery or DSL operators** (S3); **no new learned models** (S5 — S7 *consumes* the combiner); **no cost re-calibration** (S6 — S7 *consumes* the calibrated coefficients); **no deep-learning / neural combiner** (#7).

**Baton → next (p2 horizon):** With v2 complete — a deterministic, bias-free, calibrated, multicore **alpha factory + portfolio brain** that mines, stores, learns, combines, prices, *and operates* a capacity-bounded book that watches itself decay and recycles its dead — the natural p2 frontier is **live**: broker connectivity, real-time PIT data ingest, intraday execution, and the cross-machine scale-out v2 deliberately deferred. v2's reproducible artifacts (the content-addressed library manifest + the byte-identical book report) and its invariant proofs (the S7.5 all-seven gate) are the foundation that makes going live trustworthy. The carried residuals (the full Gârleanu-Pedersen forward-aim, the true QP solver, S6 borrow-accrual, the re-eval adapter at scale) are the first p2 hardening backlog.

---

## §7 — References (open-source web research)

**Multi-period / dynamic portfolio construction (S7.1)**
- Gârleanu, N. & Pedersen, L. H. *Dynamic Trading with Predictable Returns and Transaction Costs.* J. Finance 68(6), 2013. DOI 10.1111/jofi.12080 — the "trade partially toward a moving aim" closed form (`x_t = (1−a/λ)x_{t-1} + (a/λ)·aim_t`; aim = weighted avg of current + future Markowitz portfolios); the partial-trade-rate S7.1 ships. https://www.nber.org/papers/w15205
- Boyd, S., Busseti, E., Diamond, S., Kahn, R., Koh, K., Nystrup, P., Speth, J. *Multi-Period Trading via Convex Optimization.* Found. Trends Optim. 3(1), 2017. arXiv:1705.00109 — the receding-horizon (MPC) single-period/multi-period objective, the separable transaction-cost model, execute-only-the-first-trade. https://arxiv.org/abs/1705.00109 · https://www.cvxportfolio.com
- Constantinides, G. *Capital Market Equilibrium with Transaction Costs.* JPE 94(4), 1986; Davis, M. & Norman, A. *Portfolio Selection with Transaction Costs.* Math. Oper. Res. 15(4), 1990 — the **no-trade region** under proportional (L1) costs; the κ‖w−w_prev‖₁ prox *is* the no-trade band ("trade to the edge"). https://www.jstor.org/stable/1833205
- Beck, A. & Teboulle, M. *FISTA.* SIAM J. Imaging Sci. 2(1), 2009; Parikh, N. & Boyd, S. *Proximal Algorithms.* 2014; Duchi, J. et al. *Efficient Projections onto the ℓ1-Ball.* ICML 2008; Boyle, J. & Dykstra, R. *Projections onto the Intersection of Convex Sets.* 1986; Stellato, B. et al. *OSQP.* Math. Program. Comput. 12, 2020 — the deterministic **fixed-iteration** projected/proximal + projection machinery the as-built solver embodies (R1). https://web.stanford.edu/~boyd/papers/prox_algs.html · http://ai.stanford.edu/~jduchi/projects/jd_ss_ys_paper.pdf · https://web.stanford.edu/~boyd/papers/osqp.html

**Alpha-decay monitoring (S7.2)**
- Page, E. S. *Continuous Inspection Schemes.* Biometrika 41, 1954; Moustakides, G. *Optimal Stopping Times for Detecting Changes in Distributions.* Ann. Statist. 14, 1986 — **CUSUM / Page-Hinkley**, minimax-optimal mean-shift detection; the down-mode streaming detector S7.2 ships. Wald, A. *Sequential Analysis.* 1947 (SPRT); Adams, R. & MacKay, D. *Bayesian Online Changepoint Detection.* arXiv:0710.3742, 2007 — the alternatives surveyed.
- Bailey, D. & López de Prado, M. *The Deflated Sharpe Ratio.* J. Portfolio Management 40(5), 2014 — PSR / DSR / **Minimum Track Record Length**; the realized-DSR-drop decay gate + the MinTRL significance floor. https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2460551
- Tulchinsky, I. (ed.) *Finding Alphas: A Quantitative Approach to Building Trading Strategies.* Wiley (2nd ed. 2019) — the alpha lifecycle + cut-loss / decay monitoring discipline (the "trust slowly, retire fast" asymmetry).

**Dead alphas as risk factors (S7.3)**
- Kakushadze, Z. & Yu, W. *Dead Alphas as Risk Factors.* arXiv:1709.06641; J. Asset Management 19(2), 2018 — the holdings-overlap matrix `X_AB = Σ_{i∈dead} P_iA P_iB`, eigendecomposition, **eRank** truncation (`K = round(exp(entropy))`), and feeding the eigenvectors as endogenous risk-factor loadings to neutralize against (R6). https://arxiv.org/abs/1709.06641
- Kakushadze, Z. & Yu, W. *How to Combine a Billion Alphas.* arXiv:1603.05937 — using position data to enlarge the risk-factor set beyond the historical-observation bound; the bridge to holdings-as-factors. Kakushadze, Z. *101 Formulaic Alphas.* arXiv:1601.00991 — mean pairwise corr ~15.9%, the raw material that produces recyclable dead alphas. Roy, O. & Vetterli, M. *The Effective Rank.* EUSIPCO 2007 — the eRank definition.

**Breadth / Kelly allocation + book reporting (S7.4)**
- Grinold, R. & Kahn, R. *Active Portfolio Management.* 2nd ed., McGraw-Hill 1999 — the **Fundamental Law** `IR ≈ IC·√BR`; Clarke, de Silva & Thorley (2002) — the constrained generalization `IR = TC·IC·√BR`. The breadth cross-check S7.4 logs.
- Thorp, E. *The Kelly Criterion in Blackjack, Sports Betting, and the Stock Market.* 2006 — `f* = (μ−r)/σ² = SR/σ`; fractional Kelly = the robust mean-variance leverage S7.4 sizes from (R7). https://gwern.net/doc/statistics/decision/2006-thorp.pdf
- Brinson, G., Hood, R. & Beebower, G. *Determinants of Portfolio Performance.* FAJ 1986; Brinson-Fachler 1985 — allocation/selection/interaction attribution; the factor-attribution analog (regress book P&L on the risk-model factors, incl dead) S7.4's report emits.
- atx-engine internal: `research/worldquant-systems-deep-dive.md` §6/§7/§10 (mega-alpha as one book, neutralization + turnover control, alpha lifecycle + dead-alpha recycling); `research/renaissance-technologies-systems-deep-dive.md` §6 (Kelly/breadth sizing, the single unified dollar-neutral book).

**Reproducibility / determinism (carried from S1/S4)**
- López de Prado, M. *Advances in Financial Machine Learning.* Wiley 2018 — Ch. 7 purged+embargoed CPCV (the firewall every fitted boundary honors); the reproducible-research discipline the byte-identical artifacts enforce.

---

## §8 — Self-review (against the spec)

- **Spec coverage:** S7.0 (marker + ledger)→S7-0 (+ the §0.1 S6-merge prerequisite check); S7.1 (multi-period/dynamic optimizer extending P4, turnover-aware schedule, look-ahead-safe forecasts, S6-calibrated cost, deterministic fixed-iteration, capacity-bounded gross)→S7-1; S7.2 (alpha-decay monitor, live-vs-backtest drift, S1 metrics+DSR, demote through S4 lifecycle, PIT)→S7-2; S7.3 (dead-alpha→risk-factor extraction, Kakushadze, feeds P4 `FactorModel`, measurably reduces exposure)→S7-3; S7.4 (capital allocation breadth-aware/capacity-bounded + book-level reporting artifacts, headless reproducible)→S7-4; S7.5 (full E2E data→mine→store→eval→combine→optimize→cost→report, deterministic, all invariants, v2 exit gate + close)→S7-5. Every spec unit + every exit criterion + every "invariant this sprint must prove" maps to a task and a named test. ✅
- **User asks satisfied:** "build out sprint-7 to be a full sprint implementation plan, same building style as S4/S4b" — §0 as-built reconciliation, §1 research rules+citations, §2 file structure + Pattern-B decisions, §3 gates + handoff block, §4 algorithms/pseudocode + data structures, §5 per-unit TDD with GoogleTest, §6 exit/invariants/deps/baton, §7 OSS references, §8 self-review — mirrors the frozen S4 template exactly. ✅ "Open-source web research where needed" — Gârleanu-Pedersen / Boyd MPC / Constantinides-Davis-Norman no-trade region / FISTA-Duchi-Dykstra-OSQP (S7.1); Page-Hinkley/CUSUM/SPRT + DSR/PSR/MinTRL + Tulchinsky lifecycle (S7.2); Kakushadze dead-alphas-as-risk-factors + eRank (S7.3); Grinold-Kahn Fundamental Law + Thorp Kelly + Brinson attribution (S7.4). ✅ "Ground in the codebase" — every unit cites the as-built signatures verified in recon (`PortfolioOptimizer::solve`, `FactorModel`/`FactorModelBuilder`, `build_exposures`/`n_dead_factors`, `capacity_curve`, `Library::mark`/`state_as_of`/`pnl`/`positions`, `LifecycleState` edges, `eval::deflated_sharpe`, `AlphaCombiner`/`CombinedSignalSource`, `cost::cost_aware_knobs`/`capacity_point`/`should_trade`, `ResearchDriver`). ✅ "Pseudo code, data structures, implementation notes" — §4.1–§4.6 give `RebalanceSchedule`/`MultiPeriodConfig`/`MultiPeriodResult`/`MultiPeriodOptimizer`, `AdmittedBaseline`/`PageHinkleyState`/`DecayConfig`/`DecayVerdict`/`DecayMonitor`, `FactorComponents`/`DeadAlphaFactors`/`extract_dead_factors`/`effective_rank`/`augment_factor_model`, `AllocationConfig`/`size_book`/`effective_breadth`, `BookReport`/`accumulate_report`/`write_report`, `BookPipeline` — with runnable pseudocode. ✅ "End-to-end high quality over partial stubs" — no unit ships a stub: the optimizer reuses the real solver, the monitor drives the real journal, the dead-factor path builds a real augmented `FactorModel`, the report writes real files, and S7.5 wires the whole pipeline with the all-seven-invariants gate. ✅
- **As-built fixes applied (the recon's value):** S6 cost on an unmerged branch → merge prerequisite + `CostInputs` scalar adapter (§0.1); single-period solver already exists → drive-not-rebuild (§0.2); dead-factor slot reserved-but-unplumbed → `build_components` refactor + `augment_factor_model` (§0.3); no schedule/time-axis → `RebalanceSchedule` over as-of period indices (§0.4); lifecycle built-but-undriven-past-Admitted → S7 is its first driver (§0.5); AlphaMetrics has no DSR → baseline recomputed from stored admitted PnL (§0.6); Portfolio has no series → report accumulates its own (§0.7); mega-alpha via non-owning CombinedSource → pipeline keeps constituents alive (§0.8); capacity-curve present but root-find on the cost branch → local zero-crossing fallback (§0.9). ✅
- **Determinism/reproducibility rigor:** the load-bearing acceptances are the single-period-equals-multi-period bit-pin (R1, the P4 regression), the two-builds-byte-identical schedule (R1), the byte-identical report (R8), the eigensolver sign-convention reproducibility (R1), and the E2E digest equality + truncation-invariance (R1/R2/R8) — all named tests. ✅
- **Non-vacuity rigor:** the decay monitor is proven **both** ways (demotes a planted-decaying alpha AND does NOT demote a stable one — false-alarm control, not just "it flagged something"); the dead-factor extraction is proven to **measurably reduce** dead-subspace exposure (not just "it returned columns"); the pipeline includes the S3/S4b noise-library guard (pure noise ⇒ ~nothing operates). The honest framing (MinTRL gate, cost-flooding discriminator, capacity clip) is in the tests, not hidden. ✅
- **Type consistency:** `AlphaId` (combine/library) threaded through; `RebalanceSchedule{periods}`, `MultiPeriodConfig{single, trade_rate, capacity_bound_gross}`, `MultiPeriodResult{books, turnover, cost_bps}`, `CostInputs{kappa, round_trip_cost_bps, capacity_gross}`, `AdmittedBaseline{sr_admit, skew, exkurt, t_admit, dsr_admit, n_trials, mean_admit, sd_admit}`, `PageHinkleyState{mean, cum, max, n}`, `DecayConfig{ph_delta, ph_lambda, ph_min_obs, psr_alpha, confirm_periods, recover_periods, cost_flood_safety}`, `DecayVerdict{flag, recommend, realized_psr, realized_dsr, min_trl, cost_flooded}`, `FactorComponents{X, F, D, fit_end}`, `DeadAlphaFactors{loadings, variances, k_dead}`, `AllocationConfig{fractional_kelly, max_gross}`, `BookReport{equity_curve, gross_leverage, net_exposure, turnover, pnl_gross/net/cost, factor_exposures, capacity_utilization, lifecycle_census}` — consistent across §2/§4/§5. Reused symbols match the recon-verified as-built signatures (`PortfolioOptimizer::solve(span, FactorModel&, span)`, `OptimizerConfig{risk_aversion, turnover_penalty, gross_leverage, name_cap, dollar_neutral, max_iters}`, `FactorModel::create(MatX, MatX, VecX, usize, usize)` + `apply_inverse`/`risk`/`neutralize`/`n_factors`/`n_instruments`, `FactorModelBuilder::build(panel, window, market_cap, group_id)`, `build_exposures`/`ExposureMatrix`/`FactorModelConfig.n_dead_factors`, `capacity_curve(weights, panel, sim, aum_grid)` + `CapacityPoint{aum, net_edge_bps}`, `library::Library::{open, admit, mark(id,to,as_of), state_as_of(id,t), pnl(id), get(id), n_alphas, snapshot}` + `LibraryStore::positions(id, period)`, `LifecycleState{Candidate..Recycled}` + the legal edges, `eval::deflated_sharpe(sr,T,skew,exkurt,N,opt<var>)` → `DsrResult{psr,sr_star,dsr,haircut_sharpe}` + `probabilistic_sharpe`, `combine::{AlphaCombiner::fit→Combination, CombinedSignalSource(sources,combo,method), compute_metrics, AlphaMetrics(7×f64)}`, `cost::{cost_aware_knobs→CostKnobs{kappa,gate,fitness_cost_floor}, capacity_point, should_trade}`, `factory::ResearchDriver(lib,dsl,panel,sim,policy,gate).run(cfg)`). ✅
