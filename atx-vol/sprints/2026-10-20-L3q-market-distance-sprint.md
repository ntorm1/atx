# Sprint note — L3q market-distance: certify the distance to the market, repair inside the quote boxes (2026-10-21)

Lane `L3q-market-distance`, branch `feat/vol-L3q-market-distance-20261020`, base `09a3c837` (L3r1's
approved tip). Pool-34, RunId `vol-20261021r4-L3q`, Opus 5.5. Brief:
`.superpowers/sdd/2026-10-18-vol-sota/lane-L3q-market-distance-brief.md` (+ PM additions r4). Evidence
`C:\atx-cache\quote-benchmark\L3q-20261021-market-distance\` (`RESULTS.md`, `sha256.txt`). Every behaviour
is opt-in (`graded.market_distance`, default `off`); keys OFF the five parity hashes are byte-identical.

## 1. Why

The certificate set (exact or L3r1's box) proves a served curve arbitrage-free; it does not bound how far
that curve sits from its own quotes. L3p: parametric arms certified 2-9 spreads off the market; L3r1 /
review m2: on SPY h = 0.5, 91 rows of 2 chains certified by the EXACT certificate sit up to 0.243 USD
outside [bid - h, ask + h]. And the remaining SPY refusals after L3r1 are calendar crossings no
one-sided repair can absorb.

## 2. What (all opt-in, `graded.market_distance = off | gate | repair`, bound `market_distance_spreads`)

- Distance in the scorer's unit: `market_distance_spreads(price, bid, ask, tick)`
  (`american_price_slice.hpp`): 0 inside, (price - ask) / s above, (bid - price) / s below, s = max(ask -
  bid, tick); zero bid = band [0, ask] (the audit's zero-bid stratum, spread = ask).
- Gate (`gate`, `repair`): last step of `fit_quote_surface`, every served Success / PriceOnly row beyond
  the bound becomes `MarketDistanceFailure` (`market_distance_failure` in the bench CSV) — ROW refusal, the
  certified slice is unchanged, so every served subset stays arbitrage-free (check_served conditions hold
  on subsets). Bound 0 = strict containment.
- Bidirectional calendar rung (`repair`): after every one-sided calendar rung failed, the crossing slice
  is projected JOINTLY with the accepted slices (first those it crosses, the others fixed; then every
  accepted slice) — `project_calendar_joint` (sparse Dykstra / Hildreth over each slice's convexity + end
  slopes and the calendar inequalities between members, boxes = quote boxes [bid - h, ask + h] widened to
  contain the current price), then the unchanged certificate and the full calendar check on every member
  (`repair_quote_slice_calendar_joint`). Moved accepted slices are re-implied and graded B.
- In-box market repair (`repair`): per served slice with a row beyond the bound, in maturity order, the
  slice is projected onto [bid, ask] (then onto the bound's band if no curve fits the quotes) under the
  calendar clamp of every other served slice (`repair_quote_slice_market`), certified, calendar-checked,
  committed as a repair (moved nodes re-implied, PriceOnly where unidentifiable). Then the gate.
- `tools/check_served.py` brought into the tree (byte-identical default output, verified by sha256 on
  an L3r1 CSV) with the opt-in `--market-distance [BOUND]` class.

## 3. Mechanism found (the "far-wing bidless" SPY crossings)

The remaining SPY calendar refusals (gaps 0.5-2 cents at K 793-885, SPY spot 773) are on the PUT side:
deep in-the-money puts sit at intrinsic K - S, and the per-expiry spot medians differ by up to 1.5 cents
(772.98 / 772.99 / 772.995 / 773.00), so an earlier expiry's intrinsic exceeds a later one's. The fix
needs the later slices lifted (a chain of maturities) — one-sided repairs and a pair projection with the
other slices fixed cannot do it; the joint rung's second attempt (all accepted slices move) can, where
the boxes allow. Ledger candidate below.

## 4. Numbers (binary `948f8d86`, source `13c279f1`; full tables + commands: evidence `RESULTS.md`)

Coverage / containment / MAE-spread (SpiderRock yardstick only; scorer = the audit's):
- SPY: keys-OFF 10.96 % / 68.2 % / 0.773; L3r1 h0 10.38 / 100 / 0.148 -> + gate same -> **+ repair 31.03 / 99.60 / 0.178**;
  L3r1 h0.5 44.6 / 87.2 / 0.250 -> + gate 44.5 / 87.4 / 0.244 -> + repair 56.9 / 86.9 / 0.251 -> **+ repair bound 0
  52.7 / 100 / 0.210**; ladder-strict 14.4 / 96.3 / 0.137 (gate / repair byte-identical on SPY). Chains served h0 4 -> 10,
  h0.5 14 -> 18 of 32.
- cohort5: OFF 38.4 / 91.6 / 0.209 -> gate 37.7 / 93.2 / 0.131; h0 39.8 / 97.1 / 0.107 -> repair 53.0 / 97.9 / 0.123; h0.5
  60.0 / 92.2 / 0.166 -> repair 69.1 / 91.9 / 0.170 -> strict 65.8 / 100 / 0.139; ladder-strict 44.3 / 98.9 / 0.070 -> repair
  45.5 / 99.0 / 0.069.
- dev200b: h0 90.4 / 98.88 / 0.1152 -> repair 92.9 / 99.03 / 0.1137; ladder-strict 89.5 / 99.80 / 0.0999 -> repair 92.3 /
  99.82 / 0.0996. dev200: h0 83.5 / 98.91 / 0.1169 -> repair 87.1 / 98.99 / 0.1168; ladder-strict 85.6 / 99.75 / 0.0954 ->
  repair 88.4 / 99.78 / 0.0940.
- On every served-in-both row set the repair arms improve (or keep) containment AND MAE/spread vs their base; aggregate
  MAE/spread rises on cohort5 / SPY only through composition (the newly served rows are harder rows).
- check_served (tree copy, `--market-distance`): 0 monotone / convex / calendar / market_distance violations on all 26 ON
  CSVs; the gate refuses exactly the rows the checker flags on every keys-OFF / L3r1 CSV (6 pairs).

## 5. Hashes

Keys-OFF RAW parity on the lane binary: dev200 auto `c89d0739`, dev200 graded-v1 `fc277413`, dev200b graded-v1 `46ad6c26`,
cohort5 graded-v1 `171fa2d2`, cohort5 ladder point `232d2c0d` — 5 / 5 MATCH; L3r1 SPY h0.5 `a3859ea1` / h0 `05f64ba6` MATCH.
New keys-ON (SPY): gate `97baf9db`, repair `672d96ef`, h0 + gate `05f64ba6`, h0 + repair `2145268a`, h0.5 + gate `ffb72607`,
h0.5 + repair `9ff102bc`, strict `3d6224a5`, ladder-strict (+ gate / repair) `80ba43a9`; cohort5 / dev in `runs.log`.

## 6. Traps

- The SPY "far-wing bidless" calendar crossings are deep-ITM PUT intrinsic crossings caused by per-expiry spot medians a cent
  apart (§3); the call side is not the problem.
- A local slack on the joint projection's convexity / calendar constraints accumulates (the certificate's global envelope;
  a chain of maturities) — the joint projection is exact; tried and reverted during the lane.
- The CLI's synthetic quotes are 0.002 wide: the gate (one-tick spread floor) is NOT inert on them at bound 1.

## 7. Remaining

- Joint projection convergence: SPY joint attempts that are feasible but do not settle in 20,000 sweeps (Dykstra on
  500-1,000 coupled variables); an ADMM / active-set QP would recover more chains.
- The per-expiry spot inconsistency itself (one snapshot spot per underlier) is an input-layer fix (out of scope).
- h0 / h0.5 x bound: the promotion decision is the PM's on the combined table (ruling 6a-5).
