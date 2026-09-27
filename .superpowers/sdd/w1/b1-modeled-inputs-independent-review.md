# W1 B1 modeled-inputs independent source review

Reviewed production `f9f73d262edf98a82e195547b612b5484cde065a`, owning fixtures
`3f64bdaa64b661b929c2318619a9e62e194dc6ed`, and author report
`c4253be03da199e3ad5af7cc71eb8a358074f151`. **Source review approved; no blocker
found in this bounded opt-in slice.** This is not complete B1 acceptance. No C++
build, runtime test, market-data read, benchmark, or dataset acquisition was made
by this reviewer.

## Source checks

- The complete-window unsigned CS/AR/EDGE equations match the pinned author
  implementations: CS takes the absolute signed-pair mean, AR the square root of
  the absolute squared-estimate mean, and EDGE preserves its population-variance
  weighting/equal-weight fallback. The ensemble averages full spread fractions;
  the surface charges half per one-way trade. The CS tanh expression is the stable
  algebraic equivalent. Sources: [CS](https://raw.githubusercontent.com/eguidotti/bidask/1caba55d63ebab6c855536be51c43bdfc48d2dec/r/R/cs.R),
  [AR](https://raw.githubusercontent.com/eguidotti/bidask/1caba55d63ebab6c855536be51c43bdfc48d2dec/r/R/ar.R),
  [EDGE](https://raw.githubusercontent.com/eguidotti/bidask/1caba55d63ebab6c855536be51c43bdfc48d2dec/r/R/edge.R).
- Consecutive session checks, bounded windows, finite raw-price geometry and
  strictly earlier input clocks are explicit. Missing, future, gapped or undefined
  estimator inputs remain unavailable rather than becoming a successful zero
  spread. The estimator uses constant scratch storage.
- FIM uses Table VII global column 5 / US column 10 coefficients with size in
  log(1 + billions USD), annualized idiosyncratic volatility in percent and
  participation in percent. The anchored size/volatility differential and its
  nonnegative multiplier are explicitly an adaptation; the separate regression
  algebra requires omitted controls from the caller. No contemporaneous return
  control is manufactured. [Primary August 2018 draft, Table VII, PDF page 63](https://spinup-000d1a-wp-offload-media.s3.amazonaws.com/faculty/wp-content/uploads/sites/3/2021/08/Trading-Cost.pdf).
- Public-predictor borrow tiers are explicit scenario priors, independently
  unavailable when clocks/predictors are missing. Checked fraction/bps conversion
  and a separate holding-fee adapter avoid treating annual borrow as a one-way
  trading cost or a locate guarantee.
- Modeled recipe identity binds active spread, FIM and borrow settings plus the
  pinned upstream version; snapshot identity binds decisions, source identity,
  ordered IDs, values, clocks and missing tags. Disabled FIM ignores inactive
  anchors intentionally. Actual input-vintage authenticity remains the caller's
  source-SHA contract. Overlapping temporary/owned row allocations and identity
  serialization are bounded before allocation; no borrowed history survives the
  immutable surface build.
- Explicit V1 preserves its old codec and quote arithmetic; new fields are
  canonicalized away. V2 distinguishes missing trade inputs from missing borrow
  inputs. Fitness, optimizer and replay adapters use the same adjusted trade
  coefficients at the same executed dollars/NAV, while borrow remains separate.

## Prepared checks and remaining qualification

All 11 new owning C++ fixtures were inspected, including three synthetic reference
windows, strict clocks, gaps, conversions, anchored units, independent availability,
active-setting hashes, adapter equality and V1 golden hashes/quote bits. They have
**not run** in this review. The author's synthetic reference receipt executes the
hash-pinned upstream Python EDGE source; CS/AR are scalar transcriptions rather
than an executed R-package comparison. Therefore full independent bidask 1e-10
runtime parity remains unqualified.

The absolute FIM 13.7/32 bp example, empirical spread-bias/scale calibration, causal
predictor acquisition, observed borrow/locates, consumer default migration and
tradeable-alpha/capacity evidence remain open as stated in the author report.
Approval permits integration and bounded qualification of this source slice; it
does not close those plan requirements.
