# Time-knot fitter WIP: stopped at user request

Development and validation stopped immediately at the parent-forwarded user stop.
This is an incomplete checkpoint, not an integration-ready or validated change.

- Branch: `feat/vol-markov-time-knots-20261016`.
- Frozen base: `b2909ede55300a6a47751292a0d09daec3806b38`.
- Worktree: `C:/atx-wt/pool-35`.
- RunId / HeartbeatId: `vol-markov-time-knots-20261016`.
- Parent and independent reviewer approved the mathematical design before code.
  The implementation itself has not received exact-SHA review.
- No postimplementation tests were added or run before the stop instruction.
  No development, tests or builds were started after it.

## Preserved implementation

Public fitter header adds `AmericanMarkovFitTimeShape::VarianceTimeKnots` and
borrowed input `variance_time_knots`, with finite nonnegative strictly increasing
absolute cumulative variance-year coordinates. Parameter count is time controls
times state knots, constrained by the existing configured/hard parameter caps.
No defaults or hard caps were changed.

The fitter samples log sigma at the represented variance midpoint of each
original interval, with flat tails and exact control/equal-endpoint shortcuts.
The effective local-vol vector is frozen throughout that original interval.
This is not exact integrated variance for a continuous time surface, and pricing
substep refinement alone cannot remove the midpoint-freezing approximation.
No expiry, dividend or other schedule boundary is inserted or moved.

`interval_groups()` was removed. Replacement owned provenance is:

- `time_shape()` and `variance_time_knots()`.
- `interval_time_weights()`: rows with `left_group`, `right_group`,
  `right_weight`, `variance_time`, `variance_active`.
- Optional `time_basis_diagnostics()`: `quoted_variance_intervals`,
  `column_support_counts`, `column_l2_norms`, `common_normalization`,
  `qr_reciprocal_condition_inf`.

New internal `src/fitting/american_markov_time_basis.hpp` implements validated
time knots, interpolation weights, and ordered bidiagonal streaming QR. It uses
positive-variance intervals through the latest positive-weight quoted expiry,
including unquoted intervening intervals. R is commonly scaled by sqrt(row
count); its observed reciprocal infinity condition must be >= fixed 1e-12.
This diagnoses the represented interval log-sigma mapping, not an option-price
Jacobian or economic identifiability. Ordering, support, zero pivots and
nonfinite/overflow cases refuse. Priors cannot rescue refused bases.

Policy ownership copies the new time-knot span and resolves parameter count
from explicit controls. It avoids the `validate_model` and work-cap lines owned
by the parent's pending resolvent integration edits. The seed API explicitly
returns Unavailable for the new mode before inversion. Existing initial/prior
semantics and index-based roughness penalties are retained.

## Completed receipts and unfinished gates

The three owning Release source objects compiled successfully using the leased
tree's absolute wrapper, preset `vol-rel-avx2`, Jobs 2:

```
check atx-vol/src/fitting/american_markov_fit.cpp
      atx-vol/src/fitting/american_markov_policy.cpp
      atx-vol/src/fitting/american_markov_seed.cpp
```

Before implementation, an eight-case diagnostic probe captured Shared and
PerVarianceInterval fit results under explicit and resolvent pricing, with flat
and three-node smiles. It records objective/control/price bits and optimizer
counters. This is a baseline receipt only; no candidate parity run occurred.

- `C:/atx-cache/quote-benchmark/markov-time-knots-20261016-parity.cpp`
  SHA256 `67B71F8A261F3ED164EB1E209258CBEB2A5ED7A9FCC11F7D941E3D974D1A03AC`.
- `C:/atx-cache/quote-benchmark/markov-time-knots-20261016-baseline.json`
  SHA256 `B3E08AB02373F8A69CB0CB05844937604C9E7450B5C291C0F53FAB0DC5F178A9`.
- Temporary CMake registration for that diagnostic probe was restored before
  checkpoint. No shared build-file changes belong to this commit.

Outstanding work if the user resumes:

1. Migrate existing fitter tests from the removed `interval_groups()` accessor.
   Those old tests are currently expected to fail compilation until migrated.
   Parent owns the native JSON/accessor seam and permanent build registration.
2. Add postimplementation independent dense QR/conditioning checks, including
   nearly dependent columns, weak columns, long bidiagonal chains, invalid
   ordering, exact knots/tails, inactive intervals and full quote-prefix coverage.
3. Verify equal controls reproduce Shared operator bits; compare legacy fitter
   outputs against the captured eight-case baseline. Check ownership/copy/move,
   explicit cap boundaries, regularization semantics and seed refusal.
4. Check many-expiry bounded parameter counts, synthetic nonflat term recovery
   and independent integrated-variance comparisons that expose midpoint bias.
5. Run owning fitter/policy/seed tests and PCH-off source/test checks. No such
   postimplementation gates, broad hygiene or performance qualification passed.
6. Obtain fresh exact-SHA independent review before any integration.

The lease's automatic default `dev` configure failed on preexisting missing
`atx-kb`, `atx-agent-db` and `atx-options-engine` directories. The lease remained
held; isolated `vol-rel-avx2` configure and the scoped source check succeeded.
All owned build/configure sessions completed before the user stop. Only the
lease heartbeat keeper remained, to be stopped by normal release after this
durable checkpoint.
