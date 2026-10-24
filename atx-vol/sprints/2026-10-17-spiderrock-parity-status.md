# SpiderRock parity — status summary, October 17, 2026 (resume 6 stop)

Where atx-vol stands against SpiderRock's marks (srVol / srPrc) on the development cohorts and
the five-symbol smile cohort. srVol/srPrc are the **yardstick only**: they are opened by the
official scorer after every prediction is written and are never an input to any fit, seed, or
policy. Nothing below is promoted to a default; every improvement is behind an opt-in key and the
holdout cohort has not been opened. All timings are unqualified (loaded shared host).

## 1. Code state

- Integration branch `feat/vol-resume4-20261016`, root tree `C:/atx-wt/pool-1`, head `0f643107`
  (see the resume-6 handoff for the lane table). Main `C:/atx` untouched at `e696e208`.
- Integrated so far (opt-in unless stated): Markov cleanup, Records serve mode + edge gate,
  smooth-dividend lattice schemes (escrowed-lr / BBS / BBSR), slice repair v3 + dominance gate,
  composite tool `--restrict`, chain batch pricing (Greeks + IV, scalar and AVX2 routes), ATXVFA
  fit archive. Not yet integrated: knot-first ladder + PriceOnly (fix round 1 complete at `2e60186c`,
  fresh review not started), AL kernel tier 1/2 (review INCOMPLETE at the stop: 0 major, 2 minor so far).
- Default-path parity (every key OFF) last proven on binary `4829965a` (root `37addcd0`) and
  re-proven per lane since: dev200 default `c89d0739`, graded-v1 `1348f63d`, R104 `6b129f64`,
  dividend default `abc75955`, dev200b default `4b273b08` — all MATCH. The two wave-3 merges
  (batch pricing, archive) touch no default-path file; the full set is owed once more at the
  final integration of this wave.

## 2. Cohort-level parity (development cohorts; official scorer)

Source `C:/atx-cache/quote-benchmark/composite-20261017/RESULTS.md` (sha256 bc01b889, reproduced
by an independent reviewer to 0 difference on 40 metrics). Price MAE in USD against srPrc-side
labels; MAE/spread = MAE ÷ max(ask − bid, 0.01); containment = served price inside [bid, ask];
"Vol MAE bp" is the scorer's row-level vol column (definition in RESULTS.md §4; it is dominated by
wide-spread wing rows and is not comparable to the per-slice numbers in §3).

dev200 (63,942 eligible rows, 10:30 ET):

| Arm | Coverage | MAE $ | MAE/spread | Containment | p99 $ | Vol MAE bp |
| --- | --- | --- | --- | --- | --- | --- |
| graded-v1 (per-expiry fitter, static policy) | 78.74% | 0.2343 | 0.109 | 98.82% | 1.972 | 1,531 |
| repair-v3 (channel repair + dominance gate, opt-in) | 84.89% | 0.2250 | 0.114 | 98.95% | 1.948 | 1,464 |
| C4 repair-v3 → m6 | 92.15% | 0.2278 | 0.145 | 98.21% | 2.013 | 1,539 |
| **C5 escrowed-lr (payers) → repair-v3 → m6** | **92.42%** | **0.2197** | **0.144** | 98.13% | **1.957** | 1,533 |
| knot-ladder point policy (lane run, UNREVIEWED) | 85.97% vol-served (86.98% incl. price-only) | 0.223 | 0.108 | 99.3% | 1.885 | — |

dev200b (62,369 eligible rows, disjoint symbols, 15:00 ET):

| Arm | Coverage | MAE $ | MAE/spread | Containment | p99 $ | Vol MAE bp |
| --- | --- | --- | --- | --- | --- | --- |
| graded-v1 | 87.15% | 0.2393 | 0.116 | 98.83% | 1.760 | 1,450 |
| repair-v3 | 90.49% | 0.2367 | 0.111 | 99.27% | 1.774 | 1,470 |
| C4 repair-v3 → m6 | 98.63% | 0.2346 | 0.113 | 98.89% | 1.800 | 1,564 |
| **C5 escrowed-lr (payers) → repair-v3 → m6** | **98.63%** | **0.2245** | **0.110** | **98.97%** | **1.782** | 1,558 |
| knot-ladder point policy (UNREVIEWED) | 89.30% vol-served | — | — | — | — | — |

Exact-intersection deltas (same rows): C1 → C4 −1.4% / −2.0%, C4 → C5 −3.5% / −4.3% (payer
rows), C1 → C5 −4.9% / −6.3%; ladder vs graded-v1 −2.75% / −2.12%, vs repair-v3 −2.00% (dev200).
The pattern generalises across the two cohorts; coverage does not transfer as a number because
dev200's ceiling is META/ASML (4,096-quote cap), V/TSM (parameter cap) and 716 adjusted
deliverables (refused by design): 4,849 dev200 rows (7.58%) and 855 dev200b rows (1.37%) stay
unserved after C5. The worst served population is the common model standing in for slices that
repair-v3 refused on a calendar crossing: 3,058 dev200 rows at 0.733 spreads, 85% containment.

Dividend payers (escrowed-lr lattice, opt-in): dev200 lattice chains certified/refused 117/63 →
149/31, payer MAE 0.197 → 0.147; dev200b 170/20 → 183/7, 0.209 → 0.186; ASML 11/13 chains
certify, 0.448 → 0.271. **Convention caveat:** the vol reported under an escrowed scheme is the
escrowed stock's vol, not the spot vol SpiderRock quotes, so payer srVol comparisons are not
like-for-like (dev200b payers 205 → 217 bp); the price comparison is. No machine-readable
`vol_convention` field exists yet.

## 3. Smile cohort — per-slice parity to srVol (five symbols, 2026-10-14 10:30 ET)

From `C:/atx-cache/quote-benchmark/plots-20261017-v3/RESULTS.md` (this stop; figures
`smile-<SYMBOL>-<expiry>.png` beside it; fits on root `022e9727` plus the unreviewed knot-ladder
lane binary, code-equal to `901757ec`). Cells = strikes inside the bid/ask IV band · median
|IV − srVol| in bp over the plotted OTM strikes (max in parentheses); violations
(monotone / convex / calendar) were 0/0/0 for every served curve. srVol itself sits inside the
band on 102/118, 114/118, 39/42, 50/51, 39/41 and 17/20 strikes respectively.

| Slice | graded-v1 (default) | slice-repair v3 (integrated, opt-in) | knot-ladder (UNREVIEWED) | C5 composite serves |
| --- | --- | --- | --- | --- |
| SPY 23 d | refused (convexity) | refused (calendar) | 113/118 · 1.8 bp (26) | m6 fallback |
| SPY 44 d | refused (convexity) | 106/118 · 0.9 bp (29) | 104/118 · 0.9 bp (30) | repair-v3 |
| AAPL Nov 20 | refused (calendar) | refused (calendar) | 41/42 · 4.6 bp (60) | m6 fallback |
| MSTR Nov 20 | refused (calendar) | 50/51 · 26 bp (509) | 49/51 · 26 bp (536), grade A | repair-v3 |
| CRWV Nov 20 | 24/41 · 178 bp (897) | 40/41 · 34.5 bp (382) | 40/41 · 34.7 bp (382) | repair-v3 |
| RIVN Nov 13 | 20/20 · 241 bp (1,682) | 20/20 · 241 bp | 20/20 · 97 bp (3,456) | repair-v3 |

Reading the table: where a curve is served, atx-vol sits inside the bid/ask band on nearly every
strike; the bp distance to srVol is small on liquid names (SPY 1-2 bp, AAPL 5 bp) and large on
wide-spread names (RIVN 97-241 bp with every strike inside the band — srVol itself sits outside
the band on 3 of 20 strikes there, so bp-to-srVol is not a quality measure on such names). The
remaining parity gaps on this cohort are coverage gaps, not shape gaps: AAPL Nov 20 and SPY 23 d
are served only by the knot-ladder lane's containment-first calendar ordering, which has not
passed review; until it does, C5 serves those rows from the m6 common model.

## 4. Known gaps vs SpiderRock, by cause

1. **Coverage** — 7.6% of dev200 rows unserved (META/ASML quote cap, V/TSM parameter cap,
   adjusted deliverables). The ladder loses 1,038 further graded-v1 rows to displacement because
   both calendar repairs clamp at a slice's own nodes (62 of 87 refusals needed ≤ 0.5
   half-spreads); the union-of-knots clamp is the next lane.
2. **Calendar-crossing fallback rows** — the common model (m6) at 0.733 spreads on 3,058 dev200
   rows; C6 (ladder → m6) not yet run.
3. **Vol convention** — escrowed-scheme vol is not spot vol; needs a `vol_convention` field
   before any srVol parity claim on payers.
4. **Performance** — record IV warm 404 ns quiet / 459-601 ns loaded (≤ 500 ns target met only
   quiet); AL Fast profile bulk p50 8.5-9.6 us (15 us target met on that rung, unqualified),
   Reference 15.3-22.5 us; chain batch Scalar t3 3.19-3.99x the loop, 3x single-thread not
   reached (needs the kernel-sharing interfaces); archive open 5-15 ms, record read ~500 ns.
   Nothing pinned on a quiet host; no "fastest" claim is justified.
5. **Kernel correctness** (verified this resume): `andersen_lake` is not monotone in vol on
   long-dated deep-ITM puts (two valid IV answers; put S=100 K=128.989 T=2), and
   `american_implied_vol` returns Ok 0.02-0.18 vol off on some such rows. Batch routes now
   refuse with a typed status; the scalar path does not yet.
6. **No held-out verdict** — every number above is a development-cohort number. A candidate
   default set (C5 + ladder) exists but must be run on the holdout under the oracle Ratchet rules
   before any promotion.

## 5. Artifacts

`C:/atx-cache/quote-benchmark/`: `composite-20261017/` (tables, strata, unserved detail),
`plots-20261017/`, `plots-20261017-v2/`, `plots-20261017-v3/` (this stop), lane folders
`<lane>-20261017-*`, review folders `<lane>-<sha>-review-v1` / `-rereview-v1`, parity
`integrate-1d-20261017-parity/`. Every results note carries SHA256s; nothing there is overwritten.
