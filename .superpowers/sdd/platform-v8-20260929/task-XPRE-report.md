# Task XPRE report: pre-registration of expansion X and of the mined campaign

Lane XPRE, worktree `C:/atx-wt/pool-7`, branch `feat/platform-v8-xpre-20261002`, base `3c6ae225`. Docs only.

## What was built

`.superpowers/sdd/platform-v8-20260929/v8x-prereg.md` (249 lines), ready for the PM to rule on:
- section 1: baseline V8-F; named books H-F, X-F0 and X-F;
- section 2: window and seal, unchanged;
- section 3: trial counting, an event table with the code name of each count; `N_tot = sum(trial_counts) +
  campaign_registry_count`;
- section 4: the deflated Sharpe: formula, V from `dsr_variance`, three printed counts (DSR_tot gates; DSR_hand and
  DSR_v8 are reported), the "DSR up" definition, the tool to add after V8-F, and scale arithmetic;
- section 5: the X budget: at most 10 cells, 39 admission trials (23 hand-written, 16 mined) and a campaign budget
  of at most 132;
- section 6: the order of cells X-1..X-10, the acceptance of each cell, and why;
- section 7: the X gate and the claims rule;
- section 8: the leverage cell (PM7-3) with its restated mechanics;
- section 9: the campaign amendments A1-A7 and the runbook (argv, and the checks run before any statistic);
- section 10: `holdout_gate.py` and the OD-3 read;
- section 11: what voids a cell;
- section 12: preconditions P1-P7;
- section 13: open choices OC-1..OC-11.

## How root verifies

Read the file. No code changed, no test applies, nothing was built or run on data.

The arithmetic in section 4 was computed with the Python standard library (`statistics.NormalDist`). The E[max]
factors 2.38 / 2.52 / 2.83 are at N 66 / 97 / 247. The illustration uses sd .057, from the seven public 2020-2023
S2 net Sharpes (B0a 1.125 .. R-4 1.237, status 6 and progress.md), with T 1,005 and normal moments.

The code names cited were checked against the source in this worktree:
- `backtest_integrity.py`: `trial_counts`, `ledger_n`, `dsr_variance`, `campaign_registry_count`, `expected_max_sr`,
  `deflated_sharpe`, the ledger kinds;
- `nav_summ.py`: `--bundle`, `--dsr-ledger`;
- `cycle_admission.py`: one admission line per `gate.admitted` candidate;
- `holdout_gate.py`: its CLI;
- `1bd448cd`: the mine verbs and runbook steps.

## Deviations

- The DSR on the total count needs a tool change: today `ledger_n` counts construction lines only, and campaign lines
  add 0. The change is specified (precondition P5, `nav_summ.py --dsr-total`) but not written. The brief asks for the
  document only, and PM5-21 freezes tool scripts until V8-F.
- An illustrative DSR from public numbers is included and labelled "gates nothing". No recommendation depends on it.
  OC-5's L 2.0 and its .100 tolerance come from the executable's limit and plan 12.2, not from any return.

## Cross-lane edits

None.

## Open risks

- **OC-1 (caps).** V8-F may hold 60 members against caps of 64. Without a raise, X-3 fits only about 4 new members,
  and `mine pool` refuses a pool of more than 64 weighted members.
- **V decides the X gate.** The variance comes from the W0-4 re-runs, which have not run. At a hypothetical sd .30 the
  DSR is below .95 whatever the count.
- **Overlap between lanes.** XIMP-C and XCOMB-2 may deliver the same mechanism (a per-theme half-life). XSIG's
  preferred fields are exactly the campaign's field list; A3 resolves this in favour of the hand-written prior.
- **Leverage guard.** The guard's "one SE" is ambiguous in PM7-3; OC-5 fixes it at .100.
