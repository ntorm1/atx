# Resume 4: direction reset toward per-expiry fitting (October 16, 2026)

Root tree `C:/atx-wt/pool-33`, branch `feat/vol-resume4-20261016`, base
`5e978fed` (resume3 handoff). Lease RunId `vol-20261016-root4`.

## Evidence that set the direction

- Diagnosis (read-only, dev200 development cohort): the common Markov model's
  error is a systematic low price level caused by ATM volatility stuck under
  resource caps (class B) and by the 0.25 sparse prior (class A). The per-expiry
  quote-slice fitter is far more accurate where it serves, but static admission
  limits it to 16.7% of eligible rows. 59% of eligible rows are refused by caps
  that engineering can lift. See the ledger line of this date and
  `C:/atx-cache/quote-benchmark/diagnosis-20261016/`.
- Research (cited): SpiderRock labels are per-expiry channel-constrained spline
  smiles priced per contract with per-expiry sdiv and forward offset. See
  `C:/atx-cache/quote-benchmark/research-20261016/`.

## Lanes dispatched this resume

| Lane | Branch | Purpose |
| --- | --- | --- |
| bench-e0-queue | none (archived seed-free binary) | E0 byte parity, then frozen R104/R416 queue |
| time-knots-lane | `feat/vol-markov-time-knots2-20261016` | finish the time-knot fitter WIP, migrate `interval_groups()` consumers, gates |
| policy-experiments | `feat/vol-policy-exp-20261016` | JSON-only X1 seed+prior, X2 five knots, X3 per-interval, X4 caps, X5 combo; status-based hybrid composition tool |
| implied-forward-lane | `feat/vol-implied-forward-20261016` | reusable per-expiry implied forward / sdiv from put-call pairs; opt-in carry mode for the quote-slice path |
| slice-coverage-lane | `feat/vol-slice-coverage-20261016` | graded admission, cross-expiry term prior, channel-penalty objective, optional knot spline for the per-expiry fitter |

All results are development-cohort evidence; no default promotion, no holdout
labels opened, no main merge. Integration, gates and receipts are appended below
as lanes report.

## Lane result: bench-e0-queue (complete)

E0 byte parity passed. R104 removes every E0 fit failure and reaches the
cap-feasible ceiling (45,070 eligible prices, 70.49%); its three-way MAE is
0.383 against E0's 0.500 with containment 97.1% and p99 2.19. R416 adds cost
and boundary mass without shared-row gain. The remaining common-model gap to the
quote midpoint (0.359 all-eligible) is still a low level (bias -0.245) on the
sparse profile and in snapshots whose ATM knot is below the market. R104 is the
development base for the policy experiments; nothing is promoted. Receipts and
hashes: `C:/atx-cache/quote-benchmark/common-resolvent-controlled-20261016-v1/RESULTS-20261016.md`.

## Lane result: time-knots-lane (complete, under review)

`f4416a74` on `feat/vol-markov-time-knots2-20261016`: variance-time-knot
shape, `interval_groups()` consumers migrated, legacy eight-case parity
232/232 values, anchored suites fit 28, policy 16, seed 15, market 20, pricing
60, native CLI 122, audit 18, campaign 22, PCH-off 15 objects. Measured
midpoint-freezing bias follows x/sinh(x): -4.9% variance inside a steep coarse
cell, +8.3% straddling a knot. Independent exact-SHA review dispatched.

## Lane result: implied-forward-lane (complete, under review)

`a5d2018a` on `feat/vol-implied-forward-20261016`: reusable per-expiry implied
forward / sdiv estimator (Tier-B headers, allocation-free, typed refusals) and
an opt-in `carry_mode` for the quote-slice path; default path byte-identical.
Benchmark verdict: the supplied carry in the raw schema already sits within
0.07-5.1 bp of the label-implied forward by tenor, while any quote-only forward
misses by 8-39 bp, so the mode is WORSE on dev200 (0.176 -> 0.259 on 7,852
shared rows) and must stay off for this benchmark. Hypothesis 7 is refuted at
the forward level; the residual >365d call/put asymmetry is strike-varying
(ASML 93%), i.e. exercise/dividend modelling. The module is kept for live use
where no vendor carry exists.

## Standing direction (user, this resume)

- SpiderRock marks are the yardstick, not the architecture. Any curve family
  or surface parameterisation that matches or beats the labels on accuracy,
  robustness and performance is preferred over replicating the vendor spline.
  Family choice is decided by dev200 evidence under one shared objective.
- Build infrastructure must not burn lane time: no monolithic test link in
  lanes, focused test targets only, vol presets limited to the vol closure.
- Source and document edits are made with the editor tools, not scripts.
- Time-knot fitter `f4416a74` integrated at `fc5ffb84` after APPROVE.

## Integration log (root branch `feat/vol-resume4-20261016`)

| Merge | Lane SHA | Outcome |
| --- | --- | --- |
| `fc5ffb84` | time-knots `f4416a74` | APPROVE; 2 minor deferred |
| `3495635f` | policy-experiments `9c1005ac` | compose tool + note; X1 modest gain; status hybrid 75.7% at 0.292 |
| `d7f4b6b5` | implied-forward `a5d2018a` | APPROVE; mode stays off (supplied carry equals the label carry); 4 minor deferred |
| `da3a4179` | build-infra `5cecf923` | vol-only presets, `vol-rel`, lease configure fix, test slices; root adopted `vol-rel` |
| `ac01702c` | cohort builder `6703f85d` | dev200b built; ranking holds; spread-normalised MAE adopted |
| `dc064a1e` | tick-path `94208f57` | BLOCK at `dcb6e3f1` (out-of-box ladder certificate) -> fix -> APPROVE; chain refresh p99 ~106-181 us unqualified |
| `be85fd21` | markov-fit `88c77c5d` | APPROVE; LM ends evaluation-limit stalls; seed-as-prior 0.3519 -> 0.3391 on the R104 rows (post-hoc); MINOR-1 one-sided seed bound must be fixed before holdout |

| `b9374ae7` | dividend-schedule `a402d615` | APPROVE; payers 0.224->0.211 (dev200) and 0.295->0.232 (dev200b); ASML blocked by lattice strike kinks |
| `31cdfeac` (ff) | slice-coverage `03583b45` + merge | BLOCK -> fix -> APPROVE -> merge APPROVE; graded admission 78.7% coverage at own MAE 0.235 vs mid 0.333; knot spline best family |
| `ef5bc6c3` | review-cleanup `3465221c` | APPROVE (0 blocker/major, 2 minor, 3 nit); closes the markov-fit, implied-forward and time-knots minors; ledger tail conflict kept both sides; root gates fit 41/41, seed 25/25, policy 17/17, implied-forward 26/26, ctest 5/5 + 2/2, pytest 149/149, PCH-off 17 TUs; parity reruns skipped (reviewer proved c89d0739, 6b129f64, definitions). MINOR-1 closed at integration: m1/m3/m5/m6 seed results before `811282cf` and composites on m6 must be rerun before held-out use. Deferred: MINOR-2 untested carry fallback causes InvalidMarket/NoModelYield/SliceRefused and Parity Contaminated exclusion; NIT-1 few-ulp retry toward a bound; NIT-2 budget-blocked retry reports jacobian_failure not evaluation_limit; NIT-3 fallback:invalid keys also renamed (now in the integration ledger line), quote-slice report schema not bumped from 2 |

Pending: serve-path records lane; smooth dated-dividend lattice lane;
composite benchmark on both cohorts; knot-first ladder.
