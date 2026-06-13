# Sprint S6 — Cost Calibration & Capacity — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan unit-by-unit. Steps use checkbox (`- [ ]`) syntax. This is the FROZEN *how*; the sprint spec [`sprint-6-cost-calibration-capacity.md`](sprint-6-cost-calibration-capacity.md) is the *what*. On conflict, **§0 (this plan's as-built amendment) overrides** the spec.

**Goal:** Make the cost model **true, not guessed** — fit the `ExecutionSimulator`'s √-impact coefficients (`δ` exponent, `Y` scale, `γ` permanent, slippage `η`) to realized fills by robust log-linear regression with **reported, auditable fit quality**; prove the temporary-vs-permanent split leaks **zero** transient cost into the forward mark; compute the **capacity curve** (net-edge-vs-AUM and the capacity point where impact erodes net edge to zero) per-alpha and per-mega-alpha through the *calibrated* sim; feed the calibrated cost back into the factory's fitness, the gate's turnover ceiling, and the optimizer's turnover penalty `κ` so discovery and combination **price turnover honestly**; and add **short-borrow / financing accrual** to complete the net-cost picture for a dollar-neutral book — every coefficient a fit/apply-firewalled, byte-stable, differentially-verified object.

**Architecture:** A new header-only engine layer `atx::engine::cost` (`include/atx/engine/cost/`) that is **purely additive** — it edits no `exec/`, `loop/`, `risk/`, `combine/`, `factory/`, or `portfolio/` source. It **calibrates** the existing `exec::ImpactCfg{Y,delta,gamma}` / `SlippageCfg` / `CommissionCfg` (calibration emits *new* config values; the sim is reconstructed from them, never edited), **reuses** the already-built `risk::capacity_curve(weights, panel, sim, aum_grid)` (extending it with a per-alpha/per-mega wrapper + a capacity-point root-find) and the already-built `risk::OptimizerConfig::turnover_penalty κ` (deriving κ from calibrated cost), and adds short-financing as a synthetic fee-only fill so it needs **no Portfolio edit**. The one numeric kernel atx-core lacks — **robust (IRLS-Huber) / nonlinear least squares** for the fat-tailed slippage fit — ships engine-local-then-lifted (Pattern B) on top of atx-core's `regression::{ols,wls}`, behind a differential test that recovers injected synthetic coefficients. There is **one cost model**: calibration emits the same `ImpactCfg` the sim and the capacity curve already read — no second impact formula anywhere.

**Tech Stack:** C++20, header-only inline (`#pragma once`), namespace `atx::engine::cost`; reuses atx-core `core::{regression::{ols,wls,ridge,OlsResult},linalg::{as_matrix,as_vector,MatX,VecX},solve::solve,math::{isclose,sign},simd::{dot}}` and engine `exec::{ExecutionSimulator,ImpactCfg,SlippageCfg,CommissionCfg,FillPayload,OrderPayload}`, `loop::{Market,InstrumentStats}`, `portfolio::{Portfolio,Holding}`, `risk::{capacity_curve,CapacityPoint,OptimizerConfig}`, `combine::{AlphaMetrics,compute_metrics,GateConfig,AlphaGate}`, `factory::{FitnessCfg,pool_aware_fitness}`, `alpha::{AlphaStreams,turnover_cost_rate}`, `core::Decimal`. GoogleTest (`atx-engine/tests/*_test.cpp`, CONFIGURE_DEPENDS — no per-unit CMake edit). clang-cl `/W4 /permissive- /WX` **+ strict FP** (`/fp:precise`, `-ffp-contract=off`). Build + ctest are the gates; clang-tidy disabled (noise).

---

## §0 — As-built reconciliation amendment

The spec was frozen before the cost/exec/risk seams were verified as-built. The dominant correction: **most of S6 is calibrate / extend / prove around code that already exists**, not greenfield construction. Eight corrections; **§0 overrides the spec on conflict.**

### 0.1 The coefficients already live in `exec::ImpactCfg` — calibration emits new config, edits nothing
The spec says "fit the `ExecutionSimulator` coefficients." As-built, those coefficients are `exec::ImpactCfg{f64 Y=1.0; f64 delta=0.5; f64 gamma=0.314;}` + `exec::SlippageCfg{...,f64 bps=5.0,...}` + `exec::CommissionCfg{...,f64 per_dollar_bps=15.0}`, passed **by value** into the `ExecutionSimulator` ctor and exposed read-only via `sim.impact_cfg()` / `sim.commission_cfg()`.

**Resolution.** S6-1 calibration is a free function `cost::calibrate_impact(fills, market, window) → CalibratedCost{ImpactCfg, SlippageCfg, FitReport}` that **returns new config structs**; the caller constructs a fresh `ExecutionSimulator` from them. **No edit to `execution_sim.hpp`.** **Consequence:** the calibrated sim is a drop-in built from fitted configs; the whole capacity + gating chain (which reads `sim.impact_cfg()`) automatically uses the calibrated values.

### 0.2 The engine's defaults are a literature mix and MUST be calibrated per-universe
As-built defaults: `γ=0.314` (this **is** the Almgren-Thum-Hauptmann-Li 2005 universal permanent coefficient, a **large-cap-US** fit), `δ=0.5` (the Bouchaud/Tóth square-root value), but Almgren-2005's *temporary* exponent is **0.6 (3/5)**. So the shipped numbers are a mix of two literatures, fit to a universe that may not be ours.

**Resolution.** S6 treats `δ` as a configurable parameter with research default in **`[0.5, 0.6]`** (cite both; §7), and the calibration recovers it from *our* fills. The synthetic-recovery test (C4) injects known `δ,Y,γ`, regenerates fills, and fits them back within tolerance — proving the harness works before it touches real data. **Consequence:** the "uncalibrated 2001-era defaults" the spec flags are replaced by a per-universe fit with reported error bars, not a different hard-coded guess.

### 0.3 `δ` is a nonlinear exponent → log-linearize + robust (IRLS), atx-core has the linear core
The square-root impact law `I = Y·σ·(Q/V)^δ` is **nonlinear** in `δ`. atx-core ships `regression::{ols, wls, ridge}` (linear) but **no nonlinear or robust LS** (verified absent).

**Resolution.** Take logs: `log(I/σ) = log(Y) + δ·log(Q/V)` is **linear in `(log Y, δ)`** → fit by atx-core `ols`. Slippage residuals are fat-tailed/heteroscedastic, so wrap it in engine-local **IRLS with a Huber weight** (re-weighted `wls` until convergence) — the Pattern-B edge (§2.1). The **permanent** channel keeps `α=1` (linear, the no-manipulation constraint) and fits `γ` by a separate linear regression of realized mark-shift on `σ·part`. **Consequence:** calibration is exact linear algebra on atx-core primitives + a small deterministic reweighting loop; the recovered `(Y, δ, γ, η)` are byte-stable.

### 0.4 The capacity curve **already exists** — S6.3 extends, does not rebuild
`risk::capacity_curve(std::span<const f64> weights, const PanelView&, const exec::ExecutionSimulator&, std::span<const f64> aum_grid) → std::vector<CapacityPoint{f64 aum; f64 net_edge_bps;}>` is **already implemented** (P4-10), reads `sim.impact_cfg()` (the §0.1 calibrated coefficients), uses `kCapacityAdvWindow=20`/`kCapacityVolWindow=60`, and computes `net_edge_bps(aum) = gross_edge_bps − Σ_i |w_i|·Y·σ_i·(shares_i/ADV_i)^δ`.

**Resolution.** S6-3 adds thin `cost::` wrappers around it: (a) `capacity_for_alpha(AlphaStreams, idx, sim, grid)` (per-alpha), (b) `capacity_for_book(combined_weights, panel, calibrated_sim, grid)` (per-mega-alpha — the P4 combined book at increasing notional), (c) `capacity_point(curve)` — the **root-find** for the AUM where `net_edge_bps` crosses 0 (monotone-decreasing, so a bisection on the sampled curve), and (d) a **monotonicity assertion**. **Consequence:** S6 does not duplicate the impact math (C6); it reuses the one curve and adds the missing capacity *point* + the per-alpha/per-mega entry points.

### 0.5 The turnover penalty `κ` already exists — S6.4 calibrates it, does not build it
`risk::OptimizerConfig{f64 risk_aversion=1.0; f64 turnover_penalty=0.0; f64 gross_leverage=1.0; f64 name_cap=1.0; bool dollar_neutral=true; usize max_iters=64;}` already implements the objective `maximize_w αᵀw − λ·wᵀVw − κ·‖w − w_prev‖₁` with `prox_turnover` (soft-threshold at `τ=κ·kStep`). `κ` defaults to 0.0 (off).

**Resolution.** S6-4 **derives `κ` from the calibrated round-trip cost** (set `κ` so the optimizer declines a rebalance whose expected edge does not clear modeled round-trip cost — the RenTech throttle that lengthens the average hold), and feeds calibrated values into `factory::FitnessCfg` (a cost/turnover budget) and `combine::GateConfig::max_turnover`. The deliverable is `cost::cost_aware_knobs(CalibratedCost, …) → {f64 kappa, GateConfig gate, f64 fitness_cost_floor}`. **Consequence:** S6-4 is a calibration-to-knobs mapping + the down-ranking proof, not a new optimizer (C6).

### 0.6 The temp/perm split is **already structured** — S6.2 is a proof + ratio-fit, not a build
As-built, temporary impact goes into the **fill price** (`FillPayload{core::Decimal price; core::Decimal fee; f64 impact;}`) and permanent impact shifts the **mark** via `Market::shift_mark(id, f64 delta)` — they are already distinct, and same-bar fill is OFF by default (`FillCfg{allow_same_bar_fill=false}`).

**Resolution.** S6-2 is a **property-proof + ratio-fit** unit: a test that round-trips a trade through `ExecutionSimulator` + `Market` and asserts the temporary cost appears **only** in the fill price and **never** in any later `mark` (zero temp leakage, the spec's hard exit criterion), plus `cost::fit_split_ratio(fills, marks)` (the perm/temp magnitude ratio) and the throttle decision `cost::should_trade(edge_bps, predicted_cost_bps)`. **Consequence:** S6-2 verifies and tunes the existing structure rather than re-deriving it.

### 0.7 Borrow accrual is the one genuinely-new behavior — kept additive via a synthetic fee-only fill
`Portfolio` has **no short-financing** (documented residual, `portfolio.hpp:168-170`). Its only cash mutator is `apply_fill(const exec::FillPayload&)`, which deducts `(notional + fee)` from cash.

**Resolution.** S6-5 models borrow as `cost::BorrowModel{f64 annual_rate; DayCount day_count;}` producing a **daily accrual** `|short_notional|·(annual_rate/360)`; it is applied by constructing a **synthetic fee-only `FillPayload{qty=0, price=0, fee=daily_borrow}`** and calling the existing `Portfolio::apply_fill` (a zero-qty fill changes no position; cash is debited by `fee`). **First verify** `apply_fill` handles `qty==0` as a pure fee debit (no avg-price/realized mutation); **if it does not, fall back** to a single minimal additive `Portfolio::accrue_financing(core::Decimal)` method — flagged in the ledger as the one reviewed engine touch. Day-count (`/360` default, `/365`, `/252`) is a config flag. **Consequence:** S6 stays purely additive (preferred path) and the net-cost picture for a half-short book is complete.

### 0.8 Determinism + firewall + the streams-undercharge residual
Calibrated coefficients are a **fitted object**: fit on a trailing window, applied forward; the cost model never peeks (truncation-invariant). The IRLS loop is deterministic (fixed max-iter, order-fixed reductions, convergence on a fixed tolerance). Capacity replays identically. **Recorded, not silently changed:** `alpha::extract_streams` PnL nets only the **PerDollar commission** (`turnover_cost_rate = per_dollar_bps/1e4`), not impact/slippage — so stored alpha PnL under-charges. S6 does **not** rewrite `streams.hpp` (that would change S3/S5 behavior mid-flight); instead the **cost-aware fitness hook (S6-4) is an additive term**, and the full-impact re-pricing of streams is recorded as a residual → ROADMAP. **Consequence:** no behavior change to already-shipped sprints; the cost-honesty improvement is opt-in through the new knobs.

> **Net scope shift vs spec:** the spec's six units (S6.0–S6.5) stay as **S6-0…S6-5**, but the work is reframed against the as-built engine: S6-1 *calibrates into existing `ImpactCfg`* (additive), S6-2 *proves zero temp leakage* in the existing split, S6-3 *extends the existing `risk::capacity_curve`* with per-alpha/per-mega/capacity-point, S6-4 *derives the existing `κ`* + gate/fitness knobs from calibration, S6-5 adds borrow accrual *additively via a synthetic fee-fill*. One Pattern-B edge (robust/nonlinear LS). One reviewed engine touch only if `apply_fill(qty=0)` misbehaves. No `exec`/`risk`/`combine`/`factory` source edits.

---

## §1 — Research foundation: the cost-fidelity design rules

Derived from the research north-stars ([`renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md) §4 + §9.2/§9.2.1 cost = the strategy; [`worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md) §8 linear cost + turnover-aware fitness) and the open-source impact literature (§7: Almgren-Chriss 2000; Almgren-Thum-Hauptmann-Li 2005; Bouchaud/Tóth square-root law; Kissell; Perold implementation shortfall). **Non-negotiable**; every S6 unit is checked against them.

| # | Rule | Why / source |
|---|---|---|
| **C1** | **Calibrated, not guessed.** Coefficients `(Y, δ, γ, η)` are **fit to realized fills** with **reported, auditable fit quality** (R², residual distribution) — never asserted. The "secret weapon" is the *number*, not the functional form. | RenTech §9.2 ("calibrate, don't guess"); the entire sprint thesis. At a 50.75% hit rate the net per-trade edge is `(2·0.5075−1)·g = 0.015·g ≈ 0.3 bp`, so a few-bp cost error flips the sign. |
| **C2** | **Fit/apply firewall.** Calibrated coefficients are a fitted object — estimated on a **trailing window, applied forward**; the cost model **never peeks** at fills it then prices. Truncation-invariance is the test. | Carried-forward invariant #2; no look-ahead in the cost model is a spec exit criterion. |
| **C3** | **Temp/perm separation, permanent linear.** Temporary cost reverts (it lives **only** in the fill price) and **never leaks into the forward mark**; permanent cost persists (mark shift) and is **linear** in trade rate (`α=1`, the no-manipulation constraint — Almgren-2005). | Almgren-Chriss temp/perm decomposition (§7); spec hard exit ("zero temporary-cost leakage"). |
| **C4** | **Differential correctness.** Inject synthetic `(δ,Y,γ)` → regenerate fills → the harness **recovers them within tolerance**. The capacity-point root is checked against a closed form; borrow accrual against hand arithmetic. | Carried-forward invariant #7; spec exit ("recover injected synthetic coefficients"). |
| **C5** | **Determinism.** Fitted coefficients are **byte-stable** given the same data (IRLS = fixed max-iter, fixed tolerance, order-fixed reductions, no RNG); the capacity curve **replays identically**. | Carried-forward invariant #1; spec ("fitted coefficients byte-stable; capacity replays identically"). |
| **C6** | **One cost model, reused — never a second formula.** Calibration emits the **same `exec::ImpactCfg`** the sim and `risk::capacity_curve` already read; capacity reuses `risk::capacity_curve`; the turnover penalty reuses `OptimizerConfig.κ`; turnover uses the **one** `Σ\|Δw\|` definition. No parallel impact math anywhere. | RenTech §9.5 ("one cost model, one risk model, one data path"); C6 prevents the classic double-count. |
| **C7** | **Pattern B — no general-purpose primitive in the engine.** Robust/nonlinear least squares (IRLS-Huber + the log-linear power-law fit) is an **atx-core L7 request**, shipped engine-local-then-lifted (precedent chain S1 `stats_ext`→L6, S2 `DetPool`→L4, S3 sep-CMA→L7, S4 SimHash→atx-core, S5 elastic-net/GBT/HMM→L7). The linear core (`ols`/`wls`/`solve`) is **consumed from atx-core**. | Module rule; the precedent chain. |
| **C8** | **Cost is a decision input, not a post-hoc deduction.** Calibrated cost + capacity feed the factory fitness, the gate's `max_turnover`, and the optimizer `κ` so discovery (S3) and combination (P4/S5) **price turnover honestly**; capacity is a **first-class reported metric**. PIT / no-survivorship / NaN-verbatim carried. | RenTech §9.2 ("make cost the gate, not the afterthought") + §9.6 ("report capacity, not just return"); WQ §8. |

**One-sentence thesis:** *the entire P&L of a weak-signal book lives in the last fraction of a basis point of cost accuracy — so S6 fits the cost coefficients to reality, proves the transient cost never contaminates the forward mark, computes the size at which the edge dies, and wires all of it back into discovery and combination, with one cost model and zero look-ahead.*

---

## §2 — File structure

### 2.1 atx-core Pattern-B request (decided at kickoff; engine-local fallback ships S6)

> The engine adds no general-purpose primitive (project rule). S6 records **one** cross-module edge and ships on existing primitives + an engine-local kernel, exactly as S1 (`stats_ext`→L6), S2 (`DetPool`→L4), S3 (sep-CMA-ES→L7), S4 (SimHash/SRP→atx-core), S5 (elastic-net/GBT/HMM→L7) did:
>
> 1. **Robust / nonlinear least squares (IRLS-Huber + power-law log-linear fit) → atx-core L7.** atx-core ships `regression::{ols,wls,ridge}` (linear, L2-only) but **no robust or nonlinear LS** (verified absent). Ship engine-local in `cost/robust_ls.hpp` — iteratively-reweighted `wls` with a Huber weight on standardized residuals, deterministic fixed-iteration (§4.1); differential-tested by recovering injected synthetic coefficients and matching plain `ols` when the Huber threshold is `+∞` (the no-outlier limit, C4). The IRLS-Huber kernel is the Pattern-B L7 lift.
>
> **Consumed directly from atx-core (no request, no re-implementation):** `regression::{ols,wls}` (the linear core of every fit), `linalg::{as_matrix,as_vector,MatX,VecX}` + `solve::solve` (design-matrix math), `math::{isclose,sign}` (convergence + soft-threshold), `simd::dot` (inner products). **Reused from the engine (no re-request, no fork — C6):** `risk::capacity_curve` (the impact sweep), `risk::OptimizerConfig::turnover_penalty` (the κ term), `exec::{ImpactCfg,SlippageCfg,CommissionCfg}` (the calibration target), `combine::compute_metrics` (the one turnover/fitness definition), `alpha::turnover_cost_rate`.

### 2.2 Engine `cost/` layer (this sprint builds these)

| File | Responsibility | Unit |
|---|---|---|
| `include/atx/engine/cost/fwd.hpp` | forward decls + the layer doc block (the C1–C8 rules, the seam map, the one-cost-model contract, the impact formulae) | S6-0 |
| `include/atx/engine/cost/robust_ls.hpp` | engine-local IRLS-Huber robust least squares over atx-core `wls` (Pattern-B edge; §4.1) | S6-1 |
| `include/atx/engine/cost/calibration.hpp` | `calibrate_impact(fills, market, window) → CalibratedCost{ImpactCfg, SlippageCfg, FitReport}`; log-linear power-law fit + `γ` perm fit; fit-on-trailing firewall (§4.2) | S6-1 |
| `include/atx/engine/cost/temp_perm.hpp` | temp/perm leakage proof helpers + `fit_split_ratio` + `should_trade` throttle (§4.3) | S6-2 |
| `include/atx/engine/cost/capacity.hpp` | `capacity_for_alpha` / `capacity_for_book` / `capacity_point` (root-find) wrappers around `risk::capacity_curve` (§4.4) | S6-3 |
| `include/atx/engine/cost/cost_aware.hpp` | `cost_aware_knobs(CalibratedCost,…) → {kappa, GateConfig, fitness_cost_floor}`; calibrated cost → factory fitness + gate + optimizer κ (§4.5) | S6-4 |
| `include/atx/engine/cost/borrow.hpp` | `BorrowModel` + `daily_accrual` + the synthetic fee-only-fill applicator (§4.6) | S6-5 |

### 2.3 Tests (one per unit, `atx-engine/tests/<name>_test.cpp`, CONFIGURE_DEPENDS)

`robust_ls_test.cpp` + `calibration_test.cpp` (S6-1), `temp_perm_test.cpp` (S6-2), `capacity_test.cpp` (S6-3), `cost_aware_test.cpp` (S6-4), `borrow_test.cpp` (S6-5), `cost_integration_test.cpp` (S6-5, the synthetic-recovery + firewall + zero-leakage + capacity-monotone + cost-down-ranks + borrow proofs end-to-end). Bench: `bench/cost_bench.cpp` (S6-5).

### 2.4 Ledger

`atx-engine/plans/p1/sprint-6-progress.md`, opened at S6-0; per-unit rows, commits table, residuals → ROADMAP backlog (the robust-LS Pattern-B lift; the streams full-impact re-pricing §0.8), baton → S7. Format = copy `sprint-4-progress.md`.

---

## §3 — Cross-cutting gates (every coding unit) + handoff block

Every unit, before its commit, must clear:

- **Build green:** `cmake --build build --config Debug --target atx-engine-tests` under `/W4 /permissive- /WX` + `/fp:precise` `-ffp-contract=off` — zero warnings.
- **Its own suite + full suite green:** `ctest --test-dir build -R <Suite> --output-on-failure`, then the whole engine suite (no regression).
- **C4 differential:** any unit that fits ships a synthetic-recovery test (inject known coefficients → recover within a stated tolerance), not a "returned something" test.
- **C2 firewall:** the calibration unit ships a `*_TruncationInvariant_*` test (calibrate on `[0,t)` vs `[0,t+k)` → identical coefficients from the trailing window).
- **C3 leakage:** S6-2 ships the zero-temp-leakage property test (a round-trip's temporary cost never appears in a later mark).
- **C5 determinism:** any fit ships a same-data-byte-identical test (coefficients hashed via `core::hash::hash_bytes`); the capacity curve ships a replay-identical test.
- **C6 one-model:** `git diff --stat` shows only `cost/*.hpp`, `tests/*_test.cpp`, `bench/*_bench.cpp`, and the ledger — **no second impact formula**; calibration emits `exec::ImpactCfg`, capacity calls `risk::capacity_curve`. (The single allowed exception is the S6-5 `Portfolio::accrue_financing` fallback **iff** `apply_fill(qty=0)` misbehaves — flagged in the ledger.)
- **Explicit-pathspec commit** + ledger row + `git show HEAD --stat`.

```text
HANDOFF — read before implementing any S6 unit
Implementation quality standard (atx): governed by .agents/cpp/agent.md + .agents/atx-engine/agent.md +
  atx-engine/plans/docs/implementation-quality.md. Positive style/API references in-tree:
  exec/execution_sim.hpp (ImpactCfg/SlippageCfg/CommissionCfg + the temp=Y*sigma*part^delta /
  perm=0.5*gamma*sigma*part formulas + sim.impact_cfg()/commission_cfg()), exec/payloads.hpp (FillPayload),
  loop/market.hpp (InstrumentStats{adv,sigma,spread} + shift_mark), portfolio/portfolio.hpp (apply_fill +
  the borrow residual note ~line 168), risk/capacity.hpp (capacity_curve + CapacityPoint), risk/optimizer.hpp
  (OptimizerConfig.turnover_penalty kappa + prox_turnover), combine/metrics.hpp (compute_metrics + the one
  turnover def + kTurnoverFloor), combine/gate.hpp (GateConfig.max_turnover), factory/fitness.hpp (FitnessCfg
  + pool_aware_fitness), alpha/streams.hpp (turnover_cost_rate + the PnL cost formula).
THIS SPRINT'S DOMINANT RISK IS A COST MODEL THAT LOOKS CALIBRATED BUT ISN'T — a fit that peeks at the fills
  it then prices (look-ahead), a second impact formula that double-counts or diverges from the sim, or a
  temporary cost that silently leaks into the forward mark. Each is a silent sign-flip the suite must catch
  by construction (C2, C3, C6).
The gates: calibrated with reported fit quality, not guessed (C1); fit-on-trailing/apply-forward,
  truncation-invariant (C2); temp reverts / perm linear / zero mark leakage (C3); recover injected synthetic
  coefficients (C4); byte-stable fit + identical capacity replay (C5); ONE cost model — emit exec::ImpactCfg,
  call risk::capacity_curve, reuse OptimizerConfig.kappa, one turnover def (C6); robust-LS is an engine-local
  Pattern-B edge (C7); cost feeds fitness/gate/kappa, capacity is first-class (C8).
No UB, no hidden look-ahead, no second cost/turnover definition. Use ATX_CHECK (not ATX_ASSERT) wherever a
  deref/write/OOB sits OUTSIDE the condition — it must hold under NDEBUG (the S4-5 21d7ae1 lesson). Header-only
  inline (#pragma once), namespace atx::engine::cost. Functions <= ~60 lines, one purpose. Money math stays
  in core::Decimal at the ledger boundary; f64 only for market stats / fit math. enum class + atx vocabulary
  types {u8,u32,u64,usize,f64,i64} + core::Decimal. // SAFETY: on every span aliasing. Result<T>/Status for
  expected failure (.has_value(), NOT .ok()). Exhaustive switch on enum class (no default).
Build gate: cmake --build build --config Debug --target atx-engine-tests (/W4 /permissive- /WX, /fp:precise,
  -ffp-contract=off) + ctest --test-dir build -R <Suite> --output-on-failure. Tests/benches auto-globbed —
  do NOT hand-edit CMakeLists.txt. Bench needs -DATX_BUILD_BENCH=ON.
Shared-branch discipline: branch feat/atx-engine-cost off feat/atx-core-stdlib (or in-place per the run's
  choice); stage EXPLICIT pathspecs only, never git add -A/-u; no push.
End commit messages with: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```

---

## §4 — Architecture & algorithms

Pseudocode (informal; `:=` assign, `->` member, `// §` plan refs). Compilable C++ lives only in §5 `TEST(...)` blocks. Money math is `core::Decimal` at the ledger boundary; all fit math is `f64`.

### 4.1 Robust least squares (`robust_ls.hpp`, S6-1) — the Pattern-B kernel

IRLS with a Huber weight over atx-core `wls`, deterministic fixed-iteration:

```cpp
struct RobustCfg { f64 huber_k = 1.345; usize max_iter = 50; f64 tol = 1e-10; };   // 1.345 = 95% Gaussian eff.
struct RobustFit { VecX beta; f64 r2; std::vector<f64> residuals; usize iters; };

RobustFit irls_huber(const MatX& X /*n x p*/, const VecX& y, const RobustCfg& c):
  w := ones(n)                                              // start at OLS
  for it in 0..c.max_iter:                                  // deterministic, RNG-free (C5)
    fit := core::regression::wls(X, y, as_vector(w))        // atx-core L7 (C7) — weighted LS
    r   := y - X*fit.beta                                   // residuals
    s   := 1.4826 * median(|r|)                             // robust scale (MAD)
    w'  := huber_weight(r / max(s,eps), c.huber_k)          // weight: 1 if |z|<=k else k/|z|
    if max|w' - w| < c.tol: break                           // fixed-tolerance convergence
    w := w'
  return { fit.beta, fit.r2, r, it }
  // huber_k = +inf  =>  all weights 1  =>  EXACTLY core::regression::ols  (C4 differential anchor)
```

**Determinism (C5):** no RNG, fixed `max_iter`, order-fixed reductions (`median` via the engine's `eval::stats_ext::median`). **Differential (C4):** at `huber_k=∞` it must equal `core::regression::ols` to ULP-class.

### 4.2 Cost-coefficient calibration (`calibration.hpp`, S6-1)

```cpp
struct CalibratedCost { exec::ImpactCfg impact; exec::SlippageCfg slippage; FitReport report; };
struct FitReport { f64 r2_temp; f64 r2_perm; f64 delta_stderr; f64 Y_stderr; usize n_fills; f64 resid_p95; };

// One observation per realized fill: realized slippage normalized by volatility vs participation.
//   participation  p_i  = |qty_i| / ADV_i            (from FillPayload.qty + Market.stats(id).adv)
//   realized_temp  I_i  = |fill_price_i - mark_before_i| / mark_before_i      (fraction of price)
//   sigma_i             = Market.stats(id).sigma
// Square-root law: I/sigma = Y * p^delta  ->  log(I/sigma) = log(Y) + delta*log(p)   (linear in (logY, delta))
CalibratedCost calibrate_impact(std::span<const exec::FillPayload> fills,
                                const Market& market_at_fill,        // mark/stats as of each fill (PIT)
                                usize trailing_window):
  rows := []                                                         // build the log-linear design
  for f in fills with f.t in trailing_window:                       // FIT-ON-TRAILING (C2)
    p := abs(f.qty) / market.stats(f.id).adv
    I := abs(to_double(f.price) - mark_before(f)) / mark_before(f)
    if p > 0 and I > 0 and sigma > 0:                               // log domain guard (NaN-safe, C/M8)
      rows.push( x = [1, log(p)], y = log(I / sigma) )
  fit := irls_huber(design(rows.x), vec(rows.y), RobustCfg{})       // §4.1 robust (C7)
  Y   := exp(fit.beta[0]);  delta := fit.beta[1]                    // recover prefactor + exponent
  gamma := fit_permanent_linear(fills, market)                     // perm: mark-shift ~ 0.5*gamma*sigma*p, alpha=1 (C3)
  eta   := fit_spread_floor(fills, market)                         // slippage/spread baseline
  return { ImpactCfg{Y, clamp(delta, 0.3, 0.9), gamma},            // clamp to a sane impact range
           SlippageCfg{ ... bps = eta_to_bps(eta) ... },
           FitReport{ fit.r2, r2_perm, stderr(delta), stderr(Y), n, p95(fit.residuals) } }
```

**Auditable (C1):** `FitReport` carries R², the `δ`/`Y` standard errors, and the 95th-percentile residual — the calibration is *reported*, not asserted. **Firewall (C2):** only fills with `t ∈ trailing_window` enter the design; the same fit on `[0,t)` vs `[0,t+k)` yields identical coefficients (truncation-invariant). **Permanent linear (C3):** `γ` is fit with the exponent fixed at 1.

### 4.3 Temp/perm split proof + throttle (`temp_perm.hpp`, S6-2)

```cpp
// PROOF helper (drives the zero-leakage property test): round-trip a trade and read marks before/after.
struct RoundTrip { f64 mark_before; f64 fill_price; f64 mark_after_fill; f64 mark_next_bar; f64 fee; };
RoundTrip simulate_round_trip(ExecutionSimulator& sim, Market& mkt, OrderPayload order):
  mark_before := mkt.mark(order.id)
  fills := sim.settle_pending(now, mkt)                    // temp -> fill price; perm -> mkt.shift_mark
  mark_after := mkt.mark(order.id)                         // moved ONLY by permanent impact
  advance_bar_no_trade(mkt)                                // next bar, no new order
  return { mark_before, fill_price, mark_after, mkt.mark(order.id), fee }
// ZERO-LEAKAGE INVARIANT (C3, the spec's hard exit): the TEMPORARY component
//   temp_frac = |fill_price - mark_before|/mark_before - perm_frac
// must NOT appear in mark_after or mark_next_bar: (mark_after - mark_before)/mark_before ~= perm_frac ONLY.

f64 fit_split_ratio(fills, marks):                         // perm magnitude / temp magnitude, for reporting
  return median( perm_shift_i / max(temp_slip_i, eps) )

// THROTTLE (RenTech ~2-day hold): decline a trade whose modeled edge does not clear modeled round-trip cost.
bool should_trade(f64 expected_edge_bps, f64 predicted_cost_bps, f64 safety = 1.0):
  return expected_edge_bps > safety * predicted_cost_bps
```

### 4.4 Capacity (`capacity.hpp`, S6-3) — wrappers + the capacity point

```cpp
// Reuse the EXISTING risk::capacity_curve (C6) — do NOT re-derive the impact sweep.
std::vector<risk::CapacityPoint>
capacity_for_alpha(const alpha::AlphaStreams& streams, usize alpha_idx,
                   const PanelView& panel, const ExecutionSimulator& calibrated_sim,
                   std::span<const f64> aum_grid):
  w := last_period_positions(streams, alpha_idx)           // the alpha's target weights
  return risk::capacity_curve(w, panel, calibrated_sim, aum_grid)   // existing engine fn

std::vector<risk::CapacityPoint>
capacity_for_book(std::span<const f64> combined_weights,   // the P4 mega-alpha book
                  const PanelView& panel, const ExecutionSimulator& calibrated_sim,
                  std::span<const f64> aum_grid):
  return risk::capacity_curve(combined_weights, panel, calibrated_sim, aum_grid)

// The capacity POINT: AUM where net_edge_bps crosses 0. Curve is monotone-decreasing under concave impact,
// so bisect the sampled curve (interpolate the zero-crossing). Returns +inf if never crosses on the grid.
f64 capacity_point(std::span<const risk::CapacityPoint> curve):
  assert_monotone_nonincreasing(curve.net_edge_bps)        // C4 sanity (concave impact)
  find adjacent (a, b) with net_edge[a] > 0 >= net_edge[b]
  return linear_interp_zero(a, b)                          // the AUM where net edge hits 0
```

### 4.5 Cost-aware knobs (`cost_aware.hpp`, S6-4)

```cpp
struct CostKnobs { f64 kappa; combine::GateConfig gate; f64 fitness_cost_floor; };

// Map the calibrated cost into the three existing decision points (C6/C8) — no new optimizer/gate/fitness.
CostKnobs cost_aware_knobs(const CalibratedCost& cc, const Market& mkt, f64 horizon_days):
  rt_cost_bps := round_trip_cost_bps(cc, typical_participation(mkt))   // modeled round-trip cost
  // kappa set so the optimizer's soft-threshold declines a rebalance whose edge < round-trip cost.
  kappa := rt_cost_bps / 1e4                              // feeds risk::OptimizerConfig.turnover_penalty
  // gate: tighten max_turnover so a net-losing high-turnover alpha is rejected at admission.
  gate := combine::GateConfig{ .max_turnover = max_turnover_for(rt_cost_bps, horizon_days), ... }
  // fitness: a cost floor the factory subtracts so gross-mirage alphas down-rank (S3 FitnessCfg hook).
  fitness_cost_floor := rt_cost_bps
  return { kappa, gate, fitness_cost_floor }

// Down-ranking proof helper: net fitness = wq_fitness - cost_penalty(turnover, rt_cost).
f64 cost_adjusted_fitness(const combine::AlphaMetrics& m, f64 rt_cost_bps):
  return m.fitness - cost_penalty(m.turnover, rt_cost_bps)   // a higher-turnover net-loser drops below a net-winner
```

### 4.6 Borrow / short-financing accrual (`borrow.hpp`, S6-5)

```cpp
enum class DayCount : u8 { D360, D365, D252 };
struct BorrowModel { f64 annual_rate; DayCount day_count = DayCount::D360; };   // GC ~25-50bps; HTB 100%+

f64 denom(DayCount d): return d==D360?360.0 : d==D365?365.0 : 252.0;

// Daily accrual on the SHORT notional only (a dollar-neutral book is ~half short).
core::Decimal daily_borrow(const BorrowModel& b, const Portfolio& pf, const Market& mkt):
  short_notional := 0
  for id in universe:
    h := pf.holding(id)
    if h.qty < 0: short_notional += |h.qty| * mkt.mark(id)    // |short_notional|
  return decimal_from( short_notional * b.annual_rate / denom(b.day_count) )

// ADDITIVE application (C6, §0.7): a synthetic fee-only fill — zero qty, fee = the daily charge.
void accrue_borrow(const BorrowModel& b, Portfolio& pf, const Market& mkt, Timestamp t):
  fee := daily_borrow(b, pf, mkt)
  pf.apply_fill( exec::FillPayload{ .id=any_id, .qty=0, .price=Decimal{0}, .fee=fee, .impact=0, .t=t } )
  // FIRST VERIFY apply_fill(qty=0) debits cash by fee and mutates no position; else fall back to a single
  // additive Portfolio::accrue_financing(Decimal) (the one reviewed engine touch — flag in ledger).
```

---

## §5 — Per-unit plan

Sequential dispatch (each unit consumes the prior). Fresh implementer → spec-compliance review → code-quality review → fix loop → ledger SHA, per `superpowers:subagent-driven-development`. **Branch `feat/atx-engine-cost` off `feat/atx-core-stdlib` → explicit-pathspec commits** (handoff block). Six units; no sub-split needed (≤7).

### Task S6-0: Marker + ledger + scaffold + as-built seam verification
**Files:** Create `cost/fwd.hpp`; Create `atx-engine/plans/p1/sprint-6-progress.md`; Test `tests/cost_scaffold_test.cpp`.
**Scope:** §2.4 + §0. Open the ledger, scaffold the layer doc block (C1–C8 + the seam map + the impact formulae), and **verify the as-built seams S6 reuses actually have the assumed shape** before any calibration unit. **First verify** `exec::ImpactCfg{Y,delta,gamma}`, `risk::capacity_curve(...)`, `risk::OptimizerConfig::turnover_penalty`, and — critically — whether `Portfolio::apply_fill` with `qty=0` is a pure fee debit (decides the §0.7 borrow path).

- [ ] **Step 1 (scaffold tests):** suite `CostScaffold` —
```cpp
#include <gtest/gtest.h>
#include "atx/engine/exec/execution_sim.hpp"   // First verify the real include paths
#include "atx/engine/risk/capacity.hpp"
#include "atx/engine/risk/optimizer.hpp"
#include "atx/engine/portfolio/portfolio.hpp"
#include "atx/engine/cost/fwd.hpp"

namespace {
using atx::f64;
namespace exec = atx::engine::exec;

TEST(CostScaffold, ImpactCfg_HasNamedCoefficients_WithExpectedDefaults) {   // §0.1 shape check
  exec::ImpactCfg c;
  EXPECT_DOUBLE_EQ(c.Y, 1.0);
  EXPECT_DOUBLE_EQ(c.delta, 0.5);
  EXPECT_DOUBLE_EQ(c.gamma, 0.314);                  // the Almgren-2005 universal perm coeff (§0.2)
}

TEST(CostScaffold, OptimizerConfig_HasTurnoverPenaltyKappa) {               // §0.5 reuse check
  atx::engine::risk::OptimizerConfig o;
  EXPECT_DOUBLE_EQ(o.turnover_penalty, 0.0);         // exists, default off — S6-4 calibrates it
}

TEST(CostScaffold, ApplyFill_ZeroQty_IsPureFeeDebit) {                      // §0.7 borrow-path decision
  auto pf = make_portfolio(/*cash*/1'000'000.0, single_universe());
  const auto cash0 = pf.cash();
  pf.apply_fill(exec::FillPayload{ .id=first_id(), .qty=0,
                .price=atx::core::Decimal{0}, .fee=atx::core::Decimal::from_double(12.5),
                .impact=0.0, .t={} });
  EXPECT_EQ(pf.holding(first_id()).qty, 0);          // no position change
  EXPECT_TRUE(cash0 - pf.cash() == atx::core::Decimal::from_double(12.5));  // cash debited by fee only
  // If this FAILS, record it: S6-5 uses the additive Portfolio::accrue_financing fallback instead.
}
} // namespace
```
- [ ] **Step 2:** Build → FAIL (`cost/fwd.hpp` missing; confirm the engine include paths resolve).
- [ ] **Step 3:** Create `cost/fwd.hpp`: `#pragma once`, `namespace atx::engine::cost {}` with the doc block (C1–C8 verbatim, the seam map, the `temp=Y·σ·part^δ` / `perm=0.5·γ·σ·part` formulae, the one-cost-model contract). Forward-declare `struct CalibratedCost; struct FitReport; struct CostKnobs; struct BorrowModel;`.
- [ ] **Step 4:** `ctest --test-dir build -R CostScaffold` → 3/3 pass. **Record the `ApplyFill_ZeroQty_*` outcome in the ledger** — it decides the S6-5 borrow path (synthetic fill vs the one additive method).
- [ ] **Step 5:** Open `sprint-6-progress.md` (copy `sprint-4-progress.md`): header (Sprint, Branch `feat/atx-engine-cost`, Base `feat/atx-core-stdlib @ <HEAD-sha>`, Status `🚧 OPEN`), the §0 amendment summary (1–8), kickoff risks `(a)` Pattern-B robust-LS edge, `(b)` the apply_fill(qty=0) borrow-path decision, `(c)` shared-branch discipline, `(d)` **dominant risk: a cost model that looks calibrated but peeks / double-counts / leaks temp into the mark**.
- [ ] **Step 6:** Commit: `git add -- atx-engine/plans/p1/sprint-6-progress.md atx-engine/include/atx/engine/cost/fwd.hpp atx-engine/tests/cost_scaffold_test.cpp; git commit -- <them> -m "docs(s6-0): open sprint-6 cost-calibration ledger + scaffold + as-built seam verification" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"; git show HEAD --stat`.

### Task S6-1: Cost-coefficient calibration harness (robust LS + log-linear fit)
**Files:** Create `cost/robust_ls.hpp`, `cost/calibration.hpp`; Test `tests/robust_ls_test.cpp`, `tests/calibration_test.cpp`.
**Scope:** §4.1/§4.2 + §0.1/§0.2/§0.3 + C1/C2/C4/C5/C7. The Pattern-B IRLS-Huber kernel + the log-linear power-law calibration that recovers `(Y, δ, γ, η)` from fills with auditable fit quality. **First verify** `core::regression::{ols,wls}` signatures + `OlsResult{beta,r2,residuals}`, `exec::FillPayload` fields, `Market::stats(id) → InstrumentStats{adv,sigma,spread}`.

- [ ] **Step 1 (robust-LS tests):** suite `RobustLs` —
```cpp
TEST(RobustLs, HuberInfinity_EqualsOls) {                          // C4 (differential anchor)
  auto [X, y] = clean_linear_fixture(/*n*/40, /*p*/2);
  auto rob = cost::irls_huber(X, y, cost::RobustCfg{ .huber_k = 1e18 });   // no down-weighting
  auto ols = atx::core::regression::ols(X, y);
  ASSERT_TRUE(ols.has_value());
  for (int j=0;j<2;++j) EXPECT_NEAR(rob.beta[j], ols->beta[j], 1e-9);
}

TEST(RobustLs, OutlierContamination_RobustBeatsOls) {              // C1 (why robust)
  auto [X, y] = linear_with_outliers_fixture(/*n*/60, /*frac*/0.1);
  auto rob = cost::irls_huber(X, y, cost::RobustCfg{});
  auto ols = atx::core::regression::ols(X, y);
  EXPECT_LT(coef_error(rob.beta, true_beta()), coef_error(ols->beta, true_beta()));  // robust closer to truth
}

TEST(RobustLs, SameData_ByteIdentical) {                          // C5
  auto [X, y] = linear_with_outliers_fixture(60, 0.1);
  EXPECT_EQ(hash_vec(cost::irls_huber(X,y,{}).beta), hash_vec(cost::irls_huber(X,y,{}).beta));
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `robust_ls.hpp` (§4.1): IRLS over `core::regression::wls`, MAD scale via `eval::stats_ext::median`, Huber weights, fixed `max_iter`/`tol`. **Step 4:** `ctest --test-dir build -R RobustLs` → pass.
- [ ] **Step 5 (calibration tests):** suite `Calibration` —
```cpp
TEST(Calibration, RecoversInjectedSyntheticCoefficients) {        // C4 (the spec's headline proof)
  const f64 Y_true=0.8, delta_true=0.55, gamma_true=0.3;
  auto [fills, market] = synthetic_fills(Y_true, delta_true, gamma_true, /*n*/2000, /*seed*/5);
  auto cc = cost::calibrate_impact(fills, market, /*window*/all());
  EXPECT_NEAR(cc.impact.Y,     Y_true,     0.05);
  EXPECT_NEAR(cc.impact.delta, delta_true, 0.03);                 // exponent recovered within tol
  EXPECT_NEAR(cc.impact.gamma, gamma_true, 0.05);
}

TEST(Calibration, ReportsHonestFitQuality) {                      // C1 (auditable, not asserted)
  auto [fills, market] = synthetic_fills(0.8, 0.55, 0.3, 2000, 5);
  auto cc = cost::calibrate_impact(fills, market, all());
  EXPECT_GT(cc.report.r2_temp, 0.8);                              // good fit on clean synthetic
  EXPECT_GT(cc.report.delta_stderr, 0.0);                         // error bars are reported
  EXPECT_EQ(cc.report.n_fills, 2000u);
}

TEST(Calibration, FitOnTrailing_TruncationInvariant) {            // C2 (no cost-model look-ahead)
  auto [fills, market] = synthetic_fills(0.8, 0.55, 0.3, 2000, 5);
  auto window = trailing(fills, /*upto_t*/1000);
  auto a = cost::calibrate_impact(fills, market, window);                  // fills up to t=1000
  auto b = cost::calibrate_impact(prefix(fills, 1500), market, window);    // more future fills present
  EXPECT_EQ(hash_impact(a.impact), hash_impact(b.impact));        // coeffs depend only on the trailing window
}

TEST(Calibration, DegenerateFill_NaNSafe_NotInDesign) {           // M8/NaN policy
  auto [fills, market] = synthetic_fills_with_zero_participation(0.8, 0.55, 0.3, /*n*/500);
  auto cc = cost::calibrate_impact(fills, market, all());         // zero/NaN participation rows dropped
  EXPECT_TRUE(std::isfinite(cc.impact.delta));                    // no NaN poisoning the fit
}
```
- [ ] **Step 6:** Build → FAIL. **Step 7:** Implement `calibration.hpp` (§4.2): build the log-linear design from fills (trailing window only), `irls_huber` for the temp channel, `fit_permanent_linear` for `γ` (exponent fixed at 1), spread floor for `η`, `FitReport` with R²/stderr/p95. **Step 8:** `ctest --test-dir build -R "RobustLs|Calibration"` → pass; full suite green. **Step 9:** Commit + ledger row (`feat(s6-1): cost-coefficient calibration (IRLS-Huber robust LS + log-linear power-law fit, auditable)`).

### Task S6-2: Almgren-Chriss temp/perm split — zero-leakage proof + throttle
**Files:** Create `cost/temp_perm.hpp`; Test `tests/temp_perm_test.cpp`.
**Scope:** §4.3 + §0.6 + C3. Prove temporary cost never leaks into the forward mark; fit the split ratio; the throttle decision. **First verify** `Market::shift_mark(id, delta)`, `Market::mark(id)`, `ExecutionSimulator::settle_pending(now, market)`, `FillCfg::allow_same_bar_fill` default false.

- [ ] **Step 1 (temp/perm tests):** suite `TempPerm` —
```cpp
TEST(TempPerm, TemporaryCost_DoesNotLeakIntoForwardMark) {        // C3 (the spec's hard exit criterion)
  auto sim = make_sim(exec::ImpactCfg{ .Y=1.0, .delta=0.5, .gamma=0.314 });
  auto mkt = make_market_single(/*mark*/100.0, /*adv*/1e6, /*sigma*/0.02);
  auto rt = cost::simulate_round_trip(sim, mkt, buy_order(/*qty*/5000));
  const f64 perm_frac = 0.5 * 0.314 * 0.02 * (5000.0/1e6);
  // the mark moves by ONLY the permanent fraction — the temporary slip is in the fill price, not the mark
  EXPECT_NEAR((rt.mark_after_fill - rt.mark_before)/rt.mark_before, perm_frac, 1e-9);
  // and it does not grow on the next (no-trade) bar
  EXPECT_NEAR(rt.mark_next_bar, rt.mark_after_fill, 1e-12);
  // the temporary component is strictly present in the fill price
  EXPECT_GT(std::abs(rt.fill_price - rt.mark_before)/rt.mark_before, perm_frac);
}

TEST(TempPerm, Throttle_DeclinesTradeBelowCost) {                 // RenTech ~2-day-hold throttle
  EXPECT_FALSE(cost::should_trade(/*edge*/0.2, /*cost*/0.5));     // edge < cost -> decline
  EXPECT_TRUE (cost::should_trade(/*edge*/2.0, /*cost*/0.5));     // edge > cost -> trade
}

TEST(TempPerm, SplitRatio_PositiveAndStable) {                   // ratio fit (reporting)
  auto [fills, marks] = round_trip_fixture(/*n*/200);
  auto r = cost::fit_split_ratio(fills, marks);
  EXPECT_GT(r, 0.0);
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `temp_perm.hpp` (§4.3): `simulate_round_trip` (drive the existing sim+market), `fit_split_ratio`, `should_trade`. No edit to `execution_sim.hpp`/`market.hpp` — the proof reads their existing behavior. **Step 4:** `ctest --test-dir build -R TempPerm` → pass; full suite green. **Step 5:** Commit + ledger row (`feat(s6-2): temp/perm split — zero-mark-leakage proof + split-ratio fit + cost throttle`).

### Task S6-3: Capacity curve — per-alpha / per-mega + capacity point
**Files:** Create `cost/capacity.hpp`; Test `tests/capacity_test.cpp`.
**Scope:** §4.4 + §0.4 + C4/C6. Extend the existing `risk::capacity_curve` with per-alpha (AlphaStreams) + per-mega (P4 book) wrappers + the capacity-point root-find + monotonicity. **First verify** `risk::capacity_curve(span<const f64> weights, const PanelView&, const exec::ExecutionSimulator&, span<const f64> aum_grid) → vector<CapacityPoint{f64 aum; f64 net_edge_bps;}>` and `alpha::AlphaStreams::positions(alpha, period)`.

- [ ] **Step 1 (capacity tests):** suite `Capacity` —
```cpp
TEST(Capacity, CurveMonotoneDecreasingInAum) {                    // §0.4 (concave impact)
  auto sim = calibrated_sim();
  auto curve = cost::capacity_for_book(real_edge_book_weights(), panel_fixture(), sim,
                                       aum_grid({1e6, 1e7, 1e8, 1e9}));
  for (size_t i=1;i<curve.size();++i)
    EXPECT_LE(curve[i].net_edge_bps, curve[i-1].net_edge_bps + 1e-9);   // non-increasing
}

TEST(Capacity, CapacityPoint_FiniteOnRealEdgeFixture) {           // spec exit criterion
  auto curve = cost::capacity_for_book(real_edge_book_weights(), panel_fixture(),
                                       calibrated_sim(), aum_grid({1e6,1e7,1e8,1e9,1e10}));
  const f64 cap = cost::capacity_point(curve);
  EXPECT_GT(cap, 0.0);
  EXPECT_TRUE(std::isfinite(cap));                                // a finite AUM where net edge hits 0
}

TEST(Capacity, ReusesSimImpactCfg_NoSecondModel) {                // C6 (one cost model)
  auto sim_a = make_sim(exec::ImpactCfg{ .Y=0.5, .delta=0.5, .gamma=0.314 });
  auto sim_b = make_sim(exec::ImpactCfg{ .Y=2.0, .delta=0.5, .gamma=0.314 });  // higher impact
  auto cap_a = cost::capacity_point(cost::capacity_for_book(w(), panel_fixture(), sim_a, grid()));
  auto cap_b = cost::capacity_point(cost::capacity_for_book(w(), panel_fixture(), sim_b, grid()));
  EXPECT_LT(cap_b, cap_a);                                        // more impact -> lower capacity (uses sim cfg)
}

TEST(Capacity, PerAlpha_FromStreams) {                            // per-alpha path
  auto streams = alpha_streams_fixture();
  auto curve = cost::capacity_for_alpha(streams, /*idx*/0, panel_fixture(), calibrated_sim(), grid());
  EXPECT_FALSE(curve.empty());
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `capacity.hpp` (§4.4): the three wrappers calling `risk::capacity_curve`, `capacity_point` (assert monotone + interpolate zero-crossing). **Step 4:** `ctest --test-dir build -R Capacity` → pass; full suite green. **Step 5:** Commit + ledger row (`feat(s6-3): capacity curve — per-alpha + per-mega wrappers + capacity-point root-find (reuses risk::capacity_curve)`).

### Task S6-4: Cost-aware fitness / gating hook
**Files:** Create `cost/cost_aware.hpp`; Test `tests/cost_aware_test.cpp`.
**Scope:** §4.5 + §0.5 + C8. Map calibrated cost into the factory fitness floor, the gate's `max_turnover`, and the optimizer `κ`; prove a high-turnover net-loser down-ranks. **First verify** `combine::AlphaMetrics{turnover,fitness,margin,...}`, `combine::GateConfig::max_turnover`, `factory::FitnessCfg`, `risk::OptimizerConfig::turnover_penalty`.

- [ ] **Step 1 (cost-aware tests):** suite `CostAware` —
```cpp
TEST(CostAware, DownRanksHighTurnoverNetLoser) {                  // spec exit (the WQ §8 thesis, calibrated)
  auto winner = metrics(/*sharpe*/2.0, /*turnover*/0.10, /*returns*/0.15);  // low turnover, net winner
  auto loser  = metrics(/*sharpe*/2.5, /*turnover*/0.90, /*returns*/0.16);  // high turnover, net loser after cost
  const f64 rt_cost_bps = 8.0;
  EXPECT_GT(cost::cost_adjusted_fitness(winner, rt_cost_bps),
            cost::cost_adjusted_fitness(loser,  rt_cost_bps));     // calibrated cost flips the ranking
}

TEST(CostAware, KappaScalesWithRoundTripCost) {                  // §0.5 (derive existing kappa)
  auto cheap = cost::cost_aware_knobs(calibrated_cost(/*bps*/2.0), market_fixture(), /*h*/5.0);
  auto dear  = cost::cost_aware_knobs(calibrated_cost(/*bps*/20.0), market_fixture(), 5.0);
  EXPECT_GT(dear.kappa, cheap.kappa);                             // costlier market -> stronger turnover penalty
}

TEST(CostAware, GateTightensWithCost) {                          // gate max_turnover from cost
  auto dear = cost::cost_aware_knobs(calibrated_cost(20.0), market_fixture(), 5.0);
  EXPECT_LT(dear.gate.max_turnover, 0.70);                        // tighter than the default ceiling
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `cost_aware.hpp` (§4.5): `round_trip_cost_bps`, `cost_aware_knobs` (κ + GateConfig + fitness floor), `cost_adjusted_fitness`. No edit to `optimizer.hpp`/`gate.hpp`/`fitness.hpp` — S6 produces the *values*; the caller feeds them in. **Step 4:** `ctest --test-dir build -R CostAware` → pass; full suite green. **Step 5:** Commit + ledger row (`feat(s6-4): cost-aware knobs — calibrated cost → factory fitness floor + gate max_turnover + optimizer kappa`).

### Task S6-5: Borrow accrual + integration proofs + bench + close
**Files:** Create `cost/borrow.hpp`; Test `tests/borrow_test.cpp`, `tests/cost_integration_test.cpp`; Create `bench/cost_bench.cpp`; edit `sprint-6-progress.md` (+ close ceremony).
**Scope:** §4.6 + §0.7 + §6 end-to-end + close. Short-financing accrual (additive via synthetic fee-fill) + the five load-bearing proofs in one suite. **First verify** the S6-0 `ApplyFill_ZeroQty_*` outcome (decides synthetic-fill vs the additive-method fallback).

- [ ] **Step 1 (borrow tests):** suite `Borrow` —
```cpp
TEST(Borrow, DailyAccrual_MatchesHandArithmetic) {                // C4
  cost::BorrowModel b{ .annual_rate=0.05, .day_count=cost::DayCount::D360 };
  auto pf = portfolio_with_short(/*short_notional*/2'000'000.0);
  auto fee = cost::daily_borrow(b, pf, market_fixture());
  EXPECT_TRUE(fee == atx::core::Decimal::from_double(2'000'000.0 * 0.05 / 360.0));  // ~277.78
}

TEST(Borrow, AccrueDebitsCashByBorrow_NoPositionChange) {         // §0.7 (additive via synthetic fill)
  cost::BorrowModel b{ .annual_rate=0.05 };
  auto pf = portfolio_with_short(2'000'000.0);
  const auto cash0 = pf.cash();
  cost::accrue_borrow(b, pf, market_fixture(), /*t*/{});
  EXPECT_TRUE(cash0 - pf.cash() == cost::daily_borrow(b, pf, market_fixture()));
}

TEST(Borrow, LongOnlyBook_NoBorrowCharge) {                       // dollar-neutral edge case
  cost::BorrowModel b{ .annual_rate=0.05 };
  auto pf = long_only_portfolio();
  EXPECT_TRUE(cost::daily_borrow(b, pf, market_fixture()) == atx::core::Decimal{0});
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `borrow.hpp` (§4.6): `BorrowModel`, `daily_borrow` (sum short notional × rate/denom), `accrue_borrow` (synthetic fee-only fill; or the `Portfolio::accrue_financing` fallback per the S6-0 finding). **Step 4:** `ctest --test-dir build -R Borrow` → pass.
- [ ] **Step 5 (integration tests):** suite `CostIntegration` —
```cpp
TEST(CostIntegration, EndToEnd_CalibrateThenCapacityThenGate) {   // the sprint's end-to-end proof
  auto [fills, market] = synthetic_fills(/*Y*/0.8,/*d*/0.55,/*g*/0.3,/*n*/2000,/*seed*/9);
  auto cc  = cost::calibrate_impact(fills, market, all());        // C1/C4
  auto sim = make_sim(cc.impact);                                 // §0.1 reconstruct from calibrated cfg
  auto cap = cost::capacity_point(cost::capacity_for_book(real_edge_book_weights(),
                                  panel_fixture(), sim, grid()));  // §0.4
  auto knobs = cost::cost_aware_knobs(cc, market, /*h*/5.0);      // §0.5
  EXPECT_NEAR(sim.impact_cfg().delta, 0.55, 0.03);                // calibrated coeffs flow into the sim
  EXPECT_TRUE(std::isfinite(cap));
  EXPECT_GT(knobs.kappa, 0.0);
}

TEST(CostIntegration, FullChain_ReplaysByteIdentical) {           // C5 (determinism end-to-end)
  auto run = [&]{ return cost::full_calibration_digest(synthetic_fixture(/*seed*/9)); };
  EXPECT_EQ(run(), run());
}

TEST(CostIntegration, CostModelNeverPeeks_TruncationInvariant) {  // C2 end-to-end
  auto fx = synthetic_fixture(9);
  EXPECT_EQ(cost::calib_digest(fx, trailing(/*t*/1000)),
            cost::calib_digest(prefix(fx,1500), trailing(1000)));
}
```
- [ ] **Step 6:** Build → FAIL. **Step 7:** Implement any thin `full_calibration_digest`/`calib_digest` harness helpers (wiring only). **Step 8:** `ctest --test-dir build -R "Borrow|CostIntegration"` → pass; **full engine suite green (record the N/N count)**.
- [ ] **Step 9 (bench):** Create `bench/cost_bench.cpp` — register: calibration fits/sec at {n=500,2000,8000} fills, IRLS iters/sec, capacity-curve sweeps/sec at {grid=8,32}, borrow accrual/sec. `BENCHMARK(...)` auto-globbed; build `-DATX_BUILD_BENCH=ON`.
- [ ] **Step 10 (close ceremony, per [`../docs/sprint.md`](../docs/sprint.md)):** fill `sprint-6-progress.md` (all rows ✅, commits table, **residuals → ROADMAP backlog**: robust/nonlinear-LS → atx-core L7; the `extract_streams` full-impact re-pricing §0.8; the one `Portfolio::accrue_financing` touch iff used); flip `p1/ROADMAP.md` S6 row `⏳ → ✅ <sha>` + bump `Last reviewed`; mark spec `Status: ✅ closed`; create `sprint-6.md` user reference; write the **baton → S7** paragraph. **⚠️ `p1/ROADMAP.md` may carry the user's uncommitted edits — coordinate before staging it; stage only the S6 row change with an explicit pathspec.** **Step 11:** Commit (`docs(s6-5): close sprint-6 — 6 units, <M> tests; cost calibration + capacity + borrow shipped`) + `git show HEAD --stat`.

---

## §6 — Exit criteria · invariants · dependencies · NOT-in-scope · baton

**Exit criteria (from the spec, made concrete):**
- **Calibration recovers known coefficients on synthetic fills** — `Calibration.RecoversInjectedSyntheticCoefficients` (inject `δ,Y,γ` → fit back within tolerance) + **honest fit quality reported** — `Calibration.ReportsHonestFitQuality` (R², `δ`-stderr, n_fills).
- **Calibrated coefficients are fit/apply-firewalled, truncation-invariant** — `Calibration.FitOnTrailing_TruncationInvariant` + `CostIntegration.CostModelNeverPeeks_TruncationInvariant`.
- **Zero temporary-cost leakage into the forward mark** — `TempPerm.TemporaryCost_DoesNotLeakIntoForwardMark` (the mark moves by *only* the permanent fraction, and does not grow on the next no-trade bar).
- **Capacity curve monotone-decreasing in AUM + finite capacity point; per-mega runs the P4 book through the calibrated sim** — `Capacity.CurveMonotoneDecreasingInAum` + `CapacityPoint_FiniteOnRealEdgeFixture` + `PerAlpha_FromStreams` + `CostIntegration.EndToEnd_*`.
- **Cost-aware fitness down-ranks a high-turnover net-loser** — `CostAware.DownRanksHighTurnoverNetLoser` (+ `KappaScalesWithRoundTripCost`, `GateTightensWithCost`).
- **Borrow accrual correct + additive** — `Borrow.DailyAccrual_MatchesHandArithmetic` + `AccrueDebitsCashByBorrow_NoPositionChange` + `LongOnlyBook_NoBorrowCharge`.
- **Determinism end-to-end** — `RobustLs.SameData_ByteIdentical` + `CostIntegration.FullChain_ReplaysByteIdentical`.
- `/W4 /permissive- /WX` + strict-FP clean; one test file per unit; full engine suite green (no regression).

**Invariants proven (C1–C8):** Calibrated-not-guessed (C1) — coefficients fit to fills with reported R²/stderr/p95. Fit/apply firewall (C2) — trailing-fit/apply-forward, truncation-invariant, the cost model never peeks. Temp/perm separation (C3) — temporary reverts (fill price only), permanent persists + linear (`α=1`), zero mark leakage. Differential correctness (C4) — synthetic recovery + `huber_k=∞`-equals-`ols` + borrow vs hand arithmetic + monotone capacity. Determinism (C5) — RNG-free IRLS, fixed iteration, byte-stable coefficients, identical capacity replay. **One cost model (C6) — calibration emits `exec::ImpactCfg`, capacity calls `risk::capacity_curve`, the turnover penalty reuses `OptimizerConfig.κ`, one `Σ|Δw|` turnover definition; no second impact formula anywhere.** Pattern B (C7) — robust/nonlinear LS engine-local-then-lifted; `ols`/`wls`/`solve` consumed from atx-core. Cost is a decision input (C8) — calibrated cost feeds fitness/gate/κ; capacity is a first-class reported metric.

**Dependencies:** Upstream **p0 Phase-2** `exec::ExecutionSimulator` + `loop::Market` + `portfolio::Portfolio` (the model being calibrated; the calibration **core** S6-1/S6-2 needs only p0 — opens with S1/S2). **Phase-3** `alpha::AlphaStreams` (per-alpha capacity). **P4** (closed `f2d22f4`) — `risk::capacity_curve` + `risk::OptimizerConfig` (the curve + κ S6 extends/derives) + the combined book (per-mega capacity). **S3** (closed `5f57a34`) — `factory::FitnessCfg`/`pool_aware_fitness` (the fitness S6-4 feeds). **combine** (P4) — `compute_metrics`/`GateConfig` (the one turnover definition + the gate S6-4 tightens). **atx-core L7/L1** — `regression::{ols,wls,ridge}`, `linalg`, `solve`, `math` (the linear core of every fit, consumed directly). **Pattern-B request (§2.1):** robust/nonlinear LS (IRLS-Huber) → atx-core L7 (engine-local this sprint). **No `exec`/`risk`/`combine`/`factory` source edits** — S6 is purely additive; the **only** possible engine touch is a single additive `Portfolio::accrue_financing` *iff* `apply_fill(qty=0)` misbehaves (decided at S6-0, flagged in the ledger).

**Explicitly NOT in this sprint** (spec + ROADMAP anti-roadmap): **no live market-impact measurement** (calibrate to historical fills / reference data, not live — anti-roadmap #1). **No limit-order-book microstructure simulation** (the exec sim models fills + impact + capacity, not a matching venue — anti-roadmap #2). **No intraday cost model** (daily-bar book; intraday is a p0 residual). **No rewrite of `extract_streams` to net full impact** (§0.8 — that would change S3/S5 behavior mid-flight; recorded as a residual, the cost-honesty improvement is opt-in via the new knobs). **No new general-purpose primitive in the engine** (C7 — robust LS is an atx-core request). **No second impact/turnover formula** (C6).

**Baton → next:** S6 makes cost **true** and capacity **known** — so **S7** (portfolio + lifecycle) can size and allocate the book against a real capacity ceiling: the multi-period optimizer consumes the **calibrated `κ`** (turnover priced honestly), the decay monitor reads the **calibrated cost** to tell a decaying alpha from a cost-flooded one, and capital allocation respects the **per-mega capacity point** S6-3 computes. The factory (S3) and the learned combiner (S5) now have a calibrated cost to price turnover against (S6-4 knobs), so discovery mines **net-profitable**, not gross-mirage, alphas. **Open at close:** the Pattern-B atx-core L7 robust-LS promotion; the `extract_streams` full-impact re-pricing (§0.8); and — if used — the one `Portfolio::accrue_financing` touch.

---

## §7 — References (research foundation)

- **RenTech deep-dive** [`renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md): §4 (cost = the strategy; concave √-impact; the throttling effect; capacity), §9.2 + §9.2.1 (the execution simulator is where backtests tell the truth; "calibrate, don't guess"; the worked arithmetic — at a 50.75% hit rate the net per-trade edge is `(2·0.5075−1)·g = 0.015·g ≈ 0.3 bp`, so a few bp of cost error flips the sign), §9.6 (report capacity, not just return).
- **WorldQuant deep-dive** [`worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md): §8 (linear cost `C~1/T`; turnover-aware fitness; returns scale with volatility not turnover — the cost-aware fitness thesis S6-4 makes calibrated).
- **Open-source impact literature (the calibration grounding):**
  - Almgren & Chriss (2000), *Optimal Execution of Portfolio Transactions*, J. Risk 3(2):5–39 — the temp/perm decomposition (`g(v)=γ·v` permanent linear; `h(v)=ε·sgn+η·v` temporary) + the mean-variance execution objective. https://www.smallake.kr/wp-content/uploads/2016/03/optliq.pdf
  - Almgren, Thum, Hauptmann & Li (2005), *Direct Estimation of Equity Market Impact*, Risk 18(7):58–62 — the empirical fit: temporary exponent **β≈0.6 (3/5)**, permanent **α=1 imposed** (no-manipulation), universal coefficients **γ=0.314±0.041**, **η=0.142±0.006** (large-cap US — recalibrate per universe). https://www.cis.upenn.edu/~mkearns/finread/costestim.pdf
  - Bouchaud / Tóth et al. — the **square-root law** (metaorder impact exponent ≈ **0.5**, near-universal). δ contested: Almgren-2005 = 0.6 vs Bouchaud = 0.5 → S6 keeps `δ` configurable in `[0.5,0.6]`, default fit from data. https://arxiv.org/abs/2311.18283
  - Kissell (2013), *The Science of Algorithmic Trading and Portfolio Management* — impact-model construction/calibration/testing (the log-linear + robust fit methodology). Perold (1988), *The Implementation Shortfall: Paper versus Reality*, JPM 14(3):4–9 — the IS benchmark slippage regressions target.
  - Grinold & Kahn (2000), *Active Portfolio Management* + Kahn-Lemmon capacity analysis — the breadth/IR framework and the capacity-as-AUM-where-net-edge→0 definition S6-3 computes.
  - Interactive Brokers, *Short Sale Cost* — the daily borrow accrual convention (`|short_notional|·rate/360`; GC 25–50 bps, HTB 100%+) S6-5 implements. https://www.interactivebrokers.com/en/pricing/short-sale-cost.php
- **In-repo grounding (read before implementing):** `exec/execution_sim.hpp` (the `ImpactCfg` being calibrated + the `temp=Y·σ·part^δ` / `perm=0.5·γ·σ·part` formulae), `risk/capacity.hpp` (the curve S6-3 extends), `risk/optimizer.hpp` (the `κ` S6-4 derives), `combine/metrics.hpp` (the one turnover/fitness definition), `loop/market.hpp` (`InstrumentStats` + `shift_mark`), `portfolio/portfolio.hpp` (`apply_fill` + the borrow residual note), `eval/stats_ext.hpp` (`median` for the MAD scale).

---

## §8 — Self-review (against the spec)

- **Spec coverage:** S6.0 marker → **S6-0**; S6.1 cost-coefficient calibration harness → **S6-1** (IRLS-Huber robust LS Pattern-B + log-linear power-law fit + `FitReport`); S6.2 Almgren-Chriss temp/perm split refinement → **S6-2** (zero-mark-leakage proof + split-ratio fit + throttle); S6.3 capacity curve → **S6-3** (per-alpha + per-mega + capacity-point, reusing `risk::capacity_curve`); S6.4 cost-aware fitness/gating hook → **S6-4** (calibrated cost → fitness floor + gate `max_turnover` + optimizer `κ`); S6.5 borrow/short-financing accrual + close → **S6-5** (`BorrowModel` + synthetic fee-fill + close). Every spec exit criterion maps to a named test in §6. ✅
- **User asks satisfied:** "cost & capacity calibration is one of the most important things to get right, often overlooked" → C1 (calibrated-not-guessed with reported fit quality) + the RenTech 0.3-bp-flips-the-sign grounding (§1/§7); "ground in our engine design" → every unit is reframed against the **as-built** seams (`exec::ImpactCfg`, `risk::capacity_curve`, `OptimizerConfig.κ`, `Portfolio::apply_fill`) in §0, reused not rebuilt (C6); "ground in open-source information" → Almgren-Chriss 2000, Almgren-2005 (the `γ=0.314` the engine already ships!), Bouchaud square-root law, Kissell, IB borrow convention — all cited in §7 with the exact functional forms + coefficient ranges. ✅
- **As-built reconciliation applied:** coefficients live in `exec::ImpactCfg` ⇒ calibration emits config, edits nothing (§0.1); defaults are a literature mix ⇒ calibrate per-universe (§0.2); `δ` nonlinear ⇒ log-linearize + IRLS on atx-core `ols`/`wls` (§0.3); capacity curve already exists ⇒ extend with per-alpha/per-mega/capacity-point (§0.4); `κ` already exists ⇒ derive from calibration (§0.5); temp/perm already split ⇒ prove zero leakage (§0.6); borrow absent ⇒ additive synthetic fee-fill (§0.7); streams under-charge recorded, not rewritten (§0.8). ✅
- **Placeholder scan:** no "TBD/TODO/handle-edge-cases" — every coding step shows the test code and names the file/§-ref to implement; the only literal `<…>` placeholders are commit SHAs, the final test count, and the `<HEAD-sha>` base, all filled at execution time. ✅
- **Type consistency:** `CalibratedCost{impact,slippage,report}`, `FitReport`, `RobustCfg`/`RobustFit`, `CostKnobs{kappa,gate,fitness_cost_floor}`, `BorrowModel{annual_rate,day_count}`, `DayCount`, `RoundTrip` are used with the same fields/signatures across §4 and §5; `calibrate_impact`/`irls_huber`/`capacity_for_book`/`capacity_point`/`cost_aware_knobs`/`cost_adjusted_fitness`/`daily_borrow`/`accrue_borrow` signatures match between pseudocode and tests; the calibrated `ImpactCfg` produced in S6-1 flows into `make_sim(cc.impact)` in S6-3/S6-5 (`sim.impact_cfg().delta`), closing the loop on the one-cost-model invariant. ✅
- **Anti-roadmap honored:** calibrate to historical/reference fills (no live impact measurement); no LOB matching engine; no intraday cost model; no engine general-purpose primitive (robust LS is Pattern B); no second impact/turnover formula (C6); no `extract_streams` rewrite (§0.8). ✅
- **One cost model is the load-bearing discipline** — calibration emits `exec::ImpactCfg`, capacity calls `risk::capacity_curve`, the penalty reuses `OptimizerConfig.κ`, turnover has one `Σ|Δw|` definition; the §3 handoff names "a second impact formula that double-counts or diverges from the sim" as a top risk, and C6 + the `Capacity.ReusesSimImpactCfg_NoSecondModel` test gate it. ✅
