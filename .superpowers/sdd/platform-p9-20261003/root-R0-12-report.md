# Root R0-12 report: re-derivation of X-10 and Y-1, then the v8 adoption print (v8y §7)

Root `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, HEAD at start `671d61d2`. Date 2026-10-03. Rulings
applied: SEAL-ALLOW (P9 `progress.md:108`), R0-6-ATT (not needed), Y1-PARENT, the two R0-12 rulings
(`progress.md:172`, `:173`).

**Status: DONE. Three items are open for the PM's ruling (end of Part B).**
- **Part A:** X-10 and Y-1 both **match** the logged verdicts.
- **Part B:**
  - Y-F0 is **deployable**: S2 net Sharpe 1.849482 >= 1.0 AND mechanics PASS.
  - **G-B1 is met**: 1.849482 >= 1.0. It is below the P9 target 1.85.
- 0 trials. The ledger stays 133 lines, `27e40f9f`.
- Commits:
  - `5292b46e`: Part A, committed before the logged verdicts were opened;
  - the next commit: this report in full, the v8 integration log's R0-12 section, and the P9 `progress.md` with the
    PM's uncommitted lines as found, not edited.

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

### Comparison with the logged verdicts (opened after commit `5292b46e`)

Logged sources:
- v8 integration log lines 7536-7546 and 7605-7606 (X-10); lines 7764-7800 and 7863-7865 (Y-1);
- P9 `progress.md:156` and `:167`;
- `root-R0-10-report.md:89-90`; `root-R0-11-report.md:279-281`.

| cell | rule text | my verdict | deciding number (mine) | file (sha256 prefix) | logged value | match |
|---|---|---|---|---|---|---|
| X-10 | PM7-34 (3), v8 `progress.md:2063-2065`; v8y §8 / §6 X-10 row | ACCEPTED | net annual .09306060940646439 | X-10 `summary.json` `1bc1b6ca`, `book.json` `d222b547` | .093061 (log 7605); 9.306% (P9 ledger 156) | yes |
| X-10 | " | " | Y-F0 net annual .05653814276222473 | Y-F0 `summary.json` `2d30b7f6` | .056538 | yes |
| X-10 | " | " | S2 net Sharpe 1.8082892420018353 vs floor 1.749482026329859 | X-10 `summary.json`; `summ.json` `4673c632` row 68; ledger line 132 | 1.808289 >= 1.749482 | yes |
| X-10 | " | " | dSR vs Y-F0 -.04119278432802487 | `summ.json` row 68 paired; `bundle.json` `a57d82d0` | -.041193 | yes |
| X-10 | " | " | gross 1.6667175942698484 in [1.5218126479540073, 1.7754480892796753] | S2 daily `e783eb74`; `mech.json` `603b5afb` | 1.666718 in [1.521813, 1.775448] | yes |
| X-10 | " | " | abs net .01044612970055375; tau .0281886016086664 / .033162962931100985 | same | .010446; .028189 / .033163 | yes |
| X-10 | " | " | accounting 3.391384395534658e-16 / 4.7994000936258844e-14 | X-10 `summary.json` | 3.39e-16 / 4.80e-14 | yes |
| X-10 | " | " | verdict | - | ACCEPTED {net_annual_above_parent: True, sharpe_within_.100: True, mechanics: True (6/6 at r 1.690903)} | **yes** |
| Y-1 | v8y §6 Y-1 row, §8 Y-1 bullet, YP-9 | ACCEPTED | dSR vs X-10 +.021480565906708105; Memmel SE .051837777160737585; p one-sided .298 | `summ.json` `78aea78d` row 69; `bundle-x10.json` `241c5bd9` | +.021481; .0518; .2980 | yes |
| Y-1 | " | " | net annual .08798970720884516 > .05653814276222473 | Y-1 `summary.json` `876d107b`; `book.json` `b85952b0` | .087990 > .056538 | yes |
| Y-1 | " | " | S2 net Sharpe 1.8297698079085447 >= 1.749482026329859; dSR vs Y-F0 -.0197122 | Y-1 `summary.json`; ledger line 133 | 1.829770 >= 1.749482; -.019712 | yes |
| Y-1 | " | " | gross 1.5556647047690102 in [0.8287955811012014, 1.6725911622024028] | S2 daily `d6d145ef`; `mech.json` `99eb2484` | 1.555665 in [.828796, 1.672591] | yes |
| Y-1 | " | " | abs net .00966843060025726; tau .02853644896237781 / .03494294094961286; accounting 4.20e-16 / 3.78e-14 | same | .009668; .028536 / .034943; 4.20e-16 / 3.78e-14 | yes |
| Y-1 | " | " | `vol_target.csv`: 48 estimates, gaps 21; 0 before the first; 584 / 21 / 399 decisions, 28 / 1 / 19 estimates; L_t mean 1.8671322726, min 1.0, max 2.0; priced_share all-rows mean .998634, min .995193; 3 warm-up estimates | `vol_target.csv` `100286a5` | 48; 0; 584 / 21 / 399; 28 / 1 / 19; 1.867132; .998634 / .995193; n_w 3 | yes |
| Y-1 | " | " | verdict | - | ACCEPTED {dsr_vs_x10_positive: True, x10_rule_vs_yf0: True, mechanics: True (6/6)} | **yes** |

**Part A result:**
- **X-10: match** (verdict and every deciding number to the printed precision).
- **Y-1: match.**
- R0-12 proceeds to Part B.
- Small note: the logged priced_share .998634 is the mean over all 1,004 rows. The mean over the 48 estimate rows is
  .998641. Both are above .9956.

## Part B: the v8 adoption print (v8y §7)

### What was run, and why it is allowed

- v8y §7 prints the bundles with `nav_summ.py --protocol v8 --bundle`. v8y §4 names the DSR tool: `nav_summ.py <book
  dirs> --dsr-total <ledger> [--dsr-hand <dir> ...]`.
- No wave-driver stage exists for the adoption print, so no wave manifest and no `--seal-allow`.
- Each of the six steps was built by the driver's own builders:
  - `wave_steps.reader_argv` for the mechanics and book readers;
  - `wave_steps.bundle_argv` for the three bundles;
  - `wave_steps.bounded_argv` for the DSR reader.
- Each ran once under `scripts/run_bounded_research.py`, as R0-10 and R0-11 did, with the readers' caps (180 s / 1,536
  MiB / 512 MiB free).
- Disclosure: under ruling `progress.md:173` the DSR step is a read-only reader, but it is not one of the driver's named
  stages.
- Gate (scratch `r12run.py`):
  - free memory >= 3,072 MiB (cap 1,536 + 1,536) at launch, sampled every .25 s during the run;
  - the ledger line count and sha256 checked before and after each step;
  - an argv carrying `--ledger` refused before launch. None did. `--ledger-n` is the read-only Appendix A printer.
- Host: no compiler, no `atx-*` exe and no other root research run. Another session's atx-db jobs were running in
  `C:/atx` (`compustat_std build`, `fund_notes build`, a pytest run). Root did not touch them.
- Interpreter `C:/Program Files/Python312/python.exe`. Source `5292b46e` (clean in the code pathspec).
- Disclosure: the two Part A scratch scripts ran under the `atx-db/.venv` python binary, the default `python` on PATH.
  They wrote nothing under `atx-db/`. Every Part B step used Python312.

### Commands, exit codes and wall time

Common prefix: `"C:/Program Files/Python312/python.exe" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536
--min-free-mib 512 --output build-equity/p9-r12-adopt/<step>-run1 --bind <tool> --bind <each NAV's summary.json,
daily_*.csv, capacity_curve.csv> -- "C:/Program Files/Python312/python.exe" <command>`. The full argv is in each run
dir's `start.json` and `receipt.json` `command`.

| step | command after `--` | launch (UTC) | gate / free before / min (MiB) | exit | wall s (runner / gate script) | peak MiB | receipt.json sha256 |
|---|---|---|---|---|---|---|---|
| mech | `scripts/wave_readers.py mechanics --nav yf0=<Y-F0> --output build-equity/p9-r12-adopt/mech.json` | 16:20:30 | 3,072 / 3,646 / 3,566 | 0 | 0.53 / 1.01 | 45 | `2865798f7c75e37d199a6388b148e246531e34fcd9e7f826ce226230e245e768` |
| bundle-r2 | `atx-impl/tools/nav_summ.py --protocol v8 --bundle <R-2> <Y-F0> --bundle-json .../bundle-r2-yf0.json` | 16:20:38 | 3,072 / 3,536 / 2,942 | 0 | 1.33 / 2.01 | 479 | `25c6c4ab3e20e8dc996935da145b39afab331f4295bbec3aa9228e4bd87fb9d4` |
| bundle-xf0 | the same with `<X-F0> <Y-F0>`, `.../bundle-xf0-yf0.json` | 16:20:41 | 3,072 / 3,568 / 3,179 | 0 | 1.08 / 1.51 | 516 | `b1986f51baf8fce42ba866f8062155d2de4530722d285880870bf9ddf9d56a90` |
| bundle-b0c | the same with `<B0c> <Y-F0>`, `.../bundle-b0c-yf0.json` | 16:20:42 | 3,072 / 3,606 / 3,172 | 0 | 1.06 / 1.51 | 479 | `36ab6f638e491217f0095baade0432c7ab8096dba9a9f326908b60fa58e7b27f` |
| book | `scripts/wave_readers.py book --nav yf0=<Y-F0> --nav r2=<R-2> --nav xf0=<X-F0> --nav b0c=<B0c> --nav yf=<Y-1> --nav x10=<X-10> --output .../book.json` | 16:20:53 | 3,072 / 4,466 / 4,399 | 0 | 0.51 / 1.01 | 45 | `c9f875fb152bc0be14a66aced63c54baaaf39312c50b347aac2f92d8f855b545` |
| dsr | `atx-impl/tools/nav_summ.py --protocol v8 <Y-F0> <R-2> --dsr-total build-equity/trials.jsonl --dsr-hand <X-F0> --dsr-hand <R-2> --ledger-n build-equity/trials.jsonl --json .../dsr.json` | 16:21:01 | 3,072 / 4,431 / 4,370 | 0 | 0.53 / 1.01 | 49 | `f04213e3f61f973bb85b3f0e031e49f5a399b39f175994534a6f73fbb79d75ae` |

NAV dirs, as pinned by the ledger lines the tool matched by daily-CSV trial id:

| book | NAV dir | S2 daily sha256 prefix | ledger line / trial |
|---|---|---|---|
| Y-F0 | `build-equity/mega-nav-v8x-theme-erc-L1.1828-v8ysb` | `73b69bcc` | 128 / `11c10defb3cf38a5` |
| R-2 (V8-F) | `build-equity/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80` | `7cfe21c4` | 50 / `2082800117b50112` |
| X-F0 (X-5) | `build-equity/mega-nav-v8x-theme-erc-L1.1720` | `529062d6` | 97 / `269cfc47be86d4a7` |
| B0c | `build-equity/mega-nav-v8-b0c-dlret-ws60-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247` | `78360530` | 41 / `d24ef3e1a8211878` |
| Y-F (Y-1) | `build-equity/mega-nav-v8y-vol-target-L2.0` | `d6d145ef` | 133 / `0ad7ea4c5122ebe3` |
| X-10 | `build-equity/mega-nav-v8x-theme-erc-L2.0-v8ysb` | `e783eb74` | 132 / `25f3b27aae4754ab` |

Each book is named as follows:
- R-2 = V8-F: v8y §1. Its NAV is from `scripts/specs/v8/lib-v80.json`.
- X-F0 = X-5, `x-theme-erc-gm.json`: v8 log line 6398 "X-F0 so far = X-5", and Y-S's parent.
- B0c: `base-b0c.json`.
- Y-F0: `lib-v8ysb-gm.json`, since Y-3, Y-2 and Y-5 were not accepted.

Output files, all under `build-equity/p9-r12-adopt/`:

| file | sha256 |
|---|---|
| `mech.json` | `c397ac3f8537169e06f61e25bb2c7ff98694c15b257e523c29e930c932ae087a` |
| `bundle-r2-yf0.json` | `803e545600bac931a37e37a6e5615532f1b6c34872887aad5da58ba7f5439df4` |
| `bundle-xf0-yf0.json` | `f30980441ba07d9ea55338037b78788e3be32922b0203253c6b4138236e93a1d` |
| `bundle-b0c-yf0.json` | `24f6479ba20f4ff3340bca1e0b504fc92b69c6996e5587b67b0766dae88b9921` |
| `book.json` | `642ad29b8693f36b3e4528898ea4ce79ceabfe50036c8d32e082de8cf5ae90ae` |
| `dsr.json` | `fdb6a9725b9464037aeb3554a0a61e1c1bb3d6c4b9d4c9f597d33be3b69fa217` |
| `mech-run1/stdout.log` | `a7070506a02a7011ca1ac1c0b034c2f0845ef86011645cd595db5f1b3e3a6ee2` |
| `bundle-r2-run1/stdout.log` | `c57801c373294f83ab09b6a3628b75e79e136f7672ed6bc15f1868687ace3c0a` |
| `bundle-xf0-run1/stdout.log` | `24ab3f17be9c22ec075fceaafa2ba29889fff38e021c8092acf01a3ead0ff3e7` |
| `bundle-b0c-run1/stdout.log` | `0512fbdb64cad34b19cdd3a776d069bce33b02f4646a743be69e547bfd4b47a4` |
| `book-run1/stdout.log` | `e288286d3a3a99c5b349d42258dcc801a8625e93403b2fb452a87beaa1fceb36` |
| `dsr-run1/stdout.log` | `cd1a637537d963c3440589c98f26ebae159e063368d72d964639daa64bc94cf0` |
| `dsr-run1/stderr.log` | `4e5ea0831d8aa2cdd37fe594919d83229bde1bb4e68351bb209508b6b4a1b09b` (the usual "2 listed dir(s) have a defined SR but --dsr-n is 10" warning) |

### Trial ledger

| when | lines | sha256 |
|---|---|---|
| start of R0-12 | 133 | `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554` |
| before and after each of the 6 steps | 133 | `27e40f9f6314585e...` (gate log) |
| end of R0-12 (after all six steps, re-measured just before the commit) | 133 | `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554` |

**0 trials.** N stays 62 of 62. The chain head is `5a3ef9d9bf242dbb`.

### Seal: per-hit check (no `--seal-allow` passed)

This scan is a hidden-data record, not a driver gate. Scratch `sealr12.py` applies `tools/sealsrc.py`'s
classification:
- source (a): the png name;
- source (b): a receipt `started_utc`;
- the wave_seal SEEDS;
- OTHER: anything else.

It covers 36 files: every file under `build-equity/p9-r12-adopt/` (run dirs and outputs) and the six scratch console
logs.

| token | source | count | where |
|---|---|---|---|
| `2026-10-02` | (a) png name in `dirty_outside_pathspec` | 18 | receipt / start.json / console, 6 runs |
| `2026-10-03` | (b) receipt `started_utc` | 18 | same |
| `20260929` | SEEDS (nav_summ v8 bootstrap seed) | 6 | bundle / dsr outputs |
| **`20261003`** | **OTHER**: the P9 sprint dir name in `".superpowers/sdd/platform-p9-20261003/progress.md"` | **18** | `dirty_outside_pathspec` of every receipt / start.json / console |

The OTHER token is a path name. The runner lists `progress.md` as dirty because it holds the PM's uncommitted lines. It
is no data date. SEAL-ALLOW names only sources (a) and (b), and says "any other token or source stops". **Open for the
PM's ruling.** The TRAIN outputs read by the six steps all went through the seal-checked loaders (`load_daily` refuses
a session at or after 2024-01-01).

### The print (S2 `modeled-1bn-stale5-v1+swap-fin-v1`, 1,005 return sessions 2020-01-02 .. 2023-12-29)

#### 1. Adoption rule (v8y §7 as PM7-34 (2) amended it)

| part | Y-F0 | limit | holds | source |
|---|---|---|---|---|
| S2 net Sharpe | 1.849482026329859 | >= 1.0 | yes | Y-F0 `summary.json` `2d30b7f6`; `book.json` `642ad29b`; ledger line 128 |
| all-rows mean gross | .9862134133265011 | [.90, 1.05] | yes | `mech.json` `c397ac3f` (S2 daily `73b69bcc`) |
| abs(all-rows mean net) | .005339981852022642 | <= .02 | yes | same |
| tau mean | .028134396709054203 | <= .20 | yes | same |
| tau p95 | .033410443831757175 | <= .30 | yes | same |
| max return identity error / max cash book relative error | 3.67544536472586e-16 / 1.1074477435259236e-13 | <= 1e-09 | yes | same |

**Rule met.** Y-F0 (`scripts/specs/v8/lib-v8ysb-gm.json`, library v8ysb, L 1.1828) is the deployable book under the
registered rule, in R-2's place.

#### 2. G-B1 (P9 plan §0.3), for Y-F0

| gate | measure | value | floor | met | P9 target | met |
|---|---|---|---|---|---|---|
| G-B1 | S2 net Sharpe, deployable unlevered book | **1.849482** | >= 1.0 | **yes** | >= 1.85 | no (1.849482) |

#### 3. Bundles (verbatim stdout)

`bundle-r2-run1/stdout.log` (`c57801c3`), JSON `bundle-r2-yf0.json` (`803e5456`):
```
== bundle build-equity\mega-nav-v8x-theme-erc-L1.1828-v8ysb vs build-equity\mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80 [modeled-1bn-stale5-v1+swap-fin-v1]: dSR(net) +0.594 = SR +1.849 - SR +1.256 | rho 0.900 | T 1005 | Memmel SE 0.226 | CBB 95% [+0.132, +1.088] | LW studentized 95% [+0.068, +1.119] p two-sided 0.0308 one-sided 0.0030 (4999 draws, block 21, seed 20260929)
   verdict PASS: dSR > 0 True; one-sided p 0.0030 < 0.1
   2020: dSR +1.651 (SR final +1.335, SR base -0.316, 252 sessions)
   2021: dSR +0.388 (SR final +2.984, SR base +2.596, 252 sessions)
   2022: dSR +0.259 (SR final +2.099, SR base +1.841, 251 sessions)
   2023: dSR +0.182 (SR final +0.820, SR base +0.638, 250 sessions)
```
`bundle-xf0-run1/stdout.log` (`24ab3f17`), JSON `bundle-xf0-yf0.json` (`f3098044`):
```
== bundle build-equity\mega-nav-v8x-theme-erc-L1.1828-v8ysb vs build-equity\mega-nav-v8x-theme-erc-L1.1720 [modeled-1bn-stale5-v1+swap-fin-v1]: dSR(net) +0.080 = SR +1.849 - SR +1.769 | rho 0.956 | T 1005 | Memmel SE 0.150 | CBB 95% [-0.236, +0.407] | LW studentized 95% [-0.256, +0.416] p two-sided 0.6262 one-sided 0.3122 (4999 draws, block 21, seed 20260929)
   verdict FAIL: dSR > 0 True; one-sided p 0.3122 < 0.1
   2020: dSR +0.857 (SR final +1.335, SR base +0.478, 252 sessions)
   2021: dSR -0.298 (SR final +2.984, SR base +3.282, 252 sessions)
   2022: dSR -0.072 (SR final +2.099, SR base +2.171, 251 sessions)
   2023: dSR -0.107 (SR final +0.820, SR base +0.926, 250 sessions)
```
`bundle-b0c-run1/stdout.log` (`0512fbdb`), JSON `bundle-b0c-yf0.json` (`24f6479b`):
```
== bundle build-equity\mega-nav-v8x-theme-erc-L1.1828-v8ysb vs build-equity\mega-nav-v8-b0c-dlret-ws60-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247 [modeled-1bn-stale5-v1+swap-fin-v1]: dSR(net) +0.717 = SR +1.849 - SR +1.133 | rho 0.872 | T 1005 | Memmel SE 0.255 | CBB 95% [+0.233, +1.189] | LW studentized 95% [+0.232, +1.202] p two-sided 0.0050 one-sided 0.0024 (4999 draws, block 21, seed 20260929)
   verdict PASS: dSR > 0 True; one-sided p 0.0024 < 0.1
   2020: dSR +1.016 (SR final +1.335, SR base +0.319, 252 sessions)
   2021: dSR +0.632 (SR final +2.984, SR base +2.352, 252 sessions)
   2022: dSR +0.163 (SR final +2.099, SR base +1.936, 251 sessions)
   2023: dSR +0.702 (SR final +0.820, SR base +0.118, 250 sessions)
```
The bundles' "verdict" line is the tool's v8-prereg item 9 freeze-gate part (alpha .10). It is printed as the tool
prints it. Under PM7-34 (2) it gates nothing.

Exact values from the JSON:

| bundle | dSR | Memmel SE | t | rho | CBB 95% | LW SE | LW 95% | p one-sided | p two-sided |
|---|---|---|---|---|---|---|---|---|---|
| R-2 -> Y-F0 | 0.5935378194523855 | 0.22583850277193793 | 2.6281515869407226 | 0.8995266242016288 | [0.13218632202186648, 1.0877341816300174] | 0.24378567151020186 | [0.06832152635810293, 1.1193451385424134] | 0.003 | 0.0308 |
| X-F0 -> Y-F0 | 0.08003070689164593 | 0.1501668298849328 | 0.5329453045853765 | 0.9556041703496023 | [-0.23580753873511617, 0.4071793491746803] | 0.16420231170280206 | [-0.25602634753526005, 0.41616745333931215] | 0.3122 | 0.6262 |
| B0c -> Y-F0 | 0.7167298992975675 | 0.25478495426966913 | 2.813077802620037 | 0.8720608637865426 | [0.23288281548688392, 1.1892566366365682] | 0.2433610317955409 | [0.2319699998463829, 1.2022034954813405] | 0.0024 | 0.005 |

#### 4. DSR_tot / DSR_hand / DSR_v8 (verbatim, `dsr-run1/stdout.log` `cd1a6375`; JSON `dsr.json` `fdb6a972`)

```
== total-count DSR build-equity/mega-nav-v8x-theme-erc-L1.1828-v8ysb (ledger build-equity\trials.jsonl, 133 lines, chain head 5a3ef9d9bf242dbb)
   DSR_tot (N_tot=233): DSR 0.6798 vs SR0 1.609 ann | N_tot = admission 61 + construction 62 + campaign registry 110 | V[SR] 1.296e-03 from 33 cells ledgered on research-window-v2 | SR +1.849 ann, T 1005, skew -0.376, kurtosis 5.636
   DSR_v8 (N_c=62, v8 rule 3 as --dsr-ledger): DSR 0.8350 vs SR0 1.347 ann | same V
== total-count DSR build-equity/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80 (ledger build-equity\trials.jsonl, 133 lines, chain head 5a3ef9d9bf242dbb)
   DSR_tot (N_tot=233): DSR 0.2468 vs SR0 1.609 ann | N_tot = admission 61 + construction 62 + campaign registry 110 | V[SR] 1.296e-03 from 33 cells ledgered on research-window-v2 | SR +1.256 ann, T 1005, skew -0.604, kurtosis 7.155
   DSR_v8 (N_c=62, v8 rule 3 as --dsr-ledger): DSR 0.4297 vs SR0 1.347 ann | same V
== DSR_hand build-equity/mega-nav-v8x-theme-erc-L1.1720: ledger prefix of 97 lines ending at its construction line 269cfc47be86d4a7 (chain head 877cf36f937ab50f)
   DSR_hand (N_tot=88): DSR 0.7741 vs SR0 1.389 ann | N_tot = admission 34 + construction 54 + campaign registry 0 | V[SR] 1.239e-03 from 25 cells ledgered on research-window-v2 | SR +1.769 ann, T 1005, skew -0.100, kurtosis 4.181
== DSR_hand build-equity/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80: ledger prefix of 50 lines ending at its construction line 2082800117b50112 (chain head 8c0f816108c73c70)
   DSR_hand (N_tot=49): DSR 0.9857 vs SR0 0.128 ann | N_tot = admission 7 + construction 42 + campaign registry 0 | V[SR] 1.272e-05 from 5 cells ledgered on research-window-v2 | SR +1.256 ann, T 1005, skew -0.604, kurtosis 7.155
```

**Y-F0's DSR_hand** follows v8y §4 and YP-2: "`dsr_total.dsr_at(moments, N_hand, V)` with N_hand from the tool's
printed parts ... less the mined wave's lines; root prints the inputs".
- X-9 is undefined (v8 log 6399-6400: "campaign admitted 0; X-9 undefined"), so X-9 is not in Y-F0's lineage and the n/a
  rule does not apply.
- Mined-wave construction or admission lines in the ledger: 0. The only `mined`-origin line is the campaign line 112,
  which counts 0 in `trial_counts`, registry 110.
- Inputs: ledger_trials 123, campaign_registry M 110, N_tot 233, so **N_hand = 233 - 110 - 0 = 123**.
  - V = 0.0012956609509370545 (33 cells).
  - Moments (from `dsr.json`): sr_daily 0.11650641657030054, sessions 1005, skew -0.3755968528926102, kurtosis
    5.635992128782652.
- Result `dsr_at`: n 123, sr0_daily 0.0936812897549936, sr0_annual 1.4871443711489767, **dsr 0.7588591647016791**.

| print | book | N | V (per session) | DSR | SR0 annual |
|---|---|---|---|---|---|
| **DSR_tot** | Y-F0 | 233 | 1.2956609509370545e-03 (33 cells) | **0.6798221234280584** | 1.6085545589140278 |
| **DSR_hand** | Y-F0 | 123 | same | **0.7588591647016791** | 1.4871443711489767 |
| **DSR_v8** | Y-F0 | 62 | same | **0.8350043658363188** | 1.3471397829207912 |
| beside: X-F0 at its own ledger state (prefix rule) | X-F0 | 88 | 1.2387730494472615e-03 (25 cells) | 0.7740795486596783 | 1.388590064592441 |
| beside: V8-F at its own ledger state (prefix rule, `--dsr-hand`) | R-2 | 49 | 1.2715334471465972e-05 (5 cells) | 0.9856882093644442 | 0.12841409018070063 |
| beside: V8-F at the print's N and V | R-2 | 233 / 62 | 1.2956609509370545e-03 | 0.24684653583838115 / 0.4297469684273689 | 1.609 / 1.347 |
| beside: V8-F public value (v8 log line 5250, the freeze print) | R-2 | 50 | 1.2971e-03 (21 cells) | .4648 | 1.3014 |

The tool's per-dir lines "deflated SR (N=10)" (Y-F0 .9894, R-2 .8760) are nav_summ's legacy listed-dirs DSR. They are
not one of the three registered DSRs.

#### 5. PBO

v8y §7 says "PBO" and names no grid. **Open for the PM.** Two values exist on disk; no new PBO run was made:

| option | grid | PBO | file (sha256) |
|---|---|---|---|
| (i) the ledger's whole grid at the print | 70 candidates (Y-1's summ grid: every ledgered cell; nothing ledgered since, ledger still 133 lines `5a3ef9d9`), 16 blocks, 12,870 splits, 754 common sessions 2020-01-06..2022-12-30 | **0.1526029526029526** | `cycle-v8y-vol-target-y-1/pbo.json` `2beb256ccd64325d28f0b7f95d3eae8e94b10b4ab57197f48c7681e7876f4ea6` |
| (ii) Y-F0's own cycle (Y-S judge, N 57) | 65 candidates, same blocks | 0.07661227661227661 | `cycle-v8ysb-gm/pbo.json` `556192c0161051011587467b86c848b916ae4ff8de3fbfbd79cd1d7b4f23aa10` |

#### 6. The 4x capacity row (verbatim `capacity_curve.csv` lines, multiple 4)

Y-F0 `build-equity/mega-nav-v8x-theme-erc-L1.1828-v8ysb/capacity_curve.csv`
(`edf1001d5afa0e81f7d41b76b73f8e8ece5441c77b2fb23dbaa28457f4b0691f`):
```
multiple,book,equivalent_initial_nav,net_sharpe,gross_sharpe,ann_mean_net,ann_vol,max_drawdown,cost_bps_per_traded_dollar,traded_dollars,trade_cost_dollars,fills,capped_fills,capped_share,unfilled_dollars,participation_p95_bound,participation_max,daily_turnover_gmv_mean,financing_dollars
4,capacity-x4-v1+swap-fin-v1,4000000000,1.6994264697927697,2.2444151916837844,0.051857872279757466,0.030514925594916201,0.026408756297586322,17.091118878976463,118571567737.97588,202652075.98763561,1704708,171171,0.10041074483137288,12272461397.694208,0.010000000000000002,0.010000000000000002,0.026826637730411752,94645815.847316995
```
R-2 `build-equity/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80/capacity_curve.csv`
(`b99b1cd14237067c06b6af36b2b3b472fee7e0aee9a6524bb80a680ebcfca3db`):
```
4,capacity-x4-v1+swap-fin-v1,4000000000,1.1784641374570275,1.5965805595372335,0.0422939152716146,0.035889013443276586,0.033344380783857597,16.86589613223909,97778968926.706726,164912993.38352692,1653598,132674,0.080233527132954924,8945726438.8507385,0.010000000000000002,0.010000000000000002,0.022992240295924019,92363337.696600586
```
The book reader's `x4_net_sharpe` (`book.json`) equals these rows: Y-F0 1.6994264697927697, R-2 1.1784641374570275.
Beside them: X-F0 1.654876435267752, B0c 0.9776314300931822, Y-1 1.6280661083698378, X-10 1.5829085298985293.

#### 7. Books: net annual return, turnover, the v8y §8 report (Y-F beside Y-F0)

`book.json` (`642ad29b`), read from each NAV's `summary.json` primary scenario and S2 daily CSV:

| book | net Sharpe 1x | net Sharpe 4x | net annual (`ann_mean`) | CAGR | gross of cost | all-rows gross | tau_gmv mean | tau per unit gross | realised vol | max DD | cost bps / traded $ |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Y-F0 | 1.849482026329859 | 1.6994264697927697 | 0.05653814276222473 | 0.05766642148886314 | 0.07067817348886478 | 0.9862134133265011 | 0.028134396709054203 | 0.028527696266223746 | 0.030569717335626075 | 0.02580037230369092 | 12.657953278624719 |
| R-2 | 1.2559442068774755 | 1.1784641374570275 | 0.045356239815555484 | 0.04571446453055339 | 0.05811796427443334 | 0.9859903463414113 | 0.023928578258270244 | 0.024268572554547792 | 0.03611326010119512 | 0.03247837029057776 | 12.465755819822 |
| X-F0 | 1.7694513194382149 | 1.654876435267752 | 0.05078176385841518 | 0.05165524880856753 | 0.06448890046788266 | 0.9862260459332546 | 0.0268443424622213 | 0.02721925928940439 | 0.028699158490858027 | 0.020598949990380255 | 12.561406606226495 |
| B0c | 1.1327521270322911 | 0.9776314300931822 | 0.04416705487438075 | 0.04435865459943833 | 0.060499386630698586 | 0.9819643798245946 | 0.03406891539013692 | 0.034694655010014265 | 0.038990926452810534 | 0.038854206516742984 | 13.13305845786589 |
| Y-F = Y-1 | 1.8297698079085447 | 1.6280661083698378 | 0.08798970720884516 | 0.09069938898574503 | 0.11196155814360795 | 1.5556647047690102 | 0.02853644896237781 | 0.0183435729273134 | 0.04808785609454272 | 0.03601760582874025 | 14.029899693994995 |
| X-10 | 1.8082892420018353 | 1.5829085298985293 | 0.09306060940646439 | 0.09605818407993016 | 0.11922471527999638 | 1.6667175942698484 | 0.0281886016086664 | 0.016912644172941124 | 0.05146334294586814 | 0.04325721752051026 | 14.505023545230529 |

Mean L_t:
- Y-1: 1.8671322725912003, from the `vol_target` block of Y-1's `summary.json` (`876d107b`), S2 book;
- X-10: fixed at 2.0;
- Y-F0: L 1.1828.

Y-F per v8y §1 is Y-1 if accepted (it is, Part A), so Y-1 is reported as Y-F beside Y-F0.

#### 8. Year table (S2; from the bundle JSONs' `year_table`; the dsr stdout prints the same rows for Y-F0 and R-2)

Verbatim from `dsr-run1/stdout.log`:
```
== build-equity\mega-nav-v8x-theme-erc-L1.1828-v8ysb [modeled-1bn-stale5-v1+swap-fin-v1] rule=aim-partial-v5+neutral-price-risk-v1
   construction: gross_lev 0.9862 net_lev +0.0054 |net| 0.0071 held_share 1.0375 held_names 1918.0 tau_gmv mean 0.0281 p95 0.0334 (1004 sessions) cost/GMV-tau 0.00125 cost_bps_traded 12.66
   leverage over all 1006 CSV rows [R6' mechanics gate]: gross_lev_all_rows 0.9862 net_lev_all_rows +0.0053 (construction gross_lev/net_lev: previous close over return rows)
   leverage post-ramp over the 943 CSV rows after the first 63 [v6 C4 L calibration; gate stays all rows]: gross_lev_post_ramp 0.9917 net_lev_post_ramp +0.0054
   WARNING CSV tau mean 0.028134 != summary 0.028134
   deflated SR (N=10): DSR 0.9894 | SR +0.11651/session (+1.849 ann) vs SR0 0.04163/session (0.661 ann) | skew -0.376 kurtosis 5.636 | T 1005 | V[SR_n] 6.990e-04 (cross-cell sample variance of 2 per-session SRs)
   year table (return rows, compounded net return, net Sharpe, volatility, tau_gmv mean, cost bps per traded dollar):
     2020 rows  252 net +0.0431 sharpe +1.335 vol 0.0320 tau 0.0304 cost_bps 14.98
     2021 rows  252 net +0.0983 sharpe +2.984 vol 0.0316 tau 0.0273 cost_bps 11.68
     2022 rows  251 net +0.0691 sharpe +2.099 vol 0.0322 tau 0.0278 cost_bps 12.36
     2023 rows  250 net +0.0211 sharpe +0.820 vol 0.0260 tau 0.0271 cost_bps 11.72
== build-equity\mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80 [modeled-1bn-stale5-v1+swap-fin-v1] rule=aim-partial-v5+neutral-price-risk-v1
   construction: gross_lev 0.9859 net_lev +0.0042 |net| 0.0073 held_share 1.0366 held_names 1915.9 tau_gmv mean 0.0239 p95 0.0284 (1004 sessions) cost/GMV-tau 0.00123 cost_bps_traded 12.47
   leverage over all 1006 CSV rows [R6' mechanics gate]: gross_lev_all_rows 0.9860 net_lev_all_rows +0.0042 (construction gross_lev/net_lev: previous close over return rows)
   leverage post-ramp over the 943 CSV rows after the first 63 [v6 C4 L calibration; gate stays all rows]: gross_lev_post_ramp 0.9918 net_lev_post_ramp +0.0043
   WARNING CSV tau mean 0.023929 != summary 0.023928
   deflated SR (N=10): DSR 0.8760 | SR +0.07912/session (+1.256 ann) vs SR0 0.04163/session (0.661 ann) | skew -0.604 kurtosis 7.155 | T 1005 | V[SR_n] 6.990e-04 (cross-cell sample variance of 2 per-session SRs)
   year table (return rows, compounded net return, net Sharpe, volatility, tau_gmv mean, cost bps per traded dollar):
     2020 rows  252 net -0.0111 sharpe -0.316 vol 0.0336 tau 0.0264 cost_bps 14.79
     2021 rows  252 net +0.0983 sharpe +2.596 vol 0.0364 tau 0.0227 cost_bps 11.45
     2022 rows  251 net +0.0796 sharpe +1.841 vol 0.0423 tau 0.0235 cost_bps 12.10
     2023 rows  250 net +0.0193 sharpe +0.638 vol 0.0310 tau 0.0231 cost_bps 11.52
```
The stdout also prints five per-scenario lines for each dir, with their financing JSON. They are left out here and are
in the file.

The four books by year (`bundle-*-yf0.json` `year_table`):

| year | rows | Y-F0 net / SR / vol / tau / bps | R-2 net / SR / vol / tau / bps | X-F0 net / SR / vol / tau / bps | B0c net / SR / vol / tau / bps |
|---|---|---|---|---|---|
| 2020 | 252 | +.0431 / +1.335 / .0320 / .0304 / 14.98 | -.0111 / -.316 / .0336 / .0264 / 14.79 | +.0130 / +.478 / .0279 / .0290 / 14.95 | +.0135 / +.319 / .0453 / .0362 / 15.67 |
| 2021 | 252 | +.0983 / +2.984 / .0316 / .0273 / 11.68 | +.0983 / +2.596 / .0364 / .0227 / 11.45 | +.1038 / +3.282 / .0302 / .0260 / 11.61 | +.0767 / +2.352 / .0316 / .0335 / 12.03 |
| 2022 | 251 | +.0691 / +2.099 / .0322 / .0278 / 12.36 | +.0796 / +1.841 / .0423 / .0235 / 12.10 | +.0684 / +2.171 / .0308 / .0265 / 12.20 | +.0859 / +1.936 / .0432 / .0332 / 12.80 |
| 2023 | 250 | +.0211 / +.820 / .0260 / .0271 / 11.72 | +.0193 / +.638 / .0310 / .0231 / 11.52 | +.0232 / +.926 / .0253 / .0259 / 11.58 | +.0034 / +.118 / .0340 / .0333 / 12.16 |

Per-year dSR of Y-F0 against each base (`years`):

| year | vs R-2 | vs X-F0 | vs B0c |
|---|---|---|---|
| 2020 | +1.6510 | +0.8569 | +1.0161 |
| 2021 | +0.3884 | -0.2982 | +0.6323 |
| 2022 | +0.2588 | -0.0719 | +0.1632 |
| 2023 | +0.1822 | -0.1065 | +0.7017 |

#### 9. Appendix A block

Verbatim from the tool (`--ledger-n`, `dsr-run1/stdout.log`):
```
Appendix A (trial ledger build-equity/trials.jsonl): 123 trials in 133 ledger lines
   admission       61  [TRAIN 2020-01-01..2023-12-31]
   composition      0
   construction    23  [TRAIN 2020-01-03..2023-12-29]
   construction    37  [TRAIN 2020-01-06..2022-12-30]
   construction     2  [TRAIN 2020-01-06..2023-12-29]
   universe         0
   data             0
   mining-campaign     0  (1 campaign line(s), registry count 110: the campaigns' own budget, not in N)
   adding no trial: 9 line(s) (0 by the defect rule, 8 window re-run(s), 1 protocol line(s))
TRAIN construction cells 62; admission trials this sprint 61; window research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024); history reads 0; 2025+ never read.
```
The Y form (v8y §14), filled from the ledger's `cycle` field on the admission lines: v80 7, v81 5, v8x2 5, v8x3 8, v8x4 9,
v8x7 12, v8ys 15.
```
TRAIN construction cells 62; admission trials this sprint 61 (v8 12, X hand-written 25, mined 0, Y hand-written 15; X-4 re-screens 9, a part the Y form has no slot for); mined campaigns 1 (v9-mine-c1: budget 110, registry count 110, admitted 0); N_tot 233; N_hand 123; window research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024); history reads 0; 2025+ never read.
```

#### 10. Claims (v8x §7 with Y-F0 for X-F0; measured values only)

| claim | registered definition | measured | status |
|---|---|---|---|
| Sharpe up | v8x §7: "only under (a)-(c)", where (a) is bundle vs V8-F dSR > 0 with one-sided p < .10, (b) is DSR_tot >= .95 and (c) is mechanics | (a) +.5935, p .0030; (b) .6798; (c) PASS | (a) met, **(b) unmet**, (c) met. v8y §7 carries v8x §7's claim text; PM7-34 (2) replaced the gate as the adoption rule |
| DSR up | v8y §4: DSR_tot(Y-F0) at the print above DSR_tot(V8-F) at V8-F's ledger state; V8-F at the print's N and V printed beside | Y-F0 .6798. V8-F own state: .9857 by the tool's prefix rule (N_tot 49, V from 5 cells) or .4648 by the "public .465 at N 50" that v8y §4 cites. V8-F at the print's N and V: .2468 | **ambiguous, open for the PM.** The two readings of "V8-F at its own ledger state" give opposite signs |
| Capacity up | sign level: Y-F0 4x against R-2's | 1.6994 vs 1.1785 | Y-F0 higher |
| Return up (Y-F0 vs R-2, matched gross) | net annual at matched gross | 5.654% vs 4.536% (all-rows gross .98621 vs .98599) | Y-F0 higher |
| Return up (Y-F vs Y-F0, leverage; the owner's risk decision, never called alpha) | net annual | Y-1 8.799% vs Y-F0 5.654% | Y-F higher |

PM7-37 / PM7-38's X-5 lines, kept as v8y §7 requires (v8 `progress.md:2105-2127`):
- X-5's +.349 is reported as in sample until the OD-3 read.
- The review's decomposition is lower volatility +.311, gross alpha +.080, trade cost -.042 and borrow +.002.
- "under the rule's own equal-Sharpe premise the expected gain is about +.15".

### Open for the PM (reported, not picked)

1. **Seal token `20261003` (18 hits, OTHER under SEAL-ALLOW).**
   - Its source is the P9 sprint directory name in the runner's `dirty_outside_pathspec` list. `progress.md` was dirty
     with the PM's uncommitted lines.
   - It is not a data date. SEAL-ALLOW names only the png name and `started_utc`.
   - Options: rule a third source (the sprint dir name `platform-p9-20261003` in the dirty list, like wave_seal's
     `20260929` sprint id); or re-run the six 0-trial steps after `progress.md` is committed. Even then, the first
     runs' receipts stay on disk and need the ruling.
2. **"DSR up" reference** (v8y §4 / §7). "V8-F at its own ledger state" can mean either:
   - the `--dsr-hand` prefix rule: .9857, N_tot 49, V from 5 cells, because the window re-runs were ledgered after
     R-2's line 50;
   - the public freeze value v8y §4 cites: .465 at N 50, V from 21 cells.
   - Y-F0's .6798 is below the first and above the second.
3. **PBO grid**: (i) the ledger's whole grid .1526, or (ii) Y-F0's own cycle .0766. A fresh `nav_summ --pbo` over the
   70-cell grid would be one more read-only step; it was not run.

### Hard rules

- Nothing was built, switched or committed in `C:/atx`. `atx-db/` was not touched.
- No push, no subagent, no expected hash edited.
- The untracked `docs/plans/2026-10-02-x5-equity-curve.png` was left as it is.
- No `--seal-allow` was used.
- Nothing dated 2024-01-01 or later was opened. Every number comes from TRAIN NAV outputs through the seal-checked
  readers, from the ledger, and from receipts.
- Real-data reads went only through `run_bounded_research.py` with the driver's builders, one at a time, under the
  memory gate. Each took under 3 s and under 520 MiB.
- 0 trials.
