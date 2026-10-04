# Root R0-8 report: Y-2 theme-tsmom

Status: **DONE.** All nine stages of the Y-2 wave are done (`wave status` exit 0). The driver's verdict is **NOT
ACCEPTED**: the paired dSR is below 0 and the mechanics pass. Under PM7-34 the cell is **rejected and counted**, and N goes
58 -> 59.
- Root: `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`. Date 2026-10-03, 13:27Z-13:39Z.
- Rulings applied: R0-7-MAN, R0-7-EOL (already done in R0-7), SEAL-ALLOW (P9 `progress.md`). R0-6-ATT was not needed:
  no attempt failed.
- No stop condition fired: no validator refused, no SHA pin mismatched, the seal scan found no source outside
  SEAL-ALLOW, the driver redid no completed stage, and the registration determined every manifest key.

## Pin and manifest (R0-7-MAN)

- **Template pin.** `scripts/specs/v8/y-theme-tsmom.json` on disk has SHA-256
  `9d3548b670547d138b3fdaeec0f170a2861b2a60d777c94788446e6c9b4cd83f`. That **equals** the registered pin (v8y §14,
  template @ `43745dd7`, which is its only commit). `git ls-files --eol` shows `i/lf w/lf`.
- **Y-2 is defined on the parent.** The fit of `lib-v8ysb-gm.json` is `--composition ew-theme-std-v1 --theme-erc
  theme-erc-v1`, with no `--theme-resid` and no `--era` (v8y §6, "Undefined" Y-2 row).
- **Manifest** `scripts/specs/v8/waves/y-2.json`, sha256
  `3655181d316f65489064576e763cd01955ddb9ac9dce7a94deea8872ec19b538`, commit **`15017bb4`** (`wave y-2: manifest
  (pre-registration of cell Y-2; v8y-prereg sections 5, 6, 14)`).
  - **From the registration:** `rule_cell` = the template and its pin. Its constants (lookback 252, lag 3, step 21,
    `fit --theme-tsmom theme-tsmom-v1`) are already the template's own flag, so the manifest has no `constants` key.
    `gross_match` pm6-6 (v8y §6: PM6-6 applies to Y-2).
  - **From the chain rule:** `parent` = the Y-3 wave-result's `next_parent`, `lib-v8ysb-gm.json` / v8ysb.
  - **Copied from `y-3.json`:** `schema`, `fields`, `acceptance` (pm7-34, the same three printed criteria),
    `construction_cap` 62, `ledger`, `record`.
  - **Per-wave values:** `wave` y-2, `n_before` 58, `budget.id` v8y-construction-n58-plus-1, `out_dir`.
  - The description states PM8-10 (c)'s dSR SE window and CM-2. There is no free constant.
- `wave plan` exits **0**, both before and after the commit (9 stages pending; cell file
  `scripts/specs/v8/y-theme-tsmom-y-2.json`).

## Commits (in order; D = made by the driver)

| commit | what |
|---|---|
| `15017bb4` | `wave y-2: manifest (pre-registration of cell Y-2; v8y-prereg sections 5, 6, 14)` |
| `3ccb8538` | log: manifest + `wave plan` exit 0 |
| `c8283fcf`, `996d889d`, `a42a452d` | log: stages 01, 02, 03 |
| `519abce7` D | `wave y-2: rule cell y-theme-tsmom-y-2.json on scripts/specs/v8/lib-v8ysb-gm.json` |
| `c3b972c8` | log: stage 04 |
| `9798742e` | log: stage 05 |
| `31250f46` D | `wave y-2: y-theme-tsmom-y-2-gm.json at L 1.2038 (gross matching, PM6-6)` |
| `be6b99dd`, `fd6c79cc`, `785ab5f5` | log: stages 06, 07, 08 |
| `d9e337b6` | log: stage 09, the dSR SE window, CM-2, the printed-only items and the driver's log section; record copies of `waves/y-2/wave-result.json` and `wave-log.md` |
| (next) | this report (`git add -f`) |

All logs are in `.superpowers/sdd/platform-v8-20260929/integration-log.md`, section "R0-8 Y-2 theme-tsmom".

## Per stage

- Every stage ran as `wave run scripts/specs/v8/waves/y-2.json --root C:/atx-wt/pool-2 --until <stage>`.
- Host check before each stage: no compiler, no `atx-*` exe and no research process. Free memory was sampled every
  0.25 s.
- Memory gates:
  - **05:** 4,322 MiB = the w plan of 2,786 + 1,536. This is the R0-6-ATT gate for the same library. The runner waited
    117 s for it.
  - **06 and 08:** 3,072 MiB = the 1,536 cap + 1,536.

| stage | exit | wall s | peak MiB | free before / min | receipt SHA-256 | commit | result |
|---|---|---|---|---|---|---|---|
| 01 preflight | 0 | 0.3 | - | 4,275 / 4,248 | `a7f876c13100abae6e73d82581d092f2c50cfc98fcd66b8cfa41401f642c62f3` | - | manifest commit `15017bb4`; template `9d3548b6`; fields `26fee5ce`; ledger 129, N 58 |
| 02 register | 0 | 0.1 | - | 4,219 / 4,219 | `87a9551cc61431be410584d90df0065c13facf352cf97a5d0b77a4aeae7bbc0a` | - | rule wave: skipped |
| 03 screen | 0 | 0.1 | - | 4,078 / 4,078 | `15ea31c51f4bb7eaf5bc109e3bf72da8fbe8c9b93bea586c57ffa25501058eff` | - | rule wave: skipped (0 admission) |
| 04 spec | 0 | 0.7 | - | 4,060 / 4,014 | `ccf78090e9af5fb1a60d077bea26f29dcc40e67a643287d1ea4d10b78d10debe` | `519abce7` D | cell `y-theme-tsmom-y-2.json` (`5e9fa011`) |
| 05 run | 0 | 106.9 | 1,432 (w) | 4,339 / 2,862 | `309b66eda38dc61c3bb106ccf5f0025ef1a735dec5dfe01404d75292ac11cb67` | - | calibration at L 1.1828: new fit 0.8 s, w 42.2 s / 1,432 MiB, NAV 43.5 s, card 18.2 s; u is the parent's |
| 06 match | 0 | 50.2 | 586 | 4,812 / 3,804 | `8d8aa008d97d5bbbc3385c0b3d0a235db540ccd1dd19f8a3ffa161b9b23fb099` | `31250f46` D | G .96898 vs .98621 -> **L 1.2038**; matched G .98623 |
| 07 verify | 0 | 0.3 | - | 4,547 / 4,510 | `60dd5f657f9a443212485e0a452007287f94b8ce474b714ecb7d150005e97482` | - | sealsrc: 6 hits, (a) x4 and (b) x2, 0 OTHER. Mechanics **PASS** 6/6; seal scan 26 logs, 0 tokens; NAV exe `72ff6d2d` = parent's |
| 08 judge | 0 | 39.1 | 609 (summ) | 4,566 / 3,891 | `d8096f2d7319e73821d32100d4be82c846d057623f5f0eacb54c74d3c739d356` | - | `verdict (pm7-34): NOT ACCEPTED {'dsr_positive': False, 'mechanics': True, 'criteria': False}` |
| 09 record | 0 | 0.4 | - | 4,330 / 4,297 | `06c23eae60666912a6ac46db6b6882282baa440f50cc684e0f630ffb5052a538` | - | sealsrc: 11 hits, (a) x7 and (b) x4, 0 OTHER. `wave-result.json` `09fbd24ec7d1...`; N 58 -> 59 |

## The cell's outcome (the driver's own `wave-log.md`, quoted)

- **Verdict:** "**Verdict (pm7-34: paired S2 net dSR > 0 AND mechanics (PM7-34); criteria printed, decide nothing): NOT
  ACCEPTED** {'criteria': False, 'dsr_positive': False, 'mechanics': True}".
- **Criteria (printed):** capacity-4x-higher unmet; turnover-per-gross-not-higher unmet; cost-bps-lower **met**.
- **Cell:** `scripts/specs/v8/y-theme-tsmom-y-2-gm.json` (rule `fit --theme-tsmom theme-tsmom-v1` on v8ysb).
  - Gross matching (PM6-6): calibration at L 1.1828 gave G .9689836621, so one correction to L 1.2038, G .9862340550.
- **Mechanics (S2):** PASS.
  - gross .98623; abs net .00484; tau mean .03022; p95 .04759.
  - Accounting errors 4.4e-16 and 3.9e-14.
- **Statistics of record (S2):**
  - net Sharpe 1.5182 vs parent 1.8495;
  - dSR -0.3313, Memmel SE 0.2307, CBB 95% [-1.057, 0.230];
  - bundle p one-sided 0.8794, two-sided 0.3976;
  - DSR (N 59) 0.6457; PBO 0.1511.
- **Returns (S2, annual):**
  - net 4.79% (CAGR 4.85%) vs 5.65%; gross of cost 6.25% vs 7.07%;
  - vol 3.15%; max drawdown 3.39%;
  - 4x net Sharpe 1.3498 vs 1.6994; tau .03022 (per unit gross .03064).
- **Next parent (driver):** `scripts/specs/v8/lib-v8ysb-gm.json`, library v8ysb.

## dSR SE and its window (PM8-10 (c); printed, decides nothing)

- **dSR -0.3313, Memmel SE 0.2307** (CBB 95% [-1.057, 0.230]; rho .8951).
- **What it covers:** the driver's bundle computes these over the 1,005 paired S2 return sessions of TRAIN, 2020-01-03
  to 2023-12-29 (the ledger line's `window`).
- **The rule's window is 2021-2023:**
  - The first schedule block starts at decision 254 (session 2021-01-05), and the last at decision 989 (2023-12-06).
  - Before decision 254, i.e. in 2020, the theme masses are the parent's. In 2020 the cell therefore differs from the
    parent only by its matched L (1.2038 vs 1.1828).
- The SE is large, as YCOMB's report predicted: the cell starts in 2021, with 12 themes and 36 blocks.

## CM-2

Y-2's sleeves use the parent's full-TRAIN weights, and out of sample the rule is a frozen mask.
- **The sleeve weights are fixed.** Y-2 forms its theme sleeves with the parent's final full-TRAIN weights (the
  theme-erc-v1 covariance and cap). So "no mean is fitted / walk-forward" holds only for the mass schedule, not for the
  sleeve weights.
- **Out of sample, the rule does not move.**
  - A role after TRAIN keeps the last block's masses.
  - A pre-TRAIN history role gets the parent's masses.
- **The registration stands (PM8-12 (e)):** a delivered Y lane is not rewritten before its cell runs.
  - The cell ran as registered, on TRAIN only.
  - No out-of-sample read was made.
  - The long-term fix is DEC-9 / lane D3 (walk-forward refit, and sleeves computed in C++ at apply time).

## Printed-only items of the registration (v8y §6 Y-2 row; decide nothing)

**From the schedule.** Source: the cell fit's `composition_weights.json` (`5f94c607`), `provenance.theme_tsmom`. The
fit has 1,004 decisions, 36 blocks and 12 themes.
- **theme_blocks_off:** 141 of 432 theme-blocks (between 2 and 7 per block).
- **theme_on_fraction** by theme:

  | theme | on fraction |
  |---|---|
  | reversal_seasonality | 1.000 |
  | investment_issuance | .944 |
  | profitability_quality | .944 |
  | value | .944 |
  | low_risk | .778 |
  | ownership_flow | .722 |
  | short_interest | .694 |
  | price_momentum | .667 |
  | earnings_momentum | .500 |
  | options_implied | .333 |
  | filing_events | .306 |
  | merger_arbitrage | .250 |

**From `wave-result.json` `stats`** (S2), cell vs parent:

| item | cell | parent |
|---|---|---|
| turnover per unit gross | .030641 | .028528 |
| cost per traded dollar (bps) | 12.614 | 12.658 |
| net annual return | 4.787% | 5.654% |
| 4x net Sharpe | 1.3498 | 1.6994 |

## Trial count

- Ledger `build-equity/trials.jsonl`: 129 -> **130 lines** (head `58bce60efae09d44`).
- **One construction trial added**: the cell, `aeeb2073e0bd8e2a` (s2_net_sr 1.51822). **N 58 -> 59** (construction cap 62).
- 0 admission trials.
- The calibration run, the -gm calibration step and the manifest work added 0.

## For R0-9

- **Parent for R0-9 (Y-5):** unchanged, `scripts/specs/v8/lib-v8ysb-gm.json` (library v8ysb, L 1.1828), as the driver
  prints. Y-2 is rejected, so the last accepted cell is still Y-S.
- **R0-7-MAN carries over** to the Y-5 manifest:
  - template `y-two-speed.json` @ `e29365d1` (R0-9-PIN: first confirm by a JSON diff that only `description` differs
    from the registered `69cf6134`);
  - `n_before` 59;
  - budget id `v8y-construction-n59-plus-1`;
  - other keys as in Y-3 and Y-2.
- **P13 comes before Y-5.**
- **SEAL-ALLOW:** every hit in this wave was (a) the png name or (b) a `started_utc` stamp of 2026-10-03.
- **Host:**
  - Free memory hovered around 4,000-4,340 MiB, so stage 05 waited 117 s for its 4,322 gate.
  - The lowest free memory was 2,862 MiB, during the stage-05 calibration chain (w peak 1,432 MiB).

## Hard rules

- Nothing was built. Every data run was launched by the driver.
- Nothing dated 2024-01-01 or later was opened.
  - Results were read only from TRAIN outputs, and only after the mechanics passed.
  - Seal tokens were classified by source only.
- `C:/atx` and `atx-db/` were not touched.
- No push, no worktree prune, no expected hash edited, no code changed, no subagents.
- `git status` is clean apart from the untracked `docs/plans/2026-10-02-x5-equity-curve.png`, which was left as it is.
