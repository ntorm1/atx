# Lane B2 re-review 1 (fix round 1 + B2-10)

## Verdict
**CHANGES_REQUIRED**: 2 Required, 7 Suggested.
- The identity merge point `1b25b9a4` is sound. Root may merge it now and run its TRAIN identity there. Both Required
  findings sit in the post-identity part (`598d2199` + `8fb17ad6`), which must not merge until they are fixed.
- The old findings are closed:
  - **R1 closed.** The stable forms are the same statistics (derivations below). The tolerance rule and the
    degenerate refusal are implemented as ruled.
  - **R2 closed and reconciled.** The first reviewer's 0.171 grid is reproduced (section (c)).
  - **R3 closed for the cell's summ.** The judge's bundle is the R3 residual: new R2.
- New R1: B2-10's summ-hash check runs in `verify`, but the summ only runs later, in `judge`. In a real wave every
  correctly named and pinned v2 cell is refused. The lane's test hides this because it calls `verify` after the whole
  wave has run.

## Reviewed SHAs
Lane head `26fe8d50` (branch `feat/p9-b2-20261003`, pool-14), lane base `1239a5ff`, previous review head `b76c2219`
(review `7395965c`).
- I read `b76c2219..26fe8d50`, the identity part `b76c2219..1b25b9a4`, the deletion `1b25b9a4..598d2199`, B2-10
  `598d2199..8fb17ad6`, and the deletion against the first deletion (`b76c2219..598d2199`, Python files only).
- The Python was exported read-only with `git archive` into my scratch dir: `1239a5ff` (base) and `1b25b9a4` (identity
  point).
- E2's head was read with `git -C pool-17 show 05a0fcd1:...`.
- No build: every question could be decided from the code, the lane's gtest logs and Python runs.

## Evidence (all run by me; synthetic data only; scratch deleted afterwards)
- **`scripts/tests/test_wave_eval_verb.py`** (pool-14, explicit path, `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`,
  `--basetemp` in scratch): exit 0, `3 passed in 5.38s`.
- **Statistics pytest:** `atx-engine/tests/fixtures/research_eval_edges/test_research_eval_edges_fixture.py` and
  `eval_tie/test_eval_tie_fixture.py`: exit 0, `12 passed in 4.08s`. The frozen copy still regenerates both fixtures.
- **`scripts/tests/test_cycle_scoring.py`:**
  - Without the two verb tests: `3 passed, 2 deselected`, exit 0.
  - `-k summ_runs_the_named_verb`: fails with `FileNotFoundError` reading the default verb. This is expected, because
    the lane deleted its build. Root must run it.
  - The `8fb17ad6` change to this path only moves a loop into `check_summ_verb`; the diff shows the same statements.
- **Stage-order probe** (scratch pytest that imports the lane's `wave_fixture` and spies on `eval_problem` during a real
  `research_wave run`). At the moment `verify` runs:
  - the probe printed `summ_exists_at_verify: False, judge_done: False`;
  - for the same cell spec with `exes.eval` named and pinned, as a correct v2 cell has it, `eval_problem` returns
    `'the v2 cell has no nav_summ --json rows (build-equity/cycle-w1/summ.json) to match to exes.eval aaaa…'`;
  - `1 passed`.
- **Flag-absent identity:** `nav_summ.py` at `1239a5ff` vs `1b25b9a4`, each in its own process, no `--engine-stats`.
  - Setup: 5 synthetic NAV dirs and a synthetic ledger written by `--ledger`.
  - argv: `--protocol v8 --draws 999 --psr --effective-n dirs --pbo --pbo-blocks 8 --dsr-ledger --dsr-total
    --dsr-hand`, plus `--json` and `--pbo-json`.
  - Run with a far reference, with a near reference (1 - rho = 4.2e-9) and as a `--bundle`.
  - stdout (108 lines), stderr, `--json` and `--pbo-json`, with `nav_summ_run` popped: **byte-equal** in all three.
- **Frozen copy:** the AST of `frozen_statistics.py` at `598d2199` against the functions at `1b25b9a4` is verbatim for
  35 of 36 functions. `dsr_at` differs only by unqualified names, as the first review found.
- **Statistics present at the identity point:** every deleted statistic is still callable at `1b25b9a4`
  (27 names checked):
  - `backtest_integrity`: psr, min_trl, deflated_sharpe, expected_max_sr, lo_null_dsr, effective_n_dsr, cscv_pbo, onc,
    onc_base, effective_trials, norm_ppf, moments, psr_report;
  - `nav_summ`: paired_stats, memmel_se, cbb_indices, bartlett_psi, dsr_rows, net_moments, norm_ppf, expected_max_sr,
    deflated_sharpe;
  - `dsr_total`: dsr_at, ledger_rows, hand_row.

## (a) Shape
- **`1b25b9a4`** keeps every Python statistic of record and its mirror-guard rows (`G-P5: 37 … 14 row(s)` in the lane's
  identity log). Root's identity run compares the Python with the engine there.
  - Between `d5f653cc` and `1b25b9a4` only the C++ house quantile and the edge fixture and gtest changed, so the
    lane's earlier flag-absent run carries over. My own run above confirms it.
- **`598d2199`** is the deletion plus changes the deletion forces:
  - default-verb resolution and lazy opening (`engine_of`), since every run now needs a verb;
  - `subprocess.Popen` in `EngineStats.run`, so tests that stand in for git through `subprocess.run` do not catch the
    verb call;
  - `book_diagnostics.engine_memmel`, generator retargets and test rewrites;
  - the mirror-guard rows;
  - the B2-R3 call-site pin, which ruling B2-8 places here.
  Nothing else is hidden in it. `backtest_integrity.py` gains only a docstring paragraph.
- **The p12_equal relaxation:** the switch from `json.dumps ==` to `p12_equal` in the old-blob tests was already in
  the first deletion (`b76c2219`). The first review accepted it under P12.

## (b) R1 / B2-R1
- **The stable forms are the same statistics.** With omr = 1 - rho:
  - **1 - rho.** za = (a - a_mean)/sa and zb = (b - b_mean)/sb, both ddof 1. Then sum((za - zb)^2) = (T-1) + (T-1)
    - 2(T-1)rho = 2(T-1)(1 - rho). This is `paired.cpp` `one_minus_rho`.
  - **Memmel.** The variance numerator has two terms. First, 2 - 2rho = 2 omr. Second,
    a^2 + b^2 - 2ab rho^2 = (a-b)^2 + 2ab(1 - (1-omr)^2) = (a-b)^2 + 2ab·omr(2-omr).
  - **LW s0.** Let z_k = g'(y_k - mean). Then g'Γ_l g = Σ_k (g'e_{k+l})(e_k'g)/T = Σ_k z_{k+l} z_k / T, and
    g'(Γ_l + Γ_l')g = 2 g'Γ_l g. So g'Ψg is the Bartlett long-run variance of z (`bartlett_quadratic`).
  - **Bootstrap s\*.** g'(Σ_j S_j S_j'/T)g = Σ_j (g'S_j)^2 / T, with g'S_j the block sum of z. Python's
    sqrt(g'Ψ*g/T) is therefore sqrt(q/T/T).
  - Printed rho stays the numpy-order Pearson value. NaN omr (sd 0) is not degenerate, and Memmel then gives NaN as
    the Python does.
- **Tolerance and refusal as ruled:**
  - `degenerate = omr < 1e-6`, using the stable omr.
  - A degenerate pair is refused: SE, t, LW se, CI and both p are null, `valid_draws` is 0, and dSR, rho, d0 and the
    CBB CI are still reported.
  - nav_summ keeps the Python keys for a non-degenerate pair and adds `degenerate` / `one_minus_rho` only for a
    degenerate one.
  - `test_nav_summ_engine.close` applies 1e-12/(1-rho) inside the `paired` block, and p-values and counts exactly.
- **Boundary:** the edge fixture has 1.02e-6 (full statistics, counts equal) and 0.98e-6 (refused). The gtests
  `EvalEdges.PairedTiesWithinTheConditioningBound` (4 cases) and `PairedRefusesADegeneratePair` (4 cases) pass in
  `b2-evidence/fix1r4-gtest-eval.log`.
- **Can the bootstrap p counts differ for a non-degenerate pair?** Not "cannot", but only at a threshold touch.
  - A count differs only if a draw's studentized statistic lies within the two codes' float gap of the threshold.
  - The engine is now near exact. The Python is off by about 1e-16/(1-rho) relative, about 1e-10 at 1-rho = 1e-6.
  - With 999 to 4999 draws the chance is about 1e-6 per p-value at the bound, and smaller above it.
  - If it happens on root's real run, it is a decision-field difference to report under B2-7. It must not be
    auto-accepted.

## (c) R2 / B2-R2 reconciled
- **The first reviewer's case is reproduced** (Python at `1b25b9a4`, synthetic). The grid is
  `np.random.default_rng(177).normal(0.001, 0.005, size=(240, 5))` plus a constant 0.001 column (index 5), with 8
  blocks.
  - `cscv_pbo` gives PBO 0.17142857 (12/70), logit mean 0.47961137, winners `[14, 27, 0, 29, 0, 0]`. This matches the
    review's 0.1714 / 0.4796 / engine `winner_counts[5] = 70`.
  - I searched 11,200 plausible grids: `default_rng` and `RandomState` normal draws, 7 means, 4 SDs, seeds 0..199.
    Only this one carries both numbers.
  - Seeds 0..2999 of that generator give the fingerprint only at seed 177.
  - The lane searched seeds 0..59, so it could not hit it.
  - 0.1714 vs the lane's 0.1857 (13/70) is two different random grids, not a contradiction. The engine's old 0.000 /
    ln 6 = 1.7918 is grid-independent: the residue Sharpe made the constant win every split.
- **The engine now matches on a constant candidate, zero or nonzero.** Three steps:
  1. **Python:** for any constant c, `_subset_sr` gives exactly 0 in every split.
     - The centred values are one exact residue δ (Sterbenz), here -4.3e-19. Its block sums and squares are exact
       small multiples, so var = q/n - m² = 0 exactly.
     - Measured: on the lane's grid and on the seed-177 grid, constants 0, 0.001, -0.0007, 0.0123456789, 1e-300 and
       3.3 give results identical to the zero column.
  2. **Engine:** `cscv_report.cpp` sends a constant column as zeros. `pbo.cpp` resolves it through the reference
     gather, where `std == 0.0 ? 0.0`, so the Sharpe is exactly 0. The tie rule
     `candidate < best || (candidate == best && c < winner)` and the first-max winner are the Python's.
  3. **Zero-column grids** matched before the fix (first review). `EvalEdges.CscvConstantCandidateHasSharpeZero` ties
     PBO, winners, ranks, logits and the winner statistics exactly on a nonzero-constant grid.
- **Residual:** a candidate constant over only some blocks.
  - Python gives a residue-over-residue SR, 1.7e-8 on my probe; the engine gives exactly 0.
  - Decisions differ only if another candidate sits in (0, 1.7e-8). This is negligible, and the lane documents it in
    `cscv_report.hpp`.

## (d) R3 / B2-R3 and B2-10
- **The producer is recorded:**
  - `EngineStats` hashes the resolved exe when it opens it.
  - `nav_summ_run` in `--json`, `--pbo-json` and `--bundle-json` carries `engine_exe` and `engine_exe_sha256`.
  - With a build key and a summ section, `effective_exes` adds `eval`. `lock --exes` pins it, and `verify_exes`
    refuses a pin mismatch (exit 3) before any step runs.
  - `summ_step` passes `--engine-stats exes.eval`.
  - `cycle_verdict.scoring_blocks` refuses a summ.json row or pbo.json of another or no verb (`VerdictError` → HARD-STOP).
- **No fallback survives:**
  - `default_engine_exe()` returns `$ATX_EQUITY_BIN/atx-research-eval.exe` whenever the variable is set, and
    `EngineStats` refuses a missing file there.
  - The only `build-equity/bin` default left (`nav_summ.py:160`) applies when ATX_EQUITY_BIN is unset. That is a
    default, not a fallback.
  - No other caller resolves a verb, except `book_diagnostics.engine_memmel` through the same default (S5).
- **A v2 cell without `driver.lock_exes`** is refused at verify, before the judge.
  - **Can it publish statistics from an unpinned or different verb? No.** The summ runs only in the judge.
  - The cost is that a library wave's screen stage has already ledgered its admission trials (S1).
- **The judge's bundle verb is not compared.**
  - **Can it publish statistics from an unpinned or different verb? Yes** (new R2).
- **The fake v2 test is not fair:**
  - The schema flip alone is a fair selector: E2's `wave_kinds.is_v2` reads the same `schema` key, and `eval_problem`
    does not depend on `kind`.
  - The calling sequence is not fair. Each test runs the whole v1 wave first, judge included, then calls `verify` with
    `done` cut at verify. So `summ.json` already exists.
  - On E2's head (`05a0fcd1`) the chain is the same: `run --stop-after nav` → `match` → `verify` → `judge`, where
    `cyc(w, "run", cell)` runs the summ. That makes new R1.

## (e) B2-9
- **The port matches the retired Python operation for operation:**
  - `0.5 * erfc(-x / sqrt 2)`, bisection on [-40, 40], at most 200 halvings, and the stop test
    `1e-15 * fmax(1, |mid|)`, returning the midpoint;
  - `kEulerMascheroni` == `EULER_GAMMA` == 0.5772156649015329, and `std::numbers::e` == `math.e`;
  - `house_dsr` is used by `house_moments.cpp:23` (house_v1, tot, cross, lo, hand) and `onc_house.cpp:512`, and no
    research path still calls `expected_max_sharpe`.
  - Gtests (lane log): 33 of 34 quantiles are the same double, and every house row is within 1e-12 for N from 2 to 1e6.
- **The one quantile that differs is 6 ulps apart, not 2:**
  - `ppf_lo` at N = 1e4 is `0x1.fb71fab2eea98p+1` vs `0x1.fb71fab2eea92p+1`, 6.7e-16 relative.
  - The bisection stops on an interval of about 9 ulps, so one flipped halving moves the midpoint a few ulps.
- **Effect on the house rows:** I recomputed SR0 and DSR at N = 1e4 with the engine's quantile on the fixture's three
  moment sets.
  - SR0 moves by at most 4.6e-16 relative and DSR by at most 3.8e-15 relative.
  - Every `.4f` DSR and `.5f` SR0 print is unchanged.
  - A printed digit or a decision could move only if a DSR lay within about 1e-16 of a rounding boundary or threshold.
    **No printed row or decision changes**, and B2-7's comparator would list such a line.

## (f) Old-blob tests
`test_backtest_integrity.test_legacy_output_byte_identical_to_the_v7_base`,
`test_dsr_total.test_flag_absent_is_byte_identical_to_the_pre_x_nav_summ` (argv 0) and
`test_nav_summ.test_existing_fields_and_text_unchanged_against_the_pre_t41_blob`. Against `b76c2219`, only the
reference argument changed. Each now names a reference that is not among the listed dirs:
- `make_cells(k=5)` with `*dirs, ref`: the generator is sequential, so the four listed cells are the same series as with
  `k=4`;
- `d["NEW"]`;
- a new `write_nav(.../ref)` on its own `default_rng(37)`, so the listed cells' draws are untouched.

No expected value or literal was edited. The assertions are unchanged: text byte-equal, JSON at P12. They still assert
what they asserted, minus the self pair, which ruling B2-6 removes from identity.

## (g) Suggested items of the first review, and seal
| item | state |
|---|---|
| S1 (quantile) | fixed as B2-9 (above) |
| S2 (null prev) | fixed: `chained()` in C++ and `.get("prev_sha256") is not None` in Python. Root check 8 applies |
| S3 (reader parity) | fixed: BOM, interior CR / NEL / LS / PS and a truthy non-object defect are refused. CRLF still reads, since `strip_line_end` runs before the check |
| S4 (trusted flags) | documented in `eval_cli.hpp` / `eval_report.hpp` |
| S5 (era functions) | dropped (`era_pool.cpp` -109 lines) |
| S6 (engine tests) | `test_nav_summ_engine` is tied to `frozen_statistics`, and nothing skips without the verb |
| S7 (nav_summ N) | `nav_summ_n` reads `deflated_ledger.n` from `nav_summ --json` |
| S8 (lazy verb) | `--ledger-n` alone opens no verb, since `results` is empty |
| S9 (100 columns) | fixed except `research_eval_edges_test.cpp:2` (101 columns; new S3). The 101-109 column lines in `pbo.cpp` predate the lane |
| S10 (ledger names) | `fixture-ledger.jsonl` |

- Flag-absent output is byte-identical to base at the identity point (evidence above).
- No window logic changed, so there is no look-ahead.
- Nothing dated 2024-01-01 or later was opened; my synthetic NAVs start at 2020-01-02.
- No 2020-2023 statistic was read; the trial ledger was not opened.

## Required
1. **`scripts/wave_stage_record.py:84-109`** (`eval_problem`), with the tests at `scripts/tests/test_wave_eval_verb.py:46-58,
   102, 108, 123`: major.
   - **Problem:** `verify` (line 47) checks the cell's `summ.json` / `pbo.json` verb hash. Verify runs before the
     judge, and the judge's `cyc(w, "run", cell)` (line 188) is what runs the summ. So for every real v2 cell, `summ.json`
     does not exist at verify, and a correctly named and pinned cell is refused with "has no nav_summ --json rows"
     (probe above).
     - On a resumed wave it would instead check a stale summ from an earlier judge.
     - The tests pass only because they run the whole wave, judge included, before calling verify.
   - **Fix:**
     - Keep at verify only the spec part: a locked cell spec with `exes.eval` and `exes_sha256.eval`, which fails closed
       before any read.
     - Drop the summ / pbo part from verify. The judge's research_cycle run already enforces it: `verify_exes` refuses a
       pin mismatch, the summ passes `--engine-stats exes.eval`, and `cycle_verdict.scoring_blocks(verb_sha256(cycle))`
       refuses a summ of another verb.
     - Optionally add an explicit `CV.check_summ_verb` for v2 in `judge` after `scored_verdict`.
     - Rewrite the tests to call verify in stage order (the state after `match`, before any judge output), and add a
       stage-order test: a real `research_wave run` of a v2-flipped manifest whose cell spec names and pins
       `exes.eval` passes verify.
     - Ruling B2-10 (c)'s text puts the hash check "at verify". The PM confirms the move (see final note).
2. **`scripts/wave_stage_record.py:143-168`** (`bundle_once`), with `scripts/wave_steps.py:125-131` (`bundle_argv`):
   major. This is the R3 residual.
   - **Problem:** the judge's PM5-23 bundle runs `nav_summ --bundle` without `--engine-stats`, so it uses nav_summ's
     default verb: ATX_EQUITY_BIN, else the checkout's Debug `build-equity/bin`.
     - Its dSR, Memmel SE, LW p two- and one-sided and `freeze_gate` go into `judge` → `wave-result.json` and are
       printed as "**Statistics of record** … bundle p one-sided …" (`scripts/wave_result.py:226-228`).
     - `bundle.json`'s `nav_summ_run.engine_exe_sha256` is recorded but never compared with the cell's pin, and a
       reused `bundle.json` is never re-checked.
     - So a P9 cell can publish statistics from an unpinned or different eval exe.
   - **Fix:** for a v2 manifest:
     - `bundle_argv` passes `--engine-stats <the cell spec's exes.eval>`;
     - `bundle_once` refuses, fail closed, a `bundle.json` (fresh or reused) whose
       `nav_summ_run.engine_exe_sha256` is not the cell spec's `exes_sha256.eval` pin (`CV.check_verb`);
     - a test covers both;
     - v1 keeps its bytes (ruling B2-10 (a)).
     These are E2's files. List the hunks for root under B2-8 as `8fb17ad6` did, as one more post-identity commit.

## Suggested
1. **`scripts/wave_stage_record.py:84`** (placement of the spec check): a v2 wave whose cell spec lacks `exes.eval` or
   its pin (no `driver.lock_exes` and an unpinned parent) is refused only at verify.
   - By then a library wave's screen stage has ledgered its admission trials.
   - Refuse it at preflight instead: the parent or template names `exes.eval` and `lock_exes(w)` holds. Then a doomed
     wave consumes no trial.
2. **`task-B2-report.md:388,475`; `atx-engine/tests/research/research_eval_edges_test.cpp:347`:** "2 ulps" is 6 ulps
   (6.7e-16 relative), and the comment "in fact it is the same double" holds for 33 of 34. Correct both before the
   ledger line is appended. The ledger's B2 line repeats "2 ulps".
3. **`atx-engine/tests/research/research_eval_edges_test.cpp:2`:** 101 columns.
4. **`task-B2-report.md:430-431`:** the classes.json request misses the new literal `atx.research-eval-edges-fixture/v1`.
5. **`atx-impl/tools/book_diagnostics.py:547,1480`:** G-3c's Memmel SE now comes from the default verb, and the output
   names no producer. Record `engine_exe` / `engine_exe_sha256` in the tool header when a verb ran. The diagnostic is
   descriptive, so this is not Required.
6. **`atx-engine/tests/research/research_eval_edges_test.cpp:398`:** `EXPECT_GT(acklam_worst, 1e-12)` pins the
   inaccuracy of another function, the shared Acklam quantile. Make it a printed note.
7. **`research_eval_edges_test.cpp:276`:** the CSCV gtest's constant never wins (winners `[57,4,9,0,0,0]`). Add a grid
   where every other candidate drifts negative, so the constant wins some splits, plus a zero column, tied to the
   frozen Python. That pins the tie path the derivation in (c) relies on.

## What root must check at merge
1. **Before the identity run:** measure and log rho for X-5 and Y-F0 against their pinned references (B2-R1 (d)).
   - 1 - rho < 1e-6: no paired test; the prereg names the fallback.
   - Otherwise: paired floats within 1e-12/(1-rho), and printed lines and counts byte-equal.
2. **Identity run on `1b25b9a4`:**
   - The argv never lists the reference among the dirs (B2-4 / B2-6).
   - Pop `nav_summ_run` before comparing.
   - The comparator follows B2-7: list every differing printed line with its float check, keep decision fields
     byte-equal, and report any such line, or any bootstrap-count difference (a threshold touch, about 1e-6 odds), to
     the PM.
3. **Re-run, with root's build,** `test_cycle_scoring.py::test_verdict_dsr_is_the_ledger_variance…` and
   `::test_the_summ_runs_the_named_verb_and_the_verdict_refuses_another`. The second fails without a verb, as observed
   here. Also re-run B2's fail-closed test on E2's split.
4. **Add `atx-research-eval` to `$canaryExes`** in `scripts/research-build.ps1`. After the deletion, a canary bin
   without it fails closed.
5. **Merge order:** merge `598d2199`, `8fb17ad6` and the fix commits for new R1 / R2 only after the identity run passes
   and those fixes are approved. Then move the B2-8 / B2-10 hunks onto E2's split:
   - `research_cycle.py` → `scripts/cycle/spec.py`: `EVAL_EXE`, `effective_exes`.
   - `research_cycle.py` → `scripts/cycle/phases.py`: `summ_step`.
   - `cycle_verdict.py`: `EVAL_KEY`, `verb_sha256`, `check_verb`, `check_summ_verb`, `scoring_blocks`, `verdict`.
   - `wave_stage_record.py`: `import cycle_verdict as CV`, `eval_problem` keyed on `SK.kind(w)` /
     `wave_kinds.is_v2` (drop `V2_SCHEMA`), and the `problems` line in `verify`.
   - The bundle hunks of new R2: `wave_steps.py`, `wave_stage_record.py`.
   - Tests: `test_research_cycle`, `test_cycle_scoring`, `test_research_ledger`, `test_no_python_mirror` and
     `test_wave_eval_verb`. On E2's split, use a real v2 manifest.
6. **`classes.json` (SQL2-CLS):** add `atx.research-eval/v1`, `atx.ledger-head/v1`, `atx.research-ledger-fixture/v1`
   and `atx.research-eval-edges-fixture/v1`. None is in `atx-engine/schemas/research_store/classes.json` at pool-2 HEAD.
   `atx.research-wave/v2` is E2's and predates this lane.
7. **Carried from the first review:** build `atx-impl-tests` and run `--gtest_filter=TrialLedger*`.
8. **S2 on real data:** count the lines of the ledger of record that carry `"prev_sha256": null` (a count only, no
   statistic read).
   - If any exist, the S2 change moves `chain_head` for every prefix at or after them. That affects
     `research_cycle.check_recorded_heads` (`research_cycle.py:2094` at `1b25b9a4`) and DSR_hand's
     `prefix_chain_head`, and a head recorded earlier would then be refused.
   - If none exist, nothing changes on real data.
