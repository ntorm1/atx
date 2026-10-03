# Root R0-14 report: final v8 report (status 8), P9's `n_before`, Phase-0 exit gate, P9 branch cut

Status: **DONE.**
- Status 8 is written: `docs/plans/2026-10-03-platform-v8-status-8.md`.
- The ledger state is recorded as P9's `n_before`.
- The Phase-0 exit gate (plan §4.3) is **met**, clause by clause.
- `wave status` shows every Y wave complete.
- `feat/platform-p9-20261003` is cut from the commit that adds this report.

Context:
- Root `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, HEAD at start `32b33236`. Date 2026-10-03, from 16:41Z.
- Rulings applied: P9L 185 ("DSR up": three readings, labelled, no claim), P9L 186 (PBO: both values), OD-P9-1
  placeholder (P9L 14), OD-P9-3 (P9L 18), E1-STALE (P9L 141).
- 0 trials. Nothing was built, and no research run or pytest ran.

## Read

- **Plan** `docs/plans/2026-10-03-p9-sprint-plan.md`: row R0-14 (line 185), §0.3 (G-B1, G-B2), §4.3 phase 0 (line 716),
  §5.1 (lines 737-744), §5.2 ceiling (line 757), §6 OD-P9-1..3.
- **P9 `progress.md`:** line 1-40 (pre-execution rulings) and 115-189 (R0-6 to the R0-14 dispatch).
- **Reports:** `root-R0-6-report.md` .. `root-R0-12-report.md`, and `status-2.md`.
- **v8 integration log** sections R0-10, R0-11 (stage 2) and R0-12 (lines 7520-7613, 7809-8045). The R0-6 .. R0-9
  sections were read through their reports and by grep for the `wave status` / record rows.
- **v8y-prereg** §1-§8 and YP-11.
- **Status 7** `docs/plans/2026-10-02-platform-v8-status-7.md` (format and location).

## Re-checks (read-only)

- **Hashes.** Every file the status-8 report cites was re-hashed and equals its logged prefix:
  - book readers `642ad29b`, `d222b547`, `b85952b0`;
  - mechanics `c397ac3f`, `603b5afb`, `99eb2484`;
  - `dsr.json` `fdb6a972`, `dsr-run1/stdout.log` `cd1a6375`;
  - bundles `803e5456`, `f3098044`, `24f6479b`, `a57d82d0`, `241c5bd9`, `4c15e0be`;
  - PBO `2beb256c`, `556192c0`;
  - wave results `57e9f5ea`, `619809ea`, `09fbd24e`, `ec4e07c2`;
  - Y-F0 `capacity_curve.csv` `edf1001d`.
- **The three books.** `p9-r12-adopt/book.json` was re-read for Y-F0, X-10 and Y-1 (net_annual, net_sharpe,
  x4_net_sharpe, ann_vol, max_drawdown, mean gross, cagr). Every digit equals R12 §7.
- **Trial ledger** `build-equity/trials.jsonl`:
  - **133 lines**; sha256 `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554` (prefix **`27e40f9f`**,
    confirmed); chain head `5a3ef9d9bf242dbb`.
  - Lines 128-133 are the six Y-program cells (kind construction, count 1): Y-S `11c10defb3cf38a5`, Y-3
    `ee5487109c705108`, Y-2 `aeeb2073e0bd8e2a`, Y-5 `cc150c210a3f98e4`, X-10 `25f3b27aae4754ab`, Y-1 `0ad7ea4c5122ebe3`.
  - A scratch script (not committed) ran `backtest_integrity.trial_counts` / `ledger_n` and printed aggregates only.
  - It printed no line content and no window string.
  - The ledger was unchanged after every R0-14 command.

## P9's `n_before` (also status 8 §5)

| count | value | how counted | source | plan §5.1 projection | vs projection |
|---|---|---|---|---|---|
| N_c | **62** | 70 construction lines (count 1) less 8 window re-runs (`rerun_basis` window; lines 64-71 per R11) under `trial_counts`; `ledger_n` 62 | trials.jsonl; DSR12 DSR_v8 N_c 62; Appendix A | <= 62 | at the ceiling (cap reached at Y-1) |
| K_a, hand-written admission trials | **40** = X 25 (v8x2 5 + v8x3 8 + v8x7 12) + Y 15 (v8ys) | admission lines by `cycle`, 1 each | trials.jsonl; R12 §9 | 40 | equal |
| K_a, all admission lines | 61 = v8 12 (v80 7 + v81 5) + X 25 + X-4 re-screens 9 (v8x4) + Y 15; mined 0 | each admission line adds 1 | trials.jsonl; DSR12 "admission 61" | v8 12 and the 9 re-screens counted apart | - |
| M | **110** | v9-mine-c1, ledger line 112 (mining-campaign, budget 110, adds 0 to trial_counts); registry count 110 | trials.jsonl; DSR12 | 110 | equal |
| N_tot | **233** = sum(trial_counts) 123 + M 110 | 61 admission + 62 construction. These add 0: 1 protocol line, 8 window re-runs, 1 campaign line | trials.jsonl; DSR12 DSR_tot N_tot 233 | <= 233 (212 + 21) | at the ceiling |
| N_hand / V (beside) | 123 / 1.2957e-03 per session (33 cells) | N_tot - M - mined-wave lines 0 / ddof-1 variance over research-window-v2 lines | R12 §4; DSR12 | - | - |

P9 room (plan §5.2): N_c <= 69, K_a <= 10, M + 0, N_tot <= 250.

## Side by side (S2, TRAIN 2020-2023; status 8 §1; sources BK12 = R12 §7 = L12 7990-8001)

| book | L / mean L_t | mean gross | net annual | SR 1x | SR 4x | vol | max DD |
|---|---|---|---|---|---|---|---|
| Y-F0 | 1.1828 fixed | .98621 | 5.654% | 1.8495 | 1.6994 | 3.057% | 2.580% |
| X-10 | 2.0 fixed | 1.66672 | 9.306% | 1.8083 | 1.5829 | 5.146% | 4.326% |
| Y-1 | mean L_t 1.8671 (cap 2.0) | 1.55566 | 8.799% | 1.8298 | 1.6281 | 4.809% | 3.602% |

## Phase-0 exit gate (plan §4.3): met

| clause | evidence | met |
|---|---|---|
| Y cells each ledgered, undefined or void by rule | ledger lines 128-133, 6 cells, count 1 each; 0 undefined, 0 void | yes |
| adoption print done | R0-12 `5292b46e`, `32b33236`; v8 log 7874-8045 | yes |
| v8 report written | `docs/plans/2026-10-03-platform-v8-status-8.md` (this commit) | yes |
| G-B1 printed for Y-F0 | 1.849482: floor 1.0 met, target 1.85 missed by .000518 (v8 log 7952-7955) | yes |
| ledger state recorded as `n_before` | the table above; status 8 §5 | yes |

## `wave status` (read-only)

- **Command:** `"C:/Program Files/Python312/python.exe" scripts/research_cycle.py wave status
  scripts/specs/v8/waves/<w>.json --root C:/atx-wt/pool-2`, the form R0-6 .. R0-9 used.
- **Results:**

  | manifest | result |
  |---|---|
  | `y-s.json` | 9/9 done, exit 0 |
  | `y-3.json` | 9/9 done, exit 0 |
  | `y-2.json` | 9/9 done, exit 0 |
  | `y-5.json` | 9/9 done, exit 0 |
  | `y-s.head.json` | exit 2, refused as a manifest: "a wave has exactly one of candidates (a library wave) and rule_cell (a rule wave)" |

- **`y-s.head.json` is not a wave.** It is the Y-S manifest minus `candidates` (R0-2, v8 log line 6677), with the same
  wave name and `out_dir`, so its wave is y-s, which is complete.
- **X-10 and Y-1** ran by hand (YP-7) and have no manifest.
- **E1-STALE's condition holds:** no wave is in flight.
- **When it ran:** once before the R0-14 commit, then again just before the branch cut.

## Commits

| commit | branch | what |
|---|---|---|
| (step 4) | `feat/platform-v8-20260929` | status 8; v8 integration log section R0-14; this report (`git add -f`); P9 `progress.md`, with the PM's uncommitted lines 184-189 included unedited |
| (step 5) | `feat/platform-p9-20261003` | the one line below |

## Hard rules

- No build, no research run, no pytest. The only processes launched were the read-only `wave status` calls and the
  aggregate ledger count. 0 trials.
- Nothing was built, switched or committed in `C:/atx`, and `atx-db/` was not touched. No push, no subagent, no expected
  hash edited.
- Files were staged by exact path only. The design agents' `cov-*`, `sql-*` and `briefs/brief-COV.md` /
  `briefs/brief-SQL*.md` files were not staged, moved or edited.
- The untracked `docs/plans/2026-10-02-x5-equity-curve.png` was left alone.
- Nothing dated 2024-01-01 or later was opened.
- The v8 branch was neither deleted nor moved.

## Branch point
- v8 head SHA = P9 branch point: `31b024c8411da8162f44038d26235da1898d5b3d` (`feat/platform-v8-20260929`; `feat/platform-p9-20261003` cut from it by `git switch -c` in `C:/atx-wt/pool-2`; `wave status` re-run just before the cut: y-s, y-3, y-2, y-5 9/9 done, y-s.head.json exit 2 as before; ledger 133 lines `27e40f9f`).
