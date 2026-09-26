# W1-E6 / E-14 causal regime cuts

Status: implementation and postimplementation fixture source. No C++ compilation,
test execution, numerical market evaluation, scale sweep or timing claim here.
This slice addresses E-14 regime cuts only; E-12/E-13 and the other E6 gates are
not claimed complete.

`eval/regime_slice.hpp` now defaults to `RegimeSliceRule::ExpandingPastV2`
(`expanding-past-v2`). `LegacyFullSampleV1` (`legacy-full-sample-v1`) explicitly
preserves the former whole-sample rank partition, date-ranked ties, warmup and
empty-regime conventions. Existing numeric/Sharpe routines on the legacy branch
were retained.

V2 precomputes each date's finite-only cross-sectional median market return once.
Positive finite current/prior closes and a finite ratio are required. A date with
no eligible returns stays unavailable, including t=0. Trailing population volatility
at t uses the complete return window [t-window,t), so current/future returns cannot
alter t's label. A chronological two-pass reduction scales only within that past
window; a missing return invalidates that window and cannot poison later clean
ones.

An online ordered multiset of strictly prior finite volatility observations
maintains empirical cuts at floor((n-1)/3) and floor(2(n-1)/3). Classification
precedes insertion of the current volatility. `min_regime_history` defaults to 3
prior finite volatility dates and values below 3 are unavailable. Ties at either
cut go to the middle regime; flat history is not artificially balanced into three
regimes. Ordered insertion costs O(log T), cut iterator updates O(1), and no future
coordinate compression/sort is used. Window reductions still cost O(T*window);
scale and performance acceptance remain unmeasured.

The minimal approved consumer changes are:

- `factory/research_driver.cpp`: passes the complete robustness config; active V2
  folds an explicit recipe into the research digest, including when no survivor is
  admitted. Gate-off and explicit V1 retain their prior digest path.
- `factory/research_driver.hpp`: exposes active rule, window, history count and
  recipe identity in the report. The recipe binds rule/window/history, walk-forward
  count and Sharpe threshold.
- `factory/robust_pipeline.hpp`: uses the same complete config for its subset gate.

`RobustnessVerdict` exposes recipe/version, finite per-regime observation counts
and coverage qualification. V2 requires at least two finite observations in every
regime before it may report robust, avoiding an all-unavailable partition passing
on legacy empty-slice zero Sharpe. Explicit V1 preserves that legacy convention.

Five new fixtures were added after implementation to the existing
`tests/eval/eval_regime_slice_test.cpp` (11 tests total in that TU): truncation and
current/future mutation; an independent sorted-prefix quantile oracle with ties;
flat-history warmup and legacy tie goldens; nonfinite cross-section exclusion and
window recovery; unavailable qualification and observable recipe changes. The
old balanced-tercile fixture now explicitly requests V1. `git diff --check` passed.

Root owns the combined build and independent source review. These source fixtures
do not constitute runtime, scaling or tradeable-alpha evidence.

Independent review correction: finite coverage alone did not prevent a NaN in a
middle regime/window from being ignored by the legacy minimum reduction. V2 now
also requires every full-sample, regime and walk-forward Sharpe to be finite;
descriptive scores and the explicit V1 verdict retain their former arithmetic.
A sixth postimplementation fixture (12 tests total) reproduces the middle-NaN
case with finite counts {4,3,4}, asserts V2 rejection, and freezes the explicit V1
result. `ExpandingVolCuts` is now noncopyable/nonmovable because its cut iterators
refer to its own ordered set. Source-only: this follow-up has not been compiled
or run.
