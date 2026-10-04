# Lane B2 review (wave 2): eval verb, ledger library, Python-statistics deletion

## Verdict
**CHANGES_REQUIRED** (BLOCK in TEMPLATES terms): 3 Required (major), 10 Suggested (minor).
- Spec verdict: **not met.** Ruling B2-2 says the port agrees to 1e-12 with decisions byte-equal. It does not for
  two inputs the lane's tests do not cover: highly correlated paired cells (R1) and a zero-variance CSCV candidate
  (R2). Every other carry item is met (list below).
- Quality verdict: **good, one blocking gap in the deletion commit (R3).**
  - The C++ builds clean under `/W4 /WX`. All 30 lane gtests and the 15 existing PBO gtests pass.
  - Flag-absent output is byte-identical.
  - The tests are strong: a rich ledger fixture, the RNG replayed bit for bit, and every tie value compared.

## Reviewed SHA
Lane head `b76c2219` (branch `feat/p9-b2-20261003`, pool-14), base `1239a5ff`, identity point `8538eb0d`.
- I reviewed `1239a5ff..8538eb0d` (the identity range) and `8538eb0d..b76c2219` (the report, then the deletion
  commit) separately.
- The C++ is the same at `8538eb0d` and `b76c2219`: the deletion commit touches Python, fixtures and docs only.
- No checkout was needed. The `8538eb0d` and `1239a5ff` Python was exported read-only with `git archive` into scratch
  trees.

## Evidence (run by the reviewer; scratch deleted afterwards, receipts kept)
- Builds, through `scripts/research-build.ps1`, equity-dev, pool-14, source `b76c2219`, 0 dirty entries:
  - `p9-b2-rv-a` failed at once, exit 1, because the tree was unconfigured ("could not load cache"). I configured it
    and rebuilt under a new tag.
  - `p9-b2-rv-b` (`atx-research-eval`, `atx-engine-research-eval-tests`, `atx-engine-research-ledger-tests`): exit 0,
    225 TUs, 12 links, 409 s.
  - `p9-b2-rv-c` (`atx-engine-w1-eval-tests`, the existing PBO tests after the `pbo.cpp` refactor): exit 0, 22 TUs.
  - The receipts are in `build-equity/mega-p9-b2-rv-{a,b,c}-*`. The object tree was deleted afterwards.
- gtests, anchored filters:
  - `atx-engine-research-ledger-tests --gtest_filter=ResearchLedger*`: exit 0, `[  PASSED  ] 14 tests.`
  - `atx-engine-research-eval-tests --gtest_filter=EvalVerb.*:ResearchEvalRng.*:Conditional.*`: exit 0,
    `[  PASSED  ] 16 tests.` (3622 ms)
  - `atx-engine-w1-eval-tests --gtest_filter=*Pbo*:*PBO*:*pbo*`: exit 0, `[  PASSED  ] 15 tests.` This covers
    `EvalPbo.*` 11, `BlockingPbo.*` 3 and `LearnNnGate.Pbo*` 1, so `pbo_cscv_checked` behaves the same after
    `run_cscv` was split out.
- pytest at the head, explicit paths, `PYTHONDONTWRITEBYTECODE=1 -p no:cacheprovider`,
  `ATX_EQUITY_BIN=<pool-14>/build-equity/bin`, `ATX_ENGINE_STATS_REQUIRED=1`:
  - The run listed 12 files: `test_nav_summ_engine`, `test_nav_summ`, `test_nav_summ_pool`, `test_nav_summ_v8`,
    `test_dsr_total`, `test_backtest_integrity`, `test_book_diagnostics`, `scripts/tests/test_no_python_mirror`,
    `test_research_ledger` and `test_cycle_scoring`, plus both fixture tests.
  - Result: exit 0, `G-P5: 19 mirrored rule function(s) in 8 row(s) left for P9 lanes` / `102 passed, 1 skipped in
    55.64s`.
  - The skip is `test_nav_summ_v8.py:72`, a pre-existing root-only real-data test.
  - Without the exe, `test_nav_summ_engine.py` gives `2 passed, 4 skipped`; see S6.
- pytest, wave suites (`test_research_wave`, `test_wave_review`, `test_wave_scoreboard`, `test_wave_driver`): exit 0,
  `58 passed in 164.41s`.
  - An earlier run of these 4 suites gave 23 failures. The cause was mine: I set `--basetemp` inside the git
    worktree, and `run_bounded_research.py --no-git` refuses a root inside a repository. It is not a lane defect.
- Flag-absent identity, `nav_summ.py` at `1239a5ff` vs `8538eb0d`:
  - argv: 5 synthetic dirs, `--reference`, `--protocol v8`, `--draws 999`, `--psr`, `--effective-n dirs`,
    `--pbo --pbo-blocks 8`, `--dsr-ledger`, `--dsr-total`, `--dsr-hand`, `--json` and `--pbo-json`, against a
    synthetic ledger written by `--ledger`. I ran it once with a far reference and once with a near-identical one.
  - Both runs: stdout, stderr, `--json` and `--pbo-json` are **byte-equal**.
  - The same argv with `--engine-stats` and the far reference: stdout byte-equal and JSON within 1e-12.
  - With the near-identical reference: stdout byte-equal, but 5 JSON values are outside 1e-12 (R1).
- Fidelity hunt: the `8538eb0d` Python against the built verb. Table below; the findings cite the numbers.

## Statistics port, formula by formula (8538eb0d Python vs C++)
| statistic | Python | C++ | verdict |
|---|---|---|---|
| Sharpe | `mean/sd(ddof 1)*sqrt(252)`, NaN if `sd<=0` or `T<2` | `house_sharpe`: same order, numpy pairwise sum | equal; NaN, T 1-2 and constant cases match |
| moments | population `e**3`, `e**4` by pow; `m2**1.5`, `m2**2`; None rules | `house_moments`: `pow(e,3)`, `pow(e,4)`, pairwise | equal, empty, T 1-2, NaN and constant all match |
| PSR / MinTRL | `(k-1)SR²/4` denominator; bisection `norm_ppf` | engine `probabilistic_sharpe`, `min_track_record_length`, `(exkurt+2)/4` with `exkurt = k-3` | `(k-3)+2 = k-1` exactly; within 1e-12 |
| DSR rows (cross / lo / house_v1 / tot / onc) | `expected_max_sr` with bisection `norm_ppf` | `expected_max_sharpe` with Acklam + Halley `norm_ppf` | within 1e-12 for N ≤ 1e3; beyond it for N ≥ 1e4 (S1) |
| Memmel SE | `max(var,0)` (NaN kept), `t` None if `se<=0` | same, including `std::max` NaN order | formula equal; rho conditioning (R1) |
| rho | `np.corrcoef`: BLAS dot, clip | sequential sums, the same divisions, clip | ~1e-16 absolute; amplified as rho → 1 (R1) |
| CBB | `default_rng(seed).integers(0,T,(draws,blocks))`, wrap, truncate | PCG64 / SeedSequence / Lemire port, buffered u32 | bit-equal (ResearchEvalRng) |
| LW / HAC | population-moment dSR, delta gradient, Bartlett `lag=block-1`, `min(lag,T-1)`, `/T`; block sums of the centred resample | the same algebra; column sums sequential, row means pairwise | within 1e-12 except rho → 1 and absurd mean/sd (R1) |
| p-values / ties | `(#{t*>=|d0|/s0}+1)/(valid+1)`; quantile linear, `_lerp` | same; `quantile_linear` ports `_lerp` | counts equal on every probe except rho = 1-6e-13 (R1) |
| CSCV PBO | `_subset_sr`: centred block sums, `var<=0` → SR 0; rank ties `oos==ow & idx<winner` | the engine CSCV (CachedMomentsV2 + reference) through `pbo_cscv_traced` | equal for plain, duplicate and constant-zero grids; differs for a constant nonzero candidate (R2) |
| ONC | k-means++ with `rng.choice(p)`, argmin / argmax first, silhouettes, recursion | house ONC port; picks replayed bit for bit | n_eff and clusters equal on every probe, duplicates and constants included |
| ledger counts | `trial_counts` / `ledger_n` / `dsr_variance` / `trial_breakdown` | the `trial_counts.cpp` port (bool, str and float `count`; NaN read as null) | N_c and N_tot equal on all probe ledgers |
| chain | `_walk` folds a null `prev_sha256`; `chain_head` treats the key as chained | `verify_chain` and `chain_head` copy both | faithful, but inconsistent (S2) |

## Required
1. **R1** `atx-engine/src/research/eval/paired.cpp:126` (`pearson`), `:74` (`bartlett_psi`), `:146` (`memmel_se`);
   `.superpowers/sdd/platform-p9-20261003/task-B2-report.md:235` | major
   - Problem: P12 (1e-12) fails for any highly correlated pair, not only the exact self-pair. The report's Concern 1
     and ruling B2-4 cover only the exact self-pair.
   - Measured on synthetic data, T 750, 999 draws (relative difference):

     | 1 - rho | worst relative difference |
     |---|---|
     | 5e-4 | 3.2e-12 (`lw.se` 1.3e-12) |
     | 5e-5 | 1.8e-11 (`memmel_se` 8e-12) |
     | 5e-6 | 2.9e-10 |
     | 5e-9 | 2.4e-7 |
     | 6e-13 | 1.3e-3, and `lw.p_value` 0.792 vs 0.791 (the bootstrap count differs) |

   - End to end, `nav_summ --engine-stats` with a near-identical `--reference` prints byte-equal lines. Its `--json`
     has `memmel_se`, `t`, `lw.se` and both `lw.ci95` bounds off by about 1e-7.
   - Cause: numpy forms rho and the HAC through BLAS dot / gemm and pairwise means; the port sums sequentially, and
     Memmel / LW amplify the rho error by about 1/(1 - rho).
   - Why it matters: a wave cell against its parent, or X-5 / Y-F0 against their bases, can easily have rho > 0.9995,
     so root's identity run can fail P12 without a defect.
   - Required fix: correct Concern 1, then either:
     - get a PM ruling that extends B2-4 to pairs with `1 - rho` below a stated bound (about 1e-3), with root
       checking each identity pair's rho before the run; or
     - compute rho and Psi in a form that matches numpy.

     Either way, pin the bound with a test.
2. **R2** `atx-engine/src/research/eval/cscv_report.cpp:34` (the traced engine CSCV in place of `cscv_pbo`) | major
   - Problem: the PBO decision differs on a zero-variance candidate.
   - Measured: 5 random cells plus one constant 0.001 series, 240 sessions, 8 blocks.

     | | PBO | logit mean | constant candidate |
     |---|---|---|---|
     | Python | 0.1714 | 0.4796 | centred block moments cancel to variance 0, so SR 0; it never wins |
     | engine | 0.0000 | 1.7918 | rounding-residue std gives a huge Sharpe; `winner_counts[5] = 70`, every IS split |

   - A degenerate cell makes the grid read as "no overfitting". The plain, duplicate-pair and constant-zero grids
     match exactly.
   - Required fix:
     - In `cscv_report`, give a candidate whose used values are constant (`ptp == 0`) SR 0, as Python does, or
       refuse the grid with a clear message (PM ruling).
     - Add a gtest with a constant nonzero candidate.
3. **R3** `atx-impl/tools/nav_summ.py:162-176` (`default_engine_exe`, `engine_of`), `:537-540` (`run_provenance`),
   `:1115` (deletion commit `b76c2219`) | major
   - Problem: after the deletion, every statistic of record comes from `atx-research-eval.exe`, but:
     - (a) its path and SHA-256 are recorded nowhere. `nav_summ_run` carries only `nav_summ.py`'s `script_sha256`.
       research_cycle's `exes_sha256` pin covers ic and nav, not the verb. So a summ output computed by an older or
       different verb is reused without refusal (E1-REUSE-a2).
     - (b) `default_engine_exe` silently falls back to `build-equity/bin` of the checkout when `$ATX_EQUITY_BIN` is set
       but lacks the exe. An equity-rel canary bin would then use a Debug, possibly stale, verb.
   - Required fix:
     - Record `engine_exe` and `engine_exe_sha256` in `nav_summ_run` and in the `--bundle-json` / `--pbo-json`
       provenance.
     - Refuse, with a clear message, when `ATX_EQUITY_BIN` is set without the exe.
     - Add to the root merge requests, beside the `$canaryExes` item: pin the verb in `exes_sha256` (E2's
       `research_cycle.py`).

## Suggested
1. **S1** `atx-engine/include/atx/engine/eval/stats_ext.hpp:97`, reached through
   `atx-engine/include/atx/engine/eval/deflated_sharpe_house.hpp:67` | minor
   - Problem: the house rows use the engine's Acklam + Halley `norm_ppf`, not the retired bisection. It agrees to
     1e-12 up to N = 1e3, but not beyond. Measured (relative difference):

     | N | difference |
     |---|---|
     | 1e4 | `lo.dsr` 1.0e-12 |
     | 1e5 | `sr0` 1.0e-12, `lo.dsr` 1.4e-11 |
     | 1e6 | `sr0` 5.5e-12, `cross.dsr` 1.9e-12 |
     | 1e7 | `sr0` 6.4e-12, `lo.dsr` 1.4e-10 |

   - No impact today: the recorded N_tot values are 13 to 233. Mining-campaign registry counts (up to 10,000
     each) can push N_tot past 1e4.
   - Fix: port the bisection for `house_dsr`, or state the bound and test the largest plausible N.
2. **S2** `atx-engine/src/research/ledger/chain.cpp:33` vs `:111` | minor
   - Problem: `chain_head` / `prefix_heads` treat a line with `"prev_sha256": null` as chained, while `verify_chain`
     and Python `_walk` / `ledger_head` fold it.
   - Measured on a legacy ledger with one null-prev line: verified head `531b3ee5…`, `chain_head` `590addc2…`.
   - This is inherited from Python's own `chain_head`. But it gives SQL-7 two rules, so SQL4's `verify --ledger`
     would refuse a head that `ledger-head.json` (read by `_walk`) recorded.
   - Fix: make `next_head` use the reader's test (`!is_null`) and add a test.
3. **S3** `atx-engine/src/research/ledger/trial_counts.cpp:168`, `ledger_file.cpp` / `json_line.cpp` | minor
   - Problem: the C++ reader accepts ledgers Python refuses:
     - a non-object `defect` (Python raises AttributeError; C++ counts the line);
     - a raw U+2028 in a string (Python's `splitlines` breaks the line);
     - a UTF-8 BOM before an all-legacy ledger (nlohmann skips it).
   - The writer never emits these, so there is no impact today.
   - Fix: fail closed for parity.
4. **S4** `atx-engine/src/research/eval/eval_report.cpp:566-567` | minor
   - Problem: the +1 in N_c / N_tot follows caller-asserted `--trial` and `--history`. A direct CLI caller can lower
     N by one; nav_summ derives both correctly.
   - Fix: document both flags as trusted-caller flags in K-P9-5, or derive `history` from the series.
5. **S5** `atx-engine/src/research/ledger/era_pool.cpp:85,99,140,170` | minor
   - Problem: `check_era_id`, `check_era_windows`, `segment_starts` and `weighted_mean` have no caller and no gtest.
     Only `pooled_sha256`, `trial_id` and `pool_label` are tied to `era_pool.py`. That leaves an untested second copy
     of the era rules.
   - Fix: tie them to `era_pool.py` on the fixture, or drop them until a caller exists.
6. **S6** `atx-impl/tools/test_nav_summ_engine.py:27,86` | minor
   - Problem: after the deletion these tests compare the engine with itself, because both runs use the verb.
     Without `ATX_ENGINE_STATS_REQUIRED=1` they still skip when the exe is missing, while every other suite now fails
     loudly.
   - Fix: retire them in the deletion commit, or compare against `frozen_statistics.py`.
7. **S7** `scripts/tests/test_research_ledger.py:61` | minor
   - Problem: `nav_summ_n` now returns `BI.ledger_n` instead of what nav_summ prints. The check "nav_summ N = cycle N"
     no longer reads the engine's `house_v1.n`.
   - Fix: read N from `nav_summ --json`.
8. **S8** `atx-impl/tools/nav_summ.py:1115` | minor
   - Problem: `EngineStats` is built on every run, so `--ledger-n LEDGER` alone (Appendix A, no statistic) now needs
     the verb.
   - Fix: build it lazily.
9. **S9** `atx-engine/src/research/eval/onc_house.cpp:258`, `paired.cpp:240` | minor
   - Problem: 101 columns, over the 100-column limit.
10. **S10** `atx-engine/tests/research/research_eval_tie_test.cpp:106`; `test_nav_summ_engine.py:89` | minor
    - Problem: synthetic test ledgers are named `trials.jsonl`.
    - Fix: rename them (`fixture-ledger.jsonl`) so they cannot be mistaken for the ledger of record.

## Carry and brief items, one by one
- **SQL-7** chain-head function: met. `chain_head` / `prefix_heads` / `kChainRule` are public, and one copy of the rule
  is used by the reader and by DSR_hand. The `prev_sha256: null` nit is S2.
- **SQL-9** ledger untouched: met.
  - The verb only reads. The only C++ writer is the IC ledger, moved verbatim: `diff --strip-trailing-cr` shows a
    namespace, include and comment change only.
  - No test writes `build-equity/trials.jsonl`.
- **P11 / G-P7** tie gtests through the verb: met. `EvalVerb.Tie*` pass at 1e-12 relative.
- **T1 minor** (keep the eval_tie checker runnable): met.
  - An AST comparison shows `frozen_statistics.py` verbatim for 35 of 36 functions. `dsr_at` differs only by
    unqualified names that resolve to the same numerics.
  - `test_eval_tie_fixture.py` gives 7 passed.
- **B2-1** separate final deletion commit: met (`b76c2219`).
- **B2-2** `paired.se` = Python `lw.se`: met, and flagged in the report. The "decisions byte-equal" part is not met
  (R1, R2).
- **B2-3** schema added only in the test JSONL: met.
- **B2-4** self-pair: applied. It is too narrow for P12 (R1).
- **B2-5** archival `v6_scorecard.py` left as it is: met. It is the only remaining caller of a deleted function.
- **K-P9-4a** (B1's factor bytes untouched) and **P14** (`backtest_integrity.py` edits, cross-lane with E2): met.
- **Carried B1 minors**: not fixed. They live in B1's admission files; the PM decides whether B2 owes them.
- **Brief gtests**: all present and passing.
  - The brief names `ResearchLedger.TrialCountsEqualPython`, `EvalVerb.JsonSchema`, `Conditional.ClosedFormNormalCase`
    and the T1 tie through the verb.
  - `wave_rules.py` (forbidden) is untouched.
  - The conditional (AKM) result is printed only; no rule reads it.
- **Deletion scope**:
  - Removed: `backtest_integrity` (27 symbols), `nav_summ` (15) and `dsr_total` (3), exactly the report's list.
  - Every library caller (`holdout_gate`, `wave_readers`, `mega_report`, `book_diagnostics`) uses only kept names or
    the verb.
  - Without the exe, nav_summ refuses with "no engine statistics executable …" and never skips (except S6).

## What root must check at merge
1. Before the identity run, compute rho for each identity pair (X-5 and Y-F0 against their references). If
   `1 - rho` is below about 1e-3, P12 will fail without a defect: get the R1 ruling first. Run the `--json`
   comparison with `nav_summ_run` popped.
2. Build `atx-research-eval` and add it to `$canaryExes` (the lane's request). Also build `atx-impl-tests` and run
   `--gtest_filter=TrialLedger*`; the moved code is not run by this review.
3. Do not merge `b76c2219` before the identity run passes and R3 is fixed. Re-review the fix SHA.
4. Add the schema literals `atx.research-eval/v1`, `atx.ledger-head/v1` and `atx.research-ledger-fixture/v1` to
   `classes.json` (ruling SQL2-CLS).
5. Note that the record stage writes `ledger-head.json` into the wave dir and copies it to `record.copy_to`. Nothing
   commits it; the plan says "commits", the brief says "writes".

## Checked
- [x] `.agents/cpp/agent.md` §10 applied to the C++ diff:
  - Lifetime, bounds and error paths look right. `evaluate()` catches json / bad_alloc / std exceptions, and the
    recursion depth is bounded.
  - The build is clean at `/W4 /WX`.
- [x] The diff stays inside the brief's scope. Cross-lane edits are all listed: CMake tails, `pbo.{hpp,cpp}`,
  `wave_stage_record.py`, `book_diagnostics.py`, the T1 guard and the tests.
- [x] The report's evidence was re-run and matches: 225 TUs, gtests 14 + 16, pytest 102 passed + 1 skipped.
- Reviewer side effects:
  - The pytest suites wrote synthetic `trials.jsonl` fixtures inside pytest temp dirs, since deleted. The real
    ledger was never opened or written.
  - No real data was read.
