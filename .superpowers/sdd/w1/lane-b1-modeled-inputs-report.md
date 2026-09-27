# W1 B1 spread / FIM / public borrow input slice

Production: `f9f73d262edf98a82e195547b612b5484cde065a`.
Postimplementation fixtures: `3f64bdaa` (11 owning C++ checks, **not executed**).
No C++ configure, compilation, benchmarks, market-data access, or acquisition was performed.
This is an explicit opt-in input layer, not full B1 acceptance or consumer migration.

## Implemented contract

- New `ModeledInputsV2` builds an immutable `CostSurface` from caller-owned, bounded
  trailing raw OHLC, liquidity and public predictors. Every active input requires a
  strictly earlier publication clock. Missing inputs remain explicit unavailable
  rows; a nonzero trade never receives a successful missing-data zero-cost quote.
- CS, AR and EDGE return full spread fractions. The all-three equal-weight mean
  has one explicit scale knob; the surface charges half that spread per one-way
  trade. Missing rows, session gaps or an undefined EDGE estimate block the whole
  ensemble. The fixed window preserves consecutive exchange-session ordinals.
  There is no missing-row deletion or partial-estimator substitution.
- Raw component diagnostics remain unscaled. CS means signed per-pair estimates
  then takes absolute value; AR means squared estimates then takes the unsigned
  square root. These are `CS`/`AR`, not `CS2`/`AR2`. EDGE uses the reference's
  equal-weight fallback when total variance is nonpositive. Processing is O(window)
  time and O(1) scratch, with caller-selected min/max window bounds.
- FIM adjustments are an explicit anchored adaptation: add the published
  size/idiosyncratic-volatility **difference** relative to caller-specified reference
  characteristics at a specified participation anchor, then translate it into a
  nonnegative multiplier of the existing square-root impact coefficient. The
  square-root shape away from the anchor and the zero floor are modeling choices.
  An unavailable active FIM input blocks trade pricing. Disabled FIM remains inert.
- Borrow is a named public-predictor prior: small cap, raw price below $5, high
  short-interest/float and young IPO contribute explicit binary risk flags. Zero
  flags select GC, one selects warm and two or more select special. Default point
  priors are annual fractions 0.00275, 0.03 and 0.275; thresholds/rates are all
  configurable and hashed. They are assumptions, not estimated coefficients or
  observed lender quotes. Missing predictors do not become GC. A rate does not
  assert locate availability. Annual-fraction/bps conversion is checked; the
  existing explicit day-count `BorrowModel` adapter retains separate holding fees.
- V2 stores trade and borrow availability independently. Borrow is queried through
  `borrow_annual_rate`; it is never silently added to one-way trading costs or
  optimizer borrow terms without a holding interval. The existing fitness,
  optimizer and replay adapters consume V2's adjusted trade coefficients.
- The modeled adapter hashes every active model knob and the pinned upstream
  version into the surface's recipe identity. Snapshot identity binds source SHA,
  decision, ordered IDs, output values, clocks and independent missing tags. The
  caller's source SHA must bind actual input vintages; the engine does not prove
  source authenticity. The adapter bounds its overlapping row/surface allocations.
- Explicit `SqrtOneWayV1` keeps its old operation order and recipe/snapshot codec;
  new row fields are ignored and canonicalized. Existing consumers retain V1.

## Primary reference provenance and limits

[bidask](https://github.com/eguidotti/bidask/tree/1caba55d63ebab6c855536be51c43bdfc48d2dec)
is pinned to `1caba55d63ebab6c855536be51c43bdfc48d2dec`; the authors' CS/AR/EDGE
implementations determine the estimator recipes. The MIT notice is retained at
`atx-engine/src/cost/bidask-LICENSE.txt`, with attribution in the ported source.
The associated [EDGE paper](https://doi.org/10.1016/j.jfineco.2024.103916) defines
the statistical estimator. Neither an estimator nor equal-weight averaging is a
guarantee of unbiased execution costs. The original plan's post-2003 upward-bias
concern remains a calibration sensitivity to measure; this slice has no empirical
bias estimate. `spread.scale` makes that sensitivity explicit, not fitted.

The 2.42-second Python reference invocation used **only fixed synthetic values**.
`b1-spread-reference.py` executes the unmodified hash-pinned upstream Python EDGE
source; receipt `b1-spread-reference.json` records all OHLC values, outputs and
source hashes. Its original SHA256 is
`68937f6011eb62e9ba361d461adac99e391c7956c85027cc71f0cfbb3827e0d5`
(Git checkout newline normalization may alter the file-byte hash).
Upstream EDGE source SHA256 is
`08305189738c2fe4b9156f23c6d8ef2c8aacd597efa415e894670aa7dc4f5f7d`.
CS/AR values use an independent scalar transcription of the pinned R equations.
R is not installed; actual R-package comparison is **unrun**. The C++ fixtures
assert all three values within 1e-10, but compilation/runtime is also unrun.
Therefore the complete bidask 1e-10 acceptance gate is **UNQUALIFIED**.

[FIM, Trading Costs, August 2018 draft](https://spinup-000d1a-wp-offload-media.s3.amazonaws.com/faculty/wp-content/uploads/sites/3/2021/08/Trading-Cost.pdf),
Table VII (PDF page 63), supplies the global-column-5 and US-column-10 coefficients.
This version explicitly follows the table's units: `ln(1+ME/billion USD)`, annualized
idiosyncratic volatility in percent and participation in percent of one-year ADV.
Some narrative passages use different size/window descriptions; they are not mixed
into this recipe. Contemporaneous return controls are excluded from pretrade inputs.
The separate reference-algebra API requires the caller to supply all omitted
controls explicitly. The table omits intercept/control means needed for the plan's
absolute 13.7/32 bp example. That gate is **UNQUALIFIED**; no constants were tuned
to reproduce it. The anchored square-root adaptation is not a refit of FIM or a
claim of empirical out-of-sample accuracy.

## Qualification prepared, not passed

`atx-engine/tests/core/cost_modeled_inputs_test.cpp` has 11 postimplementation tests:
three synthetic reference windows, scale/units, missing/future/session-gap behavior,
raw geometry/bounds, borrow tiers/conversions, FIM units/differential/zero floor,
V2 fitness/optimizer/replay equality at the same executed dollars and NAV, independent
borrow availability, active knob identities, and strict clocks/multiplier validity.
An independent Python encoding of the preexisting V1 little-endian codec produced
recipe `3879022a980db24a1fcc54c9b2ea198fe68eb9b1c7559a9fecacea62a579ab49`
and snapshot `15208617c87033f4884283d5ec7b91a40629c0a4a590dc44b670743a1143124d`
for the fully specified fixture, now asserted alongside quote-bit reproduction.
No C++ test or hygiene pass is claimed. Independent source review is pending.

Root registration needed: `src/cost/spread_estimators.cpp`,
`src/cost/borrow_tiers.cpp`, `src/cost/fim_adjustment.cpp`,
`src/cost/modeled_inputs.cpp`; add the new owning test TU to the bounded cost target.
No CMake or ledger file was edited in this lane.

Remaining B1 scope: actual cost-consumer/default integration with explicit run
identities; independent package reference runtime; causal public-predictor acquisition
and calibration; empirical spread bias/scale evidence; observed borrow/locate data;
full FIM intercept/control scenario for the absolute example; real tradeable-alpha
and capacity evidence. This slice does not close those requirements.
