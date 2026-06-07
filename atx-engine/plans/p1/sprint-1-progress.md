# Sprint 1 — Evaluation & Validation Spine — Implementation Progress

**Status:** 🔵 IN PROGRESS
**Worktree:** in-place (p0 precedent; shared branch — explicit pathspecs only, no push)
**Branch:** `feat/atx-core-stdlib`
**Base:** `feat/atx-core-stdlib` @ `fdc1257` (p0 Phases 1–4 CLOSED at f2d22f4; fdc1257 = concurrent databento disk-layer design doc atop the close)
**Started:** 2026-06-07
**Source plan:** [`sprint-1-evaluation-validation-implementation-plan.md`](sprint-1-evaluation-validation-implementation-plan.md)
**Spec:** [`sprint-1-evaluation-validation.md`](sprint-1-evaluation-validation.md)

---

## Plan adjustments vs. the source plan

Base is `fdc1257` not `master` (the branch carries concurrent databento disk-layer work atop the p0 close at `f2d22f4`). `combine::compute_metrics` is reused for Sharpe; no second Sharpe convention is introduced. atx-core lacks distribution/higher-moment helpers (norm_cdf, norm_ppf, skewness, excess_kurtosis, median), so these are built engine-local in `eval/stats_ext.hpp` following the `combine/correlation.hpp` precedent (small self-contained helper, no atx-core dependency). Pattern-B upstream request (add helpers to atx-core) is recorded as a close residual. New `eval/` and `validation/` directories under `atx-engine/include/atx/engine/`; namespace `atx::engine::eval` throughout. Tests are flat `atx-engine/tests/*_test.cpp` (CONFIGURE_DEPENDS, no CMake edit per unit needed).

---

## Per-unit status

| Unit  | Title                                      | Status      | Commit SHA     | Tests | Notes |
|-------|--------------------------------------------|-------------|----------------|-------|-------|
| S1-0  | Marker + ledger + eval scaffold            | done        | `ec1aaf1`      | —     | fwd.hpp + this ledger (landed in a shared-branch-bundled databento commit; content correct) |
| S1-1  | stats_ext + perf_metrics                   | done        | (this commit)  | 11    | Sharpe bit-equal to combine (delegated, no second convention); pnl[0] excluded; turnover/fitness via combine::compute_metrics |
| S1-2  | Deflated Sharpe (DSR)                      | done        | (this commit)  | 6     | PSR + expected-max-Sharpe + DSR + haircut (Bailey-LdP 2014 §3.3); reuses norm_cdf/norm_ppf; N==1 ⇒ DSR=PSR(0); reference literal derived offline (DSR≈0.0485312) |
| S1-3  | Probability of Backtest Overfitting (CSCV) | done-with-concern | (this commit) | 4/5 | CSCV PBO (Bailey et al. 2017 §3.4); lexicographic next_combination, no RNG; split counts C(4,2)=6, C(8,4)=70 verified. Algorithm validated correct on i.i.d. noise (PBO≈0.457). **CONCERN:** verbatim `PureNoise_AboutHalf` fixture `noise_matrix` is NOT noise — `sin(0.7(c+1)+0.3t)-sin(0.7(c+1))` imposes a persistent per-candidate level edge (constant `-sin(0.7(c+1))`), so a correct CSCV returns PBO≈0 for ALL S (swept S=2..16), not 0.5. Test fixture defect, not an impl bug; tolerance not loosened per brief. Needs fixture repair (e.g. i.i.d.-style generator) to reach 5/5. Also added Rust-spelled `is_ok()/is_err()` to atx-core `error.hpp` (the verbatim test calls `.is_err()`, absent on raw `tl::expected`; additive subclass, full engine suite recompiled green). |
| S1-4  | CPCV fold generator                        | done        | (this commit)  | 6     | Purged + embargoed CPCV (LdP AFML Ch. 7); lexicographic next_combination over C(K,k), no RNG; per-test-observation half-open overlap purge (NOT a global [min t0,max t1) window — that empties non-contiguous folds); forward-in-index embargo (embargo_len=ceil(h*N)). Verified fold[0] train sizes N=60,K=6,k=2: unit h=1=40, purge h=3=38, embargo h=0.10=34. Fold count C(6,2)=15. Deterministic + death test (fit/apply firewall) green. |
| S1-5  | Integration / walk-forward validation      | not started | —              | —     | |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| `ec1aaf1` | S1-0 | docs(s1-0): open sprint-1 evaluation/validation ledger + eval scaffold (bundled into a shared-branch databento commit) |
| (this commit) | S1-1 | feat(s1-1): PerfMetrics suite + numeric base (norm_cdf/norm_ppf/skew/kurt/median) |
| (this commit) | S1-2 | feat(s1-2): deflated Sharpe ratio + haircut (Bailey-LdP 2014) |
| (this commit) | S1-3 | feat(s1-3): PBO via combinatorially-symmetric CV (Bailey et al. 2017) |
| (this commit) | S1-4 | feat(s1-4): purged + embargoed CPCV harness (LdP AFML ch.7) |
