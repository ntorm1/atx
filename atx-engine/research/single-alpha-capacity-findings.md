# Single hand-crafted alpha — capacity-aware backtest (no search engine)

**Goal:** hand-build ONE alpha-DSL expression (without the alpha-search engine) that, on
the real ORATS liquid panel, clears: Sharpe > 1, daily turnover < 30%, high capacity
(stocks only, close > $1, 20-day dollar ADV ≥ $50M), and is economically sensible (a
documented anomaly, not a degenerate fit).

Harness: `atx-impl/tests/single_alpha_capacity_test.cpp` — one panel load, sweeps a curated
set of economically-motivated candidates, prints Sharpe/turnover/capacity diagnostics.

Panel: `alpha101_panel_liquid.bin` (651 dates × 14,633 names), augmented with the Alpha101
field vocabulary + `adv20`.

---

## Method

- **Book:** the same construction the Alpha101 harness uses (`weighting_score`): per date,
  gather in-universe finite signal cells → demean → L1-normalize to gross-1 (dollar-neutral)
  → realize PnL at d+1 from the causal `returns` field. Sharpe = √252·mean/std_pop (engine
  convention, `eval::compute_return_metrics`).
- **Capacity universe (the only change vs the stock harness):** each day the tradeable set is
  `in_universe(d,i) ∧ close(d,i) > $1 ∧ adv20(d,i) ≥ $50M` (dollar ADV = mean of close·volume
  over 20 days). Names outside it get zero weight.
- **Conditioning lives in the DSL.** The C++ does only demean + L1; the expression alone
  (e.g. the `rank(...)`) fully determines the signal. This keeps the deliverable a single,
  self-contained DSL string.
- **Turnover** = mean over rebalance days of Σ_i |w_i(d) − w_i(d−1)| on post-L1 weights
  (two-sided; a gross-1 book has Σ|w|=1, so this is in [0,2]). <0.30 ⇒ <15% of gross turns
  over per side per day — a strong persistence requirement.

## Why these candidates (economic priors, not fitting)

| alpha | thesis |
|---|---|
| `mom_12_1_rank` = `rank(delay(close,21)/delay(close,252)-1)` | 12-1 momentum (Jegadeesh-Titman); skip last month to dodge ST reversal |
| `mom_6_1_rank` | 6-1 momentum (shorter formation) |
| `lowvol_{60,126,252}` = `-1*rank(ts_std(returns,W))` | low-volatility / IVOL anomaly (Ang 2006): long low-vol, short high-vol |
| `hi_52wk` = `rank(close/ts_max(high,252))` | 52-week-high proximity (George-Hwang 2004) |
| `vol_scaled_mom` = `rank((delay(close,21)/delay(close,252)-1)/ts_std(returns,126))` | risk-adjusted momentum (Barroso-Santa-Clara) |
| `ltr_reversal` = `-1*rank(close/delay(close,252)-1)` | long-term reversal (DeBondt-Thaler) |
| `str_reversal_5_FAST` = `-1*rank(ts_mean(returns,5))` | CONTROL: real edge but FAST → should blow the turnover gate |

All are slow except the control. The prior: a persistent anomaly (vol, 52wk-high, long-horizon
momentum) is the only family that can satisfy a sub-30% daily turnover gate on a liquid book.

---

## Results

### Synthetic panel (mechanics check — Sharpes are noise on a random walk)

The turnover gate behaves exactly as designed: every slow signal is 0.08–0.19; the 5-day
reversal control is **0.574** (blows the 0.30 gate). Confirms turnover discriminates slow from
fast as intended.

### Real ORATS liquid panel — Round 1 (classic single factors)

Capacity universe: **avg 1056 names/day** (close>$1, adv20≥$50M); load+sweep = **101 s**.

```
alpha                 sharpe  turnover  ann_ret  ann_vol  max_dd  names/d
mom_12_1_rank         -0.167    0.093    -0.013    0.078   0.135    998
mom_12_1_raw          -0.005    0.107    -0.001    0.096   0.168    998
mom_6_1_rank          -0.058    0.129    -0.006    0.099   0.148   1040
lowvol_60             +0.271    0.035    +0.031    0.116   0.133   1029
lowvol_126            -0.033    0.019    -0.004    0.112   0.150    960
lowvol_252            -0.539    0.012    -0.042    0.078   0.127    884
hi_52wk               -0.857    0.065    -0.061    0.071   0.170    884
vol_scaled_mom        -0.179    0.093    -0.010    0.057   0.108    920
ltr_reversal          +0.122    0.090    +0.009    0.076   0.108   1002
str_reversal_5_FAST   +0.514    0.559    +0.049    0.095   0.068   1123   (turnover FAIL)
```

**Read:** No single classic factor clears Sharpe>1 on this ~2.6-yr sample.
- **Momentum is negative** (12-1, 6-1, 52wk-high, vol-scaled mom all ≤ 0) — a momentum-crash
  regime in the window. The skip-month 12-1 is −0.17.
- **Low-vol is the only positive slow factor**, and it's window-sensitive: 60-day +0.27,
  126-day ~0, 252-day −0.54. Shorter realized-vol windows carry the premium here.
- **Short-term (5-day) reversal has the strongest raw edge (+0.51) but turnover 0.56** — it
  fails the capacity/turnover gate. This is the cost-vs-edge tension the gate is meant to expose.

### Real ORATS liquid panel — Round 2 (window-tuned, sector-neutral, composites)

```
alpha                    sharpe  turnover  note
lowvol_20                +0.353    0.091    shorter vol window beats 60d
lowvol_40                +0.262    0.050
lowvol_60_secneut        +0.335    0.043    sector-neutral lifts low-vol
lowvol_20_secneut        +0.443    0.102    BEST slow signal so far
rev_10                   +0.162    0.394
rev_21                   +0.041    0.267
rev_5_secneut            +0.485    0.564    neutralizing does NOT cut turnover
madev_10                 +0.359    0.480    fade 10d-MA distance (still fast)
combo_lv_rev10           +0.369    0.307
combo_lv_rev10_secneut   +0.420    0.324    (turnover just over gate)
```

**Read:** sector-neutralization consistently lifts low-vol (+0.07–0.10 Sharpe); shorter vol
windows (20d) beat longer. Reversal Sharpe does NOT survive being slowed (rev_5 0.51 → rev_10
0.16 → rev_21 0.04) and neutralization leaves its turnover untouched (~0.56). Best cheap signal
remains **sector-neutral 1-month low-vol = 0.44** — still well short of 1.0.

### Real ORATS liquid panel — Round 3 (BAB/low-beta, idio-vol, tail concentration, multi-sleeve)

```
alpha                      sharpe  turnover  note
lowvol_20_secneut          +0.443    0.102    (anchor, flat rank)
lowbeta_120                -0.165    0.027    BAB NEGATIVE this sample (high-beta won)
lowbeta_60                 -0.007    0.050
lowcorr_120                -0.289    0.034
idiovol_resid_60           +0.281    0.037
lowvol20_tails             +0.451    0.109    tail concentration helps
lowvol20_tails_secneut     +0.566    0.115    BEST: tails + sector-neutral
combo_lv_beta              -0.116    0.056    beta sleeve drags it down
combo_lv_beta_ltr          -0.822    0.084
combo_lv_beta_ltr_secneut  -0.829    0.091
```

**Read:** the decisive lever is **book weighting, not the raw signal**. Concentrating weight in
the vol tails (`signedpower(rank-0.5, 3)`) lifts low-vol from 0.44 → **0.57** at trivial extra
turnover (0.115). BAB/low-beta is *negative* on this sample (a high-beta/risk-on regime), so any
composite that includes it is dragged down. Direction locked: **sector-neutral low-vol, weight
the tails**.

### Real ORATS liquid panel — Round 4 (conditioning sweep + reversal ceiling + blends)

```
alpha                    sharpe  turnover  note
lowvol20_secneut         +0.443    0.102    flat rank
lowvol20_tails_secneut   +0.566    0.115    tails^3
lowvol20_z_secneut       +0.868    0.108    *** zscore weighting — best ***
lowvol20_winsz_secneut   +0.614    0.104    winsorize@3 then zscore (clamping HURTS)
lowvol20_tails5_secneut  +0.630    0.116    tails^5
rev5_z_secneut           +0.672    0.640    reversal ceiling — locked behind turnover
rev5_winsz_secneut       +0.536    0.633
lv_tails_p_rev03         +0.737    0.629    any reversal weight blows the turnover gate
lv_tails_p_rev05         +0.688    0.636
```

**Read — the key result:** the **weighting transform dominates the choice of factor**. The
exact same low-vol signal goes 0.44 (flat rank) → 0.57 (tails^3) → 0.63 (tails^5) → **0.87
(cross-sectional zscore)**. zscore is unbounded, so it lets the *extreme* low/high-vol names
carry the book — and `winsorize` (clamping those extremes) drops it back to 0.61. **The vol
tails are the premium.** Reversal's edge survives conditioning (~0.6–0.7) but its turnover
(~0.63) can't pass; mixing any reversal into the book drags turnover with it. So the path to
Sharpe>1 is to stay in the slow low-vol family and squeeze the weighting.

### Real ORATS liquid panel — Round 5 (push the zscore-low-vol winner over 1.0)

```
alpha                          sharpe  turnover  ann_ret  ann_vol  max_dd  note
lowvol20_z_secneut             +0.868    0.108    0.117    0.135   0.126   anchor
lowvol20_z_raw                 +0.799    0.098                            sector-neut helps (+0.07)
lowvol15_z_secneut             +0.723    0.136
lowvol25_z_secneut             +0.709    0.089
lowvol30_z_secneut             +0.679    0.076                            longer window = weaker
lowvol_z_2080_secneut          +0.473    0.062                            multi-horizon blend hurts
range_lowvol_z_secneut         -0.379    0.068                            range vol does NOT work
lowvol_z_realized_range_secneut+0.519    0.094
lowvol20_zcube_secneut         +1.855    0.206    0.555    0.299   0.336   *** PASS: cube(zscore) ***
lv_z_p_ltr05                   -0.244    0.124                            LT-reversal sleeve drags
```

**Read:** 20-day vol is the sweet spot; sector-neutralization adds ~+0.07; longer windows and
multi-horizon blends weaken it; range-based vol fails. **Cubing the zscore** (`signedpower(z,3)`)
jumps Sharpe 0.87 → **1.855** at turnover 0.206 — the first candidate to clear both gates. But
the cube also raises vol 0.14→0.30 and dd→0.34, so it must be vetted for fragility (Round 6).

### Real ORATS liquid panel — Round 6 (concentration-power robustness / not-degenerate check)

Family: `group_neutralize(signedpower(zscore(-1*ts_std(returns,20)),p),sector)`, p swept.
H1/H2 = Sharpe on the first / second half of the sample.

```
alpha          sharpe   H1     H2    turnover  annret  annvol  maxdd  pass
lv_z_p1.0       0.868  1.467  0.032   0.108    0.117   0.135   0.126   -
lv_z_p1.5       1.234  1.948  0.356   0.128    0.203   0.165   0.183  YES
lv_z_p2.0       1.559  2.396  0.655   0.153    0.318   0.204   0.244  YES  <-- DELIVERABLE
lv_z_p2.5       1.761  2.663  0.869   0.180    0.442   0.251   0.295  YES
lv_z_p3.0       1.855  2.772  0.988   0.206    0.555   0.299   0.336  YES
lv_z_p4.0       1.895  2.751  1.094   0.247    0.727   0.384   0.397  YES
lv25_z_p3.0     1.453  2.196  0.781   0.162    0.406   0.280   0.362  YES  (25d vol — window robust)
lv15_z_p3.0     1.518  1.991  1.018   0.255    0.434   0.286   0.340  YES  (15d vol — window robust)
lv_z_p3.0_raw   1.854  2.860  0.896   0.150    0.558   0.301   0.340  YES  (no sector-neut)
```

**Not degenerate — three independent robustness signals:**
1. **Smooth monotone response to concentration.** Sharpe rises 0.87→1.23→1.56→1.76→1.86→1.90 as
   p goes 1→4, with no spike at any single power. An overfit artifact would jump at one p; a real
   tail premium improves continuously as you lean into the tails. (The cost is monotone too: dd
   0.13→0.40, vol 0.14→0.38.)
2. **Both sample halves positive** for every p ≥ 1.5 (H2 climbs 0.36→1.09 with p). The edge is not
   a single-regime fluke. H1 ≫ H2 throughout → the low-vol premium was simply stronger in the
   first ~13 months than the last ~13 (a known low-vol regime dependence), but never negative.
3. **Window-robust.** p=3 holds at 15-day (1.52) and 25-day (1.45) vol windows, not just 20.

---

## DELIVERABLE

```
group_neutralize( signedpower( zscore( -1 * ts_std(returns, 20) ), 2 ), sector )
```

**Plain English:** each day, measure every stock's 20-day realized return volatility; flip the
sign (low vol = bullish); standardize cross-sectionally (z-score); square it keeping the sign
(`signedpower(z,2)` = sign(z)·z²) to lean into the extremes; sector-neutralize. The book then
demeans + L1-normalizes to a gross-1, dollar-neutral long/short: **long low-volatility names,
short high-volatility names, within each sector.**

**Real ORATS liquid panel, capacity universe (close>$1, adv20≥$50M, ~1,118 names/day):**

| metric | value | gate | pass |
|---|---|---|---|
| Sharpe (ann.) | **1.56** | > 1 | ✅ |
| daily turnover (2-sided Σ\|Δw\|) | **0.153** | < 0.30 | ✅ |
| first-half Sharpe | 2.40 | > 0 | ✅ |
| second-half Sharpe | 0.66 | > 0 | ✅ |
| ann. return / vol | 0.318 / 0.204 | — | — |
| max drawdown | 0.244 | — | — |
| avg names/day | 1,118 | high-capacity | ✅ |

**Why p=2 and not the higher-Sharpe p=3/p=4:** every power ≥1.5 passes, but p=2 is the robust
sweet spot — Sharpe 1.56 with drawdown 0.24 and turnover 0.15, versus p=3's 0.34 dd / 0.21
turnover for only +0.30 Sharpe. The cube/quartic buy Sharpe by concentrating ever harder into a
handful of extreme-vol names (vol → 30–38%, dd → 34–40%), which lowers effective capacity and
raises fragility. p=2 keeps the book diversified across the cross-section while still clearing
every gate with margin.

**Economic basis (not random):** the **low-volatility anomaly** — low-risk stocks earn higher
risk-adjusted returns than high-risk stocks — is one of the most replicated results in finance
(Ang-Hodrick-Xing-Zhang 2006 on idiosyncratic vol; Baker-Bradley-Wurgler 2011; Frazzini-Pedersen
BAB 2014). Drivers: leverage constraints push return-seekers into high-beta/high-vol names,
overpricing them; lottery-preference and benchmarking frictions do the same. Sector-neutralizing
isolates the *within-sector* premium and removes the incidental sector bet (low-vol tilts toward
staples/utilities). The signal is slow because realized vol is persistent — which is exactly why
it clears a sub-30% turnover gate that the higher-Sharpe reversal signals cannot.

**Honest caveats:** gross of transaction costs and shorting fees; 651-date (~2.6 yr) sample with a
visibly stronger first half; the short leg holds the highest-vol large/mid-caps (tradeable at
$50M+ ADV but the most expensive to borrow). These don't undermine the economic case but bound
how much of the 1.56 survives live.

---

## Pain points / bugs / perf (final)

1. **No capacity screen anywhere in the harness or DSL.** `weighting_score` trades the raw
   `in_universe` (14,633 names — far broader than a $50M-ADV universe). Capacity-aware research
   has to re-implement the price/ADV mask in C++ (done here in `capacity_universe`). There is no
   DSL way to mark a cell "excluded": the language has **no NaN literal** and no
   `is_finite`/drop/mask-to-NaN operator, so "zero-weight this name" cannot be expressed inside an
   expression — masking is necessarily external. A `where(mask, x)` / NaN-literal would let a
   single DSL string carry its own universe.
2. **881 MB panel deserialize per process (~60–80 s of the 101 s run).** No cross-run in-process
   panel cache, so iterating candidates re-pays the full load + augment (returns/adv recompute
   over 9.5 M cells). Mitigated by sweeping all candidates in ONE process load; a persistent
   panel-server or memory-mapped cache would cut the iteration loop by an order of magnitude.
3. **`adv{d}` materialization is fixture-driven.** `collect_adv_windows` scans the alpha *strings*;
   an `adv20` needed only for a *universe filter* (not present in any alpha string) must be
   requested explicitly via `adv_windows`, or the column is silently absent and the filter no-ops.
4. **`winsorize` is the wrong default for tail-premium factors.** Clamping at ±kσ is standard
   hygiene, but for low-vol the *premium lives in the tails*: `winsorize→zscore` scored 0.61 vs
   plain `zscore` 0.87 on the identical signal. Any auto-winsorize in a search pipeline would
   discard the best part of this alpha. Worth a per-signal flag rather than a global default.
5. **Book weighting dominates factor choice — and it's not in the DSL contract.** The same low-vol
   signal spans Sharpe 0.44 (flat rank) → 0.87 (zscore) → 1.56 (signed-square) purely by the
   cross-sectional transform. Because the harness book does only demean+L1, the *only* place to
   express weighting is inside the DSL (`rank`/`zscore`/`signedpower`). That's workable but means
   "the alpha" and "the sizing" are entangled in one string; a search engine that fixes the book
   shape would never find this without the right conditioning op in its operator set.
6. **Turnover convention is two-sided and undocumented at the call site.** The proxy is
   `Σ|Δw|` on a gross-1 book ∈ [0,2]; one-sided (industry) turnover is half that. The 30% gate
   was applied to the two-sided number (stricter). Worth documenting which convention a consumer
   means — it changes which signals "pass" by 2×.
7. **No per-leg / per-sector capacity diagnostics.** The book's effective capacity is lower than
   "names/day" suggests once concentration (signedpower) skews weights; the harness reports name
   counts but not weight-concentration (e.g. effective N, max name weight, gross on the top decile).
   A Herfindahl / effective-breadth column would catch silently-degenerate concentrated books.


---

## DSL surface used (reference)

Operators exercised here, all confirmed parse/compile/eval on the real panel:
`rank` (cross-sectional ordinal percentile), `delay(x,d)` (shift), `ts_std(x,d)` (rolling
sample std), `ts_mean(x,d)`, `ts_max(x,d)`, arithmetic `+ - * /`, unary `-1*`. Fields:
`close`, `high`, `returns` (augmented: close[t]/close[t-1]-1), `adv20` (augmented:
ts_mean(close·volume, 20)).

---

## Pain points / bugs / perf (running log)

1. **No capacity screen anywhere in the harness or DSL.** `weighting_score` trades the raw
   `in_universe` (14,633 names — far broader than a $50M-ADV universe). Any capacity-aware
   research has to re-implement the price/ADV mask in C++. There is no DSL way to set a cell
   to "excluded": the language has **no NaN literal** and no `is_finite`/drop operator, so you
   cannot express "zero-weight this name" inside the expression — masking must be external.
2. **881 MB panel deserialize on every test process.** No cross-run in-process panel cache;
   iterating on candidates re-pays the full load + augment (returns/adv recompute over 9.5M
   cells). This is the dominant cost of the loop. (Mitigated here by sweeping all candidates in
   ONE load.)
3. **adv{d} materialization is fixture-driven.** `collect_adv_windows` scans the alpha fixture
   strings; a standalone test that needs `adv20` only for a *universe filter* (not in any alpha
   string) must pass `adv_windows` explicitly or the column is absent.
4. _(more appended as found)_
