# Root R0-9 report: Y-5 two-speed

Status: **DONE.** R0-9-PIN and P13 both pass. All nine stages of the Y-5 wave are done (`wave status` 9/9). The driver's
verdict is **NOT ACCEPTED**: the paired dSR is below 0 and the mechanics pass. Under PM7-34 the cell is **rejected and
counted**, and N goes 59 -> 60.
- Root: `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`. Date 2026-10-03, 13:49Z-14:41Z.
- Rulings applied: R0-9-PIN, R0-7-MAN, R0-7-EOL (done in R0-7), SEAL-ALLOW. R0-6-ATT was not needed: no attempt failed.
  DEC-16 was context only.
- No stop condition fired:
  - the PIN diff showed only `description`;
  - P13 passed;
  - no validator refused;
  - the seal scan found 0 OTHER sources;
  - the parent was not ambiguous;
  - the driver redid no completed stage.

## R0-9-PIN

- **Registered:** `git show e2ac7d63:scripts/specs/v8/y-two-speed.json` has SHA-256
  `69cf613457ff5b474d56269ff200d3d0211b5f06a0e67162e03b9b3c06b3291e`, which is v8y §14's pin.
- **HEAD and disk:** `e29365d1597a31ba659996120ec2fc05f8d3947419249e4bbe945736533ca023`, `i/lf w/lf`.
  - The only later commit is `b3b5dab4` (2026-10-02 21:26 -0400).
- **JSON diff:** each file has 12 leaf paths; only `/description` differs (3,191 -> 4,286 chars).
  - The top-level key order is equal, and the files are equal once `description` is removed.
  - `git diff` shows 1 line changed.
- The manifest pins `e29365d1`. Both SHAs are logged, and so is the description-only diff.

## Parent

- **The registration** (`parent: null`, "the last accepted book ... whose fit carries a standardised composition") gives
  `lib-v8ysb-gm.json`:
  - Y-S is the last accepted cell;
  - its fit has `theme_standardise` with rerank true, and no residualise, schedule or sleeves.
- **The driver** gives the same: Y-2 wave-result `09fbd24e` `next_parent`. **They agree.**
- **Y-5 is defined on this parent** (v8y §6 "Undefined"):
  - the NAV is aim-partial-v5 with a fixed `--trade-fraction .05`;
  - there is no `--hold-band`, `--vol-scale` or `--adv-hold-q`;
  - all 12 weighted themes are in the table (`merger_arbitrage` 126, `0ad71615`, which is in the v8-16d source);
  - the fast bucket holds reversal_seasonality, so neither bucket is empty.

## P13 evidence (v8y §13): PASS, before the cell's NAV

The P5 build is v8-16d: ic `985019d9`, targets `72ff6d2d`, and ic-tests `3393a953` from v8-16e.

**(a) Flag absent.** The parent's receipt argv was re-run under the bounded runner, with only `--output` renamed and the
same binds and limits.

| pass | run dir | receipt | result |
|---|---|---|---|
| fit | `p9-r09-p13-fit-run` | `e099fad9` | 3/3 byte-identical (`admission.csv`, `admission.json` `121f7046`, `composition_weights.json` `0c480773`) |
| w | `p9-r09-p13-w-run` | `3983802f` | 10/12 byte-identical (the six `train_combined.*`, `train_daily_ic.csv`, `orientations.json`, `recipe.json`, `train_planned_targets.csv`). `summary.json` and `train_candidates.jsonl` differ only in `stage_seconds`, `wall_seconds` and `hash_seconds` |
| NAV | `p9-r09-p13-nav-run` | `c8620524` | 27/27 byte-identical |

The first w try waited 900 s and its 4,322 MiB gate was never met, so nothing launched. The retry met the gate after
647 s.

**(b) `TwoSpeedRunner.*`** passes 2/2 on `atx-impl-strategy-ic-tests.exe` `3393a953`. The xml is
`build-equity/p9-r09-p13-twospeedrunner.xml` (`08412bc0`).

**(c) The cell's own w pass.**
- **How it ran.** After stage 04 wrote the copy with `parent` `lib-v8ysb-gm.json`, root ran `research_cycle.py run
  scripts/specs/v8/y-two-speed-y-5.json --stop-after w`. That is the strict prefix of stage 05's own `--stop-after nav`.
  - It ran fit `82632867`, card `9d3544ee` and w `8a40eb39` (62.2 s, 1,559 MiB).
  - It appended 0 admission lines.
- **Compared with the parent's w** (`mega-v8xw-train-theme-erc-v8ysb-2`):
  - byte-identical: `train_combined.f64` `70d863f9`, `_finite.u8`, `_member.u8`, `_ids.u64`, `_sessions.i64` and
    `train_planned_targets.csv` `cb744a74`;
  - the 2,926 `__combined__` rows are identical; so is the whole `train_daily_ic.csv`;
  - `train_combined.json`: the same 5-file set; it differs only in `composition_sleeves` (which pins
    `train_sleeves.json` `4f536da8`), `composition_weights_sha256` and `run_recipe_sha256`;
  - `recipe.json` differs only in `composition_sleeves` and `composition_weights_sha256`.
- **The cell fit** differs from the parent's fit only by the added `/theme_sleeves/rule`.
- **Stage 05 then adopted** this fit, card and w and ran only the calibration NAV.

## Commits (in order; D = made by the driver)

| commit | what |
|---|---|
| `73e50d6a` | log: R0-9-PIN diff, parent check, P13 plan |
| `85130016` | log: P13 (a) and (b) results |
| `43128be8` | `wave y-5: manifest (pre-registration of cell Y-5; v8y-prereg sections 5, 6, 13 P13, 14)`. File `scripts/specs/v8/waves/y-5.json`, sha256 `f538f9f590570d3cb80f31aa9e3e8dd1f38fd3b1cb168cc738756ad4da762e89`. `wave plan` exits 0 both before and after the commit |
| `c02e967c` | log: manifest and plan |
| `d52a8ea4`, `8d2c1c27`, `119aef97` | log: stages 01, 02, 03 |
| `875fcc0f` D | `wave y-5: rule cell y-two-speed-y-5.json on scripts/specs/v8/lib-v8ysb-gm.json` |
| `ef1b77ac` | log: stage 04 |
| `1b0726c6` | log: P13 (c) PASS |
| `74e0a0a5` | log: stage 05 |
| `ebeadabe` D | `wave y-5: y-two-speed-y-5-gm.json at L 1.2658 (gross matching, PM6-6)` |
| `8e12f9b0`, `6e449864`, `2a5f2b1c` | log: stages 06, 07, 08 |
| `a063ad51` | log: stage 09, the printed-only items and the driver's log section; record copies of `waves/y-5/wave-result.json` and `wave-log.md` |
| (next) | this report (`git add -f`) |

All logs are in `.superpowers/sdd/platform-v8-20260929/integration-log.md`, section "R0-9 Y-5 two-speed".

## Per stage

- Every stage ran as `wave run scripts/specs/v8/waves/y-5.json --root C:/atx-wt/pool-2 --until <stage>`.
- Host check before each stage. Free memory was sampled every 0.25 s.
- Memory gates:
  - **05:** 3,072 MiB. Only the NAV remained, because P13 (c) had already run fit, card and w under a 4,322 MiB gate.
  - **06 and 08:** 3,072 MiB.

| stage | exit | wall s | peak MiB | free before / min | receipt SHA-256 | commit | result |
|---|---|---|---|---|---|---|---|
| 01 preflight | 0 | 0.4 | - | 4,688 / 4,665 | `c1d7ad9ab16cd9402de476490112880ee1a09627444b32073669181121de70e9` | - | manifest `43128be8`; template `e29365d1`; fields `26fee5ce`; ledger 130, N 59 |
| 02 register | 0 | 0.1 | - | 4,604 / 4,604 | `05b1cd155d78f9f2a6de5f6fc80032088efa279ec1b516ac66e8644f2bdf17fa` | - | rule wave: skipped |
| 03 screen | 0 | 0.1 | - | 4,597 / 4,597 | `8b01c8bfd13700c65a2f72f937b6a4c3cc35f967e9dc3dfc14498d0a46880747` | - | rule wave: skipped (0 admission) |
| 04 spec | 0 | 0.6 | - | 4,573 / 4,526 | `fff1031e9d253f7a6af8477a32c6873fe99f8ace3837ed4b10ec4207f2eecaff` | `875fcc0f` D | cell `y-two-speed-y-5.json` (`b2671dff`); parent set |
| 05 run | 0 | 80.1 | 713 (nav) | 5,131 / 4,040 | `fdc99f643c9939f6972870cebbf8412342e0efa64ce286bb2f2b249c59154ba3` | - | P13 (c)'s fit, card and w adopted; calibration NAV at L 1.1828, 78.9 s |
| 06 match | 0 | 85.1 | 713 | 5,062 / 3,931 | `6e7cb5b0813368bc2a86cd5c1a36274eac637c6417e6fdd2b5ba016a69085954` | `ebeadabe` D | G .92152 vs .98621 -> **L 1.2658**; matched G .98629 |
| 07 verify | 0 | 0.3 | - | 4,683 / 4,660 | `997d151a41236165bf78585027501c4177b7d9be37323797c4678e04e0974c89` | - | sealsrc: 6 hits, (a) x4 and (b) x2, 0 OTHER. Mechanics **PASS** 6/6; seal scan 26 logs, 0 tokens; NAV exe `72ff6d2d` = parent's |
| 08 judge | 0 | 43.0 | 571 (summ) | 4,716 / 3,956 | `687283dd822c86b8aa2ffb78d3a952b12e9f5d437ba3b80f38c14d8d7e8128cc` | - | `verdict (pm7-34): NOT ACCEPTED {'dsr_positive': False, 'mechanics': True, 'criteria': False}` |
| 09 record | 0 | 0.5 | - | 4,265 / 4,230 | `a41d4dbdafada006e0ee32fdce3988a8e391792eb2d5d57cb1755c71c2573bbe` | - | sealsrc: 11 hits, (a) x7 and (b) x4, 0 OTHER. `wave-result.json` `ec4e07c29aae...`; N 59 -> 60 |

Root's own run dirs (`p9-r09-p13-*`) and the cell's run dirs were scanned the same way: 38 files, 0 OTHER.

## The cell's outcome (the driver's own `wave-log.md`, quoted)

- **Verdict:** "**Verdict (pm7-34: paired S2 net dSR > 0 AND mechanics (PM7-34); criteria printed, decide nothing): NOT
  ACCEPTED** {'criteria': False, 'dsr_positive': False, 'mechanics': True}".
- **Criteria (printed):** capacity-4x-higher unmet; turnover-per-gross-not-higher **met**; cost-bps-lower **met**.
- **Cell:** `scripts/specs/v8/y-two-speed-y-5-gm.json` (rule `fit` + `nav --two-speed two-speed-v1` on v8ysb).
  - Gross matching (PM6-6): calibration at L 1.1828 gave G .9215182646, so one correction to L 1.2658, G .9862947654.
- **Mechanics (S2):** PASS.
  - gross .98629; abs net .00528; tau mean .02608; p95 .03182.
  - Accounting errors 3.7e-16 and 3.9e-14.
- **Statistics of record (S2):**
  - net Sharpe 1.7294 vs parent 1.8495;
  - dSR -0.1201, Memmel SE 0.0753, CBB 95% [-0.265, 0.040], rho .9889;
  - bundle p one-sided 0.9316, two-sided 0.1398;
  - DSR (N 60) 0.7804; PBO 0.1509.
- **Returns (S2, annual):**
  - net 5.36% (CAGR 5.45%) vs 5.65%; gross of cost 6.71% vs 7.07%;
  - vol 3.10%; max drawdown 2.67%;
  - 4x net Sharpe 1.6023 vs 1.6994; tau .02608 (per unit gross .02644).
- **Next parent (driver):** `scripts/specs/v8/lib-v8ysb-gm.json`, library v8ysb.

## Printed-only items of the registration (v8y §6 Y-5 row; decide nothing)

Cell vs parent, from `wave-result.json` `stats` (S2):

| item | cell | parent |
|---|---|---|
| turnover per unit gross | .026441 | .028528 |
| cost per traded dollar (bps) | 12.648 | 12.658 |
| net Sharpe at 4x NAV | 1.6023 | 1.6994 |
| net annual return | 5.357% | 5.654% |

**Fast mass share.** Source: `train_sleeve_fast_share.f64` (`f38407e8`), pinned by `train_sleeves.json`. The only fast
theme is reversal_seasonality.
- Over the 1,006 TRAIN dates of 2020-2023 it is constant at **.120861**.
- Over all 1,405 role dates the mean is .116307: 63 dates are 0 (no theme present, PM8-16 #9), and the rest lie in
  .1209-.1267.

**NAV `construction.two_speed`.** The block is the same in all 5 scenarios:
- cadence 1; theta_fast .129449; theta_slow .05;
- rebalances skipped by a sleeve 0; parent rebalances skipped 0; parent constructions failed 0.

**Reading:** the rule did what it is built to do. Turnover per unit gross fell by 7%, and the cost per traded dollar
fell slightly. To reach the parent's gross it needed L 1.2658, and its gross return fell (6.71% vs 7.07%) by more than
the cost it saved. The verdict stands by PM7-34.

## Trial count

- Ledger `build-equity/trials.jsonl`: 130 -> **131 lines** (head `6c5f0ff1faca1a1b`).
- **One construction trial added**: the cell, `cc150c210a3f98e4` (s2_net_sr 1.72936). **N 59 -> 60** (construction cap
  62; 2 slots left).
- 0 admission trials.
- These added 0: the P13 re-runs, the P13 (c) prefix run, the calibration NAV and the manifest work.

## For R0-10 (X-10, L 2.0 on Y-F0)

- **Y-F0** is the last accepted cell after Y-5 (v8y §1): Y-S's `scripts/specs/v8/lib-v8ysb-gm.json` (library v8ysb, L_P
  1.1828, NAV `mega-nav-v8x-theme-erc-L1.1828-v8ysb`, S2 daily `73b69bcc`). The driver's `next_parent` says the same.
- **X-10 is defined:** L_P 1.1828 < 2.0, so r = 2.0 / 1.1828 = 1.6909.
- **The scaled mechanics** (v8y §8, PM7-17) must be written into the plan before the run:
  - gross [.90, 1.05] x r = [1.5218, 1.7754];
  - |mean net| <= .0338;
  - tau mean <= .3382; p95 <= .5073.
- **Not gross-matched**, with a new manifest by R0-7-MAN; `n_before` 60.
- **SEAL-ALLOW** stands through R0-11.

## Hard rules

- Nothing was built. Every data run went through the bounded runner, `research_cycle.py` or the wave driver.
- No code was changed and no subagent was used.
- Nothing dated 2024-01-01 or later was opened:
  - P13 was compared by SHA-256 and JSON paths only;
  - results and the printed items were read only from TRAIN outputs, after the mechanics passed.
- `C:/atx` and `atx-db/` were not touched.
- No push, no worktree prune, no expected hash edited.
- `git status` is clean apart from the untracked `docs/plans/2026-10-02-x5-equity-curve.png`, which was left as it is.
