# Task B2 report: one engine `eval` verb and the ledger library (P9 wave 2, merge slot 4)

## Outcome

**Fix round 1 (end of file) supersedes the SHAs below: identity merge point `1b25b9a4`, deletion commit `598d2199`.**

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
