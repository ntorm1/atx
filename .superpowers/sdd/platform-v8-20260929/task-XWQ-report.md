# Task XWQ report: the 101 formulaic alphas in the DSL (expansion X, cell X-7; Rulings PM7-33, PM7-34)

Lane XWQ, pool 12, branch `feat/platform-v8-xwq-20261002` from `bc153439`. Python and documentation only; synthetic data
only. Written 2026-10-02 before any IC, TRAIN, return, turnover or NAV read of any formula (this lane has no trials).
This file is the registration of cell X-7.

## Status

| item | status | commit |
|---|---|---|
| New fields `open_adj`, `high_adj`, `low_adj` (builder, entry, synthetic tests) | DONE | `387ed0e9`, `1f8bf582` (text) |
| Table of the 101 formulas, readings, the selection rule coded (`xwq_check.py`) | DONE | `5b8e7b55` |
| Picks verified against the printed formulas; frozen add-alpha lines | DONE | `46225af7` |
| Registration (this report: table, rule, picks, rows, v9 list, rulings) | DONE | this commit |

Test lines:
- `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-engine/tools/test_research_fields_ohlc.py`
  -> 7 passed (with the xdata and price field tests: 23 passed).
- `"C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-v8-20260929/xwq_check.py` -> `xwq_check: PASS`
  (101 rows; 46 exact transcriptions through the XSIG compiler mirror and verified for their exactness class on a
  synthetic world; 6 picks equal to the printed formulas cell for cell; 25 mutants fail). `xsig_check.py` still PASS.

## 1. Result in one paragraph

Of the 101 printed formulas, **46 are exactly expressible** with the op catalog and fields in house plus three new
same-session bar fields built here (`open_adj`, `high_adj`, `low_adj`); **55 are not**: 43 need `vwap` (no source in house
carries a volume-weighted price, and no daily-bar proxy meets the paper's definition), 12 take a cross-sectional op of a
price level inside a time-series op, which the paper's adjustment rule makes a two-dimensional re-evaluation the DSL
cannot express. No operator is missing: ts_argmax, signedpower, product, scale, indneutralize and correlation of ranks
all exist. The rule of section 3, fixed before selection and blind, picks **6 strings**, one per mechanism cluster:
`wq_099`, `wq_035`, `wq_055`, `wq_006`, `wq_002`, `wq_101`. The cluster the roster already holds (1-10 session
close-to-close reversal, 18 of the 46 exact formulas) and the overnight-gap cluster (contested sign; house precedent)
are not taken; 40 exact formulas are listed for v9 (section 8). Six, not twenty: the paper's exact set is dominated by
reversal variants and by several versions of the same price-volume correlation idea, and the brief prefers fewer
strong picks.

## 2. Readings (how the paper's inputs and functions map to the house; fixed blind)

The paper (appendix A.2/A.3) defines its inputs and functions in one line each. Where a definition leaves a choice, the
reading below is fixed here, once, for all 101 rows.

- **R1 prices (the adjustment rule).** The paper adjusts prices for splits and dividends as of the day the alpha is
  computed (section 2: "yesterday's close is adjusted for any splits and dividends if the ex-date is today"). The house
  `close` is the vendor raw close times the vendor cumulReturnFactor, whose level (anchor) is arbitrary across lines
  (and re-anchored on 2021-01-04). The new bar fields are on the same basis. Three classes follow:
  - **S (scale-free, 22 exact rows):** the value does not change when a line's whole price history is multiplied by a
    positive constant (ratios, returns, correlations, time-series ranks, comparisons of two price terms). On the house
    basis it equals the paper's value on prices adjusted as of each day, whatever the anchor.
  - **L (level-dependent, rebased, 24 exact rows):** the value depends on the price level (dollar differences ranked
    across names, a dollar constant like the 0.001 of #101), but every level term is a homogeneous function of the
    price history evaluated at the current row. Multiplying that term by `(raw_close / close)` (= 1 / F(t)) converts the
    house basis to prices adjusted as of row t: exact.
  - **N (nested level, 12 rows, not exact):** a cross-sectional op of a price level sits inside a time-series op (for
    example `Ts_Rank(rank(low), 9)`). Under R1 each past cross-section must be re-evaluated on prices adjusted as of the
    current day; the DSL evaluates a past cross-section once. Not an operator or a field gap: marked not exact.
  `xwq_check.py` verifies the classes on a synthetic world with splits, dividends and a different anchor per line:
  every S and rebased L string is anchor-free; every L string without its rebase is not.
- **R2 volume** = the session's raw share volume (`volume`); the paper states no volume adjustment.
- **R3 returns** = `((close / delay(close, 1)) - 1)`, the adjusted close-to-close return (the house idiom).
- **R4 adv{d}** = `ts_mean((raw_close * volume), d)`: the house dollar ADV (atx-impl `kEquityDollarAdvDsl`), written
  inline (the name `adv{d}` is a derived-field name elsewhere in the engine). The paper compares or divides `adv20`
  (dollars) with `volume` (shares) in #7, #17, #21, #39, #43, #100; taken as printed.
- **R5 cap** = `me_company`. **R6 IndClass** sector / industry / subindustry = `grp_ff12` / `grp_ff49` / `grp_sic2`
  (the paper: any classification, levels of similar granularity; no pick uses one).
- **R7 rank** = cross-sectional average-tie percentile in [0, 1] (#1's `- 0.5` implies [0, 1]); **ts_rank** the same
  within the window (#35's `1 - Ts_Rank` implies [0, 1]); **stddev** ddof 1; **correlation** Pearson;
  **ts_argmax / ts_argmin** = 1-based position of the first extreme counted from the oldest day (the house; the paper's
  "which day" could also read as days ago, which reverses the order: flagged, see E2); **signedpower(x, a)** the house
  sign(x)|x|^a equals the paper's x^a in both uses (#1, #84: x >= 0).
- **R8 syntax.** Fractional windows floored (the paper's own rule); `x ^ y` = `power(x, y)`; `min(x, d)` / `max(x, d)`
  with a number d = `ts_min` / `ts_max`, with two series element-wise; `sum` = `ts_sum`; a boolean used as a number is
  `(c ? 1 : 0)`; `(x) * -1` is written `(-1 * x)`.
- **R9 timing.** The house decides at row t from session t's close and fills at session t+1: the paper's delay-1. The
  four delay-0 alphas (42, 48, 53, 54; paper footnote 11) would trade one session late (E4).

## 3. The selection rule (fixed before selection; reads no return, IC or Sharpe of any window)

House forms (PM7-34, fixed before selection, no per-alpha tuning): `rank(decay_linear(alpha, 5))` when the longest
printed window of the formula (after flooring; `adv{d}` counts d; `returns` counts 1) is under 10 sessions,
`rank(decay_linear(alpha, 21))` otherwise. Plain cross-sectional rank (formulas that industry-neutralize do it inside).

Eligibility (a formula failing any is not taken):
- **E1** exact (class S or L).
- **E2** its sign does not depend on a reading the paper leaves open: excludes #60, #100 (rank of ts_argmax / argmin).
- **E3** not degenerate as printed: #7 and #21 compare `adv20` (dollars) with `volume` (shares) or `volume / adv20`
  with 1, which holds only below about a $1 price, so one branch is taken almost everywhere; #45 correlates over 2
  sessions (always +-1).
- **E4** not a delay-0 alpha (#48, #53, #54): the paper trades them at the computing close; the house fills a session
  later, so the premise is lost.
- **E5** within the house budget (bars <= 314, slots <= 7, extra fields <= 5, <= 4,096 bytes) in its house form; an
  over-budget transcription may be replaced by the same formula with the operands of a commutative op or of a comparison
  swapped (`a < b` written `b > a`), proved identical by `canonical()` in `xwq_check.py` (the house's mechanical-rewrite
  precedent, XSIG section 5). Used once: #99 (8 slots as written, 7 swapped).
- **E6** the mechanism cluster is not one the roster or the literature closes: **ctc_rev** (1-10 session close-to-close
  reversal, alone or scaled by momentum, size, volume level or volatility; 18 exact rows) restates `ind_adj_rev_5` /
  `ind_adj_rev_5_nx` (PM7-34: "a 1-day reversal that the roster already holds is still a poor pick"); **gap** (#20,
  the overnight gap against the previous day's high, close and low) has a contested sign (Lou-Polk-Skouras 2019:
  overnight winners continue; the formula reverses them) and the house withdrew `night_day` on that ground
  (library-v8-draft R7-5).

Clusters (the input that sets the sign; assigned in the table before selection): `pv_vol` (time-series correlation of
a price or range series with share volume), `pv_liq` (correlation of a price series with dollar ADV), `vol_ret`
(correlation of volume change with the intraday return), `range_vol` (range position with volume), `bar` (same-day
open / high / low / close structure), `vol_rev` (reversal confirmed by abnormal volume, in own-history ranks), `ar1`
(own return autocorrelation), plus the two excluded above.

Pick: **one per cluster**, the first by a fixed tie-break: **T0** the paper states the formula's mechanism and delay
(section 2: #101 "a delay-1 momentum alpha"), **T1** class S before L (compares names on returns, not dollars, and needs
no rebase), **T2** longest printed window first (slower inputs pay less of the ~12 bp per traded dollar; PM7-34 net of
cost), **T3** fewer DAG nodes, **T4** lower paper number. Rank order of the picks (admission tie-break) = longest
printed window first. The paper's own statistics are aggregate only (Sharpe 1.24-4.16, holding 0.6-6.4 days, mean
pairwise correlation 15.9%, before costs; 80 of 101 in production, not named), so they rank nothing; they set the tier.

The rule is coded (`select()`); its output on the table is the registration (asserted). One clause was added after the
first mirror run: E5's budget order, prompted by #99's static slot count (8), not by any return; without it the
`pv_liq` pick would be #85 (window 30) instead of #99 (window 60).

## 4. The 101 formulas

Class: S / L exact (section 2), V needs vwap, N nested price level. Window = longest printed window; form = house
decay; bars / slots of the house string (mirror). The DSL of every exact row is `dsl(n)` in `xwq_check.py`.

| # | exact | class | missing | cluster | window / form / bars / slots | outcome |
|---|---|---|---|---|---|---|
| 1 | no | N | nested price level: close and stddev(returns) compared inside ts_argmax | - | - | not exact |
| 2 | yes | S | - | vol_ret | 6 / 5 / 11 / 5 | **PICK** (wq_002) |
| 3 | no | N | nested price level: rank(open) inside correlation | - | - | not exact |
| 4 | no | N | nested price level: rank(low) inside ts_rank | - | - | not exact |
| 5 | no | V | vwap | - | - | not exact |
| 6 | yes | S | - | pv_vol | 10 / 21 / 29 / 5 | **PICK** (wq_006) |
| 7 | yes | S | - | ctc_rev | 60 / 21 / 86 / 6 | E3 degenerate |
| 8 | yes | L | - | ctc_rev | 10 / 21 / 35 / 7 | E6 cluster ctc_rev |
| 9 | yes | L | - | ctc_rev | 5 / 5 / 9 / 7 | E6 cluster ctc_rev |
| 10 | yes | L | - | ctc_rev | 4 / 5 / 8 / 7 | E6 cluster ctc_rev |
| 11 | no | V | vwap | - | - | not exact |
| 12 | yes | L | - | ctc_rev | 1 / 5 / 5 / 6 | E6 cluster ctc_rev |
| 13 | no | N | nested price level: rank(close) inside covariance | - | - | not exact |
| 14 | yes | S | - | pv_vol | 10 / 21 / 29 / 5 | one pick per cluster (pv_vol: #6 ranks first) |
| 15 | no | N | nested price level: rank(high) inside correlation | - | - | not exact |
| 16 | no | N | nested price level: rank(high) inside covariance | - | - | not exact |
| 17 | yes | L | - | vol_rev | 20 / 21 / 43 / 5 | one pick per cluster (vol_rev: #35 ranks first) |
| 18 | yes | L | - | bar | 10 / 21 / 29 / 7 | one pick per cluster (bar: #101 ranks first) |
| 19 | yes | S | - | ctc_rev | 250 / 21 / 270 / 5 | E6 cluster ctc_rev |
| 20 | yes | L | - | gap | 1 / 5 / 5 / 7 | E6 cluster gap |
| 21 | yes | S | - | ctc_rev | 20 / 21 / 39 / 8 | E3 degenerate |
| 22 | yes | L | - | pv_vol | 20 / 21 / 39 / 6 | one pick per cluster (pv_vol: #6 ranks first) |
| 23 | yes | L | - | ctc_rev | 20 / 21 / 39 / 6 | E6 cluster ctc_rev |
| 24 | yes | L | - | ctc_rev | 100 / 21 / 219 / 7 | E6 cluster ctc_rev |
| 25 | no | V | vwap | - | - | not exact |
| 26 | yes | S | - | pv_vol | 5 / 5 / 14 / 5 | one pick per cluster (pv_vol: #6 ranks first) |
| 27 | no | V | vwap | - | - | not exact |
| 28 | yes | L | - | bar | 20 / 21 / 43 / 5 | one pick per cluster (bar: #101 ranks first) |
| 29 | no | N | nested price level: rank(delta(close, 5)) in dollars inside ts_min; also log of a minimum rank of 0 | - | - | not exact |
| 30 | yes | S | - | vol_rev | 20 / 21 / 39 / 6 | one pick per cluster (vol_rev: #35 ranks first) |
| 31 | no | N | nested price level: rank(delta(close, 10)) in dollars inside decay_linear | - | - | not exact |
| 32 | no | V | vwap | - | - | not exact |
| 33 | yes | S | - | bar | 0 / 5 / 4 / 5 | one pick per cluster (bar: #101 ranks first) |
| 34 | yes | L | - | ctc_rev | 5 / 5 / 9 / 7 | E6 cluster ctc_rev |
| 35 | yes | S | - | vol_rev | 32 / 21 / 52 / 7 | **PICK** (wq_035) |
| 36 | no | V | vwap | - | - | not exact |
| 37 | yes | L | - | bar | 200 / 21 / 220 / 5 | one pick per cluster (bar: #101 ranks first) |
| 38 | yes | S | - | bar | 10 / 21 / 29 / 4 | one pick per cluster (bar: #101 ranks first) |
| 39 | yes | L | - | ctc_rev | 250 / 21 / 270 / 8 | E5 over the house budget |
| 40 | yes | L | - | pv_vol | 10 / 21 / 29 / 7 | one pick per cluster (pv_vol: #6 ranks first) |
| 41 | no | V | vwap | - | - | not exact |
| 42 | no | V | vwap | - | - | not exact |
| 43 | yes | S | - | vol_rev | 20 / 21 / 58 / 5 | one pick per cluster (vol_rev: #35 ranks first) |
| 44 | yes | S | - | pv_vol | 5 / 5 / 8 / 5 | one pick per cluster (pv_vol: #6 ranks first) |
| 45 | yes | L | - | ctc_rev | 20 / 21 / 44 / 8 | E3 degenerate |
| 46 | yes | L | - | ctc_rev | 20 / 21 / 40 / 8 | E5 over the house budget |
| 47 | no | V | vwap | - | - | not exact |
| 48 | yes | S | - | ar1 | 250 / 21 / 271 / 6 | E4 delay-0 alpha |
| 49 | yes | L | - | ctc_rev | 20 / 21 / 40 / 7 | E6 cluster ctc_rev |
| 50 | no | V | vwap | - | - | not exact |
| 51 | yes | L | - | ctc_rev | 20 / 21 / 40 / 7 | E6 cluster ctc_rev |
| 52 | yes | L | - | ctc_rev | 240 / 21 / 260 / 6 | E6 cluster ctc_rev |
| 53 | yes | S | - | bar | 9 / 5 / 13 / 5 | E4 delay-0 alpha |
| 54 | yes | S | - | bar | 0 / 5 / 4 / 7 | E4 delay-0 alpha |
| 55 | yes | S | - | range_vol | 12 / 21 / 36 / 6 | **PICK** (wq_055) |
| 56 | yes | S | - | ctc_rev | 10 / 21 / 30 / 7 | E6 cluster ctc_rev |
| 57 | no | V | vwap | - | - | not exact |
| 58 | no | V | vwap | - | - | not exact |
| 59 | no | V | vwap | - | - | not exact |
| 60 | yes | S | - | range_vol | 10 / 21 / 29 / 9 | E2 sign depends on the ts_argmax / ts_argmin direction reading |
| 61 | no | V | vwap | - | - | not exact |
| 62 | no | V | vwap | - | - | not exact |
| 63 | no | V | vwap | - | - | not exact |
| 64 | no | V | vwap | - | - | not exact |
| 65 | no | V | vwap | - | - | not exact |
| 66 | no | V | vwap | - | - | not exact |
| 67 | no | V | vwap | - | - | not exact |
| 68 | no | N | nested price level: rank(high) inside correlation | - | - | not exact |
| 69 | no | V | vwap | - | - | not exact |
| 70 | no | V | vwap | - | - | not exact |
| 71 | no | V | vwap | - | - | not exact |
| 72 | no | V | vwap | - | - | not exact |
| 73 | no | V | vwap | - | - | not exact |
| 74 | no | V | vwap | - | - | not exact |
| 75 | no | V | vwap | - | - | not exact |
| 76 | no | V | vwap | - | - | not exact |
| 77 | no | V | vwap | - | - | not exact |
| 78 | no | V | vwap | - | - | not exact |
| 79 | no | V | vwap | - | - | not exact |
| 80 | no | N | nested price level: indneutralize of an open / high price level inside delta | - | - | not exact |
| 81 | no | V | vwap | - | - | not exact |
| 82 | yes | L | - | pv_vol | 17 / 21 / 53 / 8 | E5 over the house budget |
| 83 | no | V | vwap | - | - | not exact |
| 84 | no | V | vwap | - | - | not exact |
| 85 | yes | S | - | pv_liq | 30 / 21 / 57 / 6 | one pick per cluster (pv_liq: #99 ranks first) |
| 86 | no | V | vwap | - | - | not exact |
| 87 | no | V | vwap | - | - | not exact |
| 88 | no | N | nested price level: rank(open), rank(low), rank(high), rank(close) inside decay_linear | - | - | not exact |
| 89 | no | V | vwap | - | - | not exact |
| 90 | yes | L | - | ctc_rev | 40 / 21 / 65 / 6 | E6 cluster ctc_rev |
| 91 | no | V | vwap | - | - | not exact |
| 92 | no | N | nested price level: rank(low) inside correlation | - | - | not exact |
| 93 | no | V | vwap | - | - | not exact |
| 94 | no | V | vwap | - | - | not exact |
| 95 | yes | L | - | pv_liq | 40 / 21 / 98 / 7 | one pick per cluster (pv_liq: #99 ranks first) |
| 96 | no | V | vwap | - | - | not exact |
| 97 | no | V | vwap | - | - | not exact |
| 98 | no | V | vwap | - | - | not exact |
| 99 | yes | S | - | pv_liq | 60 / 21 / 104 / 7 | **PICK** (wq_099) |
| 100 | yes | S | - | range_vol | 30 / 21 / 49 / 10 | E2 sign depends on the ts_argmax / ts_argmin direction reading |
| 101 | yes | L | - | bar | 0 / 5 / 4 / 5 | **PICK** (wq_101) |

Counts: exact 46 (S 22, L 24); not exact 55 (vwap 43; nested price level 12). Missing operators: none. Missing fields:
`vwap` only (open, high, low built here; `adv{d}`, `returns`, `cap` expressible from fields in house).

## 5. The picks

All: tier **C+** (practitioner source with no per-alpha published statistic; sample 2010-2013; published holding period
0.6-6.4 days against a book that trades slowly; the grade XSIG gave its single-study candidates); prior sign **+1** (the
printed sign is the position; origin prior); one admission trial each. Citation: Kakushadze (2016), "101 Formulaic
Alphas", arXiv:1601.00991 (read from arXiv; also published in Wilmott magazine, not checked here).

| rank | id | paper | theme | form | bars | slots | nodes | extra fields | DSL sha256 (16) | nearest member | turnover [est] |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `wq_099` | #99 | price_volume (new) | 21 | 104 | 7 | 28 | high_adj, low_adj | `3ea99660fd87ccf3` | stmom | mid-high |
| 2 | `wq_035` | #35 | reversal_seasonality | 21 | 52 | 7 | 22 | high_adj, low_adj | `cf7571ddb7ae552f` | ind_adj_rev_5_nx | mid |
| 3 | `wq_055` | #55 | price_volume (new) | 21 | 36 | 6 | 19 | high_adj, low_adj | `e9b110e3cb7d01f4` | high_52w | mid |
| 4 | `wq_006` | #6 | price_volume (new) | 21 | 29 | 5 | 9 | open_adj | `9393d144e5ace0b0` | stmom | mid |
| 5 | `wq_002` | #2 | price_volume (new) | 5 | 11 | 5 | 17 | open_adj | `9fdd5972c3390803` | stmom | high |
| 6 | `wq_101` | #101 | price_momentum | 5 | 4 | 5 | 16 | open_adj, high_adj, low_adj | `88e1e37aa66d1832` | stmom | high |

Base fields (`close`, `raw_close`, `volume`) are not counted. Every row is within the house budget.

### 5.1 `wq_099` (Alpha#99): price-liquidity against price-volume co-movement
- Printed: `((rank(correlation(sum(((high + low) / 2), 19.8975), sum(adv60, 19.8975), 8.8136)) < rank(correlation(low,
  volume, 6.28259))) * -1)`.
- Frozen DSL: `rank(decay_linear((-1 * ((rank(correlation(low_adj, volume, 6)) > rank(correlation(ts_sum(((high_adj +
  low_adj) / 2), 19), ts_sum(ts_mean((raw_close * volume), 60), 19), 8))) ? 1 : 0)), 21))` (operands of `<` swapped
  for the 7-slot budget, E5; same semantics).
- Mechanism and sign: short (value -1) a name whose low moves with its share volume more than its smoothed mid price
  moves with its smoothed dollar liquidity, in cross-sectional rank. Practitioner (WorldQuant) evidence only. Nearest:
  `stmom`, the only member conditioning on trading activity (turnover); no member reads a price-volume correlation.
  Overlap by construction with `wq_006` through the price-volume term (corr(low, volume, 6) against corr(open, volume,
  10)): expected |rho| .2-.4 [est]. Binary input: higher turnover than its 21-session form suggests.

### 5.2 `wq_035` (Alpha#35): volume-confirmed reversal in own-history ranks
- Printed: `((Ts_Rank(volume, 32) * (1 - Ts_Rank(((close + high) - low), 16))) * (1 - Ts_Rank(returns, 32)))`.
- Frozen DSL: `rank(decay_linear(((ts_rank(volume, 32) * (1 - ts_rank(((close + high_adj) - low_adj), 16))) * (1 -
  ts_rank(((close / delay(close, 1)) - 1), 32))), 21))`.
- Mechanism and sign: long names trading unusually heavy volume while price and return sit low against their own
  recent history: price moves on high volume reverse when liquidity-driven (Campbell, Grossman and Wang 1993, QJE;
  Llorente, Michaely, Saar and Wang 2002, RFS); the volume leg agrees with the high-volume return premium (Gervais,
  Kaniel and Mingelgrin 2001, JF). Citations from memory, not re-read in this lane. Nearest: `ind_adj_rev_5_nx`
  (cross-sectional 5-session industry-adjusted reversal): not a restatement, the signal is normalized by each name's own
  32-session history and is zero without unusual volume; expected rho .2-.4 [est].

### 5.3 `wq_055` (Alpha#55): range position against volume
- Printed: `(-1 * correlation(rank(((close - ts_min(low, 12)) / (ts_max(high, 12) - ts_min(low, 12)))), rank(volume),
  6))`.
- Frozen DSL: `rank(decay_linear((-1 * correlation(rank(((close - ts_min(low_adj, 12)) / (ts_max(high_adj, 12) -
  ts_min(low_adj, 12)))), rank(volume), 6)), 21))`.
- Mechanism and sign: short names whose position in their 12-session high-low range (stochastic %K) rises and falls
  with volume (buying pressure that reverses). Practitioner evidence only. Nearest: `high_52w` (range position over 252
  sessions, a level signal): here the signal is the co-movement of position and volume, not the position; expected
  |rho| < .15 [est]; with `wq_006` .15-.3 [est].

### 5.4 `wq_006` (Alpha#6): price-volume correlation
- Printed: `(-1 * correlation(open, volume, 10))`. Frozen DSL: `rank(decay_linear((-1 * correlation(open_adj, volume,
  10)), 21))`.
- Mechanism and sign: short names whose open moves with share volume over two weeks (the same liquidity-reversal
  argument as 5.2, in correlation form). Practitioner evidence only; the paper's simplest price-volume member. Nearest:
  `stmom`; expected |rho| < .15 with every member [est].

### 5.5 `wq_002` (Alpha#2): volume change against the intraday return
- Printed: `(-1 * correlation(rank(delta(log(volume), 2)), rank(((close - open) / open)), 6))`.
- Frozen DSL: `rank(decay_linear((-1 * correlation(rank(delta(log(volume), 2)), rank(((close - open_adj) / open_adj)),
  6)), 5))`.
- Mechanism and sign: short names whose volume surges coincide with intraday gains (the dynamic volume-return relation
  of Llorente et al. 2002; liquidity-driven moves reverse). Fast (form 5). Nearest: `stmom`; expected |rho| < .1 [est].

### 5.6 `wq_101` (Alpha#101): the day's body relative to its range
- Printed: `((close - open) / ((high - low) + .001))`.
- Frozen DSL: `rank(decay_linear((((close - open_adj) * (raw_close / close)) / (((high_adj - low_adj) * (raw_close /
  close)) + 0.001)), 5))` (class L: the 0.001 is in dollars, so the body and the range are rebased to the session's own
  prices).
- Mechanism and sign: long names that closed well above their open within the day's range. The paper names it a
  delay-1 momentum alpha (section 2); the published sign of intraday-return continuation agrees (Lou, Polk and Skouras
  2019, JFE: past intraday winners earn +.64% a month close to close; Barardehi, Bogousslavsky and Muravyev: momentum in
  intraday returns; both as summarized in library-v8-draft R7-5). Nearest: `stmom` (short-term continuation; .05-.2
  [est]); against `ind_adj_rev_5_nx` -.1 to -.3 [est] (a strong body is part of the close-to-close return the roster
  reverses). Fast (form 5).

### 5.7 Pairwise overlap among the picks (by construction)
One pick per cluster; four picks share the new theme, so the theme weighting (1/T per theme, ew-theme-std-v1) caps
their joint share. Expected |rho| [est]: wq_006 / wq_099 .2-.4, wq_006 / wq_055 .15-.3, wq_035 / wq_002 .1-.2, every
other pair < .15.

## 6. Registry rows and add-alpha lines (strings frozen)

**L1 theme (new, Ruling XWQ-a):**
```json
"price_volume": "Formulaic price-volume structure (Kakushadze 2016, 101 Formulaic Alphas): short-window time-series correlation of a price, range position or dollar liquidity with trading volume, and of volume changes with the intraday return; prices that move with volume predict lower returns."
```

**L2 fields table** (root copies `clock` and `basis` checks against the build of record):
```json
"open_adj": {"formula_id": "ohlc-open-adj-v1", "origin": "fields_x7", "producer": "atx-engine/tools/research_fields_ohlc.py", "clock": "ohlc-same-session-v1: row t reads the vendor TickerHistory3 row of session t (open, high and low, delivered in the end-of-day row that also carries the role's close: known at session t's 22:00 UTC close mark, TH_CLOCK) and the role's close.f64, raw_close.f64 and present.u8 of row t; nothing dated after session t; rows on or after the seal are never read", "basis": "the session's vendor open x close.f64 / raw_close.f64 of the row (the role close's split-and-dividend basis); NaN unless the role has the line present with a finite positive close, the vendor key is unique, open, high and low are finite and positive, and low <= min(open, raw close) <= max(open, raw close) <= high (formula id ohlc-open-adj-v1; producer atx-engine/tools/research_fields_ohlc.py, lane XWQ)"},
"high_adj": {"formula_id": "ohlc-high-adj-v1", "origin": "fields_x7", "producer": "atx-engine/tools/research_fields_ohlc.py", "clock": "as open_adj", "basis": "the session's vendor high x close.f64 / raw_close.f64 of the row; same cell rule as open_adj (formula id ohlc-high-adj-v1)"},
"low_adj": {"formula_id": "ohlc-low-adj-v1", "origin": "fields_x7", "producer": "atx-engine/tools/research_fields_ohlc.py", "clock": "as open_adj", "basis": "the session's vendor low x close.f64 / raw_close.f64 of the row; same cell rule as open_adj (formula id ohlc-low-adj-v1)"}
```
(Root may write the full clock text in the two short rows; it is the same.)

**add-alpha lines** (printed by `xwq_check.py` with a SHA-256 prefix each; `PY="C:/Program Files/Python312/python.exe"`).
Run after L1 and L2, in this order. Substitutions allowed: `<X-7 parent>`, `<X-7 name>`, `<X-7 parent spec>`, `<X-7
fields dir>` (the build of fields v13 plus the three bar fields, section 7); every other byte is frozen.
```bash
"$PY" scripts/research_cycle.py add-alpha --id wq_099 --dsl "rank(decay_linear((-1 * ((rank(correlation(low_adj, volume, 6)) > rank(correlation(ts_sum(((high_adj + low_adj) / 2), 19), ts_sum(ts_mean((raw_close * volume), 60), 19), 8))) ? 1 : 0)), 21))" --theme price_volume --tier C+ --prior-sign 1 --citation "Kakushadze (2016, arXiv:1601.00991) 101 Formulaic Alphas, Alpha#99" --origin prior --prior-sign-source "Kakushadze 2016 (printed sign)" --form "R(decay_linear(x, 21))" --formula "-1 when the cross-sectional rank of the 8-session correlation between the 19-session sums of the mid price (high + low) / 2 and of 60-session dollar ADV is below the rank of the 6-session correlation of the low with share volume, else 0 (Alpha#99; written b > a for the 7-slot budget, same semantics)" --domain "NaN while a window is short or holds a NaN (104 bars) or a correlation window is flat; values -1 and 0" --deviation "adv60 = mean of raw close x share volume over 60 sessions (paper: average daily dollar volume); fractional windows floored as the paper states (19.8975 -> 19, 8.8136 -> 8, 6.28259 -> 6); prices on the role close's split-and-dividend basis (open_adj / high_adj / low_adj, research_fields_ohlc.py); the formula is scale-free, so it equals the paper's prices adjusted as of each day; house form decay_linear 21 then rank (paper: raw alpha, delay 1)" --parent <X-7 parent> --name <X-7 name> --parent-spec <X-7 parent spec> --fields <X-7 fields dir>
"$PY" scripts/research_cycle.py add-alpha --id wq_035 --dsl "rank(decay_linear(((ts_rank(volume, 32) * (1 - ts_rank(((close + high_adj) - low_adj), 16))) * (1 - ts_rank(((close / delay(close, 1)) - 1), 32))), 21))" --theme reversal_seasonality --tier C+ --prior-sign 1 --citation "Kakushadze (2016, arXiv:1601.00991) 101 Formulaic Alphas, Alpha#35" --origin prior --prior-sign-source "Kakushadze 2016 (printed sign)" --form "R(decay_linear(x, 21))" --formula "ts_rank(volume, 32) x (1 - ts_rank(close + high - low, 16)) x (1 - ts_rank(daily return, 32)) (Alpha#35): unusually high volume with a price and a return low against their own recent history" --domain "NaN until 32 sessions of every input; values in [0, 1]" --deviation "ts_rank = average-tie percentile of today in its window, in [0, 1] (the paper's 1 - Ts_Rank implies [0, 1]); returns = adjusted close-to-close; volume = raw shares; prices on the role close's split-and-dividend basis (open_adj / high_adj / low_adj, research_fields_ohlc.py); the formula is scale-free, so it equals the paper's prices adjusted as of each day; house form decay_linear 21 then rank" --parent <X-7 parent> --name <X-7 name> --parent-spec <X-7 parent spec> --fields <X-7 fields dir>
"$PY" scripts/research_cycle.py add-alpha --id wq_055 --dsl "rank(decay_linear((-1 * correlation(rank(((close - ts_min(low_adj, 12)) / (ts_max(high_adj, 12) - ts_min(low_adj, 12)))), rank(volume), 6)), 21))" --theme price_volume --tier C+ --prior-sign 1 --citation "Kakushadze (2016, arXiv:1601.00991) 101 Formulaic Alphas, Alpha#55" --origin prior --prior-sign-source "Kakushadze 2016 (printed sign)" --form "R(decay_linear(x, 21))" --formula "-correlation over 6 sessions of the cross-sectional rank of the 12-session stochastic %K, (close - min low) / (max high - min low), with the rank of share volume (Alpha#55)" --domain "NaN while a window is short, holds a NaN or is flat; a 12-session range of zero gives NaN" --deviation "prices on the role close's split-and-dividend basis (open_adj / high_adj / low_adj, research_fields_ohlc.py); the formula is scale-free, so it equals the paper's prices adjusted as of each day; house form decay_linear 21 then rank" --parent <X-7 parent> --name <X-7 name> --parent-spec <X-7 parent spec> --fields <X-7 fields dir>
"$PY" scripts/research_cycle.py add-alpha --id wq_006 --dsl "rank(decay_linear((-1 * correlation(open_adj, volume, 10)), 21))" --theme price_volume --tier C+ --prior-sign 1 --citation "Kakushadze (2016, arXiv:1601.00991) 101 Formulaic Alphas, Alpha#6" --origin prior --prior-sign-source "Kakushadze 2016 (printed sign)" --form "R(decay_linear(x, 21))" --formula "-correlation(open, volume, 10) (Alpha#6): open prices that move with share volume predict lower returns" --domain "NaN while the window is short, holds a NaN or is flat" --deviation "prices on the role close's split-and-dividend basis (open_adj / high_adj / low_adj, research_fields_ohlc.py); the formula is scale-free, so it equals the paper's prices adjusted as of each day; volume = raw shares (a split inside the window shifts its level); house form decay_linear 21 then rank" --parent <X-7 parent> --name <X-7 name> --parent-spec <X-7 parent spec> --fields <X-7 fields dir>
"$PY" scripts/research_cycle.py add-alpha --id wq_002 --dsl "rank(decay_linear((-1 * correlation(rank(delta(log(volume), 2)), rank(((close - open_adj) / open_adj)), 6)), 5))" --theme price_volume --tier C+ --prior-sign 1 --citation "Kakushadze (2016, arXiv:1601.00991) 101 Formulaic Alphas, Alpha#2" --origin prior --prior-sign-source "Kakushadze 2016 (printed sign)" --form "R(decay_linear(x, 5))" --formula "-correlation over 6 sessions of rank(2-session change of log volume) with rank((close - open) / open) (Alpha#2): volume surges that coincide with intraday gains predict lower returns" --domain "NaN while a window is short, holds a NaN or is flat; a zero volume gives a log of -inf" --deviation "same-session price ratio only (no adjustment enters); house form decay_linear 5 (longest printed window 6) then rank" --parent <X-7 parent> --name <X-7 name> --parent-spec <X-7 parent spec> --fields <X-7 fields dir>
"$PY" scripts/research_cycle.py add-alpha --id wq_101 --dsl "rank(decay_linear((((close - open_adj) * (raw_close / close)) / (((high_adj - low_adj) * (raw_close / close)) + 0.001)), 5))" --theme price_momentum --tier C+ --prior-sign 1 --citation "Kakushadze (2016, arXiv:1601.00991) 101 Formulaic Alphas, Alpha#101" --origin prior --prior-sign-source "Kakushadze 2016 (printed sign)" --form "R(decay_linear(x, 5))" --formula "(close - open) / ((high - low) + 0.001) on the session's own prices (Alpha#101, the paper's delay-1 momentum example): the day's body relative to its range" --domain "NaN without the session bar; values in about [-1, 1]" --deviation "level-dependent through the 0.001 dollar term: computed on the session's raw prices (close - open and high - low rebased by raw_close / close); house form decay_linear 5 then rank" --parent <X-7 parent> --name <X-7 name> --parent-spec <X-7 parent spec> --fields <X-7 fields dir>
```
Line SHA-256 prefixes (as printed): wq_099 `c67ccc21bfa0c64a`, wq_035 `cb5ff7a2634157f4`, wq_055 `b065878b74eb3995`,
wq_006 `0d3139043a9c738e`, wq_002 `c8a638920d17fc0f`, wq_101 `c08aed20f131674d`.

Expected K1 rows (`--plan-only` at add-alpha; bars / slots / extra fields; the exe's node count may exceed the mirror's
by one, as in X batch 1): wq_099 104 / 7 / high_adj, low_adj; wq_035 52 / 7 / high_adj, low_adj; wq_055 36 / 6 /
high_adj, low_adj; wq_006 29 / 5 / open_adj; wq_002 11 / 5 / open_adj; wq_101 4 / 5 / high_adj, low_adj, open_adj. A K1
row that differs is reported; a string K1 refuses is rewritten only mechanically (same semantics) or withdrawn at 0
trials.

**Withdrawal rule.** A pick whose fields are absent from the X-7 fields build is withdrawn at 0 trials. **Trial
accounting** (PM7-33): 6 admission trials (X hand-written 13 -> 19 of at most 33); roster 60 -> 66 (cap 80).

## 7. The new fields (`research_fields_ohlc.py`)

- `open_adj` / `high_adj` / `low_adj`: row t = the vendor open / high / low of session t times close.f64[t] /
  raw_close.f64[t] (the role's own adjustment of that row: the role's close is f64(f32 raw close) x the vendor
  cumulReturnFactor). So `x_adj / close` is the vendor bar's `x / raw close` of the same row, and every cross-session
  ratio carries exactly the adjustment of `close`. Cell rule: present, finite positive close and raw close, a unique
  vendor key, finite positive open / high / low, `low <= min(open, raw close)` and `high >= max(open, raw close)` (a
  violating bar is withheld whole and counted). `LAG_SESSIONS = 0`: the bar comes in the same vendor end-of-day row as
  the close, known at the session's 22:00 UTC mark before the 23:00 UTC decision (the builder's visibility mark;
  `TH_CLOCK`); this is the paper's delay-1 (Ruling XWQ-b).
- Seal: refuses unless the builder's `SEAL` is `research_window.SEAL`; vendor rows on or after it skipped and counted.
  Price source pinned by the role's `source_sha256`; `--reuse` interface (no input pin beyond the role).
- Draft entry `prepare_research_fields_ohlc.py` (`FIELDS_OHLC_DRAFT`); the plain builder does not register the module.
- Tests (`test_research_fields_ohlc.py`, 7): values bit for bit against an independent definition (withheld bars:
  order violation, null open, zero low, duplicated key); the basis (x_adj / close = x / raw close; across a 2:1 split the
  adjusted open continues the previous adjusted close while the raw open halves); the look-ahead probe (every vendor row
  after session t moved: rows <= t bit-identical, later rows move) and its teeth (the variant reading the next session's
  bar, `LAG_SESSIONS = -1`, moves row t); `--reuse` copies identical bytes; seal, missing column, wrong source and
  missing option refusals before any output; CLI equals API; the plain builder knows none of the fields.
- X-7 fields build (root): fields v13's recorded argv with `--fields <v13's 75 names>,open_adj,high_adj,low_adj`, `--reuse`
  the v13 directory, one process registering the three draft entries: `import sys; sys.path.insert(0,
  'atx-engine/tools'); import prepare_research_fields as b; import prepare_research_fields_draft as d; import
  prepare_research_fields_xdata as x; import prepare_research_fields_ohlc as o; d.register(vars(b)); x.register(vars(b));
  o.register(vars(b)); b.main(sys.argv[1:])`. Expected 75 reused / 3 computed; one pass over the price source holding
  three f32 matrices and a u16 count (14 bytes per role cell plus 32 MiB, admitted against `--max-rss-mib` before the
  read).

## 8. Unselected exact formulas (for v9)

40 exact rows are not screened in v8 (PM7-33); their strings are `dsl(n)` in `xwq_check.py`, already checked.
- Same cluster as a pick (16): pv_vol #14, #22, #26, #40, #44 (#82 over budget); pv_liq #85, #95; vol_rev #17, #30,
  #43; bar #18, #28, #33, #37, #38 (#33 and #38 take the opposite, reversal sign on the intraday body: a v9 sign test
  of the bar cluster would read them against #101).
- ctc_rev (15): #8, #9, #10, #12, #19, #23, #24, #34, #49, #51, #52, #56, #90, and #39, #46 (over budget; #7, #21,
  #45 of the cluster are counted below as degenerate). A v9 wave would register at most one, as a refinement of `ind_adj_rev_5_nx` (lane XIMP's
  domain).
- gap #20 (needs a ruling on the overnight sign). Delay-0: #48 (own-autocorrelation AR(1) forecast, a distinct
  mechanism worth a v9 look if the book ever fills at the computing close), #53, #54. Argmax direction: #60, #100.
  Degenerate as printed: #7, #21, #45.
- Not exact (55): 43 need vwap (a vendor VWAP or trade-level data: an XDATA ask); 12 need the as-of-day re-evaluation
  of past cross-sections (no fix in the DSL; under a causal reading, each day's cross-section on that day's raw prices,
  they become expressible with raw-price fields; a reading, not the paper's rule).

## 9. Rulings root needs (decision -- why -- cost if wrong)

- **XWQ-a (new theme `price_volume`):** wq_099, wq_055, wq_006 and wq_002 join a new theme (text in section 6); root adds
  it to the registry and the fitter's theme order (`V7_APPENDED_THEMES`, finding R6B-O-2) before the first add-alpha --
  no theme reads a price-volume correlation; one theme caps the four picks' joint share -- cost if wrong: one more theme
  in T (each theme's share 1 / T shrinks); the alternative `reversal_seasonality` would hold four more members.
- **XWQ-b (bar fields at lag 0):** the bars are read on the role close's own session -- the formulas mix close and the
  bar of one day; the vendor row carries both and the builder's visibility mark covers it; a lag-1 bar would mix two
  days -- cost if wrong: the vendor's real delivery (05:00 Chicago T+1) is later than the modeled mark for every
  same-date value, close included; if the house ever lags close, the bars follow.
- **XWQ-c (adjustment reading R1 and the classes):** S and rebased L strings are exact; the 12 nested formulas are not --
  the paper's own words (section 2) fix the adjustment as of the day -- cost if wrong: a causal reading would make the 12
  expressible (v9 list), none of which the rule would take for this wave (all are price-level ranks).
- **XWQ-d (`adv{d}` and `volume` as printed):** dollar ADV = raw close x shares (house definition); #7 and #21 degenerate
  as printed -- the paper defines adv as dollar volume and the formulas mix it with share volume -- cost if wrong: two
  ctc_rev formulas, excluded by E6 anyway.
- **XWQ-e (E5 budget order):** #99 registered with `a < b` written `b > a` (8 -> 7 slots) -- same semantics, proved by
  canonical parse trees; clause added after a static slot count, no return -- cost if wrong: the pv_liq pick would be #85.
- **XWQ-f (turnover):** wq_002 and wq_101 take the 5-session form (PM7-34 rule) and raise book turnover [est]; X-7 is
  judged on dSR > 0 and mechanics with turnover printed (PM7-34) -- cost if wrong: none to inference.

## 10. How root verifies

1. `"C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-v8-20260929/xwq_check.py` from the repository root
   prints the 46 exact rows, the rule's 6 picks with full SHA-256, the class check, the six semantic lines, the mutation
   probe, the six add-alpha lines and `xwq_check: PASS` (about 15 s; synthetic data only; reads registry.cpp,
   registry.json, libraries/v80.json and lib-v80.json through `xsig_check.py`).
2. `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-engine/tools/test_research_fields_ohlc.py`
   -> 7 passed.
3. Identity: no existing code path changed. `git diff --stat bc153439..HEAD` lists three new files under
   `atx-engine/tools/` (`research_fields_ohlc.py`, `prepare_research_fields_ohlc.py`, `test_research_fields_ohlc.py`) and
   two under the sprint directory; `prepare_research_fields.py` does not import the new module (tested), so every field
   build and every accepted output is byte-identical without the new entry.
4. K1 at add-alpha must print the rows of section 6.

## 11. Hygiene (what was read)

- Read: lane-rules.md; task-X-briefs.md "Rules for every X lane"; the PM7-33 ledger extract; task-XSIG-report.md and
  xsig_check.py; op_catalog.cpp, registry.cpp, oracle.cpp / oracle.hpp (kernel semantics); the v8.0 roster strings and
  themes (registry.json, libraries/v80.json, lib-v80.json fields v10 list); progress.md PM session 7 lines on fields v13
  and PM7-32; integration-log "X batch 1" (fields v13 names, K1 notes); task-XIMP / task-XDATA reports (member names and
  strings only); library-v8-draft R7-5 (night_day); research_fields_price.py, research_fields_xdata.py and their tests,
  prepare_research_fields.py (Role, RoleRows, FieldWriter, run, reuse), prepare_tickerhistory.py (vendor columns),
  prepare_recent_research.py (close recipe), strategy_nav_replay headers (fill timing), wq101_battery.hpp (not used).
- Web: only the paper, arXiv:1601.00991v3 PDF (downloaded with curl into the session scratchpad, text extracted
  locally). Nothing else fetched or searched.
- Opened under `C:/atx-wt/pool-2/build-equity/`: nothing. No data payload of any kind; nothing dated 2024-01-01 or later
  read; no return, IC, Sharpe, turnover or NAV of any window. Pool 10 not entered; `atx-db/` not touched.

## Cross-lane edits

None. Files changed: the three new tool files, `xwq_check.py`, this report.

## Deviations from the brief

- 6 picks, not up to 20: the coded rule's output (one per mechanism cluster; the roster's reversal cluster and the gap
  cluster closed). The cap is not a target.
- PM7-34 arrived during the lane, before selection: the rule uses its two house forms and its revised criterion (b).
- E5's budget-order clause was added after the first mirror run (static slot figure only); disclosed in section 3.
- The bar fields are lag 0, unlike the lag-1 convention of the other field modules (XWQ-b).

## Open risks

- The published horizon (0.6-6.4 days) is far shorter than the book's; four picks take the 21-session form, which may
  smooth away most of the signal; the two 5-session picks pay turnover.
- The vendor open / high / low have no vintage proof and no auction-print guarantee; order-violating bars are withheld.
- Four picks share one mechanism family (volume-conditioned price pressure); the overlap is bounded by construction
  and by the single theme, not measured.
- `wq_099` is a binary signal; its smoothed form flips more than its window suggests.
- The citations of section 5 for the volume-reversal mechanism (Campbell-Grossman-Wang, Llorente et al.,
  Gervais-Kaniel-Mingelgrin) are from memory and were not re-read in this lane.
