# Isolated empirical vector seed tier

Status: concrete experiment design; no production implementation or default
change. Parent approved investigation after the complete cold-seed replay.
Baseline is production integration `f30c96a80b89051f6670d425590a23538110ecf4`;
replay is `c42de25ced067a76384c5e4f3379b6026b2b5b03`.

The objective is to remove most of the 496 cold target boundary solves paid after
the bounded family economic proposal cannot close its interval. The original
scalar floor remains mandatory and already shares 11 exact boundaries. Sharing
expiry setup is rejected as a useful standalone change: the diagnostic setup
cost is small compared with those 496 solves. The quiet ThinLTO component receipt
must precede any prediction of full-fit gain.

## What the current evidence establishes

The fresh captured workload contains 496 calls, 11 batches and 11 exact
expiry/carry/options keys. All original scalar proofs accept. Scalar geometry and
boundary-handle prices match scalar cold price bits. Vector prices differ by at
most `2.2737367544323206e-13` USD/share, with no exercise-class or monetary
price-gate changes. Sixteen tail prices use the scalar route. The smallest scalar
budget margin is `0.0009352708916831034` USD/share. Predictions retain SHA256
`e086fd069a81c2c9eb84bb99e25e8a59e5d78fca8b02b0d51556668eff354d5f`.

These are workload-specific observations. They do not prove a universal vector
error bound, equality to the scalar Reference map, or Greek accuracy.

## Existing error controls are empirical

`american_iv_premium.hpp::kIvBatchConfirmMarginRel = 1e-11` is documented as a
tested chain-grid rounding class. Existing accurate batch confirms use that
margin to keep side and far-root decisions away from their thresholds. It is a
useful precedent for an explicitly tested tier, not an interval theorem.

The broader `AmericanBoundaryBatchSchemeMapping` normal/stress tests allow
absolute differences of `1e-6`/`1e-3` USD. Passing them does not establish a
`1e-11 * max(price,1)` bound over their full domain. The new experiment must
measure actual discrepancies and preserve failures across the entire selected
test domain, rather than quote the looser test's pass count as precision evidence.

The vector kernel uses Cody rational erfc, vector exponential/logarithm and a
vector BAW seed. Scalar Reference uses the scalar library CDF and its own seed
arithmetic. Cody's original implementation describes highly accurate rational
approximations but explicitly makes achieved accuracy dependent on arithmetic,
compiler, intrinsic functions and machine constants. This supports the CDF's
provenance; it does not bound error propagated through this boundary fixed-point
solve. [Cody's CALERF source at Netlib](https://netlib.org/specfun/erf).

A generic certified enclosure would additionally need outward error propagation
through BAW initialization, each fixed-point/Newton update, branch/clamp decisions
and final premium integration, including ill-conditioned boundary states. None
of the current tested price margins provides that certificate.

## Candidate contract

Use a private opt-in enum distinguishing `ScalarReference` from
`VectorReferenceEmpirical`. The default and original scalar helper remain
unchanged. The research binary and policy receipt must identify the selected
tier; do not report vector work as an exact scalar cold proof. An eventual public
configuration requires parser/default/copy/commit/archive review before exposure.

For a prepared original family-lane sigma:

1. Apply the unchanged method/control, finite-domain, exact target ceiling and
   zero-volatility screens. Retain all immutable request, family and envelope
   identity checks. Zero economic budget stays on the exact scalar route.
2. Admit only the same accurate Reference scheme initially. Reject other
   profiles/schemes from the vector experiment, retaining the scalar fallback.
   Continuous carry is required; dated cash dividends are outside this tier.
3. Route targets in the original nonmonotone band to the complete original
   scalar helper, including its positive cold chord. No vector chord substitutes
   for that proof. This preserves the original conditional slope requirement.
4. Batch only the remaining original sigma proposals, with exact call/put dual
   mapping and per-lane typed output: price, actual solved internal boundary,
   exercise class, route/patch reason, and arithmetic origin. Do not infer
   exercise classification from price alone.
5. Require continuation and a finite positive boundary. Exclude the established
   `0.25 * sigma * sqrt(T)` exercise-edge band using
   `log(internal_spot / boundary)/(sigma*sqrt(T))`. This is a conservative
   engineering exclusion, not a price-error certificate. All 496 current scalar
   boundary distances exceed 0.386; vector distances must be measured separately.
6. Reserve a declared empirical arithmetic allowance
   `A = 1e-11 * max(1, S, K, abs(target), abs(vector_price))` USD/share for the
   initial nonnegative-rate/yield experiment. Require
   `abs(vector_price-target) + A <= raw_economic_budget` and a finite result.
   This is deliberately more conservative than the existing confirm scale;
   it is still an empirical allowance, not a generic certified bound. Unsupported
   model/range conditions or insufficient margin use the original scalar helper.
7. Require the target strictly above the **exact scalar Reference** reporting
   floor using the existing immutable-model floor cache. Failure preserves the
   original typed fallback/classification; it never changes quote boxes or
   fabricates an identified IV. Scalar tails can reuse their already evaluated
   exact classified target through a shared private predicate, avoiding a second
   scalar solve while preserving the original numerical order.
8. Store the proof origin and reserved allowance in the prepared result. Every
   consumer rechecks model/options, target/budget, sigma, family/envelope stamp
   and origin. Candidate mode changes cannot consume a prior incompatible record.

Every lane declined by this tier receives its existing scalar seed proof and
existing subsequent fallback. There is no new terminal refusal or silent loss of
valid rows. The allowlist is numerical scope, not a whitelist of SPY row IDs.

## Required controls and measurements

| Control | Evidence required before integration |
|---|---|
| Original scalar route | Default bit identity, including targets at/below floor and nonmonotone-band typed fallbacks |
| Request coverage | Every original attempt assigned once to vector admission or a named scalar fallback; no dropped tail/patch lanes |
| Monetary threshold | Tests immediately inside/outside allowance-adjusted budget; zero/tiny budgets, nonfinite values and altered immutable keys |
| Boundary semantics | Both sides; actual exercise/near-edge cases; supported negative/nonpositive carry falls back; patch and tail origins remain explicit |
| Chord/floor semantics | Concrete historical subfloor promotions stay refused; nonmonotone targets retain complete scalar chord decisions |
| Arithmetic domain | Existing batch stress corpus plus edge/rate/tenor/volatility sweeps, all attempted denominators, pack permutations and partial packs; maxima and exceptions retained |
| Full SPY output | Same 33 valid slices and 12,866 eligible rows; all served, raw containment at least 99.5%, no disallowed identification demotions |
| Independent accuracy | Actual API export checked with scalar cold Reference prices, exact floor semantics, metadata, interpolation and shape/calendar/parity gates |
| Inclusive performance | Packing/allocation/setup, vector solve, scalar patches/fallbacks, exact floors and bookkeeping included; archived ThinLTO binary, quiet physical-core window |

Counters distinguish vector attempts/lanes/admissions, scalar tails, kernel
patches, unsupported scope, near-edge and margin retries, floor/chord refusals,
and total scalar fallbacks. Stage clocks report vector work, original scalar
retry work and exact floor work separately. They must reconcile to the existing
total cold-seed stage without counting a reused floor as a new solve.

Any observed scalar Reference violation blocks promotion of that candidate.
Even if the complete SPY audit passes, the initial result is a qualification of
that frozen SPY workload and declared experiment scope. Broader library support
requires a broader stable comparator corpus; no generic certified bound, default
change or Greek guarantee follows from the SPY pass.

## Structural alternative if the experiment is not worthwhile

The remaining family intervals are much wider than their observed actual errors.
Investigate locally adaptive sigma/carry enclosure refinement on the occupied
cells that cause fallback, charging every new cold validation solve and tensor
node. The target is fewer necessary target solves, rather than cheaper repeated
setup. This must preserve the envelope's declared evidence class and may lose if
additional family construction exceeds the saved fallback work. It is a separate
algorithm experiment, not a tolerance increase or a budget derived from Greeks.
