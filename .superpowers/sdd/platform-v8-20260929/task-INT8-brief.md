# Integration 8 brief (root, pool-2; after the v8 freeze gate; one integrator)

Binding: `integrator-rules.md`; `.agents/cpp/agent.md` sections 8 and 10 (clang-cl 18, `/W4 /permissive- /WX` = `-Wall
-Wextra` with warnings as errors; no `-Wshadow`/`-Wconversion` under clang-cl). Build only through `powershell -File
scripts/research-build.ps1 -Tag <tag> -Targets "a,b" -Preset equity-dev`; tag prefix from the dispatch (v8-12 holds the
pinned Wave 0 executables, v8-13 the unadopted Release build). Merge by SHA with `git merge --no-ff <sha> -m ...`.
Heads read 2026-10-02 by a read-only planner: v8 `96bdba9d`; C++ code head `807af678` (no `.cpp/.hpp/CMakeLists` changed
on v8 since integration 7, so every C++ baseline below is integration 7's). Root moves while R-8 / R-9 / V8-F land: at
dispatch re-run `git merge-tree --write-tree --name-only HEAD <sha>` for each SHA; the resolutions below stay valid.

## 1. Branches: none is an ancestor of the v8 head (`git merge-base --is-ancestor`, all NOT)

| lane | branch | SHA | merge base | ahead | content |
|---|---|---|---|---|---|
| mining (MINE-MEM, -STAT, -RUN, -JOIN, ENG-SLOT) | `feat/platform-v8-minejoin-20261001` | `1bd448cd` | `20e7bd19` | 22 | C++: `search_driver.{hpp,cpp}`, `strategy_mine*` (+ new `strategy_mine_pinned_file.{hpp,cpp}`), 3 test files (1 new: `alpha_vm_slot_reuse_test.cpp`); Python `research_mine.py`, `mine_overlap_factor.py`, specs v9 |
| FIELDS-V9 / LIB3 | `feat/platform-v8-lib3-20261001` | `834d5a05` | `67f04389` | 5 | Python only: `atx-engine/tools/research_fields_v9.py`, `prepare_research_fields_draft.py` + tests; v9 draft |
| XSIG | `feat/platform-v8-xsig-20261002` | `7467448f` | `3c6ae225` | 2 | report + `xsig_check.py` (sprint dir) |
| XIMP | `feat/platform-v8-ximp2-20261002` | `3db253d5` | `3c6ae225` | 3 | report only (`ximp-20261002` is its ancestor) |
| XCOMB | `feat/platform-v8-xcomb-20261002` | `914f9944` | `3c6ae225` | 3 | C++: 2 engine headers, `strategy_ic_theme_erc.{hpp,cpp}` (new), admission, target/NAV replay; 4 test files (2 new engine tests); Python fitter; 2 X specs |
| XDATA | `feat/platform-v8-xdata-20261002` | `e3654b93` | `3c6ae225` | 7 | Python only: `research_fields_xdata.py`, `research_fields_gold.py`, `prepare_research_fields_xdata.py` + tests |
| XPRE | `feat/platform-v8-xpre-20261002` | `da3bb239` | `3c6ae225` | 4 | Python: `dsr_total.py` (new), `nav_summ.py --dsr-total/--dsr-hand`; `v8x-prereg.md` |

MINE-RUN `6ea76460` and MINE-STAT `c4bd8d09` are ancestors of `1bd448cd` (MINE-JOIN merged them): merge `1bd448cd`
only. Two heads are ahead of what `progress.md` records: XDATA (`d44fa7f6` recorded; `9ffbce6c`, `3c0aee08`,
`e3654b93` are task GOLD under PM7-19) and XCOMB (`914f9944`, delivery not yet recorded). Merge the SHAs the dispatch
names; with `d44fa7f6` for XDATA only `research_fields_gold.py` and its test drop out. No lane touches `atx-db/`.

## 2. Merge order and predicted conflicts

Simulation: `git merge-tree --write-tree` of every SHA against `caefcd6a` and again against `96bdba9d` (git 2.40 takes
commits only, so a result tree cannot be fed forward). Lane-to-lane overlap is one file, `atx-impl/CMakeLists.txt`
(mining and XCOMB): `git merge-tree 1bd448cd 914f9944` is clean (disjoint hunks: mining adds
`strategy_mine_pinned_file.cpp` at line 26, XCOMB adds `strategy_ic_theme_erc.cpp` at line 23 and to the two Debug
`/O2` lists). Six of seven merge clean; XCOMB has one conflicting file (three hunks).

| # | merge | why here | predicted |
|---|---|---|---|
| 1 | XPRE `da3bb239` | Python; P5 of `v8x-prereg.md` (`--dsr-total`) is needed by every X verdict | clean |
| 2 | XSIG `7467448f`, XIMP `3db253d5` | documents; frozen strings for X-2 / X-3 | clean |
| 3 | LIB3 `834d5a05`, XDATA `e3654b93` | Python field builders, opt-in (plain builder unchanged); X-3 needs XDATA's | clean (different files) |
| 4 | to-write Python items 5c, 5d, 5e, 5f | X-2..X-4 need them; no build | - |
| 5 | XCOMB `914f9944`, then 5a, 5b | smaller C++; unblocks X-5 / X-6; build A isolates it | 1 file, 3 hunks |
| 6 | mining `1bd448cd`, then 5g | largest C++, engine header: widest rebuild and the golden; last, so the INT7 revert rule (`git revert -m 1`) cannot disturb XCOMB | clean |

XCOMB conflict, `scripts/tests/test_research_spec.py` (ours = v8, theirs = XCOMB); take the union in every hunk:
- hunk 1 (NULL_PINS, ~line 69): keep v8's `r3-aim-gain-gm`, `r6-spo-v3-gm`, `lib-v81-gm` and `r9a`/`r9b` pins, add
  XCOMB's `x-theme-erc.json` and `x-inv-vol.json` lines; keep ONE `STORE_FILLS` line (v8's spelling).
- hunk 2 (EXPECTED_CHANGES, ~line 119): keep v8's `r3-...-gm`, `r6-...-gm`, `r9a/r9b` lines and `THETA`; add XCOMB's
  two `x-*` lines.
- hunk 3 (`nav_delta`, ~line 573): close the dict with `"<fill:nav.flags --risk-model-sha256>"],` then
  `"x-inv-vol.json": pn + ["--vol-scale", "inv-vol-v1"]}`; keep v8's `nav_delta["r6-spo-v3-gm.json"]`, the `THETA`
  loop and `spo`; then `assert cn == nav_delta.get(name, pn)` and
  `assert "--capacity-curve" in cn or name not in ("r5-adv-hold.json", "x-inv-vol.json") + spo`.
Auto-merged by git, re-read after merge 6 (checked consistent in the simulated tree): `atx-impl/CMakeLists.txt`,
`backtest_integrity.py` (`MINED_MAX_BUDGET = 10000`), `test_trial_ledger_rules.py`, `test_research_ledger.py`
(10000 everywhere, no stale 1000), `research_cycle.py` (the `mine` dispatch at the top of `main`).

## 3. First-compile review (mining and XCOMB; read in full against the headers at the lane SHAs)

Read in full: mining `strategy_mine_pinned_file.{hpp,cpp}`, `strategy_mine_pool.{hpp,cpp}`, `strategy_mine_promote.cpp`,
`strategy_mine_rule.{hpp,cpp}`, `strategy_mine.cpp` (run_search, mine_memory, run_mine), `strategy_mine_trials.cpp`,
`strategy_mine_detail.hpp`, `search_driver.{hpp,cpp}`, `strategy_mine_test.cpp`, `alpha_vm_slot_reuse_test.cpp`, the
`factory_signal_fitness_test.cpp` hunk; XCOMB every C++ file and test addition. Result: 0 BLOCKER, 0 LIKELY.

| # | sev | file:line | problem | fix |
|---|---|---|---|---|
| F1 | RUNTIME | XCOMB `strategy_ic_runner_test.cpp:3657` `erc_doc` vs item 5a | `erc_doc` writes `theme_standardise` without `provenance.rule`; after R6C-7 the `good` doc of `CompositionV8.ThemeErcRefusals...` and both ThemeErc runs are refused | add `{"provenance",{{"rule",rule}}}` in `erc_doc` when 5a lands (or skip 5a) |
| F2 | RUNTIME | XCOMB `strategy_target_replay.hpp:165-169` (ConstructionDay +5 fields) | +40 B per decision day and book in the NAV workspace charge (`strategy_nav_replay.cpp:425`, `:2197`); flag-absent outputs unchanged, but a run sized at the edge of `--max-bytes` now refuses | none needed; an identity refusal is a budget refusal, report it |
| F3 | hazard | `atx-core/include/atx/core/error.hpp:155-159` | `ATX_TRY` expands to three statements (not do/while): as the body of an unbraced `if`/`for` it breaks; both lanes brace every use (checked) | any compile fix the integrator writes must brace `ATX_TRY` |

Checked and expected to compile (lane-listed risks): `PinnedReadFile` (`<windows.h>` after `NOMINMAX`; the impl PCH
`atx-impl/pch.hpp` has no `windows.h`; `std::min` on `size_t`; `ReadFile` + `OVERLAPPED` on a sync handle);
move-only `MinePool` through `tl::expected`/`ATX_TRY(auto pool, ...)`/`co::Ok(std::move(...))`, `MinePoolMember
member{pin, {}}`, the ternary in `ATX_TRY_VOID` (`strategy_mine_pool.cpp:239`); `rho_check` lambdas (every capture
used; `const span&` vs prvalue `span` ternary); `if constexpr (kPinnedReadDeniesWriters)` (both branches valid, test
`:1048-1078`); `std::array<MinedFactorBand,3>` + `static_assert(...back().top)`; `bands_json(span)` from
`std::array`; `TrialStatus::RungFailed` covered in the only switch (`strategy_mine_trials.cpp:106-115`);
`search_driver.cpp`: `make_full_engine` (`:149`) follows `apply_mask` (`:130`), `<unordered_set>` included (`:14`),
`evaluate_generation` is non-const (`rung_panels_.clear()` legal). XCOMB: `group_erc.hpp`, `inverse_vol.hpp` (Ok/Err
overloads, includes), `StandardiseRule` 4-row table and `verify_theme_erc` declared and defined in the same unnamed
namespace (`strategy_ic_admission.cpp:500`, `:599`), `vol_scaled_desired` signatures (`eb` alias `:36`, `Ranked`
`:39`), `form_desired_target` (`nan` `:47`, `Construction::state` `:222`), InvVol tests (`CapBench::input() const`,
`st::PriceRiskScratch` via `strategy_price_exposures.hpp`, `NavDecision::sigma`, `TargetNeutralize::None`).
ODR / duplicates: the three new engine tests sit in their group globs AND in impl test targets (separate
executables); suites `GroupErc`, `BookInverseVol`, `ThemeErcV1`, `InvVol`, `AlphaVmSlotReuse` and the test namespaces
are new on v8; no symbol is shared between mining and XCOMB. API drift: XCOMB none (no C++ on v8 since `807af678`, an
ancestor of `3c6ae225`); mining's base `20e7bd19` -> v8 changed `ic_detail::method_recipe` (bool -> string_view),
which no mine file calls; the `ic_detail` helpers it uses (`hex`, `hash_valid`, `field_identifier`, `pinned_json`,
`load_pinned_f64`, `io_chunk`) are unchanged; no `kMinedOverlapFactor` or old `mined_shortlist/rho_select` user remains.

## 4. Build and test sequence

Build A (after merge 5 and items 5a, 5b): `-Targets "atx-equity-strategy-ic,atx-equity-strategy-targets,
atx-impl-strategy-ic-tests,atx-impl-strategy-target-tests,atx-engine-combine-tests,atx-engine-book-tests"` (a
reconfigure follows from the CMakeLists change; the engine groups glob the two new engine tests). Then:
- `atx-impl-strategy-ic-tests --gtest_filter=GroupErc.*:ThemeErcV1.*:CompositionV8.*:ThemeResid*` then whole (145 +
  12 + 5a's tests).
- `atx-impl-strategy-target-tests --gtest_filter=InvVol.*:BookInverseVol.*` then whole (259 + 9); the `[spo-pin]`
  lines must print v1 `0xda6b6871e7e267c5` / `0xaabdbb72f99a6e13`, `[spo-v3-pin]` v2 `0xb039820b40d5cf24` /
  `0xd24b61721a7c698c`.
- `atx-engine-combine-tests` whole (233 + 5), `atx-engine-book-tests` whole (155 + 5).
Build B (after merge 6 and item 5g): `-Targets "atx-equity-strategy-mine,atx-impl-strategy-mine-tests,
atx-engine-factory-tests,atx-engine-alpha-tests,atx-shm-worker"`. Then:
- golden `0x889874a3b9b29c55`, never edited, in BOTH `atx-engine-factory-tests` and `atx-impl-strategy-mine-tests`:
  `--gtest_filter=SignalFitnessDefaults.*:NsgaSearch.ScalarRaw_ReproducesGoldenDigest:SignalFitnessPath.*` -- the
  `ExplicitDefaults...AtEveryWorkerCount` test loops 1 and 4 workers; record both. On a mismatch: INT7 section 3
  (slip fixed in place, else STOP, `git revert -m 1 <merge>`, rebuild, record the first differing registry entry).
- `atx-impl-strategy-mine-tests --gtest_filter=StrategyMineRule.*:StrategyMine.*:StrategyMineCampaign.*` then whole
  (INT7 31 + the lanes'). `StrategyMineCampaign.PromotesThePlantedSignalsOnlyInFiveSeeds` asserts `rung_failed == 0`
  and `failed == 0`: if not, STOP (MINE-MEM report), never edit. Run `SameSeedSameChainHeadAtOneAndFourWorkers` with
  `--gtest_output=xml:build-equity/v8-i8-mine-heads.xml` and log `fixture_registry_head`, `fixture_trials_csv_sha256`
  (they differ from 20e7bd19's by design: MINE-STAT's recipe keys). `MembersStreamByDateAsStored` exercises the
  Windows share mode (write / remove / rename must fail while held).
- `atx-engine-factory-tests` whole (390 + the lanes'); `atx-engine-alpha-tests.exe --gtest_filter=AlphaVmSlotReuse.*`
  run directly (one process builds the fixture once); an abort or a byte difference is an ENGINE finding: stop and
  report, do not patch the test. Then the whole alpha binary once (never built in this sprint's integrations).
Build C (one tag for the record): every research executable and test target together --
`atx-equity-strategy-ic,-targets,-risk,-mine,atx-impl-strategy-mine-tests,atx-engine-factory-tests,
atx-engine-alpha-tests,atx-impl-strategy-ic-tests,atx-impl-strategy-target-tests,atx-impl-tests,
atx-impl-strategy-tests,atx-engine-book-tests,atx-engine-combine-tests,atx-shm-worker`; list the four research exe
digests from the receipt. Then `atx-impl-tests` from the repo root (INT7: 1,028 run / 1,022 / 5 skipped / 1 known
`ConfigJsonNotInDiscoverDigest`; grows by the glob: ThemeErcV1, InvVol, CompositionV8, mining tests) and
`atx-impl-strategy-tests` (46). Counts may only grow by the lanes' tests.
Python (`"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider`):
- XPRE: `atx-impl/tools/test_dsr_total.py test_nav_summ.py test_nav_summ_v8.py test_nav_summ_pool.py
  test_backtest_integrity.py test_trial_ledger_rules.py test_holdout_gate.py`.
- mining: `scripts/tests/test_research_mine.py test_research_ledger.py test_research_spec.py test_research_cycle.py
  atx-impl/tools/test_trial_ledger_rules.py test_mine_overlap_factor.py`; then `research_cycle.py mine plan
  scripts/specs/v9/mine-c1.json` (metadata only; expect UNLOCKED / TO FILL).
- XCOMB: `atx-impl/tools/test_composition_theme_erc.py test_composition_ic_shrink.py test_composition_rules.py
  test_composition_resid.py test_fit_composition_weights.py test_fit_composition_weights_pool.py
  test_fit_composition_weights_store.py`.
- LIB3 / FIELDS-V9: `atx-engine/tools/test_research_fields_v9_nt.py test_research_fields_v9_earn.py
  test_prepare_research_fields_draft.py` (26); XDATA: `test_research_fields_xdata.py test_research_fields_gold.py`.
- XSIG: `python .superpowers/sdd/platform-v8-20260929/xsig_check.py` prints `xsig_check: PASS`.
- whole: `atx-engine/tools`, `atx-impl/tools` (`ATX_EQUITY_BIN`, `ATX_EQUITY_TARGETS_EXE` absolute, final tag),
  `atx-impl/strategies` (+ `ATX_V71_PLAN_JSON` run of `test_generate_library.py`), `scripts/tests` (tiny_world: no
  golden moves; `git status` clean after).
Identities (bounded runner, clean tree, one at a time; a mismatch is a finding -- stop, record the first file):
- INT7's 1a, 1b, 4 (steps 1, 2), 7, 8 with the argv of the `v8-i7-*` receipts, outputs renamed `v8-i8-*` (every exe
  is rebuilt: `ConstructionDay`, `TargetReplayConfig`, `SearchResult` and `DesiredState` changed layout).
- XCOMB theme-erc flag absent: the V8-F book's w pass (its receipt argv, new `--output`): `recipe.json`,
  `train_combined.{f64,json}`, `_member.u8`, `_finite.u8`, `train_planned_targets.csv`, `train_daily_ic.csv`
  byte-identical; its fit argv without `--theme-erc`: `composition_weights.json`, `admission.json` identical except
  `script_sha256` (this also covers items 5b and 5d).
- XCOMB inv-vol flag absent: the V8-F book's nav argv (new `--output`): every `daily_*.csv`, `events_*.csv`,
  `recipe.json`, `summary.json` and the capacity pass's files byte-identical (the R-4 comparison).
- XPRE: `test_flag_absent_is_byte_identical_to_the_pre_x_nav_summ` (4 argv sets) in the suite above; with
  `ATX_EQUITY_ROOT` set, `test_legacy_n37_numbers_reproduced`. LIB3 / XDATA: `git diff --stat <base> HEAD --
  atx-engine/tools` lists only their new files (plain builder untouched: no fields rebuild now). Mining: no flag;
  the golden is its identity.

## 5. To-write items (each its own commit, `fix(<area>): ... (integration 8)`)

- 5a R6C-7 (C++, with build A). `atx-impl/src/strategy_ic_admission.cpp:654` `composition_recorded_rule`: where it now
  returns Ok for a file without a string `provenance.rule`, return Ok only when `!j.contains("theme_standardise")`,
  else `Err(InvalidArgument, "IC runner: composition weights carry a theme_standardise block without a string
  provenance.rule (finding R6C-7)")`; fix the comment at `:646-653`. Tests: in `CompositionV8.RecordedRuleMustWrite...`
  (`strategy_ic_runner_test.cpp:3546`) move `doc("ic-shrink-v1","")` from `admitted` to `cases`; give every
  hand-written doc with a `theme_standardise` block a `provenance.rule` (one edit per doc builder: `themed_text`,
  `shrink_doc`, `pin_weights`, XCOMB's `erc_doc`; 60 block mentions in that file, 1 in `strategy_ic_shrink_test.cpp`;
  the 4 in `strategy_marginal_ic_test.cpp` go through `ic_weights_themes`, untouched). Real files carry the key
  (`fit_composition_weights.py:2152`; the identity device records `ew-theme-v1`). Largest test edit here: cut 1.
- 5b R6C-3 C++ half (tests). `atx-impl/tests/strategy_ic_theme_resid_test.cpp`: new
  `ThemeResid.UnequalTieBlocksBesideSingletonsPinTheBlockMean` porting `UNEQUAL_PLANES/_NAMES/_EXPECTED` from
  `test_composition_resid.py:368` through `st::add_theme_residualised` (W .5/.5; expected to 1e-15; one value per
  block; add order `[0,1,5,7,2,4,6,3]`). The 1e16 summation pin (`:357`) cannot be reached through the kernel (its
  residuals are of ranks): the loop is file-local `mean_over_tie_blocks` (`strategy_ic_theme_resid.cpp:48`). Seam if
  the PM wants it: a header template `tie_block_means(z_at, std::span<f64> e, row)` with the same operations, called
  with `[&](usize k){ return own[offset + s.support[k]]; }`; `NoTieCompositeIsTheRegisteredRuleBitForBit` and
  `EnvelopeAddsOnlyTheRegressionScratch` must stay green. Otherwise log the 1e16 pin as Python-only.
- 5c PM6-9 (Python). `atx-impl/strategies/generate_library.py:481` `exe_plan(..., max_memory_mib: int | None = None)`
  appends `["--max-memory-mib", str(max_memory_mib)]` when set; `scripts/research_add_alpha.py:281` `plan_for` passes
  `RC.option_value(c.spec["ic"]["flags"], "--max-memory-mib")` (2560 on the lo3 specs). Test: in
  `test_research_cycle.py:1912` let `FAKE_IC_PLAN` (`:1749`) record its argv and assert the flag and value.
- 5d PM7-15 (a) (Python). The registry half is done (`7402d7b2`: `registry.json` themes end with `filing_events`; the
  C++ `theme_resid_order`, `strategy_ic_theme_resid.hpp:23-26`, already ends with it). Remaining:
  `fit_composition_weights.py:271` `V7_APPENDED_THEMES = ("ownership_flow", "filing_events")`. Today every
  `--theme-resid` fit refuses (`require_resid_order`, `:428`: registry 11 vs `PRIOR_THEMES` 10). Update the tests
  that assume it absent: `test_composition_resid.py:260-295` (`PRIOR_THEMES + ("filing_events",)` now repeats a
  theme; the "refused before registration" case at `:288`) and `:665-680`.
- 5e PM7-13 roster (JSON). `atx-impl/strategies/alphas/registry.json:16` `house_budget.max_roster` 80
  (`test_generate_library.py` already excludes the cap, PM7-22). Re-probe the IC pass memory before X-3.
- 5f marginal cap (spec). R-7's marginal took 170.3 s of `runner.seconds` 180 (no `runner.phases.marginal`;
  `RUNNER_PHASE_RULES` covers u and w only). The verb reads every library candidate against the pool and the theme
  composites, so time grows with the roster (80 / 57 x 170 s = about 240 s). Write `runner.phases.marginal =
  {"seconds": 360}` into each X wave spec as spec data (precedent OD-2, E-28's `W_3072`) and admit
  `runner.phases.marginal.seconds` in `test_research_spec.py` `EXPECTED_CHANGES` for those templates; read R-7's
  marginal receipt peak MiB (a receipt field, no statistic) and raise `max_rss_mib` if 80/57 x peak > 1,400. The PM
  restates the cap in `v8x-prereg.md` before X-2.
- 5g PM7-13 pool (C++, with build B). `atx-impl/src/strategy_mine_pool.hpp:30` `kMaxMinePoolMembers = 80` (comments
  `:13`, `strategy_mine.hpp:137`); twin `scripts/research_mine.py:72` `MAX_POOL_MEMBERS = 80`; runbook
  `docs/plans/2026-10-01-v9-mine-campaign-runbook.md:80,100`. `StrategyMine.WorkingBytesAreThePeakOfThePhases`
  loops `{1, kMaxMinePoolMembers}` (`strategy_mine_test.cpp:818`) and must keep the 53-member peaks (at M 80, K 16,
  6,100 names, member + rho rows are about 9 MB, far under the promotion-engine term). Identity: the golden.

## 6. Risks and the order to cut scope

Risks: (1) the golden and `rung_failed == 0` rest on MINE-MEM's per-generation engine release and rebuild
(signal-fitness racing path only) and on numpy replicas; (2) `AlphaVmSlotReuse` is unrun: about 1,300 variants x 5
arms x 4 configs in Debug, runtime unknown, an abort is an engine finding; (3) the Windows share-mode behaviour in
`MembersStreamByDateAsStored` is asserted, never observed; (4) XCOMB grows `ConstructionDay` by 40 B per day and book:
a NAV identity sized at the exact edge of `--max-bytes` refuses (a budget refusal, not a mismatch: report it);
(5) a merged but unbuildable C++ lane leaves the tree ahead of the executables on disk: revert that merge
(`git revert -m 1`) before any cell runs; (6) `atx-engine-alpha-tests` and `atx-impl-tests` are the heaviest links:
expect the build script's memory admission to wait.
Cut order if the build does not come clean (each cut logged as an open item, carried to v9 with its ruling):
1. 5a R6C-7 (minor, reachable only by a hand-edited file; the largest test churn).
2. 5b's source seam (keep the tests-only unequal-block case).
3. ENG-SLOT (`alpha_vm_slot_reuse_test.cpp`, tests only): a compile slip is fixed in place; an abort or byte
   difference is reported to the PM, blocks nothing but the mined wave's confidence.
4. The mining merge (revert): blocks only the mined wave X-9 and 5g. X-2..X-6 proceed.
5. The XCOMB merge (revert): blocks only X-5 / X-6.
Never cut merges 1-4 and items 5c-5f: X-2, X-3, X-4 need add-alpha, the fields builders and these, and none needs a
new executable (the v8-12 IC and NAV executables serve them as long as no unbuilt C++ merge stays in the tree).

Report (integration-log section "integration 8"): merges (SHA, merge commit, conflicts), tags and compile fixes (file,
reason), the four research exe digests, golden at 1 and 4 workers, the mine fixture heads, every suite count,
identities (digests), the cuts. Final reply at most 12 lines.
