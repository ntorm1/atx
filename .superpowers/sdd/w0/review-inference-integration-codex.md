# Independent W0 inference integration review

Reviewer: `w0_gate_audit`; implementation owner: root in pool-2.
Reviewed initial `9caf9e851a6dc565e0bb376f138de56dd97bdaa1`, then fix-only
`c8e7e990f4bfa558cfc898ad578bc1529d60ec19`. Read-only review; no inference
implementation edits by this reviewer.

## Findings and disposition

- **H1, resolved statically:** Uniform HAC at the full available lag can cancel to
  a tiny positive number and report enormous significance. The five-point series
  `{.037921444273233657, .0333917854612267, .032102668894305145,
  .03600658924381094, .039304876836250696}` reproduced this with horizon 5.
  V3 now withholds unsupported overlapping horizons and guards near-zero uniform
  long-run variance relative to lag-zero variance, with Bartlett fallback. V1/V2
  arithmetic remains frozen.
- **H2, resolved statically:** The old weighted VIF rescaling is not the direct
  sandwich variance with unequal weights. V3 now uses
  `VIF=(S_w/sum(w*e^2))*(sum(w)/sum(w^2))`, which cancels the caller's weighted IID
  variance to `S_w/sum(w)^2`. The independent unequal-half-life test computes the
  covariance sum directly. Equal weights alone would not expose this defect.
- **M1/M2, resolved statically:** Public documentation now limits IC return
  treatment to IC construction; invalid treatment enum and nontrivial two-alpha
  legacy weights are covered. Legacy IID/raw is selectable through the public API.
- **Documentation correction requested:** Daily-label equivalence to V2 applies
  to unweighted means. Corrected V3 weighted normalization intentionally changes
  unequal-weight EWMA even at horizon 1. Usable-row compaction is an approximation
  to calendar lags, not universally a conservative estimator.

## Follow-up full-lag edge

An independent scalar probe found that the relative guard alone can still miss
degenerate full-lag cancellation for very low-variance ICs. Transform the original
five points with `x_i=0.03+(base_i-0.035)*1e-10`: automatic lag remains 4,
uniform `S=1.925929944387236e-34`, but the threshold is only
`5.1560467624668484e-39`. A direct full-lag V3 fallback (`lag == n-1`) was requested
for both unweighted and weighted inference. At full lag the centered covariance
sum is mathematically zero regardless of this floating residual. This follow-up
was implemented and independently inspected in the post-`e2c8da0a` diff: both
V3 paths explicitly fall back at `lag == n-1`. Public horizon-2 and scaled
five-point cases now compare against the Bartlett result. Static disposition:
resolved; final SHA and execution evidence pending.

## Validation status

Initial fix-only review resolved the reported normal-scale reproductions; the
subsequent full-lag edge above is also resolved statically. Runtime approval is **pending**
targeted and integration test evidence. The
five-point test covers unsupported horizons; the separate cancellation fallback
branch was inspected but does not yet have a direct execution result in this
review. No real data were read and no alpha-performance claim is made.

Independent read of `pool-2/build-equity/codex-inference-tests.log` found 25/26
anchored checks passing, including public legacy reproduction, unequal-weight
sandwich normalization, and full-lag guard cases. However,
`DeclaredHorizonCalibratesOverlappingNull` failed: 141/2000 rejections (7.05%)
exceeds its 7% ceiling and nominal 5% materially. Approval remains withheld.
Requested diagnosis of automatic-lag versus fixed declared-horizon HH behavior;
the cutoff must not simply be widened to fit the observed result.

## Final fix-only review and independent execution

Reviewed `52b8c6eaee90eee17b9f63e60f435134139a607a`: the overlapping-label
Uniform path now uses `max(h-1, rule-of-thumb lag)`, matching the established eval
Hansen-Hodrick path. The NW1994 automatic bandwidth is specific to Bartlett;
daily-label behavior retains it. Both unweighted and weighted V3 paths agree.
The test seed, sample count and 3%-7% acceptance band did not change.

Independently ran the six `CombineInferenceConfig.*` tests from the frozen pool-2
binary; all six passed in 2.349 seconds. Log:
`pool-5/build-equity/w0-inference-independent-six.log`. The unchanged 2000-stream
fixture reports 126/2000 (6.30%) with HH lag 20, versus 141/2000 (7.05%) for the
obsolete Uniform/NW-plugin combination, whose mean lag was 30.98 with zero
fallbacks. This supports the bandwidth diagnosis; it is not exact 5% calibration.
Independently inspected the owner's `codex-inference-bandwidth-tests.log`: 26/26
anchored tests passed (5.65 seconds).

**APPROVE for integration.** No remaining blocker. Independently checked the
root's hygiene logs and cache: PCH off, equity only, isolated pool-2/deps/hygiene;
signal_combiner.cpp, orthogonalize.cpp and cross_section_ic.cpp all compiled
successfully. Whole-target qualification remains root's integrated gate.
