# Second development cohort dev200b and generalisation re-score

Tooling `6703f85d` (`feat/vol-cohort-builder-20261016`, base `f9727373`), merged
into the root at `ac01702c`. `quote_benchmark.py prepare` gains `--draw-seed`,
`--exclude-cohort`, `--liquidity-strata` and `--match-strata-cohort`; the
defaults reproduce dev200's `quotes.csv`, `quotes.parquet` and `labels.parquet`
byte for byte; 94 standalone pytest cases pass (6 new). The builder change was
needed because the draw order was tied to the dev/validation split seed.

## Cohort

`C:/atx-cache/quote-benchmark/20261014-dev200b` (manifest SHA256 `08022b0f...`):
15:00 ET snapshot (dev200 is 10:30), draw seed `atx-vol-dev200b`, all 200 dev200
underliers excluded, 20 liquidity strata matched to dev200's realised counts,
63,812 rows, 62,369 eligible, zero symbol overlap. Tenor, moneyness and
liquid-core shares closely match dev200. Development-only; no holdout touched.

## Results (own population; coverage of eligible rows)

| Model | Coverage dev200 -> dev200b | MAE | Bias | p99 | Containment | MAE / spread |
| --- | --- | --- | --- | --- | --- | --- |
| quote-slice | 16.7% -> 31.2% | 0.1510 -> 0.2572 | -0.029 -> -0.055 | 1.44 -> 1.83 | 98.3% -> 98.3% | 0.077 -> 0.088 |
| R104 | 70.5% -> 93.6% | 0.3519 -> 0.4620 | -0.186 -> -0.232 | 2.19 -> 3.47 | 93.0% -> 90.9% | 0.248 -> 0.225 |
| X1 | 70.5% -> 93.6% | 0.3428 -> 0.4543 | -0.179 -> -0.227 | 2.14 -> 3.46 | 93.3% -> 91.3% | 0.243 -> 0.221 |
| hybrid quote-slice -> X1 | 75.7% -> 97.8% | 0.2919 -> 0.3267 | -0.176 -> -0.179 | 2.08 -> 2.35 | 95.1% -> 95.1% | 0.211 -> 0.169 |

Exact intersections: R104 -> X1 is -1.7% on 57,659 rows (dev200 -2.6%; X1
better on 43.9% of rows, worse on 39.1%); R104 -> hybrid -30.3% (dev200 -15.2%);
quote-slice versus X1 where both serve 0.231 versus 0.684, x2.96 on 16,808 rows
(dev200 x3.13). The dev200 ranking holds on every exact intersection.

What does not transfer: dollar MAE (served rows are dearer, average $35 versus
$13 per option, because the common models now serve CAT, DE, ULTA, BRK.B instead
of refusing META/ASML/TSM/V; measured against the spread the common models and
the hybrid improve); coverage (dev200's 70% was set by the four refused large
chains; on similar-sized chains 97.5% versus 98.6%); the 7-30 day band, where
the common models worsen even in spread units (0.206 -> 0.276, undiagnosed).
Dividend payers are the worst stratum for every model in both cohorts and X1
barely helps them. Spread-normalised error is the metric to carry forward.

## Schema inventory (parquet)

- No ex-date or per-event dividend column, but the schedule is recoverable:
  `ddivPv = sum D_i * exp(-rate_T * days_i / 365)` holds; the first ex-date is an
  integer day for 98.5% of 3,177 payers, the second for 91%. `ddiv` reaches
  `quotes.csv` but the native binary never reads it.
- `sdiv` is a vendor per-strike carry (same for each call/put pair, varies across
  strikes in 28% of chains), not a declared yield.
- `yearsC == years` for American equity options; only European index products
  carry calendar ACT/365 time. `rate` is a per-expiry zero curve shared across
  underliers apart from about 10k off-curve rows.
- Unused raw fields: NBBO sizes and depth (`bid_size`/`ask_size` reach the CSV
  but are never read), exchange masks, second price level, `prtVolume`, `uPrc`
  (positive where underlier quotes are -99), `sDaysE`.
- Absent: deliverable/multiplier (adjusted series show only as a root digit
  suffix or leading `_`), AM/PM settlement. Missing values are -99, never null.

## Unfinished

No bootstrap intervals (the thin X1-over-R104 margin needs them); the 7-30 day
regression is unattributed; the quote-slice arbitrage-refusal drop (8,824 ->
1,104) is not split between time of day and names; the dividend recovery is
checked numerically only and used by no model; the registered ctest python lane
was not run (pytest `--noconftest` only).

Artifacts: `C:/atx-cache/quote-benchmark/cohort-20261016/RESULTS.md`,
`parquet-schema-inventory.md`, `artifact-sha256.json`.
