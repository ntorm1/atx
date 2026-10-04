# Lane B2 handoff: fix round 1, stopped by the owner

**Where:** pool-14, branch `feat/p9-b2-20261003`.
- Base: `1239a5ff`.
- Head: the commit that adds this file. Its parent is `dee6cacb` (the WIP deletion).
- **Fix round 1 is about 75% done.**
  - Every change before the deletion is committed, built and tested.
  - The re-applied deletion is committed as WIP and is not finished.
  - The report's "Fix round 1" section is not written yet.
- **Inputs:**
  - The review: `task-B2-review.md` (commit `7395965c`).
  - Rulings B2-R1, B2-R2 and B2-R3: in `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/progress.md`, lines 310-312.

## Commits of this round, in order

| commit | what |
|---|---|
| `d12dd945` | Reverts the deletion `b76c2219`, without rewriting history (the lane A3 pattern). |
| `ec8043b7` | **B2-R1 and B2-R2 in C++.**<br>- Paired statistics are formed from the difference series.<br>- A pair with 1 - rho < 1e-6 is refused as degenerate.<br>- A constant CSCV candidate gets Sharpe 0.<br>- Adds the `fixtures/research_eval_edges` fixture and the `EvalEdges.*` gtests.<br>- Documents `--trial` / `--history` as trusted-caller flags (S4). |
| `a12b4c30` | **nav_summ, B2-R1 (c) and the pre-deletion part of B2-R3.**<br>- The adapter keeps a degenerate pair's refused values null and prints `na` plus one degenerate line.<br>- With an engine, `nav_summ_run` gains `engine_exe` and `engine_exe_sha256`.<br>- Updates `test_nav_summ_engine.py`.<br>- Renames that test's synthetic ledger to `fixture-ledger.jsonl` (S10). |
| `b295fa4f` | **Suggested fixes S2, S3, S5, S9 and S10** (details below). |
| `f07ba1be` | Fixes a `-Wrange-loop-construct` error in the S3 test. |
| `c85dc6f2` | Fixes Python truthiness of a string `defect` in the S3 reader check. |
| `d5f653cc` | Moves the edge fixture's 5e-4 case to 5.005e-4. At exactly 5e-4, rho = 0.9995 sits on the rounding boundary of the 3-decimal rho print, so a 1e-16 difference flipped the printed digit. |
| `dee6cacb` | **WIP, not for merge:** the deletion re-applied, unfinished (details below). |

**Identity merge point: `d5f653cc`.** All identity-relevant code is at or before it.
- **The deletion sits at `dee6cacb` and is incomplete.** Once it is finished, it must again be the last code commit.
- Two options when finishing it:
  - Add follow-up commits, then report the final deletion SHA as the range `d5f653cc..<last>`.
  - Or squash the WIP and its follow-ups into one new commit. That rewrites only commits made this round, never `b76c2219` or earlier.

## State per item

### B2-R1 (a): difference-series forms — DONE in `ec8043b7`
In `paired.cpp`, all of the following are now computed from the difference series:
- **1 - rho:** `sum((za - zb)^2) / (2 (T - 1))`.
- **Memmel SE:** uses `2 omr` and `(a - b)^2 + 2 a b omr (2 - omr)`, where `omr` is 1 - rho.
- **LW point SE `s0`:** the Bartlett long-run variance of the scalar influence series `g'(y_k - mean)`.
- **Bootstrap SE `s*`:** the sum of squared block sums of that series.

`rho` itself is still the numpy-order Pearson value.

### B2-R1 (b): tolerance — DONE
- **Tolerance:** 1e-12 / (1 - rho), used in `EvalEdges.PairedTiesWithinTheConditioningBound` and in `test_nav_summ_engine.close`, where counts and p-values are compared exactly.
- **Printed lines:** byte-equal at 1 - rho = 5.005e-4, 5e-5, 5e-6 and 1.02e-6, checked through nav_summ in `test_near_identical_pairs_follow_the_conditioning_rule`.
- **Caveat:** a printed value that sits exactly on a rounding boundary can still flip. That is how the original 5e-4 case failed; no tolerance can prevent it.

### B2-R1 (c): degenerate refusal — DONE
- **Rule:** `kDegenerateOneMinusRho = 1e-6`, applied to the stable 1 - rho.
- **Refused:** `memmel_se`, `t`, `lw.se`, the LW CI, `p_value` and `p_one_sided` are null; `valid_draws` is 0.
- **Still reported:** dSR, rho, d0 and the CBB interval.
- **Verb JSON:** the paired block gains `one_minus_rho` and `degenerate`, and the text print adds one line.
- **nav_summ:**
  - Prints "Memmel SE na t na" and the degenerate line.
  - In `--json`, `degenerate` and `one_minus_rho` appear only on a degenerate pair, so every other pair has the Python's keys.
- **Self pair:** it now prints SE "na", where ruling B2-4 said SE 0. B2-R1 (c) says "same as the self-pair", and I applied "na" to both. **The PM should confirm this.**

### B2-R1 (d): tests — DONE
- **Fixture** `atx-engine/tests/fixtures/research_eval_edges/`:
  - Contains the generator, `expected.json` and a pytest.
  - The series come from an exact LCG, repeated bit for bit by the gtest, so no series file is committed.
  - Cases: 1 - rho = 5.005e-4, 5e-5, 5e-6, 1.02e-6, 0.98e-6, 5e-9, 6e-13, and the self pair. T is 750, with 999 draws.
- **Gtests:** `EvalEdges.*` (5 tests).
- **pytest:** `test_near_identical_pairs_follow_the_conditioning_rule[...]` (7 cases).
- **Still owed:** Concern 1 in the report must be updated. Text to use:
  - The engine now forms these statistics stably.
  - The tolerance is 1e-12 / (1 - rho).
  - Below 1e-6 the pair is refused.
  - Root measures rho for the X-5 and Y-F0 pairs.

### B2-R2: constant CSCV candidate — DONE in `ec8043b7`
- **Fix:** `cscv_report.cpp` passes a column whose used returns are all equal to the engine CSCV as zeros, so its Sharpe is 0 in every split. That is exactly the Python's `var > 0` rule, because the Python's centred moments are exactly 0 for such a column.
- **JSON:** the column is listed in `pbo.engine.constant_candidates`.
- **Test:** `EvalEdges.CscvConstantCandidateHasSharpeZero`, on 5 generated candidates plus a constant 0.001 column (240 sessions, 8 blocks). The engine now equals the Python exactly: PBO 0.1857, logits and winner counts `[57,4,9,0,0,0]`.
- **Not "0.171":** the review's exact series is unknown. A search over numpy seeds 0..59 did not reproduce it.
- **Not covered:** a candidate that is constant over only some blocks. Both implementations divide by a rounding residue there. This is documented in `cscv_report.hpp`.

### B2-R3: name and pin the verb — PARTIAL
**Done before the deletion (`a12b4c30`):**
- `EngineStats` hashes the exe when it opens it.
- `nav_summ_run` in `--json`, `--pbo-json` and `--bundle-json` records `engine_exe` and `engine_exe_sha256` whenever an engine was used.
- The test is `test_the_engine_run_names_its_verb`.

**In the WIP `dee6cacb`, untested:**
- `default_engine_exe` returns `$ATX_EQUITY_BIN/atx-research-eval.exe` whenever `ATX_EQUITY_BIN` is set, so a missing exe there fails closed in `EngineStats`.
- Lazy `engine_of`, which is S8.

**NOT done:**
1. **`scripts/research_cycle.py`** (cross-lane with E2):
   - `effective_exes` adds `"eval": "atx-research-eval.exe"` when `"summ"` is in the spec. Then `lock --exes` pins it and `verify_exes` checks it.
   - `summ_step` passes `--engine-stats <exes.eval>` right after the script when `exes.eval` is set.
   - Do **not** add a `--bind` for the exe: `run_bounded_research` refuses binds over 16 MiB, and the Debug exe is about 2.2 MB, so it fits, but a Release exe might not.
   - Update `test_build_key_resolves_exe_dir`, which asserts the exact exes dict, and add a test.
2. **`scripts/cycle_verdict.py` `scoring_blocks`:** refuse a `summ.json` or `pbo.json` whose `nav_summ_run.engine_exe_sha256` differs from the cycle's eval pin. Without a pin, compare against the sha of `spec.exes.eval`. Raise `VerdictError` ("not reused"). Add a test in `test_cycle_scoring.py`.
3. **Test for the fail-closed rule:** `ATX_EQUITY_BIN` set to a dir without the exe means `NS.main` raises SystemExit. Reset `NS._ENGINE` first, because it caches the default verb per process.
4. **Merge requests to add to the report:**
   - Add `atx-research-eval` to `$canaryExes` in `research-build.ps1`.
   - The `exes_sha256` pin, if root prefers E2 to own `research_cycle`.

### Suggested
| item | state | where | detail or reason |
|---|---|---|---|
| S1 (norm_ppf) | not done | — | The house rows use the engine's shared Acklam+Halley `norm_ppf` (`stats_ext.hpp`), which other callers use too. Recorded N_tot is 13..233, and 1e-12 holds up to N = 1e3. List with this reason; the PM may rule a bound. |
| S2 | done | `b295fa4f` | `chain_head`, `prefix_heads` and `is_chained_line` treat a line as chained only when its `prev_sha256` is non-null, in C++ and in `backtest_integrity.chain_head`. The rule name stays `/v1`. Tests: gtest `ANullPrevLineFoldsInEveryRule` and pytest `test_a_null_prev_line_folds_for_chain_head_as_for_the_reader`. |
| S3 | done | `b295fa4f`, `c85dc6f2` | The reader refuses three inputs: a BOM at a line start; a raw CR, U+0085, U+2028 or U+2029 inside a line; and a truthy non-object `defect`. Test: `ResearchLedger.ReaderRefusesWhatThePythonRefuses`. |
| S4 | done, as docs | `ec8043b7` | `eval_cli.hpp` and `eval_report.hpp` document the flags. K-P9-5 itself lives in the plan, which I did not edit. |
| S5 | done | `b295fa4f` | Dropped the uncalled `check_era_id`, `check_era_windows`, `segment_starts` and `weighted_mean`. |
| S6 | not done | belongs in the deletion | After the deletion, `test_nav_summ_engine` compares the engine with itself. Option 1: retire the comparison tests and keep the arg, refusal, provenance and degenerate tests, which never skip. Option 2: compare `nav_summ --json` against `frozen_statistics.paired_stats` at the conditioning tolerance. The edge cases there import `generate_research_eval_edges`, whose `paired_case` needs the generator retarget below. |
| S7 | not done | belongs in the deletion | In `scripts/tests/test_research_ledger.py`, `nav_summ_n` should read N from `nav_summ --json` (`deflated_ledger.n` or `house_v1`) instead of `BI.ledger_n`. |
| S8 | WIP | `dee6cacb` | Lazy engine. |
| S9 | done | `b295fa4f` | Lines over 100 columns. |
| S10 | done | `a12b4c30`, `b295fa4f` | `fixture-ledger.jsonl`. |

**Deletion follow-up besides R3, S6 and S7:** retarget `generate_research_eval_edges.py` from `nav_summ` and `backtest_integrity` to `fixtures/eval_tie/frozen_statistics.py` (`paired_stats`, `cscv_pbo`), the same way `generate_eval_tie.py` was retargeted. Otherwise its import of `NS.paired_stats` breaks after the deletion.

## Builds and tests run
All builds used `scripts/research-build.ps1` with the equity-dev preset. The receipts are in `build-equity/mega-p9-b2-c*`.

**Builds:**
- `p9-b2-c` (dirty tree): FAILED on the `-Wrange-loop-construct` error, fixed in `f07ba1be`.
- `p9-b2-c2`: source `f07ba1be`, 0 dirty entries, exit 0, 11 TUs.
- `p9-b2-c3`: source `c85dc6f2`, exit 0. The tree was configured fresh this round.

**gtests, run at `c85dc6f2` and `d5f653cc` (logs in `b2-evidence/fix1-gtest-*.log`):**
- `atx-engine-research-ledger-tests --gtest_filter=ResearchLedger*`: 16 of 16 pass.
- `atx-engine-research-eval-tests --gtest_filter=EvalVerb.*:ResearchEvalRng.*:Conditional.*:EvalEdges.*`: 21 of 21 pass. The `eval_tie` tie still holds at 1e-12.

**pytest before the deletion, at `d5f653cc`** (`ATX_EQUITY_BIN` set to the pool-14 bin, `ATX_ENGINE_STATS_REQUIRED=1`):
- The nav_summ, ledger and fixture suites (14 files): 134 passed, 1 skipped, the known root-only v8 test. Log: `b2-evidence/fix1-pytest-pre-deletion.log`.
- The wave, holdout and mega-report suites: 146 passed. Log: `b2-evidence/fix1-pytest-pre-deletion-wave.log`.

**Flag-absent identity, nav_summ at `1239a5ff` vs `d5f653cc`:**
- Setup: 5 synthetic dirs, a v8 argv with PSR, effective N, PBO, the ledger DSR, the total DSR and the hand DSR, and a shared pristine ledger. Run once with a far reference and once with a near-identical one (1 - rho = 1.25e-10).
- Result: stdout, stderr, `--json` and `--pbo-json` are **byte-equal** in both runs.

**`--engine-stats` against Python at `d5f653cc`, same setup:**
- Far reference: stdout equal, 0 JSON differences.
- Near reference: the pair is refused as degenerate. As expected, its stdout differs and its JSON gains `degenerate` and `one_minus_rho`.
- These two scripts were scratch and are deleted. Their content is described here.

**Not run:** anything after the deletion. The WIP has not been tested.

## Exact next steps
1. In pool-14 at the head, finish the deletion:
   - B2-R3 in `research_cycle.py` and `cycle_verdict.py`, with tests.
   - The fail-closed test.
   - S6, S7 and the edge-generator retarget.
   - Commit as follow-up commits, or squash this round's WIP into one deletion commit.
2. Run the post-deletion pytests: the same 14 files plus the wave, holdout and mega-report suites, plus `scripts/tests/test_research_cycle.py`, `test_wave_driver.py` and `test_research_spec.py` (because of `research_cycle`). Use `ATX_EQUITY_BIN` and `ATX_ENGINE_STATS_REQUIRED=1`.
3. Rebuild only if C++ changes; the exe at `d5f653cc` is current, with sha256 `2398a582…` from `p9-b2-c2`. Check that `p9-b2-c3`'s exe is the same.
4. Append "Fix round 1" to `task-B2-report.md`:
   - The new identity merge point is `d5f653cc`, plus the deletion SHA.
   - Concern 1 rewritten per B2-R1.
   - B2-R2, B2-R3 and the Suggested table above.
   - Merge requests: `$canaryExes`, and root checking each identity pair's rho first.
   - Commit with `git add -f`.
5. Delete the `build-equity` object tree (DISK-2), keeping `mega-*` receipts and logs. (The gtest scratch dirs
   `%TEMP%/atx_research_eval_tie` / `_edges` were checked: absent.)

## Open questions for the PM
1. **Self pair:** it now prints SE "na" (B2-R1 (c)) instead of SE 0 (B2-4). Confirm.
2. **Rounding boundaries:** printed lines can still differ when a printed value lands exactly on a rounding boundary. The 5e-4 case hit this on the 3-decimal rho print. The probability is negligible for real data. Is a note enough?
3. **B2-R3 placement:** I put the `research_cycle` / `cycle_verdict` pin of the verb in the deletion part, because before the deletion the summ's statistics are `nav_summ.py`'s own and `script_sha256` already covers them. These are E2's files. Who owns this edit?
4. **S1:** port the bisection `norm_ppf` for the house rows, or rule a bound (N_tot ≤ 1e3 holds 1e-12)?

## Scratch
- Deleted by name: my scratchpad files `b2fix_*`, `b2c*_*`, `b2r1_*`, `b2fix_base/` and `b2fix_flagwork/`.
- My stash `b2-fix1-deletion-wip-2` was popped into `dee6cacb`, so no stash of mine remains.
- Left in place: the `build-equity` object tree, which the next session needs for the exe and the gtests.
