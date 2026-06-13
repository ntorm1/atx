# Phase 4 — Alpha Pool Gates + Signal Combination + Factor Risk + Risk-Aware Construction — Implementation Plan

**Module:** `atx-engine` (`p0`)
**Phase theme:** The **mega-alpha layer**. Phase 3 turns a quant idea written as a string into a
per-alpha cross-sectional signal vector. Phase 4 takes a *stream of evaluated alphas*, **stores** them,
**gates** them on orthogonality/turnover/fitness, **combines** the survivors into one unified target
book, builds a **Barra-style factor risk model**, and sizes the book with a **turnover-penalized,
risk-aware optimizer** — then feeds the result back through the *existing* Phase-2 loop as just another
`ISignalSource`. This is the layer that turns *"many faint, mostly-uncorrelated signals"* into
*"one extraordinary aggregate"* — the `IR ≈ IC·√BR` breadth thesis made into code.
**Status:** frozen at kickoff (fossil — scope changes go in the ledger's *Plan adjustments*, not here).
**Amended 2026-06-06 (post-Phase-3 close):** see **§0 — Phase-3 as-built reconciliation** below. The frozen
design §1–§11 is unchanged; §0 records what Phase 3b/3c actually shipped (`AlphaStreams`/`extract_streams`,
`compile_batch`+CSE telemetry, `VmSignalSource`, the `Delay` knob) and how the P4 units reconcile. **Where §0
and §1–§11 disagree, §0 wins.**
**Authority:** [`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md). Quality bar:
[`../docs/implementation-quality.md`](../docs/implementation-quality.md).
**Read first:** [`engine-architecture-audit.md`](engine-architecture-audit.md) §1 (the weak-signal thesis —
this phase **is** the combination step it exists to serve), §3 (invariants: determinism, no look-ahead,
no data-snooping), and §4 (the cost/risk mandates: √-impact, Barra `V = XFXᵀ + D`, singular-SCM
combination).
**Grounding:** every design choice below is sourced in the verified deep-dive research:
- [`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md)
  §4.4 (fitness + correlation gates), §6.1–6.3 (bounded-regression / O(N) / dead-alpha-risk combiners),
  §6.4 (neutralization), §6.5 (turnover↔cost), §9.4 (the `WeightPolicy` extension), §9.5 (metrics),
  §9.7 (the combination + risk layer).
- [`../../research/renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md)
  §9.3 (signal combination is the product), §9.4 (out-of-sample discipline enforced by the harness),
  §9.6 (report capacity, not just return).

Section cites below point at those reports; the metric/combiner/risk formulas are reproduced in
Appendix A and the source index in Appendix B.
**Roadmap position:** this realizes the [`ROADMAP.md`](ROADMAP.md) **Phase 4** sketch ("Alpha pool gates +
signal combination + risk"). It **receives** the alpha **store** and **correlation/turnover orthogonality
gates** that moved here from the old Phase-3 sketch. Phase 3 stops at *source → signal vector*; Phase 4 is
the layer above it: *pool → gated pool → mega-alpha → risk-aware book*.
**Upstream (atx-core):** **all required layers have landed** as of the Phase-3 base — L7 `linalg`
(`regression` ols/ridge/wls, `pca`, `spd`, `solve`, `decompose`), L6 `stats/cross_section`, L9
`series/frame`, L3/L2/L1/L0. Phase 4 is therefore **not** Pattern-B blocked-on anything (unlike Phases 1/3
at their kickoff) — the entire sprint targets fully green. See §6.

**Sprint split (decided at design):** Phase 4 is **two sprints** because the ROADMAP scope is ~11 units
(> the 4–7/sprint guidance in [`../docs/sprint.md`](../docs/sprint.md)), matching the `p0` 4/4b/4c
precedent:

- **Sprint 4a — alpha pool + gates + combiner** (P4-0 … P4-5): the mega-alpha, gross-of-risk-model.
- **Sprint 4b — factor risk model + risk-aware construction** (P4-6 … P4-10): the Barra layer + optimizer.

Each sprint opens its own ledger (`phase-4a-progress.md`, `phase-4b-progress.md`) at its own kickoff; this
plan is the frozen design for both.

---

## 0. Phase-3 as-built reconciliation (amended 2026-06-06 — post-Phase-3 close)

> The frozen design (§1–§11) is **unchanged**. This block records what Phase 3b/3c **actually shipped** and how
> the frozen P4 units reconcile against it — the role a ledger's *Plan adjustments* section plays, written here
> because Phase 4's ledgers don't open until kickoff. Phase 3 (3b + 3c) closed at `bf4cbfb`; sprint-3c shipped
> `compile_batch`, cross-alpha CSE telemetry, `AlphaStreams`/`extract_streams`, `VmSignalSource`, and the
> `Delay` knob — surfaces this plan was frozen *before* and therefore only assumed. **Where §0 and §1–§11
> disagree, §0 wins.** All signatures below are verified against the as-built headers.

**(A) The pool's storage substrate AND its producer already exist — `alpha::AlphaStreams` + `extract_streams`
(P3c-2).** P4-1's `AlphaStore` re-specs dense `pnl_[n_alphas×n_periods]` + `pos_[n_alphas×n_periods×n_instruments]`
matrices. Those are `AlphaStreams` verbatim (`alpha/streams.hpp`): `pnl(a) → span<const f64>` (length
`n_periods()`), `positions(a, t) → span<const f64>` (length `n_instruments()`), `n_alphas()/n_periods()/n_instruments()`,
with `n_periods() == panel.dates()`. The producer is
`extract_streams(const SignalSet&, const WeightPolicy&, const alpha::Panel&, const exec::ExecutionSimulator&) → Result<AlphaStreams>`.
So **P4-1 wraps `AlphaStreams`** (adds stable `AlphaId`s + per-alpha `AlphaMetrics` + a re-evaluable source
handle) rather than rebuilding the matrices, and the §1 ingestion loop ("run each alpha once through the loop →
PnL + positions") collapses to a single `extract_streams(...)` call over a batch `SignalSet`.

**(B) Batch evaluation + cross-alpha CSE is the as-built default, not a per-alpha loop (P3c-1).**
`compile_batch(std::span<const std::string_view>, const Library&) → Result<Program>` folds N named alphas into
ONE hash-consed DAG; one `Engine::evaluate(Program) → SignalSet` returns N alphas × dates × instruments;
telemetry rides on `Program` (`unique_nodes`, `total_ast_nodes`, `cache_hits`, `intern_attempts`,
`cache_hit_pct()`; invariant `intern_attempts == cache_hits + unique_nodes`). The combiner's input pool is
therefore naturally **one batch `SignalSet` → `extract_streams`**, and constituents that share subexpressions
are computed **once** (the CSE lever P3c-1 measured at ~74% node-fold / 73.7% cache-hit on a mined 24-alpha
battery). §1's "each run once through the Phase-2 loop" mental model is superseded by this single-pass path.

**(C) `CombinedSignalSource` (P4-5) mirrors `VmSignalSource` (P3c-3); its PRODUCTION form is ONE batch Program,
not N sources.** The as-built `ISignalSource` is exactly what P4-5 assumes —
`evaluate(PanelView) → Result<SignalView>`, `max_lookback() const noexcept`, a non-owning `SignalView` borrowing
the source's own buffer. Two implementations: **(test)** the plan's `std::vector<ISignalSource*>` over
`ScriptedSignalSource` constituents — keep it for known-value blend tests; **(production)** a single
`compile_batch` Program inside a `VmSignalSource`-style adapter that evaluates *all* constituents in one
CSE-shared VM pass and blends the `SignalSet`'s N alpha rows by the frozen `Combination` weights — strictly
better than N separate sources (no N× `alpha::Panel`/`Engine` rebuilds; CSE reuse). `max_lookback()` forwards
`Program::required_lookback` (a `u16` **field**, not a `Program::max_lookback()` method — reconcile P4-5's
wording). **Alloc residual (carry into the DoD):** the as-built `alpha::Engine` binds its `Panel` at
construction (no `rebind`), so each `evaluate()` builds a fresh `Panel`+`Engine`; the transpose/extract are
steady-state alloc-free but the per-call Panel-column copy + Engine pool allocate. The §11 DoD line "zero
steady-state allocation on the apply path" is therefore **scoped to the transpose/extract** for the VM path,
not the whole call, exactly as P3c-3 documented; `evaluate()` runs at rebalance cadence (not per-bar), so this
is a documented residual, not a hot-path regression. A future `alpha::Engine::rebind(Panel&)` would close it.

**(D) The delay knob is `loop::Delay{Same,Next}` (P3c-3), default `Next`.** §3.1's "(or ≥ t₁ under delay-0)" is
`Delay::Same`. Default `Delay::Next` keeps the loop's no-look-ahead firewall structurally intact (single
pre-queue settle); `Delay::Same` adds a dedicated post-queue `settle_at(t)` gated entirely by the delay flag.
Phase 4's fit/apply firewall (§3.1) **composes with** — does not replace — this loop-level delay firewall.

**(E) Metrics + combiner fit on the CONSTANT-WEIGHT analytic stream, not the loop's fixed-share book equity
(P3c-2 residual).** `extract_streams` pnl is `Σ_j w_j[t-1]·ret_j[t] − turnover·cost_rate`, rebased to constant
weights each period; the `BacktestLoop` holds an integer-share book and divides dollar PnL by *drifted* equity.
They agree exactly only against an undrifted base (proven in P3c-2's anchor test). §5.1's "its own
dollar-neutral book net-return stream" **is** `AlphaStreams::pnl(a)` — that constant-weight stream, consumed by
P4-2/P4-4 **by contract**. A sized backtest's realized equity is the loop's, not the stream's.

**(F) `pnl[0] = 0` is a STRUCTURAL zero (P3c-2).** Period 0 has no prior weight or prior close, so
`extract_streams` writes `pnl[..][0] = 0`. P4-2's Sharpe/vol must treat period 0 as structural (exclude it, or
document that it biases mean/vol downward) — a real as-built gotcha §5.1 cannot see. `instrument_return` also
returns 0 for a NaN / non-positive prior close, so not-yet-listed cells contribute 0, not NaN.

**(G) One cost surface, two fidelities (P3c-2).** The stream charges only the linear PerDollar coefficient
(`ExecutionSimulator::commission_cfg().per_dollar_bps / 1e4`); PerShare commission, slippage, and √-impact are
share-/participation-scaled with no closed per-turnover form and are **not** in the stream (documented
residual; the frictionless stream is bit-exact to a direct loop run). P4-9's turnover penalty `κ` and P4-10's
capacity √-impact must read the **same** `ExecutionSimulator` — the capacity curve uses the FULL sim
(authoritative for sizing), the per-alpha stream uses the linear approximation. Do **not** introduce a second
cost path.

**(H) Group-neutralization PARTIALLY exists in the DSL (P3b-2).** `indneutralize` / `group_neutralize` lowers to
`CsDemeanG` (per-group demean), a registered alpha op — group-neutralization is already expressible *inside* an
alpha expression, and the `IndClass` group dtype already flows through Phase 3. P4-8's `WeightPolicy`
group-demean is for **post-combination** neutralization; it reuses that same per-group-demean semantics + group
map rather than building demeaning from scratch. (Factor-neutralize, decay, truncation in P4-8 are still
net-new.)

**(I) Phase 3 is CLOSED — the P4-5 cross-phase edge is RESOLVED.** §6, the P4-5 note, and the §10 watch-item
describe `VmSignalSource` as "in flight / green-gates when Phase 3 closes." It has closed (`bf4cbfb`) and
`VmSignalSource` is proven driving the real `BacktestLoop` (P3c-4 bridge E2E: byte-identical replay, costs-off
frictionless recovery, both `Delay` modes). Sprint 4a can therefore wire **real `VmSignalSource` constituents
from day one**; `ScriptedSignalSource` remains the unit-test double, not the only available path.

**Net effect on the unit plan** (the frozen specs stay; these are the as-built reconciliations):

| Unit | Reconciliation |
|---|---|
| P4-1 `AlphaStore` | Wrap `alpha::AlphaStreams` + ids/metrics/source-ref; don't rebuild the dense matrices (delta A). Smaller unit. |
| P4-2 metrics | Compute over `AlphaStreams::pnl(a)`; treat `pnl[0]=0` as structural (delta F). |
| P4-3 gate | Unchanged; correlations run over the `AlphaStreams` pnl rows (pairwise-complete). |
| P4-4 combiner | Input pool = batch `SignalSet` → `extract_streams`; fit on the constant-weight stream (deltas B, E). |
| P4-5 `CombinedSignalSource` | Production = single `compile_batch` Program adapter (delta C); `vector<ISignalSource*>` form stays for tests; `required_lookback` is a field. |
| P4-8 neutralize | Group-demean reuses the DSL `CsDemeanG` semantics + `IndClass` map (delta H). |
| P4-9 / P4-10 | Reconcile `κ` / capacity to the one `ExecutionSimulator` cost surface (delta G). |
| §11 DoD | "Zero steady-state apply-path alloc" scoped to transpose/extract for the VM path (delta C). |

The handoff block (§9) should add `alpha/streams.hpp`, `alpha/bytecode.hpp` (`compile_batch` + `Program`
telemetry), and `alpha/vm.hpp` to its as-built-header reference list, and name `VmSignalSource` as the P4-5
production template.

---

## 1. What this sprint delivers

The combination + risk layer that sits **above** the Phase-3 DSL and **feeds** the Phase-2 loop. After
Phase 4, a caller can do:

```cpp
namespace combine = atx::engine::combine;
namespace risk    = atx::engine::risk;

// 1. A pool of evaluated alphas (each already run through the Phase-2 loop once -> PnL + positions).
combine::AlphaStore pool;
for (const auto& [signal_src, pnl, positions] : evaluated_alphas) {
  const combine::AlphaMetrics m = combine::compute_metrics(pnl, positions, book_size);   // P4-2
  if (gate.admit(m, pnl, pool) == combine::GateVerdict::Accept) {                         // P4-3
    pool.insert(signal_src_id, pnl, positions, m);
  }
}

// 2. Fit combination weights over the gated pool on a TRAILING (in-sample) window.
combine::AlphaCombiner combiner{cfg};                                                     // P4-4
ATX_TRY(const combine::Combination w, combiner.fit(pool, fit_window));

// 3. The mega-alpha is just another ISignalSource -> drops into the existing BacktestLoop.
combine::CombinedSignalSource mega{pool.sources(), w};                                    // P4-5
// ... loop.run(mega, ...) exactly as it ran VmSignalSource — no loop change.

// 4. (4b) A Barra-style factor risk model, fitted on a trailing window, kept FACTORED (never M x M).
risk::FactorModelBuilder builder{fm_cfg};                                                 // P4-6/7
ATX_TRY(const risk::FactorModel V, builder.build(panel, t, risk_window));

// 5. (4b) Size the book with a turnover-penalized, risk-aware optimizer instead of plain scale().
risk::PortfolioOptimizer opt{opt_cfg};                                                    // P4-9
ATX_TRY(const std::vector<f64> weights, opt.solve(mega_signal, V, w_prev, universe));
```

Concretely the phase ships:

**Sprint 4a:**
1. An **alpha store** (`combine::AlphaStore`) — an append-only, deterministically-keyed pool of evaluated
   alphas, each carrying its realized **PnL stream**, **position (weight) stream**, and a reference to its
   `ISignalSource` so the combiner can re-evaluate it point-in-time.
2. **Per-alpha metrics** (`combine::AlphaMetrics`) — Sharpe, turnover, returns, drawdown, margin,
   **fitness**, holding period — adopting WorldQuant's published `fitness` formula verbatim so results are
   comparable to BRAIN intuition.
3. **Orthogonality / quality gates** (`combine::AlphaGate`) — admit an alpha only if it clears
   Sharpe/fitness/turnover floors **and** is sufficiently *diversifying* (low correlation-to-pool). This is
   the operational core of the millions-of-weak-alphas thesis: a new alpha's value is its *diversifying
   contribution*, not its standalone Sharpe.
4. A **progressive combiner** (`combine::AlphaCombiner`) — equal-weight → rank-average → IC/Sharpe-weighted
   → Ledoit-Wolf shrinkage mean-variance → bounded/ridge regression over SCM principal components. Each rung
   handles the singular-SCM (N alphas ≫ T observations) obstacle more aggressively; the registry of methods
   makes the rung a config knob.
5. A **`CombinedSignalSource`** (`ISignalSource`) — wraps the pool + frozen combination weights so the
   mega-alpha runs in the *existing* `BacktestLoop` with **zero loop changes** (the seam the Phase-2
   `signal_source.hpp` header explicitly anticipated: *"Phase-4's mega-alpha combiner will plug in the same
   way"*).

**Sprint 4b:**
6. A **factor exposure matrix** (`risk::FactorModel::X`) — sector dummies (`IndClass`) + **price-derived**
   style factors (size, momentum, volatility, beta, liquidity). **No fundamental factors** (value/quality
   need fundamentals — out of the price-volume anti-roadmap).
7. **Factor-return estimation + covariance** — per-date cross-sectional WLS `r = Xf + u`; specific variance
   `D` from the residuals `u`; shrunk factor covariance `F`; the risk model `V = XFXᵀ + D` kept **factored**
   (never materialize the M×M matrix). Optional **PCA statistical factors** and optional **endogenous
   dead-alpha risk directions**.
8. **Neutralization in `WeightPolicy`** — wire the already-inert `industry_neutral` flag to group-demean;
   add **factor-neutralization** (residualize the signal on `X`); add **decay** and **truncation** stages
   (the two stages the research flags as missing from the current pipeline).
9. A **turnover-penalized, risk-aware optimizer** (`risk::PortfolioOptimizer`) — maximize
   `αᵀw − λ·wᵀVw − κ·‖w − w_prev‖₁` subject to dollar-neutrality, gross-leverage, and per-name caps. The
   factored `V` makes `V⁻¹` cheap via Woodbury; caps + L1 turnover solved by a fixed-iteration deterministic
   solver.
10. A **capacity curve** output — for any book, the AUM at which modeled √-impact erodes the edge to zero
    (RenTech §9.6: *report capacity, not just return*).
11. The **walk-forward / out-of-sample harness rail** — combiner weights, gate stats, and the factor model
    are *fitted* objects; this phase proves they are fitted on **trailing data only** and applied
    out-of-sample (truncation-invariance), establishing the IS/OOS split that Phase 5 deepens into
    deflated-Sharpe.

**Not** in this phase (deliberate, per the ROADMAP anti-roadmap and the Phase-5 boundary):
- No cost **calibration** (fitting `δ`/`Y`/`γ`/spread/latency to realized fills) — Phase 5. Phase 4 *uses*
  the Phase-2 cost model with documented defaults.
- No **deflated-Sharpe / multiple-testing correction** — Phase 5. Phase 4 ships the IS/OOS split + the
  truncation proof; Phase 5 deflates.
- No **ML / autoML combiner** — the research warns ML overfits at this N; the linear/shrinkage progression
  is the proven baseline (audit §1; ROADMAP anti-roadmap #5).
- No **fundamental factors** (value, quality, earnings) — needs alternative data (anti-roadmap #3).

**Maps to research:** WorldQuant deep-dive §4.4 (gates), §6.1–6.5 (combiners + neutralization + turnover),
§9.4/9.5/9.7 (the extension + metrics + combination/risk layer); RenTech deep-dive §9.3 (combination is the
product), §9.4 (OOS discipline), §9.6 (capacity); audit §1/§3/§4.

---

## 2. Reference architecture (research- and code-grounded)

Phase 4 is a **fitting-and-application** layer wrapped around the existing loop. The canonical flow:

```
  Phase-3 DSL                                       ATX COMBINATION + RISK LAYER (Phase 4)
  ┌──────────────────────┐
  │ N compiled alphas     │ ── each run once ──▶  per-alpha PnL stream + position stream + signal
  │ (alpha::Program ×N)   │   through Phase-2 loop                 │
  └──────────────────────┘                                        ▼
                                              ┌──────── compute_metrics (P4-2) ──────┐
                                              │ sharpe·turnover·fitness·holding·dd     │ (§5.1)
                                              └───────────────────┬───────────────────┘
                                                                  ▼
                                              ┌──────── AlphaGate.admit (P4-3) ───────┐
   reject: low Sharpe/fitness, high turnover, │  Accept iff  fitness ≥ φ  AND          │ (§5.2)
   too correlated with the accepted pool  ◀───│  corr_to_pool(candidate, pool) ≤ ρ     │
                                              └───────────────────┬───────────────────┘
                                                          accepted ▼
                                              ┌──────── AlphaStore (P4-1) ────────────┐
   append-only, stable ids, owns the          │ AlphaRecord[]: pnl·positions·metrics·  │
   PnL/position matrices for the combiner      │ source-ref (re-evaluable PIT)          │
                                              └───────────────────┬───────────────────┘
                                                                  ▼
                                              ┌──── AlphaCombiner.fit (P4-4) ─────────┐
   fit on a TRAILING window (IS), apply OOS    │ eq-wt → rank-avg → IC/Sharpe-wt →      │ (§5.3)
   singular SCM (N≫T) -> shrinkage / PCs        │ Ledoit-Wolf MV → bounded regression    │
                                              └───────────────────┬───────────────────┘
                                                          weights ▼
                                              ┌──── CombinedSignalSource (P4-5) ──────┐
   the mega-alpha is "just another             │ : ISignalSource  evaluate(PanelView)   │
   ISignalSource" -> existing loop unchanged   │   = Σ_i w_i · alpha_i(panel)            │
                                              └───────────────────┬───────────────────┘
                                                                  ▼   (combined cross-sectional signal)
  ════════════════════════ 4a above · 4b below ══════════════════════════════════════════════
                                                                  │
                          ┌──── FactorModel (P4-6/7) ─────────────┤  V = X F Xᵀ + D  (kept factored)
   X = [sector | size momentum vol beta liq]   │ per-date WLS r=Xf+u → f,u → F,D         │ (§5.4)
   fit on trailing window; PIT only            └───────────────────┬───────────────────┘
                                                                  ▼
                          ┌── WeightPolicy ext. (P4-8) ───────────┐   group/factor neutralize
   winsorize→transform→[NEUTRALIZE]→[DECAY]→    │ residualize signal on X; demean in group│ (§5.5)
   dollar-neutral→[TRUNCATE]→gross-normalize    └───────────────────┬───────────────────┘
                                                                  ▼
                          ┌── PortfolioOptimizer (P4-9) ──────────┐   max αᵀw − λ wᵀVw − κ‖w−w_prev‖₁
   risk-aware sizing replaces plain scale();    │ Woodbury V⁻¹; caps + L1 turnover;       │ (§5.6)
   Σw=0, Σ|w|≤L, |w_i|≤cap                       │ deterministic fixed-iteration solve     │
                                              └───────────────────┬───────────────────┘
                                                          weights ▼
                                              ──▶ Phase-2 ExecutionSimulator → Portfolio (unchanged)
                                                  + capacity curve (P4-10): AUM where impact kills edge
```

Each subsystem maps to a proven reference:

| ATX component | Reference (research / open-source) | What we borrow |
|---|---|---|
| `AlphaStore` / alpha pool | WorldQuant "Alpha Factory" / BRAIN alpha pool (WQ §3, §9.7) | a managed library of evaluated alphas, screened before admission to the mega-alpha |
| Fitness metric | WorldQuant BRAIN `fitness = sqrt(abs(returns)/max(turnover,0.125))·sharpe` (WQ §4.4) | adopt verbatim so users compare to BRAIN intuition; turnover priced as cost drag, not return-killer (WQ §6.5) |
| Correlation gate | WorldQuant submission gate "low correlation with the existing alpha pool" (WQ §4.4); RenTech "prized non-obvious, diversifying signals" (RT §9.3) | admit on *marginal diversifying contribution*, not standalone Sharpe |
| Combiner | Kakushadze "Combining Alphas via Bounded Regression" (arXiv:1501.05381) + "How to Combine a Billion Alphas" (arXiv:1603.05937) (WQ §6.1–6.2); RenTech shrinkage progression (RT §9.3) | singular SCM ⇒ shrinkage / PCs / bounded weights; progression rank-avg → shrinkage → regression |
| `CombinedSignalSource` | The Phase-2 `ISignalSource` seam (`signal_source.hpp`) | the mega-alpha is just another signal source; loop is unchanged |
| Factor risk model | Barra `r = Xf + u`, `V = XFXᵀ + D` (audit §4); WorldQuant neutralization (WQ §6.4) | factored covariance; cross-sectional WLS for factor returns; never materialize M×M |
| Dead-alpha risk directions | Kakushadze & Yu "Dead Alphas as Risk Factors" (arXiv:1709.06641) (WQ §6.3) | flatlined alphas encode directions with "no money to be made" — neutralize against them (optional) |
| Turnover-penalized optimizer | mean-variance + L1 turnover penalty (ROADMAP); RenTech cost-throttled rebalancing (RT §9.2/9.3) | trade only when expected edge clears modeled round-trip cost |
| Capacity curve | RenTech capacity-bounded edge (RT §9.6; audit §4) | report the AUM at which √-impact erodes the edge to zero |
| Numerics | atx-core L7 `regression` (ols/ridge/wls), `pca`, `spd`, `solve`; L6 `cross_section`; L9 `series` | the linear-algebra kernels; no new general-purpose math in the engine |

> **Build-order note.** The combination layer (4a) ships before the risk model (4b) because the mega-alpha
> is a runnable, testable deliverable on its own (gross of the factor risk model) — it earns the first
> end-to-end *combined* backtest. The risk model then upgrades sizing from `scale()` to a risk-aware
> optimizer. 4a is correct and useful without 4b; 4b is a quality upgrade, not a prerequisite.

---

## 3. Performance & correctness model (build this in from unit 1)

> Phase 4 is **fit-then-apply**, and the dominant risk is **look-ahead via fitting**. A combiner weight, a
> gate statistic, and a factor covariance are all estimated from data; if that data includes the future, the
> backtest lies — and it lies *invisibly*, because the per-alpha look-ahead rails (Phase 1/3) don't see the
> combiner. So the central correctness rail of this phase is **fit-window discipline**, designed in from
> P4-1.

### 3.1 The fit/apply firewall (the non-negotiable new invariant)

- Every fitted object (`Combination`, `AlphaMetrics` used by a gate, `FactorModel`) is parameterized by an
  **explicit fit window `[t₀, t₁]`** and is only ever *applied* at dates `> t₁` (or `≥ t₁` under delay-0). A
  fit function **must not read** any panel/PnL row beyond its window — enforced by passing it a *truncated
  view*, never the full history.
- **Walk-forward is the application pattern**: re-fit on a rolling trailing window, apply forward until the
  next re-fit. The harness (P4-10) drives this; the fitted types just carry their window and refuse to be
  applied inside it (`ATX_ASSERT(apply_date > fit_window.end)`).
- **Proof (P4-10):** truncate the panel/PnL after date `t` and re-run; every output for dates `≤ t` is
  byte-identical (future rows provably invisible) — the same truncation-invariance test Phases 1–3 use,
  extended to the combiner + risk model.

### 3.2 Determinism (audit §3 invariant — extended to fitting)

- **No RNG anywhere.** PCA/eigen, shrinkage, and the optimizer are deterministic; any iterative solver runs
  a **fixed iteration count** in a **fixed order** (no convergence-dependent early exit that could vary).
- **Order-fixed reductions.** Covariance/regression sums run in canonical instrument/alpha-id order with a
  fixed scheme (the Phase-3 rule), so a result is bit-reproducible regardless of threading.
- **Stable tie-breaking.** Rank-average combination and any sort reuse the Phase-3 `CsRank` convention
  (ordinal + stable id, NaN-last).
- Proven by P4-10's repeat-run + mutation-detecting hash, identical to the Phase-1/2/3 harnesses.

### 3.3 NaN / missing-data policy (consistent with Phases 2–3)

- **Alpha PnL/position streams** may have NaN gaps (an alpha with no opinion that day, or before its first
  valid date). Metrics, correlations, and the combiner use a **pairwise-complete** policy (a date is used
  for a pair only if both are valid), documented once and shared everywhere.
- **Factor model:** an instrument missing an exposure (e.g. no `cap`) is **dropped from that date's
  cross-sectional regression** (not imputed) and gets specific-variance-only risk; its signal is NaN ("no
  opinion") downstream, matching the `WeightPolicy` convention.
- **NaN = "no opinion" ⇒ weight 0** is preserved end-to-end (the existing `WeightPolicy`/`SignalView`
  contract).

### 3.4 No hot-path allocation, but fitting is a cold path

- The **fit** functions (combiner, factor model, gates) run at **re-fit cadence** (rare — monthly/quarterly
  in walk-forward), so they **may allocate** (scratch matrices, eigen workspaces). They are the cold path,
  like Phase-3 parsing/compiling.
- The **apply** path — `CombinedSignalSource::evaluate`, `WeightPolicy`, the optimizer's per-rebalance solve
  — runs at rebalance cadence and follows the existing "allocate-once-per-rebalance is acceptable"
  `WeightPolicy` precedent (caller-provided-scratch overloads are a tracked residual, as in Phase 2). The
  factored `V` is never materialized M×M; the optimizer works in factor space (K ≪ M).

### 3.5 Performance budgets (record measured; no invented figures)

| Path | Budget (intent) | Notes |
|---|---|---|
| `compute_metrics` per alpha | O(T) over the PnL stream | one pass; Welford for Sharpe |
| `corr_to_pool` candidate vs pool | O(\|pool\|·T) | pairwise-complete Pearson; the gate's cost |
| combiner `fit` (shrinkage MV) | O(N²T + N³) worst case; O(N·k·T) over k PCs | report which method + the N,T,k used |
| factor `build` | O(M·K·D) regression + O(K²·D) cov | per-date WLS over the fit window; K ≪ M |
| `V⁻¹` (Woodbury) | O(M·K + K³) | never O(M³); factored form is the whole point |
| optimizer `solve` per rebalance | O(iters·(M·K + M)) | fixed iters; factor-space risk term |

Benches report measured numbers with host/build/panel-shape context only — never unverified third-party
figures (audit §5 warning).

---

## 4. Data model (the types Phase 4 introduces)

Two new namespaces. `combine` is sprint 4a; `risk` is sprint 4b. Neutralization extends the existing
`loop::WeightPolicy`.

```cpp
namespace atx::engine::combine {

// Stable, insertion-ordered identity. NOT a pointer — survives store growth.
struct AlphaId { atx::u32 value; };

// Per-alpha realized performance summary (P4-2). All f64; NaN where undefined
// (e.g. fitness when turnover and returns are both ~0).
struct AlphaMetrics {
  atx::f64 sharpe;        // annualized: sqrt(252) * mean(pnl) / std(pnl)
  atx::f64 turnover;      // mean daily dollar-traded / book size
  atx::f64 returns;       // annualized mean periodic return
  atx::f64 drawdown;      // max peak-to-trough drawdown (fraction in [0,1])
  atx::f64 margin;        // return per dollar traded (bps) = returns / max(turnover, eps)
  atx::f64 fitness;       // sqrt(abs(returns) / max(turnover, 0.125)) * sharpe  (WQ §4.4)
  atx::f64 holding_days;  // ~ 1 / turnover
};

// One evaluated alpha in the pool (P4-1). Owns its realized streams so the
// combiner can compute covariances without re-running the loop; keeps a source
// handle so the CombinedSignalSource (P4-5) can re-evaluate it point-in-time.
struct AlphaRecord {
  AlphaId               id;
  AlphaMetrics          metrics;
  // pnl_[t]   : net periodic return of THIS alpha's own dollar-neutral book at rebalance t
  // (positions stored separately in the store's flat matrix; see AlphaStore)
};

// Append-only pool. Deterministic ids (insertion order). Owns two dense
// row-major matrices: pnl_ (n_alphas x n_periods) and pos_ (n_alphas x
// n_periods x n_instruments, optional, for position-based combination/crossing).
class AlphaStore {
 public:
  // Insert an admitted alpha; returns its stable id. `pnl` length == n_periods.
  AlphaId insert(atx::core::Result<...> source_ref, std::span<const atx::f64> pnl,
                 std::span<const atx::f64> positions_flat, AlphaMetrics) ;        // cold path
  [[nodiscard]] atx::usize size() const noexcept;
  [[nodiscard]] const AlphaRecord& get(AlphaId) const noexcept;
  // T x N view (period-major) of the pool PnL for the combiner's SCM.
  [[nodiscard]] std::span<const atx::f64> pnl_matrix() const noexcept;            // [n_periods*n_alphas]
  [[nodiscard]] atx::usize n_periods() const noexcept;
};

// Gate configuration + verdict (P4-3). All floors are config knobs (Phase-5 calibrates).
struct GateConfig {
  atx::f64 min_sharpe   = 1.0;     // standalone-Sharpe floor
  atx::f64 min_fitness  = 1.0;     // BRAIN "gold standard for submission" (WQ §4.4)
  atx::f64 max_turnover = 0.70;    // generous default; cost-gate, not a return claim (WQ §6.5)
  atx::f64 max_pool_corr = 0.7;    // reject if too correlated with an accepted alpha
};
enum class GateVerdict : atx::u8 { Accept, RejectSharpe, RejectFitness, RejectTurnover, RejectCorrelated };

struct AlphaGate {
  GateConfig cfg;
  // Admit iff metrics clear the floors AND max pairwise corr-to-pool <= max_pool_corr.
  // `candidate_pnl` is the candidate's PnL stream (pairwise-complete vs each pool member).
  [[nodiscard]] GateVerdict admit(const AlphaMetrics&, std::span<const atx::f64> candidate_pnl,
                                  const AlphaStore& pool) const;                  // exhaustive switch on verdict
};

// Combiner (P4-4). Closed taxonomy of methods; switch is exhaustive, no default.
enum class CombineMethod : atx::u8 { EqualWeight, RankAverage, IcWeighted, ShrinkageMv, BoundedRegression };

struct CombinerConfig {
  CombineMethod method        = CombineMethod::ShrinkageMv;
  atx::f64      shrinkage      = -1.0;  // Ledoit-Wolf if < 0 (auto), else fixed intensity in [0,1]
  atx::f64      weight_bound   = 0.10;  // per-alpha |weight| cap (BoundedRegression)
  atx::f64      ridge_lambda   = 1e-3;  // regression regularization
  atx::usize    n_pcs          = 0;     // 0 => use all; else regress over top-k SCM PCs (N>>T)
};

// Frozen combination weights + the fit window they belong to (the fit/apply firewall, §3.1).
struct Combination {
  std::vector<atx::f64> weights;   // length == pool size; per-alpha blend weight
  atx::usize            fit_begin; // [fit_begin, fit_end) periods used to fit (apply only AFTER fit_end)
  atx::usize            fit_end;
};

class AlphaCombiner {
 public:
  CombinerConfig cfg;
  // Fit blend weights over the pool using ONLY periods in [fit_begin, fit_end). Cold path; may allocate.
  [[nodiscard]] atx::core::Result<Combination>
  fit(const AlphaStore& pool, atx::usize fit_begin, atx::usize fit_end) const;
};

} // namespace atx::engine::combine
```

```cpp
namespace atx::engine::risk {

// Style factors are price/volume-derivable ONLY (no fundamentals; anti-roadmap #3).
enum class StyleFactor : atx::u8 { Size, Momentum, Volatility, Beta, Liquidity };

struct FactorModelConfig {
  bool       sector_factors    = true;   // one dummy column per IndClass group
  atx::u8    style_mask        = 0x1F;   // bitset over StyleFactor (default: all 5)
  atx::usize n_stat_factors    = 0;      // optional PCA statistical factors (0 = none)
  atx::usize n_dead_factors    = 0;      // optional dead-alpha risk directions (0 = none) (WQ §6.3)
  atx::f64   factor_cov_shrink = -1.0;   // Ledoit-Wolf if < 0
};

// V = X F Xᵀ + D, kept FACTORED. Never materializes the M x M matrix.
//   X : M x K  exposures (M instruments, K factors)
//   F : K x K  factor covariance
//   D : M      specific (idiosyncratic) variances (diagonal)
class FactorModel {
 public:
  [[nodiscard]] atx::usize n_factors() const noexcept;       // K
  [[nodiscard]] atx::usize n_instruments() const noexcept;   // M
  // wᵀ V w  computed in factor space: (Xᵀw)ᵀ F (Xᵀw) + Σ d_i w_i²   (O(MK + K²), never O(M²))
  [[nodiscard]] atx::f64 risk(std::span<const atx::f64> w) const noexcept;
  // Residualize a signal on the factor exposures: s - X (XᵀX)⁻¹ Xᵀ s  (factor-neutralization).
  void neutralize(std::span<atx::f64> signal) const;
  // V⁻¹ applied to a vector via Woodbury: D⁻¹ - D⁻¹X(F⁻¹ + XᵀD⁻¹X)⁻¹XᵀD⁻¹  (O(MK + K³)).
  void apply_inverse(std::span<const atx::f64> in, std::span<atx::f64> out) const;
  atx::usize fit_begin, fit_end;  // the fit/apply firewall (§3.1)
};

class FactorModelBuilder {
 public:
  FactorModelConfig cfg;
  // Build X from the panel at date t (cap, trailing returns, IndClass), then estimate F,D by per-date
  // WLS r=Xf+u over [t-window, t). Cold path; may allocate. PIT: reads only rows < t.
  [[nodiscard]] atx::core::Result<FactorModel>
  build(const PanelView& panel, atx::usize t, atx::usize window) const;
};

struct OptimizerConfig {
  atx::f64 risk_aversion   = 1.0;   // lambda (penalty on wᵀVw)
  atx::f64 turnover_penalty = 0.0;  // kappa (penalty on ||w - w_prev||_1); 0 = ignore turnover
  atx::f64 gross_leverage  = 1.0;   // L (target Σ|w|)
  atx::f64 name_cap        = 1.0;   // max |w_i| (1.0 = effectively uncapped)
  bool     dollar_neutral  = true;  // Σ w = 0
  atx::usize max_iters     = 64;    // FIXED iteration count (determinism, §3.2)
};

class PortfolioOptimizer {
 public:
  OptimizerConfig cfg;
  // max αᵀw − λ wᵀVw − κ‖w − w_prev‖₁  s.t. Σw=0, Σ|w|≤L, |w_i|≤cap.
  // Deterministic fixed-iteration solve in factor space. Apply path (per rebalance).
  [[nodiscard]] atx::core::Result<std::vector<atx::f64>>
  solve(std::span<const atx::f64> alpha, const FactorModel& V,
        std::span<const atx::f64> w_prev) const;
};

// Capacity curve (P4-10): for a target book, the AUM at which √-impact erodes net edge to zero.
struct CapacityPoint { atx::f64 aum; atx::f64 net_edge_bps; };
[[nodiscard]] std::vector<CapacityPoint>
capacity_curve(std::span<const atx::f64> weights, const PanelView& panel,
               const exec::ExecutionSimulator& sim, std::span<const atx::f64> aum_grid);

} // namespace atx::engine::risk
```

---

## 5. The math (reproduced so a zero-context engineer can implement it)

### 5.1 Per-alpha metrics (P4-2) — WorldQuant §4.4/§9.5

Given an alpha's per-rebalance net-return stream `r[0..T)` (its own dollar-neutral book, gross-normalized to
`Σ|w|=1`) and per-rebalance dollar-traded fraction `u[0..T)` (= traded notional / book size):

```
mean_r  = mean(r)                              ; periodic mean return
vol_r   = std(r)                               ; periodic stdev (population)
sharpe  = sqrt(252) * mean_r / vol_r           ; annualized (paper Eq. 4)         (252 = trading days)
returns = 252 * mean_r                          ; annualized mean return
turnover = mean(u)                              ; mean daily dollar-traded / book size
margin   = returns / max(turnover, 1e-9)        ; return per dollar traded
fitness  = sqrt(abs(returns) / max(turnover, 0.125)) * sharpe                     ; WQ verbatim (§4.4)
holding_days = 1.0 / max(turnover, 1e-9)
drawdown = max over t of (peak_equity[..t] - equity[t]) / peak_equity[..t]         ; equity = cumprod(1+r)
```

NaN policy: if `vol_r == 0` (flat alpha) `sharpe = 0`; if `turnover == 0` and `returns == 0`,
`fitness = 0`. The `max(turnover, 0.125)` floor is the paper's blow-up guard — keep it verbatim.

### 5.2 Correlation-to-pool gate (P4-3) — WorldQuant §4.4, RenTech §9.3

A candidate alpha's PnL stream `c[0..T)` against each accepted member `m_j[0..T)`:

```
corr(c, m_j) = pairwise-complete Pearson over dates where BOTH are non-NaN
corr_to_pool = max_j |corr(c, m_j)|            ; "max" = strictest; "avg" available as a knob
admit iff:  metrics.sharpe  >= cfg.min_sharpe
        AND metrics.fitness >= cfg.min_fitness
        AND metrics.turnover <= cfg.max_turnover
        AND corr_to_pool     <= cfg.max_pool_corr   ; the diversification gate
```

The first failing condition determines the `GateVerdict` (checked in a fixed order, so the verdict is
deterministic). Empty pool ⇒ `corr_to_pool = 0` (first alpha always passes the correlation gate).

### 5.3 The combiner (P4-4) — progressive, handles singular SCM (WorldQuant §6.1–6.2, RenTech §9.3)

Let `R` be the pool's `T × N` net-return matrix (T periods in the fit window, N alphas). The combiner emits
per-alpha blend weights `w ∈ ℝᴺ`. Five rungs, increasingly aggressive about the **singular SCM** problem
(when `N ≫ T`, the sample covariance `Σ = cov(R)` is rank-deficient and not invertible):

1. **EqualWeight:** `w_i = 1/N`. The baseline; always valid.
2. **RankAverage:** convert each alpha's *signal* to a cross-sectional rank per date and average the ranks
   (this combines *signals*, not returns; most robust; WorldQuant's intuition). Blend weight is uniform but
   the combination happens in rank space (handled by `CombinedSignalSource`, see P4-5 note).
3. **IcWeighted:** `w_i ∝ max(IC_i, 0)` where `IC_i = corr(signal_i, forward_return)` over the fit window
   (or, simpler and return-only: `w_i ∝ max(sharpe_i, 0)`). Normalize `Σw = 1`. Heuristic; no matrix
   inversion.
4. **ShrinkageMv:** Markowitz over alphas — `w ∝ Σ̂⁻¹ μ`, where `μ = mean(R)` and `Σ̂` is the
   **Ledoit-Wolf-shrunk** covariance: `Σ̂ = (1−ρ)·S + ρ·(tr(S)/N)·I`, `S = cov(R)`, `ρ` the optimal
   intensity (or a fixed knob). Shrinkage makes `Σ̂` invertible even when `S` is singular. Solve via atx-core
   `linalg::spd::solve` (`Σ̂` is SPD after shrinkage). Renormalize `Σ|w| = 1`.
5. **BoundedRegression:** Kakushadze's method (arXiv:1501.05381) — regress forward returns on the alpha
   signals over the **top-k principal components** of `S` (atx-core `pca`), with **bounded weights**
   (clip `|w_i| ≤ weight_bound`, then re-project) to restore diversification and control turnover skew.
   Ridge (`ridge_lambda`) regularizes. This is the most faithful to the published WorldQuant combiner.

The default is `ShrinkageMv` (a strong, simple, invertible baseline). All five share the same `fit` signature
and the same fit-window discipline (§3.1). The method is a config knob; the switch over `CombineMethod` is
exhaustive (no `default`).

> **Position-based combination (deferred knob).** "How to Combine a Billion Alphas" combines on *holdings*
> (position vectors) not returns, enabling O(N) scaling and trade-crossing cost savings (WQ §6.2). The
> `AlphaStore` keeps the position matrix `pos_` so this is a future rung without a data-model change — listed
> in residuals, not built in 4a.

### 5.4 The factor risk model (P4-6/7) — Barra `V = XFXᵀ + D` (audit §4, WorldQuant §6.4)

**Exposures `X` (M instruments × K factors), built per date from the panel (price/volume only):**

```
sector dummies : one 0/1 column per IndClass group  (K_sector = #groups)
Size           : ln(cap)                                   , then cross-sectionally standardized (z-score)
Momentum       : trailing return, e.g. ts_sum(returns, 252) − ts_sum(returns, 21)  (12-1 month), standardized
Volatility     : trailing stddev(returns, 60)             , standardized
Beta           : slope of regress(instrument_return ~ market_return) over trailing 252d, standardized
Liquidity      : ln(adv20)                                 , standardized
```

Standardize each style column cross-sectionally (mean 0, std 1 over the valid universe) so factor returns
are comparable. Instruments missing an exposure are dropped from that date's regression (§3.3).

**Factor returns + covariance (estimated over the fit window `[t−window, t)`):**

```
for each date s in [t-window, t):
    f[s] = argmin_f  Σ_i d_i⁻¹ (r_i[s] − X[s]·f)²        ; cross-sectional WLS (weight by 1/specific var)
                                                          ; atx-core linalg::regression::wls
    u[s] = r[s] − X[s]·f[s]                               ; specific (residual) returns
F = LedoitWolf( cov( f[t-window..t) ) )                   ; K x K shrunk factor covariance (SPD)
D_i = var( u_i[t-window..t) )                             ; per-instrument specific variance (diagonal)
V   = X F Xᵀ + diag(D)                                    ; kept FACTORED — store X, F, D, never the M x M
```

**Optional rungs (config-gated, default off):**
- `n_stat_factors > 0`: append the top-`n` principal components of the return covariance as extra factor
  columns (atx-core `pca`) — captures structure the hand-built factors miss.
- `n_dead_factors > 0`: append the position vectors of *flatlined* ("dead") alphas as risk columns — the
  directions where there is "no money to be made" (WorldQuant §6.3, arXiv:1709.06641). Neutralizing against
  them removes crowded/over-arbitraged bets. **Advanced; off by default.**

`risk(w)` and `apply_inverse` use the factored form (Woodbury), never the M×M matrix — the whole point of
the factor model is dimensionality collapse (audit §4: 1,600 assets ⇒ ~4,095 numbers via ~90 factors, not
1.28M covariances).

### 5.5 Neutralization stages in `WeightPolicy` (P4-8) — WorldQuant §9.4

Extend the existing pipeline (do **not** rewrite it — the current
winsorize→transform→dollar-neutral→gross-normalize is correct and tested):

```
winsorize -> transform(rank|zscore) -> [GROUP neutralize] -> [FACTOR neutralize] -> [DECAY]
   -> dollar-neutralize (demean) -> [TRUNCATE per-name cap] -> gross-normalize (scale to Σ|w|=L)
```

- **GROUP neutralize** (wires the inert `industry_neutral`): demean the signal *within each group* (sector/
  industry/subindustry) — `indneutralize` semantics. Needs the `Group` map (an `InstrumentId → group_id`
  table); the Phase-3 `IndClass.*` group dtype is the source. The `WeightPolicy` header already documents
  this exact future: *"Remove the assert and implement group-demeaning when the group map lands."*
- **FACTOR neutralize:** residualize the signal on the factor exposures `X` (`FactorModel::neutralize`) — the
  bet becomes residual-on-factors, not just dollar-neutral (RenTech §9.3: "add factor neutralization so the
  bet is residual-on-factors").
- **DECAY:** optional `decay_linear` over the final signal (the `TsDecayLinear` kernel from the Phase-3
  registry, applied at the policy layer) — a turnover lever (BRAIN's *Decay* setting).
- **TRUNCATE:** cap `|w_i| ≤ truncation` *before* gross-normalization, then re-normalize — forces
  diversification, a cheap overfitting guard (BRAIN's *Truncation*, typ. 0.01–0.10).

All new stages are config-gated and default to no-op, so the existing Phase-2 tests stay green unchanged.

### 5.6 The turnover-penalized optimizer (P4-9) — ROADMAP, RenTech §9.3

Replace the plain `scale()` sizing (optional — `WeightPolicy` remains the default for simple strategies)
with a risk-aware book:

```
maximize_w   αᵀw  −  λ·wᵀVw  −  κ·‖w − w_prev‖₁
subject to   Σ w = 0            (dollar-neutral)
             Σ |w| ≤ L          (gross leverage)
             |w_i| ≤ cap        (per-name)
```

- `α` = the combined mega-alpha signal (expected-return proxy). `V` = the factored `FactorModel`.
- **Solve:** the equality-constrained quadratic (`λwᵀVw` with `Σw=0`) has an analytic solution using the
  factored `V⁻¹` via Woodbury (atx-core `spd`/`solve` on the small K×K systems). The inequality terms (name
  caps) and the L1 turnover penalty are handled by a **deterministic fixed-iteration** projected/proximal
  loop (`max_iters`, fixed order — §3.2): gradient step on the smooth part, soft-threshold (proximal-L1) for
  turnover, project onto `{Σw=0, Σ|w|≤L, |w_i|≤cap}`. No convergence-dependent early exit.
- `κ = 0` recovers a pure risk-aware mean-variance book; `λ = 0`, `κ = 0`, caps off recovers the
  `WeightPolicy` dollar-neutral book (a regression check).

The turnover penalty is the RenTech *cost-throttle*: the optimizer only churns a position when the expected
edge change clears the modeled round-trip cost (RT §9.2/9.3 — *"the cost-aware system simply declines the
many trades whose edge does not clear cost"*).

---

## 6. Cross-module dependencies — all landed (NOT blocked)

Unlike Phases 1 and 3 (which opened Pattern-B blocked-on unbuilt atx-core layers), **every atx-core layer
Phase 4 needs already exists** as of the Phase-3 base SHA. Phase 4 therefore targets **fully green** from
P4-0; no `atx_engine_pending` staged-red label is needed.

| Phase-4 unit | atx-core dependency | Status |
|---|---|---|
| P4-1 `AlphaStore` | L3 `fixed_vector`/`hash_map`, L2 `arena` | ✅ present |
| P4-2 metrics | L0, L6 `stats/online_stats` (Welford), `math` | ✅ present |
| P4-3 gates | L6 `stats/cross_section` (corr), L0 | ✅ present |
| P4-4 combiner | **L7 `linalg::regression` (ols/ridge/wls), `pca`, `spd`, `solve`** | ✅ present |
| P4-5 `CombinedSignalSource` | Phase-2 `ISignalSource`/`PanelView`, Phase-3 `alpha::Engine` (for re-eval) | ✅ Phase-2 **and** Phase-3 CLOSED (`bf4cbfb`); `VmSignalSource` green-gated (P3c-3) — see §0(C,I) |
| P4-6 exposures | L9 `series/frame`, L6 `stats`, `math` (ln) | ✅ present |
| P4-7 factor cov | **L7 `linalg::wls`, `pca`, `spd` (Ledoit-Wolf SPD), `decompose`** | ✅ present |
| P4-8 neutralization | L6 `cross_section` (demean), Phase-3 `TsDecayLinear`, the Group map | ✅ present (Group map from Phase-3) |
| P4-9 optimizer | **L7 `spd`/`solve` (Woodbury K×K), `math`** | ✅ present |
| P4-10 capacity/harness | Phase-2 `ExecutionSimulator`, L1 `hash` (determinism) | ✅ present |

> **The one cross-phase edge — now RESOLVED (Phase 3 closed `bf4cbfb`; see §0(I)).** P4-5's
> `CombinedSignalSource` re-evaluates constituent alphas, which in production are `VmSignalSource`s wrapping
> `alpha::Program`/`alpha::Engine` (Phase 3). That VM path is **shipped and proven** driving the real
> `BacktestLoop` (P3c-3/P3c-4), so Sprint 4a can wire **real `VmSignalSource` constituents from day one** — and
> per §0(C) the production combiner is better expressed as ONE `compile_batch` Program (CSE-shared) than as N
> separate sources. `ScriptedSignalSource` constituents remain the unit-test double (as in the Phase-2 loop
> tests), not the only available path.

---

## 7. Cross-cutting gates (agent.md — every unit)

- **TDD:** failing GoogleTest first; one behavior per `TEST`; name `Subject_Condition_ExpectedResult`; cover
  happy path, **boundaries** (empty pool, single alpha, 1-instrument universe, all-NaN PnL, T<N singular
  SCM, zero-variance alpha, all-equal exposures, `λ=κ=0` regression-to-WeightPolicy), error paths
  (size mismatch, non-SPD covariance, **fit-window violation**), invariant violations (`EXPECT_DEATH` on
  applying a fitted object inside its own fit window).
- **Differential / known-value correctness:** the combiner, factor regression, and optimizer are checked
  against **hand-computed fixtures** (e.g. two anti-correlated alphas ⇒ equal-and-opposite-risk equal
  weights; a single-factor model ⇒ analytic `V`; `λ=κ=0` optimizer ⇒ the `WeightPolicy` dollar-neutral
  weights bit-for-bit). Numeric kernels (regression, eigen) are validated against atx-core L7's own tested
  outputs, not re-derived.
- **Warnings = errors:** `/W4 /permissive- /WX` (clang-cl) / `-Wall -Wextra -Wpedantic -Wconversion
  -Wshadow -Werror` (Clang); clang-tidy (project `.clang-tidy`) + clang-format clean.
- **Sanitizers:** ASan + UBSan on every test run; TSan where the toolchain supports it (the optimizer's
  fixed-iteration loop is single-threaded by default — any parallel covariance build is written race-clean,
  join-before-assert, with the same Linux-TSan-residual caveat as Phases 1/3).
- **API discipline:** `const`/`constexpr`/`noexcept`/`[[nodiscard]]` where they hold; `Result<T>`/`Status`
  for expected failures (size mismatch, non-SPD, fit-window misuse), **never throw** for control flow;
  weakest sufficient type at interfaces (`span`/`string_view`); no owning raw pointers; Rule of Zero;
  **exhaustive `enum class` switches** over `CombineMethod`/`GateVerdict`/`StyleFactor` (no `default`);
  functions ≤ ~60 lines; `// SAFETY:` on every deviation.
- **The fit/apply firewall is structural (§3.1):** every fitted type carries its `[fit_begin, fit_end)` and
  `ATX_ASSERT`s it is applied only after `fit_end`. Fit functions receive a **truncated view**, never the
  full history — a fit function physically cannot read the future.
- **Determinism (§3.2):** no RNG; fixed iteration counts; order-fixed reductions; stable tie-breaking. The
  whole layer is byte-reproducible.
- **No hot-path allocation on the apply path** (§3.4); fit (cold path) may allocate.

---

## 8. Unit decomposition

Dispatch **sequential** (each consumes the prior's headers). TDD red→green→refactor within each. Sprint 4a
(P4-0..P4-5) ships the mega-alpha; Sprint 4b (P4-6..P4-10) ships the risk model + optimizer. None are
upstream-blocked.

---

## SPRINT 4a — Alpha pool + gates + combiner

### P4-0 — Module scaffold + CMake + ledger  *(marker commit)*

**Scope.** Create the `atx::engine::combine` tree and wire the build so new `*_test.cpp`/`*_bench.cpp` are
auto-discovered. Open the Phase-4a ledger and freeze scope. No combination logic.

**Deliverables.**
- `include/atx/engine/combine/fwd.hpp` (namespace doc + forward decls: `AlphaId`, `AlphaMetrics`,
  `AlphaRecord`, `AlphaStore`, `GateConfig`, `GateVerdict`, `AlphaGate`, `CombineMethod`, `CombinerConfig`,
  `Combination`, `AlphaCombiner`, `CombinedSignalSource`).
- `tests/`/`bench/` wiring mirroring the existing engine layout; trivial `combine_scaffold_test.cpp` proves
  GTest link + the new namespace compiles.
- `phase-4a-progress.md` ledger opened per [`../docs/sprint.md`](../docs/sprint.md) skeleton; `Base: <branch>
  @ <SHA>`.

**Acceptance.** `cmake --build` green; `combine_scaffold_test` passes; ledger marker commit
`docs(p4a-0): open phase-4a sprint ledger`.

---

### P4-1 — `AlphaStore` + `AlphaRecord` (the pool)

**Scope.** The append-only, deterministically-keyed pool that owns each evaluated alpha's PnL stream (and
optional position stream) and a re-evaluable source handle. The data substrate the gate and combiner read.

**Data structures.** As in §4 (`AlphaId`, `AlphaMetrics`, `AlphaRecord`, `AlphaStore`). PnL stored as a
dense row-major `[n_alphas × n_periods]` matrix (period-major view exposed for the combiner's SCM); ids are
insertion order (`u32`).

**Contract.** `insert` is the cold path (may allocate); returns a stable `AlphaId`. `pnl_matrix()` exposes a
`T×N` view in fixed id order (determinism). All PnL streams MUST share the same `n_periods` (asserted). An
alpha's pre-first-valid periods are NaN (handled by the pairwise-complete policy downstream, §3.3).

**Tests.** insert/size/get round-trip; stable ids across growth; `pnl_matrix` layout (a known 2×3 fixture
reads back row/col correct); period-count mismatch → `ATX_ASSERT`/`Err`; empty store. Boundary: single
alpha; all-NaN stream stored verbatim.

**Acceptance.** `/W4 /WX` clean; green. Commit `feat(p4a-1): alpha store + record`.

---

### P4-2 — Per-alpha metrics (`compute_metrics`)

**Scope.** Compute `AlphaMetrics` (Sharpe, turnover, returns, drawdown, margin, **fitness**, holding) from a
PnL stream + a dollar-traded stream. The WorldQuant fitness formula adopted verbatim.

**Algorithm.** Exactly §5.1. One pass (Welford for mean/var via atx-core `online_stats`); equity curve =
`cumprod(1+r)` for drawdown. `fitness = sqrt(abs(returns)/max(turnover,0.125))·sharpe`. NaN guards
documented (flat alpha ⇒ sharpe 0; zero turnover+returns ⇒ fitness 0).

**Contract.** Pure, `[[nodiscard]]`, no allocation (single pass over spans). The `252` annualization factor
and the `0.125` turnover floor are named constants with a comment citing WorldQuant §4.4.

**Tests (known-value).** hand-computed Sharpe on a fixed return vector (vs `√252·mean/std`); fitness on a
fixed `(returns, turnover)` pair matches the formula by hand; drawdown on a known peak-trough path;
zero-volatility ⇒ sharpe 0; zero-turnover ⇒ fitness 0 (no div-by-zero); holding ≈ 1/turnover. Boundary:
length-1 stream; all-NaN (returns NaN metrics, documented).

**Acceptance.** `/W4 /WX` clean; green. Commit `feat(p4a-2): per-alpha metrics + fitness`.

---

### P4-3 — Orthogonality / quality gates (`AlphaGate`)

**Scope.** Admit an alpha only if it clears the Sharpe/fitness/turnover floors **and** is sufficiently
diversifying (correlation-to-pool ≤ `max_pool_corr`). The diversification gate is the operational core of
the weak-signal thesis.

**Algorithm.** Exactly §5.2. `corr_to_pool` = max `|pairwise-complete Pearson|` of the candidate's PnL vs
each accepted member (atx-core `cross_section`/`stats` correlation). Verdict from the first failing condition
in fixed order (deterministic). Empty pool ⇒ `corr_to_pool = 0`.

**Contract.** `[[nodiscard]] GateVerdict admit(...)`; exhaustive switch when callers map a verdict to an
action (no `default`). Pairwise-complete NaN policy shared with the combiner (one helper).

**Tests.** below-Sharpe → `RejectSharpe`; below-fitness → `RejectFitness`; above-turnover →
`RejectTurnover`; a perfect-copy candidate (corr 1.0) → `RejectCorrelated`; an anti-correlated candidate
(corr −1.0, `|corr|=1`) → `RejectCorrelated` (magnitude gate); first alpha (empty pool) → `Accept`; an
orthogonal candidate (corr ~0) clearing floors → `Accept`. Boundary: pairwise-complete with NaN gaps;
all-NaN candidate.

**Acceptance.** `/W4 /WX` clean; green. Commit `feat(p4a-3): orthogonality + quality gates`.

---

### P4-4 — Progressive combiner (`AlphaCombiner`)

**Scope.** Fit per-alpha blend weights over the gated pool on a trailing window. Five methods (§5.3),
default `ShrinkageMv`; each handles the singular-SCM (N≫T) obstacle. The fit/apply firewall (§3.1) is built
in: `fit(pool, fit_begin, fit_end)` reads only `[fit_begin, fit_end)`.

**Algorithm.** Exactly §5.3. `EqualWeight`/`IcWeighted` are closed-form. `ShrinkageMv`: Ledoit-Wolf shrink
`S = cov(R)` toward `(tr(S)/N)·I`, solve `Σ̂ w = μ` via atx-core `spd::solve` (SPD after shrinkage),
renormalize `Σ|w|=1`. `BoundedRegression`: regress forward returns on signals over the top-k PCs (atx-core
`pca`), ridge-regularize, clip `|w_i|≤weight_bound`, re-project. `RankAverage` returns uniform weights and
flags rank-space combination for P4-5. Exhaustive switch over `CombineMethod` (no `default`).

**Contract.** `[[nodiscard]] Result<Combination>`; returns `Err` on a non-SPD covariance that shrinkage
cannot rescue (documented) or a window with `T<2`. `Combination` carries `[fit_begin, fit_end)`. Cold path
(may allocate scratch matrices).

**Tests (known-value + differential).** EqualWeight ⇒ `1/N`; two identical alphas under ShrinkageMv ⇒ equal
weights summing to the gross; two anti-correlated equal-Sharpe alphas ⇒ equal weights (risk-balanced);
IcWeighted ⇒ weights ∝ IC; ShrinkageMv with `T<N` (singular `S`) still returns finite weights (shrinkage
rescues); BoundedRegression respects `weight_bound`; **fit-window**: weights computed on `[a,b)` ignore rows
≥ b (truncating the pool after b leaves the weights byte-identical). Boundary: single alpha (`w=[1]`); empty
pool → `Err`; `T<2` → `Err`.

**Acceptance.** `/W4 /WX` clean; green. Commit `feat(p4a-4): progressive alpha combiner`.

---

### P4-5 — `CombinedSignalSource` (the mega-alpha as an `ISignalSource`)

**Scope.** Wrap the pool's constituent signal sources + a frozen `Combination` so the mega-alpha runs in the
*existing* `BacktestLoop` with **zero loop changes** — the seam Phase-2 explicitly anticipated.

**Data structure.**
```cpp
namespace atx::engine::combine {
class CombinedSignalSource final : public ISignalSource {
 public:
  // `sources[i]` is constituent alpha i; `combo.weights[i]` its blend weight (frozen, fit OOS-safely).
  CombinedSignalSource(std::vector<ISignalSource*> sources, Combination combo,
                       CombineMethod method) noexcept;
  // evaluate each constituent over `panel`, blend into one cross-sectional signal:
  //   linear methods:  out[k] = Σ_i w_i * source_i(panel)[k]        (NaN constituents skipped per-cell)
  //   RankAverage:     out[k] = mean_i rank_cs(source_i(panel))[k]  (combine in rank space)
  [[nodiscard]] atx::core::Result<SignalView> evaluate(PanelView panel) override;
  [[nodiscard]] atx::usize max_lookback() const noexcept override;  // max over constituents
 private:
  std::vector<ISignalSource*> sources_;
  Combination combo_;
  CombineMethod method_;
  std::vector<atx::f64> out_;  // owned blend buffer; SignalView borrows it (seam contract)
};
}
```

**Contract.** Pure in `panel` (the Phase-2 `ISignalSource` contract). Returns a `SignalView` borrowing
`out_` (no per-call allocation beyond the owned buffer — the seam forbids hot-path alloc). NaN ("no opinion")
constituents are skipped per-cell; if all constituents are NaN at instrument `k`, `out_[k]` is NaN.
`max_lookback` = max over constituents (so the loop sizes its `RollingPanel` correctly). The frozen
`Combination` was fit OOS-safely by P4-4; this source only *applies* it.

**Tests (differential).** two `ScriptedSignalSource` constituents + known weights ⇒ hand-computed blend;
RankAverage ⇒ mean of cross-sectional ranks; a NaN constituent at a cell is skipped (re-normalized);
all-NaN cell ⇒ NaN; `max_lookback` = max; **runs in the real `BacktestLoop`** (an integration test:
mega-alpha over a synthetic panel produces deterministic fills, exactly as a single alpha did in Phase 2).
Boundary: single constituent (= that alpha scaled); weights summing to 0.

**Acceptance.** `/W4 /WX` clean; green; the loop-integration test passes. Commit
`feat(p4a-5): combined signal source (mega-alpha)`.

---

### P4-5-close — Sprint 4a close

Run the [`../docs/sprint.md`](../docs/sprint.md) close ceremony: lift residuals (position-based combiner
rung; production `VmSignalSource` constituents pending Phase-3 close; caller-scratch overloads) to the
ROADMAP backlog; update the ROADMAP Phase-4 status table (4a rows ✅); bump `Last reviewed`; write "What 4a
proves / Next" (baton → 4b); merge `--no-ff` (`merge: phase-4a — alpha pool + combiner`). Commit
`docs(p4a-close): close phase-4a — 6 units, <M> tests`.

---

## SPRINT 4b — Factor risk model + risk-aware construction

### P4-6 — Factor exposures (`FactorModel::X` builder)

**Scope.** Build the per-date exposure matrix `X` (M×K) from the panel: sector dummies (`IndClass`) + the
five price-derived style factors (§5.4), each cross-sectionally standardized. No fundamentals.

**Scope opens 4b.** `include/atx/engine/risk/fwd.hpp` + `risk/` scaffold + `phase-4b-progress.md` ledger are
part of this unit's marker (or a tiny preceding `docs(p4b-0)` marker — orchestrator's choice; record in the
ledger).

**Algorithm.** §5.4 exposure column construction. Size = `ln(cap)`; Momentum = `ts_sum(returns,252) −
ts_sum(returns,21)`; Volatility = `stddev(returns,60)`; Beta = trailing 252d slope vs a market proxy
(equal-weight universe return); Liquidity = `ln(adv20)`. Standardize each style column over the valid
universe (z-score). Sector dummies are 0/1 per `IndClass` group. Missing exposure ⇒ instrument dropped from
that date (§3.3).

**Contract.** PIT: reads only rows `< t`. Returns the exposure block + the column→factor map. The style mask
+ sector flag are config (`FactorModelConfig`). Pure given the panel.

**Tests (known-value).** size = `ln(cap)` on a fixture; standardized column has mean≈0/std≈1; sector dummies
sum to the group size; momentum/vol windows read the right trailing rows (truncation-invariant); a missing
`cap` drops that instrument. Boundary: 1-instrument universe (standardize degenerate ⇒ 0); a single sector.

**Acceptance.** `/W4 /WX` clean; green. Commit `feat(p4b-6): factor exposure matrix`.

---

### P4-7 — Factor returns + covariance (`FactorModelBuilder::build`)

**Scope.** Estimate factor returns by per-date cross-sectional WLS, specific variances `D` from residuals,
shrunk factor covariance `F`; assemble the factored `V = XFXᵀ + D`. Optional PCA + dead-alpha rungs.

**Algorithm.** §5.4 estimation. Per date in `[t−window, t)`: WLS `r = Xf + u` (atx-core
`linalg::regression::wls`, weights `1/D_i`, bootstrapped from an initial OLS pass); residuals `u`;
`D_i = var(u_i)`; `F = LedoitWolf(cov(f))` (SPD). Store `X, F, D` factored. `risk(w)` and `apply_inverse`
(Woodbury) operate in factor space (§4). Optional `n_stat_factors` (PCA columns) and `n_dead_factors`
(dead-alpha positions), default 0.

**Contract.** `[[nodiscard]] Result<FactorModel>`; `Err` if `F` cannot be made SPD or the window is too
short (`T < K`). Carries `[fit_begin, fit_end)`; `risk`/`apply_inverse`/`neutralize` are the apply path
(no M×M ever). PIT enforced by the truncated panel view.

**Tests (known-value + differential).** a one-factor model on a fixture ⇒ analytic `V`; `risk(w)` matches
`wᵀ(XFXᵀ+D)w` computed densely on a small M (cross-check the factored form against the explicit matrix);
`apply_inverse` then `V·` round-trips to identity (within ULP); WLS factor returns vs a hand OLS on a
2-factor fixture; `D` = residual variance; **fit-window**: building at `t` ignores rows ≥ t. Boundary:
`T<K` → `Err`; single factor; an all-zero specific-variance instrument (floor `D`).

**Acceptance.** `/W4 /WX` clean; green. Commit `feat(p4b-7): factor returns + covariance (V=XFXᵀ+D)`.

---

### P4-8 — Neutralization stages in `WeightPolicy`

**Scope.** Extend `loop::WeightPolicy` with the four stages the research flags (§5.5): group-neutralize
(wire the inert `industry_neutral`), factor-neutralize, decay, truncation. Existing pipeline + tests stay
green (new stages default to no-op).

**Algorithm.** Insert stages in the §5.5 order. GROUP: demean within each `group_id` (needs an
`InstrumentId→group_id` map argument; source = Phase-3 `IndClass`). FACTOR: `FactorModel::neutralize`
(residualize on `X`). DECAY: `decay_linear` over the final signal (Phase-3 `TsDecayLinear` kernel).
TRUNCATE: clamp `|w_i|≤truncation`, re-normalize. Remove the `ATX_ASSERT(!industry_neutral)` and implement
it (the header already names this exact change).

**Contract.** New config fields: `group_map` (optional span), `factor_neutral` (bool + `FactorModel*`),
`decay` (window, 0=off), `truncation` (cap, 0=off). All default to the current behavior. `to_target_weights`
stays `const` and allocate-once-per-rebalance (the existing precedent).

**Tests (known-value).** group-demean ⇒ each group sums to 0; factor-neutralize ⇒ result orthogonal to `X`
columns (residual); decay ⇒ smoothed signal (weights sum to 1); truncation ⇒ no `|w_i|` exceeds cap, still
`Σ|w|=L` after re-norm; **all stages off ⇒ byte-identical to the Phase-2 `WeightPolicy` output** (the
regression guard). Boundary: single group; a name pinned at the cap; decay window = 1 (identity).

**Acceptance.** `/W4 /WX` clean; green; Phase-2 `WeightPolicy` tests still pass unchanged. Commit
`feat(p4b-8): group/factor neutralize + decay + truncation`.

---

### P4-9 — Turnover-penalized risk-aware optimizer (`PortfolioOptimizer`)

**Scope.** The risk-aware book: maximize `αᵀw − λwᵀVw − κ‖w−w_prev‖₁` s.t. dollar-neutral, gross ≤ L,
per-name caps (§5.6). Deterministic fixed-iteration solve in factor space.

**Algorithm.** §5.6. Equality-constrained MV solved analytically via the factored `V⁻¹` (Woodbury,
`FactorModel::apply_inverse`); name caps + L1 turnover via a fixed-iteration projected/proximal loop
(`max_iters`, fixed order): smooth-gradient step, proximal soft-threshold for the L1 turnover term, project
onto `{Σw=0, Σ|w|≤L, |w_i|≤cap}`. No convergence-dependent early exit (determinism).

**Contract.** `[[nodiscard]] Result<std::vector<f64>>`; `Err` on dimension mismatch. `κ=0` ⇒ pure
risk-aware MV; `λ=κ=0`, caps off ⇒ recovers the `WeightPolicy` dollar-neutral book (the regression check).
Apply path (per rebalance); the factored `V` keeps it O(iters·(MK+M)).

**Tests (known-value + differential).** `λ=κ=0`, caps off ⇒ matches `WeightPolicy::to_target_weights`
dollar-neutral output (bit-for-bit within 1e-9); raising `λ` shrinks gross exposure to high-variance names;
`κ>0` reduces turnover vs `w_prev` (fewer/smaller trades); name cap is never exceeded; Σw=0 and Σ|w|≤L hold
(within 1e-9); **determinism**: same inputs ⇒ identical weights across runs; fixed iters ⇒ no variation.
Boundary: `w_prev = w*` (no trade); single name (degenerate dollar-neutral ⇒ 0); cap below the
equal-weight ⇒ all names pinned.

**Acceptance.** `/W4 /WX` clean; green. Commit `feat(p4b-9): turnover-penalized risk-aware optimizer`.

---

### P4-10 — Integration: fit/apply firewall · capacity · determinism · bench · sprint close

**Scope.** The proof. An integration suite proving (a) the **fit/apply firewall** (no look-ahead via
fitting), (b) **determinism** of the whole layer, (c) the **capacity curve**, and (d) a full **walk-forward
combined backtest**; then the bench; then the 4b close.

**Design.**
- **Fit/apply firewall (the headline new invariant):** fit the combiner + factor model on `[t₀, t₁)`, run
  the mega-alpha forward; then **truncate** the panel/PnL after a later date `t`; re-run; assert every output
  for dates `≤ t` is byte-identical (future rows provably invisible). Also assert applying any fitted object
  *inside* its fit window aborts (`EXPECT_DEATH`).
- **Determinism:** evaluate the whole pipeline twice (and single- vs multi-thread where parallel);
  hash the ordered `(date, instrument, weight-bits)` stream; identical hashes. A mutated input (reorder
  alphas, perturb one PnL, add a late row) flips the hash (non-vacuous).
- **Capacity curve:** for a fitted book, sweep AUM through `risk::capacity_curve` over the Phase-2
  `ExecutionSimulator`'s √-impact; assert the net edge is monotone-decreasing in AUM and crosses zero (the
  capacity point). RenTech §9.6: report capacity, not just return.
- **Walk-forward combined backtest:** a realistic multi-alpha pool over a synthetic panel (incl. delisted
  symbols + NaN gaps): gate → store → re-fit combiner on a rolling window → CombinedSignalSource → (4b)
  factor-neutralize + optimizer → ExecutionSimulator → Portfolio. Assert the end-to-end run is deterministic
  and cost-honest (costs-off recovers the frictionless equity, as in Phase 2).

**Bench.** combiner `fit` (by method, with N,T,k), factor `build`, optimizer `solve` per rebalance, and the
full walk-forward loop. Measured only; host/build/panel recorded.

**Sprint close (sprint.md ceremony).**
1. Lift residuals to ROADMAP backlog (position-based combiner; dead-alpha risk default-off; deflated-Sharpe
   + walk-forward harness deepening → Phase 5; caller-scratch overloads; computed `V` parallelism Linux-TSan
   pass).
2. Update ROADMAP Phase 4 status table (4b rows `⏳ → ✅ <sha>`).
3. Bump ROADMAP `Last reviewed`.
4. Write "What Phase 4 proves / Next sprint priorities" in the ledger (baton → Phase 5 validation).
5. Write `phase-4.md` user reference (the combiner methods + gate config + factor model + optimizer API +
   the fit/apply firewall + determinism + capacity guarantees).
6. Merge `--no-ff` (`merge: phase-4b — factor risk + risk-aware construction`). **Do not push unless asked.**

**Acceptance.** Firewall/determinism/capacity/walk-forward suites green; bench recorded; close items 1–6
done; ledger + ROADMAP reconciled. Commit `docs(p4b-close): close phase-4b — 5 units, <M> tests`.

---

## 9. Sub-agent dispatch checklist (per unit)

Each brief includes (per [`../docs/sprint.md`](../docs/sprint.md)): worktree path + branch
(`worktree-phase-4a-combiner` / `worktree-phase-4b-risk`, or in-place on the active branch per the
established engine workflow — record the choice in the ledger header); the unit's scope **quoted** from this
plan; acceptance criteria; the verbatim *"Marker-commit pattern is mandatory: commit before stopping or work
is lost."*; commit format `feat(p4a-N)` / `feat(p4b-N)`; predecessor SHAs; the ledger-row instructions; and
the handoff block below.

### Handoff block (paste into every coding sub-agent brief)
```text
Implementation quality standard (atx):
Governed by .agents/cpp/agent.md (safety-critical-grade C++20). Use atx-core public headers (error.hpp,
types.hpp, hash.hpp, linalg/regression.hpp, linalg/pca.hpp, linalg/spd.hpp, linalg/solve.hpp,
stats/cross_section.hpp, series/frame.hpp) and the as-built engine headers (loop/weight_policy.hpp,
loop/signal_source.hpp, exec/execution_sim.hpp) as the positive style + API reference: module-level intent
comment, grouped types/APIs, explicit ownership/lifetime/error contracts, concise comments that explain
invariants and non-obvious control flow — never narrate code.

This phase is FIT-THEN-APPLY. The dominant correctness risk is LOOK-AHEAD VIA FITTING: a combiner weight, a
gate statistic, and a factor covariance are estimated from data; if that data includes the future the
backtest lies invisibly (the per-alpha look-ahead rails do not see the combiner). THE central rail: every
fitted object carries an explicit [fit_begin, fit_end) window, fit functions receive a TRUNCATED view (they
physically cannot read the future), and applying a fitted object inside its own window ABORTS. Prove it by
truncation-invariance (truncate after date t -> outputs for <= t byte-identical).

Determinism is non-negotiable: NO RNG; iterative solvers run a FIXED iteration count in FIXED order (no
convergence-dependent early exit); reductions are order-fixed (canonical instrument/alpha-id order); rank
tie-breaking reuses the Phase-3 CsRank convention. Same input -> byte-identical weights.

Numerics reuse atx-core L7 (regression ols/ridge/wls, pca, spd, solve) — do NOT re-implement linear algebra
in the engine (anti-roadmap: no new general-purpose primitives in the engine). The factor covariance V=XFXᵀ+D
is kept FACTORED — never materialize the M x M matrix; risk(w) and V⁻¹ (Woodbury) work in factor space (K<<M).

No UB, no narrowing, no uninitialized vars, no owning raw pointers. const/constexpr/noexcept/[[nodiscard]]
where they hold. Expected failures (size mismatch, non-SPD covariance, fit-window misuse) -> Result<T>/Status,
never throw for control flow. Weakest sufficient type at interfaces (span/string_view). Every enum-class
switch exhaustive (CombineMethod/GateVerdict/StyleFactor, no default); loops bounded; functions <= ~60 lines.
// SAFETY: on every deviation. The FIT path (combiner/factor/gate) is the COLD path and may allocate; the
APPLY path (CombinedSignalSource::evaluate, WeightPolicy, optimizer solve) follows the existing
allocate-once-per-rebalance WeightPolicy precedent — no per-cell allocation.

TDD: failing GoogleTest first, watch it fail for the right reason, then implement. Prefer KNOWN-VALUE and
hand-computed fixtures (equal-weight = 1/N; two anti-correlated alphas -> equal weights; one-factor model ->
analytic V; λ=κ=0 optimizer -> the WeightPolicy dollar-neutral weights bit-for-bit). Cover happy path,
boundaries (empty pool, single alpha, 1-instrument universe, T<N singular SCM, all-NaN, all-equal exposures),
error paths (size mismatch, non-SPD, fit-window violation), invariant violations (EXPECT_DEATH on
apply-inside-fit-window). Build /W4 /WX clean; clang-tidy/format clean; tests pass under ASan+UBSan.

A unit is done only when header + impl + tests + ledger row + build/test gate are all present. No TODO stubs,
no fake success paths, no untested skeletons. Prefer a smaller complete slice over a larger partial one.
```

---

## 10. Risks / watch-items

- **Look-ahead via fitting (the #1 risk).** The combiner/gate/factor-model are the first *fitted* objects in
  the engine; the existing per-datum look-ahead rails don't cover them. Mitigation is structural (§3.1): the
  fit-window firewall + truncation-invariance proof (P4-10). Treat any fit function that takes the full
  history (not a truncated view) as a defect.
- **Singular SCM (N ≫ T).** Naively inverting the alpha covariance blows up. The combiner's default
  `ShrinkageMv` (Ledoit-Wolf) and `BoundedRegression` (PCs) are the designed-in defenses (§5.3); never
  expose a raw-inverse method. Test the `T<N` case explicitly (P4-4).
- **Non-SPD covariance.** Shrinkage should keep `Σ̂`/`F` SPD, but floating error or a degenerate window can
  break it. Return `Err` (don't abort) and floor specific variances; test the degenerate window.
- **Optimizer non-determinism.** A convergence-tolerance early exit makes iteration count input-dependent and
  breaks bit-reproducibility. Use a **fixed iteration count** (config), fixed update order, no early exit
  (§3.2). This trades a little optimality for determinism — the right call for a backtester.
- **`WeightPolicy` regression.** P4-8 must not change existing behavior when the new stages are off. The
  bit-for-bit "all stages off ⇒ Phase-2 output" test is the guard; run the Phase-2 `WeightPolicy` suite
  unchanged per unit.
- **Factor-model cost vs benefit.** A full Barra build per rebalance is heavy. Keep `V` factored, fit at
  *re-fit* cadence (not every bar), and reuse the fitted model across the apply window. If profiling shows
  the build dominates, widen the re-fit interval (a config knob), don't micro-optimize the regression.
- **Cross-phase edge (P4-5) — RESOLVED.** Phase 3 closed (`bf4cbfb`); `VmSignalSource` is green-gated and
  proven driving the real `BacktestLoop` (P3c-4). Real `VmSignalSource` constituents are available from day one;
  `ScriptedSignalSource` stays the unit-test double. Per §0(C) prefer ONE `compile_batch` Program (CSE-shared)
  over N separate sources for the production mega-alpha. The remaining residual is the per-`evaluate()`
  `Panel`/`Engine` build (no `Engine::rebind`) — scoped to rebalance cadence (§0(C)).
- **Scope creep into Phase 5.** Cost *calibration*, deflated-Sharpe, multiple-testing correction, and the
  full walk-forward harness are **Phase 5**. Phase 4 ships the *models* + the IS/OOS split + the truncation
  proof; it does not *validate/calibrate* them. When a feature argues for "let's also fit δ / deflate the
  Sharpe here," it goes to Phase 5 (ROADMAP backlog).
- **Position-based combination temptation.** "Combine a billion alphas" on *holdings* (O(N), trade-crossing
  savings) is real and high-value, but it's a *rung* on the existing `AlphaStore` (which already keeps the
  position matrix) — future-work, not 4a scope. The data model is ready; the method isn't built.
- **Dead-alpha risk factors.** A striking idea (WQ §6.3) but advanced; default off (`n_dead_factors=0`) and
  test it only as an optional rung. Don't let it gate the core factor model.
- **LSP false positives** without `compile_commands.json` — verify against real `cmake --build`; ignore
  phantom missing-include / out-of-policy clang-tidy noise (every prior phase saw this).

---

## 11. Definition of done (Phase 4)

- **Sprint 4a:** P4-0…P4-5 implemented; the mega-alpha runs end-to-end in the existing `BacktestLoop`
  (combined backtest on a multi-alpha pool); gates + metrics + combiner green with known-value fixtures.
- **Sprint 4b:** P4-6…P4-10 implemented; factored `V = XFXᵀ + D` validated against a dense small-M
  cross-check; neutralization stages extend `WeightPolicy` without regressing it; the optimizer recovers the
  `WeightPolicy` book at `λ=κ=0` bit-for-bit.
- `/W4 /permissive- /WX` clean; clang-tidy (project policy) + format clean; tests pass under ASan+UBSan.
- **Fit/apply firewall proven:** truncation-invariance across the combiner + factor model (future rows
  provably invisible); applying a fitted object inside its fit window aborts.
- **Determinism proven:** identical input → identical weight-hash; single-thread == multi-thread; mutation
  detected (non-vacuous).
- **Capacity curve** computed and monotone; the AUM-where-edge-dies is reported (RenTech §9.6).
- Zero steady-state allocation on the **apply** path (instrumented); fit (cold path) may allocate.
- Benches recorded (combiner/factor/optimizer/walk-forward) with host/build context — measured only.
- Both ledgers closed; ROADMAP Phase 4 status + `Last reviewed` updated; `phase-4.md` written; worktrees
  merged `--no-ff` (not pushed unless asked).

---

## Appendix A — Formula reference (the math, in one place)

**Metrics (§5.1) — WorldQuant §4.4/§9.5:**
```
sharpe   = sqrt(252) * mean(r) / std(r)
returns  = 252 * mean(r)
turnover = mean(dollar_traded / book_size)
margin   = returns / max(turnover, 1e-9)
fitness  = sqrt(abs(returns) / max(turnover, 0.125)) * sharpe        ; max(.,0.125) is the paper's floor
holding  = 1 / max(turnover, 1e-9)
drawdown = max_t (peak(equity[..t]) - equity[t]) / peak(equity[..t]) ; equity = cumprod(1+r)
```

**Gate (§5.2):** `Accept iff sharpe≥φ_s ∧ fitness≥φ_f ∧ turnover≤τ ∧ max_j|corr(c,m_j)|≤ρ`.

**Combiner (§5.3):**
```
EqualWeight       : w_i = 1/N
RankAverage       : combine cross-sectional ranks (rank-space), uniform blend
IcWeighted        : w_i ∝ max(IC_i, 0)            ; IC_i = corr(signal_i, fwd_return)
ShrinkageMv       : w ∝ Σ̂⁻¹ μ ;  Σ̂ = (1−ρ)S + ρ(tr(S)/N)I ;  S=cov(R), μ=mean(R) ; renormalize Σ|w|=1
BoundedRegression : ridge-regress fwd_return on signals over top-k PCs of S; clip |w_i|≤b; re-project
```

**Factor risk model (§5.4) — Barra:**
```
per date:  f = argmin_f Σ_i (1/D_i)(r_i − X_i·f)²   (WLS) ;  u = r − Xf
F = LedoitWolf(cov(f)) ;  D_i = var(u_i) ;  V = X F Xᵀ + diag(D)        ; kept factored
wᵀVw    = (Xᵀw)ᵀ F (Xᵀw) + Σ D_i w_i²
V⁻¹·x   = D⁻¹x − D⁻¹X(F⁻¹ + XᵀD⁻¹X)⁻¹XᵀD⁻¹x          ; Woodbury (O(MK+K³))
neutralize(s) = s − X (XᵀX)⁻¹ Xᵀ s                    ; residual on factors
```

**Optimizer (§5.6):** `max αᵀw − λwᵀVw − κ‖w−w_prev‖₁  s.t. Σw=0, Σ|w|≤L, |w_i|≤cap` (fixed-iteration
proximal/projected; deterministic).

---

## Appendix B — Source index

| Source | What we took | Primary reference |
|---|---|---|
| WorldQuant deep-dive §4.4 | `fitness` formula; correlation/turnover/Sharpe submission gates | [`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md) |
| WorldQuant deep-dive §6.1 | bounded regression over PCs; singular SCM (N≫T) | Kakushadze, *Combining Alphas via Bounded Regression*, arXiv:1501.05381 |
| WorldQuant deep-dive §6.2 | O(N) combination; combine on positions; mega-alpha framing | Kakushadze & Yu, *How to Combine a Billion Alphas*, arXiv:1603.05937 |
| WorldQuant deep-dive §6.3 | dead alphas as risk factors (endogenous risk directions) | Kakushadze & Yu, *Dead Alphas as Risk Factors*, arXiv:1709.06641 |
| WorldQuant deep-dive §6.4 | neutralization (market/sector/industry) at formula + portfolio layers | 101 Formulaic Alphas, arXiv:1601.00991 |
| WorldQuant deep-dive §6.5/§9.8 | turnover↔cost (`C~1/T`, `R~V^x`); turnover priced as cost, not return-killer | Kakushadze & Tulchinsky, *Performance v. Turnover*, arXiv:1509.08110 |
| WorldQuant deep-dive §9.4 | the missing `WeightPolicy` stages: group-neutralize, decay, truncation | BRAIN docs (Neutralization/Decay/Truncation settings) |
| RenTech deep-dive §9.3 | signal combination is the product; shrinkage progression; factor neutralization | *The Man Who Solved the Market* + audit |
| RenTech deep-dive §9.4 | out-of-sample discipline enforced by the harness, not intentions | Patterson/Simons public talks + audit |
| RenTech deep-dive §9.6 | report capacity, not just return; edge is capacity-bounded | Cornell, *The Ultimate Counterexample?* + audit |
| Architecture audit §4 | Barra `V=XFXᵀ+D`; singular alpha covariance; combiner progression | [`engine-architecture-audit.md`](engine-architecture-audit.md) |
| atx-core L7 | regression (ols/ridge/wls), pca, spd, solve, decompose | `atx-core/include/atx/core/linalg/*.hpp` |
| atx-core L6 | cross_section (corr/demean/rank/zscore), online_stats (Welford) | `atx-core/include/atx/core/stats/*.hpp` |

*Full URL index + confidence tags: the two research deep-dives' Reference sections.*
