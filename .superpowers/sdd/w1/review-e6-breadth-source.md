# Independent bounded eval source review

Reviewer owns neither implementation. Verdict: **source approved, compilation
and focused runtime pending**. No build or numerical timing was launched.

## PBO boundary validation

Reviewed `84460488efae4324cc38797734c53e82b56940e9` and test accessor fix
`e8adfe4aa5c5c15dc6e90671c8b18f54f092f78e`.

The checked API rejects a ragged candidate-major matrix before entering the
core, then enforces positive even S <= 16 and S <= T. The exhaustive binomial
reserve and enumeration are bounded by C(16,8) = 12,870. Release builds now
check the Result before the convenience wrapper dereferences it. Valid core
arithmetic is unchanged. Per-candidate trailing-period trimming retains the
original T stride; the new T=17 versus T=16 fixture is valid because its generator
hashes (candidate,time), independently of row width. Existing production factory
calls use S <= 8.

No PBO optimization or full E6 acceptance is claimed. The convenience wrapper's
precondition comment should also list rectangular shape and S <= 16; this is a
documentation nit, not a source blocker.

## PSD breadth trace identity

Reviewed `a5bb66ac5ac22b490b2ca45d67174c898f0f8f70`, including the actual
`stage_combine` call and its two covariance constructors.

For symmetric PSD C, tr(C)^2 / ||C||F^2 equals the eigenvalue participation ratio.
Scaling by the largest finite absolute entry makes at least one squared term
equal one, so the nonzero-matrix denominator cannot underflow. Compensated
nonnegative reductions retain small contributions; squaring trace/sqrt(F²)
avoids the avoidable large unscaled square. The normal path uses O(K²) work
and O(1) scratch. Shape, finiteness, diagonal sign and scaled symmetry checks
precede the ratio; PSD remains an explicit caller precondition.

The stage uses R-transpose R/T for its MLE covariance, or the cleaned covariance
whose final step is PSD eigenvalue clipping. V2 is explicitly named in telemetry.
The prior clipped-eigen arithmetic remains selectable as V1, with no bit-identity
promise between rules. Only recorded breadth/implied-IC telemetry changes; the
fit, weights and hashed artifacts are not consumers of these scalars.

Three postimplementation fixtures cover independent eigen-reference parity on
full and deficient rank, finite extreme scales, and malformed inputs. Seven
existing fixtures remain in the pending focused target. No measured speedup,
full-suite pass or production-scale gate is claimed. The older stage comment
about byte identity can clarify that it concerns the covariance constructor,
since breadth rounding changes under V2.
