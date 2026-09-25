# W0 inference integration repair

## Outcome

Implementation and post-implementation regression checks written; build, execution, and fresh
adversarial review pending. No acceptance pass claimed.

## Branch / frozen base / lease

feat/aes-codex-integration-20260925, C:\atx-wt\pool-2;
base bc5cc646b46f6a7c23a60e87d28dfa9b972671ec;
run aes-codex-integ-20260925, heartbeat aes-codex-integ-20260925T2130.

## Problem and implementation

W0-E0a changed t-statistics and IC return winsorization but left callers unable to select
historical semantics through public combiner APIs. Its automatic bandwidth also ignored
forward-label overlap. E-03/E-15 integration now exposes SignalInferenceConfig on GK,
ICIR-EWMA, Fama-MacBeth, and Kakushadze, plus explicit rule/horizon on marginal_ic.

- IidV1 plus RawV1 remains available to reconstruct earlier fits.
- NeweyWestAutoV2 remains available unchanged.
- HorizonAwareV3 retains V2 unweighted daily-label behavior. Weighted EWMA uses the exact
  sandwich variance even at horizon one. When callers declare overlapping
  labels it uses a Hansen-Hodrick kernel and at least h-1 lags, with the existing positive
  Bartlett fallback. The weighted ICIR haircut has matching horizon-aware inference.
- Callers must declare the horizon of their supplied return stream. Existing streams have
  no label-horizon metadata, so the default is one stream date; do not claim automatic
  detection of a multi-day horizon. W2/W3 consumers must carry their actual horizon.
- Invalid inference enums, zero horizons, and invalid return-treatment enums are rejected.

## Files changed

- atx-engine/include/atx/engine/eval/hac.hpp
- atx-engine/include/atx/engine/combine/signal_combiner.hpp
- atx-engine/include/atx/engine/combine/orthogonalize.hpp
- atx-engine/src/combine/signal_combiner.cpp
- atx-engine/src/combine/orthogonalize.cpp
- atx-engine/tests/combine/combine_w0e0a_hac_tstat_test.cpp

## Planned validation

Implementation preceded tests, per user instruction. Added public legacy-fit comparisons,
an overlapping MA(20) null calibration (2000 independent series), weighted-haircut change,
and invalid-boundary cases. Existing CombineHacTstat tests must remain unchanged and green.
Use wrapper check, targeted combine/eval build, anchored CombineInferenceConfig and existing
CombineHacTstat/EvalHac suites, then touched-target integration gate.

## Evidence

git diff --check exited zero. Isolated Debug configuration completed successfully with
FETCHCONTENT_BASE_DIR=C:/atx-wt/pool-2/deps/equity-dev. Both the initial and reviewed-fix
single-TU checks succeeded:

```
powershell -NoProfile -File scripts/atx-build.ps1 check -Preset equity-dev atx-engine/src/combine/signal_combiner.cpp
[1/2] Building CXX object atx-engine/CMakeFiles/atx-engine.dir/src/combine/signal_combiner.cpp.obj
exit=0
```

Targeted combine/eval build is running; no numeric test results yet.

## Fresh review and fix pass

Independent w0_gate_audit reviewed 9caf9e85 and requested changes:

- H1: full-lag uniform-kernel cancellation could yield a huge t-stat with too few samples.
  V3 now withholds inference when the overlap horizon reaches the sample count and uses
  a relative cancellation guard. A concrete five-point reproduction and oversized horizon
  are added to the regression checks. V1/V2 behavior is retained.
- H2: legacy weighted VIF rescaling was approximate for unequal weights. V3 now normalizes
  by the actual weighted IID variance so the result equals the direct weighted sandwich
  S_w/(sum w)^2. A finite 17-date half-life is checked against an independent explicit
  covariance sum. V1/V2 retain their arithmetic.
- M1/M2: documented the IC-only return-treatment scope; added invalid-enum coverage and
  a nontrivial two-alpha legacy-weight comparison against the two-dimensional closed form.

Fix-only static re-review at c8e7e990 found H1/H2 resolved and no remaining blocker.
Runtime approval remains pending. Clarified weighted daily behavior and finite-row compaction
per the reviewer; the five-point regression now also exercises the cancellation fallback.

## Deviations and remaining limits

This is an authorized cross-lane integration repair of the W0 documented public API gap.
Finite-date compaction remains the existing behavior: lags count usable rows, an approximation
that does not preserve exact calendar HAC for irregularly missing observations.
No economics or profitable-alpha assertion follows from this inference repair.

## Ledger candidates

None until validation completes.
