# Slice-repair sprint (2026-10-17)

Lane `slice-repair`, branch `feat/vol-slice-repair-20261017`, frozen base
`7ca31c95d5297aea412f05f980a2d7af3ad79d21`, pool lease `pool-4` (run id
`vol-20261017-slice-repair`). Commits: `8baa143b` (feature), `3087a641`
(deferred minor). Requirements: `.superpowers/sdd/2026-10-17-vol-resume5/`
SHARED-BRIEF and `lane-slice-repair-brief.md`. Goal: stop the SPY-class
whole-slice refusals and the CRWV-class channel escape seen in the 2026-10-16
smile figure, behind opt-in policy keys, without touching any default.

Artifacts (all new folders, each with `sha256.txt`):
`C:/atx-cache/quote-benchmark/slice-repair-20261017-{repro,diag,policies,bin,bench,gates}`
and `C:/atx-cache/quote-benchmark/plots-20261017` (the figure). Numbers below
come from `slice-repair-20261017-bench/RESULTS.md`. Timings are unqualified
(loaded host).

**Fix round 1** (commit `7d883fa1`, section 9) follows a BLOCK review of
`04203854`. It replaces the dominance-gate behaviour described in sections 3, 6
and 7, together with their gate numbers, and it corrects the disclosure in
section 8. Sections 1-8 are kept as the first lane wrote them.

## 1. Reproduction at 7ca31c95

The base binary (`slice-repair-20261017-bin/base-7ca31c95`, sha256 `df8a2613...`)
on the five-symbol 10:30 cohort with graded-v1 reproduces the figure's fit run
byte for byte (`7fa3c415...bb577`). In that run:

- **SPY**: every 20-60 d expiry (23, 30, 37, 44, 47 d; 300-564 two-sided
  quotes) is refused `arbitrage_failure`. The pass-A dense point fit fails the
  strict certificate on micro-kinks ("call/put convexity violated above the
  global lower convex envelope"). The channel refit fails too: its bounded
  repair would need 8-15 half-spreads. The kinks are small and local. Two
  causes: per-strike supplied carry, which moves 12-27 nodes by up to 7e-5;
  and the deep put wing, where the knot curve's served multiple is capped at
  `mult_ceil` = 3 x ATM while the SPY market wing reaches about 4.1 x ATM, so
  the wing goes flat and then droops.
- **AAPL / MSTR Nov 20** (37 d): pass-A fits served, then refused
  `calendar_arbitrage`. The crossings need 3.6 and 1.0 half-spreads, beyond
  the 0.5 bounded repair. For AAPL the crossing is against the deep-wing
  extrapolation of a neighbouring convex-dense point fit, which is meaningless
  outside its admitted band (a 0.00035 floor, then linear).
- **CRWV Nov 20**: served as a calendar-repaired pass-A **eSSVI point fit**
  (grade B), outside bid/ask on 17 of the 41 plotted strikes.

## 2. Channel repair (opt-in, `graded.repair: channel`)

`QuoteSliceRepairConfig` (`quote_slice.hpp`) runs only after the strict
certificate (and the bounded-move convexity repair, when enabled) has failed.
It works on the **fitted** node prices in two steps:

1. Lower only the nodes that sit more than the certificate allowance above
   their side's lower convex envelope. This is exact and cheap, and enough for
   isolated kinks.
2. Otherwise, project each side (Dykstra/Hildreth, uniform weights, at most
   20,000 sweeps) onto the set the certificate's strike checks describe:
   convex, end slopes in [-discount, 0] / [0, discount], intrinsic and upper
   bounds, and each node inside its own quote's [bid, ask] widened to contain
   its fitted price. Sides without a quote are held fixed.

A step is accepted only if all of the following hold:

- no moved node ends farther outside its own channel;
- at most `repair_max_nodes` strikes move by more than
  `repair_immaterial_price` (default $5e-4, the pricing-accuracy floor; carry
  noise moves many nodes by a few 1e-5);
- moved node-sides without their own quote change by at most `repair_max_vol`
  in American IV.

The unchanged certificate then runs again. Each step records its outcome
(applied, no_move, infeasible, left_channel, node_budget, vol_budget or
not_certified), the nodes and material nodes moved, and the largest price and
IV moves. Moved nodes are re-implied like the other repairs.

Under `fit_quote_surface` the repair runs only where a check has already
failed:

- pass A runs without it;
- the channel refits (B1, B2 and the calendar retry) run with it, after their
  bounded-move repair;
- an expiry still refused for a failed certificate gets its pass-A fit again
  with the repair (the **repair rung**, grade B);
- a calendar crossing that neither the bounded-move repair nor the
  stronger-prior retry fixes is offered `repair_quote_slice_calendar_in_channel`
  on its own prices, then on the retry's. This is the same projection with
  every box also clamped to [max earlier, min later]; it is a public callable
  step that the knot-first ladder can reuse.

A repaired slice is never A. A newly served expiry still feeds the B2 prior and
the greedy calendar order, so it can displace a later candidate. The benchmark
reports these as lost rows.

Development findings that shaped the budgets:

- A plain node budget is consumed by immaterial carry noise. Hence the
  material-move floor.
- An IV budget on every moved side refuses deep-wing, sub-tick nodes: MSTR
  needed 0.25 vol and AAPL 0.20 on nodes worth less than a tick. Hence the
  budget covers only unquoted sides, where no channel protects the price; IV
  moves are still recorded for every side.
- Lowering alone cannot fix drooping wings. Hence the projection.
- Spread-scaled projection weights (ratios up to about 1e5) converged far too
  slowly: SPY 37 d needed 400k sweeps and 104 s per surface. Hence uniform
  weights, with the channel carried by the box.

## 3. Channel dominance (CRWV), diagnosed before any objective change

The CRWV escape is not a channel-objective problem. The served Nov 20 slice is
a pass-A eSSVI point fit. The rigid three-parameter family cannot follow
CRWV's smile, and its fit is outside bid/ask on 25.6% of its two-sided quotes.
Graded admission still counts that fit as pass-A evidence. The channel weight,
the wing cap and the term prior therefore never act on this slice. The channel
refit of the same expiry, with an unchanged objective, is outside on 1 of 41
plotted strikes.

The one targeted change is therefore an opt-in gate:
`channel.point_max_outside_fraction` (per profile, graded only). A pass-A
point fit outside bid/ask on more than that share of its strictly two-sided
quotes is treated as a channel candidate. The point fit is kept as fallback
and served as B (`point_fallback`) only when no refit is served. A first version (one surface-wide key, no cap change) lost SPY. The 65 d dense
fit is 61% outside, so it is gated, and its channel refit is infeasible; that
reorders calendar acceptance. The key is now per profile so that a policy can
scope it. repair-dominance-v2 still sets it on all three profiles, and the
dense `mult_ceil 6` is what keeps SPY served: repair-gate-v2, without the cap
change, serves no SPY 20-60 d expiry.

## 4. Deferred minor (separate commit 3087a641)

`fit_quote_slice` dispatched a non-Supplied carry mode to carry resolution
before `validate()`. The implied-forward path replaces every contract yield, so
a NaN supplied yield was **served** (red test recorded in
`slice-repair-20261017-gates/item4-red-without-fix.txt`). Inputs are now
validated first; the Supplied path is unchanged.

## 5. Gates (at 3087a641)

- **Tests.** `atx-vol-quote-slice-tests`: 92 tests (84 existing + 8 new):
  - micro-kink served only with the key, inside its channels, re-implied;
  - node-budget refusal;
  - infeasible or left-channel refusal;
  - channel calendar repair: the bounded repair fails while the channel repair
    lifts onto the floor inside the quotes; node budget; infeasible floor;
  - invalid repair and gate configs, which are Err for the slice and the
    surface;
  - surface repair, with neighbours bit-identical and A;
  - dominance gate refit, with a limit of 1 bit-identical to off;
  - gated point fallback.

  The new carry test is in `atx-vol-implied-forward-tests` (25 tests). Two
  pytest cases were added to `test_quote_bench_cli.py`: key rejection, and keys
  ON being byte-identical on clean quotes.
- **ctest.** `-Ctest -Preset vol-rel` with `atx-vol-quote-slice`,
  `atx-vol-implied-forward` and `atx-vol-quote-benchmark-{tools,native,campaign}`:
  5/5 passed. `build-vol-rel` was reconfigured with
  `-DPython3_EXECUTABLE=C:/Program Files/Python312/python.exe`.
- **PCH off.** `vol-hygiene check`, 0 warnings, on `quote_slice.cpp`,
  `quote_surface.cpp`, `quote_bench_main.cpp`, `quote_bench_common_policy.cpp`
  and the three test TUs. `quote_slice.hpp` and `quote_bench_policy.hpp` are
  covered through those TUs.
- **Keys-OFF parity** (final binary `9233d372...`):

  | Run | sha256 |
  |---|---|
  | dev200 auto | `c89d07393c90ad86860a6bcacd3d1a84ede85f4647d9cce136567e34763d5b7c` |
  | dev200 graded-v1 | `1348f63dba576874653663be67b69389d8360efd2339bf48215587be0b1fe377` |
  | dev200b auto | `4b273b084231f8a3811c7d6505d42500355ae29c864a5c2e1c2b964a7aab70fb` |
  | dev200b graded-v1 | `4c70d2bce706cd75c189cd08b65829c7c64a962e7f75f9cc1eb0b3e10f41da79` |
  | cohort5 graded-v1 | `7fa3c415...bb577` |

  dev200b graded-v1 equals the base binary's output on the same input.

## 6. Benchmarks, keys ON (vs graded-v1)

Policies:

- `repair-v2` = graded-v1 + repair (32 strikes, 0.01 vol).
- `repair-dominance-v2` = repair-v2 + gate 0.05 on every profile + dense
  `channel.mult_ceil` 6. The cap change is policy only: it lifts the SPY wing
  cap.

**dev200** (63,942 eligible rows):

| Policy | Coverage | Intersection MAE (rows) | Newly served (MAE USD) | graded-v1 rows lost |
|---|---|---|---|---|
| graded-v1 | 78.74% | | | |
| repair-v2 | **83.71%** | **+0.04%** (49,112; 47,128 identical) | 3,885 (**0.160**) | 719 (calendar displacement, 6 chains) |
| repair-dominance-v2 | **84.53%** | **-1.45%** (47,497) | 5,982 (**0.173**) | 2,334 |

Own population, repair-dominance-v2:

| | MAE USD | MAE/spread | Containment | p99 |
|---|---|---|---|---|
| graded-v1 | 0.2343 | 0.1085 | 0.988 | 1.97 |
| repair-dominance-v2 | 0.2226 | 0.1133 | 0.991 | 1.87 |

Done criteria (coverage >= 78.7%, intersection within +1%, newly served MAE
<= 0.30) hold for both policies. Attribution on dev200: the dense
`mult_ceil 6` is immaterial (repair-ceil6-v2 = repair-v2 to 1e-5 USD), and the
gate carries the whole difference.

**dev200b** (62,369 eligible rows, no threshold):

| Policy | Coverage | Intersection MAE | Newly served |
|---|---|---|---|
| graded-v1 | 87.15% | | |
| repair-v2 | 90.27% | +0.12% | 2,209 at 0.277 (p99 4.41) |
| repair-dominance-v2 | 89.98% | -2.10% | 2,676 at 0.298 |

The independent shape check (monotone, convex, calendar order at common
strikes) finds 0 violations in every served set on all three cohorts.

## 7. Figure (the lane's gate)

`plots-20261017/smile-comparison-20261014-slice-repair-v2.png` uses
repair-dominance-v2 with the final binary and keeps the per-panel CSVs.

- **SPY**: served on 23 d (shown; 112/118 inside, 2 bp vs srVol) and 44 d.
- **AAPL Nov 20**: served, 42/42 inside, 6 bp.
- **MSTR Nov 20**: served, 51/51 inside, 35 bp.
- **CRWV Nov 20**: outside on **1 of 41** (was 17), 75 bp (was 226).
- **RIVN**: shows 44 d, 427 bp in a 34-vol-point-wide market.

**Regression:** RIVN serves 1 of its 4 expiries in 20-60 d (was 4). The cause
is the gate alone: gate-v2 also loses RIVN 30 d and 37 d, because gated point
fits become channel refits that cross their neighbours.

## 8. Concerns / follow-ups

- The budgets, the 0.05 gate and `mult_ceil 6` were chosen with the five
  figure slices in view. dev200 and dev200b are development cohorts, not a
  holdout.
- Displacement: newly served expiries can push previously served ones out
  through calendar order (dev200: 719 rows with repair-v2, 2,334 with the
  gate).
- The gate cost RIVN coverage on the cohort. A minimum outside-count (not only
  a share) for tiny chains is the obvious next variant; it is not done here.
- `repair_quote_slice_calendar_in_channel` holds unquoted sides fixed, so a
  crossing on an unquoted side stays a refusal.

## 9. Fix round 1 (review of 04203854: BLOCK, 2 MAJOR)

Run id `vol-20261017-slice-repair-fix1`, pool lease `pool-3`, code commit
`7d883fa1`. Artifacts (new folders, each with `sha256.txt`):
`C:/atx-cache/quote-benchmark/slice-repair-20261017-{policies-v2,bin-v2,diag-v2,bench-v2,gates-v2}`
and `plots-20261017-v2`. Numbers come from `slice-repair-20261017-bench-v2/RESULTS.md`.

### MAJOR-1: the gate no longer refuses

The cause was the order of the passes. The gate replaced a point fit with its
channel refit **before** the calendar pass. A refit that then crossed a
neighbour was refused, and the point fit was never offered again. It also had
no minimum count, so RIVN 23 d was gated at 3 of 48.

The dominance gate (`point_max_outside_fraction`) now runs **last**: after
pass A, the channel passes, the repair rung and the calendar pass. Its steps:

1. **Measure.** It measures the final point fit of each expiry, served or
   refused only for a calendar crossing. It gates the fit when the share of
   two-sided quotes outside [bid, ask] is above the limit **and** the count is
   at least the new `channel.point_min_outside_quotes` (default 1; a policy
   key that requires the fraction).
2. **Offer a refit.** It offers a channel refit under the prior of the served
   expiries, with gated point fits left out of the prior.
3. **Adopt only if consistent.** It adopts the refit only when the final
   served set stays calendar-consistent with it. The bounded-move and channel
   calendar repairs are allowed. Adoptions that still cross are undone, lowest
   confidence first.
4. **Otherwise keep the point fit.** The point fit is kept (`point_fallback`),
   and a served A becomes B.
5. **Re-admit.** Once a refit has been adopted, calendar refusals are
   re-checked against the final served set (`calendar_readmitted`).

So the gate never refuses an expiry and never changes another served one. It
also covers the repair rung (minor a): the rung's fits are measured like any
other point fit.

Results, all keys ON:

- **dev200.** Coverage 84.89%, against 84.53% for the reviewed gate and 83.71%
  for repair-v2. The intersection with graded-v1 is 49,112 rows, at -1.49% MAE.
  4,626 rows are newly served, at MAE $0.138. 719 graded-v1 rows are lost:
  exactly repair-v2's calendar displacement, and **0 from the gate** (the
  reviewed gate lost 2,334). 48 slices are gated: 28 refits adopted, 11 point
  fits kept, and 9 that were already calendar refusals stay refused.
- **dev200b.** Coverage 90.49% and -2.07% intersection MAE. 2,356 rows are
  newly served, at $0.331; 286 are lost, all repair-v2's. Against the same
  keys minus the gate (repair-ceil6-v2), the gate run is a strict superset.
- **Cohort.** Coverage 62.73%. 146 rows are lost, all repair-v2's.
- **RIVN** is back to **4 of 4**. Its 37 d slice (6 of 30 outside) keeps its
  point fit as B.
- **CRWV Nov 20** is still served by its refit, with 1 of 41 strikes outside.
  **MSTR Nov 20** is served. **SPY** is served on 44 d.
- **AAPL Nov 20 is refused again** (calendar), as in graded-v1. Neither its
  refit nor its own fit is consistent with the served AAPL expiries. The first
  lane served it only by refusing AAPL 5 d, 128 d and 246 d. Serving it
  without that loss needs the confidence-ordered eviction, which the PM
  deferred. The first lane's Done criterion "AAPL Nov 20 served" therefore no
  longer holds.
- **Minimum count, 4 vs 1.** It changes no served price on dev200 or cohort5;
  only the grade differs, on 40 and 38 rows.

### MAJOR-2: tests that fail on revert

These are in `atx-vol-quote-slice-tests`; there are now 97 tests. Each was run
against its revert, with logs in `slice-repair-20261017-gates-v2/revert-*.txt`:

- **`QuoteSurface.ChannelCalendarRepairServesACrossingTheBoundedRepairAndRetryCannot`.**
  The bounded-move repair is off and no refit can be served. The later expiry
  is served B with `calendar_channel_repaired`, every price inside its quote,
  and calendar-consistent. Removing the two calendar-stage `repair_in_channel`
  call sites fails it: grade Refused.
- **`QuoteSliceChannel.RepairVolBudgetBindsOnlyUnquotedNodeSides`.** The chain
  is quoted out of the money only, and the refusal is VolBudget. It fails on
  revert of the step-outcome fix (the record said Infeasible).
- **`QuoteSliceChannel.ChannelCalendarRepairIsNotCertifiedWhenANeighbourKnotStillCrosses`.**
  A later neighbour has knots between this slice's nodes.
  `repair_quote_slice_calendar_in_channel` now runs `check_american_calendar`
  against every earlier and later slice, so the result is NotCertified and the
  slice is unchanged. Without that check the repair returned true.
- **`QuoteSurface.DominanceGateNeedsTheMinimumOutsideCount`.** It fails with
  the count condition removed.
- **`QuoteSurface.DominanceGateNeverCostsAnExpiryTheCalendarPassServed`.** This
  is the review's RIVN case. A gated expiry's refit crosses a served
  neighbour, while its point fit does not. The expiry is served (point kept,
  B) and every price equals gate-off. With the old order emulated (gate before
  the calendar pass, refit adopted unchecked), it fails with
  `1 calendar_arbitrage American calendar price crossing`.
- **`QuoteSurface.DominanceGateKeepsThePointFitWhenNoRefitIsServed`.**

Two earlier tests are now pinned to the path that actually fires
(review NIT):

- `RepairRefusesWhenNoConvexPriceSetFitsInsideTheQuotes` expects LeftChannel.
- The micro-kinked surface test expects the channel refit, not the rung.

### Minors

- **(b) Stale bounded-repair diagnostics.** `repaired_nodes` is reset to 0
  when the bounded result is discarded for the channel repair. On dev200 this
  goes from 7 stale slices to 0.
- **(c) Disclosure, corrected.** These were all chosen during development,
  with the five figure slices and the dev200 aggregates in view:
  - the budgets: 32 strikes, and 1 vol point for unquoted sides;
  - the 0.05 gate;
  - `point_min_outside_quotes` 4;
  - the dense `mult_ceil 6`;
  - the $5e-4 immaterial-price floor (`repair_immaterial_price`, a library
    default the policies inherit);
  - the 20,000-sweep projection cap;
  - the uniform projection weights.

  SPY is served by the dense `mult_ceil 6` plus the bounded-move calendar
  repair, **not by the channel repair**. In the first lane's figure policy,
  SPY 23 d and 44 d had no channel-repair move. Under v3, SPY 44 d is
  cal.-repaired only.
- **(d) What the vol budget bounds.** `repair_max_vol` binds only on sides
  without a quote, and those sides move only through the residual-lowering
  step, because the projection holds them fixed. A quoted side is bounded only
  by its own channel, [min(bid, fitted), max(ask, fitted)]; its IV can move by
  that channel's IV width. On dev200, over 58 applied repairs,
  `max_unquoted_vol_move` is 0.0 everywhere and the largest quoted-side move
  is 3.71 vol points. So on real chains the budget does not bind; the test
  above shows where it does. This is stated in `quote_slice.hpp` and in the
  quote-bench semantics text.
- **Deferred by PM ruling, not done here:**
  - confidence-ordered eviction (MINOR-1), the 719 and 286 displaced rows;
  - point fits escaping the market when the gate is off (MINOR-2).

### Gates (at 7d883fa1)

Logs are in `slice-repair-20261017-gates-v2/`.

- **Tests.** quote-slice 97/97; implied-forward 25/25; quote-bench pytest 48
  passed.
- **ctest.** The 5 focused ctests passed.
- **PCH-off.** `vol-hygiene check`, with 0 warnings and 0 errors, on 7 TUs:
  `quote_slice.cpp`, `quote_surface.cpp`, `quote_bench_main.cpp`,
  `quote_bench_common_policy.cpp` and the 3 test TUs.
- **Keys-OFF parity on the final binary** (`021cb029...`):

  | Run | sha256 |
  |---|---|
  | dev200 auto | `c89d0739...` |
  | dev200 graded-v1 | `1348f63d...` |
  | dev200b auto | `4b273b08...` |
  | dev200b graded-v1 | `4c70d2bc...` |
  | cohort5 graded-v1 | `7fa3c415...` |

  repair-v2 is byte-identical to the first lane's output on all three
  datasets.
- **Shape check.** `check_served.py` finds 0 violations in every served set.
- **Figure.** `plots-20261017-v2/smile-comparison-20261014-slice-repair-v3-r2.png`.
