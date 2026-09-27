# Independent E-14 causal regime source review

Reviewed source `20761395194406547c44d7214b2db382a8bbef07` and correction
`db7bf1b4736d8e5036792362b03cdf978b8cb645` in pool3. Reviewer owns no E-14
implementation. Verdict: **source approved after correction; compilation and
12 owning test executions pending**. No build, benchmark or market-data read
was performed for this review.

The V2 return at date t uses finite positive closes at t-1 and t. The label at t
uses only returns in [t-window,t), so the current return is excluded. Volatility
cuts contain strictly earlier finite volatility observations: classification
precedes insertion. Precomputation of future returns does not influence earlier
labels or normalization. Missing full windows remain unavailable and later clean
windows recover. Flat histories remain middle; labels are not balanced after
observing the whole sample.

The ordered (volatility,date) set and tracked empirical-cut iterators are sound
under the actual chronological insertion contract. Insertion changes each old
rank by at most one; each desired rank also changes by at most one, so iterator
correction is bounded and never steps outside the ordered set. Classification
starts only after at least three observations. The helper now deletes copy/move
operations because its iterators refer to its own container.

Actual research and robust-subset consumers pass the complete configuration.
The research digest folds the V2 recipe before the run loop, distinguishing
active recipes even if no alpha survives. The identity binds rule, volatility
window, prior-history minimum, walk-forward count and Sharpe threshold. Gate-off
and explicit V1 retain the former digest path and arithmetic.

Review found a V2 admission defect: finite coverage counts alone allowed an
undefined middle-regime or middle-window Sharpe to be hidden by the legacy
minimum reduction. With four observations in each regime, PnL repeating
{1,2,1,2}, and a NaN at index 6, counts remain {4,3,4}; the first and third
positive scores previously let the verdict pass. The correction explicitly
requires finite full-sample, every regime and every walk-forward Sharpe for V2.
The exact regression keeps the positive minima visible and asserts V2 refusal
while freezing the explicit V1 result.

Six postimplementation source fixtures cover prefix/current/future invariance,
independent sorted-prefix cut calculations and ties, flat-history warmup,
missing-window recovery, recipe/coverage observability, and the undefined-score
admission regression. Runtime evidence remains pending. This source approval
does not establish performance, historical regime robustness, tradeable alpha
or full E6 acceptance.
