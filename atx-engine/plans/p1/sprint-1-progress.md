# Sprint 1 — Evaluation & Validation Spine — Implementation Progress

**Status:** ✅ CLOSED (2026-06-07)
**Worktree:** in-place (p0 precedent; shared branch — explicit pathspecs only, no push)
**Branch:** `feat/atx-core-stdlib`
**Base:** `feat/atx-core-stdlib` @ `fdc1257` (p0 Phases 1–4 CLOSED at f2d22f4; fdc1257 = concurrent databento disk-layer design doc atop the close)
**Started:** 2026-06-07 · **Closed:** 2026-06-07
**Source plan:** [`sprint-1-evaluation-validation-implementation-plan.md`](sprint-1-evaluation-validation-implementation-plan.md)
**Spec:** [`sprint-1-evaluation-validation.md`](sprint-1-evaluation-validation.md) · **User reference:** [`sprint-1.md`](sprint-1.md)

---

## Plan adjustments vs. the source plan

Base is `fdc1257` not `master` (the branch carries concurrent databento disk-layer work atop the p0 close at `f2d22f4`). `combine::compute_metrics` is reused for Sharpe/drawdown; no second Sharpe convention is introduced. atx-core lacks distribution/higher-moment helpers (norm_cdf, norm_ppf, skewness, excess_kurtosis, median), so these are built engine-local in `eval/stats_ext.hpp` following the `combine/correlation.hpp` precedent (small self-contained helper, no atx-core dependency). Pattern-B upstream request (add helpers to atx-core L6) is recorded as a close residual below. New `eval/` and `validation/` directories under `atx-engine/include/atx/engine/`; namespace `atx::engine::eval` (metrics/DSR/PBO/CPCV) + `atx::engine::validation` (bias audit). Tests are flat `atx-engine/tests/*_test.cpp` (CONFIGURE_DEPENDS, no CMake edit per unit needed).

**Two source-plan defects were found and corrected during execution** (recorded for the implementation-plan revision):
1. **S1-3 PBO test fixture** — the plan's verbatim `noise_matrix` `sin(0.7(c+1)+0.3t) − sin(0.7(c+1))` subtracts a per-candidate *constant*, imposing a permanent cross-sectional level edge → a *correct* CSCV returns PBO≈0 for ALL split counts, never 0.5. Replaced with a genuine zero-edge SplitMix64 hash generator (no per-candidate persistent mean, no RNG). The implementation was correct; the fixture was the bug — tolerances were NOT loosened.
2. **S1-4 CPCV purge** — the plan's "global `[min t0, max t1)` test window" over-purges *non-contiguous* test-group combinations to empty folds. Replaced with the correct **per-test-observation** half-open overlap purge (a train obs is dropped iff its label overlaps ANY test obs's label), which keeps non-contiguous folds usable; the purge test was rewritten to be non-vacuous over *overlapping* (horizon>1) label spans.

---

## Per-unit status

| Unit  | Title                                      | Status | Commit SHA(s)                   | Tests | Notes |
|-------|--------------------------------------------|--------|---------------------------------|-------|-------|
| S1-0  | Marker + ledger + eval scaffold            | ✅ done | `ec1aaf1` (+ plan docs `1c63593`) | —     | `eval/fwd.hpp` + this ledger. `ec1aaf1` was bundled into a concurrent databento commit by a shared-branch `git add .` race; content is correct (verified). |
| S1-1  | stats_ext + perf_metrics                   | ✅ done | `09260bc` + `6ce7033`           | 13    | Sharpe/drawdown **delegated** to `combine::compute_metrics` (bit-equal, no second convention); `pnl[0]==0` excluded; adds Sortino/Calmar/IR/appraisal/hit-rate/monthly over `eval::stats_ext`. norm_ppf(0.975)=1.9599639845; Acklam+Halley probit. |
| S1-2  | Deflated Sharpe (DSR)                       | ✅ done | `e104497` + `0370b4d`           | 7     | PSR + expected-max-Sharpe + DSR + haircut (Bailey-LdP 2014 §3.3); reuses `norm_cdf`/`norm_ppf`; N==1 ⇒ DSR=PSR(0); **reference literal 0.0485312173 derived offline (independent probit, non-tautological)**; degenerate T<2 / var_term≤0 → NaN (guarded, death-tested). |
| S1-3  | PBO via CSCV                                | ✅ done | `457b0be` + `b786cbd` + `2e1af87` | 5   | CSCV PBO (Bailey et al. 2017 §3.4); lexicographic `next_combination`, no RNG; split counts C(4,2)=6, C(8,4)=70. **Non-vacuous: PureNoise PBO=0.5714 vs OneGenuineEdge PBO=0.0000.** Decomposed (<60-line helpers), fail-fast unchecked entry, bounds asserts. *(Fixture defect + an unauthorized atx-core `error.hpp` edit were both reverted/repaired in `b786cbd`; atx-core net diff is empty.)* |
| S1-4  | CPCV fold generator                        | ✅ done | `c4333b0` + `c380b7e`           | 7     | Purged + embargoed CPCV (LdP AFML Ch. 7); lexicographic over C(K,k), no RNG; **per-test-observation** half-open overlap purge; forward-in-index embargo (`ceil(h·N)`). fold[0] train (N=60,K=6,k=2): unit=40 → purge h=3=38 → embargo h=.10=34. C(6,2)=15. Determinism + fit/apply-firewall death test; negative-embargo guard death-tested. |
| S1-5  | Bias-audit gate battery                    | ✅ done | `10e21e0` + `2158a17`           | 4     | Reusable `atx::engine::validation` asserts (`bias_audit.hpp`): `check_no_lookahead` (truncation-invariance, bit-for-bit), `check_survivorship_frozen` (delisted PnL present + frozen post-delist), `catches_overfit_synthetic` (**both gates fire: pbo=1.0, single-test PSR=0.9704, dsr=0.0866**). Reuse verbatim in S3/S4/S5. |
| close | Sprint close ceremony                      | ✅ done | (this commit)                   | —     | fwd.hpp `LabelSpan` decl (final-review nit), ledger, ROADMAP S1 ✅, spec closed, `sprint-1.md` user ref. |

**Totals:** 6 units · **36 new tests** (EvalStatsExt 6 · EvalPerfMetrics 7 · EvalDsr 7 · EvalPbo 5 · EvalCpcv 7 · ValidationBiasAudit 4) · full engine suite **1623/1623 green** under clang-cl `/W4 /permissive- /WX` · **atx-core net diff EMPTY** across the whole sprint range.

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| `1c63593` | S1-0 | docs(s1): sprint-1 implementation plan + reconciled spec |
| `ec1aaf1` | S1-0 | docs(s1-0): open ledger + eval scaffold *(bundled into a concurrent databento commit by a shared-branch race; content correct)* |
| `09260bc` | S1-1 | feat(s1-1): PerfMetrics suite + numeric base (norm_cdf/norm_ppf/skew/kurt/median) |
| `6ce7033` | S1-1 | fix(s1-1): direct includes, explicit month cast, monthly-grid helper, +kurtosis/sortino tests |
| `e104497` | S1-2 | feat(s1-2): deflated Sharpe ratio + haircut (Bailey-LdP 2014) |
| `0370b4d` | S1-2 | fix(s1-2): drop dead include, guard degenerate T<2 + non-positive variance |
| `457b0be` | S1-3 | feat(s1-3): PBO via combinatorially-symmetric CV (Bailey et al. 2017) |
| `b786cbd` | S1-3 | fix(s1-3): revert atx-core error.hpp, repair noise fixture (zero-edge), native has_value() check |
| `2e1af87` | S1-3 | fix(s1-3): decompose split loop, fail-fast unchecked entry, bounds asserts, S=8 split-count test |
| `c4333b0` | S1-4 | feat(s1-4): purged + embargoed CPCV harness (LdP AFML ch.7) |
| `c380b7e` | S1-4 | fix(s1-4): drop dead include, add <utility>, guard negative embargo |
| `10e21e0` | S1-5 | feat(s1-5): reusable bias-audit gate battery (lookahead/survivorship/snooping) |
| `2158a17` | S1-5 | fix(s1-5): drop dead include, tighten anti-persistence comment + helper contracts |
| (this commit) | close | docs(s1-close): close sprint-1 — fwd LabelSpan, ledger, ROADMAP, user reference |

---

## What S1 proves

S1 is the **no-look-ahead / no-snooping proof machine** the rest of p1 reuses. Every validity gate is demonstrated non-vacuous — proven to FAIL on a planted-bad input and PASS on good:
- **Determinism:** no RNG anywhere; order-fixed reductions; lexicographic split/fold enumeration; two-runs-equal tests on perf_metrics / PBO / CPCV.
- **Differential correctness:** eval Sharpe bit-equal to `combine`; DSR vs a cited offline reference (1e-6); norm_cdf/ppf vs known constants (1e-9).
- **No snooping:** DSR → 0 as N → ∞ (selection penalty bites); PBO discriminates noise (≈0.5) from a genuine edge (<0.30).
- **No look-ahead:** CPCV purge+embargo make leakage structurally impossible (per-fold overlap proof + the fit/apply-firewall `EXPECT_DEATH`); the bias-audit battery catches a planted leak/overfit and passes honest input.

## Close residuals → p1 ROADMAP future-work backlog

1. **Pattern-B atx-core L6 stats request** — lift `norm_cdf`/`norm_ppf`/`skewness`/`excess_kurtosis`/`median` (built engine-local in `eval/stats_ext.hpp`) into atx-core L6 `stats`. (The p1 ROADMAP already lists the S1.2/S1.3 → L6 edge.)
2. **monthly-decomposition-needs-dates** — `ReturnMetrics::monthly` is empty without caller-supplied per-observation `dates` aligned to `r=pnl[1..T)`. `AlphaStreams` carries no dates today; S7 reporting (or an S3/S5 caller) must plumb real timestamps for a non-empty monthly grid.
3. **appraisal-vs-real-factor** — the appraisal ratio consumes a caller-supplied `residual` series (a pluggable input); downstream must hand in a genuine factor-model residual for the metric to be meaningful.
4. **DSR haircut NaN-corner (minor)** — for pathological skew/kurtosis driving `var_term ≤ 0`, `psr`/`dsr` correctly go NaN but `haircut = max(0, sr − NaN)` yields 0.0; optionally propagate NaN for symmetry. Outside the documented input domain; not a correctness risk.

## Baton → next

S1 hands **S3** (formulaic factory) a pool-aware **deflated fitness** (`eval::deflated_sharpe`) to mine against and **S5** (learned signals) a **CPCV harness + DSR/PBO admission gate** to train/admit learned models inside. The `validation::` bias-audit asserts are reused verbatim by S3/S4/S5 as the shared validity spine. Public APIs already match downstream data shapes: `pbo_cscv` takes S3's candidate×period matrix; `cpcv_folds` takes the `[fit_begin,fit_end)` label spans P4 fitted objects carry; `deflated_sharpe` takes the moment-parameterized form S5 ML admission needs. Per the p1 ROADMAP DAG, **S2 (parallel) and S6 (cost) can open concurrently** with the factory track (no P4 *blocking* dependency).
