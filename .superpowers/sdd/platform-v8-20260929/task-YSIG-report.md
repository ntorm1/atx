# Task YSIG report: the Y screen set (round 2; new signals orthogonal by construction)

Lane YSIG, pool 12, branch `feat/platform-v8-ysig-20261002` from `798d3b23`. Python and documentation only; synthetic
data only; nothing built, nothing run on data. Declared 2026-10-02, before any IC, TRAIN, return, turnover or NAV read of
any candidate (this lane has no trials). This file is the registration of the Y screen set, including the forecast
horizon per candidate and per theme that Ruling PM8-5 adds (section 7).

## Status

| item | status | commit |
|---|---|---|
| Checker `ysig_check.py` (readers and unread fields, compiler mirror, numpy oracle, mutants, causality probe) | DONE | `e14161e7` |
| Registration (this report: candidates, horizons, registry rows, add-alpha lines, op asks, rulings) | DONE | this commit |

Test line: `"C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-v8-20260929/ysig_check.py` -> `ysig_check:
PASS` (86 reader strings, each SHA-checked against its registration; 9 candidates within the house budget; 6 new strings
equal to an independent numpy implementation of the registered definition on every cell and NaN pattern of seeded
synthetic panels; 22 planted errors fail; every string causal, and the probe fails on a reference that reads t+1; the 3
carried strings byte-equal to their LIB2 add-alpha lines). `xsig_check.py` still PASS.

## 1. Result in one paragraph

The Y screen set has **9 candidates**, not 16: 6 new and 3 carried byte for byte from LIB2 (R-12's screen set, which
lapsed at 0 trials when R-12 was not run). They span the horizon classes: 1 fast, 3 medium, 5 slow. The highest priors
are a one-month industry-peer momentum (Moskowitz-Grinblatt), the So-Wang pre-announcement reversal (fast; it reads the
unread `ea_window_pre5`), vol-of-vol (the second member of the smallest theme, options_implied) and the Akbas et al.
tug-of-war frequency (it reads the unread `ret_overnight` / `ret_intraday`). No new field was needed: the one family
that would have needed one (an abnormal FINRA short-flow field) is contradicted on exactly that data by Wang, Yan and
Zheng (2020, JFE; section 8). Short interest gets no candidate and options one; the in-house ATM-only IV and the
126-session FINRA ratio are already used for their canonical forms. One op ask (`group_sum`) would let the canonical
value-weighted peer return fit the slot budget.

## 2. Fields of v14 that no roster or X member reads (search class 1)

Readers checked: the 52 v8.0 members, the 22 X-2 / X-3 / X-4 strings of `v8x-prereg.md` section 14 (SHA-256 equal to
the pinned ones) and the 12 X-7 strings (SHA-256 prefixes of the XWQ report). Fields v14 = v13's 75
(`lib-v8x4b.json`, manifest `e5f7f28c...`) + `open_adj`, `high_adj`, `low_adj` = 78; 53 are read; **25 have zero
readers**:

| field | disposition |
|---|---|
| `ea_window_pre5` | **read by Y-2 `so_wang_rev`** |
| `ret_overnight`, `ret_intraday` | **read by Y-4 `day_rev_freq`** (carried) |
| `noa_lag4` | **read by Y-7 `dato`** |
| `iv_atm_63d`, `iv_atm_126d` | IV term structure and longer-tenor level are measured hypotheses (`pv_fields_ic121_v3`: iv_term_63_21, iv_term_126_21, iv_level_21; atx-db iv_term_slope): prior exposure; ATM-only IV has no other canonical stock-return form |
| `ea_window_post3` | a window flag without a hypothesis; `ind_adj_rev_5_nx` already removes the post-announcement window through `ea_days_since` |
| `ea_delay_days` | Johnson-So (2018): priced at the announcement, no drift prior; `ea_overdue` holds the pre-announcement state |
| `ea_time_of_day` | an interaction with `ear` (after-hours or Friday inattention) with no robust published sign at the time-of-day level |
| `ins_net_buy_ratio`, `ins_n_buyers`, `ins_n_sellers`, `ins_cluster_buy` | a second variant of the informed-insider hypothesis that `ins_opp_buy` (X-2) holds; cluster buys are mostly opportunistic buys |
| `k8_item_material_21`, `k8_days_since_any` | pooled 8-K items: a news-drift reading (Chan 2003) would oppose `ind_adj_rev_5_nx` on news names; no canonical sign for days-since |
| `ceq_iss_5y`, `coskew_60m` | read only by the R-7 strings (comp_eq_iss_5y, coskew_60m): that wave was screened and not accepted; not re-registered |
| `vol_126` | volume level: measured (pv libraries; plan section 13) |
| `xrd0_ttm` | `op_rd` / `cbop` already zero-fill `xrd_ttm`; R&D / sales has no return sign (Chan-Lakonishok-Sougiannis 2001) |
| `lt` | leverage enters `qmj_safety` (debt / at) and NOA; book leverage has no agreed sign (Bhandari + against distress -) |
| `grp_sic2` | a classification; the momentum members group by FF49 (house), finer groupings are lane XIMP's |
| `inst_breadth_chg` | conflicting signs (Chen-Hong-Stein 2002 + against Lehavy-Sloan 2008 -); withdrawn in v7 |
| `inst_n_holders` | investor recognition is mostly size, which `ladv63` projects out |
| `regsho_threshold_days63` | the fails-to-deliver family of `ftd_fail`; no separate sign |
| `sv_offexchange_share126` | no signed prior (retail and market-maker mix) |

## 3. The screen set, ranked by prior of marginal contribution to the book's Sharpe and gross return

Ranking criterion (blind; owner directive of 2026-10-02: Sharpe and gross return first, significance and capacity
second): published effect after the house haircut class x orthogonality to the book by construction x breadth x how
much of the effect a daily-rebalanced book can take. Nothing measured ranks anything.

| rank | id | theme | tier | class | origin | mechanism (one line) | horizon | half-life (sessions) | turnover [est] | bars / slots / nodes | extra fields | DSL sha256 (16) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `peer_mom_1m` | price_momentum | B- | 3 | new | last month's return of the stock's FF49 peers continues (industry momentum peaks at one month) | medium | 21 | mid-high | 41 / 6 / 15 | grp_ff49 | `67d4ff4a86c439d7` |
| 2 | `so_wang_rev` | reversal_seasonality | B- | 2, 1 | new | short-term reversal is six times larger just before scheduled earnings news | fast | 2 | high (event) | 3 / 5 / 13 | ea_window_pre5, grp_ff49 | `c145cd2a7c1f85ef` |
| 3 | `iv_vol_of_vol` | options_implied | B- | 4 | LIB2 C-1, carried | uncertainty about risk: high vol-of-vol of ATM IV predicts low returns | medium | 21 | low-mid | 40 / 5 / 13 | iv_atm_21d | `c5ecec15fbdb4807` |
| 4 | `day_rev_freq` | reversal_seasonality | B- | 5, 1 | LIB2 C-2, carried | frequent positive-overnight / negative-intraday sessions predict higher returns | medium | 21 | mid | 40 / 4 / 12 | ret_intraday, ret_overnight | `a5416c4ea13d4422` |
| 5 | `mom_turn` | price_momentum | C+ | 2 | new | momentum is larger among high-turnover stocks (the interaction alone) | slow | 126 | low | 272 / 6 / 22 | shares_out | `09946f74588e3060` |
| 6 | `ea_uvol` | earnings_momentum | C+ | 3 | new | abnormal announcement volume (opinion divergence) predicts higher post-announcement returns | slow | 31 | low-mid | 211 / 5 / 26 | ea_days_since, shares_out | `15f682ffd7504fbb` |
| 7 | `dato` | profitability_quality | C+ | 1, 2 | new | a rise in asset turnover (DuPont) predicts higher returns | slow | 126 | very low | 272 / 5 / 19 | grp_ff12, noa, noa_lag4, sale_ttm | `694fbe591a52faf1` |
| 8 | `fscore_hbm` | profitability_quality | C+ | 2 | new | F-score separates winners from losers inside value stocks | slow | 252 | very low | 20 / 5 / 17 | be, fscore, me_company | `f4d3d3fa7f14fb6f` |
| 9 | `exch_switch` | filing_events | C+ | 1 | LIB2 C-3, carried | negative drift after a listing move up to NYSE / NYSE American | slow | 252 | very low | 0 / 3 / 4 | exch_up_365d | `f433b32af008e74c` |

Class = the brief's search class (1 unread field, 2 conditional / interaction, 3 cross-sectional lead-lag / analyst-free
drift, 4 options / short interest, 5 intraday bars). All nine pass the house budget (bars <= 314 and the runner's 336;
slots <= 7; extra fields <= 5; DSL <= 4,096 B) with no exception. Figures are the XSIG compiler mirror's (calibrated on
16 recorded rows); K1 (`--plan-only` through add-alpha) is the checker of record. Roster order (admission tie-break) =
the rank order, appended after the parent's members.

Full SHA-256 of each frozen DSL string:

| id | SHA-256 |
|---|---|
| peer_mom_1m | `67d4ff4a86c439d7734f7cf6ceff56318137ee1565a3729430a736caada2de77` |
| so_wang_rev | `c145cd2a7c1f85ef88a9bed87662ee8e3af8c71becb1d2b3f742aacd10cf59c8` |
| iv_vol_of_vol | `c5ecec15fbdb480700d8db39b5f8042338ce1a49fd6d86de3a2f7c3a7fc9fcc7` |
| day_rev_freq | `a5416c4ea13d4422592b378716d2a11b493d0152d06c2a48afc3b5937e42c5a7` |
| mom_turn | `09946f74588e3060c9f54a0bbefde0c173e1bb39efcdbb10e6d95633c522b373` |
| ea_uvol | `15f682ffd7504fbb249d5cffedc80a8be28ce93e781e41a5843413f7135d515c` |
| dato | `694fbe591a52faf131cebef6b0ccc753b3d30d7a7fa99609ce3f1915a916a20d` |
| fscore_hbm | `f4d3d3fa7f14fb6ff2d61651880837b187bb66429b4051f167593815e839f21f` |
| exch_switch | `f433b32af008e74c13f04026537a12f735f636cd3a2dadade94200dcd0263ac3` |

## 4. The six new candidates

Every candidate: prior sign +1 (the literature sign is embedded in the string), origin `prior`, one admission trial.
Expected correlations are [est], from construction, not measured.

### Y-1 `peer_mom_1m`: one-month industry-peer momentum (rank 1)

- **Canonical definition and sign.** Moskowitz and Grinblatt (1999, JF 54(4), 1249-1290) "Do industries explain
  momentum?": buying past winning industries and selling past losing ones is profitable after controls for size, B/M
  and individual momentum; unlike stock-level returns, which reverse at one month, **industry momentum is strongest at
  the one-month horizon** and lasts up to a year. Sign: long stocks whose industry won last month.
- **Frozen DSL:**
```
rank(decay_linear((((group_mean(((close / delay(close, 21)) - 1), grp_ff49) * group_count(((close / delay(close, 21)) - 1), grp_ff49)) - ((close / delay(close, 21)) - 1)) / (group_count(((close / delay(close, 21)) - 1), grp_ff49) - 1)), 21))
```
- **How it reads.** r21 = the 21-session return; n = the FF49 group's names with a finite r21; (n x mean - own) / (n - 1)
  is the equal-weighted mean of the peers' r21 with the stock itself left out; a stock alone in its industry is 0 / 0 =
  NaN. House slow form (window >= 10, PM7-34 rule). Fields: close, grp_ff49.
- **Constants.** 21 sessions = the paper's one-month formation; FF49 = the house momentum grouping (`ind_mom_12_1`); 21
  = house decay (it plays the paper's one-month holding with overlapping portfolios).
- **Nearest members and why this is not a restatement.** `ind_adj_rev_5_nx` reverses the stock's 5-session return net
  of its FF49 mean: the industry component it removes is exactly what this member holds, and the stock's own return is
  excluded here (|rho| < .10). `ind_mom_12_1` (FF49 mean of the 12-1 return) uses months 2-12, a disjoint window
  (.10-.25). `stmom` conditions the stock's own one-month return on turnover (.05-.20). `mom_12_1`, `res_mom_ind`
  .05-.20.
- **Deviations.** Equal-weighted peers, not the paper's value-weighted industry portfolios: the value-weighted
  leave-one-out string needs 8 slots in the mirror (op ask, section 9); equal weights follow `ind_mom_12_1`. Leave-one-out
  (the paper's industry portfolio contains the stock; leaving it out keeps the stock's own reversing return out of the
  signal). 49 industries, not 20. Rolling daily, house decay. A sibling share class of the same company is a peer.
- **Prior.** Haircut class: generic 65-75% (1963-1995 sample, as recalled; industry momentum weaker since 2000 in some studies).
  Breadth: every name with an FF49 label. Turnover: mid-high (the input moves a month at a time).

### Y-2 `so_wang_rev`: short-term reversal ahead of scheduled earnings news (rank 2; fast)

- **Canonical definition and sign.** So and Wang (2014, JFE 114(1), 20-35) "News-driven return reversals: liquidity
  provision ahead of earnings announcements" (abstract read): "a six-fold increase in short-term return reversals during
  earnings announcements relative to non-announcement periods"; market makers demand higher expected returns before
  announcements because of the inventory risk of holding through the news. Their sort uses the 3-day return over days
  a-4..a-2 and holds over the announcement window. Sign: long recent losers, short recent winners, among names about to
  announce.
- **Frozen DSL:**
```
rank((ea_window_pre5 * (-1 * group_neutralize(((close / delay(close, 3)) - 1), grp_ff49))))
```
- **How it reads.** `ea_window_pre5` = 1 when the expected announcement (house yoy_364 calendar) is 1..5 sessions ahead,
  0 otherwise, NaN without a calendar. The value is minus the FF49-demeaned 3-session return inside the window, 0
  elsewhere (most names tie at the middle rank). Event-window form R(x): no decay, so the position lives only while
  the announcement is ahead. Reads the unread field `ea_window_pre5`.
- **Constants.** 3 sessions = the paper's formation window; 1..5 sessions = the field's own window (fixed in
  research_fields_sec.py, formula `sec-ea-window-pre5-v1`); FF49 = the house reversal grouping.
- **Nearest members.** `ind_adj_rev_5_nx` (same family, unconditional, excludes the post-announcement window; this
  member is the conditional amplification on about 8% of names at a time): .15-.30. `earn_season` (window 0..21 with a
  centred earnings-seasonality rank: no return input) < .10. XWQ vol_rev members (`wq_035`, `wq_030`, `wq_043`) .05-.15.
- **Deviations.** Rolling 3-session return ending at the decision session while the expected date is 1..5 sessions ahead
  (paper: fixed days a-4..a-2 around the realised date); expected, not realised, date; industry-demeaned (paper: raw);
  the house fills at t+1, so an announcement expected one session ahead is partly missed.
- **Prior.** Haircut class: generic 65-75% (published 2014, sample before 2012). Turnover: high, by design (a name enters
  and leaves within about five sessions each quarter). Under the slow aim it is the candidate most exposed to the
  multi-horizon rule (PM8-5).

### Y-5 `mom_turn`: momentum x turnover interaction (rank 5)

- **Canonical definition and sign.** Lee and Swaminathan (2000, JF 55(5), 2017-2069) (abstract read): "past trading
  volume ... predicts both the magnitude and persistence of price momentum"; in their Table II (as recalled; root
  verifies) the winner-minus-loser spread is larger among high-turnover stocks. Sign of the interaction: +.
- **Frozen DSL:**
```
rank(decay_linear(((rank(((delay(close, 21) / delay(close, 252)) - 1)) - 0.5) * (rank(delay(ts_mean((volume / shares_out), 231), 21)) - 0.5)), 21))
```
- **How it reads.** The product of the centred cross-sectional ranks of the house 12-1 return and of mean daily turnover
  over the same 231 sessions (t-251..t-21; per-day volume / shares_out, both on the session's share basis, so a split
  inside the window cancels). Long high-turnover winners and low-turnover losers; short high-turnover losers and
  low-turnover winners.
- **Orthogonality by construction.** A product of two centred ranks has zero covariance with either factor when the two
  are independent (E[x^2 y] = 0); it carries neither the momentum main effect (`mom_12_1`) nor the turnover main effect.
  Nearest: `mom_12_1` < .10, `stmom` (one-month return, turnover-selected) .05-.15, value members (high turnover is
  glamour, LS2000) .05-.15.
- **Deviations.** Interaction term only; continuous ranks, not 10 x 3 portfolios; the house 12-1 window with the skip
  month; vendor shares_out (A8 90-day lag).
- **Prior.** Haircut class: generic 65-75% (1965-1995 sample, as recalled). Momentum crash exposure is concentrated in its
  high-turnover legs. Turnover: low.

### Y-6 `ea_uvol`: abnormal announcement volume (rank 6)

- **Canonical definition and sign.** Garfinkel and Sokobin (2006, JAR 44(1), 85-112): "post-event returns are strictly
  increasing in the component of volume [at the earnings date] that is unexplained by prior trading activity",
  interpreted as opinion divergence priced as risk (Varian 1985). Sign: +.
- **Frozen DSL:**
```
rank(decay_linear(ts_backfill((log((ts_sum((volume / shares_out), 3) / (3 * delay(ts_mean((volume / shares_out), 60), 7)))) + (0 * log((1 - abs((ea_days_since - 1)))))), 126), 21))
```
- **How it reads.** At t with `ea_days_since` = 1 (t = a + 1, a = the reaction session; the first session at which both
  pre-market and post-market releases are visible under the SEC clock) the cell is log(turnover over a-1..a+1 /
  (3 x mean daily turnover over a-65..a-6)); `0 x log(1 - |ea_days_since - 1|)` is 0 at 1 and NaN on every other
  session; `ts_backfill(., 126)` holds the latest event's value (the house `ear` idiom); decay 21.
- **Constants.** 3 sessions around the reaction day (the announcement window); 60-session baseline ending 5 sessions
  before the window (a quarter of normal trading, clear of the run-up); 126 = `ear`'s backfill; 21 = house decay.
- **Nearest members.** `ear` / `ear_mom_12m` (signed announcement returns; this input is unsigned volume) < .10; the
  pv `volume_shock` family was measured unconditionally (this one is event-anchored); `wq_035` .05-.15.
- **Deviations.** Abnormal volume is the log ratio to the stock's own baseline, not a regression residual; unsigned.
- **Prior.** Haircut class: generic 65-75%. Turnover: low-mid (one update a quarter).

### Y-7 `dato`: change in asset turnover (rank 7)

- **Canonical definition and sign.** Soliman (2008, The Accounting Review 83(3)) "The use of DuPont analysis by market
  participants": the change in asset turnover (sales / net operating assets) predicts future returns
  positively, beyond profitability levels (as recalled; root verifies the paper's table). Sign: +.
- **Frozen DSL:**
```
group_rank(decay_linear(((((sale_ttm / noa) - (delay(sale_ttm, 252) / noa_lag4)) + (0 * log(noa))) + (0 * log(noa_lag4))), 21), grp_ff12)
```
- **How it reads.** ATO now minus ATO four quarters earlier; `noa_lag4` (unread field) is the exact four-quarter NOA of
  the same filing; the `0 x log` guards make non-positive NOA NaN. Ranked within FF12 (house R1).
- **Nearest members.** `opex_at` / `opex_at_f49` (cost level) .10-.20; `noa` / `noa_f49` (NOA level) -.10 to -.20 (a
  falling NOA raises ATO); `asset_growth` .10-.20; `gpa` .05-.15. Not a restatement: a change in an efficiency ratio,
  where the roster holds levels.
- **Deviations.** Period-end NOA (not average); prior-year sales from `delay(sale_ttm, 252)`, which can read a TTM one
  quarter off for a few sessions around a filing (as XSIG-d); continuous rank within FF12.
- **Prior.** Haircut class: generic 65-75% (accounting anomaly, sample to the mid-2000s). Turnover: very low.

### Y-8 `fscore_hbm`: F-score inside value stocks (rank 8)

- **Canonical definition and sign.** Piotroski (2000, JAR 38 supplement, 1-41): within the highest book-to-market
  quintile, a strategy long high F-score and short low F-score firms earns large returns; the paper's universe is value
  stocks only. Sign: +.
- **Frozen DSL:**
```
rank(decay_linear(((rank(((be / me_company) + (0 * log(be)))) > 0.8) ? (fscore - 4.5) : 0), 21))
```
- **How it reads.** Top cross-sectional B/M quintile (positive book only): fscore - 4.5 (the midpoint of 0..9); every
  other name 0.
- **Nearest member.** `fscore` (all names, within FF12): .35-.50; this adds F-score weight inside value names only, the
  paper's own conditioning. Value members < .10 (centred inside the quintile).
- **Deviations.** Continuous score, not 8-9 minus 0-1 portfolios; quintile each session on the role universe.
- **Prior.** Haircut class: generic 65-75%; the published effect is concentrated in small, illiquid value stocks.
  Turnover: very low.

## 5. The three carried candidates (registration unchanged from LIB2, `task-LIB2-report.md` sections 1, 3, 4)

Strings, tiers, themes, citations, formula, domain and deviation texts are exactly LIB2's; `ysig_check.py` asserts the
DSL SHA-256 and that each add-alpha line equals LIB2's up to `--parent` (only the four placeholders change). They were
R-12's screen set; R-12 was not run (it ran only if R-6 was accepted), so their 3 trials lapsed unspent and no
statistic of them exists. Ruling YSIG-a (section 10).

- **`iv_vol_of_vol`** (Baltussen, van Bekkum and van der Grient 2018, JFQA 53(4)): -VOV over 21 sessions of
  `iv_atm_21d`. options_implied has one member (`iv_rv_spread_xe`, risk share .0455); VOV is scaled by the IV level and
  is in neither the pv IV grid nor atx-db's characteristics (LIB2). Nearest: `iv_rv_spread_xe` .10-.25; low_risk
  members .10-.30. Medium, half-life 21 (monthly measure, one-month holding).
- **`day_rev_freq`** (Akbas, Boehmer, Jiang and Koch 2022, JFE 145(3)): share of the last 21 sessions with a positive
  overnight and a negative intraday return; canonical frequency form with a published sign (not the withdrawn
  `night_day` difference). Barardehi, Bogousslavsky and Muravyev (RFS, accepted; abstract read) agree at this horizon:
  intraday returns show the short-term reversal, overnight returns do not. Reads the unread `ret_overnight` /
  `ret_intraday`. Nearest: `ind_adj_rev_5_nx` .05-.20. Medium, half-life 21 (Akbas et al.: next month's returns).
- **`exch_switch`** (Dharan and Ikenberry 1995, JF 50(5)): short names that moved up to NYSE / NYSE American within 365
  days. Needs `exch_up_365d` (built by LIB2's holdings kind `xsw`, on this base since `b63edb31`; not in fields v14):
  withdrawn at 0 trials if the Y fields build lacks it. Sparse, short-only. Slow, half-life 252.

## 6. Registry rows and add-alpha lines (strings frozen)

**L1 themes.** No new theme. Members join existing themes: price_momentum (+2), reversal_seasonality (+2),
options_implied (+1), earnings_momentum (+1), profitability_quality (+2), filing_events (+1).

**L2 fields table** (rows needed: `ea_window_pre5`, `noa_lag4` drafted here from the producers' specs; `ret_overnight`,
`ret_intraday`, `exch_up_365d` verbatim from `task-LIB2-report.md` section 3; root copies `clock` and `basis` checks
against the fields manifest of the build of record):
```json
"ea_window_pre5": {"formula_id": "sec-ea-window-pre5-v1", "origin": "fields_v8", "producer": "atx-engine/tools/research_fields_sec.py", "clock": "sec-acceptance-lag1-v1: a source row is usable at role session t iff available_at (UTC, the EDGAR acceptance resolved by atx-db acceptance-per-file-clock-v1) < 22:00 UTC of session t-1 on the session calendar (role sessions inside the role range, the NYSE rule calendar nyse-rule-v1 outside it); rows available on or after 2024-01-01 are dropped; sessions are counted on the same calendar", "basis": "1 if 1 <= ea_days_to_expected <= 5 else 0 (the expected next earnings announcement, house yoy_364 calendar, is 1..5 sessions ahead); NaN wherever ea_days_to_expected is NaN (formula id sec-ea-window-pre5-v1; producer atx-engine/tools/research_fields_sec.py, fields-v8)"},
"noa_lag4": {"formula_id": null, "origin": "fields_v5", "producer": "atx-engine/tools/prepare_research_fields.py", "clock": "T21 fund group over the T20 atx.fundamental-events/v1 contract: the latest event row (row-level, latest clock wins; restatements enter at the restating filing's clock) of the session line's T19 primary-linked CIK whose clock (FSDS accepted_utc; FC1 fallback filed + 46 h, labelled) is before the session close mark, usable from the next session (--fund-lag-sessions 1); every item of a row shares its anchor period_end; the row is stale (all NaN) when date(session) - period_end exceeds 200 days (quarterly filer) or 400 days (annual-only); no link or no row -> NaN; values modeled/unaccepted", "basis": "net operating assets four fiscal quarters earlier (the asset side at the balance date nearest A - 365 d: at - che - lt + debt, Hirshleifer, Hou, Teoh and Zhang 2004), USD; planned fields-v5 name (T18 section 6, produced by T21); the producer manifest is authoritative"}
```

**add-alpha lines** (`PY="C:/Program Files/Python312/python.exe"`; printed by `ysig_check.py` with these SHA-256
prefixes). Run after L2, in this order. Substitutions allowed: `<Y parent>`, `<Y name>`, `<Y parent spec>`, `<Y fields
dir>` (the fields v14 build, plus `exch_up_365d` for line 9), and `--plan-json` (PM6-9); every other byte is frozen.
Line prefixes: peer_mom_1m `89e07be1e33db181`, so_wang_rev `d7830943610fc079`, iv_vol_of_vol `06ef2910b06dcc1e`,
day_rev_freq `f57d3a76a9df22a1`, mom_turn `0a53d97cc2f24308`, ea_uvol `9f61609229cf55d7`, dato `61728bcc94dcc87f`,
fscore_hbm `ceeed83aa97b7d0f`, exch_switch `7f2cac46fb20a586`.
```bash
"$PY" scripts/research_cycle.py add-alpha --id peer_mom_1m --dsl "rank(decay_linear((((group_mean(((close / delay(close, 21)) - 1), grp_ff49) * group_count(((close / delay(close, 21)) - 1), grp_ff49)) - ((close / delay(close, 21)) - 1)) / (group_count(((close / delay(close, 21)) - 1), grp_ff49) - 1)), 21))" --theme price_momentum --tier B- --prior-sign 1 --citation "Moskowitz and Grinblatt (1999, JF 54(4)) Do industries explain momentum?" --origin prior --prior-sign-source "Moskowitz-Grinblatt 1999" --form "R(decay_linear(x, 21))" --formula "equal-weighted mean 21-session return of the stock's FF49 industry peers with the stock itself excluded: (n x group_mean(r21) - r21) / (n - 1), n = the industry's names with a finite r21; industries that won over the last month continue (industry momentum is strongest at one month)" --domain "NaN where the stock's own 21-session return or its FF49 label is NaN, and for a stock alone in its industry (n = 1: 0 / 0)" --deviation "equal-weighted peers, not the paper's value-weighted industry portfolios (the value-weighted leave-one-out string needs 8 slots in the compiler mirror, over the house 7; equal weights as ind_mom_12_1); the stock is left out of its own industry mean (its own one-month return, which reverses, is not in the signal); 49 Fama-French industries, not the paper's 20; rolling 21 sessions with the house decay, not monthly portfolios; a sibling share class of the same company counts as a peer" --parent <Y parent> --name <Y name> --parent-spec <Y parent spec> --fields <Y fields dir>
"$PY" scripts/research_cycle.py add-alpha --id so_wang_rev --dsl "rank((ea_window_pre5 * (-1 * group_neutralize(((close / delay(close, 3)) - 1), grp_ff49))))" --theme reversal_seasonality --tier B- --prior-sign 1 --citation "So and Wang (2014, JFE 114(1)) News-driven return reversals: liquidity provision ahead of earnings announcements" --origin prior --prior-sign-source "So-Wang 2014" --form "R(x)" --formula "-(3-session return, FF49 industry-demeaned) while the expected earnings announcement is 1..5 sessions ahead (ea_window_pre5 = 1), else 0; short-term reversals are about six times larger ahead of scheduled earnings news (market makers price the inventory risk of holding through it)" --domain "NaN where the earnings calendar is NaN (no visible primary 8-K 2.02 within 200 days, or overdue by more than 63 sessions) or the 3-session return is NaN; 0 outside the window (most names tie at the middle rank)" --deviation "rolling 3-session return ending at the decision session while the expected date is 1..5 sessions ahead (paper: the return over days a-4..a-2, held a-1..a+1 around the realised date); expected date from the house yoy_364 calendar, not the realised date; FF49-demeaned as the house reversal members (paper: raw returns); no decay (event-window form); the house fills at t+1" --parent <Y parent> --name <Y name> --parent-spec <Y parent spec> --fields <Y fields dir>
"$PY" scripts/research_cycle.py add-alpha --id iv_vol_of_vol --dsl "rank(decay_linear(((-1 * (ts_std_mp(iv_atm_21d, 21, 12) / ts_mean_mp(iv_atm_21d, 21, 12))) + (0 * log(ts_std_mp(iv_atm_21d, 21, 12)))), 21))" --theme options_implied --tier B- --prior-sign 1 --citation "Baltussen, van Bekkum and van der Grient (2018, JFQA 53(4)) Unknown unknowns: uncertainty about risk and stock returns" --origin prior --prior-sign-source "Baltussen-van Bekkum-van der Grient 2018" --form "R(decay_linear(x, 21))" --formula "-VOV, VOV = sample std of iv_atm_21d over the last 21 sessions / its mean over the same sessions (each with at least 12 finite cells); long low vol-of-vol" --domain "NaN with fewer than 12 finite IV cells in 21 sessions (iv_atm_21d is NaN outside [0.02, 5]) and on a flat window (std 0: 0 * log(0) is NaN)" --deviation "21 sessions with 12 required (paper: 20 days, 12 required); ddof 1 (paper 1/20); vendor 21-session constant-maturity clean ATM IV (paper: OptionMetrics nearest-ATM call/put average); flat windows NaN; house 21-session decay on a monthly measure; top 3,000" --parent <Y parent> --name <Y name> --parent-spec <Y parent spec> --fields <Y fields dir>
"$PY" scripts/research_cycle.py add-alpha --id day_rev_freq --dsl "rank(decay_linear(ts_mean_mp((((ret_overnight > 0) && (ret_intraday < 0)) ? 1 : 0), 21, 15), 21))" --theme reversal_seasonality --tier B- --prior-sign 1 --citation "Akbas, Boehmer, Jiang and Koch (2022, JFE 145(3)) Overnight returns, daytime reversals, and future stock returns" --origin prior --prior-sign-source "Akbas-Boehmer-Jiang-Koch 2022" --form "R(decay_linear(x, 21))" --formula "share of the last 21 sessions (at least 15 with both returns finite) with a positive overnight return followed by a negative intraday return of the same session (ret_overnight > 0 and ret_intraday < 0); a frequent tug of war predicts higher returns" --domain "a session counts only when both returns are finite (compare and && propagate NaN); NaN with fewer than 15 such sessions; withdrawn at 0 trials if the fields lack the vendor open (FieldNeedsOpen)" --deviation "rolling 21 sessions, not the calendar month; vendor open, not the auction print (a factor step dated t-1 sits in the overnight return); one-session field lag; house 21-session decay" --parent <Y parent> --name <Y name> --parent-spec <Y parent spec> --fields <Y fields dir>
"$PY" scripts/research_cycle.py add-alpha --id mom_turn --dsl "rank(decay_linear(((rank(((delay(close, 21) / delay(close, 252)) - 1)) - 0.5) * (rank(delay(ts_mean((volume / shares_out), 231), 21)) - 0.5)), 21))" --theme price_momentum --tier C+ --prior-sign 1 --citation "Lee and Swaminathan (2000, JF 55(5)) Price momentum and trading volume" --origin prior --prior-sign-source "Lee-Swaminathan 2000" --form "R(decay_linear(x, 21))" --formula "(rank(12-1 return) - 0.5) x (rank(mean daily turnover, volume / shares_out, over the same sessions t-251..t-21) - 0.5): the momentum x turnover interaction alone; momentum is larger among high-turnover stocks" --domain "NaN where the 12-1 return or any of the 231 daily turnovers is NaN" --deviation "the interaction term only, with centred cross-sectional ranks (no momentum main effect, which mom_12_1 carries, and no turnover main effect); continuous, not the paper's 10 x 3 portfolios; the house 12-1 return with turnover over the same 231 sessions; daily turnover = raw share volume / vendor shares_out (A8 90-day lag, split-restated); house decay" --parent <Y parent> --name <Y name> --parent-spec <Y parent spec> --fields <Y fields dir>
"$PY" scripts/research_cycle.py add-alpha --id ea_uvol --dsl "rank(decay_linear(ts_backfill((log((ts_sum((volume / shares_out), 3) / (3 * delay(ts_mean((volume / shares_out), 60), 7)))) + (0 * log((1 - abs((ea_days_since - 1)))))), 126), 21))" --theme earnings_momentum --tier C+ --prior-sign 1 --citation "Garfinkel and Sokobin (2006, JAR 44(1)) Volume, opinion divergence, and returns: a study of post-earnings announcement drift" --origin prior --prior-sign-source "Garfinkel-Sokobin 2006" --form "R(decay_linear(x, 21))" --formula "log(turnover over sessions a-1..a+1 around the reaction session a of the latest earnings announcement / (3 x mean daily turnover over sessions a-65..a-6)), set at t = a+1 and held (ts_backfill 126) until the next announcement; post-announcement returns rise with abnormal announcement volume (opinion divergence)" --domain "NaN without an announcement reaction session in the last 126 sessions with finite turnover windows (turnover = volume / shares_out; ea_days_since NaN for non-filers of 8-K 2.02)" --deviation "abnormal volume = log ratio to the stock's own pre-announcement turnover (paper: volume unexplained by a regression on prior trading activity); 3-session window a-1..a+1; carried to the next announcement with the house ear idiom (ts_backfill 126) and decay 21; unsigned (not conditioned on the earnings news)" --parent <Y parent> --name <Y name> --parent-spec <Y parent spec> --fields <Y fields dir>
"$PY" scripts/research_cycle.py add-alpha --id dato --dsl "group_rank(decay_linear(((((sale_ttm / noa) - (delay(sale_ttm, 252) / noa_lag4)) + (0 * log(noa))) + (0 * log(noa_lag4))), 21), grp_ff12)" --theme profitability_quality --tier C+ --prior-sign 1 --citation "Soliman (2008, The Accounting Review 83(3)) The use of DuPont analysis by market participants" --origin prior --prior-sign-source "Soliman 2008" --form "R(decay_linear(x, 21))" --formula "change in asset turnover: sale_ttm / noa - (sale_ttm 252 sessions earlier) / noa_lag4, ranked within FF12; a rise in asset turnover predicts higher returns" --domain "NaN where net operating assets now or four quarters earlier are not positive (0 x log guard) or an input is NaN" --deviation "NOA at the period end, not average NOA; prior-year sales = sale_ttm 252 sessions earlier (the filing clock can read a TTM one quarter off for a few sessions around a filing; noa_lag4 is the exact four-quarter lag); continuous rank within FF12 (house R1), not deciles; house decay" --parent <Y parent> --name <Y name> --parent-spec <Y parent spec> --fields <Y fields dir>
"$PY" scripts/research_cycle.py add-alpha --id fscore_hbm --dsl "rank(decay_linear(((rank(((be / me_company) + (0 * log(be)))) > 0.8) ? (fscore - 4.5) : 0), 21))" --theme profitability_quality --tier C+ --prior-sign 1 --citation "Piotroski (2000, JAR 38 supplement) Value investing: the use of historical financial statement information to separate winners from losers" --origin prior --prior-sign-source "Piotroski 2000" --form "R(decay_linear(x, 21))" --formula "fscore - 4.5 for the top book-to-market quintile (cross-sectional rank of be / me_company above 0.8), 0 elsewhere; inside value stocks high F-score firms beat low F-score firms" --domain "NaN where book equity is not positive or be / me_company is NaN; for value names NaN where fscore is NaN" --deviation "continuous F-score centred at 4.5 (the midpoint of 0..9), not the paper's high (8-9) minus low (0-1) portfolios; market-wide quintile on the role universe each session, not annual sorts; filing-clock items; house decay; the unconditional fscore member stays (this adds weight inside value names only)" --parent <Y parent> --name <Y name> --parent-spec <Y parent spec> --fields <Y fields dir>
"$PY" scripts/research_cycle.py add-alpha --id exch_switch --dsl "rank((-1 * exch_up_365d))" --theme filing_events --tier C+ --prior-sign 1 --citation "Dharan and Ikenberry (1995, JF 50(5)) The long-run negative drift of post-listing stock returns; Chen-Zimmermann ExchSwitch" --origin prior --prior-sign-source "Dharan-Ikenberry 1995" --form "R(x)" --formula "-1{the FINRA listing market moved up (Nasdaq -> NYSE, NYSE American -> NYSE, Nasdaq -> NYSE American) on a row disseminated within the last 365 days} (exch_up_365d, finra-listing-up-switch365-v1); prior sign negative" --domain "NaN when the listing market is unknown (latest visible name row older than 45 days) or the name history is shorter than 410 days; a binary flag: switched names tie at the bottom rank, the rest tie above" --deviation "FINRA short-interest market class at its semi-monthly dissemination (up to about 15 days after the move) instead of CRSP exchange codes; 365 calendar days; moves that keep the vendor line count (de-SPACs included), moves with a new line are not seen; Arca, BZX and OTC moves are not events; continuous rank, not an event-time portfolio" --parent <Y parent> --name <Y name> --parent-spec <Y parent spec> --fields <Y fields dir>
```

Expected K1 rows (`--plan-only`; bars / slots / extra fields; the exe's node count may exceed the mirror's by one, as in
X batch 1): peer_mom_1m 41 / 6 / grp_ff49; so_wang_rev 3 / 5 / ea_window_pre5, grp_ff49; iv_vol_of_vol 40 / 5 /
iv_atm_21d; day_rev_freq 40 / 4 / ret_intraday, ret_overnight; mom_turn 272 / 6 / shares_out; ea_uvol 211 / 5 /
ea_days_since, shares_out; dato 272 / 5 / grp_ff12, noa, noa_lag4, sale_ttm; fscore_hbm 20 / 5 / be, fscore, me_company;
exch_switch 0 / 3 / exch_up_365d. A K1 row that differs is reported; a string K1 refuses is rewritten only mechanically
(same semantics) or withdrawn at 0 trials.

**Withdrawal rule.** A candidate whose field is absent from the Y fields build is withdrawn at 0 trials (exch_switch
without `exch_up_365d`; every other candidate reads only fields v14). **Trial accounting.** Up to 9 admission trials;
carried candidates spend their trial once, here (the v9 wave does not register them again). **Roster.** Library v8x4
holds 58 members; + 12 (X-7) + 9 = 79, under the cap 80 (registry `house_budget.max_roster`).

## 7. Forecast horizon (Ruling PM8-5; registered blind, from the paper or the construction window)

Classes: fast = alpha half-life under 5 sessions; medium 5 to 21; slow over 21. Turnover [est] as in section 3.

| id | horizon class | half-life (sessions) | source of the half-life | expected daily turnover |
|---|---|---|---|---|
| peer_mom_1m | medium | 21 | Moskowitz-Grinblatt: industry momentum strongest over a one-month holding period | mid-high |
| so_wang_rev | fast | 2 | So-Wang: the reversal is earned in the 3-session announcement window a-1..a+1 | high (event) |
| iv_vol_of_vol | medium | 21 | Baltussen et al.: monthly VOV, one-month holding | low-mid |
| day_rev_freq | medium | 21 | Akbas et al.: the frequency predicts the next month's returns | mid |
| mom_turn | slow | 126 | Lee-Swaminathan: momentum holding periods of 3-12 months; the interaction inherits momentum's horizon | low |
| ea_uvol | slow | 31 | Garfinkel-Sokobin: post-announcement window to the next announcement (about 63 sessions; half of it) | low-mid |
| dato | slow | 126 | Soliman: returns over the following year; quarterly inputs | very low |
| fscore_hbm | slow | 252 | Piotroski: one-year buy-and-hold after annual statements | very low |
| exch_switch | slow | 252 | the field's 365-day window; Dharan-Ikenberry drift over three years | very low |

The set spans the three classes (1 fast, 3 medium, 5 slow). Few fast effects with a published sign survive the house's
data and prior exposure (section 8: the abnormal short-flow and announcement-premium families fail on recent evidence;
the fast price-volume space is XWQ's and the pv libraries').

**Appendix A. One half-life per existing theme (proposal; the PM rules; YCOMB reads it).**

| theme | horizon class | half-life (sessions) | source (blind) | expected daily turnover |
|---|---|---|---|---|
| value | slow | 252 | annual book-to-market formation, premia persist for years (Fama-French 1992) | very low |
| profitability_quality | slow | 252 | annual profitability formation (Novy-Marx 2013; Piotroski 2000) | very low |
| investment_issuance | slow | 252 | annual asset growth and issuance sorts (Cooper-Gulen-Schill 2008; Pontiff-Woodgate 2008) | very low |
| earnings_momentum | slow | 31 | post-announcement drift concentrated in the 60 sessions after the news (Bernard-Thomas 1989) | low |
| price_momentum | slow | 126 | 3-12 month holding periods (Jegadeesh-Titman 1993) | low |
| low_risk | slow | 126 | monthly-rebalanced persistent risk characteristics (Frazzini-Pedersen 2014; Bali-Cakici-Whitelaw 2011) | low |
| short_interest | slow | 126 | shorting-flow predictability "decays slowly and lasts for a year" (Wang-Yan-Zheng 2020) | low |
| reversal_seasonality | fast | 5 | weekly reversal (Lehmann 1990), the theme's founding member `ind_adj_rev_5`; heterogeneous: seasonality members 21 (Heston-Sadka 2008), `inst_persist` 252, `div_season` 21 | high |
| options_implied | medium | 21 | one-month horizon of the IV-RV spread (Bali-Hovakimian 2009) | low-mid |
| ownership_flow | slow | 63 | quarterly 13F conviction and opportunistic insider trades predicting the following months (Cohen-Polk-Silli 2010; Cohen-Malloy-Pomorski 2012) | low |
| filing_events | slow | 126 | the 126-session NT window (Bartov-Konchitchki 2017); `k8_intensity` yearly; `nonreliance_402` 63 | very low |
| price_volume | fast | 3 | published holding periods of 0.6-6.4 days (Kakushadze 2016) | high |

reversal_seasonality mixes a fast core with slow calendar members; one theme half-life fits it poorly, so YCOMB may need
a member-level override there (the table above gives the member values).

## 8. Considered and not included (each examined blind)

| family | reason |
|---|---|
| Abnormal FINRA short-sale flow (Boehmer-Jones-Zhang 2008; Diether-Lee-Werner 2009) | Wang, Yan and Zheng (2020, JFE 135, 191-212; paper read), on the public FINRA daily data 2010-2015: "abnormal short-term shorting flows do not predict future returns"; long-term flows do, which `sv_flow` (126-session level) holds. The field builder was therefore not written |
| Earnings-announcement premium (Frazzini-Lamont 2007; Barber et al. 2013) | reported gone in the US after 2004 (Heitz, Narayanamoorthy and Zekhnini, WP; shifted to 8-K filing periods); a gross-return prior near zero |
| High-volume return premium (Gervais-Kaniel-Mingelgrin 2001) | measured as `volume_shock_5_63` / `volume_spike_21_126` (pv libraries): prior exposure; the XWQ vol_rev members carry uncentred volume ranks |
| Intraday one-month reversal (Barardehi-Bogousslavsky-Muravyev) | the canonical sort is on the intraday return: |rho| about .6-.7 with the measured `reversal_21_skip5` and .3-.4 with `ind_adj_rev_5_nx` [est]; `day_rev_freq` carries the decomposition's one-month hypothesis |
| Intraday 12-1 momentum (same paper; Lou-Polk-Skouras 2019) | a refinement of `mom_12_1` (|rho| .7-.8 [est]): lane XIMP's domain |
| Value x momentum (Asness 1997; Daniel-Titman 1999); momentum x uncertainty (Zhang 2006) | second and third momentum interactions: turnover, volatility and glamour are correlated conditioners; one interaction (`mom_turn`) is registered |
| Insider cluster buys (`ins_cluster_buy`) | a second insider variant beside `ins_opp_buy` |
| News versus no-news drift (Chan 2003) on `k8_item_material_21` | would hold the opposite sign to `ind_adj_rev_5_nx` on news names (PM7-36 c: no opposed priors on one mechanism); the 8-K is a coarse news clock |
| Distraction and drift (Hirshleifer-Lim-Teoh 2009) | expressible (`vec_sum` of same-day announcers) but an interaction on `ear` with a modest prior and 7+ slots |
| Revenue surprise (Jegadeesh-Livnat 2006) | needs quarterly sales; 63-session lags of `sale_ttm` misread the 10-K's longer gap; a `sale` lag item would change the fundamental-events export (out of scope) |
| Short-term residual reversal (Blitz et al. 2013) | the reversal family at 21 sessions: restates the theme's core |
| Price delay (Hou-Moskowitz 2005) | measured (`lagged_market_corr_240`) |
| Range-based volatility on `high_adj` / `low_adj` (Parkinson; Garman-Klass) | a volatility level that the book's `price-risk-v1` projection (vol63) removes |
| High-low spread (Corwin-Schultz 2012; Abdi-Ranaldo 2017) | illiquidity premium weak in large caps; `ladv63` projected |
| Close location value; overnight gap | no peer-reviewed sign; gap cluster closed (XWQ E6) |
| Options conditioned on liquidity; IV change / term / level | no published conditional with a sign; the IV grid is measured; skew, volume and open interest need data (XDATA section 5, item 1) |
| Short interest x institutional ownership | `si_low_io` holds it |
| Hou (2007) lead-lag (`ind_leadlag`, v9 C-2) | rejected by XSIG; `peer_mom_1m` takes the industry-level one-month signal |
| Intra-industry information transfer (Ramnath 2002 against Thomas-Zhang 2008) | conflicting signs |

## 9. Op asks (no op written)

- **`group_sum(x, g)`**: per date, the sum of x over the group's valid members (x not NaN, the same non-NaN label),
  broadcast to every valid member; a NaN label or a NaN x gives NaN; summation in ascending instrument order (as the sum
  inside `group_mean`); lookahead-safe, cross-sectional, arity 2, group argument as `group_mean`. It equals
  `group_mean(x, g) * group_count(x, g)` up to rounding, in one node. Use: the canonical value-weighted leave-one-out
  peer return `(group_sum(r * w, g) - r * w) / (group_sum(w + 0 * r, g) - (w + 0 * r))` fits 7 slots in the mirror (41
  bars, 7 slots, 20 nodes), where today's spelling needs 8. No frozen string uses it: `peer_mom_1m` is registered
  equal-weighted; a value-weighted variant would be a later registration, not a swap.

No data ask beyond XDATA's list (option skew / volume / open interest; securities-lending fees would open the
short-interest families that public FINRA data cannot).

## 10. Rulings root needs before the Y freeze (decision -- why -- cost if wrong)

- **YSIG-a (carried R-12 registrations):** register `iv_vol_of_vol`, `day_rev_freq`, `exch_switch` with LIB2's frozen
  argv (only the four placeholders change) -- R-12 never ran, so no statistic exists and no trial was spent; they fill
  the smallest theme and two unread fields -- cost if wrong: if R-12's lapse closes them, the set is 6.
- **YSIG-b (exch_switch field):** the Y fields build = v14 + `exch_up_365d` (`--reuse` v14; the xsw kind is on the base)
  -- one field, no new option -- cost if wrong: the candidate is withdrawn at 0 trials.
- **YSIG-c (equal-weighted peers):** `peer_mom_1m` uses equal weights because the value-weighted string breaks the slot
  budget -- a static figure, no return read -- cost if wrong: small stocks weigh more in the peer mean than in the
  paper (Hou: large firms lead), diluting the lead-lag part.
- **YSIG-d (event form without decay):** `so_wang_rev` takes R(x), not the PM7-34 decay rule (that rule was written
  for the 101 formulas' price windows) -- a decay would hold the position past the announcement it is priced for --
  cost if wrong: high turnover; the multi-horizon rule (PM8-5) is where its cost is judged.
- **YSIG-e (fscore beside fscore_hbm):** no member is removed -- one variant per hypothesis: the conditional is
  Piotroski's own definition -- cost if wrong: F-score weight inside value names roughly doubles.
- **YSIG-f (horizon table):** section 7 and Appendix A are a proposal for YCOMB -- PM8-5 asks for one registered table
  -- cost if wrong: YCOMB mis-weights horizons until the PM amends it.

## 11. How root verifies

1. `"C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-v8-20260929/ysig_check.py` from the repository
   root (about 15 s; synthetic data only) prints the reader count and the 25 unread fields, the nine candidate rows
   (bars, slots, nodes, bytes, extra fields, unread-field reads, registry rows needed, full SHA-256), the six semantic
   lines, the mutation and causality probe line, the nine add-alpha lines with their SHA-256 prefixes and `ysig_check:
   PASS`. It reads `registry.cpp`, `registry.json`, `libraries/v80.json`, `scripts/specs/v8/lib-v8x4b.json` (field
   names), `task-LIB2-report.md` and the XSIG / XWQ checkers.
2. K1 at add-alpha must print the rows of section 6.
3. Identity: no code path changed. `git diff --stat 798d3b23..HEAD` lists only `ysig_check.py` and this report, so every
   accepted output is byte-identical by construction.

## 12. Hygiene (what was read)

- Read: lane-rules.md; task-X-briefs.md (rules for every X lane, lane XSIG); task-XSIG-report.md; task-XWQ-report.md;
  task-XDATA-report.md sections 1 and 3; v8x-prereg.md section 14; library-v8-draft.md (sections 0-2, R7-5);
  task-LIB2-report.md sections 1, 3, 4, 6; `scripts/specs/v8/lib-v80.json` and the v8 spec field lists; the registry
  (themes, fields, 90 strings); `libraries/v80.json`; `op_catalog.cpp`, `registry.cpp`, `oracle.cpp` (group and
  backfill kernels), `lit_ops.hpp` (min-periods semantics); the field specs in `research_fields_sec.py`,
  `prepare_research_fields.py` (sv and fundamental items), `build_fundamental_events.py` (NOA items); the DSL ids and
  strings of the three pv libraries (no result); `xsig_check.py`, `xwq_check.py`.
- Web (definitions, signs, samples, published statistics only): So-Wang 2014 abstract (IDEAS); Barardehi-Bogousslavsky-
  Muravyev abstract (Illinois experts) and conference slides (Jacobs Levy center); Wang-Yan-Zheng 2020 (author copy);
  Lee-Swaminathan 2000 abstract (EconPapers); search summaries for Moskowitz-Grinblatt 1999, Garfinkel-Sokobin 2006 and
  Heitz-Narayanamoorthy-Zekhnini. Literature samples end before 2024 where stated (2010-2015; 1926-2019).
- Opened under `C:/atx-wt/pool-2/build-equity/`: nothing. No data payload; nothing dated 2024-01-01 or later; pool 10
  not entered; `atx-db/` not touched.
- **Disclosures.** (1) Two greps of `progress.md` (to find the XWQ withdrawal ruling PM7-36 c and R-12's status)
  printed adjacent PM lines carrying cell-level deflated-Sharpe readings of X batch 1 (X-2, X-3, X-4) and of R-7. Seen,
  used for nothing: no candidate here is a member or variant of those cells, and none was chosen or ranked by them.
  (2) Member counts of libraries v8x2 / v8x3 / v8x4 were printed (52 / 60 / 58); used only for the roster arithmetic
  (the public "6 of 8" already implies them). (3) No return, IC, Sharpe, turnover or NAV output of any window was opened.

## Cross-lane edits

None. Files: `.superpowers/sdd/platform-v8-20260929/ysig_check.py`, this report.

## Deviations from the brief

- 9 candidates, not up to 16: the quality bar (a published sign that survives recent evidence, orthogonal by
  construction, not prior-exposed) left no more in house; padding would only add trials.
- No new field builder: no surviving candidate needs a new field (the abnormal short-flow field was dropped on WYZ 2020).
- Three candidates are carried registrations (LIB2), as XSIG carried the v9 draft's (precedent XSIG-e).
- PM8-5 (horizon per candidate and per theme) added mid-lane and done (section 7).

## Open risks

- `peer_mom_1m` is equal-weighted (canonical: value-weighted); industry momentum is weaker since 2000 in some studies.
- `so_wang_rev` depends on the expected date's accuracy and on the fill lag; under the slow aim it may capture little.
- The carried three are unrun on data; `exch_switch` needs a field build.
- `mom_turn` concentrates momentum crash risk in high-turnover names; `fscore_hbm` and `dato` are old accounting
  effects strongest in small stocks; `ea_uvol` rests on one study.
- Two signs are as recalled from the paper tables (Lee-Swaminathan Table II, Soliman); root verifies before the freeze.
- Static figures are the mirror's; K1 at add-alpha decides.

## Round 2b (Ruling PM8-6: fill toward 16, at most 7 more; same rules; registered blind 2026-10-02)

PM8-6 granted YSIG-a, YSIG-b and YSIG-c and raised `max_roster` to 96 for Y. Round 2b searched, in the order given,
short_interest, the 25 unread v14 fields, then ownership_flow and value. **Result: 1 more candidate, `ins_cluster`
(ownership_flow).** The search found no other signal with a published sign that is not already a roster or X member,
prior-exposed, or missing its data. I stopped there rather than pad (PM8-6). The Y set is **10**.

| item | status | commit |
|---|---|---|
| `ysig_check.py` round 2b (`ins_cluster`: oracle, 3 mutants, causality) | DONE | `b501d370` |
| This section | DONE | this commit |

Test line: `ysig_check.py` -> `ysig_check: PASS` (10 candidates; 7 new strings equal to their numpy definitions; 25
mutants fail; the 9 round-2 add-alpha lines byte-unchanged, same SHA-256 prefixes as section 6).

### 2b.1 short_interest (priority 1): no candidate

Every short-interest signal with a published sign that the house data carry is already a member. The rest need
lending data that no v14 field carries.

| suggestion / family | published basis | disposition |
|---|---|---|
| Low short interest is good news | Boehmer, Huszar and Jordan (2010, JFE) | the low end of `si_ratio`'s rank (a cross-sectional rank holds both tails): restatement |
| Short-interest change ("momentum" of SI) | Desai et al. (2002, JF); BHJ (2010) | `si_change` (21-session change of SI / shares); the pv library measured si_change_10 / 21 / 42: restatement and prior exposure |
| Days to cover | Hong, Li, Ni, Scheinkman and Yan (2016) | `dtc`, `dtc_slow` (126-session volume): restatement |
| SI conditioned on institutional ownership (lendable supply proxy) | Asquith, Pathak and Ritter (2005, JFE); Nagel (2005) | `si_low_io` = SI / IO, the utilisation proxy of the public-data literature: restatement |
| Utilisation, borrow fee, lendable supply | Cohen, Diether and Malloy (2007, JF); Engelberg, Reed and Ringgenberg (2018, JF); Drechsler and Drechsler (2016) | no v14 field (no fee, utilisation or loan-quantity column; atx-db `borrow_proxy/` is "public short-side inputs" and is not bound, XDATA 1b): **data ask** (securities-lending data, as XDATA section 5) |
| Threshold-list days as a hard-to-borrow conditioner (`regsho_threshold_days63`) | Evans, Geczy, Musto and Reed (2009, RFS): fails mark expensive borrowing | the FTD family that `ftd_fail` holds; using the list to condition SI would be my construction, not a published signal |
| Shorting flow (FINRA daily) | Wang, Yan and Zheng (2020, JFE) | the long-window level is `sv_flow`; the abnormal short-window flow does not predict (round 2, section 8) |

### 2b.2 The 25 unread v14 fields: does a published signal read each one?

| field | published signal that reads it | taken? |
|---|---|---|
| `ea_window_pre5` | So-Wang (2014): pre-announcement reversal | yes, Y-2 `so_wang_rev` |
| `ret_overnight`, `ret_intraday` | Akbas et al. (2022): tug-of-war frequency | yes, `day_rev_freq` |
| `noa_lag4` | Soliman (2008): change in asset turnover | yes, Y-7 `dato` |
| `ins_cluster_buy` | Alldredge-Blank (2019): clustered insider purchases | **yes, round 2b `ins_cluster`** |
| `ins_n_buyers`, `ins_n_sellers`, `ins_net_buy_ratio` | Lakonishok-Lee (2001): net purchase ratio | no: the all-insider net ratio adds back the routine trades that Cohen-Malloy-Pomorski show are noise, beside `ins_opp_buy`; one insider candidate (`ins_cluster`) is taken |
| `iv_atm_63d`, `iv_atm_126d` | IV term-structure slope (Vasquez 2017 for option returns) | no: measured in `pv_fields_ic121_v3` (iv_term_63_21, iv_term_126_21): prior exposure |
| `ea_delay_days` | Johnson-So (2018, JFQA 53(6)): advancers beat delayers by 2.6% in the month after the calendar revision, priced at the announcement | no: the field is the realised delay, known only at the announcement, after the return; the pre-announcement state is `ea_overdue` |
| `ea_window_post3` | none (a window flag) | no |
| `ea_time_of_day` | after-hours / Friday inattention drift (DellaVigna-Pollet 2009) | no: an interaction on `ear` with no robust time-of-day sign |
| `k8_item_material_21` | Chan (2003) news drift, read through 8-K items | no: opposes `ind_adj_rev_5_nx` on news names (PM7-36 c) |
| `k8_days_since_any` | none | no |
| `ceq_iss_5y`, `coskew_60m` | Daniel-Titman (2006); Harvey-Siddique (2000) | no: R-7 members, screened and not accepted (their trials are spent) |
| `vol_126` | turnover level (Datar, Naik and Radcliffe 1998) | no: measured (pv libraries); `ladv63` projected |
| `xrd0_ttm` | R&D / ME (Chan, Lakonishok and Sougiannis 2001) | no: `rd_me`, `op_rd` read R&D already |
| `lt` | market leverage A/ME (Fama-French 1992); net debt financing (Bradshaw, Richardson and Sloan 2006) | no: A/ME is subsumed by B/M; the debt-financing leg is investment_issuance (asset growth, NOA) |
| `grp_sic2` | none (a classification) | no |
| `inst_breadth_chg` | Chen-Hong-Stein (2002) + against Lehavy-Sloan (2008) - | no: conflicting signs |
| `inst_n_holders` | investor recognition (Merton 1987; Lehavy-Sloan 2008) | no: a size proxy, `ladv63` projected; no signed 13F-count level form |
| `regsho_threshold_days63` | Evans et al. (2009); Fotak, Raman and Yadav (2014): fails do not lower prices | no: no return sign beyond `ftd_fail` |
| `sv_offexchange_share126` | none with a return sign | no |

### 2b.3 ownership_flow and value (priority 3)

- **ownership_flow:** `ins_cluster` (below). Also examined: Lakonishok-Lee net purchase ratio (above); opportunistic
  *sales* (would reverse X-2's buy-leg ruling for `ins_opp_buy`); 13F IO level (Gompers-Metrick 2001: a one-time demand
  shift, size-correlated); one-quarter IO change (conflicting signs, v7); fund flows and manager types (no data, XDATA
  ranks 2 and 7).
- **value: no candidate.** Every value signal with a published sign that is in house is held: B/M, E/P, CF/P, FCF/P,
  EBIT/EV (the enterprise multiple of Loughran-Wellman 2011 without D&A), net payout (dividends included), S/P, R&D/ME,
  the composite. Penman, Richardson and Tuna's (2007) enterprise B/P equals B/M for a firm without net debt
  (|rho| .7-.85 [est]). A/ME is subsumed by B/M. Long-term reversal needs more than the runner's 336 bars and is spanned
  by HML (Fama-French 1996). Intangible-adjusted value (Eisfeldt, Kim and Papanikolaou 2022; Lev-Srivastava 2022) needs
  multi-year SG&A and R&D histories that no field carries: a builder ask, not written here (the issuer export has no
  SG&A or R&D lags).

### 2b.4 Y-10 `ins_cluster`: clustered insider purchases

- **Registration.** ownership_flow; tier C+; prior sign +1 (long flagged names); origin `prior`; 1 admission trial.
  Citation: Alldredge and Blank (2019, Journal of Financial Research 42(2), 331-360) "Do insiders cluster trades with
  colleagues? Evidence from daily insider trading" (abstract via search summary): insider purchases made within two days
  of a peer insider's purchase earn abnormal returns of 2.1% over the next month, 0.9 points more than solitary
  purchases; clustering is greater when information asymmetry is high. Sign: +.
- **Frozen DSL** (SHA-256 `a3dfad407c4cd493562022b50172e18ab764033812844a6b23b9209a788f23d2`):
```
rank(ins_cluster_buy)
```
- **How it reads.** `ins_cluster_buy` (fields v8, `sec-ins-cluster-buy21-min3-v1`) = 1 when at least 3 distinct primary
  reporting owners (directors and officers on original Form 4s) have an open-market purchase with a transaction date in
  the last 21 sessions, visible at t (acceptance before 22:00 UTC of t-1), else 0; NaN without Section 16 presence.
  Event-flag form R(x), as `ins_opp_buy`: flagged names tie at the top. Bars 0, slots 2, nodes 2; 1 extra field (registry
  row below). Reads an unread field.
- **Constants.** The field's own 21-session window and 3-buyer threshold (fixed in research_fields_sec.py; one month =
  the paper's return horizon). No decay: the flag expires with its window.
- **Nearest member and why this is not a restatement.** `ins_opp_buy` = max(net opportunistic shares bought over 126
  sessions, 0) / shares out (Cohen-Malloy-Pomorski: routine versus opportunistic). `ins_cluster` conditions on how many
  insiders independently buy within one month, whatever their routine class. First-time buyers are unclassified under
  CMP's three-year rule and are excluded there but counted here. Expected rho .20-.40 [est]. `inst_best_ideas` < .10.
- **Deviations.** At least 3 buyers within 21 sessions, not a purchase within two days of a peer's purchase; directors
  and officers only (the field's rule); a continuous rank of a flag; the flag stays on for 21 sessions after the trade
  date, less the filing lag (up to 2 business days for Form 4).
- **Horizon (PM8-5).** Medium; half-life 10 sessions (the paper's one-month abnormal return, held over the field's
  21-session window: half of it); expected daily turnover: event (a small share of names, changing within a month).
- **Prior.** Haircut class: generic 65-75% (single study, published 2019). Breadth: sparse (cluster buys are rare in
  large caps), long-only by construction.

**L2 fields table row** (from the producer's spec; root checks it against the fields manifest of the build of record):
```json
"ins_cluster_buy": {"formula_id": "sec-ins-cluster-buy21-min3-v1", "origin": "fields_v8", "producer": "atx-engine/tools/research_fields_sec.py", "clock": "sec-acceptance-lag1-v1: a Form 4 row is usable at session t iff its EDGAR acceptance is before 22:00 UTC of session t-1 (lag 1 session)", "basis": "1 if at least 3 distinct primary reporting owners have an open-market purchase (code P) on an original Form 4 visible at t with transaction_date on or after session t-21 (21 sessions), else 0; directors and officers, joint filings with a 10% owner dropped (W5a rule 1); NaN without a visible insider row within 365 days (Section 16 presence) or while the window starts before the stage's first filing quarter (2015q1) (formula id sec-ins-cluster-buy21-min3-v1; producer atx-engine/tools/research_fields_sec.py, fields-v8)"}
```

**add-alpha line** (run after the nine lines of section 6; same substitutions; line SHA-256 prefix `2c7272b4d4e4c852`):
```bash
"$PY" scripts/research_cycle.py add-alpha --id ins_cluster --dsl "rank(ins_cluster_buy)" --theme ownership_flow --tier C+ --prior-sign 1 --citation "Alldredge and Blank (2019, Journal of Financial Research 42(2)) Do insiders cluster trades with colleagues? Evidence from daily insider trading" --origin prior --prior-sign-source "Alldredge-Blank 2019" --form "R(x)" --formula "ins_cluster_buy: 1 when at least 3 distinct insiders (directors or officers) made open-market purchases with transaction dates in the last 21 sessions, visible at t, else 0; clustered insider purchases are followed by higher abnormal returns over the next month than solitary ones" --domain "NaN without a visible insider transaction row of the issuer within 365 days (Section 16 presence; foreign private issuers NaN); a binary flag: flagged names tie at the top rank, the rest tie below" --deviation "cluster = at least 3 distinct buyers within 21 sessions (field sec-ins-cluster-buy21-min3-v1), not the paper's purchase within two days of a peer insider's purchase; directors and officers on original Form 4s (10% owners and joint 10%-owner filings excluded); continuous rank of a flag, not an event-time portfolio; no decay (the flag lives 21 sessions from the trade date)" --parent <Y parent> --name <Y name> --parent-spec <Y parent spec> --fields <Y fields dir>
```
Expected K1 row: 0 bars / 2 slots / ins_cluster_buy. Roster: 58 + 12 + 10 = 80, under the PM8-6 cap of 96. Admission
trials: up to 10. Horizon mix of the Y set: 1 fast, 4 medium, 5 slow.

### 2b.5 Hygiene, cross-lane edits, risks (round 2b)

- Read in addition: `research_fields_sec.py` insider constants and specs (INS_CLUSTER_WINDOW 21, INS_CLUSTER_MIN 3); the
  registry rows of `ins_opportunistic_net` and `ins_opp_buy`. Web (abstract and search summaries only): Alldredge-Blank
  2019; Johnson-So 2018. Nothing opened under `build-equity/`; no payload; nothing dated 2024 or later; `atx-db/` not
  touched. No cross-lane edits (files: `ysig_check.py`, this report).
- Risks: `ins_cluster` rests on one study and its effect is mostly in small firms. Its cluster rule (3 buyers, 21
  sessions) is looser than the paper's (2 days). Flagged names are few, so its weight in the theme may outrun its
  breadth. The trade-date window plus the filing lag shortens the flag's live time by up to two sessions.
