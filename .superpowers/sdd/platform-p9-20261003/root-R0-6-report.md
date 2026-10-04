# Root R0-6 report (resume after the owner stop): Y-S wave, stages 05-09 and the wave result

Status: **DONE.** All nine stages of the Y-S wave are done (`wave status`). The driver's verdict is **ACCEPTED**
(PM7-34), and both P12 checks are verified. N went 56 -> 57. Root: `C:/atx-wt/pool-2`, branch
`feat/platform-v8-20260929`. Date 2026-10-03. Rulings applied:
- the PM resume ruling (lock, failed attempt);
- R0-6-ATT (`--attempt w=2`);
- SEAL-ALLOW (progress.md:108).

## Commits (in order; D = made by the driver)

| commit | what |
|---|---|
| `d66f93f6` | log: stage 04 spec rows (committed alone) |
| `c66cf00e` | log: lock removed (pid 25424 dead), receipts 01-04 done, first stop (killed w attempt 1) |
| `de5823f2` | log: R0-6-ATT, w attempt 2 |
| `843fe55c` | log: stage 05 run |
| `c0f1fae6` D | `wave y-s: lib-v8ysb-gm.json at L 1.1828 (gross matching, PM6-6)` |
| `4833e844` | log: stage 06 match |
| `88b51a3c` | log: stage 07 verify HARD-STOP (seal scan) |
| `4284b822` | log: stage 07 verify under SEAL-ALLOW |
| `f4808a37` | log: stage 08 judge |
| `fd55d436` D | `wave y-s: queue status of <15 candidates> (ACCEPTED)` |
| `666436a8` | log: stage 09 record and the P12 checks; record copies `waves/y-s/wave-result.json` and `wave-log.md` |
| (next) | this report, `root-R0-3to5-report.md`, `tools/mcheck.py`, `tools/sealsrc.py` (`git add -f`) |

## Resume steps

- **Lock.** Pid 25424 was confirmed dead with Get-Process and tasklist, and no research or compiler process was
  running. Removed 12:35:03Z.
- **Receipts 01-04.** All done; SHA-256 equal to the logged values (`f4f75908`, `facb7fb2`, `8562e09b`, `ce88ee29`).
- **Killed stage-05 attempt.** Screen re-read PASS, ref NAV completed, w attempt 1 killed with no receipt. It added 0
  trials. It stays on disk, and the seal scan found no hit in it.
- **R0-6-ATT run (once).** `research_cycle.py run scripts/specs/v8/lib-v8ysb.json --stop-after nav --attempt w=2 --root
  C:/atx-wt/pool-2`:
  - free 5,573 MiB before; exit 0; wall 114 s; min free during 3,546 MiB;
  - w attempt 2: 52.8 s, 1,436 MiB, receipt `543860bb`;
  - nav: 59.7 s, 586 MiB, receipt `26fdb282`.

## Per stage

| stage | exit | wall s | peak MiB | free before / min | receipt SHA-256 | commit | result |
|---|---|---|---|---|---|---|---|
| 05 run | 0 | 2.3 | - (no exe) | 5,191 / 5,134 | `eb23472eb751dd75a54edc2d872658050b2cc1a598a85d8703acddd143612571` | - | **the driver took attempt 2 as its own** (`w: done (-2)`, `nav: done`, binding OK); no third w or nav |
| 06 match | 0 | 59.3 | 586 | 4,847 / 3,879 | `b23bd9289ee3e9e52616868e91ec31d9cdb58ca2af9f1c7353caa3963d66c5ce` | `c0f1fae6` | PM6-6: G .9772114158 vs .9862260459 led to one correction, L 1.1828; matched G .9862134133. The -gm run reused w attempt 2 |
| 07 verify, attempt 1 | 4 | 0.7 | - | 4,557 / 4,634 | `07-verify.failed-1.json` `ae50e347` | - | mechanics pass; seal scan 9 tokens, which led to SEAL-ALLOW |
| 07 verify | 0 | 0.9 | - | 5,333 / 5,357 | `82f72a1b11c10d018b17fcc31c7aba7c5148036ee972440770f081bd1c63fe86` | - | per-hit check: 9 hits, sources (a) x7 and (b) x2, 0 other. Mechanics **PASS**; binding `a9bf5d2a`; seal scan 68 logs, 0 tokens; NAV exe `72ff6d2d` vs parent `a95f6f0a` (ref ran) |
| 08 judge | 0 | 46.5 | 626 (summ) | 5,303 / 4,524 | `5fdc3b015ac4fe4cfe7d9c9fa76e33ab80310a158d5e076a8372ce3fac0a9c46` | - | `verdict (pm7-34): ACCEPTED {'dsr_positive': True, 'mechanics': True, 'criteria': False}` |
| 09 record | 0 | 1.0 | - | 4,759 / 4,605 | `7c1079381b22d8b723cf4da12a95117bf8a8d0474c729257d1b8589616f80924` | `fd55d436` | per-hit check: 14 hits, (a) x10 and (b) x4, 0 other. `wave-result.json` `57e9f5ea4a56...`; N 56 -> 57; seal scan 79 logs, 0 tokens |

## The cell's outcome (the driver's own `wave-log.md`, quoted)

- **Verdict:** "**Verdict (pm7-34: paired S2 net dSR > 0 AND mechanics (PM7-34); criteria printed, decide nothing):
  ACCEPTED** {'criteria': False, 'dsr_positive': True, 'mechanics': True}".
- **Criteria (printed):** capacity-4x-higher met; turnover-per-gross-not-higher unmet; cost-bps-lower unmet.
- **Cell:** `scripts/specs/v8/lib-v8ysb-gm.json` (library v8ysb = v8x3b + 9 kept strings), L 1.1828.
- **Statistics of record (S2):**
  - net Sharpe 1.8495 vs parent 1.7695;
  - dSR +0.0800, Memmel SE 0.1502, CBB 95% [-0.236, 0.407];
  - bundle p one-sided 0.3122, two-sided 0.6262;
  - DSR (N 57) 0.8468; PBO 0.0766.
- **Returns (S2, annual):**
  - net 5.65% vs 5.08%; gross of cost 7.07% vs 6.45%;
  - vol 3.06%; max drawdown 2.58%;
  - 4x net Sharpe 1.6994 vs 1.6549; tau .02813 (per unit gross .02853).
- **Next parent (driver):** `scripts/specs/v8/lib-v8ysb-gm.json`, library v8ysb.

## Trial count

- Ledger `build-equity/trials.jsonl`: 127 -> **128 lines** (head `e186895aeed3f7f3`).
- **One construction trial added**: the cell, `11c10defb3cf38a5`. **N 56 -> 57** (construction cap 62).
- Admission trials are unchanged at 40 of 40; they were all ledgered at stage 03.
- The killed attempt, w attempt 2, the gross-match calibration and the failed verify added 0.

## P12 checks

- **b-reuse:**
  - First half (stage 03): 15 of 15 rows on 7 keys.
  - Second half: `tools/mcheck.py result` gives **9 of 9 rows byte-equal** on id, ic21, ic21_hac_t, marginal_ic21 and
    marginal_hac_t. That is canonical-JSON SHA-256 per row of the wave result's carried rows vs the source
    `marginal_ic.json` (`3df5f949`). max_abs_rho and max_rho_member are null in all 9 rows, as the result's source line
    states.
  - **Verified.**
- **Scoreboard 4x:** `research_cycle.py scoreboard` exits 0.
  - The y-s 4x net SR `1.6994264697927697` equals the `capacity-x4-v1+swap-fin-v1` `net_sharpe` read directly from
    `mega-nav-v8x-theme-erc-L1.1828-v8ysb/capacity_curve.csv` (`edf1001d`), byte for byte.
  - The parent row `1.654876435267752` equals the parent CSV's `1.6548764352677521` (the same double).
  - The scoreboard's ledger check: trial ledgered, s2_net_sr equal.
  - **Verified.**

## For R0-7 and later

- **Parent for the next Y cells:** the driver names `scripts/specs/v8/lib-v8ysb-gm.json` (library v8ysb, L 1.1828,
  NAV `mega-nav-v8x-theme-erc-L1.1828-v8ysb`). The program's parent order is the PM's to state in R0-7's brief.
- **SEAL-ALLOW stands through R0-11.**
  - Every driver run prints the png's name and `started_utc` stamps, so every wave's verify will need the two flags.
  - Run `.superpowers/sdd/platform-p9-20261003/tools/sealsrc.py --manifest <wave manifest>` before each use (exit 0 means
    0 OTHER). It reads the same files as `wave_seal.wave_logs`.
  - The (b) token is the run's own UTC date, so a run past 00:00Z needs the new date.
- **A driver gap to fix later** (E1 / DEC-13 scope; not fixed here): `wave run` cannot pass `--attempt`. Each killed
  phase therefore needs one direct research_cycle `--attempt` run, under a ruling like R0-6-ATT.
- **Host:** the stage-06 NAV took 55.6 s and summ 40.0 s, well inside their 180 s caps. The lowest free memory in this
  session was 3,546 MiB, during w attempt 2.

## Hard rules

Nothing was built. The only exe root launched directly is the ruled `--attempt w=2` run; the driver launched every
other run. Nothing dated 2024-01-01 or later was opened: the seal tokens were classified by token and key only, and
results were read only from TRAIN outputs. `C:/atx` and `atx-db/` were not touched. No push, no worktree prune, no
expected hash edited, no code changed, no directory moved, no subagents. The untracked
`docs/plans/2026-10-02-x5-equity-curve.png` was left as it is.
