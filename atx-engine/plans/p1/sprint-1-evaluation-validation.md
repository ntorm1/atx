# Sprint S1 — Evaluation & Validation Spine (sprint spec)

**Status:** ✅ CLOSED (2026-06-07, `feat/atx-core-stdlib @ 2158a17`). Shipped as `eval::` (stats_ext /
perf_metrics / deflated_sharpe / pbo / cpcv) + `validation::bias_audit` — 36 tests, full engine suite green.
See the close ledger [`sprint-1-progress.md`](sprint-1-progress.md) and the user reference
[`sprint-1.md`](sprint-1.md). S1 *reused* the P4 fit/apply firewall + `combine::compute_metrics` (no second
Sharpe convention). Opened **first** of p1, independent of the *factory* track.
**Implementation plan (the frozen *how*):** [`sprint-1-evaluation-validation-implementation-plan.md`](sprint-1-evaluation-validation-implementation-plan.md) — per-unit S1-0…S1-5, exact APIs, TDD steps.
**Roadmap:** [`ROADMAP.md`](ROADMAP.md) · **Discipline:** [`../docs/sprint.md`](../docs/sprint.md)
**Grounded in:** [`../../research/renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md)
§7 (overfitting discipline: survivorship / look-ahead / data-snooping; deflated Sharpe; walk-forward) ·
[`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md) §4
(fitness / correlation gating; sub-universe robustness).

---

## Why this sprint is first

The single dominant failure mode of an alpha factory is **data-snooping at scale**: test 10⁵–10⁹ candidate
expressions, keep the best, and the "best" is a near-certainty of being fit to noise. RenTech's edge is as
much *discipline* as discovery — out-of-sample testing, walk-forward, and a **deflated** Sharpe that
corrects for how many things you tried. WorldQuant gates every alpha on fitness *and* marginal correlation,
and re-tests Sharpe on weaker sub-universes to penalize overfit.

So the **scorer must exist before the factory**, or the factory just manufactures plausible garbage faster.
S1 is the truth layer every later sprint (S3 mining fitness, S5 ML admission, S7 reporting) calls into.

p0 shipped a thin `BacktestResult` (equity curve, fills, turnover) and P4 introduced the fit/apply firewall
(truncation-invariant fitted objects). S1 turns that into a **complete evaluation + validation toolkit**.

---

## Scope — units

### S1.0 — Marker + ledger
Open `sprint-1-progress.md`, freeze scope, record `Base: feat/atx-core-stdlib @ f2d22f4` (p0 Phases 1–4
closed; shared branch — explicit pathspecs only). Scaffold `eval/fwd.hpp`.

### S1.1 — `PerfMetrics` suite
A pure, deterministic metrics computer over an equity curve and over `AlphaStreams::pnl(a)`. Annualized
**Sharpe** (√252), **Sortino** (downside-deviation denominator), **max drawdown** + **Calmar**,
**information ratio** / **appraisal ratio** (vs a benchmark / vs the factor-residual), **hit rate** (batting
average), **average holding period** (≈1/turnover), and **monthly/annual return decomposition**. Plus the
WQ **fitness** (`sqrt(abs(returns)/max(turnover,0.125))·sharpe`) if P4 didn't already centralize it (reuse,
don't duplicate). All f64 sums in **fixed instrument/date order** (determinism). Handles the `pnl[0]==0`
structural zero documented in P3c streams (exclude or document the bias — pick one, make it explicit).

*Surface (sketch):* `struct PerfMetrics { f64 sharpe, sortino, max_dd, calmar, ir, appraisal, hit_rate,
holding_days, fitness; MonthlyTable monthly; };  PerfMetrics compute_metrics(span<const f64> pnl, ...);`

### S1.2 — Deflated Sharpe Ratio + multiple-testing haircut
Bailey/López de Prado **Deflated Sharpe Ratio (DSR)**: given the observed Sharpe, the number of independent
trials `N`, and the variance/skew/kurtosis of the return stream, compute the probability the true Sharpe
> 0 after correcting for selection across `N` trials. The **haircut Sharpe** (the Sharpe you can honestly
claim). The trial count `N` is supplied by the caller (S3 reports how many expressions it evaluated; S5
how many model configs). *Needs an atx-core L6 stats helper: normal/t CDF + the variance-of-Sharpe estimator.*

### S1.3 — Probability of Backtest Overfitting (PBO)
Combinatorially-symmetric cross-validation (CSCV): partition the trial matrix (candidates × periods) into
`S` combinatorial splits, and estimate the probability that the in-sample best is below-median
out-of-sample. PBO is the headline overfit number for a *batch* of candidates — exactly what the factory
(S3) and the ML config search (S5) produce. Deterministic split enumeration (no RNG).

### S1.4 — Purged + embargoed CPCV harness
Extend P4's walk-forward / fit-apply firewall into a full **Combinatorial Purged Cross-Validation** harness:
train/validate/test splits with **purging** (drop training observations whose labels overlap the test
window) and an **embargo** (gap after each test block) so leakage across the fit/apply boundary is
structurally impossible — the same truncation-invariance proof P4 uses, generalized to many folds. This is
the harness S5 trains learned models inside and S3 validates mined alphas against. *Reuses the P4 fitted-
object `[fit_begin, fit_end)` window contract.*

### S1.5 — Bias-audit gate battery + close
A **reusable** set of assertions any sprint can drop into a test: survivorship (a delisted symbol's PnL is
present and freezes), look-ahead (truncation invariance — output ≤ t identical with/without > t), and
snooping (a deliberately-overfit synthetic alpha is **caught** by DSR/PBO — non-vacuous). Package as a small
header (`validation/bias_audit.hpp`) the factory and combiner reuse verbatim. Sprint close ceremony.

---

## Exit criteria

- `PerfMetrics` reproduces hand-computed Sharpe/Sortino/DD/Calmar/IR/hit-rate/holding on a fixture, byte-stable
  across two runs, fixed-order sums.
- DSR + haircut Sharpe match a published worked example (Bailey/LdP) within ULP tolerance; DSR → 0 as trial
  count → ∞ for a fixed observed Sharpe (the selection penalty bites).
- PBO on a synthetic batch of *pure-noise* alphas → ≈ 0.5 (no real edge); on a batch with one genuine edge →
  materially < 0.5 (non-vacuous).
- CPCV harness is truncation-invariant: no training fold ever reads a test-window observation (purge+embargo
  proven by an overlap assertion).
- Bias-audit battery **catches** a planted-overfit alpha and **passes** an honest one — the snooping gate is
  non-vacuous.
- All under `/W4 /permissive- /WX`, clang-tidy + clang-format clean; new test file per unit.

## Invariants this sprint must prove

Determinism (fixed-order sums, no RNG in splits), no look-ahead (purge+embargo is the structural gate),
differential correctness (DSR/PBO vs worked examples). This sprint *is* the no-look-ahead/no-snooping proof
machine the rest of v2 reuses.

## Dependencies

- **Upstream:** p0 Phase-3 `AlphaStreams`, Phase-2 `BacktestResult`, P4 fit/apply firewall (`[fit_begin,fit_end)`),
  and **P4's `combine::compute_metrics`** (Sharpe/drawdown/fitness/turnover/holding — reuse, don't duplicate).
- **atx-core (Pattern B edge):** L6 `stats` is MISSING the distribution/higher-moment helpers DSR/PBO need —
  no `norm_cdf`/`norm_ppf`/skewness/kurtosis/median (verified). S1 builds these **engine-local** in
  `eval/stats_ext.hpp` over `<cmath>` (the `combine/correlation.hpp` precedent), and records an upstream
  request to lift them into atx-core L6 as a close residual. **Not a blocker — S1 ships end-to-end.**
- **No Phase-4 *blocking* dependency** — P4 is DONE, so S1 reuses it (the spec's earlier "no P4 dependency"
  framing predates the P4 close).

## Explicitly NOT in this sprint

No metric *visualization* (headless artifacts only). No new combiner/optimizer (P4/S5/S7). No cost
calibration (S6 — S1 consumes whatever cost the streams already carry). No parallelism (S2 parallelizes the
CPCV folds later).

## Baton → next

S1 hands S3 a **pool-aware, deflated fitness** to mine against and S5 a **CPCV harness + DSR/PBO admission
gate** to train and admit learned models inside. Without S1, those sprints cannot tell signal from noise.
