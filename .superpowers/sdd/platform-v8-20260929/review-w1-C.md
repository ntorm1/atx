# Review W1, area C (research cycle tooling, trial ledger, statistics, diagnostics, report seal)

Snapshot: `C:/atx-wt/pool-9` at 7af37e9d, change `ef11f462..7af37e9d`. Read-only. **Stopped early at owner instruction**:
the source files below were read; no test file, fixture or spec content was reviewed (see the two lists at the end).
Severity: **I** important, **M** medium, **m** minor. "Verified" = the claim follows from code I read; anything else is
marked unverified with what would confirm it.

Formulas derived and compared with the code, no discrepancy found: annualised Sharpe (`nav_summ.py:263`), Memmel SE
(`:269`), the delta-method gradient of dSR (`:284`), Bartlett HAC with lag block-1 (`:294`), the Ledoit-Wolf studentized
circular block bootstrap two-sided p and the one-sided p (`:327-354`), DSR / PSR / MinTRL and the expected maximum SR
(`backtest_integrity.py:153-184`), CSCV PBO logit and rank convention (`:233-287`), year table (`nav_summ.py:514`),
pairing by session (`align`, `:432`). Return rows at the warm-start boundary agree with the C++ (`strategy_nav_replay.cpp
:1187`: first return row is begin+1 warm, begin+2 flat; `return_mask` drops the first CSV row and non-return rows).

---

## C-1 (I) The DSR that reaches `cycle_verdict.json` and the nav_summ headline is not the pre-registered DSR

`scripts/cycle_verdict.py:55-59`, `scripts/research_cycle.py:901-923`, `atx-impl/tools/nav_summ.py:406-429, 546-565`.

v8-prereg item 3: N = ledger count, V[SR] = variance over the cells ledgered on 2020-2023. That number is produced only by
`nav_summ --dsr-ledger` (key `deflated_ledger`). `summ_step` never passes `--dsr-ledger`; it passes `--dsr-n N`. With
`--dsr-n`, `dsr_rows` takes V from the dirs on the command line: with one dir (spec with `dsr_n: "ledger+1"` and no
`cells_from_ledger`) it is the Lo (2002) sampling variance of that single cell; with `cells_from_ledger` it is the
variance over every ledgered dir, each on its own window (3-year legacy cells mixed with 4-year cells), which is the
"legacy" figure that ruling W0-h says gates nothing. `cycle_verdict.scoring_blocks` writes `dsr.cell_count` from
`row["deflated"]` and never reads `deflated_ledger`.

Example (normal returns, T 1,006, SR 1.0 annual, N 41, E[max] factor 2.199): Lo variance gives SR0 1.10 annual and
DSR .42; a cross-trial SD of .15 annual gives SR0 .33 and DSR .91; SD .10 gives .94. The freeze gate is "cell-count DSR
>= .95", so the convention decides the gate.

Fix: when `summ.dsr_n == "ledger+1"`, `summ_step` adds `--dsr-ledger <ledger>`; `cycle_verdict` reports
`deflated_ledger.dsr` as `dsr.cell_count` (and the legacy value beside it, labelled).

## C-2 (M) The cycle does not apply the v8 protocol to the scoring step

`scripts/research_cycle.py:913-920`, `scripts/research_add_alpha.py:141-143`, `atx-impl/tools/nav_summ.py:734-745, 878-880`,
`atx-impl/tools/backtest_integrity.py:783-784`.

`--protocol v8` and `--origin` reach nav_summ only through `summ.extra`. A spec derived by `add-alpha` copies the
parent's `extra` (v71: `--effective-n dirs --psr --pbo`). Then (a) the paired bootstrap runs with seed 20260927 and 2,000
draws, not the pre-registered 20260929 and 4,999 (prereg item 4), and these are the `paired` numbers in the verdict;
(b) the ledger line has no `window_id` and no `origin`, so `dsr_variance` puts the v8 cell in the legacy group
(`"window_id" not in r`) and leaves it out of the current-window variance; with fewer than two lines on the window
`variance_sr` is None and the OD-4 DSR is None.

Fix: `summ_step` adds `--protocol v8 --origin <summ.origin>` for a v8 spec (refuse the spec without `summ.origin`), or
`validate_spec` refuses `dsr_n: "ledger+1"` without them. Unverified: the v8 base specs (lane A2 drafts) are not in this
commit; if they carry the flags in `extra`, (a) and (b) do not occur for those specs.

## C-3 (M) A defect or a re-run basis cannot be recorded for a cell that is already ledgered

`atx-impl/tools/backtest_integrity.py:657-663`, `atx-impl/tools/nav_summ.py:765-773`.

`ledger_append` skips a record whose `trial_id` (kind + daily CSV SHA) is present; the v8 fields are not part of the id.
`research_cycle run` ledgers every cell in its summ step. If the cell is later found invalid, `nav_summ --ledger
--ledger-defect REASON` on the same dir prints "appended 0, skipped 1" and exits 0; the defect is not logged and N still
counts the cell (prereg item 7: "an invalid cell is logged and excluded from N"). The same holds for a 4-year re-run of
a v7 cell ledgered by the cycle without `--rerun-of`: it cannot be turned into a window re-run afterwards, so N is one
too high per re-run (up to about 9).

Fix: a separate event line (kind `defect` / `reclass`, count 0, naming the trial_id, chained) that `trial_counts`
honours; at least exit non-zero when the flags given differ from the ledgered line.

## C-4 (M) `rerun_of` is not checked against the ledger

`atx-impl/tools/backtest_integrity.py:575-576, 689-700`.

`--rerun-of X --rerun-basis window` makes the line add 0 whatever X is. A cell name, a typo or an id of another kind is
accepted. Example: a new construction cell ledgered with `--rerun-of b0a --rerun-basis window` adds 0: N is one too low
and every later DSR is too high.

Fix: `ledger_append` refuses a `rerun_of` that is not the trial_id of an earlier line of the same kind; for `window` the
target must be on another `window_id` (or have none).

## C-5 (M) A blind re-run takes any named cell out of N, without a declared defect

`atx-impl/tools/backtest_integrity.py:690-697`.

`replaced` = every `rerun_of` of a blind line; the target then adds 0 whether or not it carries `defect`. Example: R-3 is
ledgered (1). R-3 with another parameter is ledgered with `--rerun-of <R-3 id> --rerun-basis blind`: counts are [0, 1],
N is unchanged, two results were read. Prereg item 5 forbids the retry and item 7 allows replacement only for an invalid
cell.

Fix: the target adds 0 only if a defect for it is in the ledger on an earlier line than the re-run (needs C-3).

## C-6 (M) The hash chain does not cover the legacy lines, the tail, or an unchained line added later

`atx-impl/tools/backtest_integrity.py:631-642, 652-674`.

A link is checked only on a line that has `prev_sha256`, and it names the previous line only. So: (a) of the 37 unchained
legacy lines only the last is pinned; adding `"rerun_basis":"window"` to legacy line 5 lowers N by 1 and `ledger_read`
passes; (b) the last line can be edited or removed with no successor to break (no head is recorded anywhere I read;
`ledger_head` has no caller outside the append); (c) a line without `prev_sha256` placed after chained lines is accepted.
The module docstring (`:18-20`) claims an edited or removed line breaks the chain.

Fix: the first chained line carries the SHA-256 of all bytes before it; `ledger_read` refuses an unchained line after a
chained one; every cycle verdict and the sprint ledger copy record `ledger_head`.

## C-7 (M) The v8 Appendix A block prints 0 admission trials

`atx-impl/tools/backtest_integrity.py:752-766` (`k`), `:560-601`, `scripts/research_cycle.py:818-822`.

`k` counts lines of kind `admission` on the window. The only writer, `ledger_record`, needs a NAV summary and a daily
CSV; `run --screen` skips summ and writes no line. No file in my area writes an admission line for a screened
candidate, so "admission trials this sprint 0" is printed on every result while the plan budgets up to 15.
Unverified: a writer outside my files (grep for `"admission"` with `ledger_append` across the repository would settle it).

Fix: `run --screen` appends one chained `admission` line per new member (count 1, origin, window_id), or the block reads
the count from the registry / verdicts and says so.

## C-8 (M) The bundle verdict uses the one-sided p; the pre-registration does not say which

`atx-impl/tools/nav_summ.py:593-598`.

Prereg item 9: "cumulative paired dSR > 0 with bootstrap p < .10", bootstrap "as coded in nav_summ.py". The p coded at
registration was the two-sided `lw.p_value`; the one-sided p was added by V-1 and is what `verdict.pass` uses. For a
positive dSR the one-sided p is about half the two-sided one: two-sided .15 is one-sided about .075, PASS instead of
FAIL. No ruling on the side is in `progress.md`. The arithmetic of both p values is correct.

Fix: a ruling written before V8-F is read; print both and name the gating one in the verdict.

## C-9 (M) `compare_window_overlap` can report `bit_identical: true` with no cell compared

`atx-impl/tools/compare_window_overlap.py:239, 249-256, 494`.

Kinds `field` and `signal`: when the cutoff leaves no common session (`--before` on or before the first session of both
roles) or the roles share no instrument and the old cells are NaN, each key row has `cells_compared 0`, `unequal 0`,
`missing 0`, hence `bit_identical: true`, and the total is true. Ruling W0-a reads that flag. (Kind `daily_ic` is safe:
no rows gives false.)

Fix: `bit_identical` needs `cells_compared > 0`; refuse an alignment with zero common sessions or instruments.

## C-10 (M) The overlap report lets W0-a be read on `max_abs_diff` while values appeared or vanished

`atx-impl/tools/compare_window_overlap.py:224-230, 543-546`.

A cell that is NaN on one side and finite on the other counts in `unequal_cells` and `nan_mismatch_cells` but never in
`max_abs_diff`. A key whose only differences are such cells has `max_abs_diff: null`; mixed with 1e-12 rounding noise
the total reads "below 1e-9". The stdout summary omits `nan_mismatch_cells`. The tool also gives no W0-a class and only
an absolute difference: a last-bit change in `me_company` (values near 1e11) is about 1e-5 and reads as "stop".

Fix: totals carry `w0a_class` = identical / below-tolerance / stop, with any NaN mismatch or missing old cell = stop;
add the largest relative difference; print `nan_mismatch_cells` on stdout.

## C-11 (M) `holdout_gate`: the ruling is not authenticated and a read leaves no record

`atx-impl/tools/holdout_gate.py:85-104, 200-215, 223-243`.

The ruling is a JSON file anyone can write; it pins the thresholds file by SHA, and a new thresholds file only needs a
new ruling file. The tool writes nothing. Nine runs bisecting `net_sharpe_min` over a range of 4 give each hidden
block's Sharpe to about .01, with no trace in the trial ledger (validation reads are counted by hand in Appendix A).
The two-bit output, the fixed exit-3 message and the order of checks are as specified.

Fix: before `evaluate`, append a chained ledger line (kind `validation`, ruling SHA, thresholds SHA, book); refuse when
a line for that book and block exists under another thresholds SHA; require the ruling file to be tracked at HEAD.

## C-12 (M) `cache gc --under` with any spelling other than the canonical relative one deletes referenced stores

`scripts/research_gc.py:39, 52, 82`.

Keep keys are `Path(d).as_posix()` of the spec's root-relative store. Candidates are `f"{base.rstrip('/')}/{name}"` with
`base` as typed. `--under ./build-equity`, `--under build-equity\` or an absolute path gives names that never equal a
keep key, so every store under it is listed as unreferenced and `--apply` removes it (the stores are regenerable; the
cost is the cold recompute and the byte-identity evidence of the cache). Windows case differences do the same.

Fix: resolve both sides to absolute paths under the root and compare those (`os.path.normcase`).

## C-13 (M) Resume treats an output as done by its marker file alone

`scripts/research_cycle.py:893-899, 967-968, 1036-1037, 1093, 1109-1118`; `scripts/cycle_verdict.py:63-80`.

`done` = `manifest.json` / `summary.json` / `index.json` / `composition_weights.json` / `marginal_ic.json` exists. The
receipt outcome and the pins the output was made from are not read. Example: a spec is re-locked (`lock --relock`)
after its library or weights changed, output names unchanged. The NAV dir of the earlier run has `summary.json`, so nav
is done; summ runs nav_summ on it (its argv carries no pin) and `cycle_verdict.json` binds the new `spec_sha256` to the
old cell's numbers. Same for a builder-made fields dir without `fields.check`. The IC passes are protected by their own
`--*-sha256` arguments; nav to summ is not. Mostly v7 logic; the verdict file and the derived shared stores are new.

Fix: for a done NAV, compare `summary.json`'s `combined_sha256`, role and fields pins with the values the step would
pass; treat a run dir whose receipt is not `completed` as failed even when the marker exists.

## C-14 (m) A full `run` after `run --screen` reuses the screen's u pass

`scripts/research_cycle.py:769-780`. The screen writes `U-<n>` with `--no-composition` and without `--save-combined`;
the full run finds it complete and marks u done, so the cell's u receipt differs from the argv `plan` prints.
Unverified: whether any full-run consumer (monitor, card, a `file` compare) needs the u-pass blend rows.

## C-15 (m) nav_summ drops the first scored session's turnover on a warm-start cell

`atx-impl/tools/nav_summ.py:200-211`. `deployment_row` is the first CSV row with a fill. With a warm start the
deployment is in the unreported warm-up, so row `score_begin` (a normal session) is dropped from tau, its cost and the
year table, and `warning_tau` fires on every warm cell because the C++ summary keeps that row (`strategy_nav_replay.cpp
:2288`, read by grep only). Effect about 1 session in 1,005. Fix: drop a row only when the summary says the deployment
is a reported row.

## C-16 (m) `--dsr-ledger` on a cell not yet ledgered uses N + 1 but leaves the cell out of V

`atx-impl/tools/nav_summ.py:546-565`. Consistent only when `--ledger` is given in the same call (append runs first).

## C-17 (m) Trial identity is the whole CSV file, not the (session, net) series

`atx-impl/tools/backtest_integrity.py:580, 600-607`. A binary that adds a column makes an identity re-run a new trial
(N too high; conservative) and makes `--dsr-ledger` treat a ledgered cell as new.

## C-18 (m) G-3a: a short with no SI/IO ratio pays the schedule median, which is the lowest fee

`atx-impl/tools/book_diagnostics.py:477-479`. Median of (25 x6, 30, 50, 150, 570) is 25 bps; the mean is 95. With 20% of
short dollars unrated the stress drag is understated by up to .2 x 70 = 14 bps a year on short value. The share is
reported; the docstring should say the choice is the cheapest tier.

## C-19 (m) `conftest.py` binds the superseded seal (2025-01-01) for every test under `atx-engine/tools`

`atx-engine/tools/conftest.py:15`, `scripts/research_tree.py:64-69`. Tests of the new v8 builders in that directory
cannot detect a read of 2024 rows. In one pytest process with `scripts/tests`, `research_tree.window_id()` imports the
rebound module and returns `research-seal-v1` (atx-impl tools use the private instance and are not affected). Known
deferred item; the fix is to redate the fixtures and delete the bind.

## C-20 (m) Report seal check: edge cases that read a hidden-named path

`atx-impl/tools/mega_report/data.py:32-33, 44-62` (diff only). (a) A path part of 16 or more digits is classed as a hash
and skipped: `nav_20240102093000123456.csv` is read. (b) `VAL` is matched in upper case only; `nav-val-1/` is read.
(c) The E-11 exemption is by root, for any file type: a CSV under `.superpowers/sdd/<sprint>/` with a 2024 date in its
name is read. Fix: hex class needs at least one a-f character; match `val` case-insensitively as a whole part; limit
the exemption to `.md`.

## C-21 (m) book_diagnostics checks the seal of cards and K6 only when the window key is present

`atx-impl/tools/book_diagnostics.py:762-763, 795-796`. A card index without `window.last_session_ns` and without
`daily_sleeve.csv` is read unchecked. Current cards write the key (`alpha_report_card.py:948`). Fix: refuse when absent.

## C-22 (m) `research_ledger.ledger_n` and `cells` read the ledger without the schema and chain checks

`scripts/research_ledger.py:64-90, 109-119`. `plan` prints an N from an unverified file; at run time nav_summ verifies
in the same step. `cells()` also returns lines of every kind and invalid or window re-run lines, which then enter
nav_summ's positional block (V[SR_n], PBO; the legacy figures of W0-h).

---

| id | sev | file:line | finding |
|---|---|---|---|
| C-1 | I | scripts/cycle_verdict.py:55 | verdict and headline DSR use Lo or mixed-window variance, not prereg item 3 |
| C-2 | M | scripts/research_cycle.py:913 | summ step does not pass `--protocol v8` / `--origin`; seed, draws, window_id missing |
| C-3 | M | atx-impl/tools/backtest_integrity.py:657 | defect / re-run flags on a ledgered cell are silently dropped |
| C-4 | M | atx-impl/tools/backtest_integrity.py:689 | `rerun_of` not checked; a window line with any id adds 0 |
| C-5 | M | atx-impl/tools/backtest_integrity.py:690 | blind re-run removes any named cell from N without a defect |
| C-6 | M | atx-impl/tools/backtest_integrity.py:636 | chain does not cover legacy lines, the tail, or later unchained lines |
| C-7 | M | atx-impl/tools/backtest_integrity.py:758 | Appendix A v8 prints 0 admission trials; no writer |
| C-8 | M | atx-impl/tools/nav_summ.py:593 | bundle gate uses one-sided p; prereg silent; no ruling |
| C-9 | M | atx-impl/tools/compare_window_overlap.py:239 | `bit_identical: true` with zero cells compared |
| C-10 | M | atx-impl/tools/compare_window_overlap.py:224 | NaN mismatches outside `max_abs_diff`; no W0-a class; absolute only |
| C-11 | M | atx-impl/tools/holdout_gate.py:200 | unauthenticated ruling, no read record: thresholds can be bisected |
| C-12 | M | scripts/research_gc.py:52 | non-canonical `--under` deletes referenced stores |
| C-13 | M | scripts/research_cycle.py:967 | resume by marker file; stale NAV scored under a new spec SHA |
| C-14 | m | scripts/research_cycle.py:773 | full run reuses the screen's `--no-composition` u pass |
| C-15 | m | atx-impl/tools/nav_summ.py:200 | warm start: first scored session dropped from tau; warning on every cell |
| C-16 | m | atx-impl/tools/nav_summ.py:550 | `--dsr-ledger` before ledgering: N + 1, V without the cell |
| C-17 | m | atx-impl/tools/backtest_integrity.py:600 | trial id on whole-file bytes |
| C-18 | m | atx-impl/tools/book_diagnostics.py:477 | unrated shorts pay the lowest fee |
| C-19 | m | atx-engine/tools/conftest.py:15 | superseded seal bound for all engine-tools tests |
| C-20 | m | atx-impl/tools/mega_report/data.py:32 | seal regex edge cases (16-digit parts, lower-case val, E-11 by root) |
| C-21 | m | atx-impl/tools/book_diagnostics.py:762 | cards / K6 seal check only when the key exists |
| C-22 | m | scripts/research_ledger.py:112 | N and cell list from an unverified ledger read |

Counts: I 1, M 12, m 9.

## Files fully reviewed (whole file read)
- `atx-impl/tools/nav_summ.py`, `atx-impl/tools/backtest_integrity.py`, `atx-impl/tools/holdout_gate.py`,
  `atx-impl/tools/compare_window_overlap.py`, `atx-impl/tools/book_diagnostics.py`, `atx-impl/tools/engine_tools.py`
- `scripts/research_cycle.py`, `scripts/research_ledger.py`, `scripts/research_tree.py`, `scripts/cycle_verdict.py`,
  `scripts/run_bounded_research.py` (no finding), `scripts/research_gc.py`, `scripts/research_add_alpha.py`
- `atx-engine/tools/conftest.py`, `atx-engine/tools/research_window.py`
- Context: plan sections 2-4, 7, 8, 12, Appendix A; `v8-prereg.md`; `progress.md`; reports A-2, A-3, V-1, V-2, W0-2-tool

## Files reviewed from the diff only
- `atx-impl/tools/mega_report/data.py`, `atx-impl/tools/mega_report/pitch.py`, `atx-impl/tools/book_monitor.py`

## Files not reached
- Every test: `scripts/tests/test_research_cycle.py`, `test_research_ledger.py`, `test_cycle_e2e.py`,
  `fixtures/tiny_world.py` and goldens; `atx-impl/tools/test_nav_summ.py`, `test_nav_summ_v8.py`,
  `test_backtest_integrity.py`, `test_holdout_gate.py`, `test_compare_window_overlap.py`, `test_book_diagnostics.py`,
  `test_mega_report_seal.py`. Hunt item 7 (tests that cannot fail, fixture masking) is not covered at all.
- `scripts/research-build.ps1`; `scripts/specs/*` content (only the `summ` block of `v71.json` was read)
- `atx-impl/tools/horizon_stats.py`, `fit_composition_weights.py` and `alpha_report_card.py` beyond the functions
  book_diagnostics calls (`centered_tied_ranks`, `Geometry`)
- Reports A-1, E-1, E-3, E-4, G-0, G, REPORT
- Not examined in depth: ONC clustering internals and k-means (read once, not derived); G-2a variance split against the
  C++ risk model; the `session_index` / holdings row mapping in G-3a against the C++ writer; Windows path length limits;
  hunt item 8 (reusability, duplicated logic: `norm_ppf`, `moments`, `expected_max_sr` and `deflated_sharpe` exist in
  both nav_summ and backtest_integrity, noted only).
