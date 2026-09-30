# v8 integration log

## 2026-09-29 early integration of B-3 (move-only IC runner split)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, base `33a32907`.

### Merged

- `f5f8754e` refactor(impl): split the IC runner into five TUs, move-only -> merge `7527a063`.
- `bdca4c3d` (lane B follow-up, on `0e95a072` K1 plan rows) -> merge `5c1ea7c3`. The PM directed merging the
  lane's own `cache_schema_v2` fix instead of a root fix; the uncommitted root fix (same change) was dropped
  before the merge, so the lane and root carry one definition. No root fix commits.

### Move verification (old `strategy_ic_runner.cpp` at `ef11f462` vs the six files at `f5f8754e`)

Method: greedy longest-common-block cover (difflib, blocks of 3 lines or more, trailing whitespace only
normalised) of every old line by the new files, plus a line multiset check. Every function body is covered by
verbatim blocks; no body line is lost or duplicated. Not verbatim, other than linkage, includes and declarations:

1. `dsl_vm_sources` pin: 29 -> 30 entries (`alpha/lit_ops.hpp` added), digest `18693b18...` -> `fa1e9d0f...`,
   tripwire comment reworded from "the runner TU's include closure" to "the IC runner TUs' (strategy_ic_*.cpp)".
   Declared in the commit: the pin was stale on the base since W2 `d3221a30` (vm.hpp includes lit_ops.hpp).
   `ic_result_sources` and both semantics versions unchanged.
2. Comments: the FP build-flavor comment now speaks of all IC runner TUs sharing one flag set; the
   `theme_redistribution` comment says "rule" instead of "block", plus a new one-line pointer in admission.
3. Shared constants became `inline constexpr` in `strategy_ic_detail.hpp` (quiet_nan, vm_eval_mode,
   theme_redistribution_rule, fields_schema, io_chunk, ic_price_field, vm_compiler, vm_fp_flavor, and with
   `bdca4c3d` cache_schema_v2). vm_compiler/vm_fp_flavor depend on per-TU macros; all five TUs share one
   `/O2 /Ob2` property list in `atx-impl/CMakeLists.txt`, so one definition holds.
4. Default arguments moved to the header declarations (method_recipe, admit, load_pinned_f64, write_partial).
5. `PartialFile` ctor/dtor moved out of line (bodies verbatim).
6. Redundant `using namespace atx;` dropped (code sits inside `atx::impl::strategy::ic_detail`).

K1 (`0e95a072`): `--plan-only` JSON field `candidates` changed from a count to the K1 row array; the count is now
`candidate_count`. No reader of the old field found in `scripts/` or the Python tools.

### Builds

| tag | source | result |
|---|---|---|
| v8-1 | 7527a063 clean | FAILED: `strategy_ic_runner.cpp(491)` undeclared `cache_schema_v2` (anonymous namespace of the signal-cache TU) |
| v8-1a | 7527a063 + uncommitted root fix | ok; superseded, binaries overwritten by v8-1b |
| v8-1b | 5c1ea7c3 clean | ok, 6 TUs, 3 links |

Provenance: CMake bakes `engine_git_sha` at configure time; the last configure ran at `7527a063`, so the v8-1b
binaries report `7527a063` while built from `5c1ea7c3` (differences: K1 plan rows and the one-line constant move).
It is recorded only (cache sidecars), never keyed.

### Tests

`atx-impl-strategy-ic-tests.exe` (all): 75/75 passed (StrategyIcRunner 47, StrategyIcComposition 7, IcScreen 16,
ResearchIc 5). Anchored `StrategyIcRunner.PlanOnlyPrintsCandidateRows`: 1/1 passed. VmSourcesPinned and
IcSourcesPinned tripwires pass.

### Identity run (TRAIN 2020-2022, argv of the accepted v7.1 u pass, new cache `build-equity/v8-cache-b3`)

`run_bounded_research.py --seconds 300 --max-rss-mib 1536 --min-free-mib 512`, runner `build-equity/v8-b3-id-u-run1`,
tool `build-equity/v8-b3-id-u-1`, source `5c1ea7c3`, exe sha256 `12d0738c...`.
Receipt outcome: **time-limit** (300.4 s, exit 15, peak tree RSS 1,212 MiB, min free 1,669 MiB). 39 of 48
candidates completed. Caps not raised; no re-run.

Cause: cold, memory-pressed host IO. With an empty cache every field is hashed and loaded: fields-verified hashed
40 fields in 100.4 s and 46 field loads took 51.2 s (v7.0 lo3 cold run on this host: 2.3 s and 2.9 s).
Per-candidate VM/IC/composition totals were also about 2x the earlier cold runs.

Per file vs `build-equity/mega-v71-train-u-1/`:

| file | result |
|---|---|
| recipe.json | identical |
| train_daily_ic.csv | partial (7.3 of 9.4 MB); a byte prefix of the reference (84,828 whole lines identical) |
| train_candidates.jsonl | partial (79 of 96 records); every record equal except wall/stage seconds and signal/IC cache hit-miss fields |
| summary.json | partial (in-progress), not comparable |
| orientations.json, train_combined.{f64,json}, train_combined_{finite,member}.u8, train_combined_ids.u64, train_combined_sessions.i64, train_planned_targets.csv | not produced (killed before composition) |

Cache side (read-only on `mega-candidate-cache-v71`): all 39 signal entries and 39 IC-result entries land at the
same paths as in the v7.1 cache (same `fp_*` directories, key stems and `ic1_dc433f93f4b362bf` subdirectory);
the 39 `.f64` payloads are byte-identical; IC-result sidecars differ only in the recorded `engine_git_sha`, signal
sidecars only in recorded engine, library and fields-manifest SHAs.

### Cache-key consequence

`dsl_vm_sources` pin changed value (`18693b18...` -> `fa1e9d0f...`); `ic_result_sources` did not
(`e3e6f2d6...`). Neither pin enters a cache key: the signal key uses `vm_identity()` (`dslvm1_<compiler><flavor>`)
and the IC key `ic_identity()` (`dslic1_..._simdN`) plus inputs; the pins are read only by the tripwire tests via
`ic_cache_vm_identity()` / `ic_result_cache_identity()`. Semantics versions stay 1. The old cache would have been a
hit, confirmed by the identical entry paths above.

### Open items

- Identity not closed: orientations.json and the combined artifacts are unverified. A re-run with the now
  populated `build-equity/v8-cache-b3` (39 hits, 9 misses) and fresh output directories should fit 300 s; needs
  a PM dispatch.
- Stale configure provenance (`7527a063` baked into the v8-1b binaries and the 39 new cache sidecars); a
  reconfigure before the next recorded run would fix it.

## integration 2 (2026-09-29): B-3 identity closed; lanes EV, B, C, A, F, D merged; tiny_world goldens

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `93119ffb`. Tag prefix v8-2.

### Part 1: B-3 identity (before any merge, HEAD `93119ffb`, exe `12d0738c...` from v8-1b)

Same argv as integration 1, cache `build-equity/v8-cache-b3` (39 of 48 entries), runner
`build-equity/v8-b3-id-u-run2`, tool `build-equity/v8-b3-id-u-2`, caps 300 s / 1536 MiB / min free 512.
Receipt: **completed**, 175.8 s, exit 0, peak tree RSS 1,162 MiB, min free 1,064 MiB. 39 hits, 9 misses; the cache now
holds 48 signal and 48 IC-result entries.

| file vs `build-equity/mega-v71-train-u-1/` | result |
|---|---|
| orientations.json, recipe.json, train_daily_ic.csv, train_planned_targets.csv | byte-identical |
| train_combined.{f64,json}, train_combined_{finite,member}.u8, train_combined_ids.u64, train_combined_sessions.i64 | byte-identical |
| summary.json | equal after the drops below |
| train_candidates.jsonl | equal after the drops below (all 96 records) |

Fields dropped (any depth) for the two JSON files: timings `wall_seconds`, `stage_seconds`, `hash_seconds`; cache
counts `signal_cache`, `ic_result_cache` (per candidate hit/miss), `hits`, `misses` (also `ic_results.*`),
`vm_evaluations`, `verify_bytes`, `research_fields.field_loads`, `loaded_bytes`, `peak_resident_fields` (fields
are only loaded for cache misses). In summary.json the cache directory prefix `build-equity/v8-cache-b3` was
rewritten to `build-equity/mega-candidate-cache-v71` before comparing (`candidate_cache.directory`,
`fields_directory`, `entries[].payload`, `entries[].sidecar`); every entry key, payload SHA and field SHA is equal.
**The B-3 move-only split is identity-clean on the v7.1 u pass.**

### Part 2: merges (SHAs, in order)

| lane | lane SHA | merge |
|---|---|---|
| EV (E-3 tiny_world + ctest; V-1 part 1 move) | `425d16db` | `4f708e2d` |
| B (B-1 `--no-composition`) | `be51d529` | `143bcd74` |
| C (C-1 fit and card stores) | `cfa18014` | `1bd3f1ae` |
| A (A-3 cycle plumbing, A-1 registry/generator) | `ea7cba01` | `2ed4e02a` |
| F (F-2 marginal IC verb + engine kernel) | `c560b8c8` | `4f594031` |
| D (D-0 NAV warm start) | `168278f2` | `492c1208` |

One textual conflict, `atx-impl/tools/equity_strategy_ic.cpp` (B-1 verb table vs F-2 `if` branch), resolved in
the merge commit `4f594031` the way B-1's report specifies: `{"marginal",&atx::impl::strategy::dispatch_marginal_ic},`
as a second verb-table row plus the include; the default path is unchanged.

V-1 move: `nav_summ.py`, `backtest_integrity.py` and their tests moved to `atx-impl/tools` (nav_summ byte-identical,
713 lines). Lane EV left a shim at the old studies path, so the six pinned specs (`v61`, `v61-ops`, `v70`, `v70-lo3`,
`v71`, `v7u-lo3`) keep `summ.script` at the old path on purpose (their plan lines are pinned by the fixture identity
tests), and `test_research_cycle.py` (STUDIES import) and `studies/v6_scorecard.py` (spec_from_file_location) load
the moved module through the shim. `tiny.json` has no summ step. Left for root: only stale docstring paths, fixed in
`755bdd2b`.

### Part 3: build and tests

| tag | source | result |
|---|---|---|
| v8-2 | `492c1208` clean | ok, 165 TUs, 10 links, 1,479 s, jobs 3; CMake re-ran (glob + CMakeLists changes), provenance `492c1208` |

Targets: atx-equity-strategy-ic, atx-equity-strategy-targets, atx-impl-strategy-ic-tests,
atx-impl-strategy-target-tests, atx-impl-strategy-tests, atx-impl-tests, atx-engine-combine-tests.
No compile or link error in any lane.

C++ (anchored first, then whole executables):

| run | result |
|---|---|
| ic-tests `NoComposition.*:StrategyIcRunner.CacheMissOnRoleChange:MarginalIc.*:CombineMarginalRankIc.*` | 16/16 |
| impl-tests `NavWarmStart.*` | 4/4 |
| combine-tests `CombineMarginalRankIc.*:CombineOrthogonalize.*` | 14/14 |
| atx-impl-strategy-ic-tests (all) | 91/91 |
| atx-impl-strategy-target-tests (all) | 181/181 |
| atx-impl-strategy-tests (all) | 45/45 |
| atx-engine-combine-tests (all) | 215/215 |
| atx-impl-tests (all) | 904 run: 898 passed, 5 skipped, **1 failed** |
| `ctest -N -L atx_equity_strategy` | 317 listed (= 91 + 181 + 45) |

The one failure, `AtxImplProvenanceDigest.ConfigJsonNotInDiscoverDigest`, is not from this sprint:
`stage_discover.cpp` writes a `config_json=` line into `_manifest.txt` since `d060cd81` (2026-09-26, in base
`ef11f462`), and the test asserts the manifest is equal across config_json-only differences. No lane touched
`stage_discover.cpp` or the test. Left open (owner: whoever owns discover; not an integration slip).

Python (`pytest -q -p no:cacheprovider`):

| paths | result |
|---|---|
| atx-impl/strategies + atx-engine/tools | 336 passed |
| atx-impl/tools | 224 passed |
| scripts/tests (ATX_EQUITY_BIN unset) | 85 passed, 5 skipped (research_window absent (W0-1), live e2e without BIN, 3 RESEARCH_CYCLE_LIVE_ROOT) |
| scripts/tests/test_cycle_e2e.py with `ATX_EQUITY_BIN=build-equity/bin` | 4 passed, 1 skipped (research_window) |

tiny_world end to end: `test_cycle_e2e.py --record` passed on the first run (copy_b `reject_redundant` with
planted_b, planted_a/b admitted, gate PASS, first run 13.3 s). Goldens committed in `69176abc`: orientations
`b2143918...`, admission decisions `43206904...`, primary daily `ca559404...`, scenario
`modeled-1bn-stale5-v1+swap-fin-v1`, exe ic `7a56699b...`, nav `1bd5a37b...`. The pytest live test then failed:
under pytest's `tmp_path` the cache publish path `<root>/tiny-cache/<role sha>/fp_*/ic1_*/<id>.<key>.json`
exceeds Windows MAX_PATH ("The system cannot find the path specified", u exit 1); `--record` roots are short
(`%TEMP%/tiny-world-*`). Fixed in the test (`7de7f712`): the live test uses the same `fresh_root()` and removes it.
Then it passes; a second cycle in the recorded root prints `== <phase>: done` for fields, u, fit, card, w, nav,
executes none and exits 0.

### Part 4: identity on TRAIN (3-year role lo1, 2020-2022), bounded runner, clean tree `69176abc`

Exes from v8-2: ic `7a56699b...`, targets `1bd5a37b...`. Caps 300 s / 1536 MiB / min free 512. Every receipt
`completed`, exit 0, `git: clean in the code pathspec`.

| check | dirs | wall / peak | result |
|---|---|---|---|
| a. B-1 `--no-composition`, cache v8-cache-b3 | `v8-i2-b1-run1` / `v8-i2-b1-1` | 8.8 s / 394 MiB | **PASS**: orientations.json `95f15e08...` and recipe.json byte-identical; train_daily_ic.csv byte-identical to the reference minus its 2,176 `__combined__` lines (9,204,707 bytes); summary `composition: skipped`; 48/48 `payload=not-loaded ic_result=hit`, 0 VM, verify_bytes 0; stage seconds load 1.42, labels 1.44, ic 0.87, vm 0 |
| b. D-0 NAV v7.1 cell, no warm-start flag | `v8-i2-nav-run1` / `v8-i2-nav-1` | 49.0 s / 347 MiB | **PASS**: all 12 files byte-identical (5 daily, 5 events, recipe.json, summary.json) |
| c1. C-1 fit, fresh store `v8-i2-fitstore` | `v8-i2-fit-run1` / `v8-i2-fit-1` | 41.6 s / 346 MiB | `fit: computed 48, reused 0`. admission.csv byte-identical. **admission.json NOT byte-identical**: first difference line 1353, byte 41287, `inputs.context_sha256` (`edb8afdf...` -> `6edcef8e...`); the only other difference is `inputs.script_sha256`. composition_weights.json differs only in `provenance.{admission,context,script}_sha256` (first difference line 4, byte 133) |
| c2. same command again | `v8-i2-fit-run2` / `v8-i2-fit-2` | 1.3 s | `fit: computed 0, reused 48`; all three files byte-identical to c1 |
| c3. card, `--work-dir v8-i2-fitstore`, accepted admission | `v8-i2-card-run1` / `v8-i2-card-1` | 30.3 s / 995 MiB | **PASS**: `card: computed 48, reused 0`; all 100 files byte-identical to `mega-cards-v71` |
| c4. card again | `v8-i2-card-run2` / `v8-i2-card-2` | 13.8 s | **PASS**: `card: computed 0, reused 48`; 100 files byte-identical |
| d. A-1 `generate_library.py --library v71 --check` | `v8-i2-lib-run1` | 0.3 s | **PASS**: `fund_industry_ic_v71.json 787c802e... 39317 bytes`, slim recipe `69e95298...` |

Finding (c1): the fit's admission.json is not byte-identical to the accepted v7.1 file. The two changed values
are the ones lane C declared: `script_sha256` (fit_composition_weights.py changed) and `context_sha256` (C-1 made
the context digest a pure content digest that no longer binds the script SHA). Every decision, statistic and the
CSV are equal. Expected hashes were not touched; the PM rules whether the W3 precedent ("equal after dropping
inputs.script_sha256, context_sha256") applies. Once accepted, later fitter edits keep `context_sha256` stable.

### Fix commits (for lanes to merge root)

| commit | lane | what |
|---|---|---|
| `4f594031` (merge resolution) | B / F | `equity_strategy_ic.cpp`: marginal verb as a verb-table row |
| `7de7f712` | EV (E-3) | `test_cycle_e2e.py`: live test root under the system temp dir (MAX_PATH) |
| `69176abc` | EV (E-3) | tiny_world goldens recorded |
| `755bdd2b` | EV (V-1) | moved tests' run lines and mega_report docstring name `atx-impl/tools` |

### Open items

- `AtxImplProvenanceDigest.ConfigJsonNotInDiscoverDigest` fails (from before the sprint, see above).
- C-1 admission.json identity needs a PM ruling (finding c1).
- A-1's real-plan check (`ATX_V71_PLAN_JSON` from `--plan-only` on the lo1 role) was not run (not dispatched).
- W0-1 not yet merged: C-1 `window_id()` uses its literal fallback, A-3 derived stores stop with exit 2, the
  e2e window test skips.
- The IC runner's cache publish is not long-path aware: a deep root (over about 110 characters) fails at MAX_PATH.

## integration 3 (2026-09-29): W0-1 and nine lane commits merged, pins, build and tests; STOPPED before Part 4

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `aca1a51a` with `git merge --no-ff 880faac7`
left conflicted by the PM. Tag prefix v8-3. Stopped on the owner's instruction at the end of Part 3 (build and tests).

### STOPPED HERE, remaining parts

Parts 1, 2 and 3 are done. **Part 4 was not started: none of the identities a-g ran** (no bounded-runner run in this
integration):
- a. W0-1: D-0 NAV identity argv of integration 2 (12 files byte-identical except strings naming the window id).
- b. C-1 ruling check: read the fitter to state what `inputs.context_sha256` hashes (code/config or data).
- c. C-3: fields rebuild with `--reuse` from fields-v9 into a fresh dir; payload identity, reused/recomputed counts.
- d. B-2: v7.1 u pass at `--workers 4` and at `--workers 12` (`--max-memory-mib 2560 --max-rss-mib 2560`), fresh
  caches; orientations and daily IC identity; vm, ic, composition seconds.
- e. D-1: v7.1 NAV identity with `--stage-timers` (summary.json gains the timers key).
- f. F-0: lo1 role rebuilt with the options off; manifest identity except `universe.inputs.code.prepare_recent_research`.
- g. E-1: research-build.ps1 as the last build of the session. Partly shown already: v8-3a, v8-3b and v8-3c were built
  with `scripts/research-build.ps1 -Preset equity-dev` and each wrote `build-equity/mega-<tag>-receipt.json` with the
  new keys `Tag`, `Script`, `BuildDir`, `Executables` (SHA-256 per target, only after exit 0). The "last build of the
  session" condition belongs to whoever runs Part 4.

### Part 1: W0-1 merge and the leftovers

Merge `8a4a3edf` (lane W0E `880faac7`). Two textual conflicts, both import blocks, both sides kept:
`prepare_research_fields.py` (`import code_fingerprint` from C-1 + `import research_window as rw` from W0-1) and
`fit_composition_weights.py` (C-1's `sys.path` append, `code_fingerprint`, `record_store` + W0-1's
`from engine_tools import research_window as rw`). The window constants auto-merged in the W0-1 form
(`FIT_BEGIN_NS = rw.TRAIN_BEGIN_NS`, `TRAIN_END_NS = rw.TRAIN_END_NS`, `AIM_SEMANTICS` from `rw` dates,
`TrainWindowError`); `HOLD_BEGIN_NS` (2022-01-01, the v3-admit-v1 split) stays, as W0-1 says.

Leftovers removed:
| commit | what |
|---|---|
| `588de0b6` | fitter `window_id()` returns `rw.WINDOW_ID`; lane C's `WINDOW_ID_FALLBACK` and the `ImportError` branch are gone. `strategy_live.hpp` deploy-manifest comment names `kResearchWindowId` / `kSealBeginDate` instead of spelling them |
| `40d1a512` (merge resolution) | lane D's own `research_window.hpp` differed from W0-1's (no `kSealBeginDate`, no `static_assert`s, other comments): W0-1's header kept byte for byte |
| `16bd52ca` | `strategy_exposures_verb.cpp` (D-2) refusal text spelled `seal (2024-01-01)`: now `kSealBeginDate` |
| `ff90d14e` | `compare_window_overlap.py` (R1) `SEAL_FALLBACK_NS` literal and its `ImportError` branch removed; the seal is read through `engine_tools.research_window` (the atx-impl instance, as W0-1 prescribes); test renamed `test_seal_is_2024_01_01_from_the_window` |

Kept on purpose (guarded, but no literal date): `backtest_integrity.research_window()` loads the same
`research_window.py` by path when `engine_tools` is not importable (the old-path shim and `spec_from_file_location`
callers have no atx-impl/tools on `sys.path`); `research_tree.window_id()` derives the id from the JSON and raises
`LookupError` when the window source is absent.

Grep gate (`2023-01-01|2025-01-01|1_672_531_200|1'735'689'600|2024-01-01` over atx-engine/tools, atx-engine/src/data,
atx-impl/src/strategy_*, atx-impl/tools, scripts; run after all merges): **no production code line is left**. What is
left:
- history comment: `atx-engine/tools/conftest.py:5,8` (superseded seal 2025-01-01 and the current one);
- docstring example in the window module itself: `atx-engine/tools/research_window.py:138`;
- test fixtures bound to the superseded window by W0E's conftest: `test_build_fundamental_events.py:170,471`;
  `test_prepare_research_fields_sec.py:43-44` (NYSE holiday list), `:608` (calendar range);
- tests that pin the current window: `test_research_window.py:119,194,196`, `test_compare_window_overlap.py:1,236,397,404`.
Other spellings (`1_704_067_200`, `1704067200`, `1672531200`, `1735689600`, `date(2023|2024|2025, 1, 1)`): none in
production; one v1 fixture line `test_research_fields_price.py:122`.

### Part 2: merges (SHAs, in order)

| lane (tasks) | lane SHA | merge | conflicts |
|---|---|---|---|
| W0E (W0-1) | `880faac7` | `8a4a3edf` | two import blocks (Part 1) |
| W0E (E-1 build script, E-4 seal check) | `40163643` | `a69d7ae6` | `mega_report/data.py` docstring: E-4 text kept, V-1's moved `atx-impl/tools/nav_summ.py` path kept |
| EV (V-1 kit, V-2 holdout gate) | `a27e69fe` | `73cfc817` | `test_backtest_integrity.py` run line (same command, two wrappings) |
| B (B-2 field caps, workers 16) | `6c7cb27e` | `7261c364` | none |
| C (C-2 report-only columns, C-3 producer reuse) | `07be7eff` | `07e07d54` | import blocks: holdings (`re` W0-1 + `sys` C-3), fitter (`research_window` W0-1 + `horizon_stats` C-2) |
| A (A-2 add-alpha, `--screen`; cache gc) | `59b89d07` | `09e9d1da` | none |
| F (F-0 delisting on lo1, F-1 fields module) | `1e44f90a` | `33742f7a` | none |
| D (D-1 timers, ring, grid; D-2 exposures) | `83375f5b` | `40d1a512` | `research_window.hpp` add/add: W0-1's kept |
| R1 (compare_window_overlap tool) | `39bacec7` | `e62576b9` | none |
| G (G-0 one trial count N, chained protocol line) | `ec206a59` | `a15673a5` | none (its EV and C-2 bases were already in) |

Source pins (`5c280b64`), recomputed with a Python mirror of `expect_sources_pinned` (digest and include closure):
- `dsl_vm_sources`: W0-1 changed `atx-engine/src/data/strategy_data.cpp` and made it include
  `atx/engine/data/research_window.hpp`, an unlisted engine header. The list gains that header (30 -> 31 paths) and the
  digest moves `fa1e9d0f...0aab` -> `afbae65d...a181`. No `dsl_vm_semantics_version` bump: the change only refuses roles
  that reach the seal; no evaluated bit moves for a role that ends before it.
- `ic_result_sources`: B-2's re-pin `5bc47755...1954` recomputes equal (this also validates the mirror). No change.
Both pin tests pass (`VmSourcesPinnedToSemanticsVersion`, `IcSourcesPinnedToSemanticsVersion`).

### Part 3: build and tests

| tag | script | source | result |
|---|---|---|---|
| v8-3 | mega-build.ps1 | `5c280b64` clean | **exit 1**, 278 s, 81 TUs: `data_research_window_test.cpp(15)`: `'nlohmann/json.hpp' file not found` |
| v8-3a | research-build.ps1 | `8b561077` clean | **exit 1**, 50 s: `strategy_role_fixture.hpp(21)` (via `strategy_data_test.cpp`): same |
| v8-3b | research-build.ps1 | `159d265f` clean | ok, 54 s, 24 TUs, 10 links (CMake re-ran) |
| v8-3c | research-build.ps1 | `464e9858` clean | ok, 24 s, 3 TUs, 8 links; `ConfiguredProvenance` stays `159d265f` (no reconfigure) |

Targets (all four builds): atx-equity-strategy-ic, atx-equity-strategy-targets, atx-equity-strategy-risk,
atx-engine-data-tests, atx-engine-combine-tests, atx-engine-factory-tests, atx-impl-strategy-ic-tests,
atx-impl-strategy-target-tests, atx-impl-tests. Exes v8-3c: ic `3f43cef0...`, targets `6f277869...`, risk `f3b49c82...`.

C++ (anchored first, then whole executables; data and combine tests are from v8-3b, unchanged by v8-3c):

| run | result |
|---|---|
| data-tests `ResearchWindow.*:StrategyResearchRole.*` | 6/6 (ResearchWindow 4) |
| ic-tests `FieldCaps.*:Workers.*` + `AdmissionReportsRequiredBytes` + both source-pin tests | 7/7 (FieldCaps 3, Workers 1) |
| impl-tests `NavTimers.*:LogRing.*:ConstructionGrid.*:NavBookWorkers.*:Exposures.*:NavWarmStart.*:StrategyLive.*` + RiskVerb seal test | 33/33 (NavTimers 1, LogRing 1, ConstructionGrid 3, NavBookWorkers 1, Exposures 4, NavWarmStart 4, StrategyLive 18, RiskVerb 1) |
| atx-engine-data-tests (all) | 298 run: 284 passed, 14 skipped |
| atx-engine-combine-tests (all) | 215/215 |
| atx-engine-factory-tests (all, v8-3c) | 377 run: 375 passed, **2 failed** (below) |
| atx-impl-strategy-ic-tests (all, v8-3c) | 96/96 |
| atx-impl-strategy-target-tests (all) | 187/187 |
| atx-impl-tests (all, v8-3c) | 919 run: 913 passed, 5 skipped, 1 failed (the known `ConfigJsonNotInDiscoverDigest`) |
| `ParallelLockstepGrid.*` | **not run**: the `parallel` test group is not configured in build-equity (`ATX_TEST_GROUPS=alpha;factory;learn;data;eval;combine;risk;book;library`), so `atx-engine-parallel-tests` does not exist; `lockstep_grid.hpp` is exercised through `ConstructionGrid.*` and `NavBookWorkers.*` |

Failures:
- `ResearchIc.ParallelAdmissionRefusesUnboundedWorkersMismatchedPoolAndAggregateBudget` (factory-tests and
  ic-tests, v8-3b): still expected 5 workers to refuse after B-2 raised the bound to 16. Fixed (`5a193dbc`): refuses
  `max_research_ic_workers + 1`. Passes on v8-3c.
- `FactoryOos.R3b_DigestUnchangedByPbo` (version_id 916304603 vs pin 4049056013) and
  `FactoryOos.HoldoutEngineReuse_DigestUnchanged` (3123399341 vs pin 703512706): **not from this sprint.** The
  sprint's whole engine diff against base `ef11f462` is seven files (marginal_rank_ic.{hpp,cpp} new, research_window.hpp
  new, lockstep_grid.hpp new, strategy_data.cpp seal refusals, ic_research.hpp constant, ic_screen.cpp worker bound);
  none is on the factory OOS library path. The factory group was not built in integrations 1 and 2. Left open (owner of
  the factory goldens).
- `AtxImplProvenanceDigest.ConfigJsonNotInDiscoverDigest`: known, from before the sprint (integration 2).

Python (`pytest -q -p no:cacheprovider`):

| paths | result |
|---|---|
| atx-impl/strategies | 163 passed (6 failed before `8b561077`, see fixes) |
| atx-engine/tools | 200 passed |
| atx-impl/tools | 282 passed, 2 skipped |
| scripts/tests with `ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin` | 95 passed, 3 skipped (the three RESEARCH_CYCLE_LIVE_ROOT tests) |

tiny_world end to end: the live test failed on one golden only, `admission_decisions_sha256`
(`43206904...` -> `71b47e87...`). Cause, checked before re-recording: **W0-1** sets admission.json
`rules.train_window_ns` to `[2020-01-01, 2024-01-01)` (upper bound was 2023-01-01, W0-1's report lists this byte
change). With that one value put back to `1672531200000000000`, the new admission.json hashes to the old golden
exactly; orientations `b2143918...` and primary daily `ca559404...` did not move (IC and NAV unchanged through every
merge). Re-recorded in `5cc9eb0d` (exe ic `3f43cef0...`, nav `6f277869...`); the test then passes (5 passed).
Note: `ATX_EQUITY_BIN` must be absolute; `build-equity/bin` resolves under the temp root and the runner stops with
"executable was not found".

Checks from the ledger:
- IC `--help`: named `--no-composition` but **not `marginal`**, so lane A's `exe_capabilities` would always skip the
  marginal phase. Fixed (`464e9858`, one usage line naming the verbs). On v8-3c `exe_capabilities` returns
  `['marginal', 'no-composition']`.
- K6 layout: the verb writes `marginal_ic.json` as `{..., "candidates": [rows]}` with row keys `id, ic21, ic21_hac_t,
  marginal_ic21, marginal_hac_t, max_abs_rho, max_rho_member` (plus `max_rho_signed`, `dates`, ... which the card
  ignores), statistics finite-or-null, member string-or-null: exactly what `alpha_report_card.load_marginal_ic` accepts
  (static comparison of writer and reader; no real marginal run).
- Real plan: `build-equity/v8-i3-plan-v71.json` = `atx-equity-strategy-ic --plan-only` of the v7.1 library on the
  3-year role with fields-v9 (the u pass argv's pins, `--max-memory-mib 1536 --min-names 1000 --workers 4`; manifests
  only). `test_plan_rows_equal_static_validation` with `ATX_V71_PLAN_JSON` **fails** (finding, expected values not
  touched): 5 of 154 comparisons differ, every DSL SHA, lookback and extra-field list is equal, the maxima are equal
  (8 slots, 37 nodes, 272 bars) and `generate_library.validate_plan` accepts the plan. The differing rows (exe vs
  recipe static_validation): q5_eg nodes 29 vs 28; ftd_fail nodes 5 vs 4; ea_overdue nodes 9 vs 8; sv_flow nodes 7 vs
  6; res_mom_ind slots 4 vs 5. The recipe figures are the Python checker's estimates; ruling R2-f makes K1 the checker
  of record, so the test's per-candidate equality is stricter than the contract. Owner: lane A.

Other findings (not fixed, not in the dispatch):
- Lane A's `marginal_step` passes `--candidate-cache --library --pool [flags] --output` only; F-2's verb also requires
  `--role MANIFEST`, and add-alpha writes `marginal.flags: ["--themes"]` with no value (the verb's `--themes` takes a
  weights JSON). The marginal phase will refuse on its first real run until the spec or the step supplies `--role` and
  the themes file. Owner: lane A with lane F.
- The engine data test group did not compile before this integration either (`strategy_data_test.cpp` included
  `<nlohmann/json.hpp>` without the include path); it was simply not built in integrations 1 and 2.

### Fix commits (for lanes to merge root)

| commit | lane (task) | what |
|---|---|---|
| `8a4a3edf` (merge resolution) | W0E / C | import blocks of the builder and the fitter |
| `588de0b6` | C (C-1), W0E | fitter `window_id()` without the literal fallback; `strategy_live.hpp` comment |
| `a69d7ae6` (merge resolution) | W0E (E-4) / EV (V-1) | mega_report docstring |
| `07e07d54` (merge resolution) | C (C-2, C-3) / W0E | import blocks of holdings and the fitter |
| `40d1a512` (merge resolution) | D / W0E | W0-1's `research_window.hpp` kept |
| `16bd52ca` | D (D-2) | exposures seal refusal names `kSealBeginDate` |
| `ff90d14e` | R1 (W0-2 tool) | overlap tool: seal through engine_tools, no literal fallback |
| `5c280b64` | W0E (W0-1) | `dsl_vm_sources` re-pin (31 paths, `afbae65d...`) |
| `cc62ff6f`, `159d265f` | W0E (W0-1) | engine tests CMake: JSON include path for `data_research_window_test.cpp` and `strategy_data_test.cpp` |
| `8b561077` | W0E (W0-1), C (C-2) | fitter appends its own directory to `sys.path` (six atx-impl/strategies tests load it by path; `engine_tools` and `horizon_stats` failed to import) |
| `5a193dbc` | B (B-2) | `ic_research_test.cpp` refuses `max_research_ic_workers + 1` |
| `464e9858` | F (F-2) / A (A-2) | IC `--help` names the `marginal` verb |
| `5cc9eb0d` | EV (E-3) | tiny_world goldens re-recorded (W0-1's `train_window_ns`) |

### Open items

- Part 4 (identities a-g), see "STOPPED HERE".
- `FactoryOos.R3b_DigestUnchangedByPbo`, `FactoryOos.HoldoutEngineReuse_DigestUnchanged` (pre-sprint goldens) and
  `AtxImplProvenanceDigest.ConfigJsonNotInDiscoverDigest` fail; none from the sprint.
- `ParallelLockstepGrid.*` needs the `parallel` group configured in build-equity (a configure change, not done here).
- `test_plan_rows_equal_static_validation` against the real plan: 5 figure differences (lane A).
- Marginal phase argv: `--role` and the themes file (lanes A, F).
- `atx-impl-strategy-tests` also compiles `strategy_data_test.cpp` (changed by W0-1) and was not in the target list;
  not rebuilt or run in this integration.
- `build_provenance.cpp` is generated at configure time: v8-3c records `159d265f`, not its source `464e9858`.
- C-1 admission.json ruling (integration 2 finding c1) waits for Part 4b.
