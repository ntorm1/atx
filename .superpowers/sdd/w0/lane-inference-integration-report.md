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
- HorizonAwareV3 defaults to the V2 daily-label behavior. When callers declare overlapping
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

git diff --check exited zero. Build configuration is in progress; no numeric results yet.

## Deviations and remaining limits

This is an authorized cross-lane integration repair of the W0 documented public API gap.
Finite-date compaction remains the existing behavior: lags count usable rows, which is
conservative for overlap when observations are missing, but not exact calendar HAC.
No economics or profitable-alpha assertion follows from this inference repair.

## Ledger candidates

None until validation completes.
