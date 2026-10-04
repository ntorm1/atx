# Lane S1 review

## Verdict
BLOCK

0 blocker, 1 major, 11 minor. Spec compliance: ❌. One clause fails: INFRA-F requires "Debug and Release never share
cache entries", and the marginal verb's fallback cache root breaks it. Every other S1 deliverable is ✅.

Every deliverable in the brief, ruling P4 and DEC-11/DEC-12 is present:
- `--candidates FILE`, using P4's exact semantics;
- member compaction;
- a content-hash pair cache;
- `--verified-digests`;
- the 33-theme cap;
- DetPool date bands;
- stage timers;
- the engine return guard, plus the shared helpers;
- `--exclude-self`;
- the build token in `vm_identity` / `ic_identity`, with `ic_identity` taken from ic_screen.cpp's TU;
- the `equity-rel-ic` preset.

The C++ reads clean under clang-cl 18 `/W4 /permissive- /WX`:
- I found no unused anonymous-namespace names and no signed/unsigned compares.
- No brace init narrows, and no `if (..) ATX_TRY_VOID(..); else` is malformed (the macro is a do-while(0)).
- The designated-initialiser macro's field order matches `BuildFlavor`.
- `std::optional<DetPool>::emplace` is fine for a pinned type.
- The `RowReader` mutex is a `unique_ptr`, so the reader stays movable.
- No header name collides with the marginal TU's anonymous namespace. strategy_ic_detail.hpp declares everything inside
  `ic_detail`.
- New lines stay within 100 columns. The only longer lines are v8 lines that gained an `icd::` or `pinned_document`
  rename.

The single major is a one-line fix. The merge is blocked on it alone.

## Reviewed SHA
17a885a8c12c82bef7ec645ef7a377779dd85b73 (base d7c1c520). Pool `C:/atx-wt/pool-18`, branch `feat/p9-s1-20261003`.
The tree was clean at HEAD.

## Evidence
The lane has no pytest (C++ and preset only). I ran a read-only static check from `git show` blobs:
- it recomputes every source pin with the test's recipe (`<path>\n<len>\n<text CRLF->LF>`, as in
  `expect_sources_pinned`) at base and at the lane SHA;
- it checks the atx/engine include closure for the IC and VM lists.

Script: scratchpad `s1/pins.py`.

```
$ "C:/Program Files/Python312/python.exe" -B s1/pins.py ; echo exit_code=$?
d7c1c520 ic  computed e7a40331a3f2f1a4268feece00d354961ae7ab8215a379733d5855f40f61579a pinned e7a40331a3f2f1a4268feece00d354961ae7ab8215a379733d5855f40f61579a
d7c1c520 vm  computed fcf8021e76ee5ac3c161302367a27b8f6c49cd28a580d1339ff801d9feca1619 pinned fcf8021e76ee5ac3c161302367a27b8f6c49cd28a580d1339ff801d9feca1619
17a885a8 ic  computed 2d758bff0cfb5090c965d1dc3e4c9403ecdb8f0e29b3a84b243e4532c99615ed pinned 2d758bff0cfb5090c965d1dc3e4c9403ecdb8f0e29b3a84b243e4532c99615ed
17a885a8 vm  computed fcf8021e76ee5ac3c161302367a27b8f6c49cd28a580d1339ff801d9feca1619 pinned fcf8021e76ee5ac3c161302367a27b8f6c49cd28a580d1339ff801d9feca1619
pair computed 88f97f24745c9a5a675b07232fd82d980e9c6e27b38c4c0b44519e48be6b39f3 pinned 88f97f24745c9a5a675b07232fd82d980e9c6e27b38c4c0b44519e48be6b39f3
closure checked
vm closure checked, unlisted = 0
exit_code=0
```

Results:
- The IC re-pin is arithmetically right: 2d758bff is the digest of the 11 listed sources.
- The VM pin is untouched, and its closure is complete. ic_screen.hpp's new include of build_flavor.hpp is listed.
- The pair pin reproduces.

I also grepped every non-test caller of a changed symbol (`git grep` over `*.cpp`/`*.hpp`). Callers:
- `PairwiseRowCorrelation`:
  - strategy_marginal_ic.cpp:634/695/769/787/919;
  - strategy_mine_promote.cpp:56;
  - strategy_mine_rule.{hpp,cpp}. These call only the unchanged ctor, `add_date`, `mean`, `dates`.
- `kMaxMarginalRegressors` (bound checks only):
  - research_ic_fitness.cpp:72;
  - strategy_mine.cpp:440;
  - strategy_mine_pool.cpp:205;
  - marginal_rank_ic.cpp:180/185;
  - strategy_marginal_ic.cpp:311/846.
- `marginal_ic_working_bytes`: the 5-argument overload is kept. The 6-argument one is new.

No caller breaks. No default output byte depends on the cap.

I resolved the presets JSON: `equity-rel` gives Release, ATX_EQUITY_ONLY ON and deps/equity-rel. `equity-bench` and
`equity-asan` keep their own `FETCHCONTENT_BASE_DIR`.

## Findings
path:line | severity | problem | required fix

- atx-impl/src/strategy_marginal_ic.cpp:908-909 (with :341-364) | major | Debug and Release still share entries
  through the marginal verb.
  - Problem:
    - `roots = {DIR/<vm identity>, DIR}`, and `accept_sidecar` never compares the sidecar's `vm_identity` with the
      build's.
    - Under equity-rel the identity is `dslvm1_clang18.1_opt_md_ndebug_xs13.0.0`. Any candidate missing from
      `DIR/<rel id>/` therefore falls back to `DIR/<role>/`, the legacy equity-dev root.
    - So a Release marginal run silently consumes Debug-built payloads. The output still records
      `build_vm_identity` = the Release identity.
    - This contradicts INFRA-F ("so Debug and Release never share cache entries") and DS-1, whose stated reason is
      CRT log/exp/pow/tanh and NDEBUG.
    - It also contradicts this lane's own preset description ("never shares a candidate-cache ... entry with the
      Debug tree").
    - The runner and the Python fitter are strict: DIR is the root only for the legacy identity, and the sidecar
      identity must match.
    - The mixed case is realistic during Release adoption: a u pass run in Debug, or parents cached only by Debug,
      then marginal run in Release.
  - Required fix:
    - Mirror the runner. Either use `DIR` only when the build identity is the legacy one and `DIR/<identity>`
      otherwise, or require `text_of(j,"vm_identity") == identity` in `accept_sidecar` (v2 sidecars always record
      it).
    - Add a gtest: an entry only under `DIR` whose sidecar has a foreign `vm_identity` gives NotFound.
    - Debug flag-absent bytes are unaffected, because the legacy identity already resolves to `DIR`.
- atx-impl/src/strategy_ic_result_cache.cpp:42-45 | minor | The re-pin e7a40331 -> 2d758bff without a semantics bump
  is legitimate but under-reported.
  - Why it is legitimate:
    - `ic_result_sources_sha256` is a source tripwire, not an output expectation. It is read only by
      `IcSourcesPinnedToSemanticsVersion`, and its documented remedy is "bump or re-pin".
    - YOPS set the precedent at strategy_ic_signal_cache.cpp:76-78.
    - It is not in any IC-result cache key. The key is `ic_identity()`, which carries the semantics version, not the
      digest. So existing Debug entries stay valid, which is correct: no scored bit changes (one accessor plus a
      macros-only header).
    - A bump would change the Debug `ic_identity` (dslic2_) and void every IC-result cache, which breaks flag-absent
      identity.
  - What is under-reported: the digest does reach one output. `recipe_json` writes `library.ic_sources_sha256`
    (strategy_mine.cpp:381), so every mining campaign's `recipe_sha256` changes. The once-only confirm guard keys on
    that recipe. The cost in P9 is nil (OD-P9-9: no mined campaign).
  - Required fix: the report and the ledger candidate must state the mine-recipe effect. The PM records the cross-lane
    re-pin of S2's file as a ruling (decision -- why -- cost). S2 then recomputes in wave 2.
- atx-impl/src/strategy_marginal_pair_cache.cpp:35-39 | minor | The pair-cache tripwire covers only
  marginal_rank_ic.{hpp,cpp}.
  - Problem:
    - A cached pair's bits also depend on the rank inputs that strategy_marginal_ic.cpp builds (process_row :639-656:
      gather, compaction, `member_m` eligibility passed to `centred_tied_ranks`).
    - They would also depend on any helper `centred_tied_ranks` later delegates to (ruling P8: D1's group_rerank
      unification).
    - An edit there without a `kPairwiseRowCorrelationVersion` bump serves stale sums. The key is complete for today's
      code; the gap is future drift.
  - Required fix: add strategy_marginal_ic.cpp to `pair_sources`, or move the rank-input code into a pinned unit, or
    apply the include-closure rule. Add a BUMP comment at process_row's ranking block.
- atx-impl/src/strategy_marginal_ic.cpp:744-750, :969-977 | minor | With `--verified-digests --pair-cache`, pairs
  computed from payloads never re-hashed are published to the durable cross-wave cache, keyed by the SHA nobody
  checked.
  - Problem: a payload changed after the u pass poisons every later run, including runs that do verify.
  - Required fix: when `--pair-cache` is set, verify the payloads that feed newly computed pairs. Otherwise, skip
    storing pairs that have a vouched endpoint.
- atx-impl/src/strategy_marginal_ic.cpp:922 vs :932 | minor | The pair cache is loaded before admission and is
  under-counted.
  - Problem: up to 2^18 pairs are loaded, and up to 4,096 shards of 16 MiB each are parsed to a DOM. Map nodes with
    two 64-char `std::string` keys exceed the 256 B of `kMarginalCachedPairBytes`, and the shard DOM transient is not
    counted.
  - Required fix: admit from the shard file sizes before parsing, or count conservatively, and raise the per-pair
    bound.
- atx-impl/src/strategy_marginal_ic.cpp:857, :870-874 | minor | Per-worker kernel transients are not admitted.
  - Problem:
    - `residualize_signal` allocates per call: `out(m)`, X and Xw (m x p) and the COD workspace. That is about
      3·m·35·8 B, roughly 4.7 MiB per worker at 34 regressors and 5,627 names, or about 75 MiB at 16 workers beyond
      the 32 MiB slack.
    - `PairwiseRowCorrelation` now also holds `compute_` (k² B) and `computed_` (16 B per pair). Neither is in
      `k*k*16`, nor in mine's `kPairBytes` (strategy_mine.cpp:68, :480).
  - Required fix: add a per-worker kernel term and the new rho members to both models. Mine is the owner's file, so
    list that part as a note.
- atx-impl/src/strategy_marginal_ic.cpp:610-630, :834-839 | minor | Under `--exclude-self` the book regressor is the
  plain s·w·r reconstruction minus the member.
  - Problem: it is not the saved composite minus the member, so under ew-theme-std / standardised blends (the live
    book) it differs from the regressor every other row uses. The output documents this and rows are report-only,
    but the brief asked for "composite excluding the member".
  - Required fix: refuse `--exclude-self` for non-plain blends, or have the PM rule the approximation.
- atx-impl/tests/strategy_marginal_ic_test.cpp:717-776 | minor | `IcIdentity.BuildTokenSeparatesCaches` pins the
  implementation, not the contract.
  - Problem:
    - :771 (`legacy_build_flavor(flavor) == suffix.empty()`) is that function's definition.
    - The "this build" `ends_with` checks re-derive the identity from the same functions.
    - Nothing asserts that the equity-dev flavour yields the empty suffix and the pre-P9 identity bytes, which is the
      flag-absent contract.
  - Required fix: under `!NDEBUG && _DEBUG && _DLL`, assert an empty suffix, `vm == "dslvm1_"+vm_compiler+vm_fp_flavor`,
    and the matching `dslic1_..._simd<w>` form.
- atx-impl/tests/strategy_marginal_ic_test.cpp:673-690 | minor | `VerifiedDigestsSkipTheRehash` pins the `accepted`
  counter, not the skipped re-hash.
  - Problem: a regression that still hashes passes.
  - Required fix: corrupt a vouched payload in place, keeping its extent. The vouched run must complete while the
    plain run refuses with a SHA mismatch.
- atx-impl/tests/strategy_marginal_ic_test.cpp:613 | minor | `marginal_ic21 > 0.2` at 34 regressors on 80 names was
  argued, never replicated (report, open risks).
  - Problem: it is a likely red at root's first build.
  - Required fix: replicate the threshold in numpy, or assert defined plus sign only.
- CMakePresets.json:189 (report "How root verifies") | minor | Release adoption is gated on the alpha oracle and
  conformance suites (plan INFRA-F; DEC-11).
  - Problem:
    - Those suites live in `atx-engine-w1-foundation-tests`. `equity-rel-ic` omits that target, and the report names
      no target or filter for them.
    - The plan's root check "`--candidates` rows equal the full run's rows" on X-5 is not listed.
    - Nothing compares Release marginal with Debug marginal.
  - Required fix: name the exact targets and filters, plus those two root checks, or add the target to the preset.
- atx-impl/src/strategy_ic_signal_cache.cpp:98-101 | minor | The Release identity adds a 41-character directory under
  DIR, where legacy uses DIR itself.
  - Problem: the layout is one the code itself calls near the 259-character MAX_PATH (strategy_ic_detail.hpp:198-201:
    ROOT/<64>/fp_<16>/ic1_<16>/<id>.<dsl16>.json). Deep roots or long ids can now fail to open in Release only.
  - Required fix: root checks the deepest Release path at the first Release u pass, or the token is shortened (for
    example `_rel` plus 8 hex of the token).

## Checked
- [x] .agents/cpp/agent.md §10 applied to the diff. No UB, narrowing or uninitialised members.
  - Every new `Status`/`Result` is propagated.
  - `seed()` validates every seed before it applies any, so an Err is atomic.
  - `restrict_to` and `seed` refuse after the first date.
  - The bounds checks short-circuit before indexing (`hi >= rows_` precedes `compute_[...]`).
- [x] Flag-absent identity: marginal_ic.json with default argv is byte-identical except `stage_seconds`, which gains
  labels/hash/read/kernel/pairwise. I checked this by reading:
  - At the defaults (workers 1, no exclude-self, 0 cached pairs), `working_bytes` equals the 5-argument peak.
  - `inputs.candidates`, `verified_digests` and `pair_cache`, `method.exclude_self` and `workers` are each present only
    with their option.
  - Rows use the same arithmetic in the same order:
    - `residualize_signal` gathers the finite rows in ascending order;
    - `average_ranks`, `pearson_on`, `centred_tied_ranks` and `pair_value` visit finite cells in index order;
    - theme sums add in library order per theme, as v8 did;
    - the m == 0 path keeps v8's NaN/NaN/not-spanned defaults.
  - The engine `research_return_guard` (role_panel.cpp:34-55) is verbatim the deleted v8 copy.
  - Note: stdout gains one "marginal: stages ..." line. That is the log, not marginal_ic.json.
- [x] Debug identity strings are unchanged:
  - The equity-dev flavour is /MDd (`mdd`), asserts on, and xsimd 13.0.0. That version is confirmed in
    `C:/atx-cache/deps/xsimd-src/include/xsimd/config/xsimd_config.hpp`, so the suffix is "".
  - ic_screen.cpp gets only `/O2 /Ob2 /clang:-finline` in Debug (atx-engine/CMakeLists.txt:204-205), with no /arch.
    Its FP words are therefore "" exactly as the impl TU's were.
  - `<opt>` is ignored by the legacy rule.
- [x] DetPool bands are deterministic:
  - Bands write disjoint row cells, `pair_values` slices, `spanned` slices and `status[b]`.
  - `day_values` is const.
  - Accumulation runs in date order after each join, and spanned is an integer sum.
  - Each worker's scratch carries no state from one row that a later row reads (every buffer is refilled over m).
  - The payload readers lock per file.
- [x] The pair-cache key is complete for today's code:
  - Fields: role SHA, member-mask SHA, score_begin, rows, min_names, method version, the computing TU's build
    flavour, and the sorted payload SHAs.
  - Bits are symmetric (`pair_value` commutes).
  - Shards are self-hashed and carry the full key.
  - Two shards disagreeing on one pair refuse.
  - The future-drift gap is listed above.
- [x] Theme cap 10 -> 33: it is a refusal bound only, so no default output byte changes.
- [x] Pair-value buffer: 16·workers·pairs·8 B, at most 66.8 MB at 16 workers and 256 candidates. It is admitted when
  workers > 1, and with workers 1 it is at most 0.26 MB inside the slack.
- [x] Release preset:
  - The new deps dir isolates spdlog-build `_ITERATOR_DEBUG_LEVEL`.
  - The first configure of an existing build-equity-rel fetches deps again into deps/equity-rel.
  - bench-gate.ps1's build-equity-rel is affected the same way.
- [x] The diff stays inside S1 scope. Edits outside it:
  - Listed cross-lane edits: ic_screen.{hpp,cpp} (S2, wave 2) and atx-impl/CMakeLists.txt (one appended line).
  - New unowned files: atx-engine/include/atx/engine/build_flavor.hpp and strategy_marginal_pair_cache.{hpp,cpp}.
  - research_cycle.py is untouched, per P4.
- [x] Report evidence matches its claims: the pins reproduce, and the preset parses.
- Note, outside S1 scope and so not a finding: research_ic_fitness.cpp:76's refusal text still says "at most 11
  regressors" while its bound is now 34. Owner: S2 or root.
- Blindness held: I opened no return, IC, Sharpe or NAV output, and nothing dated 2024-01-01 or later. I built nothing
  and edited no tree. The only file I wrote is this review.
