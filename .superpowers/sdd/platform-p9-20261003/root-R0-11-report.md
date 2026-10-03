# Root R0-11 report: Y-1, vol-target-v1 at cap 2.0 on Y-F0, child of X-10

Status: **DONE.** Y-1 ran by hand through `research_cycle.py`, which is the route the registration sets for this cell
(PM8-14 / YP-7). Its mechanics pass 6/6. Under the registered rule (v8y 6 Y-1 row and v8y 8), applied by hand, the cell
is **ACCEPTED**:
- its paired S2 net dSR against X-10 is +.021481 (> 0);
- X-10's leverage rule against Y-F0 holds: net annual .087990 > .056538 AND net Sharpe 1.829770 >= 1.749482;
- its mechanics pass.

N goes 61 -> 62, which is the construction cap (no slot left). Y-1 replaces X-10 as Y-F.

**How this report was written.** The R0-11 root agent (a2129746310fcb219) was killed by an owner stop after its last
commit `0829f784` and before it wrote this report (P9 `progress.md` lines 167-168). This report was written afterwards
by the root-r0-11-report agent. It ran nothing (no build, no research run, no pytest, no real-data process). Every
number below is copied from:
- the v8 integration log `.superpowers/sdd/platform-v8-20260929/integration-log.md`, section "R0-11 Y-1 vol-target on
  Y-F0, child of X-10" (lines 7614-7872);
- the four R0-11 commits and the files they add;
- the reader, bundle and verdict JSON files and receipts named in the log, read and hashed without re-running anything.

Nothing was recomputed.

- Root: `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`. HEAD before the step: `dbc2b975` (the R0-10 report).
- Date: 2026-10-03. The log header gives the start as 15:16Z and logs no end time. The last commit, `0829f784`, is at
  15:35:44Z (11:35:44 -0400).
- Ledger before the step: `build-equity/trials.jsonl` had 132 lines (file `e242e161`, head `15484aa1699daa9a`), N 61.
- Rulings applied (log line 7618):
  - Y1-PARENT (P9 `progress.md`:158): parent = X-10's spec.
  - SEAL-ALLOW (`progress.md`:108): a per-hit source check of Y-1's own run dirs.
  - R0-6-ATT: only if an attempt fails. Every logged step exited 0, and the section shows no second attempt.
  - R0-7-MAN: the cell's template spec, as in R0-10.
  - R0-7-EOL: done in R0-7.
  - OD-P9-3: P9 keeps L <= 2.0. Y-1's cap is 2.0, and its clip keeps L_t <= 2.0.
- Stop conditions: none fired, as logged.
  - Template pin: PASS.
  - Parent check: no stop.
  - P14: PASS.
  - Every exit was 0.
  - `vol_target.csv` stop checks: "none fires".
  - Both seal checks: 0 OTHER.

## Route: by hand through `research_cycle.py` (log lines 7622-7633)

- **Registration:**
  - v8y 6 Y-1 row, v8y 8 Y-1 bullet, v8y 13 P14, and v8y 14 Y-1 row;
  - the template `scripts/specs/v8/y-vol-target.json` @ `47d6afd9`, file
    `4e190f5434dfc13aced1459721cde476faf3560ef99aa41e05bfe492e2f141b4`;
  - YP-8 and YP-9.
- **Registered constants:** cap 2.0, floor 1, cadence 21, annualisation 252. The change is `nav --vol-target
  vol-target-v1 --risk-model <store> --risk-model-sha256 <pin>`.
- **Form of the cell:**
  - NAV-only;
  - not gross-matched (PM7-20 reading B);
  - YP-15 does not apply, because Y-5 was rejected.
- **Why by hand:** PM8-14 / YP-7 say "X-10 and Y-1 by hand through `research_cycle.py`". The driver has only `v8-mech`
  and `pm7-34`. Y-1's gross band and its three-part rule are not in `scripts/wave_rules.py`, as R0-10 checked.
- **Runbook, as in R0-10:**
  1. template copy, `lock --write`, `plan`;
  2. pre-registration commit;
  3. `run --stop-after nav`;
  4. mechanics before any return;
  5. `run` (summ ledgers the cell);
  6. bundles and the book reader, using the driver's own argv builders (`wave_steps.bundle_argv`, `reader_argv`);
  7. verdict by hand.
- **Before every launch:**
  - memory gate: free >= cap + 1,536 MiB, which is 3,072 MiB for the NAV, summ and readers;
  - host check: no compiler, no `atx-*` exe, no research process;
  - free memory sampled every 0.25 s.

## Order of events (git commit times; receipt `started_utc`)

| # | what | when (UTC) | evidence |
|---|---|---|---|
| 1 | Pin, parent and P14 checks (read-only); mechanics limits, `vol_target.csv` stop checks and acceptance written | before 15:21:25Z | commit **`d1f2d7f2`** (log +100 lines, 7613-7712); no run before it |
| 2 | Identity gtests; spec written; `lock --write`; `plan`; `test_research_spec.py` | after `d1f2d7f2` (the log heads it "source `d1f2d7f2`, the plan commit") | logged in `ac74a48c` |
| 3 | Pre-registration commit: the spec and the log section together | 15:27:27Z | commit **`ac74a48c`** (spec +30 lines, log +40 lines, 7713-7752) |
| 4 | NAV `run --stop-after nav` | receipt `started_utc` 15:27:38Z (log: launch 15:27:37Z) | receipt `cefce32d`, source `ac74a48c` |
| 5 | Mechanics reader, then the `vol_target.csv` checks (keys only) | receipt `started_utc` 15:29:01Z | receipt `461fb484`, source `ac74a48c` |
| 6 | Stage 1 logged: mechanics PASS, 0 trials | 15:32:06Z | commit **`00f0521a`** (log +56 lines, 7753-7808) |
| 7 | Cycle resume: summ ledgers the cell | summ receipt `started_utc` 15:32:16Z (log: launch 15:32:15Z) | receipt `6c3810ef`, source `00f0521a` |
| 8 | Bundle vs X-10, bundle vs Y-F0, book reader | 15:33:37Z / 15:33:38Z / 15:33:40Z | receipts `98e17ee5` / `1eecc772` / `cc074535`, source `00f0521a` |
| 9 | Stage 2 logged; registered rule applied by hand: ACCEPTED | 15:35:44Z | commit **`0829f784`** (log +64 lines, 7809-7872) |

Times: the commit times are 11:21:25 / 11:27:27 / 11:32:06 / 11:35:44 -0400. The receipt times are the receipts'
`started_utc`.

## Pin, parent and P14 checks (log lines 7635-7658; committed in `d1f2d7f2` before anything ran)

1. **Template pin: PASS.**
   - The disk sha256 `4e190f5434dfc13aced1459721cde476faf3560ef99aa41e05bfe492e2f141b4` equals v8y 14's registered file
     pin.
   - The last commit on the path is `47d6afd9`.
   - `git diff HEAD` on the path is empty.
2. **Parent check (Y1-PARENT): no stop.**
   - The template has `"parent": null`. Its `"nominal_parent": "base-b0c.json"` is the planning placeholder (v8y 14:
     "root sets `parent` to the last accepted spec at run time"). The Y-3 precedent kept the same placeholder.
   - The template description's "parent = X-F0" is settled by v8y Appendix C (9) as Y-F0.
   - With `parent` = `x-leverage-L2.0.json` (= Y-F0 + `nav.leverage` 2.0), two things follow:
     - the resolved NAV argv is v8y 8's "Y-F0's spec + `nav.leverage` 2.0 + `nav --vol-target ...`";
     - summ's paired reference is X-10's NAV, so summ's paired dSR is Y-1 vs X-10.
3. **P14 (v8y 13: "the risk store's role pin equals Y-F0's role"): PASS, equal.** So the store is not rebuilt (0
   trials).
   - The store is `build-equity/v8-risk-lo3-v10`. Its `manifest.json` re-hashed to
     `862515d92623be37fbd4b126c8f0350a20135e7977f1cbf66c6c33f1644ecd5a`, which is the registered pin.
   - The store's `role` is `manifest_sha256` `e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4`, path
     `build-equity/train-2020-2023-lo3/manifest.json`.
   - Y-F0's spec (`lib-v8ysb-gm.json` `1e3ec118`) has `inputs.role` = the same path and the same sha256.
   - The role manifest on disk re-hashed to `e1c67101...395f4`.
   - Store keys: schema `atx.risk-model/v1`, model `atx-risk-v1.1`, seal begin 2024-01-01, role_last_session_ns
     1703808000000000000 (2023-12-29).
4. **Defined (the v8y 6 "Undefined" Y-1 row):**
   - X-10 is accepted, so it is neither undefined nor void.
   - X-10's NAV rule is aim-partial-v5 with no `--risk-target`, no spo and no v6.
   - The store is given.

Books the rule reads (log lines 7660-7666, from records; no new read):
- **Y-F0:** `lib-v8ysb-gm.json` `1e3ec118`, L_P 1.1828.
  - NAV `build-equity/mega-nav-v8x-theme-erc-L1.1828-v8ysb` (`summary.json` `2d30b7f6`, S2 daily `73b69bcc`).
  - All-rows S2 gross G_P .9862134133265011; net annual .05653814276222473; S2 net Sharpe 1.849482026329859
    (`p9-r10-x10/book.json` `d222b547`).
- **X-10:** `x-leverage-L2.0.json` `ebe68e6e`, L 2.0.
  - NAV `build-equity/mega-nav-v8x-theme-erc-L2.0-v8ysb` (`summary.json` `1bc1b6ca`, S2 daily `e783eb74`).
  - Trial `25f3b27aae4754ab`, s2_net_sr 1.8082892420018353.

## Thresholds and acceptance, committed in `d1f2d7f2` before any Y-1 run (log lines 7668-7712)

Scope: all rows of the S2 daily CSV (`modeled-1bn-stale5-v1+swap-fin-v1`). P = Y-F0, the unlevered parent.
r = 2.0 / L_P = 2.0 / 1.1828 = 1.6909029421711192.

| row | registered | Y-1 limit | unrounded limit used by the check |
|---|---|---|---|
| mean gross leverage | [1.0, 2.0] x G_P / L_P, widened by .005 (G_P / L_P = .9862134133265011 / 1.1828) | **[.828796, 1.672591]** | [0.8287955811012014, 1.6725911622024028] |
| abs(mean net leverage) | .02 r (X-10's) | **<= .033818** | 0.033818058843422386 |
| tau mean | .20 r (X-10's) | **<= .338181** | 0.3381805884342239 |
| tau p95 | .30 r (X-10's) | **<= .507271** | 0.5072708826513358 |
| max return identity error; max cash book relative error | unchanged | <= the summary's `accounting_checks.tolerance` | - |

**Cross-checks the log records:**
- The band equals the dispatch's [.8288, 1.6726] and R0-10's report line 135.
- The other rows equal X-10's limits as committed at `50406181`.
- Printed only: the band computed on X-10's own gross / L (.8333588) would be [.828359, 1.671718]. The registration
  names the unlevered parent's G_P / L_P instead.
- A value outside its limit is a mechanics fail, which means rejected and counted (v8y 12).

**`vol_target.csv` stop checks, written before the run.** Root stops for a PM ruling before summ (0 trials) on any of:
- an L_t outside [1, 2.0];
- an L_t other than 2.0 before the first estimate or at it;
- two estimates fewer than 21 sessions apart;
- no estimate at all;
- a priced_share mean below .9956 (R-8's minimum on this store).

**Acceptance, fixed before the run** (log lines 7703-7707):
1. paired S2 net dSR of Y-1 against X-10 **> 0** (summ's paired block, reference = X-10's NAV; the bundle X-10 vs Y-1
   prints both p); AND
2. X-10's leverage rule against Y-F0: S2 net annual return at 1x (book reader `net_annual`) **above
   .05653814276222473** AND S2 net Sharpe **>= 1.849482026329859 - .100 = 1.749482026329859**; AND
3. mechanics as above.

**Printed, deciding nothing:**
- dSR against Y-F0, and both p of each bundle;
- net annual return, realised vol and max drawdown of Y-F0, X-10 and Y-1 side by side;
- mean L_t;
- net Sharpe at 1x and at 4x NAV;
- cost per traded dollar;
- DSR (N 62), PBO and the year table.

Count: 1 construction trial whatever the verdict.

## Identity, spec, lock and plan (logged in `ac74a48c`; ran at source `d1f2d7f2`; log lines 7714-7752)

- **Identity gtests** ("before any return"):
  - run with `build-equity/bin/atx-impl-strategy-target-tests.exe` (v8-16d receipt `d504994d...ed0c` = disk) and
    `--gtest_filter=VolTarget.*:RiskTarget.*:BookVolTarget.*`;
  - result: exit 0, **17 / 17 passed** (VolTarget 5, RiskTarget 7 including `FlagAbsentKeepsThePinnedBenchDigests`,
    BookVolTarget 5);
  - xml `build-equity/p9-r11-y1/voltarget-identity.xml`, sha256
    `cd9401955f246ef77acbb567c7b161f0e99570b63d62fc3c57db6dabcea02b52` (tests 17, failures 0, errors 0);
  - the parent's NAV argv on the targets exe `72ff6d2d` had already reproduced Y-F0's files 27 / 27 (R0-9 P13 (a)).
- **Spec** `scripts/specs/v8/y-vol-target-y-1.json`:
  - written by the driver's builder, `wave_steps.rule_cell_doc(template, ..., "scripts/specs/v8/x-leverage-L2.0.json",
    {constants: {flags: {nav: fills}}}, "y-1")`;
  - three keys differ from the template:
    - `name` is `v8y-vol-target-y-1`;
    - `parent` is `x-leverage-L2.0.json`;
    - `change.flags.nav` holds the two root fills, `--risk-model build-equity/v8-risk-lo3-v10` and
      `--risk-model-sha256 862515d92623be37fbd4b126c8f0350a20135e7977f1cbf66c6c33f1644ecd5a`;
  - `lock --write` then added the `locked` block;
  - "No free constant."
- **`lock --write`:** exit 0.
  - reference_cell = **X-10's** `summary.json` `1bc1b6ca`; reference_admission `121f7046`.
  - File sha256 **`faffee1795ba500fbebaa5a52e2449acaf02c92f7ee972011a267a52ba265f77`**.
  - Spec digest **`95a2173d195006578bcab0449e5838b5a13792fff55e5271b52504aa4ae5a98f`**.
- **`plan`:** exit 0.
  - 6 pins `[locked, verified]`: library `41010b0b`, recipe `9d357c75`, role `e1c67101`, label_role `95e16cfe`,
    reference_cell `1bc1b6ca`, reference_admission `121f7046`.
  - Chain `y-vol-target-y-1.json` -> `x-leverage-L2.0.json` -> `lib-v8ysb-gm.json`.
  - u, fit, card, w and monitor are Y-F0's (done); nav pending.
  - summ `--reference` X-10's NAV, `--dsr-n 62`, 71 listed dirs.
- **NAV argv vs X-10's NAV receipt** (47 vs 53 tokens):
  - equal except `--output` and the six inserted tokens `--vol-target vol-target-v1 --risk-model
    build-equity/v8-risk-lo3-v10 --risk-model-sha256 862515d9...4ecd5a`;
  - NAV exe sha256 `72ff6d2d...` = X-10's NAV receipt `executable_sha256`.
- **`scripts/tests/test_research_spec.py`:** **87 passed** (179.7 s), with the new locked file on disk.

## Per stage (log lines 7754-7829)

| step | exit | s / peak MiB | gate / free before / min | receipt.json SHA-256 | output (SHA-256) | source |
|---|---|---|---|---|---|---|
| NAV `run --stop-after nav` | **0** | 48.1 / 590 (cycle wall 48.8) | 3,072 / 6,452 / 5,693 | `cefce32db0a9a0862e299798a17032580e7f7654ca1e450fced2960694442cb4` | `build-equity/mega-nav-v8y-vol-target-L2.0`: `summary.json` `876d107b`, S2 daily `d6d145ef`, `vol_target.csv` `100286a5` | `ac74a48c`; NAV exe `72ff6d2d` |
| mechanics reader (keys only) | **0** | 0.5 / 44 | 3,072 / 6,383 / 6,336 | `461fb4840bc6e0b3f359499d3e64ae81ecd8bda8a1b537e0d95d3dcb85bab5dc` | `p9-r11-y1/mech.json` `99eb2484efcd753938f576cc633d020795d349837d4c621f537f3dbbf1e11468` | `ac74a48c` |
| cycle resume (summ ledgers the cell) | **0** | cycle 43.6; summ 42.5 / 562 | 3,072 / 5,748 / 4,967 | summ `6c3810efaa6eed971e974d19fdb731428b891b6f1d5ffe55139f2cd1960697dc` | `cycle-v8y-vol-target-y-1/summ.json` `78aea78d`, `pbo.json` `2beb256c`, `cycle_verdict.json` `5c113f3f`; `cycle_binding.json` `34c86e52` (binding spec `95a2173d`, argv `6c830675`) | `00f0521a` |
| bundle vs X-10 (deciding dSR's p) | **0** | 0.8 / 468 | 3,072 / 5,802 / 5,403 | `98e17ee523d6bcef352046ddab34fc7faf8192e6c9558a6275e2d3e18fa166e8` | `p9-r11-y1/bundle-x10.json` `241c5bd9248499dd9bb15623b03995209256f669b438724c7116d338c755b9c4` | `00f0521a` |
| bundle vs Y-F0 (printed) | **0** | 0.8 / 443 | 3,072 / 5,788 / 5,442 | `1eecc7728291adab13d5533fc115679a0f5894ba18be9c6c209d45552e04c50e` | `p9-r11-y1/bundle-yf0.json` `4c15e0befd562654b9ec1a82993fcfe101252e3c60ed32b9ebc0266393633966` | `00f0521a` |
| book reader | **0** | 0.5 / 45 | 3,072 / 5,787 / 5,725 | `cc07453501a86307b4895a6600695c085073674354c2de20c7e6ec99eb7bc23b` | `p9-r11-y1/book.json` `b85952b024a351b217b81d2d5cf87f8a86d15572114372674bcd5549d5917f2e` | `00f0521a` |

**What the receipts and run logs record:**
- The NAV receipt is completed and "clean in the code pathspec". It is dirty outside that pathspec only for the
  untracked png.
- The recipe rule is `aim-partial-v5+neutral-price-risk-v1+vol-target-v1`.
- Gate p1-v8ysb was re-read: PASS, with 0 status changes and 0 admission lines appended.
- The four stage-2 receipts are completed, exit 0, "clean in the code pathspec", source `00f0521a`.
- summ's stderr holds nav_summ's usual "70 listed dir(s) have a defined SR but --dsr-n is 62" warning.

## Mechanics (`mech.json` `99eb2484`; limits from `d1f2d7f2`): PASS 6/6

| row | Y-1 | limit | result | X-10 / Y-F0 (beside) |
|---|---|---|---|---|
| all-rows mean gross | **1.555665** | [.828796, 1.672591] | PASS | 1.666718 / .986213 |
| abs(all-rows mean net) | **.009668** | <= .033818 | PASS | .010446 / .005340 |
| tau mean | **.028536** | <= .338181 | PASS | .028189 / .028134 |
| tau p95 | **.034943** | <= .507271 | PASS | .033163 / .033410 |
| max return identity error | 4.20e-16 | <= 1e-09 | PASS | 3.39e-16 / 3.68e-16 |
| max cash book relative error | 3.78e-14 | <= 1e-09 | PASS | 4.80e-14 / 1.11e-13 |

Also logged (keys only):
- aim_leverage 2.0;
- post-ramp gross 1.5575 (943 rows); max gross 1.8099; max |net| .0441;
- gross by year 1.382 / 1.638 / 1.547 / 1.658;
- 1,006 CSV rows, 1,005 return rows, 1,004 tau sessions;
- turnover flags `meets_daily_turnover_mean` / `_p95` true;
- gross ratio Y-1 / X-10 .933370 (= the mean multiplier .933566, within .0002); Y-1 / Y-F0 1.577412.

## `vol_target.csv` before any return (`vol_target.csv` `100286a5`; log lines 7780-7803)

- (a) **48 estimates** in 1,004 scored decisions (1,004 / 21 = 47.81). All 47 gaps between estimates are exactly 21
  decisions.
- (b) **decisions_before_first_estimate 0** (the NAV's summary block).
  - The book's first estimates fall in the unscored 60-session warm-up.
  - sigma_ref of every in-window estimate equals the running mean of 3 warm-up estimates plus the in-window sigma_hat:
    **max error 0.0 over 48 estimates**.
  - L_t = clip(2.0 x sigma_ref / sigma_hat, 1, 2.0) **exactly (max error 0.0) on all 1,004 decisions**.
- (c) Clip counts and summary statistics:
  - decisions at the cap 2.0 **584**, at the floor 1 **21**, unclipped **399**;
  - estimates at the cap 28, at the floor 1, unclipped 19;
  - **L_t n 1,004, mean 1.867132, min 1.0, max 2.0** (multiplier mean .933566);
  - sigma_hat mean .025992 (min .013146, max .046352); sigma_ref mean .025920 (min .017902, max .027769).
- (d) **priced_share mean .998634** (min .995193, max 1.0).
- **Stop checks: "none fires."**
- **Disclosure the log records:** scratch `vty1.py` first printed "sigma_ref_1 != sigma_hat_1". The log attributes that
  to the script's indexing. `vty1b.py`'s exact reconstruction shows that the first estimate is a warm-up estimate.

## Verdict (applied by hand; log lines 7859-7872)

**No tool prints Y-1's registered rule.**
- `cycle_verdict.json` carries the paired, dsr and pbo blocks, with no accepted field.
- The bundles' `verdict` blocks are the v8-prereg item 9 freeze-gate part, not Y-1's rule. Vs X-10 they print `pass:
  false, dsr_positive: true`.

**Rule text applied** (`.superpowers/sdd/platform-v8-20260929/v8y-prereg.md`):
- **v8y 6, Y-1 row, "acceptance (decides)":** "paired S2 net dSR > 0 against X-10 AND X-10's rule against Y-F0 AND
  mechanics (YCOMB registered)".
- **v8y 8, Y-1 bullet:** "Acceptance (registered): paired S2 net dSR > 0 against X-10 (the fixed-L book at the same
  cap: the hypothesis) AND X-10's leverage rule against Y-F0 (net annual return higher AND S2 net Sharpe not lower by
  more than .100) AND mechanics. If accepted it replaces X-10 as Y-F."
- **As fixed with numbers at `d1f2d7f2`:** see "Acceptance, fixed before the run" above.

Root's line, as logged:

`verdict (v8y 6 / 8 Y-1 registered rule, by hand): ACCEPTED {dsr_vs_x10_positive: True (+.021481; Memmel SE .0518;
p one-sided .2980), x10_rule_vs_yf0: True (net_annual .087990 > .056538 AND net Sharpe 1.829770 >= 1.749482; dSR vs
Y-F0 -.019712), mechanics: True (6/6; gross 1.555665 in [.828796, 1.672591])}`

Sources of the deciding numbers, re-read for this report, with no recomputation:
- **dSR vs X-10:** `bundle-x10.json` `241c5bd9` `paired.dsr` 0.021480565906708105, `memmel_se` 0.051837777160737585.
  `cycle_verdict.json` `5c113f3f` `paired.dsr` is the same.
- **net_annual:** `book.json` `b85952b0` `navs.cell.net_annual` 0.08798970720884516, against `navs.yf0.net_annual`
  0.05653814276222473.
- **net Sharpe:** `navs.cell.net_sharpe` 1.8297698079085447.
- **gross:** `mech.json` `99eb2484` `navs.cell.mean_gross_leverage_all_rows` 1.5556647047690102.

## Statistics of record (S2 `modeled-1bn-stale5-v1+swap-fin-v1`; log lines 7831-7838)

- **Net Sharpe:** Y-1 1.8298, X-10 1.8083, Y-F0 1.8495.
- **Paired vs X-10:** studentized CBB, block 21, seed 20260929, 4,999 resamples, 1,005 sessions.
  - dSR +0.0215, rho .99471, Memmel SE .0518 (t .41), CBB 95% [-.0580, +.1061];
  - LW SE .0416, 95% [-.0660, +.1090];
  - bootstrap p one-sided .2980, two-sided .6326;
  - by year: 2020 -.0279, 2021 -.0456, 2022 +.1232, 2023 +.0042.
  - The bundle vs X-10 and the cycle agree on dSR, SE, CI and two-sided p.
- **Paired vs Y-F0 (printed):** dSR -0.0197, Memmel SE .0519, CBB 95% [-.1024, +.0649], p one-sided .6652, two-sided
  .6602 (`bundle-yf0.json` `4c15e0be`).
- **DSR (N 62, `--dsr-ledger`)** (`cycle_verdict.json`):
  - ledger .8251, from V[SR] 1.2957e-03 per session over 33 research-window-v2 lines;
  - effective-N .9316; legacy .9618.
- **PBO:** .1526.

## The v8y 8 report columns (`book.json` `b85952b0`; 4x from `capacity_curve.csv`; log lines 7840-7850)

| book | L (mean L_t) | all-rows gross | net annual (CAGR) | gross of cost | net Sharpe 1x | net Sharpe 4x | realised vol | max drawdown | cost bps / traded $ |
|---|---|---|---|---|---|---|---|---|---|
| Y-F0 (`lib-v8ysb-gm.json`) | 1.1828 (fixed) | .98621 | 5.654% (5.767%) | 7.068% | 1.8495 | 1.6994 | 3.057% | 2.580% | 12.66 |
| X-10 (`x-leverage-L2.0.json`) | 2.0 (fixed) | 1.66672 | 9.306% (9.606%) | 11.922% | 1.8083 | 1.5829 | 5.146% | 4.326% | 14.51 |
| Y-1 (`y-vol-target-y-1.json`) | cap 2.0; mean L_t 1.8671 | 1.55566 | **8.799%** (9.070%) | 11.196% | **1.8298** | **1.6281** | **4.809%** | **3.602%** | 14.03 |

- **Y-1 annual costs:** trade 1.563%, borrow .516%, long financing .318% (X-10: 1.722% / .554% / .340%).
- **Capacity curve and year table:** log lines 7848-7857. The capped share at 4x is .1882 for Y-1, .2047 for X-10 and
  .1004 for Y-F0.

**Consequence (log lines 7869-7872):**
- Y-1 is accepted, so **Y-1 replaces X-10 as Y-F**, the levered book offered to the owner.
- Y-F0 stays the unlevered claims book. Neither X-10 nor Y-1 is in the Sharpe claim.
- Which book is deployed is the owner's risk decision (PM7-3). OD-P9-3 keeps P9 at L <= 2.0.
- Y-1 gets no OD-3 read (YP-11: it is a `--risk-model` NAV).

## Trials and ledger (log lines 7821-7824; re-checked read-only for this report)

- **`build-equity/trials.jsonl`:** 132 -> **133 lines**.
  - file sha256 `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554`;
  - head `5a3ef9d9bf242dbb` (the sha256 of the last line), which `cycle_verdict.json` `ledger.head` repeats with 133
    lines.
- **The new line (line 133):**
  - kind construction, trial **`0ad7ea4c5122ebe3`**;
  - cell `build-equity/mega-nav-v8y-vol-target-L2.0`;
  - s2_net_sr 1.8297698079085447;
  - origin prior, count 1, window research-window-v2;
  - `prev_sha256` `15484aa1...` (X-10's head); `recorded_by.git_head` `00f0521a`.
- **N 61 -> 62 of 62** (`ledger_n`; construction cap 62: **the cap is reached, no slot left**).
- 0 admission lines were appended (61 unchanged). The stage-1 log records the ledger at 132 lines after the NAV and the
  mech reader ("Trials so far: 0").
- **How N 62 was re-checked here (grep only):**
  - 70 `"kind":"construction"` lines, each with `"count":1`;
  - 8 of them (lines 64-71) carry `"rerun_basis":"window"` and add 0 under `trial_counts`;
  - none carries `era_of`, defect, a POOL window or invalid;
  - so 70 - 8 = 62.
  - `cycle_verdict.json` `dsr.n` is 62.

## Seal (SEAL-ALLOW uses; log lines 7805-7808 and 7826-7829)

The R0-11 section passes no `--seal-allow` argument: no wave driver ran. SEAL-ALLOW was applied twice, as the
per-hit source check (`progress.md`:108).
1. **Stage 1** (scratch `sealx10.py`, wave_seal's four forms and date rule):
   - 9 files: `stdout.log`, `stderr.log` and `receipt.json` of the NAV and mech run dirs, plus the NAV, mech and
     identity-gtest console logs;
   - `2026-10-02` source (a) x4 (the png name in the runner's dirty list);
   - `2026-10-03` source (b) x3 (receipt `started_utc`);
   - **0 OTHER**.
2. **Stage 2:**
   - 32 files: every Y-1 run dir and console, the six `start.json` files and the NAV `cycle_binding.json`;
   - `2026-10-02` source (a) x18; `2026-10-03` source (b) x16;
   - the bootstrap seeds `20260927` x1 / `20260929` x72 (wave_seal.SEEDS);
   - **0 OTHER**.

The log adds: "No return, Sharpe or NAV figure has been read" before summ, and stage 1 spent 0 trials.

## Left undone by the killed agent

- **This report.** `progress.md`:167 records "root-R0-11-report.md NOT written". It is written now.
- **No end time.** The log header reads "(2026-10-03, 15:16Z-)".
- **No closing statement.** Unlike the R0-10 report, the R0-11 log section has no end-of-step statement of:
  - the hard rules;
  - the final `git status`;
  - a hand-off for the next step.
- **What the next step needs** comes from `status-2.md` instead:
  - §4 item 2 / line 33: R0-12 must re-derive X-10's and Y-1's hand verdicts independently from the registered rule
    text and the receipts / bundles, and a mismatch stops R0-12;
  - `progress.md`:157 rules this for X-10, and `status-2.md` extends it to Y-1.
- **Scratch files not in the repo.** The log cites these scratch scripts and console logs: `gate_run.py`, `mechy1.py`,
  `vty1.py`, `vty1b.py`, `sealx10.py`, and the NAV / mech / gtest / run / bundle / book consoles. They were kept in the
  killed agent's scratchpad, not in the repo. This report did not locate them. The receipts, outputs and the gtest xml
  above are on disk and hash as logged.
- Nothing else was found open. No uncommitted R0-11 file is in the tree. The spec at HEAD is the committed blob
  (`faffee17`). The ledger is at 133 lines.

## Discrepancies

**None found between the log and the four commits.**
- Each commit message's numbers match the log section it adds:
  - `d1f2d7f2`: band [.828796, 1.672591], |net| .0338, tau .3382 / .5073, P14 role `e1c67101`.
  - `ac74a48c`: 17/17, 87 passed, reference_cell `1bc1b6ca`, `--dsr-n 62`.
  - `00f0521a`: NAV receipt `cefce32d`, exe `72ff6d2d`, `mech.json` `99eb2484`, gross 1.5557, 48 estimates, mean L_t
    1.8671, 0 trials.
  - `0829f784`: dSR +.0215, net 8.80% > 5.65%, SR 1.8298 >= 1.7495, trial `0ad7ea4c5122ebe3`, head `5a3ef9d9`, N 62.
- The receipt `source_sha` values match the log's stage sources: `ac74a48c` for the NAV and mech, `00f0521a` for the
  other four.
- These files on disk hash to the values the log gives:
  - the receipts and outputs: 6 receipts, `cycle_binding.json`, `mech.json`, `summ.json`, `pbo.json`,
    `cycle_verdict.json`, both bundles, `book.json`, `summary.json`, the S2 daily CSV, `vol_target.csv` and the gtest
    xml;
  - the spec;
  - the template;
  - the ledger.
- Two timing differences, recorded and not judged: the log's NAV "launch 15:27:37Z" vs the receipt `started_utc`
  15:27:38Z, and the cycle "launch 15:32:15Z" vs the summ receipt 15:32:16Z.

**Differences from the dispatch, not between the log and the commits:**
- `git status` of pool-2 is not clean apart from the untracked png. `.superpowers/sdd/platform-p9-20261003/progress.md`
  also carries an uncommitted 2-line edit: lines 169-170, the PM session 3 "RESUME" line and this report's dispatch
  line. It was left as it is, and this commit does not include it.
- The template description says "parent = X-F0". v8y 8 / Appendix C (9) read it as Y-F0. This is a known registration
  note that the log addresses (check 2 above), not a log/commit disagreement.

## Hard rules (this report agent)

- Nothing was run: no build, no research run, no pytest, no real-data process. Files were only read and hashed.
- No code, spec, ledger or expected hash was changed. The only new file is this report.
- `C:/atx` and `atx-db/` were not touched. No push, no subagent.
- The untracked `docs/plans/2026-10-02-x5-equity-curve.png` and the PM's uncommitted `progress.md` edit were left alone.
