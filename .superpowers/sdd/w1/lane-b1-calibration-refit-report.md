# W1-B1 calibration refit: source qualification pending

Implementation `63ea1ef43cb3ed167826d52d80b5b3ab0d19a21f`, postimplementation
fixtures `cdddaaabac4401b7be47f5d8b0511aa79d311e43`, independent-review repair
`cdde0a553fa2d298ada9308a245f787fbbe915e0`. Only
`include/atx/engine/cost/calibration.hpp` and its new owning
`tests/core/calibration_refit_test.cpp` changed. No CMake registration is needed
for production; root may add the focused test TU to its next bounded target.
No compiler, calibration run, stochastic simulation or market-data read occurred.

## Actual change

The previous fit clamped the impact exponent but retained the unconstrained
intercept, residuals and fit diagnostics. Consequently the returned cost law was
different from the law described by its report.

`CostCalibrationRule::RefitClampedV2` is now the default. After selecting a
clamped exponent it robustly fits the one-column response
`log(temp) - log(sigma) - applied_delta * log(participation)`. It then recomputes
Y, residuals, R-squared against the original log-response, residual p95, and
uncertainty for the returned model. The public checked
`calibrate_from_obs_fixed_delta(obs, delta, prior)` does the same for a declared
exponent, including the CostSurface's required 0.5.

An exponent-specific Y must not be transplanted into a different power law.
The fixed-slope entry point intentionally does not estimate an unused raw slope;
raw coefficients are unavailable in that mode. A clamp retains both the raw and
applied coefficients/diagnostics. The report distinguishes applied, insufficient,
degenerate, numerical-fallback and invalid-config/prior states. Finite-positive
input guards prevent nonfinite observations entering the corrected design.
Difference-of-logs avoids overflow in an otherwise finite temp/sigma ratio.
Unknown permanent impact retains the prior, with an explicit unfit flag.

Uncertainty remains the existing OLS approximation to a robust regression.
Constrained intercept uncertainty uses one fitted parameter (n-1 residual degrees
of freedom); delta stderr is zero because it is fixed in that fit. The enum
`ConditionalOnAppliedDelta` explicitly excludes uncertainty from selecting a
clamp using the data. This is not a sandwich or unconditional confidence bound.

`LegacyClampV1` is explicit through the existing entry points and preserves the
original finite valid-domain coefficient and old-diagnostic arithmetic. New
report fields were appended so positional aggregate initialization stays valid.
The fill adapter passes the rule through; converting the signed quantity to f64
before abs also avoids integer-absolute-value UB at INT64_MIN without changing
the regular rounded magnitude. No simulator-origin observation is promoted to
observed execution evidence by this change.

## Review correction and bounded checks

Root's independent review found that a nonzero log-participation range did not
guarantee numerical rank for the uncentered [1,log(p)] matrix. Around log(p)=-700
with a range around 1e-10, its columns are nearly parallel and the robust solver's
checked WLS result could abort. The repair normalizes the V2 slope column by its
midrange and range before fitting, then transforms coefficients back. Fixed-delta
fitting bypasses that two-column fit completely. Legacy retains its old design.
Final fix-only review and runtime qualification remain pending.

Six `CostCalibrationRefit.*` checks now cover:

- Upper/lower clamping against a nine-row symmetric analytic power law; applied
  residuals, R-squared (including a legitimately negative value) and conditional
  stderr checked independently from the returned coefficients.
- Explicit legacy intercept/diagnostic behavior and ordinary in-range recovery.
- A fixed square-root fit with exponent-specific Y and invalid-delta rejection.
- Finite row filtering, singular participation, prior validation and fixed-slope
  success when a raw slope cannot be identified.
- Finite extreme raw inputs with an unrepresentable fitted scale and unavailable
  permanent marks; fallback is explicit and does not emit an infinite Y.
- The large-log, near-constant rank regression plus an exactly singular fixed fit.

The existing `Calibration.FitOnTrailing_TruncationInvariant` remains the closed
fill-window check for the corrected default. No new timing, large simulation,
5-percent empirical calibration recovery or general execution-quality claim is
made. `git diff --check` passed; all C++ compilation/runtime is pending root's next
shared batch. B1 spread estimators, borrow tiers, FIM reference scenario and actual
consumer migrations remain open as recorded in the separate core/adapters report.
