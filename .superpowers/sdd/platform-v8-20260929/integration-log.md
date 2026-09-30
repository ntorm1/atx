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

## integration 3 Part 4 (2026-09-30)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, HEAD `41ac94fd`, tree clean before every run.
Scope: identities a-f of "STOPPED HERE" (g, E-1, was not in this dispatch; no build ran in this session). Executables:
build v8-3c as found (ic `3f43cef0...`, targets `6f277869...`; source `464e9858`, only docs commits since). Every run
went through `scripts/run_bounded_research.py`, one at a time; every receipt says `git: clean in the code pathspec`,
source `41ac94fd`. Outputs are all under `build-equity/v8-i3p4-*`.

| run | receipt | caps s / MiB / min free | outcome | wall s | peak MiB |
|---|---|---|---|---|---|
| a | `v8-i3p4-a-run` | 180 / 1,536 / 512 | completed, exit 0 | 20.4 | 358 |
| c (1) | `v8-i3p4-c-run` | 600 / 2,560 / 512 | process-error, exit 1 (stage pin refusal) | 0.5 | 46 |
| c (2) | `v8-i3p4-c-run2` | 600 / 2,560 / 512 | completed, exit 0 | 189.2 | 958 |
| d, 4 workers | `v8-i3p4-d-w4-run` | 300 / 2,560 / 512 | completed, exit 0 (cold, fresh cache) | 119.4 | 1,212 |
| d, 12 workers | `v8-i3p4-d-w12-run` | 300 / 2,560 / 512 | completed, exit 0 (cold, fresh cache) | 142.5 | 1,216 |
| e | `v8-i3p4-e-run` | 180 / 1,536 / 512 | completed, exit 0 | 17.3 | 358 |
| f | `v8-i3p4-f-run` | 600 / 2,560 / 512 | completed, exit 0 | 1.6 | 216 |

### a. W0-1: v7.1 NAV cell, integration 2 argv (PASS)

Argv: that of `v8-i2-nav-run1/receipt.json` with `--output build-equity/v8-i3p4-a-nav`. Result: **all 12 files
byte-identical** to `mega-nav-v71u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247` (5 daily, 5 events, recipe.json,
summary.json). No NAV output names the window id, so nothing needed excepting. stdout.log hash equals integration 2's
(`813ce59c...`).

### b. C-1: what `inputs.context_sha256` hashes (read, no run)

`fit_composition_weights.py` `Context.__init__` (lines 898-910): `digest = sha256(canonical_compact(meta))`. `meta` holds:
- `schema` and `semantics`: two constant strings;
- `role_manifest_sha256`, `decision_begin`, `decision_end_exclusive` and the `refused` decision list;
- `arrays`: dtype, shape and SHA-256 of the five derived arrays `columns`, `used`, `basis` (the price-risk-v1
  neutralization bases), `forward` (forward returns) and `used_rows`, computed from the role payloads.

So it hashes **data** (derived content bound to the role), not code. Before C-1 (base `ef11f462`, line 841) `meta` also
held `"script_sha256": SCRIPT_SHA256`, the SHA-256 of the fitter file's bytes, so every edit of the fitter moved the
digest. C-1 removed that key; the code is now bound by the store path (`WorkStore.context_dir` = CONTEXT_PRODUCERS
fingerprint), not by the digest.

Proof, metadata only (stored `v8-i2-fitstore/.../context/aab64ac1.../context.json`):
- sha256 of the C-1 meta as stored = `6edcef8e...` = integration 2 `v8-i2-fit-1` `inputs.context_sha256`;
- sha256 of the same meta plus `script_sha256` = the accepted file's `inputs.script_sha256` (`4cff96b6...`) =
  `edb8afdf...` = the accepted `mega-weights-v71-ew` `inputs.context_sha256`, exactly.

So the five array SHAs, the role binding and the window are equal to the accepted run's. The two provenance
differences of integration 2 are both code SHAs:
- `script_sha256`: the file changed;
- `context_sha256`: the file's SHA was dropped from the hashed meta.

admission.csv was byte-identical because it holds only decisions and statistics, no provenance hash.

### c. C-3: fields rebuild with `--reuse` of fields-v9 (FINDING)

fields-v9 has no receipt (built direct). Argv as reconstructed in `w0-2-runbook.md` R10 for the 3-year role, with the
source roots and stage pins recorded in fields-v9's own entries:
- role lo1 (`3e79978a...`), the 63 names of `scripts/specs/v71.json` `fields.list`;
- sources: `--finra`, `--tickerhistory`, `--finra-short-volume`, identity-bridge-r4-v1, fundamental-events-v2,
  `--fund-lag-sessions 1`, `--sec-stages` with the v2-pit bridge and the three SEC stage pins, the five holdings stages;
- `--max-rss-mib 2048 --max-seconds 580`;
- `--reuse build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9 --reuse-sha256 8fd00e9f...` in copy mode, so no
  inode is shared with the accepted directory.

- Run 1 (`v8-i3p4-c-run`), with fields-v9's own regsho pin `68f431f0...`: refused before any compute or output.
  `research_fields_holdings.pin_stage`: "regsho_threshold stage manifest SHA-256 does not match
  --regsho-threshold-sha256". The stage was republished (W0-2 runbook blocker 1). **fields-v9's argv can no longer
  be replayed as recorded.**
- Run 2 (`v8-i3p4-c-run2`, output `v8-i3p4-c-fields2`, manifest `5e5def8d...`): same argv with the live pin
  `fb073c62...` (runbook value). Completed.

Payload identity vs fields-v9:
- **62 of 63 field payloads byte-identical** (manifest sha256 = fields-v9 pin = bytes on disk).
- `regsho_threshold_days63` differs (`031016d4...` -> `2cb2fe40...`, with its coverage, NaN reasons and stage pin),
  from the republished stage.

Other entry differences:
- the 14 SEC fields: `clock` and `formula_sha256` (W0-1: `SEC_CLOCK` names the seal date);
- `inst_own_share`, `inst_own_chg_q`, `ftd_shares_ratio21`: in `sources`, only the path of this run's
  `shares_out.f64` (same SHA).

Top-level differences: `seal` (2024-01-01), code identity, `source_checks`, `reuse`.

**Reuse counts: reused 0, computed 63** (C-3 expected 40 builder fields reused on this first rebuild). `reuse.not_reused`
states the reasons:
- 40 builder fields: "producing code differs (builder closure of group role/finra/th/issuer; prior code from git
  blob `0347e46c`)". A closure diff (`code_fingerprint.Module.reach` / `closure`, against prior blob `0347e46c` and
  v9's builder blob `3f8e8c09`) shows every group now reaches the new `rw` import binding and changed `SEAL`
  (`= rw.SEAL`) and `Role` (seal refusal): **W0-1**, whose report says "--reuse recomputes once". Also changed:
  - `FIELDS` (finra, th);
  - issuer: `issuer_fields`, `load_events`, `sic_mapping` and the new `SIC_STAGE_*` / `reference_classifications`
    names.
  - C-3's own comparison was of pre- and post-C-3 code only, before W0-1 was merged.
- sv_ratio126: producing code differs (finra_sv: `sv_field` / `sv_window_files` from C-3, plus `rw` / `SEAL`).
- 14 SEC fields: "formula differs" (W0-1's clock text).
- 8 holdings fields: "producing code differs" (module or builder blob changed; C-3 open risk).

The copy path of C-3 was therefore not exercised. C-3's step 2 was not dispatched and not run: `--reuse
v8-i3p4-c-fields2` with the live regsho pin, expecting reused 63, computed 0.

Read set: equal to fields-v9's.
- CNMS: 1,112 files, list SHA `6a968e5a...`.
- insider: 33 files read, 13 not read after the role.
- TickerHistory3: rows scanned and selected equal.

SEC row counters moved slightly, because the seal moved and fewer post-role rows count as used. No payload moved.

### d. B-2: v7.1 u pass at 4 and 12 workers (PASS)

Argv: that of `mega-v71-train-u-run1/receipt.json`, with these changes:
- `--max-memory-mib 2560` (was 1536);
- `--workers 4` or `--workers 12`;
- fresh cache roots `v8-i3p4-d-w{4,12}-cache`;
- outputs `v8-i3p4-d-w{4,12}-u`.

Both runs were cold and finished well inside 300 s, so no re-run was needed.

Byte-identical across w4, w12 and the accepted `mega-v71-train-u-1`: `train_daily_ic.csv`,
`train_planned_targets.csv`, `train_combined.f64`, `train_combined_{member,finite}.u8`, `train_combined_ids.u64`,
`train_combined_sessions.i64`.

Differences, all by design:
- `orientations.json`: only `recipe_sha256`; `candidates` and every other key equal.
- `recipe.json` vs the accepted run: `max_working_bytes` (1,610,612,736 -> 2,684,354,560) at both counts, plus
  `vm_workers` / `research_ic_workers` 4 -> 12 at 12.
- `train_combined.json`: only `run_recipe_sha256`.
- `train_candidates.jsonl` (96 records): only `wall_seconds`, `stage_seconds.*` and the signal / IC cache hit-miss
  fields.
- `summary.json`:
  - the recipe and orientation hashes;
  - the cache directory, entry paths and hit-miss counts;
  - timings, `workers`, `ic_scratch_bytes` and `admitted_working_bytes`;
  - field-load counts, which differ from the accepted warm run.

Admitted working bytes: 1,553,063,994 at 4 workers (= the runbook formula) and 1,670,986,170 (1,594 MiB) at 12, as
B-2 estimated.

| workers | vm s | ic s | composition s | fields_verify s | fields_load s | role wall s |
|---|---|---|---|---|---|---|
| 4 | 65.53 | 18.33 | 11.90 | 5.16 | 5.03 | 119.0 |
| 12 | 81.82 | 8.92 | 26.96 | 4.35 | 5.72 | 142.0 |

Finding (timing only): on this host (12 physical / 16 logical cores, about 3.5-5.5 GB free), 12 workers halve the IC
stage but slow VM (+25%) and composition (x2.3), for a net +23 s. One sample each, cold.

### e. D-1: v7.1 NAV cell with `--stage-timers` (PASS)

Argv: that of a plus `--stage-timers`, output `v8-i3p4-e-nav`.
- 11 files byte-identical to the accepted cell.
- `summary.json` gains exactly one top-level key, `stage_seconds`, inserted in sorted position before the last key
  `status` (the first 122,071 bytes are unchanged). With the key dropped it equals the accepted summary, key order
  included.
- Timers: load 1.22, exposures 6.70, construction 1.42, books 6.74, hash 0.01, write 1.05; the six sum to wall 17.13 s.

### f. F-0: lo1 role rebuilt, delisting options off (FINDING)

Argv: that of `recent-fast-train-2020-2022-v2-lo1-run/receipt.json` (same three `--bind`s, `--check-fields` fields-v6),
with `--out build-equity/v8-i3p4-f-role`. Manifest `19383422...` (accepted `3e79978a...`).

- **Payloads: all 7 files byte-identical** (close, raw_close, volume, present, member, ids, sessions).
- The manifest differs in 19 JSON paths, not one:
  1. `universe.inputs.code.prepare_recent_research` (3 values): the expected difference.
  2. `universe.inputs.code.prepare_research_fields` (3 values): F-0's report anticipated this ("plus the
     prepare_research_fields values if that file has changed since").
  3. W0-1, the seal moved from 2025-01-01 to 2024-01-01:
     - `universe.point_in_time`: equal after replacing the seal date;
     - 8 `universe.inputs.identity_bridge` counters (`checks.linked_ciks`, `rows_available_on_or_after_2025_dropped`,
       `rows_ignored_off_axis`, `rows_never_qualifying_on_role`, `rows_used`,
       `rows_used_available_exactly_at_start_mark`, `rows_used_primary`, `class_status_rows.common`);
     - 4 `universe.inputs.sic_events.checks` counters (`rows_available_on_or_after_2025_dropped`,
       `rows_ignored_unlinked_cik`, `rows_sharing_cik_and_clock`, `rows_used`).

     Rows available on or after the new seal are now dropped before classification. The key names still say 2025
     (W0-1 deviation 6). The counts are not copied here because they count rows in the sealed year.
- F-0's flag-off path is payload-identical. The manifest SHA changes by W0-1 as much as by F-0. The PM rules whether
  that is acceptable.

### Hidden-data record

- No session on or after 2024-01-01 in any stdout or stderr of these runs (grep for 2024+ dates, `year=`, `q` forms:
  0 hits).
- Inputs were the accepted TRAIN role, fields and artifacts only. No directory named validation, val, holdout or a
  2023+ range was opened.
- Like the accepted fields-v9 manifest, the new fields manifest carries listing metadata that reaches past the seal:
  - the CNMS directory `listed_last_date` and `downloaded_at_*`;
  - the insider stage's `quarters_listed` range;
  - the SEC module's NYSE rule calendar `calendar.last`;
  - caveat texts.

  These are file names, download times and a rule calendar, not data rows. My scan printed only their JSON paths,
  never the values.
- The read set equals fields-v9's (see c); no file after the role was read.
- The role and fields manifests hold seal-drop counters; I saw the role's (f) while diffing, and they are not copied.
- **No disclosure.**

### Open items

- C-1: the PM rules on `context_sha256` (b: a data digest; integration 2 moved only the two code SHAs).
- C-3:
  - the regsho_threshold stage was republished, so fields-v9 cannot be rebuilt as pinned;
  - W0-1 invalidated every builder group's reuse fingerprint once;
  - C-3's step 2 (`--reuse v8-i3p4-c-fields2`, expect reused 63) is the remaining check of the copy path.
- F-0: the lo1 manifest cannot stay byte-identical after W0-1: seal text and counters, plus both code identities.
- B-2: 12 workers are slower than 4 on this host (vm and composition).
- g (E-1 "last build of the session"): not in this dispatch.

## integration 4 part A (2026-09-30)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `4647d325` (clean). Tag prefix v8-4.
Scope: merges, pins, build, Ruling E-22a, tests. No identity run and no real-data run in this dispatch.

### Merges (in order)

| # | lane (tasks) | lane SHA | merge | conflicts |
|---|---|---|---|---|
| 1 | R1 (R-1 `ew-theme-std-v1`) | `ec2dfd16` | `168b6578` | `fit_composition_weights.py` import block: root's path block (ENGINE_TOOLS, IMPL_TOOLS, `code_fingerprint`, `record_store`, `rw`, `horizon_stats`) kept, then R1's `import composition_rules` (same directory, importable through IMPL_TOOLS) |
| 2 | R45 (R-4 hold band, R-5 ADV cap + tests, E-16, grid fix `dca2165a`) | `8fa1005f` (PM note: `1e4a50c8` + docs) | `30a2a950` | none |
| 3 | G (book diagnostics) | `fea9b6e9` | `94f953eb` | none |
| 4 | R6 (warm-up exclusion, engine `solve_tracking`) | `a7a26df1` | `0a399c4c` | none (engine CMake auto-merged: `target_tracking.cpp` and R45's `target_shaping.cpp` both listed) |
| 5 | F3 (F-1 reuse fix E-21, F-A `grp_ff12f49`, F-B `k8_item402_63`) | `0687e82f` | `b5243c5d` | none |
| 6 | REPORT (E-11 seal exemption, memmap seal check) | `ec47b8ce` | `a6bbf8c7` | none |
| 7 | H3 (H-3 mining glue part 1) | `95859cc9` | `bf4d28a4` | `atx-engine/CMakeLists.txt` Debug `/O2` blocks: R6's `target_tracking.cpp` and H3's `role_panel.cpp`, both kept |
| 8 | H1 (report only) | `1e66a7e1` | `3c1d0e01` | none |
| 9 | A2 (marginal step argv, E-19 plan-rows test) | `d55ad8e1` | `30b80cf6` | none |
| 4b | R6 (R-6 part 2, spo-v3 rule; PM note) | `23d663b4` | `5c6efcd4` | `atx-impl/tests/CMakeLists.txt` target-tests list: R6's two spo-v3 tests and R45's `book_target_shaping_test.cpp`, all kept |

Checks after the merges:
- `research_window.hpp`: byte-identical to root's W0-1 file (`git diff 4647d325 HEAD` empty; H3 had imported the same bytes).
- F-1 reuse interface (after merge 5): complete in `atx-engine/tools/research_fields_price.py` (lane commit `eca04c18`):
  `HOST_HANDLES = ("h",)`, `producer_group`, `field_spec`, `reuse_inputs`, `entry_inputs`; entries record `producer`
  (no `producer_code` left). Nothing to complete.
- R1 / H3 both edit `strategy_ic_runner.cpp`: no overlap (R1 threads the theme rule; H3 moves the guard and the IC
  config to the engine). IC `--help` still names the `marginal` verb and `--no-composition`.
- No lane A2 report arrived with the merge (lane commit messages only); `task-A-2-report.md` is the earlier one.

### Source pins (`3287922a`)

Recomputed with a Python mirror of `expect_sources_pinned` (digest + closure). The mirror reproduces root's
`afbae65d...a181` and `5bc47755...1954` on `4647d325` before any use.
- `dsl_vm_sources`: 33 paths = root's 31 (W0-1's `data/research_window.hpp` already listed) + H3's
  `atx-engine/include/atx/engine/data/role_panel.hpp` and `atx-engine/src/data/role_panel.cpp`. After the merges the
  list was 33 paths with root's old digest. New digest `f24cfbbec5404cec34f785b89724f1d0525469823358b2374ac053e009cbf55c`
  (H3's lane value `ec506918...` was computed on its 32-path list without W0-1's header). Closure clean.
- `ic_result_sources`: 10 paths; H3's lane re-pin `e7a40331a3f2f1a4268feece00d354961ae7ab8215a379733d5855f40f61579a`
  (ic_research.hpp label accessors, ic_screen.cpp `research_window_ic_config`) recomputes equal: no change.
- No semantics bump. `overlay_panel` and `research_return_guard` are the runner's `dsl_panel` and `guard_for` moved
  verbatim (read side by side: same statements, `co::`/`al::` spelled `core::`/`alpha::`); `research_window_ic_config`
  sets the same seven fields as the five deleted lines of `score_role`; the label accessors are read-only. The guard
  has no multiply-add to contract. The signal-cache key never hashes sources and the IC-result key hashes the guard
  and config by value, so an unexpected change would miss, never hit stale.
- R1's `combine/group_rerank.hpp` is in the IC runner TUs' include closure but in neither list: it is composition,
  which no cache stores (same as `combine/marginal_rank_ic.hpp`).

### Builds (`scripts/research-build.ps1 -Preset equity-dev`)

Targets (all except b and c): atx-equity-strategy-ic, atx-equity-strategy-targets, atx-equity-strategy-risk,
atx-engine-data-tests, atx-engine-combine-tests, atx-engine-factory-tests, atx-engine-book-tests,
atx-impl-strategy-ic-tests, atx-impl-strategy-target-tests, atx-impl-strategy-tests, atx-impl-tests.

| tag | source | result |
|---|---|---|
| v8-4 | `3287922a` clean | **exit 1**, 88 s, 19 TUs: `strategy_target_replay.cpp(561)`: call to `form_desired` is ambiguous (fix `21b498d9`) |
| v8-4a | `21b498d9` clean | ok, 243 s, 79 TUs, 12 links (all 11 targets) |
| v8-4b | `21b498d9` + 1 dirty (E-22a probe) | ok, 14 s, 1 TU; factory-tests only |
| v8-4c | `c41d401e` clean | ok, 1 TU; factory-tests only (`f4ad1ae2...`) |
| v8-4d | `5c6efcd4` clean (R6 part 2 merged) | ok, 63 s, 17 TUs, 8 links; configure re-ran (provenance `5c6efcd4`) |
| **v8-4e** | `d5e5510a` clean | ok, 38 s, 3 TUs, 8 links; `ConfiguredProvenance` stays `5c6efcd4` (no reconfigure) |

v8-4e executables: ic `44552200707baed9b5ffa24daa6272c8c0a3630630be5f09ee09a46f1d7a1077`, targets
`38489b90c8dfc7feac7442bb421452f6742d99e26a4f481600316c04aeb945c2`, risk
`b9e22d8502e1cf20d3eb9d51d99b04df361a328a0cb4ee38880ded74216df322`; engine test exes unchanged since v8-4a/4c
(data `141bfbfd...`, combine `56d2d547...`, factory `f4ad1ae2...`, book `cfad97bb...`).

The lanes' uncompiled C++ produced one compile error in total (R45, v8-4); R6 part 2, H3, R1 and the engine kernels
compiled clean under `/W4 /WX`.

### Ruling E-22a (FactoryOos)

- Probe (v8-4b, not committed): `lib::CorrIndexRule::LegacyBandsV1` as the 4th argument of `Library::open` in
  `R3b_DigestUnchangedByPbo` (1 call) and `HoldoutEngineReuse_DigestUnchanged` (2 calls). **Both passed against the
  old pins** 4049056013 and 703512706 (and the unchanged digest and admitted pins).
- Edit reverted (tree clean), then re-pinned (`c41d401e`): 916304603 and 3123399341, each with a comment citing
  a187e2fe and the v8-4b confirmation. v8-4c: both pass; factory-tests whole 377/377.

### Tests

C++, anchored first (exe of the build named):

| exe (build) | filter | result |
|---|---|---|
| strategy-ic-tests (v8-4d) | `CompositionV8.*:StrategyIcRunner.*:StrategyIcComposition.*` | 61/61, CompositionV8 5 (all five of R-1), both tripwires `VmSourcesPinnedToSemanticsVersion`, `IcSourcesPinnedToSemanticsVersion` pass |
| strategy-ic-tests (v8-4e) | H-3's `StrategyIcRunner.*:NoComposition.*:FieldCaps.*:Workers.*:StrategyIcComposition.*:IcScreen.*:ResearchIc.*:MarginalIc.*:CombineMarginalRankIc.*` | 101/101 (= the whole exe) |
| combine-tests (v8-4d) | `GroupRerank.*` | 5/5 |
| book-tests (v8-4d) | `BookTargetShaping.*:TargetTracking.*` | 19/19 (10, 9) |
| factory-tests (v8-4e = v8-4c exe) | H-3's `FactoryFidelity*:FactorySearch*:ResearchIc*:IcScreen*` | 45/45 |
| strategy-target-tests (v8-4d) | `HoldBand.*:AdvHold.*:BookTargetShaping.*:StrategyLive.*:NavV7Hook.*:Spo*` | 88 run: 85 passed, 1 skipped, **2 failed** (fixed, below) |
| strategy-target-tests (v8-4e) | same | 88 run: **87 passed, 1 skipped**: HoldBand 10, AdvHold 6, BookTargetShaping 10, StrategyLive 19, NavV7Hook 11, Spo* 32 (SpoSolver 7, SpoAlpha 1, SpoCalibration 2, SpoRisk 2, SpoHook 11, SpoTripwire 2, SpoPin 2, SpoV3 5) |
| strategy-target-tests (v8-4e) | R-5's rest `StrategyTargetReplay.*:TargetReplayV5.*:TargetReplayV6.*:StrategyNavReplay.*:NavWarmStart.*:NavV5.*:NavV6.*:ConstructionGrid.*:NavBookWorkers.*:NavTimers.*` | 75/75 |

R6 part 2 requirements (PM note):
1. SpoPin v1 digests pass unchanged: `[spo-pin] weights=0xda6b6871e7e267c5`, `replay=0xaabdbb72f99a6e13` (pins
   untouched by the merge; `strategy_spo_pin_test.cpp` only moved its procedures to `strategy_spo_digest.hpp`).
2. `SpoV3.V1AndV2DigestsUnchanged` passes its v1 checks and SKIPS (v2 placeholder 0). The capture protocol needs a
   build of a pre-R6 tree: not done (open item). For the record only, the R6 head prints
   `[spo-v3-pin] v2 weights=0xb039820b40d5cf24 (40 diagnostics rows) replay=0xd24b61721a7c698c (30 days)`; these
   are **not** pins, the pre-R6 build must print them first.
3. SpoV3 runtimes (v8-4e): ZeroCostNoLimits 65 ms, GrossCapIsSlackOnFixture 220 ms, ReportsTrackingError 202 ms,
   Parse 2 ms, V1AndV2 214 ms (skip). No fixture solve near the 2,000-iteration cap in wall time.

Whole executables:

| exe | build | result |
|---|---|---|
| atx-engine-data-tests | v8-4d (= v8-4e exe) | 298 run: 284 passed, 14 skipped |
| atx-engine-combine-tests | v8-4d (= v8-4e exe) | 220/220 |
| atx-engine-factory-tests | v8-4d (= v8-4c = v8-4e exe) | 377/377 (FactoryOos green after E-22a) |
| atx-engine-book-tests | v8-4d (= v8-4e exe) | 147/147 |
| atx-impl-strategy-ic-tests | v8-4e | 101/101 |
| atx-impl-strategy-target-tests | v8-4e | 220 run: 219 passed, 1 skipped (SpoV3 v2 placeholder) |
| atx-impl-strategy-tests | v8-4e | 45/45 |
| atx-impl-tests | v8-4e | 947 run: 940 passed, 6 skipped, 1 failed: the known `AtxImplProvenanceDigest.ConfigJsonNotInDiscoverDigest` |

Python (`pytest -q -p no:cacheprovider`; no `.py` file changed after `21b498d9`):

| paths | result |
|---|---|
| atx-impl/strategies | 163 passed |
| atx-engine/tools | 214 passed (6 subtests) |
| atx-impl/tools, `ATX_EQUITY_BIN` absolute, v8-4e | 314 passed, 2 skipped (`ATX_EQUITY_TARGETS_EXE`, `ATX_EQUITY_ROOT` unset) |
| `test_exposures_export.py` with `ATX_EQUITY_TARGETS_EXE` = v8-4e targets | 3 passed (the skipped verb test included; synthetic role) |
| scripts/tests, `ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin`, v8-4e | 97 passed, 3 skipped (the three RESEARCH_CYCLE_LIVE_ROOT tests) |

- `test_dsr_n_equals_trial_counts_with_defect_and_rerun_lines`: passed.
- tiny_world end to end (`test_cycle_e2e_goldens_redundant_copy_and_idempotent_rerun`, live on v8-4e): **passed, no
  golden moved**, nothing re-recorded.
- E-19 (A2): `test_plan_rows_equal_static_validation` with `ATX_V71_PLAN_JSON=build-equity/v8-i3-plan-v71.json`
  (integration 3's saved `--plan-only` JSON, read only): passes (3 plan tests passed). Integration 3's 5 per-member
  slot and node differences are now checked against the house budget, not for equality; DSL SHA, lookback, extra
  fields and the three maxima still must be equal, per the ruling.
- R-5 note: `NavHolding` +16 B. No test refused on a workspace budget; identity runs with a tight `--max-bytes` were
  not in this dispatch.

### Fixes (for lanes to merge root)

| commit | lane (task) | file | reason |
|---|---|---|---|
| `168b6578` (merge resolution) | R1 (R-1) / W0E, C | `atx-impl/tools/fit_composition_weights.py` | import block, both sides |
| `bf4d28a4` (merge resolution) | H3 / R6 | `atx-engine/CMakeLists.txt` | two Debug `/O2` blocks, both kept |
| `5c6efcd4` (merge resolution) | R6 part 2 / R45 | `atx-impl/tests/CMakeLists.txt` | target-tests list, all three sources kept |
| `3287922a` | H3 (H-3) | `atx-impl/src/strategy_ic_signal_cache.cpp` | `dsl_vm_sources` digest for the merged 33-path list (the commit title also names W0-1's header, which root had already listed; only the role_panel lift moved the digest) |
| `21b498d9` | R45 (R-4) | `atx-impl/src/strategy_target_replay.cpp` | v8-4 compile error: `detail::DesiredState*` brings `detail::form_desired` in by ADL, the unqualified call in `replay_targets` was ambiguous; qualified as the detail forwarder does |
| `c41d401e` | E-22a | `atx-engine/tests/factory/factory_oos_test.cpp` | two `kPinnedVersionId` re-pinned, comments cite a187e2fe |
| `09bb1ad4` | R45 (R-5, grid fix `dca2165a`) | `atx-impl/src/strategy_nav_replay.cpp` | `replay_books` compared the base variant with itself; `same_shared` now includes `adv_hold_q`, so a NaN Q was refused with the grid message before `validate_nav_config` (`AdvHold.RefusedOutsideTheNavPathAndWhenMalformed`). Loop starts at the second variant; every non-NaN configuration and every multi-variant grid refuses as before |
| `d5e5510a` | R6 (R-6 part 2) | `atx-impl/tests/strategy_spo_v3_test.cpp` | `SpoV3.ZeroCostNoLimitsReturnsAimTo1e8` fed a demeaned uniform signal (gross about 5) as the desired target, so L x desired crossed the 2 x L bound; the fixture now scales to gross 1 like every real desired target. No production code changed |

No design error found.

### Hidden-data record

No real-data run. Inputs were the unit-test fixtures (synthetic) and one read of integration 3's plan JSON for the
2020-2022 role (a `--plan-only` output: DSL SHAs, lookbacks, slot and node counts, no statistic). Nothing dated
2024-01-01 or later was opened. **No disclosure.**

### Open items

- spo-v2 pin capture for `SpoV3.V1AndV2DigestsUnchanged` (section 6 step 4 of `task-R-6-report.md`) needs a build of a
  pre-R6 tree; the test skips until then.
- Part B identities from the lane reports, none run here: R-1 identity cell (rerank false), R-4/R-5 flag-off NAV and
  holdings identities (the `NavHolding` +16 B reserve included), R-6 flag-off identities (a) v7.1 and (b) spo-v2, H-3
  warm v7.1 u pass (48 cache and 48 IC hits), C-3 step 2 (`--reuse` from `v8-i3p4-c-fields2`, expect 63 reused),
  REPORT's E-4 step 3 re-render, E-1 "last build of the session".
- `AtxImplProvenanceDigest.ConfigJsonNotInDiscoverDigest`: known, pre-sprint.
- Lane A2 sent no report with its merge (commit messages only).
- `ParallelLockstepGrid.*` still needs the `parallel` group configured in build-equity.
- `build_provenance.cpp` is generated at configure time: v8-4e records `5c6efcd4`, not its source `d5e5510a`.
