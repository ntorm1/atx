# Root R0-7 report: Y-3 norm-score (gm template)

Status: **DONE.** All nine stages of the Y-3 wave are done (`wave status` exit 0). The driver's verdict is **NOT
ACCEPTED**: the paired dSR is below 0 and the mechanics pass. Under PM7-34 the cell is **rejected and counted**, and N goes
57 -> 58.
- Root: `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`. Date 2026-10-03, 13:07Z-13:20Z.
- Rulings applied: R0-7-MAN, R0-7-EOL, SEAL-ALLOW (P9 `progress.md`). R0-6-ATT was not needed: no attempt failed.

## History

- **First pass: stopped before stage 01.**
  - No Y-3 wave manifest existed.
  - The template's CRLF checkout (`73d16673`) did not match its registered pin (`693c64f5`).
  - Commits: `3c42a2c8` (log) and `18e313b2` (stop report).
  - The PM then ruled R0-7-MAN, R0-7-EOL and R0-9-PIN.
- **R0-7-EOL.** For each of the four `y-*.json` templates:
  - `git diff --ignore-cr-at-eol` showed 0 lines;
  - the disk bytes with CR stripped equal the HEAD blob;
  - the file was deleted and re-checked out with `git -c core.autocrlf=false checkout --`, and now shows `i/lf w/lf`.

  | template | disk SHA-256 after the restore | pin |
  |---|---|---|
  | y-norm-score | `693c64f5` | registered |
  | y-theme-tsmom | `9d3548b6` | registered |
  | y-vol-target | `4e190f54` | registered |
  | y-two-speed | `e29365d1` | HEAD blob (R0-9-PIN) |

  All four are **equal** to their pins. No hash was edited.
- **R0-7-MAN.**
  - The draft, without the "DRAFT" prefix, was committed as `scripts/specs/v8/waves/y-3.json`: sha256
    `a49e53874a3432edcf29e492f660816b76f6de16d65635b663a0252063a1dc10`, commit **`c18a92b7`**.
  - `wave plan` on the committed file exits 0.

## Commits (in order; D = made by the driver)

| commit | what |
|---|---|
| `435c70b4` | log: R0-7-EOL, four templates |
| `c18a92b7` | `wave y-3: manifest (pre-registration of cell Y-3; v8y-prereg sections 5, 6, 14)` |
| `e6c45813` | log: manifest + `wave plan` exit 0 |
| `ababfbf4`, `192b27f8`, `0087c794` | log: stages 01, 02, 03 |
| `4f1e0e08` D | `wave y-3: rule cell y-norm-score-y-3.json on scripts/specs/v8/lib-v8ysb-gm.json` |
| `c7bf51d9` | log: stage 04 |
| `9ae7697e` | log: stage 05 |
| `5049f58f` D | `wave y-3: y-norm-score-y-3-gm.json at L 1.1645 (gross matching, PM6-6)` |
| `56063c2d`, `348d11d6`, `cec41aca` | log: stages 06, 07, 08 |
| `1e7e96be` | log: stage 09, the YP-7 hand print and the driver's log section; record copies `waves/y-3/wave-result.json` and `wave-log.md` |
| (next) | this report (`git add -f`) |

## Per stage

- Every stage ran as `wave run scripts/specs/v8/waves/y-3.json --root C:/atx-wt/pool-2 --until <stage>`.
- Host check before each stage: no compiler, no `atx-*` exe and no pool-2 research process.
- A real run needed free memory >= 3,072 MiB (cap 1,536 + 1,536).

| stage | exit | wall s | peak MiB | free before / min | receipt SHA-256 | commit | result |
|---|---|---|---|---|---|---|---|
| 01 preflight | 0 | 0.5 | - | 4,480 / 4,480 | `003e265c5436eda829785718a53c364562226b44c5616fa9ac159f00a1db323e` | - | manifest commit `c18a92b7`; template `693c64f5`; fields `26fee5ce`; ledger 128, N 57 |
| 02 register | 0 | 0.2 | - | 4,204 / 4,204 | `b06d66b2eca413b28f0a0ec5a228681ec1825481dcb3131c921ade493af3e7a8` | - | rule wave: skipped |
| 03 screen | 0 | 0.2 | - | 4,104 / 4,104 | `9d4571c0612f0419f73d59d16d8afc520da73bdf0430d708256842a62c99c4c0` | - | rule wave: skipped (0 admission) |
| 04 spec | 0 | 1.2 | - | 4,000 / 3,920 | `47fcb6e2d0fe5d6404f9a20000804087996291f51bf673cbbcc18866fc00330a` | `4f1e0e08` D | cell `y-norm-score-y-3.json` (`7e38a595`) |
| 05 run | 0 | 56.0 | 586 | 3,724 / 3,033 | `3aa00ea5da844f806e83a667e7a3fd19144d661669207d7021c1b73311bd86d2` | - | calibration NAV at L 1.1828, 54.6 s; u, fit, w and card are the parent's |
| 06 match | 0 | 53.8 | 586 | 4,021 / 2,353 | `6d0b78fb726803bcaf62c779725089681893e4e1b342ba7be48d804bdf2c7cf2` | `5049f58f` D | G 1.00169 vs .98621 -> **L 1.1645**; matched G .98619 |
| 07 verify | 0 | 0.5 | - | 3,496 / 3,380 | `63dfb015ab2ce900f94123d9887dadf276e3e2a4cc3bd6aba425b4c8670e55ce` | - | sealsrc: 6 hits, (a) x4 and (b) x2, 0 OTHER. Mechanics **PASS** 6/6; seal scan 30 logs, 0 tokens; NAV exe `72ff6d2d` = parent's |
| 08 judge | 0 | 40.2 | 580 (summ) | 3,819 / 3,133 | `7f4964cd2a55bf437d36404ba79eef2dbbbae14438dad4221ceb098b9d5a9659` | - | `verdict (pm7-34): NOT ACCEPTED {'dsr_positive': False, 'mechanics': True, 'criteria': False}` |
| 09 record | 0 | 0.5 | - | 3,927 / 3,927 | `3712c174053ee4cc4c13a40f2ba7131ff08ff65444d561031f901e7c7ffed522` | - | sealsrc: 11 hits, (a) x7 and (b) x4, 0 OTHER. `wave-result.json` `619809ea396b...`; N 57 -> 58 |

## The cell's outcome (the driver's own `wave-log.md`, quoted)

- **Verdict:** "**Verdict (pm7-34: paired S2 net dSR > 0 AND mechanics (PM7-34); criteria printed, decide nothing): NOT
  ACCEPTED** {'criteria': False, 'dsr_positive': False, 'mechanics': True}".
- **Criteria (printed):** capacity-4x-higher unmet; turnover-per-gross-not-higher unmet; cost-bps-lower unmet.
- **Cell:** `scripts/specs/v8/y-norm-score-y-3-gm.json` (rule `nav --rank-shape norm-score-v1` on v8ysb).
  - Gross matching (PM6-6): calibration at L 1.1828 gave G 1.0016936049, so one correction to L 1.1645, G .9861866382.
- **Mechanics (S2):** PASS.
  - gross .98619; abs net .00574; tau mean .02933; p95 .03473.
  - Accounting errors 3.6e-16 and 8.4e-14.
- **Statistics of record (S2):**
  - net Sharpe 1.8119 vs parent 1.8495;
  - dSR -0.0376, Memmel SE 0.0401, CBB 95% [-0.106, 0.028];
  - bundle p one-sided 0.8644, two-sided 0.2690;
  - DSR (N 58) 0.8221; PBO 0.0765.
- **Returns (S2, annual):**
  - net 5.83% (CAGR 5.95%) vs 5.65%; gross of cost 7.29% vs 7.07%;
  - vol 3.22%; max drawdown 2.70%;
  - 4x net Sharpe 1.6874 vs 1.6994; tau .02933 (per unit gross .02974).
- **Next parent (driver):** `scripts/specs/v8/lib-v8ysb-gm.json`, library v8ysb.

## YP-7 hand print: Y-3's registered criterion (print-only; decides nothing)

S2 gross-of-cost annual return per unit of all-rows gross. Source: `wave-result.json` `stats`, `gross_annual` /
`mean_gross_leverage_all_rows`.
- Cell: .0729263299 / .9861866382 = **.0739477976**.
- Parent: .0706781735 / .9862134133 = **.0716662058**.
- Difference +.0022816: **above the parent's (met)**.

Also printed, cell vs parent:

| item | cell | parent |
|---|---|---|
| net annual | 5.834% | 5.654% |
| realised vol | 3.220% | 3.057% |
| turnover per unit gross | .029742 | .028528 |
| cost per traded dollar (bps) | 12.708 | 12.658 |
| borrow (per year) | .3306% | .3276% |
| 4x net Sharpe | 1.6874 | 1.6994 |

`construction.rank_shape` was active in every scenario: norm-score-v1, max |z| 3.2806 (the template predicted about
3.3), 1,004 scored decisions, mean 1,846.3 names.

**Reading:** at matched gross, the rule earned more gross return per unit of gross, as hypothesised. Higher vol, turnover
and cost took more than that gain, so the net Sharpe fell. The verdict stands by PM7-34.

## Trial count

- Ledger `build-equity/trials.jsonl`: 128 -> **129 lines** (head `a083edaa836b69ef`).
- **One construction trial added**: the cell, `ee5487109c705108` (s2_net_sr 1.81193). **N 57 -> 58** (construction cap 62).
- 0 admission trials.
- The calibration run, the -gm calibration step and the manifest work added 0.

## For R0-8 and later

- **Parent for R0-8 (Y-2):** unchanged, `scripts/specs/v8/lib-v8ysb-gm.json` (library v8ysb, L 1.1828), as the driver
  prints. Y-3 is rejected, so the last accepted cell is still Y-S.
- **R0-7-MAN carries over.** Y-2's manifest is written the same way:
  - template `y-theme-tsmom.json` @ `9d3548b6` (disk is now LF);
  - `n_before` 58;
  - parent as above;
  - `printed` as Y-S and Y-3;
  - the budget id updated.
- **SEAL-ALLOW:** every hit in this wave was (a) the png name or (b) a `started_utc` stamp of 2026-10-03. A run past
  00:00Z brings a new (b) date.
- **Host:** the lowest free memory was 2,353 MiB, during the stage-06 matched NAV. Other lanes' pytest was running at
  the time; it was not blocking.

## Hard rules

- Nothing was built. Every data run was launched by the driver.
- Nothing dated 2024-01-01 or later was opened. Results were read only from TRAIN outputs, after the mechanics passed;
  seal tokens were classified by source only.
- `C:/atx` and `atx-db/` were not touched.
- No push, no worktree prune, no expected hash edited, no code changed, no subagents.
- The only working-tree change outside commits is the ruled LF restore of four templates, after which `git status` was
  clean. The untracked `docs/plans/2026-10-02-x5-equity-curve.png` was left as it is.
