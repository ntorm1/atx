# Common-model bid/ask interval calibration

This checkpoint adds `fit_american_markov_intervals`, a reusable offline
calibrator over the existing positive American Markov operator. It accepts
raw contracts and a caller-supplied common spot, funding/yield, cash-dividend
and dual-clock schedule. There are no vendor labels, ticker rules or
strike-dependent drifts in the API.

The fitted parameters are **absolute positive sigmas**. Model baseline sigma
is fixed to one, so multiplying baseline and local-vol nodes cannot create
redundant parameters. A single knot gives flat local volatility. A caller
can select more knots in log(X / initial spot), shared across time or with
separate sigma vectors for each interval that advances variance time.
Stopped-variance intervals have no additional parameters. Their map points
to group zero, whose values have no diffusion effect there. Callers choose
configuration complexity and regularization for their liquidity profile.

Every trial compiles one process and batch-prices grouped expiry/side queries.
All strikes and expiry prefixes use that process. Pricing retains the existing
positive transition, cash-event and Snell rollback construction. The owned
result contains the best finite model, absolute sigmas, knot coordinates,
interval-to-parameter map and prices in original quote order. No input span
survives the call. There is no independently interpolated price surface in
this layer.

## Objective and termination

For quote i, let `scale=max((ask-bid)/2, price_scale_floor)` and let distance
be zero inside [bid,ask], otherwise distance to its nearest endpoint. The
data term is the quote-weighted mean of `(distance/scale)^2`. An optional
midpoint tie-break uses the same scaling, **only for positive bids**. A zero
bid supplies an upper constraint; it never creates an ask/2 target. Locked
positive quotes are valid and use the explicit price floor.

Regularization adds mean squared log-sigma distances to a fixed prior and
adjacent strike/time parameter groups. These are index differences, not
coordinate-scaled derivatives. An empty prior means the initial sigma vector;
it does not move with the incumbent. Shared-time fits have no temporal
roughness term. Zero regularization is supported but does not establish
parameter identification from sparse or bidless quotes.

The deterministic optimizer evaluates bounded positive/negative coordinate
steps in log sigma, keeps strict objective improvements and halves its step
after an unimproved sweep. Explicit termination is step tolerance, evaluation
limit or sweep limit. Diagnostics report initial/final components, one-based
best evaluation, accepted steps, refused trials, raw price interval errors,
bidless count and inside-interval count. A returned result can miss every
quote interval; its finite model is valid but its fit can be inadequate.
There is no claim of a global optimum or quote feasibility certificate.

Invalid inputs and failure of the initial evaluation return an error with no
model. Later failed compilations/prices/nonfinite objectives retain the last
finite best iterate and increment the refused-evaluation count. Hard limits
cover quotes, parameters, evaluations, sweeps and row-option rollback work;
workspace and compiled-model storage have separate caps. An evaluation cap
includes initial and refused evaluations. Peak working memory includes the
incumbent and one candidate model, scratch intervals/nodes and two batch
buffers. Compilation allocates; this is not an allocation-free hot-path fit.

## Validation and scope

Post-implementation tests cover synthetic shared-flat and skew recovery,
flat/flexible objective comparison, per-active-interval recovery with a
stopped-variance cash event, common-model strike/calendar relations, deep
ownership, bidless upper bounds, prior/time smoothing, exact sigma bounds,
evaluation/sweep termination, best retention after a trial exceeds caps,
weight-scale/chunk invariance, duplicate contracts, malformed schedules,
nonfinite inputs/objectives and resource refusal. An independent no-cash
Black-Scholes call panel includes ATM and verifies approximate recovery while
requiring a measurable finite-grid residual. It is not an exact-model test.

This is a **finite Bermudan** calibration. Model state boundaries are absorbing
and cash has the existing limited-liability convention. Continuous-American
accuracy, tail/domain convergence, parameter confidence, Greeks, real-data
fit quality, warm updates and HFT latency are not established by these tests.
The current model's known discretization and PWL compilation issues are not
fixed here. No srPrc/srVol labels or holdout data were read by this lane.

Root owns permanent CMake and umbrella registration. The lane temporarily
adds `src/fitting/american_markov_fit.cpp` to the library and registers focused
`atx-vol-american-markov-fit-tests`; those CMake edits are restored before
commit. Proposed focused test name is `atx-vol-american-markov-fit`.

Lane receipt: Debug `vol-dev` target built, then all **12 tests passed** in
2.221 seconds with zero skips; XML was written to
`build-vol/markov-fit-tests.xml` in the leased tree. PCH-off `vol-hygiene`
source and test objects compiled successfully after the final API comment.
`git diff --check` and manual 100-column scan passed. These are focused lane
checks, not Release performance evidence or full module qualification.
