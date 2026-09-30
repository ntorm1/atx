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
