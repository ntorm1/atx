# Task B-3 report: move-only split of the IC runner, K1 plan rows

Lane B, worktree `C:/atx-wt/pool-10`, branch `feat/platform-v8-b-20260929`, base `ef11f462`.
Nothing was built or run (lane rules). Three commits:

| commit | content |
|---|---|
| `f5f8754e` | move-only split + CMake lists + `dsl_vm_sources` pin repair |
| `0e95a072` | K1 `candidates[]` plan rows + test + this report |
| `bdca4c3d` | build fix to the split: `cache_schema_v2` (used by `score_role`) moved to the header |

`f5f8754e` alone does not compile (`score_role` names `cache_schema_v2`, left in the signal cache TU's anonymous
namespace); `bdca4c3d` fixes it. A script then checked every name the split left in an anonymous namespace
against every other TU (none other is used across TUs) and that each header-declared function is defined exactly
once. **Identity runs for the move-only state use `bdca4c3d`** (or any later lane-B commit with the flags off).

## What was built

### Commit 1: move-only split

`atx-impl/src/strategy_ic_runner.cpp` (2,568 lines) became five TUs plus one internal header. Every function body
was copied from the HEAD blob by line number with a script (`split_runner.py`, scratch); the script asserts that
every original line lands exactly once except the nine listed below, and a sorted-line diff of old vs new shows
only the edits listed.

| file | lines | holds (original lines) |
|---|---|---|
| `strategy_ic_detail.hpp` | 278 | shared constants (44-59 subset, 155), build flavor (108-134), all shared structs (178-205, 328-331, 791-801, 1019-1035, 1286-1310, 1398-1403, 1558-1563, 1612-1647, 1748-1751, 1776-1777), declarations of every cross-TU function |
| `strategy_ic_library.cpp` | 318 | pinned-input helpers (210-246), `field_plan`, `library` (507-609), field residency runtime (1527-1611, 1648-1702) |
| `strategy_ic_admission.cpp` | 570 | `method_recipe`, fields recipe, frozen TRAIN (247-413), `admit` (206-209, 610-658), fields binding (659-775), composition weights and frozen bindings (776-986) |
| `strategy_ic_signal_cache.cpp` | 612 | VM identity + `dsl_vm_sources` pin (60-107, 135-146), candidate signal cache and `--cache-report` (987-1526), `ic_cache_vm_identity()` (2333-2337) |
| `strategy_ic_result_cache.cpp` | 297 | IC identity + `ic_result_sources` pin (147-177), IC-result cache (1752-1989), `ic_result_cache_identity()` (2338-2342) |
| `strategy_ic_runner.cpp` | 747 | output writers (414-506), `guard_for`, result JSON (1703-1747), `series`, `candidate_signal`, `score_role` (1990-2331), `run_ic`, `dispatch_ic` (2343-2568) |

Linkage: helpers used by one TU stay in an anonymous namespace inside `atx::impl::strategy::ic_detail` in that TU
(original order kept, anonymous blocks opened and closed around them). Helpers used across TUs are declared in
`strategy_ic_detail.hpp` and defined once, with external linkage, in `ic_detail`. The runner-local functions stay
in `atx::impl::strategy::(anonymous)` with `using namespace ic_detail;`.

- Namespace is `ic_detail`, not `detail`: `atx::impl::strategy::detail` is already used by the NAV and target
  replay headers, and a second `detail::Role`/`detail::hex` there would be an ODR hazard.
- The only textual edits inside moved code: default arguments moved to the header declarations (`method_recipe`,
  `admit`, `load_pinned_f64`, `write_partial`); header constants became `inline constexpr`; `PartialFile`'s
  constructor and destructor bodies moved out of line (verbatim) into strategy_ic_signal_cache.cpp; two comments
  that said "this TU" now name the IC runner TUs. Not copied: lines 3 and 31 (includes now in the TUs that need
  them / the header), 36 (blank), 39-43 (namespace aliases, now in the header), 52 (a comment, reworded per file).
- `vm_compiler` / `vm_fp_flavor` are `inline constexpr` in the header. They depend on compiler version and
  `__FMA__`/`__AVX2__`/`__FAST_MATH__`; all five TUs are on one flag set (same CMake lists), so they have one
  definition. `vm_identity()` and `ic_identity()` return the same strings as before.
- ODR checks: no two TUs define the same `ic_detail` symbol; every anonymous-namespace function and constant is
  used in its own TU (no `-Wunused-function` / `-Wunused-const-variable` under `/W4 /WX`); no anonymous
  namespace in the header; every cross-TU declaration was checked against its definition signature.
- `atx-impl/CMakeLists.txt`: the four new sources are in `atx-impl-core`, in the Debug `/O2 /Ob2 -finline`
  list and in the `SKIP_PRECOMPILE_HEADERS` list, so every IC TU keeps the flags the single TU had.

### Source pins (`dsl_vm_sources`, `ic_result_sources`)

- The pins list only atx-engine files (the runner's engine include closure); no atx-impl file is hashed. The split
  adds no engine include (the union of the five TUs' `atx/engine` includes is the old TU's), so the lists need no
  path for the split itself.
- **`dsl_vm_sources` was red on the base.** W2 (`d3221a30`, 2026-09-28) made `vm.hpp` include
  `alpha/lit_ops.hpp` and edited vm.hpp, registry, typecheck, parser, dag and bytecode without listing lit_ops.hpp
  or re-setting the digest, so `StrategyIcRunner.VmSourcesPinnedToSemanticsVersion` fails on `ef11f462` (digest and
  closure). Commit 1 lists `lit_ops.hpp` (30 paths; its own engine includes, cs_ops.hpp and registry.hpp, are
  listed) and re-sets the digest to `fa1e9d0f...0aab`. No semantics bump: the v7 review ruled that the W2 ops leave
  existing bits unchanged (`dsl_vm_semantics_version` stays 1, only the pin is reset), and the W2 golden test pins
  every pre-W2 opcode digest.
- `ic_result_sources` is unchanged (digest `e3e6f2d6...16f7` recomputed equal).
- Both digests were recomputed with a Python mirror of `expect_sources_pinned` (same material, CRLF->LF); the
  mirror reproduces the committed `ic_result_sources` digest exactly, which validates it.

**Cache keys do not change.** Neither key hashes source bytes: the signal key is `vm_identity()` (`dslvm1_` +
compiler + FP flavor) + eval mode + layout + role + DSL + field payloads; the IC key is `ic_identity()` + scope.
All are unchanged, so a warm cache stays warm (root may still run cold; see below).

### Commit 2: contract K1

`--plan-only` JSON:
- `candidates`: array, one row per library candidate in library order:
  `{id, dsl_sha256, num_slots, required_lookback, extra_fields[], node_count}`, built by
  `ic_detail::candidate_plan_rows(const Library&)` (strategy_ic_library.cpp) from the compiled program the VM runs.
  `num_slots` = `Program::num_slots`; `required_lookback` = `Program::required_lookback`; `extra_fields` = the
  sorted non-base fields it reads; `node_count` = `Program::unique_nodes` (DAG nodes after CSE, what the
  generators call `dag_nodes`).
- The count moved to `candidate_count` (the old `candidates` number). Only two tests read the old key; both updated.
  No script parses the plan-only JSON (checked scripts/, atx-impl/tools, atx-impl/strategies, atx-engine/tools).

## How root verifies

Build (target-scoped): `atx-impl-strategy-ic-tests atx-equity-strategy-ic`.

gtest: `atx-impl-strategy-ic-tests --gtest_filter=StrategyIcRunner.*:StrategyIcComposition.*:IcScreen.*:ResearchIc.*`
(StrategyIcRunner: 47 = the 46 existing + `PlanOnlyPrintsCandidateRows`). `VmSourcesPinnedToSemanticsVersion`
turns green with commit 1. If another lane edits a pinned engine file before merge, re-pin at merge.

Identity (commit 1 is the move-only commit; run both from its build):
1. Warm: the v7.1 u pass exactly as receipted in `build-equity/mega-v71-train-u-run1/receipt.json` with a new
   `--output`:
   `atx-equity-strategy-ic --library atx-impl/strategies/fund_industry_ic_v71.json --library-sha256 787c802e...2259
   --train build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json --train-sha256 3e79978a...b809
   --train-fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9 --train-fields-sha256 8fd00e9f...769b
   --output build-equity/b3-v71-train-u-warm --max-memory-mib 1536 --min-names 1000 --workers 4 --save-combined
   --cache-legacy-fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v7
   --candidate-cache build-equity/mega-candidate-cache-v71`.
   Expect `candidate_cache.hits` 48 and `ic_results.hits` 48 (keys unchanged).
2. Cold: the same with `--candidate-cache build-equity/b3-cold-cache` (new, empty) and
   `--output build-equity/b3-v71-train-u-cold`: every VM and IC path runs through the split code.
3. For both: `recipe.json`, `orientations.json`, `train_daily_ic.csv`, `train_planned_targets.csv`,
   `train_combined.{f64,json}`, `train_combined_member.u8`, `train_combined_finite.u8` SHA-equal to
   `mega-v71-train-u-1`; the cold run's cache payloads SHA-equal to the warm cache's entries.
4. e2e fixture (E-3) SHAs unchanged.

## Deviations

- Five TUs as briefed, but two seams the brief did not place were assigned: composition weights and the frozen
  TRAIN recipe went to strategy_ic_admission.cpp (both are pre-payload validation of pinned inputs); field residency
  went to strategy_ic_library.cpp (it is the runtime side of the field plan, and B-2 edits both).
- The dsl_vm pin repair (lit_ops.hpp) is not a split artefact; the brief's "update the pins in the same commit"
  is where it belongs, and the test was red without it.
- K1 renames the plan's old `candidates` count to `candidate_count` because K1 fixes `candidates` as the array.

## Cross-lane edits

- `atx-impl/CMakeLists.txt` (lane E's file per the plan; named in this brief): source list and the two `/O2` lists.

## Open risks

- Not compiled. Highest-risk items to look at first if the build fails: `using namespace ic_detail;` lookups in
  strategy_ic_runner.cpp, the `inline constexpr` flavor block in the header, the out-of-line `PartialFile`.
- `atx-engine/include/atx/engine/alpha/vm.hpp:13` still says `dsl_vm_semantics_version` is in
  strategy_ic_runner.cpp (now strategy_ic_signal_cache.cpp). Left alone: editing vm.hpp re-pins and recompiles
  every VM consumer. Python comments in fit_composition_weights.py name strategy_ic_runner.cpp functions too.
- The pinned digest covers the worktree's engine files at `ef11f462`; any engine edit merged first changes it.
