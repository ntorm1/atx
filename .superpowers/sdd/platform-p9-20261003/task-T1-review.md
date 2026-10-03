# Lane T1 review

## Verdict
BLOCK

Spec compliance: ❌. The planted-signal recovery is printed, not asserted. Ruling T1-SE (progress.md) requires a
pre-registered 2-SE band, asserted beside the exe-vs-numpy tie. Everything else complies with brief-T1 (1)-(5) as
amended by P1, P2, P11 and P13, with plan §0.4 G-P4..G-P8, §0.6, §2.2 row T1, and §2.3 K-P9-5.

Counts: blocker 0, major 1, minor 6. The fix round is small: one assertion block in `test_cycle_e2e.py`, plus the
optional minors.

## Reviewed SHA
`3fbc94e0a1e524f40e91849e62694ecf669121fb` (base `d7c1c520`). Commits:
- canary `8bde1f28`
- CTest `57aa5e4b`
- tie `37a01ced`
- guards `019239f6`
- class-C deletion `dee23d2a`
- report `3fbc94e0`

Pool-19 was at HEAD 3fbc94e0 and clean before and after every command (`git status --short` empty).

## Evidence
1. The lane's command 1, re-run in pool-19:
   `PYTHONDONTWRITEBYTECODE=1 PYTHONHASHSEED=0 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
   scripts/tests/test_cycle_e2e.py scripts/tests/test_no_versioned_scripts.py scripts/tests/test_no_python_mirror.py
   atx-engine/tests/fixtures/eval_tie/test_eval_tie_fixture.py` gave exit_code=0:
   ```
   ...........s.........
   G-P5: 37 mirrored rule function(s) in 14 row(s) left for P9 lanes
   .........                                           [100%]
   29 passed, 1 skipped in 4.77s
   ```
   This matches the report (the skip is the live canary without ATX_EQUITY_BIN).
2. The slice-2 check on the deletion head, from the committed saved plan (no exe, no writes):
   `cd atx-impl/strategies && PYTHONDONTWRITEBYTECODE=1 $PY generate_from_spec.py --spec specs/library-v71.json --check`
   gave exit_code=0:
   ```
   fund_industry_ic_v71.json 787c802ed1cdf7cfc507500b014525c3d4dff148f44e77ebd8368c0a686c2259 39317 bytes
   fund_industry_ic_v71.recipe.v2.json 69e95298470d1e4fc38b7b4e6d35809740b5a1479f6c5be1c34a8daac3017107 25389 bytes
   frozen: 19 artefacts verified; plan (alphas/fixtures/v71_plan_k1.json): 48 K1 rows within budget
   ```
3. The planted z, reproduced offline on synthetic seeds with `tiny_world_ic.research_ic` and `planted_rank_ic`
   (scratchpad script, `sys.dont_write_bytecode`), at seed 7:

   | member | mean | SE | z |
   |---|---|---|---|
   | planted_a | 0.02864 | 0.01884 | -0.82 |
   | planted_b | 0.01069 | 0.02998 | -1.14 |
   | noise_c | 0.03543 | 0.01590 | +2.23 (planted 0) |

   The planted values are 0.044070 and 0.044911. Seeds 1-28 show the claimed under-coverage of the lag-42 HAC SE:
   for example, seed 10 has noise_c at z +5.23 and seed 25 has planted_a at z +6.42. The report's numbers hold.
4. The receipt is unchanged without `-Canary`. In PowerShell 5.1, `[pscustomobject][ordered]@{...} | ConvertTo-Json
   -Depth 4` (the base's literal) and `[pscustomobject]$orderedVar | ConvertTo-Json -Depth 4` (the lane's variable cast,
   nested ordered dicts included) are byte-equal: `equal=True`. `Parser::ParseFile(scripts\research-build.ps1)` gave 0
   errors.
5. G-P4 count: the gtest cases in the registered sources number 31 (fields: clock 7, writer 8, volume_mean 5,
   finra_asof 5, fixture 3, cli 3) + 29 (`strategy_mine_test.cpp`) + 15 (`factory_signal_fitness_test.cpp`) = 75,
   which equals the report's "Total Tests 75".

Blindness: one rtk-wrapped grep for JSON key names printed key lines (`calendar_dates`, `inference_defined`,
`required_hac_lag` with date counts) from a `.superpowers/.../orientations.json`. No IC, return, Sharpe or NAV value
was shown or read. Nothing dated 2024-01-01 or later was opened.

## Findings
path:line | severity | problem | required fix

1. `scripts/tests/test_cycle_e2e.py:408-410` (also `record()` :447-456, docstring :23-26,
   `scripts/tests/fixtures/tiny_world_ic.py:201-205`) | **major** |
   - Problem: the planted members' recovery (DS §6 "missing"; brief T1 (1)) is printed and stored, never asserted.
     Ruling T1-SE binds the lane: "asserted with a pre-registered 2-SE band ... an unasserted check pins nothing". At
     3fbc94e0, a canary whose planted signal collapsed would stay green as long as the exe still tied the numpy
     reference.
   - Required fix:
     - Add a module constant `PLANTED_BAND_SE = 2.0`, cited to T1-SE.
     - In `_live_cycle`, assert `abs(z) <= PLANTED_BAND_SE` for planted_a and planted_b, and make `record()` refuse to
       write when it fails.
     - Add an offline test asserting the reference at seed 7 sits inside the band (z -0.82 and -1.14, reproduced
       above), so the band is checked without an exe.
     - Keep noise_c out of the band: it sits at z +2.23 from 0 at seed 7, so extending the band to it would turn
       the canary red.
     - Store `within_two_se` (or `band_se`) in `planted_report`.
     - Update the docstring and report Deviation 1 to cite T1-SE.
2. `scripts/tests/test_no_versioned_scripts.py:47` (as left by `dee23d2a`) | minor |
   - Problem: `FROZEN_SIZE = 30` stays while the deletion commit leaves 11 rows. After the deletion merges, 19 rows can
     be added with no other edit, so the docstring's ":10 the list never grows" and the report's "the list cannot
     grow" are false. A size check also lets a row be swapped for a new one. `test_no_python_mirror.py:78`
     `FROZEN_ROWS` has the same pattern once lanes retire rows.
   - Required fix:
     - In `dee23d2a`, set `FROZEN_SIZE = 11`, or better, assert `set(ALLOWLIST) <= FROZEN_KEYS`, a frozenset (or its
       SHA-256) of the base rows.
     - State that each retiring lane lowers `FROZEN_ROWS` in the mirror guard (or use the same subset check there).
3. `scripts/tests/test_cycle_e2e.py:454-460` | minor |
   - Problem: `--record` (and `research-build.ps1 -CanaryRecord`) silently replaces an already recorded
     `builds[<type>]`. It checks only copy_b, not that planted_a and planted_b were admitted (the live test checks
     both). A CanaryRecord on a later tag can therefore re-pin goldens with no old-to-new trace other than a dirty
     tree. That is a tool-mediated hash edit, which §0.6 forbids outside a ruled re-pin.
   - Required fix: refuse to overwrite a non-null entry unless `--repin` is given, and print the old and new digests.
     Before writing, apply the live test's admission checks (planted admitted) and the finding-1 band.
4. `scripts/specs/v61.json:127`, `v61-ops.json:127`, `v70.json:125`, `v71.json:168` | minor |
   - Problem: `static_check.script` still names the deleted `atx-impl/strategies/check_fund_ic_v6.py`, so replaying
     these historical cycles fails at the check phase. Tests compare plan strings only (`test_research_cycle.py:36,59`)
     and stay green.
   - The accepted lineage is clean. The v8 roots `base-lo1.json` and `base-lo3.json`, every `lib-v8*`, the `x-*` and
     `y-*` templates (X-5 = `x-theme-erc` -> `lib-v8x3b-gm`) and `v8-rerun/*` carry no `static_check` and reference no
     deleted file. Derived templates drop `static_check` anyway (`research_spec.py:53` IDENTITY_SECTIONS).
   - Required fix: one of the following, not a T1 file edit:
     - a PM ruling line ("historical v61/v70/v71 specs are not replayable after the audit §4 deletion; their libraries
       and recipes stay frozen, sha-pinned data -- cost if wrong: a replay needs the file from git history");
     - a later lane drops `static_check` from the four specs.
5. `atx-engine/tests/fixtures/eval_tie/generate_eval_tie.py:57-59`, `test_eval_tie_fixture.py:22` | minor |
   - Problem: the Python-side verifier imports `backtest_integrity`, `nav_summ` and `dsr_total`, which B2 deletes in
     wave 2. The verifier is not in any routine suite (R0-2 runs `scripts/tests`, `atx-engine/tools` and
     `atx-impl/tools`), so it will break unnoticed. The report's open risks do not mention it.
   - Required fix: add an open-risk or handoff line saying B2 retires or converts the verifier in its deletion commit
     (the data files and `expected.json` stay as B2's gtest inputs), and that root runs it explicitly each wave until
     then.
6. `atx-engine/tests/fixtures/eval_tie/generate_eval_tie.py:354` | minor |
   - Problem: bitwise exactness is keyed on the numpy version alone. `np.corrcoef` and `@` go through OpenBLAS, whose
     kernel is chosen per CPU, so the same numpy on another host can differ in the last bit and turn `--check` red.
     Every run today is on one host.
   - Required fix: key exactness on numpy version plus a recorded BLAS/CPU identity (for example
     `np.show_config()`-derived or `platform.processor()`), and use 1e-12 relative otherwise. Or document that exact
     mode is host-bound.
7. `atx-engine/tests/CMakeLists.txt:396-418`, `atx-impl/tests/CMakeLists.txt:156-178` (report "CTest count") | minor |
   - Problem: G-P4 says "every research and strategy gtest target". The lane registers the fields and mine targets
     (and admission when it lands) but does not say why three exes with research sources stay unlabelled:
     `atx-engine-ic-screen-tests`, `atx-impl-ic-screen-tests` and `atx-engine-w1-eval-tests` (eval_pbo, cpcv). Also,
     an unfiltered `ctest` now gains two `<target>_NOT_BUILT` failures on trees where these targets are unbuilt. That
     matches the existing `atx_equity_strategy` precedent but is not stated.
   - Required fix: add one report line listing the excluded exes and why (bounded duplicates whose sources are already
     registered through the owning groups, or through `atx-impl-strategy-ic-tests` for `ic_screen_test` /
     `ic_research_test`), or register them. Note the NOT_BUILT behaviour for unfiltered runs.

## Verified (no finding)
**IC reference = the C++ estimator.** `tiny_world_ic.py` was checked line by line against `ic_screen.cpp`
`rank_values` / `evaluate_row` / `prepare_cache` / `estimate` and `eval/hac.hpp` `mean_inference`:
- label: `close(d+1+h)/close(d+1)-1`;
- rows: `active = (dates - score_begin) - (1 + h)`;
- Spearman: the Pearson correlation of ranks with ties averaged by exact equality (0- vs 1-based ranks does not
  matter);
- mean: the finite sum divided by `valid`;
- lag: `max(2h, floor(4 (n/100)^(2/9)))` on calendar n (= 42);
- HAC: Bartlett `1 - j/(L+1)`, `S/n^2 * n/(n-1)`, on the series centred with NaN set to 0 (re-centred inside, as
  C++), then times `n/valid`, then max with the lag-0 bound.

The C++ paths the reference omits (`min_names`, 80% coverage, segment refusals, the correlation clamp and flat floor)
cannot trigger on tiny_world (64 names, all present). The test asserts `inference_defined` and exact lag and date
counts. The 1e-12 / 1e-9 tolerances are rounding-sized (C++ SIMD reductions after max-abs scaling vs numpy). The
reference is a test oracle under `fixtures/`, not a production mirror.

**Goldens.**
- Schema v2 holds one entry per build type, both null.
- The live test fails on a null entry and prints the record command. `-Canary` sets `ATX_CANARY_REQUIRED=1`, so a
  missing bin fails instead of skipping. A null golden cannot pass on any canary run.
- `test_goldens_belong_to_the_current_world` ties the goldens to `tiny.json`'s fields pin, so a relock forces a reset.
- The v1 digests moved to `superseded_v1` (P13 moved the fields manifest, and `orientations.json` embeds its SHA).
  Nothing reads them, and no expected hash was edited to pass.
- Every base live assertion survives the rewrite.

**Seal (P13).** The block is `{exclusive_end: research_window.current()["SEAL_DATE"]}`, read from the JSON
(2024-01-01), in `prepare_research_fields.py:2592`'s shape. The world refuses a last session at or after the seal. The
other tiny_world users (`atx-impl/tools` tests) carry no bind.

**CTest.**
- The DEFER + `if(TARGET)` + `CTEST_DISCOVERED_TEST_COUNTER` guard is sound in PRE_TEST mode. The deferred call runs
  in the defining directory, after `include(GoogleTest)` in the same scope.
- B1's target is named `atx-engine-research-admission-tests` in `atx-engine/tests` (pool-14 `01f20405`), so the
  engine-side deferred call registers it and the impl fallback skips it.
- `atx_equity_strategy` is untouched. Builds are untouched (targets stay EXCLUDE_FROM_ALL). CMake minimum is 3.20, and
  DEFER needs 3.19.

**Tie fixture (P11).**
- The fixture holds the inputs, today's values, the CBB starts and the ONC pick trace.
- The starts are exactly `cbb_indices`' only draw in `paired_stats` (`nav_summ.py:380-381`).
- The ONC replay consumes every pick.
- There is no gtest, and nothing needs Python at C++ test time.
- The Python statistics are not edited (forbidden list respected).
- The tie rules state a reason per value.

**Guards.**
- No false positives: none on root's tree (pool-2 untracked set holds nothing in scope) and none on wave-1 heads A1
  aa783bb1, A2 2b6f8e6f, B1 01f20405, C1 dd925b7f, E1 a57961bb and S1 17a885a8. None adds a versioned `.py`
  (B1's `screen_v4_v1.json` is out of the rule), and none edits any of the 37 mirrored names.
- The name-only detection and the missing A1/A2 seal-rule row are disclosed open risks. Root adds that row, bumping
  `FROZEN_ROWS`, when A1 and A2 merge.

**Class-C deletion.**
- 11 files, exactly audit §2's class-C rows (§4 "eleven ... 5,069 lines": 837+715+670+560+535+459+321+315+308+204+145
  = 5,069), plus 9 tests.
- The commit holds only those deletions and the 19 guard rows.
- No importer remains (`git grep` outside docs, `.superpowers` and JSON finds docstrings and the v61 dry-run fixture
  strings only).
- The slice-2 check passes on the deletion head (evidence 2).

**`research-build.ps1`.**
- `Preset` is a ValidateSet, so the build-type map is total.
- The canary refuses before the build if a canary exe is not in `-Targets`.
- It runs only after build exit 0, in the root tree.
- The canary's exit code becomes the script's.
- With the flag absent, DryRun and the receipt are unchanged (evidence 4).

## Checked
- [x] `.agents/cpp/agent.md` §10: no C++ in the diff (CMake registration only); no `/W4` surface.
- [x] The diff stays inside §2.2 row T1. Declared cross-lane edits: `scripts/specs/tiny.json` (P13 relock, no
  wave-1 owner) and the comment lines in the two tests lists (P2). `tiny_world_ic.py` and `tiny_world_goldens.json`
  are canary fixtures and have no other owner.
- [x] The report's evidence matches its claims (re-ran pytest command 1 and the slice-2 check; reproduced the seed-7
  z table, the receipt identity and the G-P4 count).
