# Task L1 report: content-keyed IC cache, verify-only-what-loads, SHA-NI
Branch feat/platform-v7-l1-iccache-20260928 @ 35861db6 (base cdc9c2a8; commits acf6f70b sha, 35861db6 runner). Not compiled, not run (lane rules).

**Changes**
- atx-core/src/sha256.cpp:161 `compress_shani` (`[[gnu::target("sha,sse4.1")]]`), CPUID gate :235, one-time dispatch :263, scalar fallback; `update` compresses whole blocks straight from caller bytes (:295); `sha256_file` reads 1 MiB (:374). Header: `Sha256Backend`, `Sha256(Sha256Backend)`, `backend()`. No CMake change: clang-cl gates the SSSE3/SSE4.1/SHA intrinsics on -m flags, so the TU includes tmmintrin/smmintrin/shaintrin directly after immintrin (guards checked in the clang 18 headers).
- atx-impl/src/strategy_ic_runner.cpp: `signal_key_text` :1053, `cache_key` :1063, `legacy_manifests` (--cache-legacy-fields) :1127, v2/v1 sidecar checks :1195/:1207, `cache_lookup` :1233, `cache_store` (v2 only) :1357, `cache_resolve` (one preflight per role) :1398, `cache_report` :1480, `verify_fields` :1591 + `FieldResidency` stamp trust :1615, `score_role` :2057, flags in `run_ic`/`dispatch_ic` :2337/:2496. hpp: `candidate_cache_legacy_fields`, `cache_report`.

**Key derivation (UTF-8, every line ends in \n)**: signal_key_sha256 = SHA256 of `atx.dsl-candidate-signal-key/v2`, `vm_identity=<dslvm{semver}_{compiler}{fp}>`, `eval_mode=ResearchFast;full-historical-asof-member-mask`, `layout=date-major-little-endian-f64;non-finite-stored-as-quiet-NaN`, `role_manifest_sha256=<hex>`, `dates=<d>`, `instruments=<n>`, `dsl_sha256=<SHA256(dsl)>`, then one `field=<name>:<payload sha256 from the pinned manifest>` per field the DSL reads, in name order. Paths: ROOT/<role>/<id>.<dsl16>.{f64,json} (base); ROOT/<role>/fp_<fk16>/<id>.<dsl16>.{f64,json}, where fk = SHA256 of the field lines. Sidecar schema atx.dsl-candidate-signal/v2 with `field_payload_sha256` {field: sha} ({} for base) and `signal_key_sha256`, matched on every part. Orientation and labels are not part of the raw-signal key; the unchanged IC-result key covers them (<entry dir>/ic1_<k16>/<stem>.json).
v1 is read in place. A base entry is read from ROOT/<role>/<id>.json. A field entry is read from ROOT/<M>/<id>.json when M is the pinned manifest or one named by --cache-legacy-fields, and M gives every field the DSL reads the same payload SHA. A v1 entry with another DSL counts as a miss; any other mismatch refuses, as before.
Fields: extents are stat'ed. A field is hashed once, before the role loads, only if some miss reads it (fail-fast kept). Later loads reuse that check while size and mtime are unchanged. Summary: candidate_cache.{layout, legacy_hits, entries[]}; per role `hash_seconds`, `verify_bytes`.

**Tests root should run**: `atx-core-tests --gtest_filter=Sha256.*` (4 new: backend fallback; NIST vectors x backend x update split; 5 MiB random + every tail 0..300 vs scalar; multi-chunk file). `atx-impl-strategy-ic-tests --gtest_filter=StrategyIcRunner.*`: 4 new tests (CandidateCacheKeyChangesOnlyForCandidatesReadingAChangedFieldPayload, CandidateCacheChangedDslUnderSameIdIsANewEntry, CandidateCacheReadsV1EntriesInPlaceThroughKnownManifests, CacheReportAccountsHitsMissesAndUnreferencedEntries). 11 cache tests were moved to v2 paths. CandidateCacheKeyCoversFieldsManifestOnlyForFieldCandidates was replaced by the first new test. stable_role now drops hash_seconds and verify_bytes.

**Root commands** (bash in pool-2, with the pins from v61_train.sh: FD6 = lo1-fields-v6b, FD7 = lo1-fields-v7, FS7 = 1d1fa87a...):
1. `powershell scripts\atx-build.ps1 build atx-core-tests atx-impl-strategy-ic-tests`, then the equity exe via mega-build.
2. `cp -al build-equity/mega-candidate-cache-v6u build-equity/mega-candidate-cache-v7l1`. Hard links, so no 2 GB copy; the runner never rewrites a file.
3. u pass: `$PY $BR --output build-equity/mega-v7l1-train-u-run1 --bind $IC --bind $L --bind $R2LO --bind $FD7/manifest.json --bind $FD6/manifest.json -- $IC --library $L --library-sha256 $LS --train $R2LO --train-sha256 $R2LOS --train-fields $FD7 --train-fields-sha256 $FS7 --output build-equity/mega-v7l1-train-u-1 --max-memory-mib 1536 --min-names 1000 --workers 4 --save-combined --candidate-cache build-equity/mega-candidate-cache-v7l1 --cache-legacy-fields $FD6`
   - Expected: hits 38, legacy_hits 38, ic_results hits 38, vm_evaluations 1 (sv_flow). Python simulation on the real sidecars: 38 hits, 38 IC entries.
   - cmp orientations.json and train_daily_ic.csv against mega-v61-train-u-1.
   - train_candidates.jsonl must match after dropping wall_seconds, stage_seconds, signal_cache and ic_result_cache.
4. w pass: step 3's flags plus `--composition-weights build-equity/mega-weights-v61-ew/composition_weights.json --composition-weights-sha256 f1a2213d...`, with output mega-v7l1w-train-ew-1. Expect 39/39 hits; compare with mega-v61w-train-ew-1.
5. Report: step 3's library/train/fields/cache flags plus `--cache-report`, without `--output`.
6. Digests: those runs check every pin through SHA-NI: library, role and fields manifests, the 38 cache-payload SHAs written by the scalar code, and the IC records. Any divergence refuses the run.

**Expected timings** (estimates):
- u about 12-14 s: composition 7.0, cache loads about 2, role load + labels 1.8, one VM, two field hashes.
- w about 11-12 s, down from 31.3: fields_verify 9.2 → 0, cache_load 10.0 → about 2.
- Cold u about 50 s (fields hashed once, not twice).

**Risks**
1. Schema bump. v1 readers cannot see v2 entries or v1 entries under another manifest: fit_composition_weights.py CacheLayout.resolve and mega_report/pitch.py an_sig_corr, which scans for schema v1. The fit refuses with "missing entry" until L2 reads `roles[train].candidate_cache.entries[]` ({id, layout, sidecar, payload, payload_sha256, field_payload_sha256, signal_key_sha256}). The directory/fields_directory keys are kept.
2. Hash-once trusts the (size, mtime) stamp between the upfront hash and the loads.
3. Nothing was compiled. The SHA-NI data flow was checked against hashlib with a Python model of the intrinsics. If clang-cl rejects the direct intrinsic includes, fall back to `/clang:-msha /clang:-msse4.1` on sha256.cpp.
4. Paths grow by about 37 characters (fp_ directory + .dsl16). The longest write is about 209 + len(id) for a 56-character root; the longest id today is 33.
5. v1 hits are not promoted to v2, so every run that needs a cross-manifest v1 hit must pass --cache-legacy-fields.
