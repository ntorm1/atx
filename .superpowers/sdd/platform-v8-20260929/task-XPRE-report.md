# Task XPRE report: pre-registration of expansion X and of the mined campaign

Lane XPRE, worktree `C:/atx-wt/pool-7`, branch `feat/platform-v8-xpre-20261002`, base `3c6ae225`.

| item | commit | what |
|---|---|---|
| draft | `71a68d60` | `v8x-prereg.md` with open choices OC-1..OC-11 |
| B (P5) | `44a27d5d` | `nav_summ.py --dsr-total / --dsr-hand`, `atx-impl/tools/dsr_total.py`, `test_dsr_total.py` |
| A | `57157a05` | `v8x-prereg.md` with every open choice written in as ruled (PM7-6..PM7-14); 258 lines |
| C | this commit | this report |

## A. The ruled pre-registration

`.superpowers/sdd/platform-v8-20260929/v8x-prereg.md` (258 lines). Every open choice of the draft is now ruled text
carrying its ruling number. Section 13 lists the rulings; "recommendation" no longer appears in the file.

Text that changed in substance:
- **Repairs (PM7-9).** Only defects the PM confirmed from XIMP's proof go to X-1. Any other repair moves to X-2 and
  takes one admission trial; it counts against X-2's cap of 8.
- **Leverage (PM7-11).** The gross limit is quoted verbatim from the ruling ("[.90, 1.05] x (2.0 / L_P) x the parent's
  matched gross ratio", written in the cell's plan before the run). The unlevered and levered books are reported side
  by side with maximum drawdown; the owner chooses which is deployed.
- **Campaign (PM7-12).** The campaign runs alone. If free physical memory is below the cap + 1 GiB, the integrator
  lowers the workers (never the budget) and stops if 1 worker does not fit. The runbook comment now says this.
- **Caps (PM7-13).** The roster and pool caps rise to 80 at integration 8. The IC-pass memory cap is re-probed before
  X-3 (P7).
- **X gate (PM7-8).** An X book that fails the gate is carried to v9 with its count.
- **P5** names the tool at `44a27d5d`.

## B. Total-count deflated Sharpe (precondition P5)

Interfaces as coded:
- `atx-impl/tools/dsr_total.py` is pure over ledger records and net moments; only `read_ledger` touches a file.
  - `read_ledger(path)` refuses a missing file (ValueError) and verifies the hash chain through `BI.ledger_read`.
  - `trial_breakdown(records)` returns the trials by kind, the `ledger_trials` sum and `campaign_registry`, with
    `n_tot = sum(trial_counts) + campaign_registry_count`.
  - `dsr_at(moments, n, V)` returns SR0 and DSR through `BI.expected_max_sr` and `BI.deflated_sharpe`. Each value is
    None when V is undefined, N < 2 or the moments are undefined.
  - `ledger_rows(moments, records, in_ledger, window_id)` returns the `tot` row (N_tot, + 1 when the book is not in
    the ledger), the `v8` row (`BI.ledger_n`, which equals the `--dsr-ledger` value) and V from `BI.dsr_variance`
    (ddof 1, number of cells in V).
  - `hand_row(moments, ledger, trial_id, window_id)` computes the same rows on the ledger prefix that ends at the
    book's own construction line, with that prefix's chain head. It refuses a book that is not ledgered.
- `nav_summ.py <dirs> --dsr-total LEDGER [--dsr-hand DIR ...]`:
  - For each listed dir it prints a `== total-count DSR` block (lines `DSR_tot` and `DSR_v8`, with N's parts, V, the
    cells in V, SR0, the moments and the chain head).
  - For each `--dsr-hand` dir it prints a `== DSR_hand` block.
  - The JSON gets the key `deflated_total` on each listed row; the `--dsr-hand` rows sit under `deflated_total.hand`.
  - Usage errors: `--dsr-hand` without `--dsr-total`; `--dsr-total` without dirs or with `--pool`.
  - Refused: a missing or broken ledger, and a `--dsr-hand` dir without `summary.json`, without its daily CSV or not
    ledgered.

Tests (`atx-impl/tools/test_dsr_total.py`, 8 cases, synthetic). The expected values come from an independent oracle:
numpy moments and `statistics.NormalDist` for Phi and PhiInv. The synthetic ledger holds legacy lines, a protocol line,
a window re-run, an invalid cell, admission lines, a campaign line with registry count 132 and mined admissions. The
cases check:
- N_tot 149 = admission 10 + construction 7 + campaign registry 132, with + 1 for an unledgered book;
- the v8 count equals the `--dsr-ledger` value;
- a campaign line moves N_tot by its registry count and nothing else;
- V uses ddof 1 (not ddof 0) and only cells on the window;
- DSR_hand on the 16-line prefix: N_tot 13, chain head equal to that of a truncated ledger;
- every refusal above;
- the flags add lines and one JSON key and change nothing else;
- the flag-absent identity against the blob, described next.

### How root verifies identity (at integration 8)

1. Tests: `"$PY" -m pytest -q -p no:cacheprovider atx-impl/tools/test_dsr_total.py atx-impl/tools/test_nav_summ.py
   atx-impl/tools/test_nav_summ_v8.py atx-impl/tools/test_nav_summ_pool.py atx-impl/tools/test_backtest_integrity.py
   atx-impl/tools/test_trial_ledger_rules.py atx-impl/tools/test_holdout_gate.py`. Here: 96 passed, 1 skipped (the
   skipped test is `test_legacy_n37_numbers_reproduced`, which runs only with `ATX_EQUITY_ROOT`).
2. `test_flag_absent_is_byte_identical_to_the_pre_x_nav_summ` loads `nav_summ.py` from git blob `ef8cbbec`. That is
   the file at `3c6ae225`, and also at root head `e770fc36`, unchanged since `0b855971`. For four argv sets
   (reference + paired; v8 + `--dsr-ledger` + `--ledger-n` + PSR + effective-N + PBO; `--effective-n LEDGER`;
   `--bundle` + `--bundle-json`), it requires byte-equal stdout and stderr, and every JSON field equal except
   `nav_summ_run`.
3. With `ATX_EQUITY_ROOT=<root>/build-equity`, `test_legacy_n37_numbers_reproduced` must reproduce
   `mega-nav-v71-summ-n37.json` byte for byte (except `nav_summ_run`) on the v7.1 cells.
4. Optional, on real ledgered cells already read. It adds 0 trials and prints only numbers already ledgered:
   - `git show ef8cbbec97ec0e5d7590b1b7e3c0d327c1b1fab3 > $TMP/nav_summ_old.py`;
   - run `PYTHONPATH=atx-impl/tools "$PY" $TMP/nav_summ_old.py <B0c nav> <last accepted nav> --protocol v8
     --dsr-ledger build-equity/trials.jsonl --json $TMP/old.json > $TMP/old.txt`;
   - run the same argv with `atx-impl/tools/nav_summ.py`, writing `new.json` and `new.txt`;
   - `old.txt` and `new.txt` must be byte-equal, and the JSONs must be equal after dropping `nav_summ_run`.
5. The only expected moves: `nav_summ_run.script_sha256` and `git_head` in every JSON that nav_summ writes after the
   merge. They are the file's own provenance.

Merge facts:
- `nav_summ.py` and the new `dsr_total.py` are tool scripts covered by PM5-21's freeze list, so they merge at
  integration 8, after V8-F.
- `backtest_integrity.py` is untouched. The mining branch's one-line change there (`MINED_MAX_BUDGET`) cannot
  conflict. The `build-equity` ledger paths are unchanged.

Other suites:
- `atx-impl/tools` whole: 592 passed, 2 skipped.
- `scripts/tests`: 17 failed, 173 passed, 4 skipped. The same 17 fail on the base with this change stashed
  (`test_research_spec.py`: `r3-aim-gain-gm.json` is missing from its spec list). PM7-4 fixes them in root.

## Deviations

- The draft named `backtest_integrity.total_trials`. The code is a separate module, `dsr_total.py`, so that
  `backtest_integrity.py` (which the mining branch edits) is not touched. The prereg text names the module.
- One extra flag, `--dsr-hand DIR` (repeatable). DSR_hand needs the H-F book, which is not the listed X-F0 book.
  The same rule gives V8-F's value at its own ledger state ("DSR up", section 4).
- An unledgered listed dir gets + 1 on both N_tot and N_c (ledger_n's convention). At the X gate X-F0 is ledgered, so
  no + 1 applies there.

## Cross-lane edits

None. `nav_summ.py` is not in another X lane's brief.

## Open risks

- PM7-11's "the parent's matched gross ratio" is not defined in any text I read. The prereg quotes it verbatim; the
  leverage cell's plan must define it before the run.
- N_tot counts every legacy kind in the ledger (composition, universe and data lines, if present). The printed parts
  show them.
- With the base roster near 60, X-3 depends on PM7-13 (caps 80) landing at integration 8.
