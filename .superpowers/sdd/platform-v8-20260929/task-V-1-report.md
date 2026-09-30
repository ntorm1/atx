# Report: task V-1 (validation kit)

Lane EV, pool-8, branch `feat/platform-v8-ev-20260929`. Status: DONE_WITH_CONCERNS (merge order: W0-1 first; the n37
reproduction runs at root only).

Commits: `425d16db` (the move, `git mv`, plus the old-path shim) and the V-1 commit that carries this report.

## What was built

| file | content |
|---|---|
| `atx-impl/tools/nav_summ.py`, `backtest_integrity.py` | moved from `.superpowers/sdd/mega-alpha-20260926/studies/` (history: `git log --follow -C`) |
| `atx-impl/tools/test_nav_summ.py`, `test_backtest_integrity.py` | moved with them; run lines and one window test updated |
| `.superpowers/sdd/mega-alpha-20260926/studies/nav_summ.py` | shim: specs v61..v71 (`summ.script`) and `scripts/tests/test_research_cycle.py` still name this path; run, it runs the moved script; imported, it is the moved module |
| `atx-impl/tools/test_nav_summ_v8.py` | the V-1 tests (below) |

Research window (W0-1). `backtest_integrity.research_window()` loads `research_window.py` the way W0-1 prescribes for
atx-impl tools (`from engine_tools import research_window`, a private instance a test harness cannot rebind); before
`engine_tools.py` is on the branch it loads the same private instance itself. Every date comes from it:

- `window_of` (ledger lines): TRAIN = `[TRAIN_BEGIN_NS, TRAIN_END_NS)`; the hard-coded 2023-01-01 is gone. A series that
  ends before the TRAIN end (the 3-year v7.1 cells) is a TRAIN series.
- `load_daily_csv` refuses (SystemExit, message names the seal date and the window id) any daily CSV with a session at or
  after `SEAL_NS` before any column is parsed; `allow_sealed=True` is for holdout_gate.py only.
- The v8 Appendix A block takes TRAIN years, the sealed year and the never-read year from the window.
- Grep gate: `2023-01-01`, `2024-01-01`, `2025-01-01`, `1_672_531_200`, `1_735_689_600`, `1_704_067_200` appear nowhere
  in the two scripts or their tests. `PRIOR_VALIDATION_READS = "2 (2023-2024)"` is the v8-prereg item 1 disclosure of past
  reads, not a window constant.

Validation kit, every option opt-in (without them the output is byte-identical to v7; the existing blob tests of
`test_nav_summ.py` pin that):

- `--protocol v8`: bootstrap seed 20260929, 4,999 draws, block 21 for the CBB and the Ledoit-Wolf studentized test
  (explicit `--seed` / `--draws` still win); year table per dir; ledger lines carry origin + window_id + the hash chain;
  `--ledger-n` adds the v8 Appendix A block.
- `--year-table`: per calendar year: return rows, compounded net return, net Sharpe, volatility, mean tau_gmv (the
  tau_t sessions), cost bps per traded dollar.
- `--bundle BASE FINAL [--bundle-alpha .10] [--bundle-json OUT]`: cumulative paired dSR(net) FINAL - BASE (Memmel SE,
  CBB CI, studentized CBB two-sided and one-sided p from the same resamples), per-year dSR, both year tables, verdict
  `pass = dSR > 0 and p_one_sided < alpha`.
- Ledger fields (all optional; old lines and the `trial_id` rule unchanged): `origin` (K5: prior | grid | mined),
  `window_id`, `rerun_of` + `rerun_basis` (window | blind | returns), `defect {invalid, reason}`; chain `prev_sha256`
  (ledger-chain-v1: SHA-256 of the previous non-blank line's bytes, 64 zeros for the first). `ledger_read` verifies every
  link; unchained (pre-v8) lines are accepted as they are; once a line is chained every later append is chained.
- Defect rule (Appendix A rule 7), `trial_counts`: protocol lines and window re-runs add 0; an invalid cell and a cell
  replaced by a blind re-run add 0 unless a returns-based re-run names it (then both count). Appendix A prints an
  "adding no trial" line only when such lines exist (a v7 ledger's block is unchanged).
- `--dsr-ledger LEDGER` (OD-4): N = construction trials by the defect rule (+1 for a dir not in the ledger); V[SR] =
  sample variance of the per-session S2 net SRs of the construction lines scored on the current window id (re-runs and
  new cells, exclusions left out); the legacy variance (lines without window_id) is printed beside it.

Protocol lines (PM addition 2). `research_cycle.py ledger-protocol` (lane A, `scripts/research_ledger.py`, commit
`fa61c67f`) is the writer; nav_summ has no writer of its own, so there is one line format. Every reader here
(`ledger_counts`, `trial_counts`, `appendix_a`, `appendix_a_v8`, `dsr_variance`, `ledger_net_series`, `--dsr-ledger`'s
cell set) skips kind `protocol` when it lists cells or counts N; `ledger_read` keeps it in the chain.

## Tests

`atx-impl/tools/test_nav_summ_v8.py`: `test_legacy_n37_numbers_reproduced` (skips unless `ATX_EQUITY_ROOT` names a
`build-equity` dir), `test_year_table`, `test_bundle_verdict`, `test_origin_class_in_ledger_line`,
`test_variance_from_rerun_cells`, `test_defect_rule_appendix_a`, `test_protocol_line_is_chained_but_not_counted` (lane
A's exact line layout; chain verified through it, tamper or removal breaks the next link, not in N / Appendix A / cell
lists / variance, nav_summ `--ledger-n`, `--effective-n LEDGER`, `--dsr-ledger` run over it),
`test_sealed_series_is_refused_before_any_statistic`, `test_v8_bootstrap_defaults`,
`test_old_study_path_is_a_shim_to_the_moved_script`.

Run here (synthetic data, with W0E's draft `research_window.py` / `.json` copied in untracked and removed before commit):

```
pytest atx-impl/tools/test_nav_summ.py atx-impl/tools/test_backtest_integrity.py atx-impl/tools/test_nav_summ_v8.py
       scripts/tests/test_cycle_e2e.py                    -> 46 passed, 2 skipped (n37, live e2e)
```

The n37 test was exercised on a synthetic 37-cell `build-equity` built in the scratchpad (same argv shape): passes, and
fails when one number of the JSON is moved by 1e-12. It was not run on the v7.1 cells here (lane rule).

## How root verifies

```bash
PY="C:/Program Files/Python312/python.exe"
# after W0-1 (lane W0E) is merged:
"$PY" -m pytest -q -p no:cacheprovider atx-impl/tools/test_nav_summ.py atx-impl/tools/test_backtest_integrity.py \
      atx-impl/tools/test_nav_summ_v8.py                                     # 42 passed, 1 skipped
ATX_EQUITY_ROOT=C:/atx-wt/pool-2/build-equity "$PY" -m pytest -q -p no:cacheprovider \
      atx-impl/tools/test_nav_summ_v8.py -k n37                              # reads the v7.1 2020-2022 cells
```

The n37 test runs `nav_summ.main` with the argv recorded in `mega-nav-v71-summ-n37.json` (cwd = the parent of
`build-equity`; `--ledger`/`--ledger-n` pointed at a temp copy of `trials.jsonl`, `--json`/`--pbo-json` at temp files)
and compares the canonical JSON of every row and of `mega-nav-v71-pbo-n37.json` with `nav_summ_run` removed and the
ledger path restored. Identity: without the new options, output and JSON are unchanged (the legacy blob tests pass).

## Deviations from the brief

1. **"Byte for byte" excludes `nav_summ_run` and `ledger.path`.** They hold the script path, the script SHA-256, the git
   head and the ledger path, which the move itself changes. Every number is compared exactly.
2. **Two commits.** The move was committed first (425d16db); git records the old nav_summ path as modified (the shim)
   and the new one as a copy, so use `git log --follow -C atx-impl/tools/nav_summ.py`.
3. **No protocol-line writer in nav_summ.** A `--ledger-protocol` option was drafted and dropped when lane A's
   `ledger-protocol` verb landed (one writer, one layout).
4. **Window crossing test.** Under research-window-v2 the TRAIN end equals the seal, and `load_daily` refuses sealed
   sessions first; `test_ledger_refuses_post_train_sessions_and_changed_series` builds the crossing series as a nets map
   from `TRAIN_END_NS` and checks that a series ending in 2022 is still ledgered.
5. `--protocol v8` uses 4,999 draws: the studentized CBB holds draws x T x 4 doubles twice, about 0.3 GB for 750
   sessions and 0.45 GB for a 4-year TRAIN per paired test.

## Cross-lane edits

- `.superpowers/sdd/mega-alpha-20260926/studies/nav_summ.py` (the shim at the old path; commit 425d16db).

## Open risks

- **Merge order: W0-1 before V-1.** Both scripts import `research_window.py` (atx-engine/tools) at first use and
  `load_daily_csv` checks the seal on every CSV, so without W0-1 nav_summ stops with ImportError. Names used: `load()`,
  `TRAIN_BEGIN_NS`, `TRAIN_END_NS`, `SEAL_NS`, `SEAL_DATE`, `is_sealed` (the brief's interface) and, with fallbacks,
  `WINDOW_ID`, `SealError`; the JSON's `hidden.never_read` feeds the Appendix A v8 line and V-2.
- **N definitions.** Lane A's `summ.dsr_n: "ledger+1"` counts ledger cells (protocol skipped). Once a ledger holds
  defect or window re-run lines, that N differs from `trial_counts` (`--dsr-ledger`, Appendix A). Lane A should count
  with `backtest_integrity.trial_counts`, or root should rule which N gates.
- Lane A's writer appends the protocol line unchained; it is pinned by the next chained line (any v8 nav_summ append).
  A protocol line at the ledger tail is not yet covered by a link.
- `scripts/tests/test_research_cycle.py`: 2 failures in this tree before and after V-1
  (`test_v71_spec_is_v70_with_the_declared_changes_only`, `test_v71_plan_dry_run_resolves_every_pin_and_the_identity_phases`:
  v71.json `dsr_n` 37 vs the ledger's 34 prior cells + 1). Lane A's A-3 sets v71 `dsr_n` to `"ledger+1"`.
- Synthetic sealed CSVs (sessions on or after the seal) are written and read by the refusal tests; no real data is.
