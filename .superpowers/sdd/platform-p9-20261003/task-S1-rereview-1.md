# Lane S1 re-review 1 (fix round 1)

## Verdict
APPROVE

The major is addressed. I found 0 blocker, 0 major and 1 new minor (non-blocking). The original review's minors stay
deferred, per the PM, and I do not re-raise them.

## Reviewed SHAs
- FIX_BASE: 17a885a8.
- Fix head: 34ef92dd (the code and gtest are in 327f7e0a; 34ef92dd adds only the report, 94 lines).
- Pool `C:/atx-wt/pool-18`, HEAD 34ef92dd, clean tree.
- Diff: strategy_marginal_ic.{cpp,hpp} and strategy_marginal_ic_test.cpp only.

## Major finding
- atx-impl/src/strategy_marginal_ic.cpp:908-909 (with :341-364): Debug and Release shared entries through the legacy
  DIR fallback. **ADDRESSED.**
  - Root selection: `marginal_cache_roots` (cpp:853-871) derives `own` from the runner's own `icd::cache_root`, so it
    follows the same rule the u pass writes by.
    - The legacy identity keeps the v8 roots in v8 order: `DIR/<id>`, then `DIR`.
    - Any other identity reads `{DIR/<id>}` alone. A Release run no longer falls back to `DIR`.
  - Sidecar check: `accept_sidecar` (cpp:358-362) now requires `vm_identity == this build's identity` under every
    identity, and refuses with InvalidArgument otherwise.
    - This is the runner's rule (`v2_payload_sha`, strategy_ic_signal_cache.cpp:305) and the fitter's rule
      (fit_composition_weights.py:810).
    - `resolve_entry` is the only producer of a `CacheEntry`, so every candidate payload the verb reads passes this
      check.

## Checks
(a) **Isolation holds in both directions on every path.**
- Release reaching a Debug entry: Release searches only `DIR/<rel id>/<role>[/fp_*]`, and every sidecar must record
  the Release identity.
- Debug reaching a Release entry: Debug searches `DIR/dslvm1_clang18.1/<role>` and `DIR/<role>[/fp_*]`.
  - A Release root `DIR/<rel id>/` is never under `DIR/<role>`, because `entry_directories` adds only `fp_*` children.
  - Even a Release sidecar planted in a Debug root refuses on the identity check.
- A Debug run pointed at `--candidate-cache DIR/<rel id>` was a second silent sharing path in v8. It now refuses as
  well.
- Pair cache: its directory is `pairs<v>_<key16>`, and the key carries the build token (pair_cache.cpp:47-58). The fix
  does not change it.
- The verb writes nothing else into a cache.

(b) **Flag-absent Debug bytes are identical. No live v8 cache hits the new refusal.**
- The roots, their order, `build_vm_identity` and `directory` are unchanged for the legacy identity. No output field
  reads the sidecar's identity (`CacheEntry::vm_identity` is never read).
- The one behaviour delta is that a v2 sidecar under the Debug roots recording a non-legacy identity now refuses. The
  check also runs before the `--fields` filter, so a stale `fp_*` entry would refuse instead of being skipped. No
  such entry can exist, for three reasons:
  - The cache_root rule ("DIR only for the legacy identity") arrived in a9ef7175. That commit is an ancestor of
    dcbf4a53, the first v2 writer.
  - Every v2 writer records `key.vm_identity = vm_identity()`.
  - The runner's signal-cache TU is the only writer of v2 sidecars. research_mine.py, the fitter and
    compare_window_overlap.py only read them.

  So every v2 sidecar under `DIR` records `dslvm1_clang18.1`.
- Every cache a Debug u pass accepts is therefore read exactly as v8 read it. The runner and the fitter already
  refuse any other identity at their own key paths.
- Live scan: I read the signal sidecars only, never the `ic1_*` result directories, using scratchpad
  `s1rr_ids.py`. Both live v8 caches have a single `<role sha>` top level and no identity subdirectory:
  - `pool-2/build-equity/mega-candidate-cache-v8-lo1`: 48/48 v2 sidecars record `dslvm1_clang18.1`, 0 keyless;
  - `pool-2/build-equity/mega-candidate-cache-v8-lo3`: 117/117 v2 sidecars record `dslvm1_clang18.1`, 0 keyless.

  No live v8 cache can hit the new refusal.

(c) **The test checks the contract, and no existing test is weakened.**
- `MarginalIc.CacheRootsNeverShareAcrossBuilds` pins the rule as strings, not through helpers:
  - legacy is the literal `dslvm1_clang18.1`;
  - a non-legacy build gets `{DIR/<id>}` only;
  - under `NDEBUG` the build must not be the legacy one.
- It then runs the review's exact case end to end. In the Release tree, Debug-only entries under `DIR` give NotFound.
  It also covers the reverse direction, and a foreign sidecar in a build's own root giving InvalidArgument.
- Run against 17a885a8, the test fails in both trees. In Release, the fallback would read `legacy_own`. In Debug,
  `own_foreign` would be accepted. So it discriminates the fix.
- The `cache_identity` probe exercises the same single-root branch Release uses. The production `icd::cache_root`
  non-legacy branch runs only in the Release tree, which root's step 2 covers.
- The fixture change is required (a "synthetic" sidecar now refuses) and weakens nothing:
  - In Debug, entries stay at the v8 path, `DIR/<role>`.
  - No existing test asserted on a sidecar's identity or relied on cross-identity acceptance. No test hard-codes the
    cache path; `RefusesInputsNotBoundToThePool` follows `cache_entry_dir`.
  - In Release the suite now exercises the real Release root instead of the legacy fallback, which strengthens it.
- No other caller of `run_marginal_ic`, and no other test that feeds the marginal verb, exists in atx-impl or
  atx-engine.

(d) **Compile hazards: none found.** I scanned all 179 added lines (scratchpad `s1rr_cols.py`): no line exceeds 100
columns, and none has non-ASCII bytes, tabs or trailing whitespace. Other checks:
- Visibility and includes:
  - `icd::cache_root` is declared in strategy_ic_detail.hpp:320 and linked through the same TU as the
    already-used `ic_cache_vm_identity`.
  - `IcRunnerConfig` is complete (strategy_ic_runner.hpp is included), and every scalar member has an initializer,
    so `IcRunnerConfig runner;` leaves nothing uninitialised.
  - The header adds `<filesystem>` and `<vector>`.
- Lifetimes and names:
  - `const auto& identity = layout.identity` refers to a local that lives for the whole scope, and is used at
    cpp:1036 in that scope.
  - No new name collides with a TU-local name.
- Test code:
  - Every lambda capture is used.
  - The `ASSERT_*` macros sit in a void lambda.
  - json-vs-`std::string` `EXPECT_EQ` follows the existing pattern (test:326).
  - `fs::copy_file` uses the throwing overload, which returns bool.

(e) **Scope:** no new file. The 3 code files are S1-owned, plus S1's own report.

Rules held: I edited no tree, built nothing, ran no real data, and opened no return, IC, Sharpe or NAV output or
anything dated 2024-01-01 or later. The cache scan read only signal-sidecar metadata keys (`schema`, `vm_identity`).

## New findings
path:line | severity | problem | required fix

- atx-impl/src/strategy_marginal_ic.hpp:69-72 (cpp:856-858) | minor | The `cache_identity` test seam documents "must
  not be the legacy one", but nothing enforces it.
  - Problem: set to `dslvm1_clang18.1`, it silently reads only `DIR/dslvm1_clang18.1`, never `DIR`. It also takes
    any string as a path component. There is no CLI flag, so there is no production exposure.
  - Required fix (non-blocking): refuse a legacy, empty-component or path-separator value in `run_marginal_ic`'s
    bounded-config check, or drop the precondition text.
