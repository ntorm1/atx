# L3a wing-tail sprint (vol-sota resume 4, 2026-10-21)

Lane `L3a-wing-tail` (Opus 5.5), branch `feat/vol-L3a-wing-tail-20261020`, base `be379ad4`, pool-33 (RunId
`vol-20261021r4-L3a`). Brief: `.superpowers/sdd/2026-10-18-vol-sota/lane-L3a-wing-tail-brief.md`; report
`lane-L3a-wing-tail-report.md` beside it. Evidence: `C:/atx-cache/quote-benchmark/L3a-20261021-wing-tail/`
(`RESULTS.md`, `sha256.txt`). srVol / srPrc are scoring labels only.

## Why

L3p: ladder-strict beats graded-v1 on every cohort's all-rows cell, except a vol-bp MAE regression on
`mega_cap_single` (+16 % dev200, +18 % dev200b) and dev200 `long_dated` (+16 %) with medians better.

## Mechanisms (receipts reused by sha256)

1. **Repaired zero-time-value wing node served at the inverter floor (dominant).** `repair_convexity` flags a
   node side moved for any lowering onto the convex minorant; a moved row reports the re-implied volatility of
   its served price (knot-ladder §9). A deep OTM side priced ~0, or an ITM side at its zero-vol value, inverts to
   `kIvMin` = 0.005 (the at-zero-vol clamp) and was served as a measured volatility: 388 / 346 / 93 rows on
   dev200 / dev200b / cohort5. Paired excess vs graded-v1: dev200 mega-cap 476k of 562k bp from 60 rows; dev200b
   662k of 754k from 140 rows. Example: dev200 META 2026-11-06 puts K 100-160 (bid 0 / ask 1.02) at 0.005 vs
   srVol 2.0-2.3.
2. **Knot wing droop beyond the last bid (secondary; LEAPS and ultra-short).** Beyond the outermost channel
   with a lower vol bound, only upper bounds remain; the term-prior-free knot rung's flat default shape and the
   natural spline pull the wing back toward the money (META 2027-12-17: 0.68 at k -2.2 -> 0.63 at k -2.5 while
   srVol rises to 0.98).

## Levers (one key: `graded.ladder_wing_tail`, knot-first ladder only, default `off`)

| commit | mode | library | what |
|---|---|---|---|
| `be9b5907` | `floor-price-only` | `QuoteSurfaceWingTail::FloorPriceOnly` | after the calendar pass, a served Success row at a moved node side re-implied at exactly `kIvMin` becomes PriceOnly (price kept, NaN vol, `price_only_nodes`); no price changes |
| `4829ccc0` | `floor-price-only+wing-floor` | `FloorPriceOnlyWingFloor` + `QuoteSliceChannelConfig::wing_floor` / `ChannelFitConfig::wing_floor` | lever 1 + the knot second solve floors every observation beyond each side's outermost lower-bounded one at the first solve's fitted vol there (below its own ask bound only); changes prices on purpose |

## Numbers (`score --split all`, class manifest `ad323ccb`, `ATX_VOL_FIT_WORKERS=2`)

Vol MAE bp, graded-v1 / ladder-strict off / + L1 / + L1+2:

| cell | graded-v1 | off | L1 | L1+2 |
|---|---|---|---|---|
| dev200 mega-cap | 378.6 | 440.6 | **344.8** | 282.3 (price cov 81.38 -> 77.55 %) |
| dev200b mega-cap | 464.9 | 552.1 | 482.2 | **424.2** (86.82 -> 86.10 %) |
| dev200 long-dated | 197.8 | 229.8 | 222.3 | 202.6 |
| dev200b long-dated | 254.9 | 244.6 | 239.6 | 232.9 |
| cohort5 mega-cap (AAPL) | 664.0 | 499.9 | 376.6 | 235.8 (68.29 -> 86.58 %) |
| all rows dev200 / dev200b / cohort5 | 1531 / 1450 / 372 | 1525 / 1533 / 404 | 1465 / 1487 / 313 | 1349 / 1404 / 220 |

Lever 1: vol MAE lower in every class of every cohort; price coverage, MAE/spread, containment, p99 identical;
vol coverage -0.05 to -3.09 pts (rows moved to `ok_price_only`). Served-set diff: every changed row is a
key-off 0.005 row turned `ok_price_only` with the same price (cohort5 93/93, dev200b 346/346). Lever 2: vol MAE
and MAE/spread lower almost everywhere, price coverage +5.4 pts cohort5 but -1.1 dev200 / -0.5 dev200b (new knot
certificate failures and calendar crossings: the step floor kinks the wing and shifts its term structure).
0 served violations in every cell.

## Parity (keys off, both binaries)

dev200 auto `c89d0739`, graded-v1 `fc277413` / `46ad6c26` / `171fa2d2`, cohort5 ladder-point `232d2c0d`, cohort5
ladder-strict `21f4ee58` (lever-1 binary): MATCH. Lever-2 binary: the same five + cohort5 `floor-price-only`
`7b450b5b` = the lever-1 arm: MATCH.

## Tests

`atx-vol-quote-slice-tests` 151 -> 156 (gtest): `QuoteSurfaceLadder.WingTailFloorPriceOnlyRefusesAVolatilityReimpliedAtTheFloor`
(fails with the post-pass call removed, 153/154), `WingTailIsANoOpWithoutAReimpliedFloorVolatility`,
`WingTailRequiresTheKnotFirstLadderAndAKnownMode`, `WingFloorVariantServesTheWingFlooredKnotSlice` (fails with the
surface wiring removed, 155/156), `SliceChannel.WingFloorKeepsABidlessWingAtTheOutermostLowerBoundedLevel` (fails,
with the variant test, when the floor is disabled in the knot core, 154/156). CLI pytest 66 -> 67 (key
strictness, echo, inertness on clean quotes). ctest quote-slice / quote-benchmark-native / -tools 3/3. PCH-off
`vol-hygiene` on 7 TUs covering `quote_slice.hpp`, `slice_channel.hpp`, `quote_bench_policy.hpp`: 0 diagnostics.

## Open

- Lever 2's floor is a step at the last lower-bounded quote: it needs a convexity-safe ramp and a calendar-
  consistent application across expiries before its coverage cost (dev200 -1.1 pts) goes away.
- dev200 `long_dated` stays +2.4 % (L1+2) / +12 % (L1) above graded-v1, whose term-prior refit borrows the
  neighbours' wing; a term-prior wing for the knot rung is the next candidate.
- `repair_convexity` flags rounding-level lowerings as moves; a materiality threshold there (certificate
  tolerance) would keep the curve's volatility on those nodes instead of PriceOnly (a different default-path
  change; not done).
