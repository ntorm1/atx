# Wave review: IC / composition C++ (GATE-1 step 8; RESUME 11, 2026-10-15)

| | |
|---|---|
| Area | IC / composition C++ (`r11-dispatch.md` "WAVE REVIEW" item 4): lanes D3W, D3W-PB, ABS, SIGN, B2's eval verb (and its research ledger library), OD3P's C++ side; plus the wave-2 lanes whose C++ sits in the same files (D2 composition fit, AL-COMB theme-cov-hedge / mom-volman, S2's IC-runner hunks, C2's `stats_ext.hpp` M1c-RED fix) |
| Range | BASE `3c9eeb01` (wave-1 gate head) .. HEAD `d51d011e`, `feat/platform-p9-20261003`, `C:/atx-wt/pool-2`. Code read by SHA only. Pool-2 HEAD at read time `475f5d8f` = `d51d011e` + root M6 docs only; its working tree equals `d51d011e` over `atx-impl/tools`, `atx-engine/tools`, `scripts`, `python`, `atx-impl/strategies` (`git diff --stat d51d011e` empty) |
| Reviewer | fresh Opus agent, read-only in pool-2 (no git write, no build); blind (no 2020-2023 statistic read or written) |
| Verdict | **CHANGES_REQUIRED** for the area: **2 Required (both M, both in the gate's D3-Q1 / EXE-FREEZE procedure, fixable by a PM ruling text and root's gate brief before P9-B0; no lane round)**, **0 code defects found**, **8 Suggested** |
| Files reviewed | **107** files, 31,155 lines at `d51d011e` (table below) |
| Own tests | 260 passed, 0 failed (7 + 180 + 40 + 33), one earlier run void (archive incomplete, below); own mutation 1 / 1 caught, restored |
| Permission refusals | none |

## 1. Files reviewed (107; lines at `d51d011e`; `+/-` over the range; lanes from commit subjects)

Area filter: `git diff --name-only 3c9eeb01..d51d011e` over `atx-engine/src/research/` and
`atx-engine/include/atx/engine/research/` minus `fields*` / `holdings*` / `store*` / `catalog*` (fields, store areas),
`atx-impl/src/strategy_ic*`, `strategy_sign_verb.*`, `strategy_marginal_pair_cache.cpp`, `trial_ledger.*` (B2's moved
ledger), the eval verb's engine kernels (`engine/eval/*.hpp`, `src/eval/pbo.cpp`), the composition kernels AL-COMB /
D2 put in `engine/combine/`, their tests, the IC exe's `main`, the rules-list fixture and the CMake hunks of these
targets (CMake files are shared: only their IC / composition / eval / ledger hunks are judged here). `atx-impl/src/research/`
does not exist; no `atx-impl/tests/` file of this area is claimed by fields / store / NAV.

Depth: **read in full** `fit.hpp`, `fit.cpp`, `composition_cli.cpp`, `strategy_ic_absent.{hpp,cpp}`,
`strategy_sign_verb.{hpp,cpp}`, `numpy_reduce.hpp`, `deflated_sharpe_house.hpp`, `conditional.cpp`, `paired.cpp`,
`eval_series.cpp`, `strategy_ic_theme_hedge.cpp` (and B1's `admission/sign_rule.hpp`, cited); **range diff read**
`strategy_ic_runner.cpp`, `strategy_ic_composition.{cpp,hpp}`, `strategy_ic_rules.cpp` (walk-forward, stages, rule
list), `strategy_ic_mom_volman.cpp` (schedule, sleeve writer), `eval_report.cpp` (`--vol`), the cache source pins, the
CMake hunks, the rules-list fixture; **grep / pin / 100-column / libm scans only** the rest (eval `cscv_report`,
`onc_house`, `numpy_rng`, `house_moments`, the ledger TUs, the gtest files).

| file | lines | +/- | lanes |
|---|---|---|---|
| `atx-engine/CMakeLists.txt` (composition / ledger / eval hunks) | 563 | +200/-0 | A3, A4, AL-SIG, B2, COV, D2, ENGINE, RESEARCH-STORE |
| `atx-engine/include/atx/engine/combine/char_cov_hedge.hpp` | 303 | +303/-0 | AL-COMB |
| `atx-engine/include/atx/engine/combine/group_shrink.hpp` | 152 | +17/-5 | D2 |
| `atx-engine/include/atx/engine/eval/deflated_sharpe_house.hpp` | 129 | +129/-0 | B2 |
| `atx-engine/include/atx/engine/eval/pbo.hpp` | 84 | +19/-0 | B2 |
| `atx-engine/include/atx/engine/eval/stats_ext.hpp` | 283 | +44/-10 | C2, EVAL |
| `atx-engine/include/atx/engine/research/composition/composition_cli.hpp` | 69 | +69/-0 | D2, D3W, D3W-PB |
| `atx-engine/include/atx/engine/research/composition/ew_theme_std.hpp` | 75 | +75/-0 | D2, D3W-PB |
| `atx-engine/include/atx/engine/research/composition/fit.hpp` | 233 | +233/-0 | D2, D3W, D3W-PB |
| `atx-engine/include/atx/engine/research/composition/ic_shrink.hpp` | 59 | +59/-0 | D2 |
| `atx-engine/include/atx/engine/research/composition/mom_volman.hpp` | 20 | +20/-0 | D3W-PB |
| `atx-engine/include/atx/engine/research/composition/theme_erc.hpp` | 59 | +59/-0 | D2 |
| `atx-engine/include/atx/engine/research/composition/theme_resid.hpp` | 38 | +38/-0 | D2 |
| `atx-engine/include/atx/engine/research/composition/theme_tsmom.hpp` | 49 | +49/-0 | D2 |
| `atx-engine/include/atx/engine/research/composition/two_speed.hpp` | 51 | +51/-0 | D2 |
| `atx-engine/include/atx/engine/research/composition/weights_schema.hpp` | 16 | +16/-0 | D2, D3W |
| `atx-engine/include/atx/engine/research/eval/conditional.hpp` | 75 | +75/-0 | B2 |
| `atx-engine/include/atx/engine/research/eval/cscv_report.hpp` | 73 | +73/-0 | B2 |
| `atx-engine/include/atx/engine/research/eval/eval_cli.hpp` | 65 | +65/-0 | B2, D3W |
| `atx-engine/include/atx/engine/research/eval/eval_report.hpp` | 123 | +123/-0 | B2, D3W |
| `atx-engine/include/atx/engine/research/eval/eval_series.hpp` | 49 | +49/-0 | B2 |
| `atx-engine/include/atx/engine/research/eval/house_moments.hpp` | 81 | +81/-0 | B2 |
| `atx-engine/include/atx/engine/research/eval/numpy_rng.hpp` | 60 | +60/-0 | B2 |
| `atx-engine/include/atx/engine/research/eval/onc_house.hpp` | 118 | +118/-0 | B2 |
| `atx-engine/include/atx/engine/research/eval/paired.hpp` | 92 | +92/-0 | B2 |
| `atx-engine/include/atx/engine/research/ledger/chain.hpp` | 78 | +78/-0 | B2 |
| `atx-engine/include/atx/engine/research/ledger/era_pool.hpp` | 46 | +46/-0 | B2 |
| `atx-engine/include/atx/engine/research/ledger/ledger_file.hpp` | 44 | +44/-0 | B2 |
| `atx-engine/include/atx/engine/research/ledger/trial_counts.hpp` | 110 | +110/-0 | B2 |
| `atx-engine/include/atx/engine/research/ledger/trial_ledger.hpp` | 302 | +302/-0 | B2 |
| `atx-engine/include/atx/engine/research/numpy_reduce.hpp` | 113 | +113/-0 | B2 |
| `atx-engine/src/combine/marginal_rank_ic.cpp` | 426 | +6/-17 | D2 |
| `atx-engine/src/eval/pbo.cpp` | 280 | +32/-8 | B2 |
| `atx-engine/src/research/composition/composition_cli.cpp` | 420 | +420/-0 | D2, D3W, D3W-PB |
| `atx-engine/src/research/composition/composition_main.cpp` | 14 | +14/-0 | D2 |
| `atx-engine/src/research/composition/ew_theme_std.cpp` | 74 | +74/-0 | D2 |
| `atx-engine/src/research/composition/fit.cpp` | 467 | +467/-0 | D2, D3W, D3W-PB |
| `atx-engine/src/research/composition/ic_shrink.cpp` | 63 | +63/-0 | D2 |
| `atx-engine/src/research/composition/theme_erc.cpp` | 62 | +62/-0 | D2 |
| `atx-engine/src/research/composition/theme_resid.cpp` | 130 | +130/-0 | D2 |
| `atx-engine/src/research/composition/theme_tsmom.cpp` | 37 | +37/-0 | D2 |
| `atx-engine/src/research/eval/conditional.cpp` | 128 | +128/-0 | B2 |
| `atx-engine/src/research/eval/cscv_report.cpp` | 124 | +124/-0 | B2 |
| `atx-engine/src/research/eval/eval_cli.cpp` | 325 | +325/-0 | B2, D3W |
| `atx-engine/src/research/eval/eval_main.cpp` | 14 | +14/-0 | B2 |
| `atx-engine/src/research/eval/eval_report.cpp` | 852 | +852/-0 | B2, D3W |
| `atx-engine/src/research/eval/eval_series.cpp` | 172 | +172/-0 | B2 |
| `atx-engine/src/research/eval/house_moments.cpp` | 151 | +151/-0 | B2 |
| `atx-engine/src/research/eval/numpy_rng.cpp` | 188 | +188/-0 | B2 |
| `atx-engine/src/research/eval/onc_house.cpp` | 520 | +520/-0 | B2 |
| `atx-engine/src/research/eval/paired.cpp` | 300 | +300/-0 | B2 |
| `atx-engine/src/research/ledger/chain.cpp` | 143 | +143/-0 | B2 |
| `atx-engine/src/research/ledger/era_pool.cpp` | 123 | +123/-0 | B2 |
| `atx-engine/src/research/ledger/json_line.cpp` | 61 | +61/-0 | B2 |
| `atx-engine/src/research/ledger/json_line.hpp` | 25 | +25/-0 | B2 |
| `atx-engine/src/research/ledger/ledger_file.cpp` | 131 | +131/-0 | B2 |
| `atx-engine/src/research/ledger/trial_counts.cpp` | 360 | +360/-0 | B2 |
| `atx-engine/src/research/ledger/trial_ledger.cpp` | 790 | +790/-0 | B2 |
| `atx-engine/tests/CMakeLists.txt` (composition / ledger / eval hunks) | 547 | +108/-0 | A3, A4, AL-SIG, B2, D2, FIELDS, RESEARCH-STORE |
| `atx-engine/tests/combine/combine_char_cov_hedge_test.cpp` | 325 | +325/-0 | AL-COMB |
| `atx-engine/tests/research/research_composition_fit_test.cpp` | 1159 | +1159/-0 | D2, D3W, D3W-PB |
| `atx-engine/tests/research/research_eval_conditional_test.cpp` | 86 | +86/-0 | B2 |
| `atx-engine/tests/research/research_eval_edges_test.cpp` | 494 | +494/-0 | B2 |
| `atx-engine/tests/research/research_eval_rng_test.cpp` | 177 | +177/-0 | B2 |
| `atx-engine/tests/research/research_eval_tie_test.cpp` | 503 | +503/-0 | B2, D3W |
| `atx-engine/tests/research/research_ledger_chain_test.cpp` | 209 | +209/-0 | B2 |
| `atx-engine/tests/research/research_ledger_counts_test.cpp` | 179 | +179/-0 | B2 |
| `atx-impl/CMakeLists.txt` (IC / sign / ledger hunks) | 312 | +123/-34 | AL-COMB, B2, C2, C3, D2, ABS, IMPL, NAV, SIGN |
| `atx-impl/src/strategy_ic_absent.cpp` | 83 | +83/-0 | ABS |
| `atx-impl/src/strategy_ic_absent.hpp` | 56 | +56/-0 | ABS |
| `atx-impl/src/strategy_ic_admission.cpp` | 831 | +212/-6 | D3W, S2 |
| `atx-impl/src/strategy_ic_composition.cpp` | 691 | +160/-13 | AL-COMB, D3W, ABS |
| `atx-impl/src/strategy_ic_composition.hpp` | 224 | +50/-9 | AL-COMB, D3W |
| `atx-impl/src/strategy_ic_detail.hpp` | 402 | +53/-4 | AL-COMB, D3W, ABS, S2 |
| `atx-impl/src/strategy_ic_mom_volman.cpp` | 241 | +241/-0 | AL-COMB, D3W, D3W-PB |
| `atx-impl/src/strategy_ic_mom_volman.hpp` | 57 | +57/-0 | AL-COMB, D3W-PB |
| `atx-impl/src/strategy_ic_result_cache.cpp` | 329 | +29/-5 | S2 |
| `atx-impl/src/strategy_ic_rules.cpp` | 851 | +299/-83 | AL-COMB, D2, D3W, D3W-PB, ABS, S2 |
| `atx-impl/src/strategy_ic_rules.hpp` | 182 | +45/-1 | AL-COMB, D2, D3W |
| `atx-impl/src/strategy_ic_runner.cpp` | 1156 | +270/-33 | AL-COMB, D2, D3W, ABS, S2 |
| `atx-impl/src/strategy_ic_runner.hpp` | 150 | +31/-2 | ABS, S2 |
| `atx-impl/src/strategy_ic_shrink.cpp` | 5 | +4/-58 | D2 |
| `atx-impl/src/strategy_ic_shrink.hpp` | 18 | +14/-45 | D2 |
| `atx-impl/src/strategy_ic_signal_cache.cpp` | 638 | +24/-11 | S2 |
| `atx-impl/src/strategy_ic_theme_erc.cpp` | 5 | +4/-57 | D2 |
| `atx-impl/src/strategy_ic_theme_erc.hpp` | 18 | +14/-47 | D2 |
| `atx-impl/src/strategy_ic_theme_hedge.cpp` | 233 | +233/-0 | AL-COMB |
| `atx-impl/src/strategy_ic_theme_hedge.hpp` | 91 | +91/-0 | AL-COMB |
| `atx-impl/src/strategy_ic_theme_resid.cpp` | 83 | +5/-121 | D2 |
| `atx-impl/src/strategy_ic_theme_resid.hpp` | 14 | +10/-33 | D2 |
| `atx-impl/src/strategy_ic_theme_tsmom.cpp` | 98 | +4/-30 | D2 |
| `atx-impl/src/strategy_ic_theme_tsmom.hpp` | 17 | +13/-42 | D2 |
| `atx-impl/src/strategy_marginal_pair_cache.cpp` | 193 | +8/-4 | D2 |
| `atx-impl/src/strategy_sign_verb.cpp` | 371 | +371/-0 | SIGN |
| `atx-impl/src/strategy_sign_verb.hpp` | 37 | +37/-0 | SIGN |
| `atx-impl/src/trial_ledger.cpp` | 5 | +4/-786 | B2 |
| `atx-impl/src/trial_ledger.hpp` | 39 | +31/-287 | B2 |
| `atx-impl/tests/CMakeLists.txt` (IC / sign hunks) | 220 | +34/-1 | AL-CLOCK, AL-COMB, C2, C3, COV, COVR, IMPL, NAV, SIGN |
| `atx-impl/tests/fixtures/composition_rules_list.json` | 355 | +146/-277 | AL-COMB, D2, ABS, S2 |
| `atx-impl/tests/strategy_ic_composition_test.cpp` | 863 | +104/-0 | D3W |
| `atx-impl/tests/strategy_ic_mom_volman_test.cpp` | 431 | +431/-0 | AL-COMB, D3W, D3W-PB |
| `atx-impl/tests/strategy_ic_rules_test.cpp` | 713 | +111/-12 | AL-COMB, D2, ABS, S2 |
| `atx-impl/tests/strategy_ic_runner_test.cpp` | 5604 | +1320/-13 | D2, D3W, ABS, S2 |
| `atx-impl/tests/strategy_ic_theme_hedge_test.cpp` | 589 | +589/-0 | AL-COMB |
| `atx-impl/tests/strategy_marginal_ic_test.cpp` | 899 | +5/-2 | D2 |
| `atx-impl/tests/strategy_sign_verb_test.cpp` | 356 | +356/-0 | SIGN |
| `atx-impl/tools/equity_strategy_ic.cpp` | 24 | +2/-0 | SIGN |

Cited, not reviewed as code (other areas or Python): the fixtures `atx-engine/tests/fixtures/{research_composition,
eval_tie,research_eval_edges,research_ledger}/` (fields area claims `atx-engine/tests/fixtures/`; I checked their pins,
section 6), the fitter's Python (`atx-impl/tools/composition_*.py`, `fit_composition_weights.py`), the screen stage
(`scripts/wave_stage_library.py`, `scripts/wave_rules.py`), `scripts/tests/test_no_python_mirror.py` (Python area;
I judged its rows for item (2)).

## 2. (1) The C++ migrations vs their Python predecessors: identity evidence, re-derived

| migration (lane) | identity of record | re-derived from | holds |
|---|---|---|---|
| composition fit, existing modes std / std-aim / ic-shrink(-aim) / erc / tsmom / resid order (D2) | `ResearchComposition.FitEqualsPythonFixture`: the C++ fit reads `fit_v1.json`, which today's Python regenerates byte for byte (`test_composition_fit_fixture.py`; exact on dyadic inputs, one non-dyadic case at 1e-12 x max(1, \|x\|)); under `--engine-fit` check 3 holds every fitted value to the Python's under P12 on every run | `fit_v1.json` created once (`d61f73c2`), never edited in range; my run of `test_composition_fit_fixture.py` (in the 33 below) | yes |
| walk-forward-v1 (D3W; no Python predecessor) | `RealExecutables.test_walk_forward_rows_are_the_cpp_fit`: the built exe's rows (decision, session, window counts exactly; every weight within P12) = the test's own Python oracle `python_rows` on the same window | my run with pool-2's p9-3a exes (in the 180: RealExecutables ran, not skipped); `WalkForward.*` 4 in root's p9-3a composition log 21 / 21 | yes |
| ew-theme-aim-v2 (D3W-PB) | `RealExecutables` aim-v2 case: check 3 under P12 then `check_written` bit for bit (the fitter's rule takes no C++ value, so D2-1 is met by bit identity); `AimV2.ClosedFormOnAPlantedModel` (= `theme_gain_weights` called directly) | my run (in the 180); my mutation M1 (section 9) shows the bit-for-bit refusal has teeth | yes |
| D3W-PB flag-absent "TRAIN identity" 21 / 24 (`pb-ident.py`: the fitter on the synthetic `test_composition_engine_fit.World` with the Python stand-ins; no exe, no real data) | 8 dirs x 3 files: 21 byte-identical; the 3 `engine-*/composition_weights.json` differ ONLY at `$.provenance.engine_fit.producer_fingerprint` | root `m5-logs/d3wpb-identity-c044.log` and `-460f.log` (21 "identical sha256" lines each, 3 DIFFERS lines naming exactly that path); `d3wpb-fingerprints-c044.json` = `-460f.json` = the lane's `pb-fingerprints-pre-deletion.json` (cmp); `OUTPUT_PRODUCERS` `d28a3089...` | yes: the differing field is exactly the producer fingerprint |
| D3W flag-absent identity 17 / 21 | 4 files differ only at `$.provenance.engine_fit.producer_fingerprint` and / or `$.provenance.resid.producer_fingerprint` | root `m3-logs/d3w-identity-cmp.txt` (17 identical; the 4 DIFFERS lines name only those two paths); `d3w-fingerprints-cmp.txt` (only `composition_engine_fit`, `composition_resid` move) | yes. NOTE for the gate: `plain-std-aim-resid` differs at `provenance.resid.producer_fingerprint` with NO new flag (D2-9 S-c moved `composition_resid`'s code), so every flag-absent `--theme-resid` fit's weights bytes move by that one producer field (section 10, item 4) |
| mom-volman-v1 sleeve (D3W, ALCOMB-7) | check 3 holds the C++ sleeve to the Python's under P12; `engine_sleeve` writes the C++ one; `RealExecutables.test_mom_volman_writes_the_cpp_sleeve_beside_the_rows` (within P12 of the flag-absent fit) | my run (in the 180) | yes |
| theme-resid-v1 (D2 relocation; D3W-PB moved the numpy reference into its test) | the C++ `ThemeResid.ThreeThemesEqualTheRegisteredRule` and `test_composition_resid.py` compare against the same exact rationals (`EXPECTED_BLEND`) | my run (in the 180) | yes |
| admission screen under `--engine-fit` (B1's exe, wired by D2) | check 2: `admission.csv` V4 columns under P12, non-float cells byte for byte, admitted order / counts / conflicts exactly; `admission.json` / `.csv` identical in every engine case | root's identity logs (admission files identical sha256 in every engine dir) | yes |
| PM7-35 sign verb (SIGN; 24 combinations) | pre-deletion: the real verb = `wave_rules.sign_pm7_35` on all 2 x 3 x 2 x 2 = 24 (status, runner sign, kind, prior) rows; root on the ledgered rows: Y-S 15 / 15 (verb = recorded = Python), Y-2 / Y-3 / Y-5 rule waves skipped | lane `sign-logs/r10-pyt-signverb-real-a.log` (`test_the_verb_equals_the_python_rule_on_every_enumerated_combination PASSED`; exe `76dbd261...` = `mega-p9-sign-a-receipt.json`); root `m5-logs/sign-identity-ledgered-3b.log` (`RESULT PASS`, exe `ef3974cf...` = p9-3b receipt); I re-derived the pinned column `PINNED = kdkd kkdd dkdk kkdd kkdd kkdd` by hand from `admission/sign_rule.hpp` `pm7_35` under `WaveZeroKeptV1` (all 24 agree); my post-deletion run: the real verb vs `PINNED` 21 passed, 0 skipped | yes |
| absent-members-v1 (ABS; OD3P's C++ side) | the defining equivalence of OD3P-1 (`AbsentMembers.*` 11; the identity filter 53 incl. the regenerated `--list-rules` fixture); cache isolation by code (`strategy_ic_runner.cpp`: blank AFTER the candidate cache served / stored the real bytes, `if (listed) std::fill(..., quiet_nan)`; `ic_cached` / `legacy_cached` false for a listed member; the `--no-composition` IC-hit path excludes listed members) | root `m3-logs/gt-2w-abs-absent.log` 11 / 11, `gt-2w-abs-identity.log` 53 / 53; flag spelling and capability agree across `strategy_ic_runner.cpp:1142-1148`, `strategy_ic_rules.cpp:547-548`, `scripts/research_roles.py:92` | yes (OD3P merged no C++: `git diff` of merges `7f6f1c75`, `a2cf7e78` over `*.cpp *.hpp CMakeLists.txt` is empty) |
| eval verb (B2) and B2-N37 | the legacy n37 test compares every float at 1e-12 relative, every other leaf exactly; golden untouched | golden SHA-256 re-hashed by me (hash only): `mega-nav-v71-summ-n37.json` `98b44b24...9f98`, `mega-nav-v71-pbo-n37.json` `bc112b38...b1ca` = the pins at `docs/plans/mega-alpha-v7-pitch.config.json:2099`, `:2169` (file unchanged in range); root `m5-logs/pyt-b2-nav-summ-v8-s0.log` 10 passed. I did NOT run the n37 test (it reads the v7.1 cells; blind) | yes |

## 3. (2) The deletion commits and the G-P5 state

| commit | Python rule removed | G-P5 row removed in the same commit | note |
|---|---|---|---|
| `598d2199` (B2) | the statistics of record (`backtest_integrity`, `nav_summ`, `dsr_total` rows) | yes (six rows) | the retired text kept verbatim as the test oracle `atx-engine/tests/fixtures/eval_tie/frozen_statistics.py` (fixtures are out of the scan by design; `git grep frozen_statistics` finds only fixtures and records, no production importer) |
| `278fc2d8` (C3b) | `matched_leverage`, `gross_matches` | yes | Python area's lane; listed for completeness |
| `460f5ba2` (D3W-PB) | `composition_resid.py` numpy reference (`centred_tied_ranks`, `tie_block_means`, ...) moved into `test_composition_resid.py` | yes | `composition_resid.py` keeps `theme_order` (see S-3) |
| `50a43599` (SIGN) | `sign_pm7_35`, `screen_decision` (`scripts/wave_rules.py`) | yes | the stage's `WR.verb_decision` only checks the receipt's echo / form / kept-dropped listing; it recomputes no verdict (no renamed copy) |

Relabels (no deletion): `4b463071` (D2: composition rows to D3, ruling D2-3), `90bc1962` (D3W-PB fix: four rows D3 -> P10,
D3WPB-Q1 / D3WPB-R1), horizon_stats D2 -> P10 (D2-5). One ruled ADDITION: `fed4d4cb` (E2) adds the
`prepare_research_fields.require_research_seal` row and its `FROZEN_PAIRS` pair (wave-2 carry "A2 note"; the comment
says root adds a ruled row there).

State at `d51d011e` (my runs on a `git archive d51d011e` copy): `test_no_python_mirror.py` 7 passed, "G-P5: 0 mirrored
rule function(s) in 0 row(s) left for P9 lanes"; with `P9_FREEZE = True` set in memory (no file edited) the freeze
test passes; 8 rows / 17 functions remain, every one `retired_by` P10; `('scripts/wave_rules.py', 'sign_pm7_35')`
stays in `FROZEN_PAIRS` (the name stays detected). The guard is the authority for the P9 list and it is clean. It
cannot see Python / C++ rule pairs whose names are not in `FROZEN_PAIRS`; four such pairs exist (S-3).

## 4. (3) D3-Q1's engine-fit identity precondition for P9-F0h

What the gate must check, as the code stands (the precise form is Required M-1; the exe set is M-2):

- **Path.** P9-W's preflight accepts a parent only if its spec's `fit.flags` carry `--engine-fit` and none of
  `--theme-tsmom` / `--theme-resid` / `--era` (`scripts/wave_kinds.py:342-356`). No cycle code wires the engine-fit
  inputs (`git grep` of `engine-factors` / `admission_exe` / `composition_exe` over `scripts/` finds none), so the
  P9-F0h spec's `fit.flags` must carry, verbatim, `--engine-fit --engine-factors <F> --admission-exe <A>
  --composition-exe <C>` (`composition_engine_fit.py:14`, `:106-109`), where F is the `atx-equity-strategy-targets
  factors` output (atx.factor-series/v1) of Y-F0's TRAIN role and library in roster order, made by the gate's Release
  NAV exe; P9-W's cell inherits those flags and adds `--walk-forward expanding --walk-forward-lag 2
  --walk-forward-step 21` (root M4 item 6: the kind's names = the fitter's).
- **Which weights files.** The TRAIN fit's output dir only: `admission.json`, `admission.csv`,
  `composition_weights.json` (no era files: `--engine-fit` refuses `--era`). Y-F0 is ew-theme-std-v1 +
  theme-erc-v1 (D3WPB-Q2), so no D2-S4 ic-shrink annotation applies.
- **Which comparison.** An `--engine-fit` weights file is the flag-absent document PLUS `provenance.engine_fit`
  (`fit_composition_weights.py:2655`; the lanes' own form `without_engine_fit`, `test_composition_engine_fit.py:255`;
  the flag-absent file never has the key, `:340`). Compare: `admission.json` and `admission.csv` by SHA-256; the
  engine `composition_weights.json` with `$.provenance.engine_fit` deleted, re-serialised canonically, by SHA-256
  against the Python file; both fits from the same cwd into fresh output dirs (the summary records paths: D3W
  ledger candidate). Record `provenance.engine_fit.status` ("agree") and whether `fit_max_rel_float_diff` is exactly 0
  (a code-agreement figure, read as 0 / non-zero by JSON path only). Expected risk, stated before the run: the written
  theme-erc-v1 covariance and ERC values are the C++ fit's, which `fit.hpp:23-31` says may differ from numpy's BLAS
  `np.dot` in the last bits on real data; such a difference is D3-Q1's FINDING path (P9-W lapses at 0 trials), not a
  re-tune.

## 5. (4) Correctness and house style

- **Summation order / fma, Debug vs Release.** clang-cl `/fp:precise` contracts `a*b + c` into `llvm.fmuladd`
  (C2's M1c-RED finding, `stats_ext.hpp:79-86`); on the non-FMA x86-64 baseline (`equity-rel`: "no AVX2", no FMA
  flag anywhere in the presets) a run-time fmuladd is mul then add in both builds, and only a Release constant fold at a
  literal call site fuses. The composition library is compiled `/O2 /Ob2` in Debug as well
  (`atx-engine/CMakeLists.txt:491-493`) and calls no libm function but `sqrt` (correctly rounded): the C++ fit is
  build-invariant (D3W's Release `p9-d3w-h` composition 17 / 17 = Debug). The eval verb calls `pow`, `erfc`, `log`,
  `exp`, `expm1` (`house_moments.cpp:46-56`, `paired.cpp:38-39`, `conditional.cpp:20-79`, `stats_ext.hpp`,
  `deflated_sharpe_house.hpp:66`): the OD5-2 class (ucrtbased vs ucrtbase, 1 ULP), so Release is the producer of record
  and B2-N37's 1e-12 covers both. No Release-only skip exists in this area's tests (`GTEST_SKIP` / `NDEBUG` scan: only
  wave-1 S1 lines in `strategy_marginal_ic_test.cpp:783-827`, unchanged). The fixed-order reductions are numpy's
  pairwise sum (three copies, S-1) and sequential loops; DetPool paths read const state (`CompositionWeightRows` 2 / 3
  workers = serial).
- **Bounded memory.** Walk-forward rows <= 4096 (`fit.hpp:208`, parse `strategy_ic_rules.cpp:699-711`), their bytes in
  `ic_composition_stage_bytes` (`strategy_ic_composition.cpp:144-170`); the per-date weight is one lookup per date, no
  allocation per cell (`Impl::weight_at`); the absent list <= 1 MiB (`metadata_text`); sign rows <= 20,000 in <= 16 MiB;
  the paired bootstrap O(T + draws) (`paired.cpp:209-216`); the eval series <= 256 MiB.
- **Seal.** The fit's TRAIN mask refuses a sealed decision (`composition_cli.cpp:57-71`); the eval verb refuses a
  session at or after the seal (`eval_series.cpp:101-105`).
- **`/W4 /WX`.** Every new target links `atx_warnings` (`atx-engine/CMakeLists.txt:489`, `:495`, `:521`, `:549`, `:551`);
  Debug receipts p9-2v / p9-2w / p9-3a / p9-3b report 0 warning lines (root report). Release is proven only for D3W
  (`p9-d3w-h`) and B2 (lane Release builds); ABS, D3W-PB and SIGN code meets Release first at the gate (TEST-1).
- **100 columns.** New engine files: one line over (`mom_volman.hpp:3`, 101; S-6); the others over 100 in
  `composition/{ic_shrink,theme_erc,theme_resid,theme_tsmom}` are lines moved unchanged from atx-impl (ruling D2-6;
  verified against BASE). The atx-impl IC files keep their dense surrounding style; ABS reflowed D3W's 19 test lines
  (`5f416d32`); the remaining new long lines there are S2's (wave 2).
- **Duplicated plumbing.** One fit entry (`fit_walk_forward` loops `fit_composition`), one sleeve formula
  (`theme_sleeve`), one `weight_at`, the IC shims re-export the engine (`strategy_ic_{shrink,theme_erc}.cpp` 5 lines).
  Remaining duplication across the fit / eval / sign TUs: S-1, S-2.
- **Tests in the right group.** Composition -> `atx-engine-research-composition-tests`, eval -> `-eval-tests`,
  ledger -> `-ledger-tests` (all `LABELS atx_research`); `SignVerb.*`, `AbsentMembers.*`, `WeightsV3.*`,
  `MomVolman.*`, `CharCovHedge.*` -> `atx-impl-strategy-ic-tests` (`atx-impl/tests/CMakeLists.txt:201-204`, `:220`).

## 6. (5) Pins: every expected hash / golden / pinned value of record that moved in the range

| pin | moved | by | ruling |
|---|---|---|---|
| `strategy_ic_result_cache.cpp` `ic_result_sources_sha256` | `2d758bff...` -> `dd3c6011...` | S2 `110fd735` (and `99ba1a64`) | S2-1 / S1-PIN (source tripwire, no version bump; in no cache key) |
| `strategy_ic_signal_cache.cpp` `dsl_vm_sources_sha256` | `fcf8021e...` -> `5bcd55c7...` | S2 `99ba1a64` | S2-1 |
| `strategy_marginal_pair_cache.cpp` `pair_sources_sha256` (+ third source `group_rerank.hpp`) | `88f97f24...` -> `df589a88...` | D2 `9c09b694` | D2 review R-1 / ruling P8 (`PairCacheSourcesPinned` asserts 3 sources) |
| `atx-impl/tests/fixtures/composition_rules_list.json` (the `--list-rules --json` envelope) | regenerated 5 times | S2, D2, AL-COMB, ABS (`265bb427`: one capability line, ABS-3) | envelope of new rules / capabilities: all 8 BASE `recipe_sha256` values present unchanged at HEAD; 2 new rule recipes added |
| `scripts/tests/test_no_python_mirror.py` `FROZEN_PAIRS` | +1 pair | E2 `fed4d4cb` | wave-2 carry "A2 note" (ruled row) |

Not moved (checked): `eval_tie/expected.json` (not in the range diff; its generator now imports
`frozen_statistics.py`, verbatim retired code); `research_eval_edges/expected.json` (after its first root merge
`c2416d62` only ADDED lines: the `cscv_wins` case, `fe152fca` PRE-15; no removed line); `research_composition/fit_v1.json`
(created once); the n37 goldens (section 2); no hex digest removed from this area's gtests other than the three source
pins above.

## 7. Required findings

| id | file:line | failing input | fix |
|---|---|---|---|
| **M-1** (missing: the comparison D3-Q1 needs) | `progress.md:726` (Ruling D3-Q1: the engine fit's `composition_weights.json` "byte-identical to the Python fit's (compare by SHA-256 only)") vs `atx-impl/tools/fit_composition_weights.py:2655` (every `--engine-fit` fit writes `provenance.engine_fit`: the two exes' SHA-256, the factor manifest SHA-256, the module fingerprint, the agreement record) and `atx-impl/tools/test_composition_engine_fit.py:255-266`, `:327-341` (the lanes' identity strips exactly that key; the flag-absent file never carries it) | Y-F0's current TRAIN fit, run flag-absent and with `--engine-fit`: the two whole-file SHA-256 differ on EVERY input, including a C++ that equals the Python bit for bit; read literally, D3-Q1 lapses P9-W at 0 trials by construction and files a false FINDING | the PM restates the comparison before P9-B0 (section 4): `admission.json` / `admission.csv` by SHA-256; `composition_weights.json` by SHA-256 of the canonical bytes with `$.provenance.engine_fit` deleted (plus D2-S4's two ic-shrink annotations on an ic-shrink parent; none on Y-F0); same cwd, fresh outputs; `status` "agree" recorded; any remaining difference (possible on theme-erc-v1, `fit.hpp:23-31`) is D3-Q1's finding. Root adds the procedure to the gate brief's step 4 and the prereg's P9-W block (2b-ii) |
| **M-2** (missing: the engine-fit exes in the gate's exe set) | `root-gate-brief.md:20-24` (step 1 builds no `atx-research-admission`) and `progress.md:406` (EXE-FREEZE names the NAV, IC and eval exes only), vs `atx-impl/tools/composition_engine_fit.py:14`, `:106-109` (`--engine-fit` requires `--engine-factors DIR --admission-exe EXE --composition-exe EXE`) | D3-Q1's P9-B0 engine-fit run (and P9-W's cell, which inherits the parent's literal `fit.flags`: no cycle code passes these flags or pins these exes) has no Release admission exe of record: it would run pool-2's Debug p9-3a exe (mixed builds, EXE-CUR) or stop; and a relink of `atx-research-admission` / `-composition` after the re-pin is not frozen, so a P9-W cell could fit against binaries other than P9-F0h's (`provenance.engine_fit` names their SHA-256: the weights bytes move, DEC-20). The gate's step 2 fitter pytest also skips `RealExecutables` silently without the two exe env vars | add `atx-research-admission` to step 1 (Release and Debug, receipt); list both research exes in the re-pin's exe table and under EXE-FREEZE; the P9-F0h spec names the Release exe paths and the factors dir F made at P9-B0 by the Release NAV exe (record F's manifest SHA-256 = `provenance.engine_fit.factors_manifest_sha256`); run the `atx-impl/tools` fitter pytests with `ATX_RESEARCH_ADMISSION_EXE` / `ATX_RESEARCH_COMPOSITION_EXE` = the Release exes and record "0 skipped" |

No Required finding in a lane's code: the two findings are gate-procedure gaps; neither needs a lane round or a revert.

## 8. Suggested

| id | file:line | problem | fix |
|---|---|---|---|
| S-1 | `atx-engine/src/research/composition/fit.cpp:32-60`; `atx-engine/include/atx/engine/research/numpy_reduce.hpp:34-60`; `atx-engine/src/research/fields/field_stats.cpp:285` (BASE) | numpy's pairwise sum, a determinism-critical reduction, now has three engine copies: D2 and B2 each added one beside the existing fields one | one helper (`numpy_reduce.hpp`) used by fields and composition (P10; bits unchanged by construction, the fixtures prove it) |
| S-2 | `composition_cli.cpp:37-55`, `eval/eval_series.cpp:117-133`, `strategy_sign_verb.cpp:177-193` (bounded readers); `strategy_sign_verb.cpp:268-294` (check-then-truncate "exclusive" write); `strategy_ic_absent.cpp:66-71` | three new hand-rolled bounded whole-file readers beside the engine's `research/fields/file_io.hpp:24` `read_bounded`; the sign receipt's `fs::exists` then `std::ofstream` leaves a window where `file_io.hpp:37` `write_exclusive` (CREATE_NEW) would not; ABS re-implements `ic_detail::pinned_text` (carried: SIGN review row 2, ABS review S-4) | reuse the existing helpers at the next touch (P10) |
| S-3 | `atx-impl/tools/backtest_integrity.py:622-628`, `:781`, `:810`, `:907` vs `atx-engine/include/atx/engine/research/ledger/trial_counts.hpp:1-8`, `chain.hpp`; `scripts/cycle/phases.py:747-752`, `:868-870`; `atx-impl/tools/composition_theme_tsmom.py:88-160`, `composition_resid.py:135`, `composition_mom_volman.py:77` vs `fit.cpp:142-246`, `:247` | Python / C++ rule pairs the G-P5 guard cannot see (names not in `FROZEN_PAIRS`): the ledger counting and chain rules (B2 kept the Python copies, report "Kept in Python ... counting rules (SQL-9)"; the cycle's Python `ledger_n` decides the cross-DSR `--dsr-n` while the verb's `house_v1` N is the C++ copy; the CMake comment `atx-engine/CMakeLists.txt:498` "chain_head, the one copy of the rule" is not true); and the theme-tsmom-v1 schedule, theme-resid-v1 order and mom-volman-v1 sleeve, which the flag-absent fit computes in Python and the C++ fit (D2 / D3W) also computes | the freeze report lists these four as P10 items beside the guard's 8 P10 rows (they are outside the P9 list, so not a G-P5 failure); correct the CMake comment at the next touch |
| S-4 | `scripts/tests/test_no_python_mirror.py:47-55` | the "C++ that decides it" column of the composition rows still names the atx-impl files (`strategy_ic_theme_erc.cpp`, `strategy_ic_shrink.cpp` are 5-line re-exports since D2); the rule now lives in `atx-engine/src/research/composition/` | text-only update of the column (pairs unchanged) when the file is next touched |
| S-5 | `atx-engine/include/atx/engine/eval/deflated_sharpe_house.hpp:95-101`, `:124-127`; `atx-engine/include/atx/engine/combine/group_erc.hpp:74` | header-inline contractible arithmetic (`(1-g) ppf + g ppf`, `beta*beta + 4 diag budget`): run-time values are build-invariant on the non-FMA baseline, but a Release constant fold at a literal call site would fuse (the M1c-RED class); no such production call site found | use `detail::mul_add_unfused` if a literal call site ever appears; nothing now |
| S-6 | `atx-engine/include/atx/engine/research/composition/mom_volman.hpp:3` | 101 columns (carried D3W-PB S-a) | reflow at the next touch |
| S-7 | `atx-impl/tests/strategy_ic_runner_test.cpp` (ABS) | the absent list's signal-cache write side and `--label-terminal` scope bypass have no test with teeth (ABS review S-2 / S-3; ruled P10 by ABS-S) | the freeze report lists them (ABS-S) |
| S-8 | `scripts/tests/test_wave_sign_verb.py:93`, `:110` | the real-exe identity cases skip silently without the exe (SIGN review row 3) | at the gate set `ATX_EQUITY_IC_EXE` to the Release IC exe and record "21 passed, 0 skipped" |

## 9. Tests run by me, the mutation, EXE-CUR

Interpreter `C:/Program Files/Python312/python.exe`, `PYTHONDONTWRITEBYTECODE=1`, `PYTHONHASHSEED=0`,
`-p no:cacheprovider`, `--basetemp` and `TEMP` / `TMP` under the session scratchpad `wave-ic/`, explicit paths. Tree: a
`git archive d51d011e` copy (atx-impl/{tools,src,strategies,tests/fixtures}, atx-engine/{tools,include,src,schemas,
tests/fixtures/research_composition}, scripts, python) in the scratchpad, except the six git-history cases (run in
pool-2, whose working tree equals `d51d011e` over those dirs; `git status --porcelain` unchanged after). Env:
`ATX_EQUITY_ROOT=C:/atx-wt/pool-2/build-equity`, `ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin`, and the exe vars
below. Nothing written under pool-2's `build-equity`.

| run | result | log (scratchpad `wave-ic/`) SHA-256 |
|---|---|---|
| `scripts/tests/test_no_python_mirror.py` (own invocation), first attempt | VOID: 1 failed / 6 passed, `test_the_allowlist_only_shrinks...`: my archive lacked `atx-impl/src` (the test checks the deciding C++ exists) | `pyt-mirror-void-archive-incomplete.log` `dc86d9a9...3e00` |
| the same, archive completed | **7 passed**; "G-P5: 0 ... in 0 row(s) left for P9 lanes" | `pyt-mirror.log` `84e3cf15...3af4` |
| `P9_FREEZE = True` in memory (module imported, no file edited) | freeze test passes; 8 rows, 17 functions, all P10 | `mirror-freeze.txt` `ff15cb8b...031a` |
| the five fitter files (`test_composition_engine_fit`, `_mom_volman`, `_resid`, `test_fit_composition_weights`, `test_fit_walk_forward`), exes below | **174 passed, 6 skipped** (all six "git history ... unavailable" in the archive), 88 subtests | `pyt-fitter5.log` `16762ccd...2d2f` |
| the six git-history cases of `test_fit_composition_weights.py` in pool-2 | **6 passed** (total 180 / 0 = root's) | `pyt-fitter-git6.log` `b4f57891...cd61` |
| `scripts/tests/test_wave_sign_verb.py` + `test_research_wave.py`, `ATX_EQUITY_IC_EXE` = pool-2 IC exe | **40 passed, 0 skipped** (21 + 19) | `pyt-sign.log` `d4be7eb4...2a46` |
| `test_composition_theme_hedge.py`, `test_fit_composition_rule_plugins.py`, `test_composition_fit_fixture.py`, `test_composition_theme_tsmom.py` | **33 passed** | `pyt-comp4.log` `b3cb0953...a398` |

Exes (SHA-256 on disk, read-only; = the receipts per root's `m5-logs/exe-sha-end.txt`):
`atx-research-admission` `411af33a0def5a6c28983dbf1ffc2fbc324694e8ac2e6b24401b27b693c9fa26` (p9-3a, bytes = p9-2v);
`atx-research-composition` `212f8b9941f0be705ebac138c03e83279fc863ab12051f00dd09501db7eed98e` (p9-3a, source
`6088bfe9`); `atx-equity-strategy-ic` `ef3974cfa82d1cc86d6a8079394e5d0964e8abcea8fd36a068b12f8b8bc99b38` (p9-3b, source
`1dd9b9b1`). **EXE-CUR:** the p9-3b IC exe is stale by source for M6's merges, but M6 touched only NAV / Python and A4's
fields TUs (`git diff 1dd9b9b1..d51d011e` over C++ / CMake: A4 fields files, `atx-engine/CMakeLists.txt` +12 fields
lines, AL-CLOCK's NAV files); `git diff --name-only` over this area's 107 files is EMPTY for `1dd9b9b1..d51d011e`,
and over composition / admission for `6088bfe9..d51d011e` and eval / ledger for `138e1d2c..d51d011e`. So the IC tests
on p9-3b describe `bba4c06f`'s IC code = `d51d011e`'s.

**Mutation (TEETH-RESTORE, TEETH-CRLF), on my own archive copy only:** `wave-ic/teeth_ic.py` (`3fb8cb28...e1`) M1
loosens `composition_engine_fit.check_written` (the bit-for-bit D2-1 check for ew-theme-aim-v2) to P12's tolerance.
Result: `test_aim_v2_carries_the_cpp_bits_or_refuses` **red ("bits" case: the run published instead of refusing):
CAUGHT**. Restore in a `finally`: raw SHA-256 `095b9038...a333` = the saved copy; `git hash-object
--path=atx-impl/tools/composition_engine_fit.py` = `git rev-parse d51d011e:atx-impl/tools/composition_engine_fit.py`
(`teeth-ic.log` `19911b8c...4bdc`). The log prints a synthetic-world summary only.

**C++ test claims (no build; from logs and receipts):**

| claim | evidence |
|---|---|
| eval 27 / 27 after B2-EVAL-KEEP (PRE-15 + `VolFlagAddsNetVolOnly`), composition 17 / 17, IC 18 / 18 (p9-2v, D3W merge) | `m3-logs/gt-2v-eval.log`, `gt-2v-comp.log`, `gt-2v-ic.log` (re-read: "[  PASSED  ] 27 / 17 / 18 tests") |
| `AbsentMembers.*` 11 / 11, identity filter 53 / 53, D3W IC 18 / 18 (p9-2w) | `m3-logs/gt-2w-abs-absent.log`, `gt-2w-abs-identity.log`, `gt-2w-d3w-ic.log` |
| D3W-PB composition 21 / 21 (p9-3a), IC 18 / 18 (p9-3a and p9-3b) | `m5-logs/gt-3a-d3wpb-comp.log`, `gt-3a-d3wpb-ic.log`, `gt-3b-d3wpb-ic.log` |
| `SignVerb*` 6 / 6 (p9-3b; lane p9-sign-a 6 / 6, exe = receipt) | `m5-logs/gt-3b-signverb.log`; pool-29 `sign-logs/r10-gtest-a-signverb.log` |
| Release: D3W composition 17, eval 25, IC 231 whole (`p9-d3w-h`, before ABS / D3W-PB / SIGN); B2 eval 26 + ledger 16 Debug and Release | pool-23 `d3w-logs/r8-rel-*.log`; `task-B2-report.md` |
| teeth: D3W-PB 4 / 4 (lane, `p9-d3wpb-b`) and the reviewer's `fit.cpp:340` (`p9-d3wpb-c`); SIGN reviewer 23 / 23 (`p9-sign-b`); ABS lane / reviewer cache-bypass teeth | the lane reviews (ledger :789, :824) |

## 10. What root must do before any cell

1. **M-1 and M-2 first** (PM ruling text + gate brief steps 1, 4, 6): the D3-Q1 comparison as in section 4; the
   Release `atx-research-admission`; both research exes under EXE-FREEZE; the factors dir of record.
2. **Gate step 2, this area's first Release run of ABS / D3W-PB / SIGN code:** whole `atx-engine-research-composition-tests`,
   `-eval-tests`, `-ledger-tests`, `atx-impl-strategy-ic-tests` in Debug and Release (0 warnings; `AimV2.*`,
   `AbsentMembers.*`, `SignVerb*`, `WalkForward.*`, `WeightsV3.*`, `EvalVerb.VolFlagAddsNetVolOnly`, the PRE-15 gtest
   among them); the fitter pytests with the Release research exes (RealExecutables 0 skipped);
   `test_wave_sign_verb.py` with the Release IC exe (0 skipped); `test_no_python_mirror.py` in its own invocation.
3. **P9_FREEZE:** set it True at the freeze (verified to pass on `d51d011e`); the freeze report lists the 8 P10 rows
   (17 functions; the 4 composition rows per D3WPB-Q1), the four unlisted pairs of S-3, the test-only oracles
   (`frozen_statistics.py`, `wave_fixture.sign_oracle`, `test_composition_resid.py` `ref.*`), ABS-S's two cache tests.
4. **Gate step 3 flag-absent identity, expected producer-only fields of fits:** `$.provenance.resid.producer_fingerprint`
   on any flag-absent `--theme-resid` fit (moved at D3W by D2-9 S-c) and `$.provenance.engine_fit.producer_fingerprint`
   on any `--engine-fit` fit; nothing else may differ. IC u / w passes without `--absent-members` / `--walk-forward`
   / `--vol`: no new key (ABS by construction; `absent_members` only with the flag); a mom-volman-v1 run gains the two
   `<role>_mom_volman_sleeve_{raw,scaled}.csv` files (ALCOMB-6; a file listing differs).
5. **EXE-FREEZE:** SIGN, D3W-PB and ABS relinked the IC exe and `atx-research-composition` before the re-pin (holds);
   if SQL4 merges at M6b it relinks the IC exe again before the re-pin, and this area gets the `d51d011e..<M6b head>`
   addendum.
6. **Unclaimed by any area's file list (PM):** S2's IC-engine files (`atx-engine/{include,src}/atx/engine/alpha/*`,
   `factory/ic_screen*`, `atx-engine/tests/{alpha,factory}/*`, incl. the OD5-2 Release skip in
   `alpha_lit_golden_test.cpp`), COV's `risk/cov_container.*` and `strategy_risk_*`, C2's `strategy_nav_calibrate.*`,
   `strategy_spo*`, `strategy_two_speed.hpp`, `strategy_target_replay.*`. I reviewed only their pins where they sit in
   this area's files.

## 11. Open questions

- M-1: does the PM keep D3-Q1's literal "SHA-256 only" (P9-W then lapses by construction) or adopt the stripped form?
- S-3: should the four unlisted Python / C++ pairs be added to `FROZEN_PAIRS` as ruled P10 rows (root's edit, as A2's)
  so the guard sees them, or only listed in the freeze report?
