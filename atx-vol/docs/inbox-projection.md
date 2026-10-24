# Exact in-box projection rung (lane C4, r6 2026-10-23; opt-in)

Key: `QuoteSliceRepairConfig::inbox_projection` (bench: `graded.inbox_projection: true`, which
requires `certificate: box`). Default off; inert under the exact certificate. Code:
`src/fitting/inbox_projection.{hpp,cpp}`; one call site, `box_certify_slice` in
`src/fitting/quote_slice.cpp`; tests `tests/inbox_projection_test.cpp`.

## Why

The C3 review solved the in-quote certificate LP on SPY's raw quotes: 31 of 33 expiries admit a grid
that passes `AmericanPriceSlice::create` strictly inside every [bid, ask], yet the box certificate
(`certify_american_box`) refused 11 of them at h = 0. On SPY every one of those refusals is the
same: its per-side Dykstra projection does not settle in 20,000 sweeps at 1e-13 x the price scale
(`QuoteSliceBoxRefusal::NotSettled`, typed by this rung). Dykstra's cyclic projections onto the
three-node convexity half-spaces move information one node per sweep, so a correction that spans
a few hundred SPY strikes does not converge in that budget, and the slice is refused as if no
curve existed. The per-side projection also ignores the affine call-minus-put bound, which
couples the sides (`CertificateRefused`).

## What it solves

Per expiry, the fitted node prices are projected onto the set below. Every inequality is exact;
the only allowance is the certificate's own `1e-8 + 1e-9 max(S, K)`, used solely to pin a box that
misses an exercise bound by less than that.

- each node side inside its box (the same boxes `certify_american_box` received: a quote's
  [bid - h, ask + h]; a side without a quote held at its fitted price) intersected with the
  certificate's node bounds (intrinsic, upper);
- each side convex in strike (consecutive triples), call slopes in [-discount_max, 0], put slopes
  in [0, discount_max] (the end slopes plus convexity imply every vertical-spread bound of
  `create`);
- at every node, prepaid_min - K discount_max <= call - put <= prepaid_max - K discount_min;
- in the non-dividend regime (prepaid_min = prepaid_max = spot, discount_max = 1, i.e. the regime
  `certify_and_serve` checks with `check_nondividend_american`): the discounted-intrinsic call
  floor S - K df, call slopes >= -df and the Legendre segment condition. The scaled European upper
  bound (C(K / df)) is not banded and is left to `check_nondividend_american`.

Objective: least squares to the fitted prices in units of each side's box half-width (half the
quote spread at h = 0), so a tick-wide at-the-money quote resists moves that a wide wing quote
absorbs.

## How

1. Exact infeasibility tests, each naming its node: `EmptyBox` (box outside the node bounds),
   `NoConvexCurve` (a lower edge above the greatest convex curve with the side's slopes under the
   upper edges - an exact test: every feasible curve lies under that curve, which is itself
   feasible), `AffineBox` (no call / put pair within reach meets the affine bound at a node).
2. Rows: box rows for every free side; convexity, end-slope, affine (and non-dividend) rows,
   dropping any row the boxes already imply. Variables interleave call / put per node, so every
   row spans at most five consecutive variables and the Newton system is banded (half-bandwidth
   4; Cholesky O(n)).
3. Phase I (log barrier on max violation s): s < 0 proves an interior point; a certified lower
   bound s - m / t > 0 proves the set empty (`Coupled`, with that bound as the excess and the row
   of the largest dual weight as the node).
4. Phase II (log barrier on the projection objective) from that interior point; Newton steps are
   capped at 99 % of the exact distance to the nearest row and backtracked on the barrier
   objective, so every iterate is strictly inside every row. Stops at m / t <= 1e-5 (half-spread^2).
5. Snap: a side within 1e-6 of a half-width of its fitted price returns to it exactly (the exact
   projection leaves every side no active row touches unmoved; the barrier only biases it), with
   any row that loses its strictly positive slack restoring its snapped sides.
6. The UNCHANGED `AmericanPriceSlice::create` certifies the result; only then is it served (moved
   sides flagged and re-implied exactly like any repair's).

Outcomes (`QuoteSliceInboxRecord` on `QuoteSliceDiagnostics::box.inbox`): `Applied`,
`InfeasibleData` (+ conflict, side, node, strike, excess), `NonConverged` (600 Newton steps,
phase I + II), `NotCertified`, `InvalidInput`. Nothing changes on any outcome but `Applied`; the
slice then refuses exactly as before (the exact certificate's error). Calendar consistency across
expiries is not part of this per-expiry rung: the surface's calendar passes and C2's publication
validation run after it unchanged.

## Measured

Arm `C4-inbox-h0` = C3's `combined-h0` + `inbox_projection: true`, against `combined-h0` on the same binary:
- K4's SPY input: served expiries 17 -> 26 / 32 fitted (bar 31 / 33: MISS); the rung ran 18 times, all
  `applied`, each a box-rung `not_settled` refusal; 0 lost rows; 0 publication / label violations.
- cohort5 / dev200 / dev200b eligible coverage 60.08 -> 80.77 / 88.33 -> 92.27 / 93.68 -> 94.64 %;
  containment 97.49 -> 98.65 / 98.97 -> 99.01 / 99.03 -> 99.02 %; 0 violations; lost rows 0 / 0 / 44
  (dev200b, 2 chains, typed `calendar_arbitrage`). dev200: 2 `infeasible_data` (`affine_box`).
- Cost (SPY cold row, 1 worker): the rung 10.7 ms median per projected expiry (max 22.3), but the cold
  wall 17.4 -> 63.5 s: re-implication of the moved nodes and the calendar passes over the newly
  served slices, not the projection.
- What still refuses on SPY is the joint calendar rung's non-settling Dykstra (6 `calendar_arbitrage`
  expiries, 2,504 eligible rows); a joint LP shows 31 expiries calendar-consistent strictly in-quote.

Report: `.superpowers/sdd/2026-10-18-vol-sota/lane-C4-inbox-projection-r6-report.md`; evidence
`C:\atx-cache\quote-benchmark\C4-20261023-inbox\`.
