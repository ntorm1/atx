# Lane S1 report

## Outcome
DONE_WITH_CONCERNS: the marginal verb speed work and the Release IC build token are implemented
and committed, and the named gtests are written. Under the lane rules no C++ was compiled and no
test was run, so every C++ claim below is unverified until root builds it.

## Branch / SHA
feat/p9-s1-20261003 @ 9ffd4587 (code), plus the commit that adds this report. Pool tree left
clean.
- bb923166 feat(ic): build flavour token in vm_identity / ic_identity; Release IC preset (DS-1)
- 84740151 feat(marginal): --candidates, compaction, pair cache, verified digests, date bands,
  33 themes
- 9ffd4587 test(marginal): P9 S1 gtests (written, not run)

## Frozen base / lease
base_sha=d7c1c520 (frozen base). worktree=C:\atx-wt\pool-18. The pool was leased by the
controller and its keeper was alive. This lane did not lease or release it, so the
lease/run/heartbeat/keeper fields are the controller's.

## Files changed
- NEW atx-engine/include/atx/engine/build_flavor.hpp: BuildFlavor, fp_flavor_suffix,
  build_flavor_token, legacy_build_flavor, build_flavor_suffix, ATX_ENGINE_BUILD_FLAVOR.
- atx-engine/include/atx/engine/factory/ic_screen.hpp and src/factory/ic_screen.cpp:
  `ic_screen_build_flavor()`, one declaration and one definition (cross-lane, see below).
- atx-impl/src/strategy_ic_signal_cache.cpp: `vm_identity()` adds
  `build_flavor_suffix(ATX_ENGINE_BUILD_FLAVOR)`.
- atx-impl/src/strategy_ic_result_cache.cpp: `ic_identity()` takes its FP words and the
  token suffix from ic_screen.cpp's TU. `ic_result_sources` adds build_flavor.hpp (include
  closure). `ic_result_sources_sha256` is re-pinned without a bump (e7a40331... ->
  2d758bff...), because no scored bit changes.
- CMakePresets.json: `equity-rel` gets `FETCHCONTENT_BASE_DIR=${sourceDir}/deps/equity-rel`
  (per-worktree, as equity-bench and equity-hygiene have). New build preset `equity-rel-ic`
  builds `atx-equity-strategy-ic` and `atx-impl-strategy-ic-tests`.
- atx-engine/include/atx/engine/combine/marginal_rank_ic.hpp and
  src/combine/marginal_rank_ic.cpp:
  - `kMaxMarginalRegressors` goes 11 -> 34.
  - New `kPairwiseRowCorrelationVersion = 1`, `marginal_rank_ic_build_flavor()` and
    `PairSeed`.
  - PairwiseRowCorrelation gains `restrict_to`, `seed` (all-or-nothing), `computed_pairs`,
    `day_values` and `accumulate` (together exactly `add_date`; the per-pair body is
    factored into `pair_value`), and `sum`.
- NEW atx-impl/src/strategy_marginal_pair_cache.{hpp,cpp}: the `--pair-cache` store.
  - Key: role, member mask SHA, score_begin, rows, min_names, method version, and the build
    flavour of the computing TU.
  - Layout: `DIR/pairs<v>_<key16>/<record32>.pairs.json`. Shards are self-hashed, carry
    the full key, store sums as IEEE bits, and are published no-replace and
    content-addressed.
  - `pair_sources_sha256` = 88f97f24...
- atx-impl/src/strategy_marginal_ic.{hpp,cpp}:
  - New options: `--candidates FILE`, `--pair-cache DIR`, `--verified-digests FILE`,
    `--workers 1..16`, `--exclude-self`.
  - Rows are compacted to the date's member names. Payloads that no listed row, theme or
    computed pair needs are never opened.
  - Timers: hash, read, kernel and pairwise.
  - Uses the engine's `research_return_guard` and the shared `icd::hash_valid`,
    `metadata_text`, `PartialFile`, `write_partial` and `publish_new` in place of the
    local copies.
  - The 6-argument `marginal_ic_working_bytes` admits the new options' memory.
- atx-impl/CMakeLists.txt: one appended `target_sources` line (cross-lane).
- atx-engine/tests/combine/combine_marginal_rank_ic_test.cpp and
  atx-impl/tests/strategy_marginal_ic_test.cpp: the tests listed under Evidence.

## Evidence
No C++ build or test ran (lane rule). Mechanical checks that did run:
- `python scratchpad/pins.py` -> exit_code=0:
  ```
  ic base (expect e7a40331...): e7a40331a3f2f1a4268feece00d354961ae7ab8215a379733d5855f40f61579a
  ic new : 2d758bff0cfb5090c965d1dc3e4c9403ecdb8f0e29b3a84b243e4532c99615ed
  pair   : 88f97f24745c9a5a675b07232fd82d980e9c6e27b38c4c0b44519e48be6b39f3
  ```
  The script reproduces the base pin from `git show d7c1c520:<path>` with the test's recipe:
  SHA-256 over `<path>\n<len>\n<text>`, with CRLF->LF. The new constants equal its output.
- Presets JSON parse -> exit_code=0:
  `['equity-rel', 'equity-rel-ic'] [{'CMAKE_BUILD_TYPE': 'Release', 'FETCHCONTENT_BASE_DIR':
  '${sourceDir}/deps/equity-rel'}]`
- Long-line and non-ASCII scan of every changed file. New lines are <= 100 columns. The only
  exceptions are v8 lines that only gained an `icd::` or `pinned_document` rename.

Tests written:
- MarginalIc:
  - CandidatesSubsetEqualsFullRows
  - CompactedRowsEqual
  - PairCacheHitEqualsCompute
  - ThirtyThreeThemes
  - BandsByteIdenticalAt1And4 (also runs 3 and 16 workers, and --exclude-self)
  - ExcludeSelfDropsTheMembersOwnTerm
  - VerifiedDigestsSkipTheRehash
  - PairCacheSourcesPinned
  - StreamsByDateUnder600MiB (extended)
- IcIdentity.BuildTokenSeparatesCaches
- CombineMarginalRankIc:
  - AcceptsThirtyFourRegressors
  - CompactedNamesKeepBits
  - PairDayValuesThenAccumulateIsAddDate
  - RestrictToKeepsListedPairs
  - SeededPairsEqualComputedPairs
  - RefusesBadShapesAndBounds (bound moved to kMax+1)

## How root verifies
1. Build the targets:
   - Debug: `powershell -File scripts\research-build.ps1 -Tag p9-s1 -Targets
     "atx-equity-strategy-ic,atx-impl-strategy-ic-tests"`
   - Release: the same command with `-Preset equity-rel`.
   - The engine tests also need the target that owns `combine_marginal_rank_ic_test.cpp` in
     atx-engine/tests (the file is also compiled into atx-impl-strategy-ic-tests).
   - The first equity-rel configure fetches deps into `deps/equity-rel`.
2. In both trees, run the anchored filter:
   `atx-impl-strategy-ic-tests --gtest_filter=MarginalIc.*:IcIdentity.*:CombineMarginalRankIc.*:StrategyIcRunner.IcSourcesPinnedToSemanticsVersion:StrategyIcRunner.VmSourcesPinnedToSemanticsVersion`.
   Under Release, also run the whole atx-impl-strategy-ic-tests binary plus the alpha oracle
   and conformance suites.
3. Flag-absent identity:
   - Run the d7c1c520 Debug exe and the new Debug exe on the same X-5 `marginal` argv. The
     two `marginal_ic.json` files must be byte-identical except `stage_seconds`, which gains
     the labels, hash, read, kernel and pairwise keys.
   - Debug u/w summaries: the `vm_identity` / `ic_identity` strings must be unchanged against
     d7c1c520, and a warm rerun must hit the existing cache roots.
4. Release against Debug:
   - The u/w daily IC CSV, candidates.jsonl, orientations and combined payload must be
     byte-identical.
   - The summary identity strings differ by design (suffix `_opt_md_ndebug_xs13.0.0`) and
     write separate cache roots.
5. Speed: E1 wires the call site. Root logs marginal wall time per pass, with and without
   `--workers 8 --pair-cache`.

## Deviations from brief
- `safe_id` and the h21 label recipe stay local: no shared copy exists in
  strategy_ic_detail.hpp. The return guard now comes from the engine.
- The `equity-rel` configure preset already existed. I added deps isolation and the
  `equity-rel-ic` build preset.
- research_cycle.py is untouched, per ruling P4.
- Per the owner's no-TDD directive, the tests were written after the implementation. None
  were run.
- `PairCacheSourcesPinned` checks the digest only, with no include-closure rule: the
  residualisation and HAC headers never touch a pair.
- `--exclude-self` is report-only. The book regressor is the plain reconstruction
  sum of s*w*r over the other weighted members, not the saved composite minus the member's
  term. That differs from the saved composite under non-plain blends.
- I tried to drop the ic_screen edit by reading the flavour in strategy_ic_result_cache.cpp's
  own TU, which shares CRT, NDEBUG and xsimd. The session's permission classifier blocked
  reverting the header include, so the original design (the flavour read in the scoring TU)
  stays.

## Cross-lane edits
- atx-impl/CMakeLists.txt: one appended `target_sources(atx-impl-core PRIVATE
  src/strategy_marginal_pair_cache.cpp)`.
- ic_screen.hpp/.cpp: S2 owns these in wave 2. They get one declaration and one definition,
  and the `ic_result_sources_sha256` re-pin follows from that. Any integration that also
  touches a listed IC source must recompute the pin with the recipe above.
- strategy_ic_detail.hpp: included only, not edited.

## Open risks
- atx-engine/src/factory/research_ic_fitness.cpp still says "at most 11 regressors"; its bound
  follows kMaxMarginalRegressors, which is now 34, and so do strategy_mine.cpp's.
- The new code has not been compiled. Spots to watch:
  - the designated-initialiser macro;
  - `std::optional<DetPool>::emplace`;
  - the RowReader mutex (`unique_ptr`).
- The statistical thresholds in ThirtyThreeThemes were argued, not replicated in numpy:
  `marginal_ic21 > 0.2`, and single-member themes spanned on every row at 34 regressors.
- The token's `<opt>` field relies on `__OPTIMIZE__` under clang-cl. If that macro is absent,
  the Release token reads `noopt_...`. It stays distinct from Debug, and the legacy rule
  ignores the field.
- At 256 candidates and 16 workers, one band chunk of pair values is about 67 MB (counted in
  admission). E1 should keep `--workers` at 8 or below under the 600 MiB default.
- Under Release, any StrategyIcRunner test that hard-codes the Debug identity literal fails by
  design.

## Ledger candidates
- The IC source-pin digest reproduces in Python: SHA-256 over `<path>\n<len>\n<text CRLF->LF>`
  per listed path. It matched e7a40331 at d7c1c520 (P9 S1).
- Build token: the legacy equity-dev flavour (asserts on, /MDd, xsimd 13.0.0) maps to "", so
  pre-P9 cache roots stay valid. equity-rel identities end `_opt_md_ndebug_xs13.0.0` (P9 S1).
- Marginal rows compact to member names bit-identically: residualize_signal,
  centred_tied_ranks and the pair body read only finite cells, in ascending index order
  (P9 S1).

## Fix round 1

### Outcome
DONE: the review's one major is fixed. FIX_BASE 17a885a8, fix commit 327f7e0a (code + gtest),
plus the commit adding this section. No C++ was built or run (lane rule). The minors stay
deferred per the PM (progress.md "Task S1: minor (deferred)"). None of them touch these lines.

### Finding addressed
- review S1 major, atx-impl/src/strategy_marginal_ic.cpp:908-909 (with :341-364).
  - The problem: the verb searched `DIR/<vm identity>` and then `DIR`, and never compared a
    sidecar's `vm_identity`. So a Release run could read Debug entries from the legacy root
    (INFRA-F, DS-1).
- The fix applies both remedies the review offered:
  - New `marginal_cache_roots(cfg)` (strategy_marginal_ic.hpp/.cpp) uses the runner's own
    rule: `icd::cache_root`.
    - The legacy identity (equity-dev Debug, root `DIR`) keeps the v8 roots in v8 order:
      `DIR/<identity>`, then `DIR`.
    - Any other identity (equity-rel) reads `DIR/<identity>` alone, never `DIR`.
  - `accept_sidecar` now requires `vm_identity == this build's identity`, under every
    identity. This is the rule the runner (`v2_payload_sha`) and the weights fitter
    (fit_composition_weights.py) already apply. A foreign sidecar refuses with
    InvalidArgument ("records vm_identity ...").
  - Together, these close both directions:
    - Release never falls back to the legacy `DIR`.
    - Debug's legacy roots never reach `DIR/<release id>/`. A foreign sidecar planted in
      `DIR` itself refuses.
  - Writes are unchanged. The verb writes only its output directory and the pair cache, and
    the pair-cache key already carries the computing TU's build flavour.
- The previous implementer's partial edit compared identities only for non-legacy builds. The
  Debug legacy fallback stayed open to a foreign sidecar, so I rewrote that part. The
  legacy-only `required_identity` field is gone.
- Test seam: `MarginalIcConfig::cache_identity`, with no CLI flag. When set, the verb reads as
  a build of that non-legacy identity: root `DIR/<it>`, and its sidecars must record it.
- Debug identity:
  - Same roots, same order.
  - The output's `build_vm_identity` and `directory` are unchanged.
  - The pin lists are untouched: strategy_marginal_ic.* is in no source pin.
  - The one behaviour delta: a v2 sidecar whose `vm_identity` is not the legacy identity now
    refuses instead of being read. The Debug runner writes the legacy identity into every v2
    entry under `DIR`, and it refuses any other identity itself. So the outputs on every
    cache the Debug u pass accepts are byte-identical.

### Files changed
- atx-impl/src/strategy_marginal_ic.hpp:
  - `MarginalCacheRoots`;
  - `marginal_cache_roots`;
  - `MarginalIcConfig::cache_identity`;
  - comments.
- atx-impl/src/strategy_marginal_ic.cpp:
  - `accept_sidecar` and `resolve_entry` take `identity`;
  - `marginal_cache_roots`;
  - `run_marginal_ic` uses them.
- atx-impl/tests/strategy_marginal_ic_test.cpp:
  - The fixture writes entries where this build's u pass would: under the legacy identity
    that is `DIR/<role>`, the v8 directory, and otherwise `DIR/<identity>/<role>`. The
    sidecars record this build's identity, where v8 recorded "synthetic". Without this the
    MarginalIc suite could not find its entries in the Release tree.
  - New gtest `MarginalIc.CacheRootsNeverShareAcrossBuilds` (written, not run). It checks
    the rule for a probe identity and for this build. Under NDEBUG it asserts that this build
    is not the legacy one. It then runs these cases end to end:
    1. Entries only in the legacy `DIR`, recording `dslvm1_clang18.1`, give NotFound for the
       probe. For this build they are read with the reference rows when it is the legacy
       build, and give NotFound otherwise (Release).
    2. Entries only in `DIR/<probe>/`, recording the probe, give NotFound for this build.
       The probe reads them, and its rows equal the reference rows.
    3. A foreign sidecar in a build's own root (the probe's, and this build's, which is
       `DIR` itself under legacy) refuses with InvalidArgument.

### Evidence
- No build and no test ran (lane rule). I re-read every edited line for clang-cl 18
  `/W4 /permissive- /WX` hazards and found none:
  - no unused names (`refuses` and `text_file` are still used elsewhere);
  - no sign conversion and no shadowing;
  - the includes are present: `<filesystem>` and `<vector>` in the header;
  - `IcRunnerConfig` and `icd::cache_root` come from the TU's existing includes.
- A scan of the added lines shows none longer than 100 columns and no non-ASCII byte.

### How root verifies
1. Build `atx-impl-strategy-ic-tests` and `atx-equity-strategy-ic` in Debug (equity-dev) and in
   Release (`equity-rel` / build preset `equity-rel-ic`).
2. In both trees, run `atx-impl-strategy-ic-tests --gtest_filter=MarginalIc.*`.
   `MarginalIc.CacheRootsNeverShareAcrossBuilds` is the fix's test, and the whole suite
   checks the fixture move.
3. Flag-absent Debug identity, unchanged from the main report's step 3: run the d7c1c520 and
   the new Debug exe on the same X-5 `marginal` argv. The two `marginal_ic.json` files must be
   byte-identical except `stage_seconds`.
4. Release adoption: a Release `marginal` run on a DIR that holds only Debug entries must
   refuse with NotFound ("no candidate cache entry"). After a Release u pass fills
   `DIR/dslvm1_clang18.1_opt_md_ndebug_xs13.0.0/`, the same run must succeed.

### Open risks
- A Release marginal run now needs a Release u pass first. E1 must pass the same `DIR` the
  Release u pass wrote; it does, because `--candidate-cache` is the u pass's DIR.
