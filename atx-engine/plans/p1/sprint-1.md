# Sprint S1 — Evaluation & Validation Spine (user reference)

**Status:** ✅ CLOSED 2026-06-07 (`feat/atx-core-stdlib @ 2158a17`). **Spec:** [`sprint-1-evaluation-validation.md`](sprint-1-evaluation-validation.md) · **Plan:** [`sprint-1-evaluation-validation-implementation-plan.md`](sprint-1-evaluation-validation-implementation-plan.md) · **Ledger:** [`sprint-1-progress.md`](sprint-1-progress.md)

S1 is the engine's **truth layer** — the deterministic, non-vacuous scorer every later p1 sprint (S3 mining, S5 ML admission, S7 reporting) calls into to tell signal from noise. Two header-only inline subsystems, no atx-core changes, no RNG.

```
atx/engine/eval/        — atx::engine::eval        (numeric base + metrics + DSR + PBO + CPCV)
atx/engine/validation/  — atx::engine::validation  (reusable bias-audit asserts)
```

---

## Public API

### `eval/stats_ext.hpp` — numeric base (engine-local; atx-core L6 lacks these)
```cpp
[[nodiscard]] f64 norm_cdf(f64 x);                          // Φ via 0.5*erfc(-x/√2)
[[nodiscard]] f64 norm_ppf(f64 p);                          // Φ⁻¹ (Acklam + one Halley step), p∈(0,1)
[[nodiscard]] MeanStd mean_std_pop(span<const f64>);        // population mean/std
[[nodiscard]] f64 skewness(span<const f64>);                // population γ3 (σ==0 ⇒ 0)
[[nodiscard]] f64 excess_kurtosis(span<const f64>);         // population κ = γ4 − 3
[[nodiscard]] f64 median(span<const f64>);                  // exact (nth_element; even-n averages)
[[nodiscard]] std::vector<f64> returns_from_equity(span<const f64>);  // eq[i]/eq[i-1]−1
```

### `eval/perf_metrics.hpp` — performance metrics (reuses `combine::compute_metrics`)
```cpp
struct ReturnMetrics { f64 sharpe, sortino, max_dd, calmar, ir, appraisal, hit_rate; std::vector<MonthlyCell> monthly; };
struct ReturnMetricsCfg { f64 periods_per_year = 252.0; span<const f64> benchmark, residual; span<const Timestamp> dates; };
[[nodiscard]] ReturnMetrics compute_return_metrics(span<const f64> pnl, const ReturnMetricsCfg&);
```
- `sharpe`/`max_dd` are **delegated** to `combine::compute_metrics` — **one** Sharpe convention (`√252·mean/std_pop` over `pnl[1..T)`, the `pnl[0]==0` structural zero excluded), bit-equal to `combine`. Calmar = ann.return/maxDD; Sortino = downside-deviation; IR vs `benchmark`; appraisal vs `residual`; hit-rate = frac(r>0). `monthly` is empty unless `dates` are supplied. Absent benchmark/residual → NaN (documented).

### `eval/deflated_sharpe.hpp` — Deflated Sharpe Ratio (Bailey & López de Prado 2014)
```cpp
struct DsrResult { f64 psr, sr_star, dsr, haircut_sharpe; };
[[nodiscard]] f64 probabilistic_sharpe(f64 sr, f64 sr_star, usize T, f64 skew, f64 exkurt);
[[nodiscard]] f64 expected_max_sharpe(usize N, f64 var);                  // N==1 ⇒ 0
[[nodiscard]] DsrResult deflated_sharpe(f64 sr, usize T, f64 skew, f64 exkurt, usize N, std::optional<f64> var);
```
- `N` = number of independent trials (S3 reports how many expressions it tried; S5 how many configs). DSR = PSR vs the expected-max-Sharpe benchmark across `N` → **the probability the true Sharpe > 0 after correcting for selection**. `haircut_sharpe = max(0, sr − sr_star)`. Guards: T<2 / var_term≤0 → NaN.

### `eval/pbo.hpp` — Probability of Backtest Overfitting via CSCV (Bailey et al. 2017)
```cpp
struct PboResult { f64 pbo; std::vector<f64> split_logits; f64 mean_logit; };
[[nodiscard]] PboResult pbo_cscv(span<const f64> perf /*N*T candidate-major M[c*T+t]*/, usize n_candidates, usize n_splits);
[[nodiscard]] Result<PboResult> pbo_cscv_checked(span<const f64> perf, usize n_candidates, usize n_splits);
```
- Combinatorially-symmetric CV: all `C(S,S/2)` lexicographic IS/OOS splits (no RNG). **PBO ≈ 0.5 = noise (overfit); < 0.30 = a genuine persistent edge.** `pbo_cscv_checked` errs on S odd / S>T / n_candidates<2; `pbo_cscv` fail-fasts (`ATX_ASSERT`).

### `eval/cpcv.hpp` — Purged + embargoed CPCV (López de Prado AFML Ch. 7)
```cpp
struct LabelSpan { usize t0, t1; };                                       // half-open; a fitted object's [fit_begin,fit_end)
struct CpcvConfig { usize n_groups = 6, n_test_groups = 2; f64 embargo = 0.01; };
struct CpcvFold   { std::vector<usize> train_idx, test_idx; };
[[nodiscard]] std::vector<CpcvFold> cpcv_folds(span<const LabelSpan> spans, const CpcvConfig&);
```
- All `C(K,k)` test-group combinations (lexicographic). **Purge** drops a train obs whose label overlaps *any* test obs's label; **embargo** drops train obs within `ceil(embargo·N)` indices after a test block. Leakage is structurally impossible — proven by a per-fold overlap assertion + the fit/apply-firewall `EXPECT_DEATH`. (`embargo ≥ 0` is a checked precondition.)

### `validation/bias_audit.hpp` — reusable gate battery (`atx::engine::validation`)
```cpp
template <class Recompute> [[nodiscard]] bool check_no_lookahead(usize full_n, usize cut, Recompute);  // truncation-invariance
[[nodiscard]] bool check_survivorship_frozen(span<const f64> pnl, usize delist_period);                // delisted PnL present + frozen
[[nodiscard]] bool catches_overfit_synthetic();                                                        // both overfit gates fire (self-test)
```
- Drop-in test-support asserts. `check_no_lookahead` compares `recompute(full_n)` vs `recompute(cut)` bit-for-bit on the first `cut` outputs. **Reuse verbatim in S3/S4/S5** as the shared validity spine.

---

## Guarantees (all proven by non-vacuous tests — fail-on-bad AND pass-on-good)

- **Deterministic:** no RNG; order-fixed reductions; lexicographic split/fold enumeration; two-runs-equal.
- **One Sharpe convention:** `eval` Sharpe is bit-equal to `combine::compute_metrics` (delegated, not re-derived).
- **No snooping:** DSR → 0 as N → ∞; PBO discriminates noise (≈0.5) from edge (<0.30); a cited offline DSR reference matches to 1e-6.
- **No look-ahead:** CPCV purge+embargo make train/test leakage structurally impossible; the bias-audit battery catches planted leaks/overfit.
- Builds under clang-cl `/W4 /permissive- /WX`; 36 tests; full engine suite green; **atx-core unmodified**.

## Known residuals (p1 backlog)

1. **atx-core L6 stats (Pattern B)** — upstream `norm_cdf`/`norm_ppf`/`skewness`/`excess_kurtosis`/`median` (engine-local for now, the `combine/correlation.hpp` precedent).
2. **monthly-decomposition-needs-dates** — `ReturnMetrics::monthly` is empty until a caller plumbs per-observation timestamps (S7 reporting).
3. **appraisal-vs-real-factor** — the appraisal ratio takes a caller-supplied `residual`; downstream must hand in a genuine factor-model residual.
