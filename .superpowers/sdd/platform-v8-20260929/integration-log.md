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

## integration 4 part B (2026-09-30), STOPPED HERE

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `2633af17` (clean). Tag prefix v8-5.
**OWNER STOP** (relayed by the PM) arrived after the merges, the v8-5 build and the gtests, while the Python sets were
running (let finish, not killed). No identity run and no real-data run was started in this dispatch.

### Merges (in order)

| # | lane (tasks) | lane SHA | merge | conflicts |
|---|---|---|---|---|
| 1 | F3 (F-C `gscore7_lowbm`, F-D `eps_consist_4y`, report) | `fc8ff96c` | `68d78dc3` | none (3 files) |
| 2 | H1 (H-1 era shards tooling) | `cf757c97` | `c23f1b43` | 2, below |
| 3 | REPORT (`mega_report/v8.py`, v8 pitch config, scorecard template) | `a9a244f6` | `a572d63f` | none (7 files) |
| 4 | R6 (Ruling E-26: spo-v1/v2 refuse `--hold-band` / `--adv-hold-q`, spo-v3 records the shaping) | `f60a524e` | `37e84d81` | none |

H1 resolutions (both sides kept):
- `atx-impl/tools/fit_composition_weights.py`, before `files[OUTPUT_WEIGHTS] = canonical_bytes(document)`: root's R-1
  `if args.composition == composition_rules.STD_RULE_ID: composition_rules.attach_std(document, std)`, then H1's
  `if pool is not None: document["provenance"]["pool"] = pool["block"]`. Both precede the bytes and the per-era copies.
- `scripts/research_cycle.py` docstring: A2's `marginal` entry, then H1's `roles` entry (H1's spacing kept).
- Union check (scratch script, `git diff -U0` +/- line multisets): for both files, merged-vs-root equals H1's own diff
  since the merge base `1e66a7e1` (150 and 133 lines) and merged-vs-H1 equals root's diff since that base (110 and 9
  lines). The auto-merged `nav_summ.py`, `backtest_integrity.py`, `cycle_verdict.py`, `research_ledger.py`,
  `run_bounded_research.py` had no root change since `1e66a7e1`, so they are H1's bytes.
- Not resolved by the merge (design, H1 open risk): the pooled fit (`--era`) refuses `ew-theme-aim-v1` but knows
  nothing of `ew-theme-std-v1`; std and pool now meet in one function untested together.

R6: `atx-impl/tests/strategy_spo_v3_test.cpp` auto-merged; root's fix `d5e5510a` (4 lines) is kept and the union check
holds (R6 242 lines, root 4 lines).

### Build (`scripts/research-build.ps1 -Preset equity-dev`)

| tag | source | result |
|---|---|---|
| **v8-5** | `37e84d81` clean (DirtyEntries 0) | ok, exit 0, 51.4 s, 16 TUs, 6 links; no `/W4 /WX` slip; `ConfiguredProvenance` stays `5c6efcd4` (no reconfigure) |

Targets: atx-equity-strategy-ic, atx-equity-strategy-targets, atx-equity-strategy-risk, atx-impl-strategy-target-tests,
atx-impl-tests. Executables (receipt `build-equity/mega-v8-5-receipt.json`):
- ic `4b4ffb7b0bc24e7735f9608548ee4b60ff32cfe5fecfb5d006976342a556d68f` (relinked on the new atx-impl-core);
- targets `0d0a6921252dac26e30b95f0ef3847728502387a178b817ceae1cf128a40a8f1`;
- risk `f45e887041a8e66f8283c38e0a7ebb53607778f78df66202f056315997100392`;
- target-tests `114cc971961d6716551c01e44f928c3cafb5cf0ad49461eacffebbbe3d26b185`, impl-tests
  `70906534b8f5817279ff5704aa29be725c1eea15a4bd9dd484bb56fc84fb11ba`.

### Tests

C++ (v8-5):

| exe | filter | result |
|---|---|---|
| atx-impl-strategy-target-tests | `Spo*:NavV7Hook.*:HoldBand.*:AdvHold.*` | 62 run: 61 passed, 1 skipped: HoldBand 10, AdvHold 6, NavV7Hook 11, SpoSolver 7, SpoAlpha 1, SpoCalibration 2, SpoRisk 2, SpoHook 11, SpoTripwire 2, SpoPin 2, SpoV3 8 (5 + R6's 3 E-26 tests) |
| atx-impl-tests | same | 62 run: 61 passed, 1 skipped |
| atx-impl-strategy-target-tests | whole exe | 223 run: 222 passed, 1 skipped |

- The skip is `SpoV3.V1AndV2DigestsUnchanged` (v2 placeholder 0). SpoPin v1 pins pass unchanged
  (`weights=0xda6b6871e7e267c5`, `replay=0xaabdbb72f99a6e13`). v8-5 prints `[spo-v3-pin] v2
  weights=0xb039820b40d5cf24 (40 diagnostics rows) replay=0xd24b61721a7c698c (30 days)`, the same as the R6 part 2
  head in part A (E-26 left the flag-free v2 path unchanged); still **not** pins.
- spo-v2 pin capture: **not done, no worktree-free way**. The digest is computed by test code linked against the
  library, and the only library this dispatch may build is post-R6. `research-build.ps1` keeps one `bin` per build
  dir (no per-tag archive), so no pre-R6 test exe survives (v8-4a's was relinked at v8-4d). The skip stays.

Python (`pytest -q -p no:cacheprovider`, `ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin`, v8-5 exes):

| paths | result | s |
|---|---|---|
| atx-impl/strategies | 163 passed | 34 |
| atx-engine/tools | 240 passed (6 subtests) = part A 214 + `test_era_pool` 17 + `test_research_fields_v8_quarters` 9 | 96 |
| atx-impl/tools | 452 passed, 2 skipped = part A 314 + `test_nav_summ_pool` 8 + `test_fit_composition_weights_pool` 16 + `test_era_data_audit` 18 + `test_mega_report_v8` 38 + `test_mega_report_v8_render` 58 | 109 |
| scripts/tests | 121 passed, 3 skipped = part A 97 + `test_research_cycle_roles` 24 | 42 |

- tiny_world: `test_cycle_e2e_goldens_redundant_copy_and_idempotent_rerun` PASSED (re-checked alone with `-rs -v`:
  5/5 in `test_cycle_e2e.py`); **no golden moved**, `git status` clean after every set.

### Identities: none run

Planned argv, prepared read-only for the next integrator (nothing executed):
- i1: the argv of `v8-i3p4-a-run/receipt.json` (v7.1 NAV cell) with `--output build-equity/v8-i4b-i1-nav`, compare
  the 12 files to `mega-nav-v71u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247`. **An accepted holdings export exists**:
  `build-equity/v7-w4-holdings` (f64, `atx.nav-holdings/v2`, from `v7-w4-nav-on-run`: v6.1 combined
  `mega-v61w-train-ew-1`, fields-v7, source `8921dc2c`). So the holdings identity is old-vs-new: rerun that receipt's
  argv with the v8-5 exe into new directories, compare the NAV dir to `v7-w4-nav-on` and the export to
  `v7-w4-holdings` (`holdings.f64`, `holdings_index.json`, `holdings_days.csv`, `manifest.json`); then the csv layout
  on the v7.1 argv (`v7-l3-holdings`, the csv export, no longer holds its `holdings.csv`: only `holdings_days.csv` and
  the manifest). Both argv carry `--max-bytes 1073741824`: watch for a `NavHolding` +16 B refusal.
- i2-i7: argv as in the dispatch and the lane reports (R-4 `--hold-band 0`, R-5 `--adv-hold-q 1e9`, R-1
  `composition_rules.py identity-weights` then the `mega-v71w-train-ew-run1` argv, H-3 warm u pass: H-3 offers no
  read-only cache mode, so point `--candidate-cache` at `v8-i3p4-d-w4-cache` and compare to `v8-i3p4-d-w4-u`, C-3
  step 2 with `--reuse v8-i3p4-c-fields2` expecting reused 49 / computed 14 (F-B moved the `sec` group fingerprint,
  F-3 report), spo-v2 cell argv from the W1b report step 4, not located yet).
- i8 (E-1): satisfied by this dispatch's last and only build: the v8-5 receipt carries `Tag` v8-5, `Script`
  `scripts/research-build.ps1`, `BuildDir` `C:\atx-wt\pool-2\build-equity` and `Executables` (the three exe SHAs
  above). **PASS.**

### Fixes

None. No compile or test failure; no fix commit.

### Hidden-data record

No real-data run. Read: lane reports, receipts (argv only), the two holdings manifests' top-level `files` / `format` /
`schema` keys (v7 TRAIN exports) and directory names under `build-equity` filtered to exclude validation, val,
holdout, 2023-2024, 2024 and 2025. Tests used synthetic fixtures and tiny_world. **No disclosure.**

### Not started (owner stop)

- Identities i1-i7 (above).
- REPORT's E-4 step 3 re-render (not in this dispatch either).

### Open items

- spo-v2 pin: needs a pre-R6 build (another tree); the test skips.
- H1 x R-1: the pooled fit does not refuse or support `ew-theme-std-v1` (H1 open risk; a PM decision before any
  pooled read with a v8 composition).
- Carried: `ConfigJsonNotInDiscoverDigest` (known), `ParallelLockstepGrid` group unconfigured, build provenance records
  `5c6efcd4` for v8-5 (source `37e84d81`).

## integration 5 part A (2026-09-30)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `d22c8e99` (clean). Tag prefix v8-6.
Scope: four merges by SHA, three integration edits, build, tests. No identity run and no real-data run.

### Merges (in order, each `--no-ff` by SHA)

| # | lane (tasks) | lane SHA | merge | conflicts |
|---|---|---|---|---|
| 1 | FIX-C (C-1..C-7, C-9..C-13, E-33) | `d0d081f2` | `d3b9513d` | none (20 files) |
| 2 | FIX-AB (A-1..A-4, B-1..B-4) | `dd677d3b` | `d3e6d855` | none (24 files) |
| 3 | R45 (E-25 `nav --label-role`, sessions 1-3) | `9cc0d3cb` | `4b57a6af` | 4 hunks, below |
| 4 | A2 (follow-up tasks 1-4, v8 specs and templates) | `79440cfa` | `67c5aff0` | 7 hunks, below |

R45 x FIX-AB (both sides kept, all additive):
- `strategy_nav_replay.hpp`: the `execution` paragraph is FIX-AB's A-3 warm-start text followed by R45's
  `execution.label_role` paragraph; the grid paragraph names both `leverage_groups` (A-1) and `label_role_sha256`.
- `strategy_nav_replay.cpp`: FIX-AB's `score_begin_gross` and R45's `label_record` both kept; `publish_nav` writes
  the A-3 `warm_start` block (with `score_begin_gross_leverage`) and then R45's `label_role`.
- Auto-merged and read: A-1's `leverage_groups` copies each variant's `Ctx` into its group, so R45's `ctx.mark` (set
  in the shared ctx loop) reaches every group; A-3's inert-warm-start check sits in `run_books`, R45's marks in MARK.

A2 x FIX-C / H1 (A2's base was `5c6efcd4`, so it saw neither FIX-C nor H1):
- `research_add_alpha.py`: docstring = A2's text plus FIX-C's `summ.origin` sentence; `derive_spec` takes A2's
  `(lib, parent_members, ..., fields_dir)` plus FIX-C's `origin`; the call passes both (`origin = wave_origin` of the
  new members, re-screens included).
- `research_gc.py`: A2's `users()` (store-base rule) now compares by FIX-C's `path_key` (C-12), signature
  `users(root, rel, keep)`; the store base is `os.path.dirname` of the key, shown root-relative.
- `research_cycle.py`: docstring keeps H1's `roles` entry then A2's `templates` / `fields pin` / `ic.w_flags`; the w
  pass keeps H1's `self.weights_name` with A2's E-28 `w_flags`; `lock()` runs H1's `roles` branch first, then A2's
  template branch and as-built fields pin.

### Integration edits

| commit | lane | file | what |
|---|---|---|---|
| `7c77fac2` | A2 (per FIX-C cross-lane) | `scripts/specs/v8/base-lo1.json`, `base-lo3.json`, `scripts/tests/test_research_spec.py` | `summ.origin "prior"` on both base specs (every v7.1 member is `prior` in the registry; the library draft's v8.0 / v8.1 members are `prior` too). `--origin` is not in either `summ.extra`; both verdict specs already name `summ.ledger` (`build-equity/trials.jsonl`). Templates inherit the summ block (every template plans with `--origin prior`). Before the edit `test_every_v8_spec_loads_and_plans[base-b0c.json]` failed on "set summ.origin" (the first failure under `-x`). The ruled-settings test pins the three rules |
| `d833b25f` | FIX-AB B-3 into R45 | `atx-impl/src/strategy_target_replay.cpp`, `_detail.hpp`, `atx-impl/tests/strategy_live_test.cpp` | `check_label_manifests(role, label, role_path)` calls `refuse_delisting_returns_signal_role(role.dump(), role_path)` right after the JSON-object test, before the seal and membership rules; applied to `role` only, never to `label`; the refusal keeps B-3's own message. New test `NavLabelRole.RefusesADelistingReturnsSignalRole`; `AdmitsOnlyTheDeclaredDelistingClearing` still runs a label role that declares `returns_applied true` (the label is not checked) |
| `229f8e78` | FIX-AB A-3 x R45 test | `atx-impl/tests/strategy_live_test.cpp` | `NavLabelRole.SameRoleIsIdentity` failed on v8-6: its 10-session warm start on PinBench's score_begin 20 had no price-risk exposures (126 return pairs), so A-3 refused the plain run ("the warm start built no book"). The test now uses `PinBench(140)`, the fixture change FIX-AB made to `StrategyLive.RecipePinBackwardCompatibleWithNewConstructionFields`. No production code changed |

Pins (edit 3): no pinned source set moved after the merges. `dsl_vm_sources_sha256` root `f24cfbbe...55c` (integration
4 part A) -> FIX-AB's re-pin `ad6c4ca710606ab2602f9bb27bc2cd593e46fd104c308c2d1198651d96113d62` (lane commit
`dd9cbcf2`, B-3's `strategy_data.hpp/.cpp`); `git diff d22c8e99 HEAD` over the 33 listed paths touches only those two
files; `StrategyIcRunner.VmSourcesPinnedToSemanticsVersion` passes on v8-6, so FIX-AB's digest is confirmed by a
build. `ic_result_sources` (10 paths, `e7a40331...579a`): no listed file changed; `IcSourcesPinnedToSemanticsVersion`
passes. No re-pin by root, no semantics bump. FactoryOos (E-22a) pins hold (factory-tests 377/377).

### Builds (`scripts/research-build.ps1 -Preset equity-dev`)

| tag | source | result |
|---|---|---|
| v8-6 | `d833b25f` clean | ok, exit 0, 121 s, 45 TUs, 14 links, 3 jobs; no `/W4 /WX` finding (FIX-AB, R45 sessions 1-3 and the B-3 wiring compiled first time) |
| **v8-6a** | `229f8e78` clean | ok, exit 0, 21 s, 2 TUs, 2 links (target-tests, impl-tests) |

Targets of v8-6: atx-equity-strategy-ic, -targets, -risk, atx-engine-data-tests, -combine-tests, -factory-tests,
-book-tests, atx-impl-strategy-ic-tests, -target-tests, -strategy-tests, atx-impl-tests. `ConfiguredProvenance` stays
`5c6efcd4` (no reconfigure). Executables: ic `e839aebd40fe1c3a770c89de7799c6e6806f1ef68f0b7f1b2afceaa93ea4428b`,
targets `5fa4913e07103d89e6b44d35e7221be804526c6ce739011d5f615e3b3d913bf1`, risk
`4abbff6e7219f182cd2f9ccbb10e686bf87587f049bbb0ca5205f0a5d442aca3` (v8-6, unchanged by v8-6a); target-tests
`d1eaaba4...0a721`, impl-tests `79990590...a52ab` (v8-6a).

### Tests

Lane filters ("how root verifies"):

| exe (build) | filter | result |
|---|---|---|
| strategy-ic-tests (v8-6) | `MarginalIc.*:CompositionV8.*` (B-2) | 11/11 |
| strategy-tests (v8-6) | `StrategyResearchRole.*` (B-3) | 3/3 |
| strategy-ic-tests (v8-6) | `StrategyIcRunner.DelistingReturnsRoleIsRefusedBeforeAnyPayloadOrOutput:StrategyIcRunner.VmSourcesPinnedToSemanticsVersion:MarginalIc.DelistingReturnsRoleIsRefused` (B-3) | 3/3 |
| strategy-ic-tests (v8-6) | `FieldCaps.*:StrategyIcRunner.*` (B-4) | 54/54 |
| strategy-target-tests (v8-6) | `SpoV3.*:SpoPin.*:NavV7Hook.SideFilesExcludeWarmUp:SpoHook.*` (A-2) | 24 run: 23 passed, 1 skipped (SpoV3 v2 placeholder) |
| strategy-target-tests (v8-6) | `NavWarmStart.*:StrategyLive.RecipePinBackwardCompatibleWithNewConstructionFields` (A-3) | 6/6 |
| strategy-target-tests (v8-6) | `SpoV3.CriterionReadsTheTradedBook:SpoV3.ReportsTrackingErrorAndShareAtTradeLimit` (A-4) | 2/2 |
| strategy-target-tests (v8-6) | `AdvHold.GridCapsEachVariantAtItsOwnLeverage:ConstructionGrid.*:HoldBand.GridSharesTheBandAndOneCadence:NavTimers.*` (A-1) | 7/7 |
| strategy-target-tests (v8-6) | `NavLabelRole.*` (R45) | 6 run: 5 passed, **1 failed** `SameRoleIsIdentity` (fixed `229f8e78`) |
| strategy-target-tests (v8-6a) | `NavLabelRole.*` (R45: the five + the integration test) | 6/6 |
| strategy-target-tests (v8-6a) | `NavV6.OrderBasisTargetAndExitRateOneAreBitIdentical` (flag-off recipe SHAs) | 1/1 |
| strategy-target-tests (v8-6a) | `NavLabelRole.*:NavV6.*:StrategyLive.*:HoldBand.*:AdvHold.*` (R45's `-R` set) | 55/55 |
| strategy-target-tests (v8-6a) | `Spo*:NavV7Hook.*:HoldBand.*:AdvHold.*` (integration 4 part B set) | 65 run: 64 passed, 1 skipped |
| impl-tests (v8-6a) | `NavLabelRole.*` | 6/6 |

Whole executables:

| exe | build | result |
|---|---|---|
| atx-engine-data-tests | v8-6 | 299 run: 285 passed, 14 skipped (+1: B-3) |
| atx-engine-combine-tests | v8-6 | 220/220 |
| atx-engine-factory-tests | v8-6 | 377/377 |
| atx-engine-book-tests | v8-6 | 147/147 |
| atx-impl-strategy-ic-tests | v8-6 | 105/105 (+4: B-2, B-3 x2, B-4) |
| atx-impl-strategy-tests | v8-6 | 46/46 (+1: B-3) |
| atx-impl-strategy-target-tests | v8-6a | 234 run: 233 passed, 1 skipped (part B 223 + FIX-AB 5 + R45 5 + integration 1) |
| atx-impl-tests | v8-6 | 965 run: 956 passed, 7 skipped, 2 failed (`ConfigJsonNotInDiscoverDigest`, `SameRoleIsIdentity`) |
| atx-impl-tests | v8-6a | 965 run: **957 passed, 7 skipped, 1 failed: the known `AtxImplProvenanceDigest.ConfigJsonNotInDiscoverDigest`** |

impl-tests skips (all environment-dependent, none in a merged lane's tests): Alpha101Orats x2, AtxImplDiscover.W6,
SingleAlphaCapacity (ATX_ALPHA101_PANEL), FundamentalZoo (opt-in), SpoV3.V1AndV2DigestsUnchanged (placeholder),
TrialLedgerRepository.ExistingCp14Ledger_StillVerifies ("not run from the repository root": run here from
`build-equity/bin`, the likely difference from part A's 6).

SpoPin v1 pins pass unchanged (`weights=0xda6b6871e7e267c5`, `replay=0xaabdbb72f99a6e13`); v8-6a prints `[spo-v3-pin] v2
weights=0xb039820b40d5cf24 (40 diagnostics rows) replay=0xd24b61721a7c698c (30 days)`, the same as v8-4e and v8-5 (A-2
changes only warm-start runs). **spo-v2 pin: not captured.** The skip reason is "spo-v2 digests unpinned (placeholder
0): capture them on the pre-R6 tree with this file and strategy_spo_digest.hpp (protocol at the top), then pin": it
needs a pre-R6 build, so the skip stays (part C, item 7).

Python (`pytest -q -p no:cacheprovider`, `ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin`, v8-6/v8-6a exes):

| paths | result |
|---|---|
| atx-impl/strategies | 163 passed |
| atx-impl/strategies/test_generate_library.py with `ATX_V71_PLAN_JSON=build-equity/v8-i3-plan-v71.json` (E-19) | 9 passed |
| atx-engine/tools | 241 passed (6 subtests) = part B 240 + B-1 `test_reuse_pins_the_session_calendar` |
| atx-impl/tools | 465 passed, 2 skipped (`ATX_EQUITY_TARGETS_EXE`, `ATX_EQUITY_ROOT` unset) |
| `test_exposures_export.py` with `ATX_EQUITY_TARGETS_EXE` = v8-6 targets | 3 passed |
| scripts/tests (before edit 2) | `test_every_v8_spec_loads_and_plans[base-b0c.json]` failed first (`-x`): FIX-C's C-2 refusal, fixed by `7c77fac2` |
| scripts/tests (spec + cycle tests after edit 2: research_spec, research_cycle, cycle_scoring, cycle_resume, roles, label_role) | 154 passed, 3 skipped |
| scripts/tests (whole, v8-6a) | 163 passed, 3 skipped (the three RESEARCH_CYCLE_LIVE_ROOT tests) |

- tiny_world: `test_cycle_e2e.py` 5/5 (`-rs -v`, live on the v8-6 exes), including
  `test_cycle_e2e_goldens_redundant_copy_and_idempotent_rerun`: **no golden moved**; `git status` clean after every set.
- A2's `test_research_spec.py` autouse fixture `label_role_input` is now a no-op (R45's `INPUT_KEYS` has the key); left
  in place.

### Hidden-data record

No real-data run. Read: lane reports, the alpha registry and library definitions (code), build receipts, and
integration 3's saved `--plan-only` JSON for 2020-2022 through the E-19 test (DSL SHAs, lookbacks, slot and node
counts; no statistic). Tests used synthetic fixtures and tiny_world. Nothing dated 2024-01-01 or later was opened.
**No disclosure.**

### Open items

- spo-v2 pin: the test's skip needs a pre-R6 build (part C item 7).
- Part C identities (none run here): R45's flag-absent and `--label-role` = `--role` NAV identities, A-2/A-3 change
  warm-start outputs only (no accepted warm-start spo cell), A-4 changes the spo-v3 CSV/JSON shape.
- B0c: A-3 refuses a warm start that leaves every book flat on `score_begin`; B0c's `--warm-start-sessions 60` must
  build a book (FIX-AB measured `score_begin` 399 on the 3-year TRAIN role, forecasts from row 315).
- FIX-C cross-lane notes for H3 (campaign writer `campaign_line`, chain=True) and any `holdout_gate.py` caller
  (`--ledger` required): nothing in this part.
- A2 open risks 6 (attempt-1 `reference_combined`) and 7 (template rename discipline) stand; H1 x R-1 pooled fit
  (`ew-theme-std-v1`) carried.
- Carried: `ConfigJsonNotInDiscoverDigest` (known), `ParallelLockstepGrid` group unconfigured, build provenance records
  `5c6efcd4` for v8-6 / v8-6a (sources `d833b25f` / `229f8e78`).

## integration 5 part B (2026-09-30)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `fd2ff7a8` (clean). Tag prefix v8-7.
Scope: merge H3 by SHA, build, own build-fix pass, mine tests and fixture acceptance, default-off golden digests, the
FIX-C / Ruling E-33 campaign-line wiring, wider suites. No identity run and no real-data run.

### Merge

| lane (tasks) | lane SHA | merge | conflicts |
|---|---|---|---|
| H3 (H-3 parts 2 and 3: engine signal-fitness search path, research IC fitness, `OpCatalogCfg`; research-role loader, `atx-equity-strategy-mine`, rule `mined-v1`) | `339c07b1` | `e2bb716b` | none (26 files, +4125 / -227) |

Merge base `95859cc9` (H3's own merge of integration 3), so part 1 (`11ff84c3`: `role_panel`, `ResearchIcCache`
accessors, `instrument_rungs`) was on root already. `strategy_ic_signal_cache.cpp` did not move: root's 33-path
`dsl_vm_sources` list with FIX-AB's digest `ad6c4ca7...3d62` stands, so H3's "tripwire fails until root re-pins" note is
stale. No path of the merge diff (`fd2ff7a8..HEAD`) is in the pinned list; `StrategyIcRunner.VmSourcesPinnedToSemanticsVersion`
and `IcSourcesPinnedToSemanticsVersion` pass on v8-7a. No re-pin, no semantics bump.

### Builds (`scripts/research-build.ps1 -Preset equity-dev`)

| tag | source | targets | result |
|---|---|---|---|
| v8-7 | `e2bb716b` clean | atx-engine-factory-tests, atx-impl-strategy-mine-tests, atx-equity-strategy-mine | ok, exit 0, 154 s, 50 TUs, 5 links, 4 jobs (reconfigure: CMakeLists changed; provenance now `e2bb716b`) |
| **v8-7a** | `ccb66a87` clean | atx-impl-strategy-mine-tests, atx-equity-strategy-mine, atx-impl-strategy-ic-tests, -strategy-tests, -strategy-target-tests, atx-impl-tests, atx-equity-strategy-ic, -targets, -risk | ok, exit 0, 77 s, 13 TUs, 11 links, 3 jobs |

**H3 compiled first time under `/W4 /WX`** (it was never compiled in the lane): every new TU of parts 2 and 3
(`research_ic_fitness.cpp`, `search_driver.cpp`, `op_catalog.cpp`, `strategy_mine*.cpp`, `strategy_research_role.cpp`,
`equity_strategy_mine.cpp`, both test files). No build fix was needed; none of the report's "first things to check"
fired. Executables: v8-7 factory-tests `c4a70660...39df`, mine-tests `da43c229...9ca7`, mine `53903a75...ba08`; v8-7a
mine-tests `9f20972c...8947`, mine `ac463657...8d03`, ic `39bc5f33...ce2b`, targets `474fabb0...b3d1`, risk
`15fb74d5...c56a`, ic-tests `cea55898...bb30`, strategy-tests `2e3181f4...8dd8c`, target-tests `575539e2...7f71`,
impl-tests `25bde20d...bc15b`. The engine library is the same in v8-7 and v8-7a (the fix touches atx-impl only).

### Integration edit (FIX-C cross-lane note: the mining verb must write `campaign_line`)

| commit | lane | files | what |
|---|---|---|---|
| `ccb66a87` | H3 x FIX-C (Ruling E-33) | `atx-impl/src/strategy_mine.{cpp,hpp}`, `atx-impl/tests/strategy_mine_test.cpp`, `scripts/research_ledger.py`, `scripts/research_cycle.py`, `scripts/tests/test_research_ledger.py` | see below |

- Why: H3's `ledger_line.json` contradicted E-33. It had `count` = new records, no `registry.count`
  (`campaign_registry_count` would read 0), and the trial_id rule `(kind, campaign_id, head)`, where `campaign_line`
  uses `(kind, head)`. So the verb's line and the writer's line for the same campaign had different identities,
  which risked a double campaign line.
- C++ (smallest change): `ledger_line.json` is now exactly `backtest_integrity.campaign_line(campaign_id, registry
  path, head, n_raw, research_window_id)`:
  - schema, kind `mining-campaign`, `count` 0, `campaign`, origin `mined`, `window_id`;
  - `registry {path (with / separators), chain_head, count = n_raw}`;
  - `trial_id` = sha256(`["mining-campaign",head]`)[:16].
  - `campaign.json` is unchanged.
- Python (the wiring): a new verb `research_cycle.py ledger-campaign --ledger L --campaign DIR [--date D]`
  (`research_ledger.campaign_main`):
  - it rebuilds the line from `DIR/campaign.json` through `campaign_line`;
  - it refuses an incomplete campaign, a line that differs from the verb's `ledger_line.json` (the error names the
    keys), a missing output or a broken chain (exit 2, nothing appended);
  - otherwise it appends the line with `chain=True`. A rerun with the same registry head is "already present".
- The C++ verb cannot append to the cycle ledger itself in integration: the chained JSONL append (legacy fold, C-6
  chain verification, `check_line`) lives only in Python. So the verb writes the line and the ledger verb appends it.
- Tests:
  - `StrategyMineCampaign.PromotesThePlantedSignalsOnlyInFiveSeeds` now pins the full line (JSON equality,
    5 seeds), in place of the old `count == new_records` check.
  - New `test_ledger_campaign_appends_the_mine_verbs_campaign_line`. It checks:
    - the appended line equals the verb's line plus `prev_sha256`, and equals `campaign_line(...)`;
    - `trial_counts` [1, 0], `campaign_registry_count` 81 and `cycle_n` are unchanged;
    - a rerun is skipped;
    - the pre-E-33 form (`count` 81), an incomplete campaign and an absent directory are each refused, with the
      ledger bytes unchanged.

### Tests

| exe / suite | build | result |
|---|---|---|
| atx-impl-strategy-mine-tests (whole) | v8-7 | 18/18 |
| atx-impl-strategy-mine-tests (whole) | v8-7a | **18/18** (with the E-33 line pinned) |
| atx-engine-factory-tests (whole) | v8-7 | **387/387** (377 + H3's 10 through the factory glob) |
| atx-engine-factory-tests `NsgaSearch.*:FactoryFidelity*:SignalFitness*:OpCatalogCfgTest.*:ResearchIc*` | v8-7 | 44/44 |
| atx-impl-strategy-ic-tests | v8-7a | 105/105 (both source pins included) |
| atx-impl-strategy-tests | v8-7a | 46/46 |
| atx-impl-strategy-target-tests | v8-7a | 234 run: 233 passed, 1 skipped (SpoV3 v2 placeholder) |
| atx-impl-tests | v8-7a | 973 run: **965 passed, 7 skipped, 1 failed: the known `ConfigJsonNotInDiscoverDigest`** (+8 = `strategy_mine_test.cpp` through the glob; the skips are part A's 7) |
| scripts/tests (whole, `ATX_EQUITY_BIN` = v8-7a bin) | v8-7a | 164 passed, 3 skipped (part A 163 + the new test; the 3 RESEARCH_CYCLE_LIVE_ROOT skips); tiny_world `test_cycle_e2e.py` in it: no golden moved, `git status` clean after |
| atx-impl/tools (whole) | - | 465 passed, 2 skipped (as part A) |

**Default-off golden digests (1 and 4 workers): hold.**
- `SignalFitnessDefaults.ImplicitDefaultsKeepTheGoldenDigest` and
  `SignalFitnessDefaults.ExplicitDefaultsKeepTheGoldenDigestAtEveryWorkerCount` (the 1- and 4-worker runs) pass in
  mine-tests (v8-7 and v8-7a) and factory-tests (v8-7).
- `NsgaSearch.ScalarRaw_ReproducesGoldenDigest` passes.
- `kGoldenDigest = 0x889874a3b9b29c55` is the same in `factory_nsga_search_test.cpp` (untouched by the merge) and
  `factory_signal_fitness_test.cpp`. No pin was edited.

**Fixture acceptance (`StrategyMineCampaign.*`, the report's three tests): pass.**

| test | v8-7 | v8-7a | what it shows |
|---|---|---|---|
| `PromotesThePlantedSignalsOnlyInFiveSeeds` | 9.3 s | 10.0 s | over seeds 1..5: p1, p2 and p3 each admitted; no admitted member without a planted field; `rank(copy)` evaluated with f1 >= hurdle and f2 < hurdle; n_raw = evaluated + screen-rejected + racing-rejected; failed 0 |
| `SameSeedSameChainHeadAtOneAndFourWorkers` | 8.8 s | 8.4 s | the same head, trials.csv bytes and members at 1, 1 and 4 workers; a reuse without `--registry-head` is refused and writes nothing; with the head, 0 new records |
| `RefusesSealedRolesAndWindowsPastTrain` | 0.7 s | 0.7 s | the 2024-01-02 role is refused before any payload; the confirm end 2024-01-02 is refused |

The whole mine binary takes 19 s in Debug. The report estimated 20 to 60 s.

### Hidden-data record

No real-data run. Read: the H-3 and FIX-C reports, sources, build receipts and logs. The tests used synthetic
fixtures and tiny_world. The mine fixture builds its own synthetic role (and a synthetic sealed role to 2024-01-02
for the refusal test); none of it is data. Nothing dated 2024-01-01 or later was opened. **No disclosure.**

### Open items

- E-33 registry count: `registry.count` = `n_raw` (the mined-v1 Bonferroni count the hurdle used). When several
  campaigns share one registry (reopened with `--registry-head`), `n_raw` is cumulative, so
  `campaign_registry_count` over their lines counts the earlier campaigns again.
  - The owner should rule whether the count is `n_raw` or `new_records`.
  - A campaign that records 0 trials has `n_raw` 0, and `campaign_line` refuses it.
  - Binds only under OD-7.
- Nothing appends campaign lines automatically: `ledger-campaign` is a manual verb, and the cycle has no mining step
  (no campaign runs in v8).
- H3 open risks carried, unmeasured on real fields:
  - the stage-2 literature ops and deny list;
  - the `mine_working_bytes` admission estimate (8 VM slots per cell; measure before OD-7);
  - the IC runner does not use `ResearchRole` yet (follow-up).
- H3's interpretations (confirm on the marginal t: this is Ruling E-32; rho before confirm; an undefined rho does not
  block) stand as coded.
- Part C item 5 (H3 warm u pass, 48 of 48 cache hits) was not run (part C).
- Carried: `ConfigJsonNotInDiscoverDigest` (known), the spo-v2 pin (part C item 7), and the `ParallelLockstepGrid`
  group (unconfigured). Build provenance is `e2bb716b` for v8-7 and v8-7a (v8-7a source `ccb66a87`).

## integration 5 part C: identities (2026-09-30)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `a7673e0a` (clean). Tag prefix v8-8.
Scope: the eight identities of `task-INT5-brief.md` part C on the existing 3-year TRAIN roles (2020-2022, lo1 and lo3)
and caches under `build-equity/`, rulings PM3-5 and PM3-7, then the spo-v2 pin. No merge.

### Executables (verified before any run)

- `mega-v8-7a-receipt.json`: Source `ccb66a87`, DirtyEntries 0, ExitCode 0. The on-disk SHA-256 of every strategy exe
  equals the receipt: targets `474fabb02ef61f7254c04cedf3444d7d85e61aff8cba974736866a8c80f3b7d1`, ic
  `39bc5f331fd3b1fb6f5acf55fe153cfe13b1e994533e55f6ed2bf80d73fbce2b`, risk `15fb74d5...c56a`, mine `ac463657...8d03`.
- `git diff --stat ccb66a87 a7673e0a`: five files, all under `.superpowers/` (docs). No source changed after the build,
  so no build ran before the identities.
- The legacy `atx-equity-strategy.exe` (2026-09-26) is used by no identity and was not rebuilt.

### Runs

Every run went through `scripts/run_bounded_research.py`, one at a time, on a clean tree at source `a7673e0a`. Every
receipt says `git: clean in the code pathspec`, outcome completed, exit 0, with no refusal on the memory floor. Outputs
are under `build-equity/v8-i5c-*`. Caps follow the dispatch: 180 s / 1,536 MiB, and IC u pass 300 s / 2,560 MiB. The
fields build (i6) got 600 s / 2,560 MiB: the same argv measured 189.2 s and 958 MiB in integration 3 (`v8-i3p4-c-run2`),
and the builder caps itself at `--max-seconds 580 --max-rss-mib 2048`.

| id | receipt dir | receipt.json SHA-256 | exe | caps s / MiB / free | wall s | peak MiB |
|---|---|---|---|---|---|---|
| 1a | `v8-i5c-i1-run` | `9824af388a9848b2e25e935fb1bb395708f80d6a2468c6721f53d86b4e40a7d9` | targets | 180 / 1,536 / 512 | 35.8 | 359 |
| 1b | `v8-i5c-i1w4-run` | `1bba1f133dae142199b255b1bb12c8f64957ad3ef874d46b4909c4a8ef202604` | targets | 180 / 1,536 / 512 | 32.7 | 360 |
| 2 | `v8-i5c-i2-run` | `b3bb3bd8e30e6226db034bdb8ed03607a58bbe7093dcb288a55abc3e446f375e` | targets | 180 / 1,536 / 512 | 34.8 | 359 |
| 3 | `v8-i5c-i3-run` | `1810963490a5dd42a84175f7ac999edde3e0f5493a12c836e928ec6de3430af5` | targets | 180 / 1,536 / 512 | 41.7 | 359 |
| 4 (weights) | `v8-i5c-i4w-run` | `aa2ccb0298afd061db8f2358804e593ec915397a2f1cee1d47a25e5204696181` | python | 180 / 1,536 / 512 | 0.8 | 34 |
| 4 (w pass) | `v8-i5c-i4-run` | `513d63e4dd62f06466a702586ce8244f122d5edae67ff40614245d4d6b217a0d` | ic | 180 / 1,536 / 512 | 35.9 | 506 |
| 5 | `v8-i5c-i5-run` | `87547a27f5fd53e431e1fe8515ad2447eb5912787f160015bafeb5e557e55d9a` | ic | 300 / 2,560 / 512 | 35.7 | 506 |
| 6 | `v8-i5c-i6-run` | `a1963e9cd4cbe72783025ffee00b6890fd36983adbcb135b36e6ebb9bf64e9f8` | python | 600 / 2,560 / 512 | 43.4 | 477 |
| 7 | `v8-i5c-i7-run` | `68bfea4d756da0eb763f2d0be58f348bc0ecaf2492af307a713e8bb91ac41928` | targets | 180 / 1,536 / 512 | 98.0 | 359 |
| 8 | `v8-i5c-i8-run` | `42a395701107034ab8b1ee81d4965016ed4a8b6fcb8488c8a3bec57d98eed74e` | targets | 180 / 1,536 / 512 | 33.5 | 464 |

Runner prefix: `python scripts/run_bounded_research.py --output build-equity/v8-i5c-<id>-run --seconds S --max-rss-mib M
--min-free-mib 512 --`. The `targets` exe is `C:\atx-wt\pool-2\build-equity\bin\atx-equity-strategy-targets.exe`.

Base argv V71: the accepted v7.1 cell, `mega-nav-v71u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247-run`:

```
targets nav --combined build-equity/mega-v71w-train-ew-1/train_combined.json --combined-sha256 bf1af1276fd4d2cb4bd835f97b27897596f74b5855dfe7cac05c61dac2486fd5 --role build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json --role-sha256 3e79978a858cbf6b723ff7a896d56814f7505dde11b805c30c5b909783ebb809 --fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9/manifest.json --fields-sha256 8fd00e9f44b475116f483e133c03fe031618390060b8374180278f1cd7b8769b --output <OUT> --rule aim-partial-v5 --cadence 1 --trade-fraction .05 --dust-multiple .1 --aim-leverage 1.247 --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 --max-bytes 1073741824 --order-basis delta --exit-rate .05 --locate-in-aim --liquidity-cache
```

### 1. NAV with every new flag absent, and holdings against `v7-w4-holdings`: PASS

- 1a. V71 with `<OUT>` = `build-equity/v8-i5c-i1-nav`.
  - **12 of 12 files byte-identical** to the accepted cell: 5 daily, 5 events, `recipe.json` `b956bbcc...34fa`,
    `summary.json` `5b109a70...170a`. The primary daily (`modeled-1bn-stale5-v1+swap-fin-v1`) is `fbec452e...f5d4`.
  - `stdout.log` `813ce59c...` equals integrations 2 and 3.
- 1b. The argv of `v7-w4-nav-on-run`, with outputs `build-equity/v8-i5c-i1w4-nav` and `--emit-holdings
  build-equity/v8-i5c-i1w4-holdings`:

  ```
  targets nav --combined build-equity/mega-v61w-train-ew-1/train_combined.json --combined-sha256 62bc30a3bf1ee047c200e35064f8cfe18c089a77adb3f651e4615b3c05d1c6c0 --role build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json --role-sha256 3e79978a858cbf6b723ff7a896d56814f7505dde11b805c30c5b909783ebb809 --fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v7/manifest.json --fields-sha256 1d1fa87a00d519bcf08fbec83f3fd17029e23650a98a9af26e99ddb1f1a73ee1 --rule aim-partial-v5 --cadence 1 --trade-fraction .05 --dust-multiple .1 --aim-leverage 1.247 --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 --max-bytes 1073741824 --order-basis delta --exit-rate .05 --locate-in-aim --liquidity-cache --output build-equity/v8-i5c-i1w4-nav --emit-holdings build-equity/v8-i5c-i1w4-holdings
  ```

  - NAV directory: **12 of 12 byte-identical** to `v7-w4-nav-on` (recipe `2324dd91...0858`, summary `998eca31...fea3`).
  - Holdings: **4 of 4 byte-identical** to `v7-w4-holdings`: `holdings.f64` `ce5523c6...eff4`, `holdings_index.json`
    `aef15691...0fde1`, `holdings_days.csv` `43d9d2fe...57`, `manifest.json` `532044d5...e141`.
  - The `NavHolding` +16 B reserve (E-16) did not refuse at `--max-bytes 1073741824`.

### 2. `--hold-band 0` equals 1: PASS

V71 plus `--hold-band 0`, `<OUT>` = `build-equity/v8-i5c-i2-nav`. **12 of 12 files byte-identical** to 1a, `recipe.json`
and `summary.json` included. b = 0 writes no key.

### 3. `--adv-hold-q 1e9` equals 1: PASS (R-5's declared differences only)

V71 plus `--adv-hold-q 1e9`, `<OUT>` = `build-equity/v8-i5c-i3-nav`.
- **The 10 daily and events CSVs are byte-identical** to 1a.
- `recipe.json` `24b0a278...6728`: only `adv_hold_q`, `adv_hold_rule` and `rule` differ. The rule is now
  `aim-partial-v5+neutral-price-risk-v1+adv-hold-1e+09`.
- `summary.json` `00caf320...368c`: 92 JSON paths differ, all of them declared ones:
  - `recipe_sha256` and `rule`;
  - `scenarios[k].construction.rule_id`;
  - `scenarios[k].construction.adv_hold.*`.

  A check script found 0 unexpected paths.
- In all 5 scenarios: `decisions` 754, `clipped_names_total` 0, `residual_breach.names_total` 0, `unplaced_mass_total` 0.

### 4. Composition v8, re-rank and cap off, equals the v7 composition: PASS

Step 1, the weights (`v8-i5c-i4w-run`):

```
python atx-impl/tools/composition_rules.py identity-weights --weights build-equity/mega-weights-v71-ew/composition_weights.json --weights-sha256 7b0a59c96284c85cda08abe89852169757f3730a37df6c33b72e602d8c29010f --out build-equity/v8-i5c-i4-identity-weights.json
```

It printed `rule ew-theme-std-v1`, `rerank false`, and SHA-256 `88635696ebe8ed4a175f48bc4316869ed6203974aa47e180e056e0adbb5c1d7e`.

Step 2, the w pass (`v8-i5c-i4-run`): the argv of `mega-v71w-train-ew-run1` with two changes, `--output
build-equity/v8-i5c-i4-w` and `--composition-weights build-equity/v8-i5c-i4-identity-weights.json
--composition-weights-sha256 88635696...7e`:

```
ic --library atx-impl/strategies/fund_industry_ic_v71.json --library-sha256 787c802ed1cdf7cfc507500b014525c3d4dff148f44e77ebd8368c0a686c2259 --train build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json --train-sha256 3e79978a... --train-fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9 --train-fields-sha256 8fd00e9f... --output build-equity/v8-i5c-i4-w --max-memory-mib 1536 --min-names 1000 --workers 4 --save-combined --cache-legacy-fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v7 --candidate-cache build-equity/mega-candidate-cache-v71 --composition-weights build-equity/v8-i5c-i4-identity-weights.json --composition-weights-sha256 88635696ebe8ed4a175f48bc4316869ed6203974aa47e180e056e0adbb5c1d7e
```

Compared with the accepted pass `mega-v71w-train-ew-1`:
- **Byte-identical:**

  | file | SHA-256 |
  |---|---|
  | `train_combined.f64` | `1cf245b1...3912` |
  | `train_combined_member.u8` | `732f47b7...a1f4` |
  | `train_combined_finite.u8` | `732f47b7...a1f4` |
  | `train_combined_ids.u64` | `102e89c6...741c` |
  | `train_combined_sessions.i64` | `89af5340...2830` |
  | `train_planned_targets.csv` | `e6dbd9a8...a65297` |
  | `train_daily_ic.csv` (whole file; its 2,176 `__combined__` rows hash `c1bfeef9...56ae` on both sides) | `7e6e596f...bce6` |

- **Differ only by hash links:**

  | file | before -> after | what differs |
  |---|---|---|
  | `train_combined.json` | `bf1af127` -> `2831faf0` | `composition_weights_sha256`, `run_recipe_sha256` |
  | `recipe.json` | | `composition_weights_sha256` |
  | `orientations.json` | | `recipe_sha256` |

- `summary.json`: the weights SHA, `composition_weights.standardise` = `ew-theme-std-v1;rerank-off`, the hash chain
  and the timings.
- `train_candidates.jsonl` (96 records): only timings differ.
- Cache: 48/48 signal and ic_result hits on `mega-candidate-cache-v71`, so nothing was written to the accepted cache.

### 5. Warm u pass: 48 of 48 cache hits, digest equal to the cold pass: PASS

The argv of `v8-i3p4-d-w4-run` (the integration 3 cold pass at 4 workers) with `--output build-equity/v8-i5c-i5-u`,
on the same cache `build-equity/v8-i3p4-d-w4-cache`:

```
ic --library atx-impl/strategies/fund_industry_ic_v71.json --library-sha256 787c802e... --train build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json --train-sha256 3e79978a... --train-fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9 --train-fields-sha256 8fd00e9f... --output build-equity/v8-i5c-i5-u --max-memory-mib 2560 --min-names 1000 --workers 4 --save-combined --cache-legacy-fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v7 --candidate-cache build-equity/v8-i3p4-d-w4-cache
```

- **Cache: 48 of 48 signal hits and 48 of 48 ic_result hits**, misses 0, `vm_evaluations` 0, and no `VM-complete` or
  `cache-write` line in stdout. The cold pass had 48 misses.
- **The cache keys held across FIX-AB's B-3 re-pin and H3's `role_panel` paths.** The VM identity is
  `dslvm1_clang18.1`, the IC identity `dslic1_clang18.1_simd2`, semantics 1 and subdirectory `ic1_dc433f93f4b362bf`.
- **Byte-identical to the cold pass `v8-i3p4-d-w4-u`:**

  | file | SHA-256 |
  |---|---|
  | `train_combined.f64` | `2df53665...c342` |
  | `train_combined.json` | `62cea35e...944a56` |
  | `orientations.json` | `eb2049a4...c6ff` |
  | `recipe.json` | `0e4445a2...9642` |
  | `train_daily_ic.csv` | `929b4a5a...a5c8` |
  | `train_planned_targets.csv` | `d48c34c6...f6b11` |
  | `train_combined_member.u8`, `train_combined_finite.u8` | `732f47b7...` |
  | `train_combined_ids.u64` | `102e89c6...` |
  | `train_combined_sessions.i64` | `89af5340...` |

- `summary.json` and `train_candidates.jsonl` differ only in:
  - cache hit/miss fields;
  - stage and wall timings;
  - `verify_bytes`;
  - the field-load counters.

### 6. Field reuse step 2 (`--reuse`): 49 reused / 14 recomputed, 63/63 payloads identical (FINDING against PM3-5)

The argv of `v8-i3p4-c-run2` (integration 3, live regsho pin `fb073c62`) with `--output build-equity/v8-i5c-i6-fields`
and `--reuse build-equity/v8-i3p4-c-fields2 --reuse-sha256 5e5def8dfddca125b9c4c400d94dcc8873cdc417fa8a98e51898b559472be699`
in copy mode, so the prior directory is untouched. Every other argument is unchanged: the 63-field v9 list, the
FINRA, TickerHistory3 and CNMS roots, identity-bridge-r4-v1, fundamental-events-v2, `--fund-lag-sessions 1`, the SEC
stages and the five holdings stages with their pins, and `--max-rss-mib 2048 --max-seconds 580`.

- **Counts: reused 49, computed 14.**
  - The 14 computed fields are the SEC group: `ea_*` x6, `ins_*` x5 and `k8_count_63`, `k8_item_material_21`,
    `k8_days_since_any`.
  - The reason given for each: "producing code differs (research_fields_sec.py group sec ...)". That is F-B's
    fingerprint move, as F-3 stated.
- **Payloads: 63 of 63 field SHA-256 equal to the prior's**, and every file re-hashes on disk to its manifest pin.
- Manifest `5e5def8d...e699` -> `3a5c5108694223109e9b71c8484d478545d20102e48c2a969b600fa0a270c8b0`. It differs only in:
  - the code identity;
  - `reuse`;
  - per field, `reused_from` (49) and `producer` (14);
  - `source_checks.holdings.stages.*.files_read` (5 paths: the reused holdings fields read no file).

  The first differing file is `manifest.json`; it is the only one.
- Expectation from the brief and F-3: 49 / 14. **Observed: 49 / 14.**
- **PM3-5's expectation (45 reused / 18 recomputed, with `ret_overnight`, `ret_intraday`, `ceq_iss_5y` and `coskew_60m`
  recomputed) is not observed, and it cannot apply to this argv:**
  - none of those four fields is in the C-3 63-field list;
  - no fields directory under `build-equity` carries any of them.

  B-1's calendar pin therefore has no field to bind in this identity. It is first exercised by the v10 build (the F-3
  argv delta), which is untested on real data.

### 7. spo-v2 side files identical to the pinned v7 side files: PASS; pin captured (PM3-7)

The argv of `mega-nav-v70-lo3-spo-v2-G1.0-run`, the recorded spo-v2 cell (W1b step 4, as run on lo3 under ruling
spo-a), with `--output build-equity/v8-i5c-i7-nav`:

```
targets nav --combined build-equity/mega-v70-lo3w-train-ew-1/train_combined.json --combined-sha256 3e39c944aabead5151d9882192f069dc4578fcdab655ef00eb0e1af4a8760cd0 --role build-equity/recent-fast-train-2020-2022-v2-lo3/manifest.json --role-sha256 40e3d832bb6223c6b5e68dbd48150037ed78fbb79633e5890bd93d01c895b78d --fields build-equity/recent-fast-train-2020-2022-v2-lo3-fields-v7/manifest.json --fields-sha256 2f14e20e3ff36b3a2d1fedaedc910f66465c5e308cc0138bd3d12923f1376e31 --output build-equity/v8-i5c-i7-nav --rule spo-v2 --cadence 1 --trade-fraction .05 --dust-multiple .1 --aim-leverage 1.247 --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 --max-bytes 1073741824 --order-basis delta --exit-rate .05 --locate-in-aim --liquidity-cache --risk-model build-equity/v7-f3-risk-lo3 --risk-model-sha256 786cb601dd4295450872ee0fd726a752b2996f2c3ea886ab6f399f3676e14913 --spo-gross 1.0 --ic-book .02 --alpha-horizon 21 --w-max .01 --adv-cap-q .05 --adv-trade-p .01 --spo-iters 500 --spo-tol 1e-8 --target-vol .05 --spo-books primary --specific-ceiling 1 --specific-ceiling-void on
```

- **9 of 9 files byte-identical** to the recorded cell:

  | file | SHA-256 |
  |---|---|
  | `spo_diagnostics.csv` | `1131cd594418a6d365b7adf530ad85c6adef4e80671c694c546aa8a068467dc2` |
  | `v7_transfer_coefficient.csv` | `a27505c2900b4284d99b6bad58aa278d7ca65af1123a26325b054b9d36731bb0` |
  | `v7_extras.json` | `d96be152727004e2e8aecff2bfc23be03dda243d4630735393faf285ab1142eb` |
  | `recipe.json` | `d6ffb028...58c8` |
  | `summary.json` | `75caf7f6...c44f` |

  The 2 daily and 2 events CSVs are byte-identical too.
- Wall 98.0 s against the cell's 47.2 s. This is host load (the same exe ran the v7.1 cells in 33-42 s), not a
  finding.
- **Pin (PM3-7):** the v8-7a target-tests (`575539e2...7f71`) print
  `[spo-v3-pin] v2 weights=0xb039820b40d5cf24 (40 diagnostics rows) replay=0xd24b61721a7c698c (30 days)`. This is the
  same line v8-4e, v8-5, v8-6a and v8-7a printed. Commit `3bfd293e` (`test(spo): ...`) sets `pinned_v2_weights =
  0xb039820b40d5cf24` and `pinned_v2_replay = 0xd24b61721a7c698c` in `atx-impl/tests/strategy_spo_v3_pin_test.cpp`, and
  the comment names PM3-7 as the capture route.
- **Build v8-8** (`scripts/research-build.ps1 -Preset equity-dev`):
  - targets atx-impl-strategy-target-tests and atx-impl-tests;
  - source `3bfd293e`, DirtyEntries 0, exit 0, 18.6 s, 2 TUs, 2 links, provenance `e2bb716b`;
  - target-tests `56d43cf8a5e57ae57f2fded594da16e178f61bcce607d80d33f05e16e6b0106f`, impl-tests
    `115ec04d42694e884330b2ca7915e44a2ca604e32175b0cb37100150cb60b8a4`;
  - the strategy exes are unchanged (v8-7a SHAs).
- On v8-8:
  - `SpoV3.V1AndV2DigestsUnchanged` **passes** and prints the same values;
  - target-tests `Spo*:NavV7Hook.*` 48/48 (was 47 + 1 skipped);
  - impl-tests `SpoV3.*:SpoPin.*` 12/12;
  - whole target-tests **234/234** (was 233 + 1 skipped).

### 8. NAV `--label-role` equal to `--role` equals flag absent: PASS (E-25's declared differences only)

V71 plus `--label-role build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json --label-role-sha256
3e79978a858cbf6b723ff7a896d56814f7505dde11b805c30c5b909783ebb809`, `<OUT>` = `build-equity/v8-i5c-i8-nav`.
- **The 10 daily and events CSVs are byte-identical** to 1a.
- `recipe.json` `10961936...6635`: the only addition is `label_role {manifest_sha256, rule}`.
- `summary.json` `6753c13c...5c00`: the additions are `label_role {basis, label_only_present_cells 0,
  label_only_present_cells_scored 0, manifest_sha256}`, and `recipe_sha256` changes.
- This is exactly the E-25 report's expectation (steps 2 and 3). The brief's wording "byte for byte" holds for every
  output CSV, not for recipe and summary, which carry the flag by design. Both are recorded here.

### Hidden-data record

- **Inputs:** only the TRAIN 2020-2022 roles (lo1 `3e79978a`, lo3 `40e3d832`), their fields-v7/v9 and fields2, the
  accepted v6.1, v7.0-lo3 and v7.1 artifacts, the lo3 risk model `786cb601`, and the integration 3 caches.
- **The fields build (i6)** read the same source stages as `v8-i3p4-c-run2`, sealed at 2024-01-01 by the builder.
- **Logs:** a scan of every `v8-i5c-*-run` stdout and stderr for dates in 2024 or later found 0 hits.
- **What I read of the manifests:** reuse reasons and counts, field SHA-256s, and the JSON paths of differences.
  Source-check values were not read.
- **Nothing dated 2024-01-01 or later was opened. No disclosure.**

### Open items

- Finding (identity 6): PM3-5's 45 / 18 expectation does not fit the identity argv (no calendar-pinned price field in
  the 63-field list; no prior on disk carries one). The brief's and F-3's 49 / 14 was observed, with 63/63 payloads
  identical. The PM rules whether PM3-5 moves to the first v10 build.
- The csv holdings layout identity (`v7-l3-holdings` keeps no `holdings.csv`) was not in this dispatch and was not run.
- Carried:
  - `ConfigJsonNotInDiscoverDigest` (known);
  - the `ParallelLockstepGrid` group (unconfigured);
  - build provenance `e2bb716b` for v8-8 (source `3bfd293e`).

## Wave 0 part 1 (R1-R9) (2026-09-30)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `864b7836` (clean). Dispatch: brief
`task-W0-run-brief.md`, runbook `w0-2-runbook.md` steps R1-R9 only (stop after R9; R10 and later not run). No merge,
no build.

### Preconditions

1. **Disk:** `df -h /c` 40 G free of 458 G (>= 30 GB). Host RAM 16,068 MiB, 3,161 MiB available at start.
2. **Runbook section 1:** `research_window.SEAL_DATE` prints `2024-01-01`; `p.SEAL, f.SEAL, h.SEAL, b.SEAL` print
   `2024-01-01` four times. `research_window.json` SHA-256 `62cf2cfab1d0f61b02731a807ccfdd326bb2df91caff1173a3cf1b37e6e63584`
   (`WINDOW_ID research-window-v2`, TRAIN `[2020-01-01, 2024-01-01)`). Vendor file unchanged: 3,617,973,507 B, mtime_ns
   1789920127331396300 (the runbook's values).
3. **Section 0 blockers at `864b7836`** (no fix needed; no precondition commit):
   1. regsho_threshold republished: no step of R1-R9 reads that stage; its re-hash belongs before R10/R11 (part 2).
      The live stage manifests R8 reads (v2-pit bridge, fundamentals, delisting) are hashed by the runner at R8's
      launch (`--bind`) and checked by the role builder against the runbook pins; recorded under R8.
   2. `LAST_SUB_QUARTER`: fixed, `atx-engine/tools/build_fundamental_events.py:76`
      `"{}q{}".format(*rw.last_quarter_before_seal())` (prints `2023q4`); `SEAL = rw.SEAL` at `:71`.
   3. insider 2024q1 file: fixed, `atx-engine/tools/research_fields_sec.py:620-622`
      `if rw.partition_is_sealed(int(year), int(quarter)): ... continue` (never opened; `partition_is_sealed(2024, 1)`
      True, `(2023, 4)` False).
   4. `TRAIN_END_EXCLUSIVE`: fixed by V-1's move, `atx-impl/tools/backtest_integrity.py:97-108` (window module) and
      `:576-585` (refusal reads `rw.TRAIN_END_NS` = 1704067200000000000); the studies `nav_summ.py` is now a shim that
      runs `atx-impl/tools/nav_summ.py`.
   5. one-line role command: procedural; linked universes refuse `--cache`
      (`atx-engine/tools/prepare_recent_research.py:973`), so R1-R3 build the base role first, as the runbook does.
   6. protocol line without a cell: fixed, `scripts/research_ledger.py:58` `NON_TRIAL_KINDS = (PROTOCOL, DEFECT,
      VALIDATION)` skipped by `cells()` (`:95-103`); `scripts/research_cycle.py:1110-1125` `ledger_cells` reads
      `research_ledger.cells`.
4. **Executables:** latest tracked builds are v8-7a (source `ccb66a87`, receipt SHA-256 `61f2fbba...b952`) and v8-8
   (source `3bfd293e`, test targets only, receipt `4058198e...eb5c`). `git diff --stat ccb66a87 864b7836` outside
   `.superpowers/` is one test file (`atx-impl/tests/strategy_spo_v3_pin_test.cpp`), linked into no strategy exe, so no
   build ran. On-disk SHA-256 equals the v8-7a receipt: ic `39bc5f331fd3b1fb6f5acf55fe153cfe13b1e994533e55f6ed2bf80d73fbce2b`,
   targets `474fabb02ef61f7254c04cedf3444d7d85e61aff8cba974736866a8c80f3b7d1`.

### Caps

Brief caps: preparation steps (R1-R8: projection, role, scan, repair, bridge, events, grp fields, lo1, lo3) run under
600 s / 2,560 MiB; R9 (IC exe, plan-only) under the IC cap 300 s / 2,560 MiB. The free-memory floor and the tools'
own inner limits are the runbook's argv unchanged (`--min-free-mib` 768 for R2, 512 elsewhere).

### Runs

Every run: `scripts/run_bounded_research.py` on a clean tree (the source column), one at a time, argv = runbook
section 3 with the caps above. Receipt dirs under `build-equity/`. Peak = sampled peak tree RSS.

| step | receipt dir | source | outcome / ExitCode | s | peak MiB | receipt.json SHA-256 | output manifest SHA-256 |
|---|---|---|---|---|---|---|---|
| R1 | `recent-projection-v2-run` | `a5ba41c5` | completed / 0 | 53.9 | 766 | `fc4d312193ad89fb9a289546805a988d0159969044ed6ed004df4cc2a0a88c5d` | `931b54ef8412b9f61ae4b9c67d663a265604209bc2639c81046669b591fc2ea5` |
| R2 | `train-2020-2023-base-v1-run` | `f92e09e4` | completed / 0 | 18.9 | 374 | `9a84e921ee595902b1c9a467ea77bc2d2538359229ef5c733a60889ed90091d9` | `6688677096b6a5692f0040ea9ed54df9da938566a062d9ab1ffd873d53636678` |
| R3 scan | `train-2020-2023-scan-run` | `0aac5f22` | completed / 0 | 1.9 | 237 | `16b767c76fd217c9f75bbeae796370f2c90ccb036d98232ce842bf124ede297c` | (none; verdict on stdout) |
| R3 repair | `train-2020-2023-base-run` | `d8e67483` | completed / 0 | 3.6 | 389 | `403522ad5f1f21fe9899038ca70ba7d53a66125e7c293eb59e3fc61ad8f96ac7` | `de8d91db7e8788dc58ac1fc6aa1bedc71dc555e63039a378c2deb469f8c49029` |
| R4 | `identity-bridge-r4-v2-run` | `4c2fcc74` | completed / 0 | 2.8 | 142 | `8fe0b16cf770aaf5f3c2c86f6a748216797d110f9ed8e7a679c3ac9ef454e2d4` | `f598c04c51e0bea33bd730a3a2beb3a9f76abe0b9630c75f21c70ad0ec3643de` |
| R5 prep | `fundamental-events-v3-run-prep` | `58a10023` | completed / 0 | 11.9 | 455 | `b0629775396545030c3d2fbfc650243dce7244f3a3d0684fdbff3fba4d821aff` | (stage; manifest at fin) |
| R5 b0-20 | `fundamental-events-v3-run-b0-20` | `6430fdda` | completed / 0 | 47.2 | 620 | `e08a9acd430888a57963db38fe2c5cad6a2d155aa482293b069f3180f782801d` | (stage; batches 0-20 computed) |
| R5 b21-41 | `fundamental-events-v3-run-b21-41` | `38b06aa9` | completed / 0 | 56.9 | 657 | `c9dcf0f64f10b4819eccf1231f7c95eeddb513ac3a801c077a897a908c26d8a2` | (stage; batches 21-41 computed) |
| R5 b42-62 | `fundamental-events-v3-run-b42-62` | `19d0a704` | completed / 0 | 49.6 | 599 | `1b2ee2338c6954390e9f92b09f4b9cbfcebbc1330f666866999e8084d9f60432` | (stage; batches 42-62 computed) |
| R5 b63-84 | `fundamental-events-v3-run-b63-84` | `d0bef9b3` | completed / 0 | 30.9 | 608 | `7d6f302b0d91b49c6afb626c56c900f92dd90726b6005d6eca3562f98991df14` | (stage; batches 63-84 computed, batch 84 0 rows) |
| R5 fin | `fundamental-events-v3-run-fin` | `19c64f0e` | completed / 0 | 6.6 | 302 | `03e33b04e8b8c0be909c80f86ece4a395e4d60595f4bf416225ef4c70038fb45` | `304d2945d6226c0ca9b56d1fb6e8309ae1f42da5dfcd9652aee1a83dc29be87b` |
| R6 | `train-2020-2023-base-grp-run` | `977d9a4b` | completed / 0 | 6.7 | 218 | `747e51d46880a31646687e8e0fd62b33e5f881ad6bcf50d8f5c5f8c7fb481475` | `d1b2eaea9087f3eae4953bf709dd63623995f518993e8167ef7b387c7e37957e` |
| R7 | `train-2020-2023-lo1-run` | `bfd6bc91` | completed / 0 | 4.0 | 232 | `983717743db5899238b8b63cc6d11c230925d602a570124ee36dfdfe3b835c65` | `2ff9d7711bdaa2d669cc096f4874f7f4705309e53572fd12dad3c0a4d7ac1e53` |
| R8 | `train-2020-2023-lo3-run` | `2d3e14ef` | completed / 0 | 4.9 | 304 | `9d3cafcd22fcd586c9ae4a517cfcf8fc02e230bdf542a6a9abdfe1b810767b11` | `e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4` |
| R9 lo1 | `w0-2-admit-probe-lo1-run` | `9322cdd1` | process-error / 1 (by design) | 0.3 | 2 | `8fb0dfc5d1e883ddd7afa324ffbe6ef45ee11a8a692fe76e0d05b04c42ff2fe4` | (none; metadata probe) |
| R9 lo3 | `w0-2-admit-probe-lo3-run` | `25b744af` | process-error / 1 (by design) | 0.3 | 3 | `aa9d85d82db080b7c669980940d4afd879707809aff8b1a9a87018785546e8c6` | (none; metadata probe) |

Every measured peak and wall time also fits the runbook's own narrower runner limits (largest: R1 766 MiB of 1,100;
R5 b21-41 56.9 s of 180).

- **R1 PASS.** Projection manifest: start 2018-06-01, `end_exclusive` 2024-01-01, 1,405 sessions 2018-06-01..2023-12-29,
  250 in 2023, none on or after the seal; 13,419,299 accepted rows; 354 MiB on disk. Runner min free 1,996 MiB.
- **R2 PASS** (`train-2020-2023-base-v1-run`, source `f92e09e4`, completed / 0, 18.9 s, 374 MiB, receipt
  `9a84e921ee595902b1c9a467ea77bc2d2538359229ef5c733a60889ed90091d9`, manifest
  `6688677096b6a5692f0040ea9ed54df9da938566a062d9ab1ffd873d53636678`). Prints `1405 5922 399 1704067200000000000`:
  **n = 5,922** (3-year union 5,627; runbook estimate 6,100, open question 7). Overlap check:
  `score_member_counts[:756]` equals the v1 role's 756-entry list (True). 207 MiB on disk.
- **R3 scan PASS.** `verdict factor-break-v1: MASS 1 session(s): 2021-01-04` (v2 detector the same). Largest jump count
  outside the mass session 44 on 2022-12-29 against the threshold 50 (the T12 margin is unchanged by the new columns);
  no 2023 session above 32. Proceeding to the repair with `--expect-sessions 2021-01-04`.
- **R3 repair PASS.** Same verdicts; repaired 2,178 crossing steps at 2021-01-04 (kept: 2 split-follow, 8
  distribution), 1,358,390 close cells changed, max |ln return error| 2.33e-15, post-repair max jump/session 44,
  max unexplained/session 1. `BASE = de8d91db...9029`; 207 MiB on disk.
- **R4 PASS.** The tool verified the r4 source SHA-256 `ac9bcda7...` (`--expect-source-sha256`). Manifest `seal`
  2024-01-01, `counts.max_end_incl` 2023-12-31, 6,368 CIKs (`ciks.txt` 6,368 lines), 6,450 lines; 2 MiB on disk. No
  comparison with v1's CIK set was made (v1's extra CIKs are 2024-only link evidence). The optional `--check`
  diagnostic was **skipped**: it opens the live `C:/atx/atx-db/data/warehouse.duckdb`, the atx-db session is active
  (open question 27 unresolved), and the dispatch forbids touching `C:/atx`.
- **R5 prep PASS.** The tool verified the companyfacts (`50e018e1...`) and FSDS (`2cad6134...`) staging pins.
  `run.json` `parameters.seal` 2024-01-01, `parameters.sub_quarters` `2009q2..2023q4`; `fsds.sub_quarters` lists 59
  quarters, the last `2023q4` (the only 2024+ date text in `run.json` is the seal itself). SIC: 6,337 CIKs, 217,340
  rows; 366,856 unique SUB accessions.
- **R5 PASS** (6 runs, 203.1 s total, max peak 657 MiB; the runbook expected ~93 s / <= 660 MiB: the event batches
  ran ~2.2x slower than the old receipts, well inside the cap). Manifest `seal` and `parameters.seal` 2024-01-01,
  `parameters.sub_quarters` `2009q2..2023q4`; 155,577 event rows; 51 MiB on disk. The only other date text past the
  seal is `caveats[2]`, the CompanyFacts snapshot label (2026-09-20, archive `ee099c73`), not data.
  `FEV = 304d2945...be87b`.
- **R6 PASS** (optional step run, so R7 keeps `--check-fields`). Fields `grp_sic2, grp_ff12, grp_ff49` on the base
  role (1,405 x 5,922), `seal.exclusive_end` 2024-01-01; 3,444 linked CIKs, 135,259 SIC rows; 191 MiB on disk.
  `GRP = d1b2eaea...957e`.
- **R7 PASS.** Role lo1 = linked-operating-v1, 1,405 x 5,922, `score_begin` 399, `score_end_ns` 1704067200000000000;
  kept member cells 2,382,552 of 3,970,647; fields cross-check `link_member_cells_equal` true against `GRP`. The
  runbook one-liner prints **`1155 1405 True`** (kept members per session identical to the 3-year lo1 role on its
  1,155 sessions). Manifest 90,101 B; 207 MiB on disk. `LO1 = 2ff9d771...1e53`.
- **R8 PASS.** Live stage manifests re-hashed at R8's launch by the runner (`--bind`) equal the runbook pins:
  identity-bridge-v2-pit `09aac28f757fa959b0ed4cd9296b2267940e70af98e0d2c67cc45b1df4f7fa01`, fundamentals (SIC)
  `9f9b2f85f6bcd5c7f3a55aee097893094a5cb85ab2b4edbfb582297dab06816b`, delisting
  `1b1166b61e5a77d8dbe007f2de3261392424fb86a59c1118028862abc264c37f`; the role builder verified the same pins.
  Role lo3 = linked-operating-v3, 1,405 x 5,922, `score_end_ns` 1704067200000000000, `--delisting-returns` off; kept
  member cells 2,490,424. The one-liner against the 3-year lo3 role (`40e3d832`) prints **`1155 1405 True`**.
  Manifest 602,407 B (runbook estimate ~560 KB; C++ metadata cap 1 MiB); 207 MiB on disk. `LO3 = e1c67101...95f4`.
- **R9 lo1 PASS** (exe ic `39bc5f33...ce2b` = v8-7a, library v7.1 `787c802e...2259`, `--max-memory-mib 64`,
  `--workers 4`, `--plan-only`). stderr: `Unavailable: IC runner: required_bytes=1989405564 max_compiled_slots=8
  exceeds configured memory budget before payload load`. **1,989,405,564 B = 1,897.2 MiB**, equal to the runbook's
  exact formula at n = 5,922 (slots 8, capacity 6, workers 4; the formula reproduces the 3-year 1,553,063,994 B).
  Below the OD-2 cap 2,684,354,560 B (2,560 MiB) by 663.1 MiB; above 1,536 MiB, so the IC phases need OD-2.
- **R9 lo3 PASS.** Same stderr line, `required_bytes=1989405564 max_compiled_slots=8`: lo1 and lo3 share axes
  (1,405 x 5,922), so one number serves both, as the runbook said. **Q2 answered:** 1,897.2 MiB at n = 5,922, fits
  2,560 MiB; open question 8 answered by the exe itself (`max_compiled_slots=8`); open question 7 answered (n = 5,922).
  Expected cold-pass RSS at the recorded 82% ratio: ~1.56 GB.

### Pins for part 2 (R10 onward)

| var | artifact | manifest SHA-256 |
|---|---|---|
| PROJ | `build-equity/recent-projection-v2` | `931b54ef8412b9f61ae4b9c67d663a265604209bc2639c81046669b591fc2ea5` |
| BASE1 | `build-equity/train-2020-2023-base-v1` | `6688677096b6a5692f0040ea9ed54df9da938566a062d9ab1ffd873d53636678` |
| BASE | `build-equity/train-2020-2023-base` | `de8d91db7e8788dc58ac1fc6aa1bedc71dc555e63039a378c2deb469f8c49029` |
| R4 | `build-equity/identity-bridge-r4-v2` | `f598c04c51e0bea33bd730a3a2beb3a9f76abe0b9630c75f21c70ad0ec3643de` |
| FEV | `build-equity/fundamental-events-v3` | `304d2945d6226c0ca9b56d1fb6e8309ae1f42da5dfcd9652aee1a83dc29be87b` |
| GRP | `build-equity/train-2020-2023-base-grp` | `d1b2eaea9087f3eae4953bf709dd63623995f518993e8167ef7b387c7e37957e` |
| LO1 | `build-equity/train-2020-2023-lo1` | `2ff9d7711bdaa2d669cc096f4874f7f4705309e53572fd12dad3c0a4d7ac1e53` |
| LO3 | `build-equity/train-2020-2023-lo3` | `e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4` |

### Disk

C: free 40 G before R1, 46 G (47,916,108 KiB) after R9; the rise is other host activity. The eight new artifact
directories hold 1,423 MiB (run dirs excluded). RAM available after R9: 3,479 MiB.

### Fixes

None. No source file changed; the only commits are this log (one per run, `git add -f`).

### Hidden-data record

- **Inputs opened by the tools:** the vendor file (reader-side seal), the r4 export (`ac9bcda7`), CompanyFacts and FSDS
  staging (pins `50e018e1`, `2cad6134`; SUB quarters to 2023q4 only), the v2-pit bridge, fundamentals and delisting
  stages (pins above), and pool-2 artifacts. Whole-file pin hashing of multi-year stages is the runbook's design (open
  question 25).
- **Logs:** a scan of all 16 run dirs' stdout and stderr for dates in 2024 or later found 0 hits.
- **What I read:** receipt fields, manifest keys named in the runbook checks (window, seal, sessions, counts, universe
  kept counts, fields cross-check), the R3 scan table (2018-06-01..2023-12-29) and one caveat string (the CompanyFacts
  snapshot label, 2026-09-20, a vintage name). I made no comparison of the r4-v2 CIK set with v1's (v1's extra CIKs
  are 2024-only evidence) and read no sealed-row counter.
- **Nothing dated 2024-01-01 or later was opened by me. No disclosure.**

### Open items

- Part 2 (R10-R14) not run, per the dispatch. Before R10/R11: re-hash the live stage manifests, in particular
  regsho_threshold (blocker 1; open question 1, pin decision still with root).
- R4's optional `--check` diagnostic was skipped (live atx-db warehouse; open question 27).
- R5 event batches ran ~2.2x slower than the old receipts (203 s total against ~93 s); still far inside every cap.
- Carried: sealed-row counter key names (open question 24); the validation-window artifacts and
  `recent-projection-v1` still hold 2024 data (open question 33, owner).

## integration 6 part A (2026-09-30)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `2a283e29` (clean). Tag prefix v8-9.
Scope: merge DLRET, ERA, RISK and LIB2 by SHA, two integration edits (E-41 plan-time N, E-42 theme), build, tests.
Not in scope (part B, after FIX-3): the pooled aim `elif` / `pooled_aim_weights` in `fit_composition_weights.py`,
left untouched. No identity run on data and no real-data run.

### Merges (`--no-ff`, in order)

| lane (tasks) | lane SHA | merge | conflicts |
|---|---|---|---|
| DLRET (E-39: lo1 `--delisting-returns` label-role proof; tests and fixture only) | `f51c5fd8` | `2ddcece9` | none (5 files, +3609 / -8) |
| ERA (E-35, P-1; round 1 E-41 `0b855971`, E-35a `51d59f48`) | `ebc254f0` | `1f2c98ab` | none (9 files, +1037 / -105) |
| RISK (R-8 risk-target-v1; branched from `28051c4c` = FIX-2's N-2, carried as intended) | `35bcda95` | `d1c86389` | none (17 files, +2009 / -30) |
| LIB2 (library v8.2 registration, field `exch_up_365d`, holdings kind `xsw`) | `5d64644e` | `31c69d9d` | none (4 files, +758 / -8) |

`scripts/research_ledger.py` (ERA) and the three CMake lists (RISK) auto-merged. RISK's `test_research_spec.py`
r8 entries merged clean beside the r10 / r11 entries already on root (the union the R-8 report asked for).

### Integration edits

| commit | lane | files | what |
|---|---|---|---|
| `e41d71b1` | ERA concern 3, Ruling E-41 | `scripts/research_cycle.py`, `scripts/research_ledger.py`, `scripts/research_roles.py`, `scripts/tests/test_research_cycle_roles.py` | plan-time `summ.dsr_n "ledger+1"` adds 0 for a history read |
| `7ca53c67` | LIB2, Ruling E-42 | `task-LIB2-report.md` | `exch_switch` joins `filing_events`; no `listing_events` theme |

- E-41: nav_summ's rule (`0b855971`) calls the pooled row a history read when its series begins before TRAIN and
  takes N with no + 1. At plan time there is no NAV output, so the roles loop applies the same rule to the declared
  era windows: `research_roles.begins_before_train(roles)` (first era's begin < TRAIN begin; `check_windows` already
  forbids a straddle). It sets `Cycle.summ_history` wherever it sets `summ_pool` (one history role, or the anchor of
  a RolesCycle). `Cycle.ledger_cells` passes it to `research_ledger.ledger_n(..., history=)`, which calls
  `backtest_integrity.ledger_n(records, history or scored_in_ledger)`, the same call nav_summ makes. A pool of eras
  inside TRAIN is unchanged (its pooled line is a trial, N + 1).
  - New test `test_e41_a_history_read_adds_no_trial_to_the_plan_time_ledger_n`, on a ledger holding one TRAIN cell:
    one history era with `--era-of` gives `--dsr-n 1`, a two-era history pool gives `--dsr-n 1`, and the tiny_world
    TRAIN split gives `--dsr-n 2`.
  - Negative check (scratch): with the flag ignored, the test fails.
- E-42: in `task-LIB2-report.md`, section 1 (table row), C-3 (registration and theme risk), section 3 L1 (the
  `listing_events` row is withdrawn; replacement `filing_events` text: "Adverse filing and listing events: an 8-K Item
  4.02 non-reliance disclosure within the last 63 sessions, or a move of the listing up to NYSE or NYSE American (from
  Nasdaq or NYSE American) within the last 365 days; a recent event predicts lower returns."), the L3 `exch_switch` row
  and the `exch_switch` add-alpha line (`--theme filing_events`), and LIB2-a marked ruled. The DSL strings and SHAs
  are unchanged.
  - LIB2 committed no registry or spec file, and no test names the theme (`git diff fd2ff7a8 5d64644e -- atx-engine/tools`
    has no theme reference), so there is no test to add.

### Builds (`scripts/research-build.ps1 -Preset equity-dev`)

| tag | source | targets | result |
|---|---|---|---|
| v8-9 | `7ca53c67` clean | atx-engine-book-tests, atx-impl-strategy-target-tests | ok, exit 0, 64.8 s, 18 TUs, 4 links, 4 jobs; reconfigured (CMake lists changed), provenance `7ca53c67`; receipt `63cfcab3...` |
| v8-9a | `7ca53c67` clean | atx-impl-tests, -strategy-tests, -strategy-ic-tests, -strategy-mine-tests, atx-engine-factory-tests, atx-equity-strategy-targets, -ic, -risk, -mine | ok, exit 0, 16.0 s, 8 TUs, 10 links, 4 jobs; receipt `69332085...` |

**RISK compiled first time under `/W4 /WX`, with no build fix** (`risk_target.cpp`, `strategy_risk_target.cpp`,
`strategy_nav_v7.cpp`, `strategy_spo.cpp` and the three test files). Both logs have 0 warnings.

Executables:
- v8-9: book-tests `a9918acf...bb2b`, target-tests `b3ce62e8...3a0a`.
- v8-9a: impl-tests `fdde916b...4795`, strategy-tests `8d5f1779...cfac`, ic-tests `4a98ef50...2860`, mine-tests
  `5d779b77...4961`, factory-tests `3e8d84f2...9817`, targets `a259a293...7304`, ic `d7dcc0db...781c`, risk
  `fab237aa...8c32`, mine `8ada5e39...0369`.

### Tests

| exe / suite | build | result |
|---|---|---|
| target-tests `RiskTarget.*:BookRiskTarget.*` (RISK) | v8-9 | **14/14** |
| target-tests `RiskTarget.FlagAbsentKeepsThePinnedBenchDigests:SpoPin.*:SpoV3.*:SpoHook.*:NavV7Hook.*` (RISK identity) | v8-9 | **37/37** (SpoPin `0xda6b6871e7e267c5` / `0xaabdbb72f99a6e13` hold) |
| target-tests `NavLabelRoleLo1.*:NavLabelRole.*` (DLRET) | v8-9 | **9/9** (3 new + R45's 6) |
| atx-impl-strategy-target-tests (whole) | v8-9 | 253/253 (part C 234 + DLRET 3 + RISK 14 + N-2 / RISK spo-v3 2) |
| atx-engine-book-tests (whole) | v8-9 | 154/154 |
| atx-impl-strategy-tests | v8-9a | 46/46 |
| atx-impl-strategy-ic-tests | v8-9a | 105/105 |
| atx-impl-strategy-mine-tests | v8-9a | 18/18 |
| atx-engine-factory-tests | v8-9a | 387/387 |
| atx-impl-tests (run from the repo root) | v8-9a | 985 run: 979 passed, 5 skipped, **1 failed: the known `ConfigJsonNotInDiscoverDigest`** (recorded only) |
| atx-impl/strategies | - | 163 passed; `test_generate_library.py` with `ATX_V71_PLAN_JSON` 9 passed |
| atx-engine/tools (whole) | - | 252 passed, 6 subtests (part A 241 + DLRET 1 + LIB2 10) |
| atx-impl/tools (whole; `ATX_EQUITY_BIN`, `ATX_EQUITY_TARGETS_EXE` = v8-9a) | v8-9a | 482 passed, 2 skipped (ERA's FIX-3 equality test; `ATX_EQUITY_ROOT` unset) |
| scripts/tests (whole; `ATX_EQUITY_BIN` = v8-9a bin) | v8-9a | 168 passed, 3 skipped (the three RESEARCH_CYCLE_LIVE_ROOT tests); tiny_world `test_cycle_e2e.py` in it, no golden moved |

- impl-tests skips: Alpha101Orats x2, AtxImplDiscover.W6, SingleAlphaCapacity, FundamentalZoo (all environment
  gates). Part B's 7 lost `SpoV3.V1AndV2DigestsUnchanged` (pinned at part C) and `TrialLedgerRepository...` (it runs
  from the repo root).
- `git status` was clean after every Python set.

### Identity

- RISK flag-absent: the identity filter above (37/37). The data-side identity argv (parent NAV re-run) is root's,
  before R-8.
- DLRET: `git diff fd2ff7a8 HEAD -- atx-engine/tools/prepare_recent_research.py` is empty;
  `test_lo1_delisting_off_is_byte_identical` passes (in atx-engine/tools).
- LIB2 step 3: holdings producer fingerprints, `fd2ff7a8` source against the merged tree, equal for 13f `cab3b9b4`,
  ftd `9b2f42b6`, regsho `cc4cf935` and svx `b1ceebb3`; xsw new `3cf03c85` (the report's values).

### Hidden-data record

No real-data run. Read: the four lane reports, progress.md rulings E-41 / E-42 / E-43, library-v8-draft E7, sources,
build receipts and logs. Tests used synthetic fixtures and tiny_world only. Nothing dated 2024-01-01 or later was
opened. **No disclosure.**

### Open items

- Part B (after FIX-3): drop `pooled_aim_weights` and its `elif` in `fit_prior`, then un-skip
  `test_pooled_aim_fit_over_one_era_equals_the_single_window_fit` (ERA round 1 concern 1).
- E-42 assumes E7 was applied at v8.1 (`filing_events` exists because `nonreliance_402` proceeded). If
  `nonreliance_402` was withdrawn, `filing_events` does not exist and exch_switch would open it alone. Root to rule.
  Root also applies the widened text to the registry row at the v8.2 freeze.
- FIX-2 is only partly on root: N-2 (`28051c4c`) came with RISK; the rest of FIX-2 (E-31a void, SPO-4 traps) merges
  with its lane. PM3-9's R10-R14 waits on FIX-2, FIX-3 and ERA.
- RISK open risks carried: check the v1.1 store's `capped_specific` count before R-8 (no specific ceiling in the
  scaler); the ADV cap reads L (up to 1.25 x Q ADV); `strategy_live` has no `--risk-target`.
- DLRET: runbook R15 is stale (the lo1 label-role command is in task-DLRET-report.md); B0c registration names `$DL`.
- Known: `ConfigJsonNotInDiscoverDigest` (1).

## integration 6 part B (2026-10-01)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `d76da89d` (clean). Tag prefix v8-10.
Scope (`task-INT6B-brief.md`): merge FIX-2, FIX-3, COMB2 and ORTH by SHA; part-B edits 1-4 (Rulings E-27b, E-35a,
E-44, E-45, PM4-4); build; every suite; identities 1, 4, 7 and 8 (Ruling PM4-3). No Wave 0 step and no cell was run.

### Merges (`--no-ff` by SHA, in order)

| lane (tasks) | lane SHA | merge | conflicts and resolution |
|---|---|---|---|
| FIX-2 (N-1, N-3, P-2, P-3, T-1, T-2, T-4; round 1 E-31a / SPO-1 / SPO-5, SPO-4, E-14a, fixture) | `de32b9ad` | `78571ef2` | 21 files, +2092 / -240. `strategy_nav_v7.cpp` (2 hunks): union. RISK's `scaler` init and `risk_target_flag` / `risk_model_flag` kept; FIX-2's SPO-4 constructor body (`capacity_engine->set_primary_book({})`) and E-31a comment kept. N-2 `28051c4c` was already on root (the merge base): no-op, as expected. |
| FIX-3 (F-1, F-2, F-3, F-5, F-6, F-8, F-9, F-10, F-14; round 1 E-27b) | `75acb091` | `b3360dcf` | 21 files, +859 / -133. Five conflicts, all unions: `backtest_integrity.appendix_a_v8` docstring (E-41 history reads and F-5 defect lines); `fit_composition_weights.py` constants (FIX-3's `AIM_V2_RULE_ID` / `AIM_RULES` beside ERA's `POOLED_*`, left for edit 1); `research_cycle.py` usage (ledger-defect `--ruling --date` beside ledger-campaign; E-41 beside the F-9 / F-6 resume text); `research_ledger.py` usage (same); `r6-spo-v3.json` description: FIX-3's F-14 `--capacity-curve` sentence with FIX-2's E-31a pre-return check (`limits_unmet_primary.count == 0`) and E-14a criterion (`aim_correlation_traded_after.mean`). |
| COMB2 (R-10 ic-shrink-v1; round 1 ic-shrink-aim-v1, E-44) | `1e8af5b8` | `de2f9636` | 21 files, +2251 / -29. Three conflicts: `fit_composition_weights.py` (registration: aim-v2, then ic-shrink-v1, ic-shrink-aim-v1; `AIM_RULES` gains ic-shrink-aim-v1; the aim-v2 and ic-shrink `elif`s side by side); `test_fit_composition_weights.py` (constants pin both lanes' ids); `test_research_spec.py` (`NULL_PINS` / `EXPECTED_CHANGES` carry r8 and r10; the template-diff composition map carries r1, r3 -> aim-v2 and r10). |
| ORTH (R-11 theme-resid-v1) | `c1cc57ce` | `7e5ff769` | 19 files, +1584 / -19. Six conflicts. C++: `method_recipe` / `save_combined_artifact` take COMB2's rule name (`std::string_view standardised`) plus ORTH's `bool residualised`; `score_role` records `std_rule` for standardise and residualise alike (both run the standardisation); `run_ic` passes `pinned.standardise_rule()` and `pinned.residualise`; help text carries both lanes' lines; both new TUs in `atx-impl-core` and in the `/O2` + no-PCH lists. theme-resid-v1 stays ORTH's separate `theme_residualise` block riding on any rerank-true `theme_standardise` row, not a row of COMB2's table (it adds no weight rule). Python: both imports; ic-shrink `attach` runs before `composition_resid.apply` (the block needs the rerank-true `theme_standardise`); `test_research_spec.py` carries r11 and `FIT_APPENDED`. |

### Part-B edits (ruled)

| commit | item | what |
|---|---|---|
| `dbfc09bc` | 1 (E-27b, E-35a) | `pooled_aim_weights`, `POOLED_AIM_TEXT` and the pooled `elif` deleted: the era pool's aim rule is ew-theme-aim-v2, fitted by `composition_rules.ew_theme_aim_v2` (FIX-3's shared `theme_gain_weights`) exactly as the single window. `POOLED_COMPOSITIONS` lists aim-v2 in place of aim-v1. A pooled `--composition ew-theme-aim-v1` is refused before any read: "...is not implemented by the pooled fit: it is the v5 rule (gains normalised across themes); Ruling E-27a's rule on an ew-theme-v1 parent is ew-theme-aim-v2 (Ruling E-27b)". The one-era equality test is un-skipped and parametrised: **passes for ew-theme-std-v1, ew-theme-std-aim-v1 and ew-theme-aim-v2** (no skip left); new refusal test. |
| `8ea2e7bf` | 2 (E-45) | `r10.json` map is `{ew-theme-std-v1: ic-shrink-v1, ew-theme-std-aim-v1: ic-shrink-aim-v1}`; the ew-theme-v1 and ew-theme-aim-v1 entries are gone, the description says E-45. `test_r10_derives_its_rule_from_the_parent_and_runs_the_w_pass_at_3072` pins the map; B0c (ew-theme-v1) and R-3-on-B0c (ew-theme-aim-v2) parents refuse at load ("maps the parent's value"). No v8 spec or template maps ew-theme-aim-v1 (r3 names it only as refused). |
| `393910ed` | 3 (E-44, E-45) | **Behaviour change.** ORTH's `composition_resid.resid_block` required `theme_standardise.rule == ew-theme-std-v1` (written before COMB2's table), so the fitter refused `--theme-resid` on an accepted R-10 parent (rule ic-shrink-v1 / -aim-v1, rerank true) while the C++ runner accepts any rerank-true row. E-44 runs slots 49-51 on the last accepted parent; E-45 and ORTH's rule 6 define R-11 on any rerank-true `theme_standardise`. New `STANDARDISE_RULES = (ew-theme-std-v1, ic-shrink-v1, ic-shrink-aim-v1)`, tested equal to `(composition_rules.STD_RULE_ID,) + composition_ic_shrink.RULES`; no block, rerank false or an unknown rule is still refused. New test: `--theme-resid` on an ic-shrink-v1 fit attaches the block; the file minus block and `provenance.resid` is the ic-shrink-v1 file byte for byte. |
| `f6288685` | 3 (PM4-4, E-44, E-45) | `r11.json` acceptance now reads as r10's: "paired S2 net dSR > 0 against the parent AND mechanics AND planned turnover per unit gross not higher than the parent's (the composition-cell criterion of R-1, plan 12.1)"; the domain text names the rerank-true parents and E-45's skip. Refusal of a parent without a rerank-true `theme_standardise` confirmed (fitter `test_refused_without_a_standardised_parent`, `test_attach_refuses_rerank_off_and_unregistered_themes`; C++ `composition_residualise`). `test_r11_appends_theme_resid_to_the_parents_fit` asserts both templates carry the same criterion text. |
| `486aa4f3` | 4 | `PRIOR_COMPOSITIONS` is one tuple in registration order (values and order unchanged), the list of record; `COMPOSITIONS = (mv-shrink, netcost) + PRIOR_COMPOSITIONS`; `AIM_RULES` one tuple. `test_declared_constants` pins the derivation, `AIM_RULES`, `POOLED_COMPOSITIONS`, `composition_ic_shrink.RULES` and `STD_RULES` inside `PRIOR_COMPOSITIONS`, aim-v1 outside the pooled list, no duplicate id; `test_research_spec` pins `research_cycle.V5_AIM_RULE` / `V8_AIM_RULE` to the fitter's ids. |

### Builds (`scripts/research-build.ps1 -Preset equity-dev`)

| tag | source | targets | result |
|---|---|---|---|
| v8-10 | `486aa4f3` clean | atx-impl-strategy-ic-tests, -strategy-target-tests, atx-engine-book-tests, atx-engine-combine-tests, atx-impl-tests, -strategy-tests, -strategy-mine-tests, atx-engine-factory-tests, atx-equity-strategy-targets, -ic, -risk, -mine | ok, exit 0, 86.4 s, 47 TUs, 12 links, 4 jobs; reconfigured (glob mismatch: new sources); receipt `4fb85a9c...e286` |
| v8-10a | `9c5cfa0c` clean | atx-impl-strategy-ic-tests, atx-impl-tests | ok, exit 0, 12.8 s, 2 TUs, 2 links; receipt `906ea731...5f5c` |

**The four lanes' C++ (FIX-2 spo-v3 / NAV hook / book test, COMB2 kernel + rule + runner wiring + tests, ORTH kernel +
rule + runner wiring + tests) compiled first time under `/W4 /WX`: 0 compile fixes, 0 warnings, 0 errors in the log.**

Executables (v8-10 unless noted): targets `47d51210...cb43`, ic `b082a3a0...f352`, risk `3e0b630b...f9b9`, mine
`04317818...0bb1`, target-tests `34ba09c4...228d`, book-tests `9796cfd8...871b`, combine-tests `0857efde...467c`,
strategy-tests `3096ebe7...9943`, mine-tests `66e44aad...a07f`, factory-tests `3e8d84f2...9817` (unchanged since v8-9a);
v8-10a: ic-tests `5ad7390f...438c`, impl-tests `572c2699...0416`. `git diff --stat 486aa4f3 9c5cfa0c`: 3 files, all
tests (two Python, `strategy_ic_runner_test.cpp`); no executable's source changed after v8-10.

### Test fixes (no compile fix)

| commit | test | reason |
|---|---|---|
| `25295a5a` | `test_prepare_research_fields_module_reuse.py::test_seal_move_recomputes_every_holdings_field` (FIX-2) | iterated `hold.HOLD_FIELDS`, which now holds LIB2's `exch_up_365d`, a v12 field outside the 63-field RECIPE the fixture builds (StopIteration). Now iterates `HOLD_V9`, the file's own recipe constant: the same N-1 rule on every field the run builds. |
| `25295a5a` | `test_research_fields_holdings_xsw.py::Reuse::test_self_copy_pin_change_and_fingerprints` (LIB2) | expected `reused_from.inputs == {security_master}`; N-1 pins the seal for every holdings kind (`build_xsw` drops rows at or after it), so the expectation names both pins exactly. |
| `9c5cfa0c` | `CompositionV8.IcShrinkRunsTheStandardisationUnchangedAndRecordsItsRule` (COMB2, never run by the lane) | the validation manifest's `orientations_artifact_sha256` is the SHA-256 of the run's `orientations.json`, which records the run's `recipe_sha256`: a recipe link like `run_recipe_sha256`. The test now asserts it equals each run's own `orientations.json` SHA (null on train) and erases it with the other pins; every other key must still be equal. |

Each is a premise slip (two of them created by the merge order: FIX-2 and LIB2 were written apart); each still asserts
the registered rule exactly. No pin, golden or expected hash was edited.

### Tests

| exe / suite | build | result |
|---|---|---|
| target-tests `SpoV3.*:SpoPin.*:SpoHook.*:SpoTripwire.*:NavV7Hook.*:CostV2Capacity.*:AdvHold.*` (FIX-2) | v8-10 | **49/49**; pins hold: v1 `0xda6b6871e7e267c5` / `0xaabdbb72f99a6e13`, v2 `0xb039820b40d5cf24` / `0xd24b61721a7c698c` (the `3bfd293e` values) |
| book-tests `TargetTracking.*` (FIX-2 T-4) | v8-10 | 10/10 |
| combine-tests `GroupShrink.*:GroupCap.*:GroupRerank.*:GroupResidualise.*` (COMB2, ORTH) | v8-10 | 18/18 |
| ic-tests `StrategyIcComposition.*:CompositionV8.*` (FIX-2 T-1) | v8-10a | 17/17 (v8-10: 16 + the IcShrink failure fixed above) |
| ic-tests `IcShrinkV1.*:IcShrinkAimV1.*:GroupShrink.*:GroupCap.*:CompositionV8.*:StrategyIcComposition.*:StrategyIcRunner.*:MarginalIc.*` (COMB2) | v8-10a | 89/89 |
| ic-tests `GroupResidualise.*:ThemeResid.*:ThemeResidRunner.*:CompositionV8.*:StrategyIcComposition.*:StrategyIcRunner.*:MarginalIc.*` (ORTH) | v8-10a | 86/86 |
| impl-tests `SpoV3.*:SpoPin.*` | v8-10a | 17/17 (same pin lines) |
| atx-impl-strategy-target-tests (whole) | v8-10 | **256/256** (253 + FIX-2) |
| atx-engine-book-tests (whole) | v8-10 | **155/155** (154 + T-4) |
| atx-impl-strategy-tests | v8-10 | **46/46** |
| atx-impl-strategy-ic-tests | v8-10a | **139/139** (105 + 34: COMB2, ORTH, FIX-2 T-1) |
| atx-impl-strategy-mine-tests | v8-10 | **18/18** |
| atx-engine-factory-tests | v8-10 | **387/387** |
| atx-engine-combine-tests (whole; no baseline in the brief) | v8-10 | 233/233 |
| atx-impl-tests (run from the repo root) | v8-10a | 1,009 run: **1,003 passed, 5 skipped, 1 failed: the known `ConfigJsonNotInDiscoverDigest`** (979 + 24) |
| atx-impl/strategies | - | **163** passed; `test_generate_library.py` with `ATX_V71_PLAN_JSON=build-equity/v8-i3-plan-v71.json` **9** passed |
| atx-engine/tools (whole) | - | **253** passed, 6 subtests (252 + FIX-2 N-1; before `25295a5a`: 251 + 2 failed) |
| atx-impl/tools (whole; `ATX_EQUITY_BIN`, `ATX_EQUITY_TARGETS_EXE` absolute, v8-10) | v8-10 | **529 passed, 1 skipped** (`ATX_EQUITY_ROOT` unset); ERA's equality test no longer skipped |
| scripts/tests (whole; `ATX_EQUITY_BIN` absolute, v8-10) | v8-10 | **178 passed, 3 skipped** (the three RESEARCH_CYCLE_LIVE_ROOT tests); tiny_world `test_cycle_e2e.py` in it: no golden moved, `git status` clean after |

impl-tests skips: Alpha101Orats x2, AtxImplDiscover.W6, SingleAlphaCapacity, FundamentalZoo (environment gates).

### Identities (Ruling PM4-3; bounded runner, one at a time, clean tree `9c5cfa0c`, exes of v8-10)

Every run: `python scripts/run_bounded_research.py --output build-equity/v8-i6b-<id>-run --seconds 180 --max-rss-mib
1536 --min-free-mib 512 -- <argv>`, the argv read from `build-equity/v8-i5c-<id>-run/receipt.json` with only the
output paths renamed `v8-i5c-` -> `v8-i6b-`. Every receipt: outcome completed, exit 0, `git: clean in the code
pathspec`, source `9c5cfa0c`. The child's console went to a file that was never read; only receipt fields, file
SHA-256s, JSON paths (no values) and a digit-masked line diff were looked at.

| id | receipt.json SHA-256 | exe | wall s | peak MiB | result |
|---|---|---|---|---|---|
| 1a | `e71eb8d9fa2ca8b46afa648f77daddbd48b807775d831467fecc455f6ddd2b0f` | targets | 14.1 | 359 | **PASS** |
| 1b | `f9b9607d971b7e02830acb133a1af9de7a75b1fc01bb30c2b82624b05bdd796f` | targets | 14.3 | 360 | **PASS** |
| 4 step 1 | `2da29bb737f8ba11e22d18e5cb4f96b22ce549e89b6135957c6da218a3e5e07f` | python | 0.3 | 5 | weights equal; module SHA differs (declared) |
| 4 step 2 | `49f7e061b964b23b433aad4c0a6d19f9c71e326d74aa233245968572ea534fba` | ic | 18.6 | 507 | **PASS** |
| 7 | `72f294b7544ab4d9cb7b66a4445563405c609c11825fc13d81cf7851ba555b6a` | targets | 34.5 | 360 | **PASS** |
| 8 | `db74e3118041ff4cedf2a5b8e6194b0d3565264fab2185ef415058a9b944ed87` | targets | 13.2 | 464 | **PASS** |

- **1a** (V71 argv, `build-equity/v8-i6b-i1-nav`): **12 of 12 files byte-identical** to the accepted cell
  `mega-nav-v71u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247` and to part C's `v8-i5c-i1-nav` (primary daily
  `fbec452e...d5f4`, recipe `b956bbcc...34fa`, summary `5b109a70...170a`); `stdout.log` `813ce59c...` equals part C.
- **1b** (`v7-w4-nav-on` argv with `--emit-holdings`): NAV **12 of 12** identical to `v7-w4-nav-on` (recipe
  `2324dd91...0858`, summary `998eca31...fea3`); holdings **4 of 4** identical to **`v7-w4-holdings`**
  (`holdings.f64` `ce5523c6...eff4`, `holdings_days.csv` `43d9d2fe...3857`, `holdings_index.json` `aef15691...fde1`,
  `manifest.json` `532044d5...e141`); `stdout.log` `b40bd0cc...` equals part C.
- **4 step 1** (`composition_rules.py identity-weights`, out `build-equity/v8-i6b-i4-identity-weights.json`):
  `d49e208c...2eae` against part C's `88635696...1d7e`. **Exactly one JSON path differs:
  `provenance.std_identity.module_sha256`** (composition_rules.py changed in FIX-3: `theme_gain_weights`, aim-v2);
  weights, signs and the `theme_standardise` block (rerank false) are identical. A declared tool-fingerprint link,
  not a weight change.
- **4 step 2** (the exact part C argv: `--composition-weights build-equity/v8-i5c-i4-identity-weights.json
  --composition-weights-sha256 88635696...`, only `--output build-equity/v8-i6b-i4-w` new):
  - against part C's `v8-i5c-i4-w`: **10 of 12 byte-identical** (`recipe.json` `15b200d8...6410`,
    `orientations.json` `6d0d1be9...849d`, `train_combined.json` `2831faf0...3e17`, every payload and both CSVs);
    `summary.json` (201 JSON paths) and `train_candidates.jsonl` (96 records, 192 paths) differ **only in timing
    paths** (a check found 0 other paths);
  - against the accepted `mega-v71w-train-ew-1`: `train_combined.f64` `1cf245b1...3912`, `_member.u8` and `_finite.u8`
    `732f47b7...f1a4`, `_ids.u64` `102e89c6...741c`, `_sessions.i64` `89af5340...2830`, `train_planned_targets.csv`
    `e6dbd9a8...5297`, `train_daily_ic.csv` `7e6e596f...bce6` **byte-identical**; recipe, orientations, combined json,
    summary and candidates differ by the hash links and timings part C recorded.
- **7** (spo-v2 argv, lo3): **9 of 9 byte-identical** to the recorded cell `mega-nav-v70-lo3-spo-v2-G1.0` and to part C
  (`spo_diagnostics.csv` `1131cd59...7dc2`, `v7_transfer_coefficient.csv` `a27505c2...1bb0`, `v7_extras.json`
  `d96be152...42eb`, recipe `d6ffb028...58c8`, summary `75caf7f6...c44f`). FIX-2's E-31a primary-book void and the
  `--spo-iters` / `--spo-tol` refusal bind spo-v3 only: spo-v2 with `--spo-iters 500 --spo-tol 1e-8` ran unchanged.
  `stdout.log` differs from part C in two lines only (digit-masked diff): the solver-timing line and the output path.
- **8** (V71 plus `--label-role` = `--role`): the **10 daily and events CSVs byte-identical** to 1a; `recipe.json`
  `10961936...6635` and `summary.json` `6753c13c...5c00` (E-25's declared `label_role` keys) **byte-identical to part
  C's `v8-i5c-i8-nav`**: 12 of 12 against part C; `stdout.log` `d8ee4a6a...` equals part C.

### Hidden-data record

- Inputs: the TRAIN 2020-2022 roles (lo1 `3e79978a`, lo3 `40e3d832`), their fields-v7 / v9, the accepted v6.1, v7.0-lo3
  and v7.1 artifacts, `v7-w4-*`, the lo3 risk model `786cb601`, `mega-candidate-cache-v71`, part C's outputs, and
  `build-equity/v8-i3-plan-v71.json` (the plan-only file of earlier integrations). Tests used synthetic fixtures and
  tiny_world.
- Read: the four lane reports, rulings, sources, build receipts and logs, runner receipts (outcome, exit, timings,
  log SHA-256s), output-file SHA-256s, JSON paths of differences (no values) and a digit-masked line diff of two stdout
  logs. No return, Sharpe or IC statistic was read or printed.
- A scan of every `v8-i6b-*-run` stdout and stderr and of the runner consoles for dates in 2024 or later found only the
  runs' own `started_utc` wall-clock (2026-10-01).
- **Nothing dated 2024-01-01 or later was opened. No disclosure.**

### Open items

- Behaviour change for the PM to confirm: `393910ed` lets `--theme-resid` attach on R-10's ic-shrink-v1 /
  ic-shrink-aim-v1 parents (E-44 / E-45 / ORTH rule 6; the runner already accepted them).
- R-10 under `--era`: COMB2's registration says the IC estimate uses "the pooled era decisions" under `--era`, but
  `POOLED_COMPOSITIONS` (ERA, E-35) does not list ic-shrink-v1 / -aim-v1, so a pooled R-10 fit is refused by name.
  Nothing in v8 plans one; root to rule if an OD-3 history read of a V8-F on an R-10 parent is planned.
- Identity 4's step-1 weights file now carries the new `composition_rules.py` module SHA (`d49e208c`); any later
  identity that regenerates it will differ from `88635696` in that one path.
- FIX-2 carried: the E-31a CLI path (exit 3, `v7_extras.json` voided / limits_unmet) is not tested end to end;
  `prepare_research_fields.py` `REUSE_MODULE_RULE` text and the not-reused reason do not name the seal (N-1 deviation);
  fields dirs built before `6ed5fef8` recompute their 9 holdings fields once on the next `--reuse` (manifests gain
  `seal_date`).
- FIX-3 carried: untouched minors F-4, F-7, F-11, F-12, F-13; the fitter has no protocol input (aim-v1 is refused for v8
  in research_cycle spec validation, not in a direct fitter call).
- Next per PM3-9 / PM4-2: Wave 0 part 2 (R10-R14) on this head; MINE-FIX (integration 7) only after the V8-F freeze.
- Known: `ConfigJsonNotInDiscoverDigest` (1).

## Wave 0 part 2a (R10, R11, field overlap) (2026-10-01)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `ebb2070a` (code head `9c5cfa0c`, clean).
Dispatch 2a of Ruling PM4-14: R10 (fields v9 on lo1), R11 (fields v9 on lo3), R13 report 1 (field overlap) only.
No merge, no build. Not run: R12, the u pass, the signal and daily IC overlap reports, R14, R15, any cell.

### Preconditions

1. **Disk:** `df -h /c` 58 G free of 458 G (59,926,588 KiB) before R10 (>= 30 GB). RAM available 5,702 MiB of
   16,068 MiB.
2. **Runbook section 1:** `research_window.SEAL_DATE` prints `2024-01-01`; `p.SEAL, f.SEAL, h.SEAL, b.SEAL` print
   `2024-01-01` four times; `WINDOW_ID` `research-window-v2`; `partition_is_sealed(2024, 1)` True, `(2023, 4)` False.
   `atx-impl/strategies/research_window.json` SHA-256 `62cf2cfab1d0f61b02731a807ccfdd326bb2df91caff1173a3cf1b37e6e63584`
   (= part 1). Vendor file unchanged: 3,617,973,507 B, mtime_ns 1789920127331396300.
3. **Executables:** build v8-10 receipt `build-equity/mega-v8-10-receipt.json` SHA-256
   `4fb85a9ce1bf3215597cffd41f458d08c2c57f0753ecbd8e4d1e50a12838e286` (source `486aa4f3`, ExitCode 0); v8-10a receipt
   `906ea731b85cc2721585d3e82dccfffc2af6fd8516f01f4cd166207352475f5c`. On disk equal to the v8-10 receipt: ic
   `b082a3a0d801e574f777e7ce8e54cfa5b301c087c36c068c776ea72c23e2f352`, targets
   `47d51210e40bc1409d37011f70a2fcf2edb2d0f0216c488886d50ad96d02cb43`. `git diff --stat 486aa4f3 ebb2070a` outside
   `.superpowers/`: 3 test files only. **Dispatch 2a runs no C++ executable** (fields builder and overlap tool are
   Python): the runner's `executable_sha256` is python.exe
   `624bbc0586d8855633b875e911883bbef8a0e8b8711e11126df480dd86f54181` (= part 1). Nothing was built.
4. **Input pins** re-hashed at 2026-10-01T10:44Z, equal to part 1: `LO1` `2ff9d771...1e53`, `LO3` `e1c67101...95f4`,
   `R4` `f598c04c...43de`, `FEV` `304d2945...be87b`.

### Live stage manifests (Ruling W0-n), re-hashed 2026-10-01T10:44:41Z, immediately before R10

| stage (`C:/atx/atx-db/data/alpha_panel/v1/...`) | manifest SHA-256 | runbook pin / R8 |
|---|---|---|
| export/identity-bridge-v2-pit | `09aac28f757fa959b0ed4cd9296b2267940e70af98e0d2c67cc45b1df4f7fa01` | equal (R8 equal) |
| earnings_calendar | `9a4a976b03d0ad04672796f01abc129d0d09e3db23d62ddf57aea68ae3c7d769` | equal |
| insider | `dcd3f1aa4ba6e266c03ef78568ca131c1336f51ade477f03faff2e88a62ba061` | equal |
| sec_filings | `5190fe99e4c2f1f13218d966a67d06995d51b7a31a73151dad2686e829aed693` | equal |
| thirteenf | `8974170f64b4a002cc1b131449c2abf0c7daaab23d4a256992afbdfc4105ffb0` | equal |
| ftd | `a76d69bed49829d9e14216f1abe2ef76b480c16c576fa49ae63fa2a28050f945` | equal |
| regsho_threshold | `fb073c6222cb16cb27968c067d45ecd2ffe50cf10958391cae8d6417ad9e6f4f` | equal to the runbook's live pin (W0-n pins it); the v7.1 fields pinned `68f431f0...` |
| security_master | `3afe06605adac9aa85494cc5d1e7307194392954ad3b6ba8142b281fa62a414a` | equal |
| short_volume_ext | `7007a13c226a1730d0ef778d7201bf7c1d0e97ed61d4c12af32c1c4567444928` | equal |
| fundamentals (SIC) | `9f9b2f85f6bcd5c7f3a55aee097893094a5cb85ab2b4edbfb582297dab06816b` | equal (R8 equal) |
| delisting | `1b1166b61e5a77d8dbe007f2de3261392424fb86a59c1118028862abc264c37f` | equal (R8 equal) |
| FINRA SI `asof/manifest.json` | `a2561d758b70f5e7ecfae4ac3721b317758da89b1d5844f8d1d29ea9c29cb4df` | equal |
| raw short volume `manifest.csv` | `8b076a16701096da19d337d31468bbec0baae21c66d4e2d6cd53189d6a851c81` | equal to the v7.1 fields' source pin |

No live stage hash differs from the one recorded at R8 or from the runbook pins.

### Runs

`scripts/run_bounded_research.py` on a clean tree (source column), one at a time, argv = runbook section 3 R10 / R11
verbatim (reconstructed argv; runner 600 s / 2,560 MiB / floor 512 MiB, the runbook's number and the brief's
preparation cap; builder `--max-rss-mib 2048 --max-seconds 580`). Argv digest = SHA-256 of the receipt's `command`
array as compact JSON (`json.dumps(command, separators=(",", ":"))`). Peak = sampled peak tree RSS.

| step | receipt dir | source | outcome / ExitCode | s | peak MiB | argv digest | receipt.json SHA-256 | output manifest SHA-256 |
|---|---|---|---|---|---|---|---|---|
| R10 | `train-2020-2023-lo1-fields-v9-run` | `814c2c0a` | completed / 0 | 166.8 | 960 | `0a5e19898e32d46cd095b155b4a8637d8d2195dc301945577d4fd7d380812a57` | `50ca556f5f2bb4c6a99d8b554ee639d90760524aa4378e81e92b06c4d1ec42e9` | `888e6616e441e863a9f91234124e1aebc907db11e18d9789d3c583cf447b8695` |
| R11 | `train-2020-2023-lo3-fields-v9-run` | `2b80cf9e` | completed / 0 | 154.6 | 1,014 | `4e828bff080775cdf6183c013671e4d541faabfa7aeff557ac7338fba2c82870` | `5675df067c2022a31d4376ccbb05e9cdfd7eef04d025a5af5b77a25df5b39a9d` | `9f1563638b5e4f7ead7be686803b96a0707ada2c608fcbc6dc084179bd9021ef` |
| R13 (1) field overlap | `w0-2-overlap-field-lo1-run` | `75567df3` | completed / 0 | 20.9 | 162 | `4cb746ca45533ecad832d6d025437945881419aa5167b3b0b10732b522aa4490` | `b9a2ee911a2952b6d99f53c1e84048c013f1d800f86a44bb25edb023de4551d8` | report `w0-2-overlap-field-lo1.json` `fafdc20f5397dca48df83f0787e7b0408ee5aeaf8e579be7d9996b587b212c77` |

- **R10 PASS.** Bindings: `prepare_research_fields.py` `b44cff42...`, `research_fields_sec.py` `27034019...`,
  `research_fields_holdings.py` `edfd1967...`, role lo1 `2ff9d771...1e53`. Runner min free 4,407 MiB. Manifest:
  status complete, `seal.exclusive_end` 2024-01-01, role `2ff9d771` 1,405 x 5,922 (2018-06-01..2023-12-29,
  score 399..1405); **63 fields, names in F63 order (True)**; `reuse` null (no `--reuse`: all 63 computed); 79 source
  paths, **none 2024-named**; stage pins in the entries = the live hashes above (regsho `fb073c62`), bridge r4-v2
  `f598c04c`, fundamental events v3 `304d2945`, FINRA SI `a2561d75`. Code `code_sha256_lf` `74df97f9...`, blob
  `e8b57af5`. 4,000 MiB on disk (runbook estimate ~4,120). stdout/stderr scanned for dates 2024+: 0 hits.
  Coverage acceptance against the v7.1 fields (`8fd00e9f`) on 2020-2022 member cells: **62 of 63 within .02**; the
  exception is the runbook's expected one, `regsho_threshold_days63` finite member fraction 0.430 / 0.444 / 0.424 ->
  1.000 / 1.000 / 1.000 (2020 / 2021 / 2022; republished stage, blocker 1).
- Stage manifests re-hashed again at 2026-10-01T10:50:03Z, immediately before R11: all eleven equal the table above.
- **R11 PASS.** Same bindings except role lo3 `e1c67101...95f4`. Runner min free 4,396 MiB. Manifest: status complete,
  `seal.exclusive_end` 2024-01-01, role `e1c67101` 1,405 x 5,922 (2018-06-01..2023-12-29, score 399..1405); **63
  fields, names in F63 order (True)**; `reuse` null (all 63 computed); 78 source paths, **none 2024-named**; pins =
  the live hashes above, issuer bridge and SEC bridge v2-pit `09aac28f`, SIC from the fundamentals stage `9f9b2f85`,
  fundamental events v3 `304d2945`. Same code identity as R10 (`74df97f9...`, blob `e8b57af5`). 4,000 MiB on disk.
  stdout/stderr scanned for dates 2024+: 0 hits. Coverage against the 3-year `recent-fast-train-2020-2022-v2-lo3-
  fields-v7` (41 common names): **all within .02** on 2020-2022 (largest gap 0.00055, `me_company` 2022). The 22 W5a /
  W5b names have no 3-year lo3 reference (open question 30): reported only, not checked.

### R13 report 1: field overlap, fields v9 lo1 against the v7.1 fields (Ruling W0-a)

Argv = runbook R13 field line plus `--per-key` (one row per field, needed for the field-by-field classes; it adds rows
to the report and changes no comparison): `compare_window_overlap.py --kind field --old
build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9 --new build-equity/train-2020-2023-lo1-fields-v9 --out
build-equity/w0-2-overlap-field-lo1.json --per-key`; runner 180 s / 1,536 MiB / 512 (the brief's default cap). Bindings:
tool `b4d7f2e8...`, old manifest `8fd00e9f...` (= `scripts/specs/v71.json` `fields.manifest_sha256`), new `888e6616...`.
Payload SHA-256s verified against both manifests (no `--no-verify`); no `--before`.

- **Alignment:** 1,155 common sessions (2018-06-01..2022-12-30: the v7.1 role's whole axis, warm-up included, so a
  superset of the 2020-2022 cells), 5,627 common instruments (**old-only instruments 0, old-only sessions 0**),
  new-only 295 instruments and 250 sessions (counted only).
- **Totals:** 63 keys, 63 compared, **409,448,655 cells compared** (6,499,185 per field, no key without cells),
  `old_cells_missing_in_new` 0, `unequal_cells` 3,006,158 of which `nan_mismatch_cells` 3,006,153, **`max_abs_diff`
  5.0**, `max_rel_diff` 1.0, `bit_identical` **false**, **`w0a_class` stop**. One differing key.
- **First differing field and cell:** `regsho_threshold_days63`, session 2018-06-04 (ns 1528070400000000000),
  instrument id 2234.

Per field class (every field is reported; "identical" = every one of its 6,499,185 common cells has the same IEEE-754
bits or NaN on both sides):

| class (why it could differ) | fields | W0-a class | numbers |
|---|---|---|---|
| FINRA short interest | si_shares, si_dtc (2) | identical | 0 unequal |
| TickerHistory3 (reader-side seal) | iv_atm_21d, iv_atm_63d, iv_atm_126d, earn_recent, shares_out (5) | identical | 0 unequal |
| role cross-section, wider instrument axis (open question 10) | mkt_ret | identical | 0 unequal |
| issuer fields on the rebuilt sealed inputs (bridge r4-v2 `f598c04c` for r4-v1, fundamental events v3 `304d2945` for v2) | be .. fscore (28) and grp_sic2, grp_ff12, grp_ff49 (3) | identical | 0 unequal |
| role-line dependent (open question 9): sum over the issuer's role lines / role ticker map | me_company, sv_ratio126 | identical | 0 unequal |
| SEC fields, clock text names the seal (W0-1) | ea_* (6), ins_* (5), k8_* (3) (14) | identical | 0 unequal |
| holdings, seal bound by N-1 (FIX-2 `6ed5fef8`; v7.1 built under seal 2025-01-01) | inst_own_share, inst_breadth_chg, inst_own_chg_q, inst_best_ideas, inst_n_holders, ftd_shares_ratio21, sv_offexchange_share126 (7) | identical | 0 unequal |
| holdings, seal bound by N-1 **and** stage republished (W0-n: live `fb073c62` against v7.1's `68f431f0`) | **regsho_threshold_days63** | **stop** (> 1e-9) | 3,006,158 unequal of 6,499,185 (46.3%): 3,006,153 NaN against a value, 5 finite value changes, max_abs_diff 5.0, max_rel_diff 1.0; first cell 2018-06-04 / id 2234 |
| calendar-pinned price fields (PM3-5a) | none: not in the 63-field list, not built, not compared | n/a | - |

Reading (facts only; the ruling is the PM's):
- 62 of 63 fields are bit-identical: no field reached the below-1e-9 class. The rebuilt bridge and fundamental events,
  the wider instrument axis (mkt_ret), the role lines (me_company, sv_ratio126), W0-1's SEC clock text and N-1's seal
  pin left every common cell of those fields unchanged; for ftd_shares_ratio21 and sv_offexchange_share126 this is
  N-1's "unverified" point settled on this role (no row available between the two seals moved a 2018-2022 cell).
- The one difference is the field the runbook named in advance (blocker 1, R10 coverage note). The coverage shift
  0.43 -> 1.00 on 2020-2022 member cells is consistent with the NaN mismatches being v7.1 NaN against a v9 value. No
  v7.1 candidate reads the field: library `787c802e` does not name it, and the v7.1 recipe lists it "at 0 trials"; in
  the v8 specs it appears only in the 63-name `fields.list`.
- Not separated here: how much of the difference is the stage republish alone. Integration 3 identity c built the
  3-year lo1 fields under seal 2024 on the live regsho pin (`build-equity/v8-i3p4-c-fields2`, manifest `5e5def8d...`):
  62 of 63 payloads byte-identical to v7.1, regsho differing (`031016d4` -> `2cb2fe40`). A field overlap of that
  directory against the 4-year lo1 fields would show whether the 4-year regsho cells equal the 3-year live-pin cells
  (i.e. whether the whole difference is the republish). **Not run** (not named in this dispatch).
- Per the brief, a stop class ends the overlap sequence: reports 2 and 3 are dispatch 2b's and were not run.

### Disk

C: free 58 G (59,926,588 KiB) before R10, 50 G (51,713,548 KiB) after the overlap report. The two new fields
directories hold 4,000 MiB each (runbook estimate ~4,120); run dirs and the report under 1 MiB each. RAM available
5,700 MiB before the overlap run.

### Fixes

None. No source file changed; no tool failed. The only commits are this log (`814c2c0a`, `2b80cf9e`, `75567df3` and
this one, `git add -f`).

### Hidden-data record

- **Inputs opened by the tools:** the vendor file and the FINRA short-interest asof files (reader-side seal); the raw
  CNMS short-volume files of the role's sessions; the atx-db stages at the pins above (multi-year non-partitioned files
  filtered at read; sealed partitions never opened: insider quarters from 2024q1, 13F `parts/source=` from 2024q1,
  `year=2024+` FTD / threshold / short-volume-ext files, `rw.partition_is_sealed`); the v2-pit bridge, bridge r4-v2,
  fundamental events v3, roles lo1 / lo3; for the overlap, the two fields directories and the two roles, refused by
  the tool on any session on or after the seal (none: last session 2023-12-29). Whole-file pin hashing of multi-year
  stage manifests is the runbook's design (open question 25).
- **Logs:** R10, R11 and overlap stdout / stderr scanned for dates in 2024 or later: 0 hits.
- **What I read:** receipt fields, manifest keys (seal, role block, names, source paths, stage pins, reuse block,
  per-year coverage fractions for 2020, 2021 and 2022 only), the overlap report (alignment, totals, per-field cell
  counts and difference sizes on the 2018-06-01..2022-12-30 common cells), the v7.1 library and recipe text for the
  regsho name. **Disclosure:** while R10 ran I read its first 20 stdout progress lines; they include two sealed-row
  counters (`si_shares-parsed` / `si_dtc-parsed` `sealed` 650,045 / 649,897 FINRA rows, and a TickerHistory counter
  key named `rows_on_or_after_2025...`, value not read in full): row counts of data past the seal (open question 24),
  not a return, Sharpe, IC or value statistic; nothing uses them.
- **No return, Sharpe or IC statistic was read, computed or printed. Nothing dated 2024-01-01 or later was opened.**

### Open items

- **Ruling needed (W0-a, Ruling W0-n):** the field overlap class is **stop** on one field, `regsho_threshold_days63`
  (46.3% of common cells NaN against a value, 5 value changes up to 5.0; first cell 2018-06-04 / id 2234); 62 fields
  bit-identical. Options the PM may weigh: accept it as the declared republish (no v7.1 candidate reads it) or ask
  first for the decomposition run above (`--old build-equity/v8-i3p4-c-fields2`, ~21 s).
- R11 has no overlap report (dispatch scope: report 1 is lo1 only); its 22 W5a / W5b fields have no 3-year lo3
  coverage reference (open question 30).
- Not run (dispatch 2b): R12 plan-only, the cold u pass, the signal and daily IC overlap reports, R14 (pins into
  `v8-prereg.md`, protocol line, `lock --write`); R15 and every cell. Pins for R14: fields v9 lo1 `888e6616...b8695`,
  lo3 `9f156363...021ef`.
- PM4-14's check: if integration 6 part C changes a field module fingerprint, fields v9 is rebuilt once (FIX-4 lists
  none); R10 / R11 bound `prepare_research_fields.py` `b44cff42`, `research_fields_sec.py` `27034019`,
  `research_fields_holdings.py` `edfd1967`.

## Wave 0 part 2a: regsho decomposition (Ruling PM4-15) (2026-10-01)

Same integrator, root `C:/atx-wt/pool-2`, start `9f593ef7` (PM4-15 ledger commit, clean). One bounded run; nothing else.

### 1. The reference build is on the republished stage (PM4-15 item 2)

`build-equity/v8-i3p4-c-fields2` (integration 3 identity c, run 2), manifest `5e5def8dfddca125b9c4c400d94dcc8873cdc417fa8a98e51898b559472be699`:
its `regsho_threshold_days63` entry pins **`regsho_threshold` `fb073c6222cb16cb27968c067d45ecd2ffe50cf10958391cae8d6417ad9e6f4f`
= the live (republished) stage**, the same pin as fields v9 lo1; `seal.exclusive_end` 2024-01-01; role the v7.1 3-year
lo1 `3e79978a` (1,155 x 5,627). The test therefore separates the causes. Entry metadata of the field in the three builds:

| item | v7.1 fields `8fd00e9f` | i3p4-c-fields2 `5e5def8d` | fields v9 lo1 `888e6616` |
|---|---|---|---|
| regsho stage pin | `68f431f0...` (old) | `fb073c62...` (live) | `fb073c62...` (live) |
| source files `lists.parquet`, `year=2018..2022/threshold.parquet` | v7.1 bytes (`53516f65`, `e0015576`, `cbf46515`, `bf80c89b`, `aca16c04`, `3a718282`) | republished bytes (`4ebd1881`, `060b20bb`, `0effca3c`, `55973ec7`, `dc757417`, `b835e62d`) | the same republished bytes, plus `year=2023` `e05c7f91` |
| security_master pin / `finra_names.parquet` | `3afe0660` / `1acfed75` | equal | equal |
| seal | 2025-01-01 | 2024-01-01 | 2024-01-01 (entry `seal_date` 2024-01-01, N-1) |
| role sessions | 1,155 to 2022-12-30 | 1,155 to 2022-12-30 | 1,405 to 2023-12-29 |
| holdings module (producer) blob | `8ba94e78` | `32ed34d3` | `b5bde414` (N-1 + LIB2 xsw) |
| `formula_id`, `definition`, `clock`, `visibility_rule`, `staleness`, `source_columns`, `caveats` | equal in all three | | |

### 2. Decomposition overlap

`compare_window_overlap.py --kind field --old build-equity/v8-i3p4-c-fields2 --new build-equity/train-2020-2023-lo1-fields-v9
--out build-equity/w0-2-overlap-field-lo1-i3p4.json --per-key`, runner 180 s / 1,536 MiB / 512; bindings tool
`b4d7f2e8...`, old manifest `5e5def8d...`, new `888e6616...`; payload SHA-256s verified, no `--before`.

| step | receipt dir | source | outcome / ExitCode | s | peak MiB | argv digest | receipt.json SHA-256 | report SHA-256 |
|---|---|---|---|---|---|---|---|---|
| PM4-15 decomposition | `w0-2-overlap-field-lo1-i3p4-run` | `9f593ef7` | completed / 0 | 21.8 | 154 | `15a1d0caa2cd110095c0c24ec2848cf7b9d913a8aee5a00eb92b9400ac258107` | `30cb18a7ee976c6d83ff73a6eef1d34a589b1a9f1c1c2a10a4a454736a798edd` | `afb12af11bf7b646d50e8e7bd32c0a1dd57c163eea57fbf516d2406fc2d8473c` |

- Alignment: 1,155 common sessions (2018-06-01..2022-12-30), 5,627 common instruments, old-only sessions 0, old-only
  instruments 0; new-only 250 sessions, 295 instruments (counted only).
- **`regsho_threshold_days63`: 6,499,185 cells compared, `bit_identical` true, 0 differing cells (0 NaN against a
  value), `max_abs_diff` none, no first differing cell; `old_cells_missing_in_new` 0.**
- **Every other field: 62 of 62 bit-identical**, 6,499,185 cells each (no key without cells). Totals: 63 keys
  compared, 409,448,655 cells, 0 unequal, `bit_identical` true, `w0a_class` **identical**.
- stdout / stderr scanned for dates 2024+: 0 hits.

### 3. Outcome (a)

**The whole `regsho_threshold_days63` difference against the v7.1 fields (3,006,158 of 6,499,185 common cells; 3,006,153
NaN against a value, 5 value changes up to 5.0; first cell 2018-06-04 / id 2234) is caused by the republished
regsho_threshold stage** (`68f431f0` -> `fb073c62`, runbook blocker 1, Ruling W0-n): on the same republished stage the
3-year build and the 4-year build agree on every common cell. Extending the role (wider instrument axis, 250 more
sessions), the seal move (2025 -> 2024) with N-1's seal pin, and the holdings module changes (`32ed34d3` -> `b5bde414`)
moved no common cell of any field. Per PM4-15 the cause is found and the 4-year values are the reference; nothing else
was run.

### 4. Candidates that read `regsho_threshold_days63` (registry and drafts only, no data)

**None.**
- v8 alpha registry `atx-impl/strategies/alphas/registry.json` (`e985aefc`, A-1): 48 alphas and 43 declared fields; the
  field is not declared and no alpha DSL names it (0 occurrences of "regsho").
- v8 library draft `library-v8-draft.md`: 0 occurrences (LIB2's iv_vol_of_vol, day_rev_freq, exch_switch included).
- v7.1 library `787c802e`: 0 occurrences; its recipe lists the field "at 0 trials". `library-v7-draft.md:215`:
  "conditioning input only (0 trials, no library member) ... reserved as a borrow-tier input (special tier for shorts)
  in a separately registered cost trial". No v8 alpha, library draft or spec reads it: in the v8 specs the field
  appears only in the 63-name `fields.list` of `base-lo1.json` / `base-lo3.json`.

### Fixes, disk, hidden data

No fix; no source changed. Disk unchanged (report < 1 MiB). Read: manifest metadata of the field (pins, source-file
SHA-256s, seal, role, producer, definition text) and the overlap report (cell counts and difference sizes on the
2018-06-01..2022-12-30 common cells); registry and library text. No return, Sharpe or IC statistic was read; nothing
dated 2024-01-01 or later was opened. Not run: R12, the u pass, the signal and daily IC overlap reports, R14, R15, any
cell.

## integration 6 part C (2026-10-01)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `67f04389` (clean; code head `9c5cfa0c`,
build v8-10). Tag prefix v8-11. Scope (`task-INT6C-brief.md`): merge FIX-4a and FIX-4b by SHA; build; every suite;
identity 4 (identity 1 only if a NAV source changed). No Wave 0 step and no cell was run.

### Merges (`--no-ff` by SHA, in order; both lanes from `43a0447d`)

| lane (findings) | lane SHA | merge | conflicts and resolution |
|---|---|---|---|
| FIX-4a (R6B-O-1..O-7, R6B-C-1, R6B-C-5) | `98ef8d89` | `c17fb449` | none; 17 files, +1593 / -178. Lane head merged whole, WIP `b8b68f4e` included (finished in `aeda5bd7`), nothing cherry-picked. |
| FIX-4b (R6B-C-2, R6B-C-4, R6B-S-1, R6B-S-2) | `0b093a4c` | `41ef00fb` | one: `scripts/tests/test_research_spec.py` import block (FIX-4a `import inspect`, FIX-4b `import argparse`, `import copy`): union, sorted. `scripts/research_cycle.py` auto-merged and checked against both lanes: FIX-4a's `INPUT_KEYS` `reference_resid_parent` and the fit step's `--theme-resid-parent` binds (O-5) and FIX-4b's `argparse_values` / `validate_e27b` called from `validate_v8_keys` (C-4) are all present. Test functions in the file: base 17, + 1 (FIX-4a) + 3 (FIX-4b) = 21. |

`strategy_spo_cli_fixture.hpp` (FIX-4b, Ruling PM5-4) needs **no CMake change**: it is a header included by quote from
`strategy_spo_test.cpp` and `strategy_spo_v3_test.cpp` in the same directory; `atx-impl/tests/CMakeLists.txt` globs
only `*_test.cpp` and lists sources only. The v8-11 build did not reconfigure and compiled both users in both owning
targets.

### Build (`scripts/research-build.ps1 -Preset equity-dev`)

| tag | source | targets | result |
|---|---|---|---|
| v8-11 | `41ef00fb` clean | atx-equity-strategy-ic, atx-impl-strategy-ic-tests, atx-impl-strategy-target-tests, atx-impl-tests | ok, exit 0, 57.5 s, 17 TUs, 5 links, 3 jobs, no reconfigure; receipt `50416bd5...41db` |

**All lane C++ (FIX-4a O-3, O-4, O-7, C-5 sources and tests; FIX-4b S-1, S-2 tests and the new header) compiled first
time under `/W4 /WX`: 0 compile fixes, 0 warnings, 0 errors in the log.** TUs: atx-impl-core `strategy_ic_library`,
`_admission`, `_runner`, `_composition`, `_result_cache`, `_signal_cache`, `_theme_resid`, `strategy_research_role`,
`strategy_mine_pool` (the last two include `strategy_ic_detail.hpp`; they use none of its changed symbols:
`PinnedWeights`, `method_recipe`, `theme_order_json`); tests `strategy_ic_theme_resid_test`, `strategy_ic_runner_test`
(ic-tests and impl-tests), `strategy_spo_test`, `strategy_spo_v3_test` (target-tests and impl-tests).

Executables (v8-11): ic `46ae5c97...d682`, ic-tests `69f46d28...f570`, target-tests `1e3b26f0...fa96`, impl-tests
`36542729...c896`. **Not rebuilt (v8-10 bytes kept): atx-equity-strategy-targets, -risk, -mine**, because no NAV, spo,
target, book, risk or mine source changed (FIX-4b is tests only; FIX-4a touches the IC runner sources only).

### Tests (no test fix)

| exe / suite | build | result |
|---|---|---|
| ic-tests `ThemeResid.*:ThemeResidRunner.*:CompositionV8.*:StrategyIcRunner.*:GroupResidualise.*` (FIX-4a) | v8-11 | **79/79**; the six new tests pass: `ThemeResid.TiedCompositeStaysTiedAfterResidualisation`, `.NoTieCompositeIsTheRegisteredRuleBitForBit`, `.SmallCaseSeparatesTheRegisteredRegressors`, `ThemeResidRunner.ThreeThemeCycleIsTheRegisteredRule`, `.RidesOnEveryRerankTrueRuleOfTheTable`, `CompositionV8.RecordedRuleMustWriteTheStandardiseBlockBeforeAnyPayloadOrOutput` |
| target-tests `SpoV3.*:SpoTripwire.*:SpoPin.*` (FIX-4b) | v8-11 | **22/22**; pins hold: `[spo-pin]` v1 `0xda6b6871e7e267c5` / `0xaabdbb72f99a6e13`; `[spo-v3-pin]` v2 `0xb039820b40d5cf24` / `0xd24b61721a7c698c` (the `3bfd293e` values) |
| atx-impl-strategy-ic-tests (whole) | v8-11 | **145/145** (139 + 6 FIX-4a) |
| atx-impl-strategy-target-tests (whole) | v8-11 | **259/259** (256 + 3 FIX-4b) |
| atx-impl-tests (run from the repo root) | v8-11 | 1,018 run: **1,012 passed, 5 skipped, 1 failed: the known `ConfigJsonNotInDiscoverDigest`** (1,003 + 9: the same 6 + 3 through the glob); same pin lines; the three new SpoV3 tests pass here too |
| atx-engine-book-tests 155, atx-impl-strategy-tests 46, atx-impl-strategy-mine-tests 18, atx-engine-factory-tests 387, atx-engine-combine-tests 233 | - | **not re-run: targets not rebuilt** (no source of theirs changed; `GroupResidualise.*` of combine ran inside ic-tests, 6/6) |
| `scripts/tests/test_research_spec.py` after both merges | - | **43 passed** |
| atx-impl/strategies | - | **163** passed; `test_generate_library.py` with `ATX_V71_PLAN_JSON=build-equity/v8-i3-plan-v71.json` **9** passed |
| atx-engine/tools (whole) | - | **253** passed, 6 subtests |
| atx-impl/tools (whole; `ATX_EQUITY_BIN`, `ATX_EQUITY_TARGETS_EXE` absolute) | v8-11 / targets v8-10 | **567 passed, 1 skipped** (`ATX_EQUITY_ROOT` unset), 14 subtests: 529 + 38, all in the four lane test files (`test_composition_resid.py` 11 -> 21 defs, `test_fit_composition_weights_pool.py` 19 -> 23, `test_mega_report_v8.py` 35 -> 42, `test_mega_report_v8_render.py` 19 -> 22) |
| scripts/tests (whole; `ATX_EQUITY_BIN` absolute) | v8-11 | **182 passed, 3 skipped** (178 + 4 in `test_research_spec.py`; the three RESEARCH_CYCLE_LIVE_ROOT skips); tiny_world `test_cycle_e2e.py` in it: no golden moved, `git status` clean after |

**R6B-S-1 / open item "E-31a void exit path untested end to end": closed.**
`SpoV3.CliVoidOnPrimaryLimitsUnmetExitsThreeWithTheExtrasOnly` **ran and passed** on the built v8-11 executables
(target-tests, 4.8 s, and impl-tests, 4.6 s). It drives the real CLI dispatch (`dispatch_nav_replay` -> `dispatch_nav_v7`
-> replay -> capture) in process, once with `--specific-ceiling-void on` and once `off`; for both it asserts exit 3,
`<output>` = exactly `spo_diagnostics.csv`, `v7_extras.json`, `v7_transfer_coefficient.csv` (no recipe, summary, daily,
events, NAV or return file), "run VOID" and no "net Sharpe", `status: void`, `voided: limits_unmet`, the
`limits_unmet` block (book = the untiered S2 label, rule citing E-31a, count and first session equal to the tripwire
record and to the primary book's `limits_met = 0` rows of the CSV, count > 0). The lane's premise (volume 0 from
session 30, ADV 0 from decision 93 under the 63-session window, asserted in the test) held: the test passed unchanged,
no test edit.

### Identities (bounded runner, clean tree `41ef00fb`)

Identity 4, argv as in part B, read from `build-equity/v8-i6b-i4w-run/receipt.json` (step 1) and
`build-equity/v8-i6b-i4-run/receipt.json` (step 2), only the output paths renamed `v8-i6b-` -> `v8-i6c-`; runner
`--seconds 180 --max-rss-mib 1536 --min-free-mib 512`. Inputs re-hashed before the run: library `787c802e`, role lo1
`3e79978a`, fields-v9 manifest `8fd00e9f`, v7.1 weights `7b0a59c9`, part C weights `88635696` (3-year role and
existing caches only). Both receipts: outcome completed, exit 0, `git: clean in the code pathspec`, source `41ef00fb`.

| id | receipt.json SHA-256 | exe | wall s | peak MiB | result |
|---|---|---|---|---|---|
| 4 step 1 | `ee62771d6db04d01a8eed159e215b2a1305daebc79d4332d901dd00b454f8e72` | python | 0.3 | 5 | **PASS: weights file byte-identical** |
| 4 step 2 | `dd147a92f03a96dd0d7e8882252bbc90813e0ceb69e49b6b304533da461005f8` | ic v8-11 `46ae5c97` | 26.4 | 506 | **PASS** |

- **Step 1** (`composition_rules.py identity-weights`, out `build-equity/v8-i6c-i4-identity-weights.json`):
  `d49e208c...2eae`, **byte-identical** to part B's `v8-i6b-i4-identity-weights.json` (`composition_rules.py`
  unchanged in FIX-4, Ruling PM5-3, so no module SHA moved). Console: one line, differing from part B's only in the
  `out` path.
- **Step 2** (the exact part B argv, `--composition-weights build-equity/v8-i5c-i4-identity-weights.json
  --composition-weights-sha256 88635696...`, `--output build-equity/v8-i6c-i4-w`): against part B's `v8-i6b-i4-w`
  **10 of 12 byte-identical** (`recipe.json` `15b200d8...6410`, `orientations.json` `6d0d1be9...849d`,
  `train_combined.json` `2831faf0...3e17`, `train_combined.f64` `1cf245b1...3912`, `_member.u8` / `_finite.u8`
  `732f47b7...f1a4`, `_ids.u64` `102e89c6...741c`, `_sessions.i64` `89af5340...2830`, `train_planned_targets.csv`
  `e6dbd9a8...5297`, `train_daily_ic.csv` `7e6e596f...bce6`); `summary.json` (6,031 paths, 200 differ) and
  `train_candidates.jsonl` (5,424 paths, 191 differ) differ **only in timing paths** (`stage_seconds.*`,
  `wall_seconds`, `hash_seconds`; no other path). The payloads equal part B's, so they equal the accepted
  `mega-v71w-train-ew-1` payloads as part B recorded. With a rerank-off identity file the new C-5 recorded-rule check
  and the O-4 order key admit it and write nothing new (the recipe is byte-identical).

**Identity 1: not run (not applicable).** No NAV source changed in either lane: FIX-4b changed tests only
(`strategy_spo_test.cpp`, `strategy_spo_v3_test.cpp`, the new `strategy_spo_cli_fixture.hpp`) plus Python, config and
template; FIX-4a changed `strategy_ic_admission.cpp`, `strategy_ic_detail.hpp`, `strategy_ic_runner.cpp`,
`strategy_ic_theme_resid.{hpp,cpp}` (the IC runner) plus Python. No `strategy_nav_v7*`, `strategy_spo*`, target or book
source moved, and the NAV executable was not rebuilt (v8-10 bytes).

### Scoped review range

- **Code: `9c5cfa0c..41ef00fb`** restricted to `research_tree.CODE_PATHSPEC` (atx-core, atx-tsdb, atx-engine,
  atx-impl, scripts, top-level CMake): 21 files, +2,463 / -317. Final code head `41ef00fb` (the FIX-4b merge); no
  integrator code commit. The only integration-made code line is the merge resolution of the
  `test_research_spec.py` import block (union).
- Read with it, outside the code pathspec: FIX-4b's C-2 `docs/plans/mega-alpha-v8-pitch.config.json` and
  `docs/plans/mega-alpha-scorecard-v8.template.md` (the R-8..R-12 ladder config the code reads).

### Hidden-data record

- Inputs: the TRAIN 2020-2022 lo1 role (`3e79978a`), its fields v9 / v7, v7.1 library and weights, the candidate
  cache v71, part C's identity weights. Tests used synthetic fixtures and tiny_world.
- Read: the two lane reports, briefs, rulings, sources, build receipt and log, runner receipts (outcome, exit,
  timings, SHA-256s), output-file SHA-256s, JSON paths of differences (no values), the one-line step 1 console (paths,
  SHA-256s, rule). The step 2 child console went to files that were not read (a pattern scan for 2024+ dates in both
  runs' stdout / stderr and runner consoles found only the runs' own `started_utc` 2026-10-01). No return, Sharpe or
  IC statistic was read or printed.
- **Nothing dated 2024-01-01 or later was opened. No disclosure.**

### Open items

- Executables not at v8-11: atx-equity-strategy-targets, -risk, -mine keep their v8-10 bytes (no source of theirs
  changed). If Wave 0 part 2b's locks want every strategy executable from one build tag, rebuild them first; the
  mine verb's TUs `strategy_mine_pool.cpp` / `strategy_research_role.cpp` were recompiled in atx-impl-core only
  because they include `strategy_ic_detail.hpp`.
- FIX-4a carried: O-5 strict module-SHA comparison (Ruling PM5-1); O-2 a theme registered after `filing_events` needs
  the C++ `theme_resid_order` extended (the pin test fails loudly); a PM4-10 refused cell is ledgered "undefined
  (PM4-10)" (Ruling PM5-3, the message text unchanged).
- FIX-4b carried: the R-8 band [.04, .06] is config constants (a target change is a config edit).
- Known: `ConfigJsonNotInDiscoverDigest` (1).
- Next per PM4-14: Wave 0 part 2b on this head.

## integration 7 (2026-10-01)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `5b2ee1b1` (clean; code head `41ef00fb`,
build v8-11). Tag prefix v8-12. Scope (`task-INT7-brief.md`, Ruling PM5-6): merge MINE-FIX `20e7bd19` by SHA; build
every research executable under one tag; mining golden at 1 and 4 workers; every suite; identities 1, 4, 7, 8. No Wave 0
step, no cell and no mining campaign on real data was run.

### Merge (`--no-ff` by SHA)

| lane (findings) | lane SHA | merge | conflicts and resolution |
|---|---|---|---|
| MINE-FIX (MINE-1..12, 17, 18; round 1 PM4-13) | `20e7bd19` (from `864b7836`; nothing newer from pool 8) | `807af678` | 26 files, +2,436 / -289. Two conflicts, both unions. `atx-impl/tools/backtest_integrity.py` `check_line` docstring: root's F-1 sentence (a defect line may name a line ledgered invalid at once with its ruling) plus the lane's MINE-3 sentence; the lane's `if is_campaign(rec): check_campaign(before, rec)` kept. `atx-impl/tools/test_trial_ledger_rules.py`: root's `defect_line(camp["trial_id"], "budget overrun", DAY, RULING)` (F-5's four-argument signature) kept, the lane's `good` dict for the `campaign_line` checks kept. Known overlap (PM4-13): FIX-C's E-33 test auto-merged at budget 1000 with the 1001 refusal. `scripts/research_ledger.py`, `scripts/tests/test_research_ledger.py`, `atx-impl/CMakeLists.txt` (+ `src/strategy_mine_ledger.cpp`) auto-merged. The merge commit message carries git's two `# Conflicts:` lines. |

### Build (`scripts/research-build.ps1 -Preset equity-dev`)

| tag | source | targets | result |
|---|---|---|---|
| v8-12 | `807af678` clean | atx-equity-strategy-targets, -ic, -risk, -mine, atx-impl-strategy-mine-tests, atx-engine-factory-tests, atx-impl-strategy-ic-tests, -target-tests, atx-impl-tests, atx-impl-strategy-tests, atx-engine-book-tests, atx-engine-combine-tests | ok, exit 0, 246.5 s, 49 TUs, 15 links, 4 jobs (4,518 MiB free, first attempt); reconfigured (CMakeLists: new source); receipt `e44344bfaf6da4d76f0d1e7cf44cef68e10a1c5954639ea99282ccc204ba6b5a` |

**The lane's C++ (never compiled before) compiled first time under `/W4 /WX`: 0 compile fixes, 0 warnings, 0 errors in
the log.** TUs: atx-engine `research_ic_fitness`, `research_driver`, `search_driver`, `factory`; atx-impl-core
`strategy_research_role`, `strategy_mine`, `_ledger` (new), `_rule`, `_pool`, `_trials`, `_promote`, `dispatch`,
`stage_sweep`, `stage_discover`, `stage_equity_mine`, the generated `build_provenance`; `equity_strategy_mine.cpp`; 23
factory-test TUs; 6 impl-tests TUs (`strategy_mine_test`, `stage_equity_mine_*`, `discover_test`,
`w0i0b_mine_membership_test`); `book_pipeline_test`; the two mine-tests TUs. **No NAV, spo, target, book, risk or IC
runner TU was recompiled**: targets, ic and risk were relinked only (changed `atx-engine.lib` / `atx-impl-core.lib` and
the regenerated `build_provenance` carrying the new git SHA).

**Every research executable is on v8-12** (one tag; bin timestamps 2026-10-01 18:54:48 .. 18:55:05; the bin files hash
to the receipt). For Wave 0 part 2b:

| executable | role in the cells | v8-12 SHA-256 |
|---|---|---|
| atx-equity-strategy-ic | IC runner | `ab7e2cbda3c87f74be7cd1d66725b7eb99b95a5182898c44aa465bbd1615452d` |
| atx-equity-strategy-targets | NAV, targets, exposures | `5497c89d5141ae5f40d72ad2bef427cf9d6bf5462236b0a6d648765435b9bca6` |
| atx-equity-strategy-risk | risk | `8967952c5c2054170d02466f121fc66122a2b3daa34d7bb99fe63a94a7eed258` |
| atx-equity-strategy-mine | mine | `cd661fe99ad06d592af2886eb6491d853c5acfd35a40f27f34f5f9088588c91d` |

Test executables (v8-12): mine-tests `082d834a...b2b3`, factory-tests `d8373ad2...b1f4`, ic-tests `a51f0f7e...79b5`,
target-tests `a677f0af...6397`, impl-tests `09f754ee...0360`, strategy-tests `e1489279...4e0b`, book-tests
`1121d845...937d`, combine-tests `65922b3a...2f5b`. The bare `atx-equity-strategy` (supplied-DSL entry point) was not
built: no research script references it (`scripts/`, `atx-impl/tools`, `atx-engine/tools` name only `-ic`,
`-targets`, `-mine`; `-risk` is the risk-model verb of the G / W0-1 reports, built above).

### Mining determinism golden `0x889874a3b9b29c55` (not edited)

| test | exe (v8-12) | 1 worker | 4 workers |
|---|---|---|---|
| `SignalFitnessDefaults.ExplicitDefaultsKeepTheGoldenDigestAtEveryWorkerCount` (loop over {1, 4}) | mine-tests and factory-tests | **holds** | **holds** |
| `SignalFitnessDefaults.ImplicitDefaultsKeepTheGoldenDigest` | mine-tests and factory-tests | holds (default config) | - |
| `NsgaSearch.ScalarRaw_ReproducesGoldenDigest` | factory-tests | holds | - |
| `StrategyMineCampaign.SameSeedSameChainHeadAtOneAndFourWorkers` (registry chain head, trials.csv and members equal for w1a, w1b, w4; re-run refused) | mine-tests | equal | equal |

**The golden holds at 1 and at 4 workers.** No revert (brief section 3 not triggered).

### Tests (no test fix)

| exe / suite | build | result |
|---|---|---|
| mine-tests `StrategyMine*:SignalFitness*:ResearchIc*:OpCatalogCfgTest.*` (the lane's filter) | v8-12 | **31/31** |
| atx-impl-strategy-mine-tests (whole) | v8-12 | **31/31** (18 + 13: 10 verb tests in `strategy_mine_test.cpp`, 3 engine tests in `factory_signal_fitness_test.cpp`) |
| atx-engine-factory-tests (whole; `NsgaSearch.*`, `FactoryFidelity*` for the race's `drop_fresh` refactor) | v8-12 | **390/390** (387 + 3 engine tests) |
| atx-impl-strategy-target-tests | v8-12 | **259/259**; pins hold: `[spo-pin]` v1 `0xda6b6871e7e267c5` / `0xaabdbb72f99a6e13`; `[spo-v3-pin]` v2 `0xb039820b40d5cf24` / `0xd24b61721a7c698c` |
| atx-engine-book-tests | v8-12 | **155/155** |
| atx-impl-strategy-tests | v8-12 | **46/46** |
| atx-impl-strategy-ic-tests | v8-12 | **145/145** |
| atx-engine-combine-tests | v8-12 | **233/233** |
| atx-impl-tests (run from the repo root) | v8-12 | 1,028 run: **1,022 passed, 5 skipped, 1 failed: the known `ConfigJsonNotInDiscoverDigest`** (1,012 + 10 verb tests through the glob); same spo pin lines |
| lane pytest trio (`test_research_ledger.py`, `test_trial_ledger_rules.py`, `test_mine_overlap_factor.py`) after the merge | - | **17 passed** (lane's 16 + the root test of `test_trial_ledger_rules.py` the lane did not have) |
| atx-impl/strategies | - | **163** passed; `test_generate_library.py` with `ATX_V71_PLAN_JSON=build-equity/v8-i3-plan-v71.json` **9** passed |
| atx-engine/tools (whole) | - | **253** passed, 6 subtests |
| atx-impl/tools (whole; `ATX_EQUITY_BIN`, `ATX_EQUITY_TARGETS_EXE` absolute, v8-12) | v8-12 | **571 passed, 1 skipped** (`ATX_EQUITY_ROOT` unset), 14 subtests: 567 + 4 (`test_mine_overlap_factor.py` 3, `test_trial_ledger_rules.py` + 1) |
| scripts/tests (whole; `ATX_EQUITY_BIN` absolute, v8-12) | v8-12 | **183 passed, 3 skipped** (182 + 1 in `test_research_ledger.py`; the three RESEARCH_CYCLE_LIVE_ROOT skips); tiny_world `test_cycle_e2e.py` in it: no golden moved, `git status` clean after |

impl-tests skips: Alpha101Orats x2, AtxImplDiscover.W6, SingleAlphaCapacity, FundamentalZoo (environment gates).

### Identities (bounded runner, one at a time, clean tree `807af678`, exes of v8-12)

Every run: `python scripts/run_bounded_research.py --output build-equity/v8-i7-<id>-run --seconds 180 --max-rss-mib
1536 --min-free-mib 512 -- <argv>`, the argv read from the earlier receipt (`v8-i6b-i1`, `-i1w4`, `-i7`, `-i8`;
`v8-i6c-i4w`, `-i4`) with only the output paths renamed to `v8-i7-`. Inputs re-hashed before identity 4: library
`787c802e`, role lo1 `3e79978a`, fields-v9 manifest `8fd00e9f`, v7.1 weights `7b0a59c9`, part C weights `88635696`
(3-year roles and existing caches only). Every receipt: outcome completed, exit 0, `git: clean in the code pathspec`,
source `807af678`, exe SHA-256 = the v8-12 receipt's.

| id | receipt.json SHA-256 | exe | wall s | peak MiB | result |
|---|---|---|---|---|---|
| 1a | `2237d525281be5ab25f175c7b34d89ca201e673bbc28342e38d2526d76c9513b` | targets `5497c89d` | 18.9 | 359 | **PASS** |
| 1b | `538ec53b57a0dc57c05f407c964992784b3bdaf409fd2aedf041dc62c06b6c14` | targets `5497c89d` | 18.2 | 360 | **PASS** |
| 4 step 1 | `3b3d6c807e77808989f71a57e6ee1525e9071f1b6f1689db11060a6af3f4ddf9` | python | 0.5 | 35 | **PASS: weights file byte-identical** |
| 4 step 2 | `77de4a2e8b0a12180415c09529ad78a5aed022d0c7378406f02422300e11f65d` | ic `ab7e2cbd` | 24.5 | 507 | **PASS** |
| 7 | `9ac0a737a4741e9e28437c3c06f92c330943ac2b0c8fda0dd5a50d42e5c2daee` | targets `5497c89d` | 57.7 | 359 | **PASS** |
| 8 | `f4403c593045299f38467e7304552fb70a4eae5da577e5e4b028540c2c9c02cd` | targets `5497c89d` | 39.5 | 464 | **PASS** |

- **1a** (V71 argv, `build-equity/v8-i7-i1-nav`): **12 of 12 byte-identical** to part B's `v8-i6b-i1-nav` and to the
  accepted cell `mega-nav-v71u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247` (primary daily `fbec452e...d5f4`, recipe
  `b956bbcc...34fa`, summary `5b109a70...170a`); `stdout.log` `813ce59c...` equals parts B and C.
- **1b** (`v7-w4-nav-on` argv with `--emit-holdings`): NAV **12 of 12** identical to `v7-w4-nav-on` and to part B
  (recipe `2324dd91...0858`, summary `998eca31...fea3`); holdings **4 of 4** identical to **`v7-w4-holdings`** and to
  part B (`holdings.f64` `ce5523c6...eff4`, `holdings_days.csv` `43d9d2fe...3857`, `holdings_index.json`
  `aef15691...fde1`, `manifest.json` `532044d5...e141`); `stdout.log` `b40bd0cc...` equals part B.
- **4 step 1** (`composition_rules.py identity-weights`, out `build-equity/v8-i7-i4-identity-weights.json`):
  `d49e208c...2eae`, **byte-identical** to part C's `v8-i6c-i4-identity-weights.json`.
- **4 step 2** (the exact part C argv, `--composition-weights build-equity/v8-i5c-i4-identity-weights.json
  --composition-weights-sha256 88635696...`, `--output build-equity/v8-i7-i4-w`): against part C's `v8-i6c-i4-w`
  **10 of 12 byte-identical** (`recipe.json` `15b200d8...6410`, `orientations.json` `6d0d1be9...849d`,
  `train_combined.json` `2831faf0...3e17`, `train_combined.f64` `1cf245b1...3912`, `_member.u8` / `_finite.u8`
  `732f47b7...f1a4`, `_ids.u64` `102e89c6...741c`, `_sessions.i64` `89af5340...2830`, `train_planned_targets.csv`
  `e6dbd9a8...5297`, `train_daily_ic.csv` `7e6e596f...bce6`); `summary.json` (6,031 paths, 201 differ) and
  `train_candidates.jsonl` (5,424 paths, 192 differ) differ **only in timing paths** (`stage_seconds.*`,
  `wall_seconds`, `hash_seconds`; no other path). As in part C: data files identical, timing fields only.
- **7** (spo-v2 argv, lo3): **9 of 9 byte-identical** to the pinned v7 side files of `mega-nav-v70-lo3-spo-v2-G1.0` and
  to part B (`spo_diagnostics.csv` `1131cd59...7dc2`, `v7_transfer_coefficient.csv` `a27505c2...1bb0`, `v7_extras.json`
  `d96be152...42eb`, recipe `d6ffb028...58c8`, summary `75caf7f6...c44f`).
- **8** (V71 plus `--label-role` = `--role`): **12 of 12 byte-identical to part B's `v8-i6b-i8-nav`** (recipe
  `10961936...6635`, summary `6753c13c...5c00`: E-25's declared `label_role` keys); its 10 daily and events CSVs are
  byte-identical to 1a's; `stdout.log` `d8ee4a6a...` equals part B.

**`strategy_research_role` and the engine header moved no byte of any IC or NAV output with the flags absent**
(identities 1, 4, 7, 8 all pass; `ResearchRole` is used by the mine verb only, and no NAV / IC runner TU was recompiled).

### Scoped review range

- **Code: `41ef00fb..807af678`** in `research_tree.CODE_PATHSPEC`: the MINE-FIX merge, 25 code files (+ the lane
  report). Final code head `807af678`; no integrator code commit. The only integration-made code lines are the two
  union resolutions above (one docstring, one test line).

### Hidden-data record

- Inputs: the TRAIN 2020-2022 roles (lo1 `3e79978a`, lo3 `40e3d832`), their fields v7 / v9, the v7.1 library and
  weights, `mega-candidate-cache-v71`, the accepted v7.0-lo3 and v7.1 artifacts, `v7-w4-*`, the lo3 risk model
  `786cb601`, parts B / C outputs, `build-equity/v8-i3-plan-v71.json`. Tests used synthetic fixtures (the mining fixture
  is synthetic; its confirm window ends at the 2024-01-01 boundary exclusive, no data) and tiny_world.
- Read: the lane report, brief, rulings, review-mine findings list, sources, build receipt and log, runner receipts
  (outcome, exit, timings, SHA-256s), output-file SHA-256s and JSON paths of differences (no values). gtest output was
  read for pass / fail / skip lines and pin lines only. No return, Sharpe or IC statistic was read or printed.
- A scan of every `v8-i7-*-run` stdout / stderr and of the runner consoles for dates in 2024 or later found only the
  runs' own `started_utc` wall-clock (2026-10-01).
- **Nothing dated 2024-01-01 or later was opened. No disclosure. No mining campaign on real data.**

### Open items

- MINE-FIX carried (lane report): MINE-14, -15, -16 deferred to v9 (MINE-15 unruled); the confirm's BY
  p = Phi(-t / F) is approximate on short confirm windows (200-row ratio 1.70 at 99%); an OD-7 campaign on the 4-year
  role needs about 10.85 GiB and an owner-approved `--max-memory-mib` cap (E-6); real stage-2 searches may file
  `slot-bound` failures (counted in N). The C++ fixture expectations rested on numpy replicas; they pass as built.
- `atx-equity-strategy` (bare) keeps its 2026-09-26 bytes; nothing in the cells uses it. Rebuild it only if a lock is
  wanted on it.
- Known: `ConfigJsonNotInDiscoverDigest` (1).
- Next per PM5-6: Wave 0 part 2b pins the four v8-12 executables above on this head.

## Wave 0 part 2b (R12-R14) (2026-10-01)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `9ed1ed09` (clean; code head `807af678`,
every research executable on v8-12). Dispatch 2b of Ruling PM4-14: R12 (plan-only), R13 (cold u pass v7.1 on lo1, the
signal and daily IC overlap reports), the FIX-5 merge gate (PM5-12), R14 (pins, protocol line, locks). Not run: R15,
fields v10, any cell.

### A. Preconditions

1. **Disk:** `df /c` 46,425,160 KiB free (44.3 GiB; `df -h` 45G) of 458 G before R12 (>= 30 GB). RAM 4,577 MiB free
   of 16,069 MiB.
2. **Runbook section 1:** `research_window.SEAL_DATE` prints `2024-01-01`; `p.SEAL, f.SEAL, h.SEAL, b.SEAL` print
   `2024-01-01` four times; `WINDOW_ID` `research-window-v2`; `partition_is_sealed(2024, 1)` True, `(2023, 4)` False.
   `atx-impl/strategies/research_window.json` SHA-256 `62cf2cfab1d0f61b02731a807ccfdd326bb2df91caff1173a3cf1b37e6e63584`
   (= parts 1 and 2a). Vendor file unchanged: 3,617,973,507 B, mtime_ns 1789920127331396300.
3. **Executables (v8-12):** receipt `build-equity/mega-v8-12-receipt.json` SHA-256
   `e44344bfaf6da4d76f0d1e7cf44cef68e10a1c5954639ea99282ccc204ba6b5a` (source `807af678`, ExitCode 0, DirtyEntries 0).
   On disk equal to the receipt: IC `ab7e2cbda3c87f74be7cd1d66725b7eb99b95a5182898c44aa465bbd1615452d`, NAV / targets
   `5497c89d5141ae5f40d72ad2bef427cf9d6bf5462236b0a6d648765435b9bca6`, risk
   `8967952c5c2054170d02466f121fc66122a2b3daa34d7bb99fe63a94a7eed258`, mine
   `cd661fe99ad06d592af2886eb6491d853c5acfd35a40f27f34f5f9088588c91d` (bin timestamps 2026-10-01 18:54:48 .. 18:55:00).
   `git diff --stat 807af678 9ed1ed09` outside `.superpowers/`: empty. No build.
4. **Input pins** re-hashed before R12, equal to parts 1 and 2a: library v7.1 `787c802e...2259`, role lo1
   `2ff9d771...1e53`, role lo3 `e1c67101...95f4`, fields v9 lo1 `888e6616...b8695`, fields v9 lo3 `9f156363...021ef`.

### Runs

`scripts/run_bounded_research.py` on a clean tree (source column), one at a time. Argv digest = SHA-256 of the receipt's
`command` array as compact JSON (`json.dumps(command, separators=(",", ":"))`). Peak = sampled peak tree RSS.

| step | receipt dir | source | caps | outcome / ExitCode | s | peak MiB | argv digest | receipt.json SHA-256 | output SHA-256 |
|---|---|---|---|---|---|---|---|---|---|
| R12 lo1 | `w0-2-plan-lo1-run` | `9ed1ed09` | 180 s / 1,536 / 512 | completed / 0 | 0.27 | 2.0 | `43dfb15848d430d92c54be3db5fecf01ab60948a2263f1438f1e063d1e2fa8b2` | `72e86b0874f29e1a65bdf1bce6727942593965685547652b54b2ab6edc348493` | plan (stdout.log) `bae09a826f3b86dbebf6724a00f05821ef8a87bf30da1e1b252ce54105c59217` |
| R12 lo3 | `w0-2-plan-lo3-run` | `9ed1ed09` | 180 s / 1,536 / 512 | completed / 0 | 0.27 | 5.0 | `cfd009d3fc85e23d43db4c54d9c1c5acb6530bd35ad72da38d048f93045e3f18` | `d29bc45a4d65058518faa92863d884e4e40cc358ef4d4c911f948204f467b4d2` | plan (stdout.log) `a7fcbac4e7b859491e65661f08750d21226c3c0fb61818bc3e0009e63e4474d8` |

- **R12 PASS (both roles).** Argv = runbook R12 verbatim (its runner caps 180 s / 1,536 MiB; the exe's
  `--max-memory-mib 2560`), exe IC `ab7e2cbd` (v8-12), stderr empty. Plan (`mode` metadata-only-no-payload):
  `candidate_count` 48, `max_compiled_slots` 8, `research_fields.resident_capacity` 6 (40 loaded / 40 declared extras,
  54 planned loads), `required_lookback` 272, workers 4, `max_working_bytes` 2,684,354,560; **`required_bytes`
  1,989,405,564 B (1,897.2 MiB) on both roles**, equal to R9's number (lo1 and lo3 share axes 1,405 x 5,922), inside
  the OD-2 cap by 663.1 MiB. Role pins in the plan: lo1 `2ff9d771`, lo3 `e1c67101`; fields pins lo1 `888e6616`, lo3
  `9f156363`. No candidate row names `regsho` (PM4-15 item 4 holds on the plan too).

### R13: cold u pass v7.1 on lo1 and overlap reports 2 and 3 (Ruling W0-a, PM4-15)

| step | receipt dir | source | caps | outcome / ExitCode | s | peak MiB | argv digest | receipt.json SHA-256 | output SHA-256 |
|---|---|---|---|---|---|---|---|---|---|
| R13 u pass (cold) | `w0-2-v71-u-lo1-run1` | `16909f97` | 300 s / 2,560 / 512 (W0-c) | completed / 0 | 129.4 | 1,572 | `05a81e90f8cff795864b2626843ef51ad7298065ceab6ebed17fff3e134b8b9e` | `7e2f7a3b1c5770087943dfbb362b44bd585b4e806a281d5cb9ebbaf675d5fa18` | `summary.json` `0c7530928719db9d9ab1be5f5d4dc134c3271e3f881146091663daafa1b5ee51` |
| R13 (2) signal overlap | `w0-2-overlap-signal-lo1-run` | `16909f97` | 180 s / 1,536 / 512 | completed / 0 | 16.8 | 150 | `735164bab409d2735cb5c4071506ba2508fad5de11cef8f16fc306100a379cec` | `84833a3c622a26231c5a2f91bb90a6b6bd240df96d853e8d4e8c57e386bb8254` | report `w0-2-overlap-signal-lo1.json` `4a2b5b6d3a0e81dcc4fb7727926176ab5ed30b4693797c4aa4f43164948932d3` |
| R13 (3) daily IC overlap | `w0-2-overlap-daily-ic-lo1-run` | `16909f97` | 180 s / 1,536 / 512 | completed / 0 | 2.4 | 95 | `7f15513ec4f0ad54ec9ca7fe89d5f07f53daa99c197868e3d0c2ede5063321d0` | `201db6008cf70cacd4fa2444e10ec7a23c5d97a38891d0bb76a56fc467bd102c` | report `w0-2-overlap-daily-ic-lo1.json` `f6111822ba859d298440b9834b2c238f0f1f930ccb0d76d739fc8e1b4b7d3a6f` |

- **u pass PASS (mechanics).** Argv = runbook R13 verbatim (`--output build-equity/w0-2-v71-u-lo1-1 --max-memory-mib
  2560 --min-names 1000 --workers 4 --save-combined --candidate-cache build-equity/mega-candidate-cache-v8-lo1`), exe IC
  `ab7e2cbd` (v8-12), bindings library `787c802e`, role lo1 `2ff9d771`, fields v9 lo1 `888e6616`. Runner min free
  4,113 MiB; stderr empty. Runbook expectation cold ~120 s / ~1.6 GB: measured 129.4 s / 1,572 MiB. `summary.json`
  `status` complete, recipe `3ea61343...`; candidate cache **cold: hits 0, misses 48, vm_evaluations 48**, layout
  `atx.dsl-candidate-signal/v2`, entries 48, cache dir `mega-candidate-cache-v8-lo1/2ff9d771.../` (fields key
  `888e6616`). Combined artifact sessions 1,405 (2018-06-01..2023-12-29), none on or after the seal. Cache 3,052 MiB
  (runbook ~3,140), u dir 93 MiB (~95). Output file SHA-256s: `recipe.json` `09d92c6d...24ef`, `orientations.json`
  `bbb6f5ba...ab68`, `train_candidates.jsonl` `3bba2b5c...edaf`, `train_daily_ic.csv` `70ab07ef...635f`,
  `train_combined.f64` `08f7969e...b98d`, `train_combined.json` `ff709231...8d92`, `_ids.u64` `761bd1df...d296`,
  `_sessions.i64` `ab244802...84b7`, `_member.u8` = `_finite.u8` `ec21a192...06dd`, `train_planned_targets.csv`
  `1c15ff2c...86a1`. No IC value, orientation sign or summary statistic of the pass was read.
- **Report 2, signal overlap: BIT-IDENTICAL (W0-a class identical; PM4-15's proof holds).** Argv = the W0-2 tool
  report's R13 signal line: `compare_window_overlap.py --kind signal --per-key --before 2022-09-30 --old
  build-equity/mega-candidate-cache-v71 --new build-equity/mega-candidate-cache-v8-lo1 --old-role <v7.1 fields
  manifest role.path = build-equity/recent-fast-train-2020-2022-v2-lo1> --new-role build-equity/train-2020-2023-lo1
  --old-run build-equity/mega-v71-train-u-1 --new-run build-equity/w0-2-v71-u-lo1-1 --out
  build-equity/w0-2-overlap-signal-lo1.json` (the runbook's "sessions before 2022-09-30 for signal and daily_ic").
  Tool `b4d7f2e8` (= part 2a); bindings old role `3e79978a`, new role `2ff9d771`, old run summary `7024b246...`, new
  run summary `0c753092...`; payloads verified (no `--no-verify`); seal source `research_window.py SEAL_NS`.
  Alignment: 1,091 common sessions (2018-06-01..2022-09-29), 5,627 common instruments, old-only sessions 0 and
  instruments 0, new-only instruments 295 (counted). **`bit_identical` true, `max_abs_diff` null, cells compared
  294,674,736** (48 keys, 6,139,057 each; keys without cells 0), unequal 0, NaN mismatches 0,
  `old_cells_missing_in_new` 0, `differing_keys` []. Mapping: 48 of 48 `dsl_sha256_equal`; unmatched (ambiguous,
  missing_in_new, new_only_keys, run_entry_not_in_cache) all empty. Every v7.1 candidate signal (the ten
  `me_company` readers and `sv_flow` included) is bit-identical on the common cells.
- **Report 3, daily IC overlap: NOT bit-identical, W0-a class STOP (> 1e-9).** Argv = the tool report's R13 daily_ic
  line: `compare_window_overlap.py --kind daily_ic --per-key --before 2022-09-30 --old build-equity/mega-v71-train-u-1
  --new build-equity/w0-2-v71-u-lo1-1 --out build-equity/w0-2-overlap-daily-ic-lo1.json`; bindings tool `b4d7f2e8`,
  old `train_daily_ic.csv` `929b4a5a...a5c8`, new `70ab07ef...635f`. Alignment: 692 common sessions (score sessions
  2020-01-02..2022-09-29), old-only 0, new-only 0. Totals: **`bit_identical` false, `max_abs_diff` 1.3089812302391757,
  cells compared 305,172** (49 keys = 48 candidates + `__combined__`, 6,228 each = 692 sessions x 3 horizons x 3 columns;
  keys without cells 0), unequal 12,451, NaN mismatches 0, `old_cells_missing_in_new` 0, `max_rel_diff` 2.0 (the
  tool's summary line), **`w0a_class` stop**.
  - **First differing cell (totals): key `__combined__`, column `pearson`, horizon 5, session 2020-01-02
    (ns 1577923200000000000).**
  - Differing keys, 4 of 49 (tool per-key identity fields): `__combined__` 6,228 of 6,228 unequal, max_abs_diff
    0.2895, first diff `pearson` h5 2020-01-02; `chtax` 2,071 of 6,228, max_abs_diff 0.5800, first diff
    `oriented_rank_ic` h5 2020-01-02; `ind_adj_rev_5` 2,076 of 6,228, max_abs_diff 1.3090, first diff
    `oriented_rank_ic` h5 2020-01-02; `ind_mom_12_1` 2,076 of 6,228, max_abs_diff 1.0431, first diff
    `oriented_rank_ic` h5 2020-01-02. The other **45 candidates: bit-identical** on all 6,228 cells each.
  - Structure, from those identity fields only: each of the three candidate keys differs on at most one column's worth
    of cells (692 x 3 = 2,076) and first in `oriented_rank_ic` (its `pearson` and `rank_ic` at that first cell are
    equal); the combined key differs on every cell. **Code reading (no data):** on a TRAIN u pass the IC runner sets
    each candidate's sign to the sign of its whole-window mean rank IC at horizon 21 (`strategy_ic_runner.cpp:419-423`,
    `sample_orientation_sign`, `orientation_horizon` 21), writes `oriented_rank_ic` with that sign (`:341`) and, with
    no pinned signs, blends the unweighted combined with the same sign (`:432`, `blend_sign = sign`). That sign is a
    quantity of the whole TRAIN window (2020-2022 vs 2020-2023), not of a session; a sign that differs between the two
    windows for these three candidates would give exactly this pattern. **Not verified**: confirming it means reading
    `orientations.json` signs (a TRAIN IC statistic), which this dispatch forbids. **Disclosure:** the identity fields
    above (a `max_rel_diff` of 2.0 = equal magnitude, opposite sign, in the oriented column) carry that implication
    about three candidates' TRAIN-window orientation; no IC level, sign list or orientation file was read.
- **Per the dispatch and W0-a: STOP.** The re-base is stopped until the PM rules on the cause. Not run after report 3:
  step D (the FIX-5 merge; SHA `d2304773` received from the PM during R13, verified to exist and not to be an ancestor
  of HEAD, **not merged**), step E (R14: no pin written into `v8-prereg.md`, no protocol line appended, no lock).

### Disk

C: free 46,425,160 KiB (44.3 GiB) before R12, **43,173,644 KiB (41.2 GiB)** after report 3 (`df -h` 45G -> 42G). New:
`mega-candidate-cache-v8-lo1` 3,052 MiB (144 files), `w0-2-v71-u-lo1-1` 93 MiB, five run dirs and two reports < 1 MiB
each. RAM free 5,966 MiB before the u pass.

### Fixes

None. No source file changed; no tool failed. Commits: `16909f97` (preconditions, R12) and this log.

### Ledger

`build-equity/trials.jsonl` untouched: 37 lines, all `construction`, unchained (0 lines with `prev_sha256`); chain
head (fold) `00c901da636e9ae0cf758020f3fa3bd55ef3912899b4bf912de8ae631b9e0d93`; file SHA-256 `a2c24f56...0810`;
**N = 37** (`backtest_integrity.ledger_n(records, True)`). Read through `ledger_read` (kinds and counts only; no line
content printed).

### Hidden-data record

- **Inputs opened by the tools:** the IC exe read role lo1 (1,405 sessions to 2023-12-29) and fields v9 lo1
  (`888e6616`; R12 metadata only, the u pass payloads); the overlap tool read the two caches, the two roles, the two
  run summaries and the two `train_daily_ic.csv`, refused by the tool on any session on or after the seal (none; both
  restricted to sessions before 2022-09-30).
- **Logs:** R12 (2), u pass, signal and daily IC stdout / stderr scanned for dates in 2024 or later: 0 hits; the
  two reports' only such date is the `seal.date` 2024-01-01.
- **What I read:** receipt fields; the R12 plan JSON (counts, bytes, pins, candidate ids and field names, no data);
  the u pass `summary.json` status, recipe SHA and candidate-cache block (hits, misses, evaluations, entry keys, no
  IC); output file SHA-256s; the two reports' alignment, totals and per-key identity fields (cells, unequal, NaN
  mismatches, max_abs_diff, first_diff); `strategy_ic_runner.cpp` and `compare_window_overlap.py` source; the
  ledger's line kinds, N and chain head. The disclosure under report 3 stands. **No IC level, return or Sharpe of
  2020-2023 was read, printed or summarised. Nothing dated 2024-01-01 or later was opened.**

### Open items

- **Ruling needed (W0-a, report 3):** daily IC overlap class **stop**: `__combined__` (all 6,228 cells) and the
  `oriented_rank_ic` column of `chtax`, `ind_adj_rev_5`, `ind_mom_12_1`; first cell `__combined__` / `pearson` / h5 /
  2020-01-02; 45 candidates bit-identical; signals bit-identical (report 2). Candidate cause (code reading, not
  verified): the TRAIN-window orientation sign (h21 mean rank IC over the whole window) of these three candidates.
  Options the PM may weigh: rule the `oriented_rank_ic` column and the u-pass `__combined__` row window-wide quantities
  (like a fit, not old values) and order a cause test that reads only identity fields (e.g. the overlap restricted to
  the `pearson` / `rank_ic` columns, which the tool cannot do today, or a sign-equality count over `orientations.json`
  read by someone allowed to see it); or keep the stop.
- **Next dispatch, once ruled:** step D (merge FIX-5 `d2304773`, every Python suite, identity 4 with the argv of
  integration 7), then step E (R14).
- **Found while reading for R14 (not run):** `lock --write` on `scripts/specs/v8/base-lo3.json` will refuse today:
  `research_cycle.lock` pins every `inputs` entry (`scripts/research_cycle.py:1791-1812`, `lock_pin` exit 3 on a
  missing file) and base-lo3's `inputs.reference_cell` is B0a's NAV `summary.json`
  (`build-equity/mega-nav-v8-b0a-lo1-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247/summary.json`), which does not exist
  before B0a runs. The A2 root sequence (`task-A2-report.md:300`) locks base-lo3 "after R5, R8, R11 and B0a's cell".
  R14 as dispatched can lock base-lo1 only; base-lo3 needs a PM ruling (lock after B0a, or drop
  `inputs.reference_cell` as the runbook Q4 note allows).
- Carried from 2a: R11 has no overlap report (open question 30).

## integration 6 part D (FIX-5) (2026-10-01)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `d19f9638` (clean; code head `807af678`, every
research executable on v8-12). Scope (PM dispatch, Ruling PM5-12, step D of Wave 0 part 2b only): merge FIX-5 `d2304773`
by SHA; the lane's Python suites, then every Python suite; executables check (no build); identity 4 with the argv of
integration 7. Not run: any Wave 0 step (no u pass, no overlap report, no pin, no protocol line, no lock), any cell,
anything on the 4-year roles.

### Merge (`--no-ff` by SHA)

| lane (findings) | lane SHA | merge | conflicts and resolution |
|---|---|---|---|
| FIX-5 (R6C-1..R6C-6 Python; R6C-3 Python half; PM5-11 texts) | `d2304773` (pool 10, from `41ef00fb`; not an ancestor of `d19f9638`) | `1cc4c6c9` | **None** (ort, auto-merged). 12 files, +644 / -52: `atx-impl/tools/fit_composition_weights.py`, `mega_report/v8.py`, `test_composition_resid.py`, `test_mega_report_v8.py`, `test_mega_report_v8_render.py`, `scripts/tests/test_research_spec.py` (the lane's hunk is inside `test_r11_appends_theme_resid_to_the_parents_fit`; no textual overlap with integration 7), `docs/plans/mega-alpha-scorecard-v8.template.md`, `docs/plans/mega-alpha-v8-pitch.config.json`, `scripts/specs/v8/r1-comp-v8.json`, `r10.json`, `r11.json`, `task-FIX-5-report.md`. **No C++, header or CMake file** (checked on the lane diff before the merge and on `d19f9638..1cc4c6c9` after). |

New code head (`research_tree.CODE_PATHSPEC`: atx-core, atx-tsdb, atx-engine, atx-impl, scripts, CMake files):
**`1cc4c6c9`**. `git diff --name-only 807af678 1cc4c6c9` outside `.superpowers/` = the 11 lane files above, nothing else.
No integrator code commit.

### Build: none (no C++ changed)

Executables on disk re-hashed after the merge, **all four equal v8-12** (receipt `build-equity/mega-v8-12-receipt.json`
SHA-256 `e44344bfaf6da4d76f0d1e7cf44cef68e10a1c5954639ea99282ccc204ba6b5a`, unchanged; bin timestamps 2026-10-01
18:54:48 .. 18:55:05, unchanged):

| executable | SHA-256 on disk | v8-12 |
|---|---|---|
| atx-equity-strategy-ic | `ab7e2cbda3c87f74be7cd1d66725b7eb99b95a5182898c44aa465bbd1615452d` | equal |
| atx-equity-strategy-targets (NAV, targets) | `5497c89d5141ae5f40d72ad2bef427cf9d6bf5462236b0a6d648765435b9bca6` | equal |
| atx-equity-strategy-risk | `8967952c5c2054170d02466f121fc66122a2b3daa34d7bb99fe63a94a7eed258` | equal |
| atx-equity-strategy-mine | `cd661fe99ad06d592af2886eb6491d853c5acfd35a40f27f34f5f9088588c91d` | equal |

### Tests (no test fix)

`"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider -rs`, `ATX_EQUITY_BIN` and
`ATX_EQUITY_TARGETS_EXE` absolute (v8-12), vcpkg bin dirs on PATH, on `1cc4c6c9`.

| suite | result | against the baseline |
|---|---|---|
| the lane's 23 files (`task-FIX-5-report.md` "How root verifies", verbatim) | **604 passed, 3 skipped**, 20 subtests | lane: 603 / 4 on the same 607 tests. The 3 skips here are the RESEARCH_CYCLE_LIVE_ROOT tests; the lane's fourth is `test_exposures_export.py:234` (skips without `ATX_EQUITY_TARGETS_EXE`, which the lane did not set and root does) |
| atx-impl/strategies (whole) | **163 passed** | = 163 |
| `test_generate_library.py` with `ATX_V71_PLAN_JSON=build-equity/v8-i3-plan-v71.json` | **9 passed** | = 9 |
| atx-engine/tools (whole) | **253 passed**, 6 subtests | = 253 |
| atx-impl/tools (whole) | **578 passed, 1 skipped**, 17 subtests | 571 + 7 = the lane's new test functions (`test_composition_resid.py` 21 -> 23, `test_mega_report_v8.py` 42 -> 45, `test_mega_report_v8_render.py` 22 -> 24); subtests 14 -> 17 in the lane's tests. Skip: `test_nav_summ_v8.py:72` (`ATX_EQUITY_ROOT` unset), as before |
| scripts/tests (whole) | **183 passed, 3 skipped** | = 183 / 3 (the lane edited `test_r11_appends_theme_resid_to_the_parents_fit`, added none); skips: the three RESEARCH_CYCLE_LIVE_ROOT tests. tiny_world `test_cycle_e2e.py` in it: **no golden moved**, `git status --porcelain` empty after every suite |

### Identity 4 (bounded runner, one at a time, clean tree, source `1cc4c6c9`)

Argv read from integration 7's receipts (`build-equity/v8-i7-i4w-run/receipt.json` step 1,
`build-equity/v8-i7-i4-run/receipt.json` step 2), only the output paths renamed `v8-i7-` -> `v8-i6d-`; runner
`--seconds 180 --max-rss-mib 1536 --min-free-mib 512`. Inputs re-hashed before the run, equal to integration 7: library
`787c802e`, role lo1 `3e79978a`, fields-v9 manifest `8fd00e9f`, v7.1 weights `7b0a59c9`, part C weights `88635696`
(3-year role and existing caches only). Both receipts: outcome completed, exit 0, `git: clean in the code pathspec`,
stderr empty.

| id | receipt.json SHA-256 | exe | wall s | peak MiB | result |
|---|---|---|---|---|---|
| 4 step 1 | `437cdf76045618b855c996f485d3b45680d4d007030003495e998d861808b7bc` | python `624bbc05` | 0.25 | 5 | **byte-identical** (see the finding below) |
| 4 step 2 | `07d9ae2347105f1f14be1db653a1e1523f1765486aee700c466d783a27f95d46` | ic `ab7e2cbd` (v8-12) | 18.1 | 507 | **PASS** |

- **Step 1** (`composition_rules.py identity-weights --weights build-equity/mega-weights-v71-ew/composition_weights.json
  --weights-sha256 7b0a59c9... --out build-equity/v8-i6d-i4-identity-weights.json`): `d49e208c...2eae`,
  **byte-identical to integration 7's `v8-i7-i4-identity-weights.json`; no key differs.** Console line differs from
  integration 7's only in the `out` path.
- **Finding (premise of the expectation, not a mismatch):** the dispatch expected this weights file to differ in
  `provenance.script_sha256` and `provenance.admission_sha256`. Identity 4's argv never runs
  `fit_composition_weights.py`: step 1 runs `composition_rules.py` (unchanged since `807af678`, module SHA
  `a8b35a6b...5bfd`, recorded as `provenance.std_identity.module_sha256`), which copies the pinned v7.1 weights document
  and grafts the identity block. Checked (keys and SHA strings only): the file's `provenance.script_sha256`
  (`4cff96b6...399d`) and `provenance.admission_sha256` (`6d68892f...cbb0`) are present and equal to those of the
  pinned source `mega-weights-v71-ew/composition_weights.json` (v7.1's fit), not computed from today's fitter (`14e157b2`
  at `807af678`, `8860483c` at `1cc4c6c9`). So the two-key move FIX-5 announces happens only in a weights file the fitter
  writes; no identity argv on record runs the fitter, and none was run here (not dispatched). The fitter's
  flag-absent behaviour is covered by the suites above (`test_fit_composition_weights*.py`, `test_composition_resid.py`
  pass; the new check returns at once without `--theme-resid`, read in the diff).
- **Step 2** (the exact integration 7 argv, `--composition-weights build-equity/v8-i5c-i4-identity-weights.json
  --composition-weights-sha256 88635696...`, `--output build-equity/v8-i6d-i4-w`): against `v8-i7-i4-w` **10 of 12
  byte-identical** (`recipe.json` `15b200d8...6410`, `orientations.json` `6d0d1be9...849d`, `train_combined.json`
  `2831faf0...3e17`, `train_combined.f64` `1cf245b1...3912`, `_member.u8` / `_finite.u8` `732f47b7...f1a4`,
  `_ids.u64` `102e89c6...741c`, `_sessions.i64` `89af5340...2830`, `train_planned_targets.csv` `e6dbd9a8...5297`,
  `train_daily_ic.csv` `7e6e596f...bce6`); `summary.json` (`2c33a9fc...` vs `ad8a5ad6...`; 6,031 paths, 200 differ) and
  `train_candidates.jsonl` (`59618f3a...` vs `963d150b...`; 5,424 paths, 191 differ) differ **only in timing paths**
  (`stage_seconds.*`, `wall_seconds`, `hash_seconds`; no other path). As in integration 7 and part C: data files
  identical, timing fields only.

### Disk

C: free 43,065,264 KiB (41.1 GiB) after identity 4. New: `v8-i6d-i4-w` (72 MiB, = `v8-i7-i4-w`), two run dirs and one weights
file (< 1 MiB each).

### Hidden-data record

- Inputs opened by tools: the TRAIN 2020-2022 role lo1 (`3e79978a`), its fields v7 / v9, the v7.1 library and weights,
  the part C identity weights, `mega-candidate-cache-v71` (identity 4); synthetic fixtures and tiny_world (suites).
- Logs: the two runs' stdout / stderr and runner consoles scanned for dates in 2024 or later: the consoles carry only
  the runs' own `started_utc` (2026-10-01); the IC stdout has no date-shaped token (its four pattern hits are digits
  inside decimal numbers; the lines were not printed).
- **Disclosure:** while locating integration 7's identity 4 argv I printed `build-equity/v8-i7-i4-run/stdout.log` (the
  IC exe's console of integration 7's step 2), which carries per-candidate lines of the v7.1 TRAIN 2020-2022 lo1 pass
  (`ic=`, `composition=`, `sign=`, `reason=` for the 48 candidates). The dispatch forbids reading IC statistics. Nothing
  was derived, recorded or used from it; no other IC or return statistic was read. The data are TRAIN 2020-2022 (no
  session on or after the seal).
- **Nothing dated 2024-01-01 or later was opened.** No Wave 0 step, no cell, nothing on the 4-year roles.

### Ledger

`build-equity/trials.jsonl` not touched (no cell, no lock).

### Open items

- Identity 4's step 1 cannot show the fitter's `script_sha256` move (above). If the PM wants that move checked on a
  real weights file, it needs a dispatched fitter run (e.g. a re-fit of a pinned v8 weights file under the bounded
  runner) and a comparison by JSON path; not run here.
- Spec digests of R-1, R-10, R-11 move with the PM5-11 description edits (lane open risk); nothing was bound, run or
  locked on them before this merge.
- Carried: the W0-a stop at report 3 (Wave 0 part 2b) still needs its ruling before step E (R14); the base-lo3 lock
  question; deferred to integration 8 with wave AG: the C++ halves of R6C-3 and R6C-7.

## Wave 0 part 2c (cause test, R14) (2026-10-01)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `9a40e34f` (clean; code head `1cc4c6c9`;
every research executable on v8-12). Dispatch 2c: the PM5-16 cause test, part (i) (required) and part (ii) (run once),
under Ruling PM5-18 (3); then R14 under Ruling PM5-17 (lock base-lo1 only). Not run: R15, fields v10, any cell (no B0a).
End head `f93868bf` + this log (tree clean).

### A. Preconditions

1. **Disk:** C: 42 G free (`df -h`) at start (part 2b ended at 43,173,644 KiB).
2. **Executables** on disk = the v8-12 receipt (`build-equity/mega-v8-12-receipt.json` `e44344bf...6b5a`): IC
   `ab7e2cbda3c87f74be7cd1d66725b7eb99b95a5182898c44aa465bbd1615452d`, NAV / targets
   `5497c89d5141ae5f40d72ad2bef427cf9d6bf5462236b0a6d648765435b9bca6`, risk
   `8967952c5c2054170d02466f121fc66122a2b3daa34d7bb99fe63a94a7eed258`, mine
   `cd661fe99ad06d592af2886eb6491d853c5acfd35a40f27f34f5f9088588c91d`. `git diff 1cc4c6c9 HEAD` on the code pathspec:
   empty before the lock commit. No build.
3. **Input pins** re-hashed, equal to parts 1, 2a, 2b: research_window.json `62cf2cfa...3584`, library v7.1
   `787c802e...2259`, recipe v7.1 `7f8a2643...b271`, role lo1 `2ff9d771...1e53`, role lo3 `e1c67101...95f4`, fields v9
   lo1 `888e6616...b8695`, fields v9 lo3 `9f156363...021ef`, identity-bridge-r4-v2 `f598c04c...43de`,
   fundamental-events-v3 `304d2945...b87b` (the last two = part 1's R4 / FEV pins).
4. **Scripts** (committed `da805a2a` before any run, sprint dir, `git add -f`):
   - `pm516_cause_i.py` `6d86696e1333c73ba2cda5781a261bdd6e9693bfea33142cc4a1bfc2063e1879`: orientation-consumers.md Q5 (i)
     as written; checked against `strategy_ic_runner.cpp:172-183` (row writer: `oriented = sign*r`, an exact
     negation) and `:340-341` (header, `setprecision(17)`, so a CSV value parses back to its exact bits). Added, without
     changing the test: `id_sets_equal`, `negated_cells` (a count), an overall `pass`, and the first failing
     (id, column) that the dispatch asks for on failure. Prints counts and booleans only.
   - `pm516_pin3y_weights.py` `31800e7b6665ea082493cf51ec19c1038a35ab4950771f807b29cf8444597750`: Q5 (ii) step 1
     verbatim (docstring added). Checked against the weights loader (`strategy_ic_admission.cpp:680-733`: schema v1,
     `library_sha256`, every candidate weighted, `train_manifest_sha256 == --train-sha256`; `:421-446`: signs +1/-1,
     every weighted candidate signed) and the default weight expression `1.0 / (families * n)`
     (`strategy_ic_composition.cpp:178-184`). Prints the file SHA and counts only.
   - Corrections to the investigator's commands (paths and flags only): scripts saved in the sprint dir and committed
     (the investigator suggested outside the repo); both daily-IC CSVs bound (9.0 and 12.2 MiB, under the 16 MiB bind
     cap); the weights script run through the bounded runner too, binding the script, the library and the 3-year
     `orientations.json` (it opens a 3-year IC artifact).

### Runs

`scripts/run_bounded_research.py` on a clean tree, one at a time. Argv digest = SHA-256 of the receipt's `command` as
compact JSON. Peak = sampled peak tree RSS. All four: outcome completed, exit 0, stderr empty.

| step | receipt dir (`build-equity/`) | source | caps | s | peak MiB | argv digest | receipt.json SHA-256 | output SHA-256 |
|---|---|---|---|---|---|---|---|---|
| (i) cause test | `pm5-16-cause-i-run` | `da805a2a` | 180 s / 1,536 / 512 | 0.78 | 75.6 | `860ad221a33ba17ca1f960203c225530a26bbfbb1dc7936c1536acfe570ee91d` | `1e363ac894fa01a2ff7fb2127824c035ee143726c43a6ed505ef0228ac75ca66` | stdout.log `f1ca9ce53fe3fbaf54a70e01dab61dfebdb8ded4cb09b72aab6164eb6032f088` |
| (ii) weights file | `pm5-16-pin3y-weights-run` | `da805a2a` | 180 s / 1,536 / 512 | 0.25 | 5.7 | `961bd5a462b4d56999413a1959800e2f31052baec0db992c9fc76d5783c65a03` | `8833761c15a3128ae991869921168d93866097442cad6f77673763bd760af99d` | `pm5-16-pin3y/composition_weights.json` `6dc43268c8c82885a2b2c79060c450fc57172b4b22360ce2873ae92eede81752` |
| (ii) IC run, 4-year lo1, 3-year signs | `pm5-16-pin3y-u-lo1-run1` | `da805a2a` | 300 s / 2,560 / 512 (W0-c) | 23.2 | 667.1 | `27c645d1497bc115d34c657e2a746db061e2a071ee9bb82ce0b838ce4e470672` | `cfb87ce933f740027f0c27ae04bcf441157ce1ecbf28f3332460d5ff6e9650fd` | `pm5-16-pin3y-u-lo1-1/train_daily_ic.csv` `0d76164cc1210223ed9736c13a612a262308005ae069a761f4817b58be101b93` |
| (ii) daily IC overlap | `pm5-16-overlap-daily-ic-pin3y-run` | `da805a2a` | 180 s / 1,536 / 512 | 2.08 | 98.7 | `6330a83965f17907a1d3ef214caed7b7b7bb4b74bbf663da31e4db0be19929ec` | `63e625131eec3bddbaebd0e0db76c33a13ae5c01c1e472aaddb30da8c29c4e44` | `pm5-16-overlap-daily-ic-pin3y.json` `0f3afa33d98282316adfeb080a7e838632a2c59a323bfe05b2df813ac88c22eb` |

- (i) argv: `python pm516_cause_i.py build-equity/mega-v71-train-u-1/train_daily_ic.csv
  build-equity/w0-2-v71-u-lo1-1/train_daily_ic.csv chtax,ind_adj_rev_5,ind_mom_12_1`; bindings script `6d86696e`, old
  CSV `929b4a5a...a5c8`, new CSV `70ab07ef...635f` (= part 2b's report 3 inputs).
- (ii) weights argv: `python pm516_pin3y_weights.py $LIB $LIBS build-equity/mega-v71-train-u-1/orientations.json $LO1
  build-equity/pm5-16-pin3y/composition_weights.json`.
- (ii) IC argv = runbook R13's u pass without `--save-combined`, plus `--composition-weights
  build-equity/pm5-16-pin3y/composition_weights.json --composition-weights-sha256 6dc43268...`: `--library $LIB
  --library-sha256 $LIBS --train build-equity/train-2020-2023-lo1/manifest.json --train-sha256 $LO1 --train-fields
  build-equity/train-2020-2023-lo1-fields-v9 --train-fields-sha256 $F1 --output build-equity/pm5-16-pin3y-u-lo1-1
  --max-memory-mib 2560 --min-names 1000 --workers 4 --candidate-cache build-equity/mega-candidate-cache-v8-lo1`; exe
  `ab7e2cbd` (v8-12); bindings IC exe, library, role lo1 manifest, fields v9 lo1 manifest, weights file. Other outputs:
  `orientations.json` `73e39fda...dcb7` (differs from the u pass's `bbb6f5ba` by construction: the recipe carries
  `composition_weights_sha256`, `strategy_ic_admission.cpp:88`; not opened), `recipe.json` `5c2cd0eb...8762`,
  `summary.json` `1ac746f9...7a6b`, `train_candidates.jsonl` `fc2d2d17...3cee`, `train_planned_targets.csv`
  `2a3639d0...12e0dd`.
- (ii) overlap argv: `compare_window_overlap.py --kind daily_ic --per-key --before 2022-09-30 --old
  build-equity/mega-v71-train-u-1 --new build-equity/pm5-16-pin3y-u-lo1-1 --out
  build-equity/pm5-16-overlap-daily-ic-pin3y.json`; tool `b4d7f2e8` (= parts 2a, 2b).

### (i) PASS: raw columns identical for all 48; oriented column equal for 45, exact negation for the 3

Script output (counts and booleans): `ids` 48, `ids_new` 48, `id_sets_equal` true, `expected_ids` 3, `cells` 99,648
(48 x 692 common sessions x 3 horizons), **`raw_columns_identical` true** (pearson and rank_ic bit-identical on every
cell of all 48 candidates), **`ids_equal` 45**, **`ids_negated_every_finite_cell` 3**, `negated_cells` 6,223,
**`ids_other` 0**, **`negated_are_expected` true** (the negated set is exactly {chtax, ind_adj_rev_5, ind_mom_12_1}),
**`equal_are_the_rest` true**, `pass` true, `first_failure` null. The 6,223 negated cells equal report 3's unequal
counts for the three keys (2,071 + 2,076 + 2,076), so every differing member cell of report 3 is an exact sign flip.

### (ii) BIT-IDENTICAL: the combined row is reproduced with the 3-year signs (cause fully shown)

- Weights file: 48 candidates, 10 families, 7 candidates with no 3-year orientation (sign 0, weight 0: skipped exactly
  as the default path skips a sign-0 candidate, `strategy_ic_composition.cpp:229`); SHA `6dc43268...1752`.
- IC run: `summary.json` `status` complete, 1 role (train, `2ff9d771`), `combined_evaluations` 1, candidate cache 48 / 48
  signal hits and 48 / 48 IC-result hits (no VM evaluation), weights record `sha256` `6dc43268`, `signs`
  `pinned-candidate-signs`, `binding` `train-manifest-sha256;TRAIN-scored-in-this-run`. No IC field of the summary read;
  the run's `stdout.log` (per-candidate `sign=` progress lines) not opened.
- Overlap, 3-year u pass vs this run, sessions before 2022-09-30 (692 common, old-only 0, new-only 0):
  **`__combined__` `bit_identical` true, `max_abs_diff` null**, cells compared 6,228, unequal 0, NaN mismatches 0,
  old cells missing in new 0. `differing_keys` = [chtax, ind_adj_rev_5, ind_mom_12_1] (their `oriented_rank_ic` column,
  2,071 / 2,076 / 2,076 unequal, 0 NaN mismatches: pinned signs orient the blend only, every member diagnostic keeps the
  run's own sign, `strategy_ic_runner.cpp:432-433`); the other 45 members bit-identical. Totals unequal 6,223 (= (i)'s
  negated cells), class `stop` by design (member diagnostics), as orientation-consumers.md Q5 (ii) predicted.
- Outcome (a) of PM5-16 / PM5-18 (3): no old value changed. Signals bit-identical (report 2), raw member IC
  bit-identical (i), and the blend with the 3-year signs on the 4-year role reproduces the 3-year combined row bit for
  bit; the whole of report 3's difference is three whole-window orientation signs (horizon-21 mean rank IC over the
  run's TRAIN window, `strategy_ic_runner.cpp:419-421`). No reproduction-path analysis needed.

### C. R14 (Ruling PM5-17)

1. **Pins into `v8-prereg.md`** (section "Pins"), each re-hashed from the file on disk immediately before writing:
   research_window.json `62cf2cfab1d0f61b02731a807ccfdd326bb2df91caff1173a3cf1b37e6e63584`; role lo1
   `2ff9d7711bdaa2d669cc096f4874f7f4705309e53572fd12dad3c0a4d7ac1e53`; role lo3
   `e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4`; fields v9 lo1
   `888e6616e441e863a9f91234124e1aebc907db11e18d9789d3c583cf447b8695` / lo3
   `9f1563638b5e4f7ead7be686803b96a0707ada2c608fcbc6dc084179bd9021ef`. `v8-prereg.md` now
   `3f9b7bc20f40fe0e2eabf86fe6cf6262b9d41084a4935f9bfbb23c2c38cdb32c`.
2. **Protocol line** (`research_cycle.py ledger-protocol --ledger build-equity/trials.jsonl --owner-ruling 'OWNER RULING
   2026-09-29: "expand the TRAIN window to include 2023 to give us more sample size, keep 2024+ hidden and out of
   sample." TRAIN [2020-01-01, 2024-01-01); hidden 2024-01-01 and later (v8-prereg item 1; task W0-3)' --date
   2026-09-29`; text and date from progress.md 2026-09-29 and the W0-3 brief): appended one line, kind `protocol`,
   count 0, window_id `research-window-v2`, research_window_sha256 `62cf2cfa...3584`, trial_id `ea824f555f4241cb`,
   **prev_sha256 `00c901da636e9ae0cf758020f3fa3bd55ef3912899b4bf912de8ae631b9e0d93`** (= the fold head of the 37 v7
   lines recorded in part 2b). Validated through `backtest_integrity`: `ledger_read` verifies the chain (38 lines: 37
   construction unchained + 1 protocol chained), **chain head `f7043d8130636a23365e8703e4c190dfdb5cd4ec0098a9d990d0c4a31196bcf8`**
   (= SHA-256 of the protocol line), `prev_sha256` = `chain_head` of the 37 lines before it, `is_event` true,
   `trial_counts` 0 for the line, **`ledger_n(records, True)` = 37** (38 for a new cell), `ledger_counts` lists
   `construction` only. Ledger file SHA-256 `a2c24f56...0810` -> `8906fea393ba4e68995f7c85849c5bbc538d907f7157681a2637ada7cab19ccc`.
   Every line after it must now carry `prev_sha256` (the chain is live).
3. **Lock** `research_cycle.py lock scripts/specs/v8/base-lo1.json --write` (base-lo3 not locked: PM5-17): exit 0,
   four null pins filled, nothing else changed (semantic JSON diff against HEAD; the file is re-serialised with
   `indent=2`, hence 148 + / 26 - lines): `fields.manifest_sha256` `888e6616...b8695`, `inputs.role.sha256`
   `2ff9d771...1e53`, `inputs.identity_bridge.sha256` `f598c04c51e0bea33bd730a3a2beb3a9f76abe0b9630c75f21c70ad0ec3643de`,
   `inputs.fund_events.sha256` `304d2945d6226c0ca9b56d1fb6e8309ae1f42da5dfcd9652aee1a83dc29be87b`. A dry `lock` after
   it exits 0 (every pin equals its file). **Spec file SHA-256 `654067870a7c...c246` -> `88031699fdf4b0d5616b071b8dccc4c8414d7e894c9a46280a94a43b751f40bc`**
   (the cell's spec digest: a plain spec's digest is its file SHA, `research_spec.py:233-242`). Commit `f93868bf`.

### What the base-lo1 lock pins, and what it does not

- **Pinned in the spec and verified by every `plan` / `run`** (`research_cycle.verify_inputs`, `research_cycle.py:783-797`:
  PIN MISMATCH exit 3): library `fund_industry_ic_v71.json` `787c802e...2259` and recipe
  `fund_industry_ic_v71.recipe.json` `7f8a2643...b271` (pinned before; unchanged), role lo1 manifest `2ff9d771`, identity
  bridge r4-v2 manifest `f598c04c`, fundamental events v3 manifest `304d2945`, as-built fields v9 lo1 manifest `888e6616`
  (the fields phase is pinned, never built by the cycle). The role's `universe.id` must read `linked-operating-v1`.
- **Pinned by the spec digest** `88031699...40bc`: every other spec value (exe paths, flags, caps, output names, gate,
  fit / card / monitor / summ settings). It is the verdict's `spec_sha256` and the NAV resume binding
  (`cycle_binding.json`, review C-13); any edit of base-lo1.json changes it. It also moves the template-chain digests of
  every v8 template resting on base-lo1 (B0c, r1 ..., nothing run or locked on them).
- **Not pinned by the lock (named by path only):** the executables (`build-equity/bin/atx-equity-strategy-ic.exe`,
  `atx-equity-strategy-targets.exe`), the python interpreter, and the tool scripts. They are held by the build receipt
  and the commit: every bounded-run receipt records `executable_sha256` and `source_sha`, and the cycle refuses a dirty
  code pathspec before each phase. State at lock (what must not change before the freeze gate unless re-ruled): exes
  IC `ab7e2cbd...452d`, NAV / targets `5497c89d...9bca6` (risk `8967952c...d258`, mine `cd661fe9...c91d`; v8-12);
  code pathspec = `1cc4c6c9` + the locked spec (`f93868bf`); scripts `run_bounded_research.py` `81b5de1e...1bc5`,
  `research_cycle.py` `f6b4043d...c829`, `research_spec.py` `9dbb7879...5101`, `research_ledger.py` `c90edb6e...162d`,
  `research_tree.py` `c313a510...151c7f`; tools `fit_composition_weights.py` `8860483c...839d`, `alpha_report_card.py`
  `ade2d777...dbe0`, `book_monitor.py` `628b2e6e...a7d`, `nav_summ.py` `719ac51f...db7d`, `backtest_integrity.py`
  `2e195823...d36e`, `compare_window_overlap.py` `b4d7f2e8...8896`. The candidate cache
  `build-equity/mega-candidate-cache-v8-lo1` is content-keyed and verified per entry (unchanged by (ii): 144 files,
  3,052 MiB, no file newer than the weights file).

### Tests after the lock: FINDING (design, not fixed)

`scripts/tests` after `f93868bf`: **20 failed, 162 passed, 4 skipped** (part D: 183 / 3; the fourth skip is the
env-gated `test_cycle_e2e.py:182`, `ATX_EQUITY_BIN` unset). All 20 failures are in `scripts/tests/test_research_spec.py`
(alone: 20 failed / 23 passed): the 12 `test_every_v8_spec_loads_and_plans[*]` cases, the r1 / r10 / r11 tests, the
e27b add-alpha test and the three add-alpha tests. `test_research_cycle.py` + `test_cycle_scoring.py`: 96 passed / 3
skipped (env-gated). Cause (from the code and the error text, e.g. "lock: role pin 2ff9d771... differs from the file
(f31a9064...); --relock to replace"): the tests read the live `scripts/specs/v8/*.json` (`V8`, `:30-31`; templates
resolve their parent chain from that directory) and plan or lock them in a temporary root of stand-in input files
(`fake_root`, `:86-110`; `v8_root`, `:624-647`), expecting base-lo1's pins null (`NULL_PINS`, `:37-45`, "as planned
today"). With real pins the stand-ins mismatch (`verify_inputs` PIN MISMATCH, `lock_pin` refusal without `--relock`,
`research_cycle.py:1818-1833`). The A2 root sequence (`task-A2-report.md`, root command sequence) locks base-lo1 before
B0a, so this break was built into the fixture design; fixing it means changing how the fixtures copy specs and resolve
template chains (null the root-filled pins in the copies, or relock on stand-ins), not a one-line slip, so per the
integrator rules it is described, not fixed. No product code is wrong: the cycle on the real root plans against the
real files. Reverting the lock would undo PM5-17.

### Disk

C: 42 G free at start; **43,017,228 KiB (41.0 GiB) free after** (`df -h` 42G). New: `pm5-16-pin3y-u-lo1-1` 14 MiB, the
weights dir < 1 MiB, four run dirs and one report < 1 MiB each.

### Hidden-data record

- **Inputs opened by the tools:** (i) the two `train_daily_ic.csv` (every row asserted before 2024-01-01; rows on or
  after 2022-09-30 dropped in memory); (ii) the library, the 3-year `orientations.json` (2020-2022), role lo1 (to
  2023-12-29), the fields v9 lo1 manifest and the candidate cache (all hits), then the two daily-IC CSVs (overlap tool,
  `--before 2022-09-30`, seal from `research_window.py`). The ledger verb read `research_window.json` and the ledger.
- **Logs:** the eight stdout / stderr logs of the four runs scanned by count for dates in 2024 or later: 0 each (count
  only; the IC run's stdout was not displayed).
- **What I read:** receipt fields; the (i) script's dict (counts, booleans); the weights script's SHA and counts; the IC
  `summary.json` status, role pin, cache counters and weights record; the overlap report's alignment, `differing_keys`,
  the `__combined__` row (`bit_identical`, `max_abs_diff`, cells, unequal, NaN, missing, status, reason) and the three
  member rows' unequal and NaN counts; ledger kinds, N, chain head and the appended protocol line; spec pins; source
  files. **Disclosure:** the weights script's `unoriented` count says 7 of the 48 candidates had sign 0 in the 3-year run
  (horizon-21 mean rank IC undefined or exactly 0); no id was attached. **No IC level, sign of a named candidate,
  orientation value, weight, return or Sharpe was read, printed or summarised. Nothing dated 2024-01-01 or later was
  opened.**

### Ledger

`build-equity/trials.jsonl`: 38 lines (37 construction + 1 protocol), chain head `f7043d81...bcf8`, **N = 37**.

### Fixes

None to source. Commits: `da805a2a` (cause-test scripts), `f93868bf` (prereg pins, base-lo1 lock), and this log.

### Open items

- **Tests:** `scripts/tests/test_research_spec.py` 20 failures after the base-lo1 lock (above); needs a test-fixture fix
  (lane A2's design) before the next "suites green" gate. base-lo3's lock after B0a will hit the same fixtures.
- **progress.md** not written by me: R14 of the runbook asks for the R1-R13 pins, seconds, peaks and the W0-a outcome
  there; every number is in this log and parts 1, 2a, 2b.
- W0-a on report 3 is closed by outcome (a) (PM5-18 (3)); the four-key `stop` class of the daily-IC report remains by
  design (member diagnostics carry the run's own window sign).
- base-lo3: lock after B0a (PM5-17). Carried: R11 has no overlap report (open question 30); R15 and fields v10 not run.

## cells batch 1a: B0a, base-lo3 lock, B0b (2026-10-01)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `7a185d9c` (clean; code head `1cc4c6c9`;
every research executable on v8-12). Procedure `task-CELLS-brief.md`; rulings W0-a/b/c, A-3, E-18, E-24, E-28, E-29,
E-34, E-37, PM5-17, PM5-18, PM5-20, PM5-21 read. Not run: B0c, R15, anything after B0b. End: `3d25e2a4` (base-lo3 lock)
+ this log, tree clean.

### A. Preconditions (before B0a)

- Tree clean at `7a185d9c`. Executables on disk = v8-12: IC `ab7e2cbd...1615452d`, NAV / targets `5497c89d...35b9bca6`,
  risk `8967952c...a7eed258`. Freeze list (PM5-21) re-hashed, all equal to the part 2c record: `run_bounded_research.py`
  `81b5de1e`, `research_cycle.py` `f6b4043d`, `research_spec.py` `9dbb7879`, `research_ledger.py` `c90edb6e`,
  `research_tree.py` `c313a510`, `fit_composition_weights.py` `8860483c`, `alpha_report_card.py` `ade2d777`,
  `book_monitor.py` `628b2e6e`, `nav_summ.py` `719ac51f`, `backtest_integrity.py` `2e195823`,
  `compare_window_overlap.py` `b4d7f2e8`; `research_window.json` `62cf2cfa`. No executable, cycle or tool script changed
  in this batch (PM5-21); the only commit is the base-lo3 lock (a spec of a cell not yet run) and this log.
- Ledger `build-equity/trials.jsonl`: 38 lines (37 construction + 1 protocol), file `8906fea3...9ccc`, chain head
  `f7043d81...bcf8`, N 37. Disk 41 G free (`df -h`), RAM 5,614 MiB free.
- Read-order discipline: each cell was run `--stop-after nav`; mechanics were read from the NAV dir with nav_summ's own
  `construction_stats` (a scratch reader printing only gross, net, tau, row and accounting fields, no return); then
  `run` resumed monitor and summ, the only phases that print returns.

### Cell B0a (base-lo1; role lo1; no parent; criterion none: the re-base)

**Spec** `scripts/specs/v8/base-lo1.json`, digest `88031699fdf4b0d5616b071b8dccc4c8414d7e894c9a46280a94a43b751f40bc`
(locked at R14). **Pins** (plan, every one `[locked, verified]`): library v7.1 `787c802e`, recipe `7f8a2643`, role lo1
`2ff9d771`, identity-bridge-r4-v2 `f598c04c`, fundamental-events-v3 `304d2945`, fields v9 lo1 as built `888e6616`
(63 rows = the spec list). Commands: `research_cycle.py plan`, then `run --stop-after nav` (source `7a185d9c`), then
`run` (resume: fields, u, fit, card, gate, w, nav done; NAV binding checked, spec `88031699`, argv `50062ffa...613b`).

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| u | 300 s / 2,560 (W0-c) | 29.2 | 667 | 0 | `151a76cb2513d51145bdbc5685c8c91774edd47703935f6a8d7f7bc4242ea112` | `summary.json` `01d4fddb...b0ad`; cache **48 / 48 signal hits, 48 / 48 IC-result hits, 0 misses, 0 VM evaluations** |
| fit | 180 / 1,536 | 33.9 | 431 | 0 | `aae495ffcc3063393d3d4480b19bcd4dba920ae787a87fc7e983c61e13ee808c` | `composition_weights.json` `8c5b6e06...9cd9`, `admission.json` `f51814c7...24b1` |
| card | 300 / 2,560 | 30.1 | 1,292 | 0 | `fdbf58ff4ab8b6a7ceff4c90674c3011fcfaba29e81e816bc2967ee5b5c9c3d7` | `daily_sleeve.csv` `e0aa312d...4d3b` |
| gate | internal | - | - | PASS | - | `b0a-readout`: admitted list empty (0 admission lines); 12 report rows |
| w | 300 / 2,560 | 24.2 | 667 | 0 | `ca87620a0fdb46d9bd382e786fa60b05c85499c6311b3076e0321a2f28c1593f` | `train_combined.json` `1705850b...d78c` |
| nav | 180 / 1,536 | 23.2 | 447 | 0 | `663baba5033bc136bff55e4d67184fd441e58bee4e278e4c1aaff152c3fe0a95` | `summary.json` `eeac8366...05a8f`, `recipe.json` `66aee6fb...d8c1`, S2 daily `6f7c6405...8cf9` |
| monitor | direct | 0.9 | - | 0 | - | `monitor.json` `541de8be...ead1` (status alarm: M2; flags only) |
| summ | direct | 2.5 | - | 0 | - | `cycle-v8-b0a-lo1/summ.json` `d5cbc369...c3f`, `pbo.json` `2b9a00d1...6fd`, `cycle_verdict.json` `40e519c1...344e` |

Every bounded receipt: outcome completed, `git: clean in the code pathspec`, source `7a185d9c`, exe = v8-12 (IC
`ab7e2cbd`, NAV `5497c89d`; Python `624bbc05` for fit / card). The warm u pass reproduces R13's cold u pass
(`w0-2-v71-u-lo1-1`) byte for byte in `recipe.json`, `orientations.json`, `train_daily_ic.csv`, `train_combined.*`,
`train_planned_targets.csv`; `train_candidates.jsonl` differs only in timing and cache-status paths (`stage_seconds.*`,
`wall_seconds`, `signal_cache`, `ic_result_cache`). **Weights `provenance.script_sha256` = `8860483ce017...839d`** (the
FIX-5 fitter), v7.1's file `4cff96b606f9...399d`: the expected move (FIX-5), now seen in a fitter-written file.
Gate report rows (no gate): 11 admitted, `mom_12_1` reject_redundant (max |rho| .9857 with res_mom_12_1); report
only, gates nothing (b0a-readout admits nothing, as registered).

**Mechanics (S2 = `modeled-1bn-stale5-v1+swap-fin-v1`, the primary; read before any return): PASS.** All-rows gross
.9709 in [.90, 1.05]; all-rows net +.0038 (|.| <= .02); tau mean .0367 <= .20, p95 .0439 <= .30 (1,003 tau sessions;
the summary's own meets_daily_turnover_mean / _p95 true). 1,006 CSV rows 2020-01-02..2023-12-29. Accounting checks:
max cash-book error 7.1e-14, return identity 3.6e-16 (tol 1e-9). Max gross 1.128, max |net| .029. Post-ramp gross
.9926. **Row score_begin (A-3):** role row 399 = 2020-01-02 = CSV row 0: gross 0.0 (no warm start, K = 0: the flat
start; first fills on row 1, 2020-01-03, gross .059; deployment row 1). No `warm_start` block (none asked). **Beta:**
aim-partial-v5 carries no beta limit and the NAV summary records none; the v8 mechanics list (pitch config
`v8.mechanics`: gross, net, tau mean, tau p95) has no beta item; nothing to check.

**Statistics of record:** B0a has no parent: no paired test. S2 net Sharpe **+1.125** (ledger `s2_net_sr` 1.12540;
T 1,004). DSR block of the verdict (`--dsr-ledger`): N 38; **pre-registered cross-trial variance undefined** (1 cell
ledgered on research-window-v2, `cell_count` null); legacy variance (37 cells) DSR .7070 (gates nothing). Beside:
listing DSR (V[SR_n] of the 38 listed dirs) .7107; effective-N DSR .8770 (ONC N_eff 2); PSR vs 0 .9847; CSCV PBO .2020
over 38 cells (exhaustive 12,870 splits, common sessions 2020-2022).

**Criterion:** none (re-base; W0-b). **Verdict:** B0a is the re-base, ledgered; mechanics pass; it is B0b's parent.
**N after: 38** (ledger 39 lines = 38 construction + 1 protocol; `ledger_n` 38; appended 1, skipped 37; trial
`9ee007561225d964`, origin prior, window_id research-window-v2, prev = the protocol head `f7043d81`; chain head
`bc95ae4f...ac1e`; file `40bb8fac...fb2f`). Matches the brief.

Year table (S2; return rows, net return, net Sharpe, vol, tau mean, cost bps per traded dollar):

| year | rows | net return | net Sharpe | vol | tau | cost bps |
|---|---|---|---|---|---|---|
| 2020 | 251 | +.0064 | +.162 | .0458 | .0477 | 15.66 |
| 2021 | 252 | +.0831 | +2.470 | .0325 | .0331 | 12.06 |
| 2022 | 251 | +.0888 | +1.947 | .0444 | .0330 | 12.76 |
| 2023 | 250 | +.0032 | +.112 | .0339 | .0332 | 12.11 |

**Net Sharpe at 4x NAV:** base-lo1 carries no capacity curve (no `--capacity-curve` in its NAV flags; E-29 puts it on
B0c); not reported, not added. **Appendix A:** `TRAIN construction cells 38; admission trials this sprint 0; window
research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024); history
reads 0; 2025+ never read.` Defects: none; fixes: none.

### base-lo3 lock (PM5-17)

`research_cycle.py lock scripts/specs/v8/base-lo3.json` (dry, exit 0), then `--write` (exit 0): exactly six null pins
filled, nothing else changed (semantic JSON diff against HEAD; the file re-serialised with `indent=2`, 150 + / 28 -):
`fields.manifest_sha256` `9f1563638b5e4f7ead7be686803b96a0707ada2c608fcbc6dc084179bd9021ef` (= prereg pin),
`inputs.role` `e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4` (= prereg pin),
`inputs.identity_bridge` (atx-db export identity-bridge-v2-pit manifest, hashed, not opened)
`09aac28f757fa959b0ed4cd9296b2267940e70af98e0d2c67cc45b1df4f7fa01` (= runbook V2PIT, R8), `inputs.fund_events`
`304d2945d6226c0ca9b56d1fb6e8309ae1f42da5dfcd9652aee1a83dc29be87b`, `inputs.sic_events` (atx-db fundamentals manifest)
`9f9b2f85f6bcd5c7f3a55aee097893094a5cb85ab2b4edbfb582297dab06816b` (= runbook SIC, R8), `inputs.reference_cell` = B0a's
NAV `summary.json` `eeac83664e9a17936dd61dfe8932f3b54eeb0051cd31a779d6208880edf05a8f`. A dry lock after it exits 0.
**Spec digest `537760bc3802fb731bc06cfbec5d5a059106e1576a79d6579a2327f8d88221d7`** (file SHA; plain spec). Commit
`3d25e2a4`. Library and recipe pins unchanged (`787c802e`, `7f8a2643`).

### Cell B0b (base-lo3; role lo3; parent B0a; accepted on paired S2 net dSR > 0 against B0a and mechanics, W0-b)

**Spec** `scripts/specs/v8/base-lo3.json`, digest `537760bc...d221d7`. **Pins** (plan, all `[locked, verified]`): the
six above plus library / recipe; fields v9 lo3 63 rows = the spec list. summ: `--reference` B0a's NAV dir, `--dsr-n 39`.
Commands as B0a (source `3d25e2a4`; resume binding spec `537760bc`, argv `3eb2b854...319b`).

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| u (cold) | 300 s / 2,560 (W0-c), 4 workers | 143.1 | 1,577 | 0 | `32a571f331335fe40a0b000cc9153f68693ff966cf406e026676702f28eef7e9` | `summary.json` `bb34cf3d...737e`; cache cold: **0 hits, 48 misses, 48 VM evaluations**; new cache `mega-candidate-cache-v8-lo3` 3,052 MiB |
| fit | 180 / 1,536 | 42.9 | 491 | 0 | `0d4a38cbb7927a66380d022b6e442d7b5511b282af23b7b8084eb191ce3f3b53` | weights `ddf1417e...de26` (script_sha256 `8860483c`), admission `29357ece...a18c` |
| card | 300 / 2,560 | 31.8 | 1,439 | 0 | `8df755c0bad4588a0f9f6ec72f30314c8a02650ab47b3add35f316f8359eb351` | `daily_sleeve.csv` `80ba33d5...007a` |
| gate | internal | - | - | PASS | - | `b0b-readout`: admitted empty (0 admission lines); report rows as B0a (11 admitted, mom_12_1 reject_redundant) |
| w | 300 / 2,560 | 23.6 | 672 | 0 | `6dab38cf3413fe04fd4cfef14a603c853110568dd7d3b017435f080ef20fcf41` | `train_combined.json` `b1903f7c...945b` |
| nav | 180 / 1,536 | 23.2 | 447 | 0 | `3346d242c5da13d83a797cadbfd9738abb6e5bfde8a17b16153e48d952faf1ab` | `summary.json` `82a596c1...4ad8`, `recipe.json` `c212e9f3...70b0`, S2 daily `b9913f6e...db1c` |
| monitor | direct | 0.9 | - | 0 | - | `monitor.json` `c034ae6e...afd` (status alarm: M2; flags only) |
| summ | direct | 15.7 | - | 0 | - | `cycle-v8-b0b-lo3/summ.json` `7cda2e9c...80c`, `pbo.json` `90949d50...527c`, `cycle_verdict.json` `5e94286d...f8a8` |
| paired one-sided p | bounded 180 / 1,536 | 0.8 | 475 | 0 | `47d61c46e328d0f27c0bda419ceb30b82b8e221ecfbf3efac9149faf4ed0e432` | `nav_summ.py --protocol v8 --bundle B0a B0b --bundle-json build-equity/v8-cells-b0b-bundle.json` `75c30860...3191` (no `--ledger`: no line written) |

Every bounded receipt: completed, clean, source `3d25e2a4`, exe = v8-12. The cycle's per-dir paired block prints only
the two-sided studentized p (`paired_stats` without `one_sided`); the one-sided p (E-34) comes from nav_summ's
`--bundle` path, the same `paired_stats` on the same aligned series with `one_sided=True` (the source the v8 ladder of
`mega_report/v8.py` reads). Its dSR, SE, CBB CI and two-sided p equal the cycle's to the last digit. Its own "verdict
FAIL" line is the freeze-gate test (p < .10, item 9), not the cell rule, and is not used.

**Mechanics (S2; read before any return): PASS.** All-rows gross .9663 in [.90, 1.05]; all-rows net +.0035; tau mean
.0370 <= .20, p95 .0439 <= .30 (1,003 sessions; summary flags true); 1,006 rows 2020-01-02..2023-12-29; accounting
5.0e-14 / 3.5e-16; max gross 1.128, max |net| .028; post-ramp gross .9879. **Row score_begin (A-3):** role row 399 =
2020-01-02: gross 0.0 (K = 0, flat start; row 1 gross .059; deployment row 1). Beta: none to check (as B0a).

**Statistics of record (paired S2 net dSR vs B0a; studentized CBB, block 21, seed 20260929, 4,999 resamples, 4,999
valid):** S2 net Sharpe B0b **+1.1389** vs B0a **+1.1254**; **dSR +.0135**; rho .9968; T 1,004; **Memmel SE .0403**
(t +.33); CBB 95% [-.0759, +.0987]; LW studentized SE .0449, 95% [-.0800, +.1071]; **bootstrap p one-sided .3820,
two-sided .7754**. Per-year dSR: 2020 +.159, 2021 -.110, 2022 -.005, 2023 +.002. DSR block: N 39; pre-registered
variance from 2 cells on the window (B0a, B0b: V[SR] 3.62e-07) gives SR0 .021 ann and cell-count DSR .9843 (see open
items); legacy variance (37 cells) DSR .7136; listing DSR .7206; effective-N DSR .8806; PSR vs 0 .9858; PBO .1828 over
39 cells.

**Criterion:** none besides dSR > 0 and mechanics (W0-b). **Verdict (prereg rule 5, Ruling W0-b): B0b ACCEPTED** --
paired S2 net dSR +.0135 > 0 AND mechanics PASS. Recorded by the tooling: `cycle-v8-b0b-lo3/cycle_verdict.json`
(paired block, spec `537760bc`, ledger head) and the ledger line. **N after: 39** (40 lines = 39 construction + 1
protocol; `ledger_n` 39; appended 1, skipped 38; trial `145e34275831ce48`, prev = B0a's head `bc95ae4f`; chain head
`ba3f4f70...4270`; file `a49acf02...6dd5`). Matches the brief.

| year | rows | net return | net Sharpe | vol | tau | cost bps |
|---|---|---|---|---|---|---|
| 2020 | 251 | +.0135 | +.322 | .0450 | .0479 | 15.48 |
| 2021 | 252 | +.0769 | +2.360 | .0316 | .0335 | 12.03 |
| 2022 | 251 | +.0862 | +1.941 | .0432 | .0332 | 12.80 |
| 2023 | 250 | +.0033 | +.115 | .0340 | .0333 | 12.16 |

**Net Sharpe at 4x NAV:** base-lo3 carries no capacity curve; not reported, not added. **Appendix A:** `TRAIN
construction cells 39; admission trials this sprint 0; window research-window-v2 (2020-2023); hidden 2024+ unread in
this sprint; validation reads before v8: 2 (2023-2024); history reads 0; 2025+ never read.` Defects: none; fixes: none.

### Winner and next parent

The rule gives **B0b** (accepted): role **lo3** (`train-2020-2023-lo3`, linked-operating-v3) carries on. Next parent:
`scripts/specs/v8/base-lo3.json` (spec `537760bc`). Per the A2 root sequence B0c then sets `"parent": "base-lo3.json"`
and re-points `change.inputs.label_role` to `build-equity/train-2020-2023-lo3-dlret` (R15 builds the lo3 dlret role;
W0-j's lo1 path is not needed). Not done here (next dispatch).

### Hidden-data record

- Inputs opened by the tools: roles lo1 / lo3 (to 2023-12-29), fields v9 lo1 / lo3, library v7.1, the lo1 cache, the
  new lo3 cache, the 37 ledgered v7 NAV dirs (2020-2022, the summ grid), the ledger. The two atx-db manifests were
  hashed by `lock` (bytes only, W0-k), not parsed. No tool asked for a file dated 2024-01-01 or later.
- Logs: every stdout / stderr of the 11 bounded runs and the four cycle consoles scanned for dates 2024-2029: 0 hits.
- What I read: plans, receipts, cache counters, the mechanics fields above, then (after mechanics) the cycle's summ
  output for B0a / B0b rows, the verdicts, the ledger. The summ console also prints the 37 v7 cells' rows (2020-2022,
  already read in v6 / v7); not used. **Nothing dated 2024-01-01 or later was opened. No disclosure.**

### Disk

C: 41 G free at start; **39,088,996 KiB (37.3 GiB)** after B0b. New: `mega-candidate-cache-v8-lo3` 3,052 MiB, four IC
dirs 93 MiB each, cards 8 MiB each, NAV dirs ~5 MiB each, `fit-work` 260 MiB total.

### Open items

- **Pre-registered DSR variance (rule 3) is degenerate until the W0-4 step 4 re-runs are ledgered.** At B0a it is
  undefined (1 cell on research-window-v2); at B0b it rests on B0a and B0b alone (rho .997, V[SR] 3.6e-07), so SR0 is
  .021 ann and the cell-count DSR .984 is not meaningful. It gates no B0 cell (acceptance is dSR, mechanics,
  criterion); it does gate the freeze gate (item 9). The re-runs of ledgered v7 cells on the 4-year role (W0-4 step 4,
  add 0 to N) are not in this batch; the PM schedules them before any DSR is read as evidence.
- One-sided p: the cycle's summ prints the two-sided p only; each later cell needs the same bounded `nav_summ --protocol
  v8 --bundle PARENT CELL --bundle-json` read (no ledger), as here. A tool change would be PM5-21 territory; none made.
- Monitor: `status alarm` (M2) on both cells in baseline mode (in-sample CUSUM over the reference itself); flags only,
  gates nothing; the member was not identified (an IC statistic).
- `scripts/tests/test_research_spec.py` (PM5-20, FIX-6): the base-lo3 lock now also fills pins its fixtures expect null;
  not run here.
- Carried: R15 (lo3 dlret role), B0c, diagnostics, Release A/B (batch 1b).

## cells batch 1b: R15, B0c, B0c diagnostics, Release A/B (2026-10-01)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `7ac86a54` (clean; code head `1cc4c6c9`;
research executables on v8-12: IC `ab7e2cbd...1615452d`, NAV / targets `5497c89d...35b9bca6`, risk
`8967952c...a7eed258`). Procedure `task-CELLS-brief.md`; read: integrator-rules, v8-prereg rules 1-11 and rulings,
A2 root sequence, plan W0-4 / G-1..G-3 / E-1-E-2 / 12.1, rulings W0-b, W0-c, A-3 (FIX-AB finding), E-2 (OD-3 / OD-7;
no Release ruling under that id), E-10, E-18, E-24, E-25, E-28, E-29, E-34, E-37, E-39, PM4-5, PM5-18, PM5-21,
PM5-22, PM5-23, batch 1a's log, runbook R8 / R15, task-DLRET and task-E25 reports, task-G report.

Preconditions: freeze list (PM5-21) re-hashed before the first run, all equal to batch 1a's record
(`run_bounded_research.py` 81b5de1e, `research_cycle.py` f6b4043d, `research_spec.py` 9dbb7879, `research_ledger.py`
c90edb6e, `research_tree.py` c313a510, `fit_composition_weights.py` 8860483c, `alpha_report_card.py` ade2d777,
`book_monitor.py` 628b2e6e, `nav_summ.py` 719ac51f, `backtest_integrity.py` 2e195823, `compare_window_overlap.py`
b4d7f2e8, `research_window.json` 62cf2cfa; exes as above). Ledger 40 lines (39 construction + 1 protocol), file
`a49acf02`. Disk 38 G free, RAM 6.6 G free. No executable, cycle or tool script changed in this batch; commits: the
B0c lock (a spec of a cell not yet run), this log, the diagnostics copies.

### R15: delisting-returns label role on lo3

Command: runbook R8's argv plus `--delisting-returns $V1/delisting`, output `build-equity/train-2020-2023-lo3-dlret`
(task-E25-report "Building the label roles", lo3), through `run_bounded_research.py` under the preparation caps
600 s / 2,560 MiB / 512 (as R8 ran), the tool's own limits as the runbook (`--memory-mib 1024 --max-seconds 170`).
The four stage pins were re-hashed immediately before: base `de8d91db`, identity-bridge-v2-pit `09aac28f`,
fundamentals (SIC) `9f9b2f85`, delisting `1b1166b6` -- equal to R8's receipt bindings; `prepare_recent_research.py`
unchanged since R8's source `2d3e14ef` (`git diff` empty); interpreter SHA `624bbc05`.

| step | receipt dir | source | outcome / exit | s | peak MiB | receipt.json SHA-256 | manifest SHA-256 |
|---|---|---|---|---|---|---|---|
| R15 | `train-2020-2023-lo3-dlret-run` | `7ac86a54` | completed / 0 | 4.7 | 483 | `06d849f2b5498703c16145208648a0903b7aa87275b755ea630f727808737121` | **`95e16cfe3ad0e0c9e7cf94acb6df4004e6f286ae59dff7c0e725ea33aace5069`** |

- Manifest 587.8 KB (under the C++ 1 MiB cap); role 207 MiB on disk. `universe.id` linked-operating-v3, dates 1,405,
  score_begin 399, score_end 1,405. `universe.delisting.returns_applied` true; `applied`: terminations 867,
  members_cleared_on_termination_session 0, skipped no_delist_return 123 (every other skip 0);
  `universe.inputs.delisting` pins the stage manifest `1b1166b6` and `events.parquet`.
- Against the decision role lo3 (`e1c67101`): `sessions.i64`, `ids.u64`, `member.u8` byte-identical;
  `present.u8`, `close.f64`, `raw_close.f64`, `volume.f64` differ (the patched termination cells);
  `kept_member_counts` equal (1,405 entries). The NAV's E-25 pair check admitted it (B0c below).
- No fields build on the dlret role: the runbook's "then an R11-style fields build" predates E-10 / E-25 (the label
  role marks the NAV only; signals, fields and decisions stay on lo3, fields v9 lo3 `9f156363`).

### Cell B0c (base-b0c on base-lo3; lo3 + label role lo3-dlret, warm start 60, capacity curve; baseline by declaration)

**Spec** `scripts/specs/v8/base-b0c.json` (template). Edits: `"parent": null` -> `"base-lo3.json"`;
`change.inputs.label_role` dir / path `train-2020-2023-lo1-dlret` -> `train-2020-2023-lo3-dlret` (A2 root sequence);
then `lock --write` filled `label_role.sha256` `95e16cfe` and wrote the derived pins into `locked`: reference_cell
= B0b's NAV `summary.json` `82a596c1`, reference_admission = B0b's `admission.json` `29357ece`. A dry lock after it
exits 0. Nothing else changed (nominal_parent, description, nav.output, the nav flags `--warm-start-sessions 60`,
`--capacity-curve` as the template had them). File SHA `62739baf58e555bc05d4fc426d70b58aebb730ceb93236a4177a7635ca15d44d`;
**spec digest (template chain) `059d9ba6d7cb0f6346c5b065c64fd5ea55940db7196c2200e950ce137c7b483d`** (the verdict's
spec_sha256 and the NAV binding). Commit `2ad09c13`.

**Pins** (plan, every one `[locked, verified]`): library `787c802e`, recipe `7f8a2643`, role lo3 `e1c67101`,
identity_bridge `09aac28f`, fund_events `304d2945`, sic_events `9f9b2f85`, reference_cell `82a596c1`,
reference_admission `29357ece`, label_role `95e16cfe`; fields v9 lo3 as built `9f156363` (63 rows = the spec list).
Commands: `plan`; `run --stop-after nav` (source `2ad09c13`); mechanics read; `run` (resume: fields, u, fit, card,
gate, w, nav done; NAV binding spec `059d9ba6`, argv `cbb52310ff1a20c60adf86f38addbd88103dc15005c78709716b580830ce938e`).

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| fields, u, fit, card, w | - | - | - | - | - | done: B0b's outputs (a NAV-only change; the template keeps them, resumed as done) |
| gate | internal | - | - | PASS | - | `b0b-readout`: 0 status changes against B0b's admission |
| nav | 180 / 1,536 | 50.0 | 586 | 0 | `f85e9c1d7408d3f7f842ed4f096ae43c2b57665ebee6620180d9e800711ac6fc` | `summary.json` `119d4cd0...1615`, `recipe.json` `f0d28ded...44f7`, S2 daily `78360530...89d4`, `capacity_curve.csv` `e56431da...26ac` |
| monitor | - | - | - | - | - | done: B0b's `mega-monitor-v8-b0b` (`c034ae6e`); the template keeps the parent's monitor output and its inputs (u daily IC, admission, fit work, card sleeves) are B0b's; the NAV is not an input |
| summ | direct | 15.5 | - | 0 | - | `cycle-v8-b0c/summ.json` `53bd338f...7375`, `pbo.json` `9fa8fd15...9941`, `cycle_verdict.json` `eadad51a...e6e6` |
| paired one-sided p (PM5-23) | bounded 180 / 1,536 | 0.8 | 508 | 0 | `228350f215da93f4d782af21a19fac5c987c249a5db7dc1a1d2b5ea01bfc98c4` | `nav_summ.py --protocol v8 --bundle B0b B0c --bundle-json build-equity/v8-cells-b0c-bundle.json` `fe849547...470f` (no `--ledger`) |

Every bounded receipt: completed, `clean in the code pathspec`, source `2ad09c13`, NAV exe v8-12 `5497c89d`.

**Mechanics (S2 = `modeled-1bn-stale5-v1+swap-fin-v1`, primary; read before any return): PASS.** All-rows gross
.9820 in [.90, 1.05]; all-rows net +.0036 (|.| <= .02); tau mean .0341 <= .20, p95 .0390 <= .30 (nav_summ, 1,004
sessions; the summary's meets_daily_turnover_mean / _p95 true, its own .03407 / .03901). 1,006 CSV rows
2020-01-02..2023-12-29, 1,005 return rows. Accounting: max cash-book error 1.24e-13, return identity 3.5e-16 (tol
1e-9). Max gross 1.128, max |net| .028; post-ramp gross .9885. Beta: none to check (aim-partial-v5; as B0a / B0b).

**Row score_begin (A-3).** `warm_start`: K 60, first decision role row 339 (2019-10-07), score_begin role row 399 =
2020-01-02 = CSV row 0; no scored deployment (`deployment.occurred` false). **S2 gross at score_begin .9257** (the
five books .9246-.9283); rows 1, 2: .9259, .9303. Against steady state: post-ramp mean (rows 63+) .9885 -> ratio
.9365, **6.4% below**; all-rows mean .9820 -> 5.7% below; rows 63-125 mean .958 -> 3.4% below. The D-0 test's
synthetic expectation (constant aim, theta .05, K 60) is 1 - .95^60 = .954 of steady state (4.6% below). So "within
5% of steady state" holds against the local early-2020 level, not against the 4-year post-ramp mean. It is not one of
the mechanics limits (gross, net, tau as v7); reported, no stop. Gross-only context: B0b's flat start stands at
.777-.799 on rows 59-62 (late March 2020) and B0c at .797-.819 on the same rows: the March 2020 dip in gross is in
both books.

**Label role (E-25):** the NAV admitted lo3-dlret against lo3 (exit 0); `summary.label_role`: 867 label-only present
cells (686 in scored rows) = the role's 867 terminations; membership equal (0 cleared).

**Statistics of record.** S2 net Sharpe **B0c +1.1328** (ledger `s2_net_sr` 1.13275; T 1,005, 2020-01-03..2023-12-29:
the warm start makes CSV row 1 a return row, one more than B0b). **Paired against B0b, for information only** (gates
nothing: B0c is the baseline by declaration, W0-b, prereg rule 6): studentized CBB, block 21, seed 20260929, 4,999
resamples (4,999 valid), 1,004 common sessions: SR B0c 1.1447 vs B0b 1.1389; **dSR +.0058**; rho .9994; **Memmel SE
.0174** (t +.33); CBB 95% [-.0172, +.0320]; LW studentized SE .0131, 95% [-.0266, +.0383]; **bootstrap p one-sided
.3014, two-sided .6792**. The one-sided p comes from the bounded bundle (PM5-23); its dSR, Memmel SE, CBB CI, LW SE /
CI and two-sided p equal the cycle's to the last digit; its "verdict FAIL" line is the freeze-gate test (item 9),
not used. On its own 1,005 rows B0c's SR is 1.1328; on the 1,004 common sessions 1.1447 (the extra row, 2020-01-03,
is a loss day): the unpaired difference to B0b is -.0062, the paired dSR +.0058. Per-year net Sharpe vs B0b (below
and batch 1a): 2020 +.319 vs +.322, 2021 +2.352 vs +2.360, 2022 +1.936 vs +1.941, 2023 +.118 vs +.115.
DSR block (verdict, `--dsr-ledger`): N 40; pre-registered variance from 3 cells on research-window-v2 (B0a, B0b, B0c:
V[SR] 1.82e-07, SR0 .0148 ann) gives cell-count DSR .9844 (degenerate until the PM5-22 re-runs, as at B0b); legacy
variance (37 cells) DSR .7072; listing DSR .7177; effective-N DSR .8763 (N_eff 2); PSR vs 0 .9855; CSCV PBO .1787
over 40 cells (12,870 splits, common sessions 2020-2022).

**Criterion:** none (baseline by declaration: prereg rule 6, W0-b, E-10). **Verdict: B0c is the v8 baseline**,
ledgered; mechanics pass; parent of R-1. Recorded by the tooling: `cycle-v8-b0c/cycle_verdict.json` (spec `059d9ba6`,
paired block, ledger head) and the ledger line. **N after: 40** (41 lines = 40 construction + 1 protocol;
`research_ledger.ledger_n` 40; invalid ids none; appended 1, skipped 39; trial `d24ef3e1a8211878`, origin prior,
window_id research-window-v2, prev = B0b's head `ba3f4f70`; chain head
`449847efa09efe2703fcde7c038df962a4202cd3d4299f6fe6bcd7c999971aa8`; file
`ed3f4139273a085ccdb6c834ac5307bf311231a19136b7807b91d8a0a6b260a1`). Matches the brief.

Year table (S2; return rows, net return, net Sharpe, vol, tau mean, cost bps per traded dollar):

| year | rows | net return | net Sharpe | vol | tau | cost bps |
|---|---|---|---|---|---|---|
| 2020 | 252 | +.0135 | +.319 | .0453 | .0362 | 15.67 |
| 2021 | 252 | +.0767 | +2.352 | .0316 | .0335 | 12.03 |
| 2022 | 251 | +.0859 | +1.936 | .0432 | .0332 | 12.80 |
| 2023 | 250 | +.0034 | +.118 | .0340 | .0333 | 12.16 |

**Capacity curve (E-29, report only).** `capacity_curve.csv` `e56431da`; x1 = the primary S2 book bit for bit
(`capacity_x1_equals_primary_bit_for_bit` true; the x1 daily CSV byte-identical to the primary).

| multiple | equivalent NAV | net Sharpe | gross Sharpe | cost bps / $ | capped fill share | tau |
|---|---|---|---|---|---|---|
| .5 | $0.5bn | 1.174 | 1.550 | 11.15 | .0011 | .0342 |
| 1 | $1bn | **1.133** | 1.552 | 13.13 | .0046 | .0341 |
| 2 | $2bn | **1.085** | 1.551 | 15.47 | .0325 | .0336 |
| 4 | $4bn | **.978** | 1.467 | 17.60 | .1206 | .0320 |
| 8 | $8bn | .851 | 1.326 | 19.24 | .2666 | .0291 |

PM4-5 disclosure: the capacity books scale only the impact law (m^.5) and the participation cap (1 / m); the aim is
the $1bn book's at every multiple (no per-multiple desired target in v8). E-15 / PM4-5's "the aim's ADV cap reads
the initial NAV" concerns `--adv-hold-q`, which B0c does not use; the 2x / 4x rows are the $1bn aim traded at NAV m,
report only.

**Appendix A:** `TRAIN construction cells 40; admission trials this sprint 0; window research-window-v2 (2020-2023);
hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024); history reads 0; 2025+ never read.`
Defects: none; fixes: none.

Findings (none a stop):
1. Warm-start gross at score_begin 6.4% below the 4-year post-ramp mean (the 5% expectation holds only against the
   local early-2020 level); above.
2. nav_summ under a warm start takes CSV row 0 (the first executed fill, the last warm-up decision's orders) as the
   deployment row and drops it from tau (1,004 sessions, mean .034069), while the NAV summary reports no scored
   deployment and counts it (.034070): `warning_tau` in summ.json. Gates nothing; nav_summ is frozen (PM5-21).
3. B0c's monitor is B0b's (see "M2 alarm" below).

**Next parent: `scripts/specs/v8/base-b0c.json`, spec digest `059d9ba6d7cb0f6346c5b065c64fd5ea55940db7196c2200e950ce137c7b483d`**
(file `62739baf`). R-1 sets `"parent": "base-b0c.json"`.

### B0c diagnostics (G-1..G-3; zero-trial, gate nothing, select nothing: prereg rule 8)

Commands: task-G-report "The commands root runs on a finished cell", every run through `run_bounded_research.py` on
the clean tree at `2ad09c13`. Caps: the NAV re-runs keep B0c's NAV caps 180 s / 1,536 MiB; every other step E-18's
600 s / 2,560 MiB. Nothing here is ledgered (no `--ledger`).

| step | receipt dir | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| holdings NAV: B0c's exact NAV argv + `--emit-holdings $N-h`, output `$N-hrun` | `$N-hrun-run` | 50.7 | 587 | 0 | `8ef2c54cd877b46ea6c1050b59ee3f89da1a1093e56c4349975cdd6bf3700526` | `$N-h` (f64, 163 MiB); all 21 CSVs (main + capacity), `summary.json`, `recipe.json`, `v7_extras.json` byte-identical to B0c's |
| lag-combined K = 1, 2, 3 | `b0c-lag{K}-combined-run` | 0.8 each | 342 | 0 | `87efe115...` / `959d6bb3...` / `29c4f83a...` | combined `43746fa3` / `04d42dbf` / `86f20458` (2,490,409 member cells each) |
| lagged NAV K = 1, 2, 3 (B0c's argv, only --combined / sha / --output changed) | `b0c-lag{K}-run` | 47.0 / 43.3 / 46.0 | 587 | 0 | `a171b806...` / `6b4b9a46...` / `62035400...` | `b0c-lag{K}/summary.json` `5c9bfe45` / `5be77297` / `e2788f68` |
| book-csv | `b0c-book-run` | 3.1 | 393 | 0 | `ee33966e19b64fa7c53e5fb2f00f554bb37d4976f635aa0501167df2dc3478de` | `b0c-book.csv` `f55121b9` (1,927,893 rows, 1,005 sessions, last 2023-12-28) |
| risk `--book-weights` `--emit-exposures all` (exe v8-12 `8967952c`; role lo3, fields v9 lo3) | `b0c-risk-run` | 35.5 | 640 | 0 | `3d5b68283cb33b26e931bbe083f17b2c260c8438e63d968b846a651ff7b2274c` | `b0c-risk/manifest.json` `5dd560d7` (481 MiB) |
| diagnostics G-1a, G-3d | `b0c-diagnostics-g1a-g3d-run` | 0.5 | 53 | 0 | `0ec0f5e1...` | `diagnostics-v8-g1a-g3d.json` `de1990b8` |
| G-1b, G-1c | `...-g1b-g1c-run` | 42.4 | 926 | 0 | `d3a0eccb...` | `...-g1b-g1c.json` `a985adf8` |
| G-2a | `...-g2a-run` | 3.9 | 364 | 0 | `29bc620a...` | `...-g2a.json` `e2a9874b` |
| G-2b | `...-g2b-run` | 96.7 | 1,163 | 0 | `2ac05ba9...` | `...-g2b.json` `86bf47af` |
| G-2c | `...-g2c-run` | 1.6 | 469 | 0 | `dd3ecc59...` | `...-g2c.json` `cbed04d7` |
| G-3a | `...-g3a-run` | 1.6 | 644 | 0 | `33263e8a...` | `...-g3a.json` `b44ea5d0` |
| G-3b | `...-g3b-run` | 5.0 | 938 | 0 | `ecc7690e...` | `...-g3b.json` `19e5e5da` |
| G-3c | `...-g3c-run` | 0.5 | 55 | 0 | `fc3e9818...` | `...-g3c.json` `9f00b5f3` |

(`$N` = `build-equity/mega-nav-v8-b0c-dlret-ws60-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247`.) Inputs: cards
`mega-cards-v8-b0b` (index `7ed72729`), weights `mega-weights-v8-b0b-ew` (`ddf1417e`), u pass `mega-v8-b0b-train-u-1`,
role lo3 `e1c67101`, fields v9 lo3, fit work `build-equity/fit-work`. All ten diagnostics `ok`. E-18's splits give
eight files (schema `atx.book-diagnostics/v1`, window research-window-v2), not one `diagnostics-v8.json`; copies
committed in `.superpowers/sdd/platform-v8-20260929/diagnostics-v8/` (bytes equal to `build-equity/b0c-diagnostics/`).
The risk verb's own bias harness printed `book: refused` (110 observations, 857 uncovered name returns, dropped share
.886 > .05) while factor (51 series) and random (64) were ok; G-2a reads the variance split, not the bias test, and
found every session complete.

What each shows (descriptive; S2 primary throughout):
- **G-1a** (cards; theta .05): weighted ic_theta .00366 (weight sum .961). Negative at theta: 8 members, weight .324
  (accruals, bac, bm, ind_adj_rev_5, ins_opp, iv_rv_spread, noa, smax5); unscored 6, weight .09 (ebit_ev, fscore,
  gpa, opbe, opex_at, rd_me). Largest contributions inst_best_ideas, high_52w, si_ratio, res_mom_12_1, ftd_fail.
  ic_theta from the decay curve (cards ran without `--ic-theta`); **f_theta and the K6 marginal IC are absent**
  (B0c's cards carry no C-2 admission columns; B0c has no marginal phase, so no K6 exists).
- **G-1b** (turnover attribution, NAV holdings): combined turnover book .0340 / model .0317. The four fast members
  (highest admission tau: ind_adj_rev_5, iv_rv_spread, seasonality_same_month, ea_overdue; weight .22): own share
  .454, **attributed share of the book's trades .596** (model .720), i.e. below the 75% the question posits on the
  book. By theme (own / attributed book): options_implied (iv_rv_spread alone) .281 / .371; reversal_seasonality .221
  / .224; low_risk .142 / .120; every other theme .075 or less. Non-linear remainder book .147.
- **G-1c** (netting): combined turnover / summed theme-sleeve turnover **.455** on the book (daily median .456, p5
  .399, p95 .519); .425 on the model; .316 against the 38 member sleeves: more than half of sleeve trading cancels.
- **G-2a** (ex-ante variance split, atx-risk on the book): 1,005 sessions, all complete; mean shares market .005,
  style .657, industry .182, **specific .156** (factor (market + style) .663 / industry .182 / specific .156);
  ex-ante vol 3.27% annual; specific share by year .180 / .217 / .097 / .127 (2020-2023).
- **G-2b** (IC h 21, weight-averaged): all .0112; volatility terciles low .0089 / mid .0103 / high .0144; ADV
  terciles low .0110 / mid .0101 / high .0105; top 1,000 by me_company .0085 vs the rest .0127.
- **G-2c** (held / ADV63, Q .10): held p50 .011, p95 .088, p99 .150, max 101.3 (one outlier cell); 3.6% of cells and
  **7.1% of held gross above Q**; aim / ADV p95 .110, 6.2% of cells and 11.1% of aim gross above Q: R-5's cap binds
  on a minority of the book.
- **G-3a** (S2-FEE, descriptive): fee drag 0.48% a year vs S2's tier fee 0.23%; restated net Sharpe **1.069 vs
  1.133 (-.064)**; short book by fee decile .079-.111 each; missing ratio .4% of the short book; short-dollar
  reconciliation gap 2.4e-15 (rows aligned).
- **G-3b** (low_risk, before / after price-risk-v1, IC h 21): bac -.0266 / -.0182 (retained .68; t -1.15 / -1.09),
  smax .0139 / .0099 (.71; t 1.32 / 1.25), smax5 .0126 / .0151 (1.20; t 1.08 / 1.50): the theme keeps most of its
  IC after its own neutraliser; bac's oriented IC is negative on this window (a diagnostic, PM5-18).
- **G-3c** (combined signal delayed k, net Sharpe vs B0c 1.133, Memmel SE): k 1 1.120 (-.012, SE .021); k 2 1.101
  (-.032, SE .039); k 3 1.093 (-.039, SE .057).
- **G-3d** (sleeve PnL clusters vs 10 theme labels): adjusted Rand .184; effective bets: themes 4.87 of 10, members
  5.85 of 38; mean rho within theme .379, between .085.
- v9 note (an idea from a result, not a spec): iv_rv_spread alone carries 37% of the book's trades and is negative at
  theta on 2020-2023.

**M2 alarm (book monitor; flags only, gates nothing).** What it measures (`book_monitor.py` M2 "alpha"): per admitted
member, (a) IC63 / IC252 = rolling means of the u pass's daily h=5 rank IC times the admission sign, warn outside the
reference's p5-p95; (b) a lower-side IC CUSUM on weekly (every 5th decision) observations standardized by the
reference mean / SD, k .5, warn at 2.5, alarm at h 5 (in-control ARL 938 weeks, 18.6 y); (c) turnover T63 of the card
sleeve vs the reference p5-p95 (warn) and (d) a two-sided turnover CUSUM on 5-decision block means (same k, h). In
baseline mode (`--baseline`, `in_sample` true) the reference is the cell's own series, so the CUSUMs run in sample over
the whole window: an alarm marks a drift of a member's sleeve turnover inside 2020-2023, not a break after a
reference. The M2 status is the worst member flag. Numbers (members with a flag; CUSUM value vs h 5):
- B0a (`mega-monitor-v8-b0a/monitor.json` `541de8be`): status alarm; member counts alarm 4, warn 8, ok 26. Alarms,
  all on the **turnover CUSUM**: iv_rv_spread 6.57, ind_mom_12_1 6.42, high_52w 6.20, sv_flow 5.31. No IC CUSUM alarm
  (9 IC-CUSUM warns, max dtc 4.63).
- B0b (`mega-monitor-v8-b0b/monitor.json` `c034ae6e`): status alarm; alarm 4, warn 10, ok 24. Turnover-CUSUM alarms:
  sv_flow 7.17, ind_mom_12_1 6.76, iv_rv_spread 6.54, high_52w 6.49. No IC CUSUM alarm (10 warns, max dtc 4.67).
- B0c: the same file as B0b (the template keeps the parent's monitor; every monitor input is B0b's): identical flags.
  M1 and M3 n/a (no holdings-days / bias inputs in the cycle's monitor argv), M4 ok on all three.

### Hidden-data record (batch 1b)

- Inputs opened by the tools: role lo3 and the new lo3-dlret role (to 2023-12-29; the delisting stage read by the
  role builder through its sealed reader, as R8), fields v9 lo3, B0b's u / fit / card / w outputs, the lo3 cache, the
  37 ledgered v7 NAV dirs (2020-2022, summ grid), the ledger, B0c's NAV, holdings, lagged NAVs, risk output. Atx-db
  stage manifests hashed, not parsed by me.
- Logs scanned for dates 2024-2029 (every stdout / stderr of the bounded runs above and the two cycle consoles): 0 data
  dates (the only hits are the bootstrap seeds 20260927 / 20260929). Last NAV session 2023-12-29; diagnostics JSON
  session stamps all before the seal. The Release A/B below reads the 3-year lo1 role (2020-2022) only; its logs: 0
  hits. **Nothing dated 2024-01-01 or later was opened. No disclosure.**

### Release A/B (task E-2): NOT identical; not adopted, the cells stay on Debug v8-12

Rule (plan E-2 step 2): adopt `build: "equity-rel"` as the spec default only if the daily CSVs, orientations and
`train_combined` are byte-identical and the CPU stages at least 25% lower; step 3: the e2e fixture under both builds,
SHAs agree. No ruling makes adoption automatic, and PM5-21 freezes the executables the cells use until V8-F, so
adoption would need a PM decision in any case. Host: disk 36 G before the build, RAM about 6 G free; not quiet (other
agent sessions active), so the timings are one sample each.

**Build** `powershell -File scripts/research-build.ps1 -Tag v8-13 -Targets "atx-equity-strategy-ic,
atx-equity-strategy-targets,atx-equity-strategy-risk" -Preset equity-rel` (dry run first: admitted, free 6,493 MiB,
commit 8,390 MiB): receipt `build-equity/mega-v8-13-receipt.json` (`cb1112e5...971b`): ExitCode 0, source `2ad09c13`,
DirtyEntries 0, Jobs 4, 265.95 s, 136 TUs, 7 links, BuildDir `build-equity-rel`. Executables: IC
`b01bf13ea3abfb72950fa9fd6a29b9900126f3d54bd3ca129fbe823f4ac8784b`, targets
`10dbee593681ef8623621e2d2eb8156460e982cfdac2630bd38e24f2f4ebd6c2`, risk
`db4b634b3a32d2ab6975a6c455e33cb48882d37876b2a2a15fbbc343419c481c`. The Debug executables in `build-equity/bin` are
untouched after the build (IC `ab7e2cbd`, targets `5497c89d`, risk `8967952c`). Note: `build-equity-rel` was configured
(before DEPS ISOLATION) with the machine-wide `FETCHCONTENT_BASE_DIR=C:/atx-cache/deps`; the build recompiled
`C:/atx-cache/deps/spdlog-build` in Release. pool-2's Debug tree uses its own `deps/equity-dev` and is unaffected; a
Debug tree elsewhere that shares `C:/atx-cache/deps` would rebuild spdlog on its next build.

**Comparison run** (the v7.1 cell, 3-year role lo1 2020-2022, argv of `mega-v71-train-u-run1`,
`mega-v71w-train-ew-run1` and the v7.1 NAV receipt; both builds on the clean tree `2789ac4d`, one after the other,
same argv except the exe dir and the outputs). u pass cold on a fresh cache per build (`e2-{dbg,rel}-cache`) with
`--max-memory-mib 2560` (the integration-3 B-2 precedent for a cold v7.1 u pass), runner 300 s / 2,560 MiB; w pass
warm on the same cache with v7.1's weights `7b0a59c9`; NAV with v7.1's flags plus `--stage-timers` (D-1; summary
only), 180 s / 1,536 MiB. PATH: Debug the debug + release vcpkg bins; Release the release bin only (research_cycle
BUILDS); `build-equity-rel/bin` carries its release DLLs.

| run | build | receipt.json SHA-256 | s | peak MiB | CPU stages s | wall s (tool) |
|---|---|---|---|---|---|---|
| u cold | Debug v8-12 | `00b943995cbc055d4ccf382e703a6f787c9864c058cd918915052809d448231f` | 104.1 | 1,213 | vm 54.02 + ic 15.14 + composition 12.89 = **82.05** | 103.6 |
| u cold | Release v8-13 | `e17cd2e895e4505119eaf1ccf7812ee69ad43a62b8abb79f128f4377506774fa` | 45.7 | 1,209 | 24.10 + 7.23 + 1.93 = **33.26 (-59%)** | 45.5 |
| w warm | Debug | `0dcb0b79145fe48dffc1732f7efbf66ab4236a150e50d2c1b11b1016d77bfba7` | 21.0 | 507 | ic .58 + composition 10.80 = **11.38** | 20.7 |
| w warm | Release | `c5de007ba732b1201d5419df5a2a71e74389c918347e6198edd3c0d041a2d373` | 9.5 | 501 | .19 + 1.91 = **2.10 (-82%)** | 9.3 |
| NAV | Debug | `c36d3f8bebd7306cc9a06449abe28ff0418383d68dec220e94238d550a3a9030` | 16.0 | 359 | exposures 6.18 + construction 1.26 + books 6.03 = **13.47** | 15.6 |
| NAV | Release | `4bb1dc5a5a9fe28f4e267616e972ede94c2a5ad276a89c45d40ec373bd663d60` | 10.2 | 350 | 3.61 + .64 + 4.99 = **9.24 (-31%)** | 10.0 |

**Identity.**
- u: `orientations.json`, `recipe.json`, `train_daily_ic.csv`, `train_planned_targets.csv`, `train_combined.json` and
  every `train_combined*` payload **byte-identical** Debug vs Release; payloads and daily IC also equal to the accepted
  `mega-v71-train-u-1`. `train_candidates.jsonl` differs only in `wall_seconds` / `stage_seconds.*`; `summary.json`
  only in the cache paths, timings, `hash_seconds`, `ic_cache_bytes` (195,934,024 vs 195,933,920) and
  `ic_scratch_bytes` (1,122,488 vs 1,122,216): provenance, not results.
- w: all ten files byte-identical across builds and equal to the accepted `mega-v71w-train-ew-1` (`train_combined.json`
  `bf1af127`, `train_combined.f64` `1cf245b1`).
- **NAV: NOT identical.** Debug reproduces the accepted v7.1 NAV cell byte for byte (all five daily, five events
  CSVs, `recipe.json`). Release: the linear-6bps daily CSV, all five events CSVs and `recipe.json` equal; **the four
  modeled-1bn daily CSVs differ**. First differing file (sorted): `daily_modeled-1bn-stale5-v1+engine-tiers-v1.csv`,
  row 68, `impact_cost_dollars` 46664.658575514375 vs 46664.65857551439; 6 rows differ there (impact, unrationed and
  trade cost dollars, `trade_cost_return`), max relative difference 3.1e-16 (one ULP). The primary S2 daily CSV
  (Debug `fbec452e...` = the accepted v7.1 file, Release `063f38d8...`): 5 of 756 rows, columns `impact_cost_dollars` (4) and
  `unrationed_cost_dollars` (1), max relative 3.6e-16, first at row 203; its `net_return` column is equal.
- e2e canary (`scripts/tests/test_cycle_e2e.py`, `ATX_EQUITY_BIN`): Debug `build-equity/bin` **5 passed**; Release
  `build-equity-rel/bin` **1 failed, 4 passed**: orientations and admission digests equal the goldens, the primary
  daily CSV SHA `eb385a15...` differs from the golden `ca559404...` (the same cost-column ULPs).

**Result: identity fails (NAV daily CSVs, last-ULP cost arithmetic of the modeled impact law), though the CPU stages
fall 59% / 82% / 31%. By E-2 step 2 Release is not adopted; specs keep the Debug default and the cells keep the v8-12
executables (`build-equity/bin`, unchanged).** `build-equity-rel/bin` now holds v8-13; nothing points at it. A v9
note: the difference sits in the S2 impact-cost arithmetic (a sqrt / pow path compiled differently under /O2); a
Release build that matches would need that path pinned (for example strict FP flags on the cost TU), a PM matter.

Disk after: **32,757,556 KiB free (31.2 GiB)** (`df -k`). New this batch: `build-equity-rel` 1.5 G (was 1.3 G),
`e2-dbg-cache` and `e2-rel-cache` 2.4 G each (A/B scratch caches, referenced by no spec: removable by `cache gc` or
the PM), lo3-dlret role 207 M, holdings 163 M, risk 481 M, lagged combined 3 x 79 M, the NAV dirs.

### Open items (batch 1b)

- Warm-start gross at score_begin: .9257, 6.4% below the 4-year post-ramp mean (5.7% below all rows, 3.4% below rows
  63-125); the 5% expectation is the D-0 synthetic test's. Not a mechanics limit; PM to say whether it needs a ruling.
- nav_summ treats a warm start's CSV row 0 as the deployment row (tau over 1,004 sessions, `warning_tau`), the NAV
  summary does not; frozen file (PM5-21), gates nothing.
- G-1a without f_theta and K6 marginal IC (B0c's cards have no C-2 columns; no marginal phase / K6 for B0c). The
  plan's single `diagnostics-v8.json` is eight split files under E-18 (committed copies in `diagnostics-v8/`); the
  scorecard section (renderer) is not written.
- B0c reuses B0b's monitor (template keeps it): no separate B0c monitor run exists.
- The DSR variance stays degenerate (3 cells) until the PM5-22 re-runs.
- Release: not adopted (identity fails); spdlog-build in the shared `C:/atx-cache/deps` rebuilt in Release (see above).
- Next parent: `scripts/specs/v8/base-b0c.json`, digest `059d9ba6d7cb0f6346c5b065c64fd5ea55940db7196c2200e950ce137c7b483d`.

## cells batch 2a: housekeeping (PM5-26); R-1 STOPPED at mechanics (gross); owner stop (2026-10-01)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `8c5920c7` (clean; code head `1cc4c6c9`;
research executables v8-12 Debug: IC `ab7e2cbd...1615452d`, NAV / targets `5497c89d...35b9bca6`, risk
`8967952c...a7eed258`; Release v8-13 not used). Procedure `task-CELLS-brief.md`; read: integrator-rules, v8-prereg rules
1-11 and rulings, A2 root sequence (incl. R-2 add-alpha), plan section 9 R-1 / R-2 and 12.1, rulings W0-c, A-3 (FIX-AB
finding), E-18, E-24, E-28, E-29, E-34, E-36, E-37, R2-a..h, PM3-5a, PM5-11, PM5-18, PM5-21, PM5-23, PM5-25, PM5-26
and the last progress section, batch 1a / 1b log sections, task-R-1-report, task-R-2-brief, library-v8-draft.md (the
R-2 draft lane's output; there is no task-R-2-report.md in the sprint directory). End: `26c85eba` (R-1 lock) + this log.

Preconditions: freeze list (PM5-21) re-hashed before the first run, all equal to batch 1a / 1b
(`run_bounded_research.py` 81b5de1e, `research_cycle.py` f6b4043d, `research_spec.py` 9dbb7879, `research_ledger.py`
c90edb6e, `research_tree.py` c313a510, `fit_composition_weights.py` 8860483c, `alpha_report_card.py` ade2d777,
`book_monitor.py` 628b2e6e, `nav_summ.py` 719ac51f, `backtest_integrity.py` 2e195823, `compare_window_overlap.py`
b4d7f2e8, `research_window.json` 62cf2cfa; the three exes as above). Ledger `build-equity/trials.jsonl` 41 lines (40
construction + 1 protocol), file `ed3f4139...60a1`, `research_ledger.ledger_n` 40. RAM about 6.3 GiB free. No
executable, cycle or tool script changed in this batch; the only commits are the R-1 lock (a spec of a cell not yet run)
and this log.

### Housekeeping (Ruling PM5-26): the two Release A/B scratch caches removed

- Paths (batch 1b log, "Release A/B"): `build-equity/e2-dbg-cache`, `build-equity/e2-rel-cache` (2.4 G each; each holds
  one `3e79978a...` candidate-cache child).
- Nothing references them: `research_cycle.py cache gc --keep-referenced-by scripts/specs/*.json
  scripts/specs/v8/*.json` (dry run, nothing applied) keeps 12 stores and lists 14 unreferenced ones; neither `e2-*`
  dir appears in either list (the tool's candidates are only names containing `candidate-cache` / `fit-work`, so it
  cannot reference them through any spec); `git grep` finds them only in this log; under `build-equity/` only the A/B
  runs' own receipts (`e2-{dbg,rel}-{u,w}-run1/receipt.json`) name them.
- Removed those two directories and nothing else (`rm -rf`); the A/B run, receipt and NAV dirs stay; nothing else of
  the gc list was touched (3-year caches stay until the PM5-22 re-runs).
- Disk (`df -k`): **before 32,591,044 KiB (31.08 GiB), after 37,473,348 KiB (35.74 GiB)**; after R-1 37,264,924 KiB
  (35.54 GiB). The 30 GB precondition holds; no data build was started in this batch.

### Cell R-1 (r1-comp-v8 on base-b0c; composition ew-theme-std-v1): STOPPED at mechanics, gross FAIL

**Spec** `scripts/specs/v8/r1-comp-v8.json` (template). Edit: `"parent": null` -> `"base-b0c.json"`, nothing else.
`lock` (dry, exit 0), `lock --write` (exit 0) wrote only the derived `locked` block: reference_cell = B0c's NAV
`summary.json` `119d4cd0cb4745204cd6a762ce230ae9603901814d5f97d42484fc20afa71615`, reference_admission = B0b's
`admission.json` `29357ece3fee19cc18a14d4c957bf8934ecdca3c6bd9f006dffc10eac142a18c` (B0c is NAV-only); a dry lock after
it exits 0. File SHA `964f6070d656e4fd0c3adb255f4eaa3a3827cc852a49cff25aca4d21165eb8c2`; **spec digest (template chain)
`1bf13f4004e6c843e7714ba243728076494bda37e2282352ccbee259f38c936d`**. Commit `26c85eba`.

**Pins** (plan, every one `[locked, verified]`): library v7.1 `787c802e`, recipe `7f8a2643`, role lo3 `e1c67101`,
identity_bridge `09aac28f`, fund_events `304d2945`, sic_events `9f9b2f85`, label_role lo3-dlret `95e16cfe`,
reference_cell `119d4cd0`, reference_admission `29357ece`; fields v9 lo3 as built `9f156363` (63 rows = the spec
list). Plan: fields, u done (B0b's `mega-v8-b0b-train-u-1`); fit, card, w, nav, monitor pending; caps fit 180 s /
1,536 MiB, card 300 / 2,560, **w 300 s / 3,072 MiB with `--max-memory-mib 3072` (E-28)**, nav 180 / 1,536; summ
`--dsr-n 41`. Command: `research_cycle.py run scripts/specs/v8/r1-comp-v8.json --stop-after nav` (source `26c85eba`).
No free-memory refusal (no retry needed).

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| fields, u | - | - | - | - | - | done (B0b's; resumed) |
| fit | 180 / 1,536 | 0.8 | 57 | 0 | `536fa3585a369c662f13a7f29789a74aa280313b1c1b6b3b4d258251d3468f6c` | `composition_weights.json` `eb98a50a...68d7`, `admission.json` `29357ece` (= B0b's); fit store: computed 0, reused 48 |
| card | 300 / 2,560 | 12.6 | 1,153 | 0 | `e5e8b01365bbc6fbfbeaa7225527d7508155ec05de00575b4ca860d4fe07ea5a` | `daily_sleeve.csv` `80ba33d5` (= B0b's: same admission) |
| gate | internal | - | - | PASS | - | `b0b-readout`: 0 status changes against B0b's admission (38 admitted of 48; mom_12_1 reject_redundant) |
| w | 300 / 3,072 (E-28) | 30.7 | 1,307 | 0 | `b21608c44529231ef82c88d0d1f9b7553aa9908473b21ca9984a84706b02f864` | `train_combined.json` `140bcf4a...` (`composition_standardise` ew-theme-std-v1, weights `eb98a50a`), `train_combined.f64` `0050fe91...`; IC admission `admitted_bytes` 2,655,038,364 (2,532 MiB) |
| nav | 180 / 1,536 | 42.3 | 586 | 0 | `0c43b021ad1b9fef82172769dbbf6cc774d3861887dd74887108e185393f85ef` | `summary.json` `25def49c...`, `recipe.json` `ae79d320...`, S2 daily `ecdced6f...`, `capacity_curve.csv` `41f537b7...` (not read) |
| monitor, summ, one-sided p | - | - | - | not run | - | stopped at mechanics (below) |

Every bounded receipt: completed, `clean in the code pathspec`, source `26c85eba`, exe v8-12 (IC `ab7e2cbd`, NAV
`5497c89d`; Python `624bbc05` for fit / card).

**Weights file (E-45 later depends on it):** schema `atx.dsl-composition-weights/v2`, **`theme_standardise` block present
with `rule` ew-theme-std-v1, `rerank` true**, 38 member themes, no `theme_redistribution` block; `provenance.rule`
ew-theme-std-v1; `provenance.std`: tier source `registry` (`atx-impl/strategies/alphas/registry.json` SHA-256
`e985aefcec3ae73b4b6673043bfd3379a2409cdd9a6dab596612ad73a1923d46`), the four declared re-grades (res_mom_12_1 B+ -> B-
applied, ear B+ -> C+ applied, sue C+ declared-unchanged, ins_opp B- -> C+ applied), T 10, cap .05 = 1/(2T), two cap
passes, capped members iv_rv_spread, ind_adj_rev_5, ins_opp, inst_best_ideas. `summary.weights.standardise`
ew-theme-std-v1 (rerank on).

**Mechanics (S2 = `modeled-1bn-stale5-v1+swap-fin-v1`, primary; nav_summ's own `construction_stats` on the S2 daily CSV
and the NAV summary's flags; read before any return; a scratch reader that prints no return statistic): FAIL.**
- **All-rows gross 1.0672 (mean_gross_leverage_all_rows 1.067164): outside [.90, 1.05]** (the v8 pitch config
  `v8.mechanics` and the R6' gate: `between` .90 / 1.05). Post-ramp gross 1.0735 (943 rows); max gross 1.2128; gross at
  score_begin (row 0, 2020-01-02) 1.0153, rows 1 / 2 1.0154 / 1.0206 (warm start K 60, first decision role row 339,
  score_begin row 399; no scored deployment).
- All-rows net +.0047 (|.| <= .02): pass. Tau mean .02456 <= .20, p95 .02976 <= .30 (1,004 sessions; summary
  meets_daily_turnover_mean / _p95 true): pass. 1,006 CSV rows 2020-01-02..2023-12-29, 1,005 return rows. Accounting:
  max cash-book error 3.0e-14, return identity 3.9e-16 (tol 1e-9). Max |net| .033. Label role admitted (867 label-only
  cells, 686 scored, as B0c). Beta: none to check (aim-partial-v5; as B0a-B0c).
- Parent B0c on the same reader: all-rows gross .9820, net +.0036, tau mean .03407, p95 .03902.

**Criterion ingredients (PM5-11; executed turnover per unit gross, S2; mechanics-side numbers, no return):** R-1
`tau_gmv_mean / mean_gross_leverage_all_rows` = .024558 / 1.067164 = **.023012**; B0c .034069 / .981964 = **.034695**.
R-1 <= B0c: the criterion alone would pass. Cost bps per traded dollar 12.74 (B0c 13.13), mean held names 1,918.9
(1,918.3).

**Stop.** The brief ("If mechanics fail, stop the cell there and report before reading returns where the tooling allows
it") and the dispatch ("Stops: any mechanics failure") apply: the run was `--stop-after nav`, so the cell stops here.
Not run: monitor, summ (statistics of record, DSR block, ledger line), the PM5-23 one-sided-p bundle. **No return
statistic of R-1 was read** (no net Sharpe, no paired dSR, no year table, no capacity row; `capacity_curve.csv` and the
summary's return fields unopened by me). By prereg rule 5 the conjunct "mechanics" is false, so the rule gives **NOT
ACCEPTED** whatever dSR is; it is not recorded by the tooling (no `cycle_verdict.json`, no ledger line) because summ did
not run. Prereg rule 7 keeps the PM's options open precisely because no return was seen (a re-run decided without
seeing returns replaces the cell with no new trial). The cause of the higher gross is not established by me (the cell
changes only the composition; L stays 1.247 as registered); no defect is claimed and no ruling is invented.

**N after: 40, not 41** (ledger 41 lines = 40 construction + 1 protocol, file `ed3f4139...60a1` unchanged; `ledger_n` 40).
The brief expects 41 after R-1: the mismatch is this stop (the ledger line is written by summ, which did not run).
**Appendix A:** `TRAIN construction cells 40 (R-1 run to NAV, not ledgered; PM ruling pending); admission trials this
sprint 0 (plus 0 re-screens); window research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation
reads before v8: 2 (2023-2024); history reads 0; 2025+ never read.`

Hidden data: inputs opened by the tools were role lo3, the lo3-dlret label role, fields v9 lo3, B0b's u / admission /
fit store, the lo3 cache, R-1's own outputs. Every stdout / stderr of the four bounded runs and the cycle console
scanned for dates 2024-2029: 0 hits; last NAV session 2023-12-29. **Nothing dated 2024-01-01 or later was opened.**

### Owner stop (coordinator message during R-1's NAV phase)

Received after R-1's `run` had started. R-1 was carried to the point the brief allows after a mechanics failure (above).
**Not started:** fields v10 (no build, no reuse count, no manifest), R-2 (no registry E1 edit, no `add-alpha`, no
`lib-v80.json`, no `run --screen`: **admission trials consumed 0**, re-screens 0). Nothing else ran after the R-1 NAV.

### Open items (batch 2a)

- **R-1 needs a PM ruling:** mechanics FAIL on all-rows gross (1.0672 > 1.05) at the registered L 1.247; criterion
  ingredients pass (.0230 vs .0347); returns unread; not ledgered. Ledgering it (N 41) needs summ, which prints returns.
- Next parent under rule 5 as it stands: **B0c**, `scripts/specs/v8/base-b0c.json`, spec digest
  `059d9ba6d7cb0f6346c5b065c64fd5ea55940db7196c2200e950ce137c7b483d` (R-1 cannot be accepted with a mechanics FAIL).
- `r1-comp-v8.json` is committed with `"parent": "base-b0c.json"` and its lock (`26c85eba`); R-1's output dirs stay
  on disk (`mega-weights-v8-r1-std`, `mega-cards-v8-r1-std`, `mega-v8-r1w-train-std-1`, the R-1 NAV dir and receipts).
- Fields v10 and R-2 carried to the next batch. R-1's weights depend on `registry.json` (`e985aefc`) as the tier
  source; R-2's E1 hand edit of the registry field rows will move that SHA (relevant to an R-3 refit, E-27).
- `scripts/tests/test_research_spec.py` not run (known fixture lane).

## interim report (owner stop) (2026-10-01)

Integrator REPORT-INTERIM in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `c4a939e0` (clean). Ruling
PM5-27. Read: integrator-rules; progress PM5-27, PM5-22, PM5-23, PM5-25, PM4-5, PM5-11 and the sections batch 1a, 1b,
owner stop; this log's cells batch 1a, 1b, 2a; task-REPORT-report.md; status-2 section 9; the registered config
(`docs/plans/mega-alpha-v8-pitch.config.json` `b828717a`) and template (`docs/plans/mega-alpha-scorecard-v8.template.md`
`a915b849`). No cell, no build, no data run other than the report tools' own reads. Not edited: `mega_report/*.py`,
`nav_summ.py`, any cycle or tool script, the registered config and template, the ledger, any spec.

**R-1 blindness.** No tool and no read of mine touched R-1's NAV dir (`mega-nav-v8-r1-std-...`), weights, cards, w pass,
returns, summary, daily or capacity files. The interim config keeps R-1's registered placeholders
(`build-equity/mega-nav-v8-r1`, `build-equity/mega-nav-v8-paired-r1.json`; neither exists, `test -e` only) and records
R-1 as text only: "R-1: constructed; mechanics fail (all-rows gross 1.0672, limit [.90, 1.05]); no return read; not
ledgered; ruling open". The render's file manifest and the filler's read list name no R-1 path.

### Interim config

`docs/plans/mega-alpha-v8-pitch.interim.config.json`, sha256
`79e40f9a1e83e85e0ff9216dc8d423f8189510f6b622c3c70c6f3a981372a157`, written from the registered config by
`interim_report_make_config.py` (sprint dir, `55794def`; it asserts every value it replaces), commit `1f8a0fb4`. Edits:
- title / kicker say INTERIM; B0c's dir = `build-equity/mega-nav-v8-b0c-dlret-ws60-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247`
  in `v8.cells` and top-level `cells`, and its cell name in `equity.extra`, `rolling.cells`, `analysis.compare_cell`;
- `v8.summ` = `build-equity/mega-nav-v8-summ-interim.json`; `v8.cells[B0b].paired` = `build-equity/v8-cells-b0b-bundle.json`
  (the PM5-23 bundle of batch 1a: the documented `--bundle B0a B0b` command; its base / final names match the config dirs);
- verdicts: B0a "re-base ledgered", B0b "accepted", B0c "baseline by declaration", R-1 the sentence above, every other
  cell "pending run" (unchanged); parents, `v8.final` (R-7), top-level `final` (v8-r7), `v8.bundle`, `v8.diagnostics`,
  `v8.member_horizon` unchanged (their inputs do not exist: unavailable by design);
- `v8.re_screens` 0 (R-2 not started: no `_f49` re-screen ran, batch 2a);
- `capacity_curve` = B0c's book (its NAV dir, `v7_extras.json`, `summary.json`; E-29 priced it inline; no S2-KO / S2-FIM
  book exists for B0c, so `beside` is empty); its layout entry moved from section 5 (V8-F book) to section 2 under an h3
  and a callout with the PM4-5 disclosure; section 5 is titled "pending at the owner stop" with a callout, its other 18
  book blocks unchanged;
- `v8_year_table` restricted to B0a, B0b, B0c (the block's own `cells` option); the ladder still lists every cell;
- narrative: the V8-F result paragraph is replaced by an interim paragraph whose B0c figures are placeholders on the v8
  nav_summ JSON (`{a:v8_summ.rows.2...}`, row 2 = B0c, checked: the rendered text names B0c's dir); a callout
  "Interim status" (PM5-27 statements) opens section 0.

### Commands (bounded, on the clean tree at `1f8a0fb4`; `$PY` = `C:/Program Files/Python312/python.exe`)

| step | command | receipt dir | caps s / MiB | s | peak MiB | exit | receipt.json SHA-256 | output SHA-256 |
|---|---|---|---|---|---|---|---|---|
| v8 summ | `nav_summ.py --protocol v8 --dsr-ledger build-equity/trials.jsonl --effective-n dirs --psr --json build-equity/mega-nav-v8-summ-interim.json <B0a dir> <B0b dir> <B0c dir>` (no `--ledger`) | `build-equity/mega-nav-v8-summ-interim-run` | 180 / 1,536 | 0.8 | 48 | 0 | `1fbcb1f7254700c6c9ade35f9095c1f57ebf2c2a4b60ff1693442a10cec9bc7b` | `mega-nav-v8-summ-interim.json` `d23cb666eb1b2278ff5d1d3c07fbcac79917a06d38f3056525e183dc8708258c` |
| Appendix A | `nav_summ.py --protocol v8 --ledger-n build-equity/trials.jsonl` | `build-equity/mega-nav-v8-appx-interim-run` | 180 / 1,536 | 0.3 | 5 | 0 | `5dc18816070eec7f8ca7fdecfea8a8f01d97bea7bc008ef801e7c5ff6507ffa1` | `stdout.log` `72de67766c502232bef22575b3462f45ce374c34a3847c346ed279a8992f6df7` |
| pitch render | `atx-impl/tools/mega_report --config docs/plans/mega-alpha-v8-pitch.interim.config.json --out <scratch>/2026-10-01-mega-alpha-v8-interim-pitch.html --stamp "2026-10-01 interim render at the owner stop (Ruling PM5-27)"` | `build-equity/v8-interim-pitch-render-run` | 300 / 2,560 | 0.5 | 55 | 0 | `4f4aac4399906b0668e46bf6b4f756e3141185fa7b3cea9fc2367670660c795b` | html below |
| scorecard fill | `<scratch>/fill_scorecard_interim.py <scratch>/2026-10-01-mega-alpha-scorecard-v8-interim.md build-equity/mega-nav-v8-appx-interim-run/stdout.log 1f8a0fb4... 37` (committed copy `interim_report_fill_scorecard.py`, `2f4e54b5`, byte-identical) | `build-equity/v8-interim-scorecard-fill-run` | 120 / 1,024 | 0.3 | 5 | 0 | `2448b2c625d4ada6d1a52f468a1f96a7c530d401cec95173e786232651a88870` | md below |

Every receipt: completed, `clean in the code pathspec`, source `1f8a0fb4`, interpreter `624bbc05`; `git status
--porcelain` empty before each run (the outputs were written to the session scratchpad and copied into `docs/plans`
after the last run). The scorecard is the registered template filled by the script: every number is read from a file
(the summ JSON, the paired bundles, the NAV summary.json files, B0c's `v7_extras.json`, the diagnostics split files, the
Appendix A stdout); the `{{PM}}` statements are this interim's verdicts and "pending" texts; the template's comment block
is deleted; the script refuses an unresolved placeholder. Ledger after all runs: 41 lines, `ed3f4139...60a1`, unchanged.

Appendix A (stdout): `TRAIN construction cells 40; admission trials this sprint 0; window research-window-v2 (2020-2023);
hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024); history reads 0; 2025+ never read.`
(ledger: 40 trials in 41 lines; 1 protocol line, 0 defect, 0 window re-run lines.)

### Inputs read (SHA-256)

| input | SHA-256 |
|---|---|
| `build-equity/mega-nav-v8-summ-interim.json` | `d23cb666eb1b2278ff5d1d3c07fbcac79917a06d38f3056525e183dc8708258c` |
| `build-equity/v8-cells-b0b-bundle.json` (PAIRED[B0b]) | `75c30860398c4d6d10b94dee899b84709f630470a65470f70f146e2db5383191` |
| `build-equity/v8-cells-b0c-bundle.json` (B0c vs B0b, information only; scorecard only) | `fe849547a537baf6c1a0729e7564409279ee9a51943a17fef891360447b6e70f` |
| `build-equity/trials.jsonl` | `ed3f4139273a085ccdb6c834ac5307bf311231a19136b7807b91d8a0a6b260a1` |
| B0a / B0b / B0c NAV `summary.json` | `eeac8366...05a8f` / `82a596c1...4ad8` / `119d4cd0cb4745204cd6a762ce230ae9603901814d5f97d42484fc20afa71615` |
| B0c S2 daily CSV (pitch: capacity block, content seal) | `78360530c943c3b8f75215147cdb8c5e3a88d11448516db74eaafc15230689d4` |
| B0c `v7_extras.json` (capacity) | `eb388cf0e1785737f4d1828a148223739733e9462da3e9eebe75f47c8dfa4153` |
| diagnostics split files (scorecard section 7 only) | g1a-g3d `de1990b8`, g1b-g1c `a985adf8`, g2a `e2a9874b`, g2b `86bf47af`, g2c `cbed04d7`, g3a `b44ea5d0`, g3b `19e5e5da`, g3c `9f00b5f3` (= batch 1b) |
| `v8-prereg.md` / literature review | `3f9b7bc2...db32c` / `14bac552...040f` |
| pitch book pre-checks (V8-F signal blocks; files that exist, TRAIN) | lo1 role `manifest.json` `2ff9d771`, `member.u8` `ffa09210`, fields v9 lo1 `888e6616` |

### Outputs

- `docs/plans/2026-10-01-mega-alpha-v8-interim-pitch.html`: 139,806 bytes, sha256
  `932061790d190c5ef02f82f0cc80f3866e5b2519e8895f8689c308e965dae7e4`; renderer: `n/a markers 210; unavailable blocks 37`;
  config sha in the header `79e40f9a`, inputs root and generator `@ 1f8a0fb4`; file manifest 11 read, 35 not read (all
  missing V8-F / pending-cell paths).
- `docs/plans/2026-10-01-mega-alpha-scorecard-v8-interim.md`: 33,363 bytes, sha256
  `3a13fe3f2492c0906a90c0eb019442579627d5018a1dfc579e078e407573eabf`.

### Unavailable blocks (renderer count 37; the tool rendered, no fallback)

| # | block | input named | reason |
|---|---|---|---|
| 1 | v8_bundle | `mega-nav-v8-bundle-b0c-v8f.json` | no V8-F: cumulative test and freeze gate not evaluated |
| 2 | v8_ladder (refused) | `v8.final` | 'R-7' is not the last accepted cell ('B0b'): no construction cell accepted, V8-F pending |
| 3 | v8_ladder | `mega-nav-v8-paired-r1.json` | R-1 stopped at mechanics; its paired test was not run (no return read) |
| 4-13 | v8_ladder | `mega-nav-v8-paired-r{2..8,10,11,12}.json` | cells not run |
| 14-16 | v8_ladder | `mega-nav-v8-r{4,5,6}/summary.json` | R-5 / R-6 criteria read NAV summaries of cells not run |
| 17 | v8_diagnostics | `mega-diagnostics-v8-b0c/diagnostics-v8.json` | not produced: E-18 split G-1..G-3 into eight files; no tool verb combines them; not hand-merged (the scorecard reads each id from its split file) |
| 18-19 | v8_member_horizon | `mega-cards-v8-r7/index.json`, `mega-weights-v8-r7-ew/admission.json` | final library (V8-F) pending |
| 20-37 | fig_equity, t_drawdowns, fig_returns, fig_rolling, t_retstats, t_stress, t_cost_model, t_financing, costdec, t_attrib, fig_cost_drag, fig_turnover, fig_exposure, fig_fills, fig_corr x3, t_theme_corr | `mega-nav-v8-r7/...`, `mega-v8-r7-train-u/...`, `fund_industry_ic_v81*` | V8-F book pending (section 5) |

R-9a..c (report-only, no paired file) render as pending rows with notes, no block. Rendered: interim callout, B0a / B0b /
B0c ladder rows (B0b dSR +0.014, SE 0.040, p one-sided 0.3820, rule PASS, ACCEPTED), year matrix and the three year
tables, B0c capacity curve and table, OD-1, trial accounting, contradictions (23 rows).

### Self-check (rendered scorecard and the pitch's text; no R-1 artifact opened)

- B0a / B0b / B0c figures equal the log (batch 1a / 1b) at the printed precision: S2 net Sharpe +1.125 / +1.139 / +1.133
  (log +1.1254 / +1.1389 / +1.1328); all-rows gross .9709 / .9663 / .9820, net +.0038 / +.0035 / +.0036, tau mean / p95
  .0367 / .0439, .0370 / .0439, .0341 / .0390; B0b vs B0a dSR +0.014 (log +.0135, printed +.3f as the template
  registers), SE 0.040, p one-sided .3820; B0c vs B0b dSR +0.006, SE 0.017, p .3014 / .6792; B0c cell-count DSR .984
  (log .9844), legacy .707, SR0 .0148 ann; the three year tables equal the log's row for row; B0c capacity +1.174 /
  +1.133 / +1.085 / +0.978 / +0.851 at .5 / 1 / 2 / 4 / 8x, cost 11.15 / 13.13 / 15.47 / 17.60 / 19.24 bps; G-1..G-3
  headlines equal the batch 1b list (G-3a 1.069, G-1c .455, G-2a style .657 + market .005 / industry .182 / specific
  .156, G-2c 7.1% of held gross above Q, G-3c delays -0.0123 / -0.0322 / -0.0395 against the log's -.012 / -.032 / -.039,
  G-3d .184 / 4.87).
- The full year table and the headline are B0c's (the V8-F rows say pending).
- No R-1 return statistic anywhere: "1.0672" occurs only inside the R-1 sentence; no R-1 path in either output or in
  the render manifest; R-1's ladder criterion detail shows only the parent's (B0c) tau / gross .03469.
- No 2024+ data date: the only 2024+ strings are the seal and OD-1 texts (`2024-01-01`, `[2024-01-01, 2025-01-01)`),
  the Appendix A words, literature citations (publication years, "CZ-own 2005-2024"), the render / ruling dates
  2026-10-01 / 2026-09-29 and two CSS colour codes (`#1B2028`). The four run logs: no 2024+ date (the Appendix A stdout
  has its own words only).
- Unavailable list = what is pending (V8-F, cells not run, R-1's unrun paired test) plus one format case (diagnostics,
  above). The `v8.final` refusal is the expected interim state.
- Tests: `pytest -q -p no:cacheprovider atx-impl/tools/test_mega_report_v8.py atx-impl/tools/test_mega_report_v8_render.py`
  131 passed (19.9 s). The registered config / template, `atx-impl`, `scripts` and the ledger have no diff since `c4a939e0`.

### Notes

- Effective-N and Lo-null DSR in the scorecard come from `--effective-n dirs` over the three listed dirs (.985 / .746 for
  B0c), not the cycle's 40-cell summ grid (.8763 in batch 1b); nav_summ warned that the listing DSR uses `--dsr-n` 10
  over 3 dirs (that column is not used). Neither is meaningful before the PM5-22 re-runs, as both outputs say.
- The ladder's "N after" for R-1 (41) and later cells is the tool's plan count from the configured states; the ledger
  holds N 40 (stated in the interim callout and in the scorecard rows).
- nav_summ's known warning on B0c (CSV tau .034069 vs summary .034070; batch 1b finding 2) appears again; gates nothing.
- Provenance scripts committed in the sprint directory: `interim_report_make_config.py` (`55794def`) and
  `interim_report_fill_scorecard.py` (`2f4e54b5`); neither is a tool script and nothing imports them.

## cells batch 2b (FIX-6 merge, fields v10) (2026-10-02)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `97a98978` (clean; code head `1cc4c6c9`;
build v8-12; PM5-21 freeze: nothing built, no executable / cycle / tool script changed). Read: integrator-rules,
task-CELLS-brief (general rules; R-2 row "fields v10 first"), this log's batch 1b / 2a, progress PM5-20, PM5-24, PM5-26,
PM session 6 (PM6-1..3), PM3-5a, plan Task R-2, task-A2-report root sequence (R-2 step 3), task-F-1 / task-F-3 reports
(v10 argv delta and expected counts), runbook R11. Two read-only agents (directory sizes) were active on the tree; they
changed nothing (`git status --porcelain` empty before and after every step). No R-1 output opened.

### 1. FIX-6 merged (tests only)

`git merge --no-ff 702cf051` -> **`0d553a34`** (ort, no conflict). Brings 048a8205, 0f00013d (round 0), ffc93e6e,
f943d6dc (round 1), e2b9758a, 702cf051 (round 2). Diff of the merge against `97a98978`: **`scripts/tests/test_research_spec.py`**
(475 lines changed) and the lane report `.superpowers/sdd/platform-v8-20260929/task-FIX-6-report.md` (new, sprint
directory); nothing else: no spec, script, tool or executable.

`"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider -rs scripts/tests`, `ATX_EQUITY_BIN` and
`ATX_EQUITY_TARGETS_EXE` absolute (v8-12), vcpkg bin dirs on PATH:

| suite | result | against |
|---|---|---|
| scripts/tests (whole) | **2 failed, 185 passed, 3 skipped** (104.8 s); skips: the three RESEARCH_CYCLE_LIVE_ROOT tests | before the locks 182 / 3; after the base-lo1 lock 20 failed / 162 / 4; FIX-6 lane 186 / 4 (190 tests, as here) |
| `scripts/tests/test_research_spec.py` alone | 2 failed, 45 passed | the 20 failures of the lock are gone |

**FINDING (not fixed; dispatch: report, no non-test edit): one cause, two failures.**
- `test_v8_base_specs_carry_the_ruled_settings` (`scripts/tests/test_research_spec.py:546`) asserts
  `as_authored(base-b0c)["change"]["inputs"]["label_role"]["dir"] == "build-equity/train-2020-2023-lo1-dlret"`. The live
  `scripts/specs/v8/base-b0c.json` carries `train-2020-2023-lo3-dlret` since batch 1b's B0c lock `2ad09c13` (the A2
  root sequence's re-point when B0b wins; the spec's own description names it). FIX-6's base `f7c430f2` predates
  `2ad09c13` (not an ancestor), so the lane's suite saw lo1-dlret and passed.
- `test_the_whole_file_passes_with_a_generated_spec_present` runs the file in a subprocess; its inner result is
  "1 failed, 45 passed, 1 deselected", the failing test being the one above.
- The fix is a test premise (as PM5-20 / PM5-24): `as_authored` would reset the label_role dir like the parent, or the
  assertion would accept either winner's dlret role. A tests-only change for the FIX-6 lane or a PM ruling; no cell
  is affected (no spec, script or executable moves). `git status --porcelain` empty after the suite (no golden moved).

### 2. Fields v10 on lo3: built; reuse counts 63 / 7, NOT the expected 49 / 21: STOPPED

**Preconditions** (re-checked immediately before the run, 2026-10-02T09:56:29Z):
- Tree clean at `0d553a34`. `atx-engine/tools` has no change from R11's source `2b80cf9e` to `0d553a34` (`git diff`
  empty): builder `b44cff42`, `research_fields_sec.py` `27034019`, `research_fields_holdings.py` `edfd1967` = R11's
  bindings; `research_fields_price.py` `fc5e4b5a`, `research_fields_v8.py` `d90bb44d`.
- Freeze list re-hashed, all equal to batches 1a-2a (`run_bounded_research.py` 81b5de1e, `research_cycle.py` f6b4043d,
  `research_spec.py` 9dbb7879, `research_ledger.py` c90edb6e, `research_tree.py` c313a510, `fit_composition_weights.py`
  8860483c, `alpha_report_card.py` ade2d777, `book_monitor.py` 628b2e6e, `nav_summ.py` 719ac51f,
  `backtest_integrity.py` 2e195823, `compare_window_overlap.py` b4d7f2e8, `research_window.json` 62cf2cfa; exes IC
  ab7e2cbd, targets 5497c89d, risk 8967952c). Ledger 41 lines, `ed3f4139...`, N 40.
- Live stage manifests (W0-n), all equal to the R11 table ("Wave 0 part 2a"): v2-pit `09aac28f`, earnings_calendar
  `9a4a976b`, insider `dcd3f1aa`, sec_filings `5190fe99`, thirteenf `8974170f`, ftd `a76d69be`, regsho_threshold
  `fb073c62`, security_master `3afe0660`, short_volume_ext `7007a13c`, fundamentals (SIC) `9f9b2f85`, delisting
  `1b1166b6`, FINRA SI asof `a2561d75`, raw short volume `manifest.csv` `8b076a16`; fundamental-events-v3 `304d2945`.
- Vendor file unchanged (3,617,973,507 B, mtime_ns 1789920127331396300); role lo3 `e1c67101` `source_sha256`
  `0ed96b26...` (the price module checks `--price-source` against it). Disk 60,460,056 KiB free (57.7 GiB); RAM
  4,792 MiB available.

**Command** = R11's recorded argv (receipt `train-2020-2023-lo3-fields-v9-run`, verbatim) with F-3's v10 delta
(task-F-3-report "Argv deltas"; task-F-1-report step 3), nothing else changed:
- `--output build-equity/train-2020-2023-lo3-fields-v10`;
- `--fields <the 63 v9 names in v9 order>,ret_overnight,ret_intraday,ceq_iss_5y,coskew_60m,vol_126,xrd0_ttm,grp_ff12f49`;
- `--reuse build-equity/train-2020-2023-lo3-fields-v9 --reuse-sha256 9f1563638b5e4f7ead7be686803b96a0707ada2c608fcbc6dc084179bd9021ef --reuse-hardlink`;
- `--price-source C:/Users/natha/Downloads/TickerHistory3.parquet`;
- builder `--max-rss-mib 2048 --max-seconds 580` (v9's; F-1 needs >= 1,200).
Runner `scripts/run_bounded_research.py --seconds 600 --max-rss-mib 2560 --min-free-mib 512` (R11's preparation caps),
`--output build-equity/train-2020-2023-lo3-fields-v10-run`, binds R11's four plus `research_fields_price.py`,
`research_fields_v8.py` and the prior's `manifest.json`.

| step | receipt dir | source | outcome / exit | s | peak MiB | argv digest | receipt.json SHA-256 | output manifest SHA-256 |
|---|---|---|---|---|---|---|---|---|
| fields v10 lo3 | `train-2020-2023-lo3-fields-v10-run` | `0d553a34` | completed / 0 | **102.5** | **1,136** | `a7c5745f3f8d72997226d48c821a09aa033dd5e29ac260bbba315bb5279a66e3` | `4fce9e313f109a22d0249c554de9e376ed9b2611777548f779c4402cc9a0bec3` | **`a4a060ae29c10080ca89b4b29335709558c2ae4842bd29db0398b23a43070809`** |

Receipt: `clean in the code pathspec`, dirty outside none, interpreter `624bbc05`, min system free 4,350 MiB. No
`FieldNeedsOpen`: the open exists in the vendor file (OD-6 settled; ret_overnight / ret_intraday built).

**Manifest** (metadata only; no payload value or coverage figure read): status complete, `seal.exclusive_end`
2024-01-01, role `e1c67101`, **70 rows**, 82 source paths, none 2024-named; `code_sha256_lf` `74df97f9...` (= v9's);
581,764 B (under the 1 MiB C++ cap). `reuse`: from v9 lo3 `9f156363`, mode hardlink.

**Counts: reused 63, computed 7** (the seven new names; reason "absent from the prior manifest" for each). **Expected
(dispatch, PM3-5a, F-3): 49 / 21. Mismatch: STOP; nothing adjusted, nothing re-run.**

**Identity of the v9 payloads:** for all 63 v9 fields the v10 entry `sha256` = the v9 entry `sha256` = the v10
`files` pin (63 / 63; so the 49 builder + holdings fields the dispatch names, and the 14 SEC fields too); all 63
re-hash on disk to the pin and are hardlinks of the v9 files (same inode); every one carries `reused_from` (v9 lo3,
host code blob `e8b57af5`). New payload pins (re-hashed equal): ret_overnight `1a41d385`, ret_intraday `f6cf5eb3`,
ceq_iss_5y `88301a7b`, coskew_60m `c3503df3`, vol_126 `ee15d4bf`, xrd0_ttm `e9b5adef`, grp_ff12f49 `14eaa17a`.

**Why 63 / 7 (from code identity and git, no data):** 49 / 21 is F-3's count for a prior P = `v8-i3p4-c-fields2`
built on integration-3 code, before F-B (`0687e82f`) moved the `sec` group fingerprint; PM3-5a carried it to "v10 from
v9". Fields v9 lo3 was built at `2b80cf9e`, and `0687e82f`, E-21 `eca04c18` and F-A `230b39e1` are all ancestors of
`2b80cf9e`; with `atx-engine/tools` unchanged since, the 14 SEC fields' producer fingerprint equals today's and they
are copied. That is F-3's other stated case ("reused 63, computed 7"). F-3 also says the 14 would recompute "with the
same bytes", so the v10 payloads are what a 49 / 21 build would have written; only the count differs. Whether v10
(`a4a060ae`) stands as built is the PM's call.

**Order note:** the manifest lists fields in the builder's registry order, not the `--fields` order: the seven new
names sit at rows 55-61 (after `k8_days_since_any`, before `inst_own_share`); the 63 v9 names keep their relative order.
A spec `fields.list` pinned to this dir must follow the manifest order (the as-built pin check compares the order).

Disk after: 60,103,140 KiB free (57.3 GiB); the v10 dir holds 4,444 MiB of which 445 MiB are new (7 payloads +
manifest; 63 hardlinks).

### Not done (by the dispatch)

R-2 not locked or run (it waits for the R-1 ruling, which fixes its parent); no registry edit, no `add-alpha`, no
`lib-v80.json`, no cell. N 40; ledger 41 lines `ed3f4139` unchanged; admission trials 0, re-screens 0; history reads 0.

### Hidden-data record (batch 2b)

- Inputs opened by the builder: role lo3, the v9 lo3 prior (manifest; payloads hardlinked, re-hashed), the vendor
  TickerHistory3 file (reader-side seal; hashed whole by the price module), FINRA SI, CNMS short volume, the atx-db
  stages and fundamental events v3 through their sealed readers. Atx-db stage manifests hashed by me, not parsed.
- Logs: runner stdout / stderr: 0 lines with a 2024-2029 date; the runner console: one hit, the run's own wall-clock
  start stamp 2026-10-02T09:57:22Z. I read receipt fields and manifest metadata (reuse block, names, digests, seal,
  source paths, code identity) only; no stdout progress line, no field value, coverage, IC, return or Sharpe.
- **Nothing dated 2024-01-01 or later was opened. No R-1 artifact opened.**

### Open items (batch 2b)

- **PM ruling: fields v10 reuse count 63 / 7 against the expected 49 / 21** (cause above; payloads of all 63 v9
  fields bit-identical; manifest `a4a060ae...0809`). The dir and its receipt stay on disk untouched.
- `scripts/tests`: 2 failures from `test_research_spec.py:546` (label_role lo1-dlret premise vs the live lo3-dlret);
  tests-only fix for the FIX-6 lane or a ruling.
- Field order of v10 is the registry order (rows 55-61 new): relevant to any `fields.list` or `add-alpha --fields`.
- R-2 waits for the R-1 ruling (parent).

## cells batch 2c: R-1 at matched gross, R-2, R-3 (2026-10-02)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `0dec40aa` (clean; code head `1cc4c6c9`;
executables v8-12 Debug: IC `ab7e2cbd`, NAV / targets `5497c89d`, risk `8967952c`; nothing built; PM5-21 holds: the
only commits are specs of cells not yet run, registry rows of the R-2 wave, and this log). Read: progress PM session 6
(PM6-4, PM6-5, PM6-6 whole), `r1-gross-investigation.md` (incl. "Follow-up: L per cell"), task-CELLS-brief (R-1, R-2,
R-3 rows), rulings E-27, E-27a, E-27b, E-28, E-36, E-44, E-45, PM5-11, PM5-23, A2 root sequence (R-2). Freeze list
re-hashed in batch 2b (2026-10-02T09:56Z, all equal); no tool, cycle script or executable changed since (`git diff
1cc4c6c9 HEAD` outside specs, registry and the sprint directory: only `docs/plans` interim / status files and the
FIX-6 test file).

**Gross matching (PM6-6), as applied.** G = nav_summ `construction_stats(...)["mean_gross_leverage_all_rows"]` on the
S2 daily CSV (the gated mechanics key), read by a scratch extractor
(`scratchpad/mech.py`) that loads the CSV through `nav_summ.load_daily` (seal-checked) and prints only gross, net,
turnover and count keys plus named summary flags (aim_leverage, meets_daily_turnover_*, accounting errors); it prints no
return, NAV, Sharpe or P&L figure. A calibration run's `stdout.log` and `summary.json` are never opened beyond that.

### Cell R-1 (r1-comp-v8-gm on base-b0c; composition ew-theme-std-v1; L 1.1474): ACCEPTED, N 41

**Calibration (step 1):** r1-comp-v8.json's NAV at L 1.247 (batch 2a; 42.3 s, 586 MiB): G_R1(1.247) =
**1.0671643034034786**; B0c G_parent = **0.9819643798245946**. L' = 1.247 x .9819643798 / 1.0671643034 = 1.1474424 ->
**1.1474**. One NAV at L' was the cell (no further correction needed).

**Spec** `scripts/specs/v8/r1-comp-v8-gm.json` (template, new file; r1-comp-v8.json untouched): parent base-b0c.json;
change = r1-comp-v8.json's (fit `--composition ew-theme-std-v1`, E-28 w caps, the same fit / card / w / monitor names,
so u, fit, card, w resume as done) plus `nav.leverage` "1.1474" and `nav.output`
`build-equity/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474`. `lock` (dry) 0, `lock --write` 0 (reference_cell
B0c `119d4cd0`, reference_admission `29357ece`, = R-1's lock), dry lock after 0. File `531f8905...f373`; **spec digest
(template chain) `60ac1feb56d08b207813c32674773b11ad8969ddae84b0737c66937d43fb9eeb`**. Commit `9559a847`. Plan: every pin
`[locked, verified]` (library v7.1 `787c802e`, recipe `7f8a2643`, role lo3 `e1c67101`, v2-pit `09aac28f`, fund events
`304d2945`, SIC `9f9b2f85`, label role lo3-dlret `95e16cfe`, reference cell / admission as above; fields v9 lo3
`9f156363`, 63 rows). The planned NAV argv equals R-1's (receipt) except `--output` and `--aim-leverage 1.247 -> 1.1474`.

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| fields, u, fit, card, w | - | - | - | - | - | done (R-1's: fit `eb98a50a` weights, w `mega-v8-r1w-train-std-1`); gate `b0b-readout` PASS (0 status changes) |
| nav (L 1.1474) | 180 / 1,536 | 52.2 | 586 | 0 | `a429a0537b9da7110364bb99685fdcda6fbf0a7ad8b8b15b210f56d78ce8fd42` | `summary.json` `090e31ba`, S2 daily `7ef3b3bd`, `capacity_curve.csv` `ad0413cc`, `recipe.json` `b73a6109` |
| monitor | direct | 0.9 | - | 0 | - | `mega-monitor-v8-r1-std/monitor.json` `dc192887`: M2 alarm (alarm 4: high_52w, ind_mom_12_1, iv_rv_spread, sv_flow, as B0b; warn 10, ok 24), M4 ok, M1 / M3 n/a |
| summ | direct | 16.7 | - | 0 | - | `cycle-v8-r1-comp-v8-gm/summ.json` `a168bf9c`, `pbo.json` `dea68446`, `cycle_verdict.json` `ae0dbd91` |
| one-sided p (PM5-23) | bounded 180 / 1,536 | 0.8 | 319 | 0 | `db300a9bf24385f79c11fdd727e02b6edb17994f5cb050cfa5528537cdf7d947` | `nav_summ --protocol v8 --bundle B0c R-1gm --bundle-json build-equity/v8-cells-r1-bundle.json` `771fbf4c` (no `--ledger`) |

NAV receipt: completed, `clean in the code pathspec`, source `9559a847`, exe `5497c89d`; `construction.v5.aim_leverage`
1.1474 in the summary.

**Gross match:** G = **0.9817064213** vs G_B0c .9819643798: |diff| **.00026** <= .005 (first NAV). Post-ramp .9876
(B0c .9885); by year .958 / 1.002 / .977 / .990 (B0c .961 / .996 / .982 / .989); gross at score_begin .9341 (B0c .9257).

**Mechanics (S2, read before any return): PASS.** All-rows gross .9817 in [.90, 1.05]; all-rows net +.0041 (|.| <=
.02); tau mean .02451 <= .20, p95 .02973 <= .30 (1,004 sessions; summary meets_daily_turnover_mean / _p95 true); max
gross 1.114, max |net| .031; 1,006 CSV rows 2020-01-02..2023-12-29, 1,005 return rows; accounting: max cash-book error
6.4e-14, return identity 4.2e-16 (tol 1e-9).

**Criterion (PM5-11):** tau_gmv_mean / mean_gross_leverage_all_rows = .0245095 / .9817064 = **.024966** vs B0c
.0340689 / .9819644 = **.034695**: not higher -> PASS (at matched gross; at L 1.247 it was .02301).

**Statistics of record** (S2 = `modeled-1bn-stale5-v1+swap-fin-v1`): net Sharpe **R-1 +1.2031** vs B0c +1.1328. Paired
(studentized CBB, block 21, seed 20260929, 4,999 resamples, 4,999 valid; 1,005 common sessions): **dSR +.0703**;
rho .917; **Memmel SE .2050** (t +.34); CBB 95% [-.355, +.479]; LW studentized SE .2167, 95% [-.355, +.496];
**bootstrap p one-sided .3622, two-sided .7448** (the bundle reproduces the cycle's dSR, SE, CI and two-sided p
exactly). DSR block (verdict, `--dsr-ledger`): N 41; cell-count DSR .9857 (V[SR] from 4 cells on research-window-v2,
degenerate until PM5-22); legacy (37) .7519; effective-N .9023 (N_eff 2); PSR vs 0 .9904; CSCV PBO .3088 (41 cells).

**Verdict (prereg rule 5): dSR +.070 > 0 AND mechanics PASS AND criterion PASS -> ACCEPTED.** Recorded by the tooling:
`cycle-v8-r1-comp-v8-gm/cycle_verdict.json` (spec `60ac1feb`) and the ledger line (trial `50b42b5db1572631`, cell =
the L1.1474 NAV dir, s2_net_sr 1.20308, origin prior, window research-window-v2, prev = B0c's head `449847ef`).
**N after: 41** (ledger 42 lines = 41 construction + 1 protocol, file `dac5a01b...f028`, head `76e86ef3`). Matches the
brief. The L 1.247 run of r1-comp-v8.json stays unledgered (calibration, PM6-6).

Returns (S2, annual): **net 4.47%** (ann mean; CAGR 4.50%) vs B0c 4.42%; **gross of cost 5.76%** (B0c 6.05%); drags:
trade cost .76% (B0c 1.11%), borrow .33% (.33%), long financing .20% (.20%); vol 3.71% (3.90%); max drawdown 3.29%
(3.89%). Gross Sharpe 1.550 (B0c 1.552).

| year | rows | net return | net Sharpe | vol | tau | cost bps |
|---|---|---|---|---|---|---|
| 2020 | 252 | -.0145 | -.412 | .0340 | .0272 | 14.90 |
| 2021 | 252 | +.0958 | +2.304 | .0401 | .0230 | 11.47 |
| 2022 | 251 | +.0839 | +1.899 | .0431 | .0241 | 12.07 |
| 2023 | 250 | +.0181 | +.624 | .0297 | .0238 | 11.51 |

Capacity curve (report only; x1 = the primary): net Sharpe .5x 1.228, 1x 1.203, 2x 1.174, **4x 1.103** (B0c .978),
8x 1.014; cost bps per traded dollar 10.68 / 12.49 / 14.70 / 16.91 / 18.72.

**Appendix A:** `TRAIN construction cells 41; admission trials this sprint 0 (plus 0 re-screens); window
research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024); history
reads 0; 2025+ never read.` Defects: none. Consequences: R-1 accepted -> E-45: R-10 / R-11 defined (if R-6 is accepted,
E-38); R-3 runs `ew-theme-std-aim-v1` (E-27); every child inherits L 1.1474 as its step-(1) L.

**Next parent: `scripts/specs/v8/r1-comp-v8-gm.json`, spec digest `60ac1feb56d08b207813c32674773b11ad8969ddae84b0737c66937d43fb9eeb`,
G 0.9817064213, L 1.1474.**

### Cell R-2 (lib-v80 on r1-comp-v8-gm; library v8.0 on fields v10): STOPPED at the marginal phase (cycle refusal)

**Wave preparation (A2 root sequence, R-2 steps 1-5; P = `scripts/specs/v8/r1-comp-v8-gm.json`):**
1. E1 + E4 registry field rows (hand edit, commit `0c8b3e2c`): `ea_days_since` (sec-ea-days-since-reaction-v1,
   fields_v8), `inst_own_share` (13f-asof45-io-share-qe-v1, fields_v9), `grp_ff12f49` (ff12-money-ff49-v1,
   fields_v10); clock copied from the fields v10 lo3 manifest rows, basis = the manifest definition + formula id and
   producer (the existing rows' form). `generate_library` tests (`ATX_V71_PLAN_JSON` = the saved v7.1 plan): 8 passed,
   1 failed: **`test_v71_library_byte_identical` PASSES** (v7.1 regenerates byte for byte);
   `test_registry_seed_is_the_v71_library` FAILS by premise (it asserts the registry is exactly the v7.1 seed: field
   names, then alpha ids; any wave registration breaks it). Tests only; not fixed (FINDING).
2. **K1 FINDING:** the first `add-alpha` refused (exit 2, nothing written): `plan: atx-equity-strategy-ic.exe --plan-only
   exit 1: Unavailable: IC runner: required_bytes=1929078268 max_compiled_slots=8 exceeds configured memory budget`.
   add-alpha's built-in K1 (`generate_library.exe_plan`) passes no `--max-memory-mib`; on the 4-year role the exe's
   default budget is below the plan's need (runbook R12 runs plan-only with `--max-memory-mib 2560`). Used the A2
   sequence's named alternative `--plan-json`: a scratch helper (`scratchpad/k1plan.py`) runs add-alpha's own argument
   parsing and in-memory library build (no write), writes those exact bytes to a temp file and runs the parent spec's IC
   exe `--plan-only --max-memory-mib 2560` on them (metadata only, run directly as add-alpha runs it); the real
   add-alpha (unpatched) then validates `plan.library_sha256` against its own bytes. All 15 calls: plan sha = library
   sha, K1 rows valid.
3. The 7 READY calls (draft section 7 strings verbatim, `--parent-spec P`; earn_surprise_comp with `--removes sue
   --removes droe --removes chtax --exception max_extra_fields=8 --exception-basis "Ruling R2-b"`), then the 8 `_f49`
   re-screens in R2-8 order (`--replaces <x> --rescreen --fields build-equity/train-2020-2023-lo3-fields-v10`; DSL =
   the original's registry DSL with `grp_ff12` -> `grp_ff12f49`, checked; theme / tier / sign / citation of the original
   + "; S-12 FF49 financials"). Every call exit 0.
4. **Counts (all equal the brief / draft):** roster 52 (48 - 3 + 7; the 8 `_f49` in their originals' positions); recipe
   `trials`: admission_trials 7, rescreens 8 (the 8 ids), removed_parent_members 11 (accruals, asset_growth, cbop, chtax,
   droe, ebit_ev, gpa, noa, opex_at, q5_eg, sue), new_candidates 15, unchanged 37; gate p1-v80 admitted = the 7
   (require any, sign_agrees), report = the 8; exceptions earn_surprise_comp max_extra_fields 8, q5_eg_f49 6 (moved from
   q5_eg), qmj_safety max_slots 8. K1 rows of the 7 equal draft section 4 (DSL sha16 / bars / slots / extra fields:
   ear_mom_12m eb4ced22 256/5/2, earn_surprise_comp 959788e7 20/6/8, op_rd 211c045b 20/5/4, dtc_slow 4554a05f 145/5/1,
   pct_accruals 4711ad7c 20/4/2, fip_id f13685ff 272/4/0, si_low_io d76f1408 20/4/3); the 8 `_f49` rows equal the R2-8
   table and their originals' rows (grp_ff12 -> grp_ff12f49). Plan: 52 candidates, max slots 8, required lookback 272.
5. Registration appended to `v8-prereg.md` ("Library v8.0 (cell R-2)": the draft by sha `326ad240`, R2-a..h, the E5
   pins: library `68ce8539...52f4`, slim recipe `fb09b740...62d0`, `libraries/v80.json` `f15c6181`, stub `a6b74143`,
   registry `570f022a`, fields v10 `a4a060ae`); commit **`9d4203f0`** (registry, library, recipe, definition, stub,
   `scripts/specs/v8/lib-v80.json`, prereg).

**Spec** `scripts/specs/v8/lib-v80.json` (generated and locked by add-alpha; file `da386bcd...dc70f`): parent cell
R-1-gm (reference_cell `090e31ba`, reference_admission `29357ece`, reference_combined `140bcf4a`, reference_weights
`eb98a50a`, reference_daily `7ef3b3bd`, reference_orientations `f28327db`, reference_daily_ic `621deb65`); fields as
built v10 `a4a060ae` (70 rows), baseline_fields v9 `9f156363`; label_role lo3-dlret `95e16cfe`; fit `--composition
ew-theme-std-v1` (inherited); **nav.leverage 1.1474 (the parent's L = step (1))**, nav.output
`build-equity/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80`; w 300 s / 3,072 MiB (E-28); summ
dsr_n ledger+1, origin prior, verdict true. `plan` exit 0, every pin `[locked, verified]`.

**`run --screen`** (source `9d4203f0`; no free-memory refusal):

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| fields | - | - | - | - | - | done (as built v10) |
| ref, ref-compare | - | - | - | - | - | skipped (screen; run with the full `run`) |
| u | 300 / 2,560 | 33.3 | 1,497 | 0 | `561c0e0d266d9411bd64d73925240aca04d52aa46f8ccfbbe40fa051bb185d04` | `mega-v8-b0b-train-u-v80-1` |
| u-compare | internal | - | - | PASS | - | parent-orientations IDENTICAL (37 objects; b adds 15); parent-train-daily-ic IDENTICAL (108,262 rows of 37 keys byte for byte; b adds 43,890 rows of 15 keys) |
| fit | 180 / 1,536 | 7.3 | 450 | 0 | `10773e33470b286e11b791fdaaaaffceffeac61c5730de0715f77b3d5e10120b` | `mega-weights-v8-r1-std-v80` (not read) |
| card | 300 / 2,560 | 20.2 | 1,350 | 0 | `141d2199704a0f42a705625c095e40997e39b06241437464235054c55056e26d` | `mega-cards-v8-r1-std-v80` (not read) |
| marginal | bounded | 0.3 | 1 | **1** | `bf1d64d2488623e3717be4d2f110a07b3506748e50dcc16fe17b05bcddf38204` | **process-error -> research_cycle HARD-STOP [marginal], exit 4** |
| gate, w, nav, monitor, summ | - | - | - | not run | - | - |

**Refusal (stderr, whole):** `InvalidArgument: marginal IC: themes: pool member accruals is not in --library (its cached
signal builds its theme composite)`. Argv: `atx-equity-strategy-ic marginal --candidate-cache
build-equity/mega-candidate-cache-v8-lo3 --library fund_industry_ic_v80.json --pool
mega-v8-r1w-train-std-1/train_combined.json --role ...lo3 --themes mega-weights-v8-r1-std/composition_weights.json
--fields ...-fields-v10 --min-names 1000 --output ...-marginal`. Cause (from the text and the argv, no data): the
marginal verb builds the parent pool's theme composites from the pool members' cached signals, located through
`--library`; library v8.0 no longer holds 11 of the parent's members (3 removed, 8 replaced by `_f49`), so the first
of them (accruals) is refused. add-alpha writes this marginal section for every wave, including one with
`--removes` / `--replaces` (the A2 tests ran it on fake tools). **Stop rule applied ("any refusal by the cycle"):
STOPPED, not worked around** (dropping the section is a spec edit, and passing the parent library is a tool change:
PM5-21). Marginal IC is report-only (prereg rule 8, E-36); the cycle orders marginal before gate and hard-stops on it.

**Not read:** the admission (`admission.json`), the cards, the gate print-out, the u pass daily IC (the compare reads
it byte for byte; no value printed), any IC, return or Sharpe. The gate did not run, so **no admission line was
ledgered: admission trials 0, re-screens 0**. Ledger unchanged: 42 lines, `dac5a01b...f028`, **N 41**. Outputs stay on
disk (u, fit, card, the failed marginal run dir); a resume after a ruling starts at marginal.

### R-3: not started

Its parent is the last accepted cell after R-2, which is not decided; the batch stops at R-2's refusal.

### Hidden-data record (batch 2c)

- Inputs opened by the tools: role lo3, the lo3-dlret label role, fields v9 lo3 and v10 lo3 (metadata for the K1 plans;
  payloads by the u pass), the lo3 candidate cache, B0b's u pass and B0c's / R-1's outputs, the ledger and the
  ledgered NAV dirs of the summ grid (2020-2022), R-1-gm's own outputs. No atx-db stage was opened in this batch.
- Logs scanned for dates 2024-2029 (every bounded run's stdout / stderr, both cycle consoles, the add-alpha consoles):
  only the bootstrap seeds 20260929 / 20260927; last NAV session 2023-12-29. **Nothing dated 2024-01-01 or later was
  opened. No R-1 (L 1.247) return was opened** (its calibration use read the S2 gross column through the extractor).

### Open items (batch 2c)

- **PM ruling needed (R-2):** the K6 marginal verb refuses a library wave that removes or replaces parent members
  (refusal above). R-2's u / fit / card are done; nothing statistical read; nothing ledgered.
- K1 in add-alpha omits `--max-memory-mib` (4-year role refuses); the `--plan-json` route was used (above). A one-line
  tool fix (pass the parent's `ic.flags` memory option to `exe_plan`) is PM5-21 territory.
- `test_registry_seed_is_the_v71_library` fails once any field row or alpha is registered (test premise).
- Current accepted parent: **R-1-gm** (`r1-comp-v8-gm.json`, digest `60ac1feb`, L 1.1474), S2 net Sharpe +1.2031,
  net annual return 4.47%. N 41; admission trials 0; history reads 0. Disk 70,779,164 KiB free (67.5 GiB).

## cells batch 2d: FIX-6 round 4, R-2 (resumed), R-3, R-4, R-5 (2026-10-02)

Integrator in `C:/atx-wt/pool-2`, start `9c3f23ec` (clean; code head `1cc4c6c9`; executables v8-12 Debug: IC `ab7e2cbd`,
NAV / targets `5497c89d`, risk `8967952c`; nothing built; PM5-21 holds). Read: progress PM6-7 (cleanup; nothing of v8),
PM6-8 (R-2 marginal: memo option (i)), PM6-9 (K1 `--plan-json` route accepted and standing), PM6-10 (registry seed
test: FIX-6 round 5), PM6-6 (gross matching, every cell). Same scratch readers as batch 2c (`mech.py`: mechanics keys
only; `cellstats.py`: return side, after mechanics passed; `bundle.sh`: the PM5-23 bundle).

### 0. FIX-6 round 4 merged (tests only)

`git merge --no-ff 8f48ee39` -> **`205ba34d`** (ort, no conflict). Diff: `scripts/tests/test_research_spec.py` (+7 / -3)
and `task-FIX-6-report.md` (+18); nothing else. `scripts/tests` (whole; `ATX_EQUITY_BIN` / `ATX_EQUITY_TARGETS_EXE`
absolute v8-12; vcpkg bins on PATH): **17 failed, 171 passed, 3 skipped** (104.6 s; skips: the three
RESEARCH_CYCLE_LIVE_ROOT tests). The B0c label-role failure of batch 2b is gone. **All 17 failures are in
`test_research_spec.py` and trace to batch 2c's additions** (not edited; PM6-10's round 5 covers them):
- 14 x `test_every_v8_spec_loads_and_plans[*]` and `test_the_fixtures_plan_a_locked_spec_as_unlocked`: the authored-set
  equality (`:278`) and the fixture map (`:225`, KeyError) do not know the hand-written `r1-comp-v8-gm.json`;
- `test_add_alpha_on_a_v8_template_removes_replaces_rescreens_and_records_exceptions` (`:1124`): add-alpha in the
  fixture root refuses (exit 2) re-registering `q5_eg_f49`, which the live registry now holds with another definition
  (the R-2 wave), and its `earn_probe` exceeds the house budget there;
- `test_the_whole_file_passes_with_a_generated_spec_present`: the meta-test (the same inner failures).
The registry seed test (`atx-impl/strategies/test_generate_library.py`) is not in this suite (known, round 5).

### Cell R-2 (lib-v80 on R-1-gm; library v8.0 on fields v10; L 1.1474): ACCEPTED, N 42

**Ruling PM6-8 applied:** `scripts/specs/v8/lib-v80.json`: `marginal.themes` deleted, `marginal.output` ->
`build-equity/mega-v8-b0b-train-u-v80-marginal-pool` (the failed `...-marginal-run` dir stays, never overwritten);
`lock` / `lock --write` / dry lock: 0 / 0 / 0, every pin unchanged (diff = the two lines). File / spec digest (plain
spec) **`306a070b865d3625dc03b477e528a63ae52558cc8c8b231d3df787ec3bab3a21`**. **PM6-9:** the 15 K1 plans committed under
`.superpowers/sdd/platform-v8-20260929/r2-plans/` (SHA-256): ear_mom_12m `8623c52d...`, earn_surprise_comp
`4f98db69...`, op_rd `a5a34e5d...`, dtc_slow `cdb604b1...`, pct_accruals `8c145931...`, fip_id `6116f3cb...`,
si_low_io `0d8f9275...`, ebit_ev_f49 `e14180dc...`, gpa_f49 `4c8b0fe0...`, cbop_f49 `a678b054...`, noa_f49
`c0ee6b37...`, accruals_f49 `51b10805...`, opex_at_f49 `84f5d539...`, asset_growth_f49 `776c50b9...`, q5_eg_f49
`8c7c3b75...` (full digests: `sha256sum r2-plans/*.json`). Commit `39f87cd6`. Plan: every pin `[locked, verified]`.

**Counts before any statistic** (batch 2c, unchanged): roster 52; 7 admission trials; 8 re-screens; 11 removed; K1 rows =
draft section 4 / R2-8; u-compare IDENTICAL (37 kept members: orientations 37 objects, daily IC 108,262 rows byte for
byte).

**Runs** (source `39f87cd6`; then resumed; no free-memory refusal):

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output / result |
|---|---|---|---|---|---|---|
| u, fit, card | - | 33.3 / 7.3 / 20.2 | 1,497 / 450 / 1,350 | 0 | batch 2c | done |
| marginal (pool only, PM6-8) | 300 / 2,560 | 135.1 | 296 | 0 | `42a9556b6bab9f062108a095327dd15d69c3724986c46e44a15b95ab4dcefc38` | `...-marginal-pool/marginal_ic.json` (report only) |
| gate p1-v80 | internal | - | - | PASS | - | 7 admission lines ledgered (chained, before the read-out) |
| ref (identity) | 180 / 1,536 | 42.6 | 585 | 0 | `f1260d27cacd84a58483fc63e4c1d8f4e62252b27704025d9766466eedc51615` | **ref-s2-daily IDENTICAL bit for bit** to R-1-gm's S2 daily (`7ef3b3bd`, 940,403 B): fields v10 reproduces the parent |
| w | 300 / 3,072 (E-28) | 35.7 | 1,306 | 0 | `1becd1aed2aa1d287ed2e4a79134554cd04620c0c47567fe6ae7424a07d158d8` | `mega-v8-r1w-train-std-v80-1`; weights `03213345` (ew-theme-std-v1, 41 of 52 weighted) |
| nav (L 1.1474 = step 1) | 180 / 1,536 | 43.2 | 586 | 0 | `15ea731572511a0d70e8b28f843231990908b17dedb8ffda447777e6de4857c1` | `summary.json` `083a56da`, S2 daily `7cfe21c4`, capacity `b99b1cd1` |
| monitor | bounded | 1.3 | 110 | 0 | - | M2 alarm 4 (high_52w, ind_mom_12_1, iv_rv_spread, sv_flow; as every cell), warn 13, ok 24; M4 ok |
| summ | bounded | 16.4 | 543 | 0 | - | `cycle-v80/summ.json` `960a19a5`, `cycle_verdict.json` `d53ee1ff` |
| one-sided p | 180 / 1,536 | 0.8 | 559 | 0 | `171668e6e0d6e1e6e5e9cc04371170e8b256cd1c7ecc21d125a46992d1e572cb` | `v8-cells-r2-bundle.json` `575d4da7` |

**Admission (gate p1-v80, v4-prior-v1; require any): PASS, 3 of 7 admitted with the prior sign** -- ear_mom_12m (HAC t
.88), pct_accruals (1.07), fip_id (.53). earn_surprise_comp and op_rd: status admitted but runner sign 0 against prior
+1 (sign_agrees false; not counted by the gate); dtc_slow reject_redundant (|rho| .959 with dtc); si_low_io
reject_redundant (.955 with si_ratio). **Re-screens: all 8 `_f49` admitted.** Reference members vs R-1's admission: 1
status change, nincr reject_redundant -> admitted (sue / droe / chtax left). Book: 41 of 52 weighted. K6 (pool only,
report only, gates nothing): marginal IC21 / HAC t ear_mom_12m +.0058 / .87, earn_surprise_comp -.0186 / -2.06, op_rd
+.0123 / 1.12, dtc_slow +.0037 / .50, pct_accruals +.0233 / 2.46, fip_id +.0108 / .76, si_low_io +.0319 / 1.98.

**Gross match (step 1 at the parent's L 1.1474): G = 0.9859903463 vs G_parent (R-1-gm) 0.9817064213: |diff| .00428 <=
.005 -> the cell stands at L 1.1474** (no `-gm` spec, no correction; calibration runs 0 beyond the cell's own NAV).

**Mechanics (S2): PASS.** All-rows gross .9860; net +.0042; tau mean .02393 / p95 .02841 (1,004 sessions; summary flags
true); max gross 1.118, max |net| .029; gross at score_begin .940; by year .964 / 1.002 / .984 / .995; 1,006 CSV rows,
1,005 return rows; accounting 4.5e-14 / 3.7e-16.

**Criterion (book turnover not higher; executed tau_gmv_mean, PM5-11 for R-2):** .023929 vs R-1-gm .024509 -> PASS.

**Statistics of record:** S2 net Sharpe **R-2 +1.2559** vs R-1-gm +1.2031. Paired (1,005 sessions, 4,999 valid
resamples): **dSR +.0529**; rho .982; **Memmel SE .0945** (t +.56); CBB 95% [-.087, +.190]; LW SE .0706, 95% [-.090,
+.196]; **bootstrap p one-sided .2398, two-sided .4694** (the bundle reproduces the cycle exactly). DSR (N 42): ledger
DSR .9859 (V[SR] from 5 cells on research-window-v2); legacy .7805; effective-N .9164; PSR vs 0 .9926; PBO .2675.

**Verdict (rule 5): dSR +.053 > 0 AND mechanics PASS AND book turnover not higher -> ACCEPTED (wave judged whole).**
Ledger line trial `2082800117b50112` (cell = the R-2 NAV dir, s2_net_sr 1.25594, prev `e2c10ef2`). **N after: 42**
(ledger 50 lines = 42 construction + 7 admission + 1 protocol; file `89ff5e26`; head `8c0f8161`). **Admission trials
this sprint: 7 (plus 8 re-screens).**

Returns (S2, annual): **net 4.54%** (CAGR 4.57%) vs R-1-gm 4.47%; **gross of cost 5.81%** (5.76%); trade cost .74%,
borrow .33%, long financing .20%; vol 3.61%; max drawdown 3.25%; gross Sharpe 1.610.

| year | rows | net return | net Sharpe | vol | tau | cost bps |
|---|---|---|---|---|---|---|
| 2020 | 252 | -.0111 | -.316 | .0336 | .0264 | 14.79 |
| 2021 | 252 | +.0983 | +2.596 | .0364 | .0227 | 11.45 |
| 2022 | 251 | +.0796 | +1.841 | .0423 | .0235 | 12.10 |
| 2023 | 250 | +.0193 | +.638 | .0310 | .0231 | 11.52 |

Capacity (report only): net Sharpe .5x 1.289, 1x 1.256, 2x 1.223, **4x 1.178** (R-1-gm 1.103), 8x 1.100.

**Appendix A:** `TRAIN construction cells 42; admission trials this sprint 7 (plus 8 re-screens); window
research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024); history
reads 0; 2025+ never read.`

**Next parent: `scripts/specs/v8/lib-v80.json` (spec `306a070b`), G 0.9859903463, L 1.1474, library v8.0.**

### Cell R-3 (r3-aim-gain-gm on lib-v80; ew-theme-std-aim-v1; L 1.1264): NOT ACCEPTED (dSR < 0), N 43

**Rule variant (quoted):** task-CELLS-brief.md:56, R-3: "net Sharpe at 2x NAV not lower AND turnover lower; rule
`ew-theme-std-aim-v1` if R-1 accepted, else `ew-theme-aim-v2`" (E-27, E-27a, E-27b). R-1 was accepted, so
**ew-theme-std-aim-v1**; the template maps the parent's `--composition ew-theme-std-v1` to it (resolved fit flags
`--orientation prior --screen v4-prior-v1 --composition ew-theme-std-aim-v1`; weights provenance.rule
ew-theme-std-aim-v1, theme_standardise ew-theme-std-v1 rerank true, 41 weighted).

**Specs.** `r3-aim-gain.json` (template): `"parent": null -> "lib-v80.json"` only; lock --write derived reference_cell R-2
`083a56da`, reference_admission `f613fe92`, reference_combined `bbbf6f2b`, reference_weights `03213345`; commit
`3137edf7`. Spec digest `544a6abd...`. Resolved: u = R-2's (done), nav.leverage 1.1474 inherited; the template also
inherits R-2's `marginal` (output `...-marginal-pool`, done: R-2's report-only K6 file; not re-run) and gate p1-v80
(re-read on R-3's admission: 0 status changes vs R-2, 3 of 7, PASS; "0 admission trial line(s) appended, 7 already
ledgered"). **Calibration step (1)** = its NAV at L 1.1474 (`...-r3-aim-...-L1.247`: the name keeps the template's
"L1.247" text; the run is at 1.1474): 52.2 s, 586 MiB, receipt `e344b0fa...d5fd`; mechanics only: **G 1.0043324403**
vs G_parent .9859903463 (|diff| .0183 > .005). L' = 1.1474 x .9859903463 / 1.0043324403 = 1.126445 -> **1.1264**.
Matched spec `scripts/specs/v8/r3-aim-gain-gm.json` (new template: r3's change + `nav.leverage` "1.1264" + `nav.output`
`build-equity/mega-nav-v8-r3-aim-t.05-d.1-fixed-obdelta-x.05-loc-L1.1264`; parent lib-v80.json; same locked pins); file
`18a791eb...`, **spec digest (template chain) `f04545ea...`**; commit `75db27aa`.

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| fit (ew-theme-std-aim-v1) | 180 / 1,536 | 32.3 | 455 | 0 | `ef82211c32230052f172a02bea061bbd46ffcc9f7cdfd591bf24eefbf5eacda5` | `mega-weights-v8-r3-aim` (weights `68a7f3f8`) |
| card | 300 / 2,560 | 13.6 | 1,240 | 0 | `42e240e3ff948a66bf0b361861718200de434b46dab929f94e41e75e742a88c5` | `mega-cards-v8-r3-aim` |
| w | 300 / 3,072 | 36.5 | 1,307 | 0 | `3e71f83bd959918dc781f631c24a4d58606a92f7a7c1282a5a8ed9efa16c4ad8` | `mega-v8-r3w-train-aim-1` |
| nav step 1 (L 1.1474, calibration) | 180 / 1,536 | 52.2 | 586 | 0 | `e344b0fa508d3a902f0438c0d58b16347d9ac6a4d65aa3274d1e66e3f4edd5fd` | mechanics only |
| nav (L 1.1264, the cell) | 180 / 1,536 | 50.8 | 585 | 0 | `4ed96658769d083ed8c3dc1e5153831ff11dd7871704cbbe5348188eb76d867b` | `summary.json` `06c8aa21`, S2 daily `a544109a`, capacity `c8170ac6` |
| monitor / summ | bounded | 1.0 / 16.9 | 112 / 556 | 0 | - | `cycle-v8-r3-aim-gain-gm/summ.json` `b9adfcc8`, verdict `6aad8733`; M2 alarm 4 (same members), warn 13 |
| one-sided p | 180 / 1,536 | 0.8 | 493 | 0 | `91d4655da12a41778740c76a9b9571f1d60a141b0ddc6c06b270564e4532e4df` | `v8-cells-r3-bundle.json` `21ee58a6` |

**Gross match:** G **0.9859230552** vs .9859903463: |diff| **.00007** (one correction). **Mechanics PASS:** gross .9859,
net +.0041, tau .02163 / .02640, max gross 1.120, max |net| .029, score_begin .939, by year .966 / 1.000 / .983 / .995,
accounting 4.5e-14 / 4.1e-16.

**Statistics:** S2 net Sharpe **R-3 +1.2441** vs R-2 +1.2559: **dSR -.0118**, rho .990, **Memmel SE .0710** (t -.17),
CBB [-.142, +.116], LW SE .0680 [-.147, +.124], **p one-sided .5814, two-sided .8678** (bundle = cycle). DSR N 43:
ledger .9840; effective-N .7346 (N_eff 4); PBO .2737.

**Criterion (config v8.cells R-3):** net Sharpe at 2x NAV 1.2172 vs R-2 1.2230: **lower -> FAIL**; tau_gmv_mean .021632
< .023929: lower -> pass. **Verdict (rule 5): dSR -.012 <= 0 (and the 2x criterion fails) -> NOT ACCEPTED.** Ledger
trial `d7b1465e1d3e894f` (s2_net_sr 1.24414, prev `8c0f8161`); **N after 43** (ledger 51 lines: 43 construction + 7
admission + 1 protocol; file `59ebfb9d`; head `8cf0157e`). Not retried. Parent stays R-2.

Returns (S2): net 4.58% (CAGR 4.62%); gross of cost 5.79%; trade cost .67%, borrow .33%, long financing .20%; vol
3.68%; max drawdown 3.26%. Capacity: .5x 1.271, 1x 1.244, 2x 1.217, **4x 1.171**, 8x 1.086.

| year | rows | net return | net Sharpe | vol | tau | cost bps |
|---|---|---|---|---|---|---|
| 2020 | 252 | -.0065 | -.166 | .0353 | .0241 | 14.84 |
| 2021 | 252 | +.0937 | +2.545 | .0355 | .0206 | 11.40 |
| 2022 | 251 | +.0830 | +1.871 | .0433 | .0211 | 12.17 |
| 2023 | 250 | +.0174 | +.554 | .0323 | .0207 | 11.49 |

Appendix A: `TRAIN construction cells 43; admission trials this sprint 7 (plus 8 re-screens); ...; history reads 0.`

### Cell R-4 (r4-hold-band on lib-v80; nav --hold-band .1; L 1.1474): NOT ACCEPTED (criterion and dSR), N 44

**N before: 43; this cell would make 44 (<= 51).** Brief row (task-CELLS-brief.md:57): "turnover at least 15% lower
(`--hold-band .10`)"; config v8.cells R-4: `tau_gmv_mean le 0.85 x parent`. Parent = the last accepted cell = R-2
(`lib-v80.json`); R-3 was not accepted.

**Identity cell:** not run here. The plan's R-4 step 3 ("identity cell with band 0") was integration 5 part C identity
2 (`--hold-band 0` = flags absent, 12 of 12 files byte-identical, PASS); Ruling PM4-3 re-ran identities 1, 4, 7, 8 only
(2 and 3 retired). The brief prescribes no identity per cell; R-1 ran the same way (identity 4 at integration).

**Spec.** `scripts/specs/v8/r4-hold-band.json` (template): `"parent": null -> "lib-v80.json"` only; lock --write derived
reference_cell R-2 `083a56da`, reference_admission `f613fe92`, reference_combined `bbbf6f2b`, reference_weights
`03213345`; dry lock after exits 0. File `a43894f6df7e759f3c4116569f126a37984d8d03079587f5040715bb95035592`, **spec digest
(template chain) `952c5a9443c4dcd0bdd791b6c83e680863aaa6763315ea62cc97275ba8a9bbc7`**; commit `58b24585`. Plan: every pin
`[locked, verified]` (library v80 `68ce8539`, recipe `fb09b740`, role lo3 `e1c67101`, label_role `95e16cfe`, fields
v10 `a4a060ae` 70 rows); u, fit, card, marginal, w, monitor = R-2's (done); nav pending with the parent's argv plus
`--hold-band .1` at `--aim-leverage 1.1474`; summ `--dsr-n 44`. The nav output name keeps the template's "L1.247" text
(cosmetic; the run is at 1.1474). Gate p1-v80 re-read: 0 status changes, 3 of 7, PASS; "0 admission trial line(s)
appended, 7 already ledgered".

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| nav (L 1.1474; step (1) = the cell) | 180 / 1,536 | 40.6 | 586 | 0 | `cb5a3de8b44629e44c88b34d122fe35464d5c50ddfdb13a9effbf43dbe58536d` | `summary.json` `e597831a`, `recipe.json` `bd5ff67e` (rule `aim-partial-v5+neutral-price-risk-v1+hold-band-0.1`), S2 daily `556ae0d2`, capacity `aeac451d` |
| summ | 180 / 1,536 | 16.4 | 546 | 0 | `9b3aba1b108b3a33ce01139cadb61807be7fa0d1057fa4035b0c8e26cba59a79` | `cycle-v8-r4-hold-band/summ.json` `e020e4df`, verdict `da7c0186` |
| one-sided p | 180 / 1,536 | 0.8 | 479 | 0 | `2fcebf4cd907caefca0e543f5b948a48edee43453b892f1c24864c5919e3e65b` | `v8-cells-r4-bundle.json` `8495f785` |

Receipts: completed, `clean in the code pathspec`, source `58b24585`, NAV exe `5497c89d` (v8-12).

**Gross match:** G **0.9874700365** vs G_parent .9859903463: |diff| **.00148** <= .005: stands, no correction, one run
(the step-(1) run is the cell). **Mechanics PASS:** gross .9875 (post-ramp .9932), net +.0040, tau .02363 / p95 .02818
(limits met), max gross 1.119, max |net| .029, score_begin .941, by year .966 / 1.003 / .985 / .996, accounting
1.3e-13 / 3.8e-16. Hold band active in every scenario: band .1, 1,004 decisions, **mean_kept_share .884** (kept
1,639,241 / moved 214,395 name-decisions).

**Criterion (config v8.cells R-4):** tau_gmv_mean **.023628** vs 0.85 x R-2's .023929 = **.020340**: **FAIL** (turnover
-1.3%, not -15%).

**Statistics:** S2 net Sharpe **R-4 +1.2372** vs R-2 +1.2559: **dSR -.0187**, rho .9994, **Memmel SE .0176** (t -1.07),
CBB [-.0517, +.0152], LW SE .0167 [-.0525, +.0151], **p one-sided .857, two-sided .2752** (bundle = cycle). DSR N 44:
ledger .9844 (7 cells), legacy .7653, effective-N .9073 (N_eff 2), cell-count .7781; PSR .9918; PBO .2673.

**Verdict (rule 5): criterion FAIL and dSR -.019 <= 0 -> NOT ACCEPTED.** Ledger trial `7e7a553d759771f1` (s2_net_sr
1.23721, prev `8cf0157e`); **N after 44** (ledger 52 lines: 44 construction + 7 admission + 1 protocol; file
`ed73406d`; head `a8d43fc9`). Not retried. Parent stays R-2.

Returns (S2): net 4.43% (CAGR 4.46%); gross of cost 5.70%; trade cost .73%, borrow .33%, long financing .20%; vol
3.58%; max drawdown 3.24%; gross Sharpe 1.593. Capacity: .5x 1.272, 1x 1.237, 2x 1.208, **4x 1.154**, 8x 1.085.

| year | rows | net return | net Sharpe | vol | tau | cost bps |
|---|---|---|---|---|---|---|
| 2020 | 252 | -.0117 | -.342 | .0328 | .0261 | 14.85 |
| 2021 | 252 | +.0949 | +2.522 | .0362 | .0225 | 11.47 |
| 2022 | 251 | +.0788 | +1.831 | .0421 | .0232 | 12.12 |
| 2023 | 250 | +.0194 | +.644 | .0307 | .0227 | 11.52 |

Appendix A: `TRAIN construction cells 44; admission trials this sprint 7 (plus 8 re-screens); ...; history reads 0.`

v9 note (from a result, not a spec): the band holds 88% of names' desired values per decision yet book turnover falls
1.3%; under aim-partial-v5 (theta .05) the traded book already lags the aim, so desired-side hysteresis barely reaches
fills.

### Owner stop (coordinator message after R-4's log commit `7863dbf8`): R-5 NOT STARTED

The halt reached the batch at a clean boundary: R-2, R-3 and R-4 finished, ledgered and logged (sections above). For
R-5 only the template edit had been made, uncommitted: `"parent": null -> "lib-v80.json"` and a `lock --write` (pins
only, no run, no plan, no output). It was restored byte for byte to HEAD's
`scripts/specs/v8/r5-adv-hold.json` (blob `520772da`, sha256 `0a60b415...b79d`, parent null). No R-5 phase ran;
no `build-equity/*r5*` or `cycle-v8-r5*` output exists; nothing read, nothing ledgered, no trial burned. **A resume
starts R-5 from its template:** set the parent (the last accepted cell, R-2 `lib-v80.json` today), lock, commit, plan,
`run --stop-after nav` at the inherited L 1.1474 (the ADV cap reads that L, memo follow-up point 4), then gross
matching. Criterion inputs a resume needs from the parent (not read here): R-2's 4x net Sharpe 1.178 (read in R-2's
cell) and R-2's S3 (terminal-adverse) net Sharpe (not yet read).

### Hidden-data record (batch 2d)

- Inputs opened by the tools: role lo3, the lo3-dlret label role, fields v10 lo3, the lo3 candidate cache, R-2's u /
  fit / card / marginal / w outputs, the ledger, the ledgered NAV dirs of the summ grid, each cell's own outputs, the
  15 committed K1 plan files. No atx-db stage was opened.
- Every console of this batch (R-2 resume, R-3 and R-3-gm plan / runs, R-4 plan / runs) scanned for 2024-2029 date
  tokens: none. Bundles carry only the bootstrap seed 20260929. Last NAV session in every cell 2023-12-29
  (`last_session_ns` 1703808000000000000). **Nothing dated 2024-01-01 or later was opened. No R-1 (L 1.247) return was
  opened. R-3's step-(1) run (L 1.1474) was read for mechanics keys only.** No `stdout.log` of a NAV run was opened.

### Open items (batch 2d)

- R-5 not started (owner stop); resume as above. N 44 leaves 7 trials of the 51 budget; R-5 would be 45.
- `scripts/tests` after round 4: 17 failed / 171 passed / 3 skipped (above), all PM6-10's. Not re-run since; expected
  further impact on the same file (not verified): `test_every_v8_spec_loads_and_plans` does not know the new
  `r3-aim-gain-gm.json`; the add-alpha template test copies the live `r4-hold-band.json` expecting `"parent": null`,
  which is now `lib-v80.json` (`r3-aim-gain.json` likewise). The round-5 lane should take the current head.
- Three spec files carry a cosmetic "L1.247" in their nav.output name though they ran at 1.1474 (`r3-aim-gain.json`'s
  step-(1), `r4-hold-band.json`); `r5-adv-hold.json` will too unless a `-gm` spec renames it.
- K1 `--max-memory-mib` in add-alpha (batch 2c item) stands; PM6-9 route used.
- **Current accepted parent: R-2** (`scripts/specs/v8/lib-v80.json`, spec name `v80`, file `306a070b`), NAV output
  `build-equity/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80`, L 1.1474, G .9859903463; S2 net
  Sharpe +1.2559, net annual return 4.54%, net Sharpe at 4x 1.178. **N 44**; admission trials 7 of 15 (plus 8
  re-screens); history reads 0. Disk 126,526,756 KiB free (120.7 GiB).

## interim report, render 3 (owner stop 2) (2026-10-02)

Ruling PM6-11, cut by the owner mid-task to speed over completeness (stats and equity curves first). Book R-2; ladder
B0a, B0b, B0c, R-1-gm, R-2, R-3, R-4. No cell, no trial (N 44), no build, no spec / cycle / executable change; the R-1
run at L 1.247 is not an input. Code commits: `6981558a` (v8_headline block; combined_label / universe_kept_label),
`bc946b4d` (config writer, copy-only B0c diagnostics assembler), `88963c19` (alpha-t raw-direction column dropped when the
recipe records none; trial-ledger block in the interim config; scorecard filler draft, not run).

- Render: `run_bounded_research.py --output build-equity/v8-interim3-pitch-render-run2 --seconds 600 --max-rss-mib 6144
  --min-free-mib 400 -- python atx-impl/tools/mega_report --config docs/plans/mega-alpha-v8-pitch.interim.config.json
  --out <scratch>/pitch-run2.html --stamp "2026-10-02 interim render 3 at the owner stop (Ruling PM6-11)"`; exit 0,
  13.5 s, HEAD `88963c19`; receipt sha256 `3210b985`, stdout `c5366a58`. Config sha256 `81a89298`. Copied byte for byte
  to `docs/plans/2026-10-02-mega-alpha-v8-interim-pitch.html`: 2,057,347 B, sha256 `fd3a415e`. Old
  `2026-10-01-mega-alpha-v8-interim-pitch.html` removed (git rm).
- Inputs (all pre-existing): `mega-nav-v8-summ-interim3.json`, paired bundles `v8-cells-{b0b,r1,r2,r3,r4}-bundle.json`,
  the seven NAV dirs, R-2 stress dir `mega-nav-v8-r2-v80-stress`, B0c diagnostics `mega-diagnostics-v8-b0c/diagnostics-v8.json`,
  R-2 weights / `-c2` cards and admission. Every input path and sha256 is in the page's appendix (t-appendix-files).
- Content: interim callout (no V8-F, no freeze gate; p one-sided per cell; L and all-rows gross per cell, PM6-6);
  headline B0c vs R-2 (`t-v8-headline`: L, S2 net Sharpe +1.1328 / +1.2559, net annual 4.42% / 4.54%, gross-of-cost
  annual 6.05% / 5.81%, net SR at 2x 1.085 / 1.223 and 4x .978 / 1.178, tau .0341 / .0239, gross .9820 / .9860, cost
  13.13 / 12.47 bps) and both year tables; ladder (`t-v8-ladder`, dSR, p, verdict, N 38..44) and its figure; per-cell
  year tables; trial ledger; diagnostics G-1..G-3 labelled B0c; R-2 member horizon; R-2 book section (equity +
  drawdown with B0c S2 beside, returns, rolling, costs, turnover, capacity curve, exposures, fills, signal correlation,
  universe, IC panel, decay, tau-IC, theme-year, alpha-t, theme weights). Numbers equal this log's batch 1b / 2c / 2d
  figures at printed precision.
- Checks: renderer unavailable 0; parse: 0 `class="unavailable"`, 22 figures / 88 inline SVG (fig-equity 12,072 path
  points), 84 "n/a" data markers (R-2's u pass has null rank means for 10 candidates; ic_theta unscored for 10 members);
  no R-1 L 1.247 dir name, no "r7"; dates 2024+ appear only in the window disclosure and literature citations. External
  refs: Google Fonts stylesheet + preconnect and literature hyperlinks only; no local src / href / fetch / import, so the
  page renders offline with fallback fonts. Headless Edge (`msedge --headless=new`) `--dump-dom`: 22 figures, 88 SVG, no
  console error; screenshots (scratchpad `render3/`) `shot-1-top.png` (1400x3600), `shot-2-equity.png` and
  `shot-3-ladder.png` (crops of a 1400x30000 shot) viewed: header, callout, headline, years, equity + drawdown drawn.
  `pytest test_mega_report_v8.py test_mega_report_v8_render.py`: 138 passed.
- Not in this interim render: the render-3 scorecard markdown (filler drafted, not run; the 2026-10-01 render-1
  scorecard stays); v7 figures fig-flow / fig-ops-loop (prose diagrams outside the v8 design) and fig-capacity (L-pair
  proxy: no readable v8 L pair). No v7 block-for-block comparison (owner cut).
- Tool changes (all in `atx-impl/tools/mega_report/`, no pin in `scripts/specs/v8/` or `v8-prereg.md` records a tool
  digest): `v8.py` new block `v8_headline`; `pitch.py` `combined_label` / `universe_kept_label` config keys (v7 defaults
  keep the v7 bytes); `report.py` `blk_alpha_t` omits the Raw dir column when no candidate records a raw direction.

## cells batch 2e: tests (PM7-4), R-5, risk model on the 4-year role, R-6 (2026-10-02)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `3c6ae225` (clean; code head `1cc4c6c9`;
executables v8-12 Debug: IC `ab7e2cbd`, NAV / targets `5497c89d`, risk `8967952c`; nothing built; PM5-21 holds).
Read: integrator-rules, status 6 sections 1, 2, 4, progress "PM session 6" to the end (PM6-1..12, PM7-1..5),
task-CELLS-brief, this log's batches 2c and 2d. Same scratch readers as batches 2c / 2d (`mech.py`: mechanics keys
only; `cellstats.py`: return side, after mechanics passed; `bundle.sh`: the PM5-23 bundle). Parent = R-2
(`scripts/specs/v8/lib-v80.json`, L 1.1474, all-rows gross .9859903463). N 44 at the start. Disk 125,601,484 KiB free.

### 1. Tests (Ruling PM7-4; tests only)

`scripts/tests/test_research_spec.py` (+3 lines): `NULL_PINS["r3-aim-gain-gm.json"] = CHILD_NULLS` (the authored-set
equality at `:290`), `EXPECTED_CHANGES["r3-aim-gain-gm.json"] = EXPECTED_CHANGES["r3-aim-gain.json"] | {"nav.leverage"}`
and the composition map entry `"r3-aim-gain-gm.json": ("ew-theme-v1", "ew-theme-aim-v2")` (as `r1-comp-v8-gm.json`'s
pins: the registered change of `r3-aim-gain.json` on its nominal parent base-b0c, plus `nav.leverage`). The third line
was needed: with the first two alone `test_templates_differ_from_the_parent_only_by_the_registered_change[r3-aim-gain-gm.json]`
failed on the fit flags (the change's composition map was unknown for the new name). Python
`"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider -rs`, `ATX_EQUITY_BIN` /
`ATX_EQUITY_TARGETS_EXE` absolute (v8-12), vcpkg bins on PATH:

| suite | before | after |
|---|---|---|
| `scripts/tests` (whole) | 17 failed, 174 passed, 3 skipped (161.7 s) | **192 passed, 3 skipped, 0 failed** (111.9 s) |
| `atx-impl/strategies` (whole) | - | **163 passed** (21.6 s) |

The 3 skips are the RESEARCH_CYCLE_LIVE_ROOT tests (as every batch).

Commit `81c80c39`.

### Cell R-5 (r5-adv-hold on lib-v80; nav --adv-hold-q .1; L 1.1474)

**N before: 44; this cell makes 45 (<= 51).** Brief row (task-CELLS-brief.md:58): "net Sharpe at 4x higher AND net at
1x not lower by more than one paired SE AND S3 not lower (`--adv-hold-q .10`, capacity curve)" (E-15, PM4-5). Parent =
the last accepted cell = R-2 (`lib-v80.json`); R-3 and R-4 were not accepted.

**Identity cell:** not run here (as R-4): the template's "identity cell first: --adv-hold-q 1e9" was integration 5
part C identity 3 (PASS, R-5's declared differences only); PM4-3 retired identities 2 and 3. No executable changed since
(v8-12, NAV `5497c89d`).

**Spec.** `scripts/specs/v8/r5-adv-hold.json` (template): `"parent": null -> "lib-v80.json"` only; `lock` (dry) 0,
`lock --write` 0 (reference_cell R-2 `083a56da`, reference_admission `f613fe92`, reference_combined `bbbf6f2b`,
reference_weights `03213345`), dry lock after 0. File `304e7f1aaf835e14038f0eb20609c65ea0f509cfbb25f6cb001b4fd0e4811dc2`,
**spec digest (template chain) `4336ab810fa19a61951b5e8cf60097ab6dd185da1fb0596614bd1527dc4ae5d3`**; commit `8ed541a6`.
`test_research_spec.py` with the locked file: 51 passed (whole-file meta-test deselected). Plan exit 0: every pin
`[locked, verified]` (library v80 `68ce8539`, recipe `fb09b740`, role lo3 `e1c67101`, label_role `95e16cfe`, fields v10
`a4a060ae` 70 rows); fields, u, fit, card, marginal, w, monitor = R-2's (done); nav pending: R-2's argv plus
`--adv-hold-q .1` (`--capacity-curve` already the parent's) at `--aim-leverage 1.1474`; summ `--dsr-n 45`. The nav
output name keeps the template's "L1.247" text (cosmetic; the run is at 1.1474). Gate p1-v80 re-read: 0 status changes,
3 of 7, PASS; "0 admission trial line(s) appended, 7 already ledgered".

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| nav (L 1.1474; step (1)) | 180 / 1,536 | 59.4 | 586 | 0 | `babe4001a3485f237bbfe29a2d1eed73f0c7a75e508302f7610ab3eb22b926d4` | `build-equity/mega-nav-v8-r5-advq.1-t.05-d.1-fixed-obdelta-x.05-loc-L1.247`; recipe rule `aim-partial-v5+neutral-price-risk-v1+adv-hold-0.1`, `adv_hold_q` .1 |

Receipt: completed, `clean in the code pathspec`, source `8ed541a6`, NAV exe `5497c89d` (v8-12).

**Gross match (step 1 at the parent's L 1.1474; mechanics only):** G **0.9839319270** vs G_parent .9859903463:
|diff| **.00206** <= .005 -> **the cell stands at L 1.1474** (no `-gm` spec, no correction; the step-(1) run is the
cell; calibration runs 0).

**Mechanics (S2, read before any return): PASS.** All-rows gross .9839 in [.90, 1.05] (post-ramp .9897); net +.0038
(<= .02); tau mean .02380 <= .20, p95 .02819 <= .30 (1,004 sessions; summary flags true); max gross 1.116, max |net|
.029; gross at score_begin .9370; by year .961 / 1.000 / .982 / .992; 1,006 CSV rows 2020-01-02..2023-12-29, 1,005
return rows; accounting 6.7e-14 / 3.7e-16. **ADV cap active** (`construction.adv_hold`, identical in all 5 scenarios,
desired-weight units): 1,004 decisions; clipped names mean 69.3, max 131, total 69,543; clipped mass mean .0164, max
.0375; unplaced 0; residual breach (one pass, reported) in 864 decisions, 4,350 name-decisions, names max 18, mass mean
5.9e-5, max .00052, excess max 9.3e-5.

**Criterion reading, fixed before any return of the cell is read.** Registered text (plan R-5, task-R-5-brief,
task-CELLS-brief:58): (a) S2 net Sharpe at 4x NAV (capacity curve, `capacity_curve.csv` row multiple 4, `net_sharpe`)
strictly higher than R-2's; (b) S2 net Sharpe at 1x "not lower by more than one paired SE": dSR >= -SE, SE = the
paired Memmel SE of the statistics of record (the pitch config labels it "within one Memmel SE of the parent's"; that
label is the report tool's, not the registration; the registered words are one-sided); (c) S3 =
`modeled-1bn-terminal-adverse-v1+swap-fin-v1` net Sharpe (summary, 1x) not lower than R-2's (>=). Acceptance (rule 5):
dSR > 0 AND mechanics AND (a) AND (b) AND (c). R-5 at 4x reads its cap at the initial NAV (E-15 / PM4-5, disclosed).
(Commit `6ca6ebfe`, before the summ phase.)

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| monitor | - | - | - | - | - | done (R-2's; not re-run) |
| summ | 180 / 1,536 | 19.6 | 588 | 0 | `766660bfafdd36b9806070cdd878f3a29ef260a55f250e2c44df97c5565b07f7` | `cycle-v8-r5-adv-hold/summ.json` `2c1f042a`, `pbo.json` `9bc752ad`, `cycle_verdict.json` `ebb0ff6c` |
| one-sided p (PM5-23) | 180 / 1,536 | 1.6 | 552 | 0 | `f09865af84b6cf12fdf82981e4deea729e0fa766998bcc8b71278bc106db3df8` | `v8-cells-r5-bundle.json` `5dcca351` (no `--ledger`) |

Files: `summary.json` `39594c15`, S2 daily `94d59265`, `capacity_curve.csv` `ec25a1af`, `recipe.json` `364dab25`.

**Statistics of record** (S2): net Sharpe **R-5 +1.2019** vs R-2 +1.2559. Paired (1,005 sessions, 4,999 valid
resamples): **dSR -.0540**; rho .9990; **Memmel SE .0226** (t -2.39); CBB 95% [-.1036, -.0053]; LW SE .0246, 95%
[-.1047, -.0034]; **bootstrap p one-sided .9768, two-sided .0396** (the bundle reproduces the cycle's dSR, SE, CI and
two-sided p exactly). DSR (N 45): ledger DSR .9822 (V[SR] from 8 cells on research-window-v2); legacy (37) .7416;
effective-N .7107; PSR vs 0 .9902; PBO .2769.

**Criterion (registered; reading fixed above):**
- (a) S2 net Sharpe at 4x NAV (capacity curve): **1.1268 vs R-2 1.1785: not higher -> FAIL.**
- (b) dSR -.0540 vs -SE -.0226: **lower by more than one paired SE -> FAIL** (t -2.39).
- (c) S3 (`modeled-1bn-terminal-adverse-v1+swap-fin-v1`) net Sharpe **.1692 vs R-2 .2163: lower -> FAIL.**

**Verdict (rule 5): dSR -.054 <= 0 AND criterion FAIL (a, b, c) -> NOT ACCEPTED.** Ledger line trial `b4e31e283cd5d53b`
(cell = the R-5 NAV dir, s2_net_sr 1.20193, origin prior, window research-window-v2, prev `a8d43fc9` = R-4's head);
**N after 45** (ledger 53 lines: 45 construction + 7 admission + 1 protocol; file `3807cc00`; head `e57a2802`). Matches
the brief. Not retried. **Parent stays R-2.**

Returns (S2, annual): net 4.34% (CAGR 4.37%) vs R-2 4.54%; gross of cost 5.59% (R-2 5.81%); trade cost .72% (.74%),
borrow .33%, long financing .20%; vol 3.61%; max drawdown 3.27%; gross Sharpe 1.549 (R-2 1.610). Every scenario's net
Sharpe is lower than R-2's (linear-6bps 1.311 / 1.371; flat-300 .983 / 1.031; engine-tiers 1.130 / 1.185).

| year | rows | net return | net Sharpe | vol | tau | cost bps |
|---|---|---|---|---|---|---|
| 2020 | 252 | -.0133 | -.380 | .0338 | .0262 | 14.32 |
| 2021 | 252 | +.0956 | +2.533 | .0363 | .0226 | 11.31 |
| 2022 | 251 | +.0781 | +1.809 | .0422 | .0234 | 11.88 |
| 2023 | 250 | +.0175 | +.580 | .0309 | .0230 | 11.18 |

Capacity (report only; R-2 beside): net Sharpe .5x 1.232 (1.289), 1x 1.202 (1.256), 2x 1.173 (1.223), **4x 1.127
(1.178)**, 8x 1.079 (1.100); cost bps per traded dollar 10.43 / 12.17 / 14.44 / 16.79 / 18.67 (R-2 10.67 / 12.47 / 14.66
/ 16.87 / 18.67); capped fill share .0009 / .0024 / .0118 / .0704 / .2008 (R-2 .0010 / .0034 / .0190 / .0802 / .2083).

**Appendix A:** `TRAIN construction cells 45; admission trials this sprint 7 (plus 8 re-screens); window
research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024); history
reads 0; 2025+ never read.` Defects: none. Consequence: R-6 runs on R-2 (E-26: R-5 not accepted, so spo-v3's aim takes
no `--adv-hold-q`; R-4 not accepted, no `--hold-band`).

v9 note (from a result, not a spec): the cap lowers cost per traded dollar only 2.4% at 1x and .5% at 4x and capped
fills 30% at 1x / 12% at 4x, while gross-of-cost Sharpe falls .06: the pro-rata redistribution moves 1.6% of desired
gross from names held large against their ADV to the rest of their side, and that costs more signal than it saves in
cost; at $1-4bn the cost model is not bound by holdings / ADV at Q .10.

Commit `da70dacb` (R-5 verdict, log and ledger line).

### 3a. Risk model atx-risk-v1.1 on the 4-year role (R-6 step 3; not a trial)

Inputs: the parent R-2's role and fields (role lo3 `e1c67101`, fields v10 lo3 `a4a060ae`; the fields R-6's NAV reads).
Caps: 180 s / 1,536 MiB (W0-c: not an IC phase; the v7 risk stores ran under the same caps). No `--book-weights` (a
risk model, not a diagnostic); `--emit-exposures all` (the spo rules refuse a store without the exposure files). Run on
the clean tree at `da70dacb` through the bounded runner (data runs only through it; the cycle has no risk phase):

```
run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 --output build-equity/v8-risk-lo3-v10-run
  --bind build-equity/bin/atx-equity-strategy-risk.exe --bind build-equity/train-2020-2023-lo3/manifest.json
  --bind build-equity/train-2020-2023-lo3-fields-v10/manifest.json -- build-equity/bin/atx-equity-strategy-risk.exe risk
  --role build-equity/train-2020-2023-lo3/manifest.json --role-sha256 e1c67101...395f4
  --fields build-equity/train-2020-2023-lo3-fields-v10/manifest.json --fields-sha256 a4a060ae...70809
  --output build-equity/v8-risk-lo3-v10 --emit-exposures all
```

| step | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| risk (exe v8-12 `8967952c`) | 180 / 1,536 | 31.2 | 612 | 0 | `5e88ccaee5eea6ff17cde45999217a5ffa73fff4dd60b6f4f4785b766d20a219` | `build-equity/v8-risk-lo3-v10/manifest.json` **`862515d92623be37fbd4b126c8f0350a20135e7977f1cbf66c6c33f1644ecd5a`** (the new pin) |

Receipt: completed, `clean in the code pathspec`, source `da70dacb`. Manifest: schema `atx.risk-model/v1`, model
**atx-risk-v1.1**, status complete, role pin `e1c67101` (the NAV's `--role-sha256`; the store refuses another role),
fields `a4a060ae`, seal begin 2024-01-01, role last session 2023-12-29 (`1703808000000000000`); geometry 1,405 dates x
5,922 instruments, 62 factors, 11 styles; first forecast 2019-09-03, 1,341 fitted / 1,090 forecast sessions, mean
R^2 .247; `unavailable` []; invariant refusals 0 (no specific variance clamped; max daily D .361 < bound 1.0); 0
style-dates dropped; forecast sessions with an unforecast exposed factor 0. Bias harness: factor ok (51 series, b mean
1.002, dropped 0), random ok (64), book not asked. Files: factor_covariance `9d9c62a2`, specific_variance `a7792a53`,
style_exposures `8ae6c185`, factor_structural `f63df1a4`, industry_slot `9b7cca17`, diagnostics `ca94f5c9` (1,405
rows). Check: every model file is byte-identical to B0c's diagnostic store `b0c-risk` (fields v9 lo3; same role) --
fields v10 adds 7 fields the risk descriptors do not read; only `bias.csv` / `bias_summary.json` differ (that store had
`--book-weights`). Not a trial, no return exists in a risk model.

### Cell R-6 (r6-spo-v3 on lib-v80; spo-v3, S_prior 20; L 1.1474)

**N before: 45; this cell makes 46 (<= 51).** Brief row (task-CELLS-brief.md:59): "cost per traded dollar not higher
AND tripwire clear AND mean `aim_correlation_traded_after` >= .9; the run voids itself on primary-book limits_unmet > 0"
(E-14, E-14a, E-26, E-31, E-31a, E-37). Parent = R-2 (`lib-v80.json`); R-4 and R-5 were not accepted, so (E-26) the
aim carries neither `--hold-band` nor `--adv-hold-q`.

**Identities:** not run here (as R-4 / R-5): flag off = integration 5 part C identity 1, spo-v2 side files and pin =
identity 7 (PM3-7), both PASS and re-run on the v8-12 executables by PM4-3 (1, 4, 7, 8); no executable changed since.

**Spec.** `scripts/specs/v8/r6-spo-v3.json` (template): `"parent": null -> "lib-v80.json"`; the two fills
`--risk-model build-equity/v8-risk-lo3-v10`, `--risk-model-sha256 862515d9...4ecd5a` (step 3a); `lock` (dry) 0, `lock
--write` 0 (reference_cell R-2 `083a56da`, reference_admission `f613fe92`, reference_combined `bbbf6f2b`,
reference_weights `03213345`), dry after 0. File `87518e08a9124fd5efa48fa750cbd689e8ea383d62037e634f3cb06315f87f19`,
**spec digest (template chain) `689c826d521e04384df0c9a7acfab955b4a733c8fcbb24ffdec8ca7248580e7d`**.
`test_research_spec.py` with the filled, locked file: 51 passed (meta-test deselected). Plan exit 0: 8 pins `[locked,
verified]`; nav pending: R-2's argv with `--rule spo-v3` and `--spo-alpha implied-aim --risk-model
build-equity/v8-risk-lo3-v10 --risk-model-sha256 862515d9... --spo-books primary` at `--aim-leverage 1.1474`
(`--capacity-curve`, `--warm-start-sessions 60`, `--label-role` the parent's); no `--spo-iters` / `--spo-tol` (refused,
E-31a); summ `--dsr-n 46`. The nav output name keeps the template's "L1.247" text (cosmetic; the run is at 1.1474).

**Pre-return reads, fixed before the run (E-31, E-31a, E-14a; template description):** (1) exit code: 3 = void (no NAV,
no return file) -> E-31a: a blind fix and a re-run, no new trial; a fix needing an executable change is a PM5-21 stop;
(2) `v7_extras.json` `spo_v3.tripwire.status` == "clear", `limits_unmet_primary.count` == 0; (3) `summary.json`
`v7.spo_v3_books.<primary>`: `unconverged`, `limits_unmet`, `mean_iterations`, decisions; the E-14 / E-14a value
`aim_correlation_traded_after.mean` (criterion, >= .9); (4) mechanics keys (mech.py) and gross matching (PM6-6). Never
`stdout.log` (it prints net Sharpe per book). Criterion of record (task-CELLS-brief:59; pitch config): (a)
`cost_bps_traded` (summ, S2 cost per traded dollar) <= R-2's 12.466; (b) tripwire status "clear"; (c) primary
`limits_unmet` 0 (E-31a: else void); (d) `aim_correlation_traded_after.mean` >= .9.
Commit `e770fc36` (spec, this text).

**Calibration step (1): r6-spo-v3.json's NAV at the parent L 1.1474** (`run --stop-after nav`; no free-memory refusal):

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| nav (L 1.1474, calibration) | 180 / 1,536 | 110.9 | 593 | 0 | `796a1133be00b00f339583f6868f3e29e3a6753700424aaa89a6170662535bea` | `build-equity/mega-nav-v8-r6-spo-v3-L1.247` (mechanics only) |

Receipt: completed, `clean in the code pathspec`, source `e770fc36`, NAV exe `5497c89d`. Exit 0: **not void** (E-31a).
Files: the two primary-scenario books only (S1 linear-6bps, S2; `--spo-books primary`), `spo_diagnostics.csv`,
`v7_extras.json`, capacity. Read (by key name, scratch `spo_mech.py`; no return, cost or Sharpe printed):
- **Tripwire: status `clear`**; `limits_unmet_primary.count` **0** (first_session none); capped_specific_decisions 0;
  gross_bound_breaches 0 (bound 2 L = 2.2948); max planned gross 1.0926.
- **Convergence (primary S2 book): 1,004 decisions, unconverged 0, limits_unmet 0, mean iterations 81.8**; max primal
  residual 3.7e-11, max dual 1.0e-9 (tol 1e-9; iters 2,000; both registered, not on the argv). S1 book: 1,004 / 0 / 0 /
  81.6.
- Calibration: gamma 835.51 = 20 / sigma_aim .023937 (aim gross 1.0879, 1,888 names, first session 2020-01-02).
- E-14 / E-14a value (S2 book): aim_correlation_traded_after mean **.9592** (min .912, n 1,003) >= .9; planned
  aim_correlation .9660; tracking error mean .0051; trade-limit share mean .036.
- Mechanics (mech.py): **G 1.0288508598** vs G_parent .9859903463: |diff| **.0429 > .005** -> correction. (Net +.0006,
  tau mean .0627 / p95 .0963, max gross 1.120, max |net| .022, score_begin .940, accounting 6.2e-14 / 3.7e-16: inside
  the limits.)
- **L' = 1.1474 x .9859903463 / 1.0288508598 = 1.09960 -> 1.0996** (>= 1, inside the exe's [1, 2]).

**Matched spec** `scripts/specs/v8/r6-spo-v3-gm.json` (new template: r6-spo-v3.json's change with the same filled
store + `nav.leverage` "1.0996" + `nav.output` `build-equity/mega-nav-v8-r6-spo-v3-L1.0996`; parent lib-v80.json;
`lock` / `lock --write` / dry lock 0 / 0 / 0, the same four derived pins). File
`2a669f35bd6a2ece959dd96ac38001818fcd927ee16dbf5db3f26c23e9451e97`, **spec digest (template chain)
`cd49cb4768472eeefd3c24c0dea8e8801c4509b19c039b4b7ad00ab9ecd87979`**. Plan exit 0: 8 pins `[locked, verified]`; the nav
argv equals the calibration's except `--aim-leverage 1.0996` and `--output`; summ `--dsr-n 46`. Tests only, so the
suite knows the hand-written spec (as R-1-gm / R-3-gm): `test_research_spec.py` `NULL_PINS`, `FILLS` (the store),
`EXPECTED_CHANGES` (r6's | nav.leverage), the nav delta and the spo-rule name check take `r6-spo-v3-gm.json`;
`scripts/tests` **194 passed, 3 skipped, 0 failed** (109.0 s). The calibration run is not a trial and is not ledgered;
its outputs stay unread beyond the keys above. Commit `3f1f63dc`.

**The cell: r6-spo-v3-gm.json's NAV at L 1.0996** (`run --stop-after nav`):

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| nav (L 1.0996, the cell) | 180 / 1,536 | 124.8 | 593 | 0 | `4b07df9ae8f97f26e9ff6d4ce585a60c34f004a464f1336c3d305da62bcc3535` | `build-equity/mega-nav-v8-r6-spo-v3-L1.0996` |

Receipt: completed, `clean in the code pathspec`, source `3f1f63dc`, NAV exe `5497c89d`. Exit 0: **not void**. Read
before any return (same keys):
- **Tripwire: status `clear`; `limits_unmet_primary.count` 0**; capped_specific 0; gross_bound_breaches 0 (bound
  2.1992); max planned gross 1.0461.
- **Convergence (S2 book): 1,004 decisions, unconverged 0, limits_unmet 0, mean iterations 81.8**; max primal 3.7e-11,
  dual 1.0e-9. S1: 1,004 / 0 / 0 / 81.6. gamma 871.83 = 20 / sigma_aim .022940 (aim gross 1.0425, 1,888 names).
- **E-14 / E-14a (S2): aim_correlation_traded_after mean .9601** (min .914, n 1,003) >= .9; planned .9669; TE mean
  .0048; trade-limit share .034.
- **Gross match: G 0.9862950362 vs G_parent .9859903463: |diff| .00030 <= .005** (one correction).
- **Mechanics (S2): PASS.** All-rows gross .9863 in [.90, 1.05] (post-ramp .9914); net +.0006; tau mean .06304 <= .20,
  p95 .09721 <= .30 (1,004 sessions; summary flags true); max gross 1.074, max |net| .021; score_begin .901; by year
  .980 / .993 / .998 / .974; 1,006 CSV rows 2020-01-02..2023-12-29, 1,005 return rows; accounting 4.7e-14 / 3.5e-16.
  (Executed turnover is 2.6 x R-2's .0239: not a criterion of this cell; reported.)
Commit `2d39d532`, then the cycle resumed (monitor = R-2's, done):

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| summ | 180 / 1,536 | 22.1 | 557 | 0 | `9e8d1f4d974a0035042b8c3ff6d3aefc19adb286f25dd943c305ac97098d3ee5` | `cycle-v8-r6-spo-v3-gm/summ.json` `8397ec3a`, `pbo.json` `71426cd0`, `cycle_verdict.json` `60dff79e` |
| one-sided p (PM5-23) | 180 / 1,536 | 0.8 | 506 | 0 | `0513a7466b2abde0d1ed0bb6bd9f4c2c7462df71641ae83fdeeeae2e56ea3553` | `v8-cells-r6-bundle.json` `74929849` (no `--ledger`) |

Files: `summary.json` `4c8770cf`, S2 daily `fce81ca2`, `capacity_curve.csv` `c4408d80`, `recipe.json` `07439591`,
`v7_extras.json` `00e1f58f`, `spo_diagnostics.csv` `f85a8b78`.

**Statistics of record** (S2): net Sharpe **R-6 +.7606** vs R-2 +1.2559. Paired (1,005 sessions, 4,999 valid
resamples): **dSR -.4954**; rho .936; **Memmel SE .1798** (t -2.76); CBB 95% [-.896, -.116]; LW SE .1988, 95% [-.905,
-.086]; **bootstrap p one-sided .9956, two-sided .0210** (the bundle reproduces the cycle exactly). DSR (N 46): ledger
DSR .7964 (V[SR] from 9 cells on research-window-v2); legacy .4134; effective-N .3930; PSR vs 0 .9339; PBO .2637.

**Criterion (registered; reading fixed above):**
- (a) cost per traded dollar (`cost_bps_traded`, S2): **18.791 bps vs R-2 12.466: higher -> FAIL.**
- (b) tripwire status **clear -> pass**. (c) primary `limits_unmet` **0 -> pass** (no void).
- (d) `aim_correlation_traded_after.mean` **.9601 >= .9 -> pass.**

**Verdict (rule 5): dSR -.495 <= 0 AND criterion (a) FAIL -> NOT ACCEPTED.** Ledger line trial `81fe22855e797aa1` (cell =
the L1.0996 NAV dir, s2_net_sr .76057, prev `e57a2802` = R-5's head); **N after 46** (ledger 54 lines: 46 construction +
7 admission + 1 protocol; file `0c1df8bd`; head `986d8b58`). Matches the brief. The L 1.1474 calibration run of
r6-spo-v3.json stays unledgered (PM6-6). Not retried. **Parent stays R-2.**

Returns (S2, annual): net 3.13% (CAGR 3.10%) vs R-2 4.54%; **gross of cost 6.63%** (R-2 5.81%); trade cost **2.96%**
(R-2 .74%), borrow .33%, long financing .20%; vol 4.12%; max drawdown 5.12%; gross Sharpe 1.609 (R-2 1.610).

| year | rows | net return | net Sharpe | vol | tau | cost bps |
|---|---|---|---|---|---|---|
| 2020 | 252 | -.0265 | -.526 | .0489 | .0787 | 22.27 |
| 2021 | 252 | +.0826 | +2.083 | .0384 | .0596 | 18.74 |
| 2022 | 251 | +.0593 | +1.349 | .0436 | .0597 | 17.28 |
| 2023 | 250 | +.0116 | +.382 | .0318 | .0541 | 15.94 |

Capacity (report only, E-37; R-2 beside): net Sharpe .5x .773 (1.289), 1x .761 (1.256), 2x .783 (1.223), **4x .807
(1.178)**, 8x .814 (1.100); cost bps per traded dollar 17.09 / 18.79 / 20.00 / 20.70 / 21.00 (R-2 10.67 / 12.47 / 14.66
/ 16.87 / 18.67); capped fill share .0088 / .0253 / .0591 / .1120 / .1815.

**Appendix A:** `TRAIN construction cells 46; admission trials this sprint 7 (plus 8 re-screens); window
research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024); history
reads 0; 2025+ never read.` Defects: none (no void, no refusal). Consequences (E-38, E-45, E-37): R-6 rejected ->
R-10, R-11 and R-12 are undefined; R-9 (three report-only theta cells) is defined (the parent R-2's rule
aim-partial-v5 reads theta). Remaining: R-7 (47), R-8 (48), R-9a-c (49-51): N 51 exactly.

v9 note (from a result, not a spec): at S_prior 20 the tracker holds the aim (corr .96) but trades 2.6 x the
aim-partial book (tau .063 vs .024; holding period 15.7 sessions) and at 1.5 x the cost per dollar; gross-of-cost
Sharpe is unchanged (1.609 / 1.610), so the whole loss is trading cost: gamma = 20 / sigma_aim (about 870) dominates the
cost / H term; v7's aim-partial theta .05 is the cheaper tracker of the same aim.

### Hidden-data record (batch 2e)

- Inputs opened by the tools: role lo3, the lo3-dlret label role, fields v10 lo3 (the risk verb's descriptors and every
  NAV), the R-2 combined signal and weights, the new risk store `v8-risk-lo3-v10` (seal begin 2024-01-01, role last
  session 2023-12-29), the ledger and the ledgered NAV dirs of the summ grid, each cell's own outputs. No atx-db stage
  was opened.
- Consoles scanned for 2024-2029 date tokens (R-5 plan / runs; risk run; R-6 and R-6-gm plans / runs; risk stdout):
  none except the runner's own start stamp 2026-10-02. Last NAV session in every run 2023-12-29
  (`last_session_ns` 1703808000000000000). **Nothing dated 2024-01-01 or later was opened.** No `stdout.log` of a NAV
  run was opened. **The R-6 calibration run (L 1.1474) was read for tripwire, convergence and mechanics keys only.**

### Open items (batch 2e)

- `scripts/tests` 194 passed / 3 skipped / 0 failed with `r6-spo-v3-gm.json` registered (tests only, commit
  `3f1f63dc`); `atx-impl/strategies` 163 passed (step 1).
- Two template outputs carry a cosmetic "L1.247" in their names though they ran at 1.1474 (`r5-adv-hold.json`, the R-6
  calibration `r6-spo-v3.json`).
- The new risk store `build-equity/v8-risk-lo3-v10` (manifest `862515d9`, 481 MiB) is the one R-8 fills (`r8.json`
  `--risk-model`); keep it.
- **Current accepted parent: R-2** (`scripts/specs/v8/lib-v80.json`), L 1.1474, G .9859903463; S2 net Sharpe +1.2559,
  net annual return 4.54%, net Sharpe at 4x 1.178, tau_gmv_mean .02393. **N 46**; admission trials 7 of 15 (plus 8
  re-screens); history reads 0. Disk 124,736,080 KiB free (119.0 GiB).

## cells batch 2f: fields v11, R-7, R-8 (stopped before the run), R-9 (not started) (2026-10-02)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `9364ffca` (clean; code head `1cc4c6c9`;
executables v8-12 Debug: IC `ab7e2cbd`, NAV / targets `5497c89d`, risk `8967952c`; nothing built; PM5-21 holds: the
only commits are registry rows, the v8.1 library files and spec, and this log). Read: integrator-rules, progress "PM
session 6" to the end (PM6-1..12, PM7-1..15, batch 2e lines), task-CELLS-brief, task-R-7-brief, task-R-8-R-9-brief,
task-R-8-report, rulings E-36, E-37, E-38, E-40, E-43, E-45, PM4-8, R7-a..c, this log's batches 2b-2e, plan R-7..R-9,
section 12.1, library-v8-draft sections 3-8, task-A2-report root sequence. Same scratch readers as batches 2c-2e
(`mech.py`: mechanics keys only; `cellstats.py` / `crit.py`: return side, after mechanics passed; `bundle.sh`: the PM5-23
bundle; `k1plan.py`: the PM6-9 K1 route). Parent = R-2 (`scripts/specs/v8/lib-v80.json`, L 1.1474, all-rows gross
.9859903463). N 46 at the start; admission trials 7 of 15. Disk 127,724,511,232 B free (118.9 GiB); RAM 5,497 MiB free.

### 1. Fields v11 on lo3 (R-7 "fields v11 first"; Ruling R7-c)

**Preconditions:** tree clean at `9364ffca`; `atx-engine/tools` unchanged since v10's source `0d553a34` (`git diff`
empty); builder `b44cff42`, `research_fields_sec.py` `27034019`, `research_fields_holdings.py` `edfd1967`,
`research_fields_price.py` `fc5e4b5a`, `research_fields_v8.py` `d90bb44d` (= v10's bindings); the builder knows the three
F-3 fields (F-B `k8_item402_63`, F-C `gscore7_lowbm`, F-D `eps_consist_4y`).

**Command** = v10's recorded argv (receipt `train-2020-2023-lo3-fields-v10-run`, verbatim; checked option by option by
script) with F-3's v11 delta (task-F-3-report "Argv deltas"), nothing else: `--output
build-equity/train-2020-2023-lo3-fields-v11`; `--fields <the 70 v10 names in v10's argv order>,k8_item402_63,gscore7_lowbm,eps_consist_4y`;
`--reuse build-equity/train-2020-2023-lo3-fields-v10 --reuse-sha256 a4a060ae...70809 --reuse-hardlink`; builder
`--max-rss-mib 2048 --max-seconds 580`. Runner `--seconds 600 --max-rss-mib 2560 --min-free-mib 512`, binds the five tool
modules, the role manifest and the prior's manifest.

| step | receipt dir | source | outcome / exit | s | peak MiB | argv digest | receipt.json SHA-256 | output manifest SHA-256 |
|---|---|---|---|---|---|---|---|---|
| fields v11 lo3 | `train-2020-2023-lo3-fields-v11-run` | `9364ffca` | completed / 0 | **24.5** | **382** | `8d15b5e9afea4e2449ce9ec10a0855918809c44b2f9aefd55dec1c2052504096` | `656a28796350e5fece4b880d64523ba25ce5f9a12024ee809ecf2b9c8e2eecaf` | **`5826c02a27f6ad292597f017e075116d1ac18a1713aa8dda1be323217e895f69`** |

Receipt: `clean in the code pathspec`, dirty outside none, min system free 5,454 MiB. Manifest (metadata only): status
complete, `seal.exclusive_end` 2024-01-01, role `e1c67101`, **73 rows**, 84 source paths, none 2024-named;
`code_sha256_lf` `74df97f9` (= v10's); 618,633 B. `reuse`: from v10 lo3 `a4a060ae`, mode hardlink.

**Counts: reused 70, computed 3 = the expected 70 / 3 (PM3-5a, F-3).** Computed: `k8_item402_63` (`27444f8b`,
sec-k8-item402-63-v1), `gscore7_lowbm` (`47cd57b9`, mohanram-g7-lowbm3-sic2-v1), `eps_consist_4y` (`1c86e185`,
alwathainani-eps-consistency-16q-v1), each "absent from the prior manifest". The 70 v10 payloads: entry sha256 = v10
entry sha256 (70 / 70), hardlinks of the v10 files (same inode, 70 / 70); all 73 payloads re-hash to their pins. Order:
registry order (the three new names at rows 56, 64, 65); the 70 v10 names keep their relative order (the spec list is
the manifest's, as add-alpha writes it). Disk: the v11 dir is 4.6 GiB on disk (hardlinks; new payloads only are new
bytes).

### Cell R-7 (library v8.1 on R-2; fields v11; L 1.1414 after one correction): NOT ACCEPTED (dSR < 0), N 47

**N before: 46; this cell makes 47 (<= 51).** Brief row (task-CELLS-brief.md:60): "turnover not higher (marginal IC gates
nothing); fields v11 first" (E-36, R7-a..c). Parent = the last accepted cell = R-2 (`lib-v80.json`); R-3..R-6 were not
accepted. The template `r7-lib-v81.json` stays unrun (parent null): its own `requires` says R-7 runs as add-alpha's
`lib-v81.json` (one cell, one trial line), as R-2 ran as `lib-v80.json`.

**Wave preparation (A2 root sequence "R-7: as R-2"; draft section 6 E6-E10):**
1. Registry hand edits (commit **`7402d7b2`**): E6 field rows `ceq_iss_5y`, `coskew_60m` (origin fields_v10), E8
   `k8_item402_63`, `gscore7_lowbm`, `eps_consist_4y` (fields_v11); clock = the v11 manifest row's clock verbatim, basis
   = its definition + "(formula id X; producer atx-engine/tools/<module>, fields-vN)", the existing rows' form (only
   name / clock / definition / formula_id / producer read). E7 theme `filing_events` (draft text). **Ruling R7-a:
   `house_budget.max_roster` 56 -> 64** (57 members > 56; generate_library refuses a roster over the cap). Registry
   `570f022a` -> `29780f04`. **FINDING (tests, not fixed):** `atx-impl/strategies/test_generate_library.py::
   test_v71_library_byte_identical` now fails at its recipe assert: the committed v7.1 slim recipe records
   `generation.house_budget` (max_roster 56), so a regeneration differs in that one value (byte 1330, "6" for "5"); the
   v7.1 IC library still regenerates byte for byte (`787c802e`, the asserts before pass). Draft section 5 named this
   consequence of the raise; no pinned file changed (the v7.1 / v8.0 recipes on disk are untouched). A test premise for
   the PM (tests only).
2. **K1 by the PM6-9 route** (`scratchpad/k1plan.py`: the exe's `--plan-only --max-memory-mib 2560` on the exact library
   bytes add-alpha builds; add-alpha checks `plan.library_sha256`): 5 plans, each sha = the library sha; committed under
   `r7-plans/` (SHA-256): comp_eq_iss_5y `47aecfb2...`, coskew_60m `660736be...`, gscore_lowbm `97b160c0...`,
   nonreliance_402 `f601cf4b...`, earn_consistency `80c50ff3...` (full: `sha256sum r7-plans/*.json`).
3. The 5 calls (draft section 7 v8.1 strings verbatim, checked by script; draft order; `--parent v80 --name v81
   --parent-spec scripts/specs/v8/lib-v80.json --fields build-equity/train-2020-2023-lo3-fields-v11`): every call exit 0.
4. **Counts (equal the draft):** roster 57 (52 + 5; the 52 v8.0 candidate rows byte-identical in the v8.1 library);
   recipe trials: admission_trials 5, new_candidates 5, unchanged 52; families 11 (`filing_events` new); no new budget
   exception. **K1 rows of the 5 = draft section 4** (DSL sha16 / bars / slots / extra fields): comp_eq_iss_5y `269ddd24`
   20/3/2, coskew_60m `de6da255` 20/3/1, gscore_lowbm `c41ec275` 20/3/2, nonreliance_402 `63a5d143` 0/3/1,
   earn_consistency `943f4187` 20/3/1 (exe node counts 8 / 7 / 5 / 5 / 4 against the mirror's 7 / 6 / 5 / 4 / -: no
   node budget exists; reported). The 52 parent rows equal R-2's last K1 plan (52 / 52). Plan: 57 candidates, max slots
   8, lookback 272.
5. Registration appended to `v8-prereg.md` ("Library v8.1 (cell R-7)": draft sha `326ad240` unchanged since R-2,
   R7-a..c, E-36; pins: library **`c6bf150e...1c56`**, slim recipe `d72589a1...bea3`, `libraries/v81.json` `8bb0aa07`,
   stub `f2d53652`, registry `3697342a`, fields v11 `5826c02a`); commit **`8d2eac5e`** (registry, library, recipe,
   definition, stub, `scripts/specs/v8/lib-v81.json`, prereg, plans).

**Spec** `scripts/specs/v8/lib-v81.json` (generated and locked by add-alpha; 12 pins; file / spec digest
`281c7b70516d3c395f1bfd0a82b482b8fe7af330c6913f6199dcf9c6afa65c46`): parent R-2 (reference_cell `083a56da`,
reference_admission `f613fe92`, reference_combined `bbbf6f2b`, reference_weights `03213345`, reference_daily `7cfe21c4`,
reference_orientations `312bbf4e`, reference_daily_ic `10f22711`); baseline library v80 `68ce8539`, baseline fields v10
`a4a060ae`; fields as built v11 `5826c02a` (73 rows); fit `ew-theme-std-v1` (inherited); gate p1-v81 (the 5, require
any, sign_agrees); **marginal on the full pool (pool reference_combined, themes reference_weights: PM6-8 held for R-2
only; R-7 only adds members)**; ref on v11 fields; nav.leverage 1.1474 (the parent's L = step (1)). `plan` exit 0, every
pin `[locked, verified]`.

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output / result |
|---|---|---|---|---|---|---|
| fields | - | - | - | - | - | done (as built v11, 73 rows == the spec list) |
| u | 300 / 2,560 | 9.9 | 927 | 0 | `f92d04eb3697de4cd861bc19849569934ca9dc84bd4638bee99d954b8fa34513` | `mega-v8-b0b-train-u-v81-1` |
| u-compare | internal | - | - | PASS | - | parent-orientations IDENTICAL (52 objects; b adds 5); parent-train-daily-ic IDENTICAL (152,152 rows of 52 keys byte for byte; b adds 14,630 rows of 5 keys) |
| fit | 180 / 1,536 | 2.9 | 422 | 0 | `06928ca4eb802b5efce69b5233b8ca8110c08df82f574f826a77346316f029cc` | weights `3249f989` (ew-theme-std-v1; 46 of 57 weighted; nonreliance_402 at the cap 1/(2T) = .04545, T 11) |
| card | 300 / 2,560 | 20.5 | 1,311 | 0 | `3d838f9935a7013cbeb17a7f93e7b782459e07baee23eb8281435aab39a0fd00` | `mega-cards-v8-r1-std-v81` |
| marginal (full pool, K6, report only) | 180 / 1,536 | 170.3 | 252 | 0 | `ac47ec275828c0dc530dec540ec66d31e03694a396a94ea9e2aed2ee61811670` | `...-u-v81-marginal-pool/marginal_ic.json` (170 s of the 180 s cap: see open items) |
| gate p1-v81 | internal | - | - | PASS | - | 5 admission lines ledgered (chained, before the read-out) |
| ref (identity, L 1.1474) | 180 / 1,536 | 50.8 | 586 | 0 | `57b9b72626972e4fd78b877f35b689a295d1b7750a731ec8cc0fa5d9fa053b64` | **ref-s2-daily IDENTICAL bit for bit** to R-2's S2 daily (`7cfe21c4`, 940,625 B): fields v11 reproduces the parent |
| w | 300 / 3,072 | 38.8 | 1,372 | 0 | `39ca1ff5df2efcd20e9138baacbcc72c70ce9efe95af0ee2ac8936c160f25171` | `mega-v8-r1w-train-std-v81-1` |
| nav step (1) (L 1.1474, calibration) | 180 / 1,536 | 45.4 | 586 | 0 | `fd87afecb4715978acea590132755788c86ec7c6d65d029da058fb72bb26574a` | mechanics only |

Sources: screen `8d2eac5e` (`run --screen`), then `run --stop-after nav` at `8d2eac5e`; exes IC `ab7e2cbd`, NAV
`5497c89d`; every receipt `clean in the code pathspec`.

**Admission (gate p1-v81, v4-prior-v1; require any): PASS, 4 of 5 admitted with the prior sign** -- comp_eq_iss_5y (HAC t
-.26; max |rho| .848 with net_payout), coskew_60m (-.03; .612 with comp_eq_iss_5y), nonreliance_402 (-.49; .194),
earn_consistency (-.01; .241); gscore_lowbm status admitted with runner sign 0 against prior +1 (sign_agrees false, not
counted by the gate; 374 train days). Reference members vs R-2's admission: 0 status changes. **Admission trials this
sprint: 12 of 15 (plus 8 re-screens).** K6 (full pool, report only, gates nothing; E-36): marginal IC21 / HAC t
comp_eq_iss_5y +.0022 / .42, coskew_60m +.0170 / 1.63, gscore_lowbm none (the verb wrote no value; not investigated: it gates nothing), nonreliance_402 +.0011 / .29,
earn_consistency -.0003 / -.11.

**Gross match, step (1) at the parent's L 1.1474 (mechanics only, mech.py):** G **0.9912148513** vs G_parent (R-2)
**0.9859903463**: |diff| **.00522 > .005** -> one correction. (The other step-(1) mechanics keys were inside the limits:
net +.0043, tau .02318 / .02772, max gross 1.125.) **L' = 1.1474 x 0.9859903463 / 0.9912148513 = 1.141352 -> 1.1414**
(>= 1).

**Matched spec** `scripts/specs/v8/lib-v81-gm.json` (new plain spec, hand-written: lib-v81.json with `name` v81-gm, a
description sentence, `nav.leverage` "1.1414", `nav.output` `build-equity/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1414-v81`;
every pin unchanged; `lock` / `lock --write` / dry 0 / 0 / 0). A template could not serve: a template's derived
reference pins come from its parent, and the paired reference must stay R-2, not the add-alpha spec's calibration run.
Tests only: `test_research_spec.py` `NULL_PINS["lib-v81-gm.json"]` (a plain spec's lock-filled pins, as a base spec's),
so the suite knows the hand-written spec (as R-1-gm / R-3-gm / R-6-gm); 54 passed (meta-test deselected). Commit
**`714770f9`**. **Refusal and spec-only fix (PM5-21):** the first `run --stop-after nav` HARD-STOPPED at `ref` before any
phase ran (exit 3): the done ref NAV (lib-v81's, at 1.1474) was made by an argv differing from this spec's ref argv only by
`--aim-leverage` (the ref step inherits `nav.leverage`). Fix: `ref.leverage` "1.1474" (a designed ref key: the reference
construction is R-2 at its own L), relocked; commit **`395071fb`**; file / spec digest
**`9b5eeec43cecab029977074ea600006ac56361f06d50ecb7e5199c4985a4df93`**. Planned NAV argv = step (1)'s except `--output` and
`--aim-leverage 1.1474 -> 1.1414` (diffed by script). The step-(1) run of lib-v81.json is not a trial and is never
resumed past nav (no monitor, no summ, no ledger line).

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| ref / u / fit / card / marginal / gate / w | - | - | - | - | - | lib-v81.json's (done; ref binding argv `905d7d17` = its own; ref-s2-daily IDENTICAL again; gate: "0 appended, 5 already ledgered") |
| nav (L 1.1414, the cell) | 180 / 1,536 | 45.2 | 586 | 0 | `485cb3481fec1125d4fc959cf160dc3f87a60fb1f3a2568722c617664fe5a77d` | `summary.json` `3205d868`, S2 daily `11743abe`, `capacity_curve.csv` `e118a926`, `recipe.json` `fc07b10a` |
| monitor | 180 / 1,536 | 1.3 | 117 | 0 | - | `mega-monitor-v8-r1-std-v81/monitor.json` `add5f6bb`: M2 alarm (alarm 5, warn 16, ok 25), M1 / M3 / M4 n/a |
| summ | 180 / 1,536 | 22.6 | 560 | 0 | - | `cycle-v81-gm/summ.json` `2eec1982`, `pbo.json` `39773d3a`, `cycle_verdict.json` `cf2e1f57` |
| one-sided p (PM5-23) | 180 / 1,536 | 0.8 | 409 | 0 | `409b3479ac11dda3969d3e34c9f065a6bfe24186d90767d0ede4ec686cf1f968` | `v8-cells-r7-bundle.json` `6175d767` (no `--ledger`) |

NAV receipt: completed, `clean in the code pathspec`, source `395071fb`, exe `5497c89d`; `construction.v5.aim_leverage`
1.1414.

**Gross match: G 0.9859967596 vs G_parent 0.9859903463: |diff| .0000064 <= .005** (one correction).
**Mechanics (S2, read before any return): PASS.** All-rows gross .9860 in [.90, 1.05] (post-ramp .9917); net +.0043
(<= .02); tau mean .02317 <= .20, p95 .02772 <= .30 (1,004 sessions; summary flags true); max gross 1.119, max |net| .030;
gross at score_begin .9431; by year .962 / 1.002 / .985 / .995; 1,006 CSV rows 2020-01-02..2023-12-29, 1,005 return rows;
accounting 1.1e-13 / 4.1e-16 (tol 1e-9).

**Criterion (book turnover not higher; executed tau_gmv_mean, as R-2):** .023173 vs R-2 .023929 -> **PASS**.

**Statistics of record** (S2 = `modeled-1bn-stale5-v1+swap-fin-v1`): net Sharpe **R-7 +1.1898** vs R-2 +1.2559. Paired
(studentized CBB, block 21, seed 20260929, 4,999 resamples, 4,999 valid; 1,005 sessions): **dSR -.0662**; rho .979;
**Memmel SE .1037** (t -.64); CBB 95% [-.269, +.125]; LW SE .1005, 95% [-.273, +.141]; **bootstrap p one-sided .7318,
two-sided .524** (the bundle reproduces the cycle's dSR, SE, CI and two-sided p exactly). DSR (N 47, verdict
`--dsr-ledger`): ledger DSR .9556 (V[SR] from 10 cells on research-window-v2); legacy (37) .7327; effective-N .7166 (N_eff
4); PSR vs 0 .9904; PBO .3172.

**Verdict (prereg rule 5): dSR -.066 <= 0 -> NOT ACCEPTED** (mechanics PASS, criterion PASS; the wave is judged whole).
Recorded by the tooling: `cycle-v81-gm/cycle_verdict.json` (spec `9b5eeec4`) and the ledger line (trial
`9a3608a7e6a0df3f`, cell = the L1.1414 NAV dir, s2_net_sr 1.18979, origin prior, window research-window-v2, prev
`2635608f`). **N after: 47** (ledger 60 lines = 47 construction + 12 admission + 1 protocol; file `e6613e7b`; head
`a11bcbb1`). Matches the brief. Not retried. **Parent stays R-2.**

Returns (S2, annual): net 4.18% (ann mean; CAGR 4.20%) vs R-2 4.54%; gross of cost 5.44% (R-2 5.81%); trade cost .72%,
borrow .34%, long financing .20%; vol 3.51%; max drawdown 3.09%; gross Sharpe 1.549 (R-2 1.610). Every scenario lower than
R-2's (linear-6bps 1.301 / 1.371; S3 terminal-adverse .138 / .216; flat-300 .960 / 1.031; engine-tiers 1.114 / 1.185).

| year | rows | net return | net Sharpe | vol | tau | cost bps |
|---|---|---|---|---|---|---|
| 2020 | 252 | -.0140 | -.446 | .0307 | .0258 | 14.79 |
| 2021 | 252 | +.0924 | +2.385 | .0374 | .0218 | 11.56 |
| 2022 | 251 | +.0723 | +1.721 | .0412 | .0227 | 12.19 |
| 2023 | 250 | +.0202 | +.695 | .0297 | .0224 | 11.62 |

Capacity (report only; R-2 beside): net Sharpe .5x 1.221 (1.289), 1x 1.190 (1.256), 2x 1.149 (1.223), **4x 1.103
(1.178)**, 8x 1.038 (1.100); cost bps per traded dollar 10.74 / 12.55 / 14.75 / 16.93 / 18.75 (R-2 10.67 / 12.47 / 14.66 /
16.87 / 18.67).

**Appendix A:** `TRAIN construction cells 47; admission trials this sprint 12 (plus 8 re-screens); window
research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024); history
reads 0; 2025+ never read.` Defects: none (one cycle refusal fixed in the spec before any phase ran).

v9 note (from a result, not a spec): the five new members' admission HAC t are all between -.49 and -.01 and the wave
lowers gross-of-cost Sharpe by .06 at an unchanged cost per dollar; a new eleventh theme takes 1/11 of every theme's
share (the one-member `filing_events` theme sits at its cap .045 on a flag that marks few names).

### Tests (end of step 1)

`ATX_EQUITY_BIN` / `ATX_EQUITY_TARGETS_EXE` absolute (v8-12), vcpkg bins on PATH:
- `scripts/tests` (whole): **195 passed, 3 skipped, 0 failed** (130.5 s; skips: the three RESEARCH_CYCLE_LIVE_ROOT tests).
- `atx-impl/strategies` (whole): **162 passed, 1 failed** -- `test_v71_library_byte_identical` (the R7-a finding above).

### Cell R-8 (ex-ante risk target, `r8.json`): STOPPED BEFORE THE RUN (how PM6-6 applies is not settled)

Nothing was edited, locked, planned or run for R-8; `r8.json` is unchanged (parent null). N 47; no trial burned.

**What is settled.** Parent = R-2 (`lib-v80.json`, L 1.1474; R-7 was not accepted). Store: `build-equity/v8-risk-lo3-v10`
(manifest `862515d9`; batch 2e: 0 invariant refusals, no specific variance clamped, max daily D .361 < 1.0; R-6's spo-v3
runs on it reported capped_specific_decisions 0), so the brief's "check the risk store's capped_specific count first" is
met from records. Acceptance: **Ruling E-43 settles it** -- prereg rule 5 governs: paired S2 net dSR > 0 against the
parent AND mechanics AND realised volatility of the S2 net series inside [.04, .06] (= [.8, 1.2] x sigma_star .05) in
each TRAIN year 2020-2023; the plan's "dSR not lower by more than one SE" (repeated in the dispatch) is the plan's
expectation, not the rule (pitch config R-8 check: `ann_vol` between .04 and .06 from the nav_summ year table).

**What is not settled: PM6-6 on a cell whose purpose is to set leverage.** PM6-6 says "every construction cell runs at the
L that puts its all-rows gross within +/- .005 of its parent's"; R-8's registered rule (E-40, task-R-8-report, `r8.json`
description) is `L_t = clip(S / (b sigma_hat_t), .8 L, 1.25 L)`, S .05, b 1.15, cadence 21, with L = the parent's
`--aim-leverage`. No brief, ruling or report says which governs (grep of PM6-6 across the sprint directory: no R-8
mention; PM7-3 / PM7-11 restate the gross limit for the X leverage cell only). The two readings, both fixed here before
any R-8 number exists:

- **Reading A (PM6-6 applies as written):** step (1) = `r8.json` on R-2 at L 1.1474, mechanics only; if |G - .98599| >
  .005, L' = L x G_parent / G in a `-gm` spec (`nav.leverage`, which here is the scaler's base L: the clip band
  [.8 L', 1.25 L'] and the leverage before the first estimate), at most two corrections; then rule 5 and the volatility
  band on the matched run. Mechanical consequence (from the registered rule only): wherever the clip does not bind, L_t =
  S / (b sigma_hat_t) does not depend on L, so gross does not move with L and the linear correction need not converge
  (PM6-6: a cell that misses +/- .005 after two corrections fails mechanics and is ledgered rejected); where matching
  succeeds it does so through the clip, so the book's mean leverage is set by the parent's gross, not by S, and the cell
  tests the timing of leverage at the parent's dollar gross.
- **Reading B (PM6-6 does not apply; R-8 is the registered leverage rule):** one run at the parent's L 1.1474 as the
  scaler's base; gross is what the rule gives. Open inside B: whether the mechanics gross limit [.90, 1.05] binds
  unchanged (PM6-6: "the mechanics limits are unchanged") or is restated for this cell before the run, as PM7-11 did for
  the X leverage cell. The registered clip allows aim leverage .918 to 1.434 (.8 and 1.25 x 1.1474); at the parent's
  gross / L ratio (.98599 / 1.1474 = .859) that is all-rows gross from about .79 to 1.23 -- outside [.90, 1.05] at both
  ends of the band. Under B the paired dSR compares books of different dollar gross (PM6-6's reason for matching).

Per the dispatch, R-8 stops here for the PM's reading (and, under B, the gross limit). Nothing else of R-8 depends on a
choice: once ruled, the sequence is `r8.json` parent -> `lib-v80.json`, fills `--risk-model build-equity/v8-risk-lo3-v10
--risk-model-sha256 862515d92623be37fbd4b126c8f0350a20135e7977f1cbf66c6c33f1644ecd5a`, lock, commit, plan, `run
--stop-after nav`; mechanics and the `risk_target` blocks (decisions_before_first_estimate, estimates about scored
sessions / 21, clip counts, priced_share) before any return; then summ, bundle, year table.

### Cells R-9a-c: NOT STARTED

They follow R-8 (order; their parent is "the final construction" = the last accepted cell after R-8, and N 49-51 follow
N 48). Open choices found in the brief while preparing (for the PM's next dispatch; nothing was written or run):
1. **Machinery:** the dispatch says R-9 needs R-6's spo-v3 machinery; Ruling E-37 defines R-9 only on a parent whose rule
   reads theta (aim-partial-v5) and calls theta meaningless in the tracker. R-6 was rejected, so the parent's rule is
   aim-partial-v5 and spo-v3 is not involved (`--trade-fraction` = theta).
2. **"With NAV 4x":** the NAV executable has no NAV-size flag (the $1bn is fixed by the scenarios); 4x exists only as the
   capacity curve's x4 row (E-29). Reading: each cell is the parent's NAV argv with `--trade-fraction` .03 / .04 / .05 and
   its frontier point is the x4 row; its $1bn row is a different book from the deployed one, which "does not change"
   (report only).
3. **theta .05 is the parent's theta:** R-9c reproduces the parent's NAV byte for byte (its x4 row = R-2's 1.178); run and
   counted (N 51, as the brief's "3 cells") or read from the parent (adds 0).
4. **PM6-6 on theta .03 / .04:** a lower theta lowers gross (the book lags the aim more); matched L or the parent's L.
5. **Spec form:** no R-9 template exists (a spec-only template is allowed); the cycle has no report-only mode: with
   `"verdict": true` it writes an accepted / rejected verdict, which the report tool refuses on a report-only cell
   (PM4-8); with `"verdict": false` research_cycle still passes summ its `--ledger` (the construction line, N + 1) and writes no
   verdict.

### Hidden-data record (batch 2f)

- Inputs opened by the tools: role lo3, the lo3-dlret label role, fields v10 / v11 lo3, the lo3 candidate cache, R-2's
  u / fit / w / NAV outputs, the atx-db stages and fundamental events v3 through the fields builder's sealed readers
  (read only; nothing under `atx-db/` written), the vendor TickerHistory3 file (reader-side seal), the ledger and the
  ledgered NAV dirs of the summ grid, the cell's own outputs.
- Consoles scanned for 2024-2029 date tokens (fields v11 runner console and logs, add-alpha consoles, R-7 plan / screen /
  three run consoles): none except the runner's own start stamp 2026-10-02. Last NAV session in every run 2023-12-29
  (`last_session_ns` 1703808000000000000). Fields v11 manifest: seal 2024-01-01, no 2024-named source.
- Disclosure: while finding the manifest keys for the registry rows I printed the full v10 manifest rows of
  grp_ff12f49, ceq_iss_5y and coskew_60m once, which include their `coverage` blocks (finite-cell counts and value
  quantiles per year 2018-2023; no return, IC or Sharpe); nothing was used from them.
- **Nothing dated 2024-01-01 or later was opened.** No `stdout.log` of a NAV run was opened. **The R-7 step-(1) run of
  lib-v81.json (L 1.1474) was read for mechanics keys only.**

### Open items (batch 2f)

- **PM ruling needed (R-8):** reading A or B of PM6-6 above (and, under B, the gross limit). R-9 waits on R-8 and on
  choices 2-5.
- `atx-impl/strategies` `test_v71_library_byte_identical` fails since R7-a's roster cap 64 (the v7.1 slim recipe records
  `house_budget`); tests-only fix or a ruling.
- R-7's marginal phase took 170.3 s of its 180 s cap (full pool, 11 regressors; add-alpha gives `marginal` the runner's
  default caps, while W0-c allows the IC phases 300 s / 2,560 MiB). A later wave on a larger pool may time out there.
- `r7-lib-v81.json` stays unrun (parent null), as `r2-lib-v80.json`.
- **Current accepted parent: R-2** (`scripts/specs/v8/lib-v80.json`), L 1.1474, G .9859903463; S2 net Sharpe +1.2559,
  net annual return 4.54%, net Sharpe at 4x 1.178, tau_gmv_mean .02393. **N 47**; admission trials 12 of 15 (plus 8
  re-screens; the 3 left lapse with R-12); history reads 0. Disk 123,940,056 KiB free (118.2 GiB).

## cells batch 2g: tests (PM7-22), R-8, R-9a/b, W0-4 re-runs, V8-F (2026-10-02)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `98d50d7c` (clean; code head `1cc4c6c9`;
executables v8-12 Debug: IC `ab7e2cbd`, NAV / targets `5497c89d`; nothing built; PM5-21 holds). Read: integrator-rules,
progress "PM session 7" to the end (PM7-1..22, batch 2e / 2f lines), this log's batch 2f from R-8 to its end,
task-R-8-R-9-brief, task-W0-4-brief, task-V8-F-brief, rulings E-2, E-34, E-43, PM5-18, PM5-22, PM5-23. Same scratch
readers as batches 2c-2f (`mech.py`: mechanics keys only; `cellstats.py` / `crit.py`: return side, after mechanics
passed; `bundle.sh`: the PM5-23 bundle). Parent = R-2 (`scripts/specs/v8/lib-v80.json`, L 1.1474, all-rows gross
.9859903463). N 47 at the start. Disk 126,778,818,560 B free (118.1 GiB); RAM 4,936 MiB free.

### 1. Tests (Ruling PM7-22; tests only)

`atx-impl/strategies/test_generate_library.py::test_v71_library_byte_identical` (+16 / -4): the v7.1 IC library is still
compared byte for byte (sha `787c802e` and the committed file); the slim recipe is compared except its roster-cap field:
the test asserts the regenerated cap is at least the committed one (64 >= 56), then regenerates in a copy of the tree
(the file's own `tree()` / `edit_registry()` helpers) with `house_budget.max_roster` set to the cap the committed recipe
records (56) and requires the library and the recipe byte for byte there, and both `--check` calls (`--strategies` the
copy; plain and `--plan-json`) return 0. No pinned file, registry or generator changed. Python `-p no:cacheprovider`,
`ATX_EQUITY_BIN` / `ATX_EQUITY_TARGETS_EXE` absolute (v8-12), vcpkg bins on PATH:

| suite | before | after |
|---|---|---|
| `atx-impl/strategies/test_generate_library.py` | 1 failed (byte 1330, "6" for "5") | 9 passed |
| `scripts/tests` (whole) | 195 passed, 3 skipped (batch 2f) | **195 passed, 3 skipped, 0 failed** (107.2 s) |
| `atx-impl/strategies` (whole) | 162 passed, 1 failed (batch 2f) | **163 passed, 0 failed** (23.7 s) |

The 3 skips are the RESEARCH_CYCLE_LIVE_ROOT tests (as every batch).

Commit `caefcd6a`.

### Cell R-8 (r8.json on lib-v80; risk target S .05, b 1.15, cadence 21; base L 1.1474; not gross matched, PM7-20)

**N before: 47; this cell makes 48 (<= 51).** Brief row (task-CELLS-brief.md:61): "realised volatility inside [.8, 1.2] x
5% in each TRAIN year; `r8.json`; check the risk store's capped_specific count first" (E-40, E-43). Parent = the last
accepted cell = R-2 (`lib-v80.json`); R-3..R-7 were not accepted. Store check met from records (batch 2f: `v8-risk-lo3-v10`,
manifest `862515d9`, 0 invariant refusals, no specific variance clamped; R-6's runs on it capped_specific_decisions 0).

**Identity (template "before any return"):** `atx-impl-strategy-target-tests.exe` (v8-12, 2026-10-01 18:55, the build of the
NAV exe `5497c89d`) `--gtest_filter=RiskTarget.*:BookRiskTarget.*:SpoPin.*:SpoV3.*:SpoHook.*:NavV7Hook.*`: **56 / 56
passed** (includes `RiskTarget.FlagAbsentKeepsThePinnedBenchDigests` and `SpoV3.V1AndV2DigestsUnchanged`). The parent's NAV
argv re-run on v8-12 reproduced R-2's S2 daily bit for bit in R-7's `ref` phase (batch 2f). No executable changed since.

**Spec.** `scripts/specs/v8/r8.json` (template): `"parent": null -> "lib-v80.json"`; the two designed fills `--risk-model
build-equity/v8-risk-lo3-v10`, `--risk-model-sha256 862515d9...4ecd5a` (manifest re-hashed now: equal); nothing else. `lock`
(dry) 0, `lock --write` 0 (reference_cell R-2 `083a56da`, reference_admission `f613fe92`, reference_combined `bbbf6f2b`,
reference_weights `03213345`), dry after 0. File `04c67351d5b9b87807598214b25e0455595c552706ac9275bd4fdeb14c47a167`, **spec
digest (template chain) `fe60420efa51a579511545f0cf83a71ab02d00fc513f2d9fd4f1142b0287ea93`**. `test_research_spec.py` with
the filled, locked file: 55 passed. Plan exit 0: 8 pins `[locked, verified]`; fields, u, fit, card, marginal, w, monitor =
R-2's (done); nav pending. **Nav argv diffed by script against R-2's NAV receipt: equal except `--output` and the five
inserted flags `--risk-target .05 --risk-target-bias 1.15 --risk-target-cadence 21 --risk-model build-equity/v8-risk-lo3-v10
--risk-model-sha256 862515d9...`** (`--aim-leverage 1.1474`, `--trade-fraction .05`, `--capacity-curve`,
`--warm-start-sessions 60`, `--label-role` the parent's). Summ `--dsr-n 48 --ledger build-equity/trials.jsonl`. The nav
output name keeps the template's "L1.247" text (cosmetic; the base L is 1.1474).

**Reading, fixed before the run (PM7-20, E-43; no number of R-8 exists):**
1. One run, base L 1.1474 (the parent's), not gross matched; no correction step; not retried in any form.
2. Before any return, in this order: (a) the `risk_target` blocks (`summary.json` `risk_target.books.<S2>`:
   decisions, estimates against scored sessions / 21, decisions / estimates at each clip, decisions_before_first_estimate,
   L_t and multiplier n / mean / min / max; `risk_target.csv` `priced_share`); (b) mechanics (S2; mech.py, keys only):
   **all-rows gross inside [.784, 1.237]** (PM7-20: [.8, 1.25] x G_parent .98599, widened by .005), |net| <= .02, tau mean
   <= .20, p95 <= .30, accounting. A mechanics value outside its limit stops the batch before any return (dispatch).
3. Acceptance (E-43, rule 5): paired S2 net dSR against R-2 > 0 AND mechanics AND realised volatility of the S2 net daily
   returns (`nav_summ --protocol v8` year table, column `vol`) inside [.04, .06] (closed) in each of 2020, 2021, 2022, 2023.
   The plan's "dSR >= -1 SE" is expectation, printed, gates nothing. The verdict line prints both books' all-rows gross.

Commit `59a27af8` (spec, this text). Then `run --stop-after nav` (no free-memory refusal):

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| fields / u / fit / card / marginal / gate / w | - | - | - | - | - | R-2's (done; gate p1-v80 re-read PASS, 0 status changes, "0 appended, 7 already ledgered") |
| nav (base L 1.1474, the cell) | 180 / 1,536 | 47.4 | 589 | 0 | `bde30cac5840c91637b8e31d8e608dd1de12d01c7e03cd2dcf747ef9acbbf19e` | `build-equity/mega-nav-v8-r8-rt.05-b1.15-c21-L1.247`; recipe rule `aim-partial-v5+neutral-price-risk-v1+risk-target-0.05` |

Receipt: completed, `clean in the code pathspec`, source `59a27af8`, NAV exe `5497c89d`.

**Pre-return reads (in the fixed order; scratch `rtblock.py` and `mech.py`, keys only):**
- **`risk_target` block** (summary.json; rule risk-target-v1, S .05, b 1.15, cadence 21, clip [.8, 1.25], 252): S2 book
  decisions 1,004, **decisions_before_first_estimate 0**, **estimates 48** (1,004 / 21 = 47.8), base L 1.1474; decisions at
  clip hi **374**, at clip lo **21**, unclipped 609; estimates at clip hi 18, at clip lo 1 (of 48); L_t mean **1.3082**
  (min .9179 = .8 L, max 1.4343 = 1.25 L), multiplier mean 1.1402; sigma_hat over the 48 estimates mean .0320 (min .0172,
  max .0587). `risk_target.csv` (5,020 rows = 1,004 x 5 books): S2 **priced_share mean .9986** (min .9956, max 1.0000);
  48 updated rows; first decision 2020-01-02, last 2023-12-27 (ns stamps). Estimates at the clip by year: hi 2020 6
  (Jan-Mar, Sep-Nov), 2021 4 (Apr, May, Sep, Oct), 2022 0, 2023 8 (May-Dec); lo once (Apr 2020, sigma_hat .0587).
- **Mechanics (S2): PASS.** All-rows gross **1.1244** in [.784, 1.237] (PM7-20; post-ramp 1.1244); net +.0050 (<= .02);
  tau mean .02428 <= .20, p95 .03061 <= .30 (1,004 sessions; summary flags true); max gross 1.368, max |net| .030; gross at
  score_begin 1.171; by year 1.083 / 1.182 / 1.040 / 1.192; 1,006 CSV rows 2020-01-02..2023-12-29, 1,005 return rows;
  accounting 7.8e-14 / 4.3e-16 (tol 1e-9); `construction.v5.aim_leverage` 1.1474 (base), theta .05.

Commit `9df90d63`, then the cycle resumed (monitor = R-2's, done):

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| summ | 180 / 1,536 | 19.5 | 610 | 0 | `7f475126a3af03341f52ef1ddbc8385c2ab99533167014e591e3da7f408e90b1` | `cycle-v8-r8-risk-target/summ.json` `61612188`, `pbo.json` `f7729e5c`, `cycle_verdict.json` `e4f65be7` |
| one-sided p (PM5-23) | 180 / 1,536 | 0.8 | 461 | 0 | `9815d5be2739b55fae89de83b676c0c6e642de84524c10e2e1e508c5e4e0ec4e` | `v8-cells-r8-bundle.json` `608b51ec` (no `--ledger`) |

**Statistics of record** (S2 = `modeled-1bn-stale5-v1+swap-fin-v1`): net Sharpe **R-8 +1.2449** vs R-2 +1.2559. Paired
(studentized CBB, block 21, seed 20260929, 4,999 resamples; 1,005 sessions): **dSR -.0111**; rho .9958; **Memmel SE
.0460** (t -.24); CBB 95% [-.1149, +.0905]; LW SE .0539, 95% [-.1247, +.1026]; **bootstrap p one-sided .5826, two-sided
.8428** (the bundle reproduces the cycle's dSR, SE, CI and two-sided p exactly). DSR (N 48, verdict `--dsr-ledger`): ledger
DSR .9635 (V[SR] 7.77e-05 from 11 cells on research-window-v2); legacy (37) .7609; effective-N .7466 (N_eff 4); PSR vs 0
.9919; PBO .3169. **All-rows gross: R-8 1.1244, R-2 .9860** (PM7-20: the paired dSR compares books of different dollar
gross).

**Criterion (E-43):** realised volatility of the S2 net daily returns (summ year table `ann_vol`): 2020 **.0401**, 2021
**.0433**, 2022 **.0441**, 2023 **.0365** -> inside [.04, .06] in 2020-2022, **below .04 in 2023 -> FAIL**. (Plan
expectation, gates nothing: dSR -.0111 >= -SE -.0460, met.)

**Verdict (rule 5, E-43): dSR -.011 <= 0 AND volatility band FAIL (2023) -> NOT ACCEPTED** (mechanics PASS). Recorded by
the tooling: `cycle-v8-r8-risk-target/cycle_verdict.json` (spec `fe60420e`; ledger head `3b00257b`, 61 lines) and the
ledger line (trial `c360f66b7af19aae`, cell = the R-8 NAV dir, s2_net_sr 1.24487, origin prior, window research-window-v2,
prev `a11bcbb1` = R-7's head). **N after: 48** (ledger 61 lines = 48 construction + 12 admission + 1 protocol; file
`2dcecbc3`). Matches the brief. Not retried, not re-parameterised (E-43). **Parent stays R-2.**

Returns (S2, annual): net **5.13%** (CAGR 5.17%) vs R-2 4.54%; gross of cost 6.61% (R-2 5.81%); trade cost .87% (.74%),
borrow .38%, long financing .23%; vol 4.12% (R-2 3.62%); max drawdown 3.26%; gross Sharpe 1.606 (R-2 1.610). Scenarios
(cell / R-2): linear-6bps 1.358 / 1.371; S3 terminal-adverse .239 / .216; flat-300 1.024 / 1.031; engine-tiers 1.172 /
1.185.

| year | rows | net return | net Sharpe | vol | tau | cost bps |
|---|---|---|---|---|---|---|
| 2020 | 252 | -.0124 | -.292 | .0401 | .0270 | 14.82 |
| 2021 | 252 | +.1153 | +2.541 | .0433 | .0231 | 11.84 |
| 2022 | 251 | +.0846 | +1.869 | .0441 | .0237 | 12.33 |
| 2023 | 250 | +.0235 | +.660 | .0365 | .0234 | 12.00 |

Capacity (report only; R-2 beside): net Sharpe .5x 1.277 (1.289), 1x 1.245 (1.256), 2x 1.211 (1.223), **4x 1.166
(1.178)**, 8x 1.095 (1.100); cost bps per traded dollar 10.87 / 12.73 / 14.93 / 17.05 / 18.77 (R-2 10.67 / 12.47 / 14.66 /
16.87 / 18.67); capped share .0011 / .0048 / .0263 / .0997 / .2366 (R-2 .0010 / .0034 / .0190 / .0802 / .2083).

**Appendix A:** `TRAIN construction cells 48; admission trials this sprint 12 (plus 8 re-screens); window
research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024); history
reads 0; 2025+ never read.` Defects: none (no refusal, no fix).

v9 note (from a result, not a spec): the rule is mostly a leverage raise: L_t averages 1.308 (1.14 x base), at the upper
clip on 37% of decisions; the ex-ante vol of the gross-1 book is low in calm spells (sigma_hat .017-.025), so S / (b
sigma_hat) exceeds 1.25 L there and the clip, not the target, sets the leverage; 2023's realised vol .0365 stays under .04
with the clip binding from May. Net return rises .59 pt for .011 of Sharpe -- the PM7-3 leverage question, not alpha.

Commit `dbc70008` (R-8 verdict, log and ledger line).

### Cells R-9a / R-9b (theta .03 / .04 on R-2; report only; PM7-21); R-9c read from the parent

**Parent = the last accepted cell after R-8 = R-2** (`lib-v80.json`, rule aim-partial-v5, theta .05, L 1.1474). E-37 / PM7-21
(1): theta = `--trade-fraction`; spo-v3 not involved. N before 48; R-9a makes 49, R-9b 50; R-9c (theta .05 = the parent)
adds 0 and is read from R-2's own files (PM7-21 (3)).

**Specs (spec-only templates, PM7-21 (5)):** `scripts/specs/v8/r9a.json` / `r9b.json`, new: parent `lib-v80.json`, nominal
parent `base-b0c.json`; change = `nav.output` (`build-equity/mega-nav-v8-r9{a,b}-t.0{3,4}-d.1-fixed-obdelta-x.05-loc-L1.1474-v80`),
`"verdict": false`, nav flag `--trade-fraction .03` / `.04`; nothing else. `lock` / `lock --write` / dry 0 / 0 / 0 each (the
four derived pins of R-2: `083a56da`, `f613fe92`, `bbbf6f2b`, `03213345`). Files r9a `81e2f0b4...7a97`, r9b
`c320310f...1614`; **spec digests (template chain) r9a `4674033ff37aea4f507110a71fe12285884f09a6d7a8f6fc79ec03bea225c6c8`,
r9b `5ef8593b83cd2a57d9e488100dca3fb8139eb398f072264dff92d10bd9d3bc3d`**. Tests only (so the suite knows the two specs):
`test_research_spec.py` `NULL_PINS` (CHILD_NULLS), `EXPECTED_CHANGES` {nav.output, nav.flags, verdict}, the nav delta (the
parent's flags with the `--trade-fraction` value replaced); 59 passed. Plans exit 0: 8 pins `[locked, verified]` each;
fields, u, fit, card, marginal, w, monitor = R-2's (done); **nav argv diffed by script against R-2's NAV receipt: equal
except `--output` and the `--trade-fraction` value (index 21: .05 -> .03 / .04)**; summ `--protocol v8 --origin prior
--ledger build-equity/trials.jsonl --ledger-kind construction` (`--dsr-n 49` at plan time), no `--json`, no `--dsr-ledger`
(no verdict).

**Reading, fixed before either run:** (1) one run each at L 1.1474, not gross matched, never retried; (2) before any
return: mechanics (S2, mech.py) with the registered limits unchanged (all-rows gross [.90, 1.05], |net| <= .02, tau mean
<= .20, p95 <= .30, accounting); a value outside a limit stops the batch before any return (dispatch stop condition;
PM7-21 restates no limit for these cells); (3) report: S2 ($1bn) net Sharpe, the capacity curve's x4 row (net Sharpe,
cost per traded dollar), all-rows gross, turnover, the bundle against R-2 (PM5-23; information only), the year table; no
verdict, nothing accepted or rejected, the deployed book stays R-2; (4) each cell's ledger line is written by its summ
(construction, N 49 / N 50).

Commit `6ac2fda5` (specs, tests, this text). **R-9a** `run --stop-after nav`:

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| nav (theta .03, L 1.1474) | 180 / 1,536 | 43.8 | 586 | 0 | `43c9ad1a4d59306856c2c705fd6f6f7661719b562256686856c8a91714f340c2` | `build-equity/mega-nav-v8-r9a-t.03-d.1-fixed-obdelta-x.05-loc-L1.1474-v80`; rule `aim-partial-v5+neutral-price-risk-v1`, theta .03 |

Receipt: completed, `clean in the code pathspec`, source `6ac2fda5`. **Mechanics (S2): PASS.** All-rows gross **.9350** in
[.90, 1.05] (post-ramp .9432); net +.0032; tau mean .01796, p95 .02124 (flags true); max gross 1.070, max |net| .030;
score_begin .812; by year .903 / .960 / .929 / .948; 1,006 CSV rows, 1,005 return rows; accounting 6.8e-14 / 4.3e-16.
Commit `104a750c`; cycle resumed: summ 19.7 s, 613 MiB, exit 0 (receipt `b7f6683e1ffab0abf2d3e3f77387543b73bda4bff01a5445f77abd928e698ff1`;
no summ.json: no verdict, `cycle_verdict.json` without scoring blocks); bundle (PM5-23) 0.8 s, 475 MiB, exit 0 (receipt
`1523993543673c9a45698d14fe3ca0697a407352a1cadd4108fd4cafae8c6fa8`, `v8-cells-r9a-bundle.json` `06c6efda`). Ledger line
trial `a9d199bf9616015b` (cell = the R-9a NAV dir, s2_net_sr 1.24329, window research-window-v2, prev `3b00257b` = R-8's
head); **N 49** (62 lines = 49 + 12 + 1; file `874e986c`).

**R-9a (theta .03), report only:** S2 ($1bn) net Sharpe **1.2433** (R-2 1.2559); **x4 row net Sharpe 1.1966** (R-2 1.1784),
cost per traded dollar 15.89 bps (16.87), capped share .044 (.080); all-rows gross .9350 (R-2 .9860); tau .01796 (.02393);
net annual return 4.27% (4.54%), gross of cost 5.27%, trade cost .49%; vol 3.44%; max drawdown 3.35%. Against R-2
(information only): dSR -.0127, Memmel SE .0573, p one-sided .563, two-sided .844. Capacity .5x 1.265, 1x 1.243, 2x 1.230,
4x 1.197, 8x 1.121; cost bps 10.15 / 11.73 / 13.71 / 15.89 / 17.84.

| year | rows | net return | net Sharpe | vol | tau | cost bps |
|---|---|---|---|---|---|---|
| 2020 | 252 | -.0133 | -.435 | .0298 | .0203 | 13.87 |
| 2021 | 252 | +.0934 | +2.580 | .0348 | .0169 | 10.77 |
| 2022 | 251 | +.0755 | +1.782 | .0415 | .0176 | 11.39 |
| 2023 | 250 | +.0196 | +.669 | .0298 | .0171 | 10.84 |

Commit `37b4dede`. **R-9b** `run --stop-after nav`:

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| nav (theta .04, L 1.1474) | 180 / 1,536 | 45.9 | 586 | 0 | `f0ba01fe6da9779ded7c2aa8a92326205a08f5410a61a18981804f976c3fa25a` | `build-equity/mega-nav-v8-r9b-t.04-d.1-fixed-obdelta-x.05-loc-L1.1474-v80`; theta .04 |

Receipt: completed, `clean in the code pathspec`, source `37b4dede`. **Mechanics (S2): PASS.** All-rows gross **.9650** in
[.90, 1.05] (post-ramp .9716); net +.0037; tau mean .02110, p95 .02496 (flags true); max gross 1.099, max |net| .030;
score_begin .892; by year .940 / .984 / .961 / .975; 1,006 CSV rows, 1,005 return rows; accounting 8.0e-14 / 4.2e-16.
Commit `83cc948d`; cycle resumed: summ 23.4 s, 613 MiB, exit 0 (receipt
`2ef388ddc3e80277004714bf82cf634cf95664fbf970bf9b10b62f0f414cc564`); bundle 0.8 s, 506 MiB, exit 0 (receipt
`c89dd5775c664481019df784c5c4691581673a5bbe2b6d2f69de7f17b17c7531`, `v8-cells-r9b-bundle.json` `bb9e35bf`). Ledger line
trial `2dd5442da54cca97` (cell = the R-9b NAV dir, s2_net_sr 1.25275, prev `30c6f7b7` = R-9a's head); **N 50** (63 lines =
50 + 12 + 1; file `9cb99c5f`).

**R-9b (theta .04), report only:** S2 ($1bn) net Sharpe **1.2528**; **x4 row net Sharpe 1.1864**, cost per traded dollar
16.45 bps, capped share .062; all-rows gross .9650; tau .02110; net annual return 4.44%, gross of cost 5.58%, trade cost
.62%; vol 3.54%; max drawdown 3.30%. Against R-2 (information only): dSR -.0032, Memmel SE .0256, p one-sided .534,
two-sided .912. Capacity .5x 1.281, 1x 1.253, 2x 1.228, 4x 1.186, 8x 1.113; cost bps 10.42 / 12.12 / 14.24 / 16.45 / 18.32.

| year | rows | net return | net Sharpe | vol | tau | cost bps |
|---|---|---|---|---|---|---|
| 2020 | 252 | -.0120 | -.362 | .0320 | .0235 | 14.37 |
| 2021 | 252 | +.0961 | +2.583 | .0358 | .0200 | 11.13 |
| 2022 | 251 | +.0783 | +1.823 | .0420 | .0207 | 11.77 |
| 2023 | 250 | +.0195 | +.653 | .0305 | .0203 | 11.20 |

**R-9c (theta .05) = R-2, read from its files (adds 0):** $1bn 1.2559; x4 1.1785, cost 16.87 bps, capped share .080; gross
.9860; tau .02393; net annual return 4.54%.

**Frontier (report only; the deployed book stays R-2):**

| theta | cell | N | all-rows gross | tau | $1bn net Sharpe | x4 net Sharpe | x4 cost bps | net annual return ($1bn) |
|---|---|---|---|---|---|---|---|---|
| .03 | R-9a | 49 | .9350 | .01796 | 1.2433 | **1.1966** | 15.89 | 4.27% |
| .04 | R-9b | 50 | .9650 | .02110 | 1.2528 | **1.1864** | 16.45 | 4.44% |
| .05 | R-9c = R-2 | (47) | .9860 | .02393 | 1.2559 | **1.1785** | 16.87 | 4.54% |

Appendix A (after R-9b): `TRAIN construction cells 50; admission trials this sprint 12 (plus 8 re-screens); window
research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024); history
reads 0; 2025+ never read.` The registered cell program ends here at N 50 (PM7-21 (3)). Defects: none.

v9 note (from a result, not a spec): at 4x a slower tracker gains .018 of net Sharpe per .01 of theta while the $1bn book
loses .003-.010 and gross falls with theta (the book is not gross matched); at matched gross the x4 ranking is not
measured.

Commit `96bdba9d` (R-9 report, ledger lines).

### 4. W0-4 re-runs of the ledgered v7 cells on the 4-year roles (PM5-22, PM5-18; add 0 to N)

**Cells (PM5-22's list = the 8 legacy construction lines):** v6.1 `a04a3d9cbf5d76a7`, C1 `ebd8da9260b0a80a`, C2
`1873d32cd7019870`, C3 `479049dd2f2b58c4`, spo-v1 `554b0d8ad9304ea5` (all v6.1's combined, role lo1), v7.0
`2b4de3cc3aaaebf2` (lo1), v7.0-lo3 `741d9c05a871a5d5`, spo-v2 `9e1ad5f7ac4b9937` (v7.0-lo3's combined). v7.1's two lines are
re-based as B0a / B0b (N 38, 39) already.

**Mechanism (tooling as built, no code change; PM5-21):** each re-run is a cycle spec whose summ ledgers one construction
line through `nav_summ --rerun-of <trial_id> --rerun-basis window` (backtest_integrity: a window re-run adds 0 to N, carries
window_id research-window-v2, enters V[SR] of prereg item 3; one re-run per target; its target is the legacy line). New
spec-only templates under `scripts/specs/v8-rerun/` (outside `scripts/specs/v8/`, so the v8 spec registry and its tests are
untouched): the library re-runs are templates on `base-lo1.json` / `base-lo3.json` (B0a / B0b: the 4-year role, fields v9,
candidate cache, IC caps, the v7 NAV argv aim-partial-v5 L 1.247) with the original library and recipe pinned
(`change.inputs`), outputs renamed, `card` / `monitor` unset (report-only phases, not part of the NAV), the derived paired
references unset (a re-run is scored alone, FIX-3 procedure), `verdict` false, `summ.cells_from_ledger` unset, `summ.dsr_n`
50, `summ.extra` `--protocol v8 --psr --json <cycle dir>/summ.json --rerun-of <id> --rerun-basis window`; the gate reads
out the original gate's members with `admitted []` (no admission line can be written: PM5-18, "evaluated and logged" by
root from admission.json, never a stop). The NAV-only cells (C1-C3, spo-v1, spo-v2) are templates on the library re-run
with the recorded NAV options.

| spec (scripts/specs/v8-rerun/) | file sha256 (16) | chain digest (16) | re-runs | parent |
|---|---|---|---|---|
| w04-v61-lo1.json | `73446559a264feff` | `c694a303b133d114` | v6.1 | base-lo1.json |
| w04-v61-c1-lo1.json | `6bb9f0749f368d28` | `44a73c5fc5ec6fcc` | C1 | w04-v61-lo1.json |
| w04-v61-c2-lo1.json | `5ca2e68f4f24ff18` | `ecbaa6e838c5f635` | C2 | w04-v61-lo1.json |
| w04-v61-c3-lo1.json | `5f471606c33078ce` | `8408b4e15188403e` | C3 | w04-v61-lo1.json |
| w04-v70-lo1.json | `2de4b3fa674139fb` | `9f4c28b648b2008e` | v7.0 | base-lo1.json |
| w04-v70-lo3.json | `c2b5092cbe93739b` | `1d4e97d31fa4bafd` | v7.0-lo3 | base-lo3.json |
| w04-v70-spo2-lo3.json | `e0a6871c13113109` | `cedff0782cbb089d` | spo-v2 | w04-v70-lo3.json |
| (spo-v1: written after its risk store exists) | | | spo-v1 | w04-v61-lo1.json |

Plans exit 0 (pins `[locked, verified]`: library / recipe of v6.1 `db35c276` / `9bf278a6`, v7.0 `e7bae75c` / `60b82300`; role
lo1 `2ff9d771` or lo3 `e1c67101`; identity bridge, fundamental events (and on lo3 the sic events manifest, read for its pin
only, as B0b / B0c); fields v9 lo1 `888e6616` / lo3 `9f156363` pinned and done). **NAV argv against each original's
receipt (by script): equal except `--combined` (new), `--role` / `--fields` and their pins (the window) and `--output`**;
C1-C3 equal as option maps (order differs: the cycle writes `--rule` after `--output`); spo-v2 also its store: the recorded
store argv (`risk --role R --fields F --emit-exposures all`) on this role and fields is `build-equity/b0c-risk` (lo3, fields v9,
manifest `5dd560d7`; its `--book-weights` adds only the bias files; model files byte-identical to `v8-risk-lo3-v10`, batch 2e).

**Reading, fixed before any re-run:** (1) a re-run carries no acceptance and no limit of its own (PM5-22: the same trials on
a longer window; prereg item 3 puts every ledgered cell's re-run into V[SR], invalid lines only left out); mechanics are read
before its summ (mech.py) and printed, and a value outside the v8 cell limits is reported, not a stop -- the v7 spo-v1 trial
itself was ledgered at all-rows gross .6465 on 2020-2022 (and spo-v2 .9735, v6.1 .9683); (2) a re-run that cannot complete
(refusal, void, non-zero exit) is "a cell that cannot be reproduced" (PM5-22): listed with the reason and left out, never
fixed in code; (3) the admission rows of the original gate members are logged (status, runner sign against prior, sign
agreement); a disagreement is reported, not a stop (PM5-18); (4) N stays 50 after every re-run (checked by the ledger count).

Commit `80b767af` (specs, this text). Each re-run: `run --stop-after nav` (scratch `rerun.sh`), mechanics (mech.py), then
`run` (summ, ledger line). Exes v8-12 (IC `ab7e2cbd`, NAV `5497c89d`, risk `8967952c`); every receipt completed, `clean in
the code pathspec`.

**Spec slip and fix (spec only, PM5-21):** the v6.1 re-run's summ (a direct phase: base-lo1 has no every-phase receipts, so
the cycle dir did not exist) appended its ledger line and printed its tables, then failed writing `--json
build-equity/cycle-v8-w04-v61-lo1/summ.json` (FileNotFoundError; HARD-STOP [summ] exit 4; no summ.json, no cycle_verdict).
The other specs' `--json` moved to `build-equity/v8-w04-summ-<spec>.json` (commit `c89b9b66`); v6.1's own change was
undone (`09969efc`, file `73446559` again) because its NAV is bound to digest `c694a303` (the cycle refused to score it under
the edited digest, exit 3, nothing run); its ledger line is final (one re-run per target) and its numbers below are its summ
console's. The NAV-only children were planned again after the fix (chain digests C1 `feb9dcdb`, C2 `cf0abf18`, C3
`194802ab`, v7.0 `0f267c73`, v7.0-lo3 `99df0403`, spo-v2 `459de0f5`).

**Risk store for spo-v1 (not a trial):** the recorded `v7-w1-risk-all` argv on the 4-year role: `run_bounded_research.py
--seconds 180 --max-rss-mib 1536 --min-free-mib 512 --output build-equity/v8-risk-lo1-v9-run --bind <risk exe, role lo1,
fields v9 lo1> -- atx-equity-strategy-risk.exe risk --role build-equity/train-2020-2023-lo1/manifest.json --role-sha256
2ff9d771... --fields build-equity/train-2020-2023-lo1-fields-v9/manifest.json --fields-sha256 888e6616... --emit-exposures
all --output build-equity/v8-risk-lo1-v9`: 27.4 s, 610 MiB, exit 0, receipt `2f1257e40958c4d9`, source `09969efc`;
**manifest `bdacc15b0ccff20dbf0e8de3bd10b49bd7552f18bfea33a742c8fffa0911e411`**; atx-risk-v1.1, complete, role `2ff9d771`, seal
2024-01-01, last session 2023-12-29; 1,405 dates x 5,922 instruments, 62 factors, 11 styles; invariant refusals 0 (nothing
clamped; max daily specific variance .367); bias harness factor b mean .999, 0 refused. Spec `w04-v61-spo1-lo1.json`
(file `8b7a064d62f770f7`; option map = the recorded argv except the store and its pin), commit `a4063b7c`.

| re-run | target trial | phases (s / peak MiB) | all-rows gross | net | tau mean / p95 | S2 net Sharpe 2020-2023 | ledger line | 
|---|---|---|---|---|---|---|---|
| v6.1 (lo1) | `a04a3d9cbf5d76a7` | u 19.8 / 666, fit 0.8 / 57, w 17.8 / 667, nav 20.6 / 448 | .9751 | +.0034 | .0367 / .0436 | **+.9802** | `1e6f4262f0e2a3ec` |
| C1 | `ebd8da9260b0a80a` | nav 21.6 / 449 | .9768 | +.0024 | .0361 / .0415 | **+.9364** | `1a9f1c9599524c9e` |
| C2 | `1873d32cd7019870` | nav 22.0 / 450 | .9778 | +.0021 | .0358 / .0411 | **+.9080** | `4867927f07ac60be` |
| C3 | `479049dd2f2b58c4` | nav 23.1 / 449 | .9779 | +.0020 | .0355 / .0424 | **+.9022** | `261feb83c2c6f2dd` |
| spo-v1 | `554b0d8ad9304ea5` | nav 47.1 / 448 | **.6793** (outside [.90, 1.05]; v7's own .6465) | +.0002 | .1197 / .1609 | **-1.3869** | `70dd1ffb2017c0ad` |
| v7.0 (lo1) | `2b4de3cc3aaaebf2` | u 20.6 / 667, fit 0.5 / 57, w 17.5 / 667, nav 16.8 / 448 | .9757 | +.0040 | .0364 / .0429 | **+1.0727** | `9c2ce0104221c6e0` |
| v7.0-lo3 | `741d9c05a871a5d5` | u 21.9 / 671, fit 0.8 / 58, w 17.7 / 672, nav 17.3 / 448 | .9711 | +.0038 | .0367 / .0432 | **+1.0839** | `5bd30f88e63bb7fd` |
| spo-v2 | `9e1ad5f7ac4b9937` | nav 43.5 / 449 | .9801 | +.0003 | .0413 / .0613 | **+.5093** | `e532228e6b9d5e3c` |

Every NAV: 1,006 CSV rows, 1,004 return rows (the v7 protocol: no warm start), last session 2023-12-29, accounting <=
1.5e-13 / 4.3e-16. The u passes hit the 4-year candidate caches (19.8-21.9 s); the fits reused the fit-work store (<= 0.8 s).
spo-v2: not void (exit 0; `--specific-ceiling-void on`). Ledger after the 8: **71 lines = 58 construction (50 counted + 8
window re-runs at count 0) + 12 admission + 1 protocol; N 50** (backtest_integrity.trial_counts); file `2b0cdd18`, head
`098251f9`. Summ JSONs `build-equity/v8-w04-summ-*.json` (C1 `d019dd7e`, C2 `0a6ee0da`, C3 `893e4616`, spo-v1 `902d55b2`,
v7.0 `601b2a33`, v7.0-lo3 `330394c4`, spo-v2 `2c0939cf`).

**Gates (PM5-18, logged, never a stop):** v6.1's p1 member sv_flow admitted, runner sign +1 = prior, agrees (HAC t 1.43);
report rows si_ratio, dtc admitted, si_change reject_veto. v7.0's p1-v70 members on lo1: q5_eg, smax5 admitted; qmj_safety
reject_veto, nincr and res_mom_ind reject_redundant (the fitter's v4-prior-v1 screen on the 4-year window, as in B0a); every
runner sign agrees with the prior. v7.0-lo3: read-out with nothing listed (as v7u-lo3). No admission line was written.

Year tables (S2; rows, net return, net Sharpe, vol):

| re-run | 2020 | 2021 | 2022 | 2023 |
|---|---|---|---|---|
| v6.1 | 251, +.0160, +.315, .0556 | 252, +.0778, +2.275, .0332 | 251, +.0840, +1.645, .0500 | 250, +.0000, +.020, .0375 |
| C1 | +.0071, +.155, .0557 | +.0746, +2.220, .0326 | +.0859, +1.646, .0511 | +.0038, +.119, .0383 |
| C2 | +.0036, +.093, .0562 | +.0728, +2.174, .0326 | +.0867, +1.648, .0514 | +.0042, +.128, .0386 |
| C3 | +.0038, +.095, .0574 | +.0724, +2.165, .0325 | +.0870, +1.653, .0515 | +.0041, +.127, .0386 |
| spo-v1 | -.0331, -1.805, .0186 | +.0001, +.011, .0125 | -.0168, -1.670, .0102 | -.0231, -2.323, .0101 |
| v7.0 | +.0072, +.170, .0491 | +.0890, +2.489, .0345 | +.0811, +1.673, .0475 | +.0053, +.172, .0343 |
| v7.0-lo3 | +.0127, +.286, .0482 | +.0835, +2.408, .0335 | +.0796, +1.676, .0465 | +.0053, +.172, .0345 |
| spo-v2 | -.0232, -.614, .0372 | +.0490, +1.597, .0302 | +.0251, +.952, .0265 | +.0083, +.432, .0197 |

Not reproducible: none (8 of 8 ran). Hidden data: every input sealed at 2024-01-01; last session 2023-12-29 in every run.

Commit `5f073613` (re-run log, ledger line).

### 5. V8-F: cumulative test, freeze gate, DSR at the v8 count; H-2

**The final book (V8-F = the last accepted cell, brief step 1): R-2** (`scripts/specs/v8/lib-v80.json`, NAV
`build-equity/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80`, L 1.1474). Base: B0c
(`build-equity/mega-nav-v8-b0c-dlret-ws60-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247`). N 50.

**Freeze gate as registered (v8-prereg item 9; task V8-F step 2), read before any V8-F number is computed:**
(a) S2 net Sharpe of R-2 on 2020-2023 >= 1.0; (b) mechanics (R-2's, PASS in batch 2d; re-read by mech.py); (c) cumulative
paired S2 net dSR (R-2 - B0c) > 0 AND studentized circular-block bootstrap p < .10, **one-sided** (Ruling E-34: "the freeze
gate's 'bootstrap p < .10' is one-sided, as coded in nav_summ since the v7 bundles -- the registered hypothesis dSR > 0 is
directional -- cost if wrong: the gate is twice as loose as a two-sided reading; the scorecard prints both p values"):
`nav_summ --protocol v8 --bundle B0c R-2` (bounded, no `--ledger`), its verdict block; (d) cell-count DSR >= .95 under
OD-4 (prereg item 3, PM7-7): nav_summ `--dsr-ledger build-equity/trials.jsonl` on R-2's dir, `deflated_ledger` (N = the
ledger's construction trials = 50; V[SR] = `backtest_integrity.dsr_variance`: sample variance, ddof 1, of the per-session
S2 net SRs of the construction lines on research-window-v2 -- the 13 v8 cells B0a..R-9b and the 8 window re-runs; the
legacy variance printed beside, gating nothing). Beside it (gate nothing): effective-N DSR, PBO, PSR, MinTRL, from one
bounded nav_summ over every ledgered cell with R-2 last, `--reference` B0c, `--effective-n dirs --psr --pbo --dsr-ledger`,
no `--ledger` (the cycle summ's form at today's ledger). The gate passes only if (a)-(d) all hold.
**OD-3:** if (d) is unmet, the record says so and names OD-3 (history 2013-2019) as the lever (brief step 3); the
registered procedure names the lever and asks for no read. No history file is opened: Ruling E-2 keeps OD-3 at "tooling
only, no read ... the plan's recommendation is to build the tools and leave the read to an explicit owner ruling; a read
cannot be undone" (plan OD-3: "not run in v8 without a ruling"); E-41 counts any such read separately. If any step asked
for that read, the batch would stop here and report.
**H-2** (task-H brief): "Root measures cold VM seconds of library v7.1 in AuditExact on E3 and the fixture." Checked from
code before any run: the IC runner fixes `EvalMode::ResearchFast` (`atx-impl/src/strategy_ic_runner.cpp:223`,
`strategy_runner.cpp:804`); no flag, verb or tool of the v8-12 executables selects AuditExact on a role (the engine's
`alpha_kernels_bench` times single kernels on synthetic panels, not a library on E3). A measurement needs an executable
change: PM5-21 forbids it, so H-2 is not run (stop condition for this item; reported).

Commit `7243bffe` (this reading). Runs (bounded, clean tree, no `--ledger`; the ledger is unchanged at 71 lines, file
`2b0cdd18`):

| step | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| bundle B0c -> R-2 (`bundle.sh`, tag v8f) | 180 / 1,536 | 0.8 | 479 | 0 | `7f5c537a22e0ee40a46f22de5f7f5752815804fb2beb96365e5b1fa0035f4f5e` | `build-equity/v8-cells-v8f-bundle.json` `ab3b5f60` |
| grid summ (57 ledgered cells + R-2 last; scratch `v8f_grid.py`) | 180 / 1,536 | 27.3 | 593 | 0 | `ca97f4e7aa460b9f261955b0bca807ed3df17fa86a7999aaeb490f3bb91cb8f2` | `build-equity/v8-f-grid-summ.json` `42ffb9d2`, `v8-f-grid-pbo.json` `d31fd389` |

(A first grid attempt, `build-equity/v8-f-grid-run`, did not start: the scratch argv file carried CR line ends, nav_summ.py
was not found, exit 2 in 0.3 s, nothing read or written; re-run as `-run2` with the same argv.)

**(a) S2 net Sharpe R-2 2020-2023: 1.2559 >= 1.0 -> PASS.**
**(b) Mechanics (R-2, re-read): PASS** -- all-rows gross .9860 in [.90, 1.05], net +.0042, tau mean .02393 / p95 .02841, 1,006
CSV rows, 1,005 return rows, last session 2023-12-29.
**(c) Cumulative paired S2 net (R-2 - B0c): dSR +.1232** (R-2 1.2559, B0c 1.1328); rho .914; **Memmel SE .2088** (t .59); CBB
95% [-.292, +.529]; LW SE .2141, 95% [-.303, +.549]; **bootstrap p one-sided .278 (E-34, the gate's), two-sided .567**;
bundle verdict `dsr_positive` true, `pass` false (alpha .10) -> **FAIL** (p >= .10). By year (R-2 / B0c): 2020 -.316 / +.319,
2021 2.596 / 2.352, 2022 1.841 / 1.936, 2023 .638 / .118.
**(d) Cell-count DSR at the v8 count: .4648 < .95 -> FAIL.** N **50** (ledger construction trials by the defect rule);
**V[SR] (`dsr_variance`) 1.2971e-03 per session** (ddof 1; 21 construction lines on research-window-v2: B0a, B0b, B0c, R-1..R-8,
R-9a, R-9b and the 8 window re-runs; = .3269 in annual units, SD .572); SR0 1.3014 annual > SR 1.2559. Beside it, gating
nothing: legacy-variance DSR .7651 (37 cells, V 5.98e-04); effective-N DSR .7240 (ONC N_eff 4 of 58 series, 754 common
sessions); listed-dirs DSR .6545 (58 dirs); PSR vs 0 .9926 (MinTRL 458 sessions, 1.82 y), vs .5 .9289 (MinTRL 1,263
sessions, 5.01 y); CSCV PBO .3590 (58 candidates, 16 blocks of 47 sessions, 12,870 splits, exhaustive; winner IS 1.652 ->
OOS 1.108 mean; P(winner OOS loss) .052).

**Freeze gate (v8-prereg item 9): UNMET** -- (a) and (b) hold; (c) fails on p (.278 one-sided); (d) fails (.465). As the brief
step 3 says: the gate is unmet, and **OD-3 (history 2013-2019) is the named lever**; the registered procedure asks for no read,
so none was made (E-2: an explicit owner ruling first). No remedy is applied. The book stays R-2.

**H-2: not measured** (see the reading: no executable selects AuditExact on a role; PM5-21).

**Final book R-2** (S2): net annual return 4.54% (CAGR 4.57%), gross of cost 5.81%, vol 3.61%, max drawdown 3.25%; net
Sharpe 1.2559 ($1bn), at 4x NAV 1.1785 (capacity curve); tau_gmv_mean .02393; all-rows gross .9860; L 1.1474.

**Appendix A (V8-F):** `TRAIN construction cells 50; admission trials this sprint 12 (plus 8 re-screens); window
research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024); history
reads 0; 2025+ never read.`

### Hidden-data record (batch 2g)

- Inputs opened by the tools: roles lo1 / lo3 (4-year), the lo3-dlret label role, fields v9 lo1 / lo3 and v10 lo3, the lo1 /
  lo3 candidate caches and fit-work store, R-2's u / fit / w / NAV outputs, the risk stores `v8-risk-lo3-v10`, `b0c-risk` and
  the new `v8-risk-lo1-v9`, the v6.1 / v7.0 libraries and recipes, the ledger and the ledgered NAV dirs (the 3-year legacy
  dirs included, for their mechanics keys and the grid), each run's own outputs. The lo3 re-run plans hash-verified the
  atx-db identity-bridge and sic-events manifests (read only, as B0b / B0c; nothing under `atx-db/` written).
- Every NAV's last session 2023-12-29 (`1703808000000000000`); every store and fields manifest sealed at 2024-01-01.
  **Nothing dated 2024-01-01 or later was opened.** No NAV `stdout.log` was opened. No history (2013-2019) file was opened.

### Open items (batch 2g)

- **Freeze gate unmet** (p .278; DSR .465): OD-3 is the named lever; an owner ruling is needed for any history read.
- **H-2** needs an executable change (an AuditExact switch on the IC runner); not done under PM5-21.
- v6.1 re-run: no `summ.json` / `cycle_verdict.json` (its summ died on the `--json` path after writing its ledger line;
  numbers from its console, kept in the log above).
- R-8 left two v9 notes (leverage, not alpha); R-9's frontier favours theta .03 at 4x (report only).
- **Current book: R-2**; N 50; admission trials 12 of 15; history reads 0. Disk 125,635,739,648 B free (117.0 GiB).

## integration 8 (2026-10-02)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `7fb2a0d2` (clean). Scope: `task-INT8-brief.md`
(merge order, items 5a-5g, builds A / B / C, tests, identities), PM7-13, PM7-15 (a), PM7-25 (freeze ended; R-2 must
reproduce under the new build before any X measurement), PM7-26. Tag prefix v8-14 (v8-12 pinned Wave 0, v8-13 the
unadopted Release build). No X cell, no add-alpha screen of an X candidate, no campaign, no X result opened (none exist).

### Merges (`--no-ff` by SHA; each `git merge-tree` re-run at dispatch: six clean, XCOMB one file as predicted)

| # | lane | lane SHA (= branch head) | merge | conflicts |
|---|---|---|---|---|
| 1 | XPRE | `da3bb239` | `491f2ea8` | none |
| 2 | XSIG | `7467448f` | `a30d0fb1` | none |
| 2 | XIMP | `3db253d5` | `e99aa994` | none |
| 3 | LIB3 / FIELDS-V9 | `834d5a05` | `f6786b9f` | none |
| 3 | XDATA incl. task GOLD | `e3654b93` (ahead of the `d44fa7f6` progress.md records by `9ffbce6c`, `3c0aee08`, `e3654b93`: task GOLD, PM7-19 / PM7-23; wanted, merged) | `7c3d63be` | none |
| 5 | XCOMB | `914f9944` | `ecef208e` | `scripts/tests/test_research_spec.py`, 3 hunks, the brief's union: NULL_PINS keeps v8's r3-gm / r6-gm / r9a-b / lib-v81-gm pins and adds `x-theme-erc.json`, `x-inv-vol.json`, one `STORE_FILLS` (v8 spelling); EXPECTED_CHANGES keeps v8's lines and `THETA`, adds the two `x-*`; `nav_delta` closes with `x-inv-vol.json`, keeps the r6-gm / THETA / `spo` lines, `assert "--capacity-curve" in cn or name not in ("r5-adv-hold.json", "x-inv-vol.json") + spo` |
| 6 | mining (MINE-MEM, -STAT, -RUN, -JOIN, ENG-SLOT) | `1bd448cd` | `31086f3a` | none; auto-merged files re-read: `atx-impl/CMakeLists.txt` (mine sources :28-34, theme-erc :114 / :121), `MINED_MAX_BUDGET = 10000`, ledger tests at 10000 (the 1000s left are campaign-line fixture budgets, not the ceiling), `research_cycle.py` `mine` dispatch in `main` |

No lane touched `atx-db/` (`git diff --name-only <base> <sha> -- atx-db` empty for all seven).

### To-write items (each its own commit)

| item | commit | what |
|---|---|---|
| 5c PM6-9 | `d148bdf9` | `generate_library.exe_plan(..., max_memory_mib=None)` appends `--max-memory-mib N`; `research_add_alpha.plan_for` passes the parent spec's `ic.flags` value. The fake IC exe records its plan argv (opt-in `FAKE_IC_ARGV`, set in `953c9569`); `test_add_alpha_validates_through_the_exe_plan` asserts `--max-memory-mib 1536` (fails without the fix, checked) |
| 5d PM7-15 (a) | `b8d4af86`, `ffbf3888` | `V7_APPENDED_THEMES = ("ownership_flow", "filing_events")`; `test_composition_resid.py` (:243, :278-294, :671-711) takes the frozen ten (`cres.FROZEN_PREFIX`) as the before-registration order; `test_fit_composition_weights.py` constants and refusal text; `ffbf3888` the same refusal literal in `test_fit_composition_weights_store.py` (found by the XCOMB suite run) |
| 5e PM7-13 | `d12c8b0b` | `registry.json` `house_budget.max_roster` 64 -> 80; `atx-impl/strategies` 163 passed |
| 5f | `953c9569` | `research_add_alpha.MARGINAL_CAPS = {"seconds": 360}` written into `runner.phases.marginal` of every add-alpha wave spec unless the parent's spec names one (spec data, OD-2 precedent). R-7's marginal receipt (`mega-v8-b0b-train-u-v81-marginal-pool-run`): wall 170.3 s of 180, peak 252 MiB; 80/57 x 252 = 354 MiB < 1,400, so `max_rss_mib` stays the runner's 1,536. No X wave spec or template exists in the tree yet, so `EXPECTED_CHANGES` has nothing to admit; the add-alpha tests in `test_research_cycle.py` and `test_research_spec.py` assert the cap. The PM restates it in `v8x-prereg.md` before X-2 |
| 5a R6C-7 | `6cb4c857` | `composition_recorded_rule`: a file without a string `provenance.rule` passes only without a `theme_standardise` block, else `Err(InvalidArgument, "... carry a theme_standardise block without a string provenance.rule (finding R6C-7)")`; comment fixed. Tests: `themed_text` gives a `std_block` doc its rule, `shrink_doc` and XCOMB's `erc_doc` record `provenance.rule` (brief finding F1); `RecordedRuleMustWrite...` moves the no-provenance `ic-shrink-v1` doc to the refused cases and admits a plain v1 file without provenance |
| 5b R6C-3 (tests only) | `57483cf3` | `ThemeResid.UnequalTieBlocksBesideSingletonsPinTheBlockMean`: UNEQUAL_PLANES / _EXPECTED / _WRONG through `st::add_theme_residualised` (W .5/.5, 1e-15, one value per block, add order 0,1,5,7,2,4,6,3). No source seam (cut 2 not needed; the seam was optional): the 1e16 summation-order pin stays Python-only |
| (mine test) | `6a1363a9` | `test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry` asserted the two `HELD_BY_V8_LIBRARY` fields unread by the registry; R-2 was accepted, so its rows read them. Now asserts the excluded classes unread and the held fields present in v9; the rule (12 fields = the template's) unchanged |
| 5g PM7-13 | `5c65cee8` | `kMaxMinePoolMembers = 80` (`strategy_mine_pool.hpp:31`, comment :13, `strategy_mine.hpp:137`), `research_mine.py MAX_POOL_MEMBERS = 80`, runbook :80 / :100. `StrategyMine.WorkingBytesAreThePeakOfThePhases` keeps the 53-member peaks at members {1, 80} |
| H-2 | `66f83b35` | see section H-2 |

### Builds (`scripts/research-build.ps1 -Preset equity-dev`; build dir `build-equity`; 4 jobs each, first attempt)

| tag | source | targets | result |
|---|---|---|---|
| v8-14a (build A) | `57483cf3` | equity-strategy-ic, -targets, impl-strategy-ic-tests, -target-tests, engine-combine-tests, -book-tests | exit 0, 74.4 s, 31 TUs, 7 links; reconfigured (GLOB mismatch: new sources); receipt `3e12fa39...c4a9` |
| v8-14b (build B) | `5c65cee8` | equity-strategy-mine, impl-strategy-mine-tests, engine-factory-tests, engine-alpha-tests, atx-shm-worker | exit 0, 98.5 s, 44 TUs (incl. `search_driver.cpp`, the seven `strategy_mine*.cpp`, `alpha_vm_slot_reuse_test.cpp`), 7 links; receipt `e0760327...e4b2` |
| **v8-14 (build C, the record)** | `66f83b35` | all fourteen research executables and test targets of the brief | exit 0, 59.7 s, 32 TUs, 12 links; receipt `e8d1831c...3e68`; `ConfiguredProvenance` `5c65cee8` (configure-time, set at build B; build C did not reconfigure; the only code between is H-2's flag; strategy_live reports this provenance as a health warning, never a refusal, review C4; the executable SHA binds the code) |

**0 compile fixes**: the mining and XCOMB C++ (never compiled before), 5a, 5b and H-2 compiled first time under `/W4 /WX`;
no warning or error line in any of the three logs. Research executables on **v8-14** (bin 2026-10-02 10:48):

| executable | v8-14 SHA-256 |
|---|---|
| atx-equity-strategy-ic | `67f7292192bf431fccb1af185857e4b51d316860d42ea021037e37af288b4e6b` |
| atx-equity-strategy-targets | `a95f6f0af06907f3707ca10b182de451d84727aeeb1ce5e7170906fd36d31417` |
| atx-equity-strategy-risk | `d7e424b23f097737cf3086dff886b5cf45e9b77dcb0d1059fa99d8c604fcf623` |
| atx-equity-strategy-mine | `2176fa4a3e7b49c5d375b0d41e30cb8504687073d35328b05df7132def2ed7ec` |

Test executables (v8-14): mine-tests `d6e40412`, factory `58bb8bee`, alpha `c623aff2`, ic `b7be8c2c`, target `ca223f8a`,
impl `c6824689`, strategy `1d05e484`, book `4ed6dc96`, combine `be5e8ff4`, shm-worker `9586f5d5`.

### Mining golden `0x889874a3b9b29c55` (not edited), fixture, slot reuse

| test | exe | 1 worker | 4 workers |
|---|---|---|---|
| `SignalFitnessDefaults.ExplicitDefaultsKeepTheGoldenDigestAtEveryWorkerCount` (loops {1, 4}) | factory-tests and mine-tests (v8-14b; mine-tests again on v8-14) | **holds** | **holds** |
| `SignalFitnessDefaults.ImplicitDefaultsKeepTheGoldenDigest` | both | holds | - |
| `NsgaSearch.ScalarRaw_ReproducesGoldenDigest` | factory-tests | holds | - |

- `--gtest_filter=SignalFitnessDefaults.*:NsgaSearch.ScalarRaw_ReproducesGoldenDigest:SignalFitnessPath.*`: factory 10/10, mine 9/9.
- `StrategyMineRule.*:StrategyMine.*:StrategyMineCampaign.*`: **29/29**. `PromotesThePlantedSignalsOnlyInFiveSeeds` passes
  (asserts **`rung_failed == 0`** and `failed == 0`); `MembersStreamByDateAsStored` passes (write / remove / rename refused
  while held: the Windows share mode observed, not only asserted); `WorkingBytesAreThePeakOfThePhases` passes at M 80.
- `SameSeedSameChainHeadAtOneAndFourWorkers` (`build-equity/v8-i8-mine-heads.xml`, `88716737...b075`):
  `fixture_registry_head` `0ca572d1d176d86de6d89746b37365f3046a8a84166f3219964251bfac36d56a`,
  `fixture_trials_csv_sha256` `e3196f0932615112abdd0006c065920eb29c22959f8f5544d6617658694602ef` (differ from `20e7bd19`'s by
  design: MINE-STAT's recipe keys).
- **`atx-engine-alpha-tests --gtest_filter=AlphaVmSlotReuse.*`, run directly: 6/6 passed** (1.99 s; no abort, no byte
  difference: AuditExact, ResearchFast, masked AuditExact, masked ResearchFast all byte-equal to a fresh engine); recorded
  `forms` 107, `variants` 1,266, `max_target_slots` 9 (`build-equity/v8-i8-slot-reuse.xml`, `be42b334...7373`). Whole
  alpha binary (never run in this sprint's integrations before): **755/755**.

### Tests (counts grow only by the lanes' tests and this integration's)

| suite | build | result |
|---|---|---|
| ic-tests `GroupErc.*:ThemeErcV1.*:CompositionV8.*:ThemeResid*` | v8-14a | **36/36** |
| atx-impl-strategy-ic-tests (whole) | v8-14 | **159/159** (145 + 12 XCOMB + 1 R6C-3 + 1 H-2) |
| target-tests `InvVol.*:BookInverseVol.*` | v8-14a | **9/9** |
| atx-impl-strategy-target-tests (whole) | v8-14a, v8-14 | **268/268** (259 + 9); `[spo-pin]` v1 `0xda6b6871e7e267c5` / `0xaabdbb72f99a6e13`, `[spo-v3-pin]` v2 `0xb039820b40d5cf24` / `0xd24b61721a7c698c` |
| atx-engine-combine-tests | v8-14a, v8-14 | **238/238** (233 + 5) |
| atx-engine-book-tests | v8-14a, v8-14 | **160/160** (155 + 5) |
| atx-impl-strategy-mine-tests (whole) | v8-14b, v8-14 | **44/44** (31 + 13) |
| atx-engine-factory-tests (whole) | v8-14b (= v8-14 bytes) | **392/392** (390 + 2) |
| atx-engine-alpha-tests | v8-14b (= v8-14 bytes) | `AlphaVmSlotReuse.*` **6/6**; whole **755/755** |
| atx-impl-tests (from the repo root) | v8-14 | 1,052 run: **1,046 passed, 5 skipped, 1 failed: the known `ConfigJsonNotInDiscoverDigest`** (1,028 + 24 through the globs); same spo pin lines |
| atx-impl-strategy-tests | v8-14 | **46/46** |
| XPRE pytest (`test_dsr_total`, `test_nav_summ`, `_v8`, `_pool`, `test_backtest_integrity`, `test_trial_ledger_rules`, `test_holdout_gate`) | - | **96 passed, 1 skipped** (`ATX_EQUITY_ROOT`); `test_flag_absent_is_byte_identical_to_the_pre_x_nav_summ` passed; with `ATX_EQUITY_ROOT=build-equity` `test_legacy_n37_numbers_reproduced` **passed** |
| mining pytest (`test_research_mine`, `test_research_ledger`, `test_trial_ledger_rules`, `test_mine_overlap_factor`) | - | **61 passed** |
| `research_cycle.py mine plan scripts/specs/v9/mine-c1.json` | - | metadata only, as expected: pins UNLOCKED, pool MISSING, pool_source / max_memory_mib TO FILL, four requires lines; budget 132 |
| XCOMB pytest (7 files) | - | **209 passed** after `ffbf3888` (1 failed before it: the 5d literal) |
| LIB3 / FIELDS-V9 (3 files) | - | **26 passed**; XDATA (`test_research_fields_xdata`, `_gold`) **14 passed** |
| XSIG `xsig_check.py` | - | **`xsig_check: PASS`** |
| atx-engine/tools (whole) | - | **293 passed** |
| atx-impl/tools (whole; `ATX_EQUITY_BIN`, `ATX_EQUITY_TARGETS_EXE` absolute, v8-14) | v8-14 | **612 passed, 1 skipped** (`ATX_EQUITY_ROOT` unset in the whole run) |
| atx-impl/strategies | - | **163 passed**; `test_generate_library.py` with `ATX_V71_PLAN_JSON` **9 passed** |
| scripts/tests (whole; v8-14) | v8-14 | **245 passed, 3 skipped** (the RESEARCH_CYCLE_LIVE_ROOT skips); tiny_world: no golden moved; `git status` clean after |

LIB3 / XDATA identity: `git diff --stat 7fb2a0d2 HEAD -- atx-engine/tools` lists only their 10 new files (plain builder
untouched; no fields rebuild).

### Identity under v8-14 (PM7-25; bounded runner, clean tree `66f83b35`, one at a time; argv = the recorded receipt's with only the output path renamed)

Every receipt: outcome completed, exit 0, `git: clean in the code pathspec`, source `66f83b35`, exe = v8-14's.

**R-2 (the V8-F book; X baseline)**

| pass | source receipt | new receipt.json SHA-256 | s / MiB | result |
|---|---|---|---|---|
| w (`--output build-equity/v8-i8-r2-w`; 300 s / 3,072 MiB) | `mega-v8-r1w-train-std-v80-run1` | `26536b276bb2731e023824ef9bb3680afa9f46d068b769488b15f34bcce279f0` | 36.2 / 1,308 | **10 of 12 byte-identical**: `recipe.json` `7380ec35`, `orientations.json` `ed90c8d0`, `train_combined.f64` `6331421f`, `train_combined.json` `bbbf6f2b`, `_member.u8` / `_finite.u8` `c61f5b62`, `_ids.u64` `761bd1df`, `_sessions.i64` `ab244802`, `train_planned_targets.csv` `2e743418`, `train_daily_ic.csv` `b631b6e3`; `summary.json` (216 paths) and `train_candidates.jsonl` (207) differ **only in timing paths** (`stage_seconds.*`, `wall_seconds`, `hash_seconds`), as INT7 |
| fit (`--output build-equity/v8-i8-r2-fit`; 180 / 1,536) | `mega-weights-v8-r1-std-v80-run1` | `422d01050ad9ac35f4910ae0f374681a064c9eb9c439cfd60d7f9735a10563df` | 0.8 / 55 | `admission.csv` byte-identical; `admission.json` differs only in `inputs/script_sha256`; `composition_weights.json` differs only in `provenance/script_sha256`, `provenance/admission_sha256` (the SHA of the new admission.json, `e7fbdfe8`) and **`provenance/std/registry_sha256`** (`570f022a` -> `566d7076`). Both files are **byte-identical after substituting those values back**. The registry is an input the fitter reads (not on the argv): `570f022a` is the registry at R-2's fit (`9d4203f0`); it changed at `7402d7b2` / `8d2eac5e` (R-7 rows and the roster cap 64, before V8-F) and at `d12c8b0b` (item 5e). Weights, signs, admission rows: identical. **Listed for the PM: one allowed-list exception (an input-pin field), no computed value differs** |
| NAV (`--output build-equity/v8-i8-r2-nav`; 180 / 1,536) | `mega-nav-v8-r1-std-...-L1.1474-v80-run` | `74d0474a9a7149de9f22e8970418758823cbd50d1514f611820d8a50c071e807` | 42.5 / 586 | **27 of 27 byte-identical** (every `daily_*.csv`, `events_*.csv`, `recipe.json` `2311993e`, `summary.json` `083a56da`, `capacity_curve.csv` `b99b1cd1`, `v7_*`, the 12 `capacity/` files; S2 daily `7cfe21c4`); `stdout.log` differs only on its output-path line. Not refused at `--max-bytes` (brief F2) |

This is also XCOMB's flag-absent identity (theme-erc: w pass and fit without `--theme-erc`; inv-vol: the V8-F NAV argv)
and items 5b / 5d.

**INT7's identities (lanes' flag-absent; the v8-i7 receipts' argv, outputs `v8-i8-*`)**

| id | receipt.json SHA-256 | s / MiB | result |
|---|---|---|---|
| 1a | `b387bb9dd27d0def6996233f109bf0320ed5eaa9e47bbdd0e104319634a50dd1` | 13.5 / 359 | **12 of 12** byte-identical to `v8-i7-i1-nav`; stdout identical |
| 1b | `dbb1097e3a8aff21b4230a67487e40792d4da35e3e7a4956114b97da73db2ff3` | 13.5 / 361 | NAV **12 of 12**, holdings **4 of 4**; stdout identical |
| 4 step 1 | `423bf780b530687784f6a2ed5bf2b1e9f360eb65976d29871080169a44c0f00d` | 0.3 / 5 | weights file **byte-identical** (`d49e208c`) |
| 4 step 2 | `fda2e9ec826a52b0c73e628242863eb0244e8222b005096d6bc0ccc570f74bf3` | 18.3 / 506 | **10 of 12**; `summary.json` / `train_candidates.jsonl` timing paths only (as INT7) |
| 7 | `83396a5c28016c213545d709fa24dd7c1c5484b2979ae9f032db5ed307282004` | 35.0 / 360 | **9 of 9** |
| 8 | `775aeca659b00ca2aeba26017fc5865a5ecd98db0dda05cce83e2cbd93a99b87` | 14.3 / 464 | **12 of 12**; stdout identical |

XPRE: flag-absent test (4 argv sets) passed; legacy n37 reproduced. Mining: no flag; the golden is its identity (holds).
**R-2 reproduces under v8-14** (w, NAV byte for byte; fit with the three provenance hashes above), so X has its baseline.

### H-2 (AuditExact cost; flag-gated, flag absent byte-identical)

The AuditExact selection is small: `IcRunnerConfig::audit_exact` (`--eval-mode audit-exact`, the only accepted value) sets
`al::EvalMode::AuditExact` where the runner builds its Engine (`strategy_ic_runner.cpp:223`) and the recipe's `vm`
(`AuditExact;full-historical-asof-member-mask`); refused with `--candidate-cache` (entries keyed on ResearchFast).
Commit `66f83b35`, test `StrategyIcRunner.AuditExactEvalModeIsRecordedAndRefusedWithTheCache`. Flag absent: every
identity above passed on the build that carries it.

Measured as the task-H brief says (cold VM seconds of library v7.1, E3 and the fixture); no IC statistic read:
- **E3** (4-year role lo3 `e1c67101`, fields v9 lo3 `9f156363`: B0b's u-pass argv without `--save-combined` /
  `--candidate-cache`, plus `--no-composition`, 4 workers, 2,560 MiB, bounded 600 s / 2,560 MiB; 48 candidates, every one
  evaluated): ResearchFast VM **59.4 s** (wall 91.7, peak 1,496 MiB, receipt `5b98f8b5...ecb12`); AuditExact VM **70.1 s**
  (wall 100.1, peak 1,496, receipt `1c4ab80f...c578c`). **Ratio 1.18x.**
- **Fixture** (tiny_world, its 4-member library, synthetic, 5 repetitions each, 1 worker): median VM 13.7 ms ResearchFast,
  14.5 ms AuditExact: ratio 1.06x (noise level).
- Debug (`equity-dev`) executables, one run per mode on E3. **AuditExact costs less than 3x ResearchFast (1.18x)**: the
  brief's condition for planning the date-blocked runner (platform review P-5b) is met; planning it is the PM's.
- The two E3 output dirs (`build-equity/v8-i8-h2-fast`, `-audit`) hold IC rows of v7.1 on 2020-2023 that were not opened;
  they are measurement runs (0 trials, prereg section 3) and select nothing.

### Campaign memory probe: not run

`v8x-prereg.md` counts a probe at 0 trials (section 3) and orders it in the runbook (section 9: after `mine pool` on H-F's
pool source, with role lo3 and H-F's fields, A2-A4), but it does not state that the probe reads no return, and its
inputs (pool from H-F, H-F's fields) do not exist before X-2..X-8. The dispatch's condition is not met.

### What the X cells can run now (v8-14; PM preconditions P4 (X list pinned) and P6 (fields v13) still the PM's / root's)

- **X-2** (refinements, 5 strings) and **X-3** (XSIG 5 + XDATA 3): add-alpha with the K1 plan under the spec's IC cap (5c),
  the marginal cap 360 s (5f), roster cap 80 (5e), `filing_events` last in the fitter (5d), the field builders (LIB3,
  XDATA) and the v8-14 IC / NAV executables. X-3 also needs the IC-pass memory cap re-probed (P7, PM7-13).
- **X-4** (value within FF49, 9 re-screens): add-alpha re-screens on the same tools.
- **X-5** theme-erc-v1 / **X-6** inv-vol-v1: built on v8-14, flag-absent identities pass on R-2; root sets the parent.
- **X-9** (mined wave): mining merged and built, golden at 1 and 4 workers, fixture `rung_failed == 0`, slot reuse 6/6,
  pool cap 80; needs H-F (after X-2..X-8), the pool step, the probe and the spec edits of the runbook (A2: role lo3).

### Scoped review range

- Code: `7fb2a0d2..66f83b35` in `research_tree.CODE_PATHSPEC`: the seven lane merges and the integration commits
  `d148bdf9`, `b8d4af86`, `d12c8b0b`, `953c9569`, `6cb4c857`, `57483cf3`, `ffbf3888`, `6a1363a9`, `5c65cee8`, `66f83b35`.

### Hidden-data record

- Inputs opened by tools: roles lo1 (3-year) / lo3 (3- and 4-year) and the lo3-dlret label role, fields v7 / v9 / v10,
  the v7.1 / v8.0 libraries and recipes, the lo3 / v71 candidate caches and fit-work store, R-2's u / w / fit / NAV
  outputs (pinned inputs of the identities), the lo3 risk model `786cb601`, the INT7 identity inputs, the v7.1 n37 cells
  (XPRE legacy test), the ledger (tests only, read, not appended), tiny_world (synthetic). Tests used synthetic fixtures.
- Read by me: lane reports, briefs, rulings, sources, build receipts and logs, runner receipts (outcome, exit, timings,
  SHA-256s, argv), output-file SHA-256s, JSON paths of differences (no values), gtest pass / fail / pin lines, the H-2
  timing fields, R-7's marginal receipt (wall, peak). No return, Sharpe or IC statistic was read or printed.
- Scan of every `v8-i8-*-run` stdout / stderr for dates in 2024 or later: none.
- **Nothing dated 2024-01-01 or later was opened. No X cell, screen or campaign was run. No ledger line was written (N 50).**

### Open items

- **For the PM:** R-2's fit identity has one difference outside the allowed list: `provenance/std/registry_sha256` (input
  pin of the registry, which changed by R-7 before V8-F and by 5e); every computed byte is identical (above).
- Restate the X waves' marginal cap (5f, 360 s) in `v8x-prereg.md` before X-2; re-probe the IC-pass memory cap before X-3.
- H-2: P-5b (date-blocked runner) is plannable on the 1.18x reading (Debug; a Release reading may differ).
- v8-14's configured provenance is `5c65cee8` (one commit behind the source: H-2's flag only).
- Known: `ConfigJsonNotInDiscoverDigest` (1). The bare `atx-equity-strategy` was not built (no research script uses it).
- Disk 125,337,964,544 B free (116.7 GiB).

## X batch 1 (cells X-2..X-6): fields v13 built; K1 pre-checks; STOPPED before the X list pin (P4) (2026-10-02)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `3d12d495` (clean; build v8-14, nothing
built). Read: integrator-rules, `v8x-prereg.md` (whole), progress "PM session 7" to the end (PM7-1..31), the XIMP, XSIG,
XDATA, XCOMB reports, this log's integration 8, batches 2c, 2d (R-2..R-4) and 2f. Baseline / first parent R-2
(`lib-v80.json`, L 1.1474, G .9859903463), N 50. Scratch readers as batches 2c-2f (`k1plan.py`, PM6-9 route).

**No X cell ran. No admission line, no construction line, no ledger write (ledger 71 lines, `2b0cdd18`, N 50). No
IC, return, Sharpe, turnover or NAV number of any X candidate exists or was read.**

### Registration gaps raised to the PM before any X read

1. XDATA's three X-3 candidates (`div_season`, `vol_beta`, `season_y2_5`) carry no tier, no settled theme for
   `vol_beta` ("low_risk (or a new macro_vol_risk)") and no add-alpha text (report section 4b: "suggested frozen
   strings for the PM's registration; not registered here"); PM7-16 (a) fixed none of them. Tier sets the
   within-theme share under ew-theme-std-v1 (registry `tier_scores`), so it is a constant root cannot choose.
2. Fields v13 (P6) needs both draft entry modules in one builder process; the fold XDATA named for integration 8
   (one tuple element in `prepare_research_fields_draft.DRAFT_MODULES`) was not made. Route used (no file changed):
   one bounded `python -c` run calling the two entries' own `register` on the builder namespace, then the builder's
   own `main` (below).

The PM answered by message and asked root to write its ruling into `progress.md` before pinning the X list (P4). **The
session's permission system refused that edit.** By the PM's order (ruling first, then P4) P4 was not pinned, and
every X cell needs P4 before its first measurement (`v8x-prereg.md` section 11 (1)): **X-2..X-6 not started.** The
decision on how to proceed is the PM's / owner's.

### 0. Fields v13 on lo3 (precondition P6; 0 trials)

Command = v11's recorded argv (receipt `train-2020-2023-lo3-fields-v11-run`) with `--output
build-equity/train-2020-2023-lo3-fields-v13`, `--fields` = v10's 70 names in v10's argv order +
`nt_first_126,earn_season_rank` (LIB3 / FIELDS-V9, `research_fields_v9.py`) + `div_month_pred,beta_dvol_21,season_y2_5`
(XDATA, `research_fields_xdata.py`), `--reuse build-equity/train-2020-2023-lo3-fields-v10 --reuse-sha256 a4a060ae...70809
--reuse-hardlink`; builder `--max-rss-mib 2048 --max-seconds 580`; runner `--seconds 600 --max-rss-mib 2560 --min-free-mib
512` (W0-i), binds the ten tool modules (plain builder, sec, holdings, price, v8, both draft entries, v9, xdata, gold),
the role manifest and v10's manifest. Driver (whole): `import sys; sys.path.insert(0, 'atx-engine/tools'); import
prepare_research_fields as b; import prepare_research_fields_draft as d; import prepare_research_fields_xdata as x;
d.register(vars(b)); x.register(vars(b)); b.main(sys.argv[1:])`. Script `scratchpad/fv13.sh`.

| step | receipt dir | source | outcome / exit | s | peak MiB | receipt.json SHA-256 | output manifest SHA-256 |
|---|---|---|---|---|---|---|---|
| fields v13 lo3 | `train-2020-2023-lo3-fields-v13-run` | `3d12d495` | completed / 0 | **44.0** | **543** | `b08496275758521c278a206fd4f6c7e5ebae340aed72cc2826c3be4fe12425a1` | **`e5f7f28c465a92885d55d55a439eb2f217b9f5f15d51017b4293ebc55935e9b2`** |

Receipt: `clean in the code pathspec`, dirty outside none, min system free 5,863 MiB; stdout `7dff1514`, stderr empty.
Manifest (metadata only): status complete, `seal.exclusive_end` 2024-01-01, role lo3 (1,405 dates, first session
2018-06-01), **75 rows**, builder `code_sha256_lf` `74df97f9` (= v10's / v11's), 633,772 B; no 2024- or 2025-named
source. **Counts: reused 70, computed 5 = the expected 70 / 5** (`nt_first_126` `3532615a` sec-nt-first365-126-v1,
`earn_season_rank` `fca9f725` chss-earnrank-ni20q-v1, producer module `research_fields_v9.py`; `div_month_pred`
`06535cab` hs-divseason-q3-6-9-12-v1, `beta_dvol_21` `f4a1cf31` ahxz-beta-dvol-spy21-v1, `season_y2_5` `ce5b9286`
hs-season-y2-5-v1, producer `research_fields_xdata.py`). The 70 v10 payloads: entry sha256 = v10's (70 / 70), hardlinks
of the v10 files (70 / 70); all 75 payloads re-hash to their pins. New rows at 62-66 (registry order). Dir 4.7 GiB
apparent (hardlinks). Logs scanned for 2024-2029 date tokens: none.

### K1 pre-checks (metadata only; PM6-9 route; nothing written to the tree; plans in the scratchpad, not of record)

Each frozen string as its own add-alpha argv on v80 (R-2) with fields v13 through `k1plan.py` (the exe's `--plan-only
--max-memory-mib 2560` on the exact library bytes). The plans of record are re-made call by call at registration.

- **X-2 (XIMP, verbatim; A-1 without `--rescreen`): 5 / 5 plan.** DSL sha16 / bars / slots / nodes / extra fields =
  the XIMP tables exactly: q5_eg_f49g `07a61a9e` 272/7/33/6 (exception inherited from q5_eg_f49), iv_rv_spread_xe
  `29e7d9cf` 21/7/31/2, ind_adj_rev_5_nx `9c1d051d` 5/6/21/2, ins_opp_buy `981d01b2` 0/3/4/1, bac_vq `116135c0`
  272/6/20/1. Library 52 members, max slots 8 (qmj_safety), lookback 272.
- **X-3 XSIG (frozen lines parsed verbatim; placeholders substituted only): 5 / 5 plan** once the four fields have
  registry rows (added in memory for the check: without them the exe refuses "undeclared DSL field", the expected
  state before the wave's registry edit). Rows = XSIG section 5: stmom `06dc6238` 41/5/shares_out, earn_season
  `64a0be8f` 0/5/ea_days_to_expected+earn_season_rank, k8_intensity `0b7d6cde` 209/5/k8_count_63, inst_persist
  `ea338c04` 272/6/inst_own_chg_q, nt_late `bafc4e3a` 0/3/nt_first_126; exe node counts 22 / 11 / 16 / 34 / 5 against the
  mirror's 21 / 11 / 15 / 34 / 4 (no node budget exists; as R-7).
- **X-3 XDATA (3): not checked** (their registration is the PM's ruling of the message above).
- **X-4 (XIMP C-1 table, verbatim; replaced member's theme / tier / sign): 9 / 9 plan**, rows = the C-1 table:
  value_composite_v49 `09fb156c` 20/6/19/5, bm_v49 `a37c3eca` 20/4/11/3, ep_v49 `0d1975e5` 20/4/11/3, cfp_v49 `86aed322`
  20/4/11/3, fcfp_v49 `408ba943` 20/3/9/4, ebit_ev_v49 `4a2b9ba7` 20/5/18/5, net_payout_v49 `8b6e8423` 20/3/11/5, sp_v49
  `1153ac46` 20/4/11/3, rd_me_v49 `bf89d6a6` 20/4/11/3.

So no X string is void at K1 on the evidence so far.

### Hidden-data record (X batch 1)

- Inputs opened by tools: role lo3, fields v10 (reuse) and the sealed atx-db stages / fundamental events / vendor
  TickerHistory3 file through the field builder's sealed readers (read only; nothing under `atx-db/` written); fields
  v13 metadata for the K1 plans.
- Read by me: lane reports, rulings, sources, the v13 receipt and manifest metadata (status, seal, names, entry
  sha256 / formula / producer, reuse lists; not the coverage blocks), plan rows (metadata).
- **Nothing dated 2024-01-01 or later was opened. No X statistic exists.**

### Open items (X batch 1)

- **For the PM / owner:** the refused `progress.md` edit (above); P4, then X-2..X-6, wait on it. Fields v13 is ready
  (manifest `e5f7f28c`); the K1 evidence above needs no re-run beyond the plans of record at registration.
- Before X-3's add-alpha: registry field rows for `k8_count_63`, `inst_own_chg_q` (XSIG L2), `nt_first_126`,
  `earn_season_rank`, `div_month_pred`, `beta_dvol_21`, `season_y2_5` (clock / basis from the v13 manifest rows); the
  IC-pass memory re-probe (PM7-31) on X-3's cumulative K1 plan.
- Current accepted parent: **R-2** (`lib-v80.json`), N 50, admission trials 12 of 15 (v8) + 0 of 13 (X hand-written).
  Disk 124,832,608,256 B free (116.3 GiB).

### X batch 1, resumed (PM message after PM7-32 was recorded by the PM at `bc153439`)

**Preconditions (0 trials).**
- Registry field rows (commit `297c5d55`, registry `566d7076` -> `fe109c9b`): `k8_count_63`, `inst_own_chg_q` = XSIG's L2
  rows verbatim (formula id, origin, producer and clock checked equal to the v13 manifest rows; XSIG's basis text is the
  longer one); `nt_first_126`, `earn_season_rank` (fields_v13, `research_fields_v9.py`), `div_month_pred`,
  `beta_dvol_21`, `season_y2_5` (fields_v13, `research_fields_xdata.py`): clock = the v13 manifest row's verbatim, basis =
  its definition + "(formula id X; producer ..., fields-v13)" (R-2 / R-7 form). No theme or budget change.
- K1 of the three XDATA strings (PM7-32 argv) on v13: div_season `5a1380a1` 0 bars / 2 slots / div_month_pred, vol_beta
  `91583af3` 20 / 3 / beta_dvol_21, season_y2_5 `13fe28b6` 0 / 2 / season_y2_5; the XSIG five re-checked against the
  committed registry (same rows as above). All 22 X strings plan; none void.
- **X list pinned (P4):** `v8x-prereg.md` section 14, commit `3603845b`, file SHA-256
  `25ba909f95bfc42722fece5dda0501ec8d12700d038fcfb8794ae344df68ecd4` (27 items with DSL / template SHA-256, lane report
  commits, the XDATA argv of PM7-32, the fields v13 pin, the criteria); recorded in `progress.md` (`5af8e6cd`).
- P6 fields v13: above (`e5f7f28c`, 70 / 5). P7 the IC-pass memory re-probe: at X-3's cumulative K1 plan (below).

**Rulings received during the batch (recorded by the PM):** PM7-34 (owner directive): from X-2 on, every X cell is
accepted iff paired S2 net dSR > 0 AND mechanics (PM6-6 unchanged); each cell's capacity / turnover / cost criterion is
computed and printed as "capacity criterion: met / unmet" and decides nothing. **The new rule took effect at X-2 (the
first X cell; no X verdict had been written).** PM7-35: see X-2 below.

### Cell X-2 (refinement wave; library v8x2 screen, then v8x2b): N 51

**Registration** (commit `8a0a48ac`): 5 add-alpha calls (`scratchpad/x2_add5.sh`; XIMP argv verbatim, A-1 without
`--rescreen`), `--parent v80 --name v8x2 --parent-spec scripts/specs/v8/lib-v80.json --fields
build-equity/train-2020-2023-lo3-fields-v13`, K1 plans of record under `x2-plans/` (PM6-9): every call exit 0. Counts =
the X list: roster 52 (5 in place at 26, 31, 33, 38, 41), recipe admission_trials 5 / new 5 / unchanged 47 / removed
bac, ind_adj_rev_5, ins_opp, iv_rv_spread, q5_eg_f49; q5_eg_f49g inherits max_extra_fields 6; K1 rows = XIMP's; the 47
unchanged rows = R-2's last plan (47 / 47). Library `46068582`, recipe `3e685f20`, spec `lib-v8x2.json` (12 pins
locked). Registration appended to `v8-prereg.md` ("Library v8x2").

**`run --screen`** (source `8a0a48ac`, then `4566ec94`):

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | result |
|---|---|---|---|---|---|---|
| u | 300 / 2,560 | 15.6 | 1,496 | 0 | `c6611af213db5f8ae3ebbb0421243c88080e629a3c52d1b60e81908fd98f242a` | `mega-v8-b0b-train-u-v8x2-1` |
| u-compare | internal | - | - | PASS | - | orientations IDENTICAL (47 objects; b adds 5); daily IC IDENTICAL (137,522 rows of 47 keys byte for byte; b adds 14,630 rows of 5) |
| fit | 180 / 1,536 | 3.1 | 443 | 0 | `0d34d64361fbd2f279df038b5cacd0426b84a94affa1f1430d30b11cadd34f8c` | `mega-weights-v8-r1-std-v8x2` |
| card | 300 / 2,560 | 19.2 | 1,398 | 0 | `dd856231668d4b9e9a08e125e833c976866abb596895ba175b120f408e9e8e5a` | `mega-cards-v8-r1-std-v8x2` |
| marginal (themes) | 360 / 1,536 | 0.3 | 2 | **1** | `374d531bafc2c9cb759a72b46a1b6931792d4090b7ab9a0be647b60f00c8b592` | refusal: "themes: pool member bac is not in --library" -> HARD-STOP exit 4 |
| marginal (pool only, PM6-8 (i)) | 360 / 1,536 | 141.1 | 296 | 0 | `e3f29f82523c1ab1bb529933850e06c9224411b9d1efbb82fc2aa9333f27ca0d` | `...-v8x2-marginal-poolonly` (report only) |
| gate p1-v8x2 | internal | - | - | PASS | - | **5 admission lines ledgered** (ledger 71 -> 76 lines) |

Spec-only fix (commit `4566ec94`; confirmed for replacing waves by PM7-32): `marginal.themes` deleted, `marginal.output`
-> `...-v8x2-marginal-poolonly`; lock / lock --write / dry 0 / 0 / 0, every pin unchanged; the screen resumed at marginal.

**Readings stated before any gate line was read** (scratch note, 15:45Z): "a refinement the gate does not admit
leaves the wave" (prereg section 6, PM7-18 b): R-a = status admitted AND runner sign = prior (the gate's own pass);
R-b = status admitted (what the prior-oriented fit weights). **Admission (gate p1-v8x2, v4-prior-v1): PASS, 2 of 5 with
the prior sign;** all 5 status admitted; q5_eg_f49g and ins_opp_buy runner sign +1 (agree); **bac_vq,
ind_adj_rev_5_nx, iv_rv_spread_xe runner sign -1 against prior +1** (the case where R-a and R-b differ). Reference
members vs R-2's admission: 0 status changes. STOPPED for the PM; **Ruling PM7-35: reading R-a** -- the three leave the
wave, bac / ind_adj_rev_5 / iv_rv_spread keep their pre-X strings at 0 trials; the 5 admission lines stay counted; for
additions (X-3) a status-admitted member with runner sign 0 stays (R-2 precedent) and one with runner sign opposite to
its prior is dropped from the wave; the same reading applies to X-4's re-screened replacements.

**Library v8x2b** (the cell): v80 with only q5_eg_f49g (38) and ins_opp_buy (41) (`scratchpad/x2b_add2.sh`, the same
argv, `--name v8x2b`; plans `x2b-plans/`); library `f60162a2`, recipe `24444852` (admission_trials 2, unchanged 50),
spec `lib-v8x2b.json` with PM6-8 (i) applied before any run. Registered in `v8-prereg.md` ("Library v8x2b").

| phase (v8x2b; source `86b0f202`) | caps | s | peak MiB | exit | receipt.json SHA-256 | output / result |
|---|---|---|---|---|---|---|
| u | 300 / 2,560 | 6.0 | 528 | 0 | `04869456798817f7de768fb17a1eb21e76db792ee86d3b8ad7429fd116bd50c7` | u-compare IDENTICAL (50 objects; 146,300 rows of 50 keys) |
| fit | 180 / 1,536 | 0.8 | 56 | 0 | `e379cf9688740c56b8355fc587bd9640cce05f71900bc5eb583533ce93aa1202` | `mega-weights-v8-r1-std-v8x2b` |
| card | 300 / 2,560 | 14.5 | 1,271 | 0 | `56e5d0747687e0fe9d732b353f7384c0b7b8923726b2be758242fe75212c5f6b` | - |
| marginal (pool only) | 360 / 1,536 | 134.6 | 251 | 0 | `b506021e45f7512904b3ab5d8b9bcdce1a8f8179320a681a27f563c485c7c6a0` | report only |
| gate p1-v8x2b | internal | - | - | PASS | - | 2 of 2 with the prior sign (q5_eg_f49g, ins_opp_buy); **0 lines appended, 2 already ledgered**; 0 reference status changes |
| ref (L 1.1474) | 180 / 1,536 | 43.3 | 586 | 0 | `bfc873982be8246e3eda349f60a7bc74e982b2815e099cd98b3158ccc0754b28` | **ref-s2-daily IDENTICAL** to R-2's S2 daily (`7cfe21c4`, 940,625 B): fields v13 reproduces the parent |
| w | 300 / 3,072 | 31.2 | 1,307 | 0 | `ae34838585c84c2b2f6cad5896f3e8c635726fdfceb20199c56d0b4ee0ee09a6` | `mega-v8-r1w-train-std-v8x2b-1` |
| nav (L 1.1474; step (1) = the cell) | 180 / 1,536 | 41.5 | 586 | 0 | `e9f43eb009e1d69ad1bbba9f2aaac6168d893da9a3439bedb7bbd7a26ca3ee3e` | `summary.json` `a9ed9f31`, S2 daily `ad9de313`, capacity `54c68d58`, recipe `f16fc8b1` |
| monitor / summ | 180 / 1,536 | 1.0 / 28.4 | 106 / 592 | 0 | - | `monitor.json` `47c1008c` (alarm); `cycle-v8x2b/summ.json` `3377c4a4`, `cycle_verdict.json` `d8c231e5` |
| one-sided p (PM5-23) | 180 / 1,536 | 0.8 | 479 | 0 | `010be337f42aa6d7d91cecf4f0c0ab057e8df6fc008691f2816a8110ec28acf3` | `v8-cells-x2-bundle.json` `39d7157f` |

**Gross match (step 1 at the parent's L 1.1474; mech.py, mechanics keys only): G 0.9862108210 vs G_parent 0.9859903463,
|diff| .00022 <= .005 -> the cell stands at L 1.1474** (no correction; no calibration run beyond the cell).
**Mechanics (S2, read before any return): PASS.** All-rows gross .9862 in [.90, 1.05] (post-ramp .9921); net +.0044
(<= .02); tau mean .02383 <= .20, p95 .02826 <= .30 (1,004 sessions; summary flags true); max gross 1.121, max |net|
.030; gross at score_begin .939; by year .963 / 1.000 / .986 / .996; 1,006 CSV rows, 1,005 return rows; accounting
7.8e-14 / 3.8e-16 (tol 1e-9).

**Statistics of record** (S2): net Sharpe **X-2 +1.2669** vs R-2 +1.2559. Paired (studentized CBB, block 21, seed
20260929, 4,999 resamples; 1,005 sessions): **dSR +.0109**, rho .993, **Memmel SE .0603** (t +.18); CBB 95% [-.123,
+.159]; LW SE .0724, 95% [-.138, +.160]; **bootstrap p one-sided .4502, two-sided .8882** (bundle = cycle). DSR (verdict,
`--dsr-ledger`, N 51): ledger DSR **.4878** (V[SR] 1.252e-03 per session from 22 window lines); effective-N .7280; legacy
.7681; PBO .3618.

**Capacity criterion (PM7-10, printed, decides nothing under PM7-34): net Sharpe at 4x NAV 1.1883 vs 1.1785 -> met.**
Turnover tau_gmv_mean .02383 vs .02393 (per unit gross .02417 vs .02427).

**Verdict (PM7-34: dSR > 0 AND mechanics): dSR +.011 > 0 AND mechanics PASS -> ACCEPTED, N 51.** Ledger line trial
`348d59bf4a3c3778` (cell = the v8x2b NAV dir, s2_net_sr 1.26688, origin prior, window research-window-v2, prev
`9bd909df`); ledger 77 lines (59 construction incl. the 8 W0-4 re-runs, 17 admission, 1 protocol), file `ce1e430d`, head
`3c84c5b2`. **Admission trials: v8 12 + X 5 (of 13 hand-written).**

Returns (S2, annual): net 4.64% (CAGR 4.67%) vs R-2 4.54%; gross of cost 5.92% (5.81%); trade cost .74%, borrow .34%,
long financing .20%; vol 3.66%; max drawdown 3.11% (3.25%); gross Sharpe 1.618 (1.610).

| year | rows | net return | net Sharpe | vol | tau | cost bps |
|---|---|---|---|---|---|---|
| 2020 | 252 | -.0038 | -.089 | .0359 | .0263 | 14.99 |
| 2021 | 252 | +.1010 | +2.649 | .0366 | .0227 | 11.56 |
| 2022 | 251 | +.0739 | +1.743 | .0416 | .0234 | 12.21 |
| 2023 | 250 | +.0187 | +.609 | .0315 | .0229 | 11.58 |

Capacity (report only; R-2 beside): net Sharpe .5x 1.305 (1.289), 1x 1.267 (1.256), 2x 1.237 (1.223), **4x 1.188
(1.178)**, 8x 1.106 (1.100); cost bps per traded dollar 10.76 / 12.58 / 14.79 / 16.99 / 18.78 (R-2 10.67 / 12.47 / 14.66 /
16.87 / 18.67). Disclosure (PM7-18 b): ins_opp_buy (B-3) is one of the refinements "chosen with TRAIN statistics in
view"; the X report prints the book with and without it.

**Next parent: `scripts/specs/v8/lib-v8x2b.json`, library v8x2b, L 1.1474, G 0.9862108210.**

### Cell X-3 (new-signal wave; library v8x3 on X-2): N 52

**Registration:** 8 add-alpha calls in the X list order (`scratchpad/x3_add8.py`: XSIG section 5 lines and the PM7-32
XDATA argv parsed verbatim, placeholders filled with v8x2b / v8x3 / `lib-v8x2b.json` / fields v13), each with its K1
plan of record (`x3-plans/`); every call exit 0. Counts = the X list: roster 60 (52 parent rows unchanged, their plan
rows = v8x2b's 52 / 52), recipe admission_trials 8 / new 8 / unchanged 52, no exception; K1 rows = XSIG section 5 and
the XDATA pre-check (bars / slots / extra): stmom 41/5/shares_out, earn_season 0/5/ea_days_to_expected+earn_season_rank,
k8_intensity 209/5/k8_count_63, inst_persist 272/6/inst_own_chg_q, nt_late 0/3/nt_first_126, div_season
0/2/div_month_pred, vol_beta 20/3/beta_dvol_21, season_y2_5 0/2/season_y2_5. Library `e5259541`, recipe `5c943ef7`,
registry `b55d8fdc`; `filing_events` is last in `V7_APPENDED_THEMES` (PM7-15 a). Marginal on the full pool with the
parent's theme weights (only additions). Registered in `v8-prereg.md` ("Library v8x3").

**IC-pass memory re-probe (PM7-31, P7; mechanics only):** the IC exe's metadata-only `--plan-only` on the final 60-member
library, role lo3, fields v13, `--workers 4`: at `--max-memory-mib 2560` the plan passes; at 1,024 it refuses with
**required_bytes 2,122,538,268 (2,024.2 MiB), max_compiled_slots 8** (R-2's library on v13: 2,122,534,172). Cap 2,560 MiB
(2,684,354,560 B) holds with 536 MiB of headroom: **the IC-phase cap stays 2,560 MiB** (W0-c). X-2's u pass peaked at
1,496 MiB.

**Wave composition, stated before X-3's screen (PM7-35):** an addition with status admitted and runner sign = prior
stays; status admitted and runner sign 0 stays (R-2 precedent); status admitted and runner sign opposite to its prior is
dropped (the cell then runs on v8x3b = v8x2b + the kept additions, same trial ids); a non-admitted addition stays at
weight 0 (R-2 / R-7 precedent); the gate stops the cell only if none of the 8 has its prior sign.

**`run --screen` of v8x3** (source `d0b329a9`; marginal on the full pool, no refusal):

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | result |
|---|---|---|---|---|---|---|
| u | 300 / 2,560 | 16.2 | 1,053 | 0 | `db8acdca448d73715e7bc3c273838ec17ec1d627b5033d62f7b5e8163df48700` | u-compare IDENTICAL (52 objects; 152,152 rows of 52 keys; b adds 23,408 rows of 8) |
| fit | 180 / 1,536 | 4.4 | 428 | 0 | `fbbe727c26d1483b83ec7d3d6856adb803194c2ab2e00591da4cfff91a424eee` | - |
| card | 300 / 2,560 | 22.0 | 1,333 | 0 | `c8a68dadf84d54f3fa1192cd24539a79994edf3c5e164d114fe5be28cdd780fe` | - |
| marginal (full pool) | 360 / 1,536 | 175.5 | 252 | 0 | `7f189543885f1dbd8ac9796f59fed94182ec0054be80027f16d9cdec862e2f79` | report only |
| gate p1-v8x3 | internal | - | - | PASS | - | **8 admission lines ledgered** (ledger 77 -> 85) |

**Admission (gate p1-v8x3): PASS, 5 of 8 with the prior sign; all 8 status admitted.** Runner sign = prior (+1):
k8_intensity, inst_persist, nt_late, vol_beta, season_y2_5. Runner sign 0: div_season (stays, R-2 precedent). **Runner
sign -1 against +1: stmom, earn_season -> dropped from the wave** (PM7-35, as stated above). Reference members vs
X-2's admission: 0 status changes. **Admission trials: X hand-written 13 of 13** (v8 12).

**Library v8x3b** (the cell): v8x2b + k8_intensity, inst_persist, nt_late, div_season, vol_beta, season_y2_5 = 58
(`scratchpad/x3b_add.py`, the same argv with `--name v8x3b`; plans `x3b-plans/`); library `32f8d69f`, recipe `698599bb`
(admission_trials 6, unchanged 52), spec `lib-v8x3b.json` (`2bd1dbf4`), marginal on the full pool. Registered in
`v8-prereg.md` ("Library v8x3b").

| phase (v8x3b; source `3b9bb3da`, then `028b2440`) | caps | s | peak MiB | exit | receipt.json SHA-256 | output / result |
|---|---|---|---|---|---|---|
| ref | - | - | - | skipped | - | fields equal the parent's (v13): identical by construction (the cycle's rule) |
| u | 300 / 2,560 | 26.6 | 673 | 0 | `3f4acb47b676e800982b3d03dffe534f88ff3dcefb5cdebd06f1ca30901e1d4e` | u-compare IDENTICAL (52 objects; 152,152 rows of 52 keys; b adds the 6 members and the `__combined__` row key, 2,926 rows each) |
| fit / card | 180 / 1,536; 300 / 2,560 | 0.8 / 15.0 | 55 / 1,390 | 0 | `35480d80...` / `a7a88870...` | - |
| marginal (full pool) | 360 / 1,536 | 167.0 | 296 | 0 | `4bad1955...` | report only |
| gate p1-v8x3b | internal | - | - | PASS | - | 5 of 6 with the prior sign (div_season sign 0); **0 lines appended, 6 already ledgered**; 0 reference status changes |
| w | 300 / 3,072 | 34.7 | 1,372 | 0 | `6e7dea427e5499ee9372d6c94a5406acd6770f3fd947680d516a7f50c3a1bb01` | `mega-v8-r1w-train-std-v8x3b-1` |
| nav step (1) (L 1.1474, calibration) | 180 / 1,536 | 41.6 | 586 | 0 | `cf580717c2611168d30c1328941163d40988f793e668120bb80e8d3fb967df13` | mechanics only |
| nav (L 1.1414, the cell; `lib-v8x3b-gm.json`) | 180 / 1,536 | 45.5 | 586 | 0 | `9752d6bd87a350151b4d6e657e361937c88ec7d8335cf66561dc697201aad3a8` | `summary.json` `e342db4f`, S2 daily `7b407f56`, capacity `318364f1`, recipe `3f448613` |
| monitor / summ | 180 / 1,536 | 1.3 / 30.5 | 121 / 581 | 0 | - | `cycle-v8x3b-gm/summ.json` `c062348f`, `cycle_verdict.json` `1486a397` |
| one-sided p | 180 / 1,536 | 0.8 | 578 | 0 | `3b7369bd6d7e66e3339a06c3050ddeda2b1f52ec3e2a94b50d53470c05e65a63` | `v8-cells-x3-bundle.json` `09d8f0d0` (FINAL v8x3b-gm vs BASE X-2) |

**Gross match:** step (1) at the parent's L 1.1474: G **0.9913686083** vs G_parent (X-2) **0.9862108210**, |diff|
**.00516 > .005** -> one correction: L' = 1.1474 x .9862108210 / .9913686083 = 1.141430 -> **1.1414**. Matched spec
`scripts/specs/v8/lib-v8x3b-gm.json` (`scratchpad/gmspec.py`: lib-v8x3b.json with name v8x3b-gm, one description
sentence, `nav.leverage` 1.1414, `nav.output` `...-loc-L1.1414-v8x3b`, `ref.leverage` 1.1474; lock / write / dry 0 / 0 /
0, every pin unchanged; file `8b1c1352`); commit `028b2440`. **Matched run: G 0.9861733264 vs .9862108210, |diff| .00004
(one correction).** The step-(1) run of lib-v8x3b.json is not a trial and is never resumed past nav.

**Mechanics (S2, matched run, read before any return): PASS.** All-rows gross .9862 (post-ramp .9922); net +.0051; tau
mean .02303 / p95 .02695 (1,004 sessions; flags true); max gross 1.122, max |net| .027; score_begin .936; by year .966 /
.998 / .984 / .998; 1,006 CSV rows, 1,005 return rows; accounting 4.9e-14 / 3.9e-16.

**Statistics of record** (S2): net Sharpe **X-3 +1.4205** vs X-2 +1.2669. Paired (1,005 sessions, 4,999 resamples):
**dSR +.1536**, rho .971, **Memmel SE .1200** (t +1.28); CBB 95% [-.091, +.398]; LW SE .1271, 95% [-.101, +.409];
**bootstrap p one-sided .0978, two-sided .2324**. DSR (verdict, N 52): ledger DSR **.6097** (V[SR] 1.229e-03 per session,
23 window lines); effective-N .8118; legacy .8455; PBO .2588.

**Capacity criterion (PM7-10, printed; decides nothing under PM7-34): net Sharpe at 4x NAV 1.3159 vs 1.1883 -> met.**
Turnover .02303 vs .02383 (per unit gross .02336 vs .02417); cost per traded dollar 12.53 vs 12.58 bps.

**Verdict (PM7-34): dSR +.154 > 0 AND mechanics PASS -> ACCEPTED, N 52.** Ledger line trial `1700a97fef85b059` (cell =
the L1.1414 v8x3b NAV dir, s2_net_sr 1.42049, prev `522dfa34`); ledger 86 lines (60 construction incl. the 8 W0-4
re-runs, 25 admission, 1 protocol), file `4e6988cd`, head `7f13f733`.

Returns (S2, annual): net 4.97% (CAGR 5.03%) vs X-2 4.64%; gross of cost 6.22% (5.92%); trade cost .72%, borrow .34%,
long financing .20%; vol 3.50%; max drawdown 2.87% (3.11%); gross Sharpe 1.779 (1.618).

| year | rows | net return | net Sharpe | vol | tau | cost bps |
|---|---|---|---|---|---|---|
| 2020 | 252 | +.0176 | +.493 | .0367 | .0252 | 14.98 |
| 2021 | 252 | +.0968 | +2.662 | .0349 | .0220 | 11.56 |
| 2022 | 251 | +.0632 | +1.690 | .0368 | .0228 | 12.11 |
| 2023 | 250 | +.0249 | +.812 | .0312 | .0220 | 11.50 |

Capacity (report only; X-2 beside): net Sharpe .5x 1.452 (1.305), 1x 1.420 (1.267), 2x 1.380 (1.237), **4x 1.316
(1.188)**, 8x 1.222 (1.106); cost bps per traded dollar 10.73 / 12.53 / 14.74 / 16.94 / 18.75.

**Next parent: `scripts/specs/v8/lib-v8x3b-gm.json`, library v8x3b, L 1.1414, G 0.9861733264.**

### Cell X-4 (value theme within FF49, XIMP C-1; library v8x4 on X-3): N 53

**Registration:** 9 add-alpha calls in roster order (`scratchpad/x4_add9.py`: C-1 DSL verbatim, each `--replaces
<member> --rescreen` with the replaced member's theme / tier / sign / prior-sign source / form / notes, citation + ";
within FF49: Ehsani, Harvey and Li 2023, FAJ"; `--parent v8x3b --name v8x4 --parent-spec
scripts/specs/v8/lib-v8x3b-gm.json --fields build-equity/train-2020-2023-lo3-fields-v13`), K1 plans `x4-plans/`; every
call exit 0. Counts: roster 58 (positions 0-8 re-screened in place), recipe admission_trials 0 / rescreens 9 / unchanged
49 (rows = v8x3b's 49 / 49); K1 rows = the C-1 table. Library `c7d52c68`, recipe `87effd86`, registry `7ff10f4e`. Spec
`lib-v8x4.json` (nav.leverage 1.1414 inherited; gate p1-v8x4 lists the 9 re-screens: add-alpha's rule for a wave of
re-screens only, so they are ledgered as admission lines, PM7-18 (c)); PM6-8 (i) applied before any run (the parent's
theme weights hold the replaced value members; marginal without themes); relocked (`2502950c`). Registered in
`v8-prereg.md` ("Library v8x4").

**Readings stated before X-4's screen:** (1) wave composition (PM7-35, "the same reading applies to X-4's re-screened
replacements"): a `_v49` re-screen stays only with status admitted AND runner sign = prior; otherwise its original
(FF12) member is restored at 0 trials (the cell then runs on v8x4b, same trial ids). (2) Criterion: PM7-34 (dSR > 0 AND
mechanics) decides; XIMP C-1's criterion (turnover per unit gross not higher, PM5-11's statistic) printed. (3) FF49
covered member cells (XIMP C-1's mechanical count, from the fields v13 payload metadata before the cell, score window
2020-2023): `grp_ff49` finite on **1,854,568 of 1,857,206** member cells (.99858) against `grp_ff12` 1,857,206 (1.0):
2,638 member cells (.14%) lose their value members under FF49.

**`run --screen` of v8x4** (source `58340960`):

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | result |
|---|---|---|---|---|---|---|
| u | 300 / 2,560 | 18.2 | 1,430 | 0 | `5ff4f9f42482d58ea6436e221e994db82f71b5bb2f2826b778e95acb3ad029c6` | u-compare IDENTICAL (49 objects; 143,374 rows of 49 keys; b adds 26,334 rows of 9) |
| fit / card | 180 / 1,536; 300 / 2,560 | 5.0 / 22.6 | 445 / 1,387 | 0 | `dfcbcb35...` / `e1959466...` | - |
| marginal (pool only, PM6-8 i) | 360 / 1,536 | 157.7 | 251 | 0 | `9d2e70d0...` | report only |
| gate p1-v8x4 | internal | - | - | PASS | - | **9 re-screen lines ledgered** (kind admission; ledger 86 -> 95) |

**Admission (gate p1-v8x4): PASS.** Status admitted with runner sign +1: **value_composite_v49, bm_v49,
net_payout_v49** (kept). reject_redundant (with value_composite_v49): ep_v49 (sign 0), cfp_v49, fcfp_v49, ebit_ev_v49
(sign 0), sp_v49. rd_me_v49: status admitted, runner sign 0. Reference members vs X-3's admission: 0 status changes. By
reading (1): **ep, cfp, fcfp, ebit_ev_f49, sp and rd_me keep their FF12 strings** (restored at 0 trials); the cell runs
on **library v8x4b** = v8x3b with value_composite, bm, net_payout re-screened in place (`scratchpad/x4b_add.py`, the
same argv with `--name v8x4b`; plans `x4b-plans/`): library `78dc39ad`, recipe `c45e92e6` (rescreens 3, unchanged 55),
spec `lib-v8x4b.json` with PM6-8 (i) before any run. Registered in `v8-prereg.md` ("Library v8x4b").

| phase (v8x4b; source `a595cd75`) | caps | s | peak MiB | exit | receipt.json SHA-256 | output / result |
|---|---|---|---|---|---|---|
| ref | - | - | - | skipped | - | fields equal the parent's |
| u | 300 / 2,560 | 26.3 | 673 | 0 | `76be1cb98e3ef680f040618f5110cdbb49393f194376c5531acd96cc6d005963` | u-compare IDENTICAL (55 objects; 160,930 rows of 55 keys) |
| fit / card | 180 / 1,536; 300 / 2,560 | 0.8 / 14.6 | 57 / 1,307 | 0 | `5a56dc9b...` / `bf760d80...` | - |
| marginal (pool only) | 360 / 1,536 | 143.8 | 251 | 0 | `f0ed8aaf...` | report only |
| gate p1-v8x4b | internal | - | - | PASS | - | 3 of 3 with the prior sign; 0 lines appended, 3 already ledgered; reference members vs X-3: 1 status change (fcfp reject_redundant -> admitted) |
| w | 300 / 3,072 | 41.5 | 1,371 | 0 | `5036d615051e0ab308d80b42886bc1010a419d36bcd96bcfb1d5e52dc74bf963` | - |
| nav (L 1.1414; step (1) = the cell) | 180 / 1,536 | 45.9 | 586 | 0 | `95523fba7e68ecf89f2eeada3ca88db0581acf8e75e1cc3d38684255bb0e3e7b` | `summary.json` `94c90ac1`, S2 daily `61950615`, capacity `965bd97e`, recipe `d93a6ed8` |
| monitor / summ | 180 / 1,536 | 1.3 / 30.2 | 121 / 608 | 0 | `04ef798e...` / `8559dc01...` | `cycle-v8x4b/summ.json` `008bc708`, `cycle_verdict.json` `9cfbf6cf` |
| one-sided p | 180 / 1,536 | 0.8 | 624 | 0 | `4f1f804bc04d84d032e4118968f35680f0a2f2f4bf3e1c1179f76fbb36eb6f83` | `v8-cells-x4-bundle.json` `ea89a295` (vs X-3) |

**Gross match:** G **0.9874558833** vs G_parent (X-3) .9861733264, |diff| **.00128** <= .005 -> stands at L 1.1414 (no
correction). **Mechanics PASS:** gross .9875 (post-ramp .9936), net +.0048, tau .02282 / p95 .02664, max gross 1.124,
max |net| .026, score_begin .937, by year .967 / .999 / .985 / .999, accounting 3.2e-14 / 4.5e-16.

**Statistics of record:** S2 net Sharpe **X-4 +1.3627** vs X-3 +1.4205: **dSR -.0578**, rho .997, **Memmel SE .0374**
(t -1.55); CBB 95% [-.126, +.014]; LW SE .0365, 95% [-.131, +.015]; **p one-sided .9434, two-sided .1210**. DSR (N 53):
ledger .5757 (24 window lines); effective-N .7774; legacy .8157; PBO .2692.

**Criterion (XIMP C-1 / PM5-11, printed; decides nothing under PM7-34): turnover per unit gross .023108 vs .023355 ->
met.** 4x net Sharpe 1.2594 vs 1.3159; cost per traded dollar 12.51 vs 12.53 bps.

**Verdict (PM7-34): dSR -.058 <= 0 -> NOT ACCEPTED, N 53** (rejected on dSR, not on the capacity criterion: PM7-34's
stop does not apply). Ledger trial `2280702d7080a6ec` (s2_net_sr 1.36266, prev `d4d2c955`); ledger 96 lines (61
construction incl. 8 W0-4 re-runs, 34 admission incl. 9 re-screens, 1 protocol), file `bf005971`, head `be896680`. Not
retried. **Parent stays X-3** (`lib-v8x3b-gm.json`).

Returns (S2): net 4.76% (CAGR 4.82%); gross of cost 6.01%; trade cost .71%, borrow .34%, long financing .20%; vol 3.50%;
max drawdown 2.86%; gross Sharpe 1.720. Years: 2020 +.0149 / .409; 2021 +.0960 / 2.710; 2022 +.0591 / 1.614; 2023 +.0240
/ .779 (net return / net Sharpe; tau .0250 / .0218 / .0227 / .0218; cost bps 14.94 / 11.53 / 12.09 / 11.49). Capacity: .5x
1.395, 1x 1.363, 2x 1.321, 4x 1.259, 8x 1.171.

### Cell X-5 (composition rule theme-erc-v1, XCOMB; template `x-theme-erc.json` on X-3): N 54

**Spec:** `scripts/specs/v8/x-theme-erc.json` with `"parent": "lib-v8x3b-gm.json"` (X-3, the last accepted cell; X-4 not
accepted), `lock --write` (commit `6a98af47`; reference_weights `82685a22`, reference_cell X-3's summary `e342db4f`). The
registered change verbatim (fit `--theme-erc theme-erc-v1`; outputs renamed); u and marginal are X-3's (done).

**Calibration (PM6-6 step 1, not a trial):** `run --stop-after nav` at the parent's L 1.1414: fit .8 s, card 16.3 s, gate
p1-v8x3b re-read PASS (0 appended, 0 status changes), w 43.0 s / 1,371 MiB, nav 45.1 s / 586 MiB
(`mega-nav-v8x-theme-erc`). Mechanics only (mech.py): all-rows S2 gross **.9604183310** vs G_parent .9861733264, |diff|
.0258 > .005 -> L' = 1.1414 x .9861733264 / .9604183310 = 1.17201 -> **1.1720**, set as nav.leverage in the template copy
`x-theme-erc-gm.json` (nav.output `mega-nav-v8x-theme-erc-L1.1720`; R-3 precedent; commit `9dde65fd`; lock verified).

**Theme shares (fit provenance, TRAIN decisions 1,004):** 11 themes, ERC dispersion 4.4e-16 (tolerance 1e-10), 10,000
sweeps: earnings_momentum .068, filing_events .048, investment_issuance .142, low_risk .123, options_implied .058,
ownership_flow .113, price_momentum .054, profitability_quality .108, reversal_seasonality .121, short_interest .079, value
.086 (parent: 1/11 = .091 each). Member cap 1/22 = .0455 bound on issuance_xbrl, iv_rv_spread, ins_opp_buy, inst_best_ideas
(one cap iteration); runner re-check to 1e-12.

**Matched run** (`run --stop-after nav`, then `run`; source `9dde65fd`): nav 45.4 s / 586 MiB at --aim-leverage 1.1720;
G **.9862260459** vs .9861733264, |diff| **.00005** <= .005 (no further correction). **Mechanics (S2, read before any
return): PASS.** All-rows gross .9862 (post-ramp .9917); net +.0051; tau .02684 / p95 .03073; max gross 1.112; max |net|
.027; score_begin .943 (all 5 scenarios .941 - .945); gross by year .967 / .995 / .985 / .997; accounting 1.1e-13 /
3.9e-16. monitor 1.3 s; summ 33.8 s / 603 MiB (`--dsr-n 54`, reference X-3's NAV). Spec sha256 `92131961`.

**Statistics of record** (S2; bundle `scratchpad/bundle.sh` X-3 vs X-5, block 21, seed 20260929, 4,999 resamples, bundle
`7805e136`): net Sharpe **X-5 +1.7695** vs X-3 +1.4205: **dSR +.3490**, rho .920, **Memmel SE .2011** (t +1.74); CBB 95%
[-.017, +.703]; LW SE .1868, 95% [-.041, +.740]; **bootstrap p one-sided .0288, two-sided .0782**. DSR (N 54): ledger
**.8291** (V[SR] 1.239e-03 per session, 25 research-window-v2 lines); effective-N .9405 (4 clusters); legacy .9580; PBO
.0384.

**Capacity criterion (registered: turnover per unit gross not higher than the parent's; printed, decides nothing under
PM7-34): .027219 vs .023355 -> unmet.** 4x net Sharpe 1.6549 vs 1.3159; cost per traded dollar 12.56 vs 12.53 bps.

**Verdict (PM7-34): dSR +.349 > 0 AND mechanics PASS -> ACCEPTED, N 54** (the registered turnover criterion is unmet; under
PM7-34 it decides nothing). Ledger trial `269cfc47be86d4a7` (s2_net_sr 1.76945, prev `be896680`); ledger 97 lines (62
construction incl. 8 W0-4 re-runs, 34 admission, 1 protocol), file `9f4aa4d9`, head `877cf36f`. Calibration run
`mega-nav-v8x-theme-erc` not ledgered (PM6-6).

Returns (S2): net 5.08% (CAGR 5.17%); gross of cost 6.45%; trade cost .84%, borrow .33%, long financing .20%; vol 2.87%;
max drawdown 2.06%; gross Sharpe 2.247. Years: 2020 +.0130 / .478; 2021 +.1038 / 3.282; 2022 +.0684 / 2.171; 2023 +.0232
/ .926 (net return / net Sharpe; tau .0290 / .0260 / .0265 / .0259; cost bps 14.95 / 11.61 / 12.20 / 11.58). Capacity: .5x
1.809, 1x 1.769, 2x 1.727, 4x 1.655, 8x 1.553. Read: the gain is a risk reduction (vol 2.87% vs 3.50%, net return 5.08% vs
4.97%) bought with 16% more turnover per unit gross.

**Next parent: `scripts/specs/v8/x-theme-erc-gm.json`, library v8x3b, theme-erc-v1, L 1.1720, G .9862260459.**

### Cell X-6 (capacity rule inv-vol-v1, XCOMB; template `x-inv-vol.json` on X-5): N 55

**Spec:** `scripts/specs/v8/x-inv-vol.json` with `"parent": "x-theme-erc-gm.json"` (X-5), `lock --write` (commit
`13488f89`; reference_cell X-5's summary `a03937cf`, reference_weights `8310da2c`). The registered change verbatim (nav
`--vol-scale inv-vol-v1`, NAV-only). **Deploy-key gap:** the cycle did not refuse (`plan` and `run` have no decide phase;
the template's "decide is out of scope" stands); the cell ran. Template note: it renames only nav.output, so the cycle
resolved the monitor phase to X-5's monitor output as done and no X-6 monitor ran (the verdict reads none of it; template
frozen, not edited).

**Calibration (not a trial):** `run --stop-after nav` at the parent's L 1.1720: nav 47.1 s / 586 MiB (`mega-nav-v8x-inv-vol`;
recipe vol_scale inv-vol-v1, floor fraction .25). Mechanics only: all-rows gross **1.0099371711** vs G_parent .9862260459,
|diff| .0237 > .005 -> L' = 1.1720 x .9862260459 / 1.0099371711 = 1.14448 -> **1.1445**, in the template copy
`x-inv-vol-gm.json` (nav.output `mega-nav-v8x-inv-vol-L1.1445`; commit `03be90ce`; lock verified).

**Matched run** (source `03be90ce`): nav 46.7 s / 586 MiB; G **.9861788467** vs .9862260459, |diff| **.00005** <= .005.
**Mechanics (S2, read before any return): PASS.** All-rows gross .9862 (post-ramp .9910); net +.0021; tau .02593 / p95
.02950; max gross 1.094; max |net| .025; score_begin .951 (all 5 scenarios .950 - .953); gross by year .970 / 1.001 / .981
/ .993; accounting 5.1e-14 / 4.3e-16. summ 28.3 s / 629 MiB (`--dsr-n 55`, reference X-5's NAV). Spec sha256 `10b57394`.

**Statistics of record** (S2; bundle X-5 vs X-6 `e11bb2bd`): net Sharpe **X-6 +1.6743** vs X-5 +1.7695: **dSR -.0952**,
rho .910, **Memmel SE .2141** (t -.44); CBB 95% [-.546, +.360]; LW SE .2324, 95% [-.585, +.395]; **p one-sided .6346,
two-sided .6914**. DSR (N 55): ledger .7720 (V[SR] 1.250e-03, 26 window lines); effective-N .9079; legacy .9381; PBO .0412.

**Capacity criterion (registered: net Sharpe at 4x NAV higher AND S2 cost_bps_traded lower; printed, decides nothing under
PM7-34): 4x 1.5725 vs 1.6549 (not higher); cost per traded dollar 10.97 vs 12.56 bps (lower) -> unmet.** Turnover per unit
gross .026295 vs .027219.

**Verdict (PM7-34): dSR -.095 <= 0 -> NOT ACCEPTED, N 55** (rejected on dSR). Ledger trial `60e3c75e3111f2db` (s2_net_sr
1.67426, prev `877cf36f`); ledger 98 lines (63 construction incl. 8 W0-4 re-runs, 34 admission, 1 protocol), file
`e8d9dcac`, head `a190f7ef`. Not retried. **Parent stays X-5.**

Returns (S2): net 3.91% (CAGR 3.96%); gross of cost 5.12%; trade cost .71%, borrow .31%, long financing .20%; vol 2.33%;
max drawdown 2.37%; gross Sharpe 2.195. Years: 2020 +.0139 / .571; 2021 +.0740 / 2.856; 2022 +.0592 / 2.760; 2023 +.0121
/ .556 (tau .0277 / .0247 / .0259 / .0253; cost bps 13.31 / 9.88 / 10.64 / 10.09). Capacity: .5x 1.721, 1x 1.674, 2x
1.616, 4x 1.573, 8x 1.493. Read: cost per traded dollar fell 13% as designed, but the book lost return (3.91% vs 5.08%)
faster than it lost risk.

### X batch 1 close

**Accepted X book = X-5** (`scripts/specs/v8/x-theme-erc-gm.json`: library v8x3b = R-2's v80 + X-2's q5_eg_f49g,
ins_opp_buy + X-3's six; composition theme-erc-v1; L 1.1720). S2: net Sharpe **1.7695**, net annual **5.08%** (CAGR
5.17%), gross of cost **6.45%**, x4 net Sharpe **1.6549**, turnover tau **.02684** (per unit gross .02722), max drawdown
**2.06%**, all-rows gross .98623. **Cumulative paired vs R-2** (`lib-v80.json`, bundle `907fda68`, block 21, seed
20260929, 4,999 resamples): S2 net Sharpe 1.7695 vs 1.2559, **dSR +.5135, Memmel SE .1841** (t +2.79), rho .933; CBB
95% [+.137, +.854]; LW SE .1848; **p one-sided .0032, two-sided .0098** (bundle's freeze-gate part: pass at alpha .10).
R-2 for reference: net 4.54%, gross of cost 5.81%, x4 1.1785, tau .02393, max drawdown 3.25%.

Cells: X-2 accepted (N 51), X-3 accepted (N 52), X-4 not accepted (N 53), X-5 accepted (N 54), X-6 not accepted (N 55).
Admission lines: v8 12 + X hand-written 13 of 13 + X-4 re-screens 9 = 34; X-5 and X-6 add none. Ledger 98 lines, head
`a190f7ef`, file `e8d9dcac`.

**Hidden-data record:** every run read TRAIN only (fields v13 `train-2020-2023-lo3`, sessions 2020-01-03 - 2023-12-29);
nothing dated 2024-01-01 or later opened; no 2013-2019 history read; `atx-db/` and `C:/atx-wt/pool-10` untouched; no
executable or tool-script change; no push.

**Open items:** (1) B-3: ins_opp_buy carries the PM7-18 b disclosure (refinement chosen with TRAIN statistics in
view) -- now in the accepted book. (2) The hand-written gm specs (`lib-v8x3b-gm.json`, `x-theme-erc-gm.json`,
`x-inv-vol-gm.json`) are not in the tests' NULL_PINS list. (3) Deploy path for theme-erc-v1 (decide / manifest pins) not
exercised in this batch. (4) `x-inv-vol.json` does not rename monitor.output (template defect, monitor-only). (5) X-5's
registered turnover criterion is unmet (+16% turnover per unit gross vs X-3); accepted under PM7-34 only.

## X batch 2 (cell X-7, the formulaic-alpha wave): XWQ merged; STOPPED at step 2 (the theme needs C++) (2026-10-02)

Integrator in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `3a6cdbc5` (clean; build v8-14, nothing
built). Read: integrator-rules; progress from the owner directive of 2026-10-02 to the end (PM7-33..37, X batch 1
lines); `v8x-prereg.md` sections 3, 11, 14; this log's "X batch 1"; `task-XWQ-report.md` (whole). Parent = X-5
(`x-theme-erc-gm.json`), N 55, X hand-written admission trials 13.

**No X-7 string was registered or screened. No data process ran. No admission or construction line (ledger 98 lines,
file `e8d9dcac`, N 55, admission trials unchanged). No IC, return, Sharpe, turnover or NAV number of any X-7 string exists
or was read.**

### 1. Lane XWQ merged (tests only fixed)

`git merge --no-ff 7bf8e68e` (lane `feat/platform-v8-xwq-20261002`, pool 12, base `bc153439`) -> **`03fcf92f`**, no
conflict: 5 new files (+2,370), `atx-engine/tools/{research_fields_ohlc,prepare_research_fields_ohlc,
test_research_fields_ohlc}.py`, `xwq_check.py`, `task-XWQ-report.md`; no C++, no existing file changed.

| suite | result |
|---|---|
| atx-engine/tools (whole) | **300 passed**, 6 subtests (293 + the lane's 7) |
| atx-impl/strategies (whole) | **163 passed** |
| atx-impl/tools (whole; `ATX_EQUITY_BIN`, `ATX_EQUITY_TARGETS_EXE` absolute, v8-14) | **612 passed, 1 skipped** (`ATX_EQUITY_ROOT` unset), 17 subtests |
| scripts/tests (whole; `ATX_EQUITY_BIN` v8-14), before the fix | 27 failed, 221 passed, 3 skipped |
| scripts/tests (whole), after `4d0c8d8f` | **248 passed, 3 skipped, 0 failed** (the RESEARCH_CYCLE_LIVE_ROOT skips) |
| `xwq_check.py` | **`xwq_check: PASS`** (101 rows, 14 picks, 59 mutants fail, 14 add-alpha lines) |
| `xsig_check.py` | **`xsig_check: PASS`** |

The 27 failures predate the merge (X batch 1's commits; scripts/tests was not run after X-3) and are fixed in tests only,
commit **`4d0c8d8f`**:
- `test_research_spec.py`: the three batch-1 gm specs join NULL_PINS (`lib-v8x3b-gm.json` = `lib-v81-gm.json`'s set,
  `x-theme-erc-gm.json` and `x-inv-vol-gm.json` CHILD_NULLS); without them `set(V8_SPECS) == set(NULL_PINS)` failed in
  every parametrized case (24) and the two whole-file tests. Two consequences of registering them: (a) the nominal plan
  allows `ref` skipped when the spec's fields are its baseline fields (`lib-v8x3b-gm.json` runs on X-2's fields v13: the
  cycle's documented ref-skip rule); (b) the live chain check treats a hand copy of an add-alpha spec
  (`ADD_ALPHA_COPIES`: `lib-v81-gm.json`, `lib-v8x3b-gm.json`) like an add-alpha parent (it has no base-only pins
  `identity_bridge`, `fund_events`), as the check's own comment already did for `lib-v80.json`.
- `test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry`: X-3's `k8_intensity` and `inst_persist`
  read `k8_count_63` and `inst_own_chg_q`, two of the template's 12 mined fields. Prereg A3 re-applies the field rule at
  the campaign lock (H-F); the test now reads the rule on the registry without the `v8x` members (the registration) and
  asserts the X members only remove fields. **For the lock (A3):** mine-c1 then has 10 fields, B = 110 (not 132).

### 2. PM7-36 (a), theme `price_volume`: STOPPED -- the change needs C++

Made as integration 8 item 5d made `filing_events` (registry `themes` table + `V7_APPENDED_THEMES` + the test literals),
uncommitted, then restored by hand (the session refused `git checkout`; tree clean, `git status` empty at `4d0c8d8f`).
With the registry row and `V7_APPENDED_THEMES = ("ownership_flow", "filing_events", "price_volume")`, 6 Python tests fail:
five are literal pins of the kind 5d edited (`test_composition_resid.py:244`, `:283`, `:676`;
`test_fit_composition_weights.py` OwnershipFlowTheme constants and refusal text; `test_fit_composition_weights_store.py`
refusal text). The sixth is not: **`test_composition_resid.py:266`** (finding R6B-O-4) pins the IC runner's copy of the
registered theme order, `theme_resid_order` in **`atx-impl/src/strategy_ic_theme_resid.hpp`** (`std::array<..., 11>`,
ending `filing_events`), to extend the fitter's `PRIOR_THEMES`. With the five literals updated, it still fails:
`cpp[:12] != PRIOR_THEMES` (the C++ list lacks `price_volume`). For `filing_events` the C++ list already held the theme
(lane R-11), so 5d was Python only; for `price_volume` it is not.

What the C++ list does: it is read only under a weights file's `theme_residualise` block (theme-resid-v1, R-11, not
accepted, not on X-5's path); there it refuses a weighted theme outside the list. X-7 itself does not read it:
admission reads the registry's `themes` table (`fit_composition_weights.prior_themes`), and theme-erc-v1 orders the
themes by name in the weights file, which the runner's theme-erc table takes as recorded (at most 32 themes). The pin
exists so that a theme-resid fit is never admitted and fitted by the fitter and then refused by the runner.

Options for the PM (not taken): (i) one C++ constant (`theme_resid_order` 11 -> 12 entries, `"price_volume"` last; the
pin literal `test_composition_resid.py:267` follows), build `atx-equity-strategy-ic,atx-impl-strategy-ic-tests` on a
new tag, flag-absent identity (X-5's w pass) before X-7; (ii) a ruling that relaxes R6B-O-4 for themes registered after
the runner's copy (Python only; theme-resid-v1 with a `price_volume` member would then be refused by the runner); (iii)
another theme for the 8 `price_volume` picks (a change of the XWQ registration).

**How theme-erc-v1 treats a twelfth theme (stated now, before any X-7 screen; holds under any option above):** the
parent X-5 already has 11 themes with a member (`filing_events` included). `price_volume` would be the twelfth.
theme-erc-v1 (`composition_theme_erc.py`, registered by XCOMB) takes T = the themes with a weighted member, in sorted
name order (`price_volume` between `price_momentum` and `profitability_quality`, whatever its registry position): the
theme sleeves are re-built from the kept members with the parent rule's (ew-theme-std-v1) within-theme tier-score
shares, all T theme shares are re-solved by ERC on the new T x T TRAIN covariance (10,000 sweeps, dispersion <= 1e-10),
and the member cap is 1/(2T) (1/24 at T = 12; 1/22 if no `price_volume` member stays). No share is set by hand; the
existing themes' shares move only through the re-solve. The 3 picks in existing themes (`wq_035`, `wq_030`, `wq_043`
in `reversal_seasonality`; `wq_101` in `price_momentum`) enter those sleeves the same way.

### 3. and 4. Not started

Fields v14 (v13 + `open_adj`, `high_adj`, `low_adj`; XWQ section 7 driver; 0 trials) and X-7 (12 strings; #33 and #38
withdrawn by PM7-36 (c)) wait for the PM's ruling on step 2: the dispatch orders them after the theme.

### Hidden-data record (X batch 2)

- Opened by tools: the test suites' synthetic fixtures and committed files only; no role, field payload, IC, NAV or
  ledger content (the ledger's line count and file hash only).
- Read by me: rulings, reports, sources, test output. **Nothing dated 2024-01-01 or later; no 2013-2019 history; no X-7
  statistic exists.** `atx-db/` and `C:/atx-wt/pool-10` untouched; no build; no push.

### Open items (X batch 2)

- **For the PM:** the ruling on step 2 (options above). Then: theme step, fields v14, X-7 as dispatched.
- Head `4d0c8d8f` before this log commit; ledger 98 lines (`e8d9dcac`), N 55, X hand-written admission trials 13.
- Disk 121,810,661,376 B free (113.4 GiB).

## X batch 2, resumed (PM7-39, PM session 8): identity under v8-15, fields v14, X-7, campaign v9-mine-c1, X-9 (2026-10-02)

Integrator (root) in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `798d3b23` (clean). Read: CLAUDE.md
build rules, integrator-rules, progress "PM session 7" to the end (PM7-1..39), `v8x-prereg.md` (whole), this log from V8-F
to the end, `task-XWQ-report.md` (sections 1-3, 5-11), `review-x5-theme-erc.md`. Rulings PM8-1..PM8-4 recorded verbatim in
`progress.md` ("PM session 8", commit `183d9184`) before any measurement. Parent X-5 (`x-theme-erc-gm.json`), N 55.

### 1. Identity under build v8-15 (PM7-39 / PM7-30): verified from disk, tests run, logged

A previous root session built and ran the identity but did not log it. Verified from disk, not from its report:
- **Build v8-15** (`scripts/research-build.ps1`, receipt `build-equity/mega-v8-15-receipt.json` `afca7cbe`): source
  `798d3b23` (theme `price_volume` 12th and last in `theme_resid_order`), DirtyEntries 0, preset equity-dev, targets
  `atx-equity-strategy-ic`, `atx-impl-strategy-ic-tests`, exit 0, 10.4 s, 3 TUs, 3 links; ConfiguredProvenance `5c65cee8`
  (configure-time, as v8-14). Executables re-hashed on disk = the receipt: **atx-equity-strategy-ic `6aac48f2...1d59`**,
  ic-tests `d6cc36ae...7dd1`. Unchanged from v8-14 (re-hashed): targets `a95f6f0a`, mine `2176fa4a`, risk `d7e424b2`.
- **X-5 identity** (bounded runner, source `798d3b23` clean, argv = X-5's recorded receipt with only `--output`
  renamed, checked token by token: 1 differing token each):

| pass | X-5 receipt | new receipt (`receipt.json` SHA-256) | exe | s / MiB | result |
|---|---|---|---|---|---|
| fit (180 / 1,536) | `mega-weights-v8x-theme-erc-run1` | `v8-i15-x5fit-run` `f3da276b69a3b04366d8bd232c8454878d81f10ca29892238fe3941d6d7ca1e9` | python (fitter) | 0.8 / 57 | `admission.csv` byte-identical; `admission.json` differs only in `inputs/script_sha256` (`4c529f5f` -> `270068c8`: `fit_composition_weights.py` gained `price_volume` in `V7_APPENDED_THEMES`); `composition_weights.json` only in `provenance/script_sha256`, `provenance/admission_sha256` (`5f5b3edb` -> `209e0b26`) and `provenance/std/registry_sha256` (`7ff10f4e` -> `19b01d11`: the `price_volume` theme row). **Both files byte-identical after substituting those values back** (weights `8310da2c`, admission `5f5b3edb`), the PM7-30 allowed list; stdout one JSON line differing in those hashes, output path and seconds |
| w (300 / 3,072) | `mega-v8xw-train-theme-erc-run1` | `v8-i15-x5w-run` `ae7f49f5311dd3ceb350e83d637a0e6aeda15cb27b9b2d3cb7295607597635f9` | ic v8-15 `6aac48f2` (X-5 ran v8-14 `67f72921`) | 39.6 / 1,370 | **10 of 12 byte-identical**; `summary.json` (238 paths) and `train_candidates.jsonl` (229) differ **only in timing paths** (`stage_seconds.*`, `wall_seconds`, `hash_seconds`); stdout 117 of 177 lines differ only in the timing tokens `seconds=`, `ic=` (ic seconds), `composition=` (composition seconds) |
| NAV (180 / 1,536) | `mega-nav-v8x-theme-erc-L1.1720-run` | `v8-i15-x5nav-run` `4b093261c7f5099db493e571176a8c32aa26a028e6fc9f45f9b77bda73cb9fa2` | targets `a95f6f0a` | 45.4 / 586 | **27 of 27 byte-identical** (S2 daily, `summary.json`, `capacity/`); stdout 1 of 17 lines differs, on the output path only |

- **Tests on v8-15 (run now; none had been recorded):** `atx-impl-strategy-ic-tests` whole **159 / 159** (xml
  `build-equity/v8-i15-ic-tests.xml` `dcc6e4c9`); `atx-impl/tools` whole (`ATX_EQUITY_BIN`, `ATX_EQUITY_TARGETS_EXE`
  absolute) **612 passed, 1 skipped**; `atx-impl/strategies` **163 passed**; `scripts/tests` whole **248 passed, 3
  skipped, 0 failed**.

**X-5 reproduces under v8-15** (w and NAV byte for byte; fit with the three provenance hashes of the PM7-30 kind). X-7
runs on v8-15. No statistic was read (comparisons by SHA-256, JSON paths and masked log tokens only). Disk 122,762,747,904
B free (114.3 GiB).

### 2. Fields v14 lo3 (XWQ section 7 driver; 0 trials; caps W0-i) -- plan, written before the run

Command = fields v13's recorded argv (receipt `train-2020-2023-lo3-fields-v13-run`), built token by token by
`scratchpad/fv14.py`, 5 tokens changed and no other: (2) the driver of XWQ section 7 verbatim (`... import
prepare_research_fields_ohlc as o; d.register(vars(b)); x.register(vars(b)); o.register(vars(b)); b.main(sys.argv[1:])`);
(8) `--output build-equity/train-2020-2023-lo3-fields-v14`; (10) `--fields` = v13's 75 names in v13's order +
`open_adj,high_adj,low_adj`; (64) `--reuse build-equity/train-2020-2023-lo3-fields-v13`; (66) `--reuse-sha256 e5f7f28c...`
(v13's manifest). `--reuse-hardlink` and `--price-source` (the role's TickerHistory3) as v13. Builder `--max-rss-mib 2048
--max-seconds 580`; runner `--seconds 600 --max-rss-mib 2560 --min-free-mib 512`, binding v13's ten tool modules +
`research_fields_ohlc.py` + `prepare_research_fields_ohlc.py`, the role manifest and v13's manifest. **Expected: 75 reused
/ 3 computed; the 75 payloads bit-identical to v13's (hardlinks); seal 2024-01-01.** A different count or a refusal stops
the step (PM8-4 b).

| step | receipt dir | source | outcome / exit | s | peak MiB | receipt.json SHA-256 | output manifest SHA-256 |
|---|---|---|---|---|---|---|---|
| fields v14 lo3 | `train-2020-2023-lo3-fields-v14-run` | `26bddfbb` | completed / 0 | **72.8** | **277** | `e389a4932f65d53228323435a1a9dbd81a6a06beb1ff4597eaf10de5ee389900` | **`4b12c0e1d90d8ab5cfa6e6281d104a4b8d229a42060505c39217f97d616cbb0c`** |

Receipt: `clean in the code pathspec`, dirty outside none, min system free 871 MiB; stdout `0023e693`, stderr empty.
Manifest (metadata only): status complete, `seal.exclusive_end` 2024-01-01, role lo3 (1,405 dates, 2018-06-01 -
2023-12-29, `e1c67101`), **78 rows** (v13's 75 + `open_adj`, `high_adj`, `low_adj`), builder `code_sha256_lf` `74df97f9`
(= v13's), 661,711 B. **Counts: reused 75, computed 3 = the expected 75 / 3**: `open_adj` `13e4ba54` ohlc-open-adj-v1,
`high_adj` `e7d4100c` ohlc-high-adj-v1, `low_adj` `e9a3a7b2` ohlc-low-adj-v1, producer `research_fields_ohlc.py`
(`code_sha256_lf` `26356cf6`), clock ohlc-same-session-v1 (PM7-36 b). The 75 v13 payloads: entry sha256 = v13's (75 /
75), hardlinks of the v13 files (75 / 75); all 78 payloads re-hash to their pins. Dir 4.9 GiB apparent (hardlinks). Logs
scanned for 2024-2029 date tokens: none; the manifest's `source_checks` carry the seal date and two calendar `last`
metadata entries (`sec`, `v9/nt_first_126`) unchanged from v13 (not read).

### 3. Cell X-7 (formulaic-alpha wave, 12 XWQ strings on X-5)

**Preconditions (0 trials).** Registry field rows `open_adj`, `high_adj`, `low_adj` (commit `2ebbdbe0`, registry
`1069d88c`): formula id, origin `fields_x7`, producer and basis = XWQ L2 verbatim; clock = the fields v14 manifest row
verbatim for all three (XWQ: "root copies clock and basis checks against the build of record"; the L2 open_adj clock
differs from the build's by the word "its"); formula ids checked equal to the v14 manifest rows. `atx-impl/strategies`
163 passed. Theme `price_volume` in the registry since `798d3b23`.

**K1 pre-check (metadata only; `scratchpad/x7_add.py` without `--go`: each frozen line verified against its printed
SHA-256 prefix, the four placeholders filled -- parent `v8x3b`, name `v8x7`, parent spec
`scripts/specs/v8/x-theme-erc-gm.json`, fields v14 -- and the exe's (v8-15) `--plan-only` on parent + that string; plans
in the scratchpad, not of record).** 11 of 12 rows = XWQ section 6 (bars / slots / extra fields; the exe's node count
exceeds the mirror's by one for wq_055, wq_006, wq_002, wq_043, wq_044, as X batch 1). **wq_099: exe 104 bars / 8 slots
/ high_adj, low_adj against XWQ's 7 slots** (the XSIG mirror's count of the `b > a` order): over the house budget
(`max_slots` 7), so add-alpha's K1 refuses it as frozen.

**Rule applied (registered, written before any variant is planned).** XWQ section 6 (the registration of X-7, accepted by
PM7-36): "a string K1 refuses is rewritten only mechanically (same semantics) or withdrawn at 0 trials"; XWQ E5 / XWQ-e
(PM7-36) fixes what "mechanically" means: the same formula with the operands of a commutative op or of a comparison
swapped, proved identical by `xwq_check.canonical()`. So: the 64 variants of the frozen wq_099 DSL generated by the six
swaps E5 allows (the comparison `a > b` / `b < a`; `+` in the mid price; `*` in the dollar volume; `*` in `-1 * x`;
the operands of each of the two `correlation` calls), each checked `canonical(variant) == canonical(frozen)`, are planned
by the v8-15 exe (`--plan-only`, metadata only, parent + variant). The string registered is the variant that plans within
the house budget with **the fewest swaps**, ties broken by (i) no `correlation` operand swap before one (a `+`, `*` or
comparison swap is exact in IEEE arithmetic, a correlation swap is exact only if the kernel is symmetric), (ii) the
leftmost swapped node in the frozen text. If none fits, wq_099 is withdrawn at 0 trials and X-7 screens 11 strings.
Every other byte of the frozen line (theme, tier, sign, citation, formula, domain, deviation) is unchanged; the formula
note "written b > a for the 7-slot budget" then describes the mirror's order, not necessarily the registered one (the
registered DSL is the record). No IC, return or statistic exists for any X-7 string.

**Variants planned** (`scratchpad/x7_wq099_variants.py`, output `x7-wq099-variants.json`): 64 of 64 canonical-equal to the
frozen string; every one 104 bars, high_adj / low_adj, 28 nodes; exe slots: 8 without the `-1 * x` swap and without the
comparison swap, 9 with the comparison swap alone, **6 with the `-1 * x` swap**, 7 with both. 32 fit. **Chosen by the
rule: one swap, `(x * -1)` for `(-1 * x)`** (no correlation swap; it is also the paper's printed order `(... * -1)`):
`rank(decay_linear((((rank(correlation(low_adj, volume, 6)) > rank(correlation(ts_sum(((high_adj + low_adj) / 2), 19),
ts_sum(ts_mean((raw_close * volume), 60), 19), 8))) ? 1 : 0) * -1), 21))`, SHA-256 in the JSON; K1 104 / 6 /
high_adj, low_adj. Multiplication by -1 is exact in either order, so the values equal the frozen string's bit for bit.

**Registration** (commit `a12854ff`; `v8-prereg.md` "Library v8x7", `ce994900`): 12 add-alpha calls
(`scratchpad/x7_add.py --go`: each printed line verified against its SHA-256 prefix; placeholders parent `v8x3b`, name
`v8x7`, parent spec `scripts/specs/v8/x-theme-erc-gm.json`, fields v14; wq_099 with the rewrite above; `--plan-json
x7-plans/<id>.json`, the plans of record made by the v8-15 exe on the exact cumulative library bytes, PM6-9); every call
exit 0. Counts = the registration: roster 70 (58 parent rows unchanged), recipe admission_trials 12 / new 12 / unchanged
58, no exception; K1 rows = XWQ section 6 (wq_099 104 / 6). Library `fd966384`, recipe `6131f1ad`, registry `f0ee2288`,
spec `lib-v8x7.json` (12 pins locked; fit flags inherited from X-5 incl. `--theme-erc theme-erc-v1`; nav L 1.1720).
IC-pass memory (mechanics only): 70 members, fields v14, `--workers 4`: passes at 2,560; refuses at 1,024 with
required_bytes 2,122,543,388 (2,024.2 MiB): **cap unchanged**.

**`run --screen` of v8x7** (source `ce994900`):

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | result |
|---|---|---|---|---|---|---|
| u | 300 / 2,560 | 58.5 | 1,244 | 0 | `e2c98d1dfab3927f253cf4cf5305ef08615ed68b2cbd6fb635eb78df760111a1` | u-compare IDENTICAL (58 objects; 169,708 rows of 58 keys byte for byte; b adds 35,112 rows of 12) |
| fit | 180 / 1,536 | 14.9 | 443 | 0 | `d81f454d607423b3955f1659e2062e66528507f42b4a9b248d31f1f9821a6163` | - |
| card | 300 / 2,560 | 60.0 | 1,554 | 0 | `1a0a68ccc35ad4f58d02f48d8a8adda5016c54a3435085ac07fcac1db9953e2d` | - |
| marginal (themes) | 360 / 1,536 | 0.5 | 12 | **1** | `b5d1252777cebdab15e9f8d698c3d60d789a79ad7b87f1040008046e712f7d1e` | refusal: "marginal IC: themes: 1..10 weighted themes (the composite plus the themes may not exceed 11 regressors)" -> HARD-STOP exit 4 |

**Marginal refusal: cause and the rule applied.** The pool's weights file is X-5's (theme-erc-v1), which weights 11
themes (`filing_events` included); the marginal verb takes at most 10 theme regressors beside the composite. Any wave
on X-5 hits it; it is not caused by the X-7 strings. The marginal phase gates nothing (the gate reads the admission
only); the registered remedy for a marginal refusal is PM6-8 (i) (spec-only: delete `marginal.themes`, new
`marginal.output`, relock, resume at marginal; standing for replacing waves by PM7-32), and it changes no number that
decides anything (the K6 diagnostic is residualised on the parent's combined signal alone). Applied under PM8-4 (a
registered rule covers the case; no reading changes a number); logged here: `marginal.output`
`...-v8x7-marginal-poolonly-b`, `themes` deleted; `lock` / `lock --write` leave every pin unchanged (diff: the marginal
block only).

| phase (source `1ab1fd89`) | caps | s | peak MiB | exit | receipt.json SHA-256 | result |
|---|---|---|---|---|---|---|
| marginal (pool only, `-poolonly-b`) | 360 / 1,536 / 512 free | 259.9 | 296 | 15 | `9276d5036d49809edcd0db35a405875dabc750a07a114ac4551c9e0e7d68518e` | **system-memory-limit**: host free memory fell to 506 MiB (other sessions' builds: a vcpkg grpc build, 17 `cl.exe`, and `C:/atx/build-server`); no output dir written, nothing read |

A failed run with no output (rule 7 blind re-run, adds nothing). Waited until no compiler ran and 7.7 GiB were free; the
receipt dir is never overwritten, so `marginal.output` -> `...-v8x7-marginal-poolonly-c` (spec-only, pins unchanged).

| phase (source `6c3ad86c`) | caps | s | peak MiB | exit | receipt.json SHA-256 | result |
|---|---|---|---|---|---|---|
| marginal (pool only, `-poolonly-c`) | 360 / 1,536 | 331.5 | 296 | 0 | `faef9217df83b5cfc252c0c548925269e6538b515b5aa3387e5bbb6136068a5e` | report only (92% of the 360 s cap) |
| gate p1-v8x7 | internal | - | - | PASS | - | **12 admission lines ledgered** (ledger 98 -> 110 lines, file `8bc57f2f`, head `af281c31`) |

**Admission (gate p1-v8x7, v4-prior-v1): PASS, 8 of 12 with the prior sign.** Status admitted, runner sign +1 = prior:
wq_099, wq_035, wq_055, wq_006, wq_095, wq_085, wq_043, wq_044. **wq_002: status admitted, runner sign -1 against +1.**
**wq_101: status reject_turnover (tau .7042 > .70), runner sign -1 against +1.** wq_030: reject_redundant (with wq_035),
runner sign +1. wq_014: reject_redundant (with wq_006), runner sign +1. Reference members vs X-5's admission: 0 status
changes. **Admission trials: X hand-written 13 + 12 = 25** (v8 12; X-4 re-screens 9; 46 admission lines).

**Wave composition (PM7-35, applied as ruled; the readings were stated before X-3's screen):** wq_002 and wq_101 have a
runner sign opposite to their prior -> **dropped** (PM7-35: "Additions (X-3, X-7): ... runner sign opposite to the prior
is dropped"; wq_101 is also not admitted, so under the other wording of the X-3 statement, "a non-admitted addition
stays at weight 0", it would sit in the library at weight 0: the fit gives it no weight, theme-erc-v1 counts only
weighted members, and the combined signal and NAV are the same either way, so no number depends on the reading).
wq_030 and wq_014 (status reject_redundant, prior sign) **stay at weight 0** (R-2 / R-7 precedent). The cell runs on
**library v8x7b = v8x3b + wq_099, wq_035, wq_055, wq_006, wq_095, wq_085, wq_030, wq_043, wq_014, wq_044** (68; the same
strings and trial ids: 0 new admission lines).

**Library v8x7b** (commit `eabf55fd`): 10 add-alpha calls with `--name v8x7b` (same argv; plans `x7b-plans/`), every call
exit 0; library `81db1d30`, recipe `f205da12` (admission_trials 10, unchanged 58), registry unchanged (`f0ee2288`), spec
`lib-v8x7b.json` with PM6-8 (i) applied before any run. Registered in `v8-prereg.md` ("Library v8x7b").

| phase (v8x7b; source `eabf55fd`) | caps | s | peak MiB | exit | receipt.json SHA-256 | result |
|---|---|---|---|---|---|---|
| u | 300 / 2,560 | 13.3 | 530 | 0 | `c7343357050e5ac02fe68667a9ce54e87852695c713443301716e8461eee6816` | u-compare IDENTICAL (58 objects; 169,708 rows of 58 keys; b adds 29,260 rows of 10) |
| fit / card | 180 / 1,536; 300 / 2,560 | 2.2 / 35.3 | 58 / 1,450 | 0 | `fd069a33...` / `24637480...` | - |
| marginal (pool only, `-poolonly-b`) | 360 / 1,536 | 360.5 | 251 | 15 | `7222547ab0d1d2d1669f3ec63e094b65562741f2903eeb7e33afc3b61503e61f` | **time-limit** (the 360 s cap of PM7-31); no output dir, nothing read |

**The time cap and what was done.** v8x7's marginal (70 members) took 331.5 s; v8x7b's (68) hit 360 s while the host was
loaded by other sessions (CPU 46-61%: VS Code / pylance at 320% of one core, Defender at 100%, a `C:/atx/build-server-rel`
clang-cl build). The phase is report only (it gates nothing). A run killed by the runner leaves no output: rule 7's
blind re-run (as the campaign runbook treats a time-limit: remove the cause without reading anything, re-run the same
spec). Cause removed by waiting for a quiet host (no compiler, CPU under 25% for 30 s); `marginal.output` ->
`...-v8x7b-marginal-poolonly-c` (spec-only, pins unchanged). **If the blind re-run hits the cap again, root stops (PM8-4
(c)).**

| phase (v8x7b; source `4c4fad7e`) | caps | s | peak MiB | exit | receipt.json SHA-256 | output / result |
|---|---|---|---|---|---|---|
| marginal (pool only, `-poolonly-c`; blind re-run, quiet host) | 360 / 1,536 | 253.1 | 296 | 0 | `203ba856d5105156cde2cdb1743c186724f931be05af7653a05096e53d35e362` | report only |
| gate p1-v8x7b | internal | - | - | PASS | - | 8 of 10 with the prior sign (wq_030, wq_014 reject_redundant, as in v8x7); **0 lines appended, 10 already ledgered**; 0 reference status changes |
| ref (L 1.1720, fields v14) | 180 / 1,536 | 59.0 | 586 | 0 | `5d8151ca9cc07dd483d9d09f99f9d4bacc1452233daf54779a4515c5cbda0ad3` | **ref-s2-daily IDENTICAL** to X-5's S2 daily bit for bit (`529062d6`, 941,374 B): fields v14 reproduces the parent |
| w | 300 / 3,072 | 53.5 | 1,436 | 0 | `e4a1f7d1909b6b1bf0ba27ba8c821ff571bd00d1216c27628c6435b342455986` | `mega-v8xw-train-theme-erc-v8x7b-1` |
| nav step (1) (L 1.1720, calibration) | 180 / 1,536 | 55.3 | 586 | 0 | `faa808f397f0b80ffb40490b768599a3f739a6ad0e607a44ee8511d251b3c365` | mechanics only |

**Gross match (PM6-6; `scratchpad/mech.py`, mechanics keys only):** step (1) at the parent's L 1.1720: all-rows S2 gross
**0.9111922356** vs G_parent (X-5) **0.9862260459**, |diff| **.07503 > .005** -> one correction: L' = 1.1720 x
.9862260459 / .9111922356 = 1.26851 -> **1.2685** (inside the executable's [1, 2]). Matched spec
`scripts/specs/v8/lib-v8x7b-gm.json` (`scratchpad/gmspec.py`: lib-v8x7b.json with name v8x7b-gm, one description
sentence, `nav.leverage` 1.2685, `nav.output` `...-L1.2685-v8x7b`, `ref.leverage` 1.1720; `lock --write` leaves every pin
unchanged; file `599655e2`). The step-(1) run of lib-v8x7b.json is not a trial and is never resumed past nav.

**Theme shares (fit provenance, theme-erc-v1 re-solved on T = 12, decisions 1,004):** ERC dispersion 6.7e-16, 10,000
sweeps, member cap 1/24 = .0417 (two cap iterations: issuance_xbrl, iv_rv_spread, ins_opp_buy, then inst_best_ideas):
price_volume **.154**, investment_issuance .124, profitability_quality .098, ownership_flow .097, low_risk .097, value
.073, reversal_seasonality .069, short_interest .068, earnings_momentum .062, options_implied .059, price_momentum .054,
filing_events .044.

| phase (v8x7b-gm; source `eb3b329e`) | caps | s | peak MiB | exit | receipt.json SHA-256 | output / result |
|---|---|---|---|---|---|---|
| nav (L 1.2685, the cell) | 180 / 1,536 | 57.4 | 586 | 0 | `bf9709e69096389b...` | `summary.json` `e25a0b4f`, S2 daily `9e6fbe91`, capacity `87f424ca`, recipe `5b1770a3` |
| monitor / summ | 180 / 1,536 | 1.6 / 44.7 | 134 / 619 | 0 | `101cc6a6...` / `e979c6ba...` | `cycle-v8x7b-gm/summ.json` `dd38d7a8`, `cycle_verdict.json` `38a7bc66` |
| one-sided p (PM5-23) | 180 / 1,536 | 2.4 | 610 | 0 | `52b2b19a1b3441e9f2e8aabfb714dec7c5929179b35ee6d4ae9c303cb3b8ffb0` | `v8-cells-x7-bundle.json` `332a97db` (FINAL v8x7b-gm vs BASE X-5) |

**Matched run: G 0.9863734223 vs .9862260459, |diff| .00015 (one correction).** **Mechanics (S2, matched run, read before
any return): PASS.** All-rows gross .9864 in [.90, 1.05] (post-ramp .9918); net +.0047 (<= .02); tau mean .03562 <= .20,
p95 .03944 <= .30 (1,005 sessions; flags true); max gross 1.123, max |net| .029; gross by year .970 / .996 / .985 / .994;
1,006 CSV rows, 1,005 return rows, last session 2023-12-29; accounting 1.5e-13 / 4.0e-16 (tol 1e-9).

**Statistics of record** (S2; bundle X-5 vs X-7, block 21, seed 20260929, 4,999 resamples; 1,005 sessions): net Sharpe
**X-7 +1.6087** vs X-5 +1.7695: **dSR -.1607**, rho .927, **Memmel SE .1927** (t -.83); CBB 95% [-.518, +.207]; LW SE
.1873, 95% [-.536, +.215]; **bootstrap p one-sided .8096, two-sided .399**. DSR (verdict, `--dsr-ledger`, N 56): ledger
DSR **.7309** (V[SR] 1.245e-03 per session, 27 research-window-v2 lines; SR0 1.299 annual); effective-N .8807; legacy
.9206; PBO .0916.

**Capacity criterion (PM7-10, printed; decides nothing under PM7-34 / PM8-1): net Sharpe at 4x NAV 1.5060 vs 1.6549 ->
unmet.** Turnover tau .03562 vs .02684 (+33%); cost per traded dollar 12.60 vs 12.56 bps.

**Verdict (PM7-34 / PM8-1: dSR > 0 AND mechanics): dSR -.161 <= 0 -> NOT ACCEPTED, N 56.** Ledger line trial
`ab36ff093e393264` (cell = the L1.2685 v8x7b NAV dir, s2_net_sr 1.60873, origin prior, window research-window-v2, prev
`af281c31`); ledger 111 lines (64 construction incl. the 8 W0-4 re-runs, 46 admission, 1 protocol), file `58bfef12`,
head `7aaf0ae1`. Not retried. **Parent stays X-5** (`x-theme-erc-gm.json`).

Returns (S2, annual): net 4.61% (CAGR 4.67%) vs X-5 5.08%; gross of cost 6.26% (6.45%); trade cost 1.12% (.84%), borrow
.33%, long financing .20%; vol 2.87% (2.87%); max drawdown 2.18% (2.06%); gross Sharpe 2.184 (2.247).

| year | rows | net return | net Sharpe | vol | tau | cost bps |
|---|---|---|---|---|---|---|
| 2020 | 252 | +.0058 | +.219 | .0284 | .0375 | 14.96 |
| 2021 | 252 | +.1017 | +3.217 | .0302 | .0346 | 11.58 |
| 2022 | 251 | +.0640 | +2.067 | .0303 | .0355 | 12.29 |
| 2023 | 250 | +.0177 | +.716 | .0251 | .0349 | 11.68 |

Capacity (report only; X-5 beside): net Sharpe .5x 1.669 (1.809), 1x 1.609 (1.769), 2x 1.563 (1.727), **4x 1.506
(1.655)**, 8x 1.433 (1.553); cost bps per traded dollar 10.75 / 12.60 / 14.85 / 17.00 / 18.66. Read: the ERC re-solve gave
the new `price_volume` sleeve the largest share (.154, a low-volatility sleeve), the book's volatility did not fall
(2.87% both), the gross return fell (6.26% vs 6.45%) and trading cost rose by .28% a year with 33% more turnover: the
formulaic picks paid less than they cost at the book's horizon.

Tests after the cell (tests only, `a80e2c47`): `lib-v8x7b-gm.json` joins NULL_PINS / ADD_ALPHA_COPIES as `lib-v8x3b-gm.json`
did; `scripts/tests` **249 passed, 3 skipped, 0 failed**. Disk: 69,372,051,456 B free (64.6 GiB): the drop from 114 GiB
is other sessions' builds (a vcpkg grpc build tree, `C:/atx/build-server-rel`); this batch's new outputs are about 2 GiB
(bar fields 3 x 69 MB, candidate-cache entries of the 22 X-7 member runs, cards, w and NAV dirs).

### 4. Campaign v9-mine-c1 (prereg section 9 runbook, PM7-2, PM7-12) -- plan, written before any campaign command

**H-F = X-5** (the last accepted hand-written X cell; X-7 not accepted): spec `x-theme-erc-gm.json`, library v8x3b, w
pass `build-equity/mega-v8xw-train-theme-erc-1`, fit `build-equity/mega-weights-v8x-theme-erc`, role lo3
(`train-2020-2023-lo3/manifest.json`, B0b won), fields v13 (`train-2020-2023-lo3-fields-v13/manifest.json`).
**Step 0 (integration 8 receipt; executables unchanged since):** mine exe `2176fa4a`, mine-tests `d6e40412`, factory-tests
`58bb8bee` (re-hashed now = v8-14's): mine-tests 44 / 44, factory 392 / 392, golden `0x889874a3b9b29c55` at 1 and 4
workers, `PromotesThePlantedSignalsOnlyInFiveSeeds` rung_failed 0 (integration 8 log). v8-15 rebuilt only the ic exe.
**A3 field rule at the lock** (registry at H-F, every X-listed member included): k8_count_63 (read by k8_intensity) and
inst_own_chg_q (inst_persist) leave the list; no X-7 string reads any of the 12. **Fields 10**: iv_atm_63d, iv_atm_126d,
ea_delay_days, ins_net_buy_ratio, ins_n_buyers, ins_n_sellers, k8_days_since_any, inst_breadth_chg,
regsho_threshold_days63, sv_offexchange_share126 (all 10 are fields v13 rows). **B = 11 x 10 = 110** (A3; F = 1.54 for
101..1,000; z(110) = 3.5062, raw discover t = 3.5062 x 1.54 = 5.3996; Fc 1.77 for m <= 16). Prereg items 4, 5 and 12 (3)
amended to match in the spec commit (runbook step 2). **Commands, in order** (`RC = python scripts/research_cycle.py`,
`SPEC = scripts/specs/v9/mine-c1.json`): edit SPEC (fields, budget 110, `inputs.role` / `inputs.fields` = H-F's,
`pool_source` = H-F's three files); `$RC mine lock $SPEC --write`; `$RC mine pool $SPEC`; `$RC mine lock $SPEC --write`;
`$RC mine probe $SPEC` (W = the largest of 4, 2, 1 with required <= 7,680 MiB; `max_memory_mib` = required at W rounded up
to 64); delete the four `requires` lines (OD-7: PM7-2, PM7-12; PM7-27 (2)); commit; `$RC mine plan $SPEC` (header
check); `$RC mine run $SPEC --date 2026-10-02` alone, only with free physical memory >= `max_memory_mib` + 1,024 MiB
(PM7-12; else A4: workers 4 -> 2 -> 1 with the probe value, never the budget; stop if 1 does not fit); `$RC
ledger-campaign --ledger build-equity/trials.jsonl --campaign build-equity/mine-v9-c1` must exit 2. Reads only in the
registered order (receipt; ledger line; mechanics; then counts, promotions, `mined_members.json`, `trials.csv` last).
The spec edits break `scripts/tests/test_research_mine.py`'s pins of the committed template (12 fields, 4 requires,
fills); fixed in tests only after the run (as `4d0c8d8f`).

**Steps 1-6 done (metadata only; no payload opened by the probe; nothing of the campaign has run):**
- Spec filled (`scratchpad/mine_fill.py`): fields 10, budget 110, role lo3, fields v13, `pool_source` = X-5's three files,
  description. `mine lock --write`: role `e1c67101`, fields `e5f7f28c`, combined `2a442f56`, weights `8310da2c`, summary
  `9303cac6` locked; pool missing (expected).
- `mine pool`: `build-equity/mine-pool-v9-c1/manifest.json` (`atx.mine-pool/v1`): 1 regressor (`book` = X-5's combined
  signal) and **47 members** (every v8x3b member with a positive weight in X-5's weights; hard links into the lo3
  candidate cache; 3.0 GiB apparent). `mine lock --write`: pool `d129f873`.
- **Finding (tool defect, fixed, `23b52a5d`):** `mine probe` failed before any verb ran: `FileNotFoundError [WinError 2]`
  -- Windows CreateProcess does not find the relative verb path `build-equity/bin/atx-equity-strategy-mine.exe` written
  with '/' (the bounded runner resolves its command with `shutil.which`, so IC / NAV phases never met it; the probe and
  `mine run`'s `--help` check launch the verb directly). Fix: `research_mine.launchable()` resolves argv[0] against the
  root at those two launches only; printed lines and the run's argv keep the spec's spelling. Test added (the probe
  launches the root-resolved verb; `launchable` unit test); `test_research_mine.py` on the committed template 41 passed.
  Rule 7: a tool defect, nothing read, 0 trials.
- **`mine probe`** (verb `2176fa4a`, `--max-memory-mib 64`, refuses before any payload): required **3,737 MiB at 4
  workers**, 2,577 at 2, 2,560 at 1. **Identity of the memory model (runbook step 5: "other numbers mean a wrong
  build"):** the runbook's 3,979 / 2,784 / 2,765 are the joined model at 12 fields and an assumed 1,405 x 6,100 shape;
  the lo3 role has 5,922 instruments (role manifest). The same probe at the registered 12 fields (`scratchpad/probe12.py`,
  spec not edited, metadata only) gives 3,865 / 2,705 / 2,687 MiB; the joined model's bytes scaled by 5,922 / 6,100 give
  3,862 / 2,702 / 2,684 MiB plus about 4 MiB of fixed terms that do not scale (`small`, metadata): **the build is the
  joined head**; the 10-field numbers are the same model with two fields fewer. **W = 4; `max_memory_mib` 3,776**
  (3,737 rounded up to 64; <= 7,680); runner 8,192 MiB / 600 s.
- `requires` deleted: (1) OD-7 granted by the owner's goal text as ruled in PM7-2 and confirmed in PM7-27 (2); (2)
  runbook step 1 passed at integration 8 on these executables (mine `2176fa4a`, mine-tests `d6e40412`, factory `58bb8bee`;
  golden at 1 and 4 workers; rung_failed 0); (3) step 2 done for the source cell H-F = X-5; (4) the field rule re-checked
  (A3: 10 fields).
- Prereg amended in the same commit (`docs/plans/2026-10-01-v9-mine-campaign-prereg.md` items 4, 5 and the reading of
  12 (3), runbook step 2): 10 fields, N 110, z(110) 3.5062, raw discover t 5.3996.
- `mine plan` header: every input and pool_source line `[locked, verified]`; `capacity 110 (templates 11 x 10, stage 2
  off); budget 110 (ceiling in force 10000)`; `Bonferroni z 3.5062`, `F 1.54`, `raw discover t 5.3996`; `Fc by m (m
  1..16: 1.77; cap 16)`; `discover [2020-01-01, 2023-01-01), confirm [2023-01-01, 2024-01-01)`; `--max-memory-mib 3776,
  runner 8192 MiB / 600 s`; registry `new`; no `# requires`, no `# fill` line.

**Step 8, run** (`mine run scripts/specs/v9/mine-c1.json --date 2026-10-02`, source `a5df67ae` clean; launched by
`scratchpad/mine_go.py` only when free physical memory was >= 3,776 + 1,536 MiB and no compiler ran: 6,201 MiB free; no
other process of this session ran during it):

| step | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| mine run (verb `2176fa4a`, W 4) | 600 / 8,192 / 512 free | 232.6 | 2,211 | 0 | `be23441d2bdf2d68569af3d42dc811877c0578c0c53bcc7880cd4b0376d0cdf2` | `build-equity/mine-v9-c1/`: `campaign.json` `10a64f8a`, `mined_members.json` `4f09e4e0`, `trials.csv` `d6d127a1`, `ledger_line.json` `90abe2c5` |

Checked before any statistic, in the registered order (prereg section 9; runbook steps 9-10):
1. **Receipt**: completed, exit 0, `clean in the code pathspec`, min system free 728 MiB.
2. **Campaign line** appended by `mine run` before anything printed a statistic: trial **`f1ce3bf835d4dc54`**, kind
   mining-campaign, count 0, registry count 110 (total 110), budget 110. Ledger 112 lines, file `0c9d36b6`, chain head
   **`d1484e69`**; campaign lines 1; construction N **56 unchanged**. `ledger-campaign` again: **exit 2** ("shares
   trial_id, recipe_sha256, campaign ... a second confirm read on the same identity is refused"); ledger unchanged.
3. **Mechanics** (`mine run`'s checks; none failed): distinct **110** = evaluated 101 + screen-rejected 9 (ic-undefined) +
   racing-rejected 0 + rung-failed 0 + failed 0 (budget 110, capacity 110); registry new 110, n_raw 110, 216,648 B, head
   `3792dd6d`; hurdle z 3.506204726617 = z(110), overlap factor 1.54, ceiling 10,000; recipe confirm bands
   [[16, 1.77], [64, 1.96], [256, 2.15]] (mined-v1's); label rows discover 734, confirm 228; recipe pins and windows =
   the spec; fields = the spec's 10; footprint required_bytes 3,918,459,408 (= the probe), 232.2 s.
4. Then, in order: **(a) counts: shortlisted 0, rho pass 0, confirm read 0, confirm defined 0, admitted 0.** (b)
   promotions `[]`; `mined_members.json`: 0 members (registry head `3792dd6d`). (c) last, diagnostic, selects nothing:
   `trials.csv` evaluated 101 with an f2; largest f2 4.998 (next 4.81, 4.76, 4.62, 4.05) against the raw discover hurdle
   5.3996 (f2 / 1.54 >= 3.5062): **no trial cleared it**; 12 of 101 had f2 >= 2.
5. `mine wave scripts/specs/v9/mine-c1.json --parent v8x3b --name v8x3bm1 --parent-spec
   scripts/specs/v8/x-theme-erc-gm.json`: "v9-mine-c1 admitted no member: no wave, no cell, no admission trial".

**Campaign v9-mine-c1: complete, final, admitted 0. M = 110** (registry count; enters N_tot by PM7-7). **X-9 (mined
wave): undefined (0)** -- no member, no cell, no admission trial (prereg item 11; v8x prereg section 6). N_c stays 56.
**N_tot = 102 (trial_counts of the ledger) + 110 (M) = 212.** Appendix A addition: `mined campaigns 1 (v9-mine-c1:
budget 110, registry count 110, admitted 0)`.

Tests (tests only, `a1d2ef74`): the spec edits broke `test_research_mine.py`'s 26 pins of the committed template, as
announced; the registered template (`23b52a5d`'s bytes) is now the fixture `scripts/tests/fixtures/mine-c1.registered.json`
that those tests read, and a new test pins the live spec as locked (A3 fields = the template's less the X members'
fields, B 110, every pin locked, no requires, no fill, the cap a multiple of 64 <= 7,680, everything else the
template's). `test_research_mine.py` **42 passed**; `scripts/tests` **251 passed, 3 skipped, 0 failed**;
`test_trial_ledger_rules`, `test_mine_overlap_factor`, `test_dsr_total` **23 passed**.

### State at the stop (PM8-3: X-10 deferred; root stops here)

- **Accepted X book = X-5** (`scripts/specs/v8/x-theme-erc-gm.json`, library v8x3b, theme-erc-v1, L 1.1720): S2 net
  Sharpe 1.7695, net annual 5.08%, gross of cost 6.45%, 4x net Sharpe 1.655, tau .02684, max drawdown 2.06%. H-F = X-5;
  X-F0 so far = X-5 (Y cells next, PM8-2).
- Cells this batch: X-7 NOT ACCEPTED (N 56, dSR -.161, p one-sided .810, two-sided .399); campaign admitted 0; X-9
  undefined. N_c 56; admission lines 46 (v8 12, X hand-written 25, X-4 re-screens 9); M 110; N_tot 212; ledger 112
  lines, head `d1484e69`.
- Not run (PM8-3): X-10, the X gate / adoption print, the hidden block, OD-3.

### Hidden-data record (X batch 2, resumed)

- Inputs opened by tools: role lo3 and the lo3-dlret label role, fields v13 / v14 (v14 built from the role's
  TickerHistory3 price source through the sealed builder: vendor rows on or after 2024-01-01 skipped and counted),
  X-5's u / fit / w / NAV outputs, the lo3 candidate cache, the X-7 library outputs, the mine pool, the ledger. The fields
  v14 build re-read the atx-db stage pins of v13's argv (hash checks; nothing under `atx-db/` written).
- Read by me: receipts, manifests (metadata), gate rows, the cycle verdicts, the NAV mechanics keys before any return,
  then X-7's returns after its mechanics passed, the campaign's receipt / ledger line / mechanics, then its counts and
  the `trials.csv` f2 column (diagnostic). Every NAV's last session 2023-12-29; every manifest sealed at 2024-01-01.
- **Nothing dated 2024-01-01 or later was opened. No 2013-2019 history read. No `stdout.log` of a NAV or of the campaign
  verb was opened.** `C:/atx`, `atx-db/` and pools 7, 8, 10, 12-15 untouched; no push.

### Open items (X batch 2, resumed)

- Tool fix `23b52a5d` (`research_mine.launchable`): a code change in the scripts pathspec, outside the build; reviewers
  may want it in the next scoped review.
- The marginal phase is near its 360 s cap at 68-70 members (253-332 s on a quiet host); a wave above about 70 members
  will hit it (a PM restatement of the cap, as PM7-31 did, would be needed before Y waves).
- X-5's weights hold 11 themes, so every add-alpha wave on it needs PM6-8 (i) for the marginal (the verb takes 10 themes).
- Disk 69,954,383,872 B free (65.1 GiB); other sessions' builds took about 50 GiB during the batch.

## Y integration (root, PM session 8 dispatch, second session): merges, theme, repair, build v8-16, fields v15, Y cells (2026-10-02)

Root in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `ac07c12a` (clean). Read: `C:/atx/CLAUDE.md`,
progress "PM session 7" and "PM session 8" to the end, `v8x-prereg.md` sections 6-8, this log's last 330 lines (X batch 2
and its resume), `pm8-rulings-draft.md`, `v8y-prereg.md` @ `87a9e0f4` in full (blob SHA-256 `d78aba66...57fe` = PM8-14's),
integrator-rules. State at the start: book X-5 (`x-theme-erc-gm.json`), N_c 56, admission lines 46 (X hand-written 25),
M 110, N_tot 212, ledger 112 lines (head `d1484e69`).

### 0. Rulings recorded (`56fd1639`, `09b4ab71`)

PM8-5..PM8-14 and the lane notes between them appended verbatim to progress.md from the PM's draft (draft deleted; it was
git-ignored); PM8-15 and PM8-17 verbatim from the PM's messages. PM8-16 was not received by root.

### 1. Merges (`--no-ff` by SHA, the dispatch's order; 0 conflicts)

| # | lane | lane SHA (branch head at merge) | merge | content | suites |
|---|---|---|---|---|---|
| 1 | YPRE | `87a9e0f4` | `ddf8ad73` | `v8y-prereg.md` (committed blob `d78aba66...57fe` = PM8-14's pin), report | docs only: none affected |
| 2 | YSIG | `5431831a` | `b5e84bb6` | report, `ysig_check.py` | `ysig_check: PASS`; `xsig_check: PASS`; `xwq_check: PASS` |
| 3 | YDATA | `e1cf4135` | `cd90e6c6` | 5 field modules + tests, `prepare_research_fields_ydata.py`, vwap data ask | `atx-engine/tools` whole **340 passed**, 6 subtests (= the lane's count) |
| 4a | YOPS | `97e6befc` (head when merged) | `fad041f7` | `group_sum`, as-of rank family, `group_delay`; `yops_check.py` | `yops_check: PASS` (40 planted errors fail); C++ at build v8-16 |
| 4b | YOPS (delivered, PM message) | `fe790af7` | `8544a36f` | op tests (`alpha_formulaic_ops_test.cpp`), `asof_ops.hpp` listed and `dsl_vm_sources_sha256` re-pinned `fcf8021e...` without a semantics bump, report, `test_yops_check.py` | C++ at build v8-16a |
| 5 | YINFRA | `85a98a5b` (cleared by the PM's message; report section 7 "Review fixes": 18 of 18 closed, each with its commit and tests) | `c3235084` | wave driver (`research_cycle.py wave`), candidate queue, scoreboard, `stage_chain.py` | see the suite table below |
| - | YCOMB | not merged | - | waits for the PM's review verdict (dispatch) | - |

No lane touched `atx-db/`. Build v8-16 (YOPS `97e6befc`) is superseded by v8-16a (YOPS `fe790af7`) before any run read
an output of it: v8-16's ic-tests showed `StrategyIcRunner.VmSourcesPinnedToSemanticsVersion` failing (vm.hpp includes
the unlisted `asof_ops.hpp`; digest `7523...d830` vs pin `ad6c...3d62`), which the lane's `75a73cac` fixes at the source.

### 2. Theme `merger_arbitrage` (`55eefd38`) and the iv_vol_of_vol repair (`cf0ec969`), 0 trials each

- `merger_arbitrage` (PM8-8 (2), PM8-14, v8y-prereg section 10): registry themes table (YDATA's L1 text verbatim),
  `fit_composition_weights.V7_APPENDED_THEMES` (4 entries), `theme_resid_order` 12 -> 13 entries in
  `atx-impl/src/strategy_ic_theme_resid.hpp:24-29` (last); the pin literals of `test_composition_resid.py` (:243-245,
  :268, :284, :677), `test_fit_composition_weights.py` (:2811-2815, :2885) and `test_fit_composition_weights_store.py`
  (:212) follow (as `798d3b23`). The three files: **124 passed**, 17 subtests (R6B-O-4's C++/Python pin included).
  `atx-impl/strategies` **163 passed**. The two-speed half-life row `{"merger_arbitrage", 126.0}` (YP-10) waits for the
  YCOMB merge (the table is YCOMB's code).
- iv_vol_of_vol (PM8-8 (6), YP-5): the registered string with every `iv_atm_21d` read as `delay(iv_atm_21d, 1)`, SHA-256
  **`4d42a72b859c3ff4ae69ed82cf34c8263d9877c4f0600b03c6cc32887ac5fac0`** (= YP-5's); `ysig_check.py`: the candidate
  string re-pinned (`REPAIRED`), the LIB2 string still checked byte for byte (`c5ecec15`), mirror figures 41 bars / 5
  slots / 15 nodes / `iv_atm_21d` (house budget 314 / 7), the add-alpha line = the LIB2 line with only `--dsl` and
  `--deviation` changed (deviation + "; IV rows t-21..t-1, the vendor IV clock (PM8-8 (6) repair)"): **line SHA-256
  `60edfca89dba05d5e3faa1f29ce7da839114858a7a2bfee19a21af0fb8721493`** (was `06ef2910...37f8`). `ysig_check: PASS`.
- Roster cap 80 -> 96 (PM8-6, P4): **not made**. The one-line registry edit was refused by this session's permission
  system ("Modify Shared Resources"); not retried by other means. It binds nothing in Y: X-F0 = X-5 holds library v8x3b
  (60 members; X-7 not accepted), so the Y-S screen library holds 60 + 15 = 75 <= 80. Reported to the PM.

### 2b. P7 blind checks (citations; before any Y read; YP-6)

- `mom_turn` (Lee-Swaminathan 2000): the paper's abstract: "high-volume portfolios, on average, realize higher momentum
  profits"; Chen-Zimmermann `MomVol` (sign +1) keeps the high-volume tercile. The interaction's +1 agrees. Stands.
- `dato` (Soliman 2008): Chen-Zimmermann `ChAssetTurnover` "Annual change in AssetTurnover", sign **+1**. Agrees. Stands.
- `div_event` (MTW 1995): Chen-Zimmermann `DivInit` "not having paid a dividend in the last 24 months" (= the 504-session
  gap) sign +1; `DivOmit` sign -1 (the field's -1). The 252-session window is MTW's one-year drift (CZ holds 6 months for
  DivInit: a portfolio choice, not the paper's window). No contradiction. Stands.
- `exch_switch`: Chen-Zimmermann `ExchSwitch` sign -1, "switched from AMEX or NASDAQ to NYSE within the past year, or from
  NASDAQ to AMEX" = the string's -1 x up-switch flag over 365 days. Stands.
- `deal_target` 252-session horizon: a mechanical cap, not a constant of Mitchell-Pulvino; not readable as a paper value.
  Registration stands; risk printed (YP-6: "a paper that cannot be read leaves the registration standing").
- `shD1` (PM8-8 (3)): SpiderRock's documentation names its slope fields "Interpolated 21 day ATM vol slope" (the same text
  YDATA quotes for `shD1`), a surface property at the session, the slope the vendor defines as the volatility difference
  between moneyness -0.5 and +0.5; the TickerHistory3 row of `shD1` itself was not reachable on the public site (risk
  printed). The house exclusion (`docs/superpowers/specs/2026-06-16-orats-history-loader-design.md:95`) was a blanket
  "forward horizon" label written without the dictionary. The field reads session t-1 only (`ivshape-lag1-v1`); its
  look-ahead probe and teeth pass (`test_research_fields_ivshape.py`, in the 340). Accepted at lag 1 as PM8-8 (3) rules.

Sources: onlinelibrary.wiley.com/doi/10.1111/0022-1082.00280 (LS 2000 abstract); github.com/OpenSourceAP/CrossSection
SignalDoc.csv (CZ rows); docs.spiderrockconnect.com (slope fields).

### 3. Build v8-16 / v8-16a and the identity of X-5 -- plan, written before the runs

Builds (`scripts/research-build.ps1 -Preset equity-dev`, target-scoped, one at a time): **v8-16** at `cf0ec969` (YOPS
`97e6befc`): `atx-equity-strategy, -ic, -targets, atx-impl-strategy-ic-tests, atx-engine-factory-tests,
atx-engine-alpha-tests`: exit 0, 601.6 s, 180 TUs, 8 links, **0 compile fixes**; ic-tests 158 / 159 (the tripwire above).
**v8-16a** at `09b4ab71` (YOPS `fe790af7`), the same six targets: exit 0, 535.4 s (3 jobs at 3,177 MiB free), 135 TUs,
8 links, **0 compile fixes, 0 warning lines**; receipt `build-equity/mega-v8-16a-receipt.json`. Executables (SHA-256):
ic **`d0afb8cabc23ade8d6f82fd57c75da09d8745e265c652c6510d3ffd143c25e61`**, targets
**`1e7c304272f8fa4dafa3694cb8a93be32fb79777d54319604085291d071a9054`**, atx-equity-strategy `34e1dace...e1`, ic-tests
`3f40261c...07ab`, factory-tests `11eb72c4...fed3`, alpha-tests `6438fe88...b884`. Unchanged since v8-14 / v8-15 (not
rebuilt): risk `d7e424b2`, mine `2176fa4a`. After YCOMB a further tag builds YCOMB's targets.

| suite (v8-16a) | result |
|---|---|
| `atx-impl-strategy-ic-tests` whole (xml `build-equity/v8-i16a-ic-tests.xml`) | **159 / 159** (`StrategyIcRunner.VmSourcesPinnedToSemanticsVersion` passes at `fcf8021e`) |
| `atx-engine-factory-tests --gtest_filter=NsgaSearch.*:SignalFitness*` | **24 / 24**: `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`, `SignalFitnessDefaults.ImplicitDefaultsKeepTheGoldenDigest`, `...ExplicitDefaultsKeepTheGoldenDigestAtEveryWorkerCount` (1 and 4 workers): **golden `0x889874a3b9b29c55` holds** |
| `atx-engine-factory-tests` whole | **392 / 392** |
| `atx-engine-alpha-tests --gtest_filter=AlphaVmSlotReuse.*` (xml `v8-i16a-slot-reuse.xml`) | **6 / 6** |
| `atx-engine-alpha-tests --gtest_filter=AlphaFormulaicOps*` | **15 / 16**: `AlphaFormulaicOps_Frozen.ReportStringsArePinnedCompileAndMatchTheOracle` fails at `alpha_formulaic_ops_test.cpp:913` (`EXPECT_GT(finite, 0U) << "#88"`): string #88 has no finite cell on the test's 130 x 8 fixture; VM == oracle on every cell (:910) and every SHA pin pass. A fixture degeneracy in a lane test, not a VM defect; no Y string reads a new op (PM8-17). Not edited by root (a lane test); reported to the PM for a YOPS test-only fix |
| `atx-engine-alpha-tests` whole | **770 / 771** (the same one) |

The PM routed it to YOPS: fix **`b25c2a2f`** (test only: each frozen string's `analyze()` lookback pinned, max 91; VM ==
oracle kept on the holed fixture; finite cells checked on a complete 132 x 16 fixture), merged **`7bea1a6d`**; build
**v8-16b** (`atx-engine-alpha-tests` only; exit 0, 10.0 s, 1 TU, 1 link; `e6d300f1...`): `AlphaFormulaicOps*` **16 / 16**,
whole alpha **771 / 771**. Every frozen string has finite cells on the C++ generator. The VM sources tripwire pin is
unchanged by it; the research executables stay v8-16a's.

Python suites after the YINFRA merge (`ATX_EQUITY_BIN`, `ATX_EQUITY_TARGETS_EXE` = v8-16a, absolute):

| suite | result |
|---|---|
| scripts/tests (whole) | 307 passed, 3 skipped, **1 failed**: `test_research_wave.py::test_add_alpha_save_plan_is_opt_in` (YINFRA #18's test). Cause: the stdout masking replaced the run's root path forms in `set` order, which follows the string hash; when `tmp/<tag>` went before `tmp/<tag>/root` a line kept `<ROOT>/root/` (fails or passes by `PYTHONHASHSEED`). **Fix `3a146fe7`** (tests only, `scripts/tests/test_research_wave.py:148-150`: longest form first); passes under seeds 0-5; `test_research_wave.py` 15 / 15 |
| atx-engine/tools (whole) | **348 passed**, 6 subtests (340 + YINFRA's stage-chain tests) |
| atx-impl/tools (whole) | **612 passed, 1 skipped**, 17 subtests |

X-5 identity under v8-16a (PM7-30; bounded runner; argv = X-5's recorded receipt with only `--output` renamed, and for
w also `--candidate-cache` -> a new empty directory `build-equity/v8-i16-cand-cache-empty` (PM message: the cache key is
unchanged, so a warm cache would never run the new VM on the X-book strings); limits and bindings as recorded):

| pass | base receipt | new output | compared to | expected |
|---|---|---|---|---|
| fit | `mega-weights-v8x-theme-erc-run1` | `v8-i16-x5-fit` | `mega-weights-v8x-theme-erc` | `admission.csv` byte-identical; `admission.json`, `composition_weights.json` identical after substituting `script_sha256`, `admission_sha256`, `provenance/std/registry_sha256` (fitter and registry gained `merger_arbitrage`) |
| w (cold cache) | `mega-v8xw-train-theme-erc-run1` | `v8-i16-x5-w` | `mega-v8xw-train-theme-erc-1` | every file byte-identical but timing / cache-count paths; `train_combined.*` byte for byte (the library re-evaluated by the v8-16a VM) |
| NAV | `mega-nav-v8x-theme-erc-L1.1720-run` | `v8-i16-x5-nav` | `mega-nav-v8x-theme-erc-L1.1720` | 27 / 27 byte-identical |

A mismatch is a stop (PM8-4 (b)). No statistic is read: comparisons by SHA-256 and JSON paths only.

## P9 Phase 0, root steps R0-3, R0-4, R0-5 (PM session 9 dispatch; 2026-10-03)

Root in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `84f06f6b` (code pathspec clean; one untracked file
outside it, `docs/plans/2026-10-02-x5-equity-curve.png`, not root's, left alone). Read: P9 plan
(`docs/plans/2026-10-03-p9-sprint-plan.md`) header DS paragraph, sections 0.6 and 1.1 (R0-3..R0-5), integrator-rules,
global-constraints, `v8y-prereg.md` sections 9 and 13 (P5, P8, P10) and Appendix B YP-12, this log's "Y integration" section
3, progress PM7-30 and PM8-4. Lane E1's P0-FIX files and `y-s.json` untouched (R0-2). No C++ built.

### R0-3. X-5 identity under build v8-16d: verified from disk (v8y P5, PM7-30)

The three passes were run by a previous root session at source `d7c1c520` (bounded runner, "clean in the code pathspec",
dirty outside none) but no result was logged. Verified from disk now, not from a report:
- **Build v8-16d** (receipt `build-equity/mega-v8-16d-receipt.json` `a3258637`): source `e0fd0297`, DirtyEntries 0, preset
  equity-dev, 8 targets (`atx-equity-strategy`, `-ic`, `-targets`, `atx-impl-strategy-ic-tests`, `-target-tests`,
  `atx-impl-tests`, `atx-engine-book-tests`, `atx-engine-research-fields-tests`), exit 0, 287.5 s, 104 TUs, 10 links.
  Executables re-hashed on disk = the receipt: **ic `985019d97d3db335430da8dda2160c9b4908c0db462344478b7e64bb5fa3c989`**,
  **targets `72ff6d2d707048ae72b089c362c40b416fda5c9bed2498d64eeb137f678d5f25`**, atx-equity-strategy `8c4cc70b...a3ea`.
  v8-16c (`5b9af8ba`) exited 1 (superseded before any run); v8-16e (`2206504b`) rebuilt `atx-impl-strategy-ic-tests` only
  (`3393a953`). Between `e0fd0297` and `84f06f6b` the only C++ change is `atx-impl/tests/strategy_ic_runner_test.cpp`
  (v8-16e's): the research executables are current.
- **Argv** (token by token against X-5's recorded receipts): fit 1 differing token (`--output`), w 2 (`--output`;
  `--candidate-cache` -> the new empty `build-equity/v8-i16d-cand-cache-empty`), NAV 1 (`--output`); limits equal.

| pass | X-5 receipt / reference | new receipt (`receipt.json` SHA-256) | exe | s / MiB | result |
|---|---|---|---|---|---|
| fit (180 / 1,536) | `mega-weights-v8x-theme-erc-run1` / `mega-weights-v8x-theme-erc` | `v8-i16d-x5-fit-run` `cd7458de867e2e7cc55e9c68d2c34afad9116e96a43e307db0b1b479092ed5d0` | python (fitter) | 1.6 / 56 | `admission.csv` **byte-identical** (`b6cb8a74`); `admission.json` differs only in `/inputs/script_sha256` (`4c529f5f` -> `8d05a9bb` = the fitter at head: `merger_arbitrage` in `V7_APPENDED_THEMES`); `composition_weights.json` only in `/provenance/script_sha256` (same), `/provenance/admission_sha256` (`5f5b3edb` -> `90adeef8`) and `/provenance/std/registry_sha256` (`7ff10f4e` -> `6a1ef89d` = `registry.json` at head: theme `merger_arbitrage`, the Y field rows). **Both byte-identical after substituting those values back** (admission `5f5b3edb`, weights `8310da2c`): the PM7-30 list, nothing else |
| w, cold cache (300 / 3,072) | `mega-v8xw-train-theme-erc-run1` / `mega-v8xw-train-theme-erc-1` | `v8-i16d-x5-w-run` `d6c9f5069a5f8d1c948960de9fdcdad582dfda99dbc0e72cfa8523e7535d8f8f` | ic v8-16d `985019d9` (X-5 ran v8-14 `67f72921`) | 254.4 / 2,373 | **10 of 12 byte-identical**: the six `train_combined.*` (`.json` `2a442f56` = X-5's pin, `.f64` `c17ac935`), `train_daily_ic.csv`, `orientations.json`, `recipe.json`, `train_planned_targets.csv` -- the library re-evaluated by the v8-16d VM (58 VM evaluations) reproduces byte for byte. `summary.json` (603 of 7,245 leaf paths) and `train_candidates.jsonl` (464 of 6,554) differ only in timing paths (`stage_seconds.*`, `wall_seconds`, `hash_seconds`), cache paths and counts (`candidate_cache.{directory, fields_directory, entries[].payload, entries[].sidecar, hits, misses, vm_evaluations, ic_results.hits, ic_results.misses}`, per candidate `signal_cache`, `ic_result_cache`) and the cold-cache I/O counters `research_fields.{field_loads, loaded_bytes, peak_resident_fields}`, `verify_bytes` |
| NAV (180 / 1,536) | `mega-nav-v8x-theme-erc-L1.1720-run` / `mega-nav-v8x-theme-erc-L1.1720` | `v8-i16d-x5-nav-run` `618f0b69d2635707758366563f185c4c7ccf723857e99ce7a3280c28d809bd35` | targets v8-16d `72ff6d2d` (X-5 ran `a95f6f0a`) | 145.7 / 585 | **27 of 27 byte-identical** (S2 daily `529062d6...3d61` = X-5's ledgered series SHA-256, trial `269cfc47be86d4a7`; `summary.json` `a03937cf`; `capacity/`); `--max-bytes 1073741824` held |

The I/O counters follow the cache state, not the build: the reference ran warm (hits 58, misses 0, vm_evaluations 0,
field_loads 0), this run cold (hits 0, misses 58, vm_evaluations 58, field_loads 68, peak resident fields 8). Cross-check:
the superseded v8-16a cold run `v8-i16-x5-w` (same argv, its own empty cache) and this run differ in `summary.json` only in
timing and cache-directory paths (I/O counters equal).

**R0-3: PASS. X-5 reproduces under v8-16d** (NAV and w byte for byte with every Y flag absent; fit with the three
provenance hashes of the PM7-30 kind). Comparisons by SHA-256 and JSON paths only; no value of `summary.json`,
`train_daily_ic.csv` or any NAV file was read; no NAV `stdout.log` opened. Observation (timing only, decides nothing): the
v8-16d NAV took 145.7 s of its 180 s cap (X-5 under v8-15: 45.4 s; the X-7 ref: 59.0 s), minimum system free 1,300 MiB
during the run; host load from other sessions is the likely cause, but Y-cell NAV phases have 34 s headroom at that pace.

### R0-4. Fields v15 (v8y section 9, P8): checks from disk

Built by the previous root session at source `5db89d36` (two bounded processes, v8y section 9 A then B); not logged.

| step | receipt dir | outcome / exit | s | peak MiB | receipt.json SHA-256 | output manifest SHA-256 |
|---|---|---|---|---|---|---|
| A, v15a | `train-2020-2023-lo3-fields-v15a-run` | completed / 0 | 294.5 | 710 | `1b87f1ae631932264d6699a136088400471a4afc01205dba4853fb28daaf83bd` | `0422a796b1deee72793a82e2adfb00bdad04614ea4789a918f98a9aca9124e7f` |
| B, v15 | `train-2020-2023-lo3-fields-v15-run` | completed / 0 | 209.8 | 799 | `201fe448da9b9813d062a1aa5f9699152716212df3b7a5397847b1fc75308252` | **`26fee5ce301b9b0bffa1d72b45e014d973d3d73a080dd59ea55cda976f133b09`** |

- Caps W0-i (600 s / 2,560 MiB) held by both; receipts "clean in the code pathspec", dirty outside none, stderr empty.
- **Pin: the v15 manifest re-hashes to `26fee5ce...3b09` = `scripts/specs/v8/waves/y-s.json` `fields.manifest_sha256`**
  (`fields.dir` `build-equity/train-2020-2023-lo3-fields-v15`).
- **Counts = section 9's expectation.** v15a: reused **78** (from v14 `4b12c0e1`), computed **5** (`iv_skew_21`,
  `stio_chg_q`, `div_init_omit`, `deal_pending`, `exch_up_365d`). v15: reused **83** (from v15a `0422a796`), computed **1**
  (`conn_ret63`); 84 fields. Reused entries: sha256 = the prior manifest's 78 / 78 and 83 / 83; hardlinks of the prior files
  78 / 78 and 83 / 83; all 84 v15 payloads re-hash to their pins (84 / 84). Builder `code_sha256_lf` `74df97f9` (= v13 /
  v14).
- **Seal**: status complete; `seal.exclusive_end` 2024-01-01 ("every source row available on or after 2024-01-01 is
  dropped before use; role sessions asserted < 2024-01-01"); role lo3 `e1c67101`, 1,405 dates, 2018-06-01 - 2023-12-29.
- Section 9 source checks (metadata): `source_checks.ivshape.orientation_line` = ticker SPY (security 549535; 1,404
  sessions with a slope, 0 duplicates quarantined, 0 rows off calendar). `mgr13f.thirteenf.per_quarter`: 28 quarters, 0
  sealed; `short_term_filers` / `filers` .246-.271 over the 24 defined quarters (the first 4 undefined: rule
  `yz-short-term-tercile-v1` needs q-3..q), i.e. the top third of the classified filers by the rule's construction.
- Date tokens in the v15 manifest (paths only, values not read): 92 tokens dated 2024 or later, 54 of them the seal date
  2024-01-01; the rest are metadata strings (field caveats, the `calendar.last` entries of `sec`, `v9/nt_first_126`,
  `deals`, short-volume file download stamps, one plausibility-rule text, one excluded-column reason), as v13 / v14.

**P8 plan, written before the run.** The parent's ref on v15 = X-5's NAV receipt argv with three tokens changed (the
X-7 ref precedent `mega-nav-v8x-theme-erc-L1.1720-v8x7b-ref`): `--fields build-equity/train-2020-2023-lo3-fields-v15/manifest.json`,
`--fields-sha256 26fee5ce...3b09`, `--output build-equity/p9-r04-x5-ref-v15` (a name the Y-S wave's own `<nav.output>-ref`
cannot collide with). Runner: `--output build-equity/p9-r04-x5-ref-v15-run --seconds 180 --max-rss-mib 1536
--min-free-mib 512`, bindings as X-5's (targets exe v8-16d `72ff6d2d`, `train_combined.json` `2a442f56`, the v15 manifest,
the lo3-dlret label role `95e16cfe`). **Expected: `daily_modeled-1bn-stale5-v1+swap-fin-v1.csv` SHA-256 =
`529062d6f06f1ceb8dab5e7cbd98a3693a2f66922b0b4547df81f956fd4e3d61`** (X-5's ledgered S2 series); any other value is a
rule-7 stop. Compared by SHA-256 only; free memory checked >= 586 + 1,536 MiB before launch.

### R0-5. IC memory re-probe for the Y-S screen library (v8y P10, YP-12) -- plan, written before the run

The Y-S library file is written by the wave's register stage (after R0-2), so the probe uses a scratch copy built in memory
by `generate_library.py`'s own functions (registry copied in memory; the 15 alphas of `y-s.json` registered in roster
order, each `dsl` checked against its `dsl_sha256`; `child_library(v8x3b -> v8ys)`, `build_library`, `encode`; nothing in
the repo written): **73 members (58 + 15), 63 fields, 12 families, SHA-256
`bbcfbf9dd000dc41055e2415ddc1a0047811a796752de55fd4e92ab636f4e9bd`** (not of record; R0-6 can compare the registered
library's bytes to it). Probe: ic v8-16d `--plan-only` (metadata only, no payload) on that library, role lo3, fields v15,
with the screen u pass's flags (`--min-names 1000 --workers 4 --no-composition`, as `mega-v8-b0b-train-u-v8x7b-run1`) and
`--max-memory-mib 8192` so the plan prints `roles[].required_bytes` instead of refusing; through the bounded runner (120 s,
1,024 MiB). Rule (plan R0-5, YP-12): <= 2,560 MiB: cap unchanged; over 2,560: probe workers 2 and 1 and report for a cap
ruling (w already runs at 3,072); over 3,072: stop.

### R0-4 / R0-5 results (source `d23efa5a`, the plan commit; one run at a time; free memory 5,546 MiB before the first)

| run | receipt dir | exe | s / peak MiB | exit | receipt.json SHA-256 | result |
|---|---|---|---|---|---|---|
| P8: X-5 ref on v15 (180 / 1,536) | `p9-r04-x5-ref-v15-run` | targets v8-16d `72ff6d2d` | 56.4 / 586 | 0 | `2dc33a24098571a174c90f9206848115a70304e5f41d8caae9d5af1e1d9f9edd` | **S2 daily `529062d6f06f1ceb8dab5e7cbd98a3693a2f66922b0b4547df81f956fd4e3d61` = X-5's ledgered series**; 23 of 27 files byte-identical to X-5's NAV, the other 4 (`recipe.json`, `summary.json`, `capacity/recipe.json`, `capacity/summary.json`) differ only in `/financing_fields/manifest_sha256` (v13 -> v15) and, in the two summaries, the `/recipe_sha256` that follows it |
| R0-5: Y-S screen library `--plan-only`, workers 4 (120 / 1,024) | `p9-r05-ys-plan-w4-run` | ic v8-16d `985019d9` | 0.27 / 2 | 0 | `c33557ebcaea383d32ff5e49cde21f74e209952eaedb7e0ae1c6debe6a6fd07a` | plan (stdout `f0623208`): metadata-only, 73 candidates, composition skipped, max compiled slots 8, lookback 272, resident field capacity 8 (87 planned loads of 60 declared fields), **required_bytes 2,047,374,058 = 1,952.5 MiB** |
| R0-5 calibration: X-5's w pass `--plan-only` (v8x3b, its weights, fields v13, workers 4) | `p9-r05-x5w-plan-w4-run` | ic v8-16d `985019d9` | 0.27 / 2 | 0 | `a28ece5f6c6e2c01e05ffd551e15339b1b54cee76086cc4f7e1723e02366a547` | plan (stdout `a8e8f69c`): 58 candidates, 11 themes standardised, slots 8, capacity 8: required_bytes 2,854,733,324 = 2,722.5 MiB |

Argv of the P8 run against X-5's NAV receipt: exactly the three planned tokens differ (11 `--fields`, 13
`--fields-sha256`, 15 `--output`); receipt "clean in the code pathspec", dirty outside only the untracked png, stderr empty,
minimum system free 4,768 MiB. **P8: PASS** (the parent reads no new field; its S2 series on v15 is X-5's byte for byte).

**R0-5: 1,952.5 MiB <= 2,560 MiB: the Y-S screen's IC cap stays 2,560 at `--workers 4`; no cap ruling needed.** The old
~3,200 MiB estimate scaled admission linearly in members; the admission of a `--no-composition` u pass does not depend on
the member count at all (`strategy_ic_admission.cpp:246-282`: cells x (72 + 8 x max slots), the resident field capacity, the
labels and the worker envelope), only on the worst candidate's slots (8, a parent member; the 15 Y rows plan at <= 6). The
composition plane is the only member- and theme-dependent term: X-5's w admission minus the Y-S u admission = 807,359,266 B
= `ic_composition_working_bytes(1,405 x 5,922, 58 members, 11 themes, standardise)` to the byte, so **the Y-S cell's w pass
(73 members, 12 themes, the same slots and capacity) admits 2,047,374,058 + 873,930,226 = 2,921,304,284 B = 2,786.0 MiB
<= its 3,072 cap** [arith, formula verified on X-5]. The 15 Y plan rows (scratch, not of record; register makes the K1
plans of record): slots 2-6, lookback 0-272, extra fields <= 4 (dato), all within the house budget (7 / 314 / 5);
`iv_vol_of_vol` 5 slots / 41 bars / `iv_atm_21d` as the repair logged.

### State and hidden-data record (R0-3..R0-5)

- R0-3 PASS, R0-4 PASS (pin, 83 / 1, seal, P8), R0-5 PASS (1,952.5 MiB; cap unchanged). Nothing for the PM to rule. Next:
  R0-2 (P0-FIX merge, `y-s.json` amendment), then R0-6.
- Opened by tools: the X-5 u / fit / w / NAV outputs and receipts (SHA-256, JSON paths, the cache / field-load counters),
  the v13 / v14 / v15a / v15 field manifests (metadata) and payload bytes (hashing only), the role and label-role
  manifests, the ledger line of X-5 (series path and SHA only), the two plan JSONs. Read by me: receipts, manifests
  (metadata), JSON path lists, plan rows. While locating the ref precedent in this log I passed X-7's already-ledgered
  public lines (section "X batch 2, resumed"); no Y statistic exists or was read.
- **Nothing dated 2024-01-01 or later was opened. No NAV `stdout.log` opened; no value of a `summary.json`, a daily CSV or
  `train_daily_ic.csv` read.** `C:/atx`, `atx-db/`, other pools and E1's files untouched; no build; no push.

## P9 Phase 0, root step R0-2: P0-FIX merge and Y-S amendment (DEC-1, DEC-2; 2026-10-03)

| item | result |
|---|---|
| head before | `fd962cb975d65e58fdfb5d2a8a6adc6808270638` (code pathspec clean; only the untracked `docs/plans/2026-10-02-x5-equity-curve.png`) |
| merge | `git merge --no-ff 3fa2dd4affe4b4de718d222435801b318ebf5cf0` (lane E1 task 0 = P0-FIX, `feat/p9-e1-20261003`, review APPROVE `task-E1t0-review.md`) -> **`1b9b37e8`**; 18 files, +763 / -126, no conflict (root's commits since `d7c1c520` are docs / sprint files only); E1's report commit `a57961bb` and later commits not merged |
| check live (pre-amend) | `wave plan scripts/specs/v8/waves/y-s.json` -> exit 2: `marginal.seconds 720 is above the bounded runner's maximum 600 s (research_tree.RUNNER_MAX_SECONDS; run_bounded_research.py refuses it)` |
| `scripts/tests`, PYTHONHASHSEED=0 (`scripts/tests/run_two_seeds.py`) | **323 passed, 4 skipped**, 0 failed (403.5 s) |
| `scripts/tests`, PYTHONHASHSEED=1 | **323 passed, 4 skipped**, 0 failed (329.6 s); runner exit 0 |
| `atx-engine/tools` | **348 passed**, 6 subtests passed, 0 failed (232.7 s) |
| `atx-impl/tools` | **625 passed, 2 skipped**, 17 subtests passed, 0 failed (255.0 s) |
| amendment (pre-registration commit) | **`4492f3015346f71ecde46199dbd0ed62ad23a82c`** `prereg(v8): amend Y-S wave manifest per DEC-1, DEC-2 (R0-2)`: `y-s.json` and `y-s.head.json` `marginal.seconds` 720 -> 600 (DEC-1); budget `admission_cycle_prefix "v8x"` -> `admission_cycle_prefixes ["v8x", "v8ys"]` (DEC-2); 2 files, +7 / -4, nothing else; head = full manifest minus `candidates` (True); `wave_manifest.validate(head + candidates)` = `[]` |
| `wave plan scripts/specs/v8/waves/y-s.json` | **exit 0**, 57 lines; manifest sha256 `22b5534f79dae93fb40d7e11f802211668c98b93123ee28fafa4c54aa05d8ac4`; stages 01 preflight .. 09 record all `pending` |

Fixes made: none. Build: none. Open item: both descriptions still say "marginal pool only with a 720 s cap" (E1 report:
"root's wording to update, not checked"); left as is because R0-2's dispatch allows no other change; the binding value is
`marginal.seconds` 600. Hidden data: no real-data run; no output, receipt, NAV, IC or ledger value opened (the plan prints
argv and receipt states only); nothing dated 2024-01-01 or later opened. `C:/atx`, `atx-db/` untouched; no push.
Next: R0-6.

## P9 Phase 0, root step R0-6: cell Y-S through the wave driver, stage by stage (v8y P9-P12; DEC-1; 2026-10-03)

Root in `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, start `4a5e84a0` (code pathspec clean; only the untracked
`docs/plans/2026-10-02-x5-equity-curve.png`, not root's). Read: P9 plan sections 0.5 (DEC-1, DEC-2), 0.6, 1.1 (R0-6 and its
pre-ruled marginal fallback), 1.2; `v8y-prereg.md` sections 3, 5, 6 (Y-S row), 7, 12, 13 (P9-P12), 14; integrator-rules;
global-constraints; `y-s.json` (manifest sha256 `22b5534f`, commit `4492f301`); E1's task-0 report (concerns: forced ref,
capacity completeness); root's R0-3..R0-5 report; driver code `research_wave.py`, `wave_stages.py`, `wave_stage_*.py`,
`wave_context.py`, `wave_steps.py` (marginal rewrite), `wave_result.py` (marginal rows), `wave_scoreboard.py`.
State dir `build-equity/waves/y-s/` does not exist yet (`wave status`: preflight pending, the rest blocked).

### Plan, written before stage 01 runs

Command per stage: `"C:/Program Files/Python312/python.exe" scripts/research_cycle.py wave run scripts/specs/v8/waves/y-s.json
--until <stage>`, one stage at a time, one real run at a time, no build (unless the pre-ruled fallback needs the IC exe).

**Memory gate before each stage that runs an exe: free physical memory >= planned peak + 1,536 MiB, else wait (poll 60 s,
up to 30 min).** Planned peaks (MiB) and gates:

| stage | exes it runs | planned peak (source) | gate free MiB |
|---|---|---|---|
| 01 preflight | none (Python checks) | - | - |
| 02 register | IC exe `--plan-only` x 15 (add-alpha K1, metadata only) | 2 (R0-5 plan-only receipt) | 1,538 |
| 03 screen | u (IC), fit, card, marginal (pool only, 600 s), gate | 1,953 (u admission 1,952.5, R0-5; card 1,554 and marginal 296 measured on X-7) | 3,489 |
| 04 spec | none if every string is kept; add-alpha `--plan-only` per kept string for a b library | 2 | 1,538 |
| 05 run | ref NAV (forced: fields v15 vs v13 and NAV exe `72ff6d2d` vs X-5's `a95f6f0a`), w (IC), NAV; a b library re-screens first | 2,786 (w admission, R0-5 arith; cap 3,072) | 4,322 |
| 06 match | mechanics reader; a -gm NAV if gross misses by > .005 | 586 (NAV, P8) | 2,122 |
| 07 verify | none | - | - |
| 08 judge | monitor, summ (nav_summ), bundle, book reader (Python) | 1,536 (cap; not measured) | 3,072 |
| 09 record | none | - | - |

**Hand plan for stage 01 (preflight; expected receipt `build-equity/waves/y-s/receipts/01-preflight.json`, exit 0):**
- manifest `scripts/specs/v8/waves/y-s.json`, sha256 `22b5534f79dae93fb40d7e11f802211668c98b93123ee28fafa4c54aa05d8ac4`, commit
  `4492f3015346f71ecde46199dbd0ed62ad23a82c`; code pathspec clean.
- parent `scripts/specs/v8/x-theme-erc-gm.json`: name `v8x-theme-erc-gm`, L 1.1720, NAV `build-equity/mega-nav-v8x-theme-erc-L1.1720`
  (`summary.json` `a03937cf...`; `capacity_curve.csv` and `v7_extras.json` present by name: the P0-FIX completeness rule is met),
  summ ledger `build-equity/trials.jsonl` = the manifest's; library `v8x3b`.
- fields `build-equity/train-2020-2023-lo3-fields-v15`, manifest `26fee5ce301b9b0bffa1d72b45e014d973d3d73a080dd59ea55cda976f133b09`,
  84 rows, status complete, `seal.exclusive_end` 2024-01-01; every candidate's declared fields in it (the 6 new ones included).
- no sealed year in the fields dir, parent path, `build-equity/waves/y-s`, `v8ys`.
- ledger 112 lines, head `d1484e69...`, N 56 = `expect.n_before` 56.
- budget `v8x-hand-25-plus-y-15`: admission used **25** (X hand-written: cycles `v8x*`; X-4's 9 re-screens left out; no `v8ys*`
  line yet), new **15** (the roster, in order), cap 40 (25 + 15 = 40, holds); prefixes `["v8x", "v8ys"]`; origin null;
  construction cap 62 (56 + 1 = 57, holds).
- queue: the 15 `scripts/specs/v8/candidates/<id>.json` pinned with the manifest's DSL SHA-256 (no problem line).
- window `research-window-v2`, seal 2024-01-01.

**Expectations for the later stages (checked when each runs):**
- 02 register: 15 add-alpha exits 0 (`--name v8ys`, `--save-plan build-equity/waves/y-s/plans/v8ys/<id>.json`); library v8ys =
  v8x3b's 58 + the 15 in roster order = 73 members; its IC library bytes compared with R0-5's scratch build
  (`bbcfbf9d...`, not of record); `lib-v8ys.json` rewritten once by the PM8-15 ruling (marginal pool only, `-poolonly`
  output, `runner.phases.marginal.seconds` 600), lock dry; one commit of exactly add-alpha's files.
- 03 screen: u, fit, card, marginal, gate under the bounded runner (u 300 s / 2,560 MiB; card 300 / 2,560; marginal 600 /
  1,536; fit 180 / 1,536); u-compare of the 58 parent rows identical (P8: the parent reads no new field); 15 admission lines
  ledgered (ledger 112 -> 127 lines, N stays 56); the PM7-35 sign rule on the 15 rows. Gate exit 10 = no cell (logged, 0).
- **Marginal fallback (pre-ruled, plan R0-6):** a time-cap failure (nothing written) gets ONE blind re-run on a quiet host
  (no compiler running, free >= peak + 1,536 MiB); a second failure moves the marginal to the Release IC exe under v8y P6 after
  its u / w identity (0 trials), building only the IC exe with `scripts/research-build.ps1` tag `p9-0r` if needed.
- 04 spec: every string kept -> the cell is the screen library (`lib-v8ys.json`); some dropped -> b library `v8ysb` (same trial
  ids, 0 new lines) with `speed.reuse_screen_marginal`: no second marginal pass; **b-reuse check (P12, once):** the
  per-row marginal fields carried into the wave result equal the screen's `marginal_ic.json` rows of the kept ids byte for
  byte (compared by canonical-JSON SHA-256, no value printed). If no string is dropped the b path does not run and the check
  is logged as not applicable (the screen-library rows checked equal instead).
- 05 run: ref compare `ref-s2-daily` reproduces X-5's S2 daily `529062d6` (P8 precedent) or exit 4 = stop; w, NAV.
- 06 match: PM6-6 (tolerance .005 on all-rows S2 gross vs X-5's .9862260459), at most one correction.
- 07 verify: mechanics (all-rows gross [.90, 1.05], |mean net| <= .02, tau mean <= .20, p95 <= .30) before any return is read;
  C-13 binding; NAV exe parent vs cell (differ: ref must have run on the cell's exe); seal scan.
- 08 judge: **acceptance PM7-34: paired S2 net dSR > 0 against X-5 AND mechanics**; printed only: capacity 4x, turnover per
  gross, cost bps; bundle both p.
- 09 record: ledger N 56 -> 57 (the cell's line among the lines appended since preflight); wave-result.json; copies to
  `.superpowers/sdd/platform-v8-20260929/waves/y-s/`. **Scoreboard 4x check (P12):** `research_cycle.py scoreboard` 4x net
  Sharpe row against the 4x row of one real `capacity_curve.csv` read directly.

### Stages 01-02 (source `c4201d94`; argv `wave run scripts/specs/v8/waves/y-s.json --until <stage>`)

| stage | exit | wall s (process / receipt) | peak MiB | free before / min during | receipt (SHA-256) | result |
|---|---|---|---|---|---|---|
| 01 preflight | 0 | - / 0.30 | - (no exe) | 4,879 / - | `01-preflight.json` `f4f759088ae95ac284e224e99fc3ab9346b1684e168df5db98ee87918cf59d14` | **= hand plan on every item**: manifest commit `4492f301`, sha `22b5534f`; parent `v8x-theme-erc-gm` L 1.1720, NAV `mega-nav-v8x-theme-erc-L1.1720`, spec digest `92131961`; fields `26fee5ce`, 84 rows, seal 2024-01-01; ledger 112 lines, head `d1484e69a5f2...`, N 56; budget used 25 + new 15 (roster order) <= 40, prefixes `["v8x", "v8ys"]`, construction cap 62; window research-window-v2 |
| 02 register | 0 | 9.1 / 8.94 | 2 per plan-only (R0-5) | 4,850 / 4,675 | `02-register.json` `facb7fb20cfb09768450377d70de6993e3276882a844dc6458fa153a48f8de2d` | 15 add-alpha exit 0; K1 plans of record `build-equity/waves/y-s/plans/v8ys/<id>.json` (59..73 rows; e.g. `peer_mom_1m` 6 slots / 41 bars, `iv_vol_of_vol` 5 / 41 / `iv_atm_21d` = the repaired string `4d42a72b`, `dato` 5 / 272 / 4 extra fields, `conn_rev` 3 / 0); **IC library `fund_industry_ic_v8ys.json` `bbcfbf9dd000dc41055e2415ddc1a0047811a796752de55fd4e92ab636f4e9bd` = R0-5's scratch build byte for byte** (73 members); `lib-v8ys.json` rewritten once by PM8-15 (marginal `-poolonly`, `runner.phases.marginal.seconds` 600), lock dry; spec `05732a95`; driver commit **`a5914373`** (exactly the 6 add-alpha files: registry, IC library, recipe, library def, prereg stub, spec) |

Gate list of `lib-v8ys.json`: the 15 strings, roster order, require any, sign_agrees. u 300 s / 2,560 MiB, w 300 / 3,072,
card 300 / 2,560, marginal 600 / 1,536 (default rss), others 180 / 1,536.

### Stage 03 screen (source `c25f7064`; gate 3,489 MiB: free 4,952 at 08:12:15, no compiler running)

argv (driver): `research_cycle.py run scripts/specs/v8/lib-v8ys.json --screen --root C:/atx-wt/pool-2`. Stage exit 0, wall
301.7 s (receipt 301.53 s); minimum system free during the stage 3,427 MiB. Receipt `03-screen.json`
`8562e09b672d089d502b004b0e1e02d8b7f472f9fc2aea1fcdd26f66e28ce11f`; console `consoles/019-screen.log`.

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | result |
|---|---|---|---|---|---|---|
| u | 300 / 2,560 | 35.2 | 1,181 | 0 | `186ca092fb811b8fa5501eadc7c5ea749d2b525f9ea3a25e348152c67fac2137` | ic `985019d9` (v8-16d); **u-compare IDENTICAL**: orientations 58 objects (b adds 15), `train_daily_ic.csv` 169,708 rows of 58 keys byte for byte (b adds 43,890 rows of 15) |
| fit | 180 / 1,536 | 8.7 | 452 | 0 | `82c5b16a7afce0611061c3c5a72337e0ac0330c735b5ed1f6f4e56dd97d2f413` | admission `b922673b` |
| card | 300 / 2,560 | 31.6 | 1,558 | 0 | `ac133b10f4714c9356361b2d0cb38d8efa1ba82fc02507199941a96968766e9a` | - |
| marginal (pool only) | **600** / 1,536 | **223.5** | 252 | 0 | `542b2e5e064c66ee2e880a037632caeff4d0089d06dbaaf626c196635b3b84bb` | `marginal_ic.json` `3df5f949` (73 rows); 37% of the cap: **no time-cap failure, the pre-ruled fallback did not trigger** |
| gate p1-v8ys | internal | - | - | **PASS** (exit 0) | - | **15 admission lines appended, 0 already ledgered** (ledger 112 -> 127 lines, file `869a0c6a`; N stays 56); reference members vs X-5's admission: 0 status changes |

**Admission (v4-prior-v1) and the PM7-35 sign rule, applied by the driver's code (`wave_rules.sign_pm7_35`):** 7 of 15 admitted
with the prior sign: `iv_vol_of_vol`, `day_rev_freq`, `mom_turn`, `ea_uvol`, `dato`, `stio_trade`, `deal_target` (keep).
`fscore_hbm` admitted with runner sign 0 (keep, R-2 precedent). `so_wang_rev` reject_turnover (tau .9659 > .70), runner sign +1
(keep at weight 0). **Admitted with runner sign -1 against prior +1 -> dropped: `peer_mom_1m`, `exch_switch`, `ins_cluster`,
`smile_slope`, `div_event`, `conn_rev`.** Kept 9, dropped 6 -> the cell is the b library **v8ysb** (v8x3b + the 9 kept, 67
members; same trial ids, 0 new lines). Admission trials: X hand-written 25 + Y hand-written 15 = 40 (cap 40).

**P12 b-reuse, first half:** the screen receipt's carried marginal rows equal the source `marginal_ic.json` rows 15 of 15 on
all 7 K6 keys (canonical-JSON SHA-256 per row; `scratchpad/mcheck.py screen`). The second half (the wave result's carried
per-row fields vs the source) is checked after the record stage.

### Stage 04 spec (source `02d524d3`; gate 1,538 MiB: free 5,235)

| stage | exit | wall s (process / receipt) | peak MiB | free before / min during | receipt (SHA-256) | result |
|---|---|---|---|---|---|---|
| 04 spec | 0 | 7.4 / 7.16 | 2 per plan-only | 5,235 / 5,080 | `04-spec.json` `ce88ee2925fc67b52dc794d918dc38578a6637da0ae7d832676cd51781bff1e9` | kind **b-library**: 9 add-alpha `--name v8ysb` exit 0 (plans `waves/y-s/plans/v8ysb/`); IC library `fund_industry_ic_v8ysb.json` `41010b0b` (67 members); `lib-v8ysb.json` `ae4808b8` (= spec digest): marginal phase and its runner cap removed (`speed.reuse_screen_marginal`, mode pool-only = the screen's: reuse true); gate p1-v8ysb lists the 9 kept; NAV `mega-nav-v8x-theme-erc-L1.1720-v8ysb` at L 1.1720, ref `...-v8ysb-ref`, paired reference = X-5's NAV; driver commit **`816be40b`** (5 files: IC library, recipe, library def, prereg stub, spec; registry unchanged) |

### Resume after the owner stop (PM session 2; 2026-10-03): lock, receipts 01-04, the killed stage-05 attempt -- STOPPED for a ruling

Stage-04 rows committed alone first: `d66f93f6` `log(p9): R0-6 Y-S stage 04 spec (816be40b)`.

**chain.lock (PM resume ruling, P9 `progress.md`: "root removes the stale y-s chain.lock only after confirming pid 25424 is
not alive and logs it"):** `build-equity/waves/y-s/chain.lock` held `{"pid": 25424, "started_utc": "2026-10-03T12:19:29+00:00"}`
(file sha256 `35b22df35d5d5ca9c5e646e1d34dc2da17879a8006ce0d899054f24d5ba2dff1`). `Get-Process -Id 25424`: not found;
`tasklist /FI "PID eq 25424"`: no task (12:31:13Z); process scan: no research python, compiler, cmake or ninja alive (editor
mypy / formatter servers only). **Lock removed 12:35:03Z** (the remedy `stage_chain.Chain.acquire` names: "check that no
run is alive, then remove it").

**Receipts 01-04 under the driver's resume logic** (`wave status scripts/specs/v8/waves/y-s.json`: `Chain.state` re-reads
every receipt and recomputes its inputs, runs nothing, takes no lock): exit 0; preflight, register, screen, spec **done**;
run pending; match, verify, judge, record blocked. SHA-256 of the four receipts = the logged values (`f4f75908`,
`facb7fb2`, `8562e09b`, `ce88ee29`). No receipt deleted; no `05-run.failed-*` receipt exists (the kill raised nothing
inside the chain).

**The killed stage-05 attempt (ruled: a failed attempt, not a cell, 0 trials).** Read from console 032 and the bounded
runner's run dirs; free / min during not recorded (the previous root was killed):

| step (attempt under lock 12:19:29Z) | argv (driver) | exit | s | peak MiB | run dir | result |
|---|---|---|---|---|---|---|
| b-library screen (its gate re-read) | `research_cycle.py run scripts/specs/v8/lib-v8ysb.json --screen --root C:/atx-wt/pool-2` | 0 | u 12.7, fit 1.9, card 24.5 | u 530, fit 58, card 1,524 | `...-u-v8ysb-run1`, `...-v8ysb-run1` (fit), `...-cards-...-v8ysb-run`: completed exit 0 | console `consoles/032-the-b-library-s-screen--its-gate-re-read.log`; u-compare **IDENTICAL** (58 objects; 169,708 rows of 58 keys; b adds 9 objects / 26,334 rows); ledger 0 appended, 9 already ledgered; gate p1-v8ysb **PASS** (7 of 9 listed admitted with the prior sign; reference members 0 status changes); verdict `build-equity/cycle-v8ysb/cycle_verdict.json` |
| calibration run: ref | `research_cycle.py run scripts/specs/v8/lib-v8ysb.json --stop-after nav --root C:/atx-wt/pool-2` | completed exit 0 | 61.9 | 586 | `mega-nav-v8x-theme-erc-L1.1720-v8ysb-ref-run` (receipt) | primary daily CSV sha256 `529062d6f06f...` (as `research_cycle.py status` prints it; = X-5's S2 daily); the ref-compare step did not run |
| calibration run: w attempt 1 | (same process) | **killed** | - | - | `mega-v8xw-train-theme-erc-v8ysb-run1`: `start.json` 12:21:16Z, **no receipt**; output `-1` partial | - |
| nav, monitor | - | not started | - | - | - | - |

No console exists for the killed command (the driver writes one when a command returns). Ledger after the attempt:
`build-equity/trials.jsonl` 127 lines, sha256 `869a0c6a18ec...` = the post-stage-03 record: **0 trials** added.

**Why stage 05 was not started (brief stop rule: a refusal; nothing improvised).** `research_cycle.py status
scripts/specs/v8/lib-v8ysb.json` (read-only) prints `w failed build-equity/mega-v8xw-train-theme-erc-v8ysb-1 | attempt left
build-equity/mega-v8xw-train-theme-erc-v8ysb-run1 without a receipt; never overwritten: rerun with --attempt w=2`. The cycle
picks an attempt by itself only among complete ones (`ic_attempt`, research_cycle.py:909-928) and its run loop raises
`HARD-STOP [w]` on a failed step (:1690); `wave run` takes no `--attempt` (research_wave.py: `--root`, `--until`,
`--dry-run`, `--seal-allow`) and the run stage calls `run <cell> --stop-after nav` without one (wave_stage_cell.py
`run_stage`). So `wave run ... --until run` would HARD-STOP at w (exit 4, `05-run.failed-1.json`). The PM's resume ruling
re-runs stage 05 "from receipts 01-04" but does not name the attempt; root asks for a ruling (options in the R0-6 report)
and does not run stage 05. Nothing was built; no exe launched by root; nothing dated 2024-01-01 or later opened; `C:/atx`,
`atx-db/` untouched; no push.

### R0-6-ATT: w attempt 2 by the cycle's documented recovery (PM ruling R0-6-ATT, P9 `progress.md`:102; option A)

`mcheck.py` copied first to `.superpowers/sdd/platform-p9-20261003/tools/mcheck.py` (sha256 `51db5e18587f...` = the scratchpad
copy). Gate 4,322 MiB: free 5,573 at 12:42:23Z, no compiler, no research process. Run **once**:
`"C:/Program Files/Python312/python.exe" scripts/research_cycle.py run scripts/specs/v8/lib-v8ysb.json --stop-after nav
--attempt w=2 --root C:/atx-wt/pool-2` -- **exit 0, wall 114 s (12:42:37Z-12:44:31Z), min free during 3,546 MiB (12:44:14Z),
no compiler seen.** Attempt 1 (`mega-v8xw-train-theme-erc-v8ysb-run1` / `-1`) stays on disk as it was: killed by the owner
stop, no receipt, 0 trials.

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | result |
|---|---|---|---|---|---|---|
| fields | - | - | - | done | - | v15 `26fee5ce`, 84 rows == the spec list |
| ref | (attempt 05-killed) | 61.9 | 586 | done | binding argv `027092b5` | **ref-s2-daily IDENTICAL** bit for bit (941,374 bytes, `529062d6`) vs X-5's S2 daily |
| u / fit / card | - | - | - | done | - | u-compare IDENTICAL (58 objects; 169,708 rows of 58 keys; b adds 9 / 26,334) |
| gate p1-v8ysb | internal | - | - | **PASS** | - | 0 admission lines appended, 9 already ledgered; 7 of 9 with the prior sign; reference members 0 status changes |
| **w attempt 2** | 300 / 3,072 | **52.8** | 1,436 | 0 | `543860bb562ced79bdc14b24b53f75b67926fbc6ce7f3e1e441e610403b57453` | exe `985019d9`; output `mega-v8xw-train-theme-erc-v8ysb-2`, `train_combined.json` `e13fbc4d` |
| **nav** | 180 / 1,536 | **59.7** | 586 | 0 | `26fdb2820217fb292feed665acc49b7548b7036bfb3c46cd06718a08df8d1525` | exe `72ff6d2d`; `mega-nav-v8x-theme-erc-L1.1720-v8ysb` at L 1.1720 (`summary.json` `26921704`; `capacity_curve.csv`, `v7_extras.json` present); binding `41c1da0e` |

Ledger after: 127 lines, `869a0c6a` (unchanged: 0 trials). Nothing read from the NAV beyond file names and hashes.

### Stage 05 run (source `de5823f2`; gate 4,322 MiB: free 5,191 at 12:45:46Z, no compiler running)

| stage | exit | wall s (process / receipt) | peak MiB | free before / min during | receipt (SHA-256) | result |
|---|---|---|---|---|---|---|
| 05 run | 0 | 2.3 / 2.08 | - (no exe launched: every phase done) | 5,191 / 5,134 | `05-run.json` `eb23472eb751dd75a54edc2d872658050b2cc1a598a85d8703acddd143612571` | **the driver adopts attempt 2**: console 033 (`run lib-v8ysb.json --screen`, exit 0: u/fit/card done, 0 admission lines appended / 9 already ledgered, gate p1-v8ysb PASS), console 034 (`run lib-v8ysb.json --stop-after nav`, exit 0, attempts auto): ref done (binding `027092b5`), **ref-s2-daily IDENTICAL** (`529062d6`, 941,374 bytes), u-compare IDENTICAL, **w: done (`mega-v8xw-train-theme-erc-v8ysb-2`)**, **nav: done** (binding spec `ae4808b8`, argv `84d9944c`); no w or nav launched a third time. Receipt phases: u run1, fit run1, card run, ref run, **w run2** (52.8 s, 1,436 MiB, exe `985019d9`), nav run (59.7 s, 586 MiB, exe `72ff6d2d`); w run1 (killed, no receipt) has no row. NAV `mega-nav-v8x-theme-erc-L1.1720-v8ysb`, `summary_sha256` `26921704`. No driver commit (the run stage commits nothing). Ledger 127 lines, `869a0c6a` (0 trials) |

### Stage 06 match (source `843fe55c`; gate 2,122 MiB: free 4,847 at 12:46:25Z, no compiler running)

| stage | exit | wall s (process / receipt) | peak MiB | free before / min during | receipt (SHA-256) | result |
|---|---|---|---|---|---|---|
| 06 match | 0 | 59.3 / 59.06 | 586 (matched NAV); readers 44 | 4,847 / 3,879 | `06-match.json` `b23bd9289ee3e9e52616868e91ec31d9cdb58ca2af9f1c7353caa3963d66c5ce` | PM6-6 (tolerance .005): calibration at L 1.1720 **G 0.9772114158 vs G_parent 0.9862260459** (miss .0090 > .005) -> `lib-v8ysb-gm.json` at **L 1.1828** (spec digest `1e3ec118`), lock dry exit 0, **driver commit `c0f1fae6`** (exactly the -gm file); matched run `run lib-v8ysb-gm.json --stop-after nav` exit 0 (console 039: ref, ref-s2-daily IDENTICAL `529062d6`, u-compare IDENTICAL, gate PASS with 0 lines appended, **w: done (attempt 2 reused)**, nav attempt 1 55.6 s / 586 MiB, receipt `fbfb6208b985...`, exe `72ff6d2d`); **matched G 0.9862134133 vs G_parent 0.9862260459**, within tolerance after 1 correction. Cell NAV `mega-nav-v8x-theme-erc-L1.1828-v8ysb` (`summary.json` `2d30b7f6`). Readers: `mech-calibration.json` `56d6be1c`, `mech-matched.json` `811be8ec` |

### Stage 07 verify (source `4833e844`; no exe, no memory gate; free 4,557 at 12:48:14Z, no compiler running) -- STOPPED for a ruling

| stage | exit | wall s (process / receipt) | peak MiB | free before / min during | receipt (SHA-256) | result |
|---|---|---|---|---|---|---|
| 07 verify | **4** | 0.7 / 0.41 | - (no exe) | 4,557 / 4,634 | `07-verify.failed-1.json` `ae50e347fe7e076835039b37a40a5178e6d616ebca33831ad48353268925ec25` (no ok receipt) | mechanics (v8-mech) on the -gm cell, as printed: gross_all_rows 0.9862134133 in [0.9, 1.05] pass; abs_net_all_rows 0.0053399819 <= 0.02 pass; tau_mean 0.0281343967 <= 0.2 pass; tau_p95 0.0334104438 <= 0.3 pass; max_return_identity_error 3.7e-16 <= 1e-9 pass; max_cash_book_relative_error 1.1e-13 <= 1e-9 pass. C-13 binding and NAV-exe check: not in the stage's problem list. **HARD-STOP: "9 date token(s) at or after the seal 2024-01-01" in consoles 019, 032, 033, 034, 035, 039, 040: "stop for the PM's ruling (the cell ran; no ledger line was written)"** |

The 9 tokens, classified with `wave_seal.tokens` (token, form and the 40 characters before it only; no data line opened):
7 x `2026-10-02` (iso) = the file name `docs/plans/2026-10-02-x5-equity-curve.png` (untracked, not root's, outside the code
pathspec), printed by research_cycle's dirty check ("dirty outside the code pathspec (listed, not a stop)") in consoles
019/032/033/034/039 and in the reader receipts' `dirty_outside_pathspec` in 035/040; 2 x `2026-10-03` (iso) = `"started_utc"`
of the bounded-runner receipt the mechanics reader prints (consoles 035, 040). None is a data date. No precedent:
y-s is the first wave whose verify stage has run (the log holds no `--seal-allow` ruling). The driver's remedy is
`research_wave.py run ... --seal-allow TOKEN=RULING` (kept by the verify receipt for the record stage's second scan).
Ledger 127 lines, `869a0c6a` (0 trials). Root stops for the PM's ruling (brief: a validator refuses).

### Stage 07 verify, second attempt under PM ruling SEAL-ALLOW (P9 `progress.md`:108; source `88b51a3c`; no exe; free 5,333 at 12:53:29Z, no compiler)

**Per-hit source check before use** (`.superpowers/sdd/platform-p9-20261003/tools/sealsrc.py`: the files
`wave_seal.wave_logs` returns over receipts 01-06 = 68 files, the scanner's own regexes, seeds and seal-reference rule;
prints file, line, token, form, source only). 9 hits = the scan's 9; **OTHER 0**:

| file (`build-equity/waves/y-s/consoles/`) : line | token | source |
|---|---|---|
| `019-screen.log`:4 | 2026-10-02 | (a) dirty list: `docs/plans/2026-10-02-x5-equity-curve.png` |
| `032-the-b-library-s-screen--its-gate-re-read.log`:4 | 2026-10-02 | (a) dirty list |
| `033-the-b-library-s-screen--its-gate-re-read.log`:4 | 2026-10-02 | (a) dirty list |
| `034-calibration-run----stop-after-nav.log`:4 | 2026-10-02 | (a) dirty list |
| `035-mechanics-reader.log`:7 | 2026-10-03 | (b) receipt `"started_utc"` wall-clock |
| `035-mechanics-reader.log`:93 | 2026-10-02 | (a) receipt `dirty_outside_pathspec` list |
| `039-matched-run----stop-after-nav.log`:4 | 2026-10-02 | (a) dirty list |
| `040-mechanics-reader.log`:7 | 2026-10-03 | (b) receipt `"started_utc"` wall-clock |
| `040-mechanics-reader.log`:63 | 2026-10-02 | (a) receipt `dirty_outside_pathspec` list |

argv: `wave run scripts/specs/v8/waves/y-s.json --until verify --seal-allow "2026-10-02=untracked owner plot file name
docs/plans/2026-10-02-x5-equity-curve.png in dirty list (PM SEAL-ALLOW)" --seal-allow "2026-10-03=receipt started_utc
wall-clock (PM SEAL-ALLOW)"`

| stage | exit | wall s (process / receipt) | peak MiB | free before / min during | receipt (SHA-256) | result |
|---|---|---|---|---|---|---|
| 07 verify | 0 | 0.9 / - | - (no exe) | 5,333 / 5,357 | `07-verify.json` `82f72a1b11c10d018b17fcc31c7aba7c5148036ee972440770f081bd1c63fe86` | as printed: mechanics (6 rows as in the first attempt) **PASS**; binding `a9bf5d2a9177` (spec `1e3ec118273c`); **seal scan 68 log(s), 0 tokens** (allowed: 2026-10-02 x7 in 7 files, 2026-10-03 x2 in 2 files, rulings kept in the receipt); NAV exe `72ff6d2d7070` vs parent `a95f6f0af069` (ref ran: equal false, ref-s2-daily identical). The first attempt's `07-verify.failed-1.json` stays for the record |

### Stage 08 judge (source `4284b822`; gate 3,072 MiB: free 5,303 at 12:54:09Z, no compiler running)

| stage | exit | wall s (process / receipt) | peak MiB | free before / min during | receipt (SHA-256) | result |
|---|---|---|---|---|---|---|
| 08 judge | 0 | 46.5 / 46.28 | 626 (summ) | 5,303 / 4,524 | `08-judge.json` `5fdc3b015ac4fe4cfe7d9c9fa76e33ab80310a158d5e076a8372ce3fac0a9c46` | driver prints: **`verdict (pm7-34): ACCEPTED {'dsr_positive': True, 'mechanics': True, 'criteria': False}`**. Ledger 127 -> **128 lines** (file `f665ed9e`; the cell's line, re-read by the record stage). No driver commit |

| phase | caps | s | peak MiB | exit | receipt.json SHA-256 | output |
|---|---|---|---|---|---|---|
| `research_cycle.py run scripts/specs/v8/lib-v8ysb-gm.json` (console 041): monitor | 180 / 1,536 | 1.6 | 128 | 0 | `95f14829eaab5922560168435ccf89db9c2b2931a49fc32427514a747460309a` | `mega-monitor-v8x-theme-erc-v8ysb` |
| summ (nav_summ, ledgers the cell) | 180 / 1,536 | 40.0 | 626 | 0 | `a769a6c31ca2554bf6c90eb937cd06c61912d403cd50f64593dfc8b3e5802806` | `cycle-v8ysb-gm/summ.json` `0ef36000`, `cycle_verdict.json` `ea3112ec` |
| bundle PM5-23 (console 042) | 180 / 1,536 | 1.0 | 318 | 0 | `929e7fad938f46aefd500ca7b9f6bac4e52e5d394182e75d116142c44822a19d` | `waves/y-s/bundle.json` `570018ba` (X-5 NAV vs the cell) |
| book reader (console 043) | 180 / 1,536 | 0.5 | 45 | 0 | `1c949fd68c748491ed50993a5c4308d056e41d650f9af0e3dda7ab077bbd850e` | `readers/book.json` `9f895ca9` |

The statistics of record (dSR, SE, p, DSR, PBO, the printed-only criteria) are pasted from the driver's own log section
after the record stage.

### Stage 09 record (source `f4808a37`; no exe; free 4,759 at 12:56:41Z, no compiler running)

**Per-hit source check before the record stage's second scan** (`tools/sealsrc.py`, 79 files): 14 hits, **OTHER 0**: the 9
of stage 07 plus `041-the-cell-s-monitor-and-summ--nav-summ-scores-and.log`:4 2026-10-02 (a) dirty list;
`042-bundle--pm5-23.log`:7 2026-10-03 (b) `"started_utc"`, :93 2026-10-02 (a) receipt `dirty_outside_pathspec`;
`043-book-reader.log`:7 2026-10-03 (b) `"started_utc"`, :93 2026-10-02 (a) receipt `dirty_outside_pathspec`.

| stage | exit | wall s (process / receipt) | peak MiB | free before / min during | receipt (SHA-256) | result |
|---|---|---|---|---|---|---|
| 09 record | 0 | 1.0 / - | - (no exe) | 4,759 / 4,605 | `09-record.json` `7c1079381b22d8b723cf4da12a95117bf8a8d0474c729257d1b8589616f80924` | `wave-result.json` `57e9f5ea4a56f1147c08442d99f4cc03535ae7ea57ca7b1cc2cb8f4774175ac0`; log section `wave-log.md` `3b872598`; **N 56 -> 57**; ledger 128 lines, head `e186895aeed3f7f3`, cell trial `11c10defb3cf38a5`; seal scan 79 logs, 0 tokens (2026-10-02 x10, 2026-10-03 x4 allowed by SEAL-ALLOW; seeds 20260927 x1, 20260929 x66); copies at `.superpowers/sdd/platform-v8-20260929/waves/y-s/` byte-equal; **driver commit `fd55d436`** (queue status of the 15 candidate files, "(ACCEPTED)") |

**P12 b-reuse, second half** (`.superpowers/sdd/platform-p9-20261003/tools/mcheck.py result`; canonical-JSON SHA-256 per
row, no value printed): source `build-equity/mega-v8-b0b-train-u-v8ys-marginal-poolonly/marginal_ic.json` `3df5f949` (73
rows); the result's marginal block: source "the screen (lib-v8ys.json): per-row fields carried, max_abs_rho,
max_rho_member left null (over the screen library)", mode pool-only, 9 rows -> **9 of 9 rows byte-equal on keys id,
ic21, ic21_hac_t, marginal_ic21, marginal_hac_t**; max_abs_rho and max_rho_member null in all 9 rows (as the source line
says). With the first half (15 of 15 on 7 keys, stage 03): **b-reuse verified**.

**P12 scoreboard 4x row** (`research_cycle.py scoreboard --root C:/atx-wt/pool-2`, exit 0; reads only wave results and
the ledger): y-s 4x net SR `1.6994264697927697` = the `capacity-x4-v1+swap-fin-v1` row's `net_sharpe` of
`build-equity/mega-nav-v8x-theme-erc-L1.1828-v8ysb/capacity_curve.csv` (`edf1001d`, read directly) byte for byte;
parent row 4x `1.654876435267752` = `mega-nav-v8x-theme-erc-L1.1720/capacity_curve.csv` (`42ef4ea9`) 4x `net_sharpe`
`1.6548764352677521` (the same IEEE double). Ledger check: "y-s: trial `11c10defb3cf38a5` ledgered; s2_net_sr equal".
**Scoreboard 4x verified.**

**The driver's log section** (`build-equity/waves/y-s/wave-log.md`, pasted as written):

### Cell y-s (library wave; library v8ys screen, then v8ysb on v8x3b): N 57

**ACCEPTED.** Manifest `scripts/specs/v8/waves/y-s.json` sha256 `22b5534f79dae93f` (commit `4492f3015346`); parent `scripts/specs/v8/x-theme-erc-gm.json` (library v8x3b, L 1.1720); driver `research_cycle.py wave run`.

Budget v8x-hand-25-plus-y-15: admission trials 25 + 15 new = 40 of 40 (cycles v8x*, v8ys*; re-screens left out); construction N 56 -> 57 of 62.

**Screen** (`scripts/specs/v8/lib-v8ys.json`, gate exit 0, sign rule pm7-35): kept so_wang_rev, iv_vol_of_vol, day_rev_freq, mom_turn, ea_uvol, dato, fscore_hbm, stio_trade, deal_target; dropped peer_mom_1m, exch_switch, ins_cluster, smile_slope, div_event, conn_rev.

| id | kind | prior | status | runner sign | decision | reason |
|---|---|---|---|---|---|---|
| peer_mom_1m | add | +1 | admitted | -1 | drop | addition admitted with runner sign -1 against prior +1: dropped from the wave |
| so_wang_rev | add | +1 | reject_turnover | 1 | keep | addition not admitted (status reject_turnover): stays at weight 0 |
| iv_vol_of_vol | add | +1 | admitted | 1 | keep | addition admitted with the prior sign |
| day_rev_freq | add | +1 | admitted | 1 | keep | addition admitted with the prior sign |
| mom_turn | add | +1 | admitted | 1 | keep | addition admitted with the prior sign |
| ea_uvol | add | +1 | admitted | 1 | keep | addition admitted with the prior sign |
| dato | add | +1 | admitted | 1 | keep | addition admitted with the prior sign |
| fscore_hbm | add | +1 | admitted | 0 | keep | addition admitted, runner sign 0 (R-2 precedent) |
| exch_switch | add | +1 | admitted | -1 | drop | addition admitted with runner sign -1 against prior +1: dropped from the wave |
| ins_cluster | add | +1 | admitted | -1 | drop | addition admitted with runner sign -1 against prior +1: dropped from the wave |
| smile_slope | add | +1 | admitted | -1 | drop | addition admitted with runner sign -1 against prior +1: dropped from the wave |
| stio_trade | add | +1 | admitted | 1 | keep | addition admitted with the prior sign |
| div_event | add | +1 | admitted | -1 | drop | addition admitted with runner sign -1 against prior +1: dropped from the wave |
| deal_target | add | +1 | admitted | 1 | keep | addition admitted with the prior sign |
| conn_rev | add | +1 | admitted | -1 | drop | addition admitted with runner sign -1 against prior +1: dropped from the wave |

**Cell** `scripts/specs/v8/lib-v8ysb-gm.json` (b-library, library v8ysb): gross match pm6-6: calibration L 1.1720 G 0.9772114158 vs G_parent 0.9862260459 -> corrected to L 1.1828, G 0.9862134133.

**Mechanics (S2, read before any return): PASS** (gross_all_rows 0.98621 [0.9, 1.05]; abs_net_all_rows 0.00534 <= 0.02; tau_mean 0.028134 <= 0.2; tau_p95 0.03341 <= 0.3; max_return_identity_error 3.6754e-16 <= 1e-09; max_cash_book_relative_error 1.1074e-13 <= 1e-09).

**Statistics of record** (S2): net Sharpe 1.8495 vs parent 1.7695: dSR +0.0800, Memmel SE 0.1502, CBB 95% [-0.23580753873511617, 0.4071793491746803], LW p 0.6262; bundle p one-sided 0.3122, two-sided 0.6262. DSR (N 57): ledger 0.8468; PBO 0.0766.

**Verdict (pm7-34: paired S2 net dSR > 0 AND mechanics (PM7-34); criteria printed, decide nothing): ACCEPTED** {'criteria': False, 'dsr_positive': True, 'mechanics': True}.
- criterion capacity-4x-higher (printed): met
- criterion turnover-per-gross-not-higher (printed): unmet
- criterion cost-bps-lower (printed): unmet

Returns (S2, annual): net 5.65% (CAGR 5.77%) vs 5.08%; gross of cost 7.07% vs 6.45%; vol 3.06%; max drawdown 2.58%; 4x net Sharpe 1.6994 vs 1.6549; tau 0.02813 (per unit gross 0.02853).

Ledger `build-equity/trials.jsonl`: lines 112 -> 128, head `e186895aeed3f7f3`, N 56 -> 57, cell trial `11c10defb3cf38a5`; admission lines appended 15.

| phase | run dir | s | peak MiB | outcome |
|---|---|---|---|---|
| u | `build-equity/mega-v8-b0b-train-u-v8ysb-run1` | 12.7 | 530 | completed |
| fit | `build-equity/mega-weights-v8x-theme-erc-v8ysb-run1` | 1.9 | 58 | completed |
| w | `build-equity/mega-v8xw-train-theme-erc-v8ysb-run2` | 52.8 | 1436 | completed |
| nav | `build-equity/mega-nav-v8x-theme-erc-L1.1720-v8ysb-run` | 59.7 | 586 | completed |
| card | `build-equity/mega-cards-v8x-theme-erc-v8ysb-run` | 24.5 | 1524 | completed |
| ref | `build-equity/mega-nav-v8x-theme-erc-L1.1720-v8ysb-ref-run` | 61.9 | 586 | completed |
| nav | `build-equity/mega-nav-v8x-theme-erc-L1.1828-v8ysb-run` | 55.6 | 586 | completed |
| monitor | `build-equity/mega-monitor-v8x-theme-erc-v8ysb-run` | 1.6 | 128 | completed |
| summ | `build-equity/cycle-v8ysb-gm/summ-run1` | 40.0 | 626 | completed |

Hidden-data record: seal scan of 79 log(s) (every run dir, reader and console of the wave; forms iso, compact, year, quarter): 0 date token(s) at or after 2024-01-01 (2026-10-02 x10 allowed: untracked owner plot file name docs/plans/2026-10-02-x5-equity-curve.png in dirty list (PM SEAL-ALLOW); 2026-10-03 x4 allowed: receipt started_utc wall-clock (PM SEAL-ALLOW); 20260927 x1 allowed: nav_summ's default bootstrap seed (not a date); 20260929 x66 allowed: nav_summ's --protocol v8 bootstrap seed and the sprint id platform-v8-20260929 (not a date)).
**Next parent: `scripts/specs/v8/lib-v8ysb-gm.json`, library v8ysb.**

## R0-7 Y-3 norm-score (P9 Phase 0): STOP before stage 01 -- no wave manifest (2026-10-03, 13:07Z)

Root `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, HEAD `ff0c5552`. Parent named by the Y-S driver:
`scripts/specs/v8/lib-v8ysb-gm.json` (library v8ysb, L 1.1828; `wave-result.json` `next_parent`). **Nothing ran; 0 trials;
ledger `build-equity/trials.jsonl` 128 lines (= R0-6's), N 57.**

| check (read-only) | argv / source | result |
|---|---|---|
| Y-3 wave manifest | `ls scripts/specs/v8/waves/`; `git log --all -- scripts/specs/v8/waves/` | **absent**: only `y-s.json`, `y-s.head.json` (commits `d7c1c520`, `4492f301`); no Y-3 manifest or head on any branch |
| registration | `v8y-prereg.md` sections 5, 6, 14; YP-7 | template `scripts/specs/v8/y-norm-score.json` @ `02633038`, file `693c64f5...`; through `wave run` as a `rule_cell`; PM6-6 gm template; PM7-34; 1 construction cell, 0 admission |
| template pin | `git show 02633038:` / `HEAD:` piped to sha256 | blob `693c64f5471ceff6...` = the registered pin (unchanged since `02633038`) |
| template on disk | `sha256sum`; `git ls-files --eol` | `73d16673f93e8ff0...`: `i/lf w/crlf` (`core.autocrlf=true`). The driver hashes disk bytes (`wave_context.Wave.sha`), so preflight with the registered pin would refuse ("not the pinned cell template"). All four `y-*.json` templates are `w/crlf`; `y-s.json` and `lib-v8ysb-gm.json` are `w/lf` |
| Y-3 defined on the parent | `lib-v8ysb-gm.json` `nav` | rule `aim-partial-v5`, L 1.1828; no `--hold-band`, `--vol-scale`, spo: **defined** |
| draft manifest (scratchpad, uncommitted) | `"C:/Program Files/Python312/python.exe" scripts/research_cycle.py wave plan <scratchpad>/y-3.draft.json --root C:/atx-wt/pool-2` | **exit 0**, 9 stages pending; cell file `scripts/specs/v8/y-norm-score-y-3.json`; draft sha256 `d4ece3a5bded754b...`; nothing written (no `build-equity/waves/y-3`, tree unchanged) |
| heads-up for R0-9 | `git log -- scripts/specs/v8/y-two-speed.json` | blob at HEAD `e29365d1` != registered `69cf6134` (@ `e2ac7d63`); changed by `b3b5dab4` "registered text follows the code" |

**Why stopped (brief stop rule "something needs a ruling"; nothing improvised).** The manifest's commit is the
pre-registration (preflight: "manifest committed"), and it holds choices no ruling fixes: the wave id, the printed
criteria list (Y-3's registered criterion, gross-of-cost return per unit gross, has no named rule; YP-7 says to write it by
hand), the budget id and caps, the record dir, and the template's EOL on disk vs its registered pin. The draft and the
choices are in `.superpowers/sdd/platform-p9-20261003/root-R0-7-report.md`.

### R0-7 resume: R0-7-EOL (P9 progress.md ruling) -- template LF bytes restored (13:2xZ)

Per template: `git diff --ignore-cr-at-eol HEAD -- <p>` 0 lines; disk bytes with CR stripped sha256 = HEAD blob (content
identical); then `rm <p>` + `git -c core.autocrlf=false checkout -- <p>`; `git ls-files --eol` now `i/lf w/lf`; `git status`
clean (only the untracked png). No hash edited.

| template | disk before (CRLF) | disk after (LF) | pin | result |
|---|---|---|---|---|
| `scripts/specs/v8/y-norm-score.json` | `73d16673f93e8ff0...` | `693c64f5471ceff6f21320c5bfee92d4f741226cbf7be295b1905e0a569e677f` | registered `693c64f5` (v8y 14) | **equal** |
| `scripts/specs/v8/y-theme-tsmom.json` | `020f0085a987c6a0...` | `9d3548b670547d138b3fdaeec0f170a2861b2a60d777c94788446e6c9b4cd83f` | registered `9d3548b6` | **equal** |
| `scripts/specs/v8/y-two-speed.json` | `93aab9c6bb126cee...` | `e29365d1597a31ba659996120ec2fc05f8d3947419249e4bbe945736533ca023` | HEAD blob `e29365d1` (R0-9-PIN; registered `69cf6134`) | **equal** to HEAD blob |
| `scripts/specs/v8/y-vol-target.json` | `d498abaded58de8e...` | `4e190f5434dfc13aced1459721cde476faf3560ef99aa41e05bfe492e2f141b4` | registered `4e190f54` | **equal** |

### R0-7 resume: R0-7-MAN (P9 progress.md ruling) -- Y-3 manifest committed

| step | argv | exit | result |
|---|---|---|---|
| manifest | the R0-6 report's validated draft, "DRAFT (uncommitted; for the PM's ruling) -- " prefix removed from `description`, 2-space JSON, LF | - | `scripts/specs/v8/waves/y-3.json` sha256 `a49e53874a3432edcf29e492f660816b76f6de16d65635b663a0252063a1dc10` (disk = blob, `i/lf w/lf`) |
| pre-registration commit | `git commit -m "wave y-3: manifest (pre-registration of cell Y-3; v8y-prereg sections 5, 6, 14)"` | 0 | **`c18a92b7`** |
| plan | `"C:/Program Files/Python312/python.exe" scripts/research_cycle.py wave plan scripts/specs/v8/waves/y-3.json --root C:/atx-wt/pool-2` | **0** | 27 lines; manifest sha256 `a49e5387...`; 9 stages pending; state `build-equity/waves/y-3/receipts`; cell file `scripts/specs/v8/y-norm-score-y-3.json` |

