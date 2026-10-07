# Task B2 report: one engine `eval` verb and the ledger library (P9 wave 2, merge slot 4)

## Outcome

**Fix round 1 (end of file) supersedes the SHAs below: identity merge point `1b25b9a4`, deletion commit `598d2199`.**

**R9 (2026-10-07): the two pending suites are done on `52cdcc8c` (root `685e238b` merged, docs only, no conflict),
Debug `p9-b2-q`: `scripts/tests` seed 1 527 passed / 5 skipped, stage chain 10 passed per seed, 0 failed (section
"R8 round", table cells and "R9 completion"). Nothing is pending in the lane; next: re-review 2 (B2-SEQ).**

**Post-identity redo (last section): R8 (2026-10-06), stopped by the owner with two suite runs pending. Root
`fd084e78` merged (`1b15330d`); PRE-15 (`fe152fca`), B2-11 with re-review R1 / R2 (`d8f80c7a`), D2R-2 (b)
(`34838462`), the G-P5 placement; builds `p9-b2-o` (Debug) / `p9-b2-p` (Release); every named suite green on
`7832719f`, and on `1b15330d` all but `scripts/tests` seed 1 and the stage chain (section "R8 round"). B2-11's teeth
by mutation are not shown (the mutation Edit was refused in R7; PERM-1).**

DONE_WITH_CONCERNS. Pool `C:/atx-wt/pool-14`, branch `feat/p9-b2-20261003`, base `1239a5ff`. Resumed from
`handoff-B2.md` (about 45%: ledger and eval libraries coded, nothing built).

Everything the brief and the carry name is built, tested and committed. The C++ compiled clean on the first build
(`/W4 /WX`, clang-cl 18). The T1 tie fixture ties at 1e-12 (gtest `EvalVerb.Tie*`). `nav_summ --engine-stats` prints the
same lines as the Python on synthetic dirs, and its JSON agrees to 1e-12.

The one concern is a degenerate input. A NAV dir that is its own `--reference` differs from the Python beyond 1e-12 (see
Concerns, item 1). Root's identity run should not list the reference among the dirs, or the PM rules on that case.

Commits, oldest first. Identity merge point: `8538eb0d`. The deletion commit comes after the report commit and is the
last commit on the branch (ruling B2-1).

| commit | content |
|---|---|
| `a1b05ef8` | WIP before the stop: the ledger and eval libraries, `pbo_cscv_traced`, `deflated_sharpe_house.hpp`, trial ledger moved down |
| `b0b0fbf2` | handoff |
| `c4e18995` | the `atx-research-eval` verb (`eval_report.cpp`, `eval_cli.*`, `eval_main.cpp`), CMake tails, 5 gtest files, the `research_ledger` fixture |
| `e27d3fda` | `nav_summ.py --engine-stats` adapter and `test_nav_summ_engine.py` |
| `6cee8769` | the record stage writes `ledger-head.json` (cross-lane, E2's file); `backtest_integrity.CHAIN_RULE` |
| `8538eb0d` | test pinning the self-paired-dir case (Concerns, item 1) |
| report commit | this file and `b2-evidence/` |
| LAST: `refactor(b2): delete the Python statistics of record` | ruling B2-1; merge only after root's identity run passes |

## Files changed

Owned by B2:
- `atx-engine/include/atx/engine/research/ledger/{chain,ledger_file,trial_counts,era_pool,trial_ledger}.hpp`
- `atx-engine/src/research/ledger/{chain,json_line,ledger_file,trial_counts,era_pool,trial_ledger}.cpp`, plus `json_line.hpp`
- `atx-engine/include/atx/engine/research/eval/{numpy_rng,house_moments,onc_house,paired,conditional,cscv_report,eval_series,eval_report,eval_cli}.hpp`
- `atx-engine/src/research/eval/*.cpp`, including `eval_main.cpp`
- `atx-engine/include/atx/engine/research/numpy_reduce.hpp`
- `atx-engine/include/atx/engine/eval/deflated_sharpe_house.hpp` (new)
- `atx-impl/src/trial_ledger.{hpp,cpp}`: now a thin re-export of the moved code
- tests: `atx-engine/tests/research/research_{ledger_chain,ledger_counts,eval_tie,eval_rng,eval_conditional}_test.cpp`
- new fixture `atx-engine/tests/fixtures/research_ledger/`: generator, `ledger.jsonl`, `expected.json`, pinning pytest,
  `.gitattributes`
- `atx-impl/tools/{nav_summ,backtest_integrity,dsr_total}.py` and the new `test_nav_summ_engine.py`
- deletion commit: `atx-engine/tests/fixtures/eval_tie/{frozen_statistics.py (new), generate_eval_tie.py}` (T1 minor)

Cross-lane edits (append-only where the file is a CMake list):
- `atx-engine/CMakeLists.txt` (tail): libraries `atx-engine-research-ledger` and `atx-engine-research-eval`, exe
  `atx-research-eval`. The ledger library is **not** EXCLUDE_FROM_ALL, because `atx-impl-core` links it. The eval
  library links `atx::engine` PRIVATE, for `eval/pbo.cpp`.
- `atx-engine/tests/CMakeLists.txt` (tail): test targets `atx-engine-research-ledger-tests` and
  `atx-engine-research-eval-tests`, registered with `gtest_discover_tests` under label `atx_research`. The deferred T1
  registration skips a target that is already registered.
- `atx-impl/CMakeLists.txt` (tail, C2's file): `target_link_libraries(atx-impl-core PUBLIC atx::engine-research-ledger)`.
- `atx-engine/include/atx/engine/eval/pbo.hpp` and `src/eval/pbo.cpp` (no owning lane):
  - adds `PboSplitTrace` and `pbo_cscv_traced`;
  - `pbo_cscv` and `pbo_cscv_traced` share one `run_cscv`, so there is still a single CSCV.
- `scripts/wave_stage_record.py` (E2): the record stage writes the ledger head (carry, OR-5).
- Deletion commit only:
  - `atx-impl/tools/book_diagnostics.py`: G-3c Memmel SE now comes from the engine verb.
  - `scripts/tests/test_no_python_mirror.py` (T1's guard): the B2 allowlist rows are deleted, as the guard requires.
  - Tests: `scripts/tests/test_cycle_scoring.py`, `scripts/tests/test_research_ledger.py`,
    `atx-impl/tools/test_{nav_summ,nav_summ_v8,dsr_total,backtest_integrity}.py`.

New schema literals, for root to add to `classes.json` (ruling SQL2-CLS):
- `atx.research-eval/v1` (the verb's document)
- `atx.ledger-head/v1` (`ledger-head.json`)
- `atx.research-ledger-fixture/v1` (test fixture only)

The rule name `atx-ledger-chain-fold/v1` is not a schema literal.

## Required carry items

- **SQL-7, chain-head function: done.**
  - API in `research/ledger/chain.hpp`: `chain_head(lines)` returns `{head_sha256, lines, rule}`, with rule
    `kChainRule = "atx-ledger-chain-fold/v1"`.
  - It works on any prefix. `prefix_heads(lines)` gives the head after every line, and `verify_chain` is the reader's
    walk.
  - There is one copy of the rule. `ledger_file.cpp` and `eval_report.cpp` (DSR_hand `prefix_chain_head`) call it.
  - Tested by `ResearchLedgerChain.*` (8 tests), including `FixturePrefixHeadsEqualPython`: every prefix head equals
    `backtest_integrity.chain_head` on a fixture with legacy lines, chained lines, a blank line and a NaN token.
- **SQL-9: kept.**
  - The verb reads and verifies the ledger and never writes it.
  - Ledger bytes and the append format are unchanged. The only C++ writer is the moved IC trial ledger, with its code
    unchanged.
  - No test touches `build-equity/trials.jsonl`.
- **P11: done.** The gtests asserting T1's `eval_tie` fixture through the verb are `EvalVerb.Tie{MomentsAndPsr,
  DsrCrossLoAndHouse, OncAndEffectiveNDsr, Pbo, Paired, NormPpf}`. Every value is compared under its tie rule: integers,
  strings and flags exact, floats within 1e-12 relative.
- **T1 minor (keep the eval_tie checker runnable): done in the deletion commit.**
  - `fixtures/eval_tie/frozen_statistics.py` is the verbatim text of the retired functions. It was extracted by AST, by
    name, and calls the live ledger rules.
  - `generate_eval_tie.py` imports the frozen module. `--check` reports "tied", and `test_eval_tie_fixture.py` passes
    after the deletion.
- **Ruling B2-3: done.** The tie test adds `"schema": "atx.trial-ledger/v1"` to the fixture's ledger records when it
  writes the test JSONL. No fixture byte or expected value changed.
- **K-P9-4a and P14:** B1's factor bytes are not touched. The `backtest_integrity.py` edits are listed above as owned
  B2 scope; E2 also owns `wave_*`.
- **Carried B1 minors: not fixed.** They are all in the admission library and comparator, which are outside B2's files.

## Contracts

K-P9-5: `atx-research-eval --daily [LABEL=]CSV ... [--paired CSV | --paired-of LABEL=CSV] --ledger L --json`. The
document has schema `atx.research-eval/v1`. Its top level holds `psr`, `min_trl`, `dsr`, `paired`, `pbo`, `onc` and
`conditional`; `psr`, `min_trl`, `dsr` and `paired` are the first cell's blocks.

- `dsr` has `house_v1`, `tot`, `cross`, `lo` and `onc`.
  - `house_v1` means N = the construction count (`ledger_n`) and V = the window cell variance (`dsr_variance`). It
    returns the keys of `nav_summ.ledger_dsr`.
  - `tot` returns the keys of `dsr_total.ledger_rows`.
- `paired` has `dsr`, `se`, `p_one`, `p_two` and `memmel_se`, plus every key of `nav_summ.paired_stats`.
- `conditional` holds rows and a cumulative sum, and is printed only.

Each block carries the Python dict keys, so the adapter assigns the blocks directly. NaN prints as null; the adapter
turns the documented keys back into nan.

Ruling B2-2, `paired.se`: `se` is the Python `paired_stats(...)["lw"]["se"]`. That is the delta-method SE of the
population-moment dSR, with a Bartlett HAC (lag = block - 1), annualized: the SE of the Ledoit-Wolf studentized
bootstrap. `p_two` and `p_one` are `lw.p_value` and `lw.p_one_sided`, the test the bundle verdict reads. `memmel_se` is
printed beside them. Flag: K-P9-5's text names no estimator for `se`. This is the Python behaviour, and it matches to
1e-12 (`EvalVerb.TiePaired`).

## Tests written

- gtest `atx-engine-research-ledger-tests` (14):
  - `ResearchLedgerChain.*` (8)
  - `ResearchLedger.{TrialCountsEqualPython, LedgerNEqualsPython, NTotBreakdownEqualsPython,
    WindowVarianceEqualsPython, ConstructionIdsAndPrefixesEqualPython, TrialIdentitiesEqualPython}`, run on the new
    pinned fixture `fixtures/research_ledger`. Its generator calls `backtest_integrity`, `dsr_total` and `era_pool`;
    `test_research_ledger_fixture.py` pins it.
- gtest `atx-engine-research-eval-tests` (16):
  - `EvalVerb.{TieMomentsAndPsr, TieDsrCrossLoAndHouse, TieOncAndEffectiveNDsr, TiePbo, TiePaired, TieNormPpf,
    JsonSchema, HandRowIsThePrefixState, CliWritesTheDocumentAndRefuses, ProtocolDefaultsFollowNavSumm}`
  - `ResearchEvalRng.{CbbStartsAreDefaultRngIntegers, OncDrawsAreTheRecordedPicks,
    ReplayedPicksGiveTheRecordedClustersAndSilhouettes}`. The CBB starts equal `cbb_starts.u16` bit for bit. The 920 ONC
    picks are reproduced exactly by the C++ numpy generator, and replaying them through `KmeansDraws` gives the
    recorded clusters.
  - `Conditional.{ClosedFormNormalCase, UnselectedCellsAreListedNotSummed, RefusesANonPositiveSe}`. The closed-form
    test checks three things: at c = 0 the null bias is se·sqrt(2/π); the conditional median inverts to μ within 1e-9;
    far above c the interval is raw ± z·se.
- pytest:
  - `atx-impl/tools/test_nav_summ_engine.py` (6): printed lines byte-equal, and `--json`, `--pbo-json` and
    `--bundle-json` within 1e-12, with and without the flag. Covered options: v8, reference, `--psr`, `--effective-n
    dirs`, `--pbo`, `--dsr-ledger`, `--dsr-total`, `--dsr-hand`, `--bundle`, a two-era pool, the refusals, and the
    self-pair pin.
  - `atx-engine/tests/fixtures/research_ledger/test_research_ledger_fixture.py` (4).

## Evidence

Builds (`scripts/research-build.ps1`, equity-dev, pool-14; receipts in `b2-evidence/`):
- `p9-b2-a`: `atx-engine-research-ledger-tests`, `atx-engine-research-eval-tests`, `atx-research-eval`. Exit 0, 225 TUs,
  12 links.
- `p9-b2-b`: `atx-impl-core`, `atx-equity-strategy-ic`. Exit 0, 85 TUs. The moved trial ledger links through the
  re-export.

gtests (logs in `b2-evidence/`):
- ledger: 14/14 PASSED
- eval: 16/16 PASSED, 3.9 s

pytest, explicit paths, `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider <files>`. All exit 0.

| state | suites | result |
|---|---|---|
| before the deletion | `test_nav_summ`, `test_nav_summ_pool`, `test_nav_summ_v8`, `test_dsr_total`, `test_backtest_integrity`, `test_nav_summ_engine`, `test_no_python_mirror`, `scripts/tests/test_research_ledger`, both fixture tests | 92 passed, 1 skipped (eval_tie regenerate host skip) |
| before the deletion | `test_research_wave`, `test_wave_scoreboard`, `test_wave_review` (record stage) | 39 passed |
| before the deletion | `test_wave_driver` | 19 passed |
| after the deletion | the first row plus `test_book_diagnostics` and `test_cycle_scoring` | 102 passed, 1 skipped (G-P5: 19 functions in 8 rows left, from 37 in 14) |
| after the deletion | `test_holdout_gate`, `test_trial_ledger_rules`, `test_research_cycle`, `test_research_cycle_roles`, `test_research_spec` | 238 passed, 3 skipped |
| after the deletion | `test_research_wave`, `test_wave_driver`, `test_wave_review`, `test_wave_scoreboard`, `test_mega_report_v8`, `test_mega_report_v8_render`, `test_mega_report_pitch3` | 223 passed |

The adapter's JSON comparison is not vacuous. On the synthetic run, 354 floats were bit-equal and 81 differed within
1e-12. Moments are bit-exact; the bootstrap and LW sums differ in the last bits.

## How root verifies

1. Build `atx-engine-research-ledger-tests`, `atx-engine-research-eval-tests` and `atx-research-eval`. Also build
   `atx-impl-tests`, which links the moved trial ledger.
2. Anchored gtest filters:
   - `atx-engine-research-ledger-tests --gtest_filter=ResearchLedger*`
   - `atx-engine-research-eval-tests --gtest_filter=EvalVerb.*:ResearchEvalRng.*:Conditional.*`
   - `atx-impl-tests --gtest_filter=TrialLedger*` (moved code, unchanged)
   - or `ctest -L atx_research`, which now also lists the two new targets
3. Identity run, on `8538eb0d` (before the deletion commit). Run each command twice: once as written, once with
   `--engine-stats build-equity/bin/atx-research-eval.exe` added.
   - X-5: `python atx-impl/tools/nav_summ.py <X-5 NAV dir> --reference <its base> --protocol v8 --psr --dsr-ledger
     <COPY of trials.jsonl> --dsr-total <same copy> --json a.json`
   - Y-F0: the same command with Y-F0's dir.
   - Expected: stdout byte-equal. `a.json` agrees with the other run to 1e-12 except `nav_summ_run.argv`. You can
     compare with `test_nav_summ_engine.close(json.load(a), json.load(b))` after popping `nav_summ_run`.
   - Never pass `--ledger` (it appends). Give the read-only ledger flags a copy of the ledger.
   - **Do not list the reference dir among the dirs** (Concerns, item 1).
4. After identity passes, merge the deletion commit. From then on, `nav_summ.py` and every consumer need
   `atx-research-eval.exe`: research_cycle's summ step, `book_diagnostics` G-3c, the shim used by the pinned v61..v71
   specs, and the pytest suites listed above. The exe is resolved from `$ATX_EQUITY_BIN`, else `build-equity/bin` of
   the checkout, else `--engine-stats EXE`. Without it these runs fail loudly; they never skip.
5. Flag-absent identity before the deletion: the Python path is untouched. The `test_nav_summ` old-blob tests passed on
   `8538eb0d`.

## Decisions

- The house ONC (MLAM 4.1-4.2, numpy k-means++) is a named method beside the engine's `onc_cluster`. They are different
  algorithms, and the engine's does not reproduce the house clusters. PBO reuses the one engine CSCV through the new
  trace (`pbo_cscv_traced`).
- numpy's `default_rng` (SeedSequence + PCG64) is ported to C++, so the verb draws the CBB starts and the ONC picks
  itself. No draw file is needed.
- The verb's input is a `session_ns,net_return` CSV written by the adapter in Python repr; `from_chars` reads it back
  exactly. Sessions are checked against the seal.
- The adapter `EngineStats` writes CSVs to a private temp dir, removed at exit, and spawns the verb once per statistic
  call. Labels are `c<k>` and `s<k>`, so paths need no escaping.
- `--pbo-max-splits` is refused: there is no engine twin for the seeded subsample.
- Conditional (AKM): the SE basis is `memmel_se`, threshold 0, alpha 0.05. It is printed by the verb (text) and stored
  in the JSON. nav_summ does not print it, so its output stays identical, and no rule reads it.
- Record stage: `ledger-head.json` (`atx.ledger-head/v1`: ledger, lines, head_sha256, rule, n, wave) is written into
  the wave dir and copied with the result into `record.copy_to` (the sprint dir). It is not committed by the stage,
  because the sprint dir is outside the code pathspec.
- Deletion scope = the B2 rows of the G-P5 allowlist, plus their private helpers and `dsr_total.{dsr_at, ledger_rows,
  hand_row}`:
  - from `backtest_integrity`: norm_cdf/ppf, moments, psr, min_trl, deflated_sharpe, psr_report, the CSCV, the ONC and
    k-means, effective_trials, effective_n_dsr, lo_null_dsr;
  - from `nav_summ`: memmel_se, cbb_indices, paired_stats, net_moments, expected_max_sr, deflated_sharpe, dsr_rows,
    ledger_dsr, norm_*.
  - Kept in Python:
    - `nav_summ.sharpe`, used by the year tables, pool rows and book_diagnostics;
    - `align` and `align_many`;
    - the ledger writer, reader and counting rules (SQL-9);
    - `dsr_total.read_ledger`, `trial_breakdown`, `construction_ids`, `prefix_length` and the printing.
- Old-blob identity tests (pre-T41, pre-v6, v7 base, pre-X), after the deletion: printed text stays byte-exact, and
  JSON values are compared at 1e-12 (`p12_equal`, ruling B2-2). The pre-X blob runs on the frozen statistics through a
  composite module (`test_dsr_total.retired_statistics`).
- Unit tests of deleted functions are retired: hand cases of psr, min_trl, DSR and norm helpers, cbb_indices, the
  literal CSCV oracle, the seeded subsample and silhouettes. The engine gtests and the eval_tie tie cover them.
- Retargeted to the engine instead of retired: the paired closed-form, bootstrap and LW tests, moments, ONC block
  recovery and CSCV iid / dominant.

## Concerns

1. **Self-paired dir (outside 1e-12; needs a PM decision).** When a listed NAV dir is also the `--reference`, numpy's
   `corrcoef` (OpenBLAS syrk) can give rho = 1 - 2.2e-16 for identical series. The Python then prints a Memmel SE of
   about 2e-8 and t = +0.00. The engine computes rho = 1 to rounding, giving SE 0 and t null, printed "na". Which way
   the residue falls depends on the data and on BLAS, so it cannot be matched bit for bit. dSR is 0 either way, and no
   decision reads a self-pair.
   - Pinned by `test_nav_summ_engine.test_a_dir_paired_with_itself`.
   - The pre-v6 old-blob test now uses a reference that is not a listed dir.
   - Root's identity argv should not self-pair. If the PM needs self-pair parity, the engine would have to reproduce
     numpy's BLAS rounding, which is not portable.
2. After the deletion commit, nothing statistical runs without the built verb (Verify, item 4).
   - `scripts/research-build.ps1 -Canary` lists only the IC and targets exes. The canary's research_cycle summ step now
     needs `atx-research-eval` in the same bin.
   - **Merge request for root**: add it to `$canaryExes`. B2 did not edit that script.
3. `.superpowers/sdd/mega-alpha-20260926/studies/v6_scorecard.py` (archival) calls deleted `nav_summ` functions. It is
   not updated (outside scope).
4. nav_summ's runtime grows by one verb spawn per statistic call, a few seconds per run. A v8 paired block with 4,999
   draws takes well under a second in C++.

## Ledger candidates

- numpy `default_rng` (PCG64, SeedSequence, Lemire `integers`, `choice`) in C++ reproduces numpy 1.26.4 bit for bit,
  for both the CBB starts and the ONC picks (`ResearchEvalRng.*`).
- `corrcoef` of identical series is not exactly 1 under OpenBLAS (rho = 1 - 2e-16), so self-paired Memmel SE and t are
  rounding residue.
- Keep the PowerShell `*>` build log in mind: `research-build` writes it as UTF-16. Read it with `iconv -f UTF-16LE`.

## Fix round 1 (review `7395965c`; rulings B2-R1..R3, B2-6..B2-9)

Status: DONE. Resumed from `handoff-B2.md` (head `57543f39`) after the owner stop; rulings B2-6..B2-9 answered the
handoff's four questions.

**Identity merge point: `1b25b9a4`.** Every identity-relevant change is at or before it (C++ verb, adapter,
fixtures). Root merges up to here and runs its identity run on this SHA.

**Deletion commit: `598d2199`** (`refactor(b2): delete the Python statistics of record (fix round 1)`), one commit, the
last code commit on the branch (ruling B2-1). Root merges it only after the identity run passes, and moves its
cross-lane hunks onto E2's split (ruling B2-8, list below). The report commit after it touches the sprint dir only.

### Commits of the round, in order

| commit | what |
|---|---|
| `d12dd945` | revert of the first deletion `b76c2219` (no history rewrite) |
| `ec8043b7` | B2-R1 (a)(c) and B2-R2 in C++; edge fixture `research_eval_edges` and `EvalEdges.*`; S4 docs |
| `a12b4c30` | nav_summ adapter: a degenerate pair prints `na` + one line; `engine_exe` / `engine_exe_sha256` in `nav_summ_run` (B2-R3, before the deletion) |
| `b295fa4f`, `f07ba1be`, `c85dc6f2` | S2, S3, S5, S9, S10 and two slips fixed in place (a `-Wrange-loop-construct` error, a string `defect` truthiness) |
| `d5f653cc` | the 5e-4 edge case moved to 5.005e-4, off the printed-rho rounding boundary (see B2-7 below) |
| `dee6cacb` | WIP deletion at the owner stop: **not for merge**, superseded |
| `41746034`, `57543f39` | handoff (sprint dir only) |
| `1b87cb6a` | revert of the WIP `dee6cacb` (the `d12dd945` pattern): B2-9 changes C++, so it goes before the deletion |
| `c25f9b24` | B2-9: house DSR rows take SR* from the bisection quantile; fixture section `house_dsr`; 3 gtests |
| `1b25b9a4` | B2-9 test refinement (lists the non-bit-equal quantile, measures the old Acklam SR*). **Identity merge point** |
| `598d2199` | **the deletion**: `dee6cacb` re-applied and finished (B2-R3 call site, S6, S7, S8, edge generator retarget) |

### Per item

**B2-R1 (a) stable forms** (`ec8043b7`, `atx-engine/src/research/eval/paired.cpp`). 1 - rho = sum((za - zb)^2) /
(2 (T - 1)); Memmel SE from `2 omr` and `(a - b)^2 + 2 a b omr (2 - omr)`; the LW point SE s0 as the Bartlett long-run
variance of the scalar influence series g'(y_k - mean); the bootstrap s* from the squared block sums of that series. rho
itself stays the numpy-order Pearson value. Test: `EvalEdges.PairedTiesWithinTheConditioningBound`.

**B2-R1 (b) tolerance.** 1e-12 / (1 - rho) for paired floats, counts and p-values exact: `EvalEdges.PairedTies...`
(4 cases: 1 - rho 5.005e-4, 5e-5, 5e-6, 1.02e-6) and `test_nav_summ_engine.close` / `tie`. Printed lines were
byte-equal to the Python at all four through nav_summ before the deletion (`test_near_identical_pairs...`, run at
`d5f653cc` and again at `1b25b9a4`).

**B2-R1 (c) degenerate refusal.** `kDegenerateOneMinusRho = 1e-6` on the stable 1 - rho: `memmel_se`, `t`, `lw.se`, the
LW CI and both p-values null, `valid_draws` 0; dSR, rho, d0 and the CBB interval still reported. Verb JSON gains
`one_minus_rho` and `degenerate`; nav_summ prints "Memmel SE na t na" and one line "paired: degenerate pair (1 - rho
... < 1e-06): ...". In nav_summ `--json` the two keys appear only on a degenerate pair. Tests:
`EvalEdges.PairedRefusesADegeneratePair` (0.98e-6, 5e-9, 6e-13, self), `EvalEdges.VerbPrintsADegeneratePairAsRefused`,
`test_near_identical_pairs_follow_the_conditioning_rule[omr_0.98e-6|5e-9|6e-13]`.

**B2-R1 (d)** is root's (rho of X-5 / Y-F0 on the pinned references, logged before the identity run). Merge request below.

**B2-6 (self pair prints "na").** Applied: the self pair is one more degenerate pair. Pinned by
`test_nav_summ_engine.test_a_dir_paired_with_itself` and `EvalEdges.PairedRefusesADegeneratePair[self]`. Consequence
after the deletion: three old-blob identity tests listed the reference among the dirs (a self pair the old blobs printed
as "Memmel SE 0.000"). Their argv now names a reference that is not a listed dir, as the pre-v6 test already did; no
expected value changed: `test_nav_summ.test_existing_fields_and_text_unchanged_against_the_pre_t41_blob`,
`test_dsr_total.test_flag_absent_is_byte_identical_to_the_pre_x_nav_summ` (argv 0),
`test_backtest_integrity.test_legacy_output_byte_identical_to_the_v7_base`.

**B2-7 (rounding boundaries): report note.** A printed value that lands exactly on a rounding boundary can differ by one
last digit while the floats agree within the B2-R1 tolerance; no tolerance can make such a line byte-equal. This is how
the original 5e-4 edge case failed: rho = 0.9995 on the 3-decimal rho print, a 1e-16 difference flipped the digit
(`d5f653cc` moved the case to 5.005e-4). For root's identity comparator (ruling B2-7): list every printed line that
differs, show for each that its underlying `--json` floats agree within 1e-12 (1e-12 / (1 - rho) for a paired block),
and that every decision field (counts, p-values, verdict flags, N) is byte-equal; report any such line on the real run
to the PM, never auto-accept it. No such line occurred on the lane's synthetic runs after the move.

**B2-R2 (constant CSCV candidate)** (`ec8043b7`, `cscv_report.cpp`). A column whose used returns are all equal goes to
the engine CSCV as zeros, so its Sharpe is 0 in every split: the Python's `var > 0` rule (its centred block moments are
exactly 0 for such a column), no new epsilon. Listed in `pbo.engine.constant_candidates`. Test:
`EvalEdges.CscvConstantCandidateHasSharpeZero` (5 generated candidates + a constant 0.001 column, 240 sessions, 8
blocks): PBO 0.1857, logits, winner counts `[57,4,9,0,0,0]` all equal to the Python. The review's 0.171 grid is not
reproducible from the review text (numpy seeds 0..59 searched); the re-reviewer reconciles it (PM note). Not covered:
a candidate constant over only some blocks (both implementations divide by a rounding residue; `cscv_report.hpp`).

**B2-R3 (name and pin the verb).**
- Before the deletion (`a12b4c30`): `EngineStats` hashes the exe when it opens it; `nav_summ_run` in `--json`,
  `--pbo-json` and `--bundle-json` records `engine_exe` and `engine_exe_sha256` whenever a verb ran.
- In the deletion commit:
  - nav_summ: `default_engine_exe()` is `$ATX_EQUITY_BIN/atx-research-eval.exe` whenever ATX_EQUITY_BIN is set, so a
    bin without it fails closed ("no engine statistics executable <that path>"); no fallback to `build-equity/bin`.
    The verb is opened lazily, cached per resolved path (S8).
  - `research_cycle.py` (E2's file, ruling B2-8): `EVAL_EXE = ("eval", "atx-research-eval.exe")`; `effective_exes`
    adds it for a spec with a build key and a summ section, and keeps an explicit `exes.eval`; `summ_step` passes
    `--engine-stats <exes.eval>` right after the script; `lock --exes` pins it like ic / nav, `verify_exes` checks it.
    No `--bind` for the exe (the runner refuses binds over 16 MiB).
  - `cycle_verdict.py` (E2's file): `verb_sha256(cycle)` = the cycle's `exes_sha256` pin of `exes.eval`, else the
    SHA-256 of `exes.eval` now; `scoring_blocks(..., verb)` refuses every `summ.json` row and the `pbo.json` whose
    `nav_summ_run.engine_exe_sha256` is another hash or absent ("... not reused (P9 ruling B2-R3; run the summ
    again)"), a `VerdictError`, so a HARD-STOP [verdict]. A named verb that is missing is refused. A spec that names no
    verb is scored as before.
- Tests: `test_nav_summ_engine.test_a_bin_without_the_verb_fails_closed` (fail-closed and lazy `--ledger-n`),
  `test_every_run_names_its_verb`, `test_cycle_scoring.test_the_summ_runs_the_named_verb_and_the_verdict_refuses_another`
  (lock pin, summ argv, pinned refusal of summ.json and pbo.json, a summ naming no verb, unpinned refusal, missing
  verb), `test_research_cycle.test_build_key_resolves_exe_dir` (the exes dict and the summ argv).
- **Gap, open question for the PM:** no pinned spec of record names the verb. Every `scripts/specs/**` cell spec has
  explicit `exes` {ic, nav} and no build key, so their summ step passes no `--engine-stats`; nav_summ then resolves its
  own verb (ATX_EQUITY_BIN, else the checkout's `build-equity/bin`) and names it in `nav_summ_run`, but `lock --exes`
  does not pin it and the verdict has nothing to compare. Deriving `exes.eval` from the nav exe's directory would cover
  them, but it changes the summ argv (the plan lines) of every pinned spec, the v61 dry-plan golden among them, and the
  `lock --exes` output of E2's tests, so I did not. Options: P9 cell templates add `exes.eval` in `change.set`, or the
  wave driver writes it, or the PM rules the derived default.

**B2-8 (placement).** The `research_cycle` / `cycle_verdict` pin is in the deletion commit as a listed cross-lane edit.
Hunks for root to move onto E2's split (`scripts/cycle/`), then re-run the B2-R3 tests above there:
- `scripts/research_cycle.py`: the module doc entry `exes.eval`; `EVAL_EXE` after `EXE_NAMES`; `effective_exes`
  (docstring and the `names` line); `Cycle.summ_step` (the three lines after `argv = [self.py, ...]`).
- `scripts/cycle_verdict.py`: the module doc sentence on `exes.eval`; `EVAL_KEY`; `verb_sha256`; `check_verb`;
  `scoring_blocks` (the `verb` parameter and its check block); `verdict` (passes `verb_sha256(cycle)`).
- `scripts/tests/test_research_cycle.py`: `test_build_key_resolves_exe_dir` (two exes dicts, one summ argv line).
- `scripts/tests/test_cycle_scoring.py`: the new test above; the C-1 test's DSR oracle written out (the deleted
  `NS.deflated_sharpe` / `expected_max_sr` replaced by `statistics.NormalDist`).
- `scripts/tests/test_research_ledger.py`: `nav_summ_n` (S7).
- `scripts/tests/test_no_python_mirror.py` (T1's guard): the B2 allowlist rows deleted, as the guard requires.

**B2-9 (bisection quantile for the house rows)** (`c25f9b24`, `1b25b9a4`).
- `atx-engine/include/atx/engine/eval/deflated_sharpe_house.hpp`: `house_norm_cdf` (0.5 erfc(-x / sqrt 2), the
  Python's operation order), `house_norm_ppf` (bisection on [-40, 40], <= 200 halvings, stop at hi - lo <= 1e-15
  max(1, |mid|), the midpoint; NaN outside (0, 1)), `house_expected_max_sr`. `house_dsr` takes SR* from it, so every
  house row (house_v1, tot, cross, lo, onc, DSR_hand) does. The engine's shared Acklam + Halley `norm_ppf` is unchanged
  for its other callers (MinTRL at the fixed 1 - alpha keeps it; `EvalVerb.TieMomentsAndPsr` ties it).
- Fixture `research_eval_edges` gains `house_dsr`: N = 2, 3, 4, 5, 7, 10, 13, 41, 100, 233, 1000, 4097, 1e4, 33333,
  1e5, 314159, 1e6; the Python quantiles at 1 - 1/N and 1 - 1/(N e); SR* and DSR at three moment sets (a v8 book on the
  window variance, the same on Lo's variance, a fat-tailed short book: DSR from 0.0017 to 0.96); and `dsr_rows` /
  `lo_null_dsr` on two generated cells at dsr_n = N. Regenerated with the house Python (numpy 1.26.4, the `host` field;
  every earlier value unchanged).
- Tests: `EvalEdges.HouseNormPpfIsThePythonBisectionForAnyN` (33 of 34 quantiles bit-equal; `ppf_lo` at N = 1e4 is
  3.9644158719723457 vs 3.964415871972343, 2 ulps, one ulp of erfc at a midpoint; all within 1e-12; the old Acklam SR*
  misses by 5.5e-12 at N = 1e6, the review's number), `HouseDsrTiesThePythonForAnyN` (51 rows),
  `VerbHouseRowsTieThePythonForAnyN` (the verb's cross and lo rows and moments at every N);
  pytest `test_the_house_dsr_grid_spans_n_2_to_a_million`.

### Suggested

| item | state | where | note |
|---|---|---|---|
| S1 | done as B2-9 | `c25f9b24`, `1b25b9a4` | above |
| S2 | done | `b295fa4f` | a line is chained only when `prev_sha256` is non-null, in `chain_head` / `prefix_heads` / `is_chained_line` and `backtest_integrity.chain_head`; gtest `ANullPrevLineFoldsInEveryRule`, pytest `test_a_null_prev_line_folds_for_chain_head_as_for_the_reader` |
| S3 | done | `b295fa4f`, `c85dc6f2` | the reader refuses a BOM at a line start, a raw CR / U+0085 / U+2028 / U+2029 inside a line, a truthy non-object `defect`; `ResearchLedger.ReaderRefusesWhatThePythonRefuses` |
| S4 | done as docs | `ec8043b7` | `--trial` / `--history` documented as trusted-caller flags in `eval_cli.hpp`, `eval_report.hpp` (K-P9-5's text is the plan's) |
| S5 | done | `b295fa4f` | uncalled `check_era_id`, `check_era_windows`, `segment_starts`, `weighted_mean` dropped |
| S6 | done | deletion commit | `test_nav_summ_engine.py` rewritten: nav_summ's `--json` / `--pbo-json` / `--bundle-json` blocks tied to the retired Python (`frozen_statistics.py`, loaded by path) on the same series: net moments, DSR rows, PSR, paired, effective N, Lo null, ledger DSR, DSR_tot, DSR_hand, PBO, bundle; the edge pairs at 1e-12 / (1 - rho). No test skips without the verb |
| S7 | done | deletion commit | `test_research_ledger.nav_summ_n` reads `deflated_ledger.n` from `nav_summ --dsr-ledger --json` |
| S8 | done | deletion commit | the verb opens on the first statistic; `--ledger-n` alone opens none (the WIP still opened it in `integrity`; fixed) |
| S9 | done | `b295fa4f` | lines over 100 columns |
| S10 | done | `a12b4c30`, `b295fa4f` | `fixture-ledger.jsonl` |

Also in the deletion commit: `generate_research_eval_edges.py` retargeted from `nav_summ` / `backtest_integrity` to
`frozen_statistics.py` (`--check` still "tied", so the frozen copy reproduces every committed value); the
`frozen_statistics.py` docstring names its loaders.

### Concern 1, rewritten (replaces Concerns item 1 above)

Highly correlated pairs, not only the self pair, are ill-conditioned: Memmel and LW amplify the rho error by about
1 / (1 - rho), and numpy's BLAS order cannot be matched bit for bit. The engine now forms the paired quantities from
the difference series (B2-R1 (a)); the Python-vs-engine tolerance for paired floats is 1e-12 / (1 - rho), with printed
lines and decision counts byte-equal while 1 - rho >= 1e-6 (B2-R1 (b)); below 1e-6 the pair is refused as degenerate
(SE / t / p "na"), the self pair included (B2-R1 (c), B2-6). Root measures rho for the X-5 and Y-F0 pairs on the pinned
references before the identity run and logs it (B2-R1 (d)); a real P9 pair under 1e-6 has no paired test and the
prereg names a fallback.

### Merge requests for root

1. Add `atx-research-eval` to `$canaryExes` in `scripts/research-build.ps1`: after the deletion the canary's summ step
   needs the verb in the canary bin (ATX_EQUITY_BIN), and a bin without it now fails closed.
2. Before the identity run: rho of each identity pair (X-5, Y-F0 against their references), logged (B2-R1 (d)); the
   comparator per B2-7 (above). Run it on `1b25b9a4`.
3. Merge the deletion commit `598d2199` only after the identity run passes; move its cross-lane hunks per B2-8 (above)
   and re-run `test_nav_summ_engine.py` and `test_cycle_scoring.py` there.
4. Carried from the review: schema literals `atx.research-eval/v1`, `atx.ledger-head/v1`,
   `atx.research-ledger-fixture/v1` into `classes.json` (SQL2-CLS); build `atx-impl-tests` and run `TrialLedger*`.
5. The `exes.eval` gap (B2-R3 above) is a PM decision before P9 cells rely on the verdict's verb check.

### Evidence of the round

Builds (`scripts/research-build.ps1`, equity-dev, pool-14; receipts `build-equity/mega-p9-b2-*-receipt.json`):

| tag | source | dirty | targets | result |
|---|---|---|---|---|
| `p9-b2-c` | dirty tree | - | verb + 2 test targets | FAILED, `-Wrange-loop-construct` (fixed `f07ba1be`) |
| `p9-b2-c2` | `f07ba1be` | 0 | same | exit 0, 11 TUs |
| `p9-b2-c3` | `c85dc6f2` | 0 | same | exit 0 |
| `p9-b2-d` | `c25f9b24` | 0 | `atx-research-eval`, `atx-engine-research-eval-tests`, `atx-engine-research-ledger-tests` | exit 0, 11 TUs, 5 links, 46 s; verb sha256 `15eebd7d...1b93` |
| `p9-b2-e` | `1b25b9a4` | 0 | `atx-engine-research-eval-tests` | exit 0, 1 TU, 36 s |

The deletion commit changes no C++: the verb of `p9-b2-d` (`15eebd7df9bc4f60d4c6dffdea06187642a1a93d011319e00df677ab35d31b93`)
is the verb of both SHAs.

gtests (`b2-evidence/fix1r4-gtest-*.log`), at `1b25b9a4`:
- `atx-engine-research-eval-tests --gtest_filter=EvalVerb.*:ResearchEvalRng.*:Conditional.*:EvalEdges.*`: 24 of 24
  PASSED (the eval_tie tie still at 1e-12).
- `atx-engine-research-ledger-tests --gtest_filter=ResearchLedger*`: 16 of 16 PASSED.

pytest, `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider --basetemp <scratch>`,
`PYTHONDONTWRITEBYTECODE=1`, `ATX_EQUITY_BIN=<pool-14>/build-equity/bin`, `ATX_ENGINE_STATS_REQUIRED=1`:

| state | suites | result | log |
|---|---|---|---|
| identity point `1b25b9a4` | tools: `test_nav_summ_engine` (engine vs Python), `test_nav_summ`, `_pool`, `_v8`, `test_dsr_total`, `test_backtest_integrity`, `test_book_diagnostics`; the three fixture tests | 109 passed, 1 skipped (the root-only v8 real-data test) | `fix1r4-pytest-identity-tools.log` |
| identity point `1b25b9a4` | scripts: `test_no_python_mirror`, `test_research_ledger`, `test_cycle_scoring` | 17 passed | `fix1r4-pytest-identity-scripts.log` |
| deletion `598d2199` | tools: the above plus `test_holdout_gate`, `test_trial_ledger_rules`, `test_mega_report_v8`, `_v8_render`, `_pitch3` | 297 passed, 1 skipped (the root-only v8 real-data test) | `fix1r4-pytest-deletion-tools.log` |
| deletion `598d2199` | scripts: the above plus `test_research_cycle`, `_roles`, `test_research_spec`, `test_research_wave`, `test_wave_driver`, `test_wave_review`, `test_wave_scoreboard` | 281 passed, 3 skipped (the three host skips of the lane's first report) | `fix1r4-pytest-deletion-scripts.log` |

Slip logged: the first deletion run failed 3 old-blob identity tests on the self pair (B2-6, above); fixed by naming a
non-listed reference; the logs are of the run after the fix.

Flag-absent identity: `nav_summ.py`, `backtest_integrity.py` and `dsr_total.py` are byte-identical between `d5f653cc`
and `1b25b9a4` (`git diff --stat` empty outside the C++ and the edge fixture), so the handoff's flag-absent run (stdout,
stderr, `--json`, `--pbo-json` byte-equal against `1239a5ff`, far and near references) holds for the new identity
point.

### Ledger candidates

- The bisection quantile in C++ (`house_norm_ppf`) reproduces the retired Python's to the bit on 33 of 34 grid points
  and within 2 ulps on the last (one ulp of erfc, UCRT vs CPython, at one midpoint flips one halving).
- The engine's Acklam + Halley `norm_ppf` misses the bisection SR* by 5.5e-12 relative at N = 1e6; a house row of record
  uses the bisection (B2-9).

### B2-10 (every P9 cell names and pins its engine verb)

Ruling B2-10 answers the open `exes.eval` question above. (a) No pinned v8 spec is edited and nothing is derived for
them; a v1 manifest keeps today's behaviour (the verb recorded in `nav_summ_run` only) and its plan lines stay
byte-identical. (b) Root adds `exes.eval` to the lineage parent specs in the W1-I2 (b) re-pin commit. (c) This lane:
a v2 cell must name and pin the verb, and verify refuses it otherwise.

**Commit `8fb17ad6`** (`fix(b2): a v2 wave cell must name and pin its engine verb (ruling B2-10 (c))`), on top of the
deletion commit `598d2199` and the report commit `8cb206c3`. The identity merge point `1b25b9a4` does not move. The
post-identity part root merges after its identity run is now `598d2199` + `8fb17ad6` (the commits between them and
after them touch only the sprint dir).

What and where:
- `scripts/wave_stage_record.py`, new `eval_problem(w, done)`, called by `verify` beside `exe_problem`. For a manifest
  of schema `atx.research-wave/v2` it refuses, fail closed:
  - a locked cell spec without `exes.eval` or without its pin `exes_sha256.eval` (`lock --exes` writes both); the
    message names the missing key(s): "the v2 cell spec <spec> lacks exes.eval and exes_sha256.eval: ...";
  - a cell with no `summ.json` rows;
  - a `summ.json` row or `pbo.json` whose `nav_summ_run.engine_exe_sha256` is another hash or absent; the message names
    both hashes: "... its statistics come from the engine verb <got> ..., not the cell spec's exes_sha256 pin of
    exes.eval <path> <pin> ...".
  The refusal joins verify's other problems: StageError "verify: ...: stop for the PM's ruling". A v1 manifest:
  `eval_problem` is None, so verify, its receipt and its plan line are as before.
- `scripts/cycle_verdict.py`: the verdict's loop over the summ rows and the pbo.json moves into
  `check_summ_verb(cycle_dir, summ, pbo, verb)`, which `scoring_blocks` and `eval_problem` both call (one rule).
- `lock --exes` already pins an `exes.eval` the spec names (deletion commit: `effective_exes` keeps an explicit
  `exes.eval`; a template inherits its parent's after root's re-pin, and its pin goes into `change.set`). Nothing new
  there.
- No C++ change.

Tests, `scripts/tests/test_wave_eval_verb.py` (new). This tree has no v2 manifest (lane E2 adds `wave_kinds.py`), so
each test runs the fixture's wave as v1 on fakes, then calls verify with the manifest's `schema` set to
`atx.research-wave/v2`, the one key `wave_kinds.is_v2` reads:
1. `test_a_v2_cell_without_exes_eval_is_refused_at_verify`: refused naming `exes.eval and exes_sha256.eval`; named but
   unpinned, refused naming `exes_sha256.eval`; named, pinned and the summ on that verb: verify passes.
2. `test_a_v2_cell_whose_summ_ran_on_another_verb_is_refused`: refused naming both hashes; also refused: a summ naming
   no verb, a pbo.json of another verb, no summ rows.
3. `test_a_v1_manifest_plans_and_verifies_as_before`: `eval_problem` is None, the verify plan line equals the deletion
   commit's literally and is in the wave's plan, verify passes with a summ of another verb, and the wave's result is
   judged and recorded. No expected value was edited anywhere.

Evidence (`b2-evidence/fix1-b2-10-pytest-scripts.log`). `"C:/Program Files/Python312/python.exe" -m pytest -q -p
no:cacheprovider --basetemp <scratch>`, `PYTHONDONTWRITEBYTECODE=1`, no ATX_EQUITY_BIN:
- `test_wave_eval_verb`, `test_cycle_scoring`, `test_wave_driver`, `test_wave_review`, `test_wave_review_minor`,
  `test_wave_hardening`, `test_research_wave`, `test_wave_scoreboard`, `test_research_cycle`: 173 passed, 3 skipped (the
  host skips of the lane's first report), 2 deselected.
- `test_no_python_mirror`: 7 passed (separate invocation).
- Deselected: `test_cycle_scoring`'s two tests that run the real nav_summ (`test_verdict_dsr_is_the_ledger_variance...`,
  `test_the_summ_runs_the_named_verb...`). They need the built verb, and this round's build tree was deleted after the
  report (DISK). Both passed at `598d2199` with it. This commit changes their code path only by moving the loop into
  `check_summ_verb`, and `test_wave_eval_verb` exercises that helper on every refusal branch. Root re-runs them with
  its build.
- v1 plan bytes: `research_wave.py plan` of the fixture's v1 manifest under the `598d2199` scripts (a `git archive`
  export) and under `8fb17ad6` give the same 32 lines, byte for byte once the checkout's root path is normalized (the
  plan prints absolute script paths); scratch, deleted.

Cross-lane hunks for root (E2's files, ruling B2-8), lines as at `8fb17ad6`. On E2's split (`05a0fcd1`) both files
keep their paths; `verify` there has the same `problems = [p for p in [exe_problem(exe, pins)] if p]` line:
- `scripts/wave_stage_record.py`:
  - line 12: `import cycle_verdict as CV`;
  - lines 27-28: `V2_SCHEMA = "atx.research-wave/v2"`. On E2's split, drop it and test `SK.kind(w) is None` (or
    `wave_kinds.is_v2(w.manifest)`) in `eval_problem` instead;
  - `verify`, line 47: `eval_problem(w, done)` joins `exe_problem(exe, pins)` in the `problems` list;
  - lines 84-110: `eval_problem`, placed after `exe_problem`.
- `scripts/cycle_verdict.py`: lines 98-106, `check_summ_verb` (after `check_verb`); `scoring_blocks`, line 119, the
  `verb` block calls it.
- New `scripts/tests/test_wave_eval_verb.py` (B2's, on E2's `wave_fixture`). On E2's split, a real v2 manifest of a
  kind can replace the schema flip.
- Optional, E2's text: `V2_EXE_TEXT` (the v2 verify plan line, ruling E2-6) could state this refusal. I did not touch
  it: this tree has no v2 plan line, and it changes v2 plan bytes.

Notes for root:
- A v2 wave whose driver does not lock its cell specs with `--exes` (`driver.lock_exes` false) has no
  `exes_sha256.eval` pin, so every cell is refused at verify (fail closed, as ruled). E2's v2 manifest does not require
  `driver.lock_exes`; the lock comes from the wave config or the template's `change.set`.
- The judge stage's `--bundle` runs nav_summ with its default verb. `bundle.json`'s `nav_summ_run` names that verb but
  is not compared with the pin (outside B2-10 (c)'s text; P10 note).

## Post-identity redo (dispatch R6-2, 2026-10-04; rulings B2-11, B2-12, PRE-15, D2R-2 (b), re-review 1)

**Status of the first round (R6-2): BLOCKED at item 1 (a permission refusal), partial.** Resumed from `handoff-B2.md`
at `4bed5c43` (pool-14, branch `feat/p9-b2-20261003`). No history rewritten: `598d2199` and `8fb17ad6` stay; no rebase.
Code commit of the round: `fe152fca`. **The redo was finished by two follow-ups: see "Follow-up 1" and "Follow-up 2"
at the end of this section** (the merges, B2-11, the G-P5 and PRE-15 re-tests, D2R-2 (b), the builds and the suites).

### What stopped the round

- `git merge e8e424b9` (plain) stopped on 3 conflicted files (list below). Resolving `scripts/research_cycle.py` means
  taking E2's version: E2's split turned the 2,316-line module into a 47-line re-export shim, and this branch's side of
  the file carries the B2-R3 hunks of the deletion commit. The command **`git checkout --theirs
  scripts/research_cycle.py`** was **refused by the permission system** ("Irreversible Local Destruction"). Under
  `resume6-addendum.md` it was not retried and not routed around: every other way to resolve that file (an editor
  rewrite, a revert-then-merge) reaches the same file state.
- I aborted the merge (`git merge --abort`; the tree is back at `4bed5c43`; nothing is lost, both sides are commits).
  The conflict state is in `b2-evidence/redo-merge-conflicts.txt`.
- So items 1-4 and 6, which need the merged tree (E2's split, a real `atx.research-wave/v2` manifest), are **pending**
  for a follow-up round once the owner allows the resolution. Item 5 (PRE-15) and the Suggested items that touch only
  B2's own files were done on the pre-merge branch. They merge without a conflict: `git diff --stat 1b25b9a4 e8e424b9`
  over every file they touch is empty, apart from `atx-engine/tests/CMakeLists.txt`, where root's additions sit above
  B2's block, far from the one line added here.

### The conflicts and the resolution the follow-up applies

| file | conflict | resolution (placing B2's hunks, E2's logic untouched) |
|---|---|---|
| `scripts/research_cycle.py` | 2 hunks: the module docstring (ours 5-190 vs E2's 3 lines), the whole code body (ours 204-2308 vs E2's re-exports) | E2's shim, plus `EVAL_EXE` in its `from cycle.spec import (...)` list. B2's hunks move to the split: the docstring entry `exes.eval` goes to the usage docstring of `scripts/cycle/cli.py`, after `exes_sha256` (`cli.py:51` at root `c8ddf736`); `EVAL_EXE` goes after `EXE_NAMES` (`cycle/spec.py:80`), and the `effective_exes` docstring and `names` line to `cycle/spec.py:415`; the three `summ_step` lines (`--engine-stats <exes.eval>`) go to `cycle/phases.py:733` |
| `scripts/tests/test_no_python_mirror.py` | 1 hunk, the ALLOWLIST head | drop the six B2 rows (G-P5, as the deletion commit does) and keep root's D2-3 comment and its D3 / P10 `retired_by` retargets of the composition rows |
| `scripts/wave_stage_record.py` | 1 hunk, `verify`'s `problems` line | `[exe_problem(exe, pins, SK.rule_cell_kind(w)), eval_problem(w, done)]`; `eval_problem` reduced to the spec check by B2-11 and keyed on `wave_kinds.is_v2(w.manifest)` (`V2_SCHEMA` dropped) |

Auto-merged without a conflict: `scripts/tests/test_research_cycle.py` (B2-R3's exes dicts; to re-run against
`cycle/spec.py`). `scripts/cycle_verdict.py` has no root change since `1b25b9a4`.

### Items

| item | state | change (file:line at `fe152fca`) | test | evidence log (`b2-evidence/`) |
|---|---|---|---|---|
| 1. merge root `e8e424b9` (D2R is now in root: `c8ddf736`) | BLOCKED (permission refusal, above) | none; merge aborted | - | `redo-merge-conflicts.txt` |
| 2. G-P5 deletion on the merged tree; `nav_summ` fails closed without the verb (B2-R3) | pending (needs 1) | the deletion `598d2199` is unchanged on the branch; `nav_summ` fails closed there (`test_a_bin_without_the_verb_fails_closed` passed this round) | `test_nav_summ_engine.py` | `redo-pytest-f-tools-s0.log` |
| 3. B2-11 | pending (needs 1) | design below | - | - |
| 4. re-review 1 R1 / R2 | pending (needs 1): R1 is B2-11's verify part, R2 its judge part | design below | - | - |
| 4. re-review 1 S2 (6 ulps) | done | `research_eval_edges_test.cpp:375-379` comment; report wording here (the fix-round text above says "2 ulps": it is 6 ulps, 6.7e-16 relative, `0x1.fb71fab2eea98p+1` vs `...92p+1`) | gtest log prints it | `redo-gtest-f-eval-whole.log` |
| 4. S3 (101 columns) | done | `research_eval_edges_test.cpp:1-3` | build | `mega-p9-b2-f` |
| 4. S4 (classes.json) | already in root (SQL2-CLS): `atx.research-eval/v1`, `atx.ledger-head/v1`, `atx.research-ledger-fixture/v1`, `atx.research-eval-edges-fixture/v1` are all in `classes.json` at root `c8ddf736` | - | - | - |
| 4. S5 (G-3c names its verb) | done | `book_diagnostics.py:554-562` (`engine_producer`), `:1427` (G-3c result key `engine`) | `test_book_diagnostics.py:567-568` | `redo-pytest-f-tools-s0.log` |
| 4. S6 (no assertion on Acklam) | done | `research_eval_edges_test.cpp` `HouseNormPpfIsThePythonBisectionForAnyN`: `EXPECT_GT(acklam_worst, 1e-12)` removed, the worst value printed (5.509e-12 at N 1e6) | gtest | `redo-gtest-f-eval-whole.log` |
| 4. S7 (the constant wins) | done | fixture section `cscv_wins` (`generate_research_eval_edges.py:15-19, 69, 156-168, 201`; `expected.json` +143 lines, every earlier key unchanged, `--check` "tied"); `research_eval_edges_test.cpp:277-336` (`tied_cscv`, `CscvConstantWinsAndTiesTheZeroColumnByIndex`) | gtest; fixture pytest `test_the_second_cscv_grid_...` | `redo-gtest-f-eval-whole.log`, `redo-pytest-f-tools-s0.log` |
| 4. S1 (refuse at preflight) | left: needs E2's split (`wave_stage_preflight.py`); follow-up | - | - | - |
| 5. PRE-15 | done | `eval_report.cpp:169-201` (`ledger_rows_json(..., hand_row)`: `tot_minus_m` = house DSR at `n_tot - campaign_registry`, same V), `:584` (whole ledger: on), `:605` (`--hand` prefix: off), `:733-737` (text row), `:813` (text "DSR_hand (prefix rule)"); `eval_report.hpp:17-33`; `dsr_total.py:20-25, 107-135`; `nav_summ.py:91-99, 1102-1104` | gtest `EvalVerb.HandRowAtNTotMinusMLeavesOutTheCampaigns`; pytest `test_dsr_total.py` (closed form, campaign-free ledger, frozen `dsr_at` tie, prefix-rule label, line count) | `redo-gtest-f-eval-whole.log`, `redo-pytest-f-tools-s0.log` |
| 6. D2R-2 (b) | pending: D2R is in root (`c8ddf736`), but building on its test needs the merge, which hits the refused resolution; the spec / argv scan was not run | - | - | - |

**PRE-15 in detail.** `dsr.tot.tot_minus_m` = `{n, sr0_daily, sr0_annual, dsr}` at N = N_tot - M. That is every trial
of the whole ledger by the defect rule, + 1 when the book has no construction line yet, without the mining campaigns'
registry counts M, on the same V as `tot` (v8y YP-2, "what dropping the campaigns gives"). The `--hand` prefix rows
(v8x PM7-2) keep their rows, carry no `tot_minus_m`, and are printed "prefix rule". nav_summ `--dsr-total` prints one
more line per dir: `   DSR_hand (N_tot - M=<n>, ruling PRE-15): DSR <x> vs SR0 <y> ann | N_tot - M = <kinds>[ + 1
(...)]; campaign registry <M> left out | same V`. `--json` gains `deflated_total.tot_minus_m`. `--dsr-hand` lines read
`== DSR_hand (prefix rule) <dir>: ...` and `   DSR_hand (prefix rule, N_tot=<n>): ...`.
- gtest (research_ledger fixture: 19 trials by the defect rule, M = 40, V from 5 cells on `ledger-fx-v1`): N_tot - M
  = 20 for a new book and 19 for a ledgered one, i.e. N_tot minus exactly M; the DSR, SR0 daily and annual equal
  `dsr_at(moments, N, V)` bit for bit and exceed DSR_tot; a `--hand` row has no `tot_minus_m`; both text labels.
- pytest: the closed form at N 17 / 18 with M 132 (`statistics.NormalDist` oracle, 1e-9); on the same ledger without
  its campaign line `tot_minus_m` equals that ledger's `tot` exactly; `tot_minus_m` ties v8y's own definition, the frozen
  `dsr_at(net_moments, N_tot - M, V)`, within 1e-12; the prefix rule's label and the added line count (per dir 4 lines,
  was 3).

**B2-11 / R1 / R2: the design the follow-up implements on the merged tree.** None of this is coded yet.
- verify (R1): `eval_problem` keeps only the spec check. A v2 cell's locked spec must name `exes.eval` and carry the
  `exes_sha256.eval` pin (`lock --exes`); otherwise it is refused, naming the missing key(s). The summ / pbo hash part
  leaves verify.
- judge (R2): for a v2 manifest, `wave_steps.bundle_argv` passes `--engine-stats <the cell spec's exes.eval>`, so the
  bundle never runs a default verb. `bundle_once` refuses, fail closed:
  - before the run, a verb on disk whose SHA-256 is not the pin;
  - after the run, and on reuse, a `bundle.json` whose `nav_summ_run.engine_exe_sha256` is not the pin (`CV.check_verb`).

  So "Statistics of record" are never published from another verb. The cell's own summ runs inside the judge's
  research_cycle run, which already passes `--engine-stats exes.eval` and has the verdict refuse another verb
  (`scoring_blocks(verb_sha256(cycle))`). A v1 manifest keeps its plan bytes and behaviour.
- tests on a real `atx.research-wave/v2` manifest in the real stage order (lock, run, verify, judge), as E2's
  `test_wave_kinds.py` builds them: verify passes a named and pinned cell before any judge output exists; verify refuses
  a missing key or pin; the judge refuses a bundle of another verb (fresh and reused) and a verb whose bytes moved. Each
  refusal gets its teeth shown by mutation.

### Suggested left

- S1 (preflight refusal of a v2 wave whose cell spec cannot name and pin `exes.eval`): it sits in E2's
  `wave_stage_preflight.py`, which needs the merged tree. It is not cheap: it needs a rule for what "the template will
  name it" means. Left for the follow-up round or P10.

### State for root

1. **Flag-absent nav_summ identity on B2's identity argv** (`<X-5 | Y-F0 dir> --reference <its base> --protocol v8
   --psr --dsr-ledger <COPY of trials.jsonl> --dsr-total <same copy> --json a.json`; never `--ledger`; the reference
   not among the dirs). Compare this branch's nav_summ (with `--engine-stats` or the default verb: after the deletion it
   is always the verb) against root's identity-run outputs at `1b25b9a4` / `c2416d62`:
   - stdout equal except one added line per listed dir, `   DSR_hand (N_tot - M=..., ruling PRE-15): ...`;
   - `a.json` within 1e-12 except the added key `deflated_total.tot_minus_m` (pop it and `nav_summ_run`);
   - the same argv without `--dsr-total` is byte-equal (stdout, stderr, JSON without `nav_summ_run`).
   - `--dsr-hand` (not in the identity argv) changes two words per line ("prefix rule").
2. **What root re-runs on TRAIN at merge:** that argv on X-5 and Y-F0 with the merged head's verb (EXE-CUR: build tag,
   SHA-256, source commit), compared as in 1. A public cross-check for PRE-15: the trial ledger is unchanged since R0-12
   (133 lines, `27e40f9f`), so Y-F0's `tot_minus_m` must reproduce R0-12's logged DSR_hand .7589 at N 123 (= N_tot 233
   - M 110), which v8y's `dsr_at` gave. A miss is a stop for the PM.
3. **New literals, flags, JSON keys:** no new `atx.<name>/v<n>` literal and no new flag. New JSON keys:
   `dsr.tot.tot_minus_m` (verb document, `atx.research-eval/v1`), `deflated_total.tot_minus_m` (nav_summ `--json`),
   `cscv_wins` (a section of `atx.research-eval-edges-fixture/v1`), G-3c `result.engine` {`engine_exe`,
   `engine_exe_sha256`} (book_diagnostics). CMake: compile definition `ATX_RESEARCH_LEDGER_FIXTURE` added to
   `atx-engine-research-eval-tests` (B2's own block).
4. **Cross-lane edits:** none made this round (blocked). Pending for the follow-up, by file and hunk: the three
   conflicts above; `scripts/cycle/{cli,spec,phases}.py` (B2-R3 placement); `scripts/wave_stage_record.py`
   (`eval_problem`, `verify`, `bundle_once`); `scripts/wave_steps.py` (`bundle_argv`); `scripts/tests/test_research_spec.py`
   (D2R-2 (b), the e27b `--protocol` cases, on D2R's version); a test file for B2-11 on E2's fixtures.

### Evidence (pre-merge branch; logs in `b2-evidence/redo-*`)

Builds (`scripts/research-build.ps1`, pool-14, target-scoped; trees configured fresh by `scripts/atx-build.ps1
configure -Preset <preset>`; receipts `build-equity/mega-p9-b2-{f,g,h}-*`):

| tag | preset | source | dirty | targets | result |
|---|---|---|---|---|---|
| `p9-b2-f` | equity-dev (Debug) | `fe152fca` | 0 | `atx-research-eval`, `atx-engine-research-eval-tests`, `atx-engine-research-ledger-tests` | exit 0, 226 TUs, 12 links, 262 s, first try; verb `4bcfce3e871a324f30f3e1816e897dd9b21b8d7978f51722f03f5f646de58ee2` |
| `p9-b2-g` | equity-dev | `fe152fca` + the teeth mutation | 2 | the verb, the eval tests | exit 0, 1 TU (mutation build, reverted after) |
| `p9-b2-h` | equity-rel (Release) | `1f6d5e4b` (C++ = `fe152fca`) | 0 | as `p9-b2-f` | exit 0, 226 TUs, 493 s; verb `ee89fceb69b313b65f318d60043c56a82dddfb4900edcff78877be833e65b75b` |

`atx-impl-tests` (`TrialLedger*`) was not built: no atx-impl C++ changed since the identity point root merged, and the
tree is pre-merge; the follow-up runs it on the merged tree.

gtests (whole targets):

| target | Debug `p9-b2-f` | Release `p9-b2-h` |
|---|---|---|
| `atx-engine-research-eval-tests` (EvalVerb, ResearchEvalRng, Conditional, EvalEdges) | 26 / 26 passed (24 before + 2 new) | 26 / 26 passed |
| `atx-engine-research-ledger-tests` (ResearchLedgerChain, ResearchLedger) | 16 / 16 passed | 16 / 16 passed |

The B2-9 quantile line prints "33 of 34 bit-equal" in Debug (the 6-ulp `ppf_lo` at N = 1e4) and "34 of 34" in Release:
erfc of the Debug and Release C runtimes differs by one ulp there (the COVR class (i) finding). The Acklam worst is
5.509e-12 at N 1e6 in both.

pytest (`"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider --basetemp <scratch>`,
`PYTHONDONTWRITEBYTECODE=1`, explicit paths, separate invocations; no exit 3221225794 in any run):

| suite | env | result | log |
|---|---|---|---|
| tools + fixtures: `test_nav_summ_engine`, `test_nav_summ`, `_pool`, `_v8`, `test_dsr_total`, `test_backtest_integrity`, `test_book_diagnostics`, `test_trial_ledger_rules`, `test_mega_report_v8`, `_v8_render`, `_pitch3`, the eval_tie / research_ledger / research_eval_edges fixture tests | Debug verb, seed 0 | 274 passed, 1 skipped | `redo-pytest-f-tools-s0.log` |
| the same | Release verb, seed 1 | 274 passed, 1 skipped (`test_nav_summ_v8.py:72`, root-only: ATX_EQUITY_ROOT) | `redo-pytest-h-tools-s1.log` |
| `scripts/tests` | Debug verb as ATX_EQUITY_BIN, seed 0 | 377 passed, 3 skipped, 2 failed (below) | `redo-pytest-f-scripts-s0.log` |
| `scripts/tests` | the same, seed 1 | 377 passed, 3 skipped, 2 failed (the same two) | `redo-pytest-f-scripts-s1.log` |
| `scripts/tests/test_cycle_e2e.py` | without ATX_EQUITY_BIN, seeds 0 and 1 | 14 passed, 1 skipped (host skip) each | `redo-pytest-f-e2e-nobin-s{0,1}.log` |
| `atx-engine/tools/test_stage_chain.py` | seeds 0 and 1 | 9 passed each | `redo-pytest-f-chain-s{0,1}.log` |
| `scripts/tests/test_no_python_mirror.py` | - | 7 passed; G-P5: 19 functions in 8 rows left (the B2 rows were retired by `598d2199`; no row retired this round) | `redo-pytest-f-mirror.log` |

The two `scripts/tests` failures, attributed (neither is in code this round touched; `git diff --stat 4bed5c43
fe152fca -- scripts atx-impl/strategies` is empty):
- `test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry`: E2's former red, "cleared at E2's merge".
  This branch does not hold E2's merge (item 1).
- `test_cycle_e2e.py::test_cycle_e2e_goldens_...`: my run's environment. I set ATX_EQUITY_BIN to pool-14's bin, which
  holds only the eval verb, so the canary ran and stopped on "executable was not found" (the IC exe). Without
  ATX_EQUITY_BIN it is a host skip (row above), as in root's ROOT-1 runs.

The e27b tests pass on this tree, which does not hold D2's fitter flag.

Teeth (`redo-teeth-pre15.log`): `eval_report.cpp:198` mutated to `dsr_at_json(m, n_tot, ...)` (M not left out),
build `p9-b2-g`. Then `EvalVerb.HandRowAtNTotMinusMLeavesOutTheCampaigns` FAILED, and `test_dsr_total.py` failed 3 of 9:
- the closed form;
- the campaign-free ledger;
- the frozen `dsr_at` tie.

The mutation was reverted by hand (the code tree is clean, `git status` shows only sprint-dir docs). No refusal was
added this round, so there are no refusal teeth to show.

Blind and data rules: synthetic and fixture data only. No 2020-2023 statistic and nothing dated 2024 or later was
opened. The trial ledger in pool-2 was not opened. `atx-impl/tools/test_holdout_gate.py` was not run (resume6).

### Follow-up 1 (owner stop, 2026-10-04; ruling B2-14): the merge of root `5c57c1ef`

Merge commit `c37ec671` (plain `git merge feat/platform-p9-20261003`, root `5c57c1ef`: E2's split, D2R, COVR), then the
handoff commit `14b881c9`. Every conflict region was resolved by an Edit of that region (no checkout / restore /
show-redirect, no copy-over, no whole-file rewrite from one side):

| file | region | side kept / how combined | why |
|---|---|---|---|
| `scripts/research_cycle.py` | 1: module docstring (ours: the 186-line usage text; root: E2's 3-line shim text) | root's (E2's) text; ours removed in chunks by Edit | E2 moved the usage text to `scripts/cycle/cli.py`; ours = merge-base text + B2's `exes.eval` entry, re-placed in `cycle/cli.py` after `exes_sha256` |
| `scripts/research_cycle.py` | 2: the module body (ours: the old 2,100-line code; root: E2's re-export imports) | root's re-exports, plus one line `from cycle.spec import EVAL_EXE  # noqa ...` | ours = merge-base code (E2 moved it to `scripts/cycle/**`) + B2-R3's hunks, each re-placed (table "B2-R3's hunks" below); none dropped: `git diff 5c57c1ef c37ec671 -- scripts/research_cycle.py` is that one line |
| `scripts/tests/test_no_python_mirror.py` | ALLOWLIST head | ours (the six B2 statistics rows stay deleted, one comment line) + root's D2-3 comment; root's D3 / P10 rows untouched | G-P5: the deletion commit `598d2199` retired them |
| `scripts/wave_stage_record.py` | `verify`'s `problems` line | combined: `[exe_problem(exe, pins, SK.rule_cell_kind(w)), eval_problem(w, done)]` | root's E2-15 (c) argument and B2-10's check both kept (B2-11 then reduced `eval_problem`) |
| sprint-dir `handoff-B2.md`, `task-B2-report.md` (2 regions) | root held an older copy of the lane's own docs | ours (the lane's newer text) | lane-owned records |

Auto-merged: `scripts/tests/test_research_cycle.py` (B2-R3's exes dicts), `atx-engine/tests/CMakeLists.txt`; root's
deletion of 129 tracked `.mypy_cache` files under the sprint dir's `tools/`.

### Follow-up 2 (dispatch R7-1, 2026-10-05; rulings B2-11, B2-14, PRE-15, D2R-2 (b), re-review 1 R1 / R2)

Resumed from `handoff-B2.md` ("Follow-up at the owner stop") at `14b881c9`, pool-14. No history rewritten (no rebase,
no amend, no reset); `598d2199`, `8fb17ad6`, `c37ec671` stay. Commits of this follow-up, in order:

| commit | what |
|---|---|
| `b1df1829` | merge root `a15ad887` (`git merge feat/platform-p9-20261003`; root had moved one docs commit past `4dc831ac`): NO conflict. It completed on its own, so its message is git's default and carries no trailer (not amended: no history rewrite) |
| `d8f80c7a` | B2-11 (re-review 1 R1 + R2), the test rewrite on a real v2 manifest, the fake-world fix for E2's v2 tests (below) |
| `34838462` | D2R-2 (b): nav_summ `allow_abbrev=False`, the e27b `--protocol` cases |
| `c3201a60` | merge root `ebc918c3` (root's SQL2-CLS commit, `classes.json` one line): NO conflict |
| (the docs commit that adds this section) | report, handoff, evidence logs |

**Conflict tables of this follow-up's merges:** `b1df1829` (root `a15ad887`): none. It brought C3 part A (`327c86b2`:
`atx-impl/src/book/nav_*.cpp` (the 14-TU NAV split), `nav_spec.{hpp,cpp}`, `strategy_nav_replay.cpp` deleted,
`atx-impl/CMakeLists.txt`, `atx-impl/tests/*` incl. `strategy_nav_spec_test.cpp`) and sprint-dir files; `git diff --stat
14b881c9 b1df1829 -- scripts atx-engine atx-impl/tools` is empty. `c3201a60` (root `ebc918c3`): none;
`atx-engine/schemas/research_store/classes.json` +1 / -1 (`atx.nav-spec/v1` registered: RESUME 7's one expected
classes-guard red clears with it).

**B2-R3's hunks (deletion commit `598d2199`, all in the old `research_cycle.py`) and where each sits now:**

| hunk at `598d2199` | now (file:line at this head) | how it got there |
|---|---|---|
| usage docstring entry `exes.eval` (after `exes_sha256`) | `scripts/cycle/cli.py:54-56` | follow-up 1, region 1 (re-placed by Edit) |
| `EVAL_EXE = ("eval", "atx-research-eval.exe")` after `EXE_NAMES` | `scripts/cycle/spec.py:81`; re-exported by the shim, `scripts/research_cycle.py:40` | follow-up 1, region 2 |
| `effective_exes` docstring + `names = dict(EXE_NAMES, **(dict([EVAL_EXE]) if "summ" in spec else {}))` | `scripts/cycle/spec.py:416-428` (`:418-419`, `:424`) | follow-up 1, region 2 |
| `summ_step`: `--engine-stats <exes.eval>` (three lines) + `EVAL_EXE` in the `cycle.spec` import | `scripts/cycle/phases.py:736-738`, import `:21` | follow-up 1, region 2 |
| `cycle_verdict.py`: `EVAL_KEY`, `verb_sha256`, `check_verb`, `check_summ_verb`, `scoring_blocks`, `verdict`'s call | the same file: `scripts/cycle_verdict.py:34, 69, 87, 98, 107, 164` (only `check_summ_verb`'s docstring changed, B2-11) | no conflict |
| `test_research_cycle.py`'s exes dicts | auto-merged, unchanged | no conflict |

#### Items

| item | state | change (file:line at this head) | test | evidence (`b2-evidence/`) |
|---|---|---|---|---|
| 1. merge root (B2-14) | DONE: `b1df1829` (root `a15ad887`) and `c3201a60` (root `ebc918c3`), no conflict; follow-up 1's `c37ec671` table above | - | the suites below | - |
| 2. B2-11 verify part (re-review 1 R1) | DONE | `wave_stage_record.py:103-124` `eval_pin`, `eval_problem` (key + pin only, keyed on `wave_kinds.is_v2`, import `:13`; `V2_SCHEMA` dropped); verify's call `:46` unchanged | `test_wave_eval_verb.py`: `test_a_named_and_pinned_v2_cell_passes_verify_and_the_judge_runs_the_pinned_verb`, `test_verify_refuses_a_v2_cell_without_exes_eval_or_its_pin` | `r7-pytest-*` |
| 2. B2-11 judge part (re-review 1 R2) | DONE | `wave_stage_record.py:127-155` `judge_verb` (key, pin, verb on disk, before anything runs), `pinned_verb`, `verb_refusal`; `:204-226` `bundle_once(..., verb)` (`--engine-stats`; `CV.check_verb` on bundle.json, fresh or reused); `:241-259` `scored_verdict(..., verb)` (`CV.check_summ_verb` on summ.json / pbo.json); `:268` `judge` passes the verb in both branches; `:297-309` `judge_plan`; `wave_steps.py:127-136` `bundle_argv(..., verb)` and the header line `:12-13`; `cycle_verdict.py:99-100` docstring | `test_the_judge_refuses_a_bundle_of_another_verb_fresh_and_reused[None / 4096]`, `test_the_judge_refuses_a_verb_whose_bytes_moved`, `test_the_judge_refuses_a_summ_or_pbo_of_another_verb`, `test_a_v1_manifest_keeps_its_plan_bytes_and_behaviour` | `r7-pytest-*`, `r7-v1-plan-identity.log` |
| 2. tests on a real v2 manifest, stage order | DONE | `scripts/tests/test_wave_eval_verb.py` rewritten (7 tests) on E2's kinds fixture `new-nav-rule.json` via `test_wave_kinds`' `load_fixture` / `rerooted` / `world` / `wave` / `KindsFake` | itself | `r7-pytest-*` |
| 2. teeth by mutation | NOT SHOWN: the mutation Edit was REFUSED by the permission system (below); not retried, no other mutation tried | - | - | `r7-teeth-b2-11.log` |
| 3. G-P5 deletion on the merged tree | see Evidence | (no change) | `test_no_python_mirror.py`; `test_nav_summ_engine.py::test_a_bin_without_the_verb_fails_closed` | `r7-gp5-*.log`, `r7-pytest-tools-*` |
| 3. PRE-15 on the merged tree | see Evidence | (no change) | gtest `EvalVerb.HandRowAtNTotMinusMLeavesOutTheCampaigns`; `test_dsr_total.py` | `r7-gtest-*`, `r7-pytest-tools-*` |
| 4. D2R-2 (b) | DONE | scan first (no hit reaches nav_summ; no stop); `atx-impl/tools/nav_summ.py:1063-1065` `allow_abbrev=False`; `scripts/tests/test_research_spec.py:874-888` (`PROTOCOL_SPELLINGS` / `PROTOCOL_ABBREVIATIONS`), `:921-924` (`e27b_cases`), `:928-957` (the parser asserts) | `test_e27b_reads_what_the_fitter_and_nav_summ_parse`, `test_e27b_refuses_every_spelling_at_plan_run_and_lock`, `..._at_add_alpha` | `r7-d2r2b-scan.{py,log}`, `r7-pytest-*` |
| 5. builds, gtests, suites | see Evidence | - | - | - |

#### B2-11 in detail

- **verify (R1):** `eval_problem` reads nothing the summ writes. A v2 cell whose locked spec lacks `exes.eval` or
  `exes_sha256.eval` is refused, naming the missing key(s): "the v2 cell spec <rel> lacks exes.eval and
  exes_sha256.eval: every P9 cell names the engine verb of its statistics and `lock --exes` pins it (rulings B2-10 (c),
  B2-11)"; verify exits 4, as for its other problems. A v1 manifest: `None`.
- **judge (R2):** `judge_verb` gives (path, pin) = the cell spec's effective `exes.eval` (the path `lock --exes`
  hashed) and its `exes_sha256.eval`. It refuses, exit 3 (EXIT_PIN), before the judge runs anything: a key missing
  since verify, or a verb on disk whose SHA-256 is not the pin ("... nothing ran (ruling B2-11)"). Then:
  - the bundle runs `nav_summ.py --engine-stats <exes.eval> --protocol v8 --bundle ...` (never a default verb);
  - `bundle_once` refuses a `bundle.json` whose `nav_summ_run.engine_exe_sha256` is not the pin, when the bundle has
    just run AND when a later judge would reuse it (`CV.check_verb`, exit 3);
  - `scored_verdict` refuses a summ.json row or pbo.json of another verb (`CV.check_summ_verb`, the verdict's own rule,
    exit 3). The research_cycle run already refuses that in its verdict; this is the judge's own check where it reads
    the verdict.
  So no "Statistics of record" are published from another verb. Both judge branches (serial; side by side under
  `driver.host_budget_mib`) pass the verb.
- **v1:** no key asked for, no `--engine-stats`, no comparison (B2-10 (a)). The fixture's 33-line v1 plan is
  byte-equal under `b1df1829`'s scripts and this head (`r7-v1-plan-identity.log`, SHA-256 `ed9b5d38...`).
- **Change against the R6-2 design:** the verb-on-disk check sits in `judge_verb` (the judge's entry), not in
  `bundle_once`. The cell's summ runs before the bundle, so a moved verb must stop the judge before the summ, not only
  before the bundle (the first test run showed the summ check catching it first). `bundle_once` keeps the bundle.json
  check.
- **Refused mutation (teeth):** the Edit of `scripts/wave_stage_record.py` replacing `    if missing:` with
  `    if missing and False:  # TEETH-M1` was refused by the permission system ("Security Test Removal"). Under
  r6-wave3-common ("if the permission system refuses a mutation edit, do not retry it; say so") and PERM-1 it was not
  retried, and no other mutation (M2-M6, listed in `r7-teeth-b2-11.log`) was tried. A pytest run launched beside the
  refused Edit ran on the unmutated tree (7 passed); it is not mutation evidence. What the tests show without
  mutation: each refusal is reached in the real stage order and its message is matched, and each test has a passing
  control on the pinned verb (so no test can pass by a refusal that always fires).

#### A merge-level red found and fixed (E2's v2 tests x B2-10 (c))

On the merged tree four of E2's tests in `test_wave_kinds.py` failed at verify with B2-10 (c)'s refusal ("lacks
exes.eval and exes_sha256.eval"): `test_the_match_stage_still_writes_its_own_leverage[pm6-6]`, `[pm6-6-engine]`,
`test_w1_i2_a_holds_through_a_ref[template-b...-None]`, `test_build_logs_are_seal_scanned`. The same refusal was already
in `eval_problem` at `14b881c9` (follow-up 1 did not run that file), so this is the merge's semantic conflict: E2's fake
v2 world has cells that name no verb, which a P9 cell may not (B2-10 (c), B2-11). Fixed in the fake world, not in the
rule:
- `scripts/tests/wave_fixture.py:401-415` (appended): `EVAL_VERB`, `pin_eval_verb(root)`: the parent of a v2 world
  names `exes.eval` and pins it (`exes_sha256.eval` of a synthetic verb file), the state root's re-pin gives the P9
  lineage parents (B2-10 (b)). No IC / NAV exe file is written: an IC exe on disk turns on ruling W1-I1's cache-path
  check, which a long pytest root fails (seen on the first try); the fake world has no exes by design.
- `scripts/tests/wave_fixture.py:363-367` (`FakeCycle.add_alpha`): the derived cell spec takes the parent's `exes` and
  `exes_sha256`, as the real add-alpha's `derive_spec` (a deep copy of the parent) does. v1 worlds: the parent's
  `exes` equal `cell_spec`'s and it has no pins, so nothing changes.
- `scripts/tests/test_wave_kinds.py:171` (D3's file, W3-8a): one call line in `world()`, `F.pin_eval_verb(root)`.

After it, `test_wave_eval_verb` + `test_wave_kinds` + `test_wave_driver`: 147 passed.

#### D2R-2 (b)

- Scan first (`r7-d2r2b-scan.py`, log `r7-d2r2b-scan.log`): tracked files minus `atx-db/` and the sprint dir. 367 JSON
  files parsed, 32 `summ.extra` / summ-flag / nav_summ-argv lists checked: 0 abbreviations. 19 text candidates in files
  naming nav_summ, each read by hand: none reaches nav_summ's parser (the engine verb's `--onc`, research_cycle's / the
  fit's `--era`, synthetic `--b` flags of `apply_flags` / `reuse_args`) except the e27b test's own `--protocol` cases.
  Dynamic `f"--{...}"` flags in those files feed the fields builder and add-alpha, not nav_summ. No pinned spec relies on
  an abbreviation: no stop.
- `nav_summ.py`: `argparse.ArgumentParser(..., allow_abbrev=False)`, with a two-line comment.
- e27b, on D2R's `test_research_spec.py`: the exact spellings (two tokens, `=`) read `--protocol v8`; `--proto v8`,
  `--prot=v8` and `--pr v8` are usage errors (exit 2) whose stderr ends `error: unrecognized arguments: <token>` (the
  full message), and the guard still counts each (`RC.argparse_values(...) == ["v8"]`: the safe side), so every
  protocol spelling is still refused at plan / run / lock / add-alpha. `--p` now fails as no flag (no longer as
  ambiguous).

<!-- EVIDENCE-R7 -->

#### R7 round at the owner stop (committed by DIRTY-1 in `426721b2`; superseded by the R8 evidence below)

At `c3201a60` the R7 round built Debug `p9-b2-i` (clean, exit 0, 466 TUs) and ran: gtests eval 26 / 26, ledger 16 / 16,
`atx-impl-tests` `TrialLedger*` 24 / 24; `scripts/tests` seed 0 520 passed / 4 skipped; tools seed 0 274 passed /
1 skipped; stage chain 9 / 9 (seeds 0, 1); mirror guard 7; fail-closed 1 (`r7-*.log`). Release: `p9-b2-j` and
`p9-b2-k` exit 1 (the known stale-PCH trap: `deps/equity-rel/eigen3-src` re-populated by the fresh configure, "has
been modified since the precompiled header"); `p9-b2-l` exit 0 (407 TUs), DirtyEntries 1 = the uncommitted report;
its wall time (77,698 s) spans the owner stop. The stop came before `scripts/tests` seed 1 and the Release suites.

#### R8 round (dispatch R8-1, 2026-10-06; DIRTY-1, EXE-CUR, REV-BASE, W3-EARLY-2)

Resumed at `c3201a60` (pool-14). No history rewritten; no rebase, amend or reset. No code changed this round.

| commit | what |
|---|---|
| `426721b2` | DIRTY-1: `task-B2-report.md` as it stood at the stop (the R7 record up to the marker) + the 11 R7 evidence logs that were untracked at the stop |
| `7832719f` | merge root `1b64ce45` (docs only past `ebc918c3`): NO conflict |
| `1b15330d` | merge root `fd084e78` (root moved during the round: SQL3 merged as `fbd2c045`, then `fd084e78`): NO conflict. Brings `seal_guard.{hpp,cpp}` + its gtest, `classes.json`, `stage_chain.py`, `book_monitor.py`, `research_store_hook.py`, `research_wave.py`, `run_bounded_research.py`, `wave_queue.py` and their tests |
| (the docs commit that adds this section) | this evidence, `handoff-B2.md`, `b2-evidence/r8-*`, receipts `mega-p9-b2-{i..p}-receipt.json` |

**Root SHA merged last: `fd084e78`.** Every suite below ran on `7832719f`; on the final `1b15330d` every suite ran
EXCEPT `scripts/tests` seed 1 (stopped at the owner stop, about 58 %) and the stage chain (not started). Those two are
the round's only pending items (next step in `handoff-B2.md`, "R8 round").

#### Evidence (R8)

Builds (`scripts/research-build.ps1`, pool-14, target-scoped, one at a time on the host, each started only when no
other build ran; targets `atx-research-eval`, `atx-engine-research-eval-tests`, `atx-engine-research-ledger-tests`,
`atx-impl-tests`; receipts copied to `b2-evidence/`):

| tag | preset | source | dirty | result | `atx-research-eval` SHA-256 |
|---|---|---|---|---|---|
| `p9-b2-m` | equity-dev (Debug) | `7832719f` | 0 | exit 0, 37 s, 7 TUs + 6 links (spdlog, below) | `845c8287658bbaf3a00281506c3e2738691f06e27a5485908485978f85eed3ae` |
| `p9-b2-n` | equity-rel (Release) | `7832719f` | 0 | exit 0, 0 TUs, 0 links: every exe byte-equal to `p9-b2-l` | `bef0c0cd5b1d47f266d5b2c2c5f28a60718158bcfd8a99486b47dbe2bcf883d1` |
| `p9-b2-o` | equity-dev (Debug) | `1b15330d` | 0 | exit 0, 33 s, 7 TUs + 6 links (spdlog again) | `b2ee9b269eb847325848bdbe67907ecd987eeeda6ccdf4c26188b8d9c71a7529` |
| `p9-b2-p` | equity-rel (Release) | `1b15330d` | 0 | exit 0, 0 TUs, 0 links: every exe byte-equal to `p9-b2-l` | `bef0c0cd5b1d47f266d5b2c2c5f28a60718158bcfd8a99486b47dbe2bcf883d1` |

- **EXE-CUR:** `git diff c3201a60..7832719f` over C++ / CMake is empty. Root's `fd084e78` changes
  `seal_guard.{hpp,cpp}`: neither `p9-b2-o` nor `p9-b2-p` compiled it, so it is outside these four targets. The exes
  of record are `p9-b2-o` (Debug) and `p9-b2-p` (Release) at `1b15330d`. Configured provenance: Debug `c3201a60`,
  Release `c3201a60-dirty` (configured by R7 while the report was uncommitted; the dirty entry was that sprint-dir
  file, committed unchanged by DIRTY-1). Neither tree was reconfigured: a reconfigure re-populates `deps/equity-rel`
  and trips the stale-PCH trap again (j / k above).
- **Why the Debug builds were not no-ops:** the Debug preset builds `spdlog` in the SHARED
  `C:\atx-cache\deps\spdlog-build`. Other lanes' Debug builds refresh it, so ninja rebuilt the 7 `spdlog` TUs and
  relinked `spdlogd.lib` and the exes; no source of this tree changed. Finding for the PM: a shared Debug deps build
  dir lets one pool's build force relinks (new exe SHA-256s) in another pool.

gtests (whole targets; `atx-impl-tests` with `--gtest_filter=*Ledger*:EvalVerb*`, i.e. `TrialLedger*` (24) plus 5
Ledger-named cases of `ConfigEquityIc`, `ReplayDiagnosticsTest`, `StageEquityUniverse`):

| target | Debug `m` / `o` | Release `n` / `p` | logs |
|---|---|---|---|
| `atx-engine-research-eval-tests` (EvalVerb, ResearchEvalRng, Conditional, EvalEdges) | 26 / 26 passed, both | 26 / 26 passed, both | `r8-gtest-{m,n,o,p}-eval-whole.log` |
| `atx-engine-research-ledger-tests` (ResearchLedgerChain, ResearchLedger) | 16 / 16 passed, both | 16 / 16 passed, both | `r8-gtest-{m,n,o,p}-ledger-whole.log` |
| `atx-impl-tests` `*Ledger*:EvalVerb*` | 29 / 29 passed, both | 29 / 29 passed, both | `r8-gtest-{m,n,o,p}-impl-ledger.log` |

PRE-15's gtest `EvalVerb.HandRowAtNTotMinusMLeavesOutTheCampaigns` passes in all four. The B2-9 quantile line prints
"33 of 34 bit-equal" in Debug and "34 of 34" in Release, as in the R6 round (the one-ulp erfc difference of the two C
runtimes); the Acklam worst is 5.509e-12 at N 1e6 (synthetic quantiles, not a statistic of data).

pytest (`"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider --basetemp <own scratch>`,
`PYTHONDONTWRITEBYTECODE=1`, explicit paths, separate invocations; ATX_EQUITY_BIN unset = the Debug verb
`build-equity/bin` unless stated):

| suite | env | on `7832719f` (Debug `m`, Release `n`) | on `1b15330d` (Debug `o`, Release `p`) |
|---|---|---|---|
| `scripts/tests` whole (incl. `test_wave_eval_verb.py` (B2-11), `test_wave_kinds.py`, `test_research_spec.py` (D2R-2 (b), e27b), `test_cycle_scoring.py` (both verb tests), `test_no_python_mirror.py`) | seed 0 | 520 passed, 4 skipped (`r8-pytest-scripts-m-s0.log`) | 527 passed, 5 skipped (`r8-pytest-scripts-o-s0.log`) |
| the same | seed 1 | 520 passed, 4 skipped (`r8-pytest-scripts-m-s1.log`) | R9, on `52cdcc8c` (= `1b15330d` + root's docs-only `685e238b`), Debug `q`: 527 passed, 5 skipped, 0 failed (`r9-pytest-scripts-q-s1.log`); the R8 run stopped at about 58 % stays in `r8-pytest-scripts-o-s1.log` (marked STOPPED, not evidence) |
| `atx-engine/tools/test_stage_chain.py` | seeds 0 and 1 | 9 passed each (`r8-pytest-chain-s{0,1}.log`) | R9, on `52cdcc8c`, Debug `q`: 10 passed each, 0 failed (`r9-pytest-chain-q-s{0,1}.log`); the tenth case is SQL3's (`d10d9e54`, in root `fd084e78`) |
| tools + fixtures (the R6 list: `test_nav_summ_engine`, `test_nav_summ`, `_pool`, `_v8`, `test_dsr_total` (PRE-15), `test_backtest_integrity`, `test_book_diagnostics`, `test_trial_ledger_rules`, `test_mega_report_v8`, `_v8_render`, `_pitch3`, the eval_tie / research_ledger / research_eval_edges fixture tests) | seed 0, Debug verb | 274 passed, 1 skipped (`r8-pytest-tools-m-s0.log`) | 274 passed, 1 skipped (`r8-pytest-tools-o-s0.log`) |
| the same | seed 1, Release verb (`ATX_EQUITY_BIN=build-equity-rel/bin`) | 274 passed, 1 skipped (`r8-pytest-tools-n-s1.log`) | 274 passed, 1 skipped (`r8-pytest-tools-p-s1.log`) |
| G-P5: `scripts/tests/test_no_python_mirror.py` | Debug | 7 passed (`r8-gp5-mirror.log`) | 7 passed; "16 mirrored rule function(s) in 7 row(s) left", the B2 rows stay retired (`r8-gp5-mirror-o.log`) |
| G-P5: `test_nav_summ_engine.py::test_a_bin_without_the_verb_fails_closed` (runs the built verb, then an empty bin) | Debug | 1 passed (`r8-gp5-failclosed-m.log`) | 1 passed (`r8-gp5-failclosed-o.log`) |

- Skips are environment skips: `test_cycle_e2e.py:439` (ATX_EQUITY_BIN unset; the canary is root's, with a full bin),
  three `test_research_cycle.py` live-root hashes (RESEARCH_CYCLE_LIVE_ROOT), and on `1b15330d` SQL3's new
  `test_research_store_hook.py:381` (`atx-research-store.exe` is not one of B2's targets). The tools skip is
  `test_nav_summ_v8.py:72` (root-only, ATX_EQUITY_ROOT). The +7 passed on `1b15330d` are SQL3's new tests.
- **One void run:** the first `scripts/tests` seed-0 run on `7832719f` (started 18:25 on the `p9-b2-i` verb) was
  stopped by me at about 54 % with no failure printed: `p9-b2-m` relinked the Debug verb under it at 18:43, so the run
  straddled two binaries. Kept as `r8-pytest-scripts-s0-void-relink.log`; re-run whole on `p9-b2-m`. From then on no
  build ran in pool-14 while a Debug-verb suite ran. No child exited 3221225794 (ENV-1) in any run.
- Known-red set: EMPTY at root `fd084e78`; 0 failed in every finished run above (the stopped seed-1 run printed no
  failure up to 58 %), so there is no red to attribute. SQL3's merge
  (record-store hook in `research_wave.py` / `wave_queue.py`) and B2-11's `wave_stage_record.py` checks pass together.
- Teeth of B2-11: the refused mutation Edit stays reported (R7, `r7-teeth-b2-11.log`), not retried (PERM-1); no other
  mutation tried. The reviewer may try its own. PRE-15's teeth stand from the R6 round (`redo-teeth-pre15.log`).

Blind and data rules: synthetic and fixture data only; no 2020-2023 statistic and nothing dated 2024 or later opened;
the trial ledger in pool-2 not opened; `atx-impl/tools/test_holdout_gate.py` not run.

#### R9 completion (dispatch R9-1, 2026-10-07; HOST-3, EXE-CUR, SPDLOG-RELINK, ENV-1, B2-SEQ)

- **Merge:** `52cdcc8c` (parents `e86d8b7c`, `685e238b`): root `685e238b` (prereg round 2b-i), NO conflict; it changes
  only `p9-prereg.md` in the sprint dir (`git diff 1b15330d 52cdcc8c` outside `.superpowers` is empty). **Root SHA
  merged last: `685e238b`.** No code changed this round.
- **Configure:** the Debug object tree was deleted at the R8 stop, so `powershell scripts\atx-build.ps1 configure
  -Preset equity-dev` first (exit 0; `r9-configure-equity-dev.log`). No Release build this round (not asked; the
  Release exe of record stays `p9-b2-p`, and `git diff 1b15330d..52cdcc8c` over C++ / CMake is empty).
- **Build `p9-b2-q`** (`scripts/research-build.ps1`, equity-dev, admitted with 3669 MiB free and no other build on the
  host): receipt `build-equity/mega-p9-b2-q-receipt.json` (copy in `b2-evidence/`): source and configured provenance
  `52cdcc8c`, dirty 0, jobs 4, exit 0, 334 s, 466 TUs, 18 links; targets `atx-research-eval`,
  `atx-engine-research-eval-tests`, `atx-engine-research-ledger-tests`, `atx-impl-tests`. SHA-256 per the receipt:
  verb `atx-research-eval` `80a73375313272011d4fcb7a90caba1b46cab371da899ddfeed560222cc0142d`; eval tests
  `54a5672a3dafbf7542d0f85a60eb0b6fbd7c691b74e4535c397e9304e3f8bd91`; ledger tests
  `0a98717e995378ea701eea51b36f3c3b606d429971aa11334904ba729339d2b1`; `atx-impl-tests`
  `085f1059eeada666573330a9c16dd5d1bccecb094a6250e484416cf1b35fe2f4`. The verb's SHA-256 differs from `p9-b2-o`'s with
  no C++ / CMake source change (fresh configure at a new provenance; the shared `C:\atx-cache\deps\spdlog-build` was
  rebuilt again, 7 TUs). SPDLOG-RELINK: the evidence below is from this receipt; the verb's SHA-256 was re-checked
  after the last suite and is unchanged; no build ran in pool-14 while a suite ran.
- **gtests (Debug `q`):** `atx-engine-research-eval-tests` 26 / 26 (incl. PRE-15's
  `EvalVerb.HandRowAtNTotMinusMLeavesOutTheCampaigns`; "33 of 34 bit-equal" as in every Debug run),
  `atx-engine-research-ledger-tests` 16 / 16, `atx-impl-tests --gtest_filter=*Ledger*:EvalVerb*` 29 / 29
  (`r9-gtest-q-{eval-whole,ledger-whole,impl-ledger}.log`).
- **pytest (Debug `q`; command form of the handoff: explicit interpreter, `-q -rs -p no:cacheprovider`, own
  `--basetemp`, `PYTHONDONTWRITEBYTECODE=1`, ATX_EQUITY_BIN unset, separate invocations):** `scripts/tests` seed 1
  527 passed / 5 skipped; `atx-engine/tools/test_stage_chain.py` seeds 0 and 1, 10 passed each. 0 failed. The five
  skips are the same named environment skips as seed 0 on `1b15330d` (above). No child exited 3221225794 (ENV-1).
- **State:** every named suite is green on the final tree; nothing pending. Re-review 2 may start (B2-SEQ); REV-BASE
  stays `26fe8d50`; `52cdcc8c` is a sixth merge with no conflict (table below).

#### Conflict tables of the merges (B2-14; for re-review 2 under REV-BASE)

| merge | root head merged | conflicts | regions and resolution |
|---|---|---|---|
| `c37ec671` (parents `294612c3`, `5c57c1ef`) | `5c57c1ef` (E2's split, D2R, COVR) | 4 files, 6 regions | the table in "Follow-up 1" above: `research_cycle.py` (2 regions: root's shim text and re-exports kept, + one `EVAL_EXE` re-export line; B2-R3's hunks re-placed in `cycle/{cli,spec,phases}.py`), `test_no_python_mirror.py` (ALLOWLIST head: B2 rows stay retired + root's D2-3 comment and D3 / P10 rows), `wave_stage_record.py` (verify's `problems` line: both calls kept), sprint-dir `handoff-B2.md` / `task-B2-report.md` (2 regions: the lane's newer text). Each by an Edit of the region; no checkout / restore / show-redirect / copy-over |
| `b1df1829` (parents `14b881c9`, `a15ad887`) | `a15ad887` (C3 part A) | none | - (git's default message, no trailer; not amended) |
| `c3201a60` (parents `34838462`, `ebc918c3`) | `ebc918c3` (SQL2-CLS `atx.nav-spec/v1`) | none | - |
| `7832719f` (parents `426721b2`, `1b64ce45`) | `1b64ce45` (docs only) | none | - |
| `1b15330d` (parents `7832719f`, `fd084e78`) | `fd084e78` (SQL3 merged) | none | - |
| `52cdcc8c` (parents `e86d8b7c`, `685e238b`) | `685e238b` (prereg round 2b-i, docs only) | none | - (R9) |

REV-BASE for re-review 2: BASE = the SHA re-review 1 names as reviewed, `26fe8d50`.

#### What root re-proves at merge

Merge `git merge <B2 head>` into root. Against root `fd084e78` the lane differs in 35 code / test files
(`git diff --stat fd084e78 <B2 head> -- . ':(exclude).superpowers'`) and in its own `task-B2-report.md`,
`handoff-B2.md` and `b2-evidence/`. Expected conflicts against `fd084e78`: none (it is already merged here). Against
lanes that merge into root first:
- `scripts/tests/test_wave_kinds.py:171` (D3's file, W3-8a): B2 added ONE call line in `world()`,
  `F.pin_eval_verb(root)`. If D3 rewrote `world()`, keep D3's version and re-add that one line (without it, four of
  E2's v2 tests are refused at verify by B2-10 (c) / B2-11; see "A merge-level red found and fixed").
- `scripts/tests/wave_fixture.py:363-367, 401-415` (`FakeCycle.add_alpha` copies the parent's `exes` / `exes_sha256`;
  `EVAL_VERB`, `pin_eval_verb` appended).
- `atx-engine/src/research/eval/eval_report.{cpp,hpp}`: B2's PRE-15 hunks (`eval_report.cpp:169-201, :584, :605,
  :733-737, :813`; `eval_report.hpp:17-33`, at `fe152fca`). Ruling D3W-EVAL assumed this file is not in B2's list; it
  is (PRE-15). D3W's eval-verb append and these hunks are keep-both.
- `scripts/cycle/spec.py` (B2-R3's `EVAL_EXE`, `effective_exes`): C3b's D2R-2 (a) text edit sits in the same file.

Then root re-proves, on the merged head, with exes rebuilt or shown current (EXE-CUR):
1. Builds: `atx-research-eval`, `atx-engine-research-eval-tests`, `atx-engine-research-ledger-tests`, `atx-impl-tests`
   (Debug and Release). gtests: the two research targets whole; `atx-impl-tests --gtest_filter=TrialLedger*`.
2. ROOT-1: `scripts/tests` under one seed and the stage chain, separate invocations; plus the lane's named files:
   `scripts/tests/test_wave_eval_verb.py`, `test_wave_kinds.py`, `test_cycle_scoring.py` (its two verb tests need the
   built verb), `test_research_spec.py` (e27b), `test_no_python_mirror.py`, and the tools list above with the verb.
3. **TRAIN identity procedure (flag-absent nav_summ; B2-1 / B2-7, re-stated for this head).** On X-5 and Y-F0, each
   with the merged head's verb (build tag, SHA-256 and source commit stated):
   `python atx-impl/tools/nav_summ.py <X-5 | Y-F0 dir> --reference <its base> --protocol v8 --psr
   --dsr-ledger <COPY of trials.jsonl> --dsr-total <same copy> --json a.json`. Never `--ledger` (it appends); the
   read-only ledger flags get a COPY of the ledger; the reference is not among the dirs. Compare against root's
   identity-run outputs at `1b25b9a4` / `c2416d62`:
   - stdout equal except ONE added line per listed dir, `   DSR_hand (N_tot - M=..., ruling PRE-15): ...`;
   - `a.json` within 1e-12 except the added key `deflated_total.tot_minus_m` (pop it and `nav_summ_run`);
   - the same argv without `--dsr-total`: stdout, stderr and the JSON without `nav_summ_run` byte-equal;
   - `--dsr-hand` (not in the identity argv) changes two words per line ("prefix rule");
   - substitution list: `nav_summ_run` (exe path / SHA-256 / argv) is the only expected difference besides the PRE-15
     line and key. D2R-2 (b) (`allow_abbrev=False`) changes nothing on this argv (every flag spelled in full);
   - the PRE-15 public cross-check is "State for root" item 2 above (a miss is a stop for the PM).
4. B2-11 at the gate: a P9 v2 wave's cell specs must name `exes.eval` and carry `exes_sha256.eval` (`lock --exes`),
   or verify refuses them; the judge runs nav_summ `--engine-stats <exes.eval>` and refuses a verb whose bytes or
   `nav_summ_run.engine_exe_sha256` differ from the pin. The W1-I2 (b) re-pin (B2-10 (b), GATE-1) adds `exes.eval` to
   the lineage parents. v1 manifests: plan bytes unchanged (`r7-v1-plan-identity.log`).
5. New literals / flags / keys: unchanged from "State for root" item 3 (no new `atx.<name>/v<n>` literal, no new flag;
   JSON keys `dsr.tot.tot_minus_m`, `deflated_total.tot_minus_m`, `cscv_wins`, G-3c `result.engine`). The canary's
   `$canaryExes` already lists `atx-research-eval` (`scripts/research-build.ps1:73`).

Disk: after this commit the pool-14 object trees are deleted by name (`build-equity/` objects, `build-equity-rel/`);
the `mega-p9-b2-*` receipts and logs stay in `build-equity/`, copies of i..p in `b2-evidence/`.

Permission refusal this round (PERM-1; not retried, not routed around): a Write of a NEW scratch file
`scratchpad/b2-r8/report-r8.md` (a draft of this section) was refused ("Subagents should return findings as text, not
write report files"). This section was then written by one Edit of this existing lane record, which the dispatch
requires.

