# Platform v8 — status 8: final v8 report (P9 Phase 0 step R0-14, 2026-10-03)

Breakpoint: every v8 Y cell has run and has been judged by its registered rule. The adoption print is done (R0-12). OD-3
was not read (R0-13; DEC-3 / OD-P9-2 defer it to the P9 freeze). The P9 branch `feat/platform-p9-20261003` is cut from
the commit that adds this file. This file is the owner's report at the close of v8.

How to read it:
- Nothing here was recomputed. Each number is copied from a root report, from the v8 integration log, or from the reader,
  bundle or summ file those name. Section 0 lists the sources.
- Where two sources differ, both values are shown and the difference is named (section 6.4).
- Every number is in sample: TRAIN 2020-2023, scenario S2 (section 6).
- Nothing dated 2024-01-01 or later has been opened. The hidden block and the OD-3 history read are both still closed.

## 0. Sources (cited by key)

| key | file | sha256 prefix / lines |
|---|---|---|
| L6 .. L12 | v8 integration log `.superpowers/sdd/platform-v8-20260929/integration-log.md`, sections R0-6 (from 6686), R0-7 (7012), R0-8 (7120), R0-9 (7227), R0-10 (7427), R0-11 (7614), R0-12 (7874-8045) | line numbers |
| R6 .. R12 | `.superpowers/sdd/platform-p9-20261003/root-R0-<n>-report.md` | section names |
| BK12 | `build-equity/p9-r12-adopt/book.json` (book reader, six books) | `642ad29b` |
| BK10, BK11 | `build-equity/p9-r10-x10/book.json`, `build-equity/p9-r11-y1/book.json` | `d222b547`, `b85952b0` |
| DSR12 | `build-equity/p9-r12-adopt/dsr.json`, `dsr-run1/stdout.log` | `fdb6a972`, `cd1a6375` |
| BU | `p9-r12-adopt/bundle-r2-yf0.json`, `bundle-xf0-yf0.json`, `bundle-b0c-yf0.json`; `p9-r10-x10/bundle.json`; `p9-r11-y1/bundle-x10.json`, `bundle-yf0.json` | `803e5456`, `f3098044`, `24f6479b`; `a57d82d0`; `241c5bd9`, `4c15e0be` |
| WR | `build-equity/waves/{y-s,y-3,y-2,y-5}/wave-result.json` | `57e9f5ea`, `619809ea`, `09fbd24e`, `ec4e07c2` |
| PBO | `build-equity/cycle-v8y-vol-target-y-1/pbo.json`; `build-equity/cycle-v8ysb-gm/pbo.json` | `2beb256c`; `556192c0` |
| T | trial ledger `build-equity/trials.jsonl` | 133 lines, `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554` |
| P9L | P9 sprint ledger `.superpowers/sdd/platform-p9-20261003/progress.md` | line numbers |
| v8y | `.superpowers/sdd/platform-v8-20260929/v8y-prereg.md` | section numbers |
| plan | `docs/plans/2026-10-03-p9-sprint-plan.md` | section / line |

R0-14 re-hashed every file above, and each equals the prefix shown. It re-read BK12's six keys for the three books in
section 1: they equal R12 section 7 to every digit.

## 1. The three books side by side (v8y §8 "Report")

Scenario S2 `modeled-1bn-stale5-v1+swap-fin-v1`, TRAIN 2020-2023, 1,005 return sessions.

| book | spec | L / mean L_t | mean gross, all rows | net annual (CAGR) | net Sharpe 1x | net Sharpe 4x NAV | realised vol | max drawdown | cost bps / traded $ |
|---|---|---|---|---|---|---|---|---|---|
| **Y-F0**: unlevered, deployable | `lib-v8ysb-gm.json` | L 1.1828, fixed | .98621 | **5.654%** (5.767%) | **1.8495** | **1.6994** | **3.057%** | **2.580%** | 12.66 |
| **X-10**: fixed leverage | `x-leverage-L2.0.json` | L 2.0, fixed | 1.66672 | **9.306%** (9.606%) | **1.8083** | **1.5829** | **5.146%** | **4.326%** | 14.51 |
| **Y-1**: vol-target, = Y-F | `y-vol-target-y-1.json` | cap 2.0; **mean L_t 1.8671** (min 1.0, max 2.0) | 1.55566 | **8.799%** (9.070%) | **1.8298** | **1.6281** | **4.809%** | **3.602%** | 14.03 |

Sources:
- BK12 (R12 §7; L12 7990-8001). These equal BK10 and BK11 (L10 7583-7586; L11 7842-7846).
- The 4x column comes from each NAV's `capacity_curve.csv` (L11 7848-7850).
- Mean L_t is from the `vol_target` block of Y-1's `summary.json`: 1.8671322726 (R12 §7).
- Y-1's 1,004 decisions: 584 at the cap 2.0, 21 at the floor 1, 399 between (R11 `vol_target.csv`; R12 Part A).

Printed beside them (decide nothing):
- **Annual costs** (trade / borrow / long financing):
  - Y-F0 .885% / .328% / .201%;
  - X-10 1.722% / .554% / .340%;
  - Y-1 1.563% / .516% / .318%.

  Sources: L10 7588; L11 7848.
- **Capped share of fills at 4x NAV:** Y-F0 .1004, X-10 .2047, Y-1 .1882 (L11 7850).
- **Year table** (L11 7852-7857):

  | year | net return Y-1 / X-10 / Y-F0 | net Sharpe Y-1 / X-10 / Y-F0 | vol Y-1 / X-10 / Y-F0 |
  |---|---|---|---|
  | 2020 | .0605 / .0696 / .0431 | 1.250 / 1.278 / 1.335 | .0479 / .0538 / .0320 |
  | 2021 | .1591 / .1673 / .0983 | 2.893 / 2.938 / 2.984 | .0515 / .0531 / .0316 |
  | 2022 | .1114 / .1152 / .0691 | 2.170 / 2.046 / 2.099 | .0494 / .0542 / .0322 |
  | 2023 | .0349 / .0354 / .0211 | .824 / .820 / .820 | .0430 / .0439 / .0260 |

**Paired tests among the three** (BU; studentized CBB, block 21, seed 20260929, 4,999 resamples):

| pair | dSR | Memmel SE | p one-sided / two-sided | source |
|---|---|---|---|---|
| X-10 vs Y-F0 | -.0412 | .0162 | .9984 / .0048 | `p9-r10-x10/bundle.json`; L10 7574-7579 |
| Y-1 vs X-10 | +.0215 | .0518 | .2980 / .6326 | `bundle-x10.json`; L11 7831-7835 |
| Y-1 vs Y-F0 | -.0197 | .0519 | .6652 / .6602 | `bundle-yf0.json`; L11 7835-7836 |

## 2. Registered verdicts, one row per cell (order run)

Every cell passed its mechanics 6/6. No cell was undefined or void. Each cell counts 1 in N_c and enters V, whatever its
verdict.

| step | cell | rule (decides) | verdict | net Sharpe vs reference | dSR (Memmel SE); p one-sided / two-sided | printed-only items | ledger line / trial | source |
|---|---|---|---|---|---|---|---|---|
| R0-6 | **Y-S**: the 15 Y strings, one add-alpha wave on X-F0 -> library v8ysb at L 1.1828 | PM7-34: dSR > 0 AND mechanics | **ACCEPTED** (driver) | 1.8495 vs X-F0 1.7695 | +.0800 (.1502); .3122 / .6262 | 4x not lower: **met** (1.6994 vs 1.6549); turnover per gross not higher: **unmet**; cost bps lower: **unmet** | 128 / `11c10defb3cf38a5` | R6; WR `57e9f5ea` |
| R0-7 | **Y-3** norm-score-v1 (L 1.1645 matched) | PM7-34 | **NOT ACCEPTED: rejected, counted** | 1.8119 vs 1.8495 | -.0376 (.0401); .8644 / .2690 | 4x: **unmet** (1.6874); turnover: **unmet** (.029742 vs .028528); cost: **unmet** (12.708 vs 12.658). Hypothesis print YP-7, gross return per unit gross: .07395 vs .07167, above the parent's | 129 / `ee5487109c705108` | R7; WR `619809ea` |
| R0-8 | **Y-2** theme-tsmom-v1 (L 1.2038 matched) | PM7-34 | **NOT ACCEPTED: rejected, counted** | 1.5182 vs 1.8495 | -.3313 (.2307 over 1,005 sessions; the rule acts 2021-01-05 .. 2023-12-06); .8794 / .3976 | 4x: **unmet** (1.3498); turnover: **unmet** (.030641); cost: **met** (12.614 vs 12.658). CM-2: the sleeves use the parent's full-TRAIN weights, and out of sample the rule is a frozen mask; the registration stands (PM8-12 (e)) | 130 / `aeeb2073e0bd8e2a` | R8; WR `09fbd24e` |
| R0-9 | **Y-5** two-speed-v1 (L 1.2658 matched; P13 passed first) | PM7-34 | **NOT ACCEPTED: rejected, counted** | 1.7294 vs 1.8495 | -.1201 (.0753); .9316 / .1398 | 4x: **unmet** (1.6023); turnover: **met** (.026441 vs .028528); cost: **met** (12.648 vs 12.658); fast mass share .120861 | 131 / `cc150c210a3f98e4` | R9; WR `ec4e07c2` |
| R0-10 | **X-10**: L 2.0 on Y-F0, by hand (YP-7) | PM7-34 (3): net annual above Y-F0's AND net Sharpe not lower by > .100 AND scaled mechanics | **ACCEPTED** by hand. **Re-derived independently at R0-12: match** | 1.8083 vs 1.8495 (floor 1.7495); net 9.306% > 5.654% | -.0412 (.0162); .9984 / .0048 | 4x guard: 1.5829 vs 1.6994, lower by .1165 | 132 / `25f3b27aae4754ab` | R10; R12 Part A; BK10 |
| R0-11 | **Y-1**: vol-target-v1 at cap 2.0 on Y-F0, child of X-10, by hand | v8y §6 / §8: dSR > 0 vs X-10 AND X-10's rule vs Y-F0 AND mechanics | **ACCEPTED** by hand. **Re-derived independently at R0-12: match** | 1.8298 vs X-10 1.8083 (and vs Y-F0 1.8495, floor 1.7495); net 8.799% > 5.654% | vs X-10: +.0215 (.0518); .2980 / .6326 | dSR vs Y-F0 -.0197; 4x 1.6281 | 133 / `0ad7ea4c5122ebe3` | R11; R12 Part A; BK11 |

- **Scorecard:** of the four Y signal / rule cells, one is accepted (Y-S) and three are rejected and counted (Y-3, Y-2,
  Y-5). Both leverage cells are accepted under their own rules.
- **Y-1 replaces X-10 as Y-F** (v8y §1). Neither leverage cell is in the Sharpe claim.
- **R0-12's re-derivation:** it compared the verdicts and every deciding number, and both match. It disclosed one
  anchoring: before deriving, it saw truncated commit subjects and "X-10 is accepted" (R12 Part A).
- **Each cell's DSR at its own N** (printed at its judge; decides nothing): Y-S .8468 (N 57), Y-3 .8221 (58), Y-2 .6457
  (59), Y-5 .7804 (60), X-10 .8184 (61), Y-1 .8251 (62).
- **Each cell's PBO on its own summ grid:** .0766, .0765, .1511, .1509, .1566, .1526 (R6 .. R11).
- **The rejected cells' reports read their own outcomes:**
  - Y-3 earned more gross per unit gross, as hypothesised, but its vol, turnover and cost took more than that (R7).
  - Y-5 cut turnover per unit gross by 7%, but it needed L 1.2658 to match gross, and it lost more gross return than it
    saved in cost (R9).
  - Y-2's SE is large: the rule acts only from 2021 (R8).

## 3. The adoption print (R0-12; v8y §7 as PM7-34 (2) amended it)

### 3.1 Rule and gate

**Rule: Y-F0 is deployable iff S2 net Sharpe >= 1.0 AND mechanics.**
- S2 net Sharpe 1.849482 >= 1.0.
- Mechanics 6/6: gross .986213, |net| .005340, tau .028134 / p95 .033410, accounting 3.68e-16 / 1.11e-13.

The rule is **met. Y-F0 (`scripts/specs/v8/lib-v8ysb-gm.json`, library v8ysb, L 1.1828) is the deployable book, in R-2's
place.** Sources: R12 §1; L12 7942-7950; `p9-r12-adopt/mech.json` `c397ac3f`.

**G-B1** (plan §0.3), S2 net Sharpe of the deployable unlevered book: **1.849482**.
- Floor 1.0: **met**.
- P9 target 1.85: **not met**. It is short by .000518, a hair.

Sources: R12 §2; L12 7952-7955.

### 3.2 Bundles into Y-F0 (BU; L12 7959-7963)

| base -> Y-F0 | dSR | SR Y-F0 / base | Memmel SE | CBB 95% | p one-sided | p two-sided | tool's freeze-gate line (v8-prereg 9; gates nothing) |
|---|---|---|---|---|---|---|---|
| R-2 (V8-F) | +.5935 | 1.8495 / 1.2559 | .2258 | [+.132, +1.088] | .0030 | .0308 | PASS |
| X-F0 (X-5) | +.0800 | 1.8495 / 1.7695 | .1502 | [-.236, +.407] | .3122 | .6262 | **FAIL** (p not < .10) |
| B0c | +.7167 | 1.8495 / 1.1328 | .2548 | [+.233, +1.189] | .0024 | .0050 | PASS |

**Per-year dSR of Y-F0** (R12 §8; L12 8005-8010). Y-F0 trails X-F0 in 2021, 2022 and 2023; its whole gain over X-F0 comes
from 2020.

| year | vs R-2 | vs X-F0 | vs B0c | Y-F0 net / Sharpe |
|---|---|---|---|---|
| 2020 | +1.651 | +.857 | +1.016 | +.0431 / 1.335 |
| 2021 | +.388 | -.298 | +.632 | +.0983 / 2.984 |
| 2022 | +.259 | -.072 | +.163 | +.0691 / 2.099 |
| 2023 | +.182 | -.107 | +.702 | +.0211 / .820 |

### 3.3 Deflated Sharpe (DSR12; R12 §4; L12 7965-7976)

V[SR] is 1.2957e-03 per session, from 33 research-window-v2 cells. Y-F0: SR 1.849 annual, T 1,005, skew -.376, kurtosis
5.636.

| print | book | N | DSR | SR0 (annual) |
|---|---|---|---|---|
| DSR_tot | Y-F0 | N_tot 233 | **.6798** | 1.609 |
| DSR_hand | Y-F0 | N_hand 123 (X-9 undefined, not in the lineage) | **.7589** | 1.487 |
| DSR_v8 | Y-F0 | N_c 62 | **.8350** | 1.347 |
| beside: X-F0 at its own state (prefix rule) | X-F0 | 88 | .7741 | 1.389 |
| beside: V8-F at its own state (prefix rule) | R-2 | 49 (V from 5 cells) | .9857 | .128 |
| beside: V8-F at the print's N and V | R-2 | 233 / 62 | .2468 / .4297 | 1.609 / 1.347 |
| beside: V8-F public freeze print (v8 log line 5250) | R-2 | 50 (V from 21 cells) | .4648 | 1.301 |

### 3.4 "DSR up": three readings that disagree in sign

Ruling P9L 185: all three readings are printed, labelled, and no single "up / not up" claim is made. v8y §4 / §7 does not
fix which V8-F reference is meant.

| reading of "DSR_tot(V8-F) at V8-F's ledger state" | V8-F | Y-F0 DSR_tot | Y-F0 vs it |
|---|---|---|---|
| (1) the tool's prefix rule (`--dsr-hand`, N_tot 49, V from 5 cells) | .9857 | .6798 | below |
| (2) the public freeze value v8y §4 cites ("public .465 at N 50") | .4648 | .6798 | above |
| (3) V8-F at the print's own N and V (N_tot 233) | .2468 | .6798 | above |

### 3.5 PBO: two values, no grid named

Ruling P9L 186: both values are printed, labelled. No new PBO run was made.

| grid | PBO | file |
|---|---|---|
| (i) the ledger's whole grid at the print: 70 candidates, 16 blocks, 12,870 splits | .1526 | `cycle-v8y-vol-target-y-1/pbo.json` `2beb256c` |
| (ii) Y-F0's own cycle (Y-S judge): 65 candidates | .0766 | `cycle-v8ysb-gm/pbo.json` `556192c0` |

### 3.6 Claims (v8x §7 with Y-F0 for X-F0; values only; R12 §10, L12 8020-8038)

- **Sharpe up** holds "only under (a)-(c)":
  - (a) dSR vs V8-F +.5935, one-sided p .0030: met.
  - (b) DSR_tot >= .95: **unmet** (.6798).
  - (c) mechanics: met.

  So **the "Sharpe up" claim is not made**. PM7-34 (2) replaced this gate as the adoption rule.
- **DSR up:** section 3.4. Three readings, no claim.
- **Capacity up** (sign level): Y-F0 4x 1.6994 vs R-2 1.1785, positive.
- **Return up, Y-F0 vs R-2** at matched gross (.98621 vs .98599): 5.654% vs 4.536%.
- **Return up, Y-F (Y-1) vs Y-F0:** 8.799% vs 5.654%. This is leverage, the owner's risk decision, and is never called
  alpha.
- **PM7-37 / PM7-38 lines, kept:**
  - X-5's +.349 is in sample until OD-3.
  - The review's decomposition: lower volatility +.311, gross alpha +.080, trade cost -.042, borrow +.002.
  - "Under the rule's own equal-Sharpe premise the expected gain is about +.15."

**Appendix A**, verbatim from L12 8012-8015:

`TRAIN construction cells 62; admission trials this sprint 61 (v8 12, X hand-written 25, mined 0, Y hand-written 15; X-4
re-screens 9, a part the Y form has no slot for); mined campaigns 1 (v9-mine-c1: budget 110, registry count 110,
admitted 0); N_tot 233; N_hand 123; window research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation
reads before v8: 2 (2023-2024); history reads 0; 2025+ never read.`

## 4. The leverage decision (the owner's; OD-P9-3)

Which book is deployed is the owner's risk decision (PM7-3, v8y §8). **OD-P9-3 keeps P9 at L <= 2.0 until the owner
rules** (P9L 18). The return hurdle is not stated: OD-P9-1 is open, and the PM uses the plan's 8.0% net / yr placeholder
until the owner states one (P9L 14).

The options:
- **(A) Y-F0, unlevered:** L 1.1828.
- **(B) X-10:** fixed L 2.0.
- **(C) Y-1:** vol-target, L_t = clip(2.0 x sigma_ref / sigma_hat, 1, 2.0), re-estimated every 21 sessions.

The table compares each with Y-F0 by plain arithmetic on the section 1 numbers.

| | (A) Y-F0 | (B) X-10 | (C) Y-1 |
|---|---|---|---|
| net annual | 5.654% | 9.306% (+3.652 pp) | 8.799% (+3.145 pp) |
| net Sharpe 1x | 1.8495 | 1.8083 (-.0412; paired p two-sided .0048) | 1.8298 (-.0197; p two-sided .6602) |
| net Sharpe 4x NAV | 1.6994 | 1.5829 (-.1165) | 1.6281 (-.0714) |
| realised vol | 3.057% | 5.146% (x1.683) | 4.809% (x1.573) |
| max drawdown | 2.580% | 4.326% (x1.677) | 3.602% (x1.396) |
| mean gross | .98621 | 1.66672 (x1.690) | 1.55566 (x1.577) |
| cost bps / traded $ | 12.66 | 14.51 (+1.85) | 14.03 (+1.37) |
| capped share at 4x | .1004 | .2047 | .1882 |
| vs the 8.0% placeholder hurdle (print only; the owner's number is not stated) | below | above | above |
| registered status | deployable (adoption rule met); the claims book | ACCEPTED (PM7-34 (3)); not Y-F, because Y-1 replaced it | ACCEPTED (v8y §6 / §8); **= Y-F**, the levered book offered |
| history read | OD-3 reads Y-F0 at the P9 freeze, beside P9-F0 (YP-11, DEC-3) | none registered (OD-3 is on Y-F0) | **none, ever:** Y-1 reads a risk model (`--risk-model build-equity/v8-risk-lo3-v10`, manifest `862515d9`), and the OD-3 runbook stops on that (YP-11) |

**(C) against (B).** Y-1 gives up .507 pp of net return. In exchange it has:
- vol .338 pp lower;
- max drawdown .724 pp lower (x.833);
- 4x Sharpe .0452 higher;
- cost .48 bps lower.

Its deciding test against X-10 is dSR +.0215 with SE .0518 (t .41, p one-sided .2980). That passes the registered "> 0";
its CBB 95% interval [-.0580, +.1061] contains 0 (L11 7833-7834).

**What each costs and buys** (from the table; no new claim):
- **(A)** keeps the highest 1x and 4x Sharpe, with the lowest vol and drawdown, at about 5.7% net.
- **(B)** buys the most return. It pays the largest Sharpe loss, which its paired test separates from 0, and the largest
  vol and drawdown.
- **(C)** buys most of (B)'s return (+3.145 of +3.652 pp) for less vol and drawdown than (B). It reads a risk model, so
  it gets no history read (YP-11).
- None of them is tested out of sample.

## 5. Ledger state: P9's `n_before`

Counted at R0-14 from T. The file has 133 lines, sha256 `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554`;
its chain head (the sha256 of the last line) is `5a3ef9d9bf242dbb`, and its last trial is `0ad7ea4c5122ebe3`. A
read-only scratch script ran `backtest_integrity.trial_counts` / `ledger_n` over it and printed aggregates only. Its
totals equal DSR12's printed parts.

| count | value | how it is counted | source | plan §5.1 projection | actual vs projection |
|---|---|---|---|---|---|
| **N_c** (construction cells) | **62** | construction lines that add a trial under `trial_counts`: 70 construction lines, each count 1, less 8 window re-runs (lines 64-71 per R11, `rerun_basis` window) = 62; `ledger_n` 62 | T; R11; DSR12 (DSR_v8 N_c 62; Appendix A "TRAIN construction cells 62") | <= 62 (56 + 6 Y cells) | **at the ceiling**; the v8 cap 62 was reached at Y-1 |
| **K_a**, admission trials, hand-written (plan §5.1 row) | **40** = X 25 (v8x2 5 + v8x3 8 + v8x7 12) + Y 15 (v8ys) | admission lines by `cycle`, each count 1 | T; R12 §9 (Y form) | 40 (X 25 + Y 15) | **equal** |
| K_a, all admission trials in the ledger | 61 = v8 12 (v80 7 + v81 5) + X hand-written 25 + X-4 re-screens 9 (v8x4) + Y 15; mined 0 | every admission line adds 1 | T; DSR12 "admission 61" | v8's 12 and the 9 re-screens counted apart | - |
| **M** (campaign evaluations) | **110** | v9-mine-c1: ledger line 112, kind mining-campaign, budget 110, adds 0 under `trial_counts`; registry count 110 | T line 112; DSR12 "campaign registry 110" | 110 | **equal** |
| **N_tot** = sum(trial_counts) + M | **233** = 123 + 110 | 61 admission + 62 construction. These add 0: 1 protocol line, 8 window re-runs, 1 campaign line | T; DSR12 (DSR_tot N_tot 233) | <= 233 (212 + 21) | **at the ceiling** |
| N_hand (beside) | 123 | N_tot - M - mined-wave lines (0) | R12 §4 | - | - |
| V (beside) | 1.2957e-03 per session | ddof-1 variance over 33 research-window-v2 construction lines | DSR12 | - | - |

P9's budget on top of this (plan §5.2): N_c <= 69 (+7); K_a <= 10; M + 0; N_tot <= 250. That is 233 + 7 + 10.

## 6. Limits of these numbers

1. **All in sample.**
   - Every number is TRAIN 2020-2023 (research-window-v2), S2 modelled costs ($1bn, stale 5, swap financing).
   - The books were chosen on the same window they are scored on.
   - 2023 was read twice at book level before v8 (v8y §2).
   - Hidden 2024+ is unread; 2025+ was never read.
2. **No history read.**
   - OD-3 has not been done (R0-13, P9L 188; DEC-3 / OD-P9-2 defer it to the P9 freeze, on P9-F0 with Y-F0 beside).
   - Y-1 will never get one (YP-11).
3. **Statistical weight.**
   - Y-S's gain over X-F0 is +.0800 with SE .1502 (p one-sided .3122). Per year, Y-F0 trails X-F0 in 2021-2023.
   - Y-1's edge over X-10 is +.0215 with SE .0518 (p .2980).
   - Of the three leverage-pair tests (section 1), only X-10's Sharpe loss against Y-F0 has p two-sided < .05 (.0048).
   - The review's out-of-sample expectation for X-5's gain is about +.15 against +.349 in sample (PM7-37 / PM7-38).
4. **Two print items have more than one value, by ruling.**
   - "DSR up" has three readings that disagree in sign (section 3.4).
   - PBO has two values (section 3.5).

   All are printed, and nothing beyond them is claimed.

   Other numbers that look different but are measured on different bases (no source contradicts another on the same
   quantity at the printed precision):
   - Y-F0's DSR is .8468 at N 57 (Y-S judge, R6) and .8350 at N_c 62 (DSR_v8, R12).
   - Y-1's priced_share mean is .998634 over all 1,004 rows (L11) and .998641 over the 48 estimate rows (R12 Part A).

   One labelling difference:
   - The 1,005 S2 return sessions are dated 2020-01-03 .. 2023-12-29 in R8 and in the ledger window (Appendix A).
   - R12's print heading says 2020-01-02 .. 2023-12-29. The S2 CSV has 1,006 rows from 2020-01-02 and 1,005 return rows
     (L10 7548), so the first return session is 2020-01-03. The heading counts the first CSV row.
5. **G-B1:** the floor 1.0 is met. The P9 target 1.85 is missed by .000518.
6. **Printed-only criteria unmet:**
   - Y-S's turnover and cost criteria.
   - All three of Y-3's.
   - Y-2's 4x and turnover.
   - Y-5's 4x.
   - X-10's 4x guard (-.1165).
   - The "Sharpe up" condition (b).
   - The X-F0 -> Y-F0 bundle's freeze-gate line (FAIL).

   They decide nothing under PM7-34, and they are reported here as plainly as the passes.
7. **Costs are modelled.** No borrow-fee or utilisation data has been bought (OD-P9-4 / OD-P9-5). The plan puts the
   realism gap at up to ~.1 of net Sharpe (plan §6, OD-P9-4).

## 7. Phase-0 exit gate (plan §4.3, phase 0)

| clause | evidence | met |
|---|---|---|
| Y cells each ledgered, undefined or void by rule | All 6 are ledgered, count 1 each, 0 undefined, 0 void: Y-S 128 `11c10defb3cf38a5`, Y-3 129 `ee5487109c705108`, Y-2 130 `aeeb2073e0bd8e2a`, Y-5 131 `cc150c210a3f98e4`, X-10 132 `25f3b27aae4754ab`, Y-1 133 `0ad7ea4c5122ebe3` (T, re-read at R0-14; R6 .. R11) | **yes** |
| adoption print done | R0-12: commits `5292b46e` and `32b33236`; L12 7874-8045; R12 | **yes** |
| v8 report written | this file (status 8) | **yes** |
| G-B1 printed for Y-F0 | 1.849482: floor met, target 1.85 not met (R12 §2; L12 7952-7955; section 3.1 here) | **yes** |
| ledger state recorded as P9's `n_before` | section 5 here; `root-R0-14-report.md` | **yes** |

**Phase 0 exit gate: met.**

## 8. Wave status and the P9 branch

- **`wave status`, run read-only at R0-14** (`research_cycle.py wave status scripts/specs/v8/waves/<w>.json --root
  C:/atx-wt/pool-2`):
  - y-s, y-3, y-2 and y-5: 9/9 stages done, exit 0.
  - `y-s.head.json` is refused, exit 2 ("a wave has exactly one of candidates ... and rule_cell"). It is not a wave: it
    is the Y-S manifest minus `candidates` (R0-2, v8 integration log line 6677), and its wave y-s is complete.
  - X-10 and Y-1 ran by hand and have no wave manifest (YP-7).

  So no Y wave is in flight. E1-STALE's condition for merging E1 holds (P9L 141).
- **Branches:**
  - `feat/platform-p9-20261003` is cut in `C:/atx-wt/pool-2` from the commit that adds this file. Its SHA is recorded in
    `root-R0-14-report.md` on the P9 branch.
  - `feat/platform-v8-20260929` is kept, not moved.

## 9. Owner decisions outstanding

1. **OD-P9-3, production leverage:** deploy Y-F0, X-10 or Y-1 (section 4); and whether L > 2.0 is allowed (today the
   exe refuses it). Until you rule, P9 keeps L <= 2.0, and P9-X (plan §5.2) waits for the chosen form (X-10 or Y-1).
2. **OD-P9-1, the return hurdle:** one net annual number for the levered book at S2. G-B2 prints against 8.0% until
   then.
3. Carried in plan §6: data asks OD-P9-4 / -5 (option strikes or securities lending) and OD-P9-6 (atx-db asks); this
   run cannot execute them (P9L 16). VWAP (OD-P9-7) and a mined campaign (OD-P9-9) were declined by ruling (P9L 17).

## 10. Next (root, on `feat/platform-p9-20261003`)

1. Wave-1 merges per `root-wave1-merge-brief.md`.
2. Then the §4.2 suites, the canary goldens (Debug + Release), Release IC adoption, G-P4, G-P8 and P9-B0.
3. The PM rules `p9-prereg.md` (plan §5.2, DEC-18) on this `n_before` before any P9 read.
