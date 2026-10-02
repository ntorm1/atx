# v8 pre-registration (declared 2026-09-29, before any read on the 2020-2023 window)

1. Window. TRAIN is [2020-01-01, 2024-01-01). Hidden: 2024-01-01 and later. Owner ruling 2026-09-29.
   Disclosure: 2023 and 2024 were read twice at book level as validation in earlier sprints. 2023 statistics are
   therefore partly selected. 2025 and later has never been read.
2. Trial count. N continues from 37. Every construction cell adds 1. Re-runs of ledgered cells on the longer window add 0.
   The protocol line in the ledger adds 0.
3. DSR. N = ledger count. Cross-trial variance = variance of annualised Sharpe over the ledgered cells that were re-run
   on 2020-2023 plus the v8 cells. The legacy variance (all cells, 2020-2022) is reported beside it and gates nothing.
4. Paired test. Studentized circular block bootstrap, block 21 sessions, seed 20260929, 4,999 resamples, as coded in
   nav_summ.py. Memmel SE and Ledoit-Wolf p are reported beside it.
5. Acceptance of a cell: paired S2 net dSR > 0 against its parent AND mechanics AND the mechanical criterion named in
   its task. Rejected cells are not retried with other parameters in v8.
6. Baseline. B0c is the baseline whatever its sign. Delisting returns on; warm start 60 sessions.
7. Defects. An invalid cell is logged and excluded from N. A rerun decided without seeing returns replaces it with no
   new trial. A rerun decided because the returns looked wrong is a new trial.
8. Diagnostics G-1..G-3 and the report-only columns ic_theta, f_theta and marginal IC gate nothing and select nothing.
9. Freeze gate: S2 net >= 1.0 AND mechanics AND cumulative paired dSR (V8-F vs B0c) > 0 with bootstrap p < .10 AND
   cell-count DSR >= .95.
10. Origin classes. prior: literature sign and canonical definition, 1 trial. grid: every cell counts. mined: rule
    mined-v1, campaign budget fixed in advance. v8 runs no mined campaign.
11. Cells, in order: B0a, B0b, B0c, R-1, R-2, R-3, R-4, R-5, R-6, R-7, optional R-8, R-9. Parameters as in the sprint plan
    docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md sections 7 and 9, which are part of this registration.

Appendix A block (on every result):
TRAIN construction cells <N>; admission trials this sprint <k>; window research-window-v2 (2020-2023);
hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024); 2025+ never read.

## Pins (filled by root as artifacts are built)
- research_window.json sha256: `62cf2cfab1d0f61b02731a807ccfdd326bb2df91caff1173a3cf1b37e6e63584`
  (`atx-impl/strategies/research_window.json`; window research-window-v2, seal 2024-01-01)
- role train-2020-2023-lo1 manifest sha256: `2ff9d7711bdaa2d669cc096f4874f7f4705309e53572fd12dad3c0a4d7ac1e53`
  (`build-equity/train-2020-2023-lo1/manifest.json`, linked-operating-v1)
- role train-2020-2023-lo3 manifest sha256: `e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4`
  (`build-equity/train-2020-2023-lo3/manifest.json`)
- fields v9 lo1 / lo3 manifest sha256: `888e6616e441e863a9f91234124e1aebc907db11e18d9789d3c583cf447b8695` /
  `9f1563638b5e4f7ead7be686803b96a0707ada2c608fcbc6dc084179bd9021ef`
  (`build-equity/train-2020-2023-lo1-fields-v9/manifest.json`, `build-equity/train-2020-2023-lo3-fields-v9/manifest.json`)
- Pinned 2026-10-01 at R14 (Wave 0 part 2c), each digest re-hashed from the file on disk immediately before writing.

## Rulings declared before any read
- Ruling W0-a (overlap): bit-identical overlap continues the v7 ledger without comment; a difference below 1e-9 is disclosed and the 4-year values become the reference; a larger difference stops the re-base until the cause is found -- a silent change in old values would make every paired comparison uninterpretable -- cost if wrong: one day.
- Ruling W0-b (baseline): B0c is the baseline whatever its sign. B0b is accepted on paired S2 net dSR > 0 against B0a and mechanics.
- Ruling W0-c (caps): the IC phases on the 4-year role run under 2,560 MiB and 300 s (OD-2, owner approved 2026-09-29). Every other phase keeps 180 s and 1,536 MiB.
- Report-only columns ic_theta, f_theta, marginal IC and diagnostics G-1..G-3 gate nothing and select nothing (rule 8).
