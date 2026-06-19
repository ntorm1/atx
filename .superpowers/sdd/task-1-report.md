# Task 1 Report: Untangle the novelty knobs

## What was implemented

Removed the dead, unsound structural-novelty penalty (`novelty_penalize` / `canonical_distance`) and replaced the overloaded `novelty_w` float with a clean boolean `enable_behavioral_novelty`.

### Changes by file

**`search_driver.hpp`**
- Removed `#include <bit>` (was only used by `canonical_distance`)
- Removed `atx::f64 novelty_w{0.1}` from `SearchConfig`
- Added `bool enable_behavioral_novelty{true}` with the new doc block
- Deleted `detail::canonical_distance()` function and doc block
- Deleted `novelty_penalize()` declaration and doc block
- Updated comments on `behavioral_novelty_pass`, `behavioral_active`, and `Scored::selection` to remove all `novelty_w` references

**`search_driver.cpp`**
- Deleted the `novelty_penalize()` definition (~35 lines)
- Deleted the call `novelty_penalize(scored, pool, cfg)` from the generation loop
- Rewrote `behavioral_active()`: `return cfg.objective_mode == ObjectiveMode::MultiObjective && cfg.enable_behavioral_novelty;`
- Updated adjacent comment on the behavioral novelty pass call

**`factory_nsga_search_test.cpp`** (Steps 7 + 8)
- Replaced `pin_config()` with `legacy_pin_cfg(seed)` per the brief's exact definition
- All 5 call sites updated to `legacy_pin_cfg(777)`
- `legacy_pin_cfg` pins `enable_behavioral_novelty = false` as Task 1's legacy value

**`factory_behavior_test.cpp`** (Step 7)
- `novelty_w = 0.1` → `enable_behavioral_novelty = true`
- `novelty_w = 0.0` → `enable_behavioral_novelty = false`
- Added `#include <bit>` (for inline `std::popcount` in the headline test)
- Updated `BehavioralRanksOppositeToCanonicalHash` to inline the Hamming distance computation (removed call to deleted `fd::canonical_distance`); added explanatory note
- Updated comments to remove `novelty_w` references

**`factory_search_driver_test.cpp`** (Step 7)
- Deleted `cfg.novelty_w = 0.1;` from `small_search_cfg`

**`search_progress_test.cpp`** (not in brief Step 7 but in same test target)
- Two `cfg.novelty_w = 0.0` → `cfg.enable_behavioral_novelty = false`
- Updated comments

**`factory_cost_aware_fitness_test.cpp`, `factory_integration_test.cpp`, `factory_mine_into_test.cpp`, `factory_oos_test.cpp`, `factory_research_driver_test.cpp`**
- All `cfg.search.novelty_w = 0.1` → `cfg.search.enable_behavioral_novelty = true`
- `cfg.search.novelty_w = 0.0` → `cfg.search.enable_behavioral_novelty = false`

**`search_quality_test.cpp`** (new file — Step 2)
- `SearchNoveltyKnob.BehavioralNoveltyTogglesDigest`: verifies that MultiObjective runs with `enable_behavioral_novelty=true` vs `false` produce different digests

## TDD evidence

### RED (Step 3 — compile failure before implementation)
The new test file was added before any header changes. First build:
```
error: no member named 'enable_behavioral_novelty' in 'atx::engine::factory::SearchConfig'
```
Expected failure: `SearchConfig` did not yet have the new field.

Additional intermediate failure:
```
error: no member named 'canonical_distance' in namespace 'atx::engine::factory::detail'
```
(factory_behavior_test.cpp still called the deleted function — fixed by inlining `std::popcount`)

```
error: no member named 'loop' in namespace 'atx::engine'
```
(search_quality_test.cpp had wrong namespace for WeightPolicy — fixed to `atx::engine::WeightPolicy`)

### GREEN (Step 9 — final run)
```
powershell -ExecutionPolicy Bypass -File build/wt-build.ps1 -Target atx-engine-factory-tests -TestRegex 'NsgaSearch|FactorySearchDriver|FactoryBehavior|SearchNoveltyKnob'
```
Output: 12/12 tests passed, WT-BUILD OK

Full behavioral suite also confirmed:
```
BehavioralHeadline|BehavioralSearch|BehavioralArchive: 9/9 passed
```

Key assertions confirmed green:
- `NsgaSearch.ScalarRaw_ReproducesGoldenDigest` → digest `0xa83f0d3e0b41a18dULL` UNCHANGED
- `FactorySearchDriver.SameSeedReplaysByteIdentical` → PASSED
- `FactorySearchDriver.DriverIsWorkerCountInvariant` → PASSED
- `SearchNoveltyKnob.BehavioralNoveltyTogglesDigest` → PASSED (new test)

## Self-review findings

1. **`legacy_pin_cfg` omits `seed_from_grammar = false`**: The original `pin_config()` explicitly set `seed_from_grammar = false`. The brief's `legacy_pin_cfg` template does NOT include this line (it's annotated as a Task 3 addition). Since `seed_from_grammar` defaults to `false`, the omission is behaviorally correct — the golden digest is unchanged. Confirmed green.

2. **`factory_behavior_test.cpp` headline test**: The test originally called `fd::canonical_distance` to document why the behavioral metric is better. After deletion, I inlined the identical Hamming popcount computation with an explanatory comment. The test still proves the same invariant.

3. **Extra files beyond the brief's Step 7 list**: The brief listed only 3 test files to migrate, but 6 more factory test files also referenced `novelty_w`. All were in the `atx-engine-factory-tests` compilation unit and would have caused build failures. All were fixed with mechanical `novelty_w = 0.1 → enable_behavioral_novelty = true` / `novelty_w = 0.0 → enable_behavioral_novelty = false` substitutions.

4. **`.selection` field retained**: As specified, `Scored::selection` is kept but now always equals `.fitness` (the `novelty_penalize` call that wrote `fitness - penalty` into it is removed; the `Scored` constructor still passes `cs.raw` as the third arg). The ScalarRaw tournament still reads `.selection` — it now equals `.fitness` exactly as `novelty_w==0` did before.

## Concerns

None. The golden digest is byte-identical. All determinism contracts hold.

---

## Fix pass (review findings)

### Files changed

Code files (compile-verified):
- `atx-engine/tests/book/book_pipeline_test.cpp` — `novelty_w = 0.1` → `enable_behavioral_novelty = true`
- `atx-engine/tests/core/research_engine_test.cpp` — same
- `atx-engine/tests/data/data_boundary_pin_test.cpp` — same
- `atx-engine/tests/data/data_e2e_byo_capstone_test.cpp` — same
- `atx-engine/tests/data/orats_e2e_smoke_test.cpp` — two occurrences: `novelty_w = 0.0` → `enable_behavioral_novelty = false`
- `atx-engine/tests/eval/eval_synthetic_alpha_test.cpp` — `novelty_w = 0.1` → `enable_behavioral_novelty = true`
- `atx-engine/tests/parallel/parallel_five_path_capstone_test.cpp` — same
- `atx-engine/tests/parallel/parallel_workload_mine_process_test.cpp` — same
- `atx-engine/tests/risk/robust_pipeline_e2e_test.cpp` — four occurrences: two `novelty_w = 0.1` → `enable_behavioral_novelty = true`, two `novelty_w = 0.0` → `enable_behavioral_novelty = false`
- `atx-engine/tests/factory/search_quality_test.cpp` — removed unused `#include "atx/engine/alpha/parser.hpp"`
- `atx-engine/include/atx/engine/factory/robust_pipeline.hpp` — doc comment line 17: `novelty_w` → `enable_behavioral_novelty`
- `atx-impl/src/stage_discover.cpp` — `sc.novelty_w = 0.1` → `sc.enable_behavioral_novelty = true`
- `python/src/_bindings/shim/mining_shim.hpp` — field `novelty_w` → `enable_behavioral_novelty` (bool)
- `python/src/_bindings/shim/mining_shim.cpp` — shim assignment updated
- `python/src/_bindings/bind_factory.cpp` — pybind11 readwrite binding updated
- `python/src/atxpy/mining.py` — function param `novelty_w=0.1` → `enable_behavioral_novelty=True`, body updated

Bench files (not compile-verified — bench disabled in this config, edits are mechanical):
- `atx-engine/bench/factory_bench.cpp` — `novelty_w = 0.1` → `enable_behavioral_novelty = true`
- `atx-engine/bench/research_bench.cpp` — same
- `atx-engine/bench/robust_search_bench.cpp` — ternary form migrated to `enable_behavioral_novelty = (mode == ObjectiveMode::MultiObjective)`, plus one direct `novelty_w = 0.1` → `enable_behavioral_novelty = true`

### Final `git grep novelty_w` result

Zero hits in code. Remaining hits are exclusively in archival plan/design docs (`atx-engine/plans/`, `docs/superpowers/plans/`) and one accurate comment in `search_quality_test.cpp` line 56 ("NOT novelty_w") explaining the migration — all correct to leave as-is.

### Build result

Command: `powershell -ExecutionPolicy Bypass -File build/wt-build.ps1 -Target all -NoTest`
Result: **WT-BUILD OK** — 271/271 targets compiled, all test executables linked.

### Test result

Command: `powershell -ExecutionPolicy Bypass -File build/wt-build.ps1 -NoBuild -TestRegex 'NsgaSearch|FactorySearchDriver|FactoryBehavior|SearchNoveltyKnob'`
Result: 12/12 passed (NsgaSearch ×5, FactorySearchDriver ×6, SearchNoveltyKnob ×1).
`NsgaSearch.ScalarRaw_ReproducesGoldenDigest` → `0xa83f0d3e0b41a18d` UNCHANGED (byte-identical).

Additional behavioral suite (Behavioral*/BehavioralSearch/BehavioralHeadline): 16/16 passed.

### Bench note

Bench targets are not compiled in the `all` build (bench is disabled in this config). Bench files (`factory_bench.cpp`, `research_bench.cpp`, `robust_search_bench.cpp`) were edited with identical field/pattern substitutions as the verified test edits; the changes are correct but not compile-verified here.
