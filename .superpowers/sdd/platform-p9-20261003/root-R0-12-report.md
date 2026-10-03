# Root R0-12 report: re-derivation of X-10 and Y-1, then the v8 adoption print (v8y §7)

Root `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, HEAD at start `671d61d2`. Date 2026-10-03. Rulings
applied: SEAL-ALLOW (P9 `progress.md:108`), R0-6-ATT (not needed), Y1-PARENT, the two R0-12 rulings
(`progress.md:172`, `:173`).

## Part A: independent re-derivation (written and committed before the logged verdicts were opened)

### Order and what was seen first

- Read first: plan lines 80-185; P9 ledger lines 102, 107, 108, 115, 158, 171-174; `root-R0-6-report.md`;
  v8y `v8y-prereg.md` §1-§8 (sha256 `5a57f60b6b0b3bb8724ab7ba8283b7ef0c654be84b880040305ad2c239463e73`, last commit
  `87a9e0f4`); PM7-34 in v8 `progress.md:2056-2072`; the two threshold commits `50406181` (X-10) and `d1f2d7f2` (Y-1).
- Disclosure (anchoring). Three things showed verdict text before this derivation:
  1. The opening `git log --oneline -8` printed the subjects of `e6418b0d` and `0829f784`, truncated: "PM7-34 (3) by hand:
     ACCEPTED (net 9.31% > 5.65%, SR 1.8083 vs ..." and "registered rule by hand: ACCEPTED (dSR vs X-10 +.0215, net
     8.8...".
  2. The diff of `d1f2d7f2`, which the dispatch names as the source of Y-1's thresholds, says "X-10 is accepted".
  3. Its 3-line diff context shows the end of the X-10 verdict row: "sharpe_within_.100: True (1.808289 >= 1.749482;
     dSR -.041193), mechanics: True (".
- Not opened before this section was committed:
  - the R0-10 / R0-11 verdict rows of the integration log;
  - the verdict text of the R0-10 / R0-11 ledger lines;
  - `root-R0-10-report.md` and `root-R0-11-report.md`.
- The earlier agents' scratch scripts are not in the repository. Every number below was read again from the on-disk
  files. Two throwaway scripts in this agent's scratch dir did the reading (not committed):
  - `recheck.py` imports the committed `atx-impl/tools/nav_summ.py`. For each NAV dir it calls `load_summary`,
    `scenario_of`, `load_daily` (seal-checked) and `construction_stats`.
  - `vtcheck.py` reads `vol_target.csv` by its key columns only; the file has no return column.
- The re-read numbers equal the reader files (`mech.json`, `book.json`) and the `summ.json` rows bit for bit.

### Registered rules applied

- **X-10:** PM7-34 (3) (v8 `progress.md:2063-2065`), restated in v8y §8 X-10 bullet and the v8y §6 X-10 row. It is
  accepted only if all three parts hold:
  - S2 net annual return at 1x is above Y-F0's;
  - S2 net Sharpe is not lower than Y-F0's by more than .100;
  - mechanics pass. Mechanics are the v8-mech limits x r, r = 2.0 / L_P = 2.0 / 1.1828 = 1.6909029421711192. The
    accounting rows are unchanged. The limits were committed before the run in `50406181`.
- **Y-1:** v8y §6 Y-1 row and §8 Y-1 bullet. It is accepted only if all three parts hold:
  - paired S2 net dSR against X-10 is > 0;
  - X-10's rule holds against Y-F0 (net annual return higher AND S2 net Sharpe not lower by more than .100);
  - mechanics pass:
    - all-rows gross inside [1.0, 2.0] x G_P / L_P, widened by .005;
    - |net|, tau mean and tau p95 at X-10's scaled limits (YP-9);
    - accounting rows unchanged;
    - the `vol_target.csv` key checks before any return.
  - The limits were committed before the run in `d1f2d7f2`.
- **Y-F0 values the rules compare against:**
  - net annual .05653814276222473 and S2 net Sharpe 1.849482026329859 (Sharpe floor 1.749482026329859);
  - G_P .9862134133265011, L_P 1.1828;
  - Y-1 band [0.8287955811012014, 1.6725911622024028], recomputed here.

### Files (sha256)

| file | sha256 |
|---|---|
| Y-F0 `build-equity/mega-nav-v8x-theme-erc-L1.1828-v8ysb/summary.json` | `2d30b7f6466cbcb3e5bf0f4758ce5570e853d26b15b44eba93814151e21aa525` |
| Y-F0 S2 daily `.../daily_modeled-1bn-stale5-v1+swap-fin-v1.csv` | `73b69bcceb122b7c21bb3f75619058c51b8104ebd8a694bbbb1b12db14b401b0` |
| X-10 `build-equity/mega-nav-v8x-theme-erc-L2.0-v8ysb/summary.json` | `1bc1b6ca4ea3239f5cfe9c41001f2cf0180c6c96ae691a9ba6d5fdd1919e7fa3` |
| X-10 S2 daily `.../daily_modeled-1bn-stale5-v1+swap-fin-v1.csv` | `e783eb740e9ef9d49919c6c24f92967c55fb87c7d8a4b42c9af180f04682f7b1` |
| X-10 `build-equity/cycle-v8x-leverage-L2.0/summ.json` (row 68; Y-F0 row 64) | `4673c632694b88dc1e036301a0155ad71d4a29c7f0fb3ccf555c1efcc3616152` |
| X-10 `build-equity/p9-r10-x10/mech.json` | `603b5afbcc05a25e1546cff705075c3835b27ca782bb49371c62052c2974a87b` |
| X-10 `build-equity/p9-r10-x10/book.json` | `d222b54716af870b7ef7cd41dcda5bf865bc082f6819b9bef5c2bec2b01d3de4` |
| X-10 `build-equity/p9-r10-x10/bundle.json` (Y-F0 -> X-10) | `a57d82d017767054b250e3b3c1749e29cf82f1af4d9efa01942aa04b0153f4fb` |
| Y-1 `build-equity/mega-nav-v8y-vol-target-L2.0/summary.json` | `876d107bf0a3bae27837fd0db787bae07ac2acb0ba3e388f50e85c9db0c9647e` |
| Y-1 S2 daily `.../daily_modeled-1bn-stale5-v1+swap-fin-v1.csv` | `d6d145ef5a97bd77fa6252bbe9efdf6b24b582879e2bd14705cf06c6b23a2abe` |
| Y-1 `.../vol_target.csv` (sessions 2020-01-02 .. 2023-12-27) | `100286a5d1c48fd14bb9a6728a08be68fe5d7d4830bafa19383455053326664d` |
| Y-1 `build-equity/cycle-v8y-vol-target-y-1/summ.json` (row 69) | `78aea78d2544d9e08f61827a29785dcac2865cd7b69c2f3de2228c4ba980a77f` |
| Y-1 `build-equity/p9-r11-y1/mech.json` | `99eb2484efcd753938f576cc633d020795d349837d4c621f537f3dbbf1e11468` |
| Y-1 `build-equity/p9-r11-y1/book.json` | `b85952b024a351b217b81d2d5cf87f8a86d15572114372674bcd5549d5917f2e` |
| Y-1 `build-equity/p9-r11-y1/bundle-x10.json` (X-10 -> Y-1) | `241c5bd9248499dd9bb15623b03995209256f669b438724c7116d338c755b9c4` |
| Y-1 `build-equity/p9-r11-y1/voltarget-identity.xml` (17 tests, 0 failures) | `cd9401955f246ef77acbb567c7b161f0e99570b63d62fc3c57db6dabcea02b52` |
| trial ledger `build-equity/trials.jsonl` (133 lines; X-10 line 132 `25f3b27aae4754ab`, Y-1 line 133 `0ad7ea4c5122ebe3`) | `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554` |

All 11 receipts read show outcome completed and exit 0. Each is listed as receipt file, sha256 prefix:

- X-10:
  - NAV `mega-nav-v8x-theme-erc-L2.0-v8ysb-run/receipt.json`, `b14ac066`;
  - mech `p9-r10-x10/mech-run1`, `2d933c88`;
  - summ `cycle-v8x-leverage-L2.0/summ-run1`, `605b710f`;
  - bundle `p9-r10-x10/bundle-run1`, `63eae3e7`;
  - book `p9-r10-x10/book-run1`, `28bf0d07`.
- Y-1:
  - NAV `mega-nav-v8y-vol-target-L2.0-run/receipt.json`, `cefce32d`;
  - mech `p9-r11-y1/mech-run1`, `461fb484`;
  - summ `cycle-v8y-vol-target-y-1/summ-run1`, `6c3810ef`;
  - bundle-x10 `98e17ee5`, bundle-yf0 `1eecc772`, book `cc074535`.

### X-10: my verdict **ACCEPTED** (PM7-34 (3))

| part | number (mine) | limit | holds | source file |
|---|---|---|---|---|
| net annual (S2, 1x; summary `ann_mean` = book `net_annual`) | .09306060940646439 | > .05653814276222473 (+.036522) | yes | X-10 `summary.json` `1bc1b6ca`; book.json `d222b547` |
| S2 net Sharpe | 1.8082892420018353 | >= 1.749482026329859 (dSR vs Y-F0 -.041193) | yes | X-10 `summary.json`; summ.json row 68 `4673c632`; ledger line 132 |
| mean gross leverage, all rows | 1.6667175942698484 | [1.5218126479540073, 1.7754480892796753] | yes | X-10 S2 daily `e783eb74` (via `construction_stats`); mech.json `603b5afb` |
| abs(mean net leverage), all rows | .01044612970055375 | <= .033818058843422386 | yes | same |
| tau (gmv) mean | .0281886016086664 | <= .3381805884342239 | yes | same |
| tau (gmv) p95 | .033162962931100985 | <= .5072708826513358 | yes | same |
| max return identity error / max cash book relative error | 3.391384395534658e-16 / 4.7994000936258844e-14 | <= 1e-09 (summary tolerance) | yes | X-10 `summary.json` accounting_checks |

Printed only:
- paired dSR vs Y-F0 -.04119278432802487;
- Memmel SE .016182620429210866;
- CBB 95% [-.068033, -.013668];
- p two-sided .0048, one-sided .9984 (bundle.json `a57d82d0`);
- 4x net Sharpe 1.5829085298985293 vs Y-F0 1.6994264697927697 (book.json).

### Y-1: my verdict **ACCEPTED** (v8y §6 / §8)

| part | number (mine) | limit | holds | source file |
|---|---|---|---|---|
| paired S2 net dSR vs X-10 | +.021480565906708105 (SR 1.8297698079085434 vs 1.8082892420018353) | > 0 | yes | summ.json row 69 `78aea78d` (paired, reference X-10); bundle-x10.json `241c5bd9` |
| net annual (S2, 1x) | .08798970720884516 | > .05653814276222473 (+.031452) | yes | Y-1 `summary.json` `876d107b`; book.json `b85952b0` |
| S2 net Sharpe | 1.8297698079085447 | >= 1.749482026329859 (dSR vs Y-F0 -.019712) | yes | Y-1 `summary.json`; summ.json row 69; ledger line 133 |
| mean gross leverage, all rows | 1.5556647047690102 | [0.8287955811012014, 1.6725911622024028] | yes | Y-1 S2 daily `d6d145ef` (via `construction_stats`); mech.json `99eb2484` |
| abs(mean net leverage) | .00966843060025726 | <= .033818058843422386 | yes | same |
| tau mean / p95 | .02853644896237781 / .03494294094961286 | <= .3381805884342239 / .5072708826513358 | yes | same |
| accounting | 4.198030811863873e-16 / 3.784294207970233e-14 | <= 1e-09 | yes | Y-1 `summary.json` |

`vol_target.csv` key checks, book `modeled-1bn-stale5-v1+swap-fin-v1` (`vtcheck.py`; `100286a5`). The other four books
give the same pattern.

| check | value |
|---|---|
| (a) estimates vs scored decisions / 21 | 48 estimates vs 1,004 / 21 = 47.81; every gap between estimates is exactly 21 decisions (min 21, max 21) |
| (b) decisions before the first estimate (NaN sigma_hat) | 0. The first scored estimate is at decision row 4. The running-mean identity gives an implied count of 5 at the next estimate, so 3 estimates fall in the unscored warm-up before 2020-01-02 |
| running-mean identity | implied counts step by exactly 1 (5, 6, ... 51) |
| (c) L_t range | n 1,004, mean 1.8671322726, min 1.0, max 2.0. Rows at the cap 2.0: 584; at the floor 1: 21; between: 399. Estimates: 28 at the cap, 1 at the floor, 19 between |
| L_t vs the rule | L_t = clip(2 sigma_ref / sigma_hat, 1, 2) on every row; max error 0; raw = 2 sigma_ref / sigma_hat, error 0 |
| (d) priced_share on estimates | mean .998641, min .995193, max 1.0. The plan's stop threshold (`d1f2d7f2`) is mean >= .9956 |
| identity before return | `voltarget-identity.xml` (VolTarget / RiskTarget / BookVolTarget): 17 tests, 0 failures |

None of these contradicts the registered rule's definition.

Printed only:
- dSR vs Y-F0 -.0197122;
- bundle X-10 -> Y-1: p two-sided .6326, one-sided .2980. The bundle's own `verdict.pass` false is the freeze-gate rule
  (v8-prereg 9), not Y-1's rule;
- 4x net Sharpe 1.6280661083698378.

### Comparison with the logged verdicts

(filled in after this section was committed; see below)
