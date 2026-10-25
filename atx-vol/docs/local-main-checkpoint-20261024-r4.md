# Local main scenario and margin checkpoint, 2026-10-24

This user-requested checkpoint advances local main from `e6634e17` with the
reviewed conditional-scenario, grid and margin fixes. Tested integration source
is `e1fa2ea3cc5baf017458da9861a7839c6b327a63`; component sources are
`84a460fa`, `15e2a263` and `c632cfbc`, each freshly approved before integration.

A valid price remains eligible for Exact repricing when some base Greeks are
unavailable. Taylor calculations require only the sensitivities used by their
actual shocks. Inactive operands are cleared before multiplication, preventing
both NaN-times-zero and finite inactive cross-term overflow. Missing active
sensitivities make the cell unavailable rather than removing its position from
the loss calculation. Margin rejects excluded positions, unavailable active risk
and nonfinite cell totals.

The grid adds a unique priced-contract count and contributing, excluded and
unavailable position counts for each cell. Duplicate positions retain their own
supply and scaling outcomes. The public native result grows from 14 to 18 fields:
C++ consumers must rebuild. Existing shorter aggregate initializers retain
defaulted trailing fields. No persisted layout or Python grid binding changes.

Combined checks on the tested source:

- 46 scenario-grid checks pass, including cold Exact bit parity, parallel
  determinism, duplicate scaled overflow and derivative-wrapper parity.
- 36 margin, strategy and quant checks pass.
- The public umbrella suite passes 7 of 10 checks. Its three failures are
  `TierCountsMatchTheReadmeTable`,
  `DemotedSurfaceContainersAreNotNamedInPublicHeaders` and
  `NoFixturePathResolvedOutsideTheSharedResolver`. All 14 failure assertions
  reproduce identically when the unchanged test executable scans the clean
  previous-main source `e6634e17`. This is a source-text baseline comparison,
  not a rebuilt baseline numerical test. The README reports 38 Tier-B headers
  where the existing inventory has 50; the path audit reports 12 literals.
- Component PCH-off checks passed for four scenario/margin owning objects and
  the two earlier conditional-helper owning objects.

Evidence root: `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/`.
`integrated-scenario-receipt-v1.json` binds source, executables and all combined
logs, with SHA-256
`1504a1aa93dd7de45b1ccd719e06c339ec9942d0354d2c2ec48411e12fb3cfae`.
The baseline comparison is retained separately as
`integrated-scenario-umbrella-baseline-comparison-v1.json`.

This checkpoint does not qualify a release or claim a latency improvement.
The prior historical BEV pin failure remains open. Candidate-capture tooling,
the second independent Greek-reference experiment and cash Gaussian research
remain isolated work. The full SPY accuracy/coverage contract and subsecond
single-core target are unchanged.
