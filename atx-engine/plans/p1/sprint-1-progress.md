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
| S1-3  | Probability of Backtest Overfitting (CSCV) | not started | —              | —     | |
| S1-4  | CPCV fold generator                        | not started | —              | —     | |
| S1-5  | Integration / walk-forward validation      | not started | —              | —     | |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| `ec1aaf1` | S1-0 | docs(s1-0): open sprint-1 evaluation/validation ledger + eval scaffold (bundled into a shared-branch databento commit) |
| (this commit) | S1-1 | feat(s1-1): PerfMetrics suite + numeric base (norm_cdf/norm_ppf/skew/kurt/median) |
| (this commit) | S1-2 | feat(s1-2): deflated Sharpe ratio + haircut (Bailey-LdP 2014) |
