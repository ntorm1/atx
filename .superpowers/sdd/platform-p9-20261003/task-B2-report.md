# Task B2 report: one engine `eval` verb and the ledger library (P9 wave 2, merge slot 4)

## Outcome

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
