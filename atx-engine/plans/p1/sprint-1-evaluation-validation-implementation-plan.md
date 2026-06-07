# Sprint S1 — Evaluation & Validation Spine — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan unit-by-unit. Steps use checkbox (`- [ ]`) syntax for tracking. This is the FROZEN *how*; the sprint spec [`sprint-1-evaluation-validation.md`](sprint-1-evaluation-validation.md) is the *what*. On conflict, **§0 (this plan's as-built amendment) overrides** the spec.

**Goal:** Ship the engine's truth layer — deterministic performance metrics, a deflated/multiple-testing-corrected Sharpe, a probability-of-overfit estimate, a purged+embargoed cross-validation harness, and a reusable bias-audit gate battery — so every later p1 sprint (mining, ML, reporting) can tell signal from noise.

**Architecture:** Two new header-only inline subsystems under `atx/engine/`: `eval/` (numeric base + `PerfMetrics` + Deflated-Sharpe + PBO + CPCV) and `validation/` (the reusable bias-audit asserts). All compute is pure, deterministic (no RNG; fixed-order reductions), and consumes the as-built p0 surfaces (`BacktestResult`, `alpha::AlphaStreams`, `combine::compute_metrics`, the `[fit_begin,fit_end)` fit/apply firewall) without re-duplicating them. atx-core lacks the distribution/higher-moment helpers, so a small `eval/stats_ext.hpp` provides them locally over `<cmath>` (the established `combine/correlation.hpp` precedent), with a Pattern-B request to upstream them recorded as a residual.

**Tech Stack:** C++20, header-only inline (`#pragma once`), GoogleTest (`atx-engine/tests/*_test.cpp`, CONFIGURE_DEPENDS glob → `atx-engine-tests`), CMake + Ninja, clang-cl `/W4 /permissive- /WX`. Consumes `atx-core` L0 (`types`, `error`, `hash`), L6 (`stats`), and the p0 engine headers. No RNG.

---

## §0 — As-built reconciliation amendment (the "up to date" fixes)

The spec was drafted while p0 Phase 4 was still in flight ("assume P4 is done", `Base: master`, "opens first / no Phase-4 dependency"). **As of 2026-06-07, p0 Phases 1–4 are CLOSED** on `feat/atx-core-stdlib` (Phase-4b close `f2d22f4`; see [`../p0/phase-4b-progress.md`](../p0/phase-4b-progress.md), [`../p0/phase-4.md`](../p0/phase-4.md)). This plan reconciles the spec to what actually shipped:

1. **Base is `feat/atx-core-stdlib @ f2d22f4`**, not `master`. The branch is **shared** with a concurrent databento effort — every commit stages **explicit pathspecs only** (`git add -- <paths>` then `git commit -- <paths>`), NEVER `git add -A`/`.`/`-a`; after each commit verify `git merge-base --is-ancestor <sha> HEAD` and `git show HEAD --stat` shows only your files (the p0 sprint hit a foreign-staged-file sweep — see the close ledger). The worktree-vs-in-place choice is recorded in the ledger header at kickoff (p0 precedent: in-place, no `--no-ff` merge; do **not** push unless asked).

2. **Reuse, do not re-derive, the p0 metrics.** `combine::compute_metrics` ([`../../include/atx/engine/combine/metrics.hpp`]) already centralizes Sharpe (annualized √252, population std, computed over `pnl[1..T)` excluding the structural `pnl[0]==0`), turnover, returns, **max-drawdown** (`detail::max_drawdown`), margin, **fitness** (`sqrt(abs(returns)/max(turnover,0.125))·sharpe`), and holding-days (`1/turnover`). S1.1 **calls** `combine::compute_metrics` for that subset and ADDS only the missing metrics (Sortino, Calmar, IR, appraisal, hit-rate, monthly). It must NOT fork a second Sharpe convention — a cross-check test pins `eval` Sharpe == `combine` Sharpe bit-for-bit.

3. **atx-core has NO distribution/higher-moment helpers** (investigated: no `norm_cdf`/`norm_ppf`/`erf` wrapper, no skew/kurtosis, no exact median, no Pearson span-corr). DSR (needs Φ, Φ⁻¹, skew, kurtosis) and PBO (needs median/rank) are therefore built on a small **engine-local `eval/stats_ext.hpp`** over `<cmath>` `std::erfc` + `std::nth_element` + `atx::core::stats::partial_rank` — the exact precedent set by `combine/correlation.hpp::pairwise_complete_corr` (engine-local because "atx-core only exposes a fixed-window `RollingCorrelation<W>`"). **This unblocks S1 end-to-end** (no upstream wait). A Pattern-B request to upstream `norm_cdf`/`norm_ppf`/`skewness`/`kurtosis`/`median` into atx-core L6 is recorded as a close residual (the p1 ROADMAP already lists the S1.2/S1.3 → L6 edge).

4. **New subsystems, no collision.** No `eval/`, `validation/`, or `metrics/` directory exists under `atx-engine/include/atx/engine/`. S1 creates `atx/engine/eval/` and `atx/engine/validation/`, header-only inline with an `eval/fwd.hpp` (the `combine/`/`risk/` pattern). **Namespace `atx::engine::eval`** (NOT `combine` — `combine::compute_metrics` is a free function; a second `eval::compute_metrics` in a different namespace avoids any overload/ODR clash).

5. **The fit/apply firewall is real and reusable.** `combine::Combination` carries **public fields** `fit_begin`/`fit_end`; `risk::FactorModel` carries **accessors** `fit_begin()`/`fit_end()`. S1.4's CPCV generalizes this `[fit_begin,fit_end)` window contract to many folds; S1.5 reuses the `phase4_integration_test.cpp` truncation-invariance + `apply_guard` `EXPECT_DEATH` idioms verbatim.

6. **Tests live in `atx-engine/tests/*_test.cpp`** (flat, CONFIGURE_DEPENDS) — NOT `tests/src/` (the sprint.md doc is stale on that point). One new `*_test.cpp` per unit auto-registers.

---

## §1 — Overview, pipeline, file structure

S1 is consumed by every later sprint as the *scorer*. The data flow:

```
BacktestResult.equity_curve ─┐
alpha::AlphaStreams.pnl(a) ──┼─► eval::compute_return_metrics ─► ReturnMetrics (Sharpe/Sortino/DD/Calmar/IR/hit/monthly)
combine::compute_metrics ────┘            (reuse: turnover/fitness/holding/drawdown)

return stream + N trials ─► eval::deflated_sharpe ─► DsrResult (PSR, SR*, DSR, haircut)
trial matrix (cand×period) ─► eval::pbo_cscv ─► PboResult (PBO, per-split logits)
fitted-object windows ─► eval::cpcv_folds ─► vector<CpcvFold>{train_idx, test_idx} (purged + embargoed)
any sprint's recompute fn ─► validation::assert_no_lookahead / assert_survivorship / assert_no_snooping
```

### Files created (all header-only inline; `#pragma once`; namespace `atx::engine::eval` except bias_audit = `atx::engine::validation`)

| File | Responsibility | Unit |
|---|---|---|
| `include/atx/engine/eval/fwd.hpp` | forward decls for the eval spine (the `combine/fwd.hpp` pattern) | S1-0 |
| `include/atx/engine/eval/stats_ext.hpp` | engine-local numeric base: `norm_cdf`, `norm_ppf`, `skewness`, `excess_kurtosis`, `median`, `mean_std_pop`, `returns_from_equity` | S1-1 |
| `include/atx/engine/eval/perf_metrics.hpp` | `ReturnMetrics`, `ReturnMetricsCfg`, `compute_return_metrics` (reuses `combine::compute_metrics`) | S1-1 |
| `include/atx/engine/eval/deflated_sharpe.hpp` | `DsrResult`, `deflated_sharpe`, `probabilistic_sharpe`, `expected_max_sharpe` | S1-2 |
| `include/atx/engine/eval/pbo.hpp` | `PboResult`, `pbo_cscv` (combinatorially-symmetric CV; deterministic split enumeration) | S1-3 |
| `include/atx/engine/eval/cpcv.hpp` | `CpcvConfig`, `CpcvFold`, `cpcv_folds` (purged + embargoed; reuses `[fit_begin,fit_end)`) | S1-4 |
| `include/atx/engine/validation/bias_audit.hpp` | reusable `assert_*` test helpers (survivorship / look-ahead / snooping) | S1-5 |

### Tests created (one per unit, `atx-engine/tests/<name>_test.cpp`)

`eval_stats_ext_test.cpp` + `eval_perf_metrics_test.cpp` (S1-1), `eval_deflated_sharpe_test.cpp` (S1-2), `eval_pbo_test.cpp` (S1-3), `eval_cpcv_test.cpp` (S1-4), `validation_bias_audit_test.cpp` (S1-5).

### Ledger

`sprint-1-progress.md` (S1-0), updated per unit (the [`../docs/sprint.md`](../docs/sprint.md) skeleton; copy [`../p0/phase-4b-progress.md`](../p0/phase-4b-progress.md) shape).

---

## §2 — Cross-cutting gates (every coding unit) + the handoff block

- **TDD:** failing GoogleTest first; one behavior per `TEST`; name `Subject_Condition_ExpectedResult`; cover happy path, **boundaries** (empty/length-1 stream, all-zero pnl, single candidate, S=2 PBO, K=2 CPCV, all-NaN, the `pnl[0]==0` structural zero), and **invariant violations** (`EXPECT_DEATH` on applying a fitted object inside its own window).
- **Determinism (§3.2 carried from p0):** NO RNG anywhere (PBO/CPCV split enumeration is lexicographic-combinatorial, not sampled); order-fixed reductions (ascending index); a wyhash (`atx::core::hash_combine`) over a result is replay-stable and flips on input mutation where a determinism test is called for.
- **Fit/apply firewall:** S1.4's folds and S1.5's asserts must make leakage *structurally impossible* and prove it by an overlap assertion + truncation-invariance, exactly as `phase4_integration_test.cpp` does.
- **Warnings = errors:** `/W4 /permissive- /WX` (clang-cl) is the gate; clang-tidy is disabled in this repo (the strict build is the acceptance). Watch `-Wconversion` (`usize`↔`f64`↔`Eigen::Index`) on non-MSVC.
- **API discipline:** `const`/`constexpr`/`noexcept`/`[[nodiscard]]` where they hold; `Result<T>`/`Status` for expected failures (never throw for control flow); weakest sufficient types (`std::span`/`std::string_view`); functions ≤ ~60 lines; `// SAFETY:` on any deviation; named constants (no magic numbers); exhaustive `enum class` switches (no `default`). Reuse atx-core / p0 — **no new general-purpose primitive** beyond the documented `eval/stats_ext.hpp` bridge.
- **No placeholders / no partial stubs:** every shipped function is complete; a deferred rung returns `Err(NotImplemented)` explicitly (never a silent stub).
- **clangd noise:** without `compile_commands.json`, the IDE flags false "missing header"/"incomplete type"/"unused include". IGNORE squiggles; trust only a real `cmake --build`.

### Handoff block (paste into every coding sub-agent brief)

```text
Implementation quality standard (atx): governed by .agents/cpp/agent.md (safety-critical-grade C++20) and
atx-engine/plans/docs/implementation-quality.md. Use the as-built engine headers as the positive style + API
reference: combine/metrics.hpp, combine/correlation.hpp, combine/combiner.hpp, risk/factor_model.hpp,
loop/backtest_loop.hpp, alpha/streams.hpp — module-level intent comment, grouped types/APIs, explicit
ownership/lifetime/error contracts, concise comments that explain invariants and non-obvious control flow,
never narrate code.

This sprint is the NO-SNOOPING / NO-LOOK-AHEAD proof machine. The dominant correctness risk is a VACUOUS
proof: a metric that is right by construction, a DSR/PBO that always "passes", a CPCV fold that silently
leaks. Every validation claim must be non-vacuous — proven to FAIL on a planted-bad input and PASS on a
planted-good one.

Determinism is non-negotiable: NO RNG (split enumeration is lexicographic-combinatorial, not sampled);
reductions are order-fixed (ascending index); the result digest is replay-stable. Reuse atx-core L6 stats
(RunningVariance, partial_rank, argsort) and the p0 combine/risk metrics — do NOT re-derive Sharpe/drawdown/
fitness (call combine::compute_metrics). The only new engine-local numeric code is eval/stats_ext.hpp
(norm_cdf/norm_ppf/skew/kurtosis/median over <cmath>), justified exactly as combine/correlation.hpp is.

No UB, no narrowing (watch -Wconversion), no uninitialized vars, no owning raw pointers, Rule of Zero.
Header-only inline (#pragma once), matching combine/*.hpp and risk/*.hpp. const/constexpr/noexcept/[[nodiscard]]
where they hold. Result<T> for expected failures, never throw for control flow. Functions ≤ ~60 lines.

Marker-commit pattern is mandatory: commit before stopping or work is lost. Stage EXPLICIT pathspecs only
(git add -- <paths>; git commit -- <paths>) — the branch is shared; never git add -A. After committing, run
git show HEAD --stat and confirm ONLY your files appear (no .gitmodules, no atx-core/*, no submodule), then
git merge-base --is-ancestor <sha> HEAD. Build gate: cmake --build build --config Debug --target
atx-engine-tests (/W4 /permissive- /WX clean) + ctest --test-dir build -C Debug -R <Suite>.
```

---

## §3 — The math (reproduced so a zero-context engineer can implement it)

### 3.1 `stats_ext` numeric base (S1-1)

- **Standard-normal CDF:** `Φ(x) = 0.5 · std::erfc(-x · M_SQRT1_2)` where `M_SQRT1_2 = 0.70710678118654752440` (1/√2; define as a named `constexpr` — `<cmath>`'s `M_SQRT1_2` is non-portable under `/permissive-`). `Φ(0)=0.5`, `Φ(1.959963985)≈0.975`.
- **Inverse normal CDF (probit) `Φ⁻¹(p)`, p∈(0,1):** Acklam's rational approximation (≈1.15e-9 abs error), refined by one Halley step over `std::erfc`. Coefficients (exact):
  ```
  a = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02, 1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00}
  b = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02, 6.680131188771972e+01, -1.328068155288572e+01}
  c = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00, -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00}
  d = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00}
  p_low = 0.02425, p_high = 1 - p_low
  ```
  Lower region (`p<p_low`): `q=sqrt(-2 ln p); x=(((((c0 q+c1)q+c2)q+c3)q+c4)q+c5)/((((d0 q+d1)q+d2)q+d3)q+1)`.
  Central (`p_low≤p≤p_high`): `q=p-0.5; r=q*q; x=(((((a0 r+a1)r+a2)r+a3)r+a4)r+a5)q/(((((b0 r+b1)r+b2)r+b3)r+b4)r+1)`.
  Upper (`p>p_high`): `q=sqrt(-2 ln(1-p)); x=-(((((c0 q+c1)q+c2)q+c3)q+c4)q+c5)/((((d0 q+d1)q+d2)q+d3)q+1)`.
  Halley refinement: `e=Φ(x)-p; u=e·sqrt(2π)·exp(x²/2); x -= u/(1+x·u/2)`. `Φ⁻¹(0.975)≈1.959963985`.
- **Population moments over a stream `r` (length n):** `μ=Σr/n`, `σ²=Σ(r−μ)²/n`, `σ=√σ²`. **Skewness** `γ3 = (1/n)Σ((r−μ)/σ)³`; **excess kurtosis** `κ = (1/n)Σ((r−μ)/σ)⁴ − 3`. (Population/biased moments — consistent with `combine`'s population-std Sharpe. Document the convention.) Degenerate `σ==0` ⇒ `γ3=κ=0`.
- **Exact median** of a span: copy → `std::nth_element` → for even n average the two central order statistics (deterministic).
- **`returns_from_equity(span<const f64> equity) → vector<f64>`:** simple per-step returns `eq[i]/eq[i-1] − 1` for `i≥1` (length n−1); guards `eq[i-1]<=0` → 0. (`BacktestResult.equity_curve` is f64 LEVELS — callers diff to returns via this.)

### 3.2 Performance metrics (S1-1) — reuse + additions

Let `r = pnl[1..T)` (exclude the structural `pnl[0]==0`; T−1 usable returns), `pp = periods_per_year` (default 252). All sums ascending-index.
- **Sharpe** (mirror `combine`, cross-checked): `σ==0 ? 0 : √pp · mean(r)/std_pop(r)`.
- **Sortino:** downside deviation `dd = √( mean( min(rᵢ,0)² ) )` over `r` (target 0); `dd==0 ? 0 : √pp · mean(r)/dd`.
- **Max drawdown:** reuse `combine`'s value (call `combine::compute_metrics(...).drawdown` when positions available, else replicate `combine::detail::max_drawdown`'s equity=cumprod(1+r) peak-to-trough fraction and cross-check). Returns a positive fraction.
- **Calmar:** `annualized_return / max(max_dd, eps)`, `annualized_return = pp · mean(r)`.
- **Information ratio (vs benchmark `b`, same length as `r`, optional):** active `a = r − b`; `std_pop(a)==0 ? 0 : √pp · mean(a)/std_pop(a)`. If no benchmark → `NaN` (documented "requires benchmark").
- **Appraisal ratio (vs a factor/residual series `resid`, optional):** `√pp · mean(resid)/std_pop(resid)`; if absent → `NaN`.
- **Hit rate:** `count(rᵢ>0) / (T−1)`.
- **Holding days / turnover / fitness:** NOT recomputed — taken from `combine::compute_metrics` when positions are available (carried on the struct as an optional). When only a bare pnl stream is given, these fields are `NaN` (documented).
- **Monthly decomposition (optional dates `span<const Timestamp>` aligned to periods):** group `r` by calendar (year,month); each cell = compounded `∏(1+rᵢ)−1`. Empty when no dates.

### 3.3 Deflated Sharpe Ratio (S1-2) — Bailey & López de Prado (2014)

Inputs: observed (non-annualized, per-period) Sharpe `SR` over `T` returns, skew `γ3`, excess kurtosis `κ` (so raw kurtosis `= κ+3`), number of independent trials `N≥1`, and (optional) the cross-trial variance of the Sharpe estimates `V`. If `V` not supplied, use the single-stream estimate `V̂ = (1/T)·(1 − γ3·SR + ((κ+2)/4)·SR²)` (the variance-of-Sharpe estimator).
- **Probabilistic Sharpe vs benchmark `SR*`:** `PSR(SR*) = Φ( (SR − SR*)·√(T−1) / √(1 − γ3·SR + ((κ+2)/4)·SR²) )`.
- **Expected-max benchmark across N trials:** `SR*_N = √V · [ (1−γₑ)·Φ⁻¹(1 − 1/N) + γₑ·Φ⁻¹(1 − 1/(N·e)) ]`, Euler–Mascheroni `γₑ = 0.5772156649015329`, `e = std::exp(1.0)`. For `N==1`, `SR*_1 = 0` (no selection) ⇒ DSR == PSR(0).
- **DSR = PSR(SR*_N).** **Haircut Sharpe** = `max(0.0, SR − SR*_N)` (the Sharpe net of the multiple-testing hurdle; document this definition).
- Properties to test (non-tautological): `DSR(N=1)==PSR(0)`; `DSR` strictly decreasing in `N` for fixed `SR>0` (selection penalty bites; `→0` as `N→∞`); `DSR` increasing in `SR`; `PSR(SR*=SR)==0.5`.

### 3.4 PBO via CSCV (S1-3) — Bailey, Borwein, López de Prado, Zhu (2017)

Input: a performance matrix `M` of `N` candidates × `T` periods (per-period return per candidate, row-major `[cand*T + period]`), and an even split count `S` (default 16; require `S` even, `S≤T`, `T%S==0` after trimming — document the trim of trailing periods so `T` divides `S`).
- Partition periods into `S` contiguous equal sub-periods. Enumerate all `C(S, S/2)` combinations of sub-periods as the in-sample (IS) set; the complement is out-of-sample (OOS). **Lexicographic enumeration, no RNG.**
- Per split: per-candidate IS performance = Sharpe over the concatenated IS periods (reuse `eval` Sharpe, non-annualized); pick IS-best candidate `n*` (ascending-index tie-break). Its OOS performance rank among all `N` candidates → relative rank `ω̄ = rank(n*)/(N+1)` (rank 1..N via `stats::partial_rank` over OOS Sharpe, ascending so best=N). Logit `λ = ln(ω̄/(1−ω̄))`.
- **PBO = fraction of splits with `λ ≤ 0`** (IS-best lands at/below the OOS median). Also return the per-split `λ` vector + the logit distribution mean.
- Properties to test (non-vacuous): pure-noise batch (all candidates iid-noise-derived, fixed-seed-free deterministic synthetic) → PBO ≈ 0.5; a batch with one genuine persistent-edge candidate → PBO materially < 0.5. `C(S,S/2)` count is exact (e.g. S=4 ⇒ 6 splits).

### 3.5 Purged + embargoed CPCV (S1-4) — López de Prado AFML Ch. 7

Input: `N` observations each carrying a label span `[t0ᵢ, t1ᵢ)` (for a fitted object, this IS its `[fit_begin,fit_end)`), a group count `K` (default 6), a test-group count `k` (default 2), and an `embargo` fraction `h` (default 0.01 of `N`).
- Partition the `N` observations into `K` contiguous groups. Enumerate all `C(K,k)` test-group combinations (lexicographic). Each combination → `test_idx` = the union of its groups' observation indices; `train_idx` = everything else, **then purge + embargo**:
  - **Purge:** drop any train index `j` whose label span `[t0ⱼ,t1ⱼ)` overlaps the test window `[min t0 over test, max t1 over test)`.
  - **Embargo:** additionally drop train indices within `embargo_len = ceil(h·N)` observations *after* each test block's end.
- Output `vector<CpcvFold>{ vector<usize> train_idx; vector<usize> test_idx; }`, fold order = lexicographic combination order.
- **The structural guarantee (tested):** for every fold, ∀ `j ∈ train_idx`: `[t0ⱼ,t1ⱼ) ∩ (test_window ∪ embargo) == ∅`. Truncation-invariance: a fitted object built on a fold's `train_idx`, applied to `test_idx`, provably never read a test-window row (reuse the `apply_guard` `EXPECT_DEATH` from `phase4_integration_test.cpp`).
- `C(K,k)` count exact (K=6,k=2 ⇒ 15 folds). No RNG.

---

## §4 — Per-unit plan

> Sequential dispatch (S1-1 is the numeric base S1-2/S1-3 reuse; S1-4 reuses the firewall; S1-5 reuses all). Each unit: fresh implementer → spec-compliance review (independent rebuild) → code-quality review → fix loop → ledger SHA backfill, per `superpowers:subagent-driven-development`.

### Task S1-0: Marker + ledger + eval scaffold

**Files:**
- Create: `atx-engine/plans/p1/sprint-1-progress.md`
- Create: `atx-engine/include/atx/engine/eval/fwd.hpp`

- [ ] **Step 1: Write the ledger** `sprint-1-progress.md` from the [`../docs/sprint.md`](../docs/sprint.md) skeleton (copy [`../p0/phase-4b-progress.md`](../p0/phase-4b-progress.md) shape): header (`Base: feat/atx-core-stdlib @ f2d22f4`, worktree choice, started date, source plan link), a "Plan adjustments vs. the source plan" paragraph quoting §0 above, an empty per-unit table (S1-0…S1-5), and an empty sprint-commits table.

- [ ] **Step 2: Write `eval/fwd.hpp`** — forward decls (the `combine/fwd.hpp` pattern):

```cpp
#pragma once
// atx::engine::eval — evaluation & validation spine forward declarations (Sprint S1).
//
// Full definitions live in (added per unit):
//   eval/stats_ext.hpp      — norm_cdf/norm_ppf/skewness/excess_kurtosis/median   (S1-1)
//   eval/perf_metrics.hpp   — ReturnMetrics, compute_return_metrics              (S1-1)
//   eval/deflated_sharpe.hpp— DsrResult, deflated_sharpe                         (S1-2)
//   eval/pbo.hpp            — PboResult, pbo_cscv                                 (S1-3)
//   eval/cpcv.hpp           — CpcvConfig, CpcvFold, cpcv_folds                    (S1-4)
#include "atx/core/types.hpp"
namespace atx::engine::eval {
struct ReturnMetrics;   // perf_metrics.hpp (S1-1)
struct DsrResult;       // deflated_sharpe.hpp (S1-2)
struct PboResult;       // pbo.hpp (S1-3)
struct CpcvConfig;      // cpcv.hpp (S1-4)
struct CpcvFold;        // cpcv.hpp (S1-4)
} // namespace atx::engine::eval
```

- [ ] **Step 3: Commit (marker).**

```bash
git add -- atx-engine/plans/p1/sprint-1-progress.md atx-engine/include/atx/engine/eval/fwd.hpp
git commit -- atx-engine/plans/p1/sprint-1-progress.md atx-engine/include/atx/engine/eval/fwd.hpp \
  -m "docs(s1-0): open sprint-1 evaluation/validation ledger + eval scaffold" \
  -m "Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git show HEAD --stat   # confirm ONLY the two files
git merge-base --is-ancestor HEAD HEAD && echo OK
```
Expected: 2 files, ancestry OK. (`fwd.hpp` need not compile into a test yet; S1-1 includes it.)

---

### Task S1-1: `PerfMetrics` suite + numeric base

**Files:**
- Create: `atx-engine/include/atx/engine/eval/stats_ext.hpp`
- Create: `atx-engine/include/atx/engine/eval/perf_metrics.hpp`
- Test: `atx-engine/tests/eval_stats_ext_test.cpp`, `atx-engine/tests/eval_perf_metrics_test.cpp`

**Scope (from spec S1.1):** "A pure, deterministic metrics computer over an equity curve and over `AlphaStreams::pnl(a)`. Annualized Sharpe (√252), Sortino, max drawdown + Calmar, information ratio / appraisal ratio, hit rate, average holding period (≈1/turnover), and monthly/annual decomposition. Reuse the WQ fitness — don't duplicate. All f64 sums in fixed instrument/date order. Handle the `pnl[0]==0` structural zero (exclude or document — pick one, make it explicit)." Math: §3.1, §3.2.

- [ ] **Step 1: Write the failing `stats_ext` tests** (`eval_stats_ext_test.cpp`, suite `EvalStatsExt`): pin the numeric base against known constants (non-tautological):

```cpp
#include <gtest/gtest.h>
#include <vector>
#include "atx/engine/eval/stats_ext.hpp"
using namespace atx::engine::eval;
TEST(EvalStatsExt, NormCdf_KnownPoints) {
  EXPECT_NEAR(norm_cdf(0.0), 0.5, 1e-12);
  EXPECT_NEAR(norm_cdf(1.959963984540054), 0.975, 1e-9);
  EXPECT_NEAR(norm_cdf(-1.959963984540054), 0.025, 1e-9);
}
TEST(EvalStatsExt, NormPpf_InvertsNormCdf) {
  for (double p : {0.01, 0.25, 0.5, 0.75, 0.975, 0.999}) EXPECT_NEAR(norm_cdf(norm_ppf(p)), p, 1e-9);
  EXPECT_NEAR(norm_ppf(0.975), 1.959963984540054, 1e-7);
}
TEST(EvalStatsExt, SkewKurt_SymmetricIsZero) {
  std::vector<double> s{-2,-1,0,1,2};            // symmetric → skew 0
  EXPECT_NEAR(skewness(s), 0.0, 1e-12);
}
TEST(EvalStatsExt, Median_EvenAndOdd) {
  std::vector<double> a{3,1,2}; std::vector<double> b{4,1,3,2};
  EXPECT_DOUBLE_EQ(median(a), 2.0); EXPECT_DOUBLE_EQ(median(b), 2.5);
}
TEST(EvalStatsExt, ReturnsFromEquity_SimpleSteps) {
  std::vector<double> eq{100,110,99}; auto r = returns_from_equity(eq);
  ASSERT_EQ(r.size(), 2U); EXPECT_NEAR(r[0], 0.10, 1e-12); EXPECT_NEAR(r[1], -0.10, 1e-12);
}
```

- [ ] **Step 2: Run to verify fail.** `cmake --build build --config Debug --target atx-engine-tests` → FAIL ("stats_ext.hpp not found").

- [ ] **Step 3: Implement `stats_ext.hpp`** — `norm_cdf` (§3.1 over `std::erfc`), `norm_ppf` (Acklam coefficients §3.1 + one Halley step), `mean_std_pop`, `skewness`, `excess_kurtosis`, `median` (`std::nth_element` on a copy), `returns_from_equity`. All `[[nodiscard]] inline`, `span<const f64>` in. Named `constexpr` for `M_SQRT1_2`, `γₑ` not here (that's S1-2). `// SAFETY:` on the `nth_element` copy + the `σ==0` guards.

- [ ] **Step 4: Run to verify pass.** `ctest --test-dir build -C Debug -R EvalStatsExt --output-on-failure` → all pass.

- [ ] **Step 5: Write the failing `perf_metrics` tests** (`eval_perf_metrics_test.cpp`, suite `EvalPerfMetrics`):

```cpp
#include <gtest/gtest.h>
#include <vector>
#include "atx/engine/eval/perf_metrics.hpp"
#include "atx/engine/combine/metrics.hpp"
using namespace atx::engine::eval;
TEST(EvalPerfMetrics, Sharpe_MatchesCombineBitForBit) {
  std::vector<double> pnl{0.0, 0.01, -0.005, 0.02, 0.0, 0.015};   // pnl[0]==0 structural
  auto rm = compute_return_metrics(pnl, {});
  // reuse-not-duplicate: eval Sharpe == combine Sharpe on the same stream (positions empty → turnover path NaN, sharpe still defined)
  auto cm = atx::engine::combine::compute_metrics(pnl, {}, /*n_instruments=*/0U, /*book_size=*/1.0);
  EXPECT_EQ(rm.sharpe, cm.sharpe);   // BIT-equal — proves no second Sharpe convention
}
TEST(EvalPerfMetrics, Sortino_DownsideOnly) {
  std::vector<double> pnl{0.0, 0.01, 0.02, 0.03};  // no downside → dd 0 → sortino 0 by convention
  EXPECT_EQ(compute_return_metrics(pnl, {}).sortino, 0.0);
}
TEST(EvalPerfMetrics, HitRate_CountsPositive) {
  std::vector<double> pnl{0.0, 1.0, -1.0, 1.0, 1.0};   // 3 of 4 positive
  EXPECT_NEAR(compute_return_metrics(pnl, {}).hit_rate, 0.75, 1e-12);
}
TEST(EvalPerfMetrics, Calmar_AnnRetOverMaxDD) {
  std::vector<double> pnl{0.0, 0.1, -0.2, 0.1};
  auto rm = compute_return_metrics(pnl, {});
  EXPECT_GT(rm.max_dd, 0.0); EXPECT_TRUE(std::isfinite(rm.calmar));
}
TEST(EvalPerfMetrics, Deterministic_TwoRunsByteIdentical) {
  std::vector<double> pnl{0.0,0.01,-0.02,0.03,-0.01,0.02};
  auto a = compute_return_metrics(pnl, {}); auto b = compute_return_metrics(pnl, {});
  EXPECT_EQ(a.sharpe,b.sharpe); EXPECT_EQ(a.sortino,b.sortino); EXPECT_EQ(a.max_dd,b.max_dd);
}
TEST(EvalPerfMetrics, EmptyAndSingle_Degenerate) {
  EXPECT_NO_FATAL_FAILURE((void)compute_return_metrics(std::span<const double>{}, {}));
  std::vector<double> one{0.0}; (void)compute_return_metrics(one, {});  // only structural zero → no usable returns
}
```

- [ ] **Step 6: Run to verify fail.** Build → FAIL ("perf_metrics.hpp not found").

- [ ] **Step 7: Implement `perf_metrics.hpp`** — `struct ReturnMetricsCfg { atx::f64 periods_per_year = 252.0; std::span<const atx::f64> benchmark{}; std::span<const atx::f64> residual{}; std::span<const atx::core::time::Timestamp> dates{}; };` and `struct ReturnMetrics { atx::f64 sharpe, sortino, max_dd, calmar, ir, appraisal, hit_rate; /* monthly */ std::vector<MonthlyCell> monthly; };` (`MonthlyCell{ i32 year; u8 month; f64 ret; }`). `[[nodiscard]] ReturnMetrics compute_return_metrics(std::span<const atx::f64> pnl, const ReturnMetricsCfg& cfg);` — exclude `pnl[0]` (document); Sharpe mirrors combine (cross-checked by Step 5's test); Sortino/Calmar/IR/appraisal/hit-rate/monthly per §3.2; turnover/fitness/holding NOT here (NaN, documented — use `combine::compute_metrics` when positions exist). Reuse `eval::stats_ext`. Module-comment the `pnl[0]==0` exclusion + the "reuse combine for turnover/fitness" boundary.

- [ ] **Step 8: Run to verify pass.** `ctest --test-dir build -C Debug -R "EvalStatsExt|EvalPerfMetrics" --output-on-failure` → all pass. Confirm full suite not regressed: `ctest --test-dir build -C Debug` green.

- [ ] **Step 9: Commit + ledger row.**

```bash
git add -- atx-engine/include/atx/engine/eval/stats_ext.hpp atx-engine/include/atx/engine/eval/perf_metrics.hpp \
  atx-engine/tests/eval_stats_ext_test.cpp atx-engine/tests/eval_perf_metrics_test.cpp \
  atx-engine/plans/p1/sprint-1-progress.md
git commit -- <those paths> -m "feat(s1-1): PerfMetrics suite + numeric base (norm_cdf/skew/kurt/median)" \
  -m "Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git show HEAD --stat   # confirm only your files
```
**Ledger row S1-1:** `EvalStatsExt N/N/0/0 + EvalPerfMetrics M/M/0/0`; note "Sharpe bit-equal to combine (no second convention); pnl[0] excluded; turnover/fitness via combine::compute_metrics".

---

### Task S1-2: Deflated Sharpe Ratio + haircut

**Files:**
- Create: `atx-engine/include/atx/engine/eval/deflated_sharpe.hpp`
- Test: `atx-engine/tests/eval_deflated_sharpe_test.cpp`

**Scope (from spec S1.2):** "Bailey/LdP Deflated Sharpe Ratio: given observed Sharpe, the number of trials N, and the variance/skew/kurtosis of the return stream, compute the probability the true Sharpe > 0 after correcting for selection across N trials. The haircut Sharpe. Trial count N supplied by the caller." Math: §3.3.

- [ ] **Step 1: Write the failing tests** (`eval_deflated_sharpe_test.cpp`, suite `EvalDsr`):

```cpp
#include <gtest/gtest.h>
#include <vector>
#include "atx/engine/eval/deflated_sharpe.hpp"
using namespace atx::engine::eval;
// inputs: observed per-period SR, T returns, skew, excess kurtosis, N trials
TEST(EvalDsr, N1_EqualsProbabilisticSharpeAtZero) {
  auto d = deflated_sharpe(/*sr=*/0.10, /*T=*/250, /*skew=*/0.0, /*exkurt=*/0.0, /*N=*/1, /*var=*/{});
  EXPECT_NEAR(d.dsr, probabilistic_sharpe(0.10, 0.0, 250, 0.0, 0.0), 1e-12);
}
TEST(EvalDsr, SelectionPenaltyBites_DecreasingInN) {
  double prev = 1.0;
  for (std::size_t N : {1U, 10U, 100U, 1000U, 100000U}) {
    auto d = deflated_sharpe(0.12, 500, -0.2, 3.0, N, {});
    EXPECT_LE(d.dsr, prev + 1e-12); prev = d.dsr;
  }
  EXPECT_LT(deflated_sharpe(0.12,500,-0.2,3.0,100000U,{}).dsr, 0.5);  // huge N kills a modest edge
}
TEST(EvalDsr, IncreasingInObservedSharpe) {
  EXPECT_LT(deflated_sharpe(0.05,500,0,0,100,{}).dsr, deflated_sharpe(0.20,500,0,0,100,{}).dsr);
}
TEST(EvalDsr, PsrAtBenchmarkEqualSharpeIsHalf) {
  EXPECT_NEAR(probabilistic_sharpe(0.10, 0.10, 250, 0.0, 0.0), 0.5, 1e-9);
}
TEST(EvalDsr, Haircut_NonNegativeAndBelowSharpe) {
  auto d = deflated_sharpe(0.15, 750, -0.1, 2.0, 500, {});
  EXPECT_GE(d.haircut_sharpe, 0.0); EXPECT_LE(d.haircut_sharpe, 0.15);
}
TEST(EvalDsr, ReferenceValue_CitedConstant) {
  // Reference computed offline (Bailey-LdP formula, see header citation): SR=0.5/sqrt(12) monthly-ish proxy.
  // Implementer: embed the offline-computed DSR literal + cite the computation in a comment. EXPECT_NEAR 1e-6.
  auto d = deflated_sharpe(0.0808, 120, -0.5, 3.0, 100, {});
  EXPECT_NEAR(d.dsr, /*REF=*/0.0 /* implementer: replace with cited offline value */, 1e-6);
}
```
*(Step-1 note for the implementer: compute the `ReferenceValue` literal offline from the §3.3 formula with a second tool, embed it + cite the computation in a code comment so the test is not a tautology against your own implementation.)*

- [ ] **Step 2: Run to verify fail.** Build → FAIL.

- [ ] **Step 3: Implement `deflated_sharpe.hpp`** — `struct DsrResult { atx::f64 psr; atx::f64 sr_star; atx::f64 dsr; atx::f64 haircut_sharpe; };`. Free functions: `probabilistic_sharpe(sr, sr_star, T, skew, exkurt)` (§3.3 PSR over `eval::norm_cdf`); `expected_max_sharpe(N, var)` (§3.3 SR*_N over `eval::norm_ppf`, `γₑ`, `e`; `N==1 ⇒ 0`); `deflated_sharpe(sr, T, skew, exkurt, N, std::optional<f64> var)` returns the full `DsrResult` (uses `V̂` when `var` absent). Named `constexpr atx::f64 kEulerMascheroni = 0.5772156649015329;`. Reuse `eval::stats_ext`. Header cites Bailey & López de Prado (2014) "The Deflated Sharpe Ratio".

- [ ] **Step 4: Run to verify pass.** `ctest --test-dir build -C Debug -R EvalDsr --output-on-failure` → pass. Full suite green.

- [ ] **Step 5: Commit + ledger row** (`feat(s1-2): deflated Sharpe ratio + haircut`; explicit pathspecs; `git show HEAD --stat`).

---

### Task S1-3: PBO via combinatorially-symmetric cross-validation

**Files:**
- Create: `atx-engine/include/atx/engine/eval/pbo.hpp`
- Test: `atx-engine/tests/eval_pbo_test.cpp`

**Scope (from spec S1.3):** "CSCV: partition the trial matrix (candidates × periods) into S combinatorial splits, estimate the probability that the in-sample best is below-median out-of-sample. PBO is the headline overfit number for a batch. Deterministic split enumeration (no RNG)." Math: §3.4.

- [ ] **Step 1: Write the failing tests** (`eval_pbo_test.cpp`, suite `EvalPbo`):

```cpp
#include <gtest/gtest.h>
#include <vector>
#include "atx/engine/eval/pbo.hpp"
using namespace atx::engine::eval;
// Build a deterministic noise matrix: candidate c, period t -> a fixed pseudo-pattern (NO rng), zero-mean.
static std::vector<double> noise_matrix(std::size_t N, std::size_t T) {
  std::vector<double> m(N*T);
  for (std::size_t c=0;c<N;++c) for (std::size_t t=0;t<T;++t)
    m[c*T+t] = std::sin(0.7*double(c+1) + 0.3*double(t)) - std::sin(0.7*double(c+1)); // bounded, ~zero-mean, no edge
  return m;
}
TEST(EvalPbo, SplitCount_IsCombinatorial) {
  auto r = pbo_cscv(noise_matrix(8, 16), 8, /*S=*/4);   // C(4,2) = 6 splits
  EXPECT_EQ(r.split_logits.size(), 6U);
}
TEST(EvalPbo, PureNoise_AboutHalf) {
  auto r = pbo_cscv(noise_matrix(20, 64), 20, /*S=*/8);   // C(8,4)=70 splits
  EXPECT_NEAR(r.pbo, 0.5, 0.20);                          // no real edge → ~coin flip
}
TEST(EvalPbo, OneGenuineEdge_MateriallyBelowHalf) {
  auto m = noise_matrix(20, 64);
  for (std::size_t t=0;t<64;++t) m[/*cand 0*/ 0*64 + t] += 0.5;  // candidate 0 has a persistent edge every period
  auto r = pbo_cscv(m, 20, 8);
  EXPECT_LT(r.pbo, 0.30);                                  // the genuine edge persists OOS → low overfit prob
}
TEST(EvalPbo, Deterministic_TwoRunsEqual) {
  auto a = pbo_cscv(noise_matrix(12,32),12,4); auto b = pbo_cscv(noise_matrix(12,32),12,4);
  EXPECT_EQ(a.pbo, b.pbo);
}
TEST(EvalPbo, OddSplitOrTooFew_Errors) {
  EXPECT_TRUE(pbo_cscv_checked(noise_matrix(4,16),4,/*S=*/3).is_err());   // S must be even
}
```

- [ ] **Step 2: Run to verify fail.** Build → FAIL.

- [ ] **Step 3: Implement `pbo.hpp`** — `struct PboResult { atx::f64 pbo; std::vector<atx::f64> split_logits; atx::f64 mean_logit; };`. `[[nodiscard]] PboResult pbo_cscv(std::span<const atx::f64> perf /* N*T row-major */, atx::usize n_candidates, atx::usize n_splits);` + a `Result`-returning `pbo_cscv_checked(...)` that `Err`s on `S` odd / `S>T` / `n_candidates<2`. Lexicographic `C(S,S/2)` enumeration (a `next_combination` over index array — no RNG); per-split IS/OOS Sharpe via `eval` Sharpe; OOS rank via `atx::core::stats::partial_rank`; logit per §3.4; PBO = fraction `λ≤0`. Trim trailing periods so `S | T` (document). Module-comment cites Bailey et al. (2017) "The Probability of Backtest Overfitting".

- [ ] **Step 4: Run to verify pass.** `ctest --test-dir build -C Debug -R EvalPbo --output-on-failure` → pass. Full suite green.

- [ ] **Step 5: Commit + ledger row** (`feat(s1-3): PBO via combinatorially-symmetric CV`).

---

### Task S1-4: Purged + embargoed CPCV harness

**Files:**
- Create: `atx-engine/include/atx/engine/eval/cpcv.hpp`
- Test: `atx-engine/tests/eval_cpcv_test.cpp`

**Scope (from spec S1.4):** "Extend P4's walk-forward / fit-apply firewall into a full Combinatorial Purged Cross-Validation harness: train/validate/test splits with purging (drop training observations whose labels overlap the test window) and an embargo (gap after each test block) so leakage across the fit/apply boundary is structurally impossible — the same truncation-invariance proof P4 uses, generalized to many folds. Reuses the P4 fitted-object `[fit_begin, fit_end)` window contract." Math: §3.5.

- [ ] **Step 1: Write the failing tests** (`eval_cpcv_test.cpp`, suites `EvalCpcv` + `EvalCpcvDeathTest`):

```cpp
#include <gtest/gtest.h>
#include <vector>
#include "atx/engine/eval/cpcv.hpp"
#include "atx/engine/risk/factor_model.hpp"   // for the apply_guard EXPECT_DEATH reuse
using namespace atx::engine::eval;
// N observations with contiguous unit label spans [i, i+1) (the simplest fitted-object window).
static std::vector<LabelSpan> unit_spans(std::size_t n){ std::vector<LabelSpan> s(n);
  for (std::size_t i=0;i<n;++i) s[i] = {i, i+1}; return s; }
TEST(EvalCpcv, FoldCount_IsCombinatorial) {
  auto folds = cpcv_folds(unit_spans(60), CpcvConfig{/*K=*/6, /*k=*/2, /*embargo=*/0.0});
  EXPECT_EQ(folds.size(), 15U);  // C(6,2)
}
TEST(EvalCpcv, NoTrainLabelOverlapsTestWindow_Purged) {
  auto spans = unit_spans(60);
  for (const auto& f : cpcv_folds(spans, CpcvConfig{6,2,0.0})) {
    atx::usize lo = spans[f.test_idx.front()].t0, hi = spans[f.test_idx.back()].t1;
    for (atx::usize j : f.train_idx) { EXPECT_FALSE(spans[j].t0 < hi && lo < spans[j].t1); } // no overlap
  }
}
TEST(EvalCpcv, Embargo_DropsTrainAfterTestBlock) {
  auto spans = unit_spans(60);
  auto no_emb = cpcv_folds(spans, CpcvConfig{6,2,0.0});
  auto emb    = cpcv_folds(spans, CpcvConfig{6,2,0.10});  // 10% embargo
  EXPECT_LT(emb[0].train_idx.size(), no_emb[0].train_idx.size());  // embargo removes more train obs
}
TEST(EvalCpcv, Deterministic_TwoRunsEqual) {
  auto a = cpcv_folds(unit_spans(48), CpcvConfig{6,2,0.05});
  auto b = cpcv_folds(unit_spans(48), CpcvConfig{6,2,0.05});
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i=0;i<a.size();++i){ EXPECT_EQ(a[i].train_idx, b[i].train_idx); EXPECT_EQ(a[i].test_idx, b[i].test_idx);} }
TEST(EvalCpcvDeathTest, ApplyInsideFitWindowAborts) {
  // reuse the phase4 caller-side firewall idiom: a model fit on [0, fit_end) must not be applied at date < fit_end
  using atx::engine::risk::FactorModel;
  // build a trivial FactorModel on a fold's train window, then assert apply-inside-window aborts:
  auto guard = [](atx::usize apply_date, atx::usize fit_end){ ATX_ASSERT(apply_date >= fit_end); };
  EXPECT_DEATH(guard(/*apply_date=*/0U, /*fit_end=*/8U), ".*");
}
```

- [ ] **Step 2: Run to verify fail.** Build → FAIL.

- [ ] **Step 3: Implement `cpcv.hpp`** — `struct LabelSpan { atx::usize t0; atx::usize t1; };`, `struct CpcvConfig { atx::usize n_groups = 6; atx::usize n_test_groups = 2; atx::f64 embargo = 0.01; };`, `struct CpcvFold { std::vector<atx::usize> train_idx; std::vector<atx::usize> test_idx; };`. `[[nodiscard]] std::vector<CpcvFold> cpcv_folds(std::span<const LabelSpan> spans, const CpcvConfig& cfg);` — contiguous K-group partition, lexicographic `C(K,k)` enumeration, purge (overlap test §3.5), embargo (`ceil(embargo·N)` after each test block). All ascending-index, no RNG. `// SAFETY:` on the overlap predicate (half-open intervals). Header cites López de Prado AFML Ch. 7; documents the reuse of the `[fit_begin,fit_end)` contract (a `LabelSpan` IS a fitted object's window).

- [ ] **Step 4: Run to verify pass.** `ctest --test-dir build -C Debug -R "EvalCpcv" --output-on-failure` → pass (incl. the death test). Full suite green.

- [ ] **Step 5: Commit + ledger row** (`feat(s1-4): purged + embargoed CPCV harness`).

---

### Task S1-5: Bias-audit gate battery + sprint close

**Files:**
- Create: `atx-engine/include/atx/engine/validation/bias_audit.hpp`
- Test: `atx-engine/tests/validation_bias_audit_test.cpp`
- Modify: `atx-engine/plans/p1/sprint-1-progress.md`, `atx-engine/plans/p1/ROADMAP.md` (S1 row + Last-reviewed), `atx-engine/plans/p1/sprint-1-evaluation-validation.md` (mark closed), create `atx-engine/plans/p1/sprint-1.md` (user reference)

**Scope (from spec S1.5):** "A reusable set of assertions any sprint can drop into a test: survivorship (a delisted symbol's PnL is present and freezes), look-ahead (truncation invariance — output ≤ t identical with/without > t), and snooping (a deliberately-overfit synthetic alpha is caught by DSR/PBO — non-vacuous). Package as a small header (`validation/bias_audit.hpp`) the factory and combiner reuse verbatim. Sprint close ceremony."

- [ ] **Step 1: Write the failing tests** (`validation_bias_audit_test.cpp`, suite `ValidationBiasAudit`):

```cpp
#include <gtest/gtest.h>
#include <vector>
#include "atx/engine/validation/bias_audit.hpp"
using namespace atx::engine::validation;
TEST(ValidationBiasAudit, NoLookahead_PassesTruncationInvariant) {
  // recompute(t_cut) returns the output for dates <= t_cut; a causal fn is invariant to future rows.
  auto causal = [](std::size_t n_visible){ std::vector<double> out; for (std::size_t i=0;i<n_visible;++i) out.push_back(double(i)); return out; };
  EXPECT_TRUE(check_no_lookahead(/*full_n=*/10, /*cut=*/6, causal));   // ≤6 identical with/without >6
}
TEST(ValidationBiasAudit, NoLookahead_CatchesLeak) {
  auto leaky = [](std::size_t n_visible){ std::vector<double> out; for (std::size_t i=0;i<6;++i) out.push_back(double(n_visible)); return out; }; // depends on future count
  EXPECT_FALSE(check_no_lookahead(10, 6, leaky));    // NON-VACUOUS: a leak is caught
}
TEST(ValidationBiasAudit, Survivorship_DelistedPresentAndFrozen) {
  std::vector<double> pnl{0.0, 0.01, 0.02, 0.0, 0.0};   // delisted after period 2 → frozen zeros
  EXPECT_TRUE(check_survivorship_frozen(pnl, /*delist_period=*/2));
  std::vector<double> bad{0.0, 0.01, 0.02, 0.03, 0.0};  // trades after delist → survivorship bias
  EXPECT_FALSE(check_survivorship_frozen(bad, 2));
}
TEST(ValidationBiasAudit, Snooping_CatchesPlantedOverfit) {
  // a planted-overfit batch (one in-sample-lucky, OOS-dead candidate among noise) → high PBO / low DSR.
  EXPECT_TRUE(catches_overfit_synthetic());   // helper builds the planted batch + asserts DSR/PBO flag it
}
```

- [ ] **Step 2: Run to verify fail.** Build → FAIL.

- [ ] **Step 3: Implement `bias_audit.hpp`** — header-only `namespace atx::engine::validation`:
  - `template <class Recompute> [[nodiscard]] bool check_no_lookahead(usize full_n, usize cut, Recompute recompute)` — calls `recompute(full_n)` and `recompute(cut)`, compares the first `min(cut, …)` outputs bit-for-bit (the truncation-invariance generalization of `phase4_integration_test.cpp`).
  - `[[nodiscard]] bool check_survivorship_frozen(std::span<const f64> pnl, usize delist_period)` — asserts all `pnl[i]==0` for `i>delist_period` (present + frozen, no post-delist trading).
  - `[[nodiscard]] bool catches_overfit_synthetic()` — builds a deterministic planted-overfit trial matrix (one candidate IS-lucky/OOS-dead among noise) and asserts `eval::pbo_cscv` flags it (PBO high) AND `eval::deflated_sharpe` of the IS-best haircuts to ~0; returns true iff both fire. Reuses `eval/pbo.hpp` + `eval/deflated_sharpe.hpp`.
  These are test-support helpers (consumed by tests across later sprints) — document "reuse verbatim in S3/S5".

- [ ] **Step 4: Run to verify pass.** `ctest --test-dir build -C Debug -R ValidationBiasAudit --output-on-failure` → pass. **Full suite green** `ctest --test-dir build -C Debug`.

- [ ] **Step 5: Sprint close ceremony** (one tight series, per [`../docs/sprint.md`](../docs/sprint.md) §"Sprint close"):
  1. Fill the ledger: per-unit rows + sprint-commits table + test counts; "What S1 proves / Next sprint priorities" baton.
  2. Lift residuals to the p1 ROADMAP future-work backlog: **the Pattern-B atx-core request** (`norm_cdf`/`norm_ppf`/`skewness`/`kurtosis`/`median` → L6 stats), `appraisal-vs-real-factor` wiring (S1.1 takes a residual span today), and `monthly-decomposition-needs-dates` (AlphaStreams carries no dates).
  3. Update `p1/ROADMAP.md`: S1 row `⏳ → ✅ <close-sha>`, bump `Last reviewed`.
  4. Mark `sprint-1-evaluation-validation.md` `Status: ✅ closed`; create `sprint-1.md` user reference (the `eval::` + `validation::` public API + the DSR/PBO/CPCV guarantees, the `phase-N.md` pattern).

- [ ] **Step 6: Commit close.**

```bash
git add -- atx-engine/include/atx/engine/validation/bias_audit.hpp atx-engine/tests/validation_bias_audit_test.cpp \
  atx-engine/plans/p1/sprint-1-progress.md atx-engine/plans/p1/ROADMAP.md \
  atx-engine/plans/p1/sprint-1-evaluation-validation.md atx-engine/plans/p1/sprint-1.md
git commit -- <those paths> -m "docs(s1-close): close sprint-1 evaluation/validation — 6 units, <M> tests" \
  -m "Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git show HEAD --stat   # confirm only your files
```
(In-place workflow: no `--no-ff` merge. Do NOT push unless asked.)

---

## §5 — Exit criteria · invariants · dependencies · NOT-in-scope · baton

**Exit criteria** (updated from the spec):
- `ReturnMetrics` reproduces hand-computed Sharpe/Sortino/DD/Calmar/IR/hit-rate on a fixture; Sharpe is **bit-equal to `combine::compute_metrics`** (no second convention); byte-stable across two runs; fixed-order sums.
- DSR + haircut match a **cited offline reference** to 1e-6; `norm_cdf`/`norm_ppf` match known constants to 1e-9; DSR → 0 as N → ∞ for fixed observed Sharpe (selection penalty bites, tested monotone).
- PBO on a pure-noise batch → ≈ 0.5 (±0.20); on a one-genuine-edge batch → materially < 0.30 (non-vacuous); split count == `C(S,S/2)` exactly.
- CPCV harness is purge+embargo-correct: a per-fold overlap assertion proves no train label overlaps the test window+embargo; fold count == `C(K,k)`; the `apply_guard` `EXPECT_DEATH` proves the firewall.
- Bias-audit battery **catches** a planted leak/overfit and **passes** an honest input (every assert proven non-vacuous).
- All `/W4 /permissive- /WX` clean; new `*_test.cpp` per unit; full engine suite stays green per unit.

**Invariants proven:** determinism (fixed-order sums, no RNG, lexicographic split enumeration), no look-ahead (purge+embargo + truncation-invariance), differential correctness (DSR/PBO vs cited references; Sharpe vs combine). This sprint IS the no-look-ahead/no-snooping proof machine the rest of p1 reuses.

**Dependencies:** Upstream p0 — `combine::compute_metrics`/`AlphaMetrics`, `alpha::AlphaStreams`, `BacktestResult`, the `[fit_begin,fit_end)` firewall, `atx::core::stats` (`RunningVariance`, `partial_rank`, `argsort`), `atx::core::hash`. **No P4 *blocking* dependency** (P4 is DONE; S1 reuses it). **Pattern-B atx-core edge:** `norm_cdf`/`norm_ppf`/`skewness`/`kurtosis`/`median` are ABSENT in atx-core — built engine-local in `eval/stats_ext.hpp` (the `combine/correlation.hpp` precedent), with an upstream request recorded as a close residual (NOT a blocker — S1 ships end-to-end).

**Explicitly NOT in this sprint:** no metric visualization (headless f64/struct artifacts only); no new combiner/optimizer (P4/S5/S7); no cost calibration (S6 — S1 consumes whatever cost the streams carry); no parallelism (S2 parallelizes CPCV folds later); no AlphaStreams date-plumbing (monthly decomposition takes optional dates, NaN-empty without — S7 reporting wires real dates).

**Baton → next:** S1 hands **S3** a pool-aware deflated fitness to mine against and **S5** a CPCV harness + DSR/PBO admission gate to train/admit learned models inside. The `validation::bias_audit` asserts are reused verbatim by S3/S4/S5. Per the p1 ROADMAP DAG, **S2 (parallel) and S6 (cost) can open concurrently** with S1 (no P4 dependency).

---

## §6 — Self-review (run against the spec)

- **Spec coverage:** S1.0→S1-0, S1.1→S1-1, S1.2→S1-2, S1.3→S1-3, S1.4→S1-4, S1.5→S1-5. Every spec unit + exit criterion maps to a task. ✅
- **Placeholder scan:** the one intentional fill-in is the S1-2 `ReferenceValue` literal (the implementer computes it offline + cites it — flagged explicitly so it is not a tautology, not a hidden TODO). Every API surface + test body is concrete. ✅
- **Type consistency:** `ReturnMetrics`/`ReturnMetricsCfg` (S1-1), `DsrResult`/`probabilistic_sharpe`/`expected_max_sharpe`/`deflated_sharpe` (S1-2), `PboResult`/`pbo_cscv`/`pbo_cscv_checked` (S1-3), `LabelSpan`/`CpcvConfig`/`CpcvFold`/`cpcv_folds` (S1-4), `check_no_lookahead`/`check_survivorship_frozen`/`catches_overfit_synthetic` (S1-5) — names are consistent across the math (§3), the file table (§1), `fwd.hpp` (S1-0), and the per-unit signatures. ✅
- **Stale-spec fixes applied:** Base SHA (`f2d22f4` not `master`), P4-done reuse (not "assume"), atx-core stats ABSENT → engine-local bridge + Pattern-B residual, `tests/*_test.cpp` flat glob, `eval::` namespace to avoid the `combine::compute_metrics` clash. ✅
