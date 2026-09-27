# Task T1 report — IC runner candidate signal cache and pinned composition weights

Status: **DONE_WITH_CONCERNS** (implemented and fixtures written; not compiled or run, per brief).
Worktree `C:/atx-wt/pool-4`, branch `feat/mega-alpha-runner-cache-20260926`, base `5c9cbaed`.

## Commits

- `09a18ec3` feat(ic-runner): candidate signal cache and pinned composition weights
  (`strategy_ic_runner.{hpp,cpp}`, `strategy_ic_composition.{hpp,cpp}`)
- `a7fd1c02` test(ic-runner): cache bit-identity, tamper refusal and pinned weights
  (`atx-impl/tests/strategy_ic_runner_test.cpp`; **no new TU, no CMake change**)

Build target for root: `atx-impl-strategy-ic-tests` (it already lists `strategy_ic_runner_test.cpp`),
then `-Ctest -R StrategyIcRunner`. `strategy_target_replay_test.cpp` and
`strategy_ic_composition_test.cpp` call `IcComposition::create` with 3 args; they still compile
because the new 4th parameter is defaulted.

## What changed (line numbers at `a7fd1c02`)

### `atx-impl/src/strategy_ic_runner.hpp`
- `:18-22` new config fields only: `candidate_cache_directory`, `composition_weights_path`,
  `composition_weights_sha256`.

### `atx-impl/src/strategy_ic_composition.{hpp,cpp}`
- hpp `:39-53`: `create(..., std::span<const f64> pinned_weights = {})`, contract documented.
- cpp `:82-84`: pinned weights must be library-sized, finite, `>= 0`.
- cpp `:106`: pinned values are stored **verbatim** in the same `weights[]` vector that the
  unchanged `add()` expression reads (`signal += sign * weights[k] * r`,
  `contribution += weights[k] * n`). No normalization, no reordering, so pinning the default
  values reproduces the default blend bit for bit.
- cpp `:136-138`: `add()` skips the per-date loop when `weights[k] == 0`. Default weights are
  always `> 0`, so the default path is unchanged. For pinned zeros, skipping is bit-identical to
  adding `±0`: the accumulator starts at `+0`, can never become `-0`, and `x + ±0 == x` for
  every other value.

### `atx-impl/src/strategy_ic_runner.cpp`
- `:43-48` constants. `vm_eval_mode` is the existing recipe `"vm"` string, reused at `:87`, so
  recipe bytes are the same.
- `:71-81` `pinned_json` is split into `pinned_text` + parse (behaviour unchanged).
- `:97-104` `method_recipe`: only when weights are pinned, `composition` becomes
  `pinned-candidate-weights;TRAIN-orientation-signs;...` and `composition_weights_sha256` is
  added. When weights are absent the recipe is byte-identical to before. The cache is never a
  recipe input.
- `:127-137` `frozen_train`: takes the source TRAIN recipe's weights pin (validated as a hash)
  into `source_cfg` before the exact `recipe != expected` check. A weighted TRAIN source
  therefore resumes exactly, and validation-only runs may use their own weights (signs do not
  depend on weights).
- `:242-259` saved-blend manifest: with pinned weights, `signal_semantics` becomes
  `exact-pre-target-composition;pinned-candidate-weights;missing-or-unoriented-neutral-fixed-denominator`
  and a `composition_weights_sha256` key is added. When weights are absent the key is missing
  (not null), so the default manifest bytes are unchanged.
- `:334-339` admission comment: the single reused candidate buffer is the existing
  "returned signal 8B/cell" term. No numeric budget change, so `required_bytes` and
  `admitted_working_bytes` are unchanged with or without the cache.
- `:350-364` `unique_key_json`: nlohmann callback parse that refuses duplicate object keys
  (otherwise the last one silently wins). It uses no-throw parse, so malformed text and number
  overflow (`1e999`) become a clear "composition weights JSON parse" error.
- `:367-393` `composition_weights`: pinned text (sha must match) → schema
  `atx.dsl-composition-weights/v1`, `library_sha256 == --library-sha256`, a `weights` object
  with every library id exactly once, no unknown id, and every value numeric, finite and
  `>= 0`. Returned in library order. Extra top-level keys are allowed (they are hashed by the
  pin).
- `:394-589` cache helpers:
  - `CacheKey`, `hex`.
  - `device_name` (`:408`): refuses ids that are Windows device basenames (con/prn/aux/nul/comN/lptN).
  - `cache_preflight`: little-endian host; the path must be a directory if it exists.
  - `cached_payload_sha` (`:426`): sidecar identity check. schema, candidate_id, `dsl_sha256`,
    `role_manifest_sha256`, eval_mode, layout, dates, instruments and bytes must all match, and
    the payload sha must be valid. Otherwise: "candidate cache entry mismatch: <id>".
  - `cache_lookup`: absent → `nullopt`, present but inconsistent → error.
  - `cache_load` (`:456`): extent check, then reads straight into the reused buffer in 1 MiB
    slices, hashing each slice as it lands (no second copy), then compares with the sidecar sha.
  - `PartialFile`: unique `.<name>.<16hex>.partial`, removed by RAII.
  - `publish_new`: `create_hard_link` = atomic no-replace publish, the same idiom as
    replay_report / stage_equity_*.
  - `write_partial`: streaming write plus sha.
  - `cache_store_payload` (`:522`): never overwrites. An existing payload without a sidecar
    (run stopped between the two commits, or a concurrent writer) is adopted only if its sha
    equals ours. Otherwise: "…refusing overwrite: <id>.f64".
  - `cache_store` (`:545`): canonicalizes non-finite cells to quiet NaN **in place** (so the run
    consumes exactly the bytes it stores), publishes the payload, then the sidecar as the commit
    record. A sidecar race is accepted only for the same identity and sha.
  - `cache_plan`: plan-only readiness (identity-checked sidecars plus stat'ed payload extents,
    no payload reads).
  - `release`: frees a vector.
- `:649-702` `candidate_signal`: cache hit → load and log
  `IC cache-hit <id> role=<role> seconds=…`. Miss → log `IC cache-miss`, **release the previous
  buffer before any VM growth or evaluation**, then run the unchanged VM (re)creation block
  (050c0efc undersized-arena release is preserved), evaluate, and move the VM output into the
  buffer. With the cache on, `cache_store` follows and logs `IC cache-write`. With the cache off
  it is the old path: same VM calls, and the same values reach IC/composition.
- `:703-865` `score_role`:
  - takes `weights` and passes it to `IcComposition::create` (`:738`).
  - creates `DIR/<role sha>` (`:739-745`).
  - consumes `std::span<const f64> signal` from the buffer.
  - with the cache on only: per-candidate `stage_seconds.cache_load/cache_write` and
    `signal_cache: hit|miss`; role `stage_seconds.cache_load/cache_write`; and role
    `candidate_cache {directory,hits,misses,vm_evaluations}`. `candidate_evaluations` still
    counts IC evaluations.
  - the buffer is released with the VM after the loop (`:817`).
- `:866-968` `run_ic`:
  - weights must come in a pair (`bounded config`).
  - `composition_weights` and `cache_preflight` run right after `library()`, before any
    admission or payload, for both real and `--plan-only` runs.
  - plan output adds `composition_weights_sha256` and `candidate_cache[]` only when the options
    are given.
  - summary adds `composition_weights_sha256` only when pinned.
- `:971-1004` CLI `--candidate-cache DIR`, `--composition-weights PATH`,
  `--composition-weights-sha256 SHA`, plus help text.

### Sidecar `DIR/<role-manifest-sha256>/<id>.json` (`atx.dsl-candidate-signal/v1`)
Fields: `candidate_id`, `family`, `dsl_sha256`, `library_sha256`, `role_manifest_sha256`, `role`,
`source_sha256`, `dates`, `instruments`, `bytes`, `layout`
(`date-major-little-endian-f64;non-finite-stored-as-quiet-NaN`), `payload`, `payload_sha256`,
`semantics`, `eval_mode` (`ResearchFast;full-historical-asof-member-mask`), `vm_workers`,
`engine_git_sha`.

## Fixtures added (`strategy_ic_runner_test.cpp`)
- `:449` CandidateCacheColdAndWarmRunsReproduceUncachedOutputsExactly (a):
  - no-cache vs cold vs warm: equal recipe sha, orientations artifact sha, full role summaries
    (timings/cache bookkeeping stripped), and byte-identical recipe.json, orientations.json,
    daily IC, planned targets, combined `.f64`/member/finite and the combined manifest `.json`.
  - hit/miss counts, and the `IC cache-hit <id> role=<r>` lines.
  - the warm log has no `IC VM-` line.
  - sidecar identity, geometry, dsl sha and payload sha; payload non-finite values are exactly
    the quiet NaN bit pattern; no leftover partials.
- `:512` CandidateCacheResumesStoppedRunAndAdoptsOnlyIdenticalOrphans:
  - the "K cached" resume: one TRAIN entry missing, plus a validation payload without its
    sidecar.
  - plan-only reports `ready_entries = 1` per role.
  - the resumed run hits and misses as expected and adopts the orphan without rewriting it
    (sha and mtime unchanged).
  - outputs are byte-identical to the cold run.
- `:545` CandidateCacheRefusesTamperedOrForeignEntriesWithoutOverwrite (b): each case below
  refuses loudly, leaves the summary as `failed`, and never rewrites the file:
  - a flipped payload bit (SHA mismatch)
  - a truncated payload (extent)
  - a sidecar with a different dsl sha (entry mismatch)
  - a differing uncommitted orphan (refusing overwrite, and no sidecar is written)
- `:592` PinnedDefaultCompositionWeightsReproduceDefaultBlendBytes (c):
  - the library has 2 families (3 + 1 candidates), so the default weights are 1/6 (inexact in
    binary) and 1/2.
  - pinned vs default runs: identical combined `.f64`/masks, planned targets, daily IC,
    orientations candidates and role summaries.
  - the recipe differs only by the composition string plus the pin key, so the recipe sha
    differs.
  - the default recipe and manifest carry no pin key and keep the original strings.
- `:638` UnequalPinnedWeightsChangeBlendAndRecipeWhileZeroWeightStaysScored (d): weights 2/0.
  - the blend equals exactly `2*r` (scalar oracle).
  - the blend and planned targets differ from the default; the recipe sha differs.
  - the zero-weight candidate's IC diagnostics are unchanged.
- `:665` InvalidCompositionWeightsRefuseBeforeAnyPayloadOrOutput (e): role `close.f64` payloads
  are deleted first.
  - accepted in plan-only mode: a valid pin.
  - refused in both plan-only and real modes: unknown id, missing id, negative, string,
    `1e999`, duplicate key, wrong library sha, wrong schema, weights array.
  - also refused: hash mismatch and an unpaired option.
  - every refusal leaves no output dir and no progress text.
- `:709` ValidationOnlyResumeComposesCandidateCacheAndPinnedWeights: validation-only with
  `--orientations` + cache + pinned weights, run cold then warm, plus the reverse (a weighted
  TRAIN source resumed without weights).
  - the validation blend, planned targets and daily IC are byte-identical to the uninterrupted
    source.
  - TRAIN is never loaded or cached.
  - `frozen_train_recipe` and the pin are recorded.

## Concerns / uncertain
1. **Saved-blend replay contract (cross-lane):** at base, `strategy_target_replay.cpp`
   `admit_saved` requires `signal_semantics` to equal exactly the equal-family string. Blends
   saved with `--composition-weights` carry the honest `pinned-candidate-weights` variant, so
   target replay (and probably the new nav replay) **will refuse pinned-weight blends** until
   the replay owners accept that string. They could also require `composition_weights_sha256`
   when it is present. Default blends are unaffected. I did not touch replay files, per
   instructions.
2. **"Engine source identity":** the runner did not record an engine identity before this task;
   it only had the role `source_sha256`. The sidecar records both `source_sha256` and
   `engine_git_sha` (`build_engine_git_sha()`, from atx-impl-core's own provenance TU) plus
   `vm_workers`. Following the brief's match list, only dsl sha, role manifest sha, geometry
   and payload sha (plus constant schema/eval_mode/layout/id) are **matched**. Engine sha and
   library sha are recorded only, so a grown library reuses entries. As a consequence, **a
   VM/kernel semantics change must use a fresh cache DIR**; the cache does not detect it. This
   is stated in a code comment.
3. The cold path consumes the NaN-canonicalized buffer while the no-cache path consumes raw VM
   output. Equality holds because `evaluate_row` (ic_screen.cpp:248) and composition `add()`
   admit a cell only via `std::isfinite`. If a future IC/composition path distinguishes Inf
   from NaN, the no-cache == cold identity would need revisiting (cold == warm stays exact by
   construction).
4. Not compiled. Likely friction points:
   - nlohmann callback `Json::parse(text, lambda, false)` with the lambda signature
     `(int, Json::parse_event_t, Json&)`.
   - `ATX_TRY(const auto weights, ...)`.
   - `co::Result<std::optional<std::string>>` / `co::Result<bool>`.
5. Publication requires hard-link support (NTFS/ReFS, as elsewhere in atx-impl). A killed write
   can leave inert `.<id>.f64.<16hex>.partial` files; no lookup reads them.
6. Windows path length: each entry path carries a 64-hex role dir. The test uses cache dir `c`
   to stay well under MAX_PATH inside the per-process scratch root.
7. Policy choices not spelled out in the brief:
   - all-zero weight files are allowed;
   - `-0` is accepted as zero;
   - extra top-level keys in the weights JSON are allowed;
   - validation-only runs may pin weights that differ from the frozen TRAIN run's (signs are
     weight-independent; both pins are recorded). If research governance wants weights frozen
     with TRAIN, that is one extra check in `frozen_train`.

## T1 fix round 1

Status: **DONE_WITH_CONCERNS** (not compiled; desk-checked). This round was done in the T7 lane on
`C:/atx-wt/pool-4`, branch `feat/mega-alpha-runner-fields-20260926`, on top of T7's `aa5f06dd`.

Commits:
- `2353bac8` fix(ic-runner): VM-identity cache key, TRAIN-bound weights, pinned signs [mega T1 fix 1]
  (`strategy_ic_runner.cpp` and its test).
- `4e018e58` docs(alpha): point VM editors at the IC cache semantics version [mega T1 fix 1].
  - Comment only, in `atx-engine/include/atx/engine/alpha/vm.hpp`.
  - It is a separate commit because touching `vm.hpp` recompiles ~93 direct includers. Root may defer it.

Build and test are the same as T7: `atx-impl-strategy-ic-tests` plus `atx-equity-strategy-ic`, then
`-Ctest -R StrategyIcRunner`. There is no CMake change. Line numbers below are at `d4992a32`.

### C1: engine/VM identity in the cache key (`strategy_ic_runner.cpp` :54-99, :685-704, :740-760)

**The identity.** It is `dslvm<V>_<compiler><fp-flavor>`, for example `dslvm1_clang18.1`:
- `V` is `dsl_vm_semantics_version` (:60, now 1). It is an explicit constant in the runner. The comment beside it
  lists what forces a bump: parse/compile, VM kernels, and the Cs/Ts ops.
- `compiler` is `__clang_major__.__clang_minor__` (or `_MSC_VER`) of this TU. This TU instantiates the header-only
  VM.
- `fp-flavor` is `_fma` / `_avx2` / `_fastmath` when those macros are defined. That separates `rel-avx2`
  (`/arch:AVX2`) from dev/rel. Patch-level compiler updates are assumed not to change strict-FP results.

**Where the version lives.** In the runner. The `vm.hpp` header comment (commit `4e018e58`) points VM editors at it.

**Why not a source-tree hash.** A digest of the VM/DSL sources is feasible through the existing configure-time
provenance (`file(SHA256)` + `CMAKE_CONFIGURE_DEPENDS`), but I did not prefer it:
- it needs a CMake change that I cannot build;
- it invalidates the cache on comment-only edits;
- coverage of every header the VM really depends on would be hand-maintained and silently incomplete;
- it would orphan the existing cache.

**Layout.**
- `ROOT = DIR/<identity>/`, so an identity change is a clean miss and recompute into a new directory.
- New sidecars record `vm_identity`. On lookup it must match, otherwise the run refuses loudly: foreign bytes in our
  own directory. `vm_identity` also appears in plan and summary.
- An unknown compiler refuses `--candidate-cache`.

**Legacy cache kept usable.** The identity `dslvm1_clang18.1` (clang-cl 18.1.8, dev preset, no `/arch`) keeps
`ROOT = DIR`, the old layout. There it accepts *keyless* sidecars only if their recorded `engine_git_sha` is
`429cbe43…` or `6d85ac2a…`. Evidence:
- the 48 existing entries under `build-equity/mega-candidate-cache/` carry exactly those two engine SHAs (37 and 11);
- `git diff` of `atx-engine/{include,src}/…/alpha`, `parallel` and `atx-core` from each SHA to HEAD is empty;
- this host's clang-cl reports 18.1.8, and the dev preset sets no arch flags.

Consequences:
- T9's `DIR/<train_sha>/` lookup keeps working for dev builds.
- Keyless entries from any other engine build, or read under any other identity, refuse.
- Note: the pending role repair changes role SHAs, so the old entries will simply stop matching (a cold recompute,
  ~60-90 s). No invalidation logic is involved.

**M1.** `vm_workers` is still recorded but not matched. The key comment states the DetPool determinism dependency,
and a new fixture pins raw payload bytes serial vs 2 workers.

### I1 plus the root-ruled weights contract (`composition_weights` :638, `composition_signs` :610, `frozen_train` :226)

**Weights file contract** (T9 must emit this). Schema `atx.dsl-composition-weights/v1` with:
- `library_sha256`;
- `weights`;
- **top-level `train_manifest_sha256`**. It is required on every pinned-weight run and must equal `--train-sha256`.
  In validation-only mode that value is also the frozen TRAIN artifact's `train_manifest_sha256`, which is already
  enforced.
- optional **`signs`**: an object of candidate id → integer `+1` or `-1`.
  - Every candidate with weight > 0 must have a sign.
  - Unknown ids, `0`, `1.0`, `"1"`, and a non-object value are refused.
  - An unsigned zero-weight candidate gets 0.
- Unknown top-level keys stay tolerated, so T9's `provenance` block is fine.

**Pinned signs** replace the IC orientation **in the blend only** (`composition.add`). IC diagnostics, `daily_ic`,
`frozen_train_sign` and orientations.json keep the TRAIN IC sign. When signs are pinned:
- recipe `composition` becomes `pinned-candidate-weights;pinned-candidate-signs;…`, which changes the canonical hash;
- summary and plan carry `composition_signs: "pinned-candidate-signs"`, and so does the saved combined manifest;
- `signal_semantics` in the combined manifest is unchanged, so replay still admits the blend;
- each candidate carries `composition_sign`.

**Frozen blends.** A blend frozen WITH pinned weights resumes only with exactly the same weights pin. Running it
without weights, or with other weights, refuses with `validation must pin exactly the same --composition-weights`.
An unweighted frozen source admits any weights bound to the same TRAIN manifest.

**M7.** Each candidate summary carries `composition_weight` when weights are pinned (an NR input).

### Other review items

- **M4**: `// SAFETY:` note added at `write_partial`'s `reinterpret_cast`.
- **M8**: `--help` now documents the cache layout, fsync and partial files, and the weights contract.
- **Not done**: M2 (fail-fast pre-scan), M3 (shared hex helper), M5 (exact equality: the composition's
  centered-rank formula is not visible to the test oracle, so it stays at 4 ULP) and M6.

### Fixtures (`strategy_ic_runner_test.cpp`)

- `pin_weights` now writes `train_manifest_sha256` (plus optional signs and file name).
- `stable_role` strips `composition_weight` and `composition_sign`.
- Cache paths are resolved through `cache_root(summary)`, the identity-scoped root reported by the run.
- `InvalidCompositionWeightsRefuseBeforeAnyPayloadOrOutput` gains refusals (plan-only and real) for:
  - missing TRAIN binding and wrong TRAIN binding;
  - signs not an object;
  - unknown signed id;
  - sign 0, `1.0` and `"1"`;
  - a missing sign for a weighted candidate.
- `ValidationOnlyResumeComposesCandidateCacheAndTrainBoundWeights` (rewritten):
  - unweighted source + TRAIN-bound weights (cold, then warm);
  - weighted source + the same weights: byte-identical to the source;
  - refusals: weighted source without weights, with other weights, and weights bound to another TRAIN.
- `PinnedSignOppositeToIcOrientationFlipsOnlyThatBlendContribution`:
  - weights 2/0 and sign −1 give a blend exactly equal to −(unsigned blend) = −2r;
  - per-candidate IC diagnostics are unchanged and orientations stay +1;
  - the records above are present;
  - a sign equal to the IC sign reproduces the unsigned blend bytes.
- `CandidateCacheIsScopedByVmIdentityAndRefusesForeignSidecars`:
  - the root is `DIR` iff the identity is `dslvm1_clang18.1`;
  - entries under other identity directories are never read (clean miss);
  - a sidecar with a foreign `vm_identity` refuses;
  - keyless sidecars refuse unless they carry a legacy engine SHA under the legacy identity.
- `CandidateCachePayloadBytesAreIdenticalAcrossVmWorkers` (M1).

### Concerns

1. Not compiled. The `#if` block that builds the identity string (compiler macros) is the likeliest friction point.
2. **T9 coordination.** T9 must write top-level `train_manifest_sha256`. Its current `provenance.role_manifest_sha256`
   alone is not read.
   - If T9 emits `signs`, every weighted candidate must be signed ±1.
   - T9's cache lookup `DIR/<train_sha>/` holds only for the dev-build identity and base-only candidates. Otherwise
     it needs `DIR/<identity>/…`, and field candidates are under `…/<fields-sha>/` (see the T7 report).
3. Relying on a version bump is a process control: a VM change without a bump is still undetected. The comment in
   `vm.hpp` and the one beside the constant are the mitigation.
