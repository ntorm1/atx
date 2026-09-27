# Task T15 report: the IC runner's warm TRAIN pass fits the 180 s cap

Lane: `C:/atx-wt/pool-9`, branch `feat/mega-alpha-runner-perf-20260927`.
Base: `55319cbd`, plus T14 `f38e79ad` and T14 fix 1 `43cc33f3`, cherry-picked as the coordinator asked
(local `f57c75d0`, `6f173bb0`). My commits cherry-pick cleanly onto root `914fd6f9`: I checked with
`git merge-tree --merge-base=6f173bb0 914fd6f9 HEAD`, and root's `atx-impl/` is identical to my base.

Status: **DONE_WITH_CONCERNS**. Nothing was built or run here, as the lane contract requires, so every
number below is an estimate. The concerns are listed in the last section.

## Commits

| SHA | Subject |
|---|---|
| `4553de7c` | perf(ic-composition): rank candidate dates across the DetPool [T15] |
| `15f79758` | perf(ic-runner): cache per-candidate IC results beside the signal cache [T15] |

Files changed:

- `atx-impl/src/strategy_ic_composition.{hpp,cpp}`
- `atx-impl/src/strategy_ic_runner.{hpp,cpp}`
- `atx-impl/tests/strategy_ic_composition_test.cpp`
- `atx-impl/tests/strategy_ic_runner_test.cpp`

There are no new source or test files and no CMake changes, so nothing needs registering.

## Lever 1 (scoped /O2): already used up, so no change

Every translation unit (TU) on the warm path is already compiled `/O2 /Ob2 /clang:-finline` in the Debug
tree. Each uses the guard `MSVC AND Clang AND CMAKE_BUILD_TYPE STREQUAL "Debug"` and skips the PCH. I
confirmed the flags in `pool-2/build-equity/compile_commands.json`:

| TU | What it does on the warm path | Where /O2 is set |
|---|---|---|
| `atx-impl/src/strategy_ic_runner.cpp` | Cache-hit read loop and IC-series CSV | `atx-impl/CMakeLists.txt` (0f618a45 block) |
| `atx-impl/src/strategy_ic_composition.cpp` | Composition inner loop | Same block |
| `atx-engine/src/factory/ic_screen.cpp` | `evaluate_research_ic` rows, ranks and HAC; `hac.hpp` and `det_pool.hpp` are header-only and instantiated here | `atx-engine/CMakeLists.txt` |
| `atx-core/src/sha256.cpp` | Payload verification | 6d85ac2a |

**Assertions.** `/O2` does not define `NDEBUG`, and `-MDd`/`_DEBUG` stay on, so `assert` and the
`_ITERATOR_DEBUG_LEVEL=2` STL checks remain. Behaviour is unchanged.

**Floating point.** clang-cl `/O2` does not enable fast-math. With no `/arch` flag the target is baseline
x86-64 (SSE2), which has no FMA instructions, so the compiler cannot contract to FMA. The runner's VM
identity `dslvm1_clang18.1` has no `_fma`, `_avx2` or `_fastmath` suffix, which confirms this.

**VM sources.** No VM translation unit or VM source changed, so the VM tripwire hash `51bc0b2e…` does not
move.

**Why the three costs remain after /O2:**

- **IC (0.62–0.73 s):** it is dominated by the tied-rank sorts: about 750 dates × 3 horizons, each over
  3–5k names. The sort still runs the IDL-2 debug comparator double-check and span bounds checks. These
  cannot be switched off for one TU, because a mismatched `_ITERATOR_DEBUG_LEVEL` fails at link time.
- **Composition (0.32–0.38 s):** a serial sort for each of the 1155 dates.
- **Load (0.35–0.44 s):** reading 52 MB plus a portable SHA-256 at roughly 250–350 MB/s.

So the remaining time can only come from levers 2 and 3.

## Lever 2: IC-result cache (`15f79758`)

The cache turns on exactly when `--candidate-cache` is set; there is no new option.

**Location.** Each entry is one JSON file, `<the candidate's signal entry dir>/ic1_<first 16 hex of
SHA256(key)>/<id>.json`. It sits next to the signal entry, in the role-sha directory or the fields-sha
directory.

**Key.** The scope key is computed once per scored role and contains:

- `semantics_version` (1) and `ic_identity()`. The identity is `dslic1_clang18.1` plus the FP flavor and
  `_simd<ex::ic_screen_simd_width()>`, which is the SIMD width compiled into `ic_screen.cpp` and fixes its
  reduction order.
- The role manifest SHA.
- dates and instruments.
- Every `IcScreenConfig` field that can change a result bit: rule, horizons, execution_delay,
  window_begin/end, maturity_end, min_names, min_dates, and the IEEE bits of practical_abs_ic and
  confidence_multiplier.
- The research options `active_horizons` and `require_endpoint_presence`.
- The price field.
- The content SHA-256 of the exact `decision_member` and return-guard spans passed to
  `prepare_research_ic`. Hashing them (about 32 MB) costs roughly 0.12 s once per role. It also catches a
  future change to the runner's `guard_for`.

**Not keyed.** Two settings are left out because they only admit or schedule work:

- `max_cache_bytes`
- `workers`: IC rows are independent per date, HAC runs after the join, and `ResearchIc` pins serial and
  parallel runs as bit-identical.

**Record contents.**

- candidate_id, dsl_sha256
- `signal_payload_sha256`: the exact signal bytes. On a hit this is the pin that was verified; on a miss it
  is the payload just committed, and `cache_store` now returns it.
- the full key
- every field of `ResearchIcResult`: all 4 horizon slots with both estimates, all 4 coverage slots,
  reject, enough_evidence, reason and active_horizons
- the active horizons' daily Pearson and rank series

Every f64 is stored as its 16-hex IEEE bit pattern, including NaNs. Integers stay JSON integers, so
`dump()` round-trips exactly. The file is `{schema, record_sha256 = SHA256(record.dump()), record,
recorded: {engine_git_sha, ic_workers}}`. The `recorded` values are provenance only and sit outside the
hash.

**On read.**

- A missing file is a miss: the runner scores the candidate, then commits the entry.
- A present entry must parse and pass the self-hash, name exactly this candidate, DSL, signal SHA and key,
  and decode strictly: types checked, reason ≤ 3, series lengths equal to this role's scratch lengths.
- Anything else is a loud refusal and the file is left in place, the same policy as the signal cache. The
  messages are `candidate IC cache entry malformed|integrity|mismatch: <id>`.
- Entries are published with no-replace (a partial file plus a hard link). If an entry raced in first, it
  is accepted only when its record is identical.

**Semantics tripwire.** `ic_result_sources` lists 10 files: `ic_screen.cpp`, its atx/engine include
closure (`ic_screen.hpp`, `ic_research.hpp`, `ic_screen_config.hpp`, `eval/hac.hpp`, `alpha/panel.hpp`,
`alpha/fwd.hpp`, `parallel/det_pool.hpp`, `parallel/fwd.hpp`) and `alpha/panel.cpp`. They are pinned to
`e3e6f2d6…`. Test `IcSourcesPinnedToSemanticsVersion` shares one helper with the VM tripwire. I checked the
digest recipe by reproducing the existing VM pin `51bc0b2e…` in Python first. Root has not touched these
files since `55319cbd`.

**Recipe and outputs.** The cache is not a method input, so `method_recipe` is untouched. The output files
are byte-identical to a run without the cache.

**Other runner changes.**

- Per-candidate summary key `ic_result_cache` (hit or miss).
- Role key `candidate_cache.ic_results` with subdirectory, identity, key, hits and misses.
- The progress line now ends `... cache=hit ic_result=hit`.
- `series()` now takes spans, from scratch or from the cached record, with identical formatting.
- The prepare call passes `ic_price_field="close"` explicitly. That is the default, so nothing changes.

## Lever 3: pooled composition (`4553de7c`)

`IcComposition::add(k, signal, sign, DetPool* pool = nullptr)`. The runner passes its existing pool, which
is idle during composition. Nothing nests: VM, IC and composition run one after another on the main thread.

- **Work split.** Dates are divided into `min(dates, 4·workers)` contiguous bands, using the same
  quotient/remainder split as the research IC rows. Each band ranks into a per-worker row reserved to the
  instrument count, so the workers never allocate.
- **Why the bits match.** Each blend cell `d*n+i` and each `contribution_fraction[d]` has exactly one
  writer, using the serial expression. Candidates are still added in library order, so the accumulation
  order of every cell is unchanged. The result is bit-identical by construction, and the reasoning is
  written as a `SAFETY` comment in the code.
- **Admission.** Admission now counts 16 B/name/worker for these rows (`1024+64+16`). That is about 0.35 MB
  at 4 × 5627; the recipe is unaffected.

## Expected times (estimates; nothing measured here)

Per warm candidate:

| Stage | Measured before (runs 1–4) | Expected after | Reason |
|---|---|---|---|
| Signal load | 0.35–0.44 s | 0.35–0.44 s | Unchanged |
| IC | 0.62–0.73 s | about 0.01 s | One ≤ ~110 KB read, a SHA over the record and a ~4.5k-word decode |
| Composition | 0.32–0.38 s | 0.09–0.13 s | Sort-bound work over 16 bands and 4 workers, assumed 3–3.5× faster |
| Other (CSV and ledger) | about 0.05 s | about 0.05 s | Unchanged |
| **Total** | **1.34–1.61 s** | **about 0.50–0.63 s** | |

**Fully warm 121-candidate TRAIN pass:** 121 × 0.50–0.63 ≈ 61–76 s, plus about 9 s of fixed time (fields
verify ~2.2 s, role load 1.5 s, labels 2.1 s, scope hash 0.1 s, combined IC and save ~2 s). That gives
**about 70–85 s**, against a target of about 90 s.

**First pass after deploying (signals warm, IC entries cold):** about 1.27 s per candidate, so roughly
160 s. That should fit under 180 s. If the guard does stop it, every IC entry already committed persists,
so the next pass resumes from there. Cold-VM candidates still add `vm` plus the signal-cache write.

## Fixtures (postimplementation; not run here)

- `StrategyIcComposition.PooledAddMatchesSerialBitsForEveryWorkerCount`: 2, 3 and 4 workers against serial,
  compared bitwise on the blend, coverage and planned proxy. The data includes ties, NaN and inf, ±0,
  nonmembers, sign 0 and a pinned zero weight.
- `StrategyIcRunner.IcResultCacheHitsReproduceUncachedOutputsExactly`: a full window with
  `--save-combined`, and a short 10-date window whose estimates are NaN and whose series are empty. For
  both, a plain run with workers=2 matches the cold and warm cached runs on every output byte and on the
  stable summary. The serial run matches the pooled run on every non-JSON output: CSVs, `.f64` and `.u8`.
- `StrategyIcRunner.IcResultCacheRefusesTamperedOrForeignEntriesAndKeysSettings`, which checks:
  - integrity: one altered digit
  - mismatch: foreign signal SHA, and an edited key
  - malformed: wrong series length, and corrupt JSON
  - each refusal leaves the file in place
  - a restored entry is served again untouched
  - a deleted entry is rescored and republished byte-identically
  - `min_dates=16` gets its own directory, is a clean miss, and matches an uncached run
- `StrategyIcRunner.IcSourcesPinnedToSemanticsVersion`: new. `VmSourcesPinnedToSemanticsVersion` now uses
  the shared helper and behaves the same.
- Adjusted: `CandidateCacheColdAndWarm…` now counts 4 files plus exactly 1 IC subdirectory per role
  directory, and asserts IC hits and misses. `stable_role` also drops `ic_result_cache`.

I checked that `CandidateCacheIsScopedByVmIdentity…` is unaffected: its non-recursive
`std::filesystem::copy` skips the IC subdirectory, and I confirmed this in the MSVC STL `_Copy_impl`.

## For root: build, test and compare

**Build:**

```
powershell scripts\atx-build.ps1 build atx-equity-strategy-ic atx-impl-strategy-ic-tests
```

(preset equity-dev / `build-equity`).

**Test:** run the whole `atx-impl-strategy-ic-tests` executable, or filter with
`--gtest_filter=StrategyIcRunner.*:StrategyIcComposition.*:ResearchIc.*:IcScreen*` (66 tests).

**Bit-identity check.** Run TRAIN on library v1-48 twice into new output directories, with arguments
identical to `build-equity/mega-v1-train-r2/recipe.json`: workers 4, `--save-combined`, min-names 2000,
min-dates 128, `--max-memory-mib 1536`, the same pins and the same `--candidate-cache` DIR.

- The first run is IC-cold (log shows ` ic_result=miss`).
- The second is fully warm (log shows ` ic_result=hit`; `summary.roles[0].candidate_cache.ic_results.hits`
  is 48).

For both runs, the SHA-256 of each of these files must equal the one in `mega-v1-train-r2/`:

- `recipe.json`
- `orientations.json`
- `train_daily_ic.csv`
- `train_planned_targets.csv`
- `train_combined.f64`
- `train_combined_member.u8`
- `train_combined_finite.u8`
- `train_combined.json`

`summary.json` and `train_candidates.jsonl` differ by design: they contain timings and the new keys. v1
declares no extra fields, so T14's orientations record does not apply.

**Timing check.** Run v3-121 TRAIN twice. The first pass is IC-cold; the second should take about 90 s or
less, with per-candidate progress around `ic≈0.01 composition≈0.1`.

## Concerns and notes

1. The numbers are estimates. Composition speedup depends on P-core versus E-core placement. The load term
   (0.35–0.65 s) now dominates and has not changed.
2. A further lever I did not implement: the i7-1260P supports SHA-NI. A SHA-NI path in `sha256.cpp` (CPUID
   dispatch; digest identical by definition) would cut about 0.2–0.3 s per candidate (warm pass about
   45–55 s) and about 2 s off fields verification. It is atx-core crypto and outside the listed levers, so
   it would be better as its own lane with `atx-core-tests`.
3. The progress line gained ` ic_result=hit|miss` after `cache=...`. Any root log parser anchored at
   end-of-line on `cache=(\w+)$` needs updating.
4. Admitted working bytes rise by 16 B/name/worker when workers > 1. `max_working_bytes`, the recipe and the
   outputs are unchanged.
5. `atx-impl-tests` also globs these two test files. Nothing else there changed.
