# Task ERA report: Ruling E-35 (pooled fit runs the v8 compositions) and review finding P-1

Branch `feat/platform-v8-era-20260930` from `b44774d6`, worktree `C:/atx-wt/pool-3`. Python only. Nothing built, no
era read, no real data: every test and check below runs on synthetic fixtures.

| item | commit | content |
|---|---|---|
| 1 Ruling E-35 | `80112395` | pooled fit implements ew-theme-std-v1 / ew-theme-std-aim-v1; explicit list; no fall-back |
| 2 P-1 | `dc84f27f` | history-read lines carry `era_of`; the three readers skip any history line |

## 1. Ruling E-35 (`atx-impl/tools/fit_composition_weights.py`)

- `POOLED_COMPOSITIONS = (ew-theme-v1, ew-theme-v6, ew-theme-std-v1, ew-theme-std-aim-v1)`. `fit()` checks it first
  when `--era` / `--era-id` is given. Any other id is refused with its name: "--era pools the prior screens with ...
  only: --composition X is not implemented by the pooled fit (no fall-back to another rule)". That covers unknown ids,
  ew-theme-aim-v1, mv-shrink and netcost. H-1's `prior and != ew-theme-aim-v1` check is gone; the list replaces it.
- `fit_prior`: its last branch used to give ew-theme-v1 weights to any prior composition it did not name. It now
  requires `ew-theme-v1` and otherwise refuses "fit: --composition X has no prior weight rule". This does not change
  any registered id: every one maps to the same branch as before.
- ew-theme-std-v1 needed no new code. `fit_prior` already calls `composition_rules.ew_theme_std` on the admitted
  members, and the pooled path runs `fit_prior` on the pooled admission. The new things are the tests, the explicit
  list, and the `theme_standardise` block in every era file (it comes with the document).
- **ew-theme-std-aim-v1** gets its E-27 gains (still `ew_theme_std(gains=)`) from the pooled decisions:
  - `era_aim_part(context, signal)` holds one era's aim inputs, computed over every scored decision of the era's
    role (the pool's explicit mask). It stores:
    - the lag correlations `c_j(d)` (the `lag_correlations` of `aim_profile`);
    - the count of ranked decisions;
    - the coverage.
  - Each era's role-keyed WorkStore keeps these parts under kind `aim-era`:
    - key schema `atx.fit-candidate-aim-era/v1`;
    - producers `("era_aim_part", "Context")`;
    - key field `mask` = every scored decision of the role.
  - `ensure_records(..., era_aim=True)` computes them. The anchor runs with `era_aim=pooled(args)`, and every other era
    goes through `era_state`.
  - `pool_aims(eras, digest)` builds the pooled aim records:
    - the eras' `c` side by side in date order (`era_pool.pool_matrix`), so no lag pair crosses an era;
    - then `correlation_profile` over that, with every pooled decision in the mask;
    - the ranked decisions summed and the coverage concatenated.
    The result equals `rank_autocorrelation` of the eras' block-diagonal rank panel (tested).
  - `fit()` passes `pool["aims"]` to `fit_prior`.
  - `aim_provenance` gained an optional `mask` (fit_prior passes its `train_mask`). Without it the result is unchanged.
- Refactor, so the pooled path calls the same code instead of copying it:
  - `aim_record` = `aim_ranks` (z and coverage) + `aim_profile` + `aim_body` (the record dict);
  - `aim_profile` = `lag_correlations` + `correlation_profile` (rho, g, halves) + `ranked_decisions`.
  - The values are the same: tested, and the identity runs below agree.
  - Side effect: the AIM_PRODUCERS fingerprint changes (`0ff4a042…` -> `f1fb0d3a…`), so every stored v8 aim record
    is a cache miss and is recomputed once. The FACTOR and CONTEXT fingerprints are unchanged (`962cd507…`, `70e1eda5…`).

Tests in `atx-impl/tools/test_fit_composition_weights_pool.py` (9 new, 25 in the file):

- **(a)** `test_pooled_std_fit_over_one_era_equals_the_single_window_fit`. The world is the R-1 fitter world, one
  role inside TRAIN; the pooled fit uses `--era-id E3`. Compared with the single-window fit:
  - The admission CSV is byte-equal.
  - `composition_weights.json` and `admission.json` are byte-equal (canonical bytes) once the keys the pool adds or
    replaces are removed. In the weights file those are `provenance.{pool, window, admission_sha256}`. In the admission
    file they are `pool`, `window` and `rules.train_window_ns`.
  - `schema`, `weights`, `signs`, `theme_standardise` and `provenance.std` are byte-equal with nothing removed.
- **(b)** `test_pooled_std_aim_fit_over_one_era_equals_the_single_window_fit`: the same comparison, and
  `provenance.aim` (rho, g, halves, coverage) is byte-equal too.
- **(c)** `test_a_composition_the_pooled_fit_does_not_implement_is_refused_by_name`. Three ids are refused with their
  names and nothing is written: an unknown id, ew-theme-aim-v1 and mv-shrink.
  `test_a_prior_composition_without_a_weight_rule_never_falls_back_to_ew_theme_v1` covers a prior id that is
  registered but has no rule: the single window refuses it in `fit_prior`, and the pooled fit refuses it by name.
- Two history eras (2014), with five members in two themes:
  - The std weights equal `ew_theme_std` of the pooled admission, and the E1 file is the same document bound to E1.
  - The std-aim gains equal `aim_gain(rank_autocorrelation(block panel))` to 1e-12. rho is checked at every lag, every
    gain is above .5 (history ranks are read), and the weights equal `ew_theme_std(gains=)`.
  - The era aim parts sit in each era's store, and a rerun reuses all of them with byte-equal outputs.

## 2. P-1

- `backtest_integrity.py` (FIX-C contracts kept: the chain, `check_line` and `check_rerun` are untouched):
  - `is_history_line(rec)` is true for any of: an `era` block, `era_of`, an `ERA` window label, or the `POOL` label.
  - `dsr_variance` (both variances) and `ledger_net_series` skip such lines.
  - `ledger_pool_records(..., era_of=None)`:
    - A one-era history read must carry `era_of` = the trial_id of the TRAIN cell it re-reads, 16 hex digits. It is
      refused without one.
    - `era_of` is refused on a one-era pool inside TRAIN and on a pool of two or more eras (those lines stay as H-1
      wrote them).
  - `ledger_append` runs a check after the batch, before anything is written. `check_era_of` requires each appended
    era line's `era_of` to name a cell line of the same kind, in the ledger or in the batch, that is not itself an era
    line. That is the TRAIN cell, or the pool's POOL line. `is_trial_id` is new.
- `scripts/research_ledger.py` `cells`: skips `is_history_line` lines. backtest_integrity is still imported only when
  a line has an era key or an ERA/POOL label.
- `nav_summ.py --era-of TRIAL_ID`: passed to `ledger_pool_records`. It needs `--pool` and `--ledger` (`ap.error`
  otherwise).
- `scripts/research_roles.py` (H-1 roles loop):
  - A single history role whose summ ledgers is refused at plan time unless `summ.extra` contains `--era-of`.
  - `--era-of` with two or more roles, or with a role inside TRAIN, is refused at plan time.
- Tests:
  - In `test_nav_summ_pool.py`: one fixture ledger (`p1_ledger`) with two v8 TRAIN cells and P-1's line: label
    `ERA E1`, an era block, the current window_id and no `era_of`, appended as the H-1 writer did. One test per reader:
    - `test_p1_dsr_variance_leaves_a_history_line_without_era_of_out`: 2 cells, the variance of the two TRAIN SRs;
    - `test_p1_research_ledger_cells_skips_a_history_line_without_era_of`;
    - `test_p1_ledger_net_series_skips_a_history_line_without_era_of`.
  - Run against this fixture ledger, b44774d6's readers count 3 / 3 / 3 and this branch's count 2 / 2 / 2 (scratch
    check).
  - `test_a_one_era_history_read_names_the_train_cell_it_re_reads` covers the writer:
    - refused without `--era-of`, with a malformed id, and with an id not in the ledger, appending nothing;
    - with the TRAIN cell's id: the line is an era line, adds 0 and is skipped by all three readers;
    - refused on two eras and on a TRAIN era; `--era-of` without `--pool`/`--ledger` exits 2.
  - The H-1 one-era history assertion moved from "no era_of, counts 1" to the new test.
  - `test_research_cycle_roles.py`: the history-role test now covers the plan-time refusal and the `--era-of` argv,
    and there is one more parametrised refusal.

## How root verifies

```bash
PY="C:/Program Files/Python312/python.exe"
"$PY" -m pytest -q -p no:cacheprovider atx-impl/tools/test_fit_composition_weights_pool.py \
  atx-impl/tools/test_nav_summ_pool.py scripts/tests/test_research_cycle_roles.py
"$PY" -m pytest -q -p no:cacheprovider atx-impl/tools scripts/tests atx-engine/tools/test_era_pool.py \
  atx-engine/tools/test_record_store.py atx-engine/tools/test_research_window.py
```

- Results:
  - first command: 62 passed (25 in the fitter pool file);
  - second command (full run): 666 passed, 6 skipped (the existing ATX_EQUITY_* / live-root gates), 367 s;
  - after the last research_roles edit, rerun of scripts/tests + the four ledger files: 203 passed, 5 skipped.
- Fitter identity (scratch; it executes b44774d6's fitter source with the worktree file's `__file__`, so
  `script_sha256` is equal). Every output byte and the summary are identical for:
  - the single window: ew-theme-v1, ew-theme-aim-v1, ew-theme-v6, ew-theme-std-v1, ew-theme-std-aim-v1, none,
    v3/mv-shrink, v3/netcost;
  - H-1's pooled ew-theme-v1 and ew-theme-v6 (two history eras), and a one-era ew-theme-v1 pool.

  Without `--era` the fitter's outputs change only in `script_sha256`, as after any edit.
- Ledger identity:
  - Lines without an era block, and every two-era pool, are written exactly as before: the H-1 tests
    `test_ledger_lines_per_era_plus_one_pooled_line_add_one_trial` and `test_each_era_has_receipt_and_ledger_line`
    pass unchanged.
  - Only a one-era history read changes: it now needs `--era-of`.

## Deviations from the brief

- The brief says "every history-read line carries `era_of`". I applied this to lines that carry an era block:
  - the one-era history line now names the TRAIN cell;
  - the era lines of a pool of two or more eras already name their POOL line.
  - The POOL line itself carries no `era_of`. It is the trial ("the read costs 1 trial", task-H brief), and
    `trial_counts` gives 0 to any line with `era_of`.
- The plan-time `--era-of` checks in research_roles.py, and the after-batch `check_era_of` in `ledger_append`, are
  additions so that a wrong or missing TRAIN cell fails before anything runs or is written.

## Cross-lane edits

- `atx-impl/tools/backtest_integrity.py` (V-1 / FIX-C file): only the P-1 predicate, the era_of rule and the readers.
  The brief allows this edit.
- `scripts/research_roles.py` and `scripts/tests/test_research_cycle_roles.py` (H-1's roles loop): plan-time
  `--era-of` checks. `scripts/research_cycle.py` is not touched.

## Open risks and concerns

1. **N for a history read on one role.** With `era_of`, `trial_counts` counts the one-era line as 0: the TRAIN cell
   stays the trial, as for a window re-run (prereg item 2). A pooled read of two or more eras still adds 1 through its
   POOL line, so a one-era read and a two-era read differ by one trial. If root wants every history read to cost 1,
   one rule in `trial_counts` has to change. Also, at plan time `ledger+1` gives N+1 for the one-era cell, but its line
   adds 0, the same as for window re-runs today.
2. **ew-theme-aim-v1 is still refused by the pooled fit.** Under E-27, R-3 on an ew-theme-v1 parent (R-1 rejected) is
   ew-theme-aim-v1, so a V8-F that carries it would block the OD-3 read. Enabling it is a one-line edit (add it to
   `POOLED_COMPOSITIONS`) plus the (b) test. The pooled aim machinery already serves it. This needs a ruling.
3. **Aim cache invalidation.** Stored aim records are recomputed once because of the fingerprint change. This costs
   compute time only; no output changes.
4. **E-28 caps on era w passes.** For the std compositions in a roles cycle, the caps come from the derived spec
   through research_cycle. I did not touch that or check it here.
5. **Size of the era aim parts.** Each candidate and era stores 37 x T floats (about 0.5 MB of JSON at T = 756).
   Each era's z is computed one candidate at a time, as for aim records.
