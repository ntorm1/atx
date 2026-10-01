# v8 cells brief (root only; one integrator at a time; every cell from B0a to V8-F)

You run research cells for the project manager (PM). Your dispatch message names the cells of your batch. This
brief is the procedure for all of them. Binding reads before the first run:

1. `integrator-rules.md` (this directory).
2. `v8-prereg.md` rules 1-11 and its rulings (the registration of record).
3. `task-A2-report.md`, section "Root command sequence" (lock, plan, run; the R-2 and R-7 add-alpha sequences).
4. The plan `docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md`: Task W0-4, section 9 (the task of each cell in
   your batch) and section 12.1.
5. `progress.md`: every ruling id your dispatch names (grep the id; read the whole entry), and the last section
   (current state, last accepted cell, N).
6. `integration-log.md`: the latest sections (executables, build receipt, what the previous batch ran and how).

## Rules for every cell

- One cell = one spec under `scripts/specs/v8/` run through `scripts/research_cycle.py` (`lock --write`, `plan`,
  `run`), on a clean tree, spec committed before `run`. Never a bare executable on data. Caps as the spec and
  the rulings W0-c, E-18, E-24, E-28 state. A free-memory refusal: wait two minutes, retry up to five times.
- Parent = the last ACCEPTED cell's spec (B0c is the baseline whatever its sign). Set `"parent"` in the
  template, nothing else. No parameter is chosen, tuned or changed after a read. A rejected cell is never
  retried in any form. An idea you get from a result goes in the log as a v9 note, not into a spec.
- Order of reads inside a cell: plan output and pins; mechanics (gross, net, turnover limits, beta, the row
  checks the task names); only then the return statistics. If mechanics fail, stop the cell there and report
  before reading returns where the tooling allows it.
- Defects (prereg rule 7): a cell that is invalid because of a tool or data defect is ledgered invalid
  (`ledger-defect --ruling <id> --date <date>` needs a PM ruling: stop and report, do not invent the ruling).
  A re-run decided without seeing returns replaces it with no new trial; a re-run cell is scored alone.
- Hidden-data rule absolute: nothing dated 2024-01-01 or later is opened. The seal is in the readers; if a
  tool asks for such a file, stop and report.
- Trial accounting: every construction cell adds 1 to N (B0a 38, B0b 39, B0c 40, then one per cell); hard
  budget N <= 51; admission trials <= 15 plus the 8 re-screens. Re-runs of ledgered cells on the longer window,
  the protocol line and history reads add 0. Check the ledger count after every cell; a mismatch is a finding.
- Statistics of record (prereg rules 3, 4): paired S2 net dSR against the parent, studentized circular block
  bootstrap (block 21, seed 20260929, 4,999 resamples) as coded in `nav_summ.py`; Memmel SE and Ledoit-Wolf p
  beside it; print the one-sided and the two-sided p (E-34). Report-only columns gate nothing (rule 8).
- Every cell reports: the year table (2020, 2021, 2022, 2023: net Sharpe, return, volatility, turnover, cost
  per traded dollar), net Sharpe at 4x NAV from the capacity curve (E-29, E-37), and the Appendix A block with
  `history reads 0`.
- Acceptance (prereg rule 5) is mechanical: paired S2 net dSR > 0 against the parent AND mechanics AND the
  mechanical criterion registered for the cell (the table below). Compute each ingredient from the cell's own
  artifacts, state the verdict the rule gives, and record it with the tooling (the verdict spec / ledger line
  the A2 sequence names). You apply the rule; you do not interpret it. If any ingredient is ambiguous, missing
  or contradicts a ruling, STOP the batch and report: the PM rules. Otherwise continue to the next cell of your
  batch with the parent the rule gives.

## Registered criteria (besides dSR > 0 and mechanics)

| cell | N after | parent | criterion | rulings |
|---|---|---|---|---|
| B0a | 38 | none | none (re-base; role lo1) | W0-b |
| B0b | 39 | B0a | none; B0b accepted on dSR > 0 vs B0a and mechanics; the winner's role carries on | W0-b |
| B0c | 40 | the winner | none: baseline by declaration. Winner's spec with `--label-role` on the delisting-returns role, `--warm-start-sessions 60`, capacity curve | E-10, E-25, E-29, E-39, A-3 |
| R-1 | 41 | B0c | planned turnover per unit gross not higher than the parent | E-28 |
| R-2 | 42 | last accepted | book turnover not higher; wave judged whole; 7 READY members + 8 `_f49` re-screens; fields v10 first | R2-a..h, E-36 precedent |
| R-3 | 43 | last accepted | net Sharpe at 2x NAV not lower AND turnover lower; rule `ew-theme-std-aim-v1` if R-1 accepted, else `ew-theme-aim-v2` | E-27, E-27a, E-27b |
| R-4 | 44 | last accepted | turnover at least 15% lower (`--hold-band .10`) | - |
| R-5 | 45 | last accepted | net Sharpe at 4x higher AND net at 1x not lower by more than one paired SE AND S3 not lower (`--adv-hold-q .10`, capacity curve) | E-15, PM4-5 |
| R-6 | 46 | last accepted | cost per traded dollar not higher AND tripwire clear AND mean `aim_correlation_traded_after` >= .9; the run voids itself on primary-book limits_unmet > 0 | E-14, E-14a, E-26, E-31, E-31a, E-37 |
| R-7 | 47 | last accepted | turnover not higher (marginal IC gates nothing); fields v11 first | E-36, R7-a..c |
| R-8 | 48 | last accepted | realised volatility inside [.8, 1.2] x 5% in each TRAIN year; `r8.json`; check the risk store's capped_specific count first | E-40, E-43 |
| R-9 | 49-51 | last accepted | three report-only theta cells; ONLY if R-6 was rejected | E-37, E-38 |
| R-10 | 49 | last accepted | planned turnover per unit gross not higher; ONLY if R-6 and R-1 were accepted; `r10.json` | E-38, E-44, E-45 |
| R-11 | 50 | last accepted | same criterion; ONLY if R-6 and R-1 were accepted; `r11.json` | E-38, E-44, E-45, PM4-11, PM4-12 |
| R-12 | 51 | last accepted | book turnover not higher; ONLY if R-6 was accepted; fields v12; three LIB2 candidates through `add-alpha` and `run --screen` | E-38, E-42, PM4-9 |

A fit refused because the 1/(2T) member cap is infeasible is a cell that cannot be formed (PM4-10): ledger it
as undefined (adds 0), keep the parent, go on. A conditional cell whose condition is not met is recorded in the
log as "undefined (ruling id)" and not run.

## Report

One section per cell in `integration-log.md`: spec path and digest, pins, each phase (seconds, peak MiB, exit
code), mechanics, the statistics of record, the criterion's numbers, the verdict and the rule that gave it, N
after, the year table, 4x, the Appendix A block, defects and fixes. Commit with `git add -f`; tree clean.
Final reply to the PM, at most 6 lines per cell: cell, N after, S2 net Sharpe of the cell and of the parent,
paired dSR with its SE, one-sided and two-sided bootstrap p, mechanics pass / fail, the criterion's numbers,
the verdict, net Sharpe at 4x; then the next parent, and anything you could not verify.
